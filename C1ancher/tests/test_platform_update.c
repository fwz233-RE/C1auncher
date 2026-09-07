#define _DEFAULT_SOURCE 1
#include "platform/app_lease.h"
#include "platform/update_health.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int failures;
static void expect(bool condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

static void test_lease(void)
{
    char root[] = "/tmp/c1-platform-lease-XXXXXX";
    char path[256], target[256];
    int descriptor;
    expect(mkdtemp(root) != NULL, "lease fixture created");
    (void)snprintf(path, sizeof(path), "%s/lease", root);
    (void)snprintf(target, sizeof(target), "%s/target", root);
    descriptor = c1_app_lease_acquire_at(target);
    expect(descriptor >= 0, "normal lease acquired");
    expect(symlink(target, path) == 0 && c1_app_lease_guard_acquire_at(path) < 0,
           "lease rejects symlink instead of trusting its target");
    c1_app_lease_release(descriptor);
    descriptor = c1_app_lease_acquire_at(path);
    expect(descriptor < 0, "exclusive lease rejects an unlocked symlink");
    c1_app_lease_release(descriptor);
    (void)unlink(path);
    expect(link(target, path) == 0, "hardlink lease fixture created");
    descriptor = c1_app_lease_acquire_at(path);
    expect(descriptor < 0, "lease rejects multiple links");
    c1_app_lease_release(descriptor);
    (void)unlink(path);
    expect(mkfifo(path, 0600) == 0, "FIFO lease fixture created");
    descriptor = c1_app_lease_acquire_at(path);
    expect(descriptor < 0, "lease rejects non-regular files without blocking");
    c1_app_lease_release(descriptor);
    (void)unlink(path);
    expect(chmod(target, 0666) == 0, "unsafe lease permissions fixture created");
    descriptor = c1_app_lease_acquire_at(target);
    expect(descriptor < 0, "lease rejects world-writable files");
    c1_app_lease_release(descriptor);
    expect(chmod(target, 0600) == 0, "lease private permissions restored");
    expect(c1_app_lease_active_at(root), "lease lookup error inhibits hardware access");
    descriptor = c1_app_lease_guard_acquire_at(target);
    expect(descriptor >= 0 && (fcntl(descriptor, F_GETFD) & FD_CLOEXEC) != 0,
           "short hardware guard cannot leak through exec");
    c1_app_lease_release(descriptor);
    descriptor = c1_app_lease_acquire_at(target);
    expect(descriptor >= 0 && (fcntl(descriptor, F_GETFD) & FD_CLOEXEC) == 0,
           "external app lease intentionally survives exec");
    c1_app_lease_release(descriptor);
    (void)unlink(target);
    (void)rmdir(root);
}

static void test_health_descendants(bool timeout)
{
    char root[] = "/tmp/c1-platform-health-XXXXXX";
    char script[256], pid_path[256];
    FILE *stream;
    struct c1_update_health_config config;
    pid_t descendant = -1;
    long value = -1;
    unsigned int attempt;
    bool reaped = false;

    expect(mkdtemp(root) != NULL, "health fixture created");
    (void)snprintf(script, sizeof(script), "%s/probe", root);
    (void)snprintf(pid_path, sizeof(pid_path), "%s/child", root);
    stream = fopen(script, "w");
    expect(stream != NULL, "health script created");
    if (stream == NULL) return;
    fprintf(stream, "#!/bin/sh\nsleep 30 &\necho $! > '%s'\n%s\n",
            pid_path, timeout ? "wait" : "exit 1");
    (void)fclose(stream);
    expect(chmod(script, 0700) == 0, "health script executable");
    config.pkg_path = script;
    config.updater_path = "/bin/true";
    config.state_root = root;
    config.ready_file = pid_path;
    config.timeout_ms = 200U;
    expect(c1_update_health_probe(&config) != 0, "bad health probe fails");
    stream = fopen(pid_path, "r");
    if (stream != NULL) {
        if (fscanf(stream, "%ld", &value) == 1 && value > 0) descendant = (pid_t)value;
        (void)fclose(stream);
    }
    expect(descendant > 0, "health probe descendant was started");
    for (attempt = 0; descendant > 0 && attempt < 100U; ++attempt) {
        struct timespec delay = {0, 10000000L};
        int status;
        if (waitpid(descendant, &status, WNOHANG) == descendant) {
            reaped = true;
            break;
        }
        (void)nanosleep(&delay, NULL);
    }
    expect(reaped, timeout ? "timed-out health probe kills its descendants" :
                            "failed health probe kills its descendants");
    if (descendant > 0 && !reaped) {
        (void)kill(descendant, SIGKILL);
        while (waitpid(descendant, NULL, 0) < 0 && errno == EINTR) {}
    }
    (void)unlink(pid_path);
    (void)unlink(script);
    (void)rmdir(root);
}

int main(void)
{
    expect(prctl(PR_SET_CHILD_SUBREAPER, 1) == 0, "test adopts orphan probe children");
    test_lease();
    test_health_descendants(false);
    test_health_descendants(true);
    if (failures) return EXIT_FAILURE;
    puts("all platform update tests passed");
    return EXIT_SUCCESS;
}
