#define _POSIX_C_SOURCE 200809L
/* Safe host-only tests. Never call the production poweroff request function.
 * Only explicitly listed harmless executables and our own sleep fixture run. */
#include "hal/linux/poweroff.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int failures;
static unsigned int heartbeat_count;

static void expect(bool condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

static void heartbeat(void)
{
    ++heartbeat_count;
}

static int64_t monotonic_ms(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return -1;
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static void test_safe_commands(const char *missing)
{
    const c1_poweroff_command success[] = {{"/bin/true", 0}};
    const c1_poweroff_command null_path[] = {{NULL, 0}};
    const c1_poweroff_command relative_path[] = {{"true", 0}};
    const c1_poweroff_command success_with_arguments[] = {{"/bin/true", 1}};
    const c1_poweroff_command failure[] = {{"/bin/false", 0}};
    const c1_poweroff_command fallback[] = {{missing, 0}, {"/bin/true", 0}};
    const c1_poweroff_command all_missing[] = {{missing, 0}, {missing, 1}};
    const c1_poweroff_command exited_failure[] = {{"/bin/false", 0}, {"/bin/true", 0}};

    expect(c1_linux_poweroff_run(success, 1, 1000U, heartbeat) == 0,
           "true reports successful helper exit");
    expect(c1_linux_poweroff_run(success_with_arguments, 1, 1000U, NULL) == 0,
           "shutdown-style arguments are safe with true and null heartbeat");
    expect(c1_linux_poweroff_run(failure, 1, 1000U, heartbeat) == EIO,
           "false reports failure rather than successful shutdown");
    expect(c1_linux_poweroff_run(fallback, 2, 1000U, heartbeat) == 0,
           "missing executable falls back to true");
    expect(c1_linux_poweroff_run(all_missing, 2, 1000U, heartbeat) == EIO,
           "all exec failures report failure");
    expect(c1_linux_poweroff_run(exited_failure, 2, 1000U, heartbeat) == EIO,
           "successful exec with failing exit cannot be masked by fallback");
    heartbeat_count = 0;
    expect(c1_linux_poweroff_run(NULL, 1, 1000U, heartbeat) == EINVAL,
           "null command list is rejected");
    expect(c1_linux_poweroff_run(success, 0, 1000U, heartbeat) == EINVAL,
           "empty command list is rejected");
    expect(c1_linux_poweroff_run(success, 1, 0U, heartbeat) == EINVAL,
           "zero timeout is rejected");
    expect(c1_linux_poweroff_run(null_path, 1, 1000U, heartbeat) == EINVAL,
           "null executable path is rejected before fork");
    expect(c1_linux_poweroff_run(relative_path, 1, 1000U, heartbeat) == EINVAL,
           "relative executable path cannot depend on working directory");
    expect(heartbeat_count == 0, "invalid calls do not enter helper wait loop");
}

static void test_timeout(const char *script)
{
    const c1_poweroff_command command[] = {{script, 0}};
    int64_t started = monotonic_ms();
    int64_t ended;
    int result;

    expect(started >= 0, "monotonic clock is available");
    if (started < 0) return;
    heartbeat_count = 0;
    result = c1_linux_poweroff_run(command, 1, 50U, heartbeat);
    ended = monotonic_ms();
    expect(result == ETIMEDOUT, "sleep fixture reaches the 50ms timeout");
    expect(ended >= started, "monotonic timing remains valid");
    expect(ended - started >= 50, "helper is not timed out before its deadline");
    expect(ended - started < 1000, "timeout returns within one second, not five");
    expect(heartbeat_count >= 2, "heartbeat continues while waiting for helper");
    {
        const c1_poweroff_command success[] = {{"/bin/true", 0}};
        int retry = EBUSY;
        for (int attempt = 0; attempt < 50 && retry == EBUSY; ++attempt) {
            struct timespec pause = {0, 20000000L};
            c1_linux_poweroff_reap();
            retry = c1_linux_poweroff_run(success, 1, 1000U, heartbeat);
            if (retry == EBUSY) (void)nanosleep(&pause, NULL);
        }
        expect(retry == 0, "timed-out helper is reaped and a fresh request can succeed");
    }
}

int main(void)
{
    char root[] = "/tmp/c1-poweroff-test-XXXXXX";
    char script[256] = {0};
    char missing[256];
    FILE *stream;
    int length;
    int written;
    int closed;
    bool script_created = false;

    /* These are test prerequisites, never substitutes for system power tools. */
    expect(access("/bin/true", X_OK) == 0, "safe true executable is available");
    expect(access("/bin/false", X_OK) == 0, "safe false executable is available");
    expect(access("/bin/sh", X_OK) == 0, "shell for owned fixture is available");
    expect(access("/bin/sleep", X_OK) == 0, "safe sleep executable is available");
    if (failures != 0) return EXIT_FAILURE;
    if (mkdtemp(root) == NULL) {
        perror("mkdtemp");
        return EXIT_FAILURE;
    }
    length = snprintf(script, sizeof(script), "%s/sleep-helper", root);
    expect(length > 0 && (size_t)length < sizeof(script), "fixture path fits");
    length = snprintf(missing, sizeof(missing), "%s/does-not-exist", root);
    expect(length > 0 && (size_t)length < sizeof(missing), "missing path fits");
    if (failures != 0) goto cleanup;

    stream = fopen(script, "w");
    expect(stream != NULL, "owned sleep script is created");
    if (stream == NULL) goto cleanup;
    script_created = true;
    /* Direct /bin/sleep receives no duration from the runner. Use exec so
     * this fixture waits five seconds without leaving a shell descendant. */
    written = fputs("#!/bin/sh\nexec /bin/sleep 5\n", stream);
    closed = fclose(stream);
    expect(written >= 0 && closed == 0, "sleep script is written and closed");
    if (failures != 0) goto cleanup;
    expect(chmod(script, 0700) == 0, "owned sleep script is executable");
    if (failures != 0) goto cleanup;

    test_safe_commands(missing);
    test_timeout(script);

cleanup:
    /* Delete only files created by this test, never recurse or use wildcards. */
    if (script_created) expect(unlink(script) == 0, "owned script is removed");
    expect(rmdir(root) == 0, "owned temporary directory is removed");
    if (failures != 0) {
        fprintf(stderr, "poweroff runner: %d failure(s)\n", failures);
        return EXIT_FAILURE;
    }
    puts("poweroff runner tests passed (safe host fixtures only)");
    return EXIT_SUCCESS;
}
