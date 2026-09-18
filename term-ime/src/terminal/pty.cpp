#include "pty.hpp"
#include <unistd.h>
#include <fcntl.h>
#include <pty.h>
#include <poll.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <cerrno>
#include <cstring>
#include <chrono>
#include <thread>
#include <spdlog/spdlog.h>

Pty::Pty() = default;

bool Pty::wait_for_exit(int timeout_ms) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        int status = 0;
        pid_t r = waitpid(pid_, &status, WNOHANG);
        if (r == pid_ || r < 0) {
            return true;  // reaped, or no such child
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;  // timed out still running
}

Pty::~Pty() {
    if (master_fd_ >= 0) {
        close(master_fd_);
    }
    if (pid_ > 0) {
        // Reap the child, but never block forever: if the shell ignores
        // SIGTERM (e.g. a trapped TERM), escalate to SIGKILL after a bounded
        // wait. A bare blocking waitpid here hangs term-ime on exit (monkey
        // finding F2).
        kill(pid_, SIGTERM);
        if (!wait_for_exit(2000)) {  // 2s grace period
            kill(pid_, SIGKILL);
            wait_for_exit(1000);  // reap the kill
        }
    }
}

bool Pty::spawn(const std::string& shell) {
    // Ask the real controlling terminal for its geometry. A hard-coded 24x80
    // makes the child shell (and every TUI it launches) lay out for the wrong
    // size. Fall back to 24x80 when no tty is reachable.
    struct winsize ws {};
    const int probe_fds[] = {STDOUT_FILENO, STDIN_FILENO, STDERR_FILENO};
    bool have_size = false;
    for (int fd : probe_fds) {
        if (ioctl(fd, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0 && ws.ws_col > 0) {
            have_size = true;
            break;
        }
        ws = winsize{};
    }
    if (!have_size) {
        ws.ws_row = 24;
        ws.ws_col = 80;
    }
    ws.ws_xpixel = 0;
    ws.ws_ypixel = 0;

    spawn_error_.clear();
    auto error = [&](int code) {
        spawn_error_ = "Cannot start shell '" + shell + "': " + std::strerror(code);
        return false;
    };
    if (master_fd_ >= 0 || pid_ > 0)
        return error(EBUSY);
    if (shell.empty())
        return error(ENOENT);

    // EOF on this close-on-exec pipe means exec succeeded. An errno from the
    // child is a startup failure, not an ordinary shell EOF with exit code 0.
    int errors[2];
    if (pipe2(errors, O_CLOEXEC) < 0)
        return error(errno);
    int master = -1;
    pid_ = forkpty(&master, nullptr, nullptr, &ws);
    if (pid_ < 0) {
        const int code = errno;
        close(errors[0]);
        close(errors[1]);
        return error(code);
    }
    if (pid_ == 0) {
        close(errors[0]);
        setenv("TERM", "xterm-256color", 1);
        execl(shell.c_str(), shell.c_str(), nullptr);
        const int code = errno;
        const char* bytes = reinterpret_cast<const char*>(&code);
        size_t sent = 0;
        while (sent < sizeof(code)) {
            const ssize_t n = ::write(errors[1], bytes + sent, sizeof(code) - sent);
            if (n < 0 && errno == EINTR)
                continue;
            if (n <= 0)
                break;
            sent += static_cast<size_t>(n);
        }
        _exit(127);
    }
    master_fd_ = master;
    close(errors[1]);
    int code = 0;
    size_t received = 0;
    while (received < sizeof(code)) {
        const ssize_t n = ::read(errors[0], reinterpret_cast<char*>(&code) + received, sizeof(code) - received);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0) {
            code = errno;
            break;
        }
        if (n == 0) {
            if (received != 0)
                code = EIO;
            break;
        }
        received += static_cast<size_t>(n);
    }
    close(errors[0]);
    if (code == 0) {
        const int flags = fcntl(master_fd_, F_GETFL);
        if (flags < 0 || fcntl(master_fd_, F_SETFL, flags | O_NONBLOCK) < 0)
            code = errno;
    }
    if (code != 0) {
        close(master_fd_);
        master_fd_ = -1;
        kill(pid_, SIGKILL);
        // Reap this exact child so failed starts leak neither fds nor zombies.
        while (waitpid(pid_, nullptr, 0) < 0 && errno == EINTR) {}
        pid_ = -1;
        return error(code);
    }
    return true;
}

std::optional<std::vector<uint8_t>> Pty::read() {
    std::vector<uint8_t> buf(4096);
    ssize_t n = ::read(master_fd_, buf.data(), buf.size());
    if (n > 0) {
        buf.resize(n);
        return buf;
    }
    return std::nullopt;
}

bool Pty::write(const std::vector<uint8_t>& data) {
    // master_fd_ is O_NONBLOCK: a single write() may accept only part of the
    // buffer (large paste, long CJK commit) or return EAGAIN. Loop until every
    // byte is handed to the kernel; dropping the tail silently lost input.
    constexpr int kPollTimeoutMs = 1000;
    size_t written = 0;
    while (written < data.size()) {
        ssize_t n = ::write(master_fd_, data.data() + written, data.size() - written);
        if (n > 0) {
            written += static_cast<size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd pfd {};
            pfd.fd = master_fd_;
            pfd.events = POLLOUT;
            int pr = poll(&pfd, 1, kPollTimeoutMs);
            if (pr < 0) {
                if (errno == EINTR) {
                    continue;
                }
                spdlog::warn("pty write: poll failed: {}", std::strerror(errno));
                return false;
            }
            if (pr == 0) {
                spdlog::warn("pty write: stalled after {} of {} bytes", written, data.size());
                return false;
            }
            if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
                spdlog::warn("pty write: {} of {} bytes sent, peer gone", written, data.size());
                return false;
            }
            continue;
        }
        spdlog::warn("pty write: {} of {} bytes sent: {}", written, data.size(),
                     n < 0 ? std::strerror(errno) : "zero-length write");
        return false;
    }
    return true;
}

int Pty::fd() const {
    return master_fd_;
}

void Pty::resize(int rows, int cols) {
    struct winsize ws;
    ws.ws_row = rows;
    ws.ws_col = cols;
    ioctl(master_fd_, TIOCSWINSZ, &ws);
}
