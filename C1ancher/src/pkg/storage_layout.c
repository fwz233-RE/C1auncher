#include "storage_layout.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#define C1PKG_STORAGE_RESERVE_BYTES (4U * 1024U * 1024U)

static const struct c1pkg_storage_layout default_layout = {
    "/",
    C1PKG_STORAGE_MOUNT,
    C1PKG_STORAGE_ROOT,
    C1PKG_APPS_ROOT,
    C1PKG_WORK_ROOT,
    C1PKG_STAGING_ROOT,
    C1PKG_TRASH_ROOT,
    C1PKG_STATE_ROOT,
    1
};

const struct c1pkg_storage_layout *c1pkg_storage_default_layout(void)
{
    return &default_layout;
}

static int valid_layout(const struct c1pkg_storage_layout *layout)
{
    return layout != NULL && layout->system_root != NULL &&
           layout->mount_root != NULL && layout->storage_root != NULL &&
           layout->apps_root != NULL && layout->work_root != NULL &&
           layout->staging_root != NULL && layout->trash_root != NULL &&
           layout->state_root != NULL && layout->system_root[0] == '/' &&
           layout->mount_root[0] == '/' && layout->storage_root[0] == '/' &&
           layout->apps_root[0] == '/' && layout->work_root[0] == '/' &&
           layout->staging_root[0] == '/' && layout->trash_root[0] == '/' &&
           layout->state_root[0] == '/';
}

static int existing_directory(const char *path, struct stat *information,
                              char *error, size_t error_size)
{
    if (lstat(path, information) != 0) {
        c1pkg_set_error(error, error_size, "%s is unavailable: %s", path,
                        strerror(errno));
        return -1;
    }
    if (!S_ISDIR(information->st_mode) || S_ISLNK(information->st_mode)) {
        c1pkg_set_error(error, error_size, "%s is not a trusted directory", path);
        return -1;
    }
    return 0;
}

static int validate_mount(const struct c1pkg_storage_layout *layout,
                          struct stat *mount_information,
                          char *error, size_t error_size)
{
    struct stat system_information;

    if (existing_directory(layout->mount_root, mount_information,
                           error, error_size) != 0 ||
        existing_directory(layout->system_root, &system_information,
                           error, error_size) != 0) {
        return -1;
    }
    if (layout->require_distinct_mount != 0 &&
        mount_information->st_dev == system_information.st_dev) {
        c1pkg_set_error(error, error_size,
                        "%s is not mounted on a separate filesystem",
                        layout->mount_root);
        return -1;
    }
    return 0;
}

static int ensure_storage_directories(const struct c1pkg_storage_layout *layout,
                                      char *error, size_t error_size)
{
    const char *const directories[] = {
        layout->storage_root,
        layout->apps_root,
        layout->work_root,
        layout->staging_root,
        layout->trash_root
    };
    const mode_t modes[] = {0755, 0755, 0700, 0700, 0700};
    size_t i;

    for (i = 0U; i < sizeof(directories) / sizeof(directories[0]); ++i) {
        if (c1pkg_mkdir_p(directories[i], modes[i], error, error_size) != 0) {
            return -1;
        }
    }
    return 0;
}

static int validate_storage_devices(const struct c1pkg_storage_layout *layout,
                                    dev_t expected_device,
                                    enum c1pkg_storage_access access,
                                    char *error, size_t error_size)
{
    const char *const directories[] = {
        layout->storage_root,
        layout->apps_root,
        layout->work_root,
        layout->staging_root,
        layout->trash_root
    };
    size_t count = access != C1PKG_STORAGE_READ ?
                   sizeof(directories) / sizeof(directories[0]) : 2U;
    size_t i;

    for (i = 0U; i < count; ++i) {
        struct stat information;

        /* A mounted, unused store is a valid empty library. Validate each
         * existing ancestor first; never excuse symlinks or mount failures. */
        if (access == C1PKG_STORAGE_READ &&
            lstat(directories[i], &information) != 0 && errno == ENOENT) {
            if (error != NULL && error_size > 0U) error[0] = '\0';
            return 0;
        }
        if (existing_directory(directories[i], &information,
                               error, error_size) != 0) {
            return -1;
        }
        if (information.st_dev != expected_device) {
            c1pkg_set_error(error, error_size,
                            "%s is outside the storage filesystem",
                            directories[i]);
            return -1;
        }
    }
    return 0;
}

static int check_available_space(const char *path, uint64_t required_bytes,
                                 char *error, size_t error_size)
{
    struct statvfs information;
    uint64_t block_size;
    uint64_t available;
    uint64_t required;

    if (statvfs(path, &information) != 0) {
        c1pkg_set_error(error, error_size, "inspect storage capacity: %s",
                        strerror(errno));
        return -1;
    }
    block_size = information.f_frsize != 0U ?
                 (uint64_t)information.f_frsize : (uint64_t)information.f_bsize;
    if (block_size == 0U) {
        c1pkg_set_error(error, error_size, "invalid storage block size");
        return -1;
    }
    available = information.f_bavail > UINT64_MAX / block_size ? UINT64_MAX :
                (uint64_t)information.f_bavail * block_size;
    required = required_bytes > UINT64_MAX - C1PKG_STORAGE_RESERVE_BYTES ?
               UINT64_MAX : required_bytes + C1PKG_STORAGE_RESERVE_BYTES;
    if (available < required) {
        c1pkg_set_error(error, error_size,
                        "insufficient storage space: need %llu bytes, have %llu bytes",
                        (unsigned long long)required,
                        (unsigned long long)available);
        return -1;
    }
    return 0;
}

static int verify_writable(const char *work_root, char *error, size_t error_size)
{
    char probe[C1PKG_PATH_MAX];
    int descriptor;
    int count = snprintf(probe, sizeof(probe), "%s/.write-test.%ld",
                         work_root, (long)getpid());

    if (count < 0 || (size_t)count >= sizeof(probe)) {
        c1pkg_set_error(error, error_size, "storage probe path is too long");
        return -1;
    }
    descriptor = open(probe, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (descriptor < 0) {
        c1pkg_set_error(error, error_size, "storage is not writable: %s",
                        strerror(errno));
        return -1;
    }
    if (write(descriptor, "1", 1U) != 1 || fsync(descriptor) != 0 ||
        close(descriptor) != 0 || unlink(probe) != 0) {
        int error_number = errno;

        (void)close(descriptor);
        (void)unlink(probe);
        c1pkg_set_error(error, error_size, "storage write probe failed: %s",
                        strerror(error_number));
        return -1;
    }
    return 0;
}

int c1pkg_storage_state_init_at(const struct c1pkg_storage_layout *layout,
                                char *error, size_t error_size)
{
    char cache[C1PKG_PATH_MAX];

    if (!valid_layout(layout) ||
        c1pkg_join(cache, sizeof(cache), layout->state_root, "cache") != 0) {
        c1pkg_set_error(error, error_size, "invalid package storage layout");
        return -1;
    }
    if (c1pkg_mkdir_p(layout->state_root, 0700, error, error_size) != 0 ||
        c1pkg_mkdir_p(cache, 0700, error, error_size) != 0) {
        return -1;
    }
    return 0;
}

int c1pkg_storage_prepare_at(const struct c1pkg_storage_layout *layout,
                             enum c1pkg_storage_access access,
                             uint64_t required_bytes,
                             char *error, size_t error_size)
{
    struct stat mount_information;

    if (!valid_layout(layout) ||
        (access != C1PKG_STORAGE_READ && access != C1PKG_STORAGE_WRITE &&
         access != C1PKG_STORAGE_MAINTENANCE)) {
        c1pkg_set_error(error, error_size, "invalid package storage layout");
        return -1;
    }
    if (validate_mount(layout, &mount_information, error, error_size) != 0) {
        return -1;
    }
    if (access != C1PKG_STORAGE_READ &&
        ensure_storage_directories(layout, error, error_size) != 0) {
        return -1;
    }
    if (validate_storage_devices(layout, mount_information.st_dev, access,
                                 error, error_size) != 0) {
        return -1;
    }
    if (access == C1PKG_STORAGE_WRITE &&
        (verify_writable(layout->work_root, error, error_size) != 0 ||
         check_available_space(layout->work_root, required_bytes,
                               error, error_size) != 0)) {
        return -1;
    }
    return 0;
}

int c1pkg_storage_state_init(char *error, size_t error_size)
{
    return c1pkg_storage_state_init_at(&default_layout, error, error_size);
}

int c1pkg_storage_prepare(enum c1pkg_storage_access access,
                          uint64_t required_bytes,
                          char *error, size_t error_size)
{
    return c1pkg_storage_prepare_at(&default_layout, access, required_bytes,
                                    error, error_size);
}