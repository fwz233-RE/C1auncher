#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "protocol.h"
#include "ime/rime_engine.hpp"
#include "util/utf8.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>
#include <fcntl.h>
#include <poll.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace {
volatile sig_atomic_t stopping = 0;
void stop_handler(int) { stopping = 1; }

struct Fd {
    int value = -1;
    explicit Fd(int fd = -1) : value(fd) {}
    ~Fd() { if (value >= 0) close(value); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    void reset(int fd = -1) { if (value >= 0) close(value); value = fd; }
};

bool private_directory(const std::string& path) {
    if (path.empty() || path[0] != '/') { errno = EINVAL; return false; }
    if (mkdir(path.c_str(), 0700) < 0 && errno != EEXIST) return false;
    Fd fd(open(path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    struct stat st {};
    if (fd.value < 0 || fstat(fd.value, &st) < 0) return false;
    if (st.st_uid != geteuid() || (st.st_mode & 07777) != 0700) { errno = EACCES; return false; }
    return true;
}

struct SocketOwner {
    Fd listener;
    std::string path;
    dev_t device = 0;
    ino_t inode = 0;
    ~SocketOwner() {
        struct stat st {};
        // Never unlink an existing/foreign/replaced socket. SIGKILL may leave a
        // stale socket; manual removal after verifying no live owner is required.
        if (inode && lstat(path.c_str(), &st) == 0 && S_ISSOCK(st.st_mode) &&
            st.st_uid == geteuid() && st.st_dev == device && st.st_ino == inode)
            unlink(path.c_str());
    }
    bool bind_socket(const std::string& socket_path) {
        sockaddr_un address {};
        if (socket_path.size() >= sizeof(address.sun_path) ||
            !private_directory(std::filesystem::path(socket_path).parent_path().string())) return false;
        listener.reset(socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
        if (listener.value < 0) return false;
        address.sun_family = AF_UNIX;
        std::memcpy(address.sun_path, socket_path.c_str(), socket_path.size() + 1);
        // No pre-bind unlink, including stale sockets. bind is the atomic
        // exclusion operation against a concurrently starting second process.
        if (bind(listener.value, reinterpret_cast<sockaddr*>(&address),
                 static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + socket_path.size() + 1)) < 0) return false;
        path = socket_path;
        struct stat st {};
        if (lstat(path.c_str(), &st) < 0) return false;
        device = st.st_dev; inode = st.st_ino;
        if (chmod(path.c_str(), 0600) < 0 || listen(listener.value, 8) < 0) return false;
        return true;
    }
};

// Service-owned data directory: do not reuse a desktop user's Rime directory.
// The managed patch makes every page exactly the five candidates the SDK shows.
bool prepare_user_data(const std::string& path, Fd& lock) {
    if (!private_directory(path)) return false;
    lock.reset(open((path + "/.c1-ime.lock").c_str(), O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600));
    struct stat st {};
    if (lock.value < 0 || fstat(lock.value, &st) < 0) return false;
    if (!S_ISREG(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & 07777) != 0600 || st.st_nlink != 1) {
        errno = EACCES; return false;
    }
    if (flock(lock.value, LOCK_EX | LOCK_NB) < 0) return false;
    constexpr char patch[] = "# Managed by c1-ime-service; dedicated user directory.\npatch:\n  menu/page_size: 5\n  switcher/hotkeys: []\n";
    const auto config = path + "/default.custom.yaml";
    Fd file(open(config.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
    if (file.value >= 0) {
        if (write(file.value, patch, sizeof(patch) - 1) != static_cast<ssize_t>(sizeof(patch) - 1)) return false;
    } else {
        if (errno != EEXIST) return false;
        file.reset(open(config.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
        if (file.value < 0 || fstat(file.value, &st) < 0) return false;
        if (!S_ISREG(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & 07777) != 0600) {
            errno = EACCES; return false;
        }
        std::array<char, sizeof(patch)> data {};
        const auto n = read(file.value, data.data(), data.size());
        if (n != static_cast<ssize_t>(sizeof(patch) - 1) || std::memcmp(data.data(), patch, sizeof(patch) - 1)) {
            errno = EINVAL; return false;
        }
    }
    return true;
}

std::string encode(const std::u32string& text) {
    std::string result;
    for (char32_t ch : text) result += utf8::encode(ch);
    return result;
}

std::string bounded_text(std::string text, uint32_t& flags) {
    if (text.size() >= C1_IME_TEXT_CAPACITY) {
        size_t length = C1_IME_TEXT_CAPACITY - 1;
        while (length && (static_cast<unsigned char>(text[length]) & 0xc0) == 0x80) --length;
        text.resize(length);
        flags |= C1_IME_TRUNCATED;
    }
    return text;
}

class Session {
   public:
    explicit Session(RimeIme& ime) : ime_(ime) {}
    bool reset() {
        ime_.cancel();
        (void)ime_.take_commit();
        ime_.set_mode(ImeMode::English);
        chinese_ = false; password_ = false; input_steps_ = 0;
        // Reapplying the schema rebuilds processors and resets options, quote
        // pairing and navigation state. RimeIme/global runtime is NOT recreated.
        return ime_.select_schema("luna_pinyin_simp");
    }
    bool respond(const c1_ime_request& request, uint32_t seq, std::vector<unsigned char>& packet) {
        bool consumed = false;
        uint32_t status = C1_IME_STATUS_OK;
        std::string commit;
        switch (request.operation) {
            case C1_IME_OP_MODE:
                clear();
                chinese_ = request.value != 0;
                ime_.set_mode(chinese_ && !password_ ? ImeMode::Chinese : ImeMode::English);
                break;
            case C1_IME_OP_PURPOSE:
                clear();
                password_ = request.value == C1_IME_PURPOSE_PASSWORD;
                ime_.set_mode(chinese_ && !password_ ? ImeMode::Chinese : ImeMode::English);
                break;
            case C1_IME_OP_CANCEL:
                clear();
                break;
            case C1_IME_OP_KEY:
                if (chinese_ && !password_) process_key(request, consumed, status, commit);
                break;
            case C1_IME_OP_SELECT:
                if (chinese_ && !password_) {
                    const auto candidates = ime_.candidates();
                    if (request.value < std::min<size_t>(candidates.size(), C1_IME_MAX_CANDIDATES)) {
                        commit = encode(ime_.select(static_cast<int>(request.value)));
                        consumed = true;
                    } else status = C1_IME_STATUS_BAD_REQUEST;
                }
                break;
            case C1_IME_OP_PAGE:
                if (chinese_ && !password_ && ime_.state() != ImeState::Inactive) {
                    if (request.value) ime_.page_down(); else ime_.page_up();
                    consumed = true;
                }
                break;
            default: break;
        }
        if (!password_) commit += encode(ime_.take_commit());
        uint32_t flags = static_cast<uint32_t>(C1_IME_READY) |
                         (consumed ? static_cast<uint32_t>(C1_IME_CONSUMED) : 0u) |
                         (chinese_ ? static_cast<uint32_t>(C1_IME_CHINESE) : 0u) |
                         (password_ ? static_cast<uint32_t>(C1_IME_PASSWORD) : 0u);
        std::string preedit;
        std::vector<std::string> candidates;
        if (chinese_ && !password_) {
            preedit = bounded_text(ime_.buffer(), flags);
            if (ime_.state() != ImeState::Inactive) flags |= C1_IME_COMPOSING;
            for (const auto& candidate : ime_.candidates()) {
                if (candidates.size() == C1_IME_MAX_CANDIDATES) break;
                candidates.push_back(bounded_text(encode(candidate.text), flags));
            }
        }
        if (preedit.empty()) input_steps_ = 0;
        // Never silently truncate committed text. A nonconforming/custom schema
        // exceeding the contract closes the connection (ambiguous -> no replay).
        if (commit.size() >= C1_IME_TEXT_CAPACITY ||
            !c1_valid_utf8(reinterpret_cast<const unsigned char*>(commit.data()), commit.size())) return false;
        packet.assign(C1_WIRE_REPLY_HEADER, 0);
        c1_put32(packet.data(), C1_WIRE_MAGIC); c1_put16(packet.data() + 4, C1_IME_PROTOCOL_VERSION);
        c1_put16(packet.data() + 6, static_cast<uint16_t>(request.operation | C1_WIRE_REPLY));
        c1_put32(packet.data() + 8, seq); c1_put32(packet.data() + 16, request.keysym);
        c1_put32(packet.data() + 20, flags); c1_put32(packet.data() + 24, status);
        c1_put16(packet.data() + 32, static_cast<uint16_t>(preedit.size()));
        c1_put16(packet.data() + 34, static_cast<uint16_t>(commit.size()));
        c1_put16(packet.data() + 36, static_cast<uint16_t>(candidates.size()));
        packet.insert(packet.end(), preedit.begin(), preedit.end());
        packet.insert(packet.end(), commit.begin(), commit.end());
        for (const auto& candidate : candidates) {
            const size_t start = packet.size(); packet.resize(start + 2);
            c1_put16(packet.data() + start, static_cast<uint16_t>(candidate.size()));
            packet.insert(packet.end(), candidate.begin(), candidate.end());
        }
        c1_put32(packet.data() + 12, static_cast<uint32_t>(packet.size()));
        return packet.size() <= C1_IME_MAX_PACKET;
    }
   private:
    RimeIme& ime_;
    bool chinese_ = false;
    bool password_ = false;
    unsigned input_steps_ = 0;
    void clear() { ime_.cancel(); (void)ime_.take_commit(); input_steps_ = 0; }
    void process_key(const c1_ime_request& request, bool& consumed, uint32_t& status, std::string& commit) {
        const bool composing = ime_.state() != ImeState::Inactive;
        const auto key = request.keysym;
        if (key == C1_IME_KEY_ESCAPE) { if (composing) { clear(); consumed = true; } return; }
        if (key == C1_IME_KEY_BACKSPACE) {
            if (composing) { ime_.backspace(); consumed = true; if (input_steps_) --input_steps_; }
            return;
        }
        if (!request.modifiers && composing) {
            if (key == C1_IME_KEY_SPACE || key == C1_IME_KEY_RETURN || (key >= '1' && key <= '5')) {
                const int index = key >= '1' && key <= '5' ? static_cast<int>(key - '1') : 0;
                if (static_cast<size_t>(index) < ime_.candidates().size()) {
                    commit = encode(ime_.select(index)); consumed = true; return;
                }
            }
            // Hidden slots never select candidates the caller cannot display.
            if (key >= '6' && key <= '9') { consumed = true; return; }
        }
        // Bound composition work and response text. Cancellation/deletion still
        // work at the limit; this request is explicitly consumed, not forwarded.
        if (input_steps_ >= 128 && key >= 0x20 && key < 0xff00) {
            consumed = true; status = C1_IME_STATUS_LIMIT; return;
        }
        consumed = ime_.process_key(static_cast<int>(key), static_cast<int>(request.modifiers));
        if (consumed && key >= 0x20 && key < 0xff00) ++input_steps_;
    }
};

int run_server(SocketOwner& socket, RimeIme& ime) {
    Session session(ime);
    if (!session.reset()) { std::fprintf(stderr, "Cannot select luna_pinyin_simp\n"); return 1; }
    Fd client;
    uint32_t expected_sequence = 1;
    std::vector<unsigned char> output;
    auto disconnect = [&]() {
        client.reset(); output.clear(); expected_sequence = 1;
        if (!session.reset()) stopping = 1;
    };
    std::fprintf(stderr, "c1-ime-service ready: %s\n", socket.path.c_str());
    while (!stopping) {
        pollfd fds[2] = {{socket.listener.value, POLLIN, 0},
                        {client.value, static_cast<short>(output.empty() ? POLLIN : POLLOUT), 0}};
        const int result = poll(fds, 2, 250);
        if (result < 0) { if (errno == EINTR) continue; return 1; }
        // Retire a disconnected owner before accepting the next focus. Clear the
        // old poll entry as close/accept may reuse the exact same descriptor.
        if (client.value >= 0 && (fds[1].revents & (POLLHUP | POLLERR | POLLNVAL))) {
            disconnect();
            fds[1].fd = -1;
            fds[1].revents = 0;
        }
        if (fds[0].revents & POLLIN) {
            // Bounded accept batch prevents connection floods starving keys.
            for (unsigned i = 0; i < 16; ++i) {
                int fd = accept4(socket.listener.value, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
                if (fd < 0) break;
                ucred peer {}; socklen_t length = sizeof(peer);
                if (client.value >= 0 || getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &length) < 0 ||
                    peer.uid != geteuid()) { close(fd); continue; }
                client.reset(fd);
            }
        }
        if (client.value < 0 || fds[1].fd != client.value) continue;
        if (fds[1].revents & (POLLHUP | POLLERR | POLLNVAL)) { disconnect(); continue; }
        if (fds[1].revents & POLLIN) {
            std::array<unsigned char, C1_IME_MAX_PACKET> data {};
            iovec iov {data.data(), data.size()}; msghdr message {};
            message.msg_iov = &iov; message.msg_iovlen = 1;
            const auto n = recvmsg(client.value, &message, MSG_DONTWAIT);
            if (n < 0 && (errno == EAGAIN || errno == EINTR)) continue;
            c1_ime_request request {}; uint32_t sequence = 0;
            if (n <= 0 || (message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) ||
                !c1_decode_request(data.data(), static_cast<size_t>(n), &request, &sequence) ||
                sequence != expected_sequence) { disconnect(); continue; }
            if (!++expected_sequence) expected_sequence = 1;
            if (!session.respond(request, sequence, output)) { disconnect(); continue; }
        }
        if (!output.empty()) {
            const auto n = send(client.value, output.data(), output.size(), MSG_DONTWAIT | MSG_NOSIGNAL);
            if (n < 0 && (errno == EAGAIN || errno == EINTR)) continue;
            if (n != static_cast<ssize_t>(output.size())) { disconnect(); continue; }
            output.clear();
        }
    }
    disconnect();
    return 0;
}
} // namespace

int main(int argc, char** argv) {
    std::string socket_path = C1_IME_DEFAULT_SOCKET;
    std::string shared_data;
    std::string user_data;
    bool prebuilt_only = false;
    for (int i = 1; i < argc; ++i) {
        const std::string option = argv[i];
        if (option == "--help") {
            std::puts("c1-ime-service [--prebuilt-only] [--socket /absolute/socket] --shared-data /absolute/rime-data --user-data /absolute/private-dir\nForeground only; caller controls lifecycle. No evdev/display access.\n--prebuilt-only: require target prebuilt dictionaries; never run dictionary maintenance/deployment.");
            return 0;
        }
        if (option == "--prebuilt-only") { prebuilt_only = true; continue; }
        if (i + 1 >= argc) { std::fprintf(stderr, "Missing argument: %s\n", argv[i]); return 2; }
        if (option == "--socket") socket_path = argv[++i];
        else if (option == "--shared-data") shared_data = argv[++i];
        else if (option == "--user-data") user_data = argv[++i];
        else { std::fprintf(stderr, "Unknown option: %s\n", argv[i]); return 2; }
    }
    if (socket_path.empty() || socket_path[0] != '/' || shared_data.empty() || shared_data[0] != '/' ||
        user_data.empty() || user_data[0] != '/') {
        std::fprintf(stderr, "Absolute socket/shared-data/user-data paths required\n"); return 2;
    }
    umask(0077);
    struct sigaction action {};
    action.sa_handler = stop_handler; sigemptyset(&action.sa_mask);
    sigaction(SIGTERM, &action, nullptr); sigaction(SIGINT, &action, nullptr);
    Fd data_lock;
    SocketOwner socket;
    if (!socket.bind_socket(socket_path)) { std::perror("Secure socket bind failed (existing sockets are never removed)"); return 1; }
    if (!prepare_user_data(user_data, data_lock)) { std::perror("Dedicated private user-data preparation failed"); return 1; }
    struct stat st {};
    if (stat((shared_data + "/luna_pinyin_simp.schema.yaml").c_str(), &st) < 0 || !S_ISREG(st.st_mode)) {
        std::fprintf(stderr, "Shared data lacks luna_pinyin_simp.schema.yaml\n"); return 1;
    }
    if (prebuilt_only) {
        // Fixed service schema, not a generic Rime package loader. Fail closed
        // before entering librime, including on incomplete app updates.
        const char* required[] = {
            "default.yaml", "build/default.yaml", "build/luna_pinyin_simp.schema.yaml",
            "build/luna_pinyin_simp.prism.bin", "build/luna_pinyin.table.bin",
            "build/luna_pinyin.reverse.bin", "opencc/t2s_full.json",
            "opencc/TSCharacters.ocd2", "opencc/TSPhrases.ocd2",
            "opencc/variants.txt", "opencc/variants_ext.txt", "opencc/variants_jp.txt",
        };
        for (const char* relative : required) {
            const auto resource = shared_data + "/" + relative;
            if (stat(resource.c_str(), &st) < 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) {
                std::fprintf(stderr, "Prebuilt-only resource missing/empty: %s; dictionary deployment disabled\n", relative);
                return 1;
            }
        }
    }
    // Exactly one RimeIme for the entire process. Deployment blocks this child,
    // never the calling desktop. No client request reaches Rime until ready.
    RimeIme ime(shared_data, user_data);
    if (!ime.initialize(prebuilt_only)) { std::fprintf(stderr, "Rime initialization failed\n"); return 1; }
    if (stopping) return 0;
    return run_server(socket, ime);
}
