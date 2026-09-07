#include "security/secure_file.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

#define C1_SECURE_PATH_MAX 4096U

void c1_secure_set_error(char *error, size_t error_size, const char *format, ...)
{
    va_list arguments;

    if (error == NULL || error_size == 0U) {
        return;
    }
    va_start(arguments, format);
    (void)vsnprintf(error, error_size, format, arguments);
    va_end(arguments);
}

static int parent_directory(const char *path, char parent[C1_SECURE_PATH_MAX])
{
    const char *slash;
    size_t length;

    if (path == NULL || path[0] == '\0' || strlen(path) >= C1_SECURE_PATH_MAX) {
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
    if (length >= C1_SECURE_PATH_MAX) {
        return -1;
    }
    (void)memcpy(parent, path, length);
    parent[length] = '\0';
    return 0;
}

int c1_secure_read_file(const char *path, unsigned char **data, size_t *size,
                        size_t maximum_size, size_t exact_size,
                        char *error, size_t error_size)
{
    struct stat information;
    unsigned char *buffer = NULL;
    size_t expected;
    size_t used = 0U;
    unsigned char extra;
    int descriptor;
    int result = -1;

    if (data == NULL || size == NULL || maximum_size == C1_SECURE_FILE_ANY_SIZE ||
        (exact_size != C1_SECURE_FILE_ANY_SIZE && exact_size > maximum_size)) {
        c1_secure_set_error(error, error_size, "invalid secure read request");
        return -1;
    }
    *data = NULL;
    *size = 0U;
    descriptor = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0) {
        c1_secure_set_error(error, error_size, "secure file open failed: %s", strerror(errno));
        return -1;
    }
    if (fstat(descriptor, &information) != 0 || !S_ISREG(information.st_mode) ||
        information.st_nlink != 1 || information.st_size < 0 ||
        (information.st_mode & (S_IWGRP | S_IWOTH)) != 0 ||
        (uintmax_t)information.st_size > (uintmax_t)maximum_size) {
        c1_secure_set_error(error, error_size, "secure file metadata rejected");
        goto done;
    }
    expected = (size_t)information.st_size;
    if (exact_size != C1_SECURE_FILE_ANY_SIZE && expected != exact_size) {
        c1_secure_set_error(error, error_size, "secure file size rejected");
        goto done;
    }
    buffer = malloc(expected + 1U);
    if (buffer == NULL) {
        c1_secure_set_error(error, error_size, "out of memory");
        goto done;
    }
    while (used < expected) {
        ssize_t amount = read(descriptor, buffer + used, expected - used);
        if (amount < 0 && errno == EINTR) {
            continue;
        }
        if (amount <= 0) {
            c1_secure_set_error(error, error_size, "secure file changed during read");
            goto done;
        }
        used += (size_t)amount;
    }
    for (;;) {
        ssize_t amount = read(descriptor, &extra, 1U);
        if (amount < 0 && errno == EINTR) {
            continue;
        }
        if (amount != 0) {
            c1_secure_set_error(error, error_size, "secure file changed during read");
            goto done;
        }
        break;
    }
    buffer[used] = 0U;
    *data = buffer;
    *size = used;
    buffer = NULL;
    result = 0;
done:
    free(buffer);
    if (close(descriptor) != 0 && result == 0) {
        free(*data);
        *data = NULL;
        *size = 0U;
        c1_secure_set_error(error, error_size, "secure file close failed: %s", strerror(errno));
        result = -1;
    }
    return result;
}

int c1_secure_sync_directory(const char *path, char *error, size_t error_size)
{
    struct stat information;
    int descriptor = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    int saved_error;

    if (descriptor < 0) {
        c1_secure_set_error(error, error_size, "directory open failed: %s", strerror(errno));
        return -1;
    }
    if (fstat(descriptor, &information) != 0 || !S_ISDIR(information.st_mode)) {
        saved_error = errno;
        (void)close(descriptor);
        c1_secure_set_error(error, error_size, "directory metadata rejected: %s",
                            strerror(saved_error));
        return -1;
    }
    if (fsync(descriptor) != 0) {
        saved_error = errno;
        (void)close(descriptor);
        c1_secure_set_error(error, error_size, "directory sync failed: %s", strerror(saved_error));
        return -1;
    }
    if (close(descriptor) != 0) {
        c1_secure_set_error(error, error_size, "directory close failed: %s", strerror(errno));
        return -1;
    }
    return 0;
}

static int write_all(int descriptor, const unsigned char *data, size_t size)
{
    size_t written = 0U;

    while (written < size) {
        ssize_t amount = write(descriptor, data + written, size - written);
        if (amount < 0 && errno == EINTR) {
            continue;
        }
        if (amount <= 0) {
            return -1;
        }
        written += (size_t)amount;
    }
    return 0;
}

int c1_secure_atomic_write(const char *path, const void *data, size_t size,
                           mode_t mode, char *error, size_t error_size)
{
    char parent[C1_SECURE_PATH_MAX];
    char temporary[C1_SECURE_PATH_MAX];
    unsigned int attempt;
    int descriptor = -1;
    int count;
    int saved_error;

    if (path == NULL || (data == NULL && size != 0U) || (mode & ~0777U) != 0U ||
        parent_directory(path, parent) != 0) {
        c1_secure_set_error(error, error_size, "invalid atomic write request");
        return -1;
    }
    for (attempt = 0U; attempt < 128U; ++attempt) {
        count = snprintf(temporary, sizeof(temporary), "%s.tmp.%ld.%u",
                         path, (long)getpid(), attempt);
        if (count < 0 || (size_t)count >= sizeof(temporary)) {
            c1_secure_set_error(error, error_size, "atomic write path is too long");
            return -1;
        }
        descriptor = open(temporary, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
                          mode);
        if (descriptor >= 0) {
            break;
        }
        if (errno != EEXIST) {
            c1_secure_set_error(error, error_size, "temporary file creation failed: %s",
                                strerror(errno));
            return -1;
        }
    }
    if (descriptor < 0) {
        c1_secure_set_error(error, error_size, "temporary file name unavailable");
        return -1;
    }
    if (fchmod(descriptor, mode) != 0 ||
        write_all(descriptor, (const unsigned char *)data, size) != 0 ||
        fsync(descriptor) != 0) {
        saved_error = errno;
        (void)close(descriptor);
        (void)unlink(temporary);
        c1_secure_set_error(error, error_size, "atomic file write failed: %s", strerror(saved_error));
        return -1;
    }
    if (close(descriptor) != 0) {
        saved_error = errno;
        (void)unlink(temporary);
        c1_secure_set_error(error, error_size, "atomic file close failed: %s", strerror(saved_error));
        return -1;
    }
    if (rename(temporary, path) != 0) {
        saved_error = errno;
        (void)unlink(temporary);
        c1_secure_set_error(error, error_size, "atomic file replace failed: %s", strerror(saved_error));
        return -1;
    }
    return c1_secure_sync_directory(parent, error, error_size);
}