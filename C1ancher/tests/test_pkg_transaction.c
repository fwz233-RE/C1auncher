/* Isolated filesystem/fault tests. Helpers and leases are substituted; no device,
 * network, production state, or signing key is accessed. */
#define C1PKG_STATE_ROOT "/tmp/c1pkg-store-lifecycle-unit"
#define C1PKG_APPS_ROOT C1PKG_STATE_ROOT "/apps"
#define C1PKG_STAGING_ROOT C1PKG_STATE_ROOT "/staging"
#define C1PKG_TRASH_ROOT C1PKG_STATE_ROOT "/trash"
#define c1pkg_sync_directory test_sync_directory
#define rename test_rename
#define execv test_execv
#define c1_app_run_acquire test_run_acquire
#define c1_app_run_active test_run_active
#define c1_app_lease_acquire test_lease_acquire
#define c1_app_lease_release test_lease_release
#define c1_app_lease_write_mode test_write_mode
#define c1_app_lease_clear_mode test_clear_mode
#include "../src/pkg/store.c"
#undef c1pkg_sync_directory
#undef rename
#undef execv
#include <sys/wait.h>

int c1pkg_sync_directory(const char *, char *, size_t);
int rename(const char *, const char *);
static int failures, sync_calls, fail_sync, crash_sync, fail_activate, crash_commit;
static int fetch_calls, fetch_fails, verify_fails, verify_calls, low_space;
static int run_busy, run_fd = -1, lease_fd = -1, lease_calls, exec_calls;
static char published_mode[16], observed_mode[16];
static int observed_run, observed_lease;
static char archive_fixture[C1PKG_PATH_MAX];
static const char *app = C1PKG_APPS_ROOT "/app";

static void expect(int condition, const char *message)
{
    if (!condition) { fprintf(stderr, "FAIL: %s (errno=%d %s)\n", message, errno, strerror(errno)); ++failures; }
}

int test_sync_directory(const char *path, char *error, size_t error_size)
{
    ++sync_calls;
    if (sync_calls == crash_sync) _exit(77);
    if (sync_calls == fail_sync) {
        errno = EIO;
        c1pkg_set_error(error, error_size, "injected directory sync failure");
        return -1;
    }
    return c1pkg_sync_directory(path, error, error_size);
}

int test_rename(const char *old_path, const char *new_path)
{
    if (fail_activate && strcmp(new_path, C1PKG_APPS_ROOT "/app/current") == 0) {
        errno = EIO;
        return -1;
    }
    if (crash_commit && strcmp(new_path, C1PKG_APPS_ROOT "/app/versions/8.0.0") == 0) {
        if (rename(old_path, new_path) != 0) _exit(78);
        _exit(77);
    }
    return rename(old_path, new_path);
}

int test_run_acquire(void)
{
    if (run_busy || run_fd >= 0) { errno = EWOULDBLOCK; return -1; }
    run_fd = open("/dev/null", O_RDONLY);
    return run_fd;
}

bool test_run_active(void) { return run_busy || run_fd >= 0; }
int test_lease_acquire(void) { ++lease_calls; lease_fd = open("/dev/null", O_RDONLY); return lease_fd; }
void test_lease_release(int fd)
{
    if (fd < 0) return;
    (void)close(fd);
    if (fd == run_fd) run_fd = -1;
    if (fd == lease_fd) lease_fd = -1;
}
bool test_write_mode(const char *mode) { strcpy(published_mode, mode); return true; }
void test_clear_mode(void) { published_mode[0] = '\0'; }
int test_execv(const char *file, char *const argv[])
{
    (void)file; (void)argv;
    ++exec_calls;
    strcpy(observed_mode, published_mode);
    observed_run = run_fd >= 0 && (fcntl(run_fd, F_GETFD) & FD_CLOEXEC) == 0;
    observed_lease = lease_fd >= 0;
    errno = ENOEXEC;
    return -1;
}

int c1pkg_storage_state_init(char *error, size_t error_size)
{
    return c1pkg_mkdir_p(C1PKG_STATE_ROOT, 0700, error, error_size);
}
int c1pkg_storage_prepare(enum c1pkg_storage_access mode, uint64_t bytes,
                          char *error, size_t error_size)
{
    (void)bytes;
    if (mode == C1PKG_STORAGE_WRITE && low_space) {
        errno = ENOSPC;
        c1pkg_set_error(error, error_size, "injected full storage");
        return -1;
    }
    return 0;
}
int c1pkg_fetch(const char *url, const char *output, uint64_t limit, char *error, size_t error_size)
{
    unsigned char *data = NULL;
    size_t size = 0U;
    int result;
    (void)url;
    ++fetch_calls;
    if (fetch_fails) { errno = 0; c1pkg_set_error(error, error_size, "download failed"); return -1; }
    if (c1pkg_read_file(archive_fixture, &data, &size, (size_t)limit, error, error_size) != 0) return -1;
    result = c1pkg_write_file(output, data, size, 0600, error, error_size);
    free(data);
    return result;
}
int c1pkg_verify_sha256(const char *path, const char *digest, char *error, size_t error_size)
{
    (void)path; (void)digest;
    ++verify_calls;
    if (verify_fails) { errno = 0; c1pkg_set_error(error, error_size, "digest mismatch"); return -1; }
    return 0;
}

static void point(const char *name, const char *target)
{
    char path[C1PKG_PATH_MAX];
    expect(c1pkg_join(path, sizeof(path), app, name) == 0, "pointer path");
    (void)unlink(path);
    expect(symlink(target, path) == 0, "create version pointer");
}
static int points_to(const char *name, const char *target)
{
    char path[C1PKG_PATH_MAX], value[C1PKG_PATH_MAX];
    return c1pkg_join(path, sizeof(path), app, name) == 0 &&
           read_link_value(path, value, sizeof(value)) == 0 && strcmp(value, target) == 0;
}
static void reset(void)
{
    (void)unlink(TRANSACTION_PATH); (void)unlink(TRANSACTION_TEMP);
    expect(c1pkg_mkdir_p(C1PKG_APPS_ROOT "/app/versions", 0755, NULL, 0U) == 0 &&
           c1pkg_mkdir_p(C1PKG_APPS_ROOT "/app/versions/1.0.0", 0555, NULL, 0U) == 0 &&
           c1pkg_mkdir_p(C1PKG_APPS_ROOT "/app/versions/2.0.0", 0555, NULL, 0U) == 0 &&
           c1pkg_mkdir_p(C1PKG_APPS_ROOT "/app/versions/3.0.0", 0555, NULL, 0U) == 0,
           "create sealed version directories");
    point("current", "versions/2.0.0"); point("previous", "versions/1.0.0");
    sync_calls = 0; fail_sync = 0; crash_sync = 0; fail_activate = 0;
    fetch_calls = 0; fetch_fails = 0; verify_fails = 0; verify_calls = 0; low_space = 0; run_busy = 0;
    errno = 0;
}
static void test_pointers(void)
{
    char error[256] = "";
    int boundary;
    for (boundary = 1; boundary <= 8; ++boundary) {
        pid_t child;
        int status;
        reset();
        child = fork();
        if (child == 0) { crash_sync = boundary; (void)switch_current(app, "3.0.0", error, sizeof(error)); _exit(0); }
        expect(child > 0 && waitpid(child, &status, 0) == child && WIFEXITED(status), "crash child exits");
        expect(recover_transaction(error, sizeof(error)) == 0 &&
               points_to("current", "versions/3.0.0") && points_to("previous", "versions/2.0.0"),
               "restart completes every WAL/pointer fsync boundary idempotently");
        expect(recover_transaction(error, sizeof(error)) == 0 && points_to("previous", "versions/2.0.0"),
               "second restart does not overwrite rollback pointer");
    }
    reset(); fail_activate = 1;
    expect(switch_current(app, "3.0.0", error, sizeof(error)) != 0 &&
           points_to("current", "versions/2.0.0") && points_to("previous", "versions/2.0.0"),
           "activation failure leaves recoverable WAL rather than losing the old current");
    fail_activate = 0;
    expect(recover_transaction(error, sizeof(error)) == 0 && points_to("current", "versions/3.0.0") &&
           points_to("previous", "versions/2.0.0"), "partial dual-pointer switch is repaired");
    reset();
    expect(c1pkg_store_rollback("app", error, sizeof(error)) == 0 && points_to("current", "versions/1.0.0"),
           "explicit rollback swaps versions");
    expect(c1pkg_store_rollback("app", error, sizeof(error)) == 0 && points_to("current", "versions/2.0.0"),
           "rollback can be reversed");
    point("previous", "versions/9.0.0");
    expect(c1pkg_store_rollback("app", error, sizeof(error)) != 0 && points_to("current", "versions/2.0.0"),
           "missing rollback never replaces current");
}
static void test_payload_commit_recovery(void)
{
    char error[256] = "";
    const char *payload = C1PKG_STAGING_ROOT "/verified";
    const char *destination = C1PKG_APPS_ROOT "/app/versions/4.0.0";
    reset();
    expect(c1pkg_mkdir_p(payload, 0755, error, sizeof(error)) == 0 &&
           begin_transaction(app, "4.0.0", payload, 1, error, sizeof(error)) == 0,
           "verified payload journal is persisted before commit");
    expect(recover_transaction(error, sizeof(error)) == 0 && points_to("current", "versions/2.0.0"),
           "interruption before payload rename never activates an absent directory");
    expect(begin_transaction(app, "4.0.0", payload, 1, error, sizeof(error)) == 0 &&
           rename(payload, destination) == 0, "simulate payload committed before current");
    expect(recover_transaction(error, sizeof(error)) == 0 && points_to("current", "versions/4.0.0") &&
           points_to("previous", "versions/2.0.0"), "committed payload is recovered without permanent retained failure");
    reset();
    expect(begin_transaction(app, "3.0.0", C1PKG_APPS_ROOT "/app/versions/3.0.0", 0, error, sizeof(error)) == 0,
           "create transaction for trust test");
    expect(chmod(C1PKG_APPS_ROOT "/app/versions/3.0.0", 0755) == 0 &&
           rename(C1PKG_APPS_ROOT "/app/versions/3.0.0", C1PKG_STAGING_ROOT "/saved-inode") == 0 &&
           mkdir(C1PKG_APPS_ROOT "/app/versions/3.0.0", 0555) == 0, "substitute a different target inode");
    expect(recover_transaction(error, sizeof(error)) != 0 && points_to("current", "versions/2.0.0"),
           "recovery rejects a substituted directory even with the same version name");
    (void)unlink(TRANSACTION_PATH);
    expect(c1pkg_write_file(TRANSACTION_PATH, "forged", 6U, 0600, NULL, 0U) == 0 &&
           recover_transaction(error, sizeof(error)) != 0, "malformed WAL blocks rather than activates");
    (void)unlink(TRANSACTION_PATH);
    expect(symlink(C1PKG_APPS_ROOT "/app/current", TRANSACTION_PATH) == 0 &&
           recover_transaction(error, sizeof(error)) != 0, "WAL symlink is rejected");
    (void)unlink(TRANSACTION_PATH);
}
static void make_archive(struct c1pkg_package *package, const char *mode, int reserved)
{
    const char *fixture = C1PKG_STATE_ROOT "/fixture";
    char manifest[512], error[256] = "";
    struct stat info;
    char *args[] = {"tar", "-czf", archive_fixture, "-C", (char *)fixture, "manifest.v1", "payload", NULL};
    (void)c1pkg_remove_tree(fixture, NULL, 0U);
    expect(c1pkg_mkdir_p(C1PKG_STATE_ROOT "/fixture/payload/bin", 0755, NULL, 0U) == 0,
           "create archive fixture");
    (void)snprintf(manifest, sizeof(manifest), "C1PKG-PACKAGE %d\nid\t%s\nversion\t%s\nentry\t%s\n%s%s%s",
                   mode == NULL ? 1 : 2, package->id, package->version, package->entry,
                   mode == NULL ? "" : "mode\t", mode == NULL ? "" : mode, mode == NULL ? "" : "\n");
    expect(c1pkg_write_file(C1PKG_STATE_ROOT "/fixture/manifest.v1", manifest, strlen(manifest), 0644, NULL, 0U) == 0 &&
           c1pkg_write_file(C1PKG_STATE_ROOT "/fixture/payload/bin/app", "#!/bin/sh\nexit 0\n", 17U, 0755, NULL, 0U) == 0,
           "write package fixture");
    if (reserved) expect(c1pkg_write_file(C1PKG_STATE_ROOT "/fixture/payload/.c1pkg-mode", "direct", 6U, 0644, NULL, 0U) == 0,
                         "create reserved mode attack fixture");
    expect(c1pkg_run(args, NULL, 0U, error, sizeof(error)) == 0 && stat(archive_fixture, &info) == 0,
           "build local tar fixture");
    package->size = (uint64_t)info.st_size;
}
static int cancel_operation(const char *message, void *context)
{
    (void)message; (void)context;
    return 1;
}

static void test_install_and_modes(void)
{
    struct c1pkg_config config = {"http://unused.invalid/", "/unused/key"};
    struct c1pkg_package package = {0};
    char error[256] = "";
    int original_cwd = open(".", O_RDONLY | O_CLOEXEC);
    reset(); strcpy(package.id, "app"); strcpy(package.version, "2.0.0");
    strcpy(package.archive, "app.tar.gz"); strcpy(package.entry, "bin/app"); package.size = 100U;
    expect(c1pkg_store_install(&config, &package, error, sizeof(error)) == C1PKG_INSTALL_SKIPPED && fetch_calls == 0,
           "same version is a no-op");
    strcpy(package.version, "1.0.0");
    expect(c1pkg_store_install(&config, &package, error, sizeof(error)) == C1PKG_INSTALL_PACKAGE_ERROR && fetch_calls == 0,
           "downgrade is rejected before download");
    strcpy(package.version, "rolling");
    expect(c1pkg_store_install(&config, &package, error, sizeof(error)) == C1PKG_INSTALL_PACKAGE_ERROR && fetch_calls == 0,
           "unordered downgrade remains rejected");
    strcpy(package.version, "5.0.0"); make_archive(&package, "terminal", 0);
    c1pkg_set_progress(cancel_operation, NULL);
    expect(c1pkg_store_install(&config, &package, error, sizeof(error)) == C1PKG_INSTALL_CANCELLED &&
           points_to("current", "versions/2.0.0"), "cancellation has a typed result and leaves current unchanged");
    c1pkg_set_progress(NULL, NULL);
    verify_fails = 1; error[0] = '\0';
    expect(c1pkg_store_install(&config, &package, error, sizeof(error)) == C1PKG_INSTALL_PACKAGE_ERROR &&
           points_to("current", "versions/2.0.0") && access(TRANSACTION_PATH, F_OK) != 0,
           "failed digest never creates an activation journal");
    verify_fails = 0; error[0] = '\0';
    expect(c1pkg_store_install(&config, &package, error, sizeof(error)) == 0 &&
           points_to("current", "versions/5.0.0"), "v2 terminal package installs");
    if (!points_to("current", "versions/5.0.0")) fprintf(stderr, "install diagnostic: %s\n", error);
    {
        struct stat mode_info;
        expect(lstat(C1PKG_APPS_ROOT "/app/versions/5.0.0/.c1pkg-mode", &mode_info) == 0 &&
               (mode_info.st_mode & 0777) == 0444, "mode metadata is sealed read-only");
    }
    lease_calls = 0;
    expect(c1pkg_store_launch("app", NULL, error, sizeof(error)) != 0 && observed_run && !observed_lease &&
           strcmp(observed_mode, "terminal") == 0 && lease_calls == 0 && run_fd == -1 && lease_fd == -1 &&
           published_mode[0] == '\0', "terminal launch inherits only run lock; exec failure releases all state");
    expect(fchdir(original_cwd) == 0, "restore launch working directory");
    expect(c1pkg_store_rollback("app", error, sizeof(error)) == 0 && points_to("current", "versions/2.0.0"),
           "roll back verified upgrade");
    {
        const char *metadata = C1PKG_APPS_ROOT "/app/versions/5.0.0/.c1pkg-mode";
        expect(chmod(metadata, 0644) == 0 && c1pkg_write_file(metadata, "direct", 6U, 0444, NULL, 0U) == 0 &&
               chmod(metadata, 0444) == 0, "alter retained mode without changing its version name");
        error[0] = '\0';
        expect(c1pkg_store_install(&config, &package, error, sizeof(error)) == C1PKG_INSTALL_SKIPPED &&
               points_to("current", "versions/2.0.0"), "mismatching retained payload is safely skipped, never reactivated");
        expect(chmod(metadata, 0644) == 0 && c1pkg_write_file(metadata, "terminal", 8U, 0444, NULL, 0U) == 0 &&
               chmod(metadata, 0444) == 0, "restore retained verified mode");
    }
    error[0] = '\0';
    expect(c1pkg_store_install(&config, &package, error, sizeof(error)) == 0 && verify_calls >= 3 &&
           points_to("current", "versions/5.0.0"), "retained newer version upgrades after fresh digest and full payload comparison");
    strcpy(package.version, "6.0.0"); make_archive(&package, "direct", 1); error[0] = '\0';
    expect(c1pkg_store_install(&config, &package, error, sizeof(error)) == C1PKG_INSTALL_PACKAGE_ERROR &&
           points_to("current", "versions/5.0.0"), "payload cannot supply reserved mode metadata");
    make_archive(&package, "unknown", 0); error[0] = '\0';
    expect(c1pkg_store_install(&config, &package, error, sizeof(error)) == C1PKG_INSTALL_PACKAGE_ERROR,
           "v2 manifest rejects an unknown mode even after digest verification");
    make_archive(&package, "direct", 0); error[0] = '\0';
    expect(c1pkg_store_install(&config, &package, error, sizeof(error)) == 0, "v2 direct package installs");
    expect(c1pkg_store_launch("app", NULL, error, sizeof(error)) != 0 && observed_run && observed_lease &&
           strcmp(observed_mode, "direct") == 0 && run_fd == -1 && lease_fd == -1,
           "direct launch inherits run and hardware locks and releases both on exec failure");
    expect(fchdir(original_cwd) == 0, "restore direct launch working directory");
    {
        const char *metadata = C1PKG_APPS_ROOT "/app/versions/6.0.0/.c1pkg-mode";
        int before = exec_calls;
        expect(chmod(metadata, 0644) == 0 && c1pkg_write_file(metadata, "direct\0x", 8U, 0444, NULL, 0U) == 0 &&
               chmod(metadata, 0444) == 0, "malformed mode fixture");
        expect(c1pkg_store_launch("app", NULL, error, sizeof(error)) != 0 && exec_calls == before && run_fd == -1,
               "bounded mode reader rejects embedded NUL and releases run lock");
    }
    strcpy(package.version, "7.0.0"); make_archive(&package, NULL, 0); error[0] = '\0';
    expect(c1pkg_store_install(&config, &package, error, sizeof(error)) == 0 &&
           access(C1PKG_APPS_ROOT "/app/versions/7.0.0/.c1pkg-mode", F_OK) != 0, "v1 remains byte-compatible with no mode metadata");
    expect(c1pkg_store_launch("app", NULL, error, sizeof(error)) != 0 &&
           strcmp(observed_mode, "terminal") == 0 && !observed_lease, "v1 uses legacy whitelist fallback");
    expect(fchdir(original_cwd) == 0, "restore legacy launch working directory");
    strcpy(package.version, "8.0.0"); make_archive(&package, "terminal", 0);
    {
        pid_t child = fork();
        int status;
        struct c1pkg_installed_list list;
        if (child == 0) {
            crash_commit = 1;
            (void)c1pkg_store_install(&config, &package, error, sizeof(error));
            _exit(79);
        }
        expect(child > 0 && waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 77,
               "interrupt real installation just after payload commit");
        expect(c1pkg_store_list(&list, error, sizeof(error)) == 0 &&
               points_to("current", "versions/8.0.0") && points_to("previous", "versions/7.0.0"),
               "first reader recovers real interrupted installation before exposing pointer state");
        fetch_calls = 0;
        expect(c1pkg_store_install(&config, &package, error, sizeof(error)) == C1PKG_INSTALL_SKIPPED && fetch_calls == 0,
               "retry after crash succeeds rather than failing on retained payload");
    }
    close(original_cwd);
}
static void test_space_and_gc(void)
{
    char error[256] = "";
    struct c1pkg_config config = {"http://unused.invalid/", "/unused/key"};
    struct c1pkg_package package = {0};
    reset();
    expect(c1pkg_write_file(C1PKG_APPS_ROOT "/app/user-data", "saved", 5U, 0600, NULL, 0U) == 0,
           "user data fixture");
    run_busy = 1; cleanup_abandoned_work();
    expect(access(C1PKG_APPS_ROOT "/app/versions/3.0.0", F_OK) == 0,
           "GC retains every version while any app runs");
    run_busy = 0; cleanup_abandoned_work();
    expect(access(C1PKG_APPS_ROOT "/app/versions/3.0.0", F_OK) != 0 &&
           access(C1PKG_APPS_ROOT "/app/versions/2.0.0", F_OK) == 0 &&
           access(C1PKG_APPS_ROOT "/app/versions/1.0.0", F_OK) == 0 &&
           access(C1PKG_APPS_ROOT "/app/user-data", F_OK) == 0,
           "idle GC keeps current, previous and user data, removes only history");
    strcpy(package.id, "app"); strcpy(package.version, "8.0.0");
    strcpy(package.archive, "app.tar.gz"); strcpy(package.entry, "bin/app"); package.size = 100U;
    low_space = 1;
    expect(c1pkg_store_install(&config, &package, error, sizeof(error)) == C1PKG_INSTALL_STORAGE_ERROR && fetch_calls == 0,
           "installation keeps its storage reserve and returns typed global failure");
    run_busy = 1;
    expect(c1pkg_store_remove("app", error, sizeof(error)) != 0, "uninstall cannot remove a running version");
    run_busy = 0;
    expect(c1pkg_store_remove("app", error, sizeof(error)) == 0 && access(app, F_OK) != 0,
           "low-space uninstall is permitted without the installation reserve");
    reset();
    {
        int manager = test_run_acquire();
        expect(manager >= 0 && c1pkg_store_remove_with_run_lock("app", manager, error, sizeof(error)) == 0,
               "graphical manager can uninstall while retaining its run mutex");
        expect(fcntl(manager, F_GETFD) >= 0 && run_fd == manager,
               "uninstall closes only borrowed duplicate, not manager run mutex");
        test_lease_release(manager);
    }
}
int main(void)
{
    if (mkdir(C1PKG_STATE_ROOT, 0700) != 0) { perror("isolated transaction directory (refuse existing)"); return 1; }
    expect(c1pkg_mkdir_p(C1PKG_STAGING_ROOT, 0700, NULL, 0U) == 0 &&
           c1pkg_mkdir_p(C1PKG_TRASH_ROOT, 0700, NULL, 0U) == 0, "create isolated work directories");
    {
        struct c1pkg_installed_list empty;
        char error[256] = "";
        expect(c1pkg_store_list(&empty, error, sizeof(error)) == 0 && empty.count == 0U && error[0] == '\0',
               "first store listing succeeds with missing apps directory");
        expect(access(C1PKG_APPS_ROOT, F_OK) != 0, "listing does not create apps directory");
    }
    strcpy(archive_fixture, C1PKG_STATE_ROOT "/fixture.tar.gz");
    test_pointers(); test_payload_commit_recovery(); test_install_and_modes(); test_space_and_gc();
    expect(remove_package_tree(C1PKG_STATE_ROOT, NULL, 0U) == 0, "clean isolated store");
    if (failures != 0) { fprintf(stderr, "%d transaction test(s) failed\n", failures); return 1; }
    puts("All package transaction tests passed.");
    return 0;
}
