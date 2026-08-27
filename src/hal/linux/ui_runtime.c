#define _DEFAULT_SOURCE 1

#include "hal/linux/ui.h"

#include "display/frame.h"
#include "hal/linux/display.h"
#include "hal/linux/system_state.h"
#include "services/ssh.h"
#include "services/terminal.h"
#include "services/wifi.h"
#include "platform/stop.h"
#include "ui/model.h"
#include "ui/render.h"
#include "ui/terminal_screen.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define C1_UI_INPUT_COUNT 2U
#define C1_UI_REFRESH_INTERVAL_MS 2000
#define C1_TERMINAL_RENDER_DELAY_MS 150
#define C1_TERMINAL_READ_BUDGET 4096U
#define C1_TERMINAL_REPEAT_DELAY_MS 450
#define C1_TERMINAL_REPEAT_INTERVAL_MS 90
#define C1_TERMINAL_NEOFETCH_COMMAND "clear; neofetch\r"
#define C1_TERMINAL_NEOFETCH_CLEAR "\033[2J\033[H"

static int64_t monotonic_milliseconds(void)
{
    struct timespec value;

    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) {
        return -1;
    }
    return (int64_t)value.tv_sec * 1000 + value.tv_nsec / 1000000;
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

static void merge_service_status(c1_ui_status *status)
{
    c1_wifi_snapshot wifi;
    c1_ssh_snapshot ssh;
    size_t index;

    if (c1_wifi_read_snapshot(&wifi)) {
        status->wifi_busy = wifi.state == C1_WIFI_SCANNING || wifi.state == C1_WIFI_CONNECTING;
        status->wifi_connected = wifi.state == C1_WIFI_CONNECTED;
        status->network_count = wifi.network_count;
        for (index = 0U; index < wifi.network_count && index < C1_UI_MAX_NETWORKS; ++index) {
            snprintf(status->networks[index].ssid,
                     sizeof(status->networks[index].ssid),
                     "%s",
                     wifi.networks[index].ssid);
            status->networks[index].signal_dbm = wifi.networks[index].signal_dbm;
            status->networks[index].secured = wifi.networks[index].secured;
        }
        snprintf(status->wifi_connected_ssid,
                 sizeof(status->wifi_connected_ssid),
                 "%s",
                 wifi.connected_ssid);
        snprintf(status->wifi_ipv4, sizeof(status->wifi_ipv4), "%s", wifi.ipv4);
        snprintf(status->wifi_message, sizeof(status->wifi_message), "%s", wifi.error);
    }
    if (c1_ssh_read_snapshot(&ssh)) {
        status->ssh_enabled = ssh.enabled;
        snprintf(status->ssh_ipv4, sizeof(status->ssh_ipv4), "%s", ssh.ipv4);
        snprintf(status->ssh_message, sizeof(status->ssh_message), "%s", ssh.error);
    }
}

static bool read_ui_status(c1_ui_status *status)
{
    if (!c1_linux_system_status_read(status)) {
        return false;
    }
    merge_service_status(status);
    return true;
}

static c1_status render_state(c1_ui_state state,
                              const c1_ui_status *system_status,
                              c1_terminal_screen *terminal,
                              bool full_refresh,
                              c1_record_sink sink)
{
    uint8_t frame[C1_DISPLAY_FRAME_BYTES];

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

static c1_status render_wifi_scanning(c1_ui_state state, c1_record_sink sink)
{
    c1_ui_status system_status;

    if (!read_ui_status(&system_status)) {
        return C1_STATUS_UNAVAILABLE;
    }
    system_status.wifi_busy = true;
    return render_state(state, &system_status, NULL, false, sink);
}

static c1_status perform_action(c1_ui_action action, c1_ui_state *state)
{
    c1_status result = C1_STATUS_OK;

    switch (action) {
    case C1_UI_ACTION_WIFI_SCAN:
        result = c1_wifi_scan(NULL);
        break;
    case C1_UI_ACTION_WIFI_CONNECT:
        result = c1_wifi_connect(state->selected_ssid, state->secret, NULL);
        c1_ui_clear_secret(state);
        state->page = C1_UI_PAGE_WIFI;
        state->selection = 0U;
        break;
    case C1_UI_ACTION_WIFI_DISABLE:
        result = c1_wifi_disable(NULL);
        break;
    case C1_UI_ACTION_SSH_ENABLE:
        result = c1_ssh_enable(NULL);
        break;
    case C1_UI_ACTION_SSH_DISABLE:
        result = c1_ssh_disable(NULL);
        break;
    case C1_UI_ACTION_TERMINAL_NEOFETCH:
    case C1_UI_ACTION_NONE:
        break;
    }
    return result;
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
    struct pollfd pollfds[C1_UI_INPUT_COUNT + 1U] = {
        {-1, POLLIN, 0}, {-1, POLLIN, 0}, {-1, 0, 0}
    };
    c1_ui_state state = c1_ui_initial_state();
    c1_terminal_session terminal_session;
    c1_terminal_screen terminal_screen;
    c1_status status;
    int64_t started_at;
    int64_t next_refresh_at;
    int64_t terminal_render_at = -1;
    bool terminal_dirty = false;
    bool terminal_neofetch_pending = false;
    bool terminal_started_once = false;
    bool terminal_ended_announced = false;
    bool shift_pressed = false;
    bool shift_chord_used = false;
    bool control_pressed = false;
    bool control_chord_used = false;
    uint16_t repeat_code = 0U;
    int64_t repeat_at = -1;

    c1_terminal_init(&terminal_session);
    status = c1_terminal_screen_init(&terminal_screen);
    if (status != C1_STATUS_OK) {
        return status;
    }
    status = open_inputs(pollfds);
    if (status != C1_STATUS_OK) {
        c1_terminal_screen_destroy(&terminal_screen);
        return status;
    }
    started_at = monotonic_milliseconds();
    if (started_at < 0) {
        status = C1_STATUS_IO_ERROR;
        goto done;
    }
    status = render_current(state, &terminal_screen, true, sink);
    if (status != C1_STATUS_OK) {
        goto done;
    }
    next_refresh_at = started_at + C1_UI_REFRESH_INTERVAL_MS;

    for (;;) {
        int poll_result;
        size_t index;
        int64_t now = monotonic_milliseconds();
        nfds_t poll_count = C1_UI_INPUT_COUNT;

        if (now < 0) {
            status = C1_STATUS_IO_ERROR;
            break;
        }
        if (c1_stop_requested()) {
            status = C1_STATUS_INTERRUPTED;
            break;
        }
        if (state.page == C1_UI_PAGE_TERMINAL && !state.terminal_symbol_picker &&
            repeat_code != 0U && repeat_at >= 0 &&
            now >= repeat_at && c1_terminal_is_running(&terminal_session)) {
            if (repeat_code == KEY_VOLUMEUP) {
                c1_terminal_screen_scroll_up(&terminal_screen, 1U);
                terminal_dirty = true;
            } else if (repeat_code == KEY_VOLUMEDOWN) {
                c1_terminal_screen_scroll_down(&terminal_screen, 1U);
                terminal_dirty = true;
            } else {
                status = send_terminal_key(&terminal_session,
                                           &terminal_screen,
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
        if (state.page == C1_UI_PAGE_TERMINAL && terminal_dirty &&
            terminal_render_at >= 0 && now >= terminal_render_at) {
            status = render_current(state, &terminal_screen, false, sink);
            if (status != C1_STATUS_OK) {
                break;
            }
            terminal_dirty = false;
            terminal_render_at = -1;
        }
        if (state.page != C1_UI_PAGE_TERMINAL && state.page != C1_UI_PAGE_LOCK &&
            now >= next_refresh_at) {
            status = render_current(state, &terminal_screen, false, sink);
            if (status != C1_STATUS_OK) {
                break;
            }
            next_refresh_at = now + C1_UI_REFRESH_INTERVAL_MS;
        }

        pollfds[C1_UI_INPUT_COUNT].fd = c1_terminal_fd(&terminal_session);
        pollfds[C1_UI_INPUT_COUNT].events = c1_terminal_poll_events(&terminal_session);
        pollfds[C1_UI_INPUT_COUNT].revents = 0;
        if (pollfds[C1_UI_INPUT_COUNT].fd >= 0) {
            poll_count = C1_UI_INPUT_COUNT + 1U;
        }
        poll_result = poll(pollfds, poll_count, 50);
        if (poll_result < 0) {
            if (errno == EINTR) {
                continue;
            }
            status = C1_STATUS_IO_ERROR;
            break;
        }

        for (index = 0U; index < C1_UI_INPUT_COUNT; ++index) {
            if ((pollfds[index].revents & POLLIN) != 0) {
                struct input_event input;
                ssize_t count;

                while ((count = read(pollfds[index].fd, &input, sizeof(input))) ==
                       (ssize_t)sizeof(input)) {
                    bool terminal_page = state.page == C1_UI_PAGE_TERMINAL;
                    bool password_page = c1_ui_is_password_page(state.page);

                    if (input.type != EV_KEY) {
                        continue;
                    }
                    if (input.code == KEY_OK && input.value == 1 &&
                        c1_ui_toggle_lock(&state)) {
                        repeat_code = 0U;
                        repeat_at = -1;
                        status = render_current(state, &terminal_screen, true, sink);
                        if (status != C1_STATUS_OK) {
                            goto done;
                        }
                        next_refresh_at = now + C1_UI_REFRESH_INTERVAL_MS;
                        continue;
                    }
                    if (state.page == C1_UI_PAGE_LOCK) {
                        continue;
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
                                    status = render_current(state, &terminal_screen, false, sink);
                                    if (status != C1_STATUS_OK) {
                                        goto done;
                                    }
                                }
                            }
                        }
                        continue;
                    }
                    if (input.code == KEY_OK && terminal_page) {
                        if (input.value == 1) {
                            control_pressed = true;
                            control_chord_used = false;
                        } else if (input.value == 0) {
                            bool send_tab = control_pressed && !control_chord_used &&
                                            c1_terminal_is_running(&terminal_session);

                            control_pressed = false;
                            control_chord_used = false;
                            if (send_tab) {
                                c1_terminal_screen_scroll_reset(&terminal_screen);
                                (void)c1_terminal_screen_special(&terminal_screen,
                                                                 C1_TERMINAL_KEY_TAB,
                                                                 0U);
                                status = flush_terminal_replies(&terminal_session,
                                                                &terminal_screen);
                                if (status != C1_STATUS_OK) {
                                    goto done;
                                }
                                terminal_dirty = true;
                                terminal_render_at = now + C1_TERMINAL_RENDER_DELAY_MS;
                            }
                        }
                        continue;
                    }
                    if (terminal_page && terminal_repeatable(input.code)) {
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
                            state.page = C1_UI_PAGE_DESKTOP;
                            state.selection = 4U;
                            status = render_current(state, &terminal_screen, true, sink);
                            if (status != C1_STATUS_OK) {
                                goto done;
                            }
                            next_refresh_at = now + C1_UI_REFRESH_INTERVAL_MS;
                            continue;
                        }
                        if (state.terminal_symbol_picker) {
                            if (input.code == KEY_WAKEUP || input.code == KEY_BACK) {
                                state.terminal_symbol_picker = false;
                            } else if (input.code == KEY_LEFT || input.code == KEY_RIGHT ||
                                       input.code == KEY_UP || input.code == KEY_DOWN) {
                                move_terminal_symbol(&state, input.code);
                            } else if (input.code == KEY_ENTER &&
                                       c1_terminal_is_running(&terminal_session)) {
                                char symbol = c1_ui_extended_symbol(state.symbol_selection);

                                if (symbol != '\0') {
                                    c1_terminal_screen_scroll_reset(&terminal_screen);
                                    (void)c1_terminal_screen_character(&terminal_screen,
                                                                       (unsigned char)symbol,
                                                                       0U);
                                    status = flush_terminal_replies(&terminal_session,
                                                                    &terminal_screen);
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
                        if (input.code == KEY_VOLUMEUP) {
                            c1_terminal_screen_scroll_page_up(&terminal_screen);
                            terminal_dirty = true;
                            terminal_render_at = now;
                            continue;
                        }
                        if (input.code == KEY_VOLUMEDOWN) {
                            c1_terminal_screen_scroll_page_down(&terminal_screen);
                            terminal_dirty = true;
                            terminal_render_at = now;
                            continue;
                        }
                        if (!c1_terminal_is_running(&terminal_session)) {
                            if (input.code == KEY_ENTER) {
                                c1_terminal_screen_reset(&terminal_screen);
                                status = c1_terminal_start(&terminal_session,
                                                           C1_TERMINAL_COLUMNS,
                                                           C1_TERMINAL_ROWS);
                                if (status != C1_STATUS_OK) {
                                    c1_terminal_screen_feed(&terminal_screen,
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
                        if (control_pressed && input.code != KEY_OK) {
                            control_chord_used = true;
                        }
                        status = send_terminal_key(&terminal_session,
                                                   &terminal_screen,
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
                        status = render_current(state, &terminal_screen, false, sink);
                        if (status != C1_STATUS_OK) {
                            goto done;
                        }
                        next_refresh_at = now + C1_UI_REFRESH_INTERVAL_MS;
                        continue;
                    }
                    {
                        c1_ui_event event = map_key(input.code);

                        if (password_page && input.code == KEY_ENTER) {
                            event = C1_UI_EVENT_SUBMIT;
                        }
                        if (event != C1_UI_EVENT_NONE) {
                            c1_ui_status current_status;
                            c1_ui_transition transition;
                            c1_ui_page previous_page = state.page;
                            bool launch_neofetch;

                            if (!read_ui_status(&current_status)) {
                                status = C1_STATUS_UNAVAILABLE;
                                goto done;
                            }
                            transition = c1_ui_step(state, event, &current_status);
                            launch_neofetch =
                                transition.action == C1_UI_ACTION_TERMINAL_NEOFETCH;
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
                                    c1_terminal_is_running(&terminal_session);

                                state.keyboard_layer = C1_UI_KEYBOARD_LOWER;
                                if (!terminal_was_running) {
                                    c1_terminal_screen_reset(&terminal_screen);
                                    status = c1_terminal_start(&terminal_session,
                                                               C1_TERMINAL_COLUMNS,
                                                               C1_TERMINAL_ROWS);
                                    if (status != C1_STATUS_OK) {
                                        c1_terminal_screen_feed(&terminal_screen,
                                                                "TERMINAL START FAILED\r\n",
                                                                23U);
                                    } else {
                                        terminal_started_once = true;
                                        terminal_ended_announced = false;
                                    }
                                }
                                if (launch_neofetch &&
                                    c1_terminal_is_running(&terminal_session)) {
                                    c1_terminal_screen_scroll_reset(&terminal_screen);
                                    c1_terminal_screen_feed(
                                        &terminal_screen,
                                        C1_TERMINAL_NEOFETCH_CLEAR,
                                        sizeof(C1_TERMINAL_NEOFETCH_CLEAR) - 1U);
                                    terminal_dirty = true;
                                    terminal_render_at = now;
                                    if (terminal_was_running) {
                                        status = c1_terminal_write(
                                            &terminal_session,
                                            C1_TERMINAL_NEOFETCH_COMMAND,
                                            sizeof(C1_TERMINAL_NEOFETCH_COMMAND) - 1U);
                                        if (status != C1_STATUS_OK) {
                                            goto done;
                                        }
                                        terminal_dirty = true;
                                        terminal_render_at =
                                            now + C1_TERMINAL_RENDER_DELAY_MS;
                                    } else {
                                        terminal_neofetch_pending = true;
                                    }
                                }
                                if (launch_neofetch) {
                                    transition.action = C1_UI_ACTION_NONE;
                                }
                                status = render_current(state, &terminal_screen, true, sink);
                                if (status != C1_STATUS_OK) {
                                    goto done;
                                }
                            } else if (transition.action == C1_UI_ACTION_NONE) {
                                status = render_current(state, &terminal_screen, false, sink);
                                if (status != C1_STATUS_OK) {
                                    goto done;
                                }
                            }
                            if (transition.action != C1_UI_ACTION_NONE) {
                                if (!password_page) {
                                    status = transition.action == C1_UI_ACTION_WIFI_SCAN
                                                 ? render_wifi_scanning(state, sink)
                                                 : render_current(state,
                                                                  &terminal_screen,
                                                                  false,
                                                                  sink);
                                    if (status != C1_STATUS_OK) {
                                        goto done;
                                    }
                                }
                                (void)perform_action(transition.action, &state);
                                status = render_current(state, &terminal_screen, false, sink);
                                if (status != C1_STATUS_OK) {
                                    goto done;
                                }
                            }
                            next_refresh_at = now + C1_UI_REFRESH_INTERVAL_MS;
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

        if (poll_count > C1_UI_INPUT_COUNT) {
            short revents = pollfds[C1_UI_INPUT_COUNT].revents;

            if ((revents & POLLNVAL) != 0) {
                status = C1_STATUS_IO_ERROR;
                break;
            }
            if ((revents & POLLOUT) != 0 && c1_terminal_flush(&terminal_session) != C1_STATUS_OK) {
                status = C1_STATUS_IO_ERROR;
                break;
            }
            if ((revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
                bool changed = false;

                status = drain_terminal(&terminal_session, &terminal_screen, &changed);
                if (status != C1_STATUS_OK && c1_terminal_is_running(&terminal_session)) {
                    break;
                }
                if (changed && terminal_neofetch_pending &&
                    c1_terminal_is_running(&terminal_session)) {
                    c1_terminal_screen_scroll_reset(&terminal_screen);
                    status = c1_terminal_write(
                        &terminal_session,
                        C1_TERMINAL_NEOFETCH_COMMAND,
                        sizeof(C1_TERMINAL_NEOFETCH_COMMAND) - 1U);
                    if (status != C1_STATUS_OK) {
                        break;
                    }
                    terminal_neofetch_pending = false;
                }
                if (changed) {
                    terminal_dirty = true;
                    if (terminal_render_at < 0) {
                        terminal_render_at = now + C1_TERMINAL_RENDER_DELAY_MS;
                    }
                }
            }
        }
        if (terminal_started_once && !c1_terminal_is_running(&terminal_session) &&
            !terminal_ended_announced) {
            char ended[48];
            int length = snprintf(ended,
                                  sizeof(ended),
                                  "\r\n[SESSION ENDED %d]\r\nENTER RESTARTS\r\n",
                                  c1_terminal_exit_code(&terminal_session));

            if (length > 0) {
                c1_terminal_screen_feed(&terminal_screen, ended, (size_t)length);
            }
            terminal_ended_announced = true;
            terminal_neofetch_pending = false;
            repeat_code = 0U;
            repeat_at = -1;
            terminal_dirty = true;
            terminal_render_at = now;
        }
    }

done:
    c1_terminal_stop(&terminal_session);
    c1_terminal_screen_destroy(&terminal_screen);
    close_inputs(pollfds);
    return status;
}