#define _GNU_SOURCE 1
#include "platform/shutdown.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define SHUTDOWN_MAGIC 0x43315344U
#define SHUTDOWN_VERSION 1U
#define SHUTDOWN_ACK 0x100U
#define SHUTDOWN_BEGIN 1U
#define SHUTDOWN_CANCEL 2U

struct shutdown_packet {
    uint32_t magic, version, operation, serial, error;
};

static struct c1_shutdown_client ui_client = {-1, 0, 0};
static bool ui_active;
static int64_t ui_renew_at;

static int64_t clock_ms(void)
{
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) return -1;
    return (int64_t)value.tv_sec * 1000 + value.tv_nsec / 1000000;
}

int c1_shutdown_pair(int descriptors[2])
{
    int enabled = 1;
    if (descriptors == NULL) return EINVAL;
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK,
                   0, descriptors) != 0) return errno;
    if (setsockopt(descriptors[0], SOL_SOCKET, SO_PASSCRED, &enabled, sizeof(enabled)) != 0) {
        int error = errno;
        close(descriptors[0]); close(descriptors[1]);
        descriptors[0] = descriptors[1] = -1;
        return error;
    }
    return 0;
}

int c1_shutdown_export(int fd)
{
    char text[32];
    if (fcntl(fd, F_SETFD, 0) != 0) return errno;
    (void)snprintf(text, sizeof(text), "%d", fd);
    return setenv(C1_SHUTDOWN_FD_ENV, text, 1) == 0 ? 0 : errno;
}

int c1_shutdown_client_init(struct c1_shutdown_client *client)
{
    const char *text = getenv(C1_SHUTDOWN_FD_ENV);
    struct ucred peer;
    struct sockaddr_storage address;
    socklen_t size;
    char *end;
    long fd;
    int type;
    if (client == NULL) return EINVAL;
    client->fd = -1;
    client->owner = getpid();
    client->serial = 0;
    if (text == NULL) return ENOTCONN;
    errno = 0;
    fd = strtol(text, &end, 10);
    if (errno || end == text || *end || fd <= STDERR_FILENO || fd > INT_MAX) {
        (void)unsetenv(C1_SHUTDOWN_FD_ENV);
        return EINVAL;
    }
    (void)unsetenv(C1_SHUTDOWN_FD_ENV);
    size = sizeof(type);
    if (getsockopt((int)fd, SOL_SOCKET, SO_TYPE, &type, &size) != 0 || type != SOCK_SEQPACKET)
        return EPROTOTYPE;
    size = sizeof(address);
    if (getsockname((int)fd, (struct sockaddr *)&address, &size) != 0 ||
        address.ss_family != AF_UNIX) return EPROTOTYPE;
    size = sizeof(peer);
    if (getsockopt((int)fd, SOL_SOCKET, SO_PEERCRED, &peer, &size) != 0 ||
        peer.pid != getppid() || peer.uid != geteuid()) return EACCES;
    if (fcntl((int)fd, F_SETFD, FD_CLOEXEC) != 0 ||
        fcntl((int)fd, F_SETFL, O_NONBLOCK) != 0) return errno;
    client->fd = (int)fd;
    return 0;
}

static int send_packet(int fd, const struct shutdown_packet *packet)
{
    ssize_t count;
    do { count = send(fd, packet, sizeof(*packet), MSG_NOSIGNAL); }
    while (count < 0 && errno == EINTR);
    return count == (ssize_t)sizeof(*packet) ? 0 : count < 0 ? errno : EIO;
}

int c1_shutdown_client_request(struct c1_shutdown_client *client, bool active)
{
    struct shutdown_packet request = {SHUTDOWN_MAGIC, SHUTDOWN_VERSION,
        active ? SHUTDOWN_BEGIN : SHUTDOWN_CANCEL, 0, 0};
    int64_t started = clock_ms();
    int error;
    if (client == NULL) return EINVAL;
    if (client->fd < 0 || client->owner != getpid()) return ENOTCONN;
    if (started < 0) return EIO;
    if (++client->serial == 0) ++client->serial;
    request.serial = client->serial;
    error = send_packet(client->fd, &request);
    if (error != 0) return error;
    for (;;) {
        struct shutdown_packet reply;
        struct pollfd watch = {client->fd, POLLIN, 0};
        int64_t now = clock_ms();
        ssize_t count;
        int result;
        if (now < started) return EIO;
        if (now - started >= C1_SHUTDOWN_ACK_MS) return ETIMEDOUT;
        result = poll(&watch, 1U, C1_SHUTDOWN_ACK_MS - (int)(now - started));
        if (result < 0 && errno == EINTR) continue;
        if (result < 0) return errno;
        if (result == 0) return ETIMEDOUT;
        count = recv(client->fd, &reply, sizeof(reply), MSG_TRUNC);
        if (count < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (count == 0) return ECONNRESET;
        if (count < 0) return errno;
        if (count != (ssize_t)sizeof(reply) || reply.magic != SHUTDOWN_MAGIC ||
            reply.version != SHUTDOWN_VERSION) return EPROTO;
        /* A timed-out request's ACK cannot satisfy a later cancellation. */
        if (reply.serial != request.serial) continue;
        if (reply.operation != (request.operation | SHUTDOWN_ACK) || reply.error > INT_MAX)
            return EPROTO;
        return (int)reply.error;
    }
}

bool c1_shutdown_server_active(struct c1_shutdown_server *server)
{
    int64_t now;
    if (server == NULL) return false;
    now = clock_ms();
    if (now < 0 || now >= server->until_ms) server->until_ms = 0;
    return server->until_ms != 0;
}

void c1_shutdown_server_poll(struct c1_shutdown_server *server, pid_t child,
                             struct c1_shutdown_client *upstream)
{
    struct shutdown_packet packet;
    union { struct cmsghdr alignment; unsigned char bytes[CMSG_SPACE(sizeof(struct ucred))]; } control;
    struct iovec vector = {&packet, sizeof(packet)};
    struct msghdr message;
    struct cmsghdr *header;
    bool authenticated = false, was_active;
    ssize_t count;
    int error = 0;
    int64_t now;
    if (server == NULL) return;
    if (server->fd < 0) return;
    memset(&message, 0, sizeof(message));
    message.msg_iov = &vector;
    message.msg_iovlen = 1;
    message.msg_control = control.bytes;
    message.msg_controllen = sizeof(control.bytes);
    count = recvmsg(server->fd, &message, MSG_DONTWAIT | MSG_TRUNC);
    if (count < 0 && (errno == EAGAIN || errno == EINTR)) return;
    if (count <= 0) {
        /* EOF during accepted shutdown is expected when init kills the child.
         * Preserve its short lease until waitpid observes that exit. */
        close(server->fd); server->fd = -1;
        return;
    }
    for (header = CMSG_FIRSTHDR(&message); header != NULL; header = CMSG_NXTHDR(&message, header)) {
        if (header->cmsg_level == SOL_SOCKET && header->cmsg_type == SCM_CREDENTIALS &&
            header->cmsg_len == CMSG_LEN(sizeof(struct ucred))) {
            struct ucred credentials;
            memcpy(&credentials, CMSG_DATA(header), sizeof(credentials));
            authenticated = credentials.pid == child && credentials.uid == geteuid();
        }
    }
    if (!authenticated || (message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) ||
        count != (ssize_t)sizeof(packet) || packet.magic != SHUTDOWN_MAGIC ||
        packet.version != SHUTDOWN_VERSION || packet.error != 0 || packet.serial == 0 ||
        (packet.operation != SHUTDOWN_BEGIN && packet.operation != SHUTDOWN_CANCEL)) {
        fprintf(stderr, "C1 shutdown: rejected unauthenticated or invalid packet\n");
        return;
    }
    was_active = c1_shutdown_server_active(server);
    if (upstream != NULL)
        error = c1_shutdown_client_request(upstream, packet.operation == SHUTDOWN_BEGIN);
    now = clock_ms();
    if (now < 0 && error == 0) error = EIO;
    if (packet.operation == SHUTDOWN_CANCEL) server->until_ms = 0;
    else if (error == 0) server->until_ms = now + C1_SHUTDOWN_LEASE_MS;
    /* Both supervision levels enter the state BEFORE acknowledging the UI. */
    packet.operation |= SHUTDOWN_ACK;
    packet.error = (uint32_t)error;
    if (send_packet(server->fd, &packet) != 0 || error != 0) {
        /* A failed renewal does not revoke an already accepted shutdown.
         * Keep its bounded lease so the UI can retry a transient failure. */
        if (!was_active || packet.operation == (SHUTDOWN_CANCEL | SHUTDOWN_ACK)) {
            server->until_ms = 0;
            if (upstream != NULL) (void)c1_shutdown_client_request(upstream, false);
        }
    }
}

int c1_shutdown_init(void)
{
    if (ui_client.fd >= 0 && ui_client.owner == getpid()) close(ui_client.fd);
    ui_active = false;
    return c1_shutdown_client_init(&ui_client);
}

int c1_shutdown_begin(void)
{
    int error = c1_shutdown_client_request(&ui_client, true);
    if (error != 0) {
        /* Ordered on the same socket: a delayed BEGIN is followed by CANCEL.
         * If even cancellation cannot be delivered, the bounded lease expires. */
        (void)c1_shutdown_client_request(&ui_client, false);
        ui_active = false;
        return error;
    }
    ui_active = true;
    ui_renew_at = clock_ms() + C1_SHUTDOWN_RENEW_MS;
    return 0;
}

int c1_shutdown_cancel(void)
{
    ui_active = false;
    return c1_shutdown_client_request(&ui_client, false);
}

int c1_shutdown_keepalive(void)
{
    int error;
    if (!ui_active || clock_ms() < ui_renew_at) return 0;
    error = c1_shutdown_client_request(&ui_client, true);
    ui_renew_at = clock_ms() + (error == 0 ? C1_SHUTDOWN_RENEW_MS : 1000);
    return error;
}
