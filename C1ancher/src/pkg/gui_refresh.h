#ifndef C1PKG_GUI_REFRESH_H
#define C1PKG_GUI_REFRESH_H

/* Private, fork-isolated repository job. Only the child calls the existing
 * signed/rollback-protected repository API. No cache file is read by the UI
 * while it is being published; the exact verified in-memory result crosses an
 * anonymous pipe. repo.c retains exclusive ownership of repo.lock and temps. */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

struct gui_refresh_result {
    uint32_t magic;
    int result, cached, initial;
    struct c1pkg_index index;
};
struct gui_refresh_job {
    pid_t pid;
    int result_fd, progress_fd, cancel_fd, cancelling;
    size_t received, progress_used;
    char progress[C1PKG_ERROR_MAX];
    struct gui_refresh_result *result;
};
#define GUI_REFRESH_MAGIC 0x43315246U

static void gui_refresh_init(struct gui_refresh_job *job)
{
    memset(job, 0, sizeof(*job));
    job->result_fd = job->progress_fd = job->cancel_fd = -1;
}

static volatile sig_atomic_t gui_worker_interrupted;
static void gui_worker_signal(int signo) { (void)signo; gui_worker_interrupted = 1; }
struct gui_worker_context { int cancel_fd, progress_fd; };

static int gui_worker_progress(const char *message, void *context)
{
    struct gui_worker_context *worker = context;
    struct pollfd cancel = {worker->cancel_fd, POLLIN, 0};
    if (gui_worker_interrupted || (poll(&cancel, 1U, 0) > 0 && cancel.revents)) return 1;
    if (message) {
        char record[C1PKG_ERROR_MAX] = "";
        snprintf(record, sizeof(record), "%s", message);
        /* <= PIPE_BUF: a nonblocking write is all-or-nothing. Dropping an old
         * progress notice is harmless; dropping a verified result is not. */
        ssize_t written = write(worker->progress_fd, record, sizeof(record));
        (void)written;
    }
    return 0;
}

static void gui_worker_close_inherited(int result, int progress, int cancel)
{
    DIR *directory = opendir("/proc/self/fd");
    if (directory) {
        struct dirent *entry;
        int own = dirfd(directory);
        while ((entry = readdir(directory)) != NULL) {
            char *end;
            long fd = strtol(entry->d_name, &end, 10);
            if (*end || fd < 0 || fd == own || fd == result || fd == progress || fd == cancel || fd == STDERR_FILENO) continue;
            (void)close((int)fd);
        }
        closedir(directory);
    } else {
        long limit = sysconf(_SC_OPEN_MAX);
        for (int fd = 0; fd < (limit > 0 ? limit : 65536); ++fd)
            if (fd != result && fd != progress && fd != cancel && fd != STDERR_FILENO) (void)close(fd);
    }
    /* Never inherit the terminal, display lease, application run lock or IME
     * socket. stderr remains diagnostic-only; stdin/stdout go to /dev/null. */
    int null_fd = open("/dev/null", O_RDWR);
    if (null_fd >= 0) {
        if (null_fd != STDIN_FILENO) (void)dup2(null_fd, STDIN_FILENO);
        if (null_fd != STDOUT_FILENO) (void)dup2(null_fd, STDOUT_FILENO);
        if (null_fd > STDERR_FILENO) close(null_fd);
    }
}

static int gui_refresh_pipe(int fd[2])
{
    if (pipe(fd)) return -1;
    if (fcntl(fd[0], F_SETFD, FD_CLOEXEC) < 0 || fcntl(fd[1], F_SETFD, FD_CLOEXEC) < 0 ||
        fcntl(fd[0], F_SETFL, O_NONBLOCK) < 0 || fcntl(fd[1], F_SETFL, O_NONBLOCK) < 0) {
        close(fd[0]); close(fd[1]); fd[0] = fd[1] = -1; return -1;
    }
    return 0;
}

static int gui_refresh_start(struct gui_refresh_job *job, const struct c1pkg_config *config)
{
    int output[2] = {-1, -1}, progress[2] = {-1, -1}, cancel[2] = {-1, -1};
    if (job->pid > 0) return 0; /* Coalesce refresh requests, never race a cache writer. */
    job->result = calloc(1U, sizeof(*job->result));
    if (!job->result || gui_refresh_pipe(output) || gui_refresh_pipe(progress) || gui_refresh_pipe(cancel)) goto failed;
    job->pid = fork();
    if (job->pid < 0) goto failed;
    if (!job->pid) {
        struct gui_worker_context worker = {cancel[0], progress[1]};
        struct sigaction action;
        struct gui_refresh_result *reply = job->result;
        size_t sent = 0U;
        char error[C1PKG_ERROR_MAX] = "";
        gui_worker_interrupted = 0;
        memset(&action, 0, sizeof(action)); action.sa_handler = gui_worker_signal;
        sigemptyset(&action.sa_mask);
        (void)sigaction(SIGTERM, &action, NULL); (void)sigaction(SIGHUP, &action, NULL);
        (void)sigaction(SIGINT, &action, NULL);
        action.sa_handler = SIG_IGN; (void)sigaction(SIGPIPE, &action, NULL);
        gui_worker_close_inherited(output[1], progress[1], cancel[0]);
        c1pkg_set_progress(gui_worker_progress, &worker);
        reply->magic = GUI_REFRESH_MAGIC;
        /* First publish a re-verified cache, without waiting for the network.
         * Parsing/signature I/O also stays off the navigation thread. */
        reply->result = -1;
        reply->cached = c1pkg_repo_load_cached(config, &reply->index, NULL, 0U) == 0;
        for (int phase = 0; phase < 2; ++phase) {
            reply->initial = phase == 0;
            if (phase == 1) {
                reply->result = c1pkg_repo_refresh(config, &reply->index, error, sizeof(error));
                if (reply->result != 0) memset(&reply->index, 0, sizeof(reply->index));
                reply->cached = 0;
            }
            sent = 0U;
            while (sent < sizeof(*reply) && !gui_worker_progress(NULL, &worker)) {
                ssize_t n = write(output[1], (const unsigned char *)reply + sent, sizeof(*reply) - sent);
                if (n > 0) sent += (size_t)n;
                else if (n < 0 && (errno == EAGAIN || errno == EINTR)) {
                    struct pollfd fds[2] = {{output[1], POLLOUT, 0}, {cancel[0], POLLIN, 0}};
                    (void)poll(fds, 2U, 100);
                } else break;
            }
            if (sent != sizeof(*reply)) break;
        }
        close(output[1]); close(progress[1]); close(cancel[0]);
        _exit(sent == sizeof(*reply) ? 0 : 1);
    }
    close(output[1]); close(progress[1]); close(cancel[0]);
    job->result_fd = output[0]; job->progress_fd = progress[0]; job->cancel_fd = cancel[1];
    job->received = job->progress_used = 0U; job->cancelling = 0;
    return 1;
failed:
    for (size_t i = 0; i < 2; ++i) {
        if (output[i] >= 0) close(output[i]);
        if (progress[i] >= 0) close(progress[i]);
        if (cancel[i] >= 0) close(cancel[i]);
    }
    free(job->result); gui_refresh_init(job); return -1;
}

static void gui_refresh_cancel(struct gui_refresh_job *job)
{
    if (job->pid <= 0) return;
    job->cancelling = 1;
    /* EOF/HUP is the cancellation signal. No kill(0), process-group signals,
     * install callback changes or parent-side deletion of shared cache temps. */
    if (job->cancel_fd >= 0) { close(job->cancel_fd); job->cancel_fd = -1; }
}

/* 0: running, 1: complete, -1: cancelled/broken. Result is consumed only after
 * both the full bounded payload and a successful waitpid of our own child. */
static int gui_refresh_poll(struct gui_refresh_job *job, char *progress, size_t size)
{
    int status = 0;
    pid_t reaped;
    if (job->pid <= 0) return 0;
    for (unsigned int i = 0; i < 8U; ++i) {
        ssize_t n = read(job->progress_fd, job->progress + job->progress_used,
                         sizeof(job->progress) - job->progress_used);
        if (n <= 0) break;
        job->progress_used += (size_t)n;
        if (job->progress_used == sizeof(job->progress)) {
            job->progress[sizeof(job->progress) - 1U] = '\0';
            if (progress && size) snprintf(progress, size, "%s", job->progress);
            job->progress_used = 0U;
        }
    }
    while (job->received < sizeof(*job->result)) {
        ssize_t n = read(job->result_fd, (unsigned char *)job->result + job->received,
                         sizeof(*job->result) - job->received);
        if (n > 0) job->received += (size_t)n;
        else if (n < 0 && errno == EINTR) continue;
        else break;
    }
    if (job->received == sizeof(*job->result) && job->result->initial) {
        job->received = 0U;
        return job->result->magic == GUI_REFRESH_MAGIC &&
               job->result->index.count <= C1PKG_MAX_PACKAGES && !job->cancelling ? 2 : 0;
    }
    reaped = waitpid(job->pid, &status, WNOHANG);
    if (!reaped || (reaped < 0 && errno == EINTR)) return 0;
    /* The child can write its final bytes between read(EAGAIN) and waitpid.
     * Once reaped, drain the pipe again before checking completeness. */
    while (job->received < sizeof(*job->result)) {
        ssize_t n = read(job->result_fd, (unsigned char *)job->result + job->received,
                         sizeof(*job->result) - job->received);
        if (n > 0) job->received += (size_t)n;
        else if (n < 0 && errno == EINTR) continue;
        else break;
    }
    job->pid = 0;
    close(job->result_fd); close(job->progress_fd);
    if (job->cancel_fd >= 0) close(job->cancel_fd);
    job->result_fd = job->progress_fd = job->cancel_fd = -1;
    return reaped > 0 && !job->cancelling && WIFEXITED(status) && WEXITSTATUS(status) == 0 &&
           job->received == sizeof(*job->result) && job->result->magic == GUI_REFRESH_MAGIC &&
           job->result->index.count <= C1PKG_MAX_PACKAGES ? 1 : -1;
}

static void gui_refresh_dispose(struct gui_refresh_job *job)
{
    gui_refresh_cancel(job);
    /* Used only on GUI exit/exec and before a synchronous mutation. Existing
     * transport cancellation polls at <=100ms, reaps its own curl group and
     * removes temporary files while still owning repo.lock. Never SIGKILL the
     * writer mid-commit or delete a successor's temporary files. */
    while (job->pid > 0) {
        (void)gui_refresh_poll(job, NULL, 0U);
        (void)poll(NULL, 0U, 10);
    }
    free(job->result);
    gui_refresh_init(job);
}
#endif
