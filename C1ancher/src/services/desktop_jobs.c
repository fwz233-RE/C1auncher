#define _DEFAULT_SOURCE 1
#include "services/desktop_jobs.h"
#include "update/update.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int64_t after(int64_t now, int64_t interval)
{
    return now > INT64_MAX - interval ? INT64_MAX : now + interval;
}

void c1_desktop_job_init(c1_desktop_job *job)
{
    if (job == NULL) return;
    memset(job, 0, sizeof(*job));
    job->pid = job->fd = -1;
}

static int private_pipe_fd(int descriptor)
{
    if (descriptor <= STDERR_FILENO) {
        int duplicate = fcntl(descriptor, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
        (void)close(descriptor);
        return duplicate;
    }
    if (fcntl(descriptor, F_SETFD, FD_CLOEXEC) != 0) {
        (void)close(descriptor);
        return -1;
    }
    return descriptor;
}

static int child_signals(void)
{
    struct sigaction action;
    sigset_t empty;
    memset(&action, 0, sizeof(action));
    action.sa_handler = SIG_DFL;
    if (sigemptyset(&action.sa_mask) != 0 || sigemptyset(&empty) != 0) return -1;
    /* exec resets caught handlers, but preserves SIG_IGN and the signal mask.
     * In particular SIGCHLD must not auto-reap the updater's curl children. */
    for (int number = 1; number < NSIG; ++number) {
        if (number == SIGKILL || number == SIGSTOP) continue;
        if (sigaction(number, &action, NULL) != 0 && errno != EINVAL) return -1;
    }
    return sigprocmask(SIG_SETMASK, &empty, NULL);
}

static int close_inherited_fds(void)
{
    DIR *directory = opendir("/proc/self/fd");
    struct dirent *entry;
    int result = 0;
    /* Linux helpers already depend on procfs. Fail closed if enumeration is
     * unavailable rather than guess a limit and leak a high inherited fd. */
    if (directory == NULL) return -1;
    for (;;) {
        char *end;
        long descriptor;
        errno = 0;
        entry = readdir(directory);
        if (entry == NULL) {
            if (errno != 0) result = -1;
            break;
        }
        errno = 0;
        descriptor = strtol(entry->d_name, &end, 10);
        if (errno == 0 && end != entry->d_name && *end == '\0' &&
            descriptor > STDERR_FILENO && descriptor <= INT_MAX && descriptor != dirfd(directory)) {
            if (close((int)descriptor) != 0 && errno != EBADF) { result = -1; break; }
        }
    }
    if (closedir(directory) != 0) result = -1;
    return result;
}

bool c1_desktop_job_start(c1_desktop_job *job, int kind, int64_t now)
{
    int output[2];
    pid_t child;
    if (job == NULL || job->pid > 0 || job->fd >= 0 || now < 0 || (kind != 1 && kind != 2)) return false;
    if (pipe(output) != 0) return false;
    /* Reserve descriptors >=3 before dup2, even if the caller closed stdio. */
    output[0] = private_pipe_fd(output[0]);
    output[1] = private_pipe_fd(output[1]);
    if (output[0] < 0 || output[1] < 0 || fcntl(output[0], F_SETFL, O_NONBLOCK) != 0) {
        if (output[0] >= 0) (void)close(output[0]);
        if (output[1] >= 0) (void)close(output[1]);
        return false;
    }
    child = fork();
    if (child < 0) { (void)close(output[0]); (void)close(output[1]); return false; }
    if (child == 0) {
        int null;
        char *const package_arguments[] = {"c1pkg", "desktop-summary", NULL};
        char *const update_arguments[] = {"c1updater", "check-configured", NULL};
        if (setpgid(0, 0) != 0 || child_signals() != 0) _exit(126);
        null = open("/dev/null", O_RDWR | O_CLOEXEC);
        if (null < 0 || dup2(null, STDIN_FILENO) < 0 || dup2(null, STDERR_FILENO) < 0 ||
            dup2(output[1], STDOUT_FILENO) < 0) _exit(126);
        /* dup2(fd, fd) preserves CLOEXEC if /dev/null originally became fd 0
         * or 2. All three standard descriptors must survive the exec. */
        for (int descriptor = 0; descriptor <= STDERR_FILENO; ++descriptor)
            if (fcntl(descriptor, F_SETFD, 0) != 0) _exit(126);
        if (close_inherited_fds() != 0) _exit(126);
        if (kind == 1) execv("/usr/data/c1/bin/c1pkg", package_arguments);
        else execv("/usr/data/c1/bin/c1updater", update_arguments);
        _exit(127);
    }
    (void)setpgid(child, child);
    (void)close(output[1]);
    c1_desktop_job_init(job);
    job->pid = child;
    job->fd = output[0];
    job->kind = kind;
    job->deadline = after(now, kind == 2 ? C1_DESKTOP_CORE_JOB_TIMEOUT_MS : C1_DESKTOP_PACKAGE_JOB_TIMEOUT_MS);
    return true;
}

static void signal_owned_child(c1_desktop_job *job, int number)
{
    /* Called only while our direct child is still unreaped: its PID cannot be
     * reused. The direct-PID fallback covers cancellation before setpgid. */
    if (job->pid > 0 && kill(-job->pid, number) != 0 && errno == ESRCH)
        (void)kill(job->pid, number);
}

void c1_desktop_job_cancel(c1_desktop_job *job, int64_t now)
{
    if (job == NULL || job->pid <= 0 || job->cancelled) return;
    job->cancelled = true;
    signal_owned_child(job, SIGTERM);
    job->deadline = after(now < 0 ? 0 : now,
                          job->kind == 2 ? C1_DESKTOP_CORE_CANCEL_GRACE_MS : C1_DESKTOP_PACKAGE_CANCEL_GRACE_MS);
}

static void drain(c1_desktop_job *job, int64_t now)
{
    /* Never monopolize the event loop, even under signal interruption or a
     * continuously writing child. The first excess byte invalidates output. */
    for (unsigned attempt = 0; attempt < 4U && job->fd >= 0 && !job->failed; ++attempt) {
        char buffer[C1_DESKTOP_JOB_OUTPUT_CAPACITY];
        ssize_t amount = read(job->fd, buffer, sizeof(buffer));
        if (amount < 0 && errno == EINTR) continue;
        if (amount < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        if (amount == 0) { (void)close(job->fd); job->fd = -1; return; }
        if (amount < 0 || job->used >= sizeof(job->output) ||
            (size_t)amount > sizeof(job->output) - 1U - job->used ||
            memchr(buffer, '\0', (size_t)amount) != NULL) {
            job->failed = true;
            /* poll clears pid BEFORE its post-reap drain. Never signal a
             * recycled PGID when that final read discovers excessive output. */
            c1_desktop_job_cancel(job, now);
            return;
        }
        memcpy(job->output + job->used, buffer, (size_t)amount);
        job->used += (size_t)amount;
        job->output[job->used] = '\0';
    }
}

int c1_desktop_job_poll(c1_desktop_job *job, int64_t now)
{
    int status = 0;
    pid_t result;
    bool success;
    if (job == NULL || job->pid <= 0) return 0;
    /* Reap first; deadline/overflow handling must not signal after ownership
     * ends, including ECHILD if another process-wide reaper consumed status. */
    result = waitpid(job->pid, &status, WNOHANG);
    if (result == job->pid || (result < 0 && errno != EINTR)) {
        job->pid = -1;
        drain(job, now);
        /* No EOF means a descendant still holds stdout. Treat the incomplete
         * stream as failure rather than accepting a valid-looking prefix. */
        success = result > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0 &&
                  now < job->deadline && !job->cancelled && !job->failed && job->fd < 0;
        if (job->fd >= 0) (void)close(job->fd);
        job->fd = -1;
        if (!success) { job->used = 0; job->output[0] = '\0'; }
        return success ? 1 : -1;
    }
    drain(job, now);
    if (now >= job->deadline) {
        if (!job->cancelled) c1_desktop_job_cancel(job, now);
        else if (!job->killed) {
            signal_owned_child(job, SIGKILL);
            job->killed = true;
        }
    }
    return 0;
}

static bool alphanumeric(unsigned char character)
{
    return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'z') ||
           (character >= 'A' && character <= 'Z');
}

bool c1_desktop_update_parse(const char *text, size_t size, uint64_t *sequence, bool *available)
{
    static const char prefix[] = "C1UPDATE-CHECK 1\nS\t";
    const char *cursor, *end, *version;
    uint64_t parsed = 0;
    size_t version_size;
    if (text == NULL || sequence == NULL || available == NULL || size < sizeof(prefix) ||
        size >= C1_DESKTOP_JOB_OUTPUT_CAPACITY || memchr(text, '\0', size) != NULL ||
        memcmp(text, prefix, sizeof(prefix) - 1U) != 0) return false;
    cursor = text + sizeof(prefix) - 1U;
    end = text + size;
    /* Match the updater manifest's canonical positive uint64, not scanf's
     * whitespace/sign/overflow-tolerant conversion. */
    if (*cursor < '1' || *cursor > '9') return false;
    while (cursor < end && *cursor >= '0' && *cursor <= '9') {
        unsigned digit = (unsigned)(*cursor++ - '0');
        if (parsed > (UINT64_MAX - digit) / 10U) return false;
        parsed = parsed * 10U + digit;
    }
    if ((size_t)(end - cursor) < 3U || memcmp(cursor, "\nV\t", 3U) != 0) return false;
    version = cursor += 3U;
    while (cursor < end && *cursor != '\n') {
        unsigned char character = (unsigned char)*cursor++;
        if (!alphanumeric(character) && character != '.' && character != '+' &&
            character != '_' && character != '-') return false;
    }
    version_size = (size_t)(cursor - version);
    if (version_size == 0U || version_size > C1_UPDATE_TOKEN_MAX ||
        !alphanumeric((unsigned char)version[0]) || !alphanumeric((unsigned char)version[version_size - 1U]) ||
        end - cursor != 5 || memcmp(cursor, "\nA\t", 3U) != 0 ||
        (cursor[3] != '0' && cursor[3] != '1') || cursor[4] != '\n') return false;
    *sequence = parsed;
    *available = cursor[3] == '1';
    return true;
}
