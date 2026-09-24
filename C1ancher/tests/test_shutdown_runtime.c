#define _XOPEN_SOURCE 700
#define _DEFAULT_SOURCE 1
/* Only exercise the real shutdown path with hardware/command stubs. This
 * executable never links poweroff.c and cannot run a system poweroff command. */
#include "../src/hal/linux/ui_runtime.c"
#include <assert.h>
#include "launcher/policy.h"

static unsigned renders, writes, status_reads, wallpaper_loads, requests, beats;
static unsigned stop_after_beats;
static int poweroff_error;
static bool stop_in_request, status_available = true;
static c1_status display_result = C1_STATUS_OK;
static c1_ui_page rendered_page;
static c1_ui_state *requested_state;
static c1_power_policy *requested_policy;
static unsigned expected_renders;
static unsigned shutdown_begins, shutdown_cancels, shutdown_renewals;
static int shutdown_begin_error, shutdown_cancel_error;
static bool shutdown_armed, stop_in_begin;

int c1_shutdown_begin(void)
{
    ++shutdown_begins;
    assert(requested_state->page == C1_UI_PAGE_LOCK);
    assert(renders == expected_renders);
    shutdown_armed = shutdown_begin_error == 0;
    if (stop_in_begin) raise(SIGTERM);
    return shutdown_begin_error;
}
int c1_shutdown_cancel(void)
{
    assert(shutdown_armed);
    assert(requested_state->page == C1_UI_PAGE_LOCK);
    ++shutdown_cancels;
    shutdown_armed = false;
    return shutdown_cancel_error;
}
int c1_shutdown_keepalive(void) { ++shutdown_renewals; return 0; }

bool c1_app_run_active(void) { return false; }
bool c1_app_lease_terminal_mode(void) { return false; }
void c1_wallpaper_load(void) { ++wallpaper_loads; }
bool c1_linux_system_status_read(c1_ui_status *status)
{
    ++status_reads;
    memset(status, 0, sizeof(*status));
    return status_available;
}
bool c1_wifi_read_snapshot(c1_wifi_snapshot *snapshot)
{
    memset(snapshot, 0, sizeof(*snapshot));
    return true;
}
int c1_update_state_load(const char *root, struct c1_update_state *state,
                         char *error, size_t size)
{
    (void)root; (void)error; (void)size;
    memset(state, 0, sizeof(*state));
    state->phase = C1_UPDATE_IDLE;
    return 0;
}
void c1_ui_render(uint8_t *frame, const c1_ui_state *state,
                  const c1_ui_status *status, c1_terminal_screen *screen)
{
    (void)status; (void)screen;
    ++renders;
    rendered_page = state->page;
    memset(frame, 0, C1_DISPLAY_FRAME_BYTES);
}
void c1_ui_render_input(uint8_t *frame, const c1_ui_state *state,
                        const struct c1_ime_response *view, bool enabled, bool failed)
{ (void)frame; (void)state; (void)view; (void)enabled; (void)failed; }
c1_status c1_linux_display_write_frame(void *context, const uint8_t *frame,
                                       uint32_t size, c1_record_sink sink)
{
    (void)context; (void)frame; (void)size; (void)sink;
    ++writes;
    return display_result;
}
c1_status c1_linux_display_write_frame_fast(void *context, const uint8_t *frame,
                                            uint32_t size, c1_record_sink sink)
{ return c1_linux_display_write_frame(context, frame, size, sink); }
void c1_liveness_beat(int64_t now)
{
    assert(now >= 0);
    ++beats;
    if (stop_after_beats && beats >= stop_after_beats) raise(SIGTERM);
}
int c1_linux_poweroff_request(void (*heartbeat)(void))
{
    ++requests;
    assert(shutdown_armed && shutdown_begins > 0);
    assert(requested_state->page == C1_UI_PAGE_LOCK);
    assert(requested_policy->state == C1_POWER_LOCKED);
    assert(renders == expected_renders);
    if (expected_renders) assert(rendered_page == C1_UI_PAGE_LOCK);
    heartbeat();
    if (stop_in_request) raise(SIGTERM);
    return poweroff_error;
}

static void reset_fixture(c1_ui_state *state, c1_power_policy *policy)
{
    c1_stop_reset();
    *state = c1_ui_initial_state();
    c1_power_policy_init(policy, 100);
    requested_state = state;
    requested_policy = policy;
    renders = writes = status_reads = wallpaper_loads = requests = beats = 0;
    expected_renders = stop_after_beats = 0;
    shutdown_begins = shutdown_cancels = shutdown_renewals = 0;
    shutdown_begin_error = shutdown_cancel_error = 0;
    shutdown_armed = stop_in_begin = false;
    poweroff_error = 0;
    stop_in_request = false;
    status_available = true;
    display_result = C1_STATUS_OK;
}

static void test_existing_lock(void)
{
    c1_ui_state state;
    c1_power_policy policy;
    c1_terminal_screen screen = {0};
    c1_record_sink sink = {0};
    for (unsigned style = C1_LOCK_WALLPAPER; style <= C1_LOCK_CALENDAR; ++style) {
        reset_fixture(&state, &policy);
        state.preferences.lock_style = (c1_lock_style)style;
        assert(c1_ui_enter_lock(&state));
        assert(c1_power_policy_lock(&policy, 200));
        c1_ui_state before = state;
        c1_power_policy before_policy = policy;
        /* Poweroff must not depend on status/display working after lock. */
        status_available = false;
        display_result = C1_STATUS_IO_ERROR;
        assert(request_poweroff_locked(&state, &policy, &screen, sink));
        assert(requests == 1 && !renders && !writes && !status_reads && !wallpaper_loads);
        assert(!memcmp(&state, &before, sizeof(state)));
        assert(!memcmp(&policy, &before_policy, sizeof(policy)));
        poweroff_error = EIO;
        assert(!request_poweroff_locked(&state, &policy, &screen, sink));
        assert(requests == 2 && !renders && !writes && !status_reads && !wallpaper_loads);
        assert(!memcmp(&state, &before, sizeof(state)));
        assert(!memcmp(&policy, &before_policy, sizeof(policy)));
    }
}

static void test_new_lock_and_failure(void)
{
    c1_ui_state state;
    c1_power_policy policy;
    c1_terminal_screen screen = {0};
    c1_record_sink sink = {0};
    reset_fixture(&state, &policy);
    state.page = C1_UI_PAGE_SETTINGS;
    expected_renders = 1;
    assert(request_poweroff_locked(&state, &policy, &screen, sink));
    assert(requests == 1 && renders == 1 && writes == 1 && wallpaper_loads == 1);
    assert(state.page == C1_UI_PAGE_LOCK && policy.state == C1_POWER_LOCKED);

    reset_fixture(&state, &policy);
    state.page = C1_UI_PAGE_SETTINGS;
    state.selection = C1_SETTING_LOCK_STYLE;
    c1_ui_state before = state;
    c1_power_policy before_policy = policy;
    expected_renders = 1;
    poweroff_error = EIO;
    assert(!request_poweroff_locked(&state, &policy, &screen, sink));
    assert(requests == 1 && renders == 2 && writes == 2 && rendered_page == C1_UI_PAGE_SETTINGS);
    assert(!memcmp(&state, &before, sizeof(state)));
    assert(!memcmp(&policy, &before_policy, sizeof(policy)));

    reset_fixture(&state, &policy);
    status_available = false;
    assert(!request_poweroff_locked(&state, &policy, &screen, sink));
    assert(!requests && !writes && state.page == C1_UI_PAGE_DESKTOP && policy.state == C1_POWER_ACTIVE);
    status_available = true;
    display_result = C1_STATUS_IO_ERROR;
    assert(!request_poweroff_locked(&state, &policy, &screen, sink));
    assert(!requests && writes == 1 && state.page == C1_UI_PAGE_DESKTOP && policy.state == C1_POWER_ACTIVE);

    reset_fixture(&state, &policy);
    /* init may stop the command itself before it can report success. */
    poweroff_error = EIO;
    stop_in_request = true;
    expected_renders = 1;
    assert(request_poweroff_locked(&state, &policy, &screen, sink));
    assert(c1_stop_requested() && state.page == C1_UI_PAGE_LOCK && renders == 1);
}

static void test_supervision_order_and_failure(void)
{
    c1_ui_state state;
    c1_power_policy policy;
    c1_terminal_screen screen = {0};
    c1_record_sink sink = {0};
    const int failures[] = {ENOTCONN, ETIMEDOUT, EPROTO, EACCES};
    for (size_t i = 0; i < sizeof(failures) / sizeof(*failures); ++i) {
        reset_fixture(&state, &policy);
        assert(c1_ui_enter_lock(&state) && c1_power_policy_lock(&policy, 200));
        shutdown_begin_error = failures[i];
        assert(!request_poweroff_locked(&state, &policy, &screen, sink));
        assert(shutdown_begins == 1 && !requests && !shutdown_cancels && !writes);
        assert(state.page == C1_UI_PAGE_LOCK && policy.state == C1_POWER_LOCKED);
    }
    reset_fixture(&state, &policy);
    expected_renders = 1;
    poweroff_error = EIO;
    shutdown_cancel_error = ETIMEDOUT;
    assert(!request_poweroff_locked(&state, &policy, &screen, sink));
    assert(shutdown_begins == 1 && requests == 1 && shutdown_cancels == 1);
    assert(shutdown_renewals == 1 && state.page == C1_UI_PAGE_DESKTOP);

    reset_fixture(&state, &policy);
    expected_renders = 1;
    stop_in_begin = true;
    assert(request_poweroff_locked(&state, &policy, &screen, sink));
    assert(shutdown_begins == 1 && !requests && !shutdown_cancels);
    assert(c1_stop_requested() && state.page == C1_UI_PAGE_LOCK && renders == 1);

    reset_fixture(&state, &policy);
    expected_renders = 1;
    poweroff_error = EIO;
    stop_in_request = true;
    assert(request_poweroff_locked(&state, &policy, &screen, sink));
    assert(requests == 1 && shutdown_armed && !shutdown_cancels);
    assert(state.page == C1_UI_PAGE_LOCK && renders == 1);
}

static void test_lock_wake_and_deadline_input(void)
{
    static const uint16_t wake_keys[] = {KEY_A, KEY_LEFTSHIFT, KEY_UP, KEY_HOME,
        KEY_BACK, KEY_ENTER, KEY_OK, KEY_WAKEUP};
    c1_ui_state state;
    c1_power_policy policy;
    for (unsigned style = C1_LOCK_WALLPAPER; style <= C1_LOCK_CALENDAR; ++style) {
        for (size_t k = 0; k < sizeof(wake_keys) / sizeof(*wake_keys); ++k) {
            reset_fixture(&state, &policy);
            state.preferences.lock_style = (c1_lock_style)style;
            state.preferences.power_mode = C1_POWER_MODE_SAVING;
            configure_power_preferences(&policy, &state.preferences);
            c1_power_policy_set_external_power(&policy, true, false, 100);
            assert(c1_ui_enter_lock(&state) && c1_power_policy_lock(&policy, 30100));
            struct input_event input = {.type = EV_KEY, .code = wake_keys[k], .value = 0};
            assert(!wake_lock_from_key(&state, &policy, &input, 150000));
            input.value = 2;
            assert(!wake_lock_from_key(&state, &policy, &input, 150000));
            input.value = 1;
            assert(wake_lock_from_key(&state, &policy, &input, 150000));
            assert(state.page == C1_UI_PAGE_DESKTOP && policy.state == C1_POWER_ACTIVE);
            assert(policy.locked_at == -1 && policy.shutdown_retry_at == -1);
            assert(c1_power_policy_timeout(&policy, 150000) == 60000);
            assert(c1_power_policy_tick(&policy, 209999) == C1_POWER_ACTION_NONE);
            assert(c1_power_policy_tick(&policy, 210000) == C1_POWER_ACTION_ENTER_LOCK);
            assert(c1_ui_enter_lock(&state));
            assert(c1_power_policy_timeout(&policy, 210000) == 120000);
        }
    }
    reset_fixture(&state, &policy);
    state.preferences.power_mode = C1_POWER_MODE_SAVING;
    configure_power_preferences(&policy, &state.preferences);
    c1_power_policy_set_external_power(&policy, true, false, 100);
    assert(c1_ui_enter_lock(&state) && c1_power_policy_lock(&policy, 100));
    c1_power_key key = {0};
    struct pollfd inputs[C1_UI_INPUT_COUNT];
    for (unsigned i = 0; i < C1_UI_INPUT_COUNT; ++i)
        inputs[i] = (struct pollfd){-1, POLLIN, 0};
    int pipefds[2];
    assert(pipe(pipefds) == 0);
    inputs[0].fd = pipefds[0];
    struct input_event input = {.type = EV_KEY, .code = KEY_A, .value = 1}, received;
    assert(write(pipefds[1], &input, sizeof(input)) == sizeof(input));
    assert(automatic_power_action(&policy, &key, inputs, 120100) == C1_POWER_ACTION_NONE);
    assert(policy.state == C1_POWER_LOCKED && policy.shutdown_retry_at == -1);
    assert(read(pipefds[0], &received, sizeof(received)) == sizeof(received));
    assert(wake_lock_from_key(&state, &policy, &received, 120100));
    assert(automatic_power_action(&policy, &key, inputs, 120100) == C1_POWER_ACTION_NONE);
    assert(c1_power_policy_lock(&policy, 120101) && c1_ui_enter_lock(&state));
    key.down = true;
    assert(automatic_power_action(&policy, &key, inputs, 240101) == C1_POWER_ACTION_NONE);
    key.down = false;
    assert(automatic_power_action(&policy, &key, inputs, 240101) == C1_POWER_ACTION_SHUTDOWN);
    input.code = KEY_POWER;
    assert(!wake_lock_from_key(&state, &policy, &input, 240101));
    input.code = KEY_RESERVED;
    assert(!wake_lock_from_key(&state, &policy, &input, 240101));
    close(pipefds[0]); close(pipefds[1]);
    assert(!requests && !renders);

    reset_fixture(&state, &policy);
    state.page = C1_UI_PAGE_SETTINGS;
    state.selection = C1_SETTING_LOCK_STYLE;
    assert(c1_ui_enter_lock(&state) && c1_power_policy_lock(&policy, 200));
    input.code = KEY_A;
    assert(wake_lock_from_key(&state, &policy, &input, 300));
    assert(state.page == C1_UI_PAGE_SETTINGS && state.selection == C1_SETTING_LOCK_STYLE);
    assert(policy.last_activity_at == 300 && policy.locked_at == -1);
}

static void test_wait_keeps_frame(void)
{
    c1_ui_state state;
    c1_power_policy policy;
    reset_fixture(&state, &policy);
    assert(c1_ui_enter_lock(&state));
    c1_ui_state before = state;
    stop_after_beats = 2;
    /* Actual stop handler receives a process-local signal from the heartbeat
     * stub. No UI events, status reads, update work or power retry can run. */
    assert(wait_for_poweroff() == C1_STATUS_SHUTDOWN_REQUESTED);
    assert(beats == 2 && shutdown_renewals == 2 && !requests && !renders && !writes && !status_reads);
    assert(!memcmp(&state, &before, sizeof(state)));
    assert(c1_status_exit_code(C1_STATUS_SHUTDOWN_REQUESTED) == C1_LAUNCHER_SHUTDOWN_EXIT);
    assert(wait_for_poweroff() == C1_STATUS_SHUTDOWN_REQUESTED && beats == 2);
}

int main(void)
{
    c1_stop_install();
    test_existing_lock();
    test_new_lock_and_failure();
    test_supervision_order_and_failure();
    test_lock_wake_and_deadline_input();
    test_wait_keeps_frame();
    puts("shutdown runtime passed: retained frame, new lock before command, rollback, quiet wait (no real poweroff)");
    return 0;
}
