#ifndef C1_CORE_POWER_POLICY_H
#define C1_CORE_POWER_POLICY_H

#include <stdbool.h>
#include <stdint.h>

#define C1_POWER_IDLE_TIMEOUT_MS (5LL * 60LL * 1000LL)
#define C1_POWER_LOCK_TIMEOUT_MS (20LL * 1000LL)
#define C1_POWER_EXTERNAL_OFFLINE_DELAY_MS (20LL * 1000LL)
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
    C1_POWER_ACTION_SUSPEND,
    C1_POWER_ACTION_SHUTDOWN
} c1_power_action;

typedef struct {
    c1_power_state state;
    int64_t last_activity_at;
    int64_t locked_at;
    int64_t external_power_offline_at;
    int64_t suspend_retry_at;
    int64_t shutdown_retry_at;
    int64_t idle_ms;
    int64_t suspend_ms;
    int64_t shutdown_ms;
    bool external_power_known;
    bool external_power_online;
    bool suspend_disabled;
    bool suspend_on_lock;
    bool suppress_wakeup_until_release;
} c1_power_policy;

/* now is nonnegative CLOCK_MONOTONIC milliseconds (awake time, not BOOTTIME).
 * init preserves the historical 5-minute idle / 20-second suspend defaults;
 * the runtime applies user preferences separately with configure. */
void c1_power_policy_init(c1_power_policy *policy, int64_t now);
/* Durations <= 0 disable that automatic action. configure always clears
 * suspend_on_lock; callers may set it afterwards to opt into zero-delay
 * suspend with suspend_ms == 0 (init leaves it false).
 * Configuration does not change last_activity_at, locked_at, retry deadlines,
 * or hardware safety gates. idle_ms measures active inactivity; shutdown_ms
 * measures time since entering lock, never active time. Unlock cancels that
 * countdown and both retry deadlines; each new lock starts a fresh countdown.
 * All automatic actions require known battery power.
 * shutdown_ms > 0 inhibits automatic mem suspend, including shutdown failures:
 * RTC wakeup is unverified, so sleeping would stop the awake-time deadline. */
void c1_power_policy_configure(c1_power_policy *policy, int64_t idle_ms,
                               int64_t suspend_ms, int64_t shutdown_ms);
void c1_power_policy_note_activity(c1_power_policy *policy, int64_t now);
/* A transition from online/unknown to known offline starts a full new idle
 * period, including when already manually locked. Repeated samples do not. */
void c1_power_policy_set_external_power(c1_power_policy *policy,
                                        bool known,
                                        bool online,
                                        int64_t now);
/* SHUTDOWN is emitted only when external power is known offline for >=20s.
 * Emission automatically arms a 60s retry backoff; it never powers hardware off.
 * Runtime must still validate its own safety conditions and execute the request. */
c1_power_action c1_power_policy_tick(c1_power_policy *policy,
                                     int64_t now);
/* Extend retry from the actual failed/cancelled request completion time. */
void c1_power_policy_shutdown_failed(c1_power_policy *policy, int64_t now);
bool c1_power_policy_lock(c1_power_policy *policy, int64_t now);
bool c1_power_policy_unlock(c1_power_policy *policy, int64_t now);
void c1_power_policy_suspend_failed(c1_power_policy *policy, int64_t now);
void c1_power_policy_suspend_cancelled(c1_power_policy *policy);
void c1_power_policy_suspend_unavailable(c1_power_policy *policy);
void c1_power_policy_restore_failed(c1_power_policy *policy, int64_t now);
/* Resume remains locked, filters the wake key through release, and gives an
 * immediate-on-lock policy a 20s awake grace before another suspend attempt. */
void c1_power_policy_resumed(c1_power_policy *policy, int64_t now);
bool c1_power_policy_filter_wakeup(c1_power_policy *policy, bool pressed);
int c1_power_policy_timeout(const c1_power_policy *policy,
                            int64_t now);

#endif