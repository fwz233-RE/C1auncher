#include "update/update.h"
#include "security/secure_file.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define C1_STATE_FILE_MAX 1024U

static int safe_token(const char *text)
{
    size_t i;
    size_t length;

    if (text == NULL) {
        return 0;
    }
    length = strlen(text);
    if (length == 0U || length > C1_UPDATE_TOKEN_MAX) {
        return 0;
    }
    for (i = 0U; i < length; ++i) {
        unsigned char character = (unsigned char)text[i];
        int alphanumeric = (character >= (unsigned char)'a' && character <= (unsigned char)'z') ||
                           (character >= (unsigned char)'A' && character <= (unsigned char)'Z') ||
                           (character >= (unsigned char)'0' && character <= (unsigned char)'9');
        int punctuation = character == (unsigned char)'.' || character == (unsigned char)'_' ||
                          character == (unsigned char)'-' || character == (unsigned char)'+';
        if ((!alphanumeric && !punctuation) ||
            ((i == 0U || i + 1U == length) && !alphanumeric)) {
            return 0;
        }
    }
    return 1;
}

static int valid_digest(const char *digest)
{
    size_t i;

    if (digest == NULL || strlen(digest) != 64U) {
        return 0;
    }
    for (i = 0U; i < 64U; ++i) {
        if (!((digest[i] >= '0' && digest[i] <= '9') ||
              (digest[i] >= 'a' && digest[i] <= 'f'))) {
            return 0;
        }
    }
    return 1;
}

static int parse_u64(const char *text, uint64_t *value, int nonzero)
{
    uint64_t result = 0U;
    size_t i;

    if (text == NULL || text[0] == '\0' || (text[0] == '0' && text[1] != '\0')) {
        return -1;
    }
    for (i = 0U; text[i] != '\0'; ++i) {
        unsigned int digit;
        if (text[i] < '0' || text[i] > '9') {
            return -1;
        }
        digit = (unsigned int)(text[i] - '0');
        if (result > (UINT64_MAX - digit) / 10U) {
            return -1;
        }
        result = result * 10U + digit;
    }
    if (nonzero != 0 && result == 0U) {
        return -1;
    }
    *value = result;
    return 0;
}

const char *c1_update_phase_name(enum c1_update_phase phase)
{
    static const char *const names[] = {
        "idle", "downloading", "verified", "prepared", "pending-boot", "confirmed"
    };

    if ((unsigned int)phase >= sizeof(names) / sizeof(names[0])) {
        return NULL;
    }
    return names[(unsigned int)phase];
}

int c1_update_phase_parse(const char *text, enum c1_update_phase *phase)
{
    enum c1_update_phase candidate;

    if (text == NULL || phase == NULL) {
        return -1;
    }
    for (candidate = C1_UPDATE_IDLE; candidate <= C1_UPDATE_CONFIRMED;
         candidate = (enum c1_update_phase)((int)candidate + 1)) {
        if (strcmp(text, c1_update_phase_name(candidate)) == 0) {
            *phase = candidate;
            return 0;
        }
    }
    return -1;
}

static int identity_valid(const struct c1_update_state *previous,
                          uint64_t sequence, uint64_t security_epoch,
                          const char *release, const char *digest)
{
    if (sequence == 0U || security_epoch == 0U || !safe_token(release) || !valid_digest(digest)) {
        return 0;
    }
    if (previous == NULL || previous->sequence == 0U) {
        return 1;
    }
    if (sequence < previous->sequence || security_epoch < previous->security_epoch) {
        return 0;
    }
    if (sequence == previous->sequence &&
        (strcmp(digest, previous->digest) != 0 || strcmp(release, previous->release) != 0)) {
        return 0;
    }
    return 1;
}

int c1_update_transition(const struct c1_update_state *previous,
                         enum c1_update_phase next_phase, uint64_t sequence,
                         uint64_t security_epoch, const char *release, const char *digest,
                         int failed, struct c1_update_state *next,
                         char *error, size_t error_size)
{
    enum c1_update_phase current = previous == NULL ? C1_UPDATE_IDLE : previous->phase;
    int allowed;

    if (next == NULL || current < C1_UPDATE_IDLE || current > C1_UPDATE_CONFIRMED ||
        (previous != NULL && previous->generation == UINT64_MAX)) {
        c1_secure_set_error(error, error_size, "invalid state transition request");
        return -1;
    }
    if (failed != 0) {
        if (current == C1_UPDATE_IDLE || next_phase != C1_UPDATE_IDLE || previous == NULL) {
            c1_secure_set_error(error, error_size, "invalid failed state transition");
            return -1;
        }
        *next = *previous;
        next->generation = previous->generation + 1U;
        next->phase = C1_UPDATE_IDLE;
        return 0;
    }
    allowed = ((int)next_phase == (int)current + 1) ||
              (current == C1_UPDATE_PENDING_BOOT && next_phase == C1_UPDATE_PREPARED) ||
              (current == C1_UPDATE_CONFIRMED && next_phase == C1_UPDATE_DOWNLOADING);
    if (!allowed || !identity_valid(previous, sequence, security_epoch, release, digest)) {
        c1_secure_set_error(error, error_size, "state transition rejected");
        return -1;
    }
    (void)memset(next, 0, sizeof(*next));
    next->generation = previous == NULL ? 1U : previous->generation + 1U;
    next->phase = next_phase;
    next->sequence = sequence;
    next->security_epoch = security_epoch;
    (void)strcpy(next->release, release);
    (void)strcpy(next->digest, digest);
    return 0;
}

static int join_path(char *out, size_t out_size, const char *left, const char *right)
{
    const char *separator;
    int count;

    if (out == NULL || left == NULL || right == NULL || left[0] == '\0') {
        return -1;
    }
    separator = left[strlen(left) - 1U] == '/' ? "" : "/";
    count = snprintf(out, out_size, "%s%s%s", left, separator, right);
    return count >= 0 && (size_t)count < out_size ? 0 : -1;
}

static int state_equal(const struct c1_update_state *left,
                       const struct c1_update_state *right)
{
    return left->generation == right->generation && left->phase == right->phase &&
           left->sequence == right->sequence && left->security_epoch == right->security_epoch &&
           strcmp(left->release, right->release) == 0 &&
           strcmp(left->digest, right->digest) == 0;
}

static int private_directory(const char *path, struct stat *information)
{
    struct stat found;

    if (lstat(path, &found) != 0 || !S_ISDIR(found.st_mode) ||
        S_ISLNK(found.st_mode) || (found.st_mode & 0777U) != 0700U ||
        found.st_uid != geteuid())
        return -1;
    if (information != NULL) *information = found;
    return 0;
}

static int private_state_file(const char *path)
{
    struct stat information;

    return lstat(path, &information) == 0 && S_ISREG(information.st_mode) &&
           information.st_nlink == 1 && (information.st_mode & 0777U) == 0600U &&
           information.st_uid == geteuid() ? 0 : -1;
}

static int parse_state_data(unsigned char *data, size_t size, uint64_t generation,
                            struct c1_update_state *state)
{
    char *lines[6];
    size_t count = 0U;
    size_t offset = 0U;

    if (size == 0U || data[size - 1U] != (unsigned char)'\n') {
        return -1;
    }
    while (offset < size) {
        size_t end = offset;
        while (end < size && data[end] != (unsigned char)'\n') {
            if ((data[end] < 0x20U && data[end] != (unsigned char)'\t') ||
                data[end] > 0x7eU) {
                return -1;
            }
            ++end;
        }
        if (end == size || end == offset || count >= 6U) {
            return -1;
        }
        data[end] = 0U;
        lines[count++] = (char *)(data + offset);
        offset = end + 1U;
    }
    if (count != 6U || strcmp(lines[0], "C1CORE-STATE 1") != 0 ||
        strncmp(lines[1], "P\t", 2U) != 0 || strchr(lines[1] + 2U, '\t') != NULL ||
        c1_update_phase_parse(lines[1] + 2U, &state->phase) != 0 ||
        strncmp(lines[2], "S\t", 2U) != 0 || strchr(lines[2] + 2U, '\t') != NULL ||
        parse_u64(lines[2] + 2U, &state->sequence, 1) != 0 ||
        strncmp(lines[3], "E\t", 2U) != 0 || strchr(lines[3] + 2U, '\t') != NULL ||
        parse_u64(lines[3] + 2U, &state->security_epoch, 1) != 0 ||
        strncmp(lines[4], "R\t", 2U) != 0 || strchr(lines[4] + 2U, '\t') != NULL ||
        !safe_token(lines[4] + 2U) ||
        strncmp(lines[5], "D\t", 2U) != 0 || strchr(lines[5] + 2U, '\t') != NULL ||
        !valid_digest(lines[5] + 2U)) {
        return -1;
    }
    state->generation = generation;
    (void)strcpy(state->release, lines[4] + 2U);
    (void)strcpy(state->digest, lines[5] + 2U);
    return 0;
}

static int load_generation_state(const char *generation_path, uint64_t generation,
                                 struct c1_update_state *state,
                                 char *error, size_t error_size)
{
    char state_path[C1_UPDATE_PATH_MAX];
    unsigned char *data = NULL;
    size_t size = 0U;
    int result = -1;

    if (private_directory(generation_path, NULL) != 0 ||
        join_path(state_path, sizeof(state_path), generation_path, "state.v1") != 0 ||
        private_state_file(state_path) != 0 ||
        c1_secure_read_file(state_path, &data, &size, C1_STATE_FILE_MAX,
                            C1_SECURE_FILE_ANY_SIZE, error, error_size) != 0 ||
        parse_state_data(data, size, generation, state) != 0) {
        c1_secure_set_error(error, error_size, "stored state generation rejected");
        goto done;
    }
    result = 0;
done:
    free(data);
    return result;
}

static int parse_generation_target(const char *target, uint64_t *generation)
{
    static const char prefix[] = "generations/";
    const char *number;

    if (strncmp(target, prefix, sizeof(prefix) - 1U) != 0) {
        return -1;
    }
    number = target + sizeof(prefix) - 1U;
    if (strchr(number, '/') != NULL) {
        return -1;
    }
    return parse_u64(number, generation, 1);
}

int c1_update_state_load(const char *root, struct c1_update_state *state,
                         char *error, size_t error_size)
{
    char current[C1_UPDATE_PATH_MAX];
    char generations[C1_UPDATE_PATH_MAX];
    char target[128];
    char generation_path[C1_UPDATE_PATH_MAX];
    char state_path[C1_UPDATE_PATH_MAX];
    unsigned char *data = NULL;
    size_t size = 0U;
    uint64_t generation;
    struct stat root_information;
    struct stat generations_information;
    struct stat current_information;
    ssize_t length;
    int result = -1;

    if (root == NULL || state == NULL ||
        join_path(current, sizeof(current), root, "current") != 0 ||
        join_path(generations, sizeof(generations), root, "generations") != 0) {
        c1_secure_set_error(error, error_size, "invalid state root");
        return -1;
    }
    (void)memset(state, 0, sizeof(*state));
    state->phase = C1_UPDATE_IDLE;
    errno = 0;
    if (lstat(root, &root_information) != 0) {
        if (errno == ENOENT) return 0;
        c1_secure_set_error(error, error_size, "invalid state root");
        return -1;
    }
    if (private_directory(root, NULL) != 0) {
        c1_secure_set_error(error, error_size, "invalid state root");
        return -1;
    }
    errno = 0;
    if (lstat(current, &current_information) != 0) {
        if (errno != ENOENT) {
            c1_secure_set_error(error, error_size, "current state link rejected");
            return -1;
        }
        errno = 0;
        if (lstat(generations, &generations_information) != 0) {
            if (errno == ENOENT) return 0;
            c1_secure_set_error(error, error_size, "current state link rejected");
            return -1;
        }
        if (!S_ISDIR(generations_information.st_mode) ||
            private_directory(generations, NULL) != 0) {
            c1_secure_set_error(error, error_size, "current state link rejected");
            return -1;
        }
        return 0;
    }
    if (!S_ISLNK(current_information.st_mode) ||
        current_information.st_uid != geteuid() ||
        private_directory(generations, NULL) != 0) {
        c1_secure_set_error(error, error_size, "current state link rejected");
        return -1;
    }
    length = readlink(current, target, sizeof(target));
    if (length <= 0 || (size_t)length >= sizeof(target)) {
        c1_secure_set_error(error, error_size, "current state link rejected");
        return -1;
    }
    target[(size_t)length] = '\0';
    if (parse_generation_target(target, &generation) != 0 ||
        join_path(generation_path, sizeof(generation_path), root, target) != 0 ||
        private_directory(generation_path, NULL) != 0 ||
        join_path(state_path, sizeof(state_path), generation_path, "state.v1") != 0 ||
        private_state_file(state_path) != 0) {
        c1_secure_set_error(error, error_size, "current state target rejected");
        return -1;
    }
    if (c1_secure_read_file(state_path, &data, &size, C1_STATE_FILE_MAX,
                            C1_SECURE_FILE_ANY_SIZE, error, error_size) != 0 ||
        parse_state_data(data, size, generation, state) != 0) {
        c1_secure_set_error(error, error_size, "stored update state rejected");
        goto done;
    }
    result = 0;
done:
    free(data);
    return result;
}

static int create_directory(const char *path, mode_t mode, int allow_existing,
                            char *error, size_t error_size)
{
    int created = 0;

    if (mkdir(path, mode) == 0) {
        created = 1;
    } else if (!(allow_existing != 0 && errno == EEXIST)) {
        c1_secure_set_error(error, error_size, "state directory creation failed: %s", strerror(errno));
        return -1;
    }
    if ((created != 0 && chmod(path, mode) != 0) || mode != 0700U ||
        private_directory(path, NULL) != 0) {
        c1_secure_set_error(error, error_size, "state directory rejected");
        return -1;
    }
    return 0;
}

static int commit_phase_valid(enum c1_update_phase previous,
                              enum c1_update_phase next)
{
    return (previous != C1_UPDATE_IDLE && next == C1_UPDATE_IDLE) ||
           ((int)next == (int)previous + 1) ||
           (previous == C1_UPDATE_PENDING_BOOT && next == C1_UPDATE_PREPARED) ||
           (previous == C1_UPDATE_CONFIRMED && next == C1_UPDATE_DOWNLOADING);
}

int c1_update_state_commit(const char *root,
                           const struct c1_update_state *previous,
                           const struct c1_update_state *next,
                           char *error, size_t error_size)
{
    struct c1_update_state actual;
    struct stat current_information;
    char generations[C1_UPDATE_PATH_MAX];
    char generation_name[32];
    char generation_path[C1_UPDATE_PATH_MAX];
    char draft_name[96];
    char draft_path[C1_UPDATE_PATH_MAX];
    char state_path[C1_UPDATE_PATH_MAX] = "";
    char current[C1_UPDATE_PATH_MAX];
    char temporary[C1_UPDATE_PATH_MAX];
    char target[64];
    char text[C1_STATE_FILE_MAX];
    const char *phase;
    unsigned int attempt;
    int text_length;
    int count;
    int draft_created = 0;
    int draft_committed = 0;
    int generation_ready = 0;
    int result = -1;

    if (root == NULL || previous == NULL || next == NULL ||
        next->generation <= previous->generation || next->sequence == 0U ||
        next->security_epoch == 0U ||
        !commit_phase_valid(previous->phase, next->phase) ||
        !safe_token(next->release) || !valid_digest(next->digest) ||
        !identity_valid(previous, next->sequence, next->security_epoch,
                        next->release, next->digest) ||
        c1_update_state_load(root, &actual, error, error_size) != 0) {
        c1_secure_set_error(error, error_size, "state commit rejected");
        return -1;
    }
    if (state_equal(&actual, next)) return c1_secure_sync_directory(root, error, error_size);
    if (!state_equal(&actual, previous)) {
        c1_secure_set_error(error, error_size, "state commit rejected");
        return -1;
    }
    phase = c1_update_phase_name(next->phase);
    if (phase == NULL || join_path(generations, sizeof(generations), root, "generations") != 0 ||
        create_directory(root, 0700, 1, error, error_size) != 0 ||
        create_directory(generations, 0700, 1, error, error_size) != 0) {
        return -1;
    }
    count = snprintf(generation_name, sizeof(generation_name), "%llu",
                     (unsigned long long)next->generation);
    if (count < 0 || (size_t)count >= sizeof(generation_name) ||
        join_path(generation_path, sizeof(generation_path), generations, generation_name) != 0) {
        c1_secure_set_error(error, error_size, "state generation path is too long");
        return -1;
    }
    errno = 0;
    if (lstat(generation_path, &current_information) == 0) {
        struct c1_update_state stored;
        if (load_generation_state(generation_path, next->generation, &stored,
                                  error, error_size) != 0 || !state_equal(&stored, next)) {
            c1_secure_set_error(error, error_size, "existing state generation conflicts");
            return -1;
        }
        generation_ready = 1;
    } else if (errno != ENOENT) {
        c1_secure_set_error(error, error_size, "state generation path rejected");
        return -1;
    }
    if (!generation_ready) {
        count = snprintf(draft_name, sizeof(draft_name), ".new.%s.%ld.XXXXXX",
                         generation_name, (long)getpid());
        if (count < 0 || (size_t)count >= sizeof(draft_name) ||
            join_path(draft_path, sizeof(draft_path), generations, draft_name) != 0 ||
            mkdtemp(draft_path) == NULL) {
            return -1;
        }
        draft_created = 1;
        if (join_path(state_path, sizeof(state_path), draft_path, "state.v1") != 0) {
            c1_secure_set_error(error, error_size, "state file path is too long");
            goto done;
        }
        text_length = snprintf(text, sizeof(text),
                               "C1CORE-STATE 1\nP\t%s\nS\t%llu\nE\t%llu\nR\t%s\nD\t%s\n",
                               phase, (unsigned long long)next->sequence,
                               (unsigned long long)next->security_epoch,
                               next->release, next->digest);
        if (text_length <= 0 || (size_t)text_length >= sizeof(text) ||
            c1_secure_atomic_write(state_path, text, (size_t)text_length, 0600,
                                   error, error_size) != 0 ||
            c1_secure_sync_directory(draft_path, error, error_size) != 0) {
            goto done;
        }
        if (rename(draft_path, generation_path) != 0) {
            c1_secure_set_error(error, error_size, "state generation commit failed: %s",
                                strerror(errno));
            goto done;
        }
        draft_committed = 1;
    }
    if (c1_secure_sync_directory(generations, error, error_size) != 0) {
        goto done;
    }
    count = snprintf(target, sizeof(target), "generations/%s", generation_name);
    if (count < 0 || (size_t)count >= sizeof(target) ||
        join_path(current, sizeof(current), root, "current") != 0) {
        c1_secure_set_error(error, error_size, "state link path is too long");
        return -1;
    }
    errno = 0;
    if (lstat(current, &current_information) == 0) {
        if (!S_ISLNK(current_information.st_mode)) {
            c1_secure_set_error(error, error_size, "current state path is not a link");
            goto done;
        }
    } else if (errno != ENOENT) {
        c1_secure_set_error(error, error_size, "current state path rejected");
        goto done;
    }
    for (attempt = 0U; attempt < 128U; ++attempt) {
        count = snprintf(temporary, sizeof(temporary), "%s.tmp.%ld.%u",
                         current, (long)getpid(), attempt);
        if (count < 0 || (size_t)count >= sizeof(temporary)) {
            c1_secure_set_error(error, error_size, "state link path is too long");
            goto done;
        }
        if (symlink(target, temporary) == 0) {
            break;
        }
        if (errno != EEXIST) {
            c1_secure_set_error(error, error_size, "temporary state link failed: %s",
                                strerror(errno));
            goto done;
        }
    }
    if (attempt == 128U) {
        c1_secure_set_error(error, error_size, "temporary state link name unavailable");
        goto done;
    }
    if (rename(temporary, current) != 0) {
        int saved_error = errno;
        (void)unlink(temporary);
        c1_secure_set_error(error, error_size, "current state replacement failed: %s",
                            strerror(saved_error));
        goto done;
    }
    result = c1_secure_sync_directory(root, error, error_size);
done:
    if (draft_created != 0 && draft_committed == 0) {
        (void)unlink(state_path);
        (void)rmdir(draft_path);
    }
    return result;
}