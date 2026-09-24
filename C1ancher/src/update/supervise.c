#define _GNU_SOURCE 1

#include "update/supervise.h"
#include "update/supervise_policy.h"
#include "update/boot.h"
#include "update/io.h"
#include "update/slot.h"
#include "update/update.h"
#include "launcher/policy.h"
#include "security/secure_file.h"
#include "platform/liveness.h"
#include "platform/shutdown.h"

#include <poll.h>
#include <fcntl.h>
#include <stdlib.h>

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* The launcher has its own three-second app shutdown grace period. */
#define C1_SUPERVISE_STOP_GRACE_MS 5000U

static volatile sig_atomic_t supervise_stop;
static volatile sig_atomic_t supervise_child = -1;

static int64_t monotonic_ms(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return -1;
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static void stop_handler(int signal_number)
{
    (void)signal_number;
    supervise_stop = 1;
    if (supervise_child > 0) (void)kill((pid_t)supervise_child, SIGTERM);
}

static void child_handler(int signal_number)
{
    (void)signal_number;
}

static void install_handlers(void)
{
    struct sigaction action;
    (void)memset(&action, 0, sizeof(action));
    action.sa_handler = stop_handler;
    (void)sigemptyset(&action.sa_mask);
    (void)sigaction(SIGTERM, &action, NULL);
    (void)sigaction(SIGINT, &action, NULL);
    (void)sigaction(SIGHUP, &action, NULL);
    action.sa_handler = child_handler;
    action.sa_flags = SA_NOCLDSTOP;
    (void)sigaction(SIGCHLD, &action, NULL);
}

static pid_t wait_launcher(pid_t child, int *status, struct c1_shutdown_server *shutdown)
{
    sigset_t blocked, original;
    pid_t result = -1;
    (void)sigemptyset(&blocked);
    (void)sigaddset(&blocked, SIGCHLD);
    (void)sigaddset(&blocked, SIGTERM);
    (void)sigaddset(&blocked, SIGINT);
    (void)sigaddset(&blocked, SIGHUP);
    if (sigprocmask(SIG_BLOCK, &blocked, &original) != 0) return -1;
    while (!supervise_stop) {
        struct pollfd watch = {shutdown->fd, POLLIN, 0};
        struct timespec interval = {0, 100000000L};
        c1_shutdown_server_poll(shutdown, child, NULL);
        (void)c1_shutdown_server_active(shutdown);
        result = waitpid(child, status, WNOHANG);
        if (result == child || (result < 0 && errno != EINTR)) break;
        watch.fd = shutdown->fd;
        (void)ppoll(&watch, 1U, &interval, &original);
    }
    (void)sigprocmask(SIG_SETMASK, &original, NULL);
    return result;
}

static void terminate_child(pid_t child)
{
    int status;
    int64_t start = monotonic_ms();
    (void)kill(child, SIGTERM);
    while (start >= 0) {
        int64_t now = monotonic_ms();
        pid_t result;
        if (now < start || (uint64_t)(now - start) >= C1_SUPERVISE_STOP_GRACE_MS) break;
        result = waitpid(child, &status, WNOHANG);
        if (result == child || (result < 0 && errno == ECHILD)) return;
        if (result < 0 && errno != EINTR) break;
        {
            struct timespec delay = {0, 100000000L};
            (void)nanosleep(&delay, NULL);
        }
    }
    (void)kill(child, SIGKILL);
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
    }
}

static pid_t start_launcher(const char *path, const char *ready_file, int *heartbeat_fd,
                            struct c1_shutdown_server *shutdown)
{
    int reports[2], channel[2] = {-1, -1};
    pid_t parent = getpid(), child;
    if (pipe(reports) != 0) return -1;
    {
        int error = c1_shutdown_pair(channel);
        if (error != 0) {
            fprintf(stderr, "C1 supervisor: shutdown channel unavailable: %s\n", strerror(error));
            channel[0] = channel[1] = -1;
        }
    }
    if (fcntl(reports[0], F_SETFD, FD_CLOEXEC) != 0 ||
        fcntl(reports[0], F_SETFL, O_NONBLOCK) != 0 ||
        fcntl(reports[1], F_SETFL, O_NONBLOCK) != 0) {
        (void)close(reports[0]); (void)close(reports[1]);
        (void)close(channel[0]); (void)close(channel[1]); return -1;
    }
    child = fork();
    if (child != 0) {
        (void)close(reports[1]);
        (void)close(channel[1]);
        if (child < 0) { (void)close(reports[0]); (void)close(channel[0]); }
        else {
            *heartbeat_fd = reports[0];
            shutdown->fd = channel[0];
            shutdown->until_ms = 0;
        }
        return child;
    }
    (void)close(channel[0]);
    (void)unsetenv(C1_SHUTDOWN_FD_ENV);
    if (channel[1] >= 0 && c1_shutdown_export(channel[1]) != 0) _exit(125);
    {
        char text[32];
        (void)close(reports[0]);
        (void)snprintf(text, sizeof(text), "%d", reports[1]);
        if (setenv(C1_SUPERVISOR_HEARTBEAT_FD_ENV, text, 1) != 0) _exit(125);
    }
    (void)signal(SIGTERM, SIG_DFL);
    (void)signal(SIGINT, SIG_DFL);
    (void)signal(SIGHUP, SIG_DFL);
    if (prctl(PR_SET_PDEATHSIG, SIGTERM) != 0 || getppid() != parent) _exit(125);
    execl(path, path, "--ready-file", ready_file, (char *)NULL);
    _exit(127);
}

static int clear_ready_file(const char *ready_file, char *error, size_t error_size)
{
    struct stat information;
    char parent[C1_UPDATE_PATH_MAX];
    const char *slash;
    size_t length;

    errno = 0;
    if (lstat(ready_file, &information) != 0) {
        if (errno == ENOENT) return 0;
        c1_secure_set_error(error, error_size, "ready marker lookup failed: %s",
                            strerror(errno));
        return -1;
    }
    if (!S_ISREG(information.st_mode) || information.st_nlink != 1 ||
        unlink(ready_file) != 0) {
        c1_secure_set_error(error, error_size, "ready marker removal failed");
        return -1;
    }
    slash = strrchr(ready_file, '/');
    if (slash == NULL) {
        (void)strcpy(parent, ".");
    } else {
        length = (size_t)(slash - ready_file);
        if (length == 0U) length = 1U;
        if (length >= sizeof(parent)) {
            c1_secure_set_error(error, error_size, "ready marker path is too long");
            return -1;
        }
        (void)memcpy(parent, ready_file, length);
        parent[length] = '\0';
    }
    return c1_secure_sync_directory(parent, error, error_size);
}

static int prepared_requires_slot_switch(const char *core_root,
                                         const char *key_path,
                                         const struct c1_update_state *state,
                                         int *required,
                                         char *error, size_t error_size)
{
    char releases[C1_UPDATE_PATH_MAX], name[2U * C1_UPDATE_TOKEN_MAX + 24U];
    char prepared_release[C1_UPDATE_PATH_MAX];
    int count;

    *required = 0;
    if (state->phase != C1_UPDATE_PREPARED) return 0;
    count = snprintf(name, sizeof(name), "%llu-%s",
                     (unsigned long long)state->sequence, state->release);
    if (count < 0 || (size_t)count >= sizeof(name) ||
        c1_update_join_path(releases, sizeof(releases), core_root, "releases") != 0 ||
        c1_update_join_path(prepared_release, sizeof(prepared_release), releases, name) != 0)
        return -1;
    return c1_update_release_requires_slot_switch(prepared_release, key_path, required,
                                                  error, error_size);
}

static int reconcile(const char *state_root, const char *core_root,
                     const char *key_path, struct c1_update_state *state,
                     char *error, size_t error_size)
{
    int slot_switch, result;

    result = c1_update_recover(state_root, core_root, key_path, error, error_size);
    if (result != 0) return result;
    if (c1_update_state_load(state_root, state, error, error_size) != 0 ||
        prepared_requires_slot_switch(core_root, key_path, state, &slot_switch,
                                      error, error_size) != 0) return -1;
    if (slot_switch != 0) return 1;
    if (state->phase == C1_UPDATE_PREPARED) {
        result = c1_update_activate(state_root, core_root, key_path, error, error_size);
        if (result != 0) return result;
    }
    if (state->phase == C1_UPDATE_PREPARED || state->phase == C1_UPDATE_PENDING_BOOT) {
        result = c1_update_recover(state_root, core_root, key_path, error, error_size);
        if (result != 0) return result;
        return c1_update_state_load(state_root, state, error, error_size);
    }
    return c1_update_validate_current(core_root, key_path,
                                      state->phase == C1_UPDATE_CONFIRMED ? state : NULL,
                                      error, error_size);
}

static int pending_watch(pid_t child, int *status, int heartbeat_fd,
                         struct c1_shutdown_server *shutdown,
                         const char *state_root, const char *core_root,
                         const char *key_path, const char *ready_file,
                         const struct c1_update_state *state,
                         char *error, size_t error_size)
{
    int64_t started = monotonic_ms(), ready_at = -1, last_beat = -1, ui_pid = -1;
    struct stat ready_identity = {0};
    char marker_error[C1_UPDATE_ERROR_MAX];
    bool was_shutting_down = false;
    for (;;) {
        pid_t result;
        bool shutting_down;
        int64_t now;
        c1_shutdown_server_poll(shutdown, child, NULL);
        shutting_down = c1_shutdown_server_active(shutdown);
        result = waitpid(child, status, WNOHANG);
        now = monotonic_ms();
        if (was_shutting_down && !shutting_down) {
            started = now;
            ready_at = last_beat = ui_pid = -1;
        }
        was_shutting_down = shutting_down;
        struct c1_update_pending_observation observation;
        enum c1_update_pending_action action;
        if (result < 0 && errno != EINTR) return -1;
        {
            struct c1_liveness_message message;
            ssize_t amount = -1;
            unsigned int drained;
            /* A noisy child must not starve shutdown requests or waitpid. */
            for (drained = 0; drained < 64U; ++drained) {
                amount = read(heartbeat_fd, &message, sizeof(message));
                if (amount <= 0) break;
                now = monotonic_ms();
                if ((size_t)amount != sizeof(message) || message.ui_pid <= 0 ||
                    message.monotonic_ms > now) return -1;
                /* Pre-cancellation beats cannot satisfy the new health window. */
                if (message.monotonic_ms < started) continue;
                if (message.monotonic_ms < last_beat) return -1;
                if (ui_pid != message.ui_pid) { ready_at = -1; ui_pid = message.ui_pid; }
                last_beat = message.monotonic_ms;
            }
            if (amount < 0 && errno != EAGAIN && errno != EINTR) return -1;
        }
        if (shutting_down) {
            if (result == child) return 1;
            if (supervise_stop) { terminate_child(child); return 2; }
            /* Shutdown progress is deliberately NOT candidate health. */
            ready_at = -1;
            {
                struct timespec delay = {0, 100000000L};
                (void)nanosleep(&delay, NULL);
            }
            continue;
        }
        marker_error[0] = '\0';
        {
            struct stat marker;
            if (c1_update_ready_matches(ready_file, state->digest,
                                        marker_error, sizeof(marker_error)) != 0 ||
                lstat(ready_file, &marker) != 0) {
                ready_at = -1;
            } else if (ready_at < 0 || marker.st_dev != ready_identity.st_dev ||
                       marker.st_ino != ready_identity.st_ino ||
                       marker.st_mtim.tv_sec != ready_identity.st_mtim.tv_sec ||
                       marker.st_mtim.tv_nsec != ready_identity.st_mtim.tv_nsec) {
                ready_identity = marker;
                ready_at = now;
            }
        }
        observation.child_exited = result == child;
        observation.stop_requested = supervise_stop != 0;
        observation.started_ms = started;
        observation.now_ms = now;
        observation.ready_at_ms = ready_at;
        action = c1_update_pending_decide(&observation);
        if (action == C1_UPDATE_PENDING_CHILD_EXIT) return 1;
        if (action == C1_UPDATE_PENDING_STOP) {
            terminate_child(child);
            return 2;
        }
        if (action == C1_UPDATE_PENDING_CONFIRM) {
            int confirmed;
            /* A timer and a stale ready inode cannot confirm a hung UI. Require
             * an actual launcher-observed UI beat beyond the entire window. */
            if (last_beat < ready_at + C1_UPDATE_READY_STABLE_MS ||
                now - last_beat > C1_HEARTBEAT_TIMEOUT_MS) {
                if (now - ready_at >= C1_UPDATE_READY_STABLE_MS + C1_HEARTBEAT_TIMEOUT_MS) return -1;
            } else {
                confirmed = c1_update_confirm(state_root, core_root, key_path, error, error_size);
                if (confirmed == 0) return 0;
                if (confirmed != C1_UPDATE_BUSY) return -1;
                /* Keep watching a healthy candidate while another transaction
                 * holds the lock; busy is never grounds for rollback. */
            }
        }
        if (action == C1_UPDATE_PENDING_ROLLBACK) return -1;
        {
            struct timespec delay = {0, 100000000L};
            (void)nanosleep(&delay, NULL);
        }
    }
}

static int rollback_wait(const char *state_root, const char *core_root,
                         const char *key_path, char *error, size_t error_size)
{
    int result;
    do {
        if (supervise_stop) return 0;
        result = c1_update_rollback(state_root, core_root, key_path, error, error_size);
        if (result == C1_UPDATE_BUSY) {
            struct timespec delay = {1, 0};
            (void)nanosleep(&delay, NULL);
        }
    } while (result == C1_UPDATE_BUSY);
    return result;
}

int c1_update_supervise(const char *state_root, const char *core_root,
                        const char *key_path, const char *ready_file,
                        const char *launcher_path,
                        char *error, size_t error_size)
{
    struct c1_update_state state;
    struct stat information;
    char expected[C1_UPDATE_PATH_MAX];
    unsigned int crashes = 0U;
    int count, heartbeat_fd = -1;
    struct c1_shutdown_server shutdown = {-1, 0};
    supervise_stop = 0;
    supervise_child = -1;
    count = snprintf(expected, sizeof(expected), "%s/current/artifacts/C1ancher-launcher", core_root);
    if (count < 0 || (size_t)count >= sizeof(expected) || launcher_path == NULL ||
        strcmp(launcher_path, expected) != 0) {
        c1_secure_set_error(error, error_size, "launcher path rejected");
        return C1_UPDATER_FATAL_EXIT;
    }
    install_handlers();
    for (;;) {
        pid_t child;
        int status = 0, watch = 0;
        int reconciliation;
        int64_t started, finished;
        struct c1_launcher_observation observation;
        struct c1_launcher_decision decision;
        if (heartbeat_fd >= 0) { (void)close(heartbeat_fd); heartbeat_fd = -1; }
        if (shutdown.fd >= 0) { (void)close(shutdown.fd); shutdown.fd = -1; }
        shutdown.until_ms = 0;
        if (supervise_stop) return 0;
        reconciliation = reconcile(state_root, core_root, key_path, &state,
                                   error, error_size);
        if (reconciliation == C1_UPDATE_BUSY) return C1_UPDATER_TRANSIENT_EXIT;
        if (reconciliation < 0) return C1_UPDATER_FATAL_EXIT;
        if (reconciliation > 0) return C1_UPDATER_SLOT_SWITCH_EXIT;
        if (clear_ready_file(ready_file, error, error_size) != 0) {
            if (state.phase == C1_UPDATE_PENDING_BOOT &&
                rollback_wait(state_root, core_root, key_path,
                                   error, error_size) == 0) continue;
            return C1_UPDATER_FATAL_EXIT;
        }
        if (stat(expected, &information) != 0 || !S_ISREG(information.st_mode) ||
            (information.st_mode & 0111U) == 0) {
            if (state.phase == C1_UPDATE_PENDING_BOOT &&
                rollback_wait(state_root, core_root, key_path, error, error_size) == 0) continue;
            c1_secure_set_error(error, error_size, "launcher is not executable");
            return C1_UPDATER_FATAL_EXIT;
        }
        started = monotonic_ms();
        child = start_launcher(expected, ready_file, &heartbeat_fd, &shutdown);
        if (child < 0) return C1_UPDATER_FATAL_EXIT;
        supervise_child = child;
        if (state.phase == C1_UPDATE_PENDING_BOOT) {
            watch = pending_watch(child, &status, heartbeat_fd, &shutdown, state_root, core_root, key_path,
                                  ready_file, &state, error, error_size);
            if (watch == 2) {
                supervise_child = -1;
                return 0;
            }
            if (watch < 0) {
                terminate_child(child);
                supervise_child = -1;
                if (rollback_wait(state_root, core_root, key_path,
                                       error, error_size) != 0) return C1_UPDATER_FATAL_EXIT;
                continue;
            }
            if (watch == 1) {
                supervise_child = -1;
                if (c1_shutdown_server_active(&shutdown) ||
                    (WIFEXITED(status) && WEXITSTATUS(status) == C1_LAUNCHER_SHUTDOWN_EXIT))
                    return 0; /* Intentional poweroff is not a failed candidate boot. */
                if (rollback_wait(state_root, core_root, key_path,
                                       error, error_size) != 0) return C1_UPDATER_FATAL_EXIT;
                continue;
            }
            if (c1_update_state_load(state_root, &state, error, error_size) != 0 ||
                state.phase != C1_UPDATE_CONFIRMED) {
                terminate_child(child);
                supervise_child = -1;
                return C1_UPDATER_FATAL_EXIT;
            }
        }
        if (watch == 0) {
            pid_t waited = wait_launcher(child, &status, &shutdown);
            if (supervise_stop) {
                terminate_child(child);
                supervise_child = -1;
                return 0;
            }
            if (waited != child) return C1_UPDATER_FATAL_EXIT;
        }
        supervise_child = -1;
        if (c1_shutdown_server_active(&shutdown)) return 0;
        finished = monotonic_ms();
        if (WIFEXITED(status) && WEXITSTATUS(status) == C1_LAUNCHER_UPDATE_EXIT) {
            struct c1_update_state reloaded;
            if (c1_update_state_load(state_root, &reloaded, error, error_size) != 0)
                return C1_UPDATER_FATAL_EXIT;
            if (reloaded.phase == C1_UPDATE_PREPARED) continue;
        }
        if (state.phase == C1_UPDATE_PENDING_BOOT) {
            if (rollback_wait(state_root, core_root, key_path, error, error_size) != 0)
                return C1_UPDATER_FATAL_EXIT;
            continue;
        }
        if (WIFEXITED(status) && WEXITSTATUS(status) == C1_LAUNCHER_CRASH_STORM_EXIT) {
            if (state.phase == C1_UPDATE_CONFIRMED &&
                rollback_wait(state_root, core_root, key_path,
                                   error, error_size) == 0) {
                crashes = 0U;
                continue;
            }
            return C1_UPDATER_FATAL_EXIT;
        }
        observation.stop_requested = false;
        observation.exited = WIFEXITED(status);
        observation.exit_code = observation.exited ? WEXITSTATUS(status) : -1;
        observation.runtime_ms = started >= 0 && finished >= started ?
                                 (uint64_t)(finished - started) : 0U;
        observation.short_crashes = crashes;
        decision = c1_launcher_decide(&observation);
        crashes = decision.short_crashes;
        if (decision.action == C1_LAUNCHER_STOP) return 0;
        if (decision.action == C1_LAUNCHER_FATAL) {
            if (state.phase == C1_UPDATE_CONFIRMED &&
                rollback_wait(state_root, core_root, key_path,
                                   error, error_size) == 0) {
                crashes = 0U;
                continue;
            }
            return C1_UPDATER_FATAL_EXIT;
        }
        while (decision.backoff_seconds != 0U && !supervise_stop)
            decision.backoff_seconds = sleep(decision.backoff_seconds);
    }
}