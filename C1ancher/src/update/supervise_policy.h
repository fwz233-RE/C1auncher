#ifndef C1_UPDATE_SUPERVISE_POLICY_H
#define C1_UPDATE_SUPERVISE_POLICY_H

#include <stdbool.h>
#include <stdint.h>

#define C1_UPDATE_READY_TIMEOUT_MS 60000U
#define C1_UPDATE_READY_STABLE_MS 30000U

enum c1_update_pending_action {
    C1_UPDATE_PENDING_WAIT = 0,
    C1_UPDATE_PENDING_CONFIRM,
    C1_UPDATE_PENDING_ROLLBACK,
    C1_UPDATE_PENDING_STOP,
    C1_UPDATE_PENDING_CHILD_EXIT
};

struct c1_update_pending_observation {
    bool child_exited;
    bool stop_requested;
    int64_t started_ms;
    int64_t now_ms;
    int64_t ready_at_ms;
};

enum c1_update_pending_action c1_update_pending_decide(
    const struct c1_update_pending_observation *observation);

#endif