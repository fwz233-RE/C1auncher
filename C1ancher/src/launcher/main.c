#define _GNU_SOURCE 1

#include "launcher/cleanup.h"
#include "launcher/policy.h"
#include "platform/liveness.h"
#include "platform/shutdown.h"

#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define C1_CHILD_STOP_GRACE_MS 3000
#define C1_LAUNCHER_PATH_MAX 4096U

static int upstream_fd = -1;
static struct c1_shutdown_client shutdown_upstream = {-1, 0, 0};
static volatile sig_atomic_t stop_requested;
static volatile sig_atomic_t child_pid = -1;
static pid_t maintenance_pid = -1;
static int maintenance_finished;
static int64_t maintenance_next;

static void maintenance_start(int64_t now)
{
    if (maintenance_finished || maintenance_pid > 0 || now < maintenance_next) return;
    maintenance_next = now + 5000;
    pid_t parent = getpid();
    maintenance_pid = fork();
    if (maintenance_pid != 0) return;
    (void)signal(SIGTERM, SIG_DFL);
    (void)signal(SIGINT, SIG_DFL);
    (void)signal(SIGHUP, SIG_DFL);
    sigset_t empty;
    sigemptyset(&empty);
    (void)sigprocmask(SIG_SETMASK, &empty, NULL);
    if (prctl(PR_SET_PDEATHSIG, SIGTERM) != 0 || getppid() != parent) _exit(1);
    /* A cleanup child must not keep UI heartbeat/shutdown endpoints alive. */
    DIR *fds = opendir("/proc/self/fd");
    if (!fds) _exit(1);
    struct dirent *entry;
    while ((entry = readdir(fds)) != NULL) {
        char *end;
        long fd = strtol(entry->d_name, &end, 10);
        if (!*end && fd > STDERR_FILENO && fd <= INT_MAX && fd != dirfd(fds)) close((int)fd);
    }
    closedir(fds);
    int result = c1_launcher_cleanup_after_update(NULL);
    if (result != 0 && errno != EAGAIN && errno != EACCES)
        fprintf(stderr, "C1 launcher: post-update cleanup will retry: %s\n", strerror(errno));
    _exit(result == 0 ? 0 : 1);
}

static int64_t monotonic_milliseconds(void)
{
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) return -1;
    return (int64_t)value.tv_sec * 1000 + value.tv_nsec / 1000000;
}

static int companion_path(char *path, size_t size)
{
    char executable[C1_LAUNCHER_PATH_MAX];
    char *slash;
    ssize_t length = readlink("/proc/self/exe", executable, sizeof(executable) - 1U);
    int count;
    if (length <= 0 || (size_t)length >= sizeof(executable) - 1U) return -1;
    executable[(size_t)length] = '\0';
    slash = strrchr(executable, '/');
    if (slash == NULL) return -1;
    *slash = '\0';
    count = snprintf(path, size, "%s/C1ancher", executable);
    return count >= 0 && (size_t)count < size ? 0 : -1;
}

static void handle_stop(int signal_number)
{
    (void)signal_number;
    stop_requested = 1;
    if (child_pid > 0) (void)kill((pid_t)child_pid, SIGTERM);
}

static void stop_child(pid_t process)
{
    int64_t now = monotonic_milliseconds();
    int64_t deadline = now < 0 ? -1 : now + C1_CHILD_STOP_GRACE_MS;
    int status;

    (void)kill(process, SIGTERM);
    while (deadline >= 0) {
        int64_t current = monotonic_milliseconds();
        pid_t result;
        if (current < 0 || current >= deadline) break;
        result = waitpid(process, &status, WNOHANG);
        if (result == process || (result < 0 && errno == ECHILD)) return;
        if (result < 0 && errno != EINTR) break;
        {
            struct timespec interval = {0, 100000000L};
            (void)nanosleep(&interval, NULL);
        }
    }
    (void)kill(process, SIGKILL);
    /* The bounded descendant cleanup below also reaps this child. A task in
     * uninterruptible kernel sleep must not block the launcher indefinitely. */
}

static int clear_ready(const char *path);

static int run_child(const char *c1ancher_path, const char *ready_file,
                     int *status, uint64_t *runtime_ms)
{
    int64_t started = monotonic_milliseconds(), last_beat = -1;
    int64_t finished;
    unsigned int startup_ms = c1_liveness_setting("C1_HEARTBEAT_STARTUP_MS", C1_HEARTBEAT_STARTUP_MS);
    unsigned int timeout_ms = c1_liveness_setting("C1_HEARTBEAT_TIMEOUT_MS", C1_HEARTBEAT_TIMEOUT_MS);
    pid_t parent = getpid(), process;
    sigset_t blocked, original;
    int heartbeat[2], shutdown_pair[2] = {-1, -1};
    struct c1_shutdown_server shutdown = {-1, 0};
    bool failed = false, shutting_down = false;

    if (pipe2(heartbeat, O_CLOEXEC | O_NONBLOCK) != 0) return -1;
    if (shutdown_upstream.fd >= 0 && c1_shutdown_pair(shutdown_pair) != 0) {
        close(heartbeat[0]); close(heartbeat[1]);
        if (shutdown_pair[0] >= 0) close(shutdown_pair[0]);
        if (shutdown_pair[1] >= 0) close(shutdown_pair[1]);
        return -1;
    }
    shutdown.fd = shutdown_pair[0];
    (void)sigemptyset(&blocked);
    (void)sigaddset(&blocked, SIGCHLD);
    (void)sigaddset(&blocked, SIGTERM);
    (void)sigaddset(&blocked, SIGINT);
    (void)sigaddset(&blocked, SIGHUP);
    if (sigprocmask(SIG_BLOCK, &blocked, &original) != 0) {
        close(heartbeat[0]); close(heartbeat[1]);
        if (shutdown_pair[0] >= 0) close(shutdown_pair[0]);
        if (shutdown_pair[1] >= 0) close(shutdown_pair[1]);
        return -1;
    }
    if (stop_requested) {
        close(heartbeat[0]); close(heartbeat[1]);
        if (shutdown_pair[0] >= 0) close(shutdown_pair[0]);
        if (shutdown_pair[1] >= 0) close(shutdown_pair[1]);
        (void)sigprocmask(SIG_SETMASK, &original, NULL);
        *status = 0; *runtime_ms = 0U;
        return 0;
    }
    process = fork();
    if (process < 0) {
        close(heartbeat[0]); close(heartbeat[1]);
        if (shutdown_pair[0] >= 0) close(shutdown_pair[0]);
        if (shutdown_pair[1] >= 0) close(shutdown_pair[1]);
        (void)sigprocmask(SIG_SETMASK, &original, NULL);
        return -1;
    }
    if (process == 0) {
        char descriptor[32];
        close(heartbeat[0]);
        if (shutdown_pair[0] >= 0) close(shutdown_pair[0]);
        if (shutdown_upstream.fd >= 0) close(shutdown_upstream.fd);
        if (shutdown_pair[1] >= 0 && c1_shutdown_export(shutdown_pair[1]) != 0) _exit(125);
        if (upstream_fd >= 0) close(upstream_fd);
        (void)unsetenv(C1_SUPERVISOR_HEARTBEAT_FD_ENV);
        (void)signal(SIGTERM, SIG_DFL);
        (void)signal(SIGINT, SIG_DFL);
        (void)signal(SIGHUP, SIG_DFL);
        if (prctl(PR_SET_PDEATHSIG, SIGTERM) != 0 || getppid() != parent) _exit(125);
        if (fcntl(heartbeat[1], F_SETFD, 0) != 0) _exit(125);
        (void)snprintf(descriptor, sizeof(descriptor), "%d", heartbeat[1]);
        if (setenv(C1_HEARTBEAT_FD_ENV, descriptor, 1) != 0) _exit(125);
        (void)sigprocmask(SIG_SETMASK, &original, NULL);
        execl(c1ancher_path, c1ancher_path, "app", (char *)NULL);
        _exit(127);
    }
    close(heartbeat[1]);
    if (shutdown_pair[1] >= 0) close(shutdown_pair[1]);
    child_pid = process;
    for (;;) {
        pid_t result;
        int64_t now;
        bool was_shutting_down = shutting_down;
        c1_shutdown_server_poll(&shutdown, process, &shutdown_upstream);
        shutting_down = c1_shutdown_server_active(&shutdown);
        now = monotonic_milliseconds();
        if (was_shutting_down && !shutting_down) {
            /* Restart the observation window, never manufacture a UI beat. */
            started = now;
            last_beat = -1;
        }
        char beats[128];
        ssize_t count;
        bool progress = false;
        /* Bounded draining: a faulty writer cannot starve the watchdog. */
        count = read(heartbeat[0], beats, sizeof(beats));
        if (count > 0) {
            ssize_t index;
            for (index = 0; index < count; ++index)
                if (beats[index] == 'H') progress = true;
        }
        if (progress) {
            last_beat = now;
            if (upstream_fd >= 0) {
                struct c1_liveness_message message = {now, process};
                ssize_t sent = write(upstream_fd, &message, sizeof(message));
                if (sent < 0 && errno != EAGAIN && errno != EINTR) {
                    close(upstream_fd);
                    upstream_fd = -1;
                }
            }
        }
        if (!stop_requested && !shutting_down && last_beat >= 0) maintenance_start(now);
        if (stop_requested || (!shutting_down && c1_liveness_expired(now, started, last_beat,
                                                  startup_ms, timeout_ms))) {
            /* Revoke candidate readiness before the stop grace, not after it. */
            if (clear_ready(ready_file) != 0) failed = true;
            stop_child(process);
            *status = stop_requested ? 0 : SIGKILL;
            break;
        }
        /* The launcher is a subreaper. Applications orphaned by a PTY
         * wrapper can exit while the UI stays healthy; reap those children
         * now rather than retaining zombies until the UI restarts. Only the
         * supervised UI's status controls restart policy. Bound each drain
         * so descendant churn cannot starve heartbeat/stop processing. */
        {
            unsigned int reaped;
            int exited_status;
            result = 0;
            for (reaped = 0U; reaped < 32U; ++reaped) {
                result = waitpid(-1, &exited_status, WNOHANG);
                if (result > 0 && result == maintenance_pid) {
                    maintenance_finished = WIFEXITED(exited_status) && WEXITSTATUS(exited_status) == 0;
                    maintenance_pid = -1;
                }
                if (result == process) {
                    *status = exited_status;
                    break;
                }
                if (result <= 0) break;
            }
        }
        if (result == process) break;
        if (result < 0 && errno != EINTR) { failed = true; break; }
        {
            /* MONOTONIC excludes machine suspend. Idle/locked/external-app UI
             * loops still pulse every two seconds; there is no PID-only bypass. */
            int64_t remaining = (last_beat < 0 ? started + startup_ms : last_beat + timeout_ms)
                                - monotonic_milliseconds();
            struct timespec interval;
            struct pollfd progress_fds[2] = {
                {count == 0 ? -1 : heartbeat[0], POLLIN, 0}, {shutdown.fd, POLLIN, 0}
            };
            if (shutting_down || remaining > 100) remaining = 100;
            if (remaining < 0) remaining = 0;
            interval.tv_sec = (time_t)(remaining / 1000);
            interval.tv_nsec = (long)(remaining % 1000) * 1000000L;
            (void)ppoll(progress_fds, 2U, &interval, &original);
        }
    }
    child_pid = -1;
    close(heartbeat[0]);
    if (shutdown.fd >= 0) close(shutdown.fd);
    if (shutting_down) *status = C1_LAUNCHER_SHUTDOWN_EXIT << 8;
    if (clear_ready(ready_file) != 0) failed = true;
    /* Adopted children are ours even if they created a new session. Never
     * restart while survivors can still hold the old run/hardware locks. */
    if (!c1_descendants_cleanup(C1_CHILD_STOP_GRACE_MS)) failed = true;
    maintenance_pid = -1; /* A terminated pass is retried after the next UI starts. */
    (void)sigprocmask(SIG_SETMASK, &original, NULL);
    finished = monotonic_milliseconds();
    *runtime_ms = started >= 0 && finished >= started ? (uint64_t)(finished - started) : 0U;
    return failed ? -2 : 0;
}

static void handle_child(int signal_number)
{
    (void)signal_number;
}

static void install_signal_handlers(void)
{
    struct sigaction action;
    (void)memset(&action, 0, sizeof(action));
    action.sa_handler = handle_stop;
    (void)sigemptyset(&action.sa_mask);
    (void)sigaction(SIGINT, &action, NULL);
    (void)sigaction(SIGTERM, &action, NULL);
    (void)sigaction(SIGHUP, &action, NULL);
    action.sa_handler = handle_child;
    action.sa_flags = SA_NOCLDSTOP;
    (void)sigaction(SIGCHLD, &action, NULL);
    (void)signal(SIGPIPE, SIG_IGN);
}

static int clear_ready(const char *path)
{
    struct stat information;
    if (path == NULL) return 0;
    if (lstat(path, &information) != 0) return errno == ENOENT ? 0 : -1;
    if (!S_ISREG(information.st_mode) || information.st_nlink != 1 ||
        information.st_uid != geteuid()) return -1;
    return unlink(path);
}

int main(int argc, char **argv)
{
    unsigned int crashes = 0U;
    const char *ready_file = NULL;
    char c1ancher_path[C1_LAUNCHER_PATH_MAX];
    if (argc == 3 && strcmp(argv[1], "--ready-file") == 0 && argv[2][0] == '/')
        ready_file = argv[2];
    else if (argc != 1) return C1_LAUNCHER_CRASH_STORM_EXIT;
    (void)prctl(PR_SET_NAME, "app_daemon");
    if (companion_path(c1ancher_path, sizeof(c1ancher_path)) != 0) return C1_LAUNCHER_CRASH_STORM_EXIT;
    install_signal_handlers();
    if (c1_descendants_adopt() != 0) return C1_LAUNCHER_CRASH_STORM_EXIT;
    {
        int error = c1_shutdown_client_init(&shutdown_upstream);
        if (error != 0 && error != ENOTCONN)
            fprintf(stderr, "C1 launcher: shutdown channel unavailable: %s\n", strerror(error));
    }
    {
        const char *value = getenv(C1_SUPERVISOR_HEARTBEAT_FD_ENV);
        if (value != NULL) {
            char *end;
            long descriptor;
            struct stat information;
            errno = 0;
            descriptor = strtol(value, &end, 10);
            if (errno || *end || descriptor <= STDERR_FILENO || descriptor > INT_MAX ||
                fstat((int)descriptor, &information) != 0 || !S_ISFIFO(information.st_mode))
                return C1_LAUNCHER_CRASH_STORM_EXIT;
            upstream_fd = (int)descriptor;
            if (fcntl(upstream_fd, F_SETFD, FD_CLOEXEC) != 0 ||
                fcntl(upstream_fd, F_SETFL, O_NONBLOCK) != 0)
                return C1_LAUNCHER_CRASH_STORM_EXIT;
        }
    }
    /* Maintenance is best effort and belongs to launcher startup, never to
     * the UI restart loop below. No persistent marker is needed on a full disk. */
    if (!stop_requested && c1_launcher_cleanup_once(NULL) != 0)
        fprintf(stderr, "C1 launcher: startup cleanup incomplete: %s\n", strerror(errno));
    for (;;) {
        struct c1_launcher_observation observation;
        struct c1_launcher_decision decision;
        uint64_t runtime = 0U;
        int status = 0;
        int ran;
        if (stop_requested) return 0;
        if (clear_ready(ready_file) != 0) return C1_LAUNCHER_CRASH_STORM_EXIT;
        ran = run_child(c1ancher_path, ready_file, &status, &runtime);
        if (ran == -2) return C1_LAUNCHER_CRASH_STORM_EXIT;
        /* Readiness belongs to this app process, not to the restarting launcher. */
        if (clear_ready(ready_file) != 0) return C1_LAUNCHER_CRASH_STORM_EXIT;

        observation.stop_requested = stop_requested != 0;
        observation.exited = ran == 0 && WIFEXITED(status);
        observation.exit_code = observation.exited ? WEXITSTATUS(status) : -1;
        observation.runtime_ms = runtime;
        observation.short_crashes = crashes;
        decision = c1_launcher_decide(&observation);
        crashes = decision.short_crashes;
        if (decision.action == C1_LAUNCHER_STOP) {
            /* Preserve shutdown across the outer updater supervisor. A plain
             * zero exit is otherwise treated as an unexpected launcher exit. */
            return observation.exited && observation.exit_code == C1_LAUNCHER_SHUTDOWN_EXIT
                       ? C1_LAUNCHER_SHUTDOWN_EXIT : 0;
        }
        if (decision.action == C1_LAUNCHER_UPDATE) return C1_LAUNCHER_UPDATE_EXIT;
        if (decision.action == C1_LAUNCHER_FATAL) return C1_LAUNCHER_CRASH_STORM_EXIT;
        while (decision.backoff_seconds != 0U && !stop_requested) {
            decision.backoff_seconds = sleep(decision.backoff_seconds);
        }
    }
}
