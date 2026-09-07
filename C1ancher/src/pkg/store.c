#include "pkg.h"
#include "gui.h"
#include "storage_layout.h"
#include "platform/app_lease.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* Run lock is distinct from the direct-I/O hardware lease. */
int c1_app_run_acquire(void);
bool c1_app_run_active(void);
static int recover_transaction(char *error, size_t error_size);
static int pointer_version(const char *app, const char *name, char *value);
static int trusted_directory(const char *path, struct stat *info);
static int acquire_lock(char *error, size_t error_size);
static void release_lock(int descriptor);
static void cleanup_abandoned_work(void);

int c1pkg_store_init(char *error, size_t error_size)
{
    int descriptor;
    if (c1pkg_storage_prepare(C1PKG_STORAGE_MAINTENANCE, 0U, error, error_size) != 0 ||
        (descriptor = acquire_lock(error, error_size)) < 0) return -1;
    cleanup_abandoned_work();
    release_lock(descriptor);
    return 0;
}

static int acquire_lock(char *error, size_t error_size)
{
    char lock_path[C1PKG_PATH_MAX];
    struct flock operation;
    struct stat lock_info;
    struct stat state_info;
    int descriptor;

    if (lstat(C1PKG_STATE_ROOT, &state_info) == 0 &&
        (!S_ISDIR(state_info.st_mode) || state_info.st_uid != geteuid() ||
         (state_info.st_mode & 0077) != 0)) {
        c1pkg_set_error(error, error_size, "untrusted package state directory");
        return -1;
    }
    if (c1pkg_storage_state_init(error, error_size) != 0 ||
        c1pkg_join(lock_path, sizeof(lock_path), C1PKG_STATE_ROOT, "lock") != 0) {
        if (error != NULL && error[0] == '\0') {
            c1pkg_set_error(error, error_size, "lock path is too long");
        }
        return -1;
    }
    descriptor = open(lock_path, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
    if (descriptor >= 0 && (fstat(descriptor, &lock_info) != 0 ||
        !S_ISREG(lock_info.st_mode) || lock_info.st_nlink != 1 ||
        lock_info.st_uid != geteuid() || (lock_info.st_mode & 0777) != 0600)) {
        (void)close(descriptor);
        descriptor = -1;
        errno = EACCES;
    }
    if (descriptor < 0) {
        c1pkg_set_error(error, error_size, "open package lock: %s", strerror(errno));
        return -1;
    }
    (void)memset(&operation, 0, sizeof(operation));
    operation.l_type = F_WRLCK;
    operation.l_whence = SEEK_SET;
    if (fcntl(descriptor, F_SETLK, &operation) != 0) {
        int error_number = errno;

        (void)close(descriptor);
        c1pkg_set_error(error, error_size,
                        error_number == EACCES || error_number == EAGAIN ?
                        "another package operation is in progress" :
                        "lock package store: %s",
                        strerror(error_number));
        return -1;
    }
    if (recover_transaction(error, error_size) != 0) {
        (void)close(descriptor);
        return -1;
    }
    return descriptor;
}

static void release_lock(int descriptor)
{
    if (descriptor >= 0) {
        (void)close(descriptor);
    }
}

/* Installed directories are sealed 0555. Make only the private tree being
 * discarded writable, never a live version or any sibling user-data directory. */
static int remove_package_tree(const char *root, char *error, size_t error_size)
{
    struct stat info;
    DIR *directory;
    struct dirent *entry;
    if (lstat(root, &info) != 0) return errno == ENOENT ? 0 : -1;
    if (!S_ISDIR(info.st_mode)) return c1pkg_remove_tree(root, error, error_size);
    if (info.st_uid != geteuid() || chmod(root, 0700) != 0 || (directory = opendir(root)) == NULL) return -1;
    while ((entry = readdir(directory)) != NULL) {
        char child[C1PKG_PATH_MAX];
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        if (c1pkg_join(child, sizeof(child), root, entry->d_name) != 0 ||
            remove_package_tree(child, error, error_size) != 0) {
            (void)closedir(directory);
            return -1;
        }
    }
    (void)closedir(directory);
    return rmdir(root);
}

static void cleanup_work_directory(const char *root)
{
    DIR *directory = opendir(root);
    struct dirent *entry;

    if (directory == NULL) {
        return;
    }
    while ((entry = readdir(directory)) != NULL) {
        char child[C1PKG_PATH_MAX];

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0 ||
            !c1pkg_safe_relpath(entry->d_name) ||
            c1pkg_join(child, sizeof(child), root, entry->d_name) != 0) {
            continue;
        }
        (void)remove_package_tree(child, NULL, 0U);
    }
    (void)closedir(directory);
}

static void cleanup_abandoned_work(void)
{
    DIR *apps;
    struct dirent *app_entry;
    int run_descriptor = c1_app_run_acquire();
    /* Hold, rather than merely probe, the run lock: launch cannot race GC. */
    if (run_descriptor < 0) return;
    cleanup_work_directory(C1PKG_STAGING_ROOT);
    cleanup_work_directory(C1PKG_TRASH_ROOT);
    apps = opendir(C1PKG_APPS_ROOT);
    if (apps != NULL) {
        while ((app_entry = readdir(apps)) != NULL) {
            char app[C1PKG_PATH_MAX], versions[C1PKG_PATH_MAX];
            char current[C1PKG_VERSION_MAX + 1U], previous[C1PKG_VERSION_MAX + 1U];
            struct stat info;
            DIR *directory;
            struct dirent *entry;
            if (!c1pkg_safe_id(app_entry->d_name) ||
                c1pkg_join(app, sizeof(app), C1PKG_APPS_ROOT, app_entry->d_name) != 0 ||
                trusted_directory(app, &info) != 0 ||
                pointer_version(app, "current", current) != 0 || current[0] == '\0' ||
                pointer_version(app, "previous", previous) != 0 ||
                c1pkg_join(versions, sizeof(versions), app, "versions") != 0 ||
                trusted_directory(versions, &info) != 0) continue;
            directory = opendir(versions);
            if (directory == NULL) continue;
            while ((entry = readdir(directory)) != NULL) {
                char path[C1PKG_PATH_MAX];
                if (!c1pkg_safe_version(entry->d_name) || strcmp(entry->d_name, current) == 0 ||
                    strcmp(entry->d_name, previous) == 0 ||
                    c1pkg_join(path, sizeof(path), versions, entry->d_name) != 0) continue;
                (void)remove_package_tree(path, NULL, 0U);
            }
            (void)closedir(directory);
            (void)c1pkg_sync_directory(versions, NULL, 0U);
        }
        (void)closedir(apps);
    }
    c1_app_lease_release(run_descriptor);
}

static int read_link_value(const char *path, char *value, size_t value_size)
{
    ssize_t amount = readlink(path, value, value_size - 1U);
    if (amount < 0 || (size_t)amount >= value_size - 1U) {
        return -1;
    }
    value[amount] = '\0';
    return 0;
}

static int current_version(const char *id, char *version, size_t version_size)
{
    char app[C1PKG_PATH_MAX];
    char current[C1PKG_PATH_MAX];
    char target[C1PKG_PATH_MAX];
    const char prefix[] = "versions/";

    if (!c1pkg_safe_id(id) || c1pkg_join(app, sizeof(app), C1PKG_APPS_ROOT, id) != 0 ||
        c1pkg_join(current, sizeof(current), app, "current") != 0 ||
        read_link_value(current, target, sizeof(target)) != 0 ||
        strncmp(target, prefix, sizeof(prefix) - 1U) != 0 ||
        !c1pkg_safe_version(target + sizeof(prefix) - 1U) ||
        strlen(target + sizeof(prefix) - 1U) >= version_size) {
        return -1;
    }
    (void)strcpy(version, target + sizeof(prefix) - 1U);
    return 0;
}

static int installed_compare(const void *left, const void *right)
{
    const struct c1pkg_installed *a = left;
    const struct c1pkg_installed *b = right;
    return strcmp(a->id, b->id);
}

int c1pkg_store_list(struct c1pkg_installed_list *list, char *error, size_t error_size)
{
    DIR *directory;
    struct dirent *entry;
    int lock_descriptor;

    list->count = 0U;
    if (c1pkg_storage_prepare(C1PKG_STORAGE_READ, 0U,
                              error, error_size) != 0) {
        return -1;
    }
    lock_descriptor = acquire_lock(error, error_size);
    if (lock_descriptor < 0) return -1;
    directory = opendir(C1PKG_APPS_ROOT);
    if (directory == NULL) {
        int saved_errno = errno;
        release_lock(lock_descriptor);
        if (saved_errno == ENOENT) {
            if (error != NULL && error_size > 0U) error[0] = '\0';
            return 0;
        }
        c1pkg_set_error(error, error_size, "open applications directory: %s", strerror(saved_errno));
        return -1;
    }
    while ((entry = readdir(directory)) != NULL) {
        struct c1pkg_installed *installed;
        if (!c1pkg_safe_id(entry->d_name)) {
            continue;
        }
        if (list->count >= C1PKG_MAX_PACKAGES) {
            (void)closedir(directory);
            release_lock(lock_descriptor);
            c1pkg_set_error(error, error_size, "too many installed applications");
            return -1;
        }
        installed = &list->items[list->count];
        if (current_version(entry->d_name, installed->version,
                            sizeof(installed->version)) == 0) {
            (void)strcpy(installed->id, entry->d_name);
            ++list->count;
        }
    }
    (void)closedir(directory);
    release_lock(lock_descriptor);
    qsort(list->items, list->count, sizeof(list->items[0]), installed_compare);
    return 0;
}

int c1pkg_store_is_installed(const struct c1pkg_installed_list *list,
                             const char *id, const char **version)
{
    size_t i;
    for (i = 0U; i < list->count; ++i) {
        if (strcmp(list->items[i].id, id) == 0) {
            if (version != NULL) {
                *version = list->items[i].version;
            }
            return 1;
        }
    }
    return 0;
}

static int validate_tar_names(const char *list_path, char *error, size_t error_size)
{
    unsigned char *data = NULL;
    size_t size = 0U;
    size_t offset = 0U;
    int result = -1;

    if (c1pkg_read_file(list_path, &data, &size, C1PKG_INDEX_MAX,
                        error, error_size) != 0 || size == 0U) {
        goto done;
    }
    while (offset < size) {
        size_t end = offset;
        char *name;
        while (end < size && data[end] != (unsigned char)'\n') {
            if (data[end] == 0U || data[end] == (unsigned char)'\r') {
                c1pkg_set_error(error, error_size, "tar listing contains forbidden bytes");
                goto done;
            }
            ++end;
        }
        data[end] = 0U;
        name = (char *)(data + offset);
        while (name[0] == '.' && name[1] == '/') {
            name += 2;
        }
        /* tar lists directory members with a trailing slash. Normalize that
         * one separator before applying the same strict relative-path policy. */
        {
            size_t length = strlen(name);
            if (length > 0U && name[length - 1U] == '/') name[length - 1U] = '\0';
        }
        if (!c1pkg_safe_relpath(name) ||
            strcmp(name, "payload/.c1pkg-mode") == 0 ||
            strncmp(name, "payload/.c1pkg-mode/", 20U) == 0 ||
            strcmp(name, "payload/.c1pkg-entry") == 0 ||
            strncmp(name, "payload/.c1pkg-entry/", 21U) == 0 ||
            (strcmp(name, "manifest.v1") != 0 && strcmp(name, "payload") != 0 &&
             strncmp(name, "payload/", 8U) != 0)) {
            c1pkg_set_error(error, error_size, "unsafe archive path: %s", name);
            goto done;
        }
        offset = end + 1U;
    }
    result = 0;
done:
    free(data);
    return result;
}

static int parse_tar_size(const char *text, size_t length, uint64_t *value)
{
    uint64_t result = 0U;
    size_t i;

    if (length == 0U) {
        return -1;
    }
    for (i = 0U; i < length; ++i) {
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
    *value = result;
    return 0;
}

static int validate_verbose_types(const char *list_path, char *error, size_t error_size)
{
    unsigned char *data = NULL;
    size_t size = 0U;
    size_t offset = 0U;
    uint64_t total = 0U;
    int result = -1;

    if (c1pkg_read_file(list_path, &data, &size, C1PKG_INDEX_MAX,
                        error, error_size) != 0 || size == 0U) {
        goto done;
    }
    while (offset < size) {
        size_t end = offset;
        char *cursor;
        char *size_start;
        uint64_t declared_size;
        while (end < size && data[end] != (unsigned char)'\n') {
            ++end;
        }
        data[end] = 0U;
        if (end == offset || (data[offset] != (unsigned char)'-' &&
                              data[offset] != (unsigned char)'d')) {
            c1pkg_set_error(error, error_size,
                            "archive contains links or special files");
            goto done;
        }
        cursor = (char *)(data + offset);
        while (*cursor != '\0' && *cursor != ' ') ++cursor;
        while (*cursor == ' ') ++cursor;
        while (*cursor != '\0' && *cursor != ' ') ++cursor;
        while (*cursor == ' ') ++cursor;
        size_start = cursor;
        while (*cursor >= '0' && *cursor <= '9') ++cursor;
        if ((*cursor != ' ' && *cursor != '\0') ||
            parse_tar_size(size_start, (size_t)(cursor - size_start), &declared_size) != 0 ||
            declared_size > C1PKG_FILE_MAX ||
            total > C1PKG_UNPACKED_MAX - declared_size) {
            c1pkg_set_error(error, error_size,
                            "archive declared size exceeds limit or is unparseable");
            goto done;
        }
        total += declared_size;
        offset = end + 1U;
    }
    result = 0;
done:
    free(data);
    return result;
}

static int inspect_tree(const char *root, uint64_t *total, char *error, size_t error_size)
{
    DIR *directory = opendir(root);
    struct dirent *entry;

    if (directory == NULL) {
        c1pkg_set_error(error, error_size, "inspect %s: %s", root, strerror(errno));
        return -1;
    }
    while ((entry = readdir(directory)) != NULL) {
        char child[C1PKG_PATH_MAX];
        struct stat information;
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        if (!c1pkg_safe_relpath(entry->d_name) ||
            c1pkg_join(child, sizeof(child), root, entry->d_name) != 0 ||
            lstat(child, &information) != 0) {
            (void)closedir(directory);
            c1pkg_set_error(error, error_size, "cannot inspect extracted package");
            return -1;
        }
        if (S_ISDIR(information.st_mode)) {
            if (inspect_tree(child, total, error, error_size) != 0) {
                (void)closedir(directory);
                return -1;
            }
        } else if (S_ISREG(information.st_mode) && information.st_nlink == 1) {
            if (information.st_size < 0 || (uint64_t)information.st_size > C1PKG_FILE_MAX ||
                *total > C1PKG_UNPACKED_MAX - (uint64_t)information.st_size) {
                (void)closedir(directory);
                c1pkg_set_error(error, error_size, "extracted package exceeds size limit");
                return -1;
            }
            *total += (uint64_t)information.st_size;
        } else {
            (void)closedir(directory);
            c1pkg_set_error(error, error_size, "extracted package contains unsafe file type");
            return -1;
        }
    }
    (void)closedir(directory);
    return 0;
}

static int seal_tree(const char *root, char *error, size_t error_size)
{
    DIR *directory = opendir(root);
    struct dirent *entry;

    if (directory == NULL) {
        c1pkg_set_error(error, error_size, "seal %s: %s", root, strerror(errno));
        return -1;
    }
    while ((entry = readdir(directory)) != NULL) {
        char child[C1PKG_PATH_MAX];
        struct stat information;
        mode_t mode;
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        if (c1pkg_join(child, sizeof(child), root, entry->d_name) != 0 ||
            lstat(child, &information) != 0) {
            (void)closedir(directory);
            c1pkg_set_error(error, error_size, "cannot seal extracted package");
            return -1;
        }
        if (S_ISDIR(information.st_mode)) {
            if (seal_tree(child, error, error_size) != 0 || chmod(child, 0555) != 0 ||
                c1pkg_sync_directory(child, error, error_size) != 0) {
                (void)closedir(directory);
                if (error != NULL && error[0] == '\0') {
                    c1pkg_set_error(error, error_size, "seal directory: %s", strerror(errno));
                }
                return -1;
            }
        } else {
            mode = (mode_t)(information.st_mode & 0555);
            mode |= 0400;
            if (chmod(child, mode) != 0) {
                (void)closedir(directory);
                c1pkg_set_error(error, error_size, "seal file: %s", strerror(errno));
                return -1;
            }
            {
                int descriptor = open(child, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
                int sync_result = descriptor >= 0 ? fsync(descriptor) : -1;
                if (descriptor >= 0 && close(descriptor) != 0) sync_result = -1;
                if (sync_result != 0) {
                    (void)closedir(directory);
                    c1pkg_set_error(error, error_size, "sync package file: %s", strerror(errno));
                    return -1;
                }
            }
        }
    }
    (void)closedir(directory);
    return c1pkg_sync_directory(root, error, error_size);
}

static int validate_manifest(const char *path, const struct c1pkg_package *package,
                             char mode[9], char *error, size_t error_size)
{
    unsigned char *data = NULL;
    size_t size = 0U;
    char expected[512];
    const char *modes[] = {"", "terminal", "direct"};
    size_t i;
    int result = -1;
    mode[0] = '\0';
    if (c1pkg_read_file(path, &data, &size, sizeof(expected), error, error_size) != 0) return -1;
    for (i = 0U; i < sizeof(modes) / sizeof(modes[0]); ++i) {
        int length = snprintf(expected, sizeof(expected),
            "C1PKG-PACKAGE %d\nid\t%s\nversion\t%s\nentry\t%s\n%s%s%s",
            i == 0U ? 1 : 2, package->id, package->version, package->entry,
            i == 0U ? "" : "mode\t", modes[i], i == 0U ? "" : "\n");
        if (length >= 0 && (size_t)length < sizeof(expected) && size == (size_t)length &&
            memcmp(data, expected, size) == 0) {
            (void)strcpy(mode, modes[i]);
            result = 0;
            break;
        }
    }
    if (result != 0) c1pkg_set_error(error, error_size, "package manifest does not match signed index or mode contract");
    free(data);
    return result;
}

/* A private, durable redo journal is written only after package verification (or
 * explicit rollback validation). The inode binds recovery to that exact payload;
 * merely placing a directory in versions/ never authorizes activation. Readers
 * take the store lock and finish recovery before observing either pointer.
 * Ordering: seal/fsync payload -> fsync WAL + state directory -> rename payload
 * -> fsync versions/app/apps -> previous + fsync app -> current + fsync app
 * -> unlink WAL + fsync state. Every replay step is idempotent. */
#define TRANSACTION_PATH C1PKG_STATE_ROOT "/transaction"
#define TRANSACTION_TEMP C1PKG_STATE_ROOT "/transaction.tmp"
struct store_transaction {
    char magic[16];
    char id[C1PKG_ID_MAX + 1U];
    char version[C1PKG_VERSION_MAX + 1U];
    char old_current[C1PKG_VERSION_MAX + 1U];
    char old_previous[C1PKG_VERSION_MAX + 1U];
    uint64_t device;
    uint64_t inode;
    unsigned int installing;
};

static int trusted_directory(const char *path, struct stat *info)
{
    return lstat(path, info) == 0 && S_ISDIR(info->st_mode) &&
           info->st_uid == geteuid() && (info->st_mode & 0022) == 0 ? 0 : -1;
}

static int pointer_version(const char *app, const char *name, char *value)
{
    char path[C1PKG_PATH_MAX], target[C1PKG_PATH_MAX];
    if (c1pkg_join(path, sizeof(path), app, name) != 0) return -1;
    if (read_link_value(path, target, sizeof(target)) != 0) {
        if (errno != ENOENT) return -1;
        value[0] = '\0';
        return 0;
    }
    if (strncmp(target, "versions/", 9U) != 0 || !c1pkg_safe_version(target + 9U)) return -1;
    (void)strcpy(value, target + 9U);
    return 0;
}

static int write_pointer(const char *app, const char *name, const char *version,
                         char *error, size_t error_size)
{
    char path[C1PKG_PATH_MAX], temp[C1PKG_PATH_MAX], target[64];
    if (c1pkg_join(path, sizeof(path), app, name) != 0 ||
        c1pkg_join(temp, sizeof(temp), app, ".pointer.tmp") != 0) return -1;
    (void)unlink(temp);
    if (version[0] == '\0') {
        if (unlink(path) != 0 && errno != ENOENT) goto failed;
    } else {
        (void)snprintf(target, sizeof(target), "versions/%s", version);
        if (symlink(target, temp) != 0 || rename(temp, path) != 0) goto failed;
    }
    return c1pkg_sync_directory(app, error, error_size);
failed:
    (void)unlink(temp);
    c1pkg_set_error(error, error_size, "publish %s pointer: %s", name, strerror(errno));
    return -1;
}

static int finish_transaction(char *error, size_t error_size)
{
    if (unlink(TRANSACTION_PATH) != 0 && errno != ENOENT) return -1;
    return c1pkg_sync_directory(C1PKG_STATE_ROOT, error, error_size);
}

static int recover_transaction(char *error, size_t error_size)
{
    struct store_transaction tx;
    struct stat info;
    char app[C1PKG_PATH_MAX], versions[C1PKG_PATH_MAX], target[C1PKG_PATH_MAX];
    int fd = open(TRANSACTION_PATH, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    ssize_t amount;
    if (fd < 0) {
        if (errno == ENOENT) return 0;
        goto failed;
    }
    if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_nlink != 1 ||
        info.st_uid != geteuid() || (info.st_mode & 0777) != 0600 ||
        info.st_size != (off_t)sizeof(tx)) {
        (void)close(fd);
        goto failed;
    }
    do { amount = read(fd, &tx, sizeof(tx)); } while (amount < 0 && errno == EINTR);
    (void)close(fd);
    if (amount != (ssize_t)sizeof(tx) || memcmp(tx.magic, "C1PKG-WAL-1", 12U) != 0 ||
        memchr(tx.id, 0, sizeof(tx.id)) == NULL || !c1pkg_safe_id(tx.id) ||
        memchr(tx.version, 0, sizeof(tx.version)) == NULL || !c1pkg_safe_version(tx.version) ||
        memchr(tx.old_current, 0, sizeof(tx.old_current)) == NULL ||
        (tx.old_current[0] != '\0' && !c1pkg_safe_version(tx.old_current)) ||
        memchr(tx.old_previous, 0, sizeof(tx.old_previous)) == NULL ||
        (tx.old_previous[0] != '\0' && !c1pkg_safe_version(tx.old_previous)) || tx.installing > 1U ||
        c1pkg_join(app, sizeof(app), C1PKG_APPS_ROOT, tx.id) != 0 ||
        trusted_directory(app, &info) != 0 ||
        c1pkg_join(versions, sizeof(versions), app, "versions") != 0 ||
        trusted_directory(versions, &info) != 0 ||
        c1pkg_join(target, sizeof(target), versions, tx.version) != 0) goto failed;
    if (lstat(target, &info) != 0) {
        char current[C1PKG_VERSION_MAX + 1U], previous[C1PKG_VERSION_MAX + 1U];
        /* Power loss before payload rename: no pointer was changed. */
        if (errno == ENOENT && tx.installing &&
            pointer_version(app, "current", current) == 0 &&
            pointer_version(app, "previous", previous) == 0 &&
            strcmp(current, tx.old_current) == 0 && strcmp(previous, tx.old_previous) == 0)
            return finish_transaction(error, error_size);
        goto failed;
    }
    if (!S_ISDIR(info.st_mode) || info.st_uid != geteuid() || (info.st_mode & 0022) != 0 ||
        (uint64_t)info.st_dev != tx.device || (uint64_t)info.st_ino != tx.inode) goto failed;
    if ((tx.installing && chmod(target, 0555) != 0) ||
        c1pkg_sync_directory(target, error, error_size) != 0 ||
        c1pkg_sync_directory(versions, error, error_size) != 0 ||
        c1pkg_sync_directory(app, error, error_size) != 0 ||
        c1pkg_sync_directory(C1PKG_APPS_ROOT, error, error_size) != 0 ||
        write_pointer(app, "previous", tx.old_current, error, error_size) != 0 ||
        write_pointer(app, "current", tx.version, error, error_size) != 0 ||
        finish_transaction(error, error_size) != 0) goto failed;
    return 0;
failed:
    c1pkg_set_error(error, error_size, "package transaction recovery failed; store is blocked safely");
    return -1;
}

static int begin_transaction(const char *app, const char *version, const char *payload,
                             int installing, char *error, size_t error_size)
{
    struct store_transaction tx;
    struct stat info;
    const char *id = strrchr(app, '/');
    if (lstat(TRANSACTION_PATH, &info) == 0 || errno != ENOENT) {
        c1pkg_set_error(error, error_size, "unfinished transaction must be recovered first");
        return -1;
    }
    (void)memset(&tx, 0, sizeof(tx));
    if (id == NULL || !c1pkg_safe_id(id + 1) || !c1pkg_safe_version(version) ||
        trusted_directory(app, &info) != 0 || trusted_directory(payload, &info) != 0 ||
        pointer_version(app, "current", tx.old_current) != 0 ||
        pointer_version(app, "previous", tx.old_previous) != 0) {
        c1pkg_set_error(error, error_size, "unsafe transaction payload or version pointers");
        return -1;
    }
    (void)strcpy(tx.magic, "C1PKG-WAL-1");
    (void)strcpy(tx.id, id + 1);
    (void)strcpy(tx.version, version);
    tx.device = (uint64_t)info.st_dev;
    tx.inode = (uint64_t)info.st_ino;
    tx.installing = installing != 0;
    (void)unlink(TRANSACTION_TEMP);
    if (c1pkg_write_file(TRANSACTION_TEMP, &tx, sizeof(tx), 0600, error, error_size) != 0 ||
        rename(TRANSACTION_TEMP, TRANSACTION_PATH) != 0 ||
        c1pkg_sync_directory(C1PKG_STATE_ROOT, error, error_size) != 0) {
        c1pkg_set_error(error, error_size, "persist package transaction: %s", strerror(errno));
        return -1;
    }
    return 0;
}

static int switch_current(const char *app, const char *version,
                          char *error, size_t error_size)
{
    char versions[C1PKG_PATH_MAX], target[C1PKG_PATH_MAX];
    if (c1pkg_join(versions, sizeof(versions), app, "versions") != 0 ||
        c1pkg_join(target, sizeof(target), versions, version) != 0 ||
        begin_transaction(app, version, target, 0, error, error_size) != 0) return -1;
    return recover_transaction(error, error_size);
}

static int payload_equal(const char *verified, const char *retained)
{
    DIR *directory;
    struct dirent *entry;
    struct stat a, b;
    size_t left_count = 0U, right_count = 0U;
    if (lstat(verified, &a) != 0 || lstat(retained, &b) != 0 ||
        !S_ISDIR(a.st_mode) || !S_ISDIR(b.st_mode) || b.st_uid != geteuid() ||
        (b.st_mode & 0222) != 0) return 0;
    directory = opendir(verified);
    if (directory == NULL) return 0;
    while ((entry = readdir(directory)) != NULL) {
        char left[C1PKG_PATH_MAX], right[C1PKG_PATH_MAX];
        int equal = 1;
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        ++left_count;
        if (c1pkg_join(left, sizeof(left), verified, entry->d_name) != 0 ||
            c1pkg_join(right, sizeof(right), retained, entry->d_name) != 0 ||
            lstat(left, &a) != 0 || lstat(right, &b) != 0 ||
            b.st_uid != geteuid() || (b.st_mode & 0222) != 0) equal = 0;
        else if (S_ISDIR(a.st_mode)) equal = payload_equal(left, right);
        else if (!S_ISREG(a.st_mode) || !S_ISREG(b.st_mode) || b.st_nlink != 1 ||
                 a.st_size != b.st_size || (a.st_mode & 0111) != (b.st_mode & 0111)) equal = 0;
        else {
            int x = open(left, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
            int y = open(right, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
            unsigned char xb[4096], yb[4096];
            off_t remaining = a.st_size;
            if (x < 0 || y < 0) equal = 0;
            while (equal && remaining > 0) {
                size_t length = remaining > (off_t)sizeof(xb) ? sizeof(xb) : (size_t)remaining;
                ssize_t nx, ny;
                do { nx = read(x, xb, length); } while (nx < 0 && errno == EINTR);
                do { ny = read(y, yb, length); } while (ny < 0 && errno == EINTR);
                if (nx != (ssize_t)length || ny != nx || memcmp(xb, yb, length) != 0) equal = 0;
                remaining -= (off_t)length;
            }
            if (x >= 0) (void)close(x);
            if (y >= 0) (void)close(y);
        }
        if (!equal) { (void)closedir(directory); return 0; }
    }
    (void)closedir(directory);
    directory = opendir(retained);
    if (directory == NULL) return 0;
    while ((entry = readdir(directory)) != NULL)
        if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0) ++right_count;
    (void)closedir(directory);
    return left_count == right_count;
}

static int package_url(char *url, size_t url_size, const struct c1pkg_config *config,
                       const struct c1pkg_package *package)
{
    int count = snprintf(url, url_size, "%s%s%s", config->repo_base,
                         config->repo_base[strlen(config->repo_base) - 1U] == '/' ? "" : "/",
                         package->archive);
    return count >= 0 && (size_t)count < url_size ? 0 : -1;
}

int c1pkg_store_install(const struct c1pkg_config *config,
                        const struct c1pkg_package *package,
                        char *error, size_t error_size)
{
    const char *tar = c1pkg_helper("/bin/tar", "tar");
    int lock_descriptor = -1;
    char stage[C1PKG_PATH_MAX] = "";
    char archive[C1PKG_PATH_MAX];
    char names[C1PKG_PATH_MAX];
    char verbose[C1PKG_PATH_MAX];
    char extract[C1PKG_PATH_MAX];
    char manifest[C1PKG_PATH_MAX];
    char payload[C1PKG_PATH_MAX];
    char app[C1PKG_PATH_MAX];
    char versions[C1PKG_PATH_MAX];
    char destination[C1PKG_PATH_MAX];
    char entry[C1PKG_PATH_MAX];
    char entry_metadata[C1PKG_PATH_MAX];
    char mode_metadata[C1PKG_PATH_MAX];
    char mode[9];
    char url[1400];
    char *list_arguments[] = {(char *)tar, "-tzf", archive, NULL};
    char *verbose_arguments[] = {(char *)tar, "-tvzf", archive, NULL};
    char *extract_arguments[] = {(char *)tar, "-xzf", archive, "-C", extract, NULL};
    struct stat information;
    uint64_t total = 0U;
    int retained = 0;
    int result = C1PKG_INSTALL_PACKAGE_ERROR;

    if (config == NULL || config->repo_base == NULL || config->repo_base[0] == '\0' ||
        package == NULL || !c1pkg_safe_id(package->id) || !c1pkg_safe_version(package->version) ||
        !c1pkg_safe_relpath(package->archive) || !c1pkg_safe_relpath(package->entry) ||
        package->size == 0U || package->size > C1PKG_PACKAGE_MAX) {
        c1pkg_set_error(error, error_size, "invalid package or repository configuration");
        return -1;
    }
    if (c1pkg_storage_prepare(C1PKG_STORAGE_MAINTENANCE, 0U,
                              error, error_size) != 0 ||
        (lock_descriptor = acquire_lock(error, error_size)) < 0) {
        return C1PKG_INSTALL_STORAGE_ERROR;
    }
    cleanup_abandoned_work();
    if (snprintf(stage, sizeof(stage), "%s/install.%ld", C1PKG_STAGING_ROOT,
                 (long)getpid()) < 0 || c1pkg_join(archive, sizeof(archive), stage, "package.tar.gz") != 0 ||
        c1pkg_join(names, sizeof(names), stage, "names.txt") != 0 ||
        c1pkg_join(verbose, sizeof(verbose), stage, "verbose.txt") != 0 ||
        c1pkg_join(extract, sizeof(extract), stage, "extract") != 0 ||
        c1pkg_join(manifest, sizeof(manifest), extract, "manifest.v1") != 0 ||
        c1pkg_join(payload, sizeof(payload), extract, "payload") != 0 ||
        c1pkg_join(app, sizeof(app), C1PKG_APPS_ROOT, package->id) != 0 ||
        c1pkg_join(versions, sizeof(versions), app, "versions") != 0 ||
        c1pkg_join(destination, sizeof(destination), versions, package->version) != 0 ||
        c1pkg_join(entry, sizeof(entry), payload, package->entry) != 0 ||
        c1pkg_join(entry_metadata, sizeof(entry_metadata), payload, ".c1pkg-entry") != 0 ||
        c1pkg_join(mode_metadata, sizeof(mode_metadata), payload, ".c1pkg-mode") != 0 ||
        package_url(url, sizeof(url), config, package) != 0) {
        c1pkg_set_error(error, error_size, "package path or URL is too long");
        goto done;
    }
    {
        char current[C1PKG_PATH_MAX];
        char installed[C1PKG_VERSION_MAX + 1U];
        if ((lstat(app, &information) == 0 ? trusted_directory(app, &information) != 0 : errno != ENOENT) ||
            (lstat(versions, &information) == 0 ? trusted_directory(versions, &information) != 0 : errno != ENOENT)) {
            c1pkg_set_error(error, error_size, "application storage path is untrusted");
            goto done;
        }
        if (c1pkg_join(current, sizeof(current), app, "current") != 0) goto done;
        if (lstat(current, &information) == 0) {
            int decision;
            if (current_version(package->id, installed, sizeof(installed)) != 0) {
                c1pkg_set_error(error, error_size, "installed version pointer is unsafe");
                goto done;
            }
            decision = c1pkg_install_decision(installed, package->version);
            if (decision == 0) {
                c1pkg_set_error(error, error_size, "already current: %s %s", package->id, installed);
                result = C1PKG_INSTALL_SKIPPED;
                goto done;
            }
            if (decision < 0) {
                c1pkg_set_error(error, error_size, "downgrade or unordered version rejected; use rollback explicitly");
                goto done;
            }
        } else if (errno != ENOENT) {
            c1pkg_set_error(error, error_size, "inspect current version: %s", strerror(errno));
            goto done;
        }
        /* A retained payload is never trusted by name. Re-download against the
         * signed digest and compare every byte/type/entry/mode before reactivation. */
        if (lstat(destination, &information) == 0) {
            if (!S_ISDIR(information.st_mode)) {
                c1pkg_set_error(error, error_size, "retained version is unsafe");
                goto done;
            }
            retained = 1;
        } else if (errno != ENOENT) {
            result = C1PKG_INSTALL_STORAGE_ERROR;
            goto done;
        }
        if (c1pkg_storage_prepare(C1PKG_STORAGE_WRITE,
              package->size + C1PKG_UNPACKED_MAX, error, error_size) != 0) {
            result = C1PKG_INSTALL_STORAGE_ERROR;
            goto done;
        }
    }
    (void)remove_package_tree(stage, NULL, 0U);
    if (c1pkg_mkdir_p(extract, 0700, error, error_size) != 0 ||
        c1pkg_fetch(url, archive, package->size, error, error_size) != 0 ||
        stat(archive, &information) != 0 || (uint64_t)information.st_size != package->size) {
        if (error != NULL && error[0] == '\0') {
            c1pkg_set_error(error, error_size, "downloaded package size mismatch");
        }
        goto done;
    }
    if (c1pkg_verify_sha256(archive, package->sha256, error, error_size) != 0 ||
        c1pkg_run(list_arguments, names, C1PKG_INDEX_MAX, error, error_size) != 0 ||
        validate_tar_names(names, error, error_size) != 0 ||
        c1pkg_run(verbose_arguments, verbose, C1PKG_INDEX_MAX, error, error_size) != 0 ||
        validate_verbose_types(verbose, error, error_size) != 0 ||
        c1pkg_run(extract_arguments, NULL, C1PKG_UNPACKED_MAX, error, error_size) != 0 ||
        inspect_tree(extract, &total, error, error_size) != 0 ||
        validate_manifest(manifest, package, mode, error, error_size) != 0 ||
        stat(entry, &information) != 0 || !S_ISREG(information.st_mode) ||
        (information.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)) == 0) {
        if (error != NULL && error[0] == '\0') {
            c1pkg_set_error(error, error_size, "entry point is missing or not executable");
        }
        goto done;
    }
    /* Defense in depth: archives may normalize names differently from tar -t. */
    if (lstat(entry_metadata, &information) == 0 || errno != ENOENT ||
        lstat(mode_metadata, &information) == 0 || errno != ENOENT) {
        c1pkg_set_error(error, error_size, "payload contains reserved package metadata");
        goto done;
    }
    if ((mode[0] != '\0' && c1pkg_write_file(mode_metadata, mode, strlen(mode), 0444,
                                           error, error_size) != 0) ||
        c1pkg_write_file(entry_metadata, package->entry, strlen(package->entry), 0444,
                         error, error_size) != 0 ||
        seal_tree(payload, error, error_size) != 0 || chmod(payload, 0755) != 0) {
        if (error != NULL && error[0] == '\0') {
            c1pkg_set_error(error, error_size, "seal package: %s", strerror(errno));
        }
        goto done;
    }
    if (c1pkg_progress(NULL)) {
        c1pkg_set_error(error, error_size, "Cancelled");
        result = C1PKG_INSTALL_CANCELLED;
        goto done;
    }
    if (retained) {
        if (!payload_equal(payload, destination)) {
            c1pkg_set_error(error, error_size, "retained version differs from verified package; skipped safely");
            result = C1PKG_INSTALL_SKIPPED;
            goto done;
        }
        result = switch_current(app, package->version, error, error_size) == 0 ?
                 C1PKG_INSTALL_OK : C1PKG_INSTALL_STORAGE_ERROR;
        goto done;
    }
    if (c1pkg_storage_prepare(C1PKG_STORAGE_WRITE, 0U, error, error_size) != 0 ||
        c1pkg_mkdir_p(versions, 0755, error, error_size) != 0 ||
        c1pkg_sync_directory(payload, error, error_size) != 0 ||
        c1pkg_sync_directory(versions, error, error_size) != 0 ||
        c1pkg_sync_directory(app, error, error_size) != 0 ||
        c1pkg_sync_directory(C1PKG_APPS_ROOT, error, error_size) != 0 ||
        begin_transaction(app, package->version, payload, 1, error, error_size) != 0 ||
        rename(payload, destination) != 0 ||
        recover_transaction(error, error_size) != 0) {
        /* Never delete a committed payload on uncertain I/O: the WAL owns it. */
        result = C1PKG_INSTALL_STORAGE_ERROR;
        if (error != NULL && error[0] == '\0')
            c1pkg_set_error(error, error_size, "commit package transaction: %s", strerror(errno));
        goto done;
    }
    result = C1PKG_INSTALL_OK;
done:
    if (result == C1PKG_INSTALL_PACKAGE_ERROR) {
        int saved_errno = errno;
        if (c1pkg_progress(NULL)) result = C1PKG_INSTALL_CANCELLED;
        else if (saved_errno == ENOSPC || saved_errno == EIO || saved_errno == EROFS ||
                 saved_errno == EDQUOT ||
                 c1pkg_storage_prepare(C1PKG_STORAGE_WRITE, 0U, NULL, 0U) != 0)
            result = C1PKG_INSTALL_STORAGE_ERROR;
    }
    (void)remove_package_tree(stage, NULL, 0U);
    if (result == C1PKG_INSTALL_OK) cleanup_abandoned_work();
    release_lock(lock_descriptor);
    return result;
}

/* The graphical manager already owns the run lock to keep the desktop idle.
 * Duplicate its descriptor rather than trying to lock the same file twice.
 * This API is in-process only; no descriptor is accepted from CLI or packages. */
int c1pkg_store_remove_with_run_lock(const char *id, int owned_run_lock,
                                     char *error, size_t error_size)
{
    int lock_descriptor = -1;
    int run_descriptor = -1;
    char app[C1PKG_PATH_MAX];
    char trash[C1PKG_PATH_MAX];
    time_t now = time(NULL);
    int result = -1;

    if (!c1pkg_safe_id(id) ||
        c1pkg_storage_prepare(C1PKG_STORAGE_MAINTENANCE, 0U,
                              error, error_size) != 0 ||
        (lock_descriptor = acquire_lock(error, error_size)) < 0) {
        if (error != NULL && error[0] == '\0') {
            c1pkg_set_error(error, error_size, "unsafe application ID");
        }
        return -1;
    }
    cleanup_abandoned_work();
    run_descriptor = owned_run_lock >= 0 ? dup(owned_run_lock) : c1_app_run_acquire();
    if (run_descriptor < 0) {
        c1pkg_set_error(error, error_size, "cannot uninstall while an application is running");
        goto done;
    }
    if (c1pkg_join(app, sizeof(app), C1PKG_APPS_ROOT, id) != 0 ||
        snprintf(trash, sizeof(trash), "%s/%s.%lld.%ld", C1PKG_TRASH_ROOT, id,
                 (long long)now, (long)getpid()) < 0) {
        c1pkg_set_error(error, error_size, "application path is too long");
        goto done;
    }
    if (rename(app, trash) != 0 ||
        c1pkg_sync_directory(C1PKG_APPS_ROOT, error, error_size) != 0 ||
        c1pkg_sync_directory(C1PKG_TRASH_ROOT, error, error_size) != 0) {
        if (error != NULL && error[0] == '\0') {
            c1pkg_set_error(error, error_size, "remove %s: %s", id, strerror(errno));
        }
        goto done;
    }
    result = 0;
    if (remove_package_tree(trash, error, error_size) != 0) {
        c1pkg_set_error(error, error_size,
                        "application removed; deferred trash cleanup is required");
    }
done:
    c1_app_lease_release(run_descriptor);
    release_lock(lock_descriptor);
    return result;
}

int c1pkg_store_remove(const char *id, char *error, size_t error_size)
{
    return c1pkg_store_remove_with_run_lock(id, -1, error, error_size);
}

int c1pkg_store_rollback(const char *id, char *error, size_t error_size)
{
    int lock_descriptor = -1;
    char app[C1PKG_PATH_MAX];
    char previous[C1PKG_PATH_MAX];
    char target[C1PKG_PATH_MAX];
    const char prefix[] = "versions/";
    int result = -1;

    if (!c1pkg_safe_id(id)) {
        c1pkg_set_error(error, error_size, "unsafe application ID");
        return -1;
    }
    if (c1pkg_storage_prepare(C1PKG_STORAGE_MAINTENANCE, 0U,
                              error, error_size) != 0 ||
        (lock_descriptor = acquire_lock(error, error_size)) < 0) {
        return -1;
    }
    cleanup_abandoned_work();
    if (c1pkg_join(app, sizeof(app), C1PKG_APPS_ROOT, id) != 0 ||
        c1pkg_join(previous, sizeof(previous), app, "previous") != 0) {
        c1pkg_set_error(error, error_size, "application path is too long");
        goto done;
    }
    if (read_link_value(previous, target, sizeof(target)) != 0 ||
        strncmp(target, prefix, sizeof(prefix) - 1U) != 0 ||
        !c1pkg_safe_version(target + sizeof(prefix) - 1U)) {
        c1pkg_set_error(error, error_size, "rollback target is unavailable or unsafe");
        goto done;
    }
    if (switch_current(app, target + sizeof(prefix) - 1U, error, error_size) != 0) {
        goto done;
    }
    result = 0;
done:
    release_lock(lock_descriptor);
    return result;
}

static int launch_locked(const char *id, char *const extra_argv[],
                          char *error, size_t error_size)
{
    char app[C1PKG_PATH_MAX];
    char current[C1PKG_PATH_MAX];
    char target[C1PKG_PATH_MAX];
    char version_root[C1PKG_PATH_MAX];
    char metadata[C1PKG_PATH_MAX];
    char entry[C1PKG_ENTRY_MAX + 1U];
    unsigned char *data = NULL;
    size_t size = 0U;
    char executable[C1PKG_PATH_MAX];
    char *arguments[66];
    size_t count = 1U;
    int lease_descriptor = -1;
    const char *launch_mode;
    struct stat mode_info;

    if (c1pkg_storage_prepare(C1PKG_STORAGE_READ, 0U,
                              error, error_size) != 0 ||
        !c1pkg_safe_id(id) ||
        c1pkg_join(app, sizeof(app), C1PKG_APPS_ROOT, id) != 0 ||
        trusted_directory(app, &mode_info) != 0 ||
        c1pkg_join(version_root, sizeof(version_root), app, "versions") != 0 ||
        trusted_directory(version_root, &mode_info) != 0 ||
        c1pkg_join(current, sizeof(current), app, "current") != 0 ||
        read_link_value(current, target, sizeof(target)) != 0 ||
        strncmp(target, "versions/", 9U) != 0 ||
        !c1pkg_safe_version(target + 9U) ||
        c1pkg_join(version_root, sizeof(version_root), app, target) != 0 ||
        trusted_directory(version_root, &mode_info) != 0 ||
        c1pkg_join(metadata, sizeof(metadata), version_root, ".c1pkg-entry") != 0 ||
        c1pkg_read_file(metadata, &data, &size, C1PKG_ENTRY_MAX,
                        error, error_size) != 0) {
        c1pkg_set_error(error, error_size, "installed entry metadata is unavailable");
        return -1;
    }
    if (size == 0U || size > C1PKG_ENTRY_MAX || memchr(data, 0, size) != NULL) {
        free(data);
        c1pkg_set_error(error, error_size, "installed entry metadata is invalid");
        return -1;
    }
    data[size] = '\0';
    if (!c1pkg_safe_relpath((char *)data)) {
        free(data);
        c1pkg_set_error(error, error_size, "installed entry metadata is unsafe");
        return -1;
    }
    (void)strcpy(entry, (char *)data);
    free(data);
    if (c1pkg_join(executable, sizeof(executable), version_root, entry) != 0) {
        c1pkg_set_error(error, error_size, "entry point path is too long");
        return -1;
    }
    arguments[0] = executable;
    if (extra_argv != NULL) {
        while (extra_argv[count - 1U] != NULL && count < 65U) {
            arguments[count] = extra_argv[count - 1U];
            ++count;
        }
        if (count == 65U && extra_argv[count - 1U] != NULL) {
            c1pkg_set_error(error, error_size, "too many launch arguments");
            return -1;
        }
    }
    arguments[count] = NULL;
    launch_mode = c1pkg_app_uses_direct_io(id) ? "direct" : "terminal";
    if (c1pkg_join(metadata, sizeof(metadata), version_root, ".c1pkg-mode") != 0) return -1;
    if (lstat(metadata, &mode_info) == 0) {
        data = NULL;
        if (!S_ISREG(mode_info.st_mode) || mode_info.st_nlink != 1 ||
            mode_info.st_uid != geteuid() || (mode_info.st_mode & 0222) != 0 ||
            c1pkg_read_file(metadata, &data, &size, 8U, error, error_size) != 0) {
            free(data);
            c1pkg_set_error(error, error_size, "installed launch mode is unsafe");
            return -1;
        }
        if (size == 8U && memcmp(data, "terminal", 8U) == 0) launch_mode = "terminal";
        else if (size == 6U && memcmp(data, "direct", 6U) == 0) launch_mode = "direct";
        else {
            free(data);
            c1pkg_set_error(error, error_size, "installed launch mode is invalid");
            return -1;
        }
        free(data);
    } else if (errno != ENOENT) {
        c1pkg_set_error(error, error_size, "installed launch mode is unavailable");
        return -1;
    }
    if (strcmp(launch_mode, "direct") == 0) {
        lease_descriptor = c1_app_lease_acquire();
        if (lease_descriptor < 0) {
            c1pkg_set_error(error, error_size, "acquire direct-I/O hardware lease: %s", strerror(errno));
            return -1;
        }
    }
    if (!c1_app_lease_write_mode(launch_mode) || chdir(version_root) != 0) {
        int error_number = errno;

        c1_app_lease_clear_mode();
        c1_app_lease_release(lease_descriptor);
        c1pkg_set_error(error, error_size, "prepare launch %s: %s", executable,
                        strerror(error_number));
        return -1;
    }
    execv(executable, arguments);
    {
        int error_number = errno;

        c1_app_lease_clear_mode();
        c1_app_lease_release(lease_descriptor);
        c1pkg_set_error(error, error_size, "launch %s: %s", executable,
                        strerror(error_number));
    }
    return -1;
}

int c1pkg_store_launch(const char *id, char *const extra_argv[],
                       char *error, size_t error_size)
{
    int store_descriptor, run_descriptor, result;
    if (!c1pkg_safe_id(id) ||
        c1pkg_storage_prepare(C1PKG_STORAGE_READ, 0U, error, error_size) != 0) return -1;
    store_descriptor = acquire_lock(error, error_size);
    if (store_descriptor < 0) return -1;
    run_descriptor = c1_app_run_acquire();
    if (run_descriptor < 0) {
        release_lock(store_descriptor);
        c1pkg_set_error(error, error_size, "another application is active or its run lock is unavailable");
        return -1;
    }
    /* Clear only after owning run, so failed concurrent launches cannot erase
     * the running application's mode. The UI refreshes mode while run is held. */
    c1_app_lease_clear_mode();
    result = launch_locked(id, extra_argv, error, error_size);
    c1_app_lease_clear_mode();
    c1_app_lease_release(run_descriptor);
    release_lock(store_descriptor);
    return result;
}