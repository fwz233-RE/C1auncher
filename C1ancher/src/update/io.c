#include "update/io.h"
#include "update/update.h"
#include "security/secure_file.h"
#include "security/sha256.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

int c1_update_join_path(char *out, size_t out_size, const char *left, const char *right)
{
    const char *separator;
    int count;
    if (out == NULL || left == NULL || right == NULL || left[0] == '\0' || right[0] == '\0' ||
        right[0] == '/' || strstr(right, "../") != NULL || strcmp(right, "..") == 0) return -1;
    separator = left[strlen(left) - 1U] == '/' ? "" : "/";
    count = snprintf(out, out_size, "%s%s%s", left, separator, right);
    return count >= 0 && (size_t)count < out_size ? 0 : -1;
}

static int no_symlink_components(const char *path)
{
    char prefix[C1_UPDATE_PATH_MAX];
    size_t length, i;
    struct stat information;
    if (path == NULL || path[0] != '/') return 0;
    length = strlen(path);
    if (length == 0U || length >= sizeof(prefix) || strstr(path, "//") != NULL ||
        strstr(path, "/./") != NULL || strstr(path, "/../") != NULL) return 0;
    (void)memcpy(prefix, path, length + 1U);
    for (i = 1U; i < length; ++i) {
        if (prefix[i] == '/') {
            prefix[i] = '\0';
            if (lstat(prefix, &information) != 0 || !S_ISDIR(information.st_mode) ||
                S_ISLNK(information.st_mode)) return 0;
            prefix[i] = '/';
        }
    }
    return 1;
}

int c1_update_check_trusted_directory(const char *path, dev_t expected_device,
                                      dev_t *device, char *error, size_t error_size)
{
    struct stat information;
    if (!no_symlink_components(path) || lstat(path, &information) != 0 || !S_ISDIR(information.st_mode) ||
        S_ISLNK(information.st_mode) || information.st_uid != geteuid() ||
        (information.st_mode & (S_IWGRP | S_IWOTH)) != 0 ||
        (expected_device != (dev_t)-1 && information.st_dev != expected_device)) {
        c1_secure_set_error(error, error_size, "trusted directory rejected");
        return -1;
    }
    if (device != NULL) *device = information.st_dev;
    return 0;
}

int c1_update_make_directory(const char *path, mode_t mode, dev_t expected_device,
                             int allow_existing, char *error, size_t error_size)
{
    int created = 0;
    if (mkdir(path, mode) == 0) {
        created = 1;
    } else if (!(allow_existing != 0 && errno == EEXIST)) {
        c1_secure_set_error(error, error_size, "directory creation failed: %s", strerror(errno));
        return -1;
    }
    if ((created != 0 && chmod(path, mode) != 0) ||
        c1_update_check_trusted_directory(path, expected_device, NULL,
                                          error, error_size) != 0) return -1;
    return 0;
}

int c1_update_check_space(const char *path, uint64_t required,
                          char *error, size_t error_size)
{
    struct statvfs space;
    uint64_t available;
    if (statvfs(path, &space) != 0 || space.f_frsize == 0U ||
        (uint64_t)space.f_bavail > UINT64_MAX / (uint64_t)space.f_frsize) {
        c1_secure_set_error(error, error_size, "filesystem space query failed");
        return -1;
    }
    available = (uint64_t)space.f_bavail * (uint64_t)space.f_frsize;
    if (available < required) {
        c1_secure_set_error(error, error_size, "insufficient filesystem space");
        return -1;
    }
    return 0;
}

static int write_all(int descriptor, const unsigned char *data, size_t size)
{
    size_t offset = 0U;
    while (offset < size) {
        ssize_t amount = write(descriptor, data + offset, size - offset);
        if (amount < 0 && errno == EINTR) continue;
        if (amount <= 0) return -1;
        offset += (size_t)amount;
    }
    return 0;
}

int c1_update_copy_file(const char *source, const char *destination,
                        uint64_t expected_size, const char *expected_sha256,
                        mode_t final_mode, char *error, size_t error_size)
{
    struct stat before, after;
    struct c1_sha256_context hash;
    unsigned char buffer[16384], digest[C1_SHA256_SIZE], checked_digest[C1_SHA256_SIZE];
    char hex[C1_SHA256_HEX_SIZE], checked_hex[C1_SHA256_HEX_SIZE];
    uint64_t total = 0U;
    int input = -1, output = -1, result = -1;
    input = open(source, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (input < 0 || fstat(input, &before) != 0 || !S_ISREG(before.st_mode) || before.st_nlink != 1 ||
        before.st_size < 0 || (uint64_t)before.st_size != expected_size) {
        c1_secure_set_error(error, error_size, "source artifact rejected");
        goto done;
    }
    output = open(destination, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (output < 0 || fchmod(output, 0600) != 0) {
        c1_secure_set_error(error, error_size, "destination artifact creation failed");
        goto done;
    }
    c1_sha256_init(&hash);
    while (total < expected_size) {
        size_t wanted = sizeof(buffer);
        ssize_t amount;
        if ((uint64_t)wanted > expected_size - total) wanted = (size_t)(expected_size - total);
        amount = read(input, buffer, wanted);
        if (amount < 0 && errno == EINTR) continue;
        if (amount <= 0 || write_all(output, buffer, (size_t)amount) != 0) {
            c1_secure_set_error(error, error_size, "artifact copy failed");
            goto done;
        }
        c1_sha256_update(&hash, buffer, (size_t)amount);
        total += (uint64_t)amount;
    }
    {
        unsigned char extra;
        ssize_t amount = read(input, &extra, 1U);
        if (amount != 0) {
            c1_secure_set_error(error, error_size, "source artifact changed during copy");
            goto done;
        }
    }
    if (fstat(input, &after) != 0 || before.st_dev != after.st_dev || before.st_ino != after.st_ino ||
        before.st_size != after.st_size || before.st_mtime != after.st_mtime ||
        before.st_mtim.tv_nsec != after.st_mtim.tv_nsec ||
        before.st_ctime != after.st_ctime || before.st_ctim.tv_nsec != after.st_ctim.tv_nsec ||
        after.st_nlink != 1) {
        c1_secure_set_error(error, error_size, "source artifact changed during copy");
        goto done;
    }
    c1_sha256_final(&hash, digest);
    c1_sha256_hex(digest, hex);
    if (strcmp(hex, expected_sha256) != 0 || fsync(output) != 0 ||
        c1_sha256_file(destination, expected_size, expected_size, checked_digest, NULL,
                       error, error_size) != 0) {
        c1_secure_set_error(error, error_size, "copied artifact digest or sync rejected");
        goto done;
    }
    c1_sha256_hex(checked_digest, checked_hex);
    if (strcmp(checked_hex, expected_sha256) != 0 ||
        fchmod(output, final_mode) != 0 || fsync(output) != 0) {
        c1_secure_set_error(error, error_size, "copied artifact digest or sync rejected");
        goto done;
    }
    result = 0;
done:
    if (input >= 0) (void)close(input);
    if (output >= 0 && close(output) != 0) result = -1;
    /* Only remove an inode created by this call, never a preexisting target. */
    if (result != 0 && output >= 0) (void)unlink(destination);
    return result;
}

int c1_update_remove_tree(const char *path)
{
    struct stat information;
    DIR *directory;
    struct dirent *entry;
    if (lstat(path, &information) != 0) return errno == ENOENT ? 0 : -1;
    if (!S_ISDIR(information.st_mode) || S_ISLNK(information.st_mode)) return unlink(path);
    directory = opendir(path);
    if (directory == NULL) return -1;
    while ((entry = readdir(directory)) != NULL) {
        char child[C1_UPDATE_PATH_MAX];
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        if (strchr(entry->d_name, '/') != NULL || c1_update_join_path(child, sizeof(child), path, entry->d_name) != 0 ||
            c1_update_remove_tree(child) != 0) {
            (void)closedir(directory);
            return -1;
        }
    }
    if (closedir(directory) != 0) return -1;
    return rmdir(path);
}

int c1_update_lock(const char *root, char *lock_path, size_t lock_path_size,
                   char *error, size_t error_size)
{
    struct stat information;
    struct flock lock;
    int descriptor;
    if (c1_update_join_path(lock_path, lock_path_size, root, ".c1updater.lock") != 0) {
        c1_secure_set_error(error, error_size, "lock path is too long");
        return -1;
    }
    descriptor = open(lock_path, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (descriptor < 0 || fstat(descriptor, &information) != 0 ||
        !S_ISREG(information.st_mode) || information.st_nlink != 1 ||
        information.st_uid != geteuid() || fchmod(descriptor, 0600) != 0) {
        if (descriptor >= 0) (void)close(descriptor);
        c1_secure_set_error(error, error_size, "update lock file rejected");
        return -1;
    }
    (void)memset(&lock, 0, sizeof(lock));
    lock.l_type = F_WRLCK;
    lock.l_whence = SEEK_SET;
    if (fcntl(descriptor, F_SETLK, &lock) != 0) {
        int busy = errno == EACCES || errno == EAGAIN;
        (void)close(descriptor);
        c1_secure_set_error(error, error_size, busy ? "update lock busy" : "update lock failed");
        return busy ? C1_UPDATE_BUSY : -1;
    }
    return descriptor;
}

void c1_update_unlock(int descriptor, const char *lock_path)
{
    (void)lock_path;
    if (descriptor >= 0) (void)close(descriptor);
}