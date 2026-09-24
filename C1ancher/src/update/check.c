#include "update/check.h"
#include "update/io.h"
#include "update/repository.h"
#include "security/secure_file.h"
#include "security/sha256.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t check_cancelled;
static const int check_signals[] = {SIGTERM, SIGINT, SIGHUP};

static void cancel_check(int signal_number)
{
    (void)signal_number;
    check_cancelled = 1;
}

static int64_t check_now(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return -1;
    return (int64_t)now.tv_sec;
}

static void restore_signals(const struct sigaction *previous, size_t count)
{
    while (count > 0U) {
        --count;
        (void)sigaction(check_signals[count], &previous[count], NULL);
    }
}

static int fetch_bounded(const char *base_url, const char *directory,
                         const char *key_path, char *error, size_t error_size)
{
    int status = 0;
    int64_t started = check_now();
    pid_t child, waited;
    if (started < 0 || check_cancelled) goto failed;
    child = fork();
    if (child == 0) {
        struct c1_update_release release = {0};
        struct sigaction action;
        char child_error[C1_UPDATE_ERROR_MAX] = "";
        size_t i;
        int result;
        /* Isolate all curl descendants so cancelling this check cannot signal
         * an updater transaction or the desktop which launched the command. */
        if (setpgid(0, 0) != 0) _exit(EXIT_FAILURE);
        (void)memset(&action, 0, sizeof(action));
        action.sa_handler = SIG_DFL;
        (void)sigemptyset(&action.sa_mask);
        for (i = 0U; i < sizeof(check_signals) / sizeof(check_signals[0]); ++i)
            if (sigaction(check_signals[i], &action, NULL) != 0) _exit(EXIT_FAILURE);
        result = c1_update_repository_fetch_release(base_url, directory, key_path,
                                                     &release, child_error, sizeof(child_error));
        c1_update_repository_release_free(&release);
        if (result != 0)
            fprintf(stderr, "metadata fetch failed: %s\n", child_error[0] ? child_error : "rejected");
        _exit(result == 0 ? EXIT_SUCCESS : EXIT_FAILURE);
    }
    if (child < 0) goto failed;
    /* Close the fork/setpgid race; the child also sets its own group before
     * starting curl. On abort, the direct child is signalled as well. */
    (void)setpgid(child, child);
    for (;;) {
        struct timespec delay = {0, 100000000L};
        int64_t now = check_now();
        if (check_cancelled || now < started ||
            now - started >= C1_UPDATE_CHECK_TIMEOUT_SECONDS) break;
        waited = waitpid(child, &status, WNOHANG);
        if (waited == child) {
            if (WIFEXITED(status) && WEXITSTATUS(status) == EXIT_SUCCESS) return 0;
            /* A crashed fetch worker may still have a curl descendant. */
            (void)kill(-child, SIGKILL);
            goto failed;
        }
        if (waited < 0 && errno != EINTR) break;
        (void)nanosleep(&delay, NULL);
    }
    (void)kill(-child, SIGKILL);
    (void)kill(child, SIGKILL);
    do {
        waited = waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    c1_secure_set_error(error, error_size, check_cancelled ? "update check cancelled" :
                                                           "update check deadline exceeded");
    return -1;
failed:
    c1_secure_set_error(error, error_size, check_cancelled ? "update check cancelled" :
                                                           "signed metadata check failed");
    return -1;
}

/* Delete only the two names owned by this invocation, never recursively clean
 * staging or follow a sibling transaction directory. Unexpected files make
 * cleanup fail closed rather than widening the deletion scope. */
static int cleanup_check(const char *directory)
{
    char path[C1_UPDATE_PATH_MAX];
    int result = 0;
    if (c1_update_join_path(path, sizeof(path), directory, C1_UPDATE_MANIFEST_NAME) != 0 ||
        (unlink(path) != 0 && errno != ENOENT)) result = -1;
    if (c1_update_join_path(path, sizeof(path), directory, C1_UPDATE_SIGNATURE_NAME) != 0 ||
        (unlink(path) != 0 && errno != ENOENT)) result = -1;
    if (rmdir(directory) != 0) result = -1;
    return result;
}

int c1_update_check(const char *base_url, const char *staging_root,
                    const char *state_root, const char *key_path,
                    struct c1_update_check_result *result,
                    char *error, size_t error_size)
{
    struct c1_update_release release = {0};
    struct c1_update_state before, state;
    struct c1_update_check_result checked = {0};
    struct sigaction action, previous[sizeof(check_signals) / sizeof(check_signals[0])];
    char directory[C1_UPDATE_PATH_MAX];
    unsigned char hash[C1_SHA256_SIZE];
    char digest[C1_SHA256_HEX_SIZE];
    size_t signals_installed = 0U;
    int created = 0, status = -1;
    dev_t device;

    if (result == NULL) return -1;
    (void)memset(result, 0, sizeof(*result));
    if (key_path == NULL ||
        c1_update_repository_validate_url(base_url, error, error_size) != 0 ||
        c1_update_check_trusted_directory(staging_root, (dev_t)-1, &device,
                                          error, error_size) != 0 ||
        c1_update_state_load(state_root, &before, error, error_size) != 0 ||
        c1_update_join_path(directory, sizeof(directory), staging_root, ".check.XXXXXX") != 0)
        return -1;

    check_cancelled = 0;
    (void)memset(&action, 0, sizeof(action));
    action.sa_handler = cancel_check;
    (void)sigemptyset(&action.sa_mask);
    for (; signals_installed < sizeof(check_signals) / sizeof(check_signals[0]); ++signals_installed) {
        if (sigaction(check_signals[signals_installed], &action,
                       &previous[signals_installed]) != 0) goto done;
    }
    if (check_cancelled || mkdtemp(directory) == NULL) goto done;
    created = 1;
    if (c1_update_check_trusted_directory(directory, device, NULL, error, error_size) != 0 ||
        fetch_bounded(base_url, directory, key_path, error, error_size) != 0 ||
        /* Reopen/verify the bounded files after the worker exits; no unsigned
         * result, pointer or unbounded pipe output crosses the process boundary. */
        c1_update_repository_load_local(directory, key_path, &release, error, error_size) != 0 ||
        c1_update_check_compatibility(&release.manifest, error, error_size) != 0 ||
        /* Immutable generations allow a fresh read without touching the lock.
         * A concurrent prepare/confirm must influence the availability result. */
        c1_update_state_load(state_root, &state, error, error_size) != 0) goto done;
    if (strcmp(release.manifest.compatibility, "c1-core-v1") != 0) {
        c1_secure_set_error(error, error_size, "release compatibility contract rejected");
        goto done;
    }
    if (state.sequence < before.sequence || state.security_epoch < before.security_epoch ||
        release.manifest.sequence < state.sequence ||
        release.manifest.security_epoch < state.security_epoch) {
        c1_secure_set_error(error, error_size, "release rollback rejected");
        goto done;
    }
    c1_sha256(release.manifest_data, release.manifest_size, hash);
    c1_sha256_hex(hash, digest);
    if (state.generation != 0U && release.manifest.sequence == state.sequence &&
        (release.manifest.security_epoch != state.security_epoch ||
         strcmp(release.manifest.version, state.release) != 0 ||
         strcmp(digest, state.digest) != 0)) {
        c1_secure_set_error(error, error_size, "release identity conflict rejected");
        goto done;
    }
    checked.sequence = release.manifest.sequence;
    (void)strcpy(checked.version, release.manifest.version);
    checked.available = release.manifest.sequence > state.sequence;
    status = 0;
done:
    c1_update_repository_release_free(&release);
    if (created && cleanup_check(directory) != 0) {
        c1_secure_set_error(error, error_size, "update check cleanup failed");
        status = -1;
    }
    if (check_cancelled) {
        c1_secure_set_error(error, error_size, "update check cancelled");
        status = -1;
    }
    restore_signals(previous, signals_installed);
    if (status == 0) {
        *result = checked;
        if (error != NULL && error_size > 0U) error[0] = '\0';
    }
    return status;
}
