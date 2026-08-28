#include "core/power_policy.h"

#include <limits.h>
#include <stddef.h>

static int timeout_until(int64_t deadline, int64_t now)
{
    int64_t remaining = deadline - now;

    if (remaining <= 0) {
        return 0;
    }
    return remaining > INT_MAX ? INT_MAX : (int)remaining;
}

void c1_power_policy_init(c1_power_policy *policy, int64_t now)
{
    if (policy == NULL) {
        return;
    }
    policy->state = C1_POWER_ACTIVE;
    policy->last_activity_at = now;
    policy->locked_at = -1;
    policy->suspend_retry_at = -1;
    policy->suspend_disabled = false;
    policy->suppress_wakeup_until_release = false;
}

void c1_power_policy_note_activity(c1_power_policy *policy, int64_t now)
{
    if (policy != NULL && policy->state == C1_POWER_ACTIVE) {
        policy->last_activity_at = now;
    }
}

bool c1_power_policy_lock(c1_power_policy *policy, int64_t now)
{
    if (policy == NULL || policy->state != C1_POWER_ACTIVE) {
        return false;
    }
    policy->state = C1_POWER_LOCKED;
    policy->locked_at = now;
    policy->suspend_retry_at = -1;
    return true;
}

bool c1_power_policy_unlock(c1_power_policy *policy, int64_t now)
{
    if (policy == NULL || policy->state != C1_POWER_LOCKED) {
        return false;
    }
    policy->state = C1_POWER_ACTIVE;
    policy->last_activity_at = now;
    policy->locked_at = -1;
    policy->suspend_retry_at = -1;
    policy->suppress_wakeup_until_release = false;
    return true;
}

c1_power_action c1_power_policy_tick(c1_power_policy *policy,
                                     int64_t now,
                                     bool desktop_page)
{
    if (policy == NULL) {
        return C1_POWER_ACTION_NONE;
    }
    if (policy->state == C1_POWER_ACTIVE && !desktop_page &&
        now - policy->last_activity_at >= C1_POWER_IDLE_TIMEOUT_MS) {
        if (c1_power_policy_lock(policy, now)) {
            return C1_POWER_ACTION_ENTER_LOCK;
        }
    }
    if (policy->state == C1_POWER_LOCKED && !policy->suspend_disabled &&
        now - policy->locked_at >= C1_POWER_LOCK_TIMEOUT_MS &&
        (policy->suspend_retry_at < 0 || now >= policy->suspend_retry_at)) {
        policy->state = C1_POWER_SUSPEND_PREPARE;
        return C1_POWER_ACTION_SUSPEND;
    }
    return C1_POWER_ACTION_NONE;
}

void c1_power_policy_suspend_failed(c1_power_policy *policy, int64_t now)
{
    if (policy == NULL) {
        return;
    }
    policy->state = C1_POWER_LOCKED;
    policy->suspend_retry_at = now + C1_POWER_RETRY_DELAY_MS;
}

void c1_power_policy_suspend_unavailable(c1_power_policy *policy)
{
    if (policy == NULL) {
        return;
    }
    policy->state = C1_POWER_LOCKED;
    policy->suspend_retry_at = -1;
    policy->suspend_disabled = true;
}

void c1_power_policy_resumed(c1_power_policy *policy, int64_t now)
{
    if (policy == NULL) {
        return;
    }
    policy->state = C1_POWER_LOCKED;
    policy->locked_at = now;
    policy->suspend_retry_at = -1;
    policy->suspend_disabled = false;
    policy->suppress_wakeup_until_release = true;
}

bool c1_power_policy_filter_wakeup(c1_power_policy *policy, bool pressed)
{
    if (policy == NULL || !policy->suppress_wakeup_until_release) {
        return false;
    }
    if (!pressed) {
        policy->suppress_wakeup_until_release = false;
    }
    return true;
}

int c1_power_policy_timeout(const c1_power_policy *policy,
                            int64_t now,
                            bool desktop_page)
{
    if (policy == NULL) {
        return -1;
    }
    if (policy->state == C1_POWER_ACTIVE && !desktop_page) {
        return timeout_until(policy->last_activity_at + C1_POWER_IDLE_TIMEOUT_MS, now);
    }
    if (policy->state == C1_POWER_LOCKED && !policy->suspend_disabled) {
        int64_t deadline = policy->locked_at + C1_POWER_LOCK_TIMEOUT_MS;

        if (policy->suspend_retry_at > deadline) {
            deadline = policy->suspend_retry_at;
        }
        return timeout_until(deadline, now);
    }
    return -1;
}