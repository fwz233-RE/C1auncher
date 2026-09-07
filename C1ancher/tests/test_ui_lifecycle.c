#define _DEFAULT_SOURCE 1
static char run_path[256], lease_path[256], mode_path[256], guard_path[256];
#define C1_APP_RUN_PATH run_path
#define C1_APP_LEASE_PATH lease_path
#define C1_APP_MODE_PATH mode_path
#define C1_APP_MODE_GUARD_PATH guard_path
#include "../src/platform/app_lease.c"
#include "../src/hal/linux/ui_runtime.c"

/* Hardware-free tests exercise the actual UI publication/render/health logic;
 * unused runtime sections are discarded by the host linker. */
static unsigned int frame_writes, probe_calls, probe_timeout;
static int probe_result = -1;
static int failures;
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
    state->phase = C1_UPDATE_PENDING_BOOT;
    return 0;
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

static void test_desktop_update_entry(void)
{
    c1_ui_state state = {0};
    c1_ui_transition transition = {0};
    c1_service_worker worker;
    state.page = C1_UI_PAGE_DESKTOP;
    service_worker_init(&worker);
    transition.action = C1_UI_ACTION_NONE;
    expect(ignore_desktop_update(&state, C1_UI_EVENT_ENTER, &transition, &worker),
           "offline desktop confirm is skipped before redraw or terminal startup");
    transition.action = C1_UI_ACTION_UPDATE_REFRESH;
    expect(!ignore_desktop_update(&state, C1_UI_EVENT_ENTER, &transition, &worker),
           "online background update check is accepted while idle");
    expect(terminal_action_command(transition.action) == NULL,
           "background update refresh cannot create a terminal session");
    worker.pid = 123;
    expect(ignore_desktop_update(&state, C1_UI_EVENT_ENTER, &transition, &worker),
           "repeated confirm cannot queue duplicate background checks");
    transition.action = C1_UI_ACTION_TERMINAL_UPDATE;
    expect(ignore_desktop_update(&state, C1_UI_EVENT_ENTER, &transition, &worker),
           "prepared state observed before worker completion cannot open a racing terminal");
    service_worker_init(&worker);
    expect(!ignore_desktop_update(&state, C1_UI_EVENT_ENTER, &transition, &worker),
           "prepared online confirmation is accepted after worker completion");
    expect(strstr(terminal_action_command(transition.action), " tui-prepared\r") != NULL,
           "homepage confirmation uses a prepared-only command that never downloads");
    state.page = C1_UI_PAGE_TERMINAL;
    expect(!ignore_desktop_update(&state, C1_UI_EVENT_ENTER, &transition, &worker),
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
    c1_ui_clear_secret(&state);
    finish_wifi_interaction(&state, &result);
    expect(state.page == C1_UI_PAGE_WIFI && state.secret_length == 0U,
           "saved credential failure reports on Wi-Fi page without inventing a password prompt");
    snprintf(state.secret, sizeof(state.secret), "late-password");
    state.secret_length = strlen(state.secret);
    state.page = C1_UI_PAGE_DESKTOP;
    finish_wifi_interaction(&state, &result);
    expect(state.page == C1_UI_PAGE_DESKTOP && state.secret_length == 0U,
           "late connection result cannot drag user back from home or retain a password");
    service_notice[0] = '\0';
}

int main(void)
{
    char root[] = "/tmp/c1-ui-lifecycle-XXXXXX";
    if (mkdtemp(root) == NULL) return 1;
    snprintf(run_path, sizeof(run_path), "%s/run", root);
    snprintf(lease_path, sizeof(lease_path), "%s/lease", root);
    snprintf(mode_path, sizeof(mode_path), "%s/mode", root);
    snprintf(guard_path, sizeof(guard_path), "%s/guard", root);
    test_publication();
    test_health_retry();
    test_desktop_update_entry();
    test_wifi_parent_activity();
    test_wifi_progress_pipe();
    test_wifi_worker_records();
    test_wifi_cooperative_shutdown();
    unlink(run_path); unlink(lease_path); unlink(mode_path); unlink(guard_path); rmdir(root);
    if (failures) return 1;
    puts("all UI lifecycle tests passed");
    return 0;
}
