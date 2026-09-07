#include "pkg.h"
#include "storage_layout.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/statvfs.h>

/* Compile a second, isolated copy with only statvfs substituted. The normal
 * implementation remains linked and is exercised by the mount/layout tests. */
static uint64_t injected_available = UINT64_MAX;
static int injected_statvfs(const char *path, struct statvfs *info)
{
    if (injected_available == UINT64_MAX) return statvfs(path, info);
    (void)memset(info, 0, sizeof(*info));
    info->f_frsize = 1U;
    info->f_bavail = (fsblkcnt_t)injected_available;
    return 0;
}
#define statvfs(path, info) injected_statvfs(path, info)
#define c1pkg_storage_default_layout isolated_default_layout
#define c1pkg_storage_state_init_at isolated_state_init_at
#define c1pkg_storage_prepare_at isolated_prepare_at
#define c1pkg_storage_state_init isolated_state_init
#define c1pkg_storage_prepare isolated_prepare
#include "../src/pkg/storage_layout.c"
#undef statvfs
#undef c1pkg_storage_default_layout
#undef c1pkg_storage_state_init_at
#undef c1pkg_storage_prepare_at
#undef c1pkg_storage_state_init
#undef c1pkg_storage_prepare

static int failures;

static void expect(bool condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

static void join_or_exit(char *out, size_t out_size,
                         const char *left, const char *right)
{
    if (c1pkg_join(out, out_size, left, right) != 0) {
        fputs("test path is too long\n", stderr);
        exit(EXIT_FAILURE);
    }
}

static bool is_directory(const char *path)
{
    struct stat information;

    return lstat(path, &information) == 0 && S_ISDIR(information.st_mode) &&
           !S_ISLNK(information.st_mode);
}

static void test_storage_layout(void)
{
    char temporary[] = "/tmp/c1pkg-storage-XXXXXX";
    char storage[C1PKG_PATH_MAX];
    char apps[C1PKG_PATH_MAX];
    char work[C1PKG_PATH_MAX];
    char staging[C1PKG_PATH_MAX];
    char trash[C1PKG_PATH_MAX];
    char state[C1PKG_PATH_MAX];
    char cache[C1PKG_PATH_MAX];
    char error[C1PKG_ERROR_MAX] = "";
    struct c1pkg_storage_layout layout;

    expect(mkdtemp(temporary) != NULL, "temporary storage root is created");
    if (temporary[0] == '\0') {
        return;
    }
    join_or_exit(storage, sizeof(storage), temporary, "storage");
    join_or_exit(apps, sizeof(apps), storage, "apps");
    join_or_exit(work, sizeof(work), storage, "pkg");
    join_or_exit(staging, sizeof(staging), work, "staging");
    join_or_exit(trash, sizeof(trash), work, "trash");
    join_or_exit(state, sizeof(state), temporary, "state");
    join_or_exit(cache, sizeof(cache), state, "cache");

    layout.system_root = "/";
    layout.mount_root = temporary;
    layout.storage_root = storage;
    layout.apps_root = apps;
    layout.work_root = work;
    layout.staging_root = staging;
    layout.trash_root = trash;
    layout.state_root = state;
    layout.require_distinct_mount = 0;

    expect(c1pkg_storage_state_init_at(&layout, error, sizeof(error)) == 0,
           "small persistent state initializes independently");
    expect(is_directory(state) && is_directory(cache),
           "state initialization creates only state and cache directories");
    expect(access(storage, F_OK) != 0,
           "state initialization does not create application storage");

    error[0] = '\0';
    expect(c1pkg_storage_prepare_at(&layout, C1PKG_STORAGE_READ, 0U,
                                    error, sizeof(error)) == 0 && error[0] == '\0',
           "first-use missing storage directories are an empty library");
    expect(access(storage, F_OK) != 0,
           "first-use browsing is read-only and creates no app directories");
    expect(mkdir(storage, 0755) == 0, "storage parent fixture is created");
    expect(c1pkg_storage_prepare_at(&layout, C1PKG_STORAGE_READ, 0U,
                                    error, sizeof(error)) == 0 && access(apps, F_OK) != 0,
           "missing apps directory with existing parent is also valid");
    error[0] = '\0';
    expect(c1pkg_storage_prepare_at(&layout, C1PKG_STORAGE_WRITE, 0U,
                                    error, sizeof(error)) == 0,
           "writable application storage initializes");
    expect(is_directory(apps) && is_directory(staging) && is_directory(trash),
           "application, staging, and trash directories are created together");

    error[0] = '\0';
    expect(c1pkg_storage_prepare_at(&layout, C1PKG_STORAGE_READ, 0U,
                                    error, sizeof(error)) == 0,
           "initialized application storage is readable");

    error[0] = '\0';
    expect(c1pkg_storage_prepare_at(&layout, C1PKG_STORAGE_WRITE, UINT64_MAX,
                                    error, sizeof(error)) != 0 &&
           strstr(error, "insufficient storage space") != NULL,
           "impossible capacity request fails before a transaction");

    error[0] = '\0';
    expect(c1pkg_storage_prepare_at(&layout, C1PKG_STORAGE_MAINTENANCE, UINT64_MAX,
                                    error, sizeof(error)) == 0,
           "maintenance bypasses free-space reserve and allocation probe");
    injected_available = 4U * 1024U * 1024U - 1U;
    expect(isolated_prepare_at(&layout, C1PKG_STORAGE_WRITE, 0U, error, sizeof(error)) != 0,
           "install rejects even one byte below the 4 MiB reserve");
    expect(isolated_prepare_at(&layout, C1PKG_STORAGE_MAINTENANCE, 0U, error, sizeof(error)) == 0,
           "cleanup/uninstall remain permitted below the install reserve");
    injected_available = 0U;
    expect(isolated_prepare_at(&layout, C1PKG_STORAGE_MAINTENANCE, 0U, error, sizeof(error)) == 0,
           "maintenance does not need an allocation probe even at zero available bytes");
    injected_available = 4U * 1024U * 1024U;
    expect(isolated_prepare_at(&layout, C1PKG_STORAGE_WRITE, 0U, error, sizeof(error)) == 0 &&
           isolated_prepare_at(&layout, C1PKG_STORAGE_WRITE, 1U, error, sizeof(error)) != 0,
           "exact reserve boundary preserves additional installation headroom");
    injected_available = UINT64_MAX;

    layout.require_distinct_mount = 1;
    error[0] = '\0';
    expect(c1pkg_storage_prepare_at(&layout, C1PKG_STORAGE_READ, 0U,
                                    error, sizeof(error)) != 0 &&
           strstr(error, "not mounted on a separate filesystem") != NULL,
           "production mount policy rejects the system filesystem");
    layout.require_distinct_mount = 0;

    expect(rmdir(apps) == 0 && symlink(temporary, apps) == 0,
           "untrusted application symlink fixture is created");
    error[0] = '\0';
    expect(c1pkg_storage_prepare_at(&layout, C1PKG_STORAGE_READ, 0U,
                                    error, sizeof(error)) != 0 &&
           strstr(error, "not a trusted directory") != NULL,
           "application storage symlinks are rejected");

    expect(c1pkg_remove_tree(temporary, error, sizeof(error)) == 0,
           "temporary storage tree is removed");
}

int main(void)
{
    test_storage_layout();
    if (failures != 0) {
        fprintf(stderr, "%d storage test(s) failed\n", failures);
        return EXIT_FAILURE;
    }
    puts("all package storage tests passed");
    return EXIT_SUCCESS;
}