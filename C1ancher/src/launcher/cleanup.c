#define _GNU_SOURCE 1

#include "launcher/cleanup.h"
#include "update/update.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define CLEANUP_PATH_MAX 4096U
#define RELEASE_NAME_MAX 192U
#define CLEANUP_DEPTH_MAX 256U
#ifndef C1_LAUNCHER_CLEANUP_BUDGET_MS
#define C1_LAUNCHER_CLEANUP_BUDGET_MS 30000
#endif
/* A compile-time override is used only by disposable host fixtures. There is
 * deliberately no environment/command-line override in the real launcher. */
#ifndef C1_LAUNCHER_DATA_ROOT
#define C1_LAUNCHER_DATA_ROOT "/usr/data"
#define REQUIRE_DATA_MOUNT 1
#else
#define REQUIRE_DATA_MOUNT 0
#endif
#define DEPLOYED_CORE C1_LAUNCHER_DATA_ROOT "/c1/core"
#define TRASH_NAME ".launcher-cleanup"

#define CLEANUP_MARKER "cleanup-completed.v1"
enum cleanup_scope { RELEASES, DATA, C1_DATA, BIN, CORE, SCOPE_COUNT };
static const char *const scope_tags[] = {"release--", "data--", "c1--", "bin--", "core--"};

struct cleanup {
    int core, trash, lock;
    int parents[SCOPE_COUNT];
    dev_t device;
    char core_path[CLEANUP_PATH_MAX];
    char current[RELEASE_NAME_MAX], previous[RELEASE_NAME_MAX];
    uint64_t predecessor;
    int deployed;
    unsigned int removed;
};

static int cleanup_done;
static int64_t cleanup_deadline;

static int reject(int error)
{
    errno = error;
    return -1;
}

static int within_budget(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return -1;
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000 < cleanup_deadline
               ? 0 : reject(ETIMEDOUT);
}

static int same_inode(const struct stat *left, const struct stat *right)
{
    return left->st_dev == right->st_dev && left->st_ino == right->st_ino;
}

static int owned_directory(const struct stat *info, int shared)
{
    return S_ISDIR(info->st_mode) && (shared == 2 || info->st_uid == geteuid()) &&
           (shared || (info->st_mode & 0022U) == 0);
}

/* st_dev alone does not detect bind mounts of the same filesystem. Linux
 * exposes the mount ID even on the device's older kernel/libc combination. */
static int mount_id(int fd, unsigned long *id)
{
    char path[64], text[1024], *line;
    ssize_t length;
    int probe, saved;
    (void)snprintf(path, sizeof(path), "/proc/self/fdinfo/%d", fd);
    probe = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (probe < 0) return -1;
    length = read(probe, text, sizeof(text) - 1U);
    saved = errno;
    close(probe);
    errno = saved;
    if (length < 0) return -1;
    text[length] = '\0';
    line = strstr(text, "mnt_id:\t");
    if (line == NULL || sscanf(line, "mnt_id:\t%lu", id) != 1) return reject(EIO);
    return 0;
}

static int same_mount(int left, int right)
{
    unsigned long a, b;
    if (mount_id(left, &a) != 0 || mount_id(right, &b) != 0) return -1;
    return a == b ? 0 : reject(EXDEV);
}

static int data_partition_mounted(int data)
{
    unsigned long data_mount, parent_mount;
    int parent = openat(data, "..", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    int result, saved;
    if (parent < 0) return -1;
    result = mount_id(data, &data_mount);
    if (result == 0) result = mount_id(parent, &parent_mount);
    if (result == 0 && data_mount == parent_mount) result = reject(EXDEV);
    saved = errno;
    close(parent);
    return result == 0 ? 0 : reject(saved);
}

static int parse_release_name(const char *name, uint64_t *sequence)
{
    uint64_t value = 0U;
    const char *cursor = name, *version;
    if (name == NULL || name[0] < '0' || name[0] > '9' ||
        strlen(name) >= RELEASE_NAME_MAX) return 0;
    while (*cursor >= '0' && *cursor <= '9') {
        unsigned int digit = (unsigned int)(*cursor++ - '0');
        if (value > (UINT64_MAX - digit) / 10U) return 0;
        value = value * 10U + digit;
    }
    if (*cursor++ != '-') return 0;
    version = cursor;
    if (*version == '\0' || *version == '.' || strlen(version) > 64U) return 0;
    while (*cursor != '\0') {
        unsigned char c = (unsigned char)*cursor++;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-' || c == '+')) return 0;
    }
    if (cursor[-1] == '.') return 0;
    *sequence = value;
    return 1;
}

static int open_directory_at(int parent, const char *name, dev_t device, int shared)
{
    struct stat info;
    int fd = openat(parent, name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return -1;
    if (fstat(fd, &info) != 0 || !owned_directory(&info, shared) ||
        info.st_dev != device) {
        close(fd);
        return reject(EACCES);
    }
    if (same_mount(parent, fd) != 0) {
        int saved = errno;
        close(fd);
        return reject(saved);
    }
    return fd;
}

/* Pin every ancestor without following links. The existing root-owned shared
 * c1 container is supported only in the exact deployed layout. Its private
 * core and running executable must still pass all identity checks below.
 * No chmod of c1, user directories, or other ancestors is necessary. */
static int open_absolute_directory(const char *path, int deployed)
{
    char copy[CLEANUP_PATH_MAX], *component, *slash;
    int fd;
    struct stat info;
    if (path == NULL || path[0] != '/' || strlen(path) >= sizeof(copy)) return reject(EINVAL);
    strcpy(copy, path);
    fd = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return -1;
    component = copy + 1;
    for (;;) {
        int next, shared;
        slash = strchr(component, '/');
        shared = deployed && slash != NULL && strcmp(slash, "/core") == 0;
        if (slash != NULL) *slash = '\0';
        shared = shared && strcmp(component, "c1") == 0;
        if (*component == '\0' || strcmp(component, ".") == 0 || strcmp(component, "..") == 0) {
            close(fd);
            return reject(EINVAL);
        }
        next = openat(fd, component, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        close(fd);
        if (next < 0) return -1;
        fd = next;
        if (fstat(fd, &info) != 0 ||
            (info.st_uid != 0 && info.st_uid != geteuid()) ||
            (shared && info.st_uid != geteuid()) ||
            ((info.st_mode & 0022U) != 0 && !shared &&
             !(slash != NULL && (info.st_mode & S_ISVTX)))) {
            close(fd);
            return reject(EACCES);
        }
        if (slash == NULL) break;
        component = slash + 1;
    }
    if (!owned_directory(&info, 0)) { close(fd); return reject(EACCES); }
    return fd;
}

/* dup() shares the readdir offset. Every pass needs its own open description. */
static DIR *directory_stream(int fd)
{
    int copy = openat(fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    DIR *stream;
    if (copy < 0) return NULL;
    stream = fdopendir(copy);
    if (stream == NULL) close(copy);
    return stream;
}

static int open_release(int root, const char *name, dev_t device)
{
    int fd = open_directory_at(root, name, device, 0), artifacts;
    if (fd < 0) return -1;
    artifacts = open_directory_at(fd, "artifacts", device, 0);
    if (artifacts < 0) { close(fd); return -1; }
    close(artifacts);
    return fd;
}

static int read_pointer(int core, const char *which, char target[RELEASE_NAME_MAX])
{
    char link[RELEASE_NAME_MAX + 9U];
    struct stat info;
    ssize_t length;
    uint64_t sequence;
    target[0] = '\0';
    if (fstatat(core, which, &info, AT_SYMLINK_NOFOLLOW) != 0)
        return errno == ENOENT ? 0 : -1;
    if (!S_ISLNK(info.st_mode) || info.st_uid != geteuid()) return reject(EACCES);
    length = readlinkat(core, which, link, sizeof(link) - 1U);
    if (length < 0) return -1;
    if ((size_t)length >= sizeof(link) - 1U) return reject(ENAMETOOLONG);
    link[length] = '\0';
    if (strncmp(link, "releases/", 9U) != 0 || !parse_release_name(link + 9U, &sequence))
        return reject(EINVAL);
    strcpy(target, link + 9U);
    return 0;
}

static int named_inode(int parent, const char *name, int fd)
{
    struct stat opened, named;
    if (fstat(fd, &opened) != 0 || fstatat(parent, name, &named, AT_SYMLINK_NOFOLLOW) != 0)
        return -1;
    return same_inode(&opened, &named) ? 0 : reject(ESTALE);
}

static int anchors_unchanged(struct cleanup *ctx)
{
    char current[RELEASE_NAME_MAX], previous[RELEASE_NAME_MAX];
    if (named_inode(ctx->core, "releases", ctx->parents[RELEASES]) != 0 ||
        read_pointer(ctx->core, "current", current) != 0 ||
        read_pointer(ctx->core, "previous", previous) != 0) return -1;
    if (strcmp(current, ctx->current) != 0 || strcmp(previous, ctx->previous) != 0)
        return reject(ESTALE);
    if (ctx->deployed &&
        (named_inode(ctx->parents[DATA], "c1", ctx->parents[C1_DATA]) != 0 ||
         named_inode(ctx->parents[C1_DATA], "core", ctx->core) != 0)) return -1;
    if (ctx->parents[BIN] >= 0 &&
        named_inode(ctx->parents[C1_DATA], "bin", ctx->parents[BIN]) != 0) return -1;
    return 0;
}

/* Hold the same nonblocking POSIX lock as prepare/activate/rollback. Never
 * replace or unlink it, and never compete with a running update transaction. */
static int lock_updater(struct cleanup *ctx)
{
    struct flock lock = {0};
    struct stat info;
    int update, state, result = -1, saved;
    update = open_directory_at(ctx->parents[C1_DATA], "update", ctx->device, 0);
    if (update < 0) return -1;
    state = open_directory_at(update, "state", ctx->device, 0);
    close(update);
    if (state < 0) return -1;
    ctx->lock = openat(state, ".c1updater.lock", O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (ctx->lock >= 0 && fstat(ctx->lock, &info) == 0 && S_ISREG(info.st_mode) &&
        info.st_uid == geteuid() && info.st_nlink == 1 && (info.st_mode & 0777U) == 0600U) {
        lock.l_type = F_WRLCK;
        lock.l_whence = SEEK_SET;
        result = fcntl(ctx->lock, F_SETLK, &lock);
        if (result == 0) result = named_inode(state, ".c1updater.lock", ctx->lock);
    } else errno = EACCES;
    saved = errno;
    close(state);
    return result == 0 ? 0 : reject(saved);
}

/* Examine a whole candidate before moving it. Destructive traversal happens
 * only after it is in the private quarantine. Seal only directories being
 * deleted: historical 0777/0666 deployment leftovers must be removable, while
 * open directory descriptors held by other users must not enable tree swaps. */
static int walk_tree(int fd, dev_t device, unsigned int depth, int remove, int legacy)
{
    DIR *stream;
    struct dirent *entry;
    int result = 0, saved;
    if (depth > CLEANUP_DEPTH_MAX) return reject(ELOOP);
    if (remove && fchmod(fd, 0700) != 0) return -1;
    stream = directory_stream(fd);
    if (stream == NULL) return -1;
    for (;;) {
        struct stat before, after;
        int child;
        if (within_budget() != 0) { result = -1; break; }
        errno = 0;
        entry = readdir(stream);
        if (entry == NULL) { if (errno != 0) result = -1; break; }
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        if (fstatat(fd, entry->d_name, &before, AT_SYMLINK_NOFOLLOW) != 0) { result = -1; break; }
        if (before.st_dev != device ||
            (!legacy && (before.st_uid != geteuid() ||
             (!S_ISLNK(before.st_mode) && (before.st_mode & 0022U) != 0)))) {
            result = reject(EACCES); break;
        }
        if (S_ISDIR(before.st_mode)) {
            child = open_directory_at(fd, entry->d_name, device, legacy ? 2 : 0);
            if (child < 0) { result = -1; break; }
            if (fstat(child, &after) != 0 || !same_inode(&before, &after)) result = reject(ESTALE);
            else result = walk_tree(child, device, depth + 1U, remove, legacy);
            saved = errno;
            close(child);
            errno = saved;
            if (result != 0) break;
        } else if (!legacy && !(S_ISREG(before.st_mode) && before.st_nlink == 1) && !S_ISLNK(before.st_mode)) {
            result = reject(EACCES); break;
        }
        if (remove) {
            if (fstatat(fd, entry->d_name, &after, AT_SYMLINK_NOFOLLOW) != 0 ||
                !same_inode(&before, &after)) { result = reject(ESTALE); break; }
            if (unlinkat(fd, entry->d_name, S_ISDIR(before.st_mode) ? AT_REMOVEDIR : 0) != 0) {
                result = -1; break;
            }
        }
    }
    saved = errno;
    if (closedir(stream) != 0 && result == 0) return -1;
    errno = saved;
    return result;
}

static int digits(const char *text, size_t count)
{
    size_t i;
    for (i = 0; i < count; ++i) if (text[i] < '0' || text[i] > '9') return 0;
    return 1;
}

/* The data partition is desktop-owned. After each confirmed update keep only
 * desktop runtime/configuration; all other names, including hidden files,
 * third-party data, old recovery downloads and debug backups, are removed.
 * /storage is never traversed. These are retained resources, not opt-in flags. */
static int desktop_entry(enum cleanup_scope scope, const char *name)
{
    static const char *const retained_c1[] = {
        "core", "bin", "pkg", "update", "wifi", "neofetch", "enabled",
        "desktop.conf", "battery-history.cache", "desktop-summary.cache",
        "desktop-seen.cache", "disable-auto-suspend"
    };
    static const char *const retained_bin[] = {
        "C1ancher", "app_daemon", "c1pkg", "c1updater", "neofetch", "c1-update-check"
    };
    const char *const *list = NULL;
    size_t count = 0U;
    if (scope == DATA) return strcmp(name, "c1") == 0;
    if (scope == CORE) return strcmp(name, "releases") == 0 ||
        strcmp(name, "current") == 0 || strcmp(name, "previous") == 0 ||
        strcmp(name, TRASH_NAME) == 0 || strcmp(name, CLEANUP_MARKER) == 0;
    if (scope == C1_DATA) {
        list = retained_c1; count = sizeof(retained_c1) / sizeof(retained_c1[0]);
    } else if (scope == BIN) {
        list = retained_bin; count = sizeof(retained_bin) / sizeof(retained_bin[0]);
    }
    for (size_t i = 0U; i < count; ++i) if (strcmp(name, list[i]) == 0) return 1;
    return 0;
}

static int candidate_name(struct cleanup *ctx, enum cleanup_scope scope, const char *name)
{
    uint64_t sequence;
    if (!name[0] || !strcmp(name, ".") || !strcmp(name, "..") || strchr(name, '/')) return 0;
    if (scope != RELEASES) return ctx->deployed && !desktop_entry(scope, name);
    if (ctx->deployed) return strcmp(name, ctx->current) != 0 && strcmp(name, ctx->previous) != 0;
    return parse_release_name(name, &sequence) && sequence < ctx->predecessor &&
           strcmp(name, ctx->current) != 0 && strcmp(name, ctx->previous) != 0;
}

static int candidate_path(struct cleanup *ctx, enum cleanup_scope scope, const char *name,
                          int staged, char out[CLEANUP_PATH_MAX])
{
    const char *base = scope == DATA ? C1_LAUNCHER_DATA_ROOT :
                       scope == C1_DATA ? C1_LAUNCHER_DATA_ROOT "/c1" :
                       scope == BIN ? C1_LAUNCHER_DATA_ROOT "/c1/bin" : ctx->core_path;
    int length = staged ? snprintf(out, CLEANUP_PATH_MAX, "%s/%s/%s", ctx->core_path, TRASH_NAME, name) :
                          snprintf(out, CLEANUP_PATH_MAX, "%s/%s%s", base, scope == RELEASES ? "releases/" : "", name);
    return length >= 0 && (size_t)length < CLEANUP_PATH_MAX ? 0 : reject(ENAMETOOLONG);
}

static int reference_matches(int parent, const char *name, const char *path)
{
    char target[CLEANUP_PATH_MAX];
    size_t size = strlen(path);
    ssize_t count = readlinkat(parent, name, target, sizeof(target) - 1U);
    if (count < 0) return errno == ENOENT || errno == ESRCH ? 0 : -1;
    if ((size_t)count >= sizeof(target) - 1U) return reject(ENAMETOOLONG);
    target[count] = '\0';
    return strncmp(target, path, size) == 0 &&
           (target[size] == '\0' || target[size] == '/' || strcmp(target + size, " (deleted)") == 0);
}

/* A test/backup directory can still be in use by a separate process. Never
 * kill that process or delete its executable, cwd, or open data files. */
static int path_in_use(const char *path)
{
    DIR *processes = opendir("/proc");
    struct dirent *entry;
    int result = 0, saved;
    if (processes == NULL) return -1;
    while ((entry = readdir(processes)) != NULL) {
        const char *const refs[] = {"exe", "cwd", "root"};
        int process, fds;
        struct stat owner;
        size_t i;
        if (within_budget() != 0) { result = -1; break; }
        if (!digits(entry->d_name, strlen(entry->d_name)) ||
            strtoul(entry->d_name, NULL, 10) == (unsigned long)getpid()) continue;
        process = openat(dirfd(processes), entry->d_name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (process < 0) { if (errno == ENOENT || errno == ESRCH) continue; result = -1; break; }
        /* Production runs as root and checks every process. An unprivileged
         * host fixture cannot inspect another account's /proc descriptors. */
        if (fstat(process, &owner) != 0) { close(process); result = -1; break; }
        if (geteuid() != 0 && owner.st_uid != geteuid()) { close(process); continue; }
        for (i = 0U; i < sizeof(refs) / sizeof(refs[0]); ++i) {
            result = reference_matches(process, refs[i], path);
            if (result != 0) break;
        }
        fds = result == 0 ? openat(process, "fd", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW) : -1;
        if (result == 0 && fds < 0 && errno != ENOENT && errno != ESRCH) result = -1;
        if (fds >= 0) {
            DIR *stream = fdopendir(fds);
            struct dirent *file;
            if (stream == NULL) { close(fds); result = -1; }
            else {
                while ((file = readdir(stream)) != NULL) {
                    if (file->d_name[0] == '.') continue;
                    if (within_budget() != 0) { result = -1; break; }
                    result = reference_matches(dirfd(stream), file->d_name, path);
                    if (result != 0) break;
                }
                saved = errno;
                closedir(stream);
                errno = saved;
            }
        }
        saved = errno;
        close(process);
        errno = saved;
        if (result != 0) break;
    }
    saved = errno;
    closedir(processes);
    errno = saved;
    return result > 0 ? reject(EBUSY) : result;
}

static int scan_predecessor(struct cleanup *ctx, uint64_t current)
{
    DIR *stream = directory_stream(ctx->parents[RELEASES]);
    struct dirent *entry;
    int saved = 0;
    if (stream == NULL) return -1;
    ctx->predecessor = 0U;
    for (;;) {
        uint64_t sequence;
        int fd;
        if (within_budget() != 0) { saved = errno; break; }
        errno = 0;
        entry = readdir(stream);
        if (entry == NULL) { saved = errno; break; }
        if (!parse_release_name(entry->d_name, &sequence) || sequence >= current || sequence <= ctx->predecessor)
            continue;
        fd = open_release(ctx->parents[RELEASES], entry->d_name, ctx->device);
        if (fd < 0) continue;
        close(fd);
        ctx->predecessor = sequence;
    }
    if (closedir(stream) != 0 && saved == 0) saved = errno;
    return saved == 0 ? 0 : reject(saved);
}

static int erase_staged(struct cleanup *ctx, const char *name, int legacy, int check_references)
{
    struct stat before, after;
    char path[CLEANUP_PATH_MAX];
    int fd = -1, result, saved;
    if (candidate_path(ctx, RELEASES, name, 1, path) != 0 ||
        (check_references && path_in_use(path) != 0) ||
        anchors_unchanged(ctx) != 0 ||
        fstatat(ctx->trash, name, &before, AT_SYMLINK_NOFOLLOW) != 0) return -1;
    if ((!ctx->deployed && before.st_uid != geteuid()) || before.st_dev != ctx->device) return reject(EACCES);
    if (S_ISDIR(before.st_mode)) {
        fd = open_directory_at(ctx->trash, name, ctx->device, legacy ? 2 : 0);
        if (fd < 0) return -1;
        result = fstat(fd, &after);
        if (result == 0 && !same_inode(&before, &after)) result = reject(ESTALE);
        if (result == 0) result = walk_tree(fd, ctx->device, 0U, 0, legacy);
        if (result == 0) result = walk_tree(fd, ctx->device, 0U, 1, legacy);
    } else result = legacy || (S_ISREG(before.st_mode) && before.st_nlink == 1) || S_ISLNK(before.st_mode)
                        ? 0 : reject(EACCES);
    if (result == 0) {
        if (fstatat(ctx->trash, name, &after, AT_SYMLINK_NOFOLLOW) != 0 || !same_inode(&before, &after))
            result = reject(ESTALE);
        else result = unlinkat(ctx->trash, name, S_ISDIR(before.st_mode) ? AT_REMOVEDIR : 0);
    }
    saved = errno;
    if (fd >= 0) close(fd);
    errno = saved;
    if (result == 0) {
        ++ctx->removed;
        fprintf(stderr, "C1 launcher: cleanup removed %s\n", name);
    }
    return result;
}

static int remove_candidate(struct cleanup *ctx, enum cleanup_scope scope, const char *name)
{
    char staged[256], path[CLEANUP_PATH_MAX];
    struct stat before, after;
    int parent = ctx->parents[scope], fd = -1, result, saved, count;
    if (fstatat(parent, name, &before, AT_SYMLINK_NOFOLLOW) != 0) return -1;
    if ((!ctx->deployed && before.st_uid != geteuid()) || before.st_dev != ctx->device) return reject(EACCES);
    /* Release-shaped symlinks and directories without artifacts are not
     * installed generations; retain them instead of treating them as releases. */
    if (scope == RELEASES && !ctx->deployed) fd = open_release(parent, name, ctx->device);
    else if (S_ISDIR(before.st_mode)) fd = open_directory_at(parent, name, ctx->device, 2);
    else if (!ctx->deployed && !S_ISLNK(before.st_mode) && !(S_ISREG(before.st_mode) && before.st_nlink == 1)) return reject(EACCES);
    if (((scope == RELEASES && !ctx->deployed) || S_ISDIR(before.st_mode)) && fd < 0) return -1;
    result = 0;
    if (fd >= 0) {
        if (fstat(fd, &after) != 0 || !same_inode(&before, &after)) result = reject(ESTALE);
        if (result == 0) result = walk_tree(fd, ctx->device, 0U, 0, ctx->deployed);
        saved = errno;
        close(fd);
        errno = saved;
    }
    if (result != 0) return -1;
    if (candidate_path(ctx, scope, name, 0, path) != 0 ||
        (scope == RELEASES && path_in_use(path) != 0) ||
        anchors_unchanged(ctx) != 0 || within_budget() != 0) return -1;
    count = snprintf(staged, sizeof(staged), "%s%s", scope_tags[scope], name);
    if (count < 0) return reject(EINVAL);
    if ((size_t)count >= sizeof(staged)) {
        uint64_t hash = UINT64_C(14695981039346656037);
        for (const unsigned char *p = (const unsigned char *)name; *p; ++p)
            hash = (hash ^ *p) * UINT64_C(1099511628211);
        snprintf(staged, sizeof(staged), "%s~%016llx", scope_tags[scope], (unsigned long long)hash);
    }
    /* Same-filesystem atomic quarantine; never overwrite a pending item. The
     * post-rename inode check detects swaps even under the shared c1 parent. */
    if (renameat2(parent, name, ctx->trash, staged, RENAME_NOREPLACE) != 0) return -1;
    if (fstatat(ctx->trash, staged, &after, AT_SYMLINK_NOFOLLOW) != 0 || !same_inode(&before, &after)) {
        (void)renameat2(ctx->trash, staged, parent, name, RENAME_NOREPLACE);
        return reject(ESTALE);
    }
    /* Recheck references after the move too. An active candidate is restored
     * without replacing anything created at its original name in the meantime. */
    if (candidate_path(ctx, scope, staged, 1, path) != 0 ||
        (scope == RELEASES && path_in_use(path) != 0)) {
        saved = errno;
        (void)renameat2(ctx->trash, staged, parent, name, RENAME_NOREPLACE);
        return reject(saved);
    }
    return erase_staged(ctx, staged, ctx->deployed, 0);
}

static int scan_scope(struct cleanup *ctx, enum cleanup_scope scope, int pending)
{
    DIR *stream;
    struct dirent *entry;
    int error = 0;
    if (!pending && ctx->parents[scope] < 0) return 0;
    stream = directory_stream(pending ? ctx->trash : ctx->parents[scope]);
    if (stream == NULL) return -1;
    for (;;) {
        const char *name;
        int result;
        if (within_budget() != 0) { error = errno; break; }
        errno = 0;
        entry = readdir(stream);
        if (entry == NULL) { if (errno != 0) error = errno; break; }
        name = entry->d_name;
        if (pending) {
            size_t length = strlen(scope_tags[scope]);
            if (strncmp(name, scope_tags[scope], length) != 0) {
                int known = 0;
                for (int i = 0; i < SCOPE_COUNT; ++i)
                    if (!strncmp(name, scope_tags[i], strlen(scope_tags[i]))) known = 1;
                if (!ctx->deployed || scope != CORE || known) continue;
                /* Retired quarantine formats are also non-desktop debris. */
            } else name += length;
        }
        if (!candidate_name(ctx, scope, name)) continue;
        result = pending ? erase_staged(ctx, entry->d_name, ctx->deployed, scope == RELEASES) : remove_candidate(ctx, scope, name);
        if (result != 0) {
            error = errno;
            fprintf(stderr, "C1 launcher: cleanup retained %s%s: %s\n",
                    scope_tags[scope], name, strerror(error));
        }
    }
    if (closedir(stream) != 0 && error == 0) error = errno;
    return error == 0 ? 0 : reject(error);
}

/* Called with the updater lock held. A release is cleaned only after its
 * health-confirmed state matches the running launcher and current pointer. */
static int confirmed_release(struct cleanup *ctx, uint64_t sequence, const char *name,
                             char token[256])
{
    struct c1_update_state state;
    char root[128], error[C1_UPDATE_ERROR_MAX];
    snprintf(root, sizeof(root), "/proc/self/fd/%d/update/state", ctx->parents[C1_DATA]);
    if (c1_update_state_load(root, &state, error, sizeof(error)) != 0) return reject(EIO);
    if (state.phase != C1_UPDATE_CONFIRMED || state.sequence != sequence ||
        strcmp(name, ctx->current) != 0) return reject(EAGAIN);
    char expected[96];
    snprintf(expected, sizeof(expected), "%llu-%s", (unsigned long long)state.sequence, state.release);
    if (strcmp(name, expected) != 0) return reject(EAGAIN);
    snprintf(token, 256, "%s\n%s\n", expected, state.digest);
    return 0;
}

static int cleanup_completed(struct cleanup *ctx, const char *token)
{
    char content[256];
    struct stat info;
    int fd = openat(ctx->core, CLEANUP_MARKER, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return errno == ENOENT ? 0 : -1;
    ssize_t length = fstat(fd, &info) == 0 && S_ISREG(info.st_mode) ?
        read(fd, content, sizeof(content)) : -1;
    close(fd);
    return length >= 0 && (size_t)length == strlen(token) && !memcmp(content, token, (size_t)length);
}

static int record_completion(struct cleanup *ctx, const char *token)
{
    char temporary[64];
    snprintf(temporary, sizeof(temporary), ".cleanup-%ld.tmp", (long)getpid());
    int fd = openat(ctx->core, temporary, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return -1;
    size_t length = strlen(token);
    int result = write(fd, token, length) == (ssize_t)length && fsync(fd) == 0 ? 0 : -1;
    int saved = errno;
    close(fd);
    if (!result && renameat(ctx->core, temporary, ctx->core, CLEANUP_MARKER) != 0) {
        result = -1; saved = errno;
    }
    if (!result && syncfs(ctx->core) != 0) { result = -1; saved = errno; }
    if (result) (void)unlinkat(ctx->core, temporary, 0);
    else fprintf(stderr, "C1 launcher: post-update partition cleanup completed for %s\n", ctx->current);
    return result == 0 ? 0 : reject(saved);
}

static int cleanup_path(const char *executable, const struct stat *identity, int after_update)
{
    struct cleanup ctx = {.core = -1, .trash = -1, .lock = -1, .parents = {-1, -1, -1, -1, -1}};
    char token[256] = "";
    char name[RELEASE_NAME_MAX], *slash;
    const char suffix[] = "/artifacts/C1ancher-launcher";
    struct stat info, file;
    uint64_t sequence;
    size_t length = strlen(executable);
    int release = -1, artifacts = -1, result = -1, saved, scope;
    if (length >= sizeof(ctx.core_path)) return reject(ENAMETOOLONG);
    if (length < sizeof(suffix) || strcmp(executable + length - sizeof(suffix) + 1U, suffix) != 0) return 0;
    strcpy(ctx.core_path, executable);
    ctx.core_path[length - sizeof(suffix) + 1U] = '\0';
    slash = strrchr(ctx.core_path, '/');
    if (slash == NULL || !parse_release_name(slash + 1, &sequence)) return 0;
    strcpy(name, slash + 1);
    *slash = '\0';
    slash = strrchr(ctx.core_path, '/');
    if (slash == NULL || strcmp(slash + 1, "releases") != 0) return 0;
    *slash = '\0';
    ctx.deployed = strcmp(ctx.core_path, DEPLOYED_CORE) == 0;
    /* Ordinary startup never clears the data partition. The maintenance
     * worker retries after confirmation and the persistent marker prevents
     * another pass on subsequent boots of this same release. */
    if (ctx.deployed != after_update) return 0;
    ctx.core = open_absolute_directory(ctx.core_path, ctx.deployed);
    if (ctx.core < 0 || fstat(ctx.core, &info) != 0) goto done;
    ctx.device = info.st_dev;
    ctx.parents[RELEASES] = open_directory_at(ctx.core, "releases", ctx.device, 0);
    if (ctx.parents[RELEASES] < 0) goto done;
    release = open_release(ctx.parents[RELEASES], name, ctx.device);
    if (release < 0) goto done;
    artifacts = open_directory_at(release, "artifacts", ctx.device, 0);
    if (artifacts < 0) goto done;
    if (fstatat(artifacts, "C1ancher-launcher", &file, AT_SYMLINK_NOFOLLOW) != 0) goto done;
    if (!S_ISREG(file.st_mode) || file.st_uid != geteuid() || file.st_nlink != 1 ||
        (file.st_mode & 0022U) != 0 || !same_inode(&file, identity)) { errno = ESTALE; goto done; }
    if (ctx.deployed) {
        ctx.parents[C1_DATA] = open_directory_at(ctx.core, "..", ctx.device, 1);
        if (ctx.parents[C1_DATA] < 0) goto done;
        ctx.parents[DATA] = open_directory_at(ctx.parents[C1_DATA], "..", ctx.device, 0);
        if (ctx.parents[DATA] < 0 ||
            (REQUIRE_DATA_MOUNT && data_partition_mounted(ctx.parents[DATA]) != 0) ||
            lock_updater(&ctx) != 0) goto done;
        ctx.parents[BIN] = open_directory_at(ctx.parents[C1_DATA], "bin", ctx.device, 1);
        if (ctx.parents[BIN] < 0 && errno != ENOENT) goto done;
        ctx.parents[CORE] = open_directory_at(ctx.core, ".", ctx.device, 0);
        if (ctx.parents[CORE] < 0) goto done;
    }
    if (read_pointer(ctx.core, "current", ctx.current) != 0 ||
        read_pointer(ctx.core, "previous", ctx.previous) != 0 ||
        scan_predecessor(&ctx, sequence) != 0 || anchors_unchanged(&ctx) != 0) goto done;
    if (ctx.deployed) {
        if (confirmed_release(&ctx, sequence, name, token) != 0) goto done;
        int completed = cleanup_completed(&ctx, token);
        if (completed != 0) { result = completed > 0 ? 0 : -1; goto done; }
    }
    if (mkdirat(ctx.core, TRASH_NAME, 0700) != 0 && errno != EEXIST) goto done;
    ctx.trash = open_directory_at(ctx.core, TRASH_NAME, ctx.device, 0);
    if (ctx.trash < 0) goto done;
    result = 0;
    saved = 0;
    for (scope = 0; scope < SCOPE_COUNT; ++scope) {
        if (scan_scope(&ctx, (enum cleanup_scope)scope, 1) != 0) saved = errno;
        if (scan_scope(&ctx, (enum cleanup_scope)scope, 0) != 0) saved = errno;
    }
    if (unlinkat(ctx.core, TRASH_NAME, AT_REMOVEDIR) != 0 && saved == 0) saved = errno;
    if (ctx.removed > 0U) fprintf(stderr, "C1 launcher: cleanup removed %u obsolete entries\n", ctx.removed);
    if (saved != 0) result = reject(saved);
    else if (ctx.deployed) result = record_completion(&ctx, token);
done:
    saved = errno;
    if (artifacts >= 0) close(artifacts);
    if (release >= 0) close(release);
    if (ctx.trash >= 0) close(ctx.trash);
    for (scope = 0; scope < SCOPE_COUNT; ++scope) if (ctx.parents[scope] >= 0) close(ctx.parents[scope]);
    if (ctx.lock >= 0) close(ctx.lock);
    if (ctx.core >= 0) close(ctx.core);
    errno = saved;
    return result;
}

static int cleanup_resolve(const char *executable_path, int after_update)
{
    char resolved[CLEANUP_PATH_MAX];
    struct stat identity;
    struct timespec now;
    ssize_t length;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return -1;
    cleanup_deadline = (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000 + C1_LAUNCHER_CLEANUP_BUDGET_MS;
    if (executable_path == NULL) {
        length = readlink("/proc/self/exe", resolved, sizeof(resolved) - 1U);
        if (length < 0) return -1;
        if ((size_t)length >= sizeof(resolved) - 1U) return reject(ENAMETOOLONG);
        resolved[length] = '\0';
        if (stat("/proc/self/exe", &identity) != 0) return -1;
        executable_path = resolved;
    } else if (lstat(executable_path, &identity) != 0) return -1;
    return cleanup_path(executable_path, &identity, after_update);
}

int c1_launcher_cleanup_once(const char *executable_path)
{
    if (cleanup_done) return 0;
    cleanup_done = 1;
    return cleanup_resolve(executable_path, 0);
}

int c1_launcher_cleanup_after_update(const char *executable_path)
{
    return cleanup_resolve(executable_path, 1);
}
