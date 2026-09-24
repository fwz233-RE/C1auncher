#define _DEFAULT_SOURCE 1

#include "hal/linux/ui.h"

#include "display/frame.h"
#include "core/power_policy.h"
#include "core/power_key.h"
#include "hal/linux/poweroff.h"
#include "hal/linux/display.h"
#include "hal/linux/led.h"
#include "hal/linux/power.h"
#include "hal/linux/system_state.h"
#include "services/terminal.h"
#include "services/time_sync.h"
#include "services/input_service.h"
#include "services/desktop_data.h"
#include "services/desktop_jobs.h"
#include "services/battery.h"
#include "services/wifi.h"
#include "platform/liveness.h"
#include "platform/shutdown.h"
#include "platform/app_lease.h"
#include "platform/stop.h"
#include "platform/update_request.h"
#include "platform/update_health.h"
#include "update/update.h"
#include "ui/chrome.h"
#include "ui/model.h"
#include "ui/input_method.h"
#include "ui/render.h"
#include "ui/terminal_screen.h"
#include "ui/wallpaper.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define C1_UI_INPUT_COUNT 2U
#define C1_UI_STATUS_INTERVAL_MS 60000
#define C1_BATTERY_SAMPLE_INTERVAL_MS 10000
#define C1_EXTERNAL_POWER_INTERVAL_MS 5000
#define C1_TERMINAL_RENDER_DELAY_MS 150
#define C1_TERMINAL_READ_BUDGET 4096U
#define C1_TERMINAL_REPEAT_DELAY_MS 450
#define C1_TERMINAL_REPEAT_INTERVAL_MS 90
#define C1_LED_SYSFS_ROOT "/sys/class/leds"
#ifndef C1_PKG_EXECUTABLE
#define C1_PKG_EXECUTABLE "/usr/data/c1/bin/c1pkg"
#endif
#define C1_TERMINAL_APP_COMMAND "clear; " C1_PKG_EXECUTABLE " gui\r"
#define C1_UPDATER_EXECUTABLE "/usr/data/c1/bin/c1updater"
#define C1_TERMINAL_UPDATE_COMMAND "clear; " C1_UPDATER_EXECUTABLE " tui-prepared\r"
#define C1_DESKTOP_BATTERY_HISTORY "/usr/data/c1/battery-history.cache"

typedef struct {
    c1_status status;
    c1_ui_action action;
    bool wifi_snapshot_valid;
    bool progress_only;
    c1_wifi_phase phase;
    c1_wifi_snapshot wifi;
} c1_service_result;
_Static_assert(sizeof(c1_service_result) <= PIPE_BUF, "service records must be atomic pipe writes");

typedef struct {
    int fd;
    int cancel_fd;
    pid_t pid;
    c1_ui_action action;
} c1_service_worker;

/* Parent-owned activity: forked service state is not shared with the UI. */
static c1_ui_action visible_service_action = C1_UI_ACTION_NONE;
static c1_wifi_phase visible_wifi_phase = C1_WIFI_PHASE_IDLE;
static bool wifi_stop_pending;
static char service_notice[C1_UI_MESSAGE_CAPACITY];
static c1_preferences desktop_preferences;
static c1_time_sync network_clock;
static bool clock_online;
static c1_desktop_data desktop_summary;
static c1_desktop_job desktop_job;
static unsigned desktop_new_apps;
static uint64_t remote_update_sequence;
static bool remote_update_available;
static c1_battery_history battery_history;

static void sample_battery_history(int64_t now)
{
    uint32_t percent = 0;
    bool online = false;
    bool available = c1_linux_battery_read(&percent);
    bool known = c1_linux_external_power_read(&online);
    c1_battery_power power = !known ? C1_BATTERY_POWER_UNKNOWN :
                              online ? C1_BATTERY_PLUGGED : C1_BATTERY_DISCHARGING;
    if (c1_battery_history_observe(&battery_history, (int64_t)time(NULL), now,
                                   available, percent, power))
        (void)c1_battery_history_save(C1_DESKTOP_BATTERY_HISTORY, &battery_history);
}

static void reload_desktop_summary(void)
{
    c1_desktop_data data, seen;
    if (!c1_desktop_load(C1_DESKTOP_CACHE, &data)) return;
    desktop_summary = data;
    if (!c1_desktop_load(C1_DESKTOP_SEEN, &seen) || strcmp(seen.source, data.source)) {
        /* First use establishes a baseline without announcing every old app. */
        if (!c1_app_run_active()) (void)c1_desktop_save(C1_DESKTOP_SEEN, &data);
        desktop_new_apps = 0;
    } else desktop_new_apps = c1_desktop_new_count(&data, &seen);
}
static c1_input_method desktop_input = {.client = C1_IME_CLIENT_INIT, .deadline = -1};
static struct c1_ime_response input_view;
static c1_ui_page input_focus = C1_UI_PAGE_DESKTOP;
static bool input_terminal_allowed;

static void input_focus_update(const c1_ui_state *state)
{
    bool allowed = state->page == C1_UI_PAGE_LOCK_TEXT ||
        (state->page == C1_UI_PAGE_TERMINAL && input_terminal_allowed && !c1_app_run_active());
    if (!allowed || input_focus != state->page) {
        c1_input_method_close(&desktop_input);
        memset(&input_view, 0, sizeof(input_view));
    }
    input_focus = state->page;
}

static void terminal_geometry(const c1_ui_state *state, unsigned int *columns, unsigned int *rows)
{
    (void)state;
    *columns = C1_CHROME_TERMINAL_COLUMNS;
    *rows = input_terminal_allowed && (desktop_input.enabled || desktop_input.failed)
        ? C1_CHROME_TERMINAL_INPUT_ROWS : C1_CHROME_TERMINAL_ROWS;
}


static c1_status start_desktop_terminal(c1_terminal_session *session, c1_terminal_screen *screen,
                                        const c1_ui_state *state, char *const argv[])
{
    unsigned columns, rows;
    terminal_geometry(state, &columns, &rows);
    c1_status result = c1_terminal_screen_resize(screen, columns, rows);
    if (result != C1_STATUS_OK) return result;
    return argv ? c1_terminal_start_exec(session, columns, rows, argv[0], argv) :
                  c1_terminal_start(session, columns, rows);
}

static void service_worker_init(c1_service_worker *worker)
{
    worker->fd = -1;
    worker->cancel_fd = -1;
    worker->pid = -1;
    worker->action = C1_UI_ACTION_NONE;
}

static void service_worker_stop(c1_service_worker *worker)
{
    if (worker->cancel_fd >= 0) { close(worker->cancel_fd); worker->cancel_fd = -1; }
    /* Give Wi-Fi its cooperative rollback budget before tearing down the
     * result pipe or signalling the process group during UI shutdown. */
    if (worker->pid > 0 && (worker->action == C1_UI_ACTION_WIFI_CONNECT ||
                            worker->action == C1_UI_ACTION_WIFI_SCAN)) {
        for (unsigned int tick = 0; tick < 650U; ++tick) {
            c1_service_result discarded;
            if (worker->fd >= 0) {
                ssize_t drained = read(worker->fd, &discarded, sizeof(discarded));
                if (drained == 0 || (drained < 0 && errno != EAGAIN && errno != EINTR)) {
                    close(worker->fd);
                    worker->fd = -1;
                }
            }
            pid_t result = waitpid(worker->pid, NULL, WNOHANG);
            if (result == worker->pid || (result < 0 && errno == ECHILD)) {
                worker->pid = -1;
                break;
            }
            struct timespec pause = {0, 20000000L};
            (void)nanosleep(&pause, NULL);
        }
    }
    if (worker->fd >= 0) {
        close(worker->fd);
        worker->fd = -1;
    }
    if (worker->pid > 0) {
        int status;
        unsigned int tick;
        pid_t result = 0;

        if (kill(-worker->pid, SIGTERM) != 0) {
            (void)kill(worker->pid, SIGTERM);
        }
        for (tick = 0U; tick < 50U; ++tick) {
            result = waitpid(worker->pid, &status, WNOHANG);
            if (result == worker->pid || (result < 0 && errno == ECHILD)) {
                break;
            }
            if (result < 0 && errno != EINTR) {
                break;
            }
            {
                struct timespec pause = {0, 20000000L};
                (void)nanosleep(&pause, NULL);
            }
        }
        if (result == 0) {
            (void)kill(-worker->pid, SIGKILL);
            while (waitpid(worker->pid, &status, 0) < 0 && errno == EINTR) {
            }
        }
        worker->pid = -1;
    }
    worker->action = C1_UI_ACTION_NONE;
}

static int64_t monotonic_milliseconds(void)
{
    struct timespec value;

    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) {
        return -1;
    }
    return (int64_t)value.tv_sec * 1000 + value.tv_nsec / 1000000;
}

static void update_external_power(c1_power_policy *policy, int64_t now)
{
    bool online = false;
    bool known = c1_linux_external_power_read(&online);

    c1_power_policy_set_external_power(policy, known, online, now);
}

static int deadline_timeout(int timeout, int64_t deadline, int64_t now)
{
    int64_t remaining;

    if (deadline < 0) {
        return timeout;
    }
    remaining = deadline - now;
    if (remaining < 0) {
        remaining = 0;
    }
    if (remaining > INT32_MAX) {
        remaining = INT32_MAX;
    }
    return timeout < 0 || remaining < timeout ? (int)remaining : timeout;
}

static c1_ui_event map_key(uint16_t code)
{
    switch (code) {
    case KEY_UP:
        return C1_UI_EVENT_UP;
    case KEY_DOWN:
        return C1_UI_EVENT_DOWN;
    case KEY_LEFT:
        return C1_UI_EVENT_LEFT;
    case KEY_RIGHT:
        return C1_UI_EVENT_RIGHT;
    case KEY_ENTER:
    case KEY_OK:
        return C1_UI_EVENT_ENTER;
    case KEY_BACK:
        return C1_UI_EVENT_BACK;
    case KEY_HOME:
        return C1_UI_EVENT_HOME;
    default:
        return C1_UI_EVENT_NONE;
    }
}

static c1_ui_event map_page_key(c1_ui_page page, uint16_t code)
{
    if (page == C1_UI_PAGE_BATTERY) {
        if (code == KEY_VOLUMEDOWN) return C1_UI_EVENT_VIEW_PREVIOUS;
        if (code == KEY_VOLUMEUP) return C1_UI_EVENT_VIEW_NEXT;
    }
    return map_key(code);
}

static char physical_letter(uint16_t code)
{
    static const struct {
        uint16_t code;
        char letter;
    } letters[] = {
        {KEY_A, 'a'}, {KEY_B, 'b'}, {KEY_C, 'c'}, {KEY_D, 'd'}, {KEY_E, 'e'},
        {KEY_F, 'f'}, {KEY_G, 'g'}, {KEY_H, 'h'}, {KEY_I, 'i'}, {KEY_J, 'j'},
        {KEY_K, 'k'}, {KEY_L, 'l'}, {KEY_M, 'm'}, {KEY_N, 'n'}, {KEY_O, 'o'},
        {KEY_P, 'p'}, {KEY_Q, 'q'}, {KEY_R, 'r'}, {KEY_S, 's'}, {KEY_T, 't'},
        {KEY_U, 'u'}, {KEY_V, 'v'}, {KEY_W, 'w'}, {KEY_X, 'x'}, {KEY_Y, 'y'},
        {KEY_Z, 'z'}
    };
    size_t index;

    for (index = 0U; index < sizeof(letters) / sizeof(letters[0]); ++index) {
        if (letters[index].code == code) {
            return letters[index].letter;
        }
    }
    return '\0';
}

static bool apply_physical_secret_key(c1_ui_state *state, uint16_t code, bool shift_chord)
{
    char letter = physical_letter(code);

    if (letter != '\0') {
        return c1_ui_secret_append(
            state,
            c1_ui_physical_character(state->keyboard_layer, letter, shift_chord));
    }
    if (code == KEY_SPACE) {
        return c1_ui_secret_append(state, ' ');
    }
    if (code == KEY_DELETE) {
        return c1_ui_secret_delete(state);
    }
    return false;
}

static bool states_equal(const c1_ui_state *left, const c1_ui_state *right)
{
    return left->page == right->page && left->selection == right->selection &&
           left->battery_view == right->battery_view &&
           left->battery_selected_at == right->battery_selected_at &&
           left->symbol_selection == right->symbol_selection &&
           left->keyboard_layer == right->keyboard_layer &&
           left->terminal_symbol_picker == right->terminal_symbol_picker &&
           left->terminal_action == right->terminal_action &&
           left->desktop_selection == right->desktop_selection &&
           strcmp(left->lock_text_draft, right->lock_text_draft) == 0 &&
           left->lock_text_cursor == right->lock_text_cursor &&
           memcmp(&left->preferences, &right->preferences, sizeof(left->preferences)) == 0 &&
           left->secret_length == right->secret_length &&
           left->secret_visible == right->secret_visible &&
           left->selected_security == right->selected_security &&
           left->selected_saved == right->selected_saved &&
           strcmp(left->wifi_notice, right->wifi_notice) == 0 &&
           strcmp(left->selected_ssid, right->selected_ssid) == 0 &&
           strcmp(left->secret, right->secret) == 0;
}

static c1_status emit_navigation(c1_record_sink sink,
                                 uint16_t key_code,
                                 c1_ui_event event,
                                 c1_ui_state state)
{
    c1_record record;

    c1_record_init(&record, "ui", "navigation");
    if (c1_record_add_integer(&record, "key_code", key_code) != C1_STATUS_OK ||
        c1_record_add_integer(&record, "ui_event", event) != C1_STATUS_OK ||
        c1_record_add_integer(&record, "page", state.page) != C1_STATUS_OK ||
        c1_record_add_integer(&record, "selection", state.selection) != C1_STATUS_OK) {
        return C1_STATUS_INVALID_ARGUMENT;
    }
    return c1_record_emit(sink, &record);
}

static c1_status emit_runtime_stats(c1_record_sink sink,
                                    uint64_t poll_calls,
                                    uint64_t poll_events,
                                    uint64_t poll_timeouts)
{
    c1_linux_display_stats display;
    c1_record record;

    c1_linux_display_get_stats(&display);
    c1_record_init(&record, "power", "runtime_stats");
    if (c1_record_add_integer(&record, "poll_calls", (int64_t)poll_calls) != C1_STATUS_OK ||
        c1_record_add_integer(&record, "poll_events", (int64_t)poll_events) != C1_STATUS_OK ||
        c1_record_add_integer(&record, "poll_timeouts", (int64_t)poll_timeouts) != C1_STATUS_OK ||
        c1_record_add_integer(&record, "display_writes", (int64_t)display.writes) != C1_STATUS_OK ||
        c1_record_add_integer(&record, "full_refreshes", (int64_t)display.full_refreshes) != C1_STATUS_OK ||
        c1_record_add_integer(&record, "unchanged_skips", (int64_t)display.unchanged_skips) != C1_STATUS_OK ||
        c1_record_add_integer(&record, "lease_skips", (int64_t)display.lease_skips) != C1_STATUS_OK) {
        return C1_STATUS_INVALID_ARGUMENT;
    }
    return c1_record_emit(sink, &record);
}

static void merge_service_status(c1_ui_status *status)
{
    c1_wifi_snapshot wifi;
    size_t index;

    if (c1_wifi_read_snapshot(&wifi)) {
        status->wifi_enabled = wifi.state != C1_WIFI_DISABLED;
        status->wifi_busy = visible_service_action == C1_UI_ACTION_WIFI_SCAN ||
                            visible_service_action == C1_UI_ACTION_WIFI_CONNECT ||
                            visible_service_action == C1_UI_ACTION_WIFI_DISABLE;
        status->service_busy = visible_service_action != C1_UI_ACTION_NONE;
        status->wifi_activity = visible_service_action;
        status->wifi_phase = visible_wifi_phase;
        status->wifi_stop_pending = wifi_stop_pending;
        status->wifi_connected = wifi.state == C1_WIFI_CONNECTED;
        status->network_count = wifi.network_count;
        for (index = 0U; index < wifi.network_count && index < C1_UI_MAX_NETWORKS; ++index) {
            snprintf(status->networks[index].ssid,
                     sizeof(status->networks[index].ssid),
                     "%s",
                     wifi.networks[index].ssid);
            status->networks[index].signal_dbm = wifi.networks[index].signal_dbm;
            status->networks[index].secured = wifi.networks[index].secured;
            status->networks[index].security = wifi.networks[index].security;
            status->networks[index].saved = wifi.networks[index].saved;
        }
        snprintf(status->wifi_connected_ssid,
                 sizeof(status->wifi_connected_ssid),
                 "%s",
                 wifi.connected_ssid);
        snprintf(status->wifi_ipv4, sizeof(status->wifi_ipv4), "%s", wifi.ipv4);
        snprintf(status->wifi_message, sizeof(status->wifi_message), "%s",
                 wifi_stop_pending ? "STOP QUEUED; WAITING FOR CURRENT TASK" :
                 service_notice[0] != '\0' ? service_notice : wifi.error);
    }
}

static void merge_update_status(c1_ui_status *status)
{
    struct c1_update_state update_state;
    char error[C1_UPDATE_ERROR_MAX] = "";

    status->update_available = false;
    status->update_prepared = false;
    if (c1_update_state_load(C1_UPDATE_DEFAULT_STATE_ROOT, &update_state,
                             error, sizeof(error)) == 0) {
        status->update_prepared = update_state.phase == C1_UPDATE_PREPARED;
        status->update_available = status->update_prepared ||
            (remote_update_available && remote_update_sequence > update_state.sequence);
    }
}

static bool read_ui_status(c1_ui_status *status)
{
    if (!c1_linux_system_status_read(status)) {
        return false;
    }
    merge_service_status(status);
    clock_online = status->wifi_connected && status->wifi_ipv4[0];
    merge_update_status(status);
    status->new_applications = desktop_new_apps;
    /* quote_zh is the authoritative original. quote_en remains only as the
     * legacy protocol/cache slot and must not change the daily quote with UI
     * language, including when an older cache contains a translation. */
    snprintf(status->daily_quote, sizeof(status->daily_quote), "%s", desktop_summary.quote_zh);
    status->time_sync_running = network_clock.state == C1_TIME_RUNNING;
    status->time_sync_ok = network_clock.state == C1_TIME_SYNCED;
    time_t wall = time(NULL);
    time_t adjusted = wall + desktop_preferences.utc_offset_minutes * 60;
    struct tm date;
    status->time_available = wall >= 1704067200 && gmtime_r(&adjusted, &date) != NULL;
    if (status->time_available) {
        status->hour = (uint32_t)date.tm_hour;
        status->minute = (uint32_t)date.tm_min;
        status->year = (uint32_t)date.tm_year + 1900U;
        status->month = (uint32_t)date.tm_mon + 1U;
        status->day = (uint32_t)date.tm_mday;
        status->weekday = (uint32_t)date.tm_wday;
    }
    status->battery_history = battery_history;
    status->battery_history_now = (int64_t)wall;
    return true;
}

static c1_status render_state(c1_ui_state state,
                              const c1_ui_status *system_status,
                              c1_terminal_screen *terminal,
                              bool full_refresh,
                              c1_record_sink sink)
{
    uint8_t frame[C1_DISPLAY_FRAME_BYTES];

    input_focus_update(&state);
    if (state.page == C1_UI_PAGE_LOCK && state.frozen_lock) return C1_STATUS_OK;
    if (c1_app_run_active() && !c1_app_lease_terminal_mode()) return C1_STATUS_OK;
    /* Reload only when drawing the lock screen, never by polling during sleep. */
    if (state.page == C1_UI_PAGE_LOCK) c1_wallpaper_load();
    c1_ui_render(frame, &state, system_status, terminal);
    c1_ui_render_input(frame, &state, &input_view, desktop_input.enabled, desktop_input.failed);
    if (full_refresh) {
        return c1_linux_display_write_frame(NULL, frame, sizeof(frame), sink);
    }
    return c1_linux_display_write_frame_fast(NULL, frame, sizeof(frame), sink);
}

static c1_status render_current(c1_ui_state state,
                                c1_terminal_screen *terminal,
                                bool full_refresh,
                                c1_record_sink sink)
{
    c1_ui_status system_status;

    if (!read_ui_status(&system_status)) {
        return C1_STATUS_UNAVAILABLE;
    }
    return render_state(state, &system_status, terminal, full_refresh, sink);
}

typedef struct {
    bool finished;
    unsigned int attempts;
    int64_t deadline;
    int64_t next_attempt;
} c1_ui_health_retry;

static void try_update_health(c1_ui_health_retry *retry, int64_t now)
{
    struct c1_update_state state;
    char error[C1_UPDATE_ERROR_MAX] = "";
    unsigned int timeout;
    if (retry->finished || now < retry->next_attempt || c1_app_run_active()) return;
    if (retry->deadline == 0) retry->deadline = now + 20000;
    if (now >= retry->deadline || retry->attempts >= 4U) {
        retry->finished = true;
        return;
    }
    if (c1_update_state_load(C1_UPDATE_DEFAULT_STATE_ROOT, &state,
                             error, sizeof(error)) != 0) {
        retry->next_attempt = now + 2000;
        return;
    }
    if (!c1_update_health_should_probe(&state)) {
        retry->next_attempt = now + 2000;
        return;
    }
    timeout = 1000U + retry->attempts * 1000U;
    if (timeout > 3000U) timeout = 3000U;
    /* The probe runs three bounded commands. Keep the entire attempt below
     * both the remaining retry budget and the twelve-second heartbeat limit. */
    if ((int64_t)timeout * 3 > retry->deadline - now)
        timeout = (unsigned int)((retry->deadline - now) / 3);
    if (timeout == 0U) { retry->finished = true; return; }
    ++retry->attempts;
    retry->finished = c1_update_health_probe_running(C1_UPDATE_DEFAULT_STATE_ROOT,
                                                     C1_UPDATE_DEFAULT_READY_FILE,
                                                     timeout) == 0;
    retry->next_attempt = monotonic_milliseconds() + 2000;
}

static bool consume_update_request_if_ready(bool lease_active)
{
    struct c1_update_state update_state;
    char error[C1_UPDATE_ERROR_MAX] = "";

    if (lease_active ||
        c1_update_state_load(C1_UPDATE_DEFAULT_STATE_ROOT, &update_state,
                             error, sizeof(error)) != 0 ||
        update_state.phase != C1_UPDATE_PREPARED ||
        c1_update_request_consume(C1_UPDATE_REQUEST_DEFAULT_PATH, update_state.digest,
                                  error, sizeof(error)) != 0) {
        return false;
    }
    return true;
}

static c1_status render_wifi_scanning(c1_ui_state state, c1_record_sink sink)
{
    c1_ui_status system_status;

    if (!read_ui_status(&system_status)) {
        return C1_STATUS_UNAVAILABLE;
    }
    system_status.wifi_busy = true;
    return render_state(state, &system_status, NULL, false, sink);
}

/* Drain queued releases first, then confirm the key with the kernel. This
 * prevents a lost release (for example during EVIOCGRAB) from powering off. */
static c1_power_key_action power_key_timer(c1_power_key *key,
                                           const struct pollfd *inputs, int64_t now)
{
    unsigned char held[(KEY_MAX + 8U) / 8U] = {0};
    int64_t deadline = c1_power_key_deadline(key);
    if (!key->down || (!key->fired && (deadline < 0 || now < deadline)))
        return C1_POWER_KEY_NONE;
    if (key->source >= C1_UI_INPUT_COUNT || key->code > KEY_MAX ||
        ioctl(inputs[key->source].fd, EVIOCGKEY(sizeof(held)), held) < 0 ||
        (held[key->code / 8U] & (1U << (key->code % 8U))) == 0) {
        c1_power_key_reset(key);
        return C1_POWER_KEY_NONE;
    }
    return c1_power_key_tick(key, now);
}

static c1_power_key_action power_key_input(c1_power_key *key, c1_power_policy *policy,
                                           unsigned int source,
                                           const struct input_event *input, int64_t now)
{
    if (input->type != EV_KEY ||
        (input->code != KEY_POWER && input->code != KEY_WAKEUP))
        return C1_POWER_KEY_NONE;
    /* The wake gesture must be released before a fresh shutdown hold. */
    if (c1_power_policy_filter_wakeup(policy, input->value != 0)) {
        c1_power_key_reset(key);
        return C1_POWER_KEY_NONE;
    }
    if (input->value == 1) c1_power_policy_note_activity(policy, now);
    return c1_power_key_event(key, source, input->code, input->value, now);
}

/* Deliver queued input before committing an automatic power deadline. A key
 * arriving at the lock/shutdown boundary must get its normal wake/reset path. */
static c1_power_action automatic_power_action(c1_power_policy *policy,
                                               const c1_power_key *key,
                                               const struct pollfd *inputs,
                                               int64_t now)
{
    if (key->down) return C1_POWER_ACTION_NONE;
    if (c1_power_policy_timeout(policy, now) == 0) {
        struct pollfd pending[C1_UI_INPUT_COUNT];
        for (unsigned i = 0; i < C1_UI_INPUT_COUNT; ++i)
            pending[i] = (struct pollfd){inputs[i].fd, POLLIN, 0};
        /* Readiness/errors are handled by the ordinary input drain below.
         * Never consume keys here or clear the deadline just for an event. */
        if (poll(pending, C1_UI_INPUT_COUNT, 0) != 0) return C1_POWER_ACTION_NONE;
    }
    return c1_power_policy_tick(policy, now);
}

/* Wake consumes this press; it must not type into or navigate the restored
 * page. Power holds are handled separately and become KEY_WAKEUP only after a
 * verified short release. Repeats/releases alone never unlock or reset timers. */
static bool wake_lock_from_key(c1_ui_state *state, c1_power_policy *policy,
                               const struct input_event *input, int64_t now)
{
    if (!state || !policy || !input || now < 0 || state->page != C1_UI_PAGE_LOCK ||
        policy->state != C1_POWER_LOCKED || input->type != EV_KEY || input->value != 1 ||
        input->code == KEY_RESERVED || input->code > KEY_MAX || input->code == KEY_POWER)
        return false;
    if (!c1_ui_unlock(state)) return false;
    return c1_power_policy_unlock(policy, now);
}

static void poweroff_heartbeat(void)
{
    static int last_error;
    int error = c1_shutdown_keepalive();
    if (error != 0 && error != last_error)
        fprintf(stderr, "C1ancher: shutdown supervision renewal failed: %s\n", strerror(error));
    last_error = error;
    c1_liveness_beat(monotonic_milliseconds());
}

static int request_poweroff(c1_record_sink sink)
{
    c1_record record;
    int error;
    c1_record_init(&record, "power", "shutdown-requested");
    (void)c1_record_emit(sink, &record);
    /* Both the launcher and outer supervisor must acknowledge shutdown before
     * init can SIGKILL app_daemon. An exit code alone cannot cover that race. */
    error = c1_shutdown_begin();
    if (error != 0) {
        c1_record_init(&record, "power", "shutdown-supervision-failed");
        (void)c1_record_add_integer(&record, "error", error);
        (void)c1_record_emit(sink, &record);
        fprintf(stderr, "C1ancher: shutdown supervision unavailable: %s\n", strerror(error));
        return error; /* No command without a consumed, authenticated ACK. */
    }
    if (c1_stop_requested()) return EINTR;
    error = c1_linux_poweroff_request(poweroff_heartbeat);
    if (error != 0 && !c1_stop_requested()) {
        int cancel_error = c1_shutdown_cancel();
        if (cancel_error != 0) {
            c1_record_init(&record, "power", "shutdown-cancel-failed");
            (void)c1_record_add_integer(&record, "error", cancel_error);
            (void)c1_record_emit(sink, &record);
            fprintf(stderr, "C1ancher: shutdown cancellation failed: %s\n", strerror(cancel_error));
        }
    }
    c1_record_init(&record, "power", error == 0 ? "shutdown-command-completed" : "shutdown-failed");
    (void)c1_record_add_integer(&record, "error", error);
    (void)c1_record_emit(sink, &record);
    if (error != 0) fprintf(stderr, "C1ancher: shutdown failed: %s\n", strerror(error));
    /* Remain alive until init stops us. Returning from the UI on fork success
     * would let the launcher restart it if exec or shutdown subsequently failed. */
    return error;
}

/* A shutdown request must never expose the desktop between the lock frame and
 * the poweroff command. The launcher also receives a dedicated exit status so
 * a SIGTERM from the shutdown sequence cannot make it restart the UI. */
static bool prepare_poweroff_lock(c1_ui_state *state,
                                  c1_power_policy *policy,
                                  int64_t now)
{
    if (state == NULL || policy == NULL || now < 0) return false;
    if (policy->state == C1_POWER_ACTIVE && !c1_power_policy_lock(policy, now)) return false;
    if (policy->state != C1_POWER_LOCKED) return false;
    if (state->page == C1_UI_PAGE_LOCK) return true;
    return c1_ui_enter_lock(state);
}

static bool request_poweroff_locked(c1_ui_state *state,
                                    c1_power_policy *policy,
                                    c1_terminal_screen *screen,
                                    c1_record_sink sink)
{
    c1_ui_state previous_state;
    c1_power_policy previous_policy;
    int64_t now = monotonic_milliseconds();
    int error;

    if (state == NULL || policy == NULL || screen == NULL || now < 0) return false;
    previous_state = *state;
    previous_policy = *policy;
    /* A visible lock frame is already the final shutdown image. Do not read
     * status, reload wallpaper or refresh the display just to power off. */
    bool was_locked = state->page == C1_UI_PAGE_LOCK;
    if (!prepare_poweroff_lock(state, policy, now) ||
        (!was_locked && render_current(*state, screen, true, sink) != C1_STATUS_OK)) {
        *state = previous_state;
        *policy = previous_policy;
        return false;
    }
    error = request_poweroff(sink);
    if (error != 0 && !c1_stop_requested()) {
        *state = previous_state;
        *policy = previous_policy;
        if (!was_locked) (void)render_current(*state, screen, true, sink);
        return false;
    }
    return true;
}

static c1_status wait_for_poweroff(void)
{
    /* After init accepts shutdown, only keep the watchdog alive until its
     * stop signal. Input, late worker/PTY exits and update requests must not
     * redraw home, trigger another power action or return a restartable error. */
    while (!c1_stop_requested()) {
        poweroff_heartbeat();
        (void)poll(NULL, 0U, 1000);
    }
    return C1_STATUS_SHUTDOWN_REQUESTED;
}

static bool automatic_suspend_safe(const c1_service_worker *worker)
{
    struct c1_update_state update;
    char error[C1_UPDATE_ERROR_MAX] = "";
    if (worker->pid > 0 || desktop_job.pid > 0 || network_clock.pid > 0 ||
        c1_app_run_active() || c1_app_lease_active()) return false;
    if (c1_update_state_load(C1_UPDATE_DEFAULT_STATE_ROOT, &update, error, sizeof(error)) != 0)
        return false; /* Unknown update state is not permission for a power action. */
    return update.phase == C1_UPDATE_IDLE || update.phase == C1_UPDATE_CONFIRMED ||
           update.phase == C1_UPDATE_PREPARED;
}

static bool automatic_shutdown_safe(const c1_service_worker *worker,
                                     const c1_terminal_session *user,
                                     const c1_terminal_session *app)
{
    /* Suspend can restore the current terminal; shutdown cannot preserve it.
     * Never stop user applications just to make the idle deadline achievable. */
    return user->child_pid <= 0 && app->child_pid <= 0 && automatic_suspend_safe(worker);
}

static const char *terminal_action_command(c1_ui_action action)
{
    switch (action) {
    case C1_UI_ACTION_TERMINAL_APP:
        return C1_TERMINAL_APP_COMMAND;
    case C1_UI_ACTION_TERMINAL_UPDATE:
        return C1_TERMINAL_UPDATE_COMMAND;
    default:
        return NULL;
    }
}

static c1_status refresh_update(void)
{
    int child_status;
    pid_t child = fork();
    pid_t waited;

    if (child < 0) {
        return C1_STATUS_IO_ERROR;
    }
    if (child == 0) {
        execl(C1_UPDATER_EXECUTABLE,
              C1_UPDATER_EXECUTABLE,
              "prepare-configured",
              C1_UPDATE_DEFAULT_STAGING_ROOT,
              C1_UPDATE_DEFAULT_CORE_ROOT,
              C1_UPDATE_DEFAULT_STATE_ROOT,
              C1_UPDATE_DEFAULT_KEY,
              (char *)NULL);
        _exit(127);
    }
    do {
        waited = waitpid(child, &child_status, 0);
    } while (waited < 0 && errno == EINTR);
    if (waited != child) {
        return C1_STATUS_IO_ERROR;
    }
    return WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0
               ? C1_STATUS_OK
               : C1_STATUS_UNAVAILABLE;
}

static void run_service_action(c1_ui_action action,
                               const c1_ui_state *state,
                               c1_service_result *result,
                               const c1_wifi_operation_options *options)
{
    memset(result, 0, sizeof(*result));
    result->action = action;
    switch (action) {
    case C1_UI_ACTION_WIFI_SCAN:
        result->status = c1_wifi_scan_ex(options, &result->wifi);
        break;
    case C1_UI_ACTION_WIFI_CONNECT:
        result->status = state->selected_saved
            ? c1_wifi_connect_saved_ex(state->selected_ssid, state->selected_security, options, &result->wifi)
            : c1_wifi_connect_ex(state->selected_ssid, state->secret,
                                 state->selected_security, options, &result->wifi);
        break;
    case C1_UI_ACTION_WIFI_DISABLE:
        result->status = c1_wifi_disable(&result->wifi);
        break;
    case C1_UI_ACTION_UPDATE_REFRESH:
        result->status = refresh_update();
        break;
    case C1_UI_ACTION_TERMINAL_APP:
    case C1_UI_ACTION_TERMINAL_UPDATE:
    case C1_UI_ACTION_NONE:
        result->status = C1_STATUS_OK;
        break;
    }
}

typedef struct {
    int output_fd, cancel_fd;
    c1_ui_action action;
    c1_wifi_phase phase;
    bool cancelled;
} c1_wifi_worker_progress;

static bool wifi_worker_progress(c1_wifi_phase phase, void *context)
{
    c1_wifi_worker_progress *progress = context;
    char byte;
    ssize_t count = read(progress->cancel_fd, &byte, 1U);
    if (count == 0 || count > 0) progress->cancelled = true;
    if (phase != progress->phase) {
        c1_service_result message = {0};
        message.action = progress->action;
        message.progress_only = true;
        message.phase = phase;
        /* A phase record fits PIPE_BUF. At most one is sent per phase, not
         * per polling tick, avoiding animation/redraw traffic on e-paper. */
        ssize_t written;
        do { written = write(progress->output_fd, &message, sizeof(message)); }
        while (written < 0 && errno == EINTR);
        if (written != (ssize_t)sizeof(message)) return false;
        progress->phase = phase;
    }
    return !progress->cancelled;
}

static c1_status service_worker_start(c1_service_worker *worker,
                                      c1_ui_action action,
                                      const c1_ui_state *state)
{
    int descriptors[2], cancellation[2];
    pid_t child;

    if (worker == NULL || state == NULL || worker->pid > 0 ||
        action == C1_UI_ACTION_NONE || terminal_action_command(action) != NULL) {
        return C1_STATUS_INVALID_ARGUMENT;
    }
    if (pipe(descriptors) != 0) {
        return C1_STATUS_IO_ERROR;
    }
    if (pipe(cancellation) != 0) {
        close(descriptors[0]); close(descriptors[1]);
        return C1_STATUS_IO_ERROR;
    }
    (void)fcntl(cancellation[0], F_SETFD, FD_CLOEXEC);
    (void)fcntl(cancellation[1], F_SETFD, FD_CLOEXEC);
    if (fcntl(cancellation[0], F_SETFL, O_NONBLOCK) != 0 ||
        fcntl(descriptors[0], F_SETFL, O_NONBLOCK) != 0) {
        close(descriptors[0]); close(descriptors[1]);
        close(cancellation[0]); close(cancellation[1]);
        return C1_STATUS_IO_ERROR;
    }
    (void)fcntl(descriptors[0], F_SETFD, FD_CLOEXEC);
    (void)fcntl(descriptors[1], F_SETFD, FD_CLOEXEC);
    child = fork();
    if (child < 0) {
        close(descriptors[0]);
        close(descriptors[1]);
        close(cancellation[0]); close(cancellation[1]);
        return C1_STATUS_IO_ERROR;
    }
    if (child == 0) {
        c1_service_result result;
        long maximum_fd = sysconf(_SC_OPEN_MAX);
        int descriptor;
        const char *bytes;
        size_t remaining;

        close(descriptors[0]);
        close(cancellation[1]);
        if (setpgid(0, 0) != 0) {
            close(descriptors[1]);
            _exit(126);
        }
        (void)signal(SIGTERM, SIG_DFL);
        (void)signal(SIGINT, SIG_DFL);
        (void)signal(SIGHUP, SIG_DFL);
        /* A disappearing UI closes the result pipe. Report EPIPE to the
         * callback so Wi-Fi can roll back instead of dying from SIGPIPE. */
        (void)signal(SIGPIPE, SIG_IGN);
        if (maximum_fd < 0 || maximum_fd > 4096) {
            maximum_fd = 4096;
        }
        for (descriptor = STDERR_FILENO + 1; descriptor < maximum_fd; ++descriptor) {
            if (descriptor != descriptors[1] && descriptor != cancellation[0]) {
                close(descriptor);
            }
        }
        c1_wifi_worker_progress progress = {descriptors[1], cancellation[0], action,
                                            C1_WIFI_PHASE_IDLE, false};
        c1_wifi_operation_options options = {wifi_worker_progress, &progress};
        run_service_action(action, state, &result, &options);
        close(cancellation[0]);
        result.wifi_snapshot_valid = action == C1_UI_ACTION_WIFI_SCAN ||
                                     action == C1_UI_ACTION_WIFI_CONNECT ||
                                     action == C1_UI_ACTION_WIFI_DISABLE;
        bytes = (const char *)&result;
        remaining = sizeof(result);
        while (remaining > 0U) {
            ssize_t count = write(descriptors[1], bytes, remaining);

            if (count > 0) {
                bytes += count;
                remaining -= (size_t)count;
            } else if (count < 0 && errno == EINTR) {
                continue;
            } else {
                break;
            }
        }
        close(descriptors[1]);
        _exit(remaining == 0U ? 0 : 1);
    }
    (void)setpgid(child, child);
    close(descriptors[1]);
    close(cancellation[0]);
    worker->fd = descriptors[0];
    worker->cancel_fd = cancellation[1];
    worker->pid = child;
    worker->action = action;
    visible_service_action = action;
    visible_wifi_phase = C1_WIFI_PHASE_PREPARING;
    service_notice[0] = '\0';
    return C1_STATUS_OK;
}

/* Repeated update confirmation during existing work cannot launch another
 * worker. Offline settings confirmations still render the explicit notice. */
static bool ignore_settings_update(const c1_ui_state *state, c1_ui_event event,
                                   const c1_ui_transition *transition,
                                   const c1_service_worker *worker)
{
    (void)transition;
    return state->page == C1_UI_PAGE_SETTINGS && state->selection == C1_SETTING_UPDATE &&
           event == C1_UI_EVENT_ENTER && worker->pid > 0;
}

static bool service_worker_finish(c1_service_worker *worker,
                                  c1_service_result *result)
{
    ssize_t count;
    int child_status;

    if (worker == NULL || result == NULL || worker->fd < 0 || worker->pid <= 0) {
        return false;
    }
    do {
        count = read(worker->fd, result, sizeof(*result));
    } while (count < 0 && errno == EINTR);
    if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        return false;
    }
    if (count == (ssize_t)sizeof(*result) && result->progress_only) {
        visible_wifi_phase = result->phase;
        return false;
    }
    if (count != (ssize_t)sizeof(*result)) {
        memset(result, 0, sizeof(*result));
        result->action = worker->action;
        result->status = C1_STATUS_IO_ERROR;
    }
    close(worker->fd);
    worker->fd = -1;
    if (worker->cancel_fd >= 0) { close(worker->cancel_fd); worker->cancel_fd = -1; }
    while (waitpid(worker->pid, &child_status, 0) < 0 && errno == EINTR) {
    }
    worker->pid = -1;
    worker->action = C1_UI_ACTION_NONE;
    visible_service_action = C1_UI_ACTION_NONE;
    return true;
}

static c1_status emit_service_result(c1_record_sink sink,
                                     const c1_service_result *result)
{
    c1_record record;

    c1_record_init(&record, "service", "action_complete");
    if (c1_record_add_integer(&record, "action", result->action) != C1_STATUS_OK ||
        c1_record_add_integer(&record, "status", result->status) != C1_STATUS_OK) {
        return C1_STATUS_INVALID_ARGUMENT;
    }
    return c1_record_emit(sink, &record);
}

static void finish_wifi_interaction(c1_ui_state *state, const c1_service_result *result)
{
    if (state->page == C1_UI_PAGE_WIFI) state->wifi_notice[0] = '\0';
    if (result->action != C1_UI_ACTION_WIFI_CONNECT) return;
    if (result->status != C1_STATUS_OK && state->page == C1_UI_PAGE_WIFI &&
        !wifi_stop_pending && (state->secret_length > 0U ||
        (state->selected_saved && state->selected_security == C1_WIFI_SECURITY_WPA_PSK))) {
        /* A stale saved password must be editable after a failed reconnect.
         * Keep the original profile on disk until a new connection commits. */
        state->selected_saved = false;
        state->page = C1_UI_PAGE_WIFI_PASSWORD;
        state->secret_visible = true;
        snprintf(state->wifi_notice, sizeof(state->wifi_notice), "%s",
                 result->wifi_snapshot_valid && result->wifi.error[0] != '\0'
                     ? result->wifi.error : "CONNECTION FAILED; PLEASE RETRY");
    } else {
        c1_ui_clear_secret(state);
    }
}

static void adopt_service_result(const c1_service_result *result)
{
    switch (result->action) {
    case C1_UI_ACTION_WIFI_SCAN:
    case C1_UI_ACTION_WIFI_CONNECT:
    case C1_UI_ACTION_WIFI_DISABLE:
        if (result->wifi_snapshot_valid) {
            c1_wifi_adopt_snapshot(&result->wifi);
        } else {
            snprintf(service_notice, sizeof(service_notice), "WI-FI TASK FAILED; PLEASE RETRY");
        }
        break;
    case C1_UI_ACTION_UPDATE_REFRESH:
    case C1_UI_ACTION_TERMINAL_APP:
    case C1_UI_ACTION_TERMINAL_UPDATE:
    case C1_UI_ACTION_NONE:
        break;
    }
}

static void close_inputs(struct pollfd *inputs)
{
    size_t index;

    for (index = 0U; index < C1_UI_INPUT_COUNT; ++index) {
        if (inputs[index].fd >= 0) {
            close(inputs[index].fd);
            inputs[index].fd = -1;
        }
    }
}

static c1_status open_inputs(struct pollfd *inputs)
{
    static const char *const paths[C1_UI_INPUT_COUNT] = {
        "/dev/input/event0", "/dev/input/event1"
    };
    size_t index;

    for (index = 0U; index < C1_UI_INPUT_COUNT; ++index) {
        inputs[index].fd = open(paths[index], O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        inputs[index].events = POLLIN;
        inputs[index].revents = 0;
        if (inputs[index].fd < 0) {
            close_inputs(inputs);
            return C1_STATUS_UNAVAILABLE;
        }
    }
    return C1_STATUS_OK;
}

static c1_status flush_terminal_replies(c1_terminal_session *session,
                                        c1_terminal_screen *screen)
{
    char reply[256];
    size_t count;

    while ((count = c1_terminal_screen_take_reply(screen, reply, sizeof(reply))) > 0U) {
        c1_status status = c1_terminal_write(session, reply, count);

        if (status != C1_STATUS_OK) {
            return status;
        }
    }
    return C1_STATUS_OK;
}

static c1_status drain_terminal(c1_terminal_session *session,
                                c1_terminal_screen *screen,
                                bool *changed)
{
    size_t total = 0U;

    while (total < C1_TERMINAL_READ_BUDGET && c1_terminal_is_running(session)) {
        char buffer[512];
        ssize_t count = c1_terminal_read(session, buffer, sizeof(buffer));

        if (count < 0) {
            return C1_STATUS_IO_ERROR;
        }
        if (count == 0) {
            break;
        }
        c1_terminal_screen_feed(screen, buffer, (size_t)count);
        total += (size_t)count;
        *changed = true;
    }
    return flush_terminal_replies(session, screen);
}

static c1_status send_pending_terminal_action(c1_terminal_session *session,
                                              c1_ui_action *pending_action)
{
    const char *command;
    c1_status status;

    if (pending_action == NULL || *pending_action == C1_UI_ACTION_NONE ||
        !c1_terminal_shell_is_foreground(session)) {
        return C1_STATUS_OK;
    }
    command = terminal_action_command(*pending_action);
    if (command == NULL) {
        *pending_action = C1_UI_ACTION_NONE;
        return C1_STATUS_INVALID_ARGUMENT;
    }
    status = c1_terminal_write(session, command, strlen(command));
    if (status == C1_STATUS_OK) {
        *pending_action = C1_UI_ACTION_NONE;
    }
    return status;
}

static void move_terminal_symbol(c1_ui_state *state, uint16_t code)
{
    uint32_t selection = state->symbol_selection;

    if (code == KEY_LEFT && selection % 6U != 0U) {
        --selection;
    } else if (code == KEY_RIGHT && selection % 6U != 5U &&
               selection + 1U < C1_UI_EXTENDED_SYMBOL_COUNT) {
        ++selection;
    } else if (code == KEY_UP && selection >= 6U) {
        selection -= 6U;
    } else if (code == KEY_DOWN && selection + 6U < C1_UI_EXTENDED_SYMBOL_COUNT) {
        selection += 6U;
    }
    state->symbol_selection = selection;
}

static bool terminal_repeatable(uint16_t code)
{
    return code == KEY_UP || code == KEY_DOWN || code == KEY_LEFT || code == KEY_RIGHT ||
           code == KEY_DELETE || code == KEY_VOLUMEUP || code == KEY_VOLUMEDOWN;
}

/* Shift is a physical modifier, not a libtsm terminal key. Track every key
 * before navigation/control/power dispatch so consumed chords cannot become
 * taps. The bitmap also covers keys held BEFORE Shift and both input devices. */
typedef struct {
    bool pressed;
    bool chord_used;
    unsigned int source;
    unsigned int held_count;
    unsigned char held[C1_UI_INPUT_COUNT][(KEY_CNT + 7U) / 8U];
    bool focus_valid;
    c1_ui_page page;
    const c1_terminal_session *session;
    pid_t child_pid;
    pid_t shell_pid;
    bool symbol_picker;
} c1_ui_shift_key;

static void shift_key_reset(c1_ui_shift_key *shift)
{
    memset(shift, 0, sizeof(*shift));
}

static void shift_key_focus(c1_ui_shift_key *shift, const c1_ui_state *state,
                            const c1_terminal_session *session)
{
    if (!shift->focus_valid || shift->page != state->page ||
        shift->session != session || shift->child_pid != session->child_pid ||
        shift->shell_pid != session->shell_pid ||
        shift->symbol_picker != state->terminal_symbol_picker ||
        state->page == C1_UI_PAGE_LOCK) {
        /* Keep other held keys across a page change, but never a pending tap. */
        shift->pressed = false;
        shift->chord_used = false;
    }
    shift->focus_valid = true;
    shift->page = state->page;
    shift->session = session;
    shift->child_pid = session->child_pid;
    shift->shell_pid = session->shell_pid;
    shift->symbol_picker = state->terminal_symbol_picker;
}

static bool shift_key_event(c1_ui_shift_key *shift, unsigned int source,
                            uint16_t code, int value)
{
    if (source >= C1_UI_INPUT_COUNT || code >= KEY_CNT || value < 0 || value > 2)
        return false;
    unsigned char mask = (unsigned char)(1U << (code % 8U));
    unsigned char *held = &shift->held[source][code / 8U];
    bool was_down = (*held & mask) != 0;
    if (value) {
        *held |= mask;
        if (!was_down) ++shift->held_count;
    } else {
        *held &= (unsigned char)~mask;
        if (was_down) --shift->held_count;
    }
    if (code != KEY_LEFTSHIFT) {
        if (shift->pressed) shift->chord_used = true;
        return false;
    }
    if (value == 1) {
        if (shift->pressed) shift->chord_used = true;
        else {
            shift->pressed = true;
            shift->source = source;
            shift->chord_used = was_down || shift->held_count != 1U;
        }
    } else if (value == 2) {
        /* Auto-repeat never arms a tap, and a held/repeating Shift is not a click. */
        if (shift->pressed) shift->chord_used = true;
    } else if (shift->pressed && shift->source == source) {
        bool tap = !shift->chord_used;
        shift->pressed = false;
        shift->chord_used = false;
        return tap;
    } else if (shift->pressed) shift->chord_used = true;
    return false;
}

static bool terminal_is_pkg_gui(const c1_ui_state *state,
                                const c1_terminal_session *session,
                                const c1_terminal_session *app_session,
                                bool app_mode)
{
    char path[64], arguments[256];
    struct stat running, expected;
    ssize_t count;
    int fd;
    /* shell_pid is the actual direct-exec child; child_pid is its supervisor.
     * A launch action or direct_exec flag alone remains true AFTER GUI execs
     * another application, so neither is sufficient to identify the receiver. */
    if (state->page != C1_UI_PAGE_TERMINAL || state->terminal_symbol_picker ||
        !app_mode || session != app_session || !session->direct_exec ||
        session->state != C1_TERMINAL_RUNNING || session->suspended ||
        session->child_pid <= 0 || session->shell_pid <= 0 || session->master_fd < 0 ||
        tcgetpgrp(session->master_fd) != session->shell_pid) return false;
    snprintf(path, sizeof(path), "/proc/%ld/exe", (long)session->shell_pid);
    /* Inode identity permits the installed path to be a release symlink, but
     * rejects renamed third-party programs and same-PID exec into another ELF. */
    if (stat(path, &running) != 0 || stat(C1_PKG_EXECUTABLE, &expected) != 0 ||
        running.st_dev != expected.st_dev || running.st_ino != expected.st_ino) return false;
    snprintf(path, sizeof(path), "/proc/%ld/cmdline", (long)session->shell_pid);
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    do { count = read(fd, arguments, sizeof(arguments)); } while (count < 0 && errno == EINTR);
    close(fd);
    if (count <= 0 || (size_t)count == sizeof(arguments)) return false;
    char *argument = memchr(arguments, '\0', (size_t)count);
    if (!argument || argument == arguments) return false;
    ++argument;
    /* Only the exact two-argument GUI invocation is eligible, never `run`,
     * shell/update commands, prefix matches, or incomplete /proc reads. */
    if (arguments + count - argument != 4 || memcmp(argument, "gui\0", 4U)) return false;
    snprintf(path, sizeof(path), "/proc/%ld/exe", (long)session->shell_pid);
    return stat(path, &running) == 0 && running.st_dev == expected.st_dev &&
        running.st_ino == expected.st_ino && tcgetpgrp(session->master_fd) == session->shell_pid;
}

static c1_status apply_shift_tap(c1_ui_state *state, c1_terminal_session *session,
                                 bool pkg_gui, bool *changed)
{
    *changed = false;
    if (pkg_gui) {
        static const char left_shift[] = "\033[57441u";
        return c1_terminal_write(session, left_shift, sizeof(left_shift) - 1U);
    }
    if (c1_ui_is_password_page(state->page) || state->page == C1_UI_PAGE_TERMINAL ||
        state->page == C1_UI_PAGE_LOCK_TEXT) {
        state->keyboard_layer = c1_ui_keyboard_next_layer(state->keyboard_layer);
        *changed = true;
    }
    return C1_STATUS_OK;
}

static c1_status send_shift_space(c1_terminal_session *session)
{
    static const char modified_space[] = "\033[32;2u";
    return c1_terminal_write(session, modified_space, sizeof(modified_space) - 1U);
}

static c1_status send_terminal_key(c1_terminal_session *session,
                                   c1_terminal_screen *screen,
                                   uint16_t code,
                                   c1_ui_keyboard_layer layer,
                                   bool shifted,
                                   bool control_pressed,
                                   bool *changed)
{
    char letter = physical_letter(code);
    unsigned int modifiers = control_pressed ? C1_TERMINAL_MOD_CONTROL : 0U;
    bool handled = false;

    if (letter != '\0') {
        char character = control_pressed
                             ? letter
                             : c1_ui_physical_character(layer, letter, shifted);

        handled = c1_terminal_screen_character(screen, (unsigned char)character, modifiers);
    } else if (code == KEY_SPACE || code == KEY_TAB) {
        handled = c1_terminal_screen_character(screen, code == KEY_TAB ? '\t' : ' ', modifiers);
    } else {
        c1_terminal_key key;

        switch (code) {
        case KEY_UP: key = C1_TERMINAL_KEY_UP; break;
        case KEY_DOWN: key = C1_TERMINAL_KEY_DOWN; break;
        case KEY_LEFT: key = C1_TERMINAL_KEY_LEFT; break;
        case KEY_RIGHT: key = C1_TERMINAL_KEY_RIGHT; break;
        /* Application PTYs must distinguish candidate pages from left/right. */
        case KEY_VOLUMEUP: key = C1_TERMINAL_KEY_PAGE_DOWN; break;
        case KEY_VOLUMEDOWN: key = C1_TERMINAL_KEY_PAGE_UP; break;
        case KEY_DELETE:
            key = shifted ? C1_TERMINAL_KEY_DELETE : C1_TERMINAL_KEY_BACKSPACE;
            break;
        case KEY_ENTER: key = C1_TERMINAL_KEY_ENTER; break;
        case KEY_BACK:
        case KEY_WAKEUP: key = C1_TERMINAL_KEY_ESCAPE; break;
        case KEY_HOME: key = C1_TERMINAL_KEY_HOME; break;
        default: return C1_STATUS_OK;
        }
        handled = c1_terminal_screen_special(screen, key, modifiers);
    }
    if (!handled) {
        return C1_STATUS_OK;
    }
    c1_terminal_screen_scroll_reset(screen);
    *changed = true;
    return flush_terminal_replies(session, screen);
}

static uint32_t input_keysym(uint16_t code, c1_ui_keyboard_layer layer, bool shift, bool control)
{
    char letter = physical_letter(code);
    if (letter) return (unsigned char)(control ? letter : c1_ui_physical_character(layer, letter, shift));
    switch (code) {
    case KEY_SPACE: return ' ';
    case KEY_TAB: return 0xff09U;
    case KEY_ENTER:
    case KEY_OK: return C1_IME_KEY_RETURN;
    case KEY_DELETE: return shift ? 0xffffU : C1_IME_KEY_BACKSPACE;
    case KEY_BACK: return C1_IME_KEY_ESCAPE;
    case KEY_UP: return 0xff52U;
    case KEY_DOWN: return 0xff54U;
    case KEY_LEFT: return C1_IME_KEY_LEFT;
    case KEY_RIGHT: return C1_IME_KEY_RIGHT;
    case KEY_VOLUMEUP: return C1_IME_KEY_PAGE_DOWN;
    case KEY_VOLUMEDOWN: return C1_IME_KEY_PAGE_UP;
    default: return 0;
    }
}

static void configure_power_preferences(c1_power_policy *policy, const c1_preferences *preferences)
{
    c1_power_settings settings = c1_preferences_power_settings(preferences->power_mode);
    c1_power_policy_configure(policy, settings.lock_minutes * 60000LL, 0,
                              settings.shutdown_minutes * 60000LL);
    policy->suspend_on_lock = settings.suspend_on_lock;
}

static void save_preferences_transition(c1_ui_state *next, const c1_ui_state *old, c1_power_policy *policy)
{
    if (!memcmp(&old->preferences, &next->preferences, sizeof(old->preferences))) return;
    if (!c1_preferences_save(&next->preferences, C1_DESKTOP_CONFIG)) {
        next->preferences = old->preferences;
        if (old->page == C1_UI_PAGE_LOCK_TEXT) {
            next->page = C1_UI_PAGE_LOCK_TEXT;
            memcpy(next->lock_text_draft, old->lock_text_draft, sizeof(next->lock_text_draft));
            next->lock_text_cursor = old->lock_text_cursor;
        }
        snprintf(next->wifi_notice, sizeof(next->wifi_notice), "%s",
                 c1_ui_tr(old->preferences.language, "保存失败，请重试", "Save failed; retry"));
        return;
    }
    next->wifi_notice[0] = 0;
    desktop_preferences = next->preferences;
    configure_power_preferences(policy, &desktop_preferences);
    (void)setenv("C1_UI_LANGUAGE", desktop_preferences.language == C1_LANGUAGE_EN ? "en" : "zh", 1);
}

/* This is the lock editor's physical-key route, before generic IME/navigation.
 * A confirm observed with outstanding input is irrevocably input-only. Its
 * eventual unconsumed reply must not become a delayed save after a prior commit. */
static bool route_lock_text_key(c1_ui_state *state, c1_power_policy *policy,
                                uint16_t code, int value, bool shift, bool control)
{
    if (state->page != C1_UI_PAGE_LOCK_TEXT) return false;
    if (value != 1) return true; /* Releases/repeats never save. */
    bool confirm = code == KEY_OK || code == KEY_ENTER;
    bool busy = desktop_input.count || desktop_input.client.pending_sequence ||
        (input_view.flags & C1_IME_COMPOSING) || input_view.preedit[0] || input_view.candidate_count;
    if (code == KEY_BACK || code == KEY_HOME || (confirm && !busy)) {
        c1_ui_event event = code == KEY_BACK ? C1_UI_EVENT_BACK :
            code == KEY_HOME ? C1_UI_EVENT_HOME : C1_UI_EVENT_ENTER;
        c1_ui_transition next = c1_ui_step(*state, event, NULL);
        save_preferences_transition(&next.state, state, policy);
        *state = next.state;
        input_focus_update(state);
        return true;
    }
    uint32_t key = input_keysym(code, state->keyboard_layer, shift, control);
    if (key && c1_input_method_key(&desktop_input, key, control ? C1_IME_MOD_CONTROL : 0U)) return true;
    if (confirm) {
        /* Stale composition after a transport failure is not permission to save. */
        desktop_input.failed = true;
        return true;
    }
    if (key >= 32U && key <= 126U) (void)c1_ui_lock_text_append_ascii(state, (char)key);
    else if (key == C1_IME_KEY_BACKSPACE || code == KEY_DELETE) (void)c1_ui_lock_text_delete(state);
    else if (key == C1_IME_KEY_LEFT || key == C1_IME_KEY_RIGHT)
        (void)c1_ui_lock_text_move(state, key == C1_IME_KEY_LEFT ? -1 : 1);
    else return false;
    return true;
}

static c1_status deliver_input(c1_ui_state *state, c1_power_policy *policy,
                               c1_terminal_session *session, c1_terminal_screen *screen,
                               const struct c1_ime_response *reply)
{
    if (reply->status != C1_IME_STATUS_OK || !(reply->flags & C1_IME_READY)) {
        c1_input_method_close(&desktop_input);
        desktop_input.failed = true;
        return C1_STATUS_OK;
    }
    input_view = *reply;
    if (reply->commit[0]) {
        if (state->page == C1_UI_PAGE_TERMINAL) {
            c1_status result = c1_terminal_write(session, reply->commit, strlen(reply->commit));
            if (result != C1_STATUS_OK) return result;
        } else if (!c1_ui_lock_text_append_utf8(state, reply->commit)) {
            snprintf(state->wifi_notice, sizeof(state->wifi_notice), "%s",
                c1_ui_tr(state->preferences.language, "文字过长或包含不支持字符", "Text too long / invalid"));
        }
    }
    if (reply->request.operation != C1_IME_OP_KEY || (reply->flags & C1_IME_CONSUMED)) return C1_STATUS_OK;
    uint32_t key = reply->request.keysym;
    if (state->page == C1_UI_PAGE_LOCK_TEXT) {
        if (key >= 32 && key <= 126) (void)c1_ui_lock_text_append_ascii(state, (char)key);
        else if (key == C1_IME_KEY_BACKSPACE) (void)c1_ui_lock_text_delete(state);
        else if (key == C1_IME_KEY_LEFT || key == C1_IME_KEY_RIGHT)
            (void)c1_ui_lock_text_move(state, key == C1_IME_KEY_LEFT ? -1 : 1);
        /* RETURN was queued while input was pending/composing: never save here,
         * even if preceding queued keys already ended the composition. */
        else if (key == C1_IME_KEY_ESCAPE) {
            c1_ui_transition next = c1_ui_step(*state, C1_UI_EVENT_BACK, NULL);
            save_preferences_transition(&next.state, state, policy);
            *state = next.state;
        }
        return C1_STATUS_OK;
    }
    /* The IME uses volume +/- as next/previous candidate page. Without a
     * composition the same physical keys retain desktop scrollback behavior,
     * rather than becoming PageUp/PageDown keystrokes sent to the shell. */
    if (key == C1_IME_KEY_PAGE_UP || key == C1_IME_KEY_PAGE_DOWN) {
        if (key == C1_IME_KEY_PAGE_DOWN) c1_terminal_screen_scroll_page_up(screen);
        else c1_terminal_screen_scroll_page_down(screen);
        return C1_STATUS_OK;
    }
    unsigned modifiers = reply->request.modifiers & C1_IME_MOD_CONTROL ? C1_TERMINAL_MOD_CONTROL : 0;
    if ((key >= 32 && key <= 126) || key == 0xff09U)
        (void)c1_terminal_screen_character(screen, key == 0xff09U ? '\t' : key, modifiers);
    else {
        c1_terminal_key special;
        switch (key) {
        case C1_IME_KEY_BACKSPACE: special = C1_TERMINAL_KEY_BACKSPACE; break;
        case C1_IME_KEY_RETURN: special = C1_TERMINAL_KEY_ENTER; break;
        case C1_IME_KEY_ESCAPE: special = C1_TERMINAL_KEY_ESCAPE; break;
        case 0xff51U: special = C1_TERMINAL_KEY_LEFT; break;
        case 0xff52U: special = C1_TERMINAL_KEY_UP; break;
        case 0xff53U: special = C1_TERMINAL_KEY_RIGHT; break;
        case 0xff54U: special = C1_TERMINAL_KEY_DOWN; break;
        case 0xffffU: special = C1_TERMINAL_KEY_DELETE; break;
        case C1_IME_KEY_PAGE_UP: special = C1_TERMINAL_KEY_PAGE_UP; break;
        case C1_IME_KEY_PAGE_DOWN: special = C1_TERMINAL_KEY_PAGE_DOWN; break;
        default: return C1_STATUS_OK;
        }
        (void)c1_terminal_screen_special(screen, special, modifiers);
    }
    c1_terminal_screen_scroll_reset(screen);
    return flush_terminal_replies(session, screen);
}

c1_status c1_linux_ui_run(c1_record_sink sink)
{
    struct pollfd pollfds[C1_UI_INPUT_COUNT + 4U] = {
        {-1, POLLIN, 0}, {-1, POLLIN, 0}, {-1, 0, 0}, {-1, POLLIN, 0}, {-1, POLLIN, 0}, {-1, POLLIN, 0}
    };
    c1_ui_state state = c1_ui_initial_state();
    c1_terminal_session user_session, app_session;
    c1_terminal_screen user_screen, app_screen;
    c1_terminal_session *terminal_session = &user_session;
    c1_terminal_screen *terminal_screen = &user_screen;
    c1_service_worker service_worker;
    c1_input_service input_service;
    c1_power_policy power_policy;
    c1_linux_led_chaser led_chaser;
    c1_status status;
    int64_t started_at;
    int64_t next_status_at;
    int64_t next_external_power_at;
    int64_t next_battery_at;
    int64_t terminal_render_at = -1;
    bool terminal_dirty = false;
    c1_ui_action pending_terminal_action = C1_UI_ACTION_NONE;
    bool terminal_started_once = false;
    bool terminal_ended_announced = false;
    bool user_started_once = false;
    bool user_ended_announced = false;
    c1_ui_action user_pending_action = C1_UI_ACTION_NONE;
    bool terminal_app_mode = false;
    bool external_app_active = false;
    bool external_app_terminal = false;
    bool shutdown_requested = false;
    c1_ui_health_retry health_retry = {0};
    c1_ui_shift_key shift_key = {0};
    bool control_pressed = false;
    c1_power_key power_key = {0};
    bool input_dropped[C1_UI_INPUT_COUNT] = {false};
    uint16_t repeat_code = 0U;
    int64_t repeat_at = -1;
    uint64_t poll_calls = 0U;
    uint64_t poll_events = 0U;
    uint64_t poll_timeouts = 0U;
    int64_t summary_next_at = 0, core_next_at = 0;
    c1_ui_page summary_page = C1_UI_PAGE_LOCK;

    c1_liveness_init();
    /* Adopt the inherited channel before any service/terminal worker forks. */
    {
        int shutdown_error = c1_shutdown_init();
        if (shutdown_error != 0)
            fprintf(stderr, "C1ancher: coordinated shutdown unavailable: %s\n", strerror(shutdown_error));
    }
    c1_input_service_init(&input_service);
    c1_input_method_init(&desktop_input);
    memset(&input_view, 0, sizeof(input_view));
    memset(&desktop_summary, 0, sizeof(desktop_summary));
    c1_battery_history_init(&battery_history);
    (void)c1_battery_history_load(C1_DESKTOP_BATTERY_HISTORY, &battery_history);
    desktop_new_apps = 0;
    remote_update_sequence = 0;
    remote_update_available = false;
    c1_desktop_job_init(&desktop_job);
    reload_desktop_summary();
    (void)c1_preferences_load(&state.preferences, C1_DESKTOP_CONFIG);
    desktop_preferences = state.preferences;
    c1_time_sync_init(&network_clock);
    (void)setenv("C1_UI_LANGUAGE", state.preferences.language == C1_LANGUAGE_EN ? "en" : "zh", 1);
    c1_terminal_init(&user_session);
    c1_terminal_init(&app_session);
    service_worker_init(&service_worker);
    memset(&led_chaser, 0, sizeof(led_chaser));
    status = c1_terminal_screen_init(terminal_screen);
    if (status != C1_STATUS_OK) {
        c1_liveness_close();
        return status;
    }
    status = c1_terminal_screen_init(&app_screen);
    if (status != C1_STATUS_OK) {
        c1_terminal_screen_destroy(&user_screen);
        c1_liveness_close();
        return status;
    }
    status = open_inputs(pollfds);
    if (status != C1_STATUS_OK) {
        c1_terminal_screen_destroy(&user_screen);
        c1_terminal_screen_destroy(&app_screen);
        c1_liveness_close();
        return status;
    }
    started_at = monotonic_milliseconds();
    if (started_at < 0) {
        status = C1_STATUS_IO_ERROR;
        goto done;
    }
    c1_power_policy_init(&power_policy, started_at);
    configure_power_preferences(&power_policy, &state.preferences);
    /* Provision public media folders on boot, without requiring any app launch. */
    (void)c1_media_directories_prepare_from(C1_MEDIA_ROOT);
    c1_wallpaper_load();
    update_external_power(&power_policy, started_at);
    sample_battery_history(started_at);
    next_battery_at = started_at + C1_BATTERY_SAMPLE_INTERVAL_MS;
    status = render_current(state, terminal_screen, true, sink);
    if (status != C1_STATUS_OK) {
        goto done;
    }
    (void)c1_input_service_start(&input_service);
    c1_liveness_beat(monotonic_milliseconds());
    try_update_health(&health_retry, monotonic_milliseconds());
    (void)c1_linux_led_chaser_start(&led_chaser, C1_LED_SYSFS_ROOT, started_at);
    next_status_at = started_at + C1_UI_STATUS_INTERVAL_MS;
    next_external_power_at = started_at + C1_EXTERNAL_POWER_INTERVAL_MS;

    for (;;) {
        int poll_result;
        size_t index;
        int64_t now = monotonic_milliseconds();
        nfds_t poll_count = C1_UI_INPUT_COUNT;

        if (shutdown_requested) {
            status = wait_for_poweroff();
            break;
        }
        if (now < 0) {
            status = C1_STATUS_IO_ERROR;
            break;
        }
        c1_liveness_beat(now);
        if (now >= next_battery_at) {
            /* Independent of input/redraws and display leases; never redraw a
             * frozen lock screen just to collect a battery observation. */
            sample_battery_history(now);
            next_battery_at = now + C1_BATTERY_SAMPLE_INTERVAL_MS;
            if (state.page == C1_UI_PAGE_BATTERY && !c1_app_lease_active()) {
                /* Battery page follows ten-second telemetry. The display
                 * backend skips identical frames; no refresh while locked. */
                status = render_current(state, terminal_screen, false, sink);
                if (status != C1_STATUS_OK) break;
            }
        }
        c1_linux_poweroff_reap();
        (void)c1_input_service_poll(&input_service);
        if (!terminal_app_mode && terminal_session == &app_session) {
            c1_terminal_stop(&app_session);
            terminal_session = &user_session;
            terminal_screen = &user_screen;
            terminal_started_once = user_started_once;
            terminal_ended_announced = user_ended_announced;
            pending_terminal_action = user_pending_action;
            terminal_dirty = false;
            terminal_render_at = -1;
        }
        shift_key_focus(&shift_key, &state, terminal_session);
        if (summary_page != state.page) {
            if (state.page == C1_UI_PAGE_DESKTOP) {
                reload_desktop_summary();
                status = render_current(state, terminal_screen, false, sink);
                if (status != C1_STATUS_OK) break;
            }
            summary_page = state.page;
        }
        if (!state.preferences.background_checks || state.page == C1_UI_PAGE_LOCK || !clock_online || wifi_stop_pending)
            c1_desktop_job_cancel(&desktop_job, now);
        int job_result = c1_desktop_job_poll(&desktop_job, now);
        if (job_result) {
            if (desktop_job.kind == 1) {
                if (job_result > 0) reload_desktop_summary();
                summary_next_at = now + (job_result > 0 ? 15 * 60000 : 5 * 60000);
            } else {
                uint64_t sequence; bool available;
                if (job_result > 0 && c1_desktop_update_parse(desktop_job.output, desktop_job.used, &sequence, &available)) {
                    remote_update_sequence = sequence; remote_update_available = available;
                    core_next_at = now + 6 * 60 * 60000LL;
                } else core_next_at = now + 15 * 60000;
            }
            if (state.page == C1_UI_PAGE_DESKTOP || state.page == C1_UI_PAGE_SETTINGS) {
                status = render_current(state, terminal_screen, false, sink);
                if (status != C1_STATUS_OK) break;
            }
        }
        if (desktop_job.pid <= 0 && state.preferences.background_checks && clock_online &&
            !wifi_stop_pending && state.page == C1_UI_PAGE_DESKTOP && power_policy.state == C1_POWER_ACTIVE &&
            service_worker.pid <= 0 && network_clock.pid <= 0 && !c1_app_run_active()) {
            int kind = now >= summary_next_at ? 1 : now >= core_next_at ? 2 : 0;
            if (kind && !c1_desktop_job_start(&desktop_job, kind, now)) {
                if (kind == 1) summary_next_at = now + 5 * 60000;
                else core_next_at = now + 15 * 60000;
            }
        }
        input_terminal_allowed = !terminal_app_mode && terminal_session == &user_session;
        input_focus_update(&state);
        if (desktop_input.client.fd >= 0) {
            struct c1_ime_response response;
            int result = c1_input_method_tick(&desktop_input, &response, now);
            if (result > 0) {
                status = deliver_input(&state, &power_policy, terminal_session, terminal_screen, &response);
                if (status != C1_STATUS_OK) break;
            }
            if (result != 0) {
                if (state.page == C1_UI_PAGE_TERMINAL) {
                    terminal_dirty = true;
                    terminal_render_at = now;
                } else {
                    status = render_current(state, terminal_screen, false, sink);
                    if (status != C1_STATUS_OK) break;
                }
            }
        }
        if (state.page == C1_UI_PAGE_TERMINAL) {
            unsigned int columns, rows;
            terminal_geometry(&state, &columns, &rows);
            if (terminal_screen->columns != columns || terminal_screen->rows != rows) {
                status = c1_terminal_screen_resize(terminal_screen, columns, rows);
                if (status == C1_STATUS_OK) status = c1_terminal_resize(terminal_session, columns, rows);
                if (status != C1_STATUS_OK) break;
                terminal_dirty = true;
                terminal_render_at = now;
            }
        }
        if (c1_stop_requested()) {
            status = shutdown_requested ? C1_STATUS_SHUTDOWN_REQUESTED : C1_STATUS_INTERRUPTED;
            break;
        }
        {
            bool lease_active = c1_app_run_active();
            if (lease_active != external_app_active)
                c1_power_key_reset(&power_key);

            if (lease_active) {
                bool terminal_mode = c1_app_lease_terminal_mode();
                if (terminal_mode != external_app_terminal) {
                    shift_key_reset(&shift_key);
                    external_app_terminal = terminal_mode;
                    terminal_dirty = terminal_mode;
                    terminal_render_at = terminal_mode ? now : -1;
                }
                if (!external_app_active) {
                    external_app_active = true;
                    external_app_terminal = terminal_mode;
                    shift_key_reset(&shift_key);
                    control_pressed = false;
                    repeat_code = 0U;
                    repeat_at = -1;
                    terminal_dirty = terminal_mode && state.page == C1_UI_PAGE_TERMINAL;
                    terminal_render_at = terminal_dirty ? now : -1;
                    c1_linux_led_chaser_quiet(&led_chaser);
                }
                if (power_policy.state != C1_POWER_ACTIVE) {
                    (void)c1_power_policy_unlock(&power_policy, now);
                }
                c1_power_policy_note_activity(&power_policy, now);
            } else if (external_app_active) {
                external_app_active = false;
                external_app_terminal = false;
                if (terminal_app_mode && !c1_terminal_is_running(terminal_session)) {
                    state.page = C1_UI_PAGE_DESKTOP;
                    state.selection = state.desktop_selection < 5U ? state.desktop_selection : 0U;
                    terminal_app_mode = false;
                    pending_terminal_action = C1_UI_ACTION_NONE;
                }
                c1_linux_display_reset_cache();
                status = render_current(state, terminal_screen, true, sink);
                if (status != C1_STATUS_OK) {
                    break;
                }
                next_status_at = now + C1_UI_STATUS_INTERVAL_MS;
                continue;
            }
        }
        c1_liveness_beat(monotonic_milliseconds());
        try_update_health(&health_retry, monotonic_milliseconds());
        now = monotonic_milliseconds();
        if (now < 0) { status = C1_STATUS_IO_ERROR; break; }
        c1_liveness_beat(now);
        if (!power_key.down && consume_update_request_if_ready(c1_app_run_active())) {
            status = C1_STATUS_UPDATE_REQUESTED;
            break;
        }
        /* Recheck the cable immediately before a due automatic action, rather
         * than acting on a sample up to five seconds old. */
        if (now >= next_external_power_at || c1_power_policy_timeout(&power_policy, now) == 0) {
            update_external_power(&power_policy, now);
            next_external_power_at = now + C1_EXTERNAL_POWER_INTERVAL_MS;
        }
        c1_linux_led_chaser_tick(&led_chaser, now);
        c1_time_sync_tick(&network_clock, state.preferences.network_time &&
                          power_policy.state == C1_POWER_ACTIVE, clock_online, now);
        {
            /* A held power key must not be interrupted by idle lock/suspend. */
            c1_power_action power_action = automatic_power_action(
                &power_policy, &power_key, pollfds, now);

            if (power_action == C1_POWER_ACTION_SHUTDOWN) {
                bool accepted = false;
                if (automatic_shutdown_safe(&service_worker, &user_session, &app_session)) {
                    accepted = request_poweroff_locked(&state, &power_policy,
                                                       terminal_screen, sink);
                    if (accepted) shutdown_requested = true;
                }
                if (!accepted) {
                    /* Failed requests and safety deferrals retry from the
                     * completion time, never at an expired deadline. */
                    int64_t completed_at = monotonic_milliseconds();
                    c1_power_policy_shutdown_failed(
                        &power_policy, completed_at >= 0 ? completed_at : now);
                }
                continue;
            }
            if (power_action == C1_POWER_ACTION_ENTER_LOCK) {
                (void)c1_ui_enter_lock(&state);
                shift_key_reset(&shift_key);
                control_pressed = false;
                c1_linux_led_chaser_quiet(&led_chaser);
                repeat_code = 0U;
                repeat_at = -1;
                terminal_render_at = -1;
                status = render_current(state, terminal_screen, true, sink);
                if (status != C1_STATUS_OK) {
                    break;
                }
                /* Publish the lock frame before a separate loop iteration
                 * can attempt immediate performance-mode suspend. */
                continue;
            }
            if (power_action == C1_POWER_ACTION_SUSPEND) {
                c1_linux_power_context power_context;
                c1_status prepare_status;

                if (!automatic_suspend_safe(&service_worker)) {
                    c1_power_policy_suspend_failed(&power_policy, now);
                    continue;
                }
                prepare_status = c1_linux_power_prepare(
                    &power_context, terminal_session);
                now = monotonic_milliseconds();
                if (now < 0) {
                    if (prepare_status == C1_STATUS_OK)
                        c1_linux_power_rollback(&power_context, terminal_session);
                    status = C1_STATUS_IO_ERROR;
                    break;
                }

                if (prepare_status == C1_STATUS_OK) {
                    c1_status suspend_status;
                    c1_status resume_status;

                    status = emit_runtime_stats(sink, poll_calls, poll_events, poll_timeouts);
                    if (status != C1_STATUS_OK) {
                        c1_linux_power_rollback(&power_context, terminal_session);
                        break;
                    }
                    c1_linux_led_chaser_stop(&led_chaser);
                    battery_history.continuous = false;
                    suspend_status = c1_linux_power_suspend(sink);
                    c1_power_key_reset(&power_key);
                    resume_status = c1_linux_power_resume(&power_context, terminal_session);
                    shift_key_reset(&shift_key);
                    control_pressed = false;
                    repeat_code = 0U;
                    repeat_at = -1;
                    terminal_render_at = -1;
                    now = monotonic_milliseconds();
                    if (now < 0) {
                        status = C1_STATUS_IO_ERROR;
                        break;
                    }
                    (void)c1_linux_led_chaser_start(&led_chaser, C1_LED_SYSFS_ROOT, now);
                    update_external_power(&power_policy, now);
                    next_external_power_at = now + C1_EXTERNAL_POWER_INTERVAL_MS;
                    if (resume_status != C1_STATUS_OK) {
                        c1_power_policy_restore_failed(&power_policy, now);
                        terminal_dirty = true;
                        terminal_render_at = now;
                    } else if (suspend_status == C1_STATUS_OK) {
                        c1_power_policy_resumed(&power_policy, now);
                    } else if (suspend_status == C1_STATUS_INTERRUPTED) {
                        /* A late cable/lease race must not busy-retry a
                         * zero-delay performance policy. */
                        c1_power_policy_suspend_failed(&power_policy, now);
                    } else if (suspend_status == C1_STATUS_UNAVAILABLE) {
                        c1_power_policy_suspend_unavailable(&power_policy);
                    } else {
                        c1_power_policy_suspend_failed(&power_policy, now);
                    }
                    if (suspend_status == C1_STATUS_OK) {
                        c1_linux_display_reset_cache();
                        status = render_current(state, terminal_screen, true, sink);
                        if (status != C1_STATUS_OK) {
                            break;
                        }
                    }
                } else if (prepare_status == C1_STATUS_INTERRUPTED) {
                    update_external_power(&power_policy, now);
                    next_external_power_at = now + C1_EXTERNAL_POWER_INTERVAL_MS;
                    c1_power_policy_suspend_failed(&power_policy, now);
                } else if (prepare_status == C1_STATUS_UNAVAILABLE) {
                    c1_power_policy_suspend_unavailable(&power_policy);
                } else {
                    c1_power_policy_suspend_failed(&power_policy, now);
                }
                continue;
            }
        }
        if ((!external_app_active || external_app_terminal) && state.page == C1_UI_PAGE_TERMINAL &&
            !state.terminal_symbol_picker &&
            repeat_code != 0U && repeat_at >= 0 &&
            now >= repeat_at && c1_terminal_is_running(terminal_session)) {
            if (repeat_code == KEY_VOLUMEUP) {
                c1_terminal_screen_scroll_up(terminal_screen, 1U);
                terminal_dirty = true;
            } else if (repeat_code == KEY_VOLUMEDOWN) {
                c1_terminal_screen_scroll_down(terminal_screen, 1U);
                terminal_dirty = true;
            } else {
                status = send_terminal_key(terminal_session,
                                           terminal_screen,
                                           repeat_code,
                                           state.keyboard_layer,
                                           shift_key.pressed,
                                           control_pressed,
                                           &terminal_dirty);
                if (status != C1_STATUS_OK) {
                    break;
                }
            }
            if (terminal_dirty && terminal_render_at < 0) {
                terminal_render_at = now + C1_TERMINAL_RENDER_DELAY_MS;
            }
            repeat_at = now + C1_TERMINAL_REPEAT_INTERVAL_MS;
        }
        if ((!external_app_active || external_app_terminal) &&
            state.page == C1_UI_PAGE_TERMINAL && terminal_dirty &&
            terminal_render_at >= 0 && now >= terminal_render_at) {
            status = render_current(state, terminal_screen, false, sink);
            if (status != C1_STATUS_OK) {
                break;
            }
            terminal_dirty = false;
            terminal_render_at = -1;
        }
        if (state.page != C1_UI_PAGE_LOCK &&
            now >= next_status_at) {
            status = emit_runtime_stats(sink, poll_calls, poll_events, poll_timeouts);
            if (status != C1_STATUS_OK) {
                break;
            }
            status = render_current(state, terminal_screen, false, sink);
            if (status != C1_STATUS_OK) {
                break;
            }
            next_status_at = now + C1_UI_STATUS_INTERVAL_MS;
        }

        pollfds[C1_UI_INPUT_COUNT].fd = state.page == C1_UI_PAGE_TERMINAL
                                                 ? c1_terminal_fd(terminal_session)
                                                 : -1;
        pollfds[C1_UI_INPUT_COUNT].events = c1_terminal_poll_events(terminal_session);
        pollfds[C1_UI_INPUT_COUNT].revents = 0;
        if (pollfds[C1_UI_INPUT_COUNT].fd >= 0 && state.page == C1_UI_PAGE_TERMINAL) {
            poll_count = C1_UI_INPUT_COUNT + 1U;
        }
        pollfds[C1_UI_INPUT_COUNT + 1U].fd = service_worker.fd;
        pollfds[C1_UI_INPUT_COUNT + 1U].events = POLLIN;
        pollfds[C1_UI_INPUT_COUNT + 1U].revents = 0;
        if (service_worker.fd >= 0) {
            poll_count = C1_UI_INPUT_COUNT + 2U;
        }
        {
            int poll_timeout = power_key.down ? -1 : c1_power_policy_timeout(&power_policy, now);
            poll_timeout = deadline_timeout(poll_timeout, c1_power_key_deadline(&power_key), now);
            int led_timeout = c1_linux_led_chaser_timeout(&led_chaser, now);

            if (led_timeout >= 0 && (poll_timeout < 0 || led_timeout < poll_timeout)) {
                poll_timeout = led_timeout;
            }
            if (state.page != C1_UI_PAGE_LOCK) {
                poll_timeout = deadline_timeout(poll_timeout, next_status_at, now);
            }
            poll_timeout = deadline_timeout(poll_timeout, next_external_power_at, now);
            poll_timeout = deadline_timeout(poll_timeout, next_battery_at, now);
            if (state.page == C1_UI_PAGE_TERMINAL) {
                poll_timeout = deadline_timeout(poll_timeout, terminal_render_at, now);
                poll_timeout = deadline_timeout(poll_timeout, repeat_at, now);
            }
            pollfds[C1_UI_INPUT_COUNT + 2U].fd =
                terminal_session != &user_session || state.page != C1_UI_PAGE_TERMINAL
                    ? c1_terminal_fd(&user_session) : -1;
            pollfds[C1_UI_INPUT_COUNT + 2U].events = c1_terminal_poll_events(&user_session);
            pollfds[C1_UI_INPUT_COUNT + 2U].revents = 0;
            pollfds[C1_UI_INPUT_COUNT + 3U].fd = desktop_input.client.fd;
            pollfds[C1_UI_INPUT_COUNT + 3U].events = c1_input_method_poll_events(&desktop_input);
            pollfds[C1_UI_INPUT_COUNT + 3U].revents = 0;
            poll_count = C1_UI_INPUT_COUNT + 4U;
            poll_timeout = deadline_timeout(poll_timeout, desktop_input.deadline, now);
            poll_timeout = deadline_timeout(poll_timeout, now + C1_HEARTBEAT_INTERVAL_MS, now);
            if (desktop_job.pid > 0 || external_app_active || pending_terminal_action != C1_UI_ACTION_NONE)
                poll_timeout = deadline_timeout(poll_timeout, now + 100, now);
            poll_result = poll(pollfds, poll_count, poll_timeout);
            ++poll_calls;
            if (poll_result == 0) {
                ++poll_timeouts;
            } else if (poll_result > 0) {
                ++poll_events;
            }
        }
        if (poll_result < 0) {
            if (errno == EINTR) {
                continue;
            }
            status = C1_STATUS_IO_ERROR;
            break;
        }
        now = monotonic_milliseconds();
        if (now < 0) {
            status = C1_STATUS_IO_ERROR;
            break;
        }

        if (c1_app_run_active()) {
            bool terminal_mode = c1_app_lease_terminal_mode();
            if (!external_app_active) {
                c1_power_key_reset(&power_key);
                external_app_active = true;
                shift_key_reset(&shift_key);
                control_pressed = false;
                repeat_code = 0U;
                repeat_at = -1;
                c1_linux_led_chaser_quiet(&led_chaser);
            }
            if (terminal_mode != external_app_terminal) {
                shift_key_reset(&shift_key);
                terminal_dirty = terminal_mode;
                terminal_render_at = terminal_mode ? now : -1;
            }
            external_app_terminal = terminal_mode;
        }
        for (index = 0U; index < C1_UI_INPUT_COUNT; ++index) {
            if ((pollfds[index].revents & POLLIN) != 0) {
                struct input_event input;
                ssize_t count;

                while ((count = read(pollfds[index].fd, &input, sizeof(input))) ==
                       (ssize_t)sizeof(input)) {
                    /* HOME and another navigation key can arrive in one input
                     * batch. Restore the user context before handling that key. */
                    if (!terminal_app_mode && terminal_session == &app_session) {
                        c1_terminal_stop(&app_session);
                        terminal_session = &user_session;
                        terminal_screen = &user_screen;
                        terminal_started_once = user_started_once;
                        terminal_ended_announced = user_ended_announced;
                        pending_terminal_action = user_pending_action;
                        terminal_dirty = false;
                        terminal_render_at = -1;
                    }
                    shift_key_focus(&shift_key, &state, terminal_session);
                    bool terminal_page = state.page == C1_UI_PAGE_TERMINAL;
                    bool password_page = c1_ui_is_password_page(state.page);
                    bool app_volume_key;
                    bool app_confirm_key;

                    if (input.type == EV_SYN && input.code == SYN_DROPPED) {
                        input_dropped[index] = true;
                        c1_power_key_reset(&power_key);
                        shift_key_reset(&shift_key);
                        control_pressed = false;
                        repeat_code = 0U;
                        repeat_at = -1;
                        continue;
                    }
                    if (input_dropped[index]) {
                        if (input.type == EV_SYN && input.code == SYN_REPORT)
                            input_dropped[index] = false;
                        continue;
                    }
                    if (input.type != EV_KEY) {
                        continue;
                    }
                    if (shutdown_requested) {
                        continue;
                    }
                    bool shift_tap = shift_key_event(&shift_key, (unsigned int)index,
                                                     input.code, input.value);
                    if (input.code == KEY_POWER || input.code == KEY_WAKEUP) {
                        c1_power_key_action power_action = power_key_input(
                            &power_key, &power_policy, (unsigned int)index, &input, now);
                        if (power_action != C1_POWER_KEY_SHORT) continue;
                        /* A legacy app may own evdev/display. A desktop-only
                         * lock would neither stop its input nor be unlockable
                         * while the run lease keeps the power policy active. */
                        if (external_app_active) continue;
                        /* Deliver short press only on release. A long hold must
                         * never lock/unlock first or forward a stray terminal key. */
                        input.code = KEY_WAKEUP;
                        input.value = 1;
                    }
                    if (external_app_active && !external_app_terminal) {
                        if (input.code == KEY_HOME && input.value == 1 && terminal_app_mode) {
                            c1_terminal_stop(terminal_session);
                            terminal_app_mode = false;
                            terminal_started_once = false;
                            terminal_ended_announced = false;
                            pending_terminal_action = C1_UI_ACTION_NONE;
                            state.page = C1_UI_PAGE_DESKTOP;
                            state.selection = state.desktop_selection < 5U ? state.desktop_selection : 0U;
                            if (!c1_app_run_active()) {
                                external_app_active = false;
                                c1_linux_display_reset_cache();
                                status = render_current(state, terminal_screen, true, sink);
                                if (status != C1_STATUS_OK) {
                                    goto done;
                                }
                                next_status_at = now + C1_UI_STATUS_INTERVAL_MS;
                            }
                        }
                        continue;
                    }
                    app_volume_key =
                        terminal_page && terminal_app_mode &&
                        (input.code == KEY_VOLUMEUP || input.code == KEY_VOLUMEDOWN) &&
                        c1_terminal_is_running(terminal_session) &&
                        !c1_terminal_shell_is_foreground(terminal_session);
                    app_confirm_key =
                        terminal_page && terminal_app_mode && input.code == KEY_OK &&
                        c1_terminal_is_running(terminal_session) &&
                        !c1_terminal_shell_is_foreground(terminal_session);
                    if (input.value == 1) {
                        c1_power_policy_note_activity(&power_policy, now);
                    }
                    if (input.value == 1 && state.page != C1_UI_PAGE_LOCK &&
                        input.code == KEY_WAKEUP && !(terminal_page && shift_key.pressed)) {
                        if (c1_power_policy_lock(&power_policy, now) && c1_ui_enter_lock(&state)) {
                            shift_key_reset(&shift_key);
                            control_pressed = false;
                            c1_linux_led_chaser_quiet(&led_chaser);
                            repeat_code = 0U;
                            repeat_at = -1;
                            status = render_current(state, terminal_screen, true, sink);
                            if (status != C1_STATUS_OK) {
                                goto done;
                            }
                            next_status_at = now + C1_UI_STATUS_INTERVAL_MS;
                        }
                        continue;
                    }
                    if (wake_lock_from_key(&state, &power_policy, &input, now)) {
                        /* Consume the wake press, including Shift state, so its
                         * release cannot open a menu on the restored page. */
                        shift_key_reset(&shift_key);
                        control_pressed = false;
                        c1_linux_led_chaser_pulse(&led_chaser, now);
                        repeat_code = 0U;
                        repeat_at = -1;
                        status = render_current(state, terminal_screen, true, sink);
                        if (status != C1_STATUS_OK) goto done;
                        next_status_at = now + C1_UI_STATUS_INTERVAL_MS;
                        continue;
                    }
                    if (state.page == C1_UI_PAGE_LOCK) {
                        continue;
                    }
                    if (input.value == 1) {
                        c1_linux_led_chaser_pulse(&led_chaser, now);
                    }
                    if (input.code == KEY_LEFTSHIFT) {
                        if (shift_tap) {
                            bool changed;
                            status = apply_shift_tap(&state, terminal_session,
                                terminal_is_pkg_gui(&state, terminal_session, &app_session,
                                                    terminal_app_mode), &changed);
                            if (status != C1_STATUS_OK) goto done;
                            if (changed && terminal_page) {
                                terminal_dirty = true;
                                terminal_render_at = now;
                            } else if (changed) {
                                status = render_current(state, terminal_screen, false, sink);
                                if (status != C1_STATUS_OK) goto done;
                            }
                        }
                        continue;
                    }
                    if (input.code == KEY_OK && terminal_page) {
                        if (app_confirm_key) {
                            if (input.value == 1) {
                                c1_terminal_screen_scroll_reset(terminal_screen);
                                (void)c1_terminal_screen_special(terminal_screen,
                                                                 C1_TERMINAL_KEY_ENTER,
                                                                 0U);
                                status = flush_terminal_replies(terminal_session,
                                                                terminal_screen);
                                if (status != C1_STATUS_OK) {
                                    goto done;
                                }
                                terminal_dirty = true;
                                terminal_render_at = now;
                            }
                        } else {
                            control_pressed = input.value != 0;
                        }
                        continue;
                    }
                    if (terminal_page && terminal_repeatable(input.code) && !app_volume_key) {
                        if (input.value == 1) {
                            repeat_code = input.code;
                            repeat_at = now + C1_TERMINAL_REPEAT_DELAY_MS;
                        } else if (input.value == 0 && repeat_code == input.code) {
                            repeat_code = 0U;
                            repeat_at = -1;
                        }
                    }
                    if (input.value != 1) {
                        continue;
                    }
                    /* The graphical package frontend owns its own IME and
                     * display. Preserve the modified key over its PTY rather
                     * than turning Shift+Space into an ordinary refresh key. */
                    if (terminal_page && terminal_app_mode && shift_key.pressed && input.code == KEY_SPACE &&
                        c1_terminal_is_running(terminal_session)) {
                        status = send_shift_space(terminal_session);
                        if (status != C1_STATUS_OK) goto done;
                        repeat_code = 0; repeat_at = -1;
                        continue;
                    }
                    bool input_target = state.page == C1_UI_PAGE_LOCK_TEXT ||
                        (terminal_page && !terminal_app_mode && !external_app_active &&
                         !state.terminal_symbol_picker && c1_terminal_is_running(terminal_session));
                    if (input_target && input.code == KEY_SPACE && shift_key.pressed) {
                        (void)c1_input_method_toggle(&desktop_input, NULL, now);
                        repeat_code = 0; repeat_at = -1;
                        if (terminal_page) { terminal_dirty = true; terminal_render_at = now; }
                        else {
                            status = render_current(state, terminal_screen, false, sink);
                            if (status != C1_STATUS_OK) goto done;
                        }
                        continue;
                    }
                    if (state.page == C1_UI_PAGE_LOCK_TEXT) {
                        c1_ui_state previous = state;
                        bool input_was_failed = desktop_input.failed;
                        if (route_lock_text_key(&state, &power_policy, input.code, input.value,
                                                shift_key.pressed, control_pressed)) {
                            repeat_code = 0; repeat_at = -1;
                            if (!states_equal(&previous, &state) || input_was_failed != desktop_input.failed) {
                                status = render_current(state, terminal_screen, false, sink);
                                if (status != C1_STATUS_OK) goto done;
                            }
                            continue;
                        }
                    }
                    if (input_target) {
                        uint32_t key = input_keysym(input.code, state.keyboard_layer, shift_key.pressed, control_pressed);
                        if (key && c1_input_method_key(&desktop_input, key, control_pressed ? C1_IME_MOD_CONTROL : 0)) {
                            repeat_code = 0; repeat_at = -1;
                            continue;
                        }
                    }
                    if (terminal_page) {
                        if (input.code == KEY_HOME && !shift_key.pressed) {
                            repeat_code = 0U;
                            repeat_at = -1;
                            state.terminal_symbol_picker = false;
                            if (terminal_app_mode) {
                                c1_terminal_stop(terminal_session);
                                terminal_app_mode = false;
                                terminal_started_once = false;
                                terminal_ended_announced = false;
                            }
                            state.page = C1_UI_PAGE_DESKTOP;
                            state.selection = state.desktop_selection < 5U ? state.desktop_selection : 0U;
                            terminal_app_mode = false;
                            status = render_current(state, terminal_screen, true, sink);
                            if (status != C1_STATUS_OK) {
                                goto done;
                            }
                            next_status_at = now + C1_UI_STATUS_INTERVAL_MS;
                            continue;
                        }
                        if (state.terminal_symbol_picker) {
                            if (input.code == KEY_WAKEUP || input.code == KEY_BACK) {
                                state.terminal_symbol_picker = false;
                            } else if (input.code == KEY_LEFT || input.code == KEY_RIGHT ||
                                       input.code == KEY_UP || input.code == KEY_DOWN) {
                                move_terminal_symbol(&state, input.code);
                            } else if (input.code == KEY_ENTER &&
                                       c1_terminal_is_running(terminal_session)) {
                                char symbol = c1_ui_extended_symbol(state.symbol_selection);

                                if (symbol != '\0') {
                                    c1_terminal_screen_scroll_reset(terminal_screen);
                                    (void)c1_terminal_screen_character(terminal_screen,
                                                                       (unsigned char)symbol,
                                                                       0U);
                                    status = flush_terminal_replies(terminal_session,
                                                                    terminal_screen);
                                    if (status != C1_STATUS_OK) {
                                        goto done;
                                    }
                                }
                                state.terminal_symbol_picker = false;
                            }
                            terminal_dirty = true;
                            terminal_render_at = now;
                            continue;
                        }
                        if (input.code == KEY_WAKEUP) {
                            c1_input_method_close(&desktop_input);
                            memset(&input_view, 0, sizeof(input_view));
                            state.terminal_symbol_picker = true;
                            state.symbol_selection = 0U;
                            repeat_code = 0U;
                            repeat_at = -1;
                            terminal_dirty = true;
                            terminal_render_at = now;
                            continue;
                        }
                        if (input.code == KEY_VOLUMEUP || input.code == KEY_VOLUMEDOWN) {
                            if (app_volume_key) {
                                c1_terminal_key key = input.code == KEY_VOLUMEUP
                                                          ? C1_TERMINAL_KEY_PAGE_DOWN
                                                          : C1_TERMINAL_KEY_PAGE_UP;

                                c1_terminal_screen_scroll_reset(terminal_screen);
                                (void)c1_terminal_screen_special(terminal_screen, key, 0U);
                                status = flush_terminal_replies(terminal_session,
                                                                terminal_screen);
                                if (status != C1_STATUS_OK) {
                                    goto done;
                                }
                            } else if (input.code == KEY_VOLUMEUP) {
                                c1_terminal_screen_scroll_page_up(terminal_screen);
                            } else {
                                c1_terminal_screen_scroll_page_down(terminal_screen);
                            }
                            terminal_dirty = true;
                            terminal_render_at = now;
                            continue;
                        }
                        if (!c1_terminal_is_running(terminal_session)) {
                            if (input.code == KEY_ENTER) {
                                c1_terminal_screen_reset(terminal_screen);
                                status = start_desktop_terminal(terminal_session, terminal_screen, &state, NULL);
                                if (status != C1_STATUS_OK) {
                                    c1_terminal_screen_feed(terminal_screen,
                                                            "TERMINAL START FAILED\r\n",
                                                            23U);
                                } else {
                                    terminal_started_once = true;
                                    terminal_ended_announced = false;
                                }
                                terminal_dirty = true;
                                terminal_render_at = now;
                            }
                            continue;
                        }
                        status = send_terminal_key(terminal_session,
                                                   terminal_screen,
                                                   input.code,
                                                   state.keyboard_layer,
                                                   shift_key.pressed,
                                                   control_pressed,
                                                   &terminal_dirty);
                        if (status != C1_STATUS_OK) {
                            goto done;
                        }
                        if (terminal_dirty && terminal_render_at < 0) {
                            terminal_render_at = now + C1_TERMINAL_RENDER_DELAY_MS;
                        }
                        continue;
                    }
                    if (password_page &&
                        apply_physical_secret_key(&state, input.code, shift_key.pressed)) {
                        status = render_current(state, terminal_screen, false, sink);
                        if (status != C1_STATUS_OK) {
                            goto done;
                        }
                        next_status_at = now + C1_UI_STATUS_INTERVAL_MS;
                        continue;
                    }
                    {
                        c1_ui_event event = map_page_key(state.page, input.code);

                        if (input.code == KEY_TAB) event = C1_UI_EVENT_SELECT_NEXT;
                        if (password_page && input.code == KEY_ENTER) {
                            event = C1_UI_EVENT_SUBMIT;
                        } else if (password_page && input.code == KEY_TAB) {
                            event = C1_UI_EVENT_TOGGLE_SECRET;
                        }
                        if (event != C1_UI_EVENT_NONE) {
                            c1_ui_status current_status;
                            c1_ui_transition transition;
                            c1_ui_page previous_page = state.page;
                            const char *terminal_command;

                            if (!read_ui_status(&current_status)) {
                                status = C1_STATUS_UNAVAILABLE;
                                goto done;
                            }
                            transition = c1_ui_step(state, event, &current_status);
                            save_preferences_transition(&transition.state, &state, &power_policy);
                            if (ignore_settings_update(&state, event, &transition,
                                                      &service_worker)) {
                                continue;
                            }
                            terminal_command = terminal_action_command(transition.action);
                            if (!password_page &&
                                emit_navigation(sink, input.code, event, transition.state) != C1_STATUS_OK) {
                                status = C1_STATUS_IO_ERROR;
                                goto done;
                            }
                            if (transition.action == C1_UI_ACTION_WIFI_DISABLE ||
                                !transition.state.preferences.background_checks)
                                c1_desktop_job_cancel(&desktop_job, now);
                            if (!states_equal(&state, &transition.state)) {
                                state = transition.state;
                            }
                            if (state.page == C1_UI_PAGE_TERMINAL &&
                                previous_page != C1_UI_PAGE_TERMINAL) {
                                bool terminal_was_running =
                                    c1_terminal_is_running(terminal_session);

                                terminal_app_mode =
                                    transition.action == C1_UI_ACTION_TERMINAL_APP ||
                                    transition.action == C1_UI_ACTION_TERMINAL_UPDATE;
                                state.keyboard_layer = C1_UI_KEYBOARD_LOWER;
                                if (terminal_app_mode) {
                                    char *arguments[] = {
                                        transition.action == C1_UI_ACTION_TERMINAL_APP
                                            ? C1_PKG_EXECUTABLE : C1_UPDATER_EXECUTABLE,
                                        transition.action == C1_UI_ACTION_TERMINAL_UPDATE
                                            ? "tui-prepared" : "gui", NULL
                                    };
                                    user_started_once = terminal_started_once;
                                    user_ended_announced = terminal_ended_announced;
                                    user_pending_action = pending_terminal_action;
                                    terminal_session = &app_session;
                                    terminal_screen = &app_screen;
                                    c1_terminal_stop(terminal_session);
                                    c1_terminal_screen_reset(terminal_screen);
                                    input_terminal_allowed = false;
                                    input_focus_update(&state);
                                    status = start_desktop_terminal(terminal_session, terminal_screen, &state, arguments);
                                    terminal_started_once = true;
                                    terminal_ended_announced = false;
                                    pending_terminal_action = C1_UI_ACTION_NONE;
                                    terminal_command = NULL;
                                    transition.action = C1_UI_ACTION_NONE;
                                    terminal_dirty = false;
                                    terminal_render_at = -1;
                                } else {
                                    terminal_session = &user_session;
                                    terminal_screen = &user_screen;
                                    input_terminal_allowed = true;
                                }
                                if (!terminal_app_mode && !terminal_was_running) {
                                    c1_terminal_screen_reset(terminal_screen);
                                    status = start_desktop_terminal(terminal_session, terminal_screen, &state, NULL);
                                    if (status != C1_STATUS_OK) {
                                        c1_terminal_screen_feed(terminal_screen,
                                                                "TERMINAL START FAILED\r\n",
                                                                23U);
                                    } else {
                                        terminal_started_once = true;
                                        terminal_ended_announced = false;
                                    }
                                }
                                if (terminal_command != NULL &&
                                    c1_terminal_is_running(terminal_session)) {
                                    if (terminal_was_running) {
                                        if (c1_terminal_shell_is_foreground(terminal_session)) {
                                            status = c1_terminal_write(terminal_session,
                                                                       terminal_command,
                                                                       strlen(terminal_command));
                                            if (status != C1_STATUS_OK) {
                                                goto done;
                                            }
                                            c1_terminal_screen_scroll_reset(terminal_screen);
                                            terminal_dirty = true;
                                            terminal_render_at =
                                                now + C1_TERMINAL_RENDER_DELAY_MS;
                                        }
                                    } else {
                                        pending_terminal_action = transition.action;
                                    }
                                }
                                if (terminal_command != NULL) {
                                    transition.action = C1_UI_ACTION_NONE;
                                }
                                status = render_current(state, terminal_screen, true, sink);
                                if (status != C1_STATUS_OK) {
                                    goto done;
                                }
                            } else if (transition.action == C1_UI_ACTION_NONE) {
                                status = render_current(state, terminal_screen, false, sink);
                                if (status != C1_STATUS_OK) {
                                    goto done;
                                }
                            }
                            if (transition.action == C1_UI_ACTION_WIFI_DISABLE &&
                                service_worker.pid > 0) {
                                /* Let an in-flight connection restore/commit its config
                                 * before stopping Wi-Fi; never SIGKILL it mid-write. */
                                wifi_stop_pending = true;
                                if (service_worker.cancel_fd >= 0 &&
                                    service_worker.action != C1_UI_ACTION_UPDATE_REFRESH) {
                                    /* EOF is a cooperative cancellation request. */
                                    close(service_worker.cancel_fd);
                                    service_worker.cancel_fd = -1;
                                }
                                state.wifi_notice[0] = '\0';
                                status = render_current(state, terminal_screen, false, sink);
                                if (status != C1_STATUS_OK) goto done;
                            }
                            if (transition.action != C1_UI_ACTION_NONE &&
                                service_worker.pid <= 0) {
                                c1_status worker_status = service_worker_start(
                                    &service_worker, transition.action, &state);

                                if (worker_status == C1_STATUS_OK) {
                                    if (transition.action == C1_UI_ACTION_WIFI_CONNECT) {
                                        state.secret_visible = false;
                                        state.page = C1_UI_PAGE_WIFI;
                                        state.selection = 0U;
                                    }
                                    status = transition.action == C1_UI_ACTION_WIFI_SCAN
                                                 ? render_wifi_scanning(state, sink)
                                                 : render_current(state,
                                                                  terminal_screen,
                                                                  false,
                                                                  sink);
                                    if (status != C1_STATUS_OK) {
                                        goto done;
                                    }
                                } else {
                                    snprintf(state.wifi_notice, sizeof(state.wifi_notice),
                                             "COULD NOT START TASK; PLEASE RETRY");
                                    status = render_current(state, terminal_screen, false, sink);
                                    if (status != C1_STATUS_OK) goto done;
                                }
                            }
                            next_status_at = now + C1_UI_STATUS_INTERVAL_MS;
                        }
                    }
                }
                if (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                    status = C1_STATUS_IO_ERROR;
                    goto done;
                }
            }
            if ((pollfds[index].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
                status = C1_STATUS_IO_ERROR;
                goto done;
            }
        }

        /* poll's power-key deadline also reaches this path with no new events:
         * shutdown occurs while held, not on release or keyboard auto-repeat. */
        now = monotonic_milliseconds();
        if (now < 0) { status = C1_STATUS_IO_ERROR; break; }
        if (power_key_timer(&power_key, pollfds, now) == C1_POWER_KEY_SHUTDOWN) {
            if (request_poweroff_locked(&state, &power_policy, terminal_screen, sink)) {
                shutdown_requested = true;
                continue;
            }
        }

        if (poll_count > C1_UI_INPUT_COUNT) {
            short revents = pollfds[C1_UI_INPUT_COUNT].revents;

            if ((revents & POLLNVAL) != 0) {
                status = C1_STATUS_IO_ERROR;
                break;
            }
            if ((revents & POLLOUT) != 0 && c1_terminal_flush(terminal_session) != C1_STATUS_OK) {
                status = C1_STATUS_IO_ERROR;
                break;
            }
            if ((revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
                bool changed = false;

                status = drain_terminal(terminal_session, terminal_screen, &changed);
                if (status != C1_STATUS_OK && c1_terminal_is_running(terminal_session)) {
                    break;
                }
                if (changed && pending_terminal_action != C1_UI_ACTION_NONE &&
                    c1_terminal_is_running(terminal_session)) {
                    status = send_pending_terminal_action(terminal_session,
                                                          &pending_terminal_action);
                    if (status != C1_STATUS_OK) {
                        break;
                    }
                    if (pending_terminal_action == C1_UI_ACTION_NONE) {
                        c1_terminal_screen_scroll_reset(terminal_screen);
                    }
                }
                if (changed) {
                    terminal_dirty = true;
                    if (terminal_render_at < 0) {
                        terminal_render_at = now + C1_TERMINAL_RENDER_DELAY_MS;
                    }
                }
            }
        }
        if (poll_count > C1_UI_INPUT_COUNT + 1U) {
            short revents = pollfds[C1_UI_INPUT_COUNT + 1U].revents;

            if ((revents & POLLNVAL) != 0) {
                status = C1_STATUS_IO_ERROR;
                break;
            }
            if ((revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
                c1_service_result result;
                c1_wifi_phase phase_before = visible_wifi_phase;

                if (service_worker_finish(&service_worker, &result)) {
                    status = emit_service_result(sink, &result);
                    if (status != C1_STATUS_OK) {
                        break;
                    }
                    adopt_service_result(&result);
                    if (result.action == C1_UI_ACTION_UPDATE_REFRESH && state.page == C1_UI_PAGE_SETTINGS) {
                        c1_ui_status updated = {0};
                        merge_update_status(&updated);
                        snprintf(state.wifi_notice, sizeof(state.wifi_notice), "%s",
                            result.status != C1_STATUS_OK ? c1_ui_tr(state.preferences.language, "检查未完成，请联网后重试", "Check failed; connect and retry") :
                            updated.update_prepared ? c1_ui_tr(state.preferences.language, "已准备更新，请按确认继续", "Update ready; OK to continue") :
                            c1_ui_tr(state.preferences.language, "当前没有可用更新", "No update available"));
                    }
                    finish_wifi_interaction(&state, &result);
                    if (wifi_stop_pending) {
                        wifi_stop_pending = false;
                        c1_status stop_status = service_worker_start(
                            &service_worker, C1_UI_ACTION_WIFI_DISABLE, &state);
                        if (stop_status != C1_STATUS_OK) {
                            snprintf(service_notice, sizeof(service_notice),
                                     "COULD NOT STOP WI-FI; PLEASE RETRY");
                        }
                    } else if (result.action == C1_UI_ACTION_WIFI_SCAN && result.status == C1_STATUS_OK &&
                               result.wifi_snapshot_valid) {
                        c1_ui_status after_scan = {0};
                        merge_service_status(&after_scan);
                        c1_ui_transition automatic = c1_ui_autoconnect(state, &after_scan);
                        if (automatic.action == C1_UI_ACTION_WIFI_CONNECT) {
                            state = automatic.state;
                            if (service_worker_start(&service_worker, automatic.action, &state) != C1_STATUS_OK) {
                                snprintf(service_notice, sizeof(service_notice), "AUTO-CONNECT FAILED; SELECT NETWORK");
                            }
                        }
                    }
                    status = render_current(state, terminal_screen, false, sink);
                    if (status != C1_STATUS_OK) {
                        break;
                    }
                    next_status_at = now + C1_UI_STATUS_INTERVAL_MS;
                } else if (phase_before != visible_wifi_phase) {
                    status = render_current(state, terminal_screen, false, sink);
                    if (status != C1_STATUS_OK) break;
                }
            }
        }
        {
            short hidden_events = pollfds[C1_UI_INPUT_COUNT + 2U].revents;
            bool changed = false;
            if (hidden_events & POLLOUT) (void)c1_terminal_flush(&user_session);
            if (hidden_events & (POLLIN | POLLHUP | POLLERR))
                (void)drain_terminal(&user_session, &user_screen, &changed);
        }
        if (terminal_started_once && !c1_terminal_is_running(terminal_session) &&
            !terminal_ended_announced) {
            if (terminal_app_mode) {
                state.page = C1_UI_PAGE_DESKTOP;
                state.selection = state.desktop_selection < 5U ? state.desktop_selection : 0U;
                terminal_app_mode = false;
                terminal_ended_announced = true;
                pending_terminal_action = C1_UI_ACTION_NONE;
                repeat_code = 0U;
                repeat_at = -1;
                terminal_dirty = false;
                terminal_render_at = -1;
                status = render_current(state, terminal_screen, true, sink);
                if (status != C1_STATUS_OK) {
                    break;
                }
            } else {
                char ended[48];
                int length = snprintf(ended,
                                      sizeof(ended),
                                      "\r\n[SESSION ENDED %d]\r\nENTER RESTARTS\r\n",
                                      c1_terminal_exit_code(terminal_session));

                if (length > 0) {
                    c1_terminal_screen_feed(terminal_screen, ended, (size_t)length);
                }
                terminal_ended_announced = true;
                pending_terminal_action = C1_UI_ACTION_NONE;
                repeat_code = 0U;
                repeat_at = -1;
                terminal_dirty = true;
                terminal_render_at = now;
            }
        }
    }

done:
    c1_desktop_job_cancel(&desktop_job, monotonic_milliseconds());
    for (unsigned attempt = 0; attempt < 600U && desktop_job.pid > 0; ++attempt) {
        (void)c1_desktop_job_poll(&desktop_job, monotonic_milliseconds());
        c1_liveness_beat(monotonic_milliseconds());
        if (desktop_job.pid > 0) (void)poll(NULL, 0, 20);
    }
    c1_input_method_close(&desktop_input);
    (void)c1_input_service_stop(&input_service);
    for (unsigned attempt = 0; attempt < 100U && input_service.pid > 0; ++attempt) {
        (void)c1_input_service_stop(&input_service);
        c1_liveness_beat(monotonic_milliseconds());
        if (input_service.pid > 0) (void)poll(NULL, 0, 20);
    }
    c1_time_sync_stop(&network_clock);
    service_worker_stop(&service_worker);
    c1_terminal_stop(&app_session);
    c1_terminal_stop(&user_session);
    c1_linux_led_chaser_stop(&led_chaser);
    c1_terminal_screen_destroy(&app_screen);
    c1_terminal_screen_destroy(&user_screen);
    c1_liveness_close();
    close_inputs(pollfds);
    return status;
}