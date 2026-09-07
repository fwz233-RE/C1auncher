#include "update/boot.h"
#include "update/io.h"
#include "update/repository.h"
#include "security/secure_file.h"
#include "security/sha256.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

static int safe_release_name(const char *name)
{
    size_t i, length;
    if (name == NULL) return 0;
    length = strlen(name);
    if (length == 0U || length > 2U * C1_UPDATE_TOKEN_MAX + 24U) return 0;
    for (i = 0U; i < length; ++i) {
        unsigned char c = (unsigned char)name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-' || c == '+')) return 0;
    }
    return name[0] != '.' && name[length - 1U] != '.';
}

static int candidate_target(const struct c1_update_state *state, char *target, size_t size)
{
    int count = snprintf(target, size, "releases/%llu-%s",
                         (unsigned long long)state->sequence, state->release);
    return count >= 0 && (size_t)count < size ? 0 : -1;
}

static int parse_target(const char *target, const char **name)
{
    static const char prefix[] = "releases/";
    if (target == NULL || strncmp(target, prefix, sizeof(prefix) - 1U) != 0 ||
        strchr(target + sizeof(prefix) - 1U, '/') != NULL ||
        !safe_release_name(target + sizeof(prefix) - 1U)) return -1;
    *name = target + sizeof(prefix) - 1U;
    return 0;
}

static int read_pointer(const char *core_root, const char *which,
                        char *target, size_t target_size,
                        char *error, size_t error_size)
{
    char path[C1_UPDATE_PATH_MAX];
    struct stat information;
    ssize_t length;
    const char *name;
    if (c1_update_join_path(path, sizeof(path), core_root, which) != 0 ||
        lstat(path, &information) != 0 || !S_ISLNK(information.st_mode)) goto rejected;
    length = readlink(path, target, target_size);
    if (length <= 0 || (size_t)length >= target_size) goto rejected;
    target[(size_t)length] = '\0';
    if (parse_target(target, &name) != 0) goto rejected;
    (void)name;
    return 0;
rejected:
    c1_secure_set_error(error, error_size, "core pointer rejected");
    return -1;
}

static int atomic_pointer(const char *core_root, const char *which, const char *target,
                          char *error, size_t error_size)
{
    char path[C1_UPDATE_PATH_MAX], temporary[C1_UPDATE_PATH_MAX];
    struct stat information;
    const char *name;
    unsigned int attempt;
    int count;
    if (parse_target(target, &name) != 0 ||
        c1_update_join_path(path, sizeof(path), core_root, which) != 0) goto rejected;
    (void)name;
    errno = 0;
    if (lstat(path, &information) == 0) {
        if (!S_ISLNK(information.st_mode)) goto rejected;
    } else if (errno != ENOENT) goto rejected;
    for (attempt = 0U; attempt < 128U; ++attempt) {
        count = snprintf(temporary, sizeof(temporary), "%s.tmp.%ld.%u",
                         path, (long)getpid(), attempt);
        if (count < 0 || (size_t)count >= sizeof(temporary)) goto rejected;
        if (symlink(target, temporary) == 0) break;
        if (errno != EEXIST) goto rejected;
    }
    if (attempt == 128U || rename(temporary, path) != 0) {
        (void)unlink(temporary);
        goto rejected;
    }
    return c1_secure_sync_directory(core_root, error, error_size);
rejected:
    c1_secure_set_error(error, error_size, "core pointer replacement failed");
    return -1;
}

static int artifact_matches(const char *path, const struct c1_update_component *component,
                            char *error, size_t error_size)
{
    struct stat information;
    unsigned char digest[C1_SHA256_SIZE];
    char hex[C1_SHA256_HEX_SIZE];
    if (lstat(path, &information) != 0 || !S_ISREG(information.st_mode) ||
        information.st_nlink != 1 || (information.st_mode & 0777U) != component->mode ||
        c1_sha256_file(path, component->size, component->size, digest, NULL,
                       error, error_size) != 0) {
        c1_secure_set_error(error, error_size, "release artifact metadata rejected");
        return -1;
    }
    c1_sha256_hex(digest, hex);
    if (strcmp(hex, component->sha256) != 0) {
        c1_secure_set_error(error, error_size, "release artifact digest rejected");
        return -1;
    }
    return 0;
}

int c1_update_validate_release(const char *core_root, const char *target,
                               const char *key_path,
                               const struct c1_update_state *identity,
                               char *error, size_t error_size)
{
    struct c1_update_release release;
    unsigned char digest_bytes[C1_SHA256_SIZE];
    char digest[C1_SHA256_HEX_SIZE], release_path[C1_UPDATE_PATH_MAX];
    char artifacts[C1_UPDATE_PATH_MAX], artifact[C1_UPDATE_PATH_MAX], expected[192];
    char releases[C1_UPDATE_PATH_MAX];
    const char *name;
    size_t i;
    int count, result = -1;
    (void)memset(&release, 0, sizeof(release));
    if (parse_target(target, &name) != 0 ||
        c1_update_check_trusted_directory(core_root, (dev_t)-1, NULL, error, error_size) != 0 ||
        c1_update_join_path(releases, sizeof(releases), core_root, "releases") != 0 ||
        c1_update_check_trusted_directory(releases, (dev_t)-1, NULL, error, error_size) != 0 ||
        c1_update_join_path(release_path, sizeof(release_path), core_root, target) != 0 ||
        c1_update_check_trusted_directory(release_path, (dev_t)-1, NULL, error, error_size) != 0 ||
        c1_update_repository_load_local(release_path, key_path, &release, error, error_size) != 0) return -1;
    if (c1_update_check_compatibility(&release.manifest, error, error_size) != 0) goto done;
    count = snprintf(expected, sizeof(expected), "%llu-%s",
                     (unsigned long long)release.manifest.sequence, release.manifest.version);
    c1_sha256(release.manifest_data, release.manifest_size, digest_bytes);
    c1_sha256_hex(digest_bytes, digest);
    if (count < 0 || (size_t)count >= sizeof(expected) || strcmp(name, expected) != 0 ||
        (identity != NULL &&
         (identity->sequence != release.manifest.sequence ||
          identity->security_epoch != release.manifest.security_epoch ||
          strcmp(identity->release, release.manifest.version) != 0 ||
          strcmp(identity->digest, digest) != 0)) ||
        c1_update_join_path(artifacts, sizeof(artifacts), release_path, "artifacts") != 0 ||
        c1_update_check_trusted_directory(artifacts, (dev_t)-1, NULL, error, error_size) != 0) goto done;
    for (i = 0U; i < C1_UPDATE_COMPONENT_COUNT; ++i) {
        const char *base = strrchr(release.manifest.components[i].path, '/');
        if (base == NULL || c1_update_join_path(artifact, sizeof(artifact), artifacts, base + 1U) != 0 ||
            artifact_matches(artifact, &release.manifest.components[i], error, error_size) != 0) goto done;
    }
    result = 0;
done:
    if (result != 0 && error != NULL && error[0] == '\0')
        c1_secure_set_error(error, error_size, "release identity rejected");
    c1_update_repository_release_free(&release);
    return result;
}

int c1_update_validate_current(const char *core_root, const char *key_path,
                               const struct c1_update_state *identity,
                               char *error, size_t error_size)
{
    char current[192];
    if (read_pointer(core_root, "current", current, sizeof(current), error, error_size) != 0)
        return -1;
    return c1_update_validate_release(core_root, current, key_path, identity, error, error_size);
}

static int clear_pending_boots(const char *root, char *error, size_t error_size)
{
    char path[C1_UPDATE_PATH_MAX];
    if (c1_update_join_path(path, sizeof(path), root, "pending-boots.v1") != 0) return -1;
    if (unlink(path) != 0) return errno == ENOENT ? 0 : -1;
    return c1_secure_sync_directory(root, error, error_size);
}

static int valid_boot_id(const char *id)
{
    size_t i;
    if (id == NULL || strlen(id) != 36U) return 0;
    for (i = 0; i < 36U; ++i) {
        if (i == 8U || i == 13U || i == 18U || i == 23U) {
            if (id[i] != '-') return 0;
        } else if (!((id[i] >= '0' && id[i] <= '9') || (id[i] >= 'a' && id[i] <= 'f'))) return 0;
    }
    return 1;
}

/* Called with the transaction lock. Same boot + manifest digest is idempotent
 * even when activate/reconcile/recover are called repeatedly in one boot. */
static int count_pending_boot(const char *root, const struct c1_update_state *state,
                              const char *boot_id, char *error, size_t error_size)
{
    char path[C1_UPDATE_PATH_MAX], text[128], digest[65], previous_boot[37];
    unsigned char *data = NULL;
    size_t size = 0;
    unsigned int attempts = 0;
    struct stat information;
    int consumed = 0, length, result = -1;
    if (!valid_boot_id(boot_id) ||
        c1_update_join_path(path, sizeof(path), root, "pending-boots.v1") != 0) return -1;
    if (lstat(path, &information) == 0) {
        if (!S_ISREG(information.st_mode) || information.st_uid != geteuid() ||
            (information.st_mode & 0777U) != 0600U || information.st_nlink != 1 ||
            c1_secure_read_file(path, &data, &size, sizeof(text) - 1U,
                                C1_SECURE_FILE_ANY_SIZE, error, error_size) != 0 ||
            sscanf((const char *)data, "%64[0-9a-f]\n%36[0-9a-f-]\n%u%n",
                   digest, previous_boot, &attempts, &consumed) != 3 ||
            consumed < 0 || (size_t)consumed + 1U != size || data[consumed] != '\n' ||
            strlen(digest) != 64U || !valid_boot_id(previous_boot) ||
            attempts == 0U || attempts > C1_UPDATE_PENDING_BOOT_LIMIT) goto done;
        length = snprintf(text, sizeof(text), "%s\n%s\n%u\n", digest, previous_boot, attempts);
        if (length < 0 || (size_t)length != size || memcmp(text, data, size) != 0) goto done;
        if (strcmp(digest, state->digest) != 0) attempts = 0;
        else if (strcmp(previous_boot, boot_id) == 0) { result = 0; goto done; }
    } else if (errno != ENOENT) goto done;
    if (attempts >= C1_UPDATE_PENDING_BOOT_LIMIT) { result = 1; goto done; }
    length = snprintf(text, sizeof(text), "%s\n%s\n%u\n", state->digest, boot_id, attempts + 1U);
    if (length <= 0 || (size_t)length >= sizeof(text)) goto done;
    result = c1_secure_atomic_write(path, text, (size_t)length, 0600, error, error_size);
done:
    free(data);
    return result;
}

static int state_advance(const char *root, struct c1_update_state *state,
                         enum c1_update_phase phase, int failed,
                         char *error, size_t error_size)
{
    struct c1_update_state next;
    if (c1_update_transition(state, phase, state->sequence, state->security_epoch,
                             state->release, state->digest, failed, &next,
                             error, error_size) != 0 ||
        c1_update_state_commit(root, state, &next, error, error_size) != 0) return -1;
    *state = next;
    if (phase == C1_UPDATE_CONFIRMED || phase == C1_UPDATE_IDLE)
        return clear_pending_boots(root, error, error_size);
    return 0;
}

static int begin_locked(const char *state_root, const char *core_root,
                        char *lock_path, size_t lock_size, int *lock,
                        char *error, size_t error_size)
{
    if (c1_update_check_trusted_directory(state_root, (dev_t)-1, NULL, error, error_size) != 0 ||
        c1_update_check_trusted_directory(core_root, (dev_t)-1, NULL, error, error_size) != 0) return -1;
    *lock = c1_update_lock(state_root, lock_path, lock_size, error, error_size);
    return *lock < 0 ? *lock : 0;
}

static int recover_locked(const char *state_root, const char *core_root,
                          const char *key_path, struct c1_update_state *state,
                          char *error, size_t error_size);
static int rollback_marker_status(const char *state_root,
                                  const struct c1_update_state *state,
                                  char *error, size_t error_size);

int c1_update_activate(const char *state_root, const char *core_root,
                       const char *key_path, char *error, size_t error_size)
{
    struct c1_update_state state;
    char lock_path[C1_UPDATE_PATH_MAX] = "", current[192], candidate[192];
    int lock = -1, result = -1, rollback_status;
    if (begin_locked(state_root, core_root, lock_path, sizeof(lock_path), &lock, error, error_size) != 0) return lock == C1_UPDATE_BUSY ? C1_UPDATE_BUSY : -1;
    if (c1_update_state_load(state_root, &state, error, error_size) != 0) goto done;
    rollback_status = rollback_marker_status(state_root, &state, error, error_size);
    if (rollback_status < 0) goto done;
    if (rollback_status > 0) {
        result = recover_locked(state_root, core_root, key_path, &state, error, error_size);
        goto done;
    }
    if (state.phase == C1_UPDATE_PENDING_BOOT) {
        result = recover_locked(state_root, core_root, key_path, &state, error, error_size);
        goto done;
    }
    if (state.phase != C1_UPDATE_PREPARED || candidate_target(&state, candidate, sizeof(candidate)) != 0 ||
        c1_update_validate_release(core_root, candidate, key_path, &state, error, error_size) != 0 ||
        read_pointer(core_root, "current", current, sizeof(current), error, error_size) != 0 ||
        c1_update_validate_release(core_root, current, key_path, NULL, error, error_size) != 0 ||
        atomic_pointer(core_root, "previous", current, error, error_size) != 0 ||
        state_advance(state_root, &state, C1_UPDATE_PENDING_BOOT, 0, error, error_size) != 0 ||
        atomic_pointer(core_root, "current", candidate, error, error_size) != 0) goto done;
    result = 0;
done:
    c1_update_unlock(lock, lock_path);
    return result;
}

static int bootstrap_pointer_status(const char *core_root, const char *which,
                                    const char *candidate,
                                    char *error, size_t error_size)
{
    char path[C1_UPDATE_PATH_MAX], target[192];
    struct stat information;
    if (c1_update_join_path(path, sizeof(path), core_root, which) != 0) return -1;
    errno = 0;
    if (lstat(path, &information) != 0) {
        if (errno == ENOENT) return 0;
        goto rejected;
    }
    if (!S_ISLNK(information.st_mode) ||
        read_pointer(core_root, which, target, sizeof(target), error, error_size) != 0 ||
        strcmp(target, candidate) != 0) goto rejected;
    return 1;
rejected:
    c1_secure_set_error(error, error_size, "bootstrap core pointer rejected");
    return -1;
}

int c1_update_bootstrap_activate(const char *state_root, const char *core_root,
                                 const char *key_path, char *error, size_t error_size)
{
    struct c1_update_state state;
    char lock_path[C1_UPDATE_PATH_MAX] = "", candidate[192];
    int lock = -1, current_status, previous_status, result = -1;
    if (begin_locked(state_root, core_root, lock_path, sizeof(lock_path), &lock,
                     error, error_size) != 0) return lock == C1_UPDATE_BUSY ? C1_UPDATE_BUSY : -1;
    if (c1_update_state_load(state_root, &state, error, error_size) != 0 ||
        (state.phase != C1_UPDATE_PREPARED && state.phase != C1_UPDATE_PENDING_BOOT) ||
        candidate_target(&state, candidate, sizeof(candidate)) != 0 ||
        c1_update_validate_release(core_root, candidate, key_path, &state,
                                   error, error_size) != 0) goto done;
    current_status = bootstrap_pointer_status(core_root, "current", candidate,
                                              error, error_size);
    previous_status = bootstrap_pointer_status(core_root, "previous", candidate,
                                               error, error_size);
    if (current_status < 0 || previous_status < 0 ||
        (state.phase == C1_UPDATE_PREPARED &&
         (current_status != 0 || previous_status != 0))) goto done;
    if (state.phase == C1_UPDATE_PREPARED &&
        state_advance(state_root, &state, C1_UPDATE_PENDING_BOOT, 0,
                      error, error_size) != 0) goto done;
    if (current_status == 0 &&
        atomic_pointer(core_root, "current", candidate, error, error_size) != 0) goto done;
    if (previous_status == 0 &&
        atomic_pointer(core_root, "previous", candidate, error, error_size) != 0) goto done;
    if (state_advance(state_root, &state, C1_UPDATE_CONFIRMED, 0,
                      error, error_size) != 0) goto done;
    result = 0;
done:
    c1_update_unlock(lock, lock_path);
    return result;
}

static int rollback_marker_path(const char *state_root, char *path, size_t size)
{
    return c1_update_join_path(path, size, state_root, "rollback.v1");
}

static int rollback_marker_status(const char *state_root,
                                  const struct c1_update_state *state,
                                  char *error, size_t error_size)
{
    struct stat information;
    unsigned char *data = NULL;
    size_t size = 0U;
    char path[C1_UPDATE_PATH_MAX], expected[66];
    int count, result = -1;

    if (rollback_marker_path(state_root, path, sizeof(path)) != 0) goto rejected;
    errno = 0;
    if (lstat(path, &information) != 0) {
        if (errno == ENOENT) return 0;
        goto rejected;
    }
    count = snprintf(expected, sizeof(expected), "%s\n", state->digest);
    if (count != 65 || !S_ISREG(information.st_mode) || information.st_nlink != 1 ||
        (information.st_mode & 0777U) != 0600U ||
        c1_secure_read_file(path, &data, &size, 65U, 65U, error, error_size) != 0 ||
        memcmp(data, expected, 65U) != 0) goto rejected;
    result = 1;
rejected:
    free(data);
    if (result < 0) c1_secure_set_error(error, error_size, "rollback marker rejected");
    return result;
}

static int mark_rollback(const char *state_root, const struct c1_update_state *state,
                         char *error, size_t error_size)
{
    char path[C1_UPDATE_PATH_MAX], text[66];
    int count;

    count = snprintf(text, sizeof(text), "%s\n", state->digest);
    if (count != 65 || rollback_marker_path(state_root, path, sizeof(path)) != 0)
        return -1;
    return c1_secure_atomic_write(path, text, (size_t)count, 0600, error, error_size);
}

static int clear_rollback_marker(const char *state_root, char *error, size_t error_size)
{
    char path[C1_UPDATE_PATH_MAX];

    if (rollback_marker_path(state_root, path, sizeof(path)) != 0) return -1;
    if (unlink(path) != 0 && errno != ENOENT) {
        c1_secure_set_error(error, error_size, "rollback marker removal failed: %s",
                            strerror(errno));
        return -1;
    }
    return c1_secure_sync_directory(state_root, error, error_size);
}

static int rollback_locked(const char *state_root, const char *core_root,
                           const char *previous, struct c1_update_state *state,
                           char *error, size_t error_size)
{
    if (mark_rollback(state_root, state, error, error_size) != 0 ||
        atomic_pointer(core_root, "current", previous, error, error_size) != 0 ||
        (state->phase == C1_UPDATE_PENDING_BOOT &&
         state_advance(state_root, state, C1_UPDATE_PREPARED, 0, error, error_size) != 0) ||
        ((state->phase == C1_UPDATE_PREPARED ||
          state->phase == C1_UPDATE_CONFIRMED) &&
         state_advance(state_root, state, C1_UPDATE_IDLE, 1,
                       error, error_size) != 0) ||
        state->phase != C1_UPDATE_IDLE ||
        clear_rollback_marker(state_root, error, error_size) != 0) return -1;
    return 0;
}

static int recover_locked(const char *state_root, const char *core_root,
                          const char *key_path, struct c1_update_state *state,
                          char *error, size_t error_size)
{
    char current[192], previous[192], candidate[192];
    int candidate_valid, rollback_status;

    rollback_status = rollback_marker_status(state_root, state, error, error_size);
    if (rollback_status < 0) return -1;
    if (rollback_status > 0) {
        if ((state->phase != C1_UPDATE_PENDING_BOOT &&
             state->phase != C1_UPDATE_PREPARED && state->phase != C1_UPDATE_IDLE &&
             state->phase != C1_UPDATE_CONFIRMED) ||
            read_pointer(core_root, "previous", previous, sizeof(previous), error, error_size) != 0 ||
            c1_update_validate_release(core_root, previous, key_path, NULL,
                                       error, error_size) != 0)
            return -1;
        if (state->phase == C1_UPDATE_IDLE) {
            if (atomic_pointer(core_root, "current", previous, error, error_size) != 0)
                return -1;
            return clear_rollback_marker(state_root, error, error_size);
        }
        if (state->phase == C1_UPDATE_CONFIRMED) {
            if (atomic_pointer(core_root, "current", previous, error, error_size) != 0 ||
                state_advance(state_root, state, C1_UPDATE_IDLE, 1,
                              error, error_size) != 0)
                return -1;
            return clear_rollback_marker(state_root, error, error_size);
        }
        return rollback_locked(state_root, core_root, previous, state, error, error_size);
    }
    if (state->phase != C1_UPDATE_PENDING_BOOT) return 0;
    if (candidate_target(state, candidate, sizeof(candidate)) != 0 ||
        read_pointer(core_root, "previous", previous, sizeof(previous), error, error_size) != 0 ||
        c1_update_validate_release(core_root, previous, key_path, NULL, error, error_size) != 0)
        return -1;
    candidate_valid = c1_update_validate_release(core_root, candidate, key_path, state,
                                                 error, error_size) == 0;
    if (candidate_valid &&
        read_pointer(core_root, "current", current, sizeof(current), error, error_size) == 0) {
        if (strcmp(current, candidate) == 0) return 0;
        if (strcmp(current, previous) == 0)
            return atomic_pointer(core_root, "current", candidate, error, error_size);
    }
    return rollback_locked(state_root, core_root, previous, state, error, error_size);
}

int c1_update_recover_boot(const char *state_root, const char *core_root,
                           const char *key_path, const char *boot_id,
                           char *error, size_t error_size)
{
    struct c1_update_state state;
    char lock_path[C1_UPDATE_PATH_MAX] = "", previous[192];
    int lock = -1, result = -1;
    if (!valid_boot_id(boot_id)) return -1;
    if (begin_locked(state_root, core_root, lock_path, sizeof(lock_path), &lock, error, error_size) != 0) return lock == C1_UPDATE_BUSY ? C1_UPDATE_BUSY : -1;
    if (c1_update_state_load(state_root, &state, error, error_size) != 0) goto done;
    result = recover_locked(state_root, core_root, key_path, &state, error, error_size);
    if (result != 0) goto done;
    if (state.phase == C1_UPDATE_PENDING_BOOT) {
        result = count_pending_boot(state_root, &state, boot_id, error, error_size);
        if (result != 0) {
            if (read_pointer(core_root, "previous", previous, sizeof(previous), error, error_size) != 0 ||
                c1_update_validate_release(core_root, previous, key_path, NULL, error, error_size) != 0) {
                result = -1;
                goto done;
            }
            result = rollback_locked(state_root, core_root, previous, &state, error, error_size);
        }
    } else result = clear_pending_boots(state_root, error, error_size);
done:
    c1_update_unlock(lock, lock_path);
    return result;
}

int c1_update_recover(const char *state_root, const char *core_root,
                      const char *key_path, char *error, size_t error_size)
{
    char boot_id[38];
    int fd = open("/proc/sys/kernel/random/boot_id", O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    ssize_t count;
    if (fd < 0) return -1;
    do { count = read(fd, boot_id, sizeof(boot_id)); } while (count < 0 && errno == EINTR);
    (void)close(fd);
    if (count != 37 || boot_id[36] != '\n') return -1;
    boot_id[36] = '\0';
    return c1_update_recover_boot(state_root, core_root, key_path, boot_id, error, error_size);
}

int c1_update_confirm(const char *state_root, const char *core_root,
                      const char *key_path, char *error, size_t error_size)
{
    struct c1_update_state state;
    char lock_path[C1_UPDATE_PATH_MAX] = "", current[192], candidate[192];
    int lock = -1, result = -1;
    if (begin_locked(state_root, core_root, lock_path, sizeof(lock_path), &lock, error, error_size) != 0) return lock == C1_UPDATE_BUSY ? C1_UPDATE_BUSY : -1;
    if (c1_update_state_load(state_root, &state, error, error_size) != 0 ||
        state.phase != C1_UPDATE_PENDING_BOOT || candidate_target(&state, candidate, sizeof(candidate)) != 0 ||
        read_pointer(core_root, "current", current, sizeof(current), error, error_size) != 0 ||
        strcmp(current, candidate) != 0 ||
        c1_update_validate_release(core_root, current, key_path, &state, error, error_size) != 0 ||
        state_advance(state_root, &state, C1_UPDATE_CONFIRMED, 0, error, error_size) != 0) goto done;
    result = 0;
done:
    c1_update_unlock(lock, lock_path);
    return result;
}

int c1_update_rollback(const char *state_root, const char *core_root,
                       const char *key_path, char *error, size_t error_size)
{
    struct c1_update_state state;
    char lock_path[C1_UPDATE_PATH_MAX] = "", previous[192], current[192];
    int lock = -1, result = -1;
    if (begin_locked(state_root, core_root, lock_path, sizeof(lock_path), &lock, error, error_size) != 0) return lock == C1_UPDATE_BUSY ? C1_UPDATE_BUSY : -1;
    if (c1_update_state_load(state_root, &state, error, error_size) != 0 ||
        (state.phase != C1_UPDATE_PENDING_BOOT && state.phase != C1_UPDATE_CONFIRMED) ||
        read_pointer(core_root, "previous", previous, sizeof(previous), error, error_size) != 0 ||
        (state.phase == C1_UPDATE_CONFIRMED &&
         (read_pointer(core_root, "current", current, sizeof(current), error, error_size) != 0 ||
          strcmp(current, previous) == 0)) ||
        c1_update_validate_release(core_root, previous, key_path, NULL, error, error_size) != 0 ||
        rollback_locked(state_root, core_root, previous, &state, error, error_size) != 0) goto done;
    result = 0;
done:
    c1_update_unlock(lock, lock_path);
    return result;
}

/* This entry point is invoked ONLY through the immutable enrolled recovery
 * binary. A generation's updater is data until this independent verifier has
 * checked its signed manifest, every artifact and the exact opened exec inode. */
int c1_update_recovery_exec(const char *state_root, const char *core_root,
                            const char *key_path, const char *which,
                            const char *ready_file, const char *launcher_path,
                            char *error, size_t error_size)
{
    char lock_path[C1_UPDATE_PATH_MAX], target[192], release_path[C1_UPDATE_PATH_MAX];
    char path[C1_UPDATE_PATH_MAX], hex[C1_SHA256_HEX_SIZE];
    struct c1_update_release release;
    struct c1_update_state state;
    struct stat information;
    struct c1_sha256_context hash;
    unsigned char buffer[16384], digest[C1_SHA256_SIZE];
    uint64_t total = 0;
    int lock = -1, fd = -1, result = -1;
    const struct c1_update_component *component = NULL;
    size_t i;
    extern char **environ;
    char *const arguments[] = { "c1updater", "supervise", (char *)state_root,
        (char *)core_root, (char *)key_path, (char *)ready_file, (char *)launcher_path, NULL };
    (void)memset(&release, 0, sizeof(release));
    if (strcmp(which, "previous") != 0 && strcmp(which, "current") != 0) return -1;
    if (begin_locked(state_root, core_root, lock_path, sizeof(lock_path), &lock,
                     error, error_size) != 0) return lock == C1_UPDATE_BUSY ? C1_UPDATE_BUSY : -1;
    if (c1_update_state_load(state_root, &state, error, error_size) != 0 ||
        read_pointer(core_root, which, target, sizeof(target), error, error_size) != 0 ||
        c1_update_validate_release(core_root, target, key_path,
            strcmp(which, "current") == 0 && (state.phase == C1_UPDATE_CONFIRMED ||
            state.phase == C1_UPDATE_PENDING_BOOT) ? &state : NULL, error, error_size) != 0 ||
        c1_update_join_path(release_path, sizeof(release_path), core_root, target) != 0 ||
        c1_update_repository_load_local(release_path, key_path, &release, error, error_size) != 0)
        goto done;
    for (i = 0; i < C1_UPDATE_COMPONENT_COUNT; ++i)
        if (strcmp(release.manifest.components[i].role, "updater") == 0)
            component = &release.manifest.components[i];
    if (component == NULL ||
        c1_update_join_path(path, sizeof(path), release_path, component->path) != 0) goto done;
    fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0 || fstat(fd, &information) != 0 || !S_ISREG(information.st_mode) ||
        information.st_nlink != 1 || information.st_uid != geteuid() ||
        (information.st_mode & 0777U) != 0700U) goto done;
    c1_sha256_init(&hash);
    for (;;) {
        ssize_t amount = read(fd, buffer, sizeof(buffer));
        if (amount < 0 && errno == EINTR) continue;
        if (amount < 0) goto done;
        if (amount == 0) break;
        total += (uint64_t)amount;
        if (total > component->size) goto done;
        c1_sha256_update(&hash, buffer, (size_t)amount);
    }
    c1_sha256_final(&hash, digest);
    c1_sha256_hex(digest, hex);
    if (total != component->size || strcmp(hex, component->sha256) != 0 || lseek(fd, 0, SEEK_SET) < 0)
        goto done;
    if (strcmp(which, "previous") == 0) {
        if (state.phase == C1_UPDATE_PENDING_BOOT || state.phase == C1_UPDATE_CONFIRMED ||
            state.phase == C1_UPDATE_PREPARED) {
            if (rollback_locked(state_root, core_root, target, &state, error, error_size) != 0) goto done;
        } else if (atomic_pointer(core_root, "current", target, error, error_size) != 0) goto done;
    }
    /* CLOEXEC releases the persistent lock inode; the bootstrap stays parent. */
    fexecve(fd, arguments, environ);
    c1_secure_set_error(error, error_size, "verified recovery updater exec failed: %s", strerror(errno));
done:
    if (fd >= 0) (void)close(fd);
    c1_update_repository_release_free(&release);
    c1_update_unlock(lock, lock_path);
    return result;
}

int c1_update_mark_ready(const char *state_root, const char *ready_file,
                         char *error, size_t error_size)
{
    struct c1_update_state state;
    char text[66];
    int count;
    if (c1_update_state_load(state_root, &state, error, error_size) != 0 ||
        state.phase != C1_UPDATE_PENDING_BOOT) {
        c1_secure_set_error(error, error_size, "ready marker requires pending boot");
        return -1;
    }
    count = snprintf(text, sizeof(text), "%s\n", state.digest);
    if (count != 65) return -1;
    return c1_secure_atomic_write(ready_file, text, (size_t)count, 0600, error, error_size);
}

int c1_update_ready_matches(const char *ready_file, const char *digest,
                            char *error, size_t error_size)
{
    struct stat information;
    unsigned char *data = NULL;
    size_t size = 0U;
    char expected[66];
    int count, result = -1;
    if (lstat(ready_file, &information) != 0 || !S_ISREG(information.st_mode) ||
        information.st_nlink != 1 || (information.st_mode & 0777U) != 0600U) goto rejected;
    count = snprintf(expected, sizeof(expected), "%s\n", digest);
    if (count != 65 || c1_secure_read_file(ready_file, &data, &size, 65U, 65U,
                                           error, error_size) != 0) goto rejected;
    if (memcmp(data, expected, 65U) == 0) result = 0;
rejected:
    free(data);
    if (result != 0) c1_secure_set_error(error, error_size, "ready marker rejected");
    return result;
}