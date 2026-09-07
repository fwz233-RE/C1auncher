#include "update/supervise_policy.h"

#include <stddef.h>

enum c1_update_pending_action c1_update_pending_decide(
    const struct c1_update_pending_observation *observation)
{
    if (observation == NULL) return C1_UPDATE_PENDING_ROLLBACK;
    if (observation->child_exited) return C1_UPDATE_PENDING_CHILD_EXIT;
    if (observation->stop_requested) return C1_UPDATE_PENDING_STOP;
    if (observation->started_ms < 0 || observation->now_ms < observation->started_ms)
        return C1_UPDATE_PENDING_ROLLBACK;
    if (observation->ready_at_ms >= 0) {
        if (observation->ready_at_ms < observation->started_ms ||
            observation->now_ms < observation->ready_at_ms ||
            (uint64_t)(observation->ready_at_ms - observation->started_ms) >=
                C1_UPDATE_READY_TIMEOUT_MS)
            return C1_UPDATE_PENDING_ROLLBACK;
        if ((uint64_t)(observation->now_ms - observation->ready_at_ms) >=
            C1_UPDATE_READY_STABLE_MS)
            return C1_UPDATE_PENDING_CONFIRM;
        return C1_UPDATE_PENDING_WAIT;
    }
    if ((uint64_t)(observation->now_ms - observation->started_ms) >=
        C1_UPDATE_READY_TIMEOUT_MS)
        return C1_UPDATE_PENDING_ROLLBACK;
    return C1_UPDATE_PENDING_WAIT;
}