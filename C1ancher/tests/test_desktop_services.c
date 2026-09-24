#define _POSIX_C_SOURCE 200809L
/* Pure power-policy regressions. No hardware actions or process signals. */
#include "core/power_policy.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>

static void test_policy_defaults_and_gates(void)
{
    c1_power_policy policy;
    c1_power_policy_init(&policy, 100);
    assert(policy.idle_ms == 300000 && policy.suspend_ms == 20000 && policy.shutdown_ms == 0);
    assert(!policy.suspend_on_lock);
    assert(c1_power_policy_timeout(&policy, 100) == -1);
    assert(c1_power_policy_tick(&policy, 300100) == C1_POWER_ACTION_NONE);
    c1_power_policy_set_external_power(&policy, true, false, 100);
    assert(c1_power_policy_timeout(&policy, 100) == 300000);
    assert(c1_power_policy_tick(&policy, 300099) == C1_POWER_ACTION_NONE);
    assert(c1_power_policy_tick(&policy, 300100) == C1_POWER_ACTION_ENTER_LOCK);
    assert(policy.last_activity_at == 100);
    assert(c1_power_policy_timeout(&policy, 300100) == 20000);
    assert(c1_power_policy_tick(&policy, 320099) == C1_POWER_ACTION_NONE);
    assert(c1_power_policy_tick(&policy, 320100) == C1_POWER_ACTION_SUSPEND);
    c1_power_policy_suspend_failed(&policy, 320100);
    assert(c1_power_policy_timeout(&policy, 320100) == 60000);
    assert(c1_power_policy_tick(&policy, 380099) == C1_POWER_ACTION_NONE);
    assert(c1_power_policy_tick(&policy, 380100) == C1_POWER_ACTION_SUSPEND);
    c1_power_policy_suspend_cancelled(&policy);
    assert(policy.state == C1_POWER_LOCKED && policy.suspend_retry_at == -1);
    c1_power_policy_suspend_unavailable(&policy);
    c1_power_policy_configure(&policy, 3000, 2000, 0);
    assert(c1_power_policy_timeout(&policy, 430000) == -1);
    assert(c1_power_policy_tick(&policy, 430000) == C1_POWER_ACTION_NONE);
    c1_power_policy_resumed(&policy, 500000);
    assert(policy.last_activity_at == 100 && policy.locked_at == 500000);
    assert(c1_power_policy_filter_wakeup(&policy, true));
    assert(c1_power_policy_filter_wakeup(&policy, false));
    assert(!c1_power_policy_filter_wakeup(&policy, true));
    assert(c1_power_policy_timeout(&policy, 500000) == 2000);
    c1_power_policy_restore_failed(&policy, 500100);
    assert(policy.suspend_disabled && policy.suppress_wakeup_until_release);
    assert(c1_power_policy_timeout(&policy, 600000) == -1);
    assert(c1_power_policy_unlock(&policy, 600000));
    assert(policy.last_activity_at == 600000);
}

static void test_policy_configuration_and_shutdown(void)
{
    c1_power_policy policy;
    c1_power_policy_init(&policy, 0);
    c1_power_policy_configure(&policy, 0, 0, 0);
    c1_power_policy_set_external_power(&policy, true, false, 0);
    assert(c1_power_policy_timeout(&policy, 1000000) == -1);
    assert(c1_power_policy_tick(&policy, 1000000) == C1_POWER_ACTION_NONE);
    assert(c1_power_policy_lock(&policy, 1000000));
    assert(c1_power_policy_timeout(&policy, 2000000) == -1);
    c1_power_policy_configure(&policy, -1, -1, -1);
    assert(policy.idle_ms == 0 && policy.suspend_ms == 0 && policy.shutdown_ms == 0);

    c1_power_policy_init(&policy, 0);
    c1_power_policy_set_external_power(&policy, true, false, 0);
    c1_power_policy_configure(&policy, 30000, 1000, 120000);
    c1_power_policy_note_activity(&policy, 10000);
    assert(c1_power_policy_tick(&policy, 39999) == C1_POWER_ACTION_NONE);
    assert(c1_power_policy_tick(&policy, 40000) == C1_POWER_ACTION_ENTER_LOCK);
    assert(policy.last_activity_at == 10000);
    assert(c1_power_policy_timeout(&policy, 40000) == 120000);
    assert(c1_power_policy_tick(&policy, 41000) == C1_POWER_ACTION_NONE);
    assert(c1_power_policy_tick(&policy, 159999) == C1_POWER_ACTION_NONE);
    assert(c1_power_policy_tick(&policy, 160000) == C1_POWER_ACTION_SHUTDOWN);
    assert(c1_power_policy_timeout(&policy, 160000) == 60000);
    assert(c1_power_policy_tick(&policy, 160000) == C1_POWER_ACTION_NONE);
    c1_power_policy_shutdown_failed(&policy, 161000);
    assert(c1_power_policy_timeout(&policy, 161000) == 60000);
    assert(c1_power_policy_tick(&policy, 220999) == C1_POWER_ACTION_NONE);
    assert(c1_power_policy_tick(&policy, 221000) == C1_POWER_ACTION_SHUTDOWN);
    c1_power_policy_configure(&policy, 30000, 1000, 120000);
    assert(c1_power_policy_timeout(&policy, 221000) == 60000);
    assert(policy.last_activity_at == 10000 && policy.locked_at == 40000);
    c1_power_policy_configure(&policy, 30000, 1000, 0);
    assert(c1_power_policy_timeout(&policy, 221000) == 0);
    assert(c1_power_policy_tick(&policy, 221000) == C1_POWER_ACTION_SUSPEND);

    c1_power_policy_init(&policy, 0);
    c1_power_policy_configure(&policy, 0, 0, 5000);
    assert(c1_power_policy_timeout(&policy, 100000) == -1);
    assert(c1_power_policy_tick(&policy, 100000) == C1_POWER_ACTION_NONE);
    c1_power_policy_set_external_power(&policy, true, true, 100000);
    assert(c1_power_policy_tick(&policy, 100000) == C1_POWER_ACTION_NONE);
    c1_power_policy_set_external_power(&policy, true, false, 100000);
    assert(c1_power_policy_timeout(&policy, 100000) == -1);
    assert(c1_power_policy_tick(&policy, 1000000) == C1_POWER_ACTION_NONE);
    assert(c1_power_policy_lock(&policy, 100001));
    assert(c1_power_policy_timeout(&policy, 100001) == 19999);
    c1_power_policy_set_external_power(&policy, true, true, 119999);
    c1_power_policy_set_external_power(&policy, true, false, 120000);
    assert(c1_power_policy_tick(&policy, 139999) == C1_POWER_ACTION_NONE);
    assert(c1_power_policy_tick(&policy, 140000) == C1_POWER_ACTION_SHUTDOWN);
    c1_power_policy_set_external_power(&policy, false, false, 200000);
    assert(c1_power_policy_timeout(&policy, 200000) == -1);

    c1_power_policy_init(&policy, 0);
    c1_power_policy_set_external_power(&policy, true, false, 0);
    c1_power_policy_configure(&policy, 60000, 20000, 60000);
    assert(c1_power_policy_tick(&policy, 60000) == C1_POWER_ACTION_ENTER_LOCK);
    assert(c1_power_policy_tick(&policy, 60000) == C1_POWER_ACTION_NONE);
    assert(c1_power_policy_timeout(&policy, 60000) == 60000);
    assert(c1_power_policy_unlock(&policy, 60001));
    assert(policy.last_activity_at == 60001);
    assert(c1_power_policy_timeout(&policy, 60001) == 60000);

    c1_power_policy_init(&policy, 100);
    c1_power_policy_set_external_power(&policy, true, false, 100);
    c1_power_policy_configure(&policy, INT64_MAX, 0, 0);
    assert(c1_power_policy_timeout(&policy, 100) == INT_MAX);
    assert(c1_power_policy_tick(&policy, 0) == C1_POWER_ACTION_NONE);
    c1_power_policy_configure(NULL, 1, 1, 1);
    c1_power_policy_shutdown_failed(NULL, 0);
    assert(c1_power_policy_tick(NULL, 0) == C1_POWER_ACTION_NONE);
    assert(c1_power_policy_timeout(NULL, 0) == -1);
}

static void configure_mode(c1_power_policy *policy, unsigned mode)
{
    static const int64_t locks[] = {60000, 180000, 300000};
    static const int64_t shutdowns[] = {120000, 300000, 0};
    c1_power_policy_configure(policy, locks[mode], 0, shutdowns[mode]);
    policy->suspend_on_lock = mode == 2;
}

static void test_policy_modes_and_manual_lock(void)
{
    for (unsigned mode = 0; mode < 3; ++mode) {
        c1_power_policy policy;
        c1_power_policy_init(&policy, 0);
        configure_mode(&policy, mode);
        c1_power_policy_set_external_power(&policy, true, false, 0);
        c1_power_policy_note_activity(&policy, 1234);
        int64_t lock = 1234 + policy.idle_ms;
        int64_t shutdown = lock + policy.shutdown_ms;
        assert(c1_power_policy_timeout(&policy, 1234) == policy.idle_ms);
        assert(c1_power_policy_tick(&policy, lock - 1) == C1_POWER_ACTION_NONE);
        assert(c1_power_policy_tick(&policy, lock) == C1_POWER_ACTION_ENTER_LOCK);
        assert(policy.state == C1_POWER_LOCKED && policy.last_activity_at == 1234);
        if (mode != 2) {
            assert(c1_power_policy_timeout(&policy, lock) == shutdown - lock);
            assert(c1_power_policy_tick(&policy, lock) == C1_POWER_ACTION_NONE);
            assert(c1_power_policy_tick(&policy, shutdown - 1) == C1_POWER_ACTION_NONE);
            assert(c1_power_policy_tick(&policy, shutdown) == C1_POWER_ACTION_SHUTDOWN);
            assert(c1_power_policy_timeout(&policy, shutdown) == 60000);
            c1_power_policy_shutdown_failed(&policy, shutdown + 7000);
            assert(c1_power_policy_tick(&policy, shutdown + 66999) == C1_POWER_ACTION_NONE);
            assert(c1_power_policy_tick(&policy, shutdown + 67000) == C1_POWER_ACTION_SHUTDOWN);
        } else {
            assert(c1_power_policy_timeout(&policy, lock) == 0);
            assert(c1_power_policy_tick(&policy, lock) == C1_POWER_ACTION_SUSPEND);
            c1_power_policy_suspend_failed(&policy, lock + 1000);
            assert(c1_power_policy_timeout(&policy, lock + 1000) == 60000);
            assert(c1_power_policy_tick(&policy, lock + 60999) == C1_POWER_ACTION_NONE);
            assert(c1_power_policy_tick(&policy, lock + 61000) == C1_POWER_ACTION_SUSPEND);
            c1_power_policy_resumed(&policy, lock + 62000);
            assert(policy.state == C1_POWER_LOCKED && policy.suppress_wakeup_until_release);
            assert(c1_power_policy_timeout(&policy, lock + 62000) == 20000);
            assert(c1_power_policy_tick(&policy, lock + 81999) == C1_POWER_ACTION_NONE);
            assert(c1_power_policy_filter_wakeup(&policy, true));
            assert(c1_power_policy_filter_wakeup(&policy, false));
            assert(!c1_power_policy_filter_wakeup(&policy, true));
            assert(c1_power_policy_tick(&policy, lock + 82000) == C1_POWER_ACTION_SUSPEND);
            c1_power_policy_restore_failed(&policy, lock + 83000);
            assert(c1_power_policy_timeout(&policy, INT64_MAX) == -1);
            assert(c1_power_policy_tick(&policy, INT64_MAX) == C1_POWER_ACTION_NONE);
        }
        assert(c1_power_policy_unlock(&policy, 1000000));
        assert(c1_power_policy_timeout(&policy, 1000000) == policy.idle_ms);
        assert(!c1_power_policy_unlock(&policy, 1000001));
        c1_power_policy_note_activity(&policy, 1000100);
        assert(c1_power_policy_lock(&policy, 1000100));
        assert(!c1_power_policy_lock(&policy, 1000101));
        assert(policy.last_activity_at == 1000100);
        if (mode != 2) {
            assert(c1_power_policy_tick(&policy, 1000100 + policy.shutdown_ms - 1) == C1_POWER_ACTION_NONE);
            assert(c1_power_policy_tick(&policy, 1000100 + policy.shutdown_ms) == C1_POWER_ACTION_SHUTDOWN);
        }
    }
}

static void test_policy_cable_transitions(void)
{
    const int64_t long_idle = 24LL * 60 * 60 * 1000;
    for (unsigned mode = 0; mode < 3; ++mode) {
        for (unsigned locked = 0; locked < 2; ++locked) {
            for (unsigned unknown = 0; unknown < 2; ++unknown) {
                c1_power_policy policy;
                c1_power_policy_init(&policy, 0);
                configure_mode(&policy, mode);
                c1_power_policy_set_external_power(&policy, true, false, 0);
                c1_power_policy_set_external_power(&policy, !unknown, true, 100);
                if (locked) assert(c1_power_policy_lock(&policy, 100));
                for (int64_t now = 100; now <= long_idle; now += 1000000) {
                    assert(c1_power_policy_tick(&policy, now) == C1_POWER_ACTION_NONE);
                    assert(c1_power_policy_timeout(&policy, now) == -1);
                }
                assert(c1_power_policy_tick(&policy, long_idle) == C1_POWER_ACTION_NONE);
                assert(c1_power_policy_timeout(&policy, long_idle) == -1);
                c1_power_policy_set_external_power(&policy, true, false, long_idle);
                assert(policy.last_activity_at == long_idle);
                int64_t due = long_idle + (locked && mode != 2 ? policy.shutdown_ms : policy.idle_ms);
                assert(c1_power_policy_timeout(&policy, long_idle) == due - long_idle);
                c1_power_policy_set_external_power(&policy, true, false, long_idle + 5000);
                assert(policy.external_power_offline_at == long_idle && policy.last_activity_at == long_idle);
                assert(c1_power_policy_tick(&policy, due - 1) == C1_POWER_ACTION_NONE);
                assert(c1_power_policy_tick(&policy, due) == (!locked ? C1_POWER_ACTION_ENTER_LOCK :
                    mode == 2 ? C1_POWER_ACTION_SUSPEND : C1_POWER_ACTION_SHUTDOWN));
                if (mode == 2 && !locked) assert(c1_power_policy_tick(&policy, due) == C1_POWER_ACTION_SUSPEND);
                if (mode == 2) c1_power_policy_suspend_failed(&policy, due);
                c1_power_policy_set_external_power(&policy, false, false, due + 1);
                assert(c1_power_policy_timeout(&policy, due + 1000000) == -1);
                assert(c1_power_policy_tick(&policy, due + 1000000) == C1_POWER_ACTION_NONE);
                assert(c1_power_policy_unlock(&policy, due + 1000000));
                assert(c1_power_policy_lock(&policy, due + 1000001));
                assert(c1_power_policy_tick(&policy, due + 2000000) == C1_POWER_ACTION_NONE);
            }
        }
    }
    c1_power_policy policy;
    c1_power_policy_init(&policy, 0);
    configure_mode(&policy, 2);
    c1_power_policy_set_external_power(&policy, true, false, 0);
    assert(c1_power_policy_lock(&policy, 30000));
    assert(c1_power_policy_tick(&policy, 30000) == C1_POWER_ACTION_SUSPEND);
    c1_power_policy_set_external_power(&policy, true, true, 30001);
    c1_power_policy_set_external_power(&policy, true, false, 30002);
    c1_power_policy_resumed(&policy, 30003);
    assert(c1_power_policy_timeout(&policy, 30003) == 299999);
    c1_power_policy_suspend_failed(&policy, 30004);
    assert(c1_power_policy_timeout(&policy, 30004) == 299998);
    c1_power_policy_configure(&policy, 300000, 0, 0);
    assert(!policy.suspend_on_lock && c1_power_policy_timeout(&policy, INT64_MAX) == -1);
    policy.suspend_on_lock = true;
    c1_power_policy_configure(&policy, -1, -1, -1);
    assert(!policy.suspend_on_lock && c1_power_policy_tick(&policy, INT64_MAX) == C1_POWER_ACTION_NONE);
}

static void test_independent_lock_shutdown_cycles(void)
{
    for (unsigned mode = 0; mode < 2; ++mode) {
        c1_power_policy policy;
        c1_power_policy_init(&policy, 0);
        configure_mode(&policy, mode);
        c1_power_policy_set_external_power(&policy, true, false, 0);
        /* Manual lock must not inherit nearly expired active-idle time. */
        int64_t manual = policy.idle_ms - 1;
        assert(c1_power_policy_lock(&policy, manual));
        assert(c1_power_policy_timeout(&policy, manual) == policy.shutdown_ms);
        int64_t wake = manual + policy.shutdown_ms - 1;
        assert(c1_power_policy_tick(&policy, wake) == C1_POWER_ACTION_NONE);
        c1_power_policy_shutdown_failed(&policy, wake + 1000000);
        assert(c1_power_policy_unlock(&policy, wake));
        assert(policy.locked_at == -1 && policy.shutdown_retry_at == -1);
        assert(c1_power_policy_timeout(&policy, wake) == policy.idle_ms);
        assert(c1_power_policy_tick(&policy, wake + 1) == C1_POWER_ACTION_NONE);
        int64_t lock = wake + policy.idle_ms;
        assert(c1_power_policy_tick(&policy, lock - 1) == C1_POWER_ACTION_NONE);
        assert(c1_power_policy_tick(&policy, lock) == C1_POWER_ACTION_ENTER_LOCK);
        assert(policy.locked_at == lock);
        assert(c1_power_policy_timeout(&policy, lock) == policy.shutdown_ms);
        assert(c1_power_policy_tick(&policy, lock + policy.shutdown_ms - 1) == C1_POWER_ACTION_NONE);
        assert(c1_power_policy_tick(&policy, lock + policy.shutdown_ms) == C1_POWER_ACTION_SHUTDOWN);
        /* Delayed event-loop processing locks first instead of skipping the
         * visible lock screen and immediately shutting down from active. */
        assert(c1_power_policy_unlock(&policy, lock + policy.shutdown_ms + 1));
        int64_t delayed = policy.last_activity_at + policy.idle_ms + policy.shutdown_ms + 60000;
        assert(c1_power_policy_tick(&policy, delayed) == C1_POWER_ACTION_ENTER_LOCK);
        assert(c1_power_policy_tick(&policy, delayed) == C1_POWER_ACTION_NONE);
        assert(c1_power_policy_timeout(&policy, delayed) == policy.shutdown_ms);
    }
}

static void test_policy_timeout_matches_tick(void)
{
    static const int64_t intervals[] = {0, 1, 20000, 60000};
    static const int64_t times[] = {0, 100, 20099, 20100, 60100, 200000};
    for (size_t i = 0; i < 4; ++i) {
        for (size_t j = 0; j < 4; ++j) {
            for (size_t k = 0; k < 4; ++k) {
                for (unsigned int flags = 0; flags < 128; ++flags) {
                    c1_power_policy policy;
                    c1_power_policy_init(&policy, 100);
                    c1_power_policy_configure(&policy, intervals[i], intervals[j], intervals[k]);
                    policy.state = (c1_power_state)(flags & 3);
                    policy.locked_at = 100;
                    policy.suspend_disabled = (flags & 4) != 0;
                    policy.suspend_on_lock = (flags & 64) != 0;
                    policy.suspend_retry_at = (flags & 8) ? 90000 : -1;
                    policy.shutdown_retry_at = policy.suspend_retry_at;
                    c1_power_policy_set_external_power(&policy, (flags & 16) != 0, (flags & 32) != 0, 100);
                    for (size_t t = 0; t < sizeof(times) / sizeof(times[0]); ++t) {
                        c1_power_policy copy = policy;
                        int timeout = c1_power_policy_timeout(&policy, times[t]);
                        c1_power_action action = c1_power_policy_tick(&copy, times[t]);
                        assert((timeout == 0) == (action != C1_POWER_ACTION_NONE));
                        if (timeout > 0) {
                            copy = policy;
                            assert(c1_power_policy_tick(&copy, times[t] + timeout - 1) == C1_POWER_ACTION_NONE);
                            assert(c1_power_policy_tick(&copy, times[t] + timeout) != C1_POWER_ACTION_NONE);
                        }
                    }
                }
            }
        }
    }
}

int main(void)
{
    test_policy_defaults_and_gates();
    test_policy_configuration_and_shutdown();
    test_policy_modes_and_manual_lock();
    test_policy_cable_transitions();
    test_independent_lock_shutdown_cycles();
    test_policy_timeout_matches_tick();
    puts("desktop service tests passed (all power-policy regressions; no hardware/signals)");
    return 0;
}
