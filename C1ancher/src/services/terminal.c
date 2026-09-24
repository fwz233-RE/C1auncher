#define _GNU_SOURCE 1
#define _XOPEN_SOURCE 600

#include "services/terminal.h"
#include "platform/liveness.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define C1_TERMINAL_STOP_GRACE_MS 500

static int64_t monotonic_milliseconds(void)
{
    struct timespec value;

    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) {
        return -1;
    }
    return (int64_t)value.tv_sec * 1000 + value.tv_nsec / 1000000;
}

static void reset_signals(void)
{
    struct sigaction action;
    sigset_t mask;
    int signal_number;

    memset(&action, 0, sizeof(action));
    action.sa_handler = SIG_DFL;
    sigemptyset(&action.sa_mask);
    for (signal_number = 1; signal_number < NSIG; ++signal_number) {
        if (signal_number != SIGKILL && signal_number != SIGSTOP) {
            (void)sigaction(signal_number, &action, NULL);
        }
    }
    sigemptyset(&mask);
    (void)sigprocmask(SIG_SETMASK, &mask, NULL);
}

static void close_master(c1_terminal_session *session)
{
    if (session->master_fd >= 0) {
        close(session->master_fd);
        session->master_fd = -1;
    }
    if (session->control_fd >= 0) {
        close(session->control_fd);
        session->control_fd = -1;
    }
    session->pending_offset = 0U;
    session->pending_length = 0U;
}

static void reap_child(c1_terminal_session *session, bool block)
{
    int status;
    pid_t result;

    if (session == NULL || session->child_pid <= 0) {
        return;
    }
    do {
        result = waitpid(session->child_pid, &status, block ? 0 : WNOHANG);
    } while (result < 0 && errno == EINTR);
    if (result != session->child_pid) {
        return;
    }
    session->child_pid = -1;
    close_master(session);
    if (WIFEXITED(status)) {
        session->exit_code = WEXITSTATUS(status);
        session->state = C1_TERMINAL_EXITED;
    } else if (WIFSIGNALED(status)) {
        session->exit_code = 128 + WTERMSIG(status);
        session->state = C1_TERMINAL_EXITED;
    } else {
        session->exit_code = 128;
        session->state = C1_TERMINAL_FAILED;
    }
}

void c1_terminal_init(c1_terminal_session *session)
{
    if (session == NULL) {
        return;
    }
    memset(session, 0, sizeof(*session));
    session->master_fd = -1;
    session->child_pid = -1;
    session->shell_pid = -1;
    session->control_fd = -1;
    session->state = C1_TERMINAL_STOPPED;
}

static int open_pty_master(char *slave_name, size_t capacity)
{
    int master;
    char *name;

    master = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (master < 0 || grantpt(master) != 0 || unlockpt(master) != 0) {
        if (master >= 0) {
            close(master);
        }
        return -1;
    }
    name = ptsname(master);
    if (name == NULL || strlen(name) + 1U > capacity) {
        close(master);
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(slave_name, name, strlen(name) + 1U);
    return master;
}

static void child_exec(int master,
                       const char *slave_name,
                       unsigned int columns,
                       unsigned int rows,
                       const char *path, char *const argv[])
{
    int slave;
    struct winsize window;

    reset_signals();
    if (prctl(PR_SET_PDEATHSIG, SIGHUP) != 0 || getppid() == 1 || setsid() < 0) {
        _exit(126);
    }
    slave = open(slave_name, O_RDWR | O_NOCTTY);
    if (slave < 0 || ioctl(slave, TIOCSCTTY, 0) != 0) {
        _exit(126);
    }
    memset(&window, 0, sizeof(window));
    window.ws_col = (unsigned short)columns;
    window.ws_row = (unsigned short)rows;
    if (ioctl(slave, TIOCSWINSZ, &window) != 0 ||
        dup2(slave, STDIN_FILENO) < 0 ||
        dup2(slave, STDOUT_FILENO) < 0 ||
        dup2(slave, STDERR_FILENO) < 0) {
        _exit(126);
    }
    if (slave > STDERR_FILENO) {
        close(slave);
    }
    close(master);
    (void)setenv("TERM", "xterm-256color", 1);
    {
        char terminal_columns[16], terminal_rows[16];
        snprintf(terminal_columns, sizeof(terminal_columns), "%u", columns);
        snprintf(terminal_rows, sizeof(terminal_rows), "%u", rows);
        (void)setenv("COLUMNS", terminal_columns, 1);
        (void)setenv("LINES", terminal_rows, 1);
    }
    (void)setenv("PATH", "/usr/data/c1/bin:/sbin:/usr/sbin:/bin:/usr/bin", 1);
    (void)setenv("SHELL", "/bin/bash", 1);
    (void)setenv("HOME", "/root", 1);
    (void)setenv("USER", "root", 1);
    (void)setenv("LOGNAME", "root", 1);
    (void)setenv("C1_C1ANCHER_TERMINAL", "1", 1);
    if (access("/usr/data", X_OK) == 0 && chdir("/usr/data") != 0 && chdir("/") != 0) {
        _exit(126);
    }
    if (path != NULL) {
        execv(path, argv);
        _exit(127);
    }
    execl("/bin/bash", "bash", "--noprofile", "--norc", "-i", (char *)NULL);
    (void)setenv("SHELL", "/bin/sh", 1);
    execl("/bin/sh", "sh", "-i", (char *)NULL);
    _exit(127);
}

static void session_child_changed(int signal_number)
{
    (void)signal_number;
}

static void supervise_terminal(int master, const char *slave_name,
                               unsigned int columns, unsigned int rows,
                               const char *path, char *const argv[],
                               int control, int report)
{
    pid_t shell;
    int status = 0;
    bool exited = false;
    sigset_t blocked, original;
    struct sigaction action;
    reset_signals();
    memset(&action, 0, sizeof(action));
    action.sa_handler = session_child_changed;
    action.sa_flags = SA_NOCLDSTOP;
    sigemptyset(&action.sa_mask);
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGCHLD);
    if (sigaction(SIGCHLD, &action, NULL) != 0 ||
        sigprocmask(SIG_BLOCK, &blocked, &original) != 0) _exit(126);
    c1_liveness_close();
    {
        DIR *fds = opendir("/proc/self/fd");
        struct dirent *entry;
        if (fds == NULL) _exit(126);
        while ((entry = readdir(fds)) != NULL) {
            char *end;
            long fd = strtol(entry->d_name, &end, 10);
            if (*end == '\0' && fd > STDERR_FILENO && fd != master &&
                fd != control && fd != report && fd != dirfd(fds))
                (void)close((int)fd);
        }
        (void)closedir(fds);
    }
    if (c1_descendants_adopt() != 0) _exit(126);
    shell = fork();
    if (shell < 0) _exit(126);
    if (shell == 0) {
        close(control);
        close(report);
        child_exec(master, slave_name, columns, rows, path, argv);
    }
    if (write(report, &shell, sizeof(shell)) != (ssize_t)sizeof(shell)) {
        (void)c1_descendants_cleanup(C1_TERMINAL_STOP_GRACE_MS);
        _exit(126);
    }
    close(report);
    for (;;) {
        struct pollfd command = {control, POLLIN, 0};
        pid_t result = waitpid(shell, &status, WNOHANG);
        if (result == shell) { exited = true; break; }
        if (result < 0 && errno != EINTR) break;
        {
            int event = ppoll(&command, 1U, NULL, &original);
            if (event > 0 || (event < 0 && errno != EINTR)) break;
        }
    }
    /* Cleanup does not depend on shell survival or a foreground group leader.
     * The subreaper owns every PID it signals, including detached grandchildren. */
    (void)c1_descendants_cleanup(C1_TERMINAL_STOP_GRACE_MS);
    close(master);
    close(control);
    _exit(exited && WIFEXITED(status) ? WEXITSTATUS(status) :
          exited && WIFSIGNALED(status) ? 128 + WTERMSIG(status) : 0);
}

static c1_status start_session(c1_terminal_session *session,
                               unsigned int columns, unsigned int rows,
                               const char *path, char *const argv[])
{
    char slave_name[64];
    int master;
    pid_t child;
    int flags;
    int control[2], report[2];

    if (session == NULL || columns == 0U || rows == 0U || columns > UINT16_MAX ||
        rows > UINT16_MAX) {
        return C1_STATUS_INVALID_ARGUMENT;
    }
    if (c1_terminal_is_running(session)) {
        return C1_STATUS_OK;
    }
    c1_terminal_stop(session);
    if (session->child_pid > 0) return C1_STATUS_UNAVAILABLE;
    master = open_pty_master(slave_name, sizeof(slave_name));
    if (master < 0) {
        session->state = C1_TERMINAL_FAILED;
        return C1_STATUS_IO_ERROR;
    }
    if (pipe2(control, O_CLOEXEC) != 0) {
        close(master);
        return C1_STATUS_IO_ERROR;
    }
    if (pipe2(report, O_CLOEXEC) != 0) {
        close(master); close(control[0]); close(control[1]);
        return C1_STATUS_IO_ERROR;
    }
    child = fork();
    if (child < 0) {
        close(master); close(control[0]); close(control[1]);
        close(report[0]); close(report[1]);
        session->state = C1_TERMINAL_FAILED;
        return C1_STATUS_IO_ERROR;
    }
    if (child == 0) {
        close(control[1]); close(report[0]);
        supervise_terminal(master, slave_name, columns, rows, path, argv,
                           control[0], report[1]);
    }
    close(control[0]); close(report[1]);
    {
        ssize_t count;
        do { count = read(report[0], &session->shell_pid, sizeof(session->shell_pid)); }
        while (count < 0 && errno == EINTR);
        close(report[0]);
        if (count != (ssize_t)sizeof(session->shell_pid)) {
            close(master); close(control[1]);
            (void)kill(child, SIGKILL);
            while (waitpid(child, NULL, 0) < 0 && errno == EINTR) {}
            return C1_STATUS_IO_ERROR;
        }
    }
    session->control_fd = control[1];
    session->direct_exec = path != NULL;
    flags = fcntl(master, F_GETFL, 0);
    if (flags < 0 || fcntl(master, F_SETFL, flags | O_NONBLOCK) != 0) {
        close(master);
        close(session->control_fd);
        session->control_fd = -1;
        kill(child, SIGKILL);
        while (waitpid(child, NULL, 0) < 0 && errno == EINTR) {
        }
        session->state = C1_TERMINAL_FAILED;
        return C1_STATUS_IO_ERROR;
    }
    session->master_fd = master;
    session->child_pid = child;
    session->state = C1_TERMINAL_RUNNING;
    session->exit_code = 0;
    session->pending_offset = 0U;
    session->pending_length = 0U;
    session->suspended = false;
    return C1_STATUS_OK;
}

c1_status c1_terminal_start(c1_terminal_session *session,
                            unsigned int columns, unsigned int rows)
{
    return start_session(session, columns, rows, NULL, NULL);
}

c1_status c1_terminal_start_exec(c1_terminal_session *session,
                                 unsigned int columns, unsigned int rows,
                                 const char *path, char *const argv[])
{
    if (path == NULL || path[0] != '/' || argv == NULL || argv[0] == NULL)
        return C1_STATUS_INVALID_ARGUMENT;
    if (session == NULL || c1_terminal_is_running(session))
        return C1_STATUS_INVALID_ARGUMENT;
    return start_session(session, columns, rows, path, argv);
}

int c1_terminal_fd(const c1_terminal_session *session)
{
    return session != NULL && session->state == C1_TERMINAL_RUNNING ? session->master_fd : -1;
}

short c1_terminal_poll_events(const c1_terminal_session *session)
{
    short events = POLLIN;

    if (session != NULL && session->pending_length > 0U) {
        events |= POLLOUT;
    }
    return events;
}

bool c1_terminal_is_running(c1_terminal_session *session)
{
    if (session == NULL) {
        return false;
    }
    reap_child(session, false);
    return session->state == C1_TERMINAL_RUNNING && session->master_fd >= 0;
}

bool c1_terminal_shell_is_foreground(c1_terminal_session *session)
{
    pid_t foreground;

    if (!c1_terminal_is_running(session) || session->child_pid <= 0) {
        return false;
    }
    foreground = tcgetpgrp(session->master_fd);
    return !session->direct_exec && foreground > 0 && foreground == session->shell_pid;
}

c1_terminal_state c1_terminal_get_state(c1_terminal_session *session)
{
    if (session == NULL) {
        return C1_TERMINAL_FAILED;
    }
    (void)c1_terminal_is_running(session);
    return session->state;
}

int c1_terminal_exit_code(const c1_terminal_session *session)
{
    return session != NULL ? session->exit_code : 128;
}

c1_status c1_terminal_resize(c1_terminal_session *session, unsigned int columns, unsigned int rows)
{
    if (!session || columns == 0 || rows == 0 || columns > 1000 || rows > 1000)
        return C1_STATUS_INVALID_ARGUMENT;
    if (session->master_fd < 0) return C1_STATUS_OK;
    struct winsize window = {0};
    window.ws_col = (unsigned short)columns;
    window.ws_row = (unsigned short)rows;
    return ioctl(session->master_fd, TIOCSWINSZ, &window) == 0 ? C1_STATUS_OK : C1_STATUS_IO_ERROR;
}

c1_status c1_terminal_flush(c1_terminal_session *session)
{
    while (session != NULL && session->pending_length > 0U) {
        ssize_t written = write(session->master_fd,
                                session->pending + session->pending_offset,
                                session->pending_length);

        if (written > 0) {
            session->pending_offset += (size_t)written;
            session->pending_length -= (size_t)written;
            if (session->pending_length == 0U) {
                session->pending_offset = 0U;
            }
            continue;
        }
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return C1_STATUS_OK;
        }
        if (written < 0 && errno == EIO) {
            reap_child(session, false);
            if (session->state == C1_TERMINAL_RUNNING && session->child_pid > 0) {
                return C1_STATUS_OK;
            }
        } else {
            reap_child(session, false);
        }
        return C1_STATUS_IO_ERROR;
    }
    return C1_STATUS_OK;
}

c1_status c1_terminal_write(c1_terminal_session *session, const void *bytes, size_t count)
{
    const char *source = bytes;

    if (session == NULL || bytes == NULL || count == 0U || !c1_terminal_is_running(session)) {
        return C1_STATUS_INVALID_ARGUMENT;
    }
    if (session->pending_offset > 0U &&
        session->pending_offset + session->pending_length + count > sizeof(session->pending)) {
        memmove(session->pending,
                session->pending + session->pending_offset,
                session->pending_length);
        session->pending_offset = 0U;
    }
    if (count > sizeof(session->pending) - session->pending_offset - session->pending_length) {
        return C1_STATUS_UNAVAILABLE;
    }
    memcpy(session->pending + session->pending_offset + session->pending_length, source, count);
    session->pending_length += count;
    return c1_terminal_flush(session);
}

ssize_t c1_terminal_read(c1_terminal_session *session, void *buffer, size_t capacity)
{
    ssize_t count;

    if (session == NULL || buffer == NULL || capacity == 0U || session->master_fd < 0) {
        errno = EINVAL;
        return -1;
    }
    do {
        count = read(session->master_fd, buffer, capacity);
    } while (count < 0 && errno == EINTR);
    if (count > 0) {
        return count;
    }
    if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        return 0;
    }
    if (count == 0 || (count < 0 && errno == EIO)) {
        reap_child(session, false);
        return 0;
    }
    return -1;
}

c1_status c1_terminal_suspend(c1_terminal_session *session, bool *was_running)
{
    pid_t foreground;

    if (session == NULL || was_running == NULL) {
        return C1_STATUS_INVALID_ARGUMENT;
    }
    *was_running = c1_terminal_is_running(session);
    if (!*was_running) {
        return C1_STATUS_OK;
    }
    foreground = tcgetpgrp(session->master_fd);
    if (foreground <= 0 || kill(-foreground, SIGSTOP) != 0) {
        return C1_STATUS_IO_ERROR;
    }
    session->suspended = true;
    return C1_STATUS_OK;
}

c1_status c1_terminal_resume(c1_terminal_session *session, bool was_running)
{
    pid_t foreground;

    if (session == NULL) {
        return C1_STATUS_INVALID_ARGUMENT;
    }
    if (!was_running || !session->suspended) {
        return C1_STATUS_OK;
    }
    foreground = session->master_fd >= 0 ? tcgetpgrp(session->master_fd) : -1;
    if (foreground <= 0 || kill(-foreground, SIGCONT) != 0) {
        c1_terminal_stop(session);
        return C1_STATUS_IO_ERROR;
    }
    session->suspended = false;
    return C1_STATUS_OK;
}

void c1_terminal_stop(c1_terminal_session *session)
{
    int64_t now, deadline;
    if (session == NULL) return;
    /* Closing the control pipe asks the per-session subreaper to clean every
     * descendant, even when the interactive shell already exited. */
    close_master(session);
    now = monotonic_milliseconds();
    deadline = now < 0 ? -1 : now + C1_TERMINAL_STOP_GRACE_MS + 1000;
    while (session->child_pid > 0) {
        struct timespec pause = {0, 10000000L};
        reap_child(session, false);
        if (session->child_pid <= 0) break;
        now = monotonic_milliseconds();
        if (deadline < 0 || now < 0 || now >= deadline) {
            /* This is our unreaped child: the PID cannot have been reused. */
            (void)kill(session->child_pid, SIGKILL);
            reap_child(session, false);
            break;
        }
        (void)nanosleep(&pause, NULL);
    }
    session->state = C1_TERMINAL_STOPPED;
    session->exit_code = 0;
    session->suspended = false;
}
