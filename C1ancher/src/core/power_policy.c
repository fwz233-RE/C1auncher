#include "core/power_policy.h"

#include <limits.h>
#include <stddef.h>

static int64_t deadline_after(int64_t start, int64_t delay)
{
    return start > INT64_MAX - delay ? INT64_MAX : start + delay;
}

static int timeout_until(int64_t deadline, int64_t now)
{
    uint64_t remaining;

    if (deadline <= now) {
        return 0;
    }
    remaining = (uint64_t)deadline - (uint64_t)now;
    return remaining > INT_MAX ? INT_MAX : (int)remaining;
}

static bool lock_deadline(const c1_power_policy *policy, int64_t *deadline)
{
    if (policy->state != C1_POWER_ACTIVE || policy->idle_ms <= 0 ||
        !policy->external_power_known || policy->external_power_online) {
        return false;
    }
    *deadline = deadline_after(policy->last_activity_at, policy->idle_ms);
    return true;
}

static bool offline_deadline(const c1_power_policy *policy, int64_t *deadline)
{
    if (!policy->external_power_known || policy->external_power_online ||
        policy->external_power_offline_at < 0) {
        return false;
    }
    *deadline = deadline_after(policy->external_power_offline_at,
                               C1_POWER_EXTERNAL_OFFLINE_DELAY_MS);
    return true;
}

static bool shutdown_deadline(const c1_power_policy *policy, int64_t *deadline)
{
    int64_t power_deadline;

    if (policy->shutdown_ms <= 0 || policy->state != C1_POWER_LOCKED ||
        policy->locked_at < 0 || !offline_deadline(policy, &power_deadline)) {
        return false;
    }
    /* The active idle timer ends at lock. Manual and automatic locks both
     * start a fresh, independent shutdown countdown. */
    *deadline = deadline_after(policy->locked_at, policy->shutdown_ms);
    if (power_deadline > *deadline) {
        *deadline = power_deadline;
    }
    if (policy->shutdown_retry_at > *deadline) {
        *deadline = policy->shutdown_retry_at;
    }
    return true;
}

/* A configured shutdown keeps the device awake for a possible deadline; the
 * runtime cannot rely on a suspended poll loop to perform that action. */
static bool suspend_deadline(const c1_power_policy *policy, int64_t *deadline)
{
    int64_t power_deadline;

    if (policy->state != C1_POWER_LOCKED || policy->suspend_disabled ||
        (policy->suspend_ms <= 0 && !policy->suspend_on_lock) || policy->shutdown_ms > 0 ||
        !offline_deadline(policy, &power_deadline)) {
        return false;
    }
    *deadline = deadline_after(policy->locked_at, policy->suspend_ms);
    if (power_deadline > *deadline) {
        *deadline = power_deadline;
    }
    if (policy->suspend_retry_at > *deadline) {
        *deadline = policy->suspend_retry_at;
    }
    return true;
}

void c1_power_policy_init(c1_power_policy *policy, int64_t now)
{
    if (policy == NULL) {
        return;
    }
    policy->state = C1_POWER_ACTIVE;
    policy->last_activity_at = now;
    policy->locked_at = -1;
    policy->external_power_offline_at = -1;
    policy->suspend_retry_at = -1;
    policy->shutdown_retry_at = -1;
    policy->idle_ms = C1_POWER_IDLE_TIMEOUT_MS;
    policy->suspend_ms = C1_POWER_LOCK_TIMEOUT_MS;
    policy->shutdown_ms = 0;
    policy->external_power_known = false;
    policy->external_power_online = false;
    policy->suspend_disabled = false;
    policy->suspend_on_lock = false;
    policy->suppress_wakeup_until_release = false;
}

void c1_power_policy_configure(c1_power_policy *policy, int64_t idle_ms,
                               int64_t suspend_ms, int64_t shutdown_ms)
{
    if (policy == NULL) {
        return;
    }
    policy->idle_ms = idle_ms > 0 ? idle_ms : 0;
    policy->suspend_ms = suspend_ms > 0 ? suspend_ms : 0;
    policy->shutdown_ms = shutdown_ms > 0 ? shutdown_ms : 0;
    policy->suspend_on_lock = false;
}

void c1_power_policy_note_activity(c1_power_policy *policy, int64_t now)
{
    if (policy != NULL && policy->state == C1_POWER_ACTIVE) {
        policy->last_activity_at = now;
    }
}

void c1_power_policy_set_external_power(c1_power_policy *policy,
                                        bool known,
                                        bool online,
                                        int64_t now)
{
    bool offline;

    if (policy == NULL) {
        return;
    }
    offline = known && !online;
    if (offline && (!policy->external_power_known || policy->external_power_online)) {
        /* Plugged-in/unknown time is never carried into a battery idle period.
         * Repeated offline samples must not keep extending that period. */
        policy->external_power_offline_at = now;
        policy->last_activity_at = now;
        if (policy->state != C1_POWER_ACTIVE) {
            int64_t deadline = deadline_after(now, policy->idle_ms);
            policy->locked_at = now;
            if (deadline > policy->suspend_retry_at) policy->suspend_retry_at = deadline;
        }
    } else if (!offline) {
        policy->external_power_offline_at = -1;
    }
    policy->external_power_known = known;
    policy->external_power_online = online;
}

bool c1_power_policy_lock(c1_power_policy *policy, int64_t now)
{
    if (policy == NULL || policy->state != C1_POWER_ACTIVE) {
        return false;
    }
    policy->state = C1_POWER_LOCKED;
    policy->locked_at = now;
    policy->suspend_retry_at = -1;
    policy->shutdown_retry_at = -1;
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
    policy->shutdown_retry_at = -1;
    policy->suppress_wakeup_until_release = false;
    return true;
}

c1_power_action c1_power_policy_tick(c1_power_policy *policy,
                                     int64_t now)
{
    int64_t deadline;

    if (policy == NULL) {
        return C1_POWER_ACTION_NONE;
    }
    /* Shutdown is eligible only after a separately completed lock transition. */
    if (shutdown_deadline(policy, &deadline) && now >= deadline) {
        c1_power_policy_shutdown_failed(policy, now);
        return C1_POWER_ACTION_SHUTDOWN;
    }
    if (lock_deadline(policy, &deadline) && now >= deadline) {
        if (c1_power_policy_lock(policy, now)) {
            return C1_POWER_ACTION_ENTER_LOCK;
        }
    }
    if (suspend_deadline(policy, &deadline) && now >= deadline) {
        policy->state = C1_POWER_SUSPEND_PREPARE;
        return C1_POWER_ACTION_SUSPEND;
    }
    return C1_POWER_ACTION_NONE;
}

void c1_power_policy_shutdown_failed(c1_power_policy *policy, int64_t now)
{
    if (policy != NULL) {
        policy->shutdown_retry_at = deadline_after(now, C1_POWER_RETRY_DELAY_MS);
    }
}

void c1_power_policy_suspend_failed(c1_power_policy *policy, int64_t now)
{
    if (policy == NULL) {
        return;
    }
    policy->state = C1_POWER_LOCKED;
    int64_t deadline = deadline_after(now, C1_POWER_RETRY_DELAY_MS);
    if (deadline > policy->suspend_retry_at) policy->suspend_retry_at = deadline;
}

void c1_power_policy_suspend_cancelled(c1_power_policy *policy)
{
    if (policy == NULL) {
        return;
    }
    policy->state = C1_POWER_LOCKED;
    policy->suspend_retry_at = -1;
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

void c1_power_policy_restore_failed(c1_power_policy *policy, int64_t now)
{
    if (policy == NULL) {
        return;
    }
    policy->state = C1_POWER_LOCKED;
    policy->locked_at = now;
    policy->suspend_retry_at = -1;
    policy->suspend_disabled = true;
    policy->suppress_wakeup_until_release = true;
}

void c1_power_policy_resumed(c1_power_policy *policy, int64_t now)
{
    if (policy == NULL) {
        return;
    }
    policy->state = C1_POWER_LOCKED;
    policy->locked_at = now;
    /* Immediate-on-lock applies to a new lock, not the wakeup itself. Keep
     * the historical 20s window to release the wake key and unlock, and any
     * longer fresh-unplug guard established during resume. */
    if (policy->suspend_on_lock) {
        int64_t deadline = deadline_after(now, C1_POWER_LOCK_TIMEOUT_MS);
        if (deadline > policy->suspend_retry_at) policy->suspend_retry_at = deadline;
    }
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
                            int64_t now)
{
    int result = -1;
    int candidate;
    int64_t deadline;

    if (policy == NULL) {
        return -1;
    }
    if (lock_deadline(policy, &deadline)) {
        result = timeout_until(deadline, now);
    }
    if (shutdown_deadline(policy, &deadline)) {
        candidate = timeout_until(deadline, now);
        if (result < 0 || candidate < result) {
            result = candidate;
        }
    }
    if (suspend_deadline(policy, &deadline)) {
        candidate = timeout_until(deadline, now);
        if (result < 0 || candidate < result) {
            result = candidate;
        }
    }
    return result;
}
