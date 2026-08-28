#ifndef C1_CORE_POWER_POLICY_H
#define C1_CORE_POWER_POLICY_H

#include <stdbool.h>
#include <stdint.h>

#define C1_POWER_IDLE_TIMEOUT_MS (5LL * 60LL * 1000LL)
#define C1_POWER_LOCK_TIMEOUT_MS (30LL * 1000LL)
#define C1_POWER_RETRY_DELAY_MS (60LL * 1000LL)

typedef enum {
    C1_POWER_ACTIVE = 0,
    C1_POWER_LOCKED,
    C1_POWER_SUSPEND_PREPARE,
    C1_POWER_RESUMING
} c1_power_state;

typedef enum {
    C1_POWER_ACTION_NONE = 0,
    C1_POWER_ACTION_ENTER_LOCK,
    C1_POWER_ACTION_SUSPEND
} c1_power_action;

typedef struct {
    c1_power_state state;
    int64_t last_activity_at;
    int64_t locked_at;
    int64_t suspend_retry_at;
    bool suspend_disabled;
    bool suppress_wakeup_until_release;
} c1_power_policy;

void c1_power_policy_init(c1_power_policy *policy, int64_t now);
void c1_power_policy_note_activity(c1_power_policy *policy, int64_t now);
c1_power_action c1_power_policy_tick(c1_power_policy *policy,
                                     int64_t now,
                                     bool desktop_page);
bool c1_power_policy_lock(c1_power_policy *policy, int64_t now);
bool c1_power_policy_unlock(c1_power_policy *policy, int64_t now);
void c1_power_policy_suspend_failed(c1_power_policy *policy, int64_t now);
void c1_power_policy_suspend_unavailable(c1_power_policy *policy);
void c1_power_policy_resumed(c1_power_policy *policy, int64_t now);
bool c1_power_policy_filter_wakeup(c1_power_policy *policy, bool pressed);
int c1_power_policy_timeout(const c1_power_policy *policy,
                            int64_t now,
                            bool desktop_page);

#endif