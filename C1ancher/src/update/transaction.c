#include "update/transaction.h"
#include "update/io.h"
#include "update/repository.h"
#include "security/secure_file.h"
#include "security/sha256.h"

#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int default_space_check(const char *path, uint64_t required, void *context,
                               char *error, size_t error_size)
{
    (void)context;
    return c1_update_check_space(path, required, error, error_size);
}

static int sum_payload(const struct c1_update_manifest *manifest, uint64_t *total)
{
    size_t i;
    uint64_t value = 0U;
    for (i = 0U; i < C1_UPDATE_COMPONENT_COUNT; ++i) {
        if (value > UINT64_MAX - manifest->components[i].size) return -1;
        value += manifest->components[i].size;
    }
    if (value > UINT64_MAX - C1_UPDATE_SPACE_RESERVE) return -1;
    *total = value + C1_UPDATE_SPACE_RESERVE;
    return 0;
}

static int make_name(char *out, size_t size, uint64_t sequence, const char *version)
{
    int count = snprintf(out, size, "%llu-%s", (unsigned long long)sequence, version);
    return count >= 0 && (size_t)count < size ? 0 : -1;
}

static int state_identity(const struct c1_update_state *state,
                          const struct c1_update_release *release, const char *digest)
{
    return state->sequence == release->manifest.sequence &&
           state->security_epoch == release->manifest.security_epoch &&
           strcmp(state->release, release->manifest.version) == 0 &&
           strcmp(state->digest, digest) == 0;
}

static int advance_state(const char *state_root, struct c1_update_state *state,
                         enum c1_update_phase phase,
                         const struct c1_update_release *release, const char *digest,
                         char *error, size_t error_size)
{
    struct c1_update_state next;
    if (c1_update_transition(state, phase, release->manifest.sequence,
                             release->manifest.security_epoch,
                             release->manifest.version, digest, 0, &next,
                             error, error_size) != 0 ||
        c1_update_state_commit(state_root, state, &next, error, error_size) != 0) return -1;
    *state = next;
    return 0;
}

static void fail_state(const char *state_root, const struct c1_update_release *release,
                       const char *digest)
{
    struct c1_update_state state, next;
    char ignored[C1_UPDATE_ERROR_MAX] = "";
    if (c1_update_state_load(state_root, &state, ignored, sizeof(ignored)) == 0 &&
        state.generation != 0U && state.phase != C1_UPDATE_IDLE &&
        state_identity(&state, release, digest) &&
        c1_update_transition(&state, C1_UPDATE_IDLE, release->manifest.sequence,
                             release->manifest.security_epoch,
                             release->manifest.version, digest, 1, &next,
                             ignored, sizeof(ignored)) == 0) {
        (void)c1_update_state_commit(state_root, &state, &next, ignored, sizeof(ignored));
    }
}

static int count_entries(const char *path, size_t expected)
{
    DIR *directory = opendir(path);
    struct dirent *entry;
    size_t count = 0U;
    if (directory == NULL) return -1;
    while ((entry = readdir(directory)) != NULL) {
        if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0) ++count;
    }
    if (closedir(directory) != 0) return -1;
    return count == expected ? 0 : -1;
}

static int file_matches(const char *path, uint64_t size, const char *expected,
                        char *error, size_t error_size)
{
    unsigned char digest[C1_SHA256_SIZE];
    char hex[C1_SHA256_HEX_SIZE];
    if (c1_sha256_file(path, size, size, digest, NULL, error, error_size) != 0) return -1;
    c1_sha256_hex(digest, hex);
    if (strcmp(hex, expected) != 0) {
        c1_secure_set_error(error, error_size, "release file digest rejected");
        return -1;
    }
    return 0;
}

static int artifact_matches(const char *path, uint64_t size, const char *expected,
                            char *error, size_t error_size)
{
    struct stat information;
    if (lstat(path, &information) != 0 || !S_ISREG(information.st_mode) ||
        information.st_nlink != 1 || (information.st_mode & 0777U) != 0700U) {
        c1_secure_set_error(error, error_size, "prepared artifact metadata rejected");
        return -1;
    }
    return file_matches(path, size, expected, error, error_size);
}

static int existing_release_matches(const char *final_path,
                                    const struct c1_update_release *release,
                                    const char *manifest_digest,
                                    char *error, size_t error_size)
{
    char path[C1_UPDATE_PATH_MAX], artifacts[C1_UPDATE_PATH_MAX];
    unsigned char signature_hash[C1_SHA256_SIZE];
    char signature_hex[C1_SHA256_HEX_SIZE];
    size_t i;
    c1_sha256(release->signature, sizeof(release->signature), signature_hash);
    c1_sha256_hex(signature_hash, signature_hex);
    if (c1_update_check_trusted_directory(final_path, (dev_t)-1, NULL, error, error_size) != 0 ||
        count_entries(final_path, 3U) != 0 ||
        c1_update_join_path(artifacts, sizeof(artifacts), final_path, "artifacts") != 0 ||
        c1_update_check_trusted_directory(artifacts, (dev_t)-1, NULL, error, error_size) != 0 ||
        count_entries(artifacts, C1_UPDATE_COMPONENT_COUNT) != 0 ||
        c1_update_join_path(path, sizeof(path), final_path, C1_UPDATE_MANIFEST_NAME) != 0 ||
        file_matches(path, release->manifest_size, manifest_digest, error, error_size) != 0 ||
        c1_update_join_path(path, sizeof(path), final_path, C1_UPDATE_SIGNATURE_NAME) != 0 ||
        file_matches(path, sizeof(release->signature), signature_hex, error, error_size) != 0) return -1;
    for (i = 0U; i < C1_UPDATE_COMPONENT_COUNT; ++i) {
        const struct c1_update_component *component = &release->manifest.components[i];
        const char *name = strrchr(component->path, '/');
        if (name == NULL || c1_update_join_path(path, sizeof(path), artifacts, name + 1U) != 0 ||
            artifact_matches(path, component->size, component->sha256,
                             error, error_size) != 0) return -1;
    }
    return 0;
}

static int write_release_metadata(const char *draft, const struct c1_update_release *release,
                                  char *error, size_t error_size)
{
    char path[C1_UPDATE_PATH_MAX];
    if (c1_update_join_path(path, sizeof(path), draft, C1_UPDATE_MANIFEST_NAME) != 0 ||
        c1_secure_atomic_write(path, release->manifest_data, release->manifest_size, 0600,
                               error, error_size) != 0 ||
        c1_update_join_path(path, sizeof(path), draft, C1_UPDATE_SIGNATURE_NAME) != 0 ||
        c1_secure_atomic_write(path, release->signature, sizeof(release->signature), 0600,
                               error, error_size) != 0) return -1;
    return 0;
}

static int acquire_artifacts(const char *source, int remote, const char *staging_artifacts,
                             const struct c1_update_release *release,
                             char *error, size_t error_size)
{
    size_t i;
    for (i = 0U; i < C1_UPDATE_COMPONENT_COUNT; ++i) {
        const struct c1_update_component *component = &release->manifest.components[i];
        const char *name = strrchr(component->path, '/');
        char input[C1_UPDATE_PATH_MAX], output[C1_UPDATE_PATH_MAX];
        if (name == NULL || c1_update_join_path(output, sizeof(output), staging_artifacts, name + 1U) != 0) return -1;
        if (remote != 0) {
            if (c1_update_repository_download(source, component->path, output,
                                              component->size, component->size,
                                              error, error_size) != 0) return -1;
        } else {
            if (c1_update_join_path(input, sizeof(input), source, component->path) != 0 ||
                c1_update_copy_file(input, output, component->size, component->sha256,
                                    0600, error, error_size) != 0) return -1;
        }
        if (file_matches(output, component->size, component->sha256, error, error_size) != 0) return -1;
    }
    return 0;
}

static int copy_to_draft(const char *staging_artifacts, const char *draft_artifacts,
                         const struct c1_update_release *release,
                         char *error, size_t error_size)
{
    size_t i;
    for (i = 0U; i < C1_UPDATE_COMPONENT_COUNT; ++i) {
        const struct c1_update_component *component = &release->manifest.components[i];
        const char *name = strrchr(component->path, '/');
        char source[C1_UPDATE_PATH_MAX], destination[C1_UPDATE_PATH_MAX];
        if (name == NULL ||
            c1_update_join_path(source, sizeof(source), staging_artifacts, name + 1U) != 0 ||
            c1_update_join_path(destination, sizeof(destination), draft_artifacts, name + 1U) != 0 ||
            c1_update_copy_file(source, destination, component->size, component->sha256,
                                0700, error, error_size) != 0 ||
            file_matches(destination, component->size, component->sha256,
                         error, error_size) != 0) return -1;
    }
    return 0;
}

static int validate_config(const struct c1_update_transaction_config *config,
                           dev_t *staging_device, dev_t *core_device,
                           char *error, size_t error_size)
{
    dev_t state_device;
    if (config == NULL || config->staging_root == NULL || config->core_root == NULL ||
        config->state_root == NULL || config->key_path == NULL ||
        c1_update_check_trusted_directory(config->staging_root, (dev_t)-1, staging_device,
                                          error, error_size) != 0 ||
        c1_update_check_trusted_directory(config->core_root, (dev_t)-1, core_device,
                                          error, error_size) != 0 ||
        c1_update_check_trusted_directory(config->state_root, (dev_t)-1, &state_device,
                                          error, error_size) != 0) return -1;
    return 0;
}

static int process_release(const struct c1_update_transaction_config *config,
                           const char *source, int remote, const char *transaction,
                           const char *staging_artifacts, dev_t staging_device, dev_t core_device,
                           struct c1_update_release *release,
                           struct c1_update_transaction_result *result,
                           char *error, size_t error_size)
{
    unsigned char digest_bytes[C1_SHA256_SIZE];
    char digest[C1_SHA256_HEX_SIZE];
    char releases[C1_UPDATE_PATH_MAX], release_name[96], final_path[C1_UPDATE_PATH_MAX];
    char draft_name[128], draft[C1_UPDATE_PATH_MAX], draft_artifacts[C1_UPDATE_PATH_MAX];
    struct c1_update_state state;
    struct stat information;
    c1_update_space_check_fn check = config->space_check == NULL ? default_space_check : config->space_check;
    uint64_t space_required;
    int draft_created = 0, final_exists = 0, state_started = 0;
    int rc = -1;

    if (c1_update_check_compatibility(&release->manifest, error, error_size) != 0) goto done;
    c1_sha256(release->manifest_data, release->manifest_size, digest_bytes);
    c1_sha256_hex(digest_bytes, digest);
    if (sum_payload(&release->manifest, &space_required) != 0 ||
        space_required > UINT64_MAX - release->manifest_size - sizeof(release->signature)) goto done;
    space_required += (uint64_t)release->manifest_size + (uint64_t)sizeof(release->signature);
    if (make_name(release_name, sizeof(release_name), release->manifest.sequence,
                  release->manifest.version) != 0 ||
        c1_update_state_load(config->state_root, &state, error, error_size) != 0) goto done;
    if (state.generation != 0U &&
        (state.sequence > release->manifest.sequence ||
         state.security_epoch > release->manifest.security_epoch)) {
        c1_secure_set_error(error, error_size, "release rollback rejected");
        goto done;
    }
    if (state.generation != 0U && state.sequence == release->manifest.sequence &&
        !state_identity(&state, release, digest)) {
        c1_secure_set_error(error, error_size, "release identity conflict rejected");
        goto done;
    }
    if (state.phase == C1_UPDATE_CONFIRMED && state_identity(&state, release, digest)) {
        /* A signed, exact confirmed identity is a successful no-op. Revalidate
         * the immutable release without creating directories, advancing state,
         * checking free space or acquiring/replacing any artifacts. */
        if (c1_update_join_path(releases, sizeof(releases), config->core_root, "releases") != 0 ||
            c1_update_check_trusted_directory(releases, core_device, NULL, error, error_size) != 0 ||
            c1_update_join_path(final_path, sizeof(final_path), releases, release_name) != 0 ||
            existing_release_matches(final_path, release, digest, error, error_size) != 0) {
            c1_secure_set_error(error, error_size, "confirmed immutable release conflicts");
            goto done;
        }
        rc = 0;
        goto success;
    }
    if (state.phase != C1_UPDATE_IDLE && state.phase != C1_UPDATE_CONFIRMED &&
        !state_identity(&state, release, digest)) {
        c1_secure_set_error(error, error_size, "another update transaction is active");
        goto done;
    }
    if (c1_update_join_path(releases, sizeof(releases), config->core_root, "releases") != 0 ||
        c1_update_make_directory(releases, 0700, core_device, 1, error, error_size) != 0 ||
        c1_update_join_path(final_path, sizeof(final_path), releases, release_name) != 0) goto done;
    errno = 0;
    if (lstat(final_path, &information) == 0) {
        final_exists = 1;
        if (existing_release_matches(final_path, release, digest, error, error_size) != 0) {
            c1_secure_set_error(error, error_size, "existing immutable release conflicts");
            goto done;
        }
    } else if (errno != ENOENT) goto done;
    if (state.phase == C1_UPDATE_PREPARED) {
        if (!final_exists) {
            c1_secure_set_error(error, error_size, "prepared state has no immutable release");
            goto done;
        }
        rc = 0;
        goto success;
    }
    if (state.phase == C1_UPDATE_IDLE || state.phase == C1_UPDATE_CONFIRMED) {
        if (advance_state(config->state_root, &state, C1_UPDATE_DOWNLOADING,
                          release, digest, error, error_size) != 0) goto done;
        state_started = 1;
    } else if (state.phase == C1_UPDATE_DOWNLOADING || state.phase == C1_UPDATE_VERIFIED) {
        state_started = 1;
    } else {
        c1_secure_set_error(error, error_size, "update phase cannot prepare a release");
        goto done;
    }
    /* Each invocation owns fresh staging. VERIFIED is durable but its old
     * staging payloads may have disappeared across interruption or reboot. */
    if (!final_exists &&
        (check(config->staging_root, space_required, config->space_context,
               error, error_size) != 0 ||
         acquire_artifacts(source, remote, staging_artifacts, release,
                           error, error_size) != 0)) goto done;
    if (state.phase == C1_UPDATE_DOWNLOADING &&
        advance_state(config->state_root, &state, C1_UPDATE_VERIFIED,
                      release, digest, error, error_size) != 0) goto done;
    if (!final_exists) {
        int count;
        if (check(config->core_root, space_required, config->space_context,
                  error, error_size) != 0) goto done;
        count = snprintf(draft_name, sizeof(draft_name), ".new.%s.%ld.XXXXXX", release_name, (long)getpid());
        if (count < 0 || (size_t)count >= sizeof(draft_name) ||
            c1_update_join_path(draft, sizeof(draft), releases, draft_name) != 0 ||
            mkdtemp(draft) == NULL) goto done;
        draft_created = 1;
        if (c1_update_join_path(draft_artifacts, sizeof(draft_artifacts), draft, "artifacts") != 0 ||
            c1_update_make_directory(draft_artifacts, 0700, core_device, 0, error, error_size) != 0) goto done;
        if (copy_to_draft(staging_artifacts, draft_artifacts, release, error, error_size) != 0 ||
            c1_secure_sync_directory(draft_artifacts, error, error_size) != 0 ||
            write_release_metadata(draft, release, error, error_size) != 0 ||
            c1_secure_sync_directory(draft, error, error_size) != 0 ||
            rename(draft, final_path) != 0 ||
            c1_secure_sync_directory(releases, error, error_size) != 0) goto done;
        draft_created = 0;
    }
    if (advance_state(config->state_root, &state, C1_UPDATE_PREPARED,
                      release, digest, error, error_size) != 0) goto done;
    rc = 0;
success:
    result->phase = state.phase;
    result->sequence = release->manifest.sequence;
    (void)strcpy(result->release, release->manifest.version);
    if (error != NULL && error_size != 0U) error[0] = '\0';
done:
    if (draft_created != 0) (void)c1_update_remove_tree(draft);
    if (rc != 0 && state_started != 0) fail_state(config->state_root, release, digest);
    (void)c1_update_remove_tree(transaction);
    (void)staging_device;
    return rc;
}

static int prepare_common(const struct c1_update_transaction_config *config,
                          const char *source, int remote,
                          struct c1_update_transaction_result *result,
                          char *error, size_t error_size)
{
    struct c1_update_release release;
    char lock_path[C1_UPDATE_PATH_MAX] = "";
    char transaction_name[64], transaction[C1_UPDATE_PATH_MAX];
    char staging_artifacts[C1_UPDATE_PATH_MAX];
    dev_t staging_device, core_device;
    int lock = -1, count, transaction_created = 0, rc = -1;
    if (result == NULL || source == NULL || validate_config(config, &staging_device, &core_device,
                                                            error, error_size) != 0) return -1;
    (void)memset(result, 0, sizeof(*result));
    (void)memset(&release, 0, sizeof(release));
    lock = c1_update_lock(config->state_root, lock_path, sizeof(lock_path), error, error_size);
    if (lock < 0) return lock;
    count = snprintf(transaction_name, sizeof(transaction_name), ".txn.%ld.XXXXXX", (long)getpid());
    if (count < 0 || (size_t)count >= sizeof(transaction_name) ||
        c1_update_join_path(transaction, sizeof(transaction), config->staging_root,
                            transaction_name) != 0 || mkdtemp(transaction) == NULL) goto done;
    transaction_created = 1;
    if ((remote != 0 && c1_update_repository_fetch_release(source, transaction, config->key_path,
                                                            &release, error, error_size) != 0) ||
        (remote == 0 && c1_update_repository_load_local(source, config->key_path,
                                                        &release, error, error_size) != 0) ||
        c1_update_join_path(staging_artifacts, sizeof(staging_artifacts), transaction,
                            "artifacts") != 0 ||
        c1_update_make_directory(staging_artifacts, 0700, staging_device, 0,
                                 error, error_size) != 0) goto done;
    if (remote == 0 && write_release_metadata(transaction, &release, error, error_size) != 0) goto done;
    rc = process_release(config, source, remote, transaction, staging_artifacts,
                         staging_device, core_device, &release, result, error, error_size);
done:
    c1_update_repository_release_free(&release);
    if (transaction_created != 0) (void)c1_update_remove_tree(transaction);
    c1_update_unlock(lock, lock_path);
    return rc;
}

int c1_update_prepare_local(const struct c1_update_transaction_config *config,
                            const char *release_directory,
                            struct c1_update_transaction_result *result,
                            char *error, size_t error_size)
{
    return prepare_common(config, release_directory, 0, result, error, error_size);
}

int c1_update_prepare(const struct c1_update_transaction_config *config,
                      const char *base_url,
                      struct c1_update_transaction_result *result,
                      char *error, size_t error_size)
{
    return prepare_common(config, base_url, 1, result, error, error_size);
}