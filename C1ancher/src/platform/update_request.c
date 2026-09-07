#include "platform/update_request.h"

#include "security/secure_file.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define C1_UPDATE_REQUEST_PATH_MAX 4096U

static int request_parent(const char *path, char parent[C1_UPDATE_REQUEST_PATH_MAX])
{
    const char *slash;
    size_t length;

    if (path == NULL || path[0] == '\0' || strlen(path) >= C1_UPDATE_REQUEST_PATH_MAX) {
        return -1;
    }
    slash = strrchr(path, '/');
    if (slash == NULL) {
        (void)strcpy(parent, ".");
        return 0;
    }
    length = (size_t)(slash - path);
    if (length == 0U) {
        (void)strcpy(parent, "/");
        return 0;
    }
    (void)memcpy(parent, path, length);
    parent[length] = '\0';
    return 0;
}

static int consuming_path(const char *path,
                          char consuming[C1_UPDATE_REQUEST_PATH_MAX])
{
    int count;

    if (request_parent(path, consuming) != 0) {
        return -1;
    }
    count = snprintf(consuming, C1_UPDATE_REQUEST_PATH_MAX, "%s.consuming", path);
    return count >= 0 && (size_t)count < C1_UPDATE_REQUEST_PATH_MAX ? 0 : -1;
}

static int valid_digest(const char *digest)
{
    size_t index;

    if (digest == NULL || strlen(digest) != C1_UPDATE_REQUEST_DIGEST_SIZE) {
        return 0;
    }
    for (index = 0U; index < C1_UPDATE_REQUEST_DIGEST_SIZE; ++index) {
        if (!((digest[index] >= '0' && digest[index] <= '9') ||
              (digest[index] >= 'a' && digest[index] <= 'f'))) {
            return 0;
        }
    }
    return 1;
}

int c1_update_request_validate_digest(const char *digest)
{
    return valid_digest(digest);
}

int c1_update_request_write(const char *path, const char *digest,
                            char *error, size_t error_size)
{
    char content[C1_UPDATE_REQUEST_FILE_SIZE + 1U];

    if (path == NULL || !valid_digest(digest)) {
        c1_secure_set_error(error, error_size, "invalid update request digest");
        return -1;
    }
    (void)snprintf(content, sizeof(content), "%s\n", digest);
    if (c1_secure_atomic_write(path, content, C1_UPDATE_REQUEST_FILE_SIZE,
                               0600, error, error_size) != 0) {
        return -1;
    }
    return 0;
}

int c1_update_request_read(const char *path, char digest[65],
                           char *error, size_t error_size)
{
    unsigned char *data = NULL;
    size_t size = 0U;
    struct stat information;
    int result = -1;

    if (path == NULL || digest == NULL || lstat(path, &information) != 0 ||
        !S_ISREG(information.st_mode) || information.st_nlink != 1 ||
        (information.st_mode & 0777U) != 0600U ||
        c1_secure_read_file(path, &data, &size, C1_UPDATE_REQUEST_FILE_SIZE,
                            C1_UPDATE_REQUEST_FILE_SIZE, error, error_size) != 0) {
        return -1;
    }
    if (size != C1_UPDATE_REQUEST_FILE_SIZE || data[64] != '\n') {
        c1_secure_set_error(error, error_size, "update request format rejected");
        goto done;
    }
    data[64] = '\0';
    if (!valid_digest((const char *)data)) {
        c1_secure_set_error(error, error_size, "update request digest rejected");
        goto done;
    }
    (void)memcpy(digest, data, C1_UPDATE_REQUEST_FILE_SIZE);
    result = 0;
done:
    free(data);
    return result;
}

int c1_update_request_consume(const char *path, const char *expected_digest,
                              char *error, size_t error_size)
{
    char digest[C1_UPDATE_REQUEST_FILE_SIZE];
    char isolated[C1_UPDATE_REQUEST_PATH_MAX];
    char parent[C1_UPDATE_REQUEST_PATH_MAX];
    struct stat information;

    if (consuming_path(path, isolated) != 0 || request_parent(path, parent) != 0) {
        c1_secure_set_error(error, error_size, "invalid update request path");
        return -1;
    }
    if (lstat(isolated, &information) != 0) {
        if (errno != ENOENT) {
            c1_secure_set_error(error, error_size,
                                "isolated update request lookup failed: %s",
                                strerror(errno));
            return -1;
        }
        if (rename(path, isolated) != 0) {
            c1_secure_set_error(error, error_size,
                                "update request isolation failed: %s",
                                strerror(errno));
            return -1;
        }
        if (c1_secure_sync_directory(parent, error, error_size) != 0) {
            return -1;
        }
    }

    if (c1_update_request_read(isolated, digest, error, error_size) != 0) {
        return -1;
    }
    if (expected_digest != NULL && strcmp(digest, expected_digest) != 0) {
        c1_secure_set_error(error, error_size, "update request digest mismatch");
        return -1;
    }
    if (unlink(isolated) != 0) {
        c1_secure_set_error(error, error_size, "update request consume failed: %s",
                            strerror(errno));
        return -1;
    }
    return c1_secure_sync_directory(parent, error, error_size);
}