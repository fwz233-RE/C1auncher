#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "c1_ime_client.h"
#include "protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

static int fail_closed(struct c1_ime_client *client, int error) {
    c1_ime_close(client);
    errno = error;
    return -1;
}

void c1_ime_close(struct c1_ime_client *client) {
    if (!client) return;
    if (client->fd >= 0) close(client->fd);
    client->fd = -1;
    client->next_sequence = 1;
    client->pending_sequence = 0;
    memset(&client->pending_request, 0, sizeof(client->pending_request));
}

int c1_ime_client_fd(const struct c1_ime_client *client) {
    return client ? client->fd : -1;
}

int c1_ime_connect(struct c1_ime_client *client, const char *path) {
    struct sockaddr_un address;
    struct stat st;
    struct ucred peer;
    socklen_t peer_size = sizeof(peer);
    char parent[sizeof(address.sun_path)];
    char *slash;
    int dirfd, error;
    if (!client) { errno = EINVAL; return -1; }
    if (client->fd >= 0) { errno = EISCONN; return -1; }
    if (!path) path = C1_IME_DEFAULT_SOCKET;
    if (path[0] != '/' || strlen(path) >= sizeof(address.sun_path)) { errno = EINVAL; return -1; }
    strcpy(parent, path);
    slash = strrchr(parent, '/');
    if (!slash || slash == parent || !slash[1]) { errno = EINVAL; return -1; }
    *slash = '\0';
    dirfd = open(parent, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (dirfd < 0) return -1;
    if (fstat(dirfd, &st) < 0) { error = errno; close(dirfd); errno = error; return -1; }
    if (st.st_uid != geteuid() || (st.st_mode & 07777) != 0700) {
        close(dirfd); errno = EACCES; return -1;
    }
    if (fstatat(dirfd, slash + 1, &st, AT_SYMLINK_NOFOLLOW) < 0) {
        error = errno; close(dirfd); errno = error; return -1;
    }
    close(dirfd);
    if (!S_ISSOCK(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & 07777) != 0600) {
        errno = EACCES; return -1;
    }
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    strcpy(address.sun_path, path);
    client->fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (client->fd < 0) return -1;
    if (connect(client->fd, (struct sockaddr *)&address,
                (socklen_t)(offsetof(struct sockaddr_un, sun_path) + strlen(path) + 1)) < 0)
        return fail_closed(client, errno);
    if (getsockopt(client->fd, SOL_SOCKET, SO_PEERCRED, &peer, &peer_size) < 0)
        return fail_closed(client, errno);
    if (peer.uid != geteuid()) return fail_closed(client, EACCES);
    client->next_sequence = 1;
    client->pending_sequence = 0;
    return 0;
}

int c1_ime_send(struct c1_ime_client *client, const struct c1_ime_request *request) {
    unsigned char packet[C1_WIRE_HEADER];
    ssize_t n;
    if (!client || client->fd < 0) { errno = ENOTCONN; return -1; }
    if (client->pending_sequence) { errno = EBUSY; return -1; }
    if (!c1_valid_request(request)) { errno = EINVAL; return -1; }
    c1_encode_request(packet, request, client->next_sequence);
    do { n = send(client->fd, packet, sizeof(packet), MSG_DONTWAIT | MSG_NOSIGNAL); } while (n < 0 && errno == EINTR);
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
    if (n < 0) return fail_closed(client, errno);
    if (n != (ssize_t)sizeof(packet)) return fail_closed(client, EIO);
    client->pending_request = *request;
    client->pending_sequence = client->next_sequence++;
    if (!client->next_sequence) client->next_sequence = 1;
    return 1;
}

static int read_text(char *out, const unsigned char *packet, size_t size, size_t *position, size_t length) {
    if (length >= C1_IME_TEXT_CAPACITY || *position > size || length > size - *position ||
        !c1_valid_utf8(packet + *position, length)) return 0;
    memcpy(out, packet + *position, length);
    out[length] = '\0';
    *position += length;
    return 1;
}

int c1_ime_receive(struct c1_ime_client *client, struct c1_ime_response *response) {
    unsigned char packet[C1_IME_MAX_PACKET];
    struct iovec iov = {packet, sizeof(packet)};
    struct msghdr message;
    ssize_t n;
    size_t position;
    uint32_t i;
    if (!client || client->fd < 0) { errno = ENOTCONN; return -1; }
    if (!response) { errno = EINVAL; return -1; }
    memset(&message, 0, sizeof(message));
    message.msg_iov = &iov;
    message.msg_iovlen = 1;
    do { n = recvmsg(client->fd, &message, MSG_DONTWAIT); } while (n < 0 && errno == EINTR);
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
    if (n < 0) return fail_closed(client, errno);
    if (!n) return fail_closed(client, ECONNRESET);
    if ((message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) || n < (ssize_t)C1_WIRE_REPLY_HEADER ||
        !client->pending_sequence || c1_get32(packet) != C1_WIRE_MAGIC ||
        c1_get16(packet + 4) != C1_IME_PROTOCOL_VERSION ||
        c1_get16(packet + 6) != (client->pending_request.operation | C1_WIRE_REPLY) ||
        c1_get32(packet + 8) != client->pending_sequence || c1_get32(packet + 12) != (uint32_t)n ||
        c1_get32(packet + 16) != client->pending_request.keysym ||
        (c1_get32(packet + 20) & ~C1_WIRE_FLAGS) || c1_get32(packet + 24) > C1_IME_STATUS_LIMIT ||
        c1_get32(packet + 28) || c1_get16(packet + 38) || c1_get16(packet + 36) > C1_IME_MAX_CANDIDATES)
        return fail_closed(client, EPROTO);
    memset(response, 0, sizeof(*response));
    response->sequence = client->pending_sequence;
    response->request = client->pending_request;
    response->flags = c1_get32(packet + 20);
    response->status = c1_get32(packet + 24);
    response->candidate_count = c1_get16(packet + 36);
    position = C1_WIRE_REPLY_HEADER;
    if (!read_text(response->preedit, packet, (size_t)n, &position, c1_get16(packet + 32)) ||
        !read_text(response->commit, packet, (size_t)n, &position, c1_get16(packet + 34)))
        return fail_closed(client, EPROTO);
    for (i = 0; i < response->candidate_count; ++i) {
        uint16_t length;
        if (position + 2 > (size_t)n) return fail_closed(client, EPROTO);
        length = c1_get16(packet + position); position += 2;
        if (!read_text(response->candidates[i], packet, (size_t)n, &position, length))
            return fail_closed(client, EPROTO);
    }
    if (position != (size_t)n) return fail_closed(client, EPROTO);
    client->pending_sequence = 0;
    memset(&client->pending_request, 0, sizeof(client->pending_request));
    return 1;
}
