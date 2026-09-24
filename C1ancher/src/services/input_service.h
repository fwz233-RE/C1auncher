#ifndef C1_INPUT_SERVICE_H
#define C1_INPUT_SERVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define C1_INPUT_SERVICE_PACKAGE_ROOT "/storage/c1/apps/c1-ime"
#define C1_INPUT_SERVICE_DEFAULT_EXE C1_INPUT_SERVICE_PACKAGE_ROOT "/current/bin/c1-ime-service"
#define C1_INPUT_SERVICE_DEFAULT_SOCKET "/run/c1-ime/socket"
#define C1_INPUT_SERVICE_DEFAULT_SHARED_DATA C1_INPUT_SERVICE_PACKAGE_ROOT "/current/share/rime-data"
#define C1_INPUT_SERVICE_DEFAULT_USER_DATA "/storage/c1/ime"
#define C1_INPUT_SERVICE_STOP_GRACE_MS 1500U
#define C1_INPUT_SERVICE_MAX_STOP_GRACE_MS 5000U

typedef enum {
    C1_INPUT_SERVICE_IDLE,
    C1_INPUT_SERVICE_UNAVAILABLE,
    C1_INPUT_SERVICE_RUNNING,
    C1_INPUT_SERVICE_STOPPING,
    C1_INPUT_SERVICE_STOPPED,
    C1_INPUT_SERVICE_FAILED
} c1_input_service_state;

typedef struct {
    /* Configurable after init and before start; strings are borrowed. The
     * supervisor never creates/removes socket, data, or lock paths. */
    const char *exe_path;
    const char *socket_path;
    const char *shared_data_path;
    const char *user_data_path;
    /* Signed c1pkg installation. Resolve current once in the child so service
     * and dictionaries cannot come from different generations during update.
     * NULL permits explicit paths for isolated host tests/developer clients. */
    const char *package_root;
    bool prebuilt_only;
    unsigned int stop_grace_ms;

    /* Read-only lifecycle/diagnostics for callers. RUNNING means an owned child,
     * not socket readiness. FAILED does not imply an external IME is absent. */
    c1_input_service_state state;
    pid_t pid;
    int last_error;
    int wait_status;
    bool wait_status_valid;
    bool kill_sent;
    int64_t stop_deadline_ms;
} c1_input_service;

/* Linux, single main-thread owner: no competing SIGCHLD handler, thread, or
 * waitpid(-1) loop may reap this child. Do not re-init/copy a live supervisor.
 * No C++, librime, UI, or liveness dependency. */
/* Validate the c1pkg-owned directory chain and pin its relative current link
 * to one immutable version. No writes; callers can use temporary test stores. */
bool c1_input_service_package_paths(const char *root, char *exe, size_t exe_size,
                                    char *shared, size_t shared_size);

void c1_input_service_init(c1_input_service *service);

/* One attempt per init, with no automatic retry or deployment. Missing or
 * non-executable binaries are UNAVAILABLE without fork. Parent never waits for
 * exec, Rime initialization, or socket readiness; English input stays usable.
 * An exited child is FAILED even when it exits with status zero. */
c1_input_service_state c1_input_service_start(c1_input_service *service);

/* Nonblocking waitpid(exact_child, WNOHANG); also advances a pending stop. */
c1_input_service_state c1_input_service_poll(c1_input_service *service);

/* Nonblocking TERM request, idempotent. Keep polling until pid == -1, servicing
 * UI/supervisor heartbeats as usual. After a grace period (default 1500 ms,
 * capped at 5000 ms), poll sends KILL, but NEVER blocks waiting to reap. A child
 * stuck in uninterruptible kernel sleep may therefore remain STOPPING. Never
 * signals a process group, external service, or an already-reaped PID. */
c1_input_service_state c1_input_service_stop(c1_input_service *service);

#endif
