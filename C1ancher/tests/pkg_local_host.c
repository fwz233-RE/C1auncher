/* Host-only adapters for test_pkg_local.py. Real CLI, index verification,
 * SHA-256, tar extraction, storage checks and transactions; no device or network.
 * C1PKG_TEST_ROOT and all package/lease paths are generated temporary paths. */
#include "pkg.h"
#include "gui.h"
#include "platform/power_config.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef C1PKG_TEST_ROOT
#error "this host adapter requires an isolated temporary root"
#endif

#define c1pkg_storage_prepare host_default_storage_prepare
#define c1pkg_storage_state_init host_default_storage_state_init
#include "../src/pkg/storage_layout.c"
#undef c1pkg_storage_prepare
#undef c1pkg_storage_state_init

static const struct c1pkg_storage_layout host_layout = {
    "/", C1PKG_TEST_ROOT "/storage", C1PKG_TEST_ROOT "/storage/c1",
    C1PKG_APPS_ROOT, C1PKG_TEST_ROOT "/storage/c1/pkg", C1PKG_STAGING_ROOT,
    C1PKG_TRASH_ROOT, C1PKG_STATE_ROOT, 0
};
int c1pkg_storage_state_init(char *error, size_t size)
{
    return c1pkg_storage_state_init_at(&host_layout, error, size);
}
int c1pkg_storage_prepare(enum c1pkg_storage_access access, uint64_t bytes,
                          char *error, size_t size)
{
    return c1pkg_storage_prepare_at(&host_layout, access, bytes, error, size);
}

static int host_rename(const char *from, const char *to);
static int host_verify_sha256(const char *path, const char *hash, char *error, size_t size);
#define rename host_rename
#define c1pkg_verify_sha256 host_verify_sha256
#include "../src/pkg/store.c"
#undef rename
#undef c1pkg_verify_sha256

static int host_rename(const char *from, const char *to)
{
    const char *version = getenv("C1PKG_TEST_CRASH_VERSION");
    int result = rename(from, to);
    const char *suffix = strstr(to, "/versions/");
    if (result == 0 && suffix != NULL && version != NULL && strcmp(suffix + 10, version) == 0)
        _exit(77); /* After payload rename, before publishing current. */
    return result;
}

static int host_verify_sha256(const char *path, const char *hash, char *error, size_t size)
{
    const char *source = getenv("C1PKG_TEST_MUTATE_SOURCE");
    const char *staging = getenv("C1PKG_TEST_TAMPER_STAGING");
    if (source != NULL && c1pkg_write_file(source, "replaced source", 15U, 0600, error, size) != 0)
        return -1;
    if (staging != NULL && c1pkg_write_file(path, "modified staged copy", 20U, 0600, error, size) != 0)
        return -1;
    return c1pkg_verify_sha256(path, hash, error, size);
}

int c1pkg_desktop_summary(const struct c1pkg_config *config) { (void)config; return 99; }
int c1pkg_gui(const struct c1pkg_config *config) { (void)config; return 99; }
int c1pkg_tui(const struct c1pkg_config *config) { (void)config; return 99; }
bool c1_power_config_auto_suspend_enabled(void) { return false; }
int c1_power_config_set_auto_suspend(bool enabled, char *error, size_t size)
{ (void)enabled; (void)error; (void)size; return -1; }

#define main host_cli_main
#include "../src/pkg/main.c"
#undef main

static int change_during_copy(const char *message, void *context)
{
    const char *change = getenv("C1PKG_TEST_SOURCE_CHANGE");
    int fd, result;
    (void)context;
    if (change == NULL || message == NULL || strcmp(message, "Copying local package") != 0) return 0;
    if (strcmp(change, "cancel") == 0) return 1;
    fd = open(C1PKG_TEST_ROOT "/bundle/packages/c1-ime.tar.gz", O_WRONLY | O_APPEND);
    if (fd < 0) _exit(98);
    result = strcmp(change, "grow") == 0 ? (write(fd, "x", 1U) == 1 ? 0 : -1) : ftruncate(fd, 0);
    if (close(fd) != 0 || result != 0) _exit(98);
    return 0;
}

int main(int argc, char **argv)
{
    c1pkg_set_progress(change_during_copy, NULL);
    return host_cli_main(argc, argv);
}
