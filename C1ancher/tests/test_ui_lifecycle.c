#define _XOPEN_SOURCE 700
#define _DEFAULT_SOURCE 1
/* The real /proc and PTY checks use this test executable as the installed GUI. */
#define C1_PKG_EXECUTABLE "/proc/self/exe"
static char run_path[256], lease_path[256], mode_path[256], guard_path[256];
#define C1_APP_RUN_PATH run_path
#define C1_APP_LEASE_PATH lease_path
#define C1_APP_MODE_PATH mode_path
#define C1_APP_MODE_GUARD_PATH guard_path
#include "../src/platform/app_lease.c"
#include "../src/hal/linux/ui_runtime.c"
/* This existing target deliberately discards unused runtime sections. Include
 * the pure policy here so mode integration can run without Makefile changes. */
#include "../src/core/power_policy.c"
#include <termios.h>

/* Hardware-free tests exercise the actual UI publication/render/health logic;
 * unused runtime sections are discarded by the host linker. */
static unsigned int frame_writes, probe_calls, probe_timeout;
static c1_ui_status runtime_system_status;
static int probe_result = -1;
static int failures;
static int update_load_error;
static enum c1_update_phase test_update_phase = C1_UPDATE_PENDING_BOOT;
static void expect(bool okay, const char *message)
{
    if (!okay) { fprintf(stderr, "FAIL: %s\n", message); ++failures; }
}

/* Media loading is outside this hardware-free rendering/lease test. */
void c1_wallpaper_load(void) {}

void c1_ui_render(uint8_t *frame, const c1_ui_state *state,
                  const c1_ui_status *status, c1_terminal_screen *screen)
{
    (void)state; (void)status; (void)screen;
    memset(frame, 0, C1_DISPLAY_FRAME_BYTES);
}
void c1_ui_render_input(uint8_t *frame, const c1_ui_state *state,
                        const struct c1_ime_response *view, bool enabled, bool failed)
{ (void)frame; (void)state; (void)view; (void)enabled; (void)failed; }

bool c1_linux_system_status_read(c1_ui_status *status)
{
    *status = runtime_system_status;
    return true;
}

c1_status c1_linux_display_write_frame(void *context, const uint8_t *frame,
                                       uint32_t size, c1_record_sink sink)
{
    int guard = c1_app_lease_guard_acquire();
    (void)context; (void)frame; (void)size; (void)sink;
    if (guard >= 0) { ++frame_writes; c1_app_lease_release(guard); }
    return C1_STATUS_OK;
}
c1_status c1_linux_display_write_frame_fast(void *context, const uint8_t *frame,
                                            uint32_t size, c1_record_sink sink)
{
    return c1_linux_display_write_frame(context, frame, size, sink);
}
int c1_update_state_load(const char *root, struct c1_update_state *state,
                         char *error, size_t error_size)
{
    (void)root; (void)error; (void)error_size;
    memset(state, 0, sizeof(*state));
    state->phase = test_update_phase;
    return update_load_error;
}
int c1_update_health_should_probe(const struct c1_update_state *state)
{
    return state->phase == C1_UPDATE_PENDING_BOOT;
}
int c1_update_health_probe_running(const char *root, const char *ready, unsigned int timeout)
{
    (void)root; (void)ready;
    ++probe_calls; probe_timeout = timeout;
    return probe_result;
}

static void test_publication(void)
{
    int run, lease, guard;
    c1_ui_state state = {0};
    c1_ui_status ui_status = {0};
    c1_record_sink sink = {0};
    expect(c1_app_lease_write_mode("terminal"), "create stale terminal mode fixture");
    guard = c1_app_lease_acquire_at(guard_path);
    run = c1_app_run_acquire_at(run_path);
    expect(guard >= 0 && run >= 0, "new run owner is inside mode publication guard");
    expect(c1_app_run_active() && !c1_app_lease_terminal_mode(),
           "acquired run lock cannot expose previous mode during publication");
    c1_app_lease_release(run); c1_app_lease_release(guard);
    run = c1_app_run_acquire();
    expect(run >= 0 && !c1_app_lease_terminal_mode(), "new run acquisition clears stale mode atomically");
    expect((fcntl(run, F_GETFD) & FD_CLOEXEC) == 0, "run mutex survives application exec");
    expect(c1_app_run_acquire() < 0, "a second app cannot run concurrently");
    expect(c1_app_lease_write_mode("terminal"), "terminal publication succeeds after run acquisition");
    expect(c1_app_lease_terminal_mode(), "UI reread recognizes newly published terminal mode");
    expect(c1_app_run_active() && !c1_app_lease_active(), "terminal run excludes apps but does not own hardware");
    (void)render_state(state, &ui_status, NULL, false, sink);
    expect(frame_writes == 1U, "terminal-mode app displays while the run lock remains held");
    lease = c1_app_lease_acquire();
    (void)render_state(state, &ui_status, NULL, false, sink);
    expect(lease >= 0 && frame_writes == 1U, "bottom display hardware lease remains authoritative");
    c1_app_lease_release(lease);
    expect(c1_app_lease_write_mode("direct"), "mode may change while run remains active");
    (void)render_state(state, &ui_status, NULL, false, sink);
    expect(frame_writes == 1U && !c1_app_lease_terminal_mode(), "continuous mode reads suppress direct-app rendering");
    c1_app_lease_clear_mode();
    expect(mkfifo(mode_path, 0600) == 0, "create untrusted mode FIFO");
    expect(!c1_app_lease_terminal_mode(), "unknown FIFO mode is rejected without blocking UI heartbeat");
    unlink(mode_path);
    c1_app_lease_release(run);
}

static void test_health_retry(void)
{
    c1_ui_health_retry retry = {0};
    int64_t now = monotonic_milliseconds();
    try_update_health(&retry, now);
    expect(probe_calls == 1U && probe_timeout == 1000U && !retry.finished,
           "first one-second health failure remains retryable");
    try_update_health(&retry, retry.next_attempt - 1);
    expect(probe_calls == 1U, "health retry has a bounded interval");
    probe_result = 0;
    try_update_health(&retry, retry.next_attempt);
    expect(probe_calls == 2U && probe_timeout == 2000U && retry.finished,
           "a later bounded health attempt can publish digest-bound readiness");
    try_update_health(&retry, retry.next_attempt + 1000);
    expect(probe_calls == 2U, "successful readiness is not repeatedly rewritten");
    memset(&retry, 0, sizeof(retry));
    retry.deadline = now;
    try_update_health(&retry, now);
    expect(retry.finished && probe_calls == 2U, "total readiness retry deadline is enforced");
    memset(&retry, 0, sizeof(retry));
    retry.deadline = now + 900;
    probe_result = -1;
    try_update_health(&retry, now);
    expect(probe_timeout == 300U, "three probe commands fit the remaining total deadline");
}

static void test_daily_quote_runtime(void)
{
    c1_desktop_data saved_summary = desktop_summary;
    c1_preferences saved_preferences = desktop_preferences;
    c1_ui_status saved_system_status = runtime_system_status;
    c1_ui_status status;

    memset(&desktop_summary, 0, sizeof(desktop_summary));
    desktop_preferences = c1_preferences_default();
    runtime_system_status = (c1_ui_status){0};
    strcpy(desktop_summary.quote_zh, "中文原句");
    strcpy(desktop_summary.quote_en, "English translation");
    desktop_preferences.language = C1_LANGUAGE_ZH;
    expect(read_ui_status(&status) && !strcmp(status.daily_quote, "中文原句"),
           "runtime uses quote_zh for Chinese UI with a legacy translated cache");
    desktop_preferences.language = C1_LANGUAGE_EN;
    expect(read_ui_status(&status) && !strcmp(status.daily_quote, "中文原句"),
           "runtime keeps the legacy cache original when UI language is English");

    strcpy(desktop_summary.quote_zh, "English original");
    strcpy(desktop_summary.quote_en, "English original");
    desktop_preferences.language = C1_LANGUAGE_ZH;
    expect(read_ui_status(&status) && !strcmp(status.daily_quote, "English original"),
           "runtime displays an English original unchanged in Chinese UI");
    desktop_preferences.language = C1_LANGUAGE_EN;
    expect(read_ui_status(&status) && !strcmp(status.daily_quote, "English original"),
           "runtime displays an English original unchanged in English UI");

    desktop_summary = saved_summary;
    desktop_preferences = saved_preferences;
    runtime_system_status = saved_system_status;
}

static void test_desktop_update_entry(void)
{
    c1_ui_state state = {0};
    c1_ui_transition transition = {0};
    c1_service_worker worker;
    state.page = C1_UI_PAGE_DESKTOP;
    service_worker_init(&worker);
    transition.action = C1_UI_ACTION_NONE;
    expect(!ignore_settings_update(&state, C1_UI_EVENT_ENTER, &transition, &worker),
           "desktop confirm is never swallowed by the update guard");
    state.page = C1_UI_PAGE_SETTINGS; state.selection = C1_SETTING_UPDATE;
    transition.action = C1_UI_ACTION_UPDATE_REFRESH;
    expect(!ignore_settings_update(&state, C1_UI_EVENT_ENTER, &transition, &worker),
           "online background update check is accepted while idle");
    expect(terminal_action_command(transition.action) == NULL,
           "background update refresh cannot create a terminal session");
    worker.pid = 123;
    expect(ignore_settings_update(&state, C1_UI_EVENT_ENTER, &transition, &worker),
           "repeated confirm cannot queue duplicate background checks");
    transition.action = C1_UI_ACTION_TERMINAL_UPDATE;
    expect(ignore_settings_update(&state, C1_UI_EVENT_ENTER, &transition, &worker),
           "prepared state observed before worker completion cannot open a racing terminal");
    service_worker_init(&worker);
    expect(!ignore_settings_update(&state, C1_UI_EVENT_ENTER, &transition, &worker),
           "prepared online confirmation is accepted after worker completion");
    expect(strstr(terminal_action_command(transition.action), " tui-prepared\r") != NULL,
           "homepage confirmation uses a prepared-only command that never downloads");
    state.page = C1_UI_PAGE_TERMINAL;
    expect(!ignore_settings_update(&state, C1_UI_EVENT_ENTER, &transition, &worker),
           "the offline desktop guard does not swallow terminal confirmation");
}

static c1_wifi_snapshot wifi_fixture;
static unsigned int wifi_adoptions;
bool c1_wifi_read_snapshot(c1_wifi_snapshot *snapshot)
{
    *snapshot = wifi_fixture;
    return true;
}
void c1_wifi_adopt_snapshot(const c1_wifi_snapshot *snapshot)
{
    wifi_fixture = *snapshot;
    ++wifi_adoptions;
}

static void test_wifi_progress_pipe(void)
{
    int output[2], cancel[2];
    expect(pipe(output) == 0 && pipe(cancel) == 0, "create isolated progress/cancel pipes");
    (void)fcntl(output[0], F_SETFL, O_NONBLOCK);
    (void)fcntl(cancel[0], F_SETFL, O_NONBLOCK);
    c1_wifi_worker_progress progress = {output[1], cancel[0], C1_UI_ACTION_WIFI_CONNECT,
                                        C1_WIFI_PHASE_IDLE, false};
    c1_service_result record;
    expect(wifi_worker_progress(C1_WIFI_PHASE_AUTHENTICATING, &progress),
           "phase callback continues without cancellation");
    expect(read(output[0], &record, sizeof(record)) == (ssize_t)sizeof(record) &&
               record.progress_only && record.phase == C1_WIFI_PHASE_AUTHENTICATING,
           "phase callback transfers a complete progress record");
    expect(wifi_worker_progress(C1_WIFI_PHASE_AUTHENTICATING, &progress) &&
               read(output[0], &record, sizeof(record)) < 0 && errno == EAGAIN,
           "repeated polling of the same phase emits no redraw traffic");
    close(cancel[1]);
    expect(!wifi_worker_progress(C1_WIFI_PHASE_RESTORING, &progress),
           "closing cancellation pipe cooperatively stops connection");
    close(cancel[0]); close(output[0]); close(output[1]);
}

static void test_wifi_worker_records(void)
{
    int output[2];
    c1_service_worker worker;
    service_worker_init(&worker);
    if (pipe(output) != 0) { expect(false, "create worker record pipe"); return; }
    pid_t child = fork();
    if (child == 0) {
        close(output[0]);
        c1_service_result record = {0};
        record.action = C1_UI_ACTION_WIFI_SCAN;
        record.progress_only = true;
        record.phase = C1_WIFI_PHASE_SCANNING;
        if (write(output[1], &record, sizeof(record)) != (ssize_t)sizeof(record)) _exit(2);
        record.progress_only = false;
        record.wifi_snapshot_valid = true;
        record.status = C1_STATUS_OK;
        if (write(output[1], &record, sizeof(record)) != (ssize_t)sizeof(record)) _exit(3);
        _exit(0);
    }
    close(output[1]);
    if (child < 0) { expect(false, "fork worker record child"); close(output[0]); return; }
    worker.fd = output[0]; worker.pid = child; worker.action = C1_UI_ACTION_WIFI_SCAN;
    visible_service_action = worker.action;
    c1_service_result record;
    struct pollfd ready = {worker.fd, POLLIN, 0};
    if (poll(&ready, 1U, 2000) <= 0) {
        expect(false, "worker publishes progress promptly");
        (void)fcntl(worker.fd, F_SETFL, O_NONBLOCK);
        service_worker_stop(&worker);
        return;
    }
    expect(!service_worker_finish(&worker, &record) && worker.pid == child &&
               visible_service_action == C1_UI_ACTION_WIFI_SCAN &&
               visible_wifi_phase == C1_WIFI_PHASE_SCANNING,
           "progress updates phase without completing or closing the worker");
    expect(service_worker_finish(&worker, &record) && record.wifi_snapshot_valid &&
               worker.pid == -1 && worker.fd == -1 && visible_service_action == C1_UI_ACTION_NONE,
           "final record completes worker and retains valid snapshot marker");
}

static void test_wifi_cooperative_shutdown(void)
{
    int output[2], cancel[2], proof[2];
    c1_service_worker worker;
    service_worker_init(&worker);
    if (pipe(output) != 0 || pipe(cancel) != 0 || pipe(proof) != 0) {
        expect(false, "create shutdown test pipes");
        return;
    }
    pid_t child = fork();
    if (child == 0) {
        char byte;
        close(output[0]); close(cancel[1]); close(proof[0]);
        if (read(cancel[0], &byte, 1U) != 0) _exit(2);
        /* A successful cooperative rollback leaves proof before exiting. */
        byte = 'R';
        if (write(proof[1], &byte, 1U) != 1) _exit(3);
        c1_service_result final = {0};
        final.action = C1_UI_ACTION_WIFI_CONNECT;
        final.wifi_snapshot_valid = true;
        if (write(output[1], &final, sizeof(final)) != (ssize_t)sizeof(final)) _exit(4);
        _exit(0);
    }
    close(output[1]); close(cancel[0]); close(proof[1]);
    if (child < 0) {
        expect(false, "fork shutdown test child");
        close(output[0]); close(cancel[1]); close(proof[0]);
        return;
    }
    worker.fd = output[0]; worker.cancel_fd = cancel[1]; worker.pid = child;
    worker.action = C1_UI_ACTION_WIFI_CONNECT;
    (void)fcntl(worker.fd, F_SETFL, O_NONBLOCK);
    int64_t before = monotonic_milliseconds();
    service_worker_stop(&worker);
    char byte = 0;
    expect(read(proof[0], &byte, 1U) == 1 && byte == 'R',
           "UI shutdown allows cooperative Wi-Fi cleanup before killing worker");
    expect(monotonic_milliseconds() - before < 2000 && worker.pid == -1 &&
               worker.fd == -1 && worker.cancel_fd == -1,
           "cooperative shutdown reaps child promptly and closes worker descriptors");
    close(proof[0]);
    service_worker_stop(&worker);
}

static void test_wifi_parent_activity(void)
{
    c1_ui_status status = {0};
    c1_service_result result = {0};
    wifi_fixture.state = C1_WIFI_CONNECTED;
    visible_service_action = C1_UI_ACTION_WIFI_CONNECT;
    merge_service_status(&status);
    expect(status.wifi_busy && status.service_busy &&
               status.wifi_activity == C1_UI_ACTION_WIFI_CONNECT,
           "parent activity survives refresh even when child snapshot is stale/connected");
    visible_service_action = C1_UI_ACTION_UPDATE_REFRESH;
    wifi_stop_pending = true;
    merge_service_status(&status);
    expect(!status.wifi_busy && status.service_busy && status.wifi_stop_pending &&
               strstr(status.wifi_message, "STOP QUEUED") != NULL,
           "shared updater work and queued Wi-Fi stop are visible without claiming a scan");
    result.action = C1_UI_ACTION_WIFI_CONNECT;
    result.status = C1_STATUS_IO_ERROR;
    adopt_service_result(&result);
    expect(wifi_adoptions == 0U && wifi_fixture.state == C1_WIFI_CONNECTED &&
               strstr(service_notice, "TASK FAILED") != NULL,
           "broken worker result does not replace Wi-Fi snapshot with zero/disabled data");
    result.wifi_snapshot_valid = true;
    result.wifi.state = C1_WIFI_READY;
    adopt_service_result(&result);
    expect(wifi_adoptions == 1U && wifi_fixture.state == C1_WIFI_READY,
           "a complete service result is adopted");
    c1_ui_state state = c1_ui_initial_state();
    state.page = C1_UI_PAGE_WIFI;
    snprintf(state.secret, sizeof(state.secret), "retry-password");
    state.secret_length = strlen(state.secret);
    result.action = C1_UI_ACTION_WIFI_CONNECT;
    result.status = C1_STATUS_UNAVAILABLE;
    result.wifi_snapshot_valid = true;
    snprintf(result.wifi.error, sizeof(result.wifi.error), "AUTHENTICATION FAILED");
    visible_service_action = C1_UI_ACTION_NONE;
    wifi_stop_pending = false;
    finish_wifi_interaction(&state, &result);
    expect(state.page == C1_UI_PAGE_WIFI_PASSWORD && state.secret_length > 0U &&
               state.secret_visible && strstr(state.wifi_notice, "AUTHENTICATION") != NULL,
           "failed connection returns to visible password entry for correction");
    state.page = C1_UI_PAGE_WIFI;
    state.selected_saved = true;
    state.selected_security = C1_WIFI_SECURITY_WPA_PSK;
    snprintf(state.selected_ssid, sizeof(state.selected_ssid), "Saved test network");
    c1_ui_clear_secret(&state);
    finish_wifi_interaction(&state, &result);
    expect(state.page == C1_UI_PAGE_WIFI_PASSWORD && state.secret_length == 0U &&
               !state.selected_saved && state.secret_visible &&
               !strcmp(state.selected_ssid, "Saved test network"),
           "failed saved PSK can be re-entered without exposing its stored value");
    state.page = C1_UI_PAGE_WIFI; state.selected_saved = true;
    wifi_stop_pending = true;
    finish_wifi_interaction(&state, &result);
    expect(state.page == C1_UI_PAGE_WIFI && state.secret_length == 0U,
           "explicit turn-off never opens a password correction prompt");
    wifi_stop_pending = false;
    state.selected_security = C1_WIFI_SECURITY_OPEN;
    finish_wifi_interaction(&state, &result);
    expect(state.page == C1_UI_PAGE_WIFI && state.secret_length == 0U,
           "open network failures never request a password");
    snprintf(state.secret, sizeof(state.secret), "late-password");
    state.secret_length = strlen(state.secret);
    state.page = C1_UI_PAGE_DESKTOP;
    finish_wifi_interaction(&state, &result);
    expect(state.page == C1_UI_PAGE_DESKTOP && state.secret_length == 0U,
           "late connection result cannot drag user back from home or retain a password");
    service_notice[0] = '\0';
}

static bool power_known, power_online;
bool c1_linux_external_power_read(bool *online)
{
    *online = power_online;
    return power_known;
}

static void test_power_modes_runtime(void)
{
    static const c1_power_mode modes[] = {
        C1_POWER_MODE_SAVING, C1_POWER_MODE_STANDARD, C1_POWER_MODE_PERFORMANCE
    };
    static const int64_t locks[] = {60000, 180000, 300000};
    static const int64_t shutdowns[] = {120000, 300000, 0};
    c1_ui_status ui_status = {0};
    c1_record_sink sink = {0};
    c1_service_worker worker = {.pid = -1};
    c1_terminal_session user = {.child_pid = -1}, app = {.child_pid = -1};
    network_clock.pid = -1;
    desktop_job.pid = -1;
    test_update_phase = C1_UPDATE_IDLE;
    for (size_t i = 0; i < sizeof(modes) / sizeof(*modes); ++i) {
        c1_ui_state state = c1_ui_initial_state();
        c1_power_policy policy;
        c1_power_policy_init(&policy, 0);
        state.preferences.power_mode = modes[i];
        configure_power_preferences(&policy, &state.preferences);
        expect(policy.idle_ms == locks[i] && policy.suspend_ms == 0 &&
                   policy.shutdown_ms == shutdowns[i] && policy.suspend_on_lock == (i == 2),
               "runtime applies the public mode mapping without retaining legacy timers");
        power_known = false; power_online = false;
        update_external_power(&policy, 0);
        expect(c1_power_policy_tick(&policy, 1000000) == C1_POWER_ACTION_NONE &&
                   c1_power_policy_timeout(&policy, 1000000) == -1,
               "failed power probe conservatively suppresses every automatic action");
        power_known = true; power_online = true;
        update_external_power(&policy, 1000000);
        expect(c1_power_policy_tick(&policy, 2000000) == C1_POWER_ACTION_NONE &&
                   c1_power_policy_timeout(&policy, 2000000) == -1 &&
                   deadline_timeout(-1, 2000000 + C1_EXTERNAL_POWER_INTERVAL_MS, 2000000) == 5000,
               "plugged-in idle contributes no zero timeout; power sampling stays bounded");
        power_online = false;
        update_external_power(&policy, 2000000);
        int64_t lock_at = 2000000 + locks[i];
        expect(c1_power_policy_tick(&policy, lock_at - 1) == C1_POWER_ACTION_NONE &&
                   c1_power_policy_tick(&policy, lock_at) == C1_POWER_ACTION_ENTER_LOCK,
               "unplug starts a full mode-specific idle timer before locking");
        expect(c1_ui_enter_lock(&state), "automatic lock enters the actual UI lock state");
        unsigned frames_before = frame_writes;
        expect(render_state(state, &ui_status, NULL, true, sink) == C1_STATUS_OK &&
                   frame_writes == frames_before + 1U && policy.state == C1_POWER_LOCKED,
               "a complete lock frame is published while policy is still locked, before suspend");
        if (i == 2) {
            expect(c1_power_policy_tick(&policy, lock_at) == C1_POWER_ACTION_SUSPEND,
                   "performance requests suspend on the next tick without another 20-second initial delay");
            worker.pid = 123;
            expect(!automatic_suspend_safe(&worker), "active task blocks performance suspend preparation");
            c1_power_policy_suspend_failed(&policy, lock_at);
            expect(c1_power_policy_timeout(&policy, lock_at) == 60000 && worker.pid == 123,
                   "a task deferral retains the task and arms bounded retry instead of spinning");
            worker.pid = -1;
            expect(c1_power_policy_tick(&policy, lock_at + 59999) == C1_POWER_ACTION_NONE &&
                       c1_power_policy_tick(&policy, lock_at + 60000) == C1_POWER_ACTION_SUSPEND &&
                       automatic_suspend_safe(&worker),
                   "finished work allows the next safe performance suspend attempt");
            c1_power_policy_resumed(&policy, lock_at + 60001);
            expect(c1_power_policy_timeout(&policy, lock_at + 60001) == 20000 &&
                       c1_power_policy_filter_wakeup(&policy, true) &&
                       c1_power_policy_filter_wakeup(&policy, false),
                   "resume retains a wake-release grace period rather than immediately re-sleeping");
        } else {
            int64_t off_at = lock_at + shutdowns[i];
            expect(c1_power_policy_tick(&policy, lock_at) == C1_POWER_ACTION_NONE &&
                       c1_power_policy_timeout(&policy, lock_at) == off_at - lock_at,
                   "saving and standard start a separate locked countdown and keep CLOCK_MONOTONIC running");
            user.child_pid = 123;
            expect(c1_power_policy_tick(&policy, off_at) == C1_POWER_ACTION_SHUTDOWN &&
                       !automatic_shutdown_safe(&worker, &user, &app) && user.child_pid == 123,
                   "live terminal work defers an otherwise due shutdown without being terminated");
            c1_power_policy_shutdown_failed(&policy, off_at + 100);
            expect(c1_power_policy_timeout(&policy, off_at + 100) == 60000 &&
                       c1_power_policy_tick(&policy, off_at + 60099) == C1_POWER_ACTION_NONE,
                   "protected shutdown is retried from the deferral time, never replaced with mem sleep");
            user.child_pid = -1;
        }
        expect(c1_power_policy_unlock(&policy, 4000000) && c1_ui_unlock(&state) &&
                   c1_power_policy_timeout(&policy, 4000000) == locks[i],
               "manual unlock restores a full active timer for every mode");
        power_online = true;
        update_external_power(&policy, 4000100);
        expect(c1_power_policy_lock(&policy, 4000100) && c1_ui_enter_lock(&state) &&
                   c1_power_policy_tick(&policy, 5000000) == C1_POWER_ACTION_NONE &&
                   c1_power_policy_timeout(&policy, 5000000) == -1,
               "manual lock remains available while plugged in without automatic follow-up actions");
        state.preferences.power_mode = C1_POWER_MODE_STANDARD;
        configure_power_preferences(&policy, &state.preferences);
        expect(!policy.suspend_on_lock && policy.shutdown_ms == 300000,
               "changing back to standard clears immediate suspend configuration");
    }
    test_update_phase = C1_UPDATE_PENDING_BOOT;
}

static void test_shutdown_guard(void)
{
    c1_service_worker worker = {.pid = -1};
    c1_terminal_session user = {.child_pid = -1}, app = {.child_pid = -1};
    network_clock.pid = -1;
    test_update_phase = C1_UPDATE_IDLE;
    expect(automatic_shutdown_safe(&worker, &user, &app), "idle desktop may request configured shutdown");
    worker.pid = 12;
    expect(!automatic_shutdown_safe(&worker, &user, &app), "service operation defers automatic shutdown");
    worker.pid = -1; user.child_pid = 13;
    expect(!automatic_shutdown_safe(&worker, &user, &app), "persistent user terminal protects unsaved work");
    user.child_pid = -1; app.child_pid = 14;
    expect(!automatic_shutdown_safe(&worker, &user, &app), "owned application session defers automatic shutdown");
    app.child_pid = -1; network_clock.pid = 15;
    expect(!automatic_shutdown_safe(&worker, &user, &app), "time operation defers automatic shutdown");
    network_clock.pid = -1; test_update_phase = C1_UPDATE_DOWNLOADING;
    expect(!automatic_shutdown_safe(&worker, &user, &app), "downloading update defers automatic shutdown");
    test_update_phase = C1_UPDATE_VERIFIED;
    expect(!automatic_shutdown_safe(&worker, &user, &app), "unprepared update defers automatic shutdown");
    test_update_phase = C1_UPDATE_PENDING_BOOT;
    expect(!automatic_shutdown_safe(&worker, &user, &app), "unconfirmed boot defers automatic shutdown");
    test_update_phase = C1_UPDATE_PREPARED;
    expect(automatic_shutdown_safe(&worker, &user, &app), "prepared update is stable without active work");
    update_load_error = -1;
    expect(!automatic_shutdown_safe(&worker, &user, &app), "unknown update state never permits automatic shutdown");
    update_load_error = 0;
    int run = c1_app_run_acquire();
    expect(run >= 0 && !automatic_shutdown_safe(&worker, &user, &app), "external app lease defers automatic shutdown");
    c1_app_lease_release(run);
    int lease = c1_app_lease_acquire();
    expect(lease >= 0 && !automatic_shutdown_safe(&worker, &user, &app) &&
               !automatic_suspend_safe(&worker), "hardware lease alone blocks both automatic power actions");
    c1_app_lease_release(lease);
    test_update_phase = C1_UPDATE_IDLE;
    expect(automatic_suspend_safe(&worker), "idle desktop allows safe suspend preparation");
    worker.pid = 123;
    expect(!automatic_suspend_safe(&worker), "service work blocks automatic suspend");
    worker.pid = -1; desktop_job.pid = 124;
    expect(!automatic_suspend_safe(&worker) && !automatic_shutdown_safe(&worker, &user, &app),
           "desktop background job blocks both automatic power actions");
    desktop_job.pid = -1; network_clock.pid = 125;
    expect(!automatic_suspend_safe(&worker), "network clock work blocks automatic suspend");
    network_clock.pid = -1;
    static const enum c1_update_phase busy_phases[] = {
        C1_UPDATE_DOWNLOADING, C1_UPDATE_VERIFIED, C1_UPDATE_PENDING_BOOT
    };
    for (size_t i = 0; i < sizeof(busy_phases) / sizeof(*busy_phases); ++i) {
        test_update_phase = busy_phases[i];
        expect(!automatic_suspend_safe(&worker), "incomplete update blocks automatic suspend");
    }
    test_update_phase = C1_UPDATE_PREPARED;
    expect(automatic_suspend_safe(&worker), "stable prepared update allows safe suspend preparation");
    update_load_error = -1;
    expect(!automatic_suspend_safe(&worker), "unknown update state blocks automatic suspend");
    update_load_error = 0;
    run = c1_app_run_acquire();
    expect(run >= 0 && !automatic_suspend_safe(&worker), "external app run blocks automatic suspend");
    c1_app_lease_release(run);
    test_update_phase = C1_UPDATE_PENDING_BOOT;
}

static void test_shutdown_lock_state(void)
{
    c1_ui_state state = c1_ui_initial_state();
    c1_power_policy policy;

    c1_power_policy_init(&policy, 100);
    expect(prepare_poweroff_lock(&state, &policy, 200) &&
               state.page == C1_UI_PAGE_LOCK && policy.state == C1_POWER_LOCKED,
           "shutdown preparation publishes lock page and locks power policy together");

    state = c1_ui_initial_state();
    state.page = C1_UI_PAGE_LOCK;
    c1_power_policy_init(&policy, 300);
    expect(prepare_poweroff_lock(&state, &policy, 400) &&
               state.page == C1_UI_PAGE_LOCK && policy.state == C1_POWER_LOCKED,
           "an already visible lock page still synchronizes an active power policy");
}

/* Only the write service is substituted: identity checks below use actual
 * /proc executable/argv and controlling-terminal foreground state. */
static char shift_output[64];
static unsigned int shift_writes;
c1_status c1_terminal_write(c1_terminal_session *session, const void *bytes, size_t count)
{
    (void)session;
    if (count >= sizeof(shift_output)) return C1_STATUS_IO_ERROR;
    memcpy(shift_output, bytes, count);
    shift_output[count] = '\0';
    ++shift_writes;
    return C1_STATUS_OK;
}

static void test_shift_events(void)
{
    c1_ui_shift_key shift = {0};
    c1_ui_state state = c1_ui_initial_state();
    c1_terminal_session session = {.child_pid = 10, .shell_pid = 11};
    state.page = C1_UI_PAGE_TERMINAL;
    shift_key_focus(&shift, &state, &session);
    expect(!shift_key_event(&shift, 0U, KEY_LEFTSHIFT, 1) && shift.pressed,
           "physical Shift press does not activate management");
    expect(shift_key_event(&shift, 0U, KEY_LEFTSHIFT, 0) && !shift.pressed,
           "only an uncombined physical Shift release produces a tap");
    expect(!shift_key_event(&shift, 0U, KEY_LEFTSHIFT, 0), "duplicate Shift release is not a second tap");
    (void)shift_key_event(&shift, 0U, KEY_LEFTSHIFT, 1);
    expect(!shift_key_event(&shift, 0U, KEY_LEFTSHIFT, 2) &&
               !shift_key_event(&shift, 0U, KEY_LEFTSHIFT, 0),
           "Shift auto-repeat and its release cannot open management");
    (void)shift_key_event(&shift, 0U, KEY_LEFTSHIFT, 2);
    expect(!shift_key_event(&shift, 0U, KEY_LEFTSHIFT, 0), "orphan repeat cannot arm a Shift tap");
    (void)shift_key_event(&shift, 0U, KEY_LEFTSHIFT, 1);
    (void)shift_key_event(&shift, 0U, KEY_LEFTSHIFT, 1);
    expect(!shift_key_event(&shift, 0U, KEY_LEFTSHIFT, 0), "duplicate press cannot reset used/repeating Shift");
    static const unsigned short chords[] = {KEY_SPACE, KEY_Q, KEY_T, KEY_A, KEY_1,
        KEY_OK, KEY_HOME, KEY_POWER, KEY_WAKEUP, KEY_VOLUMEUP, KEY_TAB, KEY_RIGHTSHIFT};
    for (size_t i = 0; i < sizeof(chords) / sizeof(*chords); ++i) {
        for (unsigned int source = 0; source < C1_UI_INPUT_COUNT; ++source) {
            shift_key_reset(&shift);
            (void)shift_key_event(&shift, 0U, KEY_LEFTSHIFT, 1);
            (void)shift_key_event(&shift, source, chords[i], 1);
            (void)shift_key_event(&shift, source, chords[i], 0);
            expect(shift.pressed && !shift_key_event(&shift, 0U, KEY_LEFTSHIFT, 0),
                   "letters/digits/Space/consumed navigation/power chords are never Shift taps");
            (void)shift_key_event(&shift, source, chords[i], 1);
            (void)shift_key_event(&shift, 0U, KEY_LEFTSHIFT, 1);
            expect(!shift_key_event(&shift, 0U, KEY_LEFTSHIFT, 0),
                   "a key held before Shift also makes a chord on either input device");
            (void)shift_key_event(&shift, source, chords[i], 0);
            (void)shift_key_event(&shift, 0U, KEY_LEFTSHIFT, 1);
            expect(shift_key_event(&shift, 0U, KEY_LEFTSHIFT, 0),
                   "releasing a previous chord permits the next clean Shift tap");
        }
    }
    shift_key_reset(&shift);
    (void)shift_key_event(&shift, 0U, KEY_LEFTSHIFT, 1);
    (void)shift_key_event(&shift, 1U, KEY_LEFTSHIFT, 0);
    expect(!shift_key_event(&shift, 0U, KEY_LEFTSHIFT, 0),
           "a different input device cannot complete a pending Shift tap");
    shift_key_reset(&shift);
    shift_key_focus(&shift, &state, &session);
    (void)shift_key_event(&shift, 0U, KEY_LEFTSHIFT, 1);
    state.page = C1_UI_PAGE_DESKTOP;
    shift_key_focus(&shift, &state, &session);
    state.page = C1_UI_PAGE_TERMINAL;
    shift_key_focus(&shift, &state, &session);
    expect(!shift_key_event(&shift, 0U, KEY_LEFTSHIFT, 0), "leaving and returning to a page cancels held Shift");
    (void)shift_key_event(&shift, 0U, KEY_LEFTSHIFT, 1);
    state.page = C1_UI_PAGE_LOCK;
    shift_key_focus(&shift, &state, &session);
    state.page = C1_UI_PAGE_TERMINAL;
    shift_key_focus(&shift, &state, &session);
    expect(!shift_key_event(&shift, 0U, KEY_LEFTSHIFT, 0), "lock and unlock cannot leave a Shift tap behind");
    (void)shift_key_event(&shift, 0U, KEY_LEFTSHIFT, 1);
    ++session.shell_pid;
    shift_key_focus(&shift, &state, &session);
    expect(!shift_key_event(&shift, 0U, KEY_LEFTSHIFT, 0), "session replacement cancels a pending Shift tap");
    (void)shift_key_event(&shift, 0U, KEY_LEFTSHIFT, 1);
    state.terminal_symbol_picker = true;
    shift_key_focus(&shift, &state, &session);
    expect(!shift_key_event(&shift, 0U, KEY_LEFTSHIFT, 0), "changing input overlay cancels a pending Shift tap");
    (void)shift_key_event(&shift, 0U, KEY_LEFTSHIFT, 1);
    shift_key_reset(&shift);
    expect(!shift_key_event(&shift, 0U, KEY_LEFTSHIFT, 0) && !shift.pressed && !shift.held_count,
           "input loss or suspend reset discards all pending modifiers and taps");

    bool changed;
    state.terminal_symbol_picker = false;
    state.keyboard_layer = C1_UI_KEYBOARD_LOWER;
    shift_writes = 0U;
    expect(apply_shift_tap(&state, &session, true, &changed) == C1_STATUS_OK &&
               shift_writes == 1U && !strcmp(shift_output, "\033[57441u") &&
               !changed && state.keyboard_layer == C1_UI_KEYBOARD_LOWER,
           "GUI Shift tap writes exact CSI-u once without changing desktop keyboard layer");
    expect(apply_shift_tap(&state, &session, false, &changed) == C1_STATUS_OK &&
               shift_writes == 1U && changed && state.keyboard_layer == C1_UI_KEYBOARD_UPPER,
           "other terminals retain layer cycling with no management bytes");
    expect(send_shift_space(&session) == C1_STATUS_OK && !strcmp(shift_output, "\033[32;2u"),
           "Shift Space retains its existing modified-key protocol");
    expect(input_keysym(KEY_Q, C1_UI_KEYBOARD_LOWER, true, false) == '1' &&
               input_keysym(KEY_T, C1_UI_KEYBOARD_LOWER, true, false) == '5' &&
               input_keysym(KEY_A, C1_UI_KEYBOARD_LOWER, true, false) == '\'',
           "Shift number and symbol mappings remain unchanged");
}

static int shift_fixture_child(void)
{
    char byte = 'R';
    if (write(STDOUT_FILENO, &byte, 1U) != 1) return 2;
    while (read(STDIN_FILENO, &byte, 1U) == 1) {
        if (byte == 'X') {
            /* Same PID, but no longer the GUI after launching a terminal app. */
            execl("/bin/cat", "third-party-terminal", (char *)NULL);
            return 3;
        }
    }
    return 0;
}

static bool start_shift_fixture(c1_terminal_session *session, const char *mode, const char *extra)
{
    char slave_name[128];
    int master = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (master < 0) return false;
    if (grantpt(master) || unlockpt(master)) { close(master); return false; }
    snprintf(slave_name, sizeof(slave_name), "%s", ptsname(master));
    pid_t child = fork();
    if (child == 0) {
        if (setsid() < 0) _exit(10);
        int slave = open(slave_name, O_RDWR);
        struct termios raw;
        if (slave < 0 || ioctl(slave, TIOCSCTTY, 0) || tcgetattr(slave, &raw)) _exit(11);
        cfmakeraw(&raw);
        if (tcsetattr(slave, TCSANOW, &raw) || dup2(slave, STDIN_FILENO) < 0 ||
            dup2(slave, STDOUT_FILENO) < 0) _exit(12);
        close(slave); close(master);
        execl("/proc/self/exe", "c1pkg", mode, extra, (char *)NULL);
        _exit(13);
    }
    if (child < 0) { close(master); return false; }
    *session = (c1_terminal_session){.master_fd = master, .child_pid = getpid(),
        .shell_pid = child, .control_fd = -1, .state = C1_TERMINAL_RUNNING, .direct_exec = true};
    struct pollfd ready = {master, POLLIN, 0};
    char byte;
    if (poll(&ready, 1U, 2000) > 0 && read(master, &byte, 1U) == 1 && byte == 'R') return true;
    kill(child, SIGKILL); (void)waitpid(child, NULL, 0); close(master);
    return false;
}

static void stop_shift_fixture(c1_terminal_session *session)
{
    kill(session->shell_pid, SIGKILL);
    while (waitpid(session->shell_pid, NULL, 0) < 0 && errno == EINTR) {}
    close(session->master_fd);
}

static void test_shift_identity(void)
{
    c1_ui_state state = c1_ui_initial_state();
    c1_terminal_session app, user = {0};
    state.page = C1_UI_PAGE_TERMINAL;
    state.terminal_action = C1_UI_ACTION_TERMINAL_APP;
    if (!start_shift_fixture(&app, "gui", NULL)) {
        expect(false, "start real GUI identity/controlling-PTY fixture"); return;
    }
    expect(terminal_is_pkg_gui(&state, &app, &app, true),
           "actual installed executable with gui argv is the owned foreground application");
    expect(!terminal_is_pkg_gui(&state, &app, &user, true) &&
               !terminal_is_pkg_gui(&state, &app, &app, false),
           "even real c1pkg gui in user terminal is ineligible for management injection");
    app.direct_exec = false;
    expect(!terminal_is_pkg_gui(&state, &app, &app, true), "shell command sessions are never GUI targets");
    app.direct_exec = true;
    pid_t actual = app.shell_pid;
    app.shell_pid = getpid();
    expect(!terminal_is_pkg_gui(&state, &app, &app, true), "nonforeground recorded process is rejected");
    app.shell_pid = actual;
    app.child_pid = -1;
    expect(!terminal_is_pkg_gui(&state, &app, &app, true), "missing owned supervisor is rejected");
    app.child_pid = getpid();
    app.suspended = true;
    expect(!terminal_is_pkg_gui(&state, &app, &app, true), "suspended application is not an input target");
    app.suspended = false;
    app.state = C1_TERMINAL_EXITED;
    expect(!terminal_is_pkg_gui(&state, &app, &app, true), "ended session cannot accept management input");
    app.state = C1_TERMINAL_RUNNING;
    state.page = C1_UI_PAGE_DESKTOP;
    expect(!terminal_is_pkg_gui(&state, &app, &app, true), "hidden application page cannot receive a tap");
    state.page = C1_UI_PAGE_TERMINAL;
    state.terminal_symbol_picker = true;
    expect(!terminal_is_pkg_gui(&state, &app, &app, true), "symbol overlay is not GUI focus");
    state.terminal_symbol_picker = false;
    expect(write(app.master_fd, "X", 1U) == 1, "GUI fixture launches a third-party terminal via same-PID exec");
    int64_t deadline = monotonic_milliseconds() + 2000;
    while (terminal_is_pkg_gui(&state, &app, &app, true) && monotonic_milliseconds() < deadline)
        (void)poll(NULL, 0U, 1);
    expect(!terminal_is_pkg_gui(&state, &app, &app, true) &&
               tcgetpgrp(app.master_fd) == actual && state.terminal_action == C1_UI_ACTION_TERMINAL_APP,
           "same PID/foreground/app action after GUI exec never identifies the third-party terminal as GUI");
    unsigned writes_before = shift_writes;
    bool changed;
    (void)apply_shift_tap(&state, &app, terminal_is_pkg_gui(&state, &app, &app, true), &changed);
    expect(shift_writes == writes_before, "third-party terminal gets no management sequence");
    stop_shift_fixture(&app);

    static const char *modes[] = {"run", "tui-prepared", "gui-other", "gui"};
    for (size_t i = 0; i < sizeof(modes) / sizeof(*modes); ++i) {
        if (!start_shift_fixture(&app, modes[i], i == 3U ? "extra" : NULL)) {
            expect(false, "start non-GUI argv fixture"); continue;
        }
        expect(!terminal_is_pkg_gui(&state, &app, &app, true),
               "same executable with run/update/prefix/extra argv is not c1pkg gui");
        stop_shift_fixture(&app);
    }
}

int main(int argc, char **argv)
{
    (void)argv;
    if (argc > 1) return shift_fixture_child();
    char root[] = "/tmp/c1-ui-lifecycle-XXXXXX";
    if (mkdtemp(root) == NULL) return 1;
    snprintf(run_path, sizeof(run_path), "%s/run", root);
    snprintf(lease_path, sizeof(lease_path), "%s/lease", root);
    snprintf(mode_path, sizeof(mode_path), "%s/mode", root);
    snprintf(guard_path, sizeof(guard_path), "%s/guard", root);
    test_shift_events();
    test_shift_identity();
    test_publication();
    test_shutdown_guard();
    test_shutdown_lock_state();
    test_power_modes_runtime();
    test_health_retry();
    test_desktop_update_entry();
    test_daily_quote_runtime();
    test_wifi_parent_activity();
    test_wifi_progress_pipe();
    test_wifi_worker_records();
    test_wifi_cooperative_shutdown();
    unlink(run_path); unlink(lease_path); unlink(mode_path); unlink(guard_path); rmdir(root);
    if (failures) return 1;
    puts("all UI lifecycle tests passed");
    return 0;
}
