#define _GNU_SOURCE
#include "update/log.h"
#include "platform/child_processes.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define LOG_BLOCK 4096U
#define LOG_MAX_ROTATIONS 16U
/* Allow the supervisor's own five-second shutdown before escalation. */
#define LOG_TERM_GRACE_MS 6000
#define LOG_KILL_GRACE_MS 2000

struct log_sink {
    int directory;
    int file;
    char name[NAME_MAX - 12];
    size_t limit;
    size_t used;
    unsigned int rotations;
};

static volatile sig_atomic_t stop_signal;

static void stopped(int number)
{
    stop_signal = number;
}

static int64_t milliseconds(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return -1;
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static int number(const char *text, unsigned long maximum, unsigned long *value)
{
    char *end;
    if (text == NULL || *text < '0' || *text > '9') return -1;
    errno = 0;
    *value = strtoul(text, &end, 10);
    return errno == 0 && *end == '\0' && *value <= maximum ? 0 : -1;
}

static void disable_sink(struct log_sink *sink)
{
    if (sink->file >= 0) (void)close(sink->file);
    if (sink->directory >= 0) (void)close(sink->directory);
    sink->file = sink->directory = -1;
}

/* Walk descriptors, never links. Only the leaf must be private; ancestors
 * must be owned by us/root and not writable by others (root sticky /tmp is
 * allowed). Missing components are created privately, never chmod/chowned. */
static int open_directory(char *path)
{
    char *part, *save = NULL;
    int fd, next;
    struct stat st;
    if (path[0] != '/') return -1;
    fd = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return -1;
    for (part = strtok_r(path, "/", &save); part != NULL;
         part = strtok_r(NULL, "/", &save)) {
        if (strcmp(part, ".") == 0 || strcmp(part, "..") == 0) goto fail;
        next = openat(fd, part, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (next < 0 && errno == ENOENT) {
            if (mkdirat(fd, part, 0700) != 0 && errno != EEXIST) goto fail;
            next = openat(fd, part, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        }
        if (next < 0) goto fail;
        (void)close(fd);
        fd = next;
        if (fstat(fd, &st) != 0 ||
            (st.st_uid != geteuid() && st.st_uid != 0) ||
            ((st.st_mode & 0022) != 0 &&
             !(st.st_uid == 0 && (st.st_mode & S_ISVTX) != 0))) goto fail;
    }
    if (fstat(fd, &st) != 0 || st.st_uid != geteuid() ||
        (st.st_mode & 0777) != 0700 || flock(fd, LOCK_EX | LOCK_NB) != 0) goto fail;
    return fd;
fail:
    (void)close(fd);
    return -1;
}

static void filename(const struct log_sink *sink, unsigned int index,
                     char out[NAME_MAX + 1])
{
    if (index == 0) (void)snprintf(out, NAME_MAX + 1, "%s", sink->name);
    else (void)snprintf(out, NAME_MAX + 1, "%s.%u", sink->name, index);
}

static int private_file(const struct stat *st)
{
    return S_ISREG(st->st_mode) && st->st_uid == geteuid() &&
           st->st_nlink == 1 && (st->st_mode & 0777) == 0600;
}

/* Validate every name before rotation, including files we would overwrite. */
static int validate_files(struct log_sink *sink, int trim_oversize)
{
    unsigned int index;
    struct stat st;
    char name[NAME_MAX + 1];
    if (fstat(sink->directory, &st) != 0 || st.st_nlink == 0 ||
        st.st_uid != geteuid() || (st.st_mode & 0777) != 0700) return -1;
    for (index = 0; index <= sink->rotations; ++index) {
        filename(sink, index, name);
        if (fstatat(sink->directory, name, &st, AT_SYMLINK_NOFOLLOW) != 0) {
            if (errno == ENOENT) continue;
            return -1;
        }
        if (!private_file(&st)) return -1;
        if (st.st_size < 0 || (uint64_t)st.st_size > sink->limit) {
            /* An old oversized log is unlinked, never truncated through a
             * descriptor another process could still hold. */
            if (!trim_oversize || unlinkat(sink->directory, name, 0) != 0) return -1;
        }
    }
    return 0;
}

static int open_log(struct log_sink *sink)
{
    struct stat st;
    sink->file = openat(sink->directory, sink->name,
                        O_WRONLY | O_APPEND | O_CREAT | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK, 0600);
    if (sink->file < 0 || fstat(sink->file, &st) != 0 || !private_file(&st) ||
        st.st_size < 0 || (uint64_t)st.st_size > sink->limit) return -1;
    sink->used = (size_t)st.st_size;
    return 0;
}

static int rotate(struct log_sink *sink)
{
    unsigned int index;
    char from[NAME_MAX + 1], to[NAME_MAX + 1];
    if (validate_files(sink, 0) != 0) return -1;
    (void)close(sink->file);
    sink->file = -1;
    filename(sink, sink->rotations, to);
    if (unlinkat(sink->directory, to, 0) != 0 && errno != ENOENT) return -1;
    for (index = sink->rotations; index > 0; --index) {
        filename(sink, index - 1, from);
        filename(sink, index, to);
        if (renameat(sink->directory, from, sink->directory, to) != 0 && errno != ENOENT) return -1;
    }
    return open_log(sink);
}

static void collect(struct log_sink *sink, const unsigned char *data, size_t length)
{
    struct stat opened, named;
    if (sink->directory < 0) return;
    if (validate_files(sink, 0) != 0 || fstat(sink->file, &opened) != 0 ||
        fstatat(sink->directory, sink->name, &named, AT_SYMLINK_NOFOLLOW) != 0 ||
        opened.st_dev != named.st_dev || opened.st_ino != named.st_ino ||
        (uint64_t)opened.st_size != sink->used) goto fail;
    while (length != 0) {
        size_t amount;
        ssize_t written;
        if (sink->used == sink->limit && rotate(sink) != 0) goto fail;
        amount = sink->limit - sink->used;
        if (amount > length) amount = length;
        written = write(sink->file, data, amount);
        if (written < 0 && errno == EINTR && !stop_signal) continue;
        if (written <= 0) goto fail;
        sink->used += (size_t)written;
        data += written;
        length -= (size_t)written;
    }
    return;
fail:
    /* No retries or stderr forwarding: a missing directory/ENOSPC must not
     * stop draining the pipe, block the service, or grow a fallback log. */
    disable_sink(sink);
}

static void initialize_sink(struct log_sink *sink, const char *path)
{
    char copy[PATH_MAX], *slash;
    size_t length = strlen(path);
    if (length >= sizeof(copy)) return;
    (void)memcpy(copy, path, length + 1);
    slash = strrchr(copy, '/');
    if (slash == NULL || slash == copy || slash[1] == '\0' ||
        strlen(slash + 1) >= sizeof(sink->name) ||
        strcmp(slash + 1, ".") == 0 || strcmp(slash + 1, "..") == 0) return;
    (void)strcpy(sink->name, slash + 1);
    *slash = '\0';
    sink->directory = open_directory(copy);
    if (sink->directory < 0) return;
    if (validate_files(sink, 1) != 0 || open_log(sink) != 0) disable_sink(sink);
}

static int direct_exec(char *const argv[])
{
    execvp(argv[0], argv);
    return errno == ENOENT ? 127 : 126;
}

/* As a Linux subreaper we also collect orphaned descendants that changed
 * session/process group. Never enumerate unrelated system processes. */
static int adopted_children(pid_t primary, int signal_number)
{
    int remaining = c1_child_processes_signal(primary, signal_number);
    return remaining < 0 ? 1 : remaining;
}

int c1_update_run_logged(const char *path, const char *limit_text,
                         const char *rotations_text, char *const argv[])
{
    struct log_sink sink = {.directory = -1, .file = -1};
    unsigned long limit, rotations;
    struct sigaction action = {0};
    sigset_t blocked, previous;
    int pipes[2], flags, status = 0, exited = 0, draining = 1, phase = 0;
    int64_t deadline = 0;
    pid_t child, parent = getpid();
    if (argv == NULL || argv[0] == NULL) return 2;
    if (number(limit_text, 16UL * 1024 * 1024, &limit) != 0 || limit == 0 ||
        number(rotations_text, LOG_MAX_ROTATIONS, &rotations) != 0)
        return direct_exec(argv);
    sink.limit = (size_t)limit;
    sink.rotations = (unsigned int)rotations;
    /* All setup failures happen before fork, so fallback cannot double-start
     * a command whose real status was 71/72/75 (or 126/127). */
    if (prctl(PR_SET_CHILD_SUBREAPER, 1) != 0) return direct_exec(argv);
    if (pipe2(pipes, O_CLOEXEC) != 0) return direct_exec(argv);
    flags = fcntl(pipes[0], F_GETFL);
    if (flags < 0 || fcntl(pipes[0], F_SETFL, flags | O_NONBLOCK) != 0) {
        (void)close(pipes[0]); (void)close(pipes[1]);
        return direct_exec(argv);
    }
    (void)sigemptyset(&blocked);
    (void)sigaddset(&blocked, SIGTERM);
    (void)sigaddset(&blocked, SIGINT);
    (void)sigaddset(&blocked, SIGHUP);
    if (sigprocmask(SIG_BLOCK, &blocked, &previous) != 0) {
        (void)close(pipes[0]); (void)close(pipes[1]);
        return direct_exec(argv);
    }
    stop_signal = 0;
    action.sa_handler = stopped;
    (void)sigemptyset(&action.sa_mask);
    if (sigaction(SIGTERM, &action, NULL) != 0 ||
        sigaction(SIGINT, &action, NULL) != 0 ||
        sigaction(SIGHUP, &action, NULL) != 0 || signal(SIGCHLD, SIG_DFL) == SIG_ERR) {
        (void)sigprocmask(SIG_SETMASK, &previous, NULL);
        (void)close(pipes[0]); (void)close(pipes[1]);
        return direct_exec(argv);
    }
    child = fork();
    if (child < 0) {
        (void)sigprocmask(SIG_SETMASK, &previous, NULL);
        (void)close(pipes[0]); (void)close(pipes[1]);
        return direct_exec(argv);
    }
    if (child == 0) {
        (void)close(pipes[0]);
        if (setpgid(0, 0) != 0 || prctl(PR_SET_PDEATHSIG, SIGTERM) != 0 ||
            getppid() != parent) _exit(125);
        if (dup2(pipes[1], STDOUT_FILENO) < 0 || dup2(pipes[1], STDERR_FILENO) < 0) _exit(125);
        if (pipes[1] > STDERR_FILENO) (void)close(pipes[1]);
        (void)signal(SIGTERM, SIG_DFL);
        (void)signal(SIGINT, SIG_DFL);
        (void)signal(SIGHUP, SIG_DFL);
        (void)sigprocmask(SIG_SETMASK, &previous, NULL);
        _exit(direct_exec(argv));
    }
    (void)setpgid(child, child);
    (void)close(pipes[1]);
    (void)sigprocmask(SIG_SETMASK, &previous, NULL);
    initialize_sink(&sink, path);
    for (;;) {
        struct pollfd event = {pipes[0], POLLIN | POLLHUP, 0};
        siginfo_t info;
        int64_t now = milliseconds();
        int descendants = 1;
        if (!exited) {
            (void)memset(&info, 0, sizeof(info));
            /* Keep the primary zombie until group cleanup, preventing PID /
             * process-group reuse while sending signals. */
            if (waitid(P_PID, (id_t)child, &info, WEXITED | WNOHANG | WNOWAIT) == 0 &&
                info.si_pid == child) exited = 1;
        }
        if (phase == 0 && (stop_signal || exited)) {
            phase = 1;
            deadline = now + LOG_TERM_GRACE_MS;
            (void)kill(-child, stop_signal ? stop_signal : SIGTERM);
            if (!exited) (void)kill(child, stop_signal ? stop_signal : SIGTERM);
        }
        if (phase != 0) {
            if (phase == 1 && (now < 0 || now >= deadline)) {
                phase = 2;
                deadline = now + LOG_KILL_GRACE_MS;
                (void)kill(-child, SIGKILL);
                if (!exited) (void)kill(child, SIGKILL);
            }
            descendants = adopted_children(child, phase == 2 ? SIGKILL : SIGTERM);
        }
        if (phase == 2 && (now < 0 || now >= deadline)) break;
        /* Bounded work per iteration: an endless writer cannot starve stop,
         * child-status observation or escalation. No line buffering. */
        if (draining) {
            unsigned char data[LOG_BLOCK];
            ssize_t count = read(pipes[0], data, sizeof(data));
            if (count > 0) { collect(&sink, data, (size_t)count); continue; }
            if (count == 0 || (count < 0 && errno != EAGAIN && errno != EINTR)) draining = 0;
        }
        if (phase && exited && !descendants && !draining) break;
        if (phase == 2 && (now < 0 || now >= deadline)) break;
        /* Idle logging must not wake the low-power device 50 times/second.
         * Pipe data and stop signals wake poll immediately; only bounded
         * shutdown cleanup needs the short timer. */
        (void)poll(draining ? &event : NULL, draining ? 1 : 0, phase ? 20 : 1000);
    }
    disable_sink(&sink);
    (void)close(pipes[0]);
    /* SIGKILL cleanup is bounded even for an uninterruptible kernel task. */
    if (waitpid(child, &status, WNOHANG) != child) return stop_signal ? 128 + stop_signal : 125;
    if (stop_signal) return 128 + stop_signal;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return WIFSIGNALED(status) ? 128 + WTERMSIG(status) : 125;
}
