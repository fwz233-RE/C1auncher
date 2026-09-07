#ifndef C1_SERVICES_TERMINAL_H
#define C1_SERVICES_TERMINAL_H

#include "core/status.h"

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

typedef enum {
    C1_TERMINAL_STOPPED = 0,
    C1_TERMINAL_RUNNING = 1,
    C1_TERMINAL_EXITED = 2,
    C1_TERMINAL_FAILED = 3
} c1_terminal_state;

typedef struct {
    int master_fd;
    pid_t child_pid; /* session supervisor, retained until reaped */
    pid_t shell_pid;
    int control_fd;
    bool direct_exec;
    c1_terminal_state state;
    int exit_code;
    char pending[512];
    size_t pending_offset;
    size_t pending_length;
    bool suspended;
} c1_terminal_session;

void c1_terminal_init(c1_terminal_session *session);
c1_status c1_terminal_start(c1_terminal_session *session,
                            unsigned int columns,
                            unsigned int rows);
c1_status c1_terminal_start_exec(c1_terminal_session *session,
                                 unsigned int columns, unsigned int rows,
                                 const char *path, char *const argv[]);
int c1_terminal_fd(const c1_terminal_session *session);
short c1_terminal_poll_events(const c1_terminal_session *session);
bool c1_terminal_is_running(c1_terminal_session *session);
bool c1_terminal_shell_is_foreground(c1_terminal_session *session);
c1_terminal_state c1_terminal_get_state(c1_terminal_session *session);
int c1_terminal_exit_code(const c1_terminal_session *session);
c1_status c1_terminal_write(c1_terminal_session *session, const void *bytes, size_t count);
ssize_t c1_terminal_read(c1_terminal_session *session, void *buffer, size_t capacity);
c1_status c1_terminal_flush(c1_terminal_session *session);
c1_status c1_terminal_suspend(c1_terminal_session *session, bool *was_running);
c1_status c1_terminal_resume(c1_terminal_session *session, bool was_running);
void c1_terminal_stop(c1_terminal_session *session);

#endif