#include "pkg.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int store_path(char *out, size_t out_size, const char *suffix,
                      char *error, size_t error_size)
{
    if (c1pkg_join(out, out_size, C1PKG_STATE_ROOT, suffix) != 0) {
        c1pkg_set_error(error, error_size, "state path is too long");
        return -1;
    }
    return 0;
}

int c1pkg_store_init(char *error, size_t error_size)
{
    static const char *const state_directories[] = {"cache", "staging", "trash"};
    size_t i;

    if (c1pkg_mkdir_p(C1PKG_STATE_ROOT, 0700, error, error_size) != 0 ||
        c1pkg_mkdir_p(C1PKG_APPS_ROOT, 0755, error, error_size) != 0) {
        return -1;
    }
    for (i = 0U; i < sizeof(state_directories) / sizeof(state_directories[0]); ++i) {
        char path[C1PKG_PATH_MAX];
        if (store_path(path, sizeof(path), state_directories[i], error, error_size) != 0 ||
            c1pkg_mkdir_p(path, 0700, error, error_size) != 0) {
            return -1;
        }
    }
    return 0;
}

static int acquire_lock(char *lock_path, size_t lock_size, char *error, size_t error_size)
{
    if (store_path(lock_path, lock_size, "lock", error, error_size) != 0) {
        return -1;
    }
    if (mkdir(lock_path, 0700) != 0) {
        c1pkg_set_error(error, error_size, errno == EEXIST ?
                        "another package operation is in progress" : "create lock: %s",
                        strerror(errno));
        return -1;
    }
    return 0;
}

static void release_lock(const char *lock_path)
{
    (void)rmdir(lock_path);
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

    list->count = 0U;
    if (c1pkg_store_init(error, error_size) != 0) {
        return -1;
    }
    directory = opendir(C1PKG_APPS_ROOT);
    if (directory == NULL) {
        c1pkg_set_error(error, error_size, "open applications directory: %s", strerror(errno));
        return -1;
    }
    while ((entry = readdir(directory)) != NULL) {
        struct c1pkg_installed *installed;
        if (!c1pkg_safe_id(entry->d_name)) {
            continue;
        }
        if (list->count >= C1PKG_MAX_PACKAGES) {
            (void)closedir(directory);
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
        if (!c1pkg_safe_relpath(name) ||
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
            if (seal_tree(child, error, error_size) != 0 || chmod(child, 0555) != 0) {
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
        }
    }
    (void)closedir(directory);
    return 0;
}

static int validate_manifest(const char *path, const struct c1pkg_package *package,
                             char *error, size_t error_size)
{
    unsigned char *data = NULL;
    size_t size = 0U;
    char expected[512];
    int length;
    int result = -1;

    length = snprintf(expected, sizeof(expected), "C1PKG-PACKAGE 1\nid\t%s\nversion\t%s\nentry\t%s\n",
                      package->id, package->version, package->entry);
    if (length < 0 || (size_t)length >= sizeof(expected)) {
        c1pkg_set_error(error, error_size, "manifest fields are too long");
        return -1;
    }
    if (c1pkg_read_file(path, &data, &size, sizeof(expected), error, error_size) == 0 &&
        size == (size_t)length && memcmp(data, expected, size) == 0) {
        result = 0;
    } else if (data != NULL) {
        c1pkg_set_error(error, error_size, "package manifest does not match signed index");
    }
    free(data);
    return result;
}

static int switch_current(const char *app, const char *version,
                          char *error, size_t error_size)
{
    char current[C1PKG_PATH_MAX];
    char previous[C1PKG_PATH_MAX];
    char temporary[C1PKG_PATH_MAX];
    char target[64];
    char old_target[C1PKG_PATH_MAX];
    int count;

    if (c1pkg_join(current, sizeof(current), app, "current") != 0 ||
        c1pkg_join(previous, sizeof(previous), app, "previous") != 0 ||
        snprintf(temporary, sizeof(temporary), "%s/current.%ld.tmp", app, (long)getpid()) < 0) {
        c1pkg_set_error(error, error_size, "application path is too long");
        return -1;
    }
    count = snprintf(target, sizeof(target), "versions/%s", version);
    if (count < 0 || (size_t)count >= sizeof(target)) {
        c1pkg_set_error(error, error_size, "version path is too long");
        return -1;
    }
    (void)unlink(temporary);
    if (symlink(target, temporary) != 0) {
        c1pkg_set_error(error, error_size, "create current pointer: %s", strerror(errno));
        return -1;
    }
    if (read_link_value(current, old_target, sizeof(old_target)) == 0) {
        char previous_tmp[C1PKG_PATH_MAX];
        if (snprintf(previous_tmp, sizeof(previous_tmp), "%s/previous.%ld.tmp", app,
                     (long)getpid()) < 0 || symlink(old_target, previous_tmp) != 0 ||
            rename(previous_tmp, previous) != 0) {
            (void)unlink(previous_tmp);
            (void)unlink(temporary);
            c1pkg_set_error(error, error_size, "save rollback pointer: %s", strerror(errno));
            return -1;
        }
    }
    if (rename(temporary, current) != 0) {
        (void)unlink(temporary);
        c1pkg_set_error(error, error_size, "activate version: %s", strerror(errno));
        return -1;
    }
    return 0;
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
    char lock[C1PKG_PATH_MAX];
    char stage[C1PKG_PATH_MAX];
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
    char url[1400];
    char *list_arguments[] = {(char *)tar, "-tzf", archive, NULL};
    char *verbose_arguments[] = {(char *)tar, "-tvzf", archive, NULL};
    char *extract_arguments[] = {(char *)tar, "-xzf", archive, "-C", extract, NULL};
    struct stat information;
    uint64_t total = 0U;
    int result = -1;

    if (c1pkg_store_init(error, error_size) != 0 ||
        acquire_lock(lock, sizeof(lock), error, error_size) != 0) {
        return -1;
    }
    if (snprintf(stage, sizeof(stage), "%s/staging/install.%ld", C1PKG_STATE_ROOT,
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
        package_url(url, sizeof(url), config, package) != 0) {
        c1pkg_set_error(error, error_size, "package path or URL is too long");
        goto done;
    }
    (void)c1pkg_remove_tree(stage, NULL, 0U);
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
        validate_manifest(manifest, package, error, error_size) != 0 ||
        stat(entry, &information) != 0 || !S_ISREG(information.st_mode) ||
        (information.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)) == 0) {
        if (error != NULL && error[0] == '\0') {
            c1pkg_set_error(error, error_size, "entry point is missing or not executable");
        }
        goto done;
    }
    if (c1pkg_write_file(entry_metadata, package->entry, strlen(package->entry), 0444,
                         error, error_size) != 0 ||
        seal_tree(payload, error, error_size) != 0 || chmod(payload, 0555) != 0) {
        if (error != NULL && error[0] == '\0') {
            c1pkg_set_error(error, error_size, "seal package: %s", strerror(errno));
        }
        goto done;
    }
    if (access(destination, F_OK) == 0) {
        c1pkg_set_error(error, error_size, "version %s is already installed", package->version);
        goto done;
    }
    if (c1pkg_mkdir_p(versions, 0755, error, error_size) != 0 ||
        rename(payload, destination) != 0) {
        c1pkg_set_error(error, error_size, "commit package files: %s", strerror(errno));
        goto done;
    }
    if (switch_current(app, package->version, error, error_size) != 0) {
        (void)c1pkg_remove_tree(destination, NULL, 0U);
        goto done;
    }
    result = 0;
done:
    (void)c1pkg_remove_tree(stage, NULL, 0U);
    release_lock(lock);
    return result;
}

int c1pkg_store_remove(const char *id, char *error, size_t error_size)
{
    char lock[C1PKG_PATH_MAX];
    char app[C1PKG_PATH_MAX];
    char trash[C1PKG_PATH_MAX];
    time_t now = time(NULL);
    int result = -1;

    if (!c1pkg_safe_id(id) || c1pkg_store_init(error, error_size) != 0 ||
        acquire_lock(lock, sizeof(lock), error, error_size) != 0) {
        if (error != NULL && error[0] == '\0') {
            c1pkg_set_error(error, error_size, "unsafe application ID");
        }
        return -1;
    }
    if (c1pkg_join(app, sizeof(app), C1PKG_APPS_ROOT, id) != 0 ||
        snprintf(trash, sizeof(trash), "%s/trash/%s.%lld.%ld", C1PKG_STATE_ROOT, id,
                 (long long)now, (long)getpid()) < 0) {
        c1pkg_set_error(error, error_size, "application path is too long");
        goto done;
    }
    if (rename(app, trash) != 0) {
        c1pkg_set_error(error, error_size, "remove %s: %s", id, strerror(errno));
        goto done;
    }
    result = 0;
    if (c1pkg_remove_tree(trash, error, error_size) != 0) {
        c1pkg_set_error(error, error_size,
                        "application removed; deferred trash cleanup is required");
    }
done:
    release_lock(lock);
    return result;
}

int c1pkg_store_rollback(const char *id, char *error, size_t error_size)
{
    char lock[C1PKG_PATH_MAX];
    char app[C1PKG_PATH_MAX];
    char previous[C1PKG_PATH_MAX];
    char target[C1PKG_PATH_MAX];
    const char prefix[] = "versions/";
    int result = -1;

    if (!c1pkg_safe_id(id)) {
        c1pkg_set_error(error, error_size, "unsafe application ID");
        return -1;
    }
    if (c1pkg_store_init(error, error_size) != 0 ||
        acquire_lock(lock, sizeof(lock), error, error_size) != 0) {
        return -1;
    }
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
    release_lock(lock);
    return result;
}

int c1pkg_store_launch(const char *id, char *const extra_argv[],
                       char *error, size_t error_size)
{
    char app[C1PKG_PATH_MAX];
    char current[C1PKG_PATH_MAX];
    char metadata[C1PKG_PATH_MAX];
    char entry[C1PKG_ENTRY_MAX + 1U];
    unsigned char *data = NULL;
    size_t size = 0U;
    char executable[C1PKG_PATH_MAX];
    char *arguments[66];
    size_t count = 1U;

    if (!c1pkg_safe_id(id) || c1pkg_join(app, sizeof(app), C1PKG_APPS_ROOT, id) != 0 ||
        c1pkg_join(current, sizeof(current), app, "current") != 0 ||
        c1pkg_join(metadata, sizeof(metadata), current, ".c1pkg-entry") != 0 ||
        c1pkg_read_file(metadata, &data, &size, C1PKG_ENTRY_MAX,
                        error, error_size) != 0) {
        c1pkg_set_error(error, error_size, "installed entry metadata is unavailable");
        return -1;
    }
    if (size == 0U || size > C1PKG_ENTRY_MAX) {
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
    if (c1pkg_join(executable, sizeof(executable), current, entry) != 0) {
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
    execv(executable, arguments);
    c1pkg_set_error(error, error_size, "launch %s: %s", executable, strerror(errno));
    return -1;
}