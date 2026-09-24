#include "launcher/policy.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

static int failures;

static void expect(bool condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

static struct c1_launcher_decision decide(bool stop, bool exited, int code,
                                          unsigned long long runtime,
                                          unsigned int crashes)
{
    struct c1_launcher_observation observation;
    observation.stop_requested = stop;
    observation.exited = exited;
    observation.exit_code = code;
    observation.runtime_ms = runtime;
    observation.short_crashes = crashes;
    return c1_launcher_decide(&observation);
}

int main(void)
{
    struct c1_launcher_decision result;
    unsigned int count;

    result = decide(true, true, C1_LAUNCHER_UPDATE_EXIT, 1U, 3U);
    expect(result.action == C1_LAUNCHER_STOP, "stop request has priority and exits cleanly");
    result = decide(false, true, C1_LAUNCHER_SHUTDOWN_EXIT, 1U, 3U);
    expect(result.action == C1_LAUNCHER_STOP && result.short_crashes == 3U,
           "accepted shutdown stops the launcher instead of restarting the desktop");
    result = decide(false, true, C1_LAUNCHER_UPDATE_EXIT, 1U, 3U);
    expect(result.action == C1_LAUNCHER_UPDATE && result.short_crashes == 3U,
           "controlled update is returned without restart");
    for (count = 0U; count < 4U; ++count) {
        result = decide(false, true, 1, 29999U, count);
        expect(result.action == C1_LAUNCHER_RESTART &&
                   result.short_crashes == count + 1U &&
                   result.backoff_seconds == (1U << count),
               "short crash increments count with capped exponential backoff");
    }
    result = decide(false, false, -1, 100U, 4U);
    expect(result.action == C1_LAUNCHER_FATAL && result.short_crashes == 5U,
           "fifth short abnormal exit becomes crash storm");
    result = decide(false, true, 1, 30000U, 4U);
    expect(result.action == C1_LAUNCHER_RESTART && result.short_crashes == 0U &&
               result.backoff_seconds == 0U,
           "stable runtime clears crash count");
    result = decide(false, true, 0, 0U, 3U);
    expect(result.action == C1_LAUNCHER_RESTART && result.short_crashes == 4U &&
               result.backoff_seconds == 8U,
           "normal short exit is still a short crash");
    result = c1_launcher_decide(NULL);
    expect(result.action == C1_LAUNCHER_FATAL, "invalid policy input fails closed");

    if (failures != 0) {
        fprintf(stderr, "%d launcher test(s) failed\n", failures);
        return EXIT_FAILURE;
    }
    puts("all launcher tests passed");
    return EXIT_SUCCESS;
}
