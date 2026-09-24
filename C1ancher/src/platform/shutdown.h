#ifndef C1_PLATFORM_SHUTDOWN_H
#define C1_PLATFORM_SHUTDOWN_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

/* An inherited, anonymous socket capability, never a PID or persistent flag. */
#define C1_SHUTDOWN_FD_ENV "C1_SHUTDOWN_FD"
#define C1_SHUTDOWN_ACK_MS 1000
#define C1_SHUTDOWN_LEASE_MS 30000
#define C1_SHUTDOWN_RENEW_MS 5000

struct c1_shutdown_client {
    int fd;
    pid_t owner;
    uint32_t serial;
};
struct c1_shutdown_server {
    int fd;
    int64_t until_ms;
};

/* Supervisor plumbing. The server authenticates each packet's kernel PID/UID;
 * the client validates the socket's kernel peer against its actual parent. */
int c1_shutdown_pair(int descriptors[2]);
int c1_shutdown_export(int fd);
int c1_shutdown_client_init(struct c1_shutdown_client *client);
int c1_shutdown_client_request(struct c1_shutdown_client *client, bool active);
void c1_shutdown_server_poll(struct c1_shutdown_server *server, pid_t child,
                             struct c1_shutdown_client *upstream);
bool c1_shutdown_server_active(struct c1_shutdown_server *server);

/* UI-thread only. All return 0 on success or an errno value. Call init before
 * spawning workers; begin MUST succeed before executing any poweroff command.
 * On command failure cancel before resuming normal UI/watchdog operation.
 * During the command/wait loop call keepalive at least every five seconds.
 * These functions never write health heartbeats or confirm a pending update. */
int c1_shutdown_init(void);
int c1_shutdown_begin(void);
int c1_shutdown_cancel(void);
int c1_shutdown_keepalive(void);

#endif
