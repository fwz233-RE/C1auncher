/* Safe host-only state-machine tests: no input devices or poweroff calls. */
#include "core/power_key.h"
#include "core/power_policy.h"

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

static void test_hold_and_retry(void)
{
    c1_power_key key = {0};

    expect(c1_power_key_deadline(&key) == -1, "idle has no deadline");
    expect(c1_power_key_tick(&key, 9000) == C1_POWER_KEY_NONE,
           "idle tick cannot shut down");
    expect(c1_power_key_event(&key, 1, 116, 1, 1000) == C1_POWER_KEY_NONE,
           "press arms without firing");
    expect(c1_power_key_deadline(&key) == 4000, "deadline is press plus 3000ms");
    expect(c1_power_key_tick(&key, 3999) == C1_POWER_KEY_NONE,
           "2999ms is too early");
    expect(c1_power_key_tick(&key, 4000) == C1_POWER_KEY_SHUTDOWN,
           "tick at exactly 3000ms fires without release");
    expect(key.down && key.fired, "fired hold stays down until release");
    expect(c1_power_key_deadline(&key) == -1, "fired hold has no deadline");
    expect(c1_power_key_tick(&key, 4000) == C1_POWER_KEY_NONE,
           "same tick cannot fire twice");
    expect(c1_power_key_tick(&key, 12000) == C1_POWER_KEY_NONE,
           "continued hold cannot fire twice");
    expect(c1_power_key_event(&key, 1, 116, 2, 12001) == C1_POWER_KEY_NONE,
           "repeat after firing cannot rearm");
    expect(c1_power_key_event(&key, 1, 116, 0, 12002) == C1_POWER_KEY_NONE,
           "release after shutdown is never a short press");
    expect(!key.down && !key.fired, "release resets fired hold");
    expect(c1_power_key_event(&key, 1, 116, 1, 13000) == C1_POWER_KEY_NONE,
           "new press allows retry after unsuccessful shutdown");
    expect(c1_power_key_deadline(&key) == 16000, "retry gets a fresh deadline");
    expect(c1_power_key_tick(&key, 16000) == C1_POWER_KEY_SHUTDOWN,
           "new hold can request shutdown again");
}

static void test_release_boundaries(void)
{
    static const int64_t elapsed[] = {0, 1, 2999, 3000, 3001, 9000};
    size_t i;

    for (i = 0; i < sizeof(elapsed) / sizeof(elapsed[0]); ++i) {
        c1_power_key key = {0};
        c1_power_key_action expected = elapsed[i] < 3000
                                      ? C1_POWER_KEY_SHORT : C1_POWER_KEY_NONE;
        (void)c1_power_key_event(&key, 2, 143, 1, 1000);
        expect(c1_power_key_event(&key, 2, 143, 0, 1000 + elapsed[i]) == expected,
               "release is SHORT only below 3000ms; queued long release is NONE");
        expect(!key.down && !key.fired, "every matching release clears hold");
        expect(c1_power_key_deadline(&key) == -1, "release cancels deadline");
        expect(c1_power_key_tick(&key, 20000) == C1_POWER_KEY_NONE,
               "tick after queued release cannot shut down");
        expect(c1_power_key_event(&key, 2, 143, 0, 20001) == C1_POWER_KEY_NONE,
               "duplicate release cannot produce short press");
    }
}

static void test_repeats_and_identity(void)
{
    c1_power_key key = {0};

    expect(c1_power_key_event(&key, 1, 116, 2, 1000) == C1_POWER_KEY_NONE,
           "unpaired repeat does nothing");
    expect(!key.down && c1_power_key_deadline(&key) == -1,
           "value 2 cannot arm hold");
    expect(c1_power_key_tick(&key, 10000) == C1_POWER_KEY_NONE,
           "unpaired repeat cannot eventually shut down");
    (void)c1_power_key_event(&key, 1, 116, 1, 10000);
    expect(c1_power_key_event(&key, 1, 116, 2, 12999) == C1_POWER_KEY_NONE,
           "repeat on held key does nothing");
    expect(c1_power_key_event(&key, 1, 116, 1, 12999) == C1_POWER_KEY_NONE,
           "duplicate press does nothing");
    expect(c1_power_key_event(&key, 2, 116, 1, 12999) == C1_POWER_KEY_NONE,
           "another source cannot replace held key");
    expect(c1_power_key_event(&key, 1, 143, 1, 12999) == C1_POWER_KEY_NONE,
           "another code cannot replace held key");
    expect(c1_power_key_event(&key, 2, 116, 0, 12999) == C1_POWER_KEY_NONE,
           "different source cannot release held key");
    expect(c1_power_key_event(&key, 1, 143, 0, 12999) == C1_POWER_KEY_NONE,
           "different code cannot release held key");
    expect(key.down && key.source == 1 && key.code == 116 && key.pressed_at == 10000,
           "repeats and unrelated events preserve original hold");
    expect(c1_power_key_deadline(&key) == 13000, "repeats never extend deadline");
    expect(c1_power_key_tick(&key, 13000) == C1_POWER_KEY_SHUTDOWN,
           "original deadline still fires");
    expect(c1_power_key_event(&key, 1, 116, 0, 13001) == C1_POWER_KEY_NONE,
           "only matching release clears fired hold");
    expect(!key.down, "matching release clears down state");
}

static void test_reset_and_backward_clock(void)
{
    c1_power_key key = {0};

    (void)c1_power_key_event(&key, 1, 116, 1, 5000);
    expect(c1_power_key_tick(&key, 4999) == C1_POWER_KEY_NONE,
           "backward clock cannot fire");
    expect(c1_power_key_tick(&key, -1) == C1_POWER_KEY_NONE,
           "negative clock cannot fire");
    expect(c1_power_key_deadline(&key) == 8000, "backward clock preserves deadline");
    expect(c1_power_key_tick(&key, 7999) == C1_POWER_KEY_NONE,
           "clock recovery must still reach original threshold");
    expect(c1_power_key_tick(&key, 8000) == C1_POWER_KEY_SHUTDOWN,
           "clock recovery fires at original deadline");
    c1_power_key_reset(&key);
    expect(!key.down && !key.fired && key.source == 0 && key.code == 0 &&
           key.pressed_at == 0, "reset erases all hold state");
    (void)c1_power_key_event(&key, 1, 116, 1, 9000);
    expect(c1_power_key_event(&key, 1, 116, 0, 8999) == C1_POWER_KEY_NONE,
           "release with backward clock is neither short nor shutdown");
    expect(!key.down, "backward-clock release clears hold");
    (void)c1_power_key_event(&key, 1, 116, 1, 10000);
    c1_power_key_reset(&key);
    expect(c1_power_key_deadline(&key) == -1, "reset cancels armed deadline");
    expect(c1_power_key_tick(&key, 13000) == C1_POWER_KEY_NONE,
           "reset prevents pending shutdown");
    expect(c1_power_key_event(&key, 1, 116, 0, 13001) == C1_POWER_KEY_NONE,
           "release after reset cannot produce a short press");
}

/* Model the documented ordering: filter wake events before feeding the key. */
static c1_power_key_action filtered_event(c1_power_policy *policy,
                                          c1_power_key *key,
                                          int value, int64_t now)
{
    if (c1_power_policy_filter_wakeup(policy, value != 0))
        return C1_POWER_KEY_NONE;
    return c1_power_key_event(key, 1, 116, value, now);
}

static void test_resume_suppression(void)
{
    c1_power_policy policy;
    c1_power_key key = {0};

    c1_power_policy_init(&policy, 0);
    c1_power_policy_resumed(&policy, 1000);
    expect(policy.suppress_wakeup_until_release, "resume enables wake suppression");
    expect(filtered_event(&policy, &key, 1, 1001) == C1_POWER_KEY_NONE,
           "wake press is suppressed");
    expect(filtered_event(&policy, &key, 2, 3999) == C1_POWER_KEY_NONE,
           "wake repeats are suppressed");
    expect(!key.down && c1_power_key_deadline(&key) == -1,
           "wake events cannot arm a hold");
    expect(c1_power_key_tick(&key, 9000) == C1_POWER_KEY_NONE,
           "holding wake key cannot shut down");
    expect(filtered_event(&policy, &key, 0, 9001) == C1_POWER_KEY_NONE,
           "wake release itself remains suppressed");
    expect(!policy.suppress_wakeup_until_release, "release restores normal input");
    expect(filtered_event(&policy, &key, 1, 10000) == C1_POWER_KEY_NONE,
           "next press is accepted");
    expect(filtered_event(&policy, &key, 0, 10001) == C1_POWER_KEY_SHORT,
           "short press works after wake release");
    (void)filtered_event(&policy, &key, 1, 11000);
    expect(c1_power_key_tick(&key, 14000) == C1_POWER_KEY_SHUTDOWN,
           "long press works after wake release");
}

int main(void)
{
    test_hold_and_retry();
    test_release_boundaries();
    test_repeats_and_identity();
    test_reset_and_backward_clock();
    test_resume_suppression();
    if (failures != 0) {
        fprintf(stderr, "power key: %d failure(s)\n", failures);
        return EXIT_FAILURE;
    }
    puts("power key tests passed");
    return EXIT_SUCCESS;
}
