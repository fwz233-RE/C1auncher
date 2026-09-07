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
#include "services/wifi.h"
#include "platform/liveness.h"
#include "platform/app_lease.h"
#include "platform/stop.h"
#include "platform/update_request.h"
#include "platform/update_health.h"
#include "update/update.h"
#include "ui/model.h"
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
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define C1_UI_INPUT_COUNT 2U
#define C1_UI_STATUS_INTERVAL_MS 60000
#define C1_EXTERNAL_POWER_INTERVAL_MS 5000
#define C1_TERMINAL_RENDER_DELAY_MS 150
#define C1_TERMINAL_READ_BUDGET 4096U
#define C1_TERMINAL_REPEAT_DELAY_MS 450
#define C1_TERMINAL_REPEAT_INTERVAL_MS 90
#define C1_LED_SYSFS_ROOT "/sys/class/leds"
#define C1_TERMINAL_NEOFETCH_COMMAND "clear; neofetch\r"
#define C1_TERMINAL_APP_COMMAND "clear; /usr/data/c1/bin/c1pkg gui\r"
#define C1_UPDATER_EXECUTABLE "/usr/data/c1/bin/c1updater"
#define C1_TERMINAL_UPDATE_COMMAND "clear; " C1_UPDATER_EXECUTABLE " tui-prepared\r"

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
           left->symbol_selection == right->symbol_selection &&
           left->keyboard_layer == right->keyboard_layer &&
           left->terminal_symbol_picker == right->terminal_symbol_picker &&
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
    if (c1_update_state_load(C1_UPDATE_DEFAULT_STATE_ROOT, &update_state,
                             error, sizeof(error)) == 0) {
        status->update_available = update_state.phase == C1_UPDATE_PREPARED;
    }
}

static bool read_ui_status(c1_ui_status *status)
{
    if (!c1_linux_system_status_read(status)) {
        return false;
    }
    merge_service_status(status);
    merge_update_status(status);
    return true;
}

static c1_status render_state(c1_ui_state state,
                              const c1_ui_status *system_status,
                              c1_terminal_screen *terminal,
                              bool full_refresh,
                              c1_record_sink sink)
{
    uint8_t frame[C1_DISPLAY_FRAME_BYTES];

    if (c1_app_run_active() && !c1_app_lease_terminal_mode()) return C1_STATUS_OK;
    /* Reload only when drawing the lock screen, never by polling during sleep. */
    if (state.page == C1_UI_PAGE_LOCK) c1_wallpaper_load();
    c1_ui_render(frame, &state, system_status, terminal);
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

static void poweroff_heartbeat(void)
{
    c1_liveness_beat(monotonic_milliseconds());
}

static void request_poweroff(c1_record_sink sink)
{
    c1_record record;
    int error;
    c1_record_init(&record, "power", "shutdown-requested");
    (void)c1_record_emit(sink, &record);
    error = c1_linux_poweroff_request(poweroff_heartbeat);
    c1_record_init(&record, "power", error == 0 ? "shutdown-command-completed" : "shutdown-failed");
    (void)c1_record_add_integer(&record, "error", error);
    (void)c1_record_emit(sink, &record);
    if (error != 0) fprintf(stderr, "C1ancher: shutdown failed: %s\n", strerror(error));
    /* Remain alive until init stops us. Returning from the UI on fork success
     * would let the launcher restart it if exec or shutdown subsequently failed. */
}

static const char *terminal_action_command(c1_ui_action action)
{
    switch (action) {
    case C1_UI_ACTION_TERMINAL_NEOFETCH:
        return C1_TERMINAL_NEOFETCH_COMMAND;
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
    case C1_UI_ACTION_TERMINAL_NEOFETCH:
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

/* An offline press and repeated presses during service work are true UI
 * no-ops: no terminal, new worker, navigation publication or forced redraw. */
static bool ignore_desktop_update(const c1_ui_state *state, c1_ui_event event,
                                  const c1_ui_transition *transition,
                                  const c1_service_worker *worker)
{
    return state->page == C1_UI_PAGE_DESKTOP && event == C1_UI_EVENT_ENTER &&
           (transition->action == C1_UI_ACTION_NONE || worker->pid > 0);
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
        state->secret_length > 0U && !wifi_stop_pending) {
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
    case C1_UI_ACTION_TERMINAL_NEOFETCH:
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

static c1_status send_terminal_key(c1_terminal_session *session,
                                   c1_terminal_screen *screen,
                                   uint16_t code,
                                   c1_ui_keyboard_layer layer,
                                   bool shift_pressed,
                                   bool control_pressed,
                                   bool *changed)
{
    char letter = physical_letter(code);
    unsigned int modifiers = control_pressed ? C1_TERMINAL_MOD_CONTROL : 0U;
    bool handled = false;

    if (letter != '\0') {
        char character = control_pressed
                             ? letter
                             : c1_ui_physical_character(layer, letter, shift_pressed);

        handled = c1_terminal_screen_character(screen, (unsigned char)character, modifiers);
    } else if (code == KEY_SPACE) {
        handled = c1_terminal_screen_character(screen, ' ', modifiers);
    } else {
        c1_terminal_key key;

        switch (code) {
        case KEY_UP: key = C1_TERMINAL_KEY_UP; break;
        case KEY_DOWN: key = C1_TERMINAL_KEY_DOWN; break;
        case KEY_LEFT: key = C1_TERMINAL_KEY_LEFT; break;
        case KEY_RIGHT: key = C1_TERMINAL_KEY_RIGHT; break;
        case KEY_DELETE:
            key = shift_pressed ? C1_TERMINAL_KEY_DELETE : C1_TERMINAL_KEY_BACKSPACE;
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

c1_status c1_linux_ui_run(c1_record_sink sink)
{
    struct pollfd pollfds[C1_UI_INPUT_COUNT + 3U] = {
        {-1, POLLIN, 0}, {-1, POLLIN, 0}, {-1, 0, 0}, {-1, POLLIN, 0}, {-1, POLLIN, 0}
    };
    c1_ui_state state = c1_ui_initial_state();
    c1_terminal_session user_session, app_session;
    c1_terminal_screen user_screen, app_screen;
    c1_terminal_session *terminal_session = &user_session;
    c1_terminal_screen *terminal_screen = &user_screen;
    c1_service_worker service_worker;
    c1_power_policy power_policy;
    c1_linux_led_chaser led_chaser;
    c1_status status;
    int64_t started_at;
    int64_t next_status_at;
    int64_t next_external_power_at;
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
    c1_ui_health_retry health_retry = {0};
    bool shift_pressed = false;
    bool shift_chord_used = false;
    bool control_pressed = false;
    c1_power_key power_key = {0};
    bool input_dropped[C1_UI_INPUT_COUNT] = {false};
    uint16_t repeat_code = 0U;
    int64_t repeat_at = -1;
    uint64_t poll_calls = 0U;
    uint64_t poll_events = 0U;
    uint64_t poll_timeouts = 0U;

    c1_liveness_init();
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
    /* Provision public media folders on boot, without requiring any app launch. */
    (void)c1_media_directories_prepare_from(C1_MEDIA_ROOT);
    c1_wallpaper_load();
    update_external_power(&power_policy, started_at);
    status = render_current(state, terminal_screen, true, sink);
    if (status != C1_STATUS_OK) {
        goto done;
    }
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

        if (now < 0) {
            status = C1_STATUS_IO_ERROR;
            break;
        }
        c1_liveness_beat(now);
        c1_linux_poweroff_reap();
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
        if (c1_stop_requested()) {
            status = C1_STATUS_INTERRUPTED;
            break;
        }
        {
            bool lease_active = c1_app_run_active();
            if (lease_active != external_app_active)
                c1_power_key_reset(&power_key);

            if (lease_active) {
                bool terminal_mode = c1_app_lease_terminal_mode();
                if (terminal_mode != external_app_terminal) {
                    external_app_terminal = terminal_mode;
                    terminal_dirty = terminal_mode;
                    terminal_render_at = terminal_mode ? now : -1;
                }
                if (!external_app_active) {
                    external_app_active = true;
                    external_app_terminal = terminal_mode;
                    shift_pressed = false;
                    shift_chord_used = false;
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
                    state.selection = 4U;
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
        if (now >= next_external_power_at) {
            update_external_power(&power_policy, now);
            next_external_power_at = now + C1_EXTERNAL_POWER_INTERVAL_MS;
        }
        c1_linux_led_chaser_tick(&led_chaser, now);
        {
            /* A held power key must not be interrupted by idle lock/suspend. */
            c1_power_action power_action = power_key.down ? C1_POWER_ACTION_NONE :
                c1_power_policy_tick(&power_policy, now);

            if (power_action == C1_POWER_ACTION_ENTER_LOCK) {
                (void)c1_ui_enter_lock(&state);
                c1_linux_led_chaser_quiet(&led_chaser);
                repeat_code = 0U;
                repeat_at = -1;
                terminal_render_at = -1;
                status = render_current(state, terminal_screen, true, sink);
                if (status != C1_STATUS_OK) {
                    break;
                }
                continue;
            }
            if (power_action == C1_POWER_ACTION_SUSPEND) {
                c1_linux_power_context power_context;
                c1_status prepare_status;

                if (service_worker.pid > 0) {
                    c1_power_policy_suspend_failed(&power_policy, now);
                    continue;
                }
                prepare_status = c1_linux_power_prepare(
                    &power_context, terminal_session);

                if (prepare_status == C1_STATUS_OK) {
                    c1_status suspend_status;
                    c1_status resume_status;

                    status = emit_runtime_stats(sink, poll_calls, poll_events, poll_timeouts);
                    if (status != C1_STATUS_OK) {
                        c1_linux_power_rollback(&power_context, terminal_session);
                        break;
                    }
                    c1_linux_led_chaser_stop(&led_chaser);
                    suspend_status = c1_linux_power_suspend(sink);
                    c1_power_key_reset(&power_key);
                    resume_status = c1_linux_power_resume(&power_context, terminal_session);
                    shift_pressed = false;
                    shift_chord_used = false;
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
                        c1_power_policy_suspend_cancelled(&power_policy);
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
                    c1_power_policy_suspend_cancelled(&power_policy);
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
                                           shift_pressed,
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
        if (state.page != C1_UI_PAGE_TERMINAL && state.page != C1_UI_PAGE_LOCK &&
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
            if (state.page != C1_UI_PAGE_TERMINAL && state.page != C1_UI_PAGE_LOCK) {
                poll_timeout = deadline_timeout(poll_timeout, next_status_at, now);
            }
            if (power_policy.state == C1_POWER_LOCKED) {
                poll_timeout = deadline_timeout(poll_timeout, next_external_power_at, now);
            }
            if (state.page == C1_UI_PAGE_TERMINAL) {
                poll_timeout = deadline_timeout(poll_timeout, terminal_render_at, now);
                poll_timeout = deadline_timeout(poll_timeout, repeat_at, now);
            }
            pollfds[C1_UI_INPUT_COUNT + 2U].fd =
                terminal_session != &user_session || state.page != C1_UI_PAGE_TERMINAL
                    ? c1_terminal_fd(&user_session) : -1;
            pollfds[C1_UI_INPUT_COUNT + 2U].events = c1_terminal_poll_events(&user_session);
            pollfds[C1_UI_INPUT_COUNT + 2U].revents = 0;
            poll_count = C1_UI_INPUT_COUNT + 3U;
            poll_timeout = deadline_timeout(poll_timeout, now + C1_HEARTBEAT_INTERVAL_MS, now);
            if (external_app_active || pending_terminal_action != C1_UI_ACTION_NONE)
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
                shift_pressed = false;
                shift_chord_used = false;
                control_pressed = false;
                repeat_code = 0U;
                repeat_at = -1;
                c1_linux_led_chaser_quiet(&led_chaser);
            }
            if (terminal_mode != external_app_terminal) {
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
                    bool terminal_page = state.page == C1_UI_PAGE_TERMINAL;
                    bool password_page = c1_ui_is_password_page(state.page);
                    bool app_volume_key;
                    bool app_confirm_key;

                    if (input.type == EV_SYN && input.code == SYN_DROPPED) {
                        input_dropped[index] = true;
                        c1_power_key_reset(&power_key);
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
                    if (input.code == KEY_POWER || input.code == KEY_WAKEUP) {
                        c1_power_key_action power_action = power_key_input(
                            &power_key, &power_policy, (unsigned int)index, &input, now);
                        if (power_action != C1_POWER_KEY_SHORT) continue;
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
                            state.selection = 4U;
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
                    if (input.value == 1 && state.page == C1_UI_PAGE_DESKTOP &&
                        input.code == KEY_WAKEUP) {
                        if (c1_power_policy_lock(&power_policy, now) && c1_ui_enter_lock(&state)) {
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
                    if (input.value == 1 && state.page == C1_UI_PAGE_LOCK &&
                        (input.code == KEY_OK || input.code == KEY_WAKEUP)) {
                        if (c1_power_policy_unlock(&power_policy, now) && c1_ui_unlock(&state)) {
                            c1_linux_led_chaser_pulse(&led_chaser, now);
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
                    if (state.page == C1_UI_PAGE_LOCK) {
                        continue;
                    }
                    if (input.value == 1) {
                        c1_linux_led_chaser_pulse(&led_chaser, now);
                    }
                    if (input.code == KEY_LEFTSHIFT) {
                        if (input.value == 1) {
                            shift_pressed = true;
                            shift_chord_used = false;
                        } else if (input.value == 0) {
                            bool shift_tap = shift_pressed && !shift_chord_used &&
                                             (password_page || terminal_page);

                            shift_pressed = false;
                            shift_chord_used = false;
                            if (shift_tap) {
                                state.keyboard_layer = c1_ui_keyboard_next_layer(state.keyboard_layer);
                                if (terminal_page) {
                                    terminal_dirty = true;
                                    terminal_render_at = now;
                                } else {
                                    status = render_current(state, terminal_screen, false, sink);
                                    if (status != C1_STATUS_OK) {
                                        goto done;
                                    }
                                }
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
                    if (shift_pressed) {
                        shift_chord_used = true;
                    }
                    if (terminal_page) {
                        if (input.code == KEY_HOME && !shift_pressed) {
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
                            state.selection = 4U;
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
                                status = c1_terminal_start(terminal_session,
                                                           C1_TERMINAL_COLUMNS,
                                                           C1_TERMINAL_ROWS);
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
                                                   shift_pressed,
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
                        apply_physical_secret_key(&state, input.code, shift_pressed)) {
                        status = render_current(state, terminal_screen, false, sink);
                        if (status != C1_STATUS_OK) {
                            goto done;
                        }
                        next_status_at = now + C1_UI_STATUS_INTERVAL_MS;
                        continue;
                    }
                    {
                        c1_ui_event event = map_key(input.code);

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
                            if (ignore_desktop_update(&state, event, &transition,
                                                      &service_worker)) {
                                continue;
                            }
                            terminal_command = terminal_action_command(transition.action);
                            if (!password_page &&
                                emit_navigation(sink, input.code, event, transition.state) != C1_STATUS_OK) {
                                status = C1_STATUS_IO_ERROR;
                                goto done;
                            }
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
                                            ? "/usr/data/c1/bin/c1pkg" : C1_UPDATER_EXECUTABLE,
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
                                    status = c1_terminal_start_exec(terminal_session,
                                                                   C1_TERMINAL_COLUMNS,
                                                                   C1_TERMINAL_ROWS,
                                                                   arguments[0], arguments);
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
                                }
                                if (!terminal_app_mode && !terminal_was_running) {
                                    c1_terminal_screen_reset(terminal_screen);
                                    status = c1_terminal_start(terminal_session,
                                                               C1_TERMINAL_COLUMNS,
                                                               C1_TERMINAL_ROWS);
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
            request_poweroff(sink);
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
                state.selection = 4U;
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