#include "update/slot.h"
#include "update/boot.h"
#include "update/io.h"
#include "update/repository.h"
#include "security/secure_file.h"
#include "security/sha256.h"

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define C1_UPDATE_SELF_TEST_TIMEOUT_MS 5000U
#define C1_UPDATE_SELF_TEST_POLL_NS 100000000L

static const struct c1_update_component *updater_component(
    const struct c1_update_release *release)
{
    size_t i;

    for (i = 0U; i < C1_UPDATE_COMPONENT_COUNT; ++i) {
        if (strcmp(release->manifest.components[i].role, "updater") == 0)
            return &release->manifest.components[i];
    }
    return NULL;
}

static int64_t monotonic_ms(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return -1;
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static void terminate_self_test_group(pid_t child)
{
    if (child > 0) (void)kill(-child, SIGKILL);
}

static void reap_self_test(pid_t child)
{
    int status;

    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
    }
}

static int self_test(const char *path, char *error, size_t error_size)
{
    int status;
    int64_t started;
    pid_t child = fork(), waited;

    if (child < 0) goto failed;
    if (child == 0) {
        if (setpgid(0, 0) != 0) _exit(126);
        execl(path, path, "--self-test", (char *)NULL);
        _exit(127);
    }
    if (setpgid(child, child) != 0 && errno != EACCES && errno != ESRCH) {
        terminate_self_test_group(child);
        (void)kill(child, SIGKILL);
        reap_self_test(child);
        goto failed;
    }
    started = monotonic_ms();
    for (;;) {
        int64_t now;

        do {
            waited = waitpid(child, &status, WNOHANG);
        } while (waited < 0 && errno == EINTR);
        if (waited == child) {
            terminate_self_test_group(child);
            if (WIFEXITED(status) && WEXITSTATUS(status) == 0) return 0;
            goto failed;
        }
        if (waited < 0) {
            terminate_self_test_group(child);
            goto failed;
        }
        now = monotonic_ms();
        if (started < 0 || now < started ||
            (uint64_t)(now - started) >= C1_UPDATE_SELF_TEST_TIMEOUT_MS) {
            terminate_self_test_group(child);
            (void)kill(child, SIGKILL);
            reap_self_test(child);
            c1_secure_set_error(error, error_size,
                                "updater candidate self-test timed out");
            return -1;
        }
        {
            struct timespec delay = {0, C1_UPDATE_SELF_TEST_POLL_NS};
            (void)nanosleep(&delay, NULL);
        }
    }
failed:
    c1_secure_set_error(error, error_size, "updater candidate self-test failed");
    return -1;
}

int c1_update_record_active_slot(const char *update_root, char active_slot,
                                 char *error, size_t error_size)
{
    char path[C1_UPDATE_PATH_MAX], text[2];
    struct stat information;

    if ((active_slot != 'a' && active_slot != 'b') ||
        c1_update_check_trusted_directory(update_root, (dev_t)-1, NULL,
                                          error, error_size) != 0 ||
        c1_update_join_path(path, sizeof(path), update_root, "updater-slot") != 0) {
        c1_secure_set_error(error, error_size,
                            "updater slot pointer request rejected");
        return -1;
    }
    errno = 0;
    if (lstat(path, &information) == 0) {
        if (!S_ISREG(information.st_mode) || information.st_nlink != 1 ||
            (information.st_mode & 0777U) != 0600U) {
            c1_secure_set_error(error, error_size,
                                "updater slot pointer metadata rejected");
            return -1;
        }
    } else if (errno != ENOENT) {
        c1_secure_set_error(error, error_size,
                            "updater slot pointer lookup failed");
        return -1;
    }
    text[0] = active_slot;
    text[1] = '\n';
    return c1_secure_atomic_write(path, text, sizeof(text), 0600,
                                  error, error_size);
}

int c1_update_release_requires_slot_switch(const char *prepared_release,
                                           const char *key_path,
                                           int *required,
                                           char *error, size_t error_size)
{
    struct c1_update_release release;
    const struct c1_update_component *component;
    unsigned char digest[C1_SHA256_SIZE];
    char executable[C1_UPDATE_PATH_MAX], hex[C1_SHA256_HEX_SIZE];
    uint64_t size = 0U;
    ssize_t length;
    int result = -1;

    if (required == NULL) return -1;
    *required = 0;
    (void)memset(&release, 0, sizeof(release));
    if (c1_update_repository_load_local(prepared_release, key_path, &release,
                                        error, error_size) != 0)
        goto done;
    if (c1_update_check_compatibility(&release.manifest, error, error_size) != 0) goto done;
    component = updater_component(&release);
    length = readlink("/proc/self/exe", executable, sizeof(executable) - 1U);
    if (component == NULL || length <= 0 || (size_t)length >= sizeof(executable)) {
        c1_secure_set_error(error, error_size,
                            "running updater identity unavailable");
        goto done;
    }
    executable[(size_t)length] = '\0';
    if (c1_sha256_file(executable, C1_UPDATE_COMPONENT_MAX,
                       C1_SHA256_ANY_SIZE, digest, &size,
                       error, error_size) != 0)
        goto done;
    c1_sha256_hex(digest, hex);
    *required = size != component->size || strcmp(hex, component->sha256) != 0;
    result = 0;
done:
    c1_update_repository_release_free(&release);
    return result;
}

int c1_update_install_prepared_slot(const char *slot_root, char active_slot,
                                    const char *core_root,
                                    const char *state_root,
                                    const char *key_path, char *installed_slot,
                                    char *error, size_t error_size)
{
    struct c1_update_state state;
    char release_name[2U * C1_UPDATE_TOKEN_MAX + 24U];
    char target[2U * C1_UPDATE_TOKEN_MAX + 40U];
    char releases[C1_UPDATE_PATH_MAX], prepared_release[C1_UPDATE_PATH_MAX];
    char lock_path[C1_UPDATE_PATH_MAX] = "";
    int count, target_count;
    int lock = -1, result = -1;

    if (c1_update_check_trusted_directory(state_root, (dev_t)-1, NULL,
                                          error, error_size) != 0)
        return -1;
    lock = c1_update_lock(state_root, lock_path, sizeof(lock_path), error, error_size);
    if (lock < 0) return lock;
    if (c1_update_state_load(state_root, &state, error, error_size) != 0 ||
        state.phase != C1_UPDATE_PREPARED ||
        c1_update_join_path(releases, sizeof(releases), core_root, "releases") != 0) {
        c1_secure_set_error(error, error_size,
                            "prepared updater slot state rejected");
        goto done;
    }
    count = snprintf(release_name, sizeof(release_name), "%llu-%s",
                     (unsigned long long)state.sequence, state.release);
    if (count < 0 || (size_t)count >= sizeof(release_name)) goto done;
    target_count = snprintf(target, sizeof(target), "releases/%s", release_name);
    if (target_count < 0 || (size_t)target_count >= sizeof(target) ||
        c1_update_join_path(prepared_release, sizeof(prepared_release),
                            releases, release_name) != 0 ||
        c1_update_validate_release(core_root, target, key_path, &state,
                                   error, error_size) != 0)
        goto done;
    result = c1_update_install_inactive_slot(slot_root, active_slot, prepared_release,
                                             key_path, installed_slot,
                                             error, error_size);
done:
    c1_update_unlock(lock, lock_path);
    return result;
}

int c1_update_install_inactive_slot(const char *slot_root, char active_slot,
                                    const char *prepared_release,
                                    const char *key_path, char *installed_slot,
                                    char *error, size_t error_size)
{
    struct c1_update_release release;
    const struct c1_update_component *component;
    char slot_name[] = "slot-a", slot_path[C1_UPDATE_PATH_MAX];
    char source[C1_UPDATE_PATH_MAX], target[C1_UPDATE_PATH_MAX];
    char temporary[C1_UPDATE_PATH_MAX];
    struct stat information;
    const char *base;
    dev_t device;
    int count, result = -1;
    unsigned int attempt;
    char inactive;

    (void)memset(&release, 0, sizeof(release));
    if (active_slot != 'a' && active_slot != 'b') goto rejected;
    inactive = active_slot == 'a' ? 'b' : 'a';
    slot_name[5] = inactive;
    if (c1_update_check_trusted_directory(slot_root, (dev_t)-1, &device,
                                          error, error_size) != 0 ||
        c1_update_join_path(slot_path, sizeof(slot_path), slot_root, slot_name) != 0 ||
        c1_update_make_directory(slot_path, 0700, device, 1,
                                 error, error_size) != 0 ||
        c1_update_repository_load_local(prepared_release, key_path, &release,
                                        error, error_size) != 0)
        goto done;
    if (c1_update_check_compatibility(&release.manifest, error, error_size) != 0) goto done;
    component = updater_component(&release);
    if (component == NULL || component->mode != 0700U) goto rejected;
    base = strrchr(component->path, '/');
    if (base == NULL || strcmp(base + 1U, "c1updater") != 0 ||
        c1_update_join_path(source, sizeof(source), prepared_release,
                            component->path) != 0 ||
        c1_update_join_path(target, sizeof(target), slot_path, "c1updater") != 0)
        goto rejected;
    errno = 0;
    if (lstat(target, &information) == 0) {
        if (!S_ISREG(information.st_mode) || information.st_nlink != 1)
            goto rejected;
    } else if (errno != ENOENT) {
        goto rejected;
    }
    for (attempt = 0U; attempt < 128U; ++attempt) {
        count = snprintf(temporary, sizeof(temporary), "%s.new.%ld.%u",
                         target, (long)getpid(), attempt);
        if (count < 0 || (size_t)count >= sizeof(temporary)) goto rejected;
        if (access(temporary, F_OK) != 0 && errno == ENOENT) break;
    }
    if (attempt == 128U ||
        c1_update_copy_file(source, temporary, component->size, component->sha256,
                            0700, error, error_size) != 0 ||
        self_test(temporary, error, error_size) != 0 ||
        rename(temporary, target) != 0 ||
        c1_secure_sync_directory(slot_path, error, error_size) != 0) {
        (void)unlink(temporary);
        goto done;
    }
    if (installed_slot != NULL) *installed_slot = inactive;
    result = 0;
    goto done;
rejected:
    c1_secure_set_error(error, error_size,
                        "inactive updater slot request rejected");
done:
    c1_update_repository_release_free(&release);
    return result;
}