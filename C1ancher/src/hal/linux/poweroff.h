#ifndef C1_LINUX_POWEROFF_H
#define C1_LINUX_POWEROFF_H

#include <stddef.h>

typedef struct {
    const char *path;
    int shutdown_arguments;
} c1_poweroff_command;

/* Returns 0 only when a command exits successfully; this does not prove that
 * the board has physically powered down. Failure reports an errno-style code.
 * Keep the supervisor heartbeat alive while waiting for init's shutdown. */
int c1_linux_poweroff_run(const c1_poweroff_command *commands, size_t count,
                          unsigned int timeout_ms, void (*heartbeat)(void));
int c1_linux_poweroff_request(void (*heartbeat)(void));
/* Nonblocking cleanup, called from the UI thread's regular loop. */
void c1_linux_poweroff_reap(void);

#endif
