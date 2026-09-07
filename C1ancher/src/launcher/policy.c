#include "launcher/policy.h"

#include <stddef.h>

struct c1_launcher_decision
c1_launcher_decide(const struct c1_launcher_observation *observation)
{
    struct c1_launcher_decision decision = {C1_LAUNCHER_FATAL, 0U, 0U};
    unsigned int crashes;

    if (observation == NULL) {
        return decision;
    }
    crashes = observation->short_crashes;
    if (observation->stop_requested) {
        decision.action = C1_LAUNCHER_STOP;
        decision.short_crashes = crashes;
        return decision;
    }
    if (observation->exited && observation->exit_code == C1_LAUNCHER_UPDATE_EXIT) {
        decision.action = C1_LAUNCHER_UPDATE;
        decision.short_crashes = crashes;
        return decision;
    }
    if (observation->runtime_ms >= C1_LAUNCHER_SHORT_RUNTIME_MS) {
        crashes = 0U;
    } else if (crashes < C1_LAUNCHER_MAX_SHORT_CRASHES) {
        ++crashes;
    }
    decision.short_crashes = crashes;
    if (crashes >= C1_LAUNCHER_MAX_SHORT_CRASHES) {
        decision.action = C1_LAUNCHER_FATAL;
        return decision;
    }
    decision.action = C1_LAUNCHER_RESTART;
    if (crashes != 0U) {
        decision.backoff_seconds = 1U << (crashes - 1U);
        if (decision.backoff_seconds > 8U) {
            decision.backoff_seconds = 8U;
        }
    }
    return decision;
}
