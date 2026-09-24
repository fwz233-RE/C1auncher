#define _POSIX_C_SOURCE 200809L
#include "update/update.h"
#include "update/check.h"
#include <errno.h>
#include <stdlib.h>
#include <time.h>

/* Reuse the local-only curl routing/logging shim, never the production defaults.
 * Renaming main keeps this test linked to the actual CLI dispatch below. */
#define main repository_fixture_main
#define __wrap_execv repository_fixture_execv
#include "update_repository_driver.c"
#undef __wrap_execv
#undef main

int __wrap_execv(const char *path, char *const argv[])
{
    const char *pid_file = getenv("C1_TEST_STUCK_CURL_PID");
    if (pid_file != NULL) {
        FILE *stream = fopen(pid_file, "w");
        if (stream == NULL) _exit(126);
        fprintf(stream, "%ld\n", (long)getpid());
        if (fclose(stream) != 0) _exit(126);
        /* Simulate curl ignoring its own --max-time. Only the outer checker
         * deadline or cancellation can stop this test-only process. */
        for (;;) pause();
    }
    return repository_fixture_execv(path, argv);
}

int __real_clock_gettime(clockid_t clock, struct timespec *value);
int __wrap_clock_gettime(clockid_t clock, struct timespec *value)
{
    int result = __real_clock_gettime(clock, value);
    if (result == 0 && clock == CLOCK_MONOTONIC && getenv("C1_TEST_FAST_CLOCK") != NULL) {
        /* Exercise the production 95s cutoff in under two wall-clock seconds. */
        value->tv_sec = value->tv_sec * 100 + value->tv_nsec / 10000000L;
        value->tv_nsec = value->tv_nsec % 10000000L * 100;
    }
    return result;
}

/* Test-only defaults, not production environment-variable configuration. */
#undef C1_UPDATE_DEFAULT_STAGING_ROOT
#undef C1_UPDATE_DEFAULT_STATE_ROOT
#undef C1_UPDATE_DEFAULT_CORE_ROOT
#undef C1_UPDATE_DEFAULT_KEY
#undef C1_UPDATE_REPOSITORY_CONFIG
#define C1_UPDATE_DEFAULT_STAGING_ROOT getenv("C1_TEST_STAGING_ROOT")
#define C1_UPDATE_DEFAULT_STATE_ROOT getenv("C1_TEST_STATE_ROOT")
#define C1_UPDATE_DEFAULT_CORE_ROOT getenv("C1_TEST_CORE_ROOT")
#define C1_UPDATE_DEFAULT_KEY getenv("C1_TEST_KEY")
#define C1_UPDATE_REPOSITORY_CONFIG getenv("C1_TEST_REPOSITORY_CONFIG")
#include "../src/update/main.c"
