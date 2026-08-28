#include "core/power_policy.h"
#include "core/record.h"
#include "core/status.h"
#include "display/frame.h"
#include "platform/stop.h"
#include "services/terminal.h"
#include "services/wifi.h"
#include "ui/canvas.h"
#include "ui/model.h"
#include "ui/render.h"
#include "ui/wallpaper.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int failures = 0;

static void expect(bool condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

static void test_display_frame(void)
{
    uint8_t frame[C1_DISPLAY_FRAME_BYTES];
    uint8_t uppercase[C1_DISPLAY_FRAME_BYTES];
    uint8_t lowercase[C1_DISPLAY_FRAME_BYTES];
    uint8_t symbols[C1_DISPLAY_FRAME_BYTES];
    static const char tested_symbols[] = "0123456789!#$%&*+=?()[]{}<>\\/|;,@_-'\":.";
    char symbol[2] = {'\0', '\0'};
    bool lowercase_visible = false;
    bool symbols_visible;
    size_t index;
    size_t symbol_index;

    expect(C1_DISPLAY_FRAME_BYTES == 5624U, "display frame has factory byte size");
    c1_display_frame_clear(frame, false);
    for (index = 0U; index < sizeof(frame); ++index) {
        expect(frame[index] == 0U, "white frame is all zero bits");
    }

    expect(c1_display_frame_set_pixel(frame, 0U, 0U, true), "top pixel sets");
    expect(c1_display_frame_set_pixel(frame, 0U, 7U, true), "bottom strip pixel sets");
    expect(frame[0] == 0x81U, "vertical pixels use MSB-first strip packing");
    expect(c1_display_frame_set_pixel(frame, 295U, 8U, true), "next strip pixel sets");
    expect(frame[591] == 0x80U, "next strip begins after 296 columns");
    expect(!c1_display_frame_set_pixel(frame, 296U, 0U, true), "x overflow is rejected");
    expect(!c1_display_frame_set_pixel(frame, 0U, 152U, true), "y overflow is rejected");

    expect(c1_display_frame_set_pixel(frame, 0U, 0U, false), "black pixel clears");
    expect(frame[0] == 0x01U, "clearing preserves neighboring vertical pixel");

    c1_display_frame_clear(frame, true);
    expect(frame[0] == 0xffU && frame[sizeof(frame) - 1U] == 0xffU,
           "black frame is all one bits");

    c1_display_frame_clear(uppercase, false);
    c1_display_frame_clear(lowercase, false);
    c1_canvas_text(uppercase, 0U, 0U, "WIFI", 1U, true);
    c1_canvas_text(lowercase, 0U, 0U, "wifi", 1U, true);
    for (index = 0U; index < sizeof(lowercase); ++index) {
        if (lowercase[index] != 0U) {
            lowercase_visible = true;
            break;
        }
    }
    expect(lowercase_visible, "lowercase SSIDs use readable glyphs");
    expect(memcmp(uppercase, lowercase, sizeof(uppercase)) != 0,
           "keyboard lowercase and uppercase layers remain distinct");
    for (symbol_index = 0U; tested_symbols[symbol_index] != '\0'; ++symbol_index) {
        symbols_visible = false;
        symbol[0] = tested_symbols[symbol_index];
        c1_display_frame_clear(symbols, false);
        c1_canvas_text(symbols, 0U, 0U, symbol, 1U, true);
        for (index = 0U; index < sizeof(symbols); ++index) {
            if (symbols[index] != 0U) {
                symbols_visible = true;
                break;
            }
        }
        expect(symbols_visible, "each password symbol uses a visible glyph");
    }
}

static bool frame_pixel(const uint8_t *frame, uint32_t x, uint32_t y)
{
    size_t offset = (size_t)(y / C1_DISPLAY_STRIP_HEIGHT) * C1_DISPLAY_WIDTH + x;
    uint8_t mask = (uint8_t)(0x80U >> (y % C1_DISPLAY_STRIP_HEIGHT));
    return (frame[offset] & mask) != 0U;
}

static bool frame_region_equal(const uint8_t *left,
                               const uint8_t *right,
                               uint32_t x,
                               uint32_t y,
                               uint32_t width,
                               uint32_t height)
{
    uint32_t row;
    uint32_t column;

    for (row = 0U; row < height; ++row) {
        for (column = 0U; column < width; ++column) {
            if (frame_pixel(left, x + column, y + row) != frame_pixel(right, x + column, y + row)) {
                return false;
            }
        }
    }
    return true;
}

static void test_wifi_ssid_codec(void)
{
    static const char escaped[] =
        "\\xe6\\xb1\\xa4\\xe6\\x82\\xa6\\xe6\\xb8\\xa9\\xe6\\xb3\\x89"
        "\\xe6\\xb1\\x97\\xe8\\x92\\xb8\\xe9\\xa6\\x86";
    static const char expected[] = {
        (char)0xe6, (char)0xb1, (char)0xa4, (char)0xe6, (char)0x82, (char)0xa6,
        (char)0xe6, (char)0xb8, (char)0xa9, (char)0xe6, (char)0xb3, (char)0x89,
        (char)0xe6, (char)0xb1, (char)0x97, (char)0xe8, (char)0x92, (char)0xb8,
        (char)0xe9, (char)0xa6, (char)0x86, '\0'
    };
    static const char expected_hex[] =
        "e6b1a4e682a6e6b8a9e6b389e6b197e892b8e9a686";
    char decoded[C1_WIFI_SSID_CAPACITY];
    char encoded[C1_WIFI_SSID_CAPACITY * 2U];
    char small[8];

    expect(c1_wifi_decode_scan_ssid(escaped, decoded, sizeof(decoded)) &&
               memcmp(decoded, expected, sizeof(expected)) == 0,
           "Wi-Fi scan decoding restores an escaped UTF-8 SSID");
    expect(c1_wifi_encode_control_ssid(decoded, encoded, sizeof(encoded)) &&
               strcmp(encoded, expected_hex) == 0,
           "Wi-Fi control encoding preserves every UTF-8 SSID byte");
    expect(c1_wifi_decode_scan_ssid("Cafe\\\\Guest", decoded, sizeof(decoded)) &&
               strcmp(decoded, "Cafe\\Guest") == 0,
           "Wi-Fi scan decoding restores escaped backslashes");
    expect(!c1_wifi_decode_scan_ssid("bad\\x0", decoded, sizeof(decoded)),
           "Wi-Fi scan decoding rejects truncated hexadecimal escapes");
    expect(!c1_wifi_decode_scan_ssid(escaped, small, sizeof(small)),
           "Wi-Fi scan decoding rejects an SSID that exceeds its destination");
}

static void test_ui(void)
{
    c1_ui_state state = c1_ui_initial_state();
    c1_ui_status status = {
        .battery_available = true,
        .battery_percent = 87U,
        .wifi_connected = true,
        .network_count = 1U,
        .wifi_connected_ssid = "TEST-NETWORK",
        .networks = {{"TEST-NETWORK", -48, true}},
        .ssh_enabled = false,
        .time_available = true,
        .hour = 23U,
        .minute = 5U
    };
    c1_ui_transition transition;
    uint8_t desktop[C1_DISPLAY_FRAME_BYTES];
    uint8_t page[C1_DISPLAY_FRAME_BYTES];
    uint8_t connected_status[C1_DISPLAY_FRAME_BYTES];
    uint8_t generic_status[C1_DISPLAY_FRAME_BYTES];

    expect(state.page == C1_UI_PAGE_DESKTOP && state.selection == 4U,
           "UI starts locked on the center desktop cell");
    expect(c1_ui_enter_lock(&state) && state.page == C1_UI_PAGE_LOCK,
           "desktop OK enters the wallpaper lock screen");
    c1_ui_render(page, &state, &status, NULL);
    expect(memcmp(page, c1_wallpaper_frame, sizeof(page)) == 0,
           "lock screen renders the embedded 296 by 152 wallpaper exactly");
    expect(c1_ui_unlock(&state) && state.page == C1_UI_PAGE_DESKTOP,
           "lock-screen OK returns to the desktop");
    {
        static const c1_ui_page confirm_pages[] = {
            C1_UI_PAGE_WIFI,
            C1_UI_PAGE_SSH,
            C1_UI_PAGE_TERMINAL,
            C1_UI_PAGE_WIFI_PASSWORD
        };
        size_t page_index;

        for (page_index = 0U;
             page_index < sizeof(confirm_pages) / sizeof(confirm_pages[0]);
             ++page_index) {
            c1_ui_state confirm_state = c1_ui_initial_state();

            confirm_state.page = confirm_pages[page_index];
            transition = c1_ui_step(confirm_state, C1_UI_EVENT_ENTER, &status);
            expect(transition.state.page != C1_UI_PAGE_LOCK,
                   "OK keeps its existing confirm function outside the desktop");
        }
    }
    transition = c1_ui_step(state, C1_UI_EVENT_ENTER, &status);
    expect(transition.state.page == C1_UI_PAGE_DESKTOP &&
               transition.state.selection == 4U &&
               transition.action == C1_UI_ACTION_NONE,
           "Enter does not activate a desktop direction");
    transition = c1_ui_step(state, C1_UI_EVENT_RIGHT, &status);
    expect(transition.state.page == C1_UI_PAGE_TERMINAL &&
               transition.action == C1_UI_ACTION_NONE,
           "right opens the terminal immediately");
    state = transition.state;
    transition = c1_ui_step(state, C1_UI_EVENT_ENTER, &status);
    expect(transition.state.page == C1_UI_PAGE_TERMINAL &&
               transition.action == C1_UI_ACTION_NONE,
           "terminal input is handled by the live PTY runtime");
    state = c1_ui_reduce(state, C1_UI_EVENT_HOME);
    expect(state.page == C1_UI_PAGE_DESKTOP && state.selection == 4U,
           "home returns to the locked center desktop cell");

    state = c1_ui_initial_state();
    transition = c1_ui_step(state, C1_UI_EVENT_LEFT, &status);
    expect(transition.state.page == C1_UI_PAGE_SSH,
           "left opens the SSH page immediately");
    transition = c1_ui_step(transition.state, C1_UI_EVENT_ENTER, &status);
    expect(transition.state.page == C1_UI_PAGE_SSH &&
               transition.action == C1_UI_ACTION_SSH_ENABLE,
           "SSH page enables service directly without a password page");
    status.ssh_enabled = true;
    transition = c1_ui_step(transition.state, C1_UI_EVENT_ENTER, &status);
    expect(transition.action == C1_UI_ACTION_SSH_DISABLE,
           "SSH page disables an active service directly");
    status.ssh_enabled = false;

    state.page = C1_UI_PAGE_SSH;
    status.ssh_enabled = true;
    snprintf(status.ssh_ipv4, sizeof(status.ssh_ipv4), "%s", "172.16.99.62");
    c1_ui_render(connected_status, &state, &status, NULL);
    expect(frame_pixel(connected_status, 148U, 80U),
           "enabled SSH page renders a visible large address");
    expect(frame_pixel(connected_status, 12U, 105U),
           "enabled SSH page identifies passwordless root access");
    status.ssh_enabled = false;
    status.ssh_ipv4[0] = '\0';

    state = c1_ui_initial_state();
    transition = c1_ui_step(state, C1_UI_EVENT_UP, &status);
    expect(transition.state.page == C1_UI_PAGE_WIFI && transition.action == C1_UI_ACTION_WIFI_SCAN,
           "up opens Wi-Fi and starts a real scan action");
    transition = c1_ui_step(transition.state, C1_UI_EVENT_ENTER, &status);
    expect(transition.action == C1_UI_ACTION_WIFI_SCAN,
           "left Wi-Fi action button starts a scan");
    transition = c1_ui_step(transition.state, C1_UI_EVENT_RIGHT, &status);
    expect(transition.state.selection == 1U,
           "right moves to the Wi-Fi off action button");
    transition = c1_ui_step(transition.state, C1_UI_EVENT_ENTER, &status);
    expect(transition.action == C1_UI_ACTION_WIFI_DISABLE,
           "right Wi-Fi action button turns Wi-Fi off");
    transition = c1_ui_step(transition.state, C1_UI_EVENT_LEFT, &status);
    transition = c1_ui_step(transition.state, C1_UI_EVENT_DOWN, &status);
    expect(transition.state.selection == 2U,
           "down moves from the action bar to the first network");
    status.networks[0].secured = false;
    transition = c1_ui_step(transition.state, C1_UI_EVENT_ENTER, &status);
    expect(transition.state.page == C1_UI_PAGE_WIFI &&
               transition.action == C1_UI_ACTION_WIFI_CONNECT &&
               transition.state.secret_length == 0U,
           "open network selection connects immediately without a password page");
    expect(strcmp(transition.state.selected_ssid, "TEST-NETWORK") == 0,
           "open network connection retains the selected SSID");
    status.networks[0].secured = true;
    transition = c1_ui_step(transition.state, C1_UI_EVENT_ENTER, &status);
    expect(transition.state.page == C1_UI_PAGE_WIFI_PASSWORD &&
               transition.action == C1_UI_ACTION_NONE,
           "secured network selection opens the password input page");
    expect(strcmp(transition.state.selected_ssid, "TEST-NETWORK") == 0,
           "selected SSID is retained for connection");
    expect(transition.state.keyboard_layer == C1_UI_KEYBOARD_LOWER,
           "password page starts in physical lowercase mode");
    expect(c1_ui_keyboard_next_layer(C1_UI_KEYBOARD_LOWER) == C1_UI_KEYBOARD_UPPER,
           "Shift advances lowercase to uppercase");
    expect(c1_ui_keyboard_next_layer(C1_UI_KEYBOARD_UPPER) == C1_UI_KEYBOARD_SYMBOLS,
           "Shift advances uppercase to symbols");
    expect(c1_ui_keyboard_next_layer(C1_UI_KEYBOARD_SYMBOLS) == C1_UI_KEYBOARD_LOWER,
           "Shift wraps symbols to lowercase");
    expect(c1_ui_physical_character(C1_UI_KEYBOARD_LOWER, 'q', false) == 'q',
           "lowercase layer keeps physical letters lowercase");
    expect(c1_ui_physical_character(C1_UI_KEYBOARD_UPPER, 'q', false) == 'Q',
           "uppercase layer capitalizes physical letters");
    expect(c1_ui_physical_character(C1_UI_KEYBOARD_SYMBOLS, 'q', false) == '1',
           "symbol layer uses the physical Q keycap number");
    expect(c1_ui_physical_character(C1_UI_KEYBOARD_LOWER, 'q', true) == '1',
           "held Shift temporarily uses the physical keycap symbol");
    expect(c1_ui_physical_character(C1_UI_KEYBOARD_LOWER, 'a', true) == '\'',
           "held Shift maps the physical A keycap apostrophe");
    expect(c1_ui_physical_character(C1_UI_KEYBOARD_LOWER, 'm', true) == ',',
           "held Shift maps the physical M keycap comma");
    expect(c1_ui_physical_character(C1_UI_KEYBOARD_LOWER, '?', true) == '\0',
           "unknown physical keys have no password character");
    {
        char extended_symbols[C1_UI_EXTENDED_SYMBOL_COUNT + 1U];
        uint32_t symbol_index;

        for (symbol_index = 0U; symbol_index < C1_UI_EXTENDED_SYMBOL_COUNT; ++symbol_index) {
            extended_symbols[symbol_index] = c1_ui_extended_symbol(symbol_index);
        }
        extended_symbols[C1_UI_EXTENDED_SYMBOL_COUNT] = '\0';
        expect(strcmp(extended_symbols, "!+=[]{}<>\\|_") == 0,
               "extended symbol palette covers every keycap gap");
    }
    expect(c1_ui_extended_symbol(C1_UI_EXTENDED_SYMBOL_COUNT) == '\0',
           "extended symbol selection rejects overflow");
    expect(c1_ui_secret_append(&transition.state, 'a'), "physical lowercase key appends");
    expect(c1_ui_secret_append(&transition.state, 'Z'), "physical uppercase key appends");
    expect(c1_ui_secret_append(&transition.state, ' '), "physical space appends");
    expect(c1_ui_secret_delete(&transition.state), "physical delete removes a character");
    expect(transition.state.secret_length == 2U, "direct physical input updates secret length");
    c1_ui_clear_secret(&transition.state);
    transition = c1_ui_step(transition.state, C1_UI_EVENT_ENTER, &status);
    expect(transition.state.secret_length == 0U && transition.action == C1_UI_ACTION_NONE,
           "OK does nothing when no extended symbol palette is visible");
    transition.state.keyboard_layer = C1_UI_KEYBOARD_SYMBOLS;
    transition.state.symbol_selection = 0U;
    transition = c1_ui_step(transition.state, C1_UI_EVENT_ENTER, &status);
    expect(strcmp(transition.state.secret, "!") == 0,
           "OK inserts the selected extended symbol");
    transition = c1_ui_step(transition.state, C1_UI_EVENT_RIGHT, &status);
    transition = c1_ui_step(transition.state, C1_UI_EVENT_ENTER, &status);
    expect(strcmp(transition.state.secret, "!+") == 0,
           "arrows select another extended symbol");
    transition = c1_ui_step(transition.state, C1_UI_EVENT_SUBMIT, &status);
    expect(transition.action == C1_UI_ACTION_WIFI_CONNECT, "physical Enter requests Wi-Fi connection");
    c1_display_frame_clear(page, false);
    c1_canvas_text(page, 12U, 52U, transition.state.secret, 2U, true);
    c1_ui_render(desktop, &transition.state, &status, NULL);
    expect(frame_region_equal(desktop, page, 12U, 52U, 16U, 10U),
           "Wi-Fi password input displays entered characters at double size");

    state = c1_ui_initial_state();
    c1_ui_render(desktop, &state, &status, NULL);
    expect(frame_pixel(desktop, 100U, 52U), "center desktop cell is locked black");
    expect(!frame_pixel(desktop, 146U, 72U) && !frame_pixel(desktop, 148U, 76U),
           "center desktop cell shows a white origin point instead of HOME text");
    expect(!frame_pixel(desktop, 100U, 10U), "direction cells remain unselected");
    expect(frame_pixel(desktop, 98U, 25U) && frame_pixel(desktop, 99U, 25U) &&
               frame_pixel(desktop, 197U, 25U) && frame_pixel(desktop, 198U, 25U),
           "desktop uses two-pixel internal vertical separators");
    expect(frame_pixel(desktop, 50U, 50U) && frame_pixel(desktop, 50U, 51U) &&
               frame_pixel(desktop, 50U, 101U) && frame_pixel(desktop, 50U, 102U),
           "desktop uses two-pixel internal horizontal separators");
    expect(frame_pixel(desktop, 0U, 0U) && frame_pixel(desktop, 295U, 0U) &&
               frame_pixel(desktop, 0U, 151U) && frame_pixel(desktop, 295U, 151U),
           "four corner status cells use black backgrounds to the screen edge");

    c1_ui_render(connected_status, &state, &status, NULL);
    status.wifi_connected = false;
    status.wifi_connected_ssid[0] = '\0';
    c1_ui_render(generic_status, &state, &status, NULL);
    expect(!frame_region_equal(connected_status, generic_status, 0U, 0U, 99U, 51U),
           "top-left status cell distinguishes connected and disconnected Wi-Fi");

    status.wifi_connected = true;
    snprintf(status.wifi_connected_ssid,
             sizeof(status.wifi_connected_ssid),
             "%s",
             "TEST-NETWORK");
    state.page = C1_UI_PAGE_WIFI;
    status.wifi_busy = false;
    c1_ui_render(connected_status, &state, &status, NULL);
    status.wifi_connected = false;
    status.wifi_connected_ssid[0] = '\0';
    c1_ui_render(generic_status, &state, &status, NULL);
    expect(!frame_region_equal(connected_status, generic_status, 220U, 59U, 68U, 19U),
           "connected network row has a prominent connection label");
    expect(!frame_region_equal(connected_status, generic_status, 8U, 145U, 180U, 7U),
           "Wi-Fi page footer identifies the connected network");

    status.wifi_connected = true;
    snprintf(status.wifi_connected_ssid,
             sizeof(status.wifi_connected_ssid),
             "%s",
             "TEST-NETWORK");
    status.wifi_busy = true;
    c1_ui_render(page, &state, &status, NULL);
    expect(frame_pixel(page, 66U, 124U),
           "Wi-Fi scanning state renders a visible loading indicator");
    status.wifi_busy = false;
    state = c1_ui_initial_state();
    transition = c1_ui_step(state, C1_UI_EVENT_DOWN, &status);
    state = transition.state;
    expect(state.page == C1_UI_PAGE_TERMINAL &&
               transition.action == C1_UI_ACTION_TERMINAL_NEOFETCH,
           "down opens DEVICE in the terminal and requests Neofetch");
    transition = c1_ui_step(state, C1_UI_EVENT_NONE, &status);
    expect(transition.state.page == C1_UI_PAGE_TERMINAL &&
               transition.action == C1_UI_ACTION_NONE,
           "DEVICE direction requests Neofetch only for its entry event");
    c1_ui_render(page, &state, &status, NULL);
    expect(memcmp(desktop, page, sizeof(page)) != 0,
           "bottom DEVICE direction reuses the terminal instead of a detail page");
}

static int64_t test_milliseconds(void)
{
    struct timespec value;

    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) {
        return -1;
    }
    return (int64_t)value.tv_sec * 1000 + value.tv_nsec / 1000000;
}

static void test_terminal_screen(void)
{
    c1_terminal_screen terminal;
    const c1_terminal_cell *cells;
    char reply[32];
    size_t count;
    c1_ui_state state = c1_ui_initial_state();
    c1_ui_status status = {0};
    uint8_t frame[C1_DISPLAY_FRAME_BYTES];

    expect(c1_terminal_screen_init(&terminal) == C1_STATUS_OK,
           "terminal screen initializes libtsm");
    if (terminal.screen == NULL) {
        return;
    }
    c1_terminal_screen_feed(&terminal, "ABC\033[2;3HZ\033[7mR\033[0m", 19U);
    cells = c1_terminal_screen_cells(&terminal);
    expect(cells != NULL && cells[0].codepoint == 'A' && cells[2].codepoint == 'C',
           "terminal screen retains ordinary text");
    expect(cells != NULL && cells[C1_TERMINAL_COLUMNS + 2U].codepoint == 'Z',
           "terminal screen applies ANSI cursor positioning");
    expect(cells != NULL && cells[C1_TERMINAL_COLUMNS + 3U].codepoint == 'R' &&
               cells[C1_TERMINAL_COLUMNS + 3U].inverse,
           "terminal screen preserves inverse attributes");

    expect(c1_terminal_screen_character(&terminal, 'c', C1_TERMINAL_MOD_CONTROL),
           "terminal screen accepts Ctrl combinations");
    count = c1_terminal_screen_take_reply(&terminal, reply, sizeof(reply));
    expect(count == 1U && reply[0] == 3,
           "Ctrl-C is encoded as an interrupt byte");
    expect(c1_terminal_screen_special(&terminal, C1_TERMINAL_KEY_UP, 0U),
           "terminal screen accepts cursor keys");
    count = c1_terminal_screen_take_reply(&terminal, reply, sizeof(reply));
    expect(count >= 3U && reply[0] == '\033' && reply[1] == '[',
           "cursor key produces an ANSI sequence");

    c1_terminal_screen_feed(&terminal, "\033[19;1H\033[7m \033[0m", 16U);
    state.page = C1_UI_PAGE_TERMINAL;
    c1_ui_render(frame, &state, &status, &terminal);
    expect(frame_pixel(frame, 0U, 1U),
           "full-screen terminal renders its first glyph");
    expect(frame_pixel(frame, 0U, 151U),
           "terminal content uses the former help row at the bottom");
    expect(!frame_pixel(frame, 295U, 151U),
           "terminal renderer stays inside the 49-column grid");
    c1_terminal_screen_destroy(&terminal);
}

static void test_terminal_pty(void)
{
    static const char command[] =
        "printf 'PTY_OK:'; test -t 0 && test -t 1 && test -t 2 && printf 'TTY '; stty size; exit\n";
    c1_terminal_session session;
    char output[4096] = {0};
    size_t length = 0U;
    int64_t deadline;

    c1_terminal_init(&session);
    expect(c1_terminal_start(&session, C1_TERMINAL_COLUMNS, C1_TERMINAL_ROWS) == C1_STATUS_OK,
           "terminal service starts an interactive PTY shell");
    if (!c1_terminal_is_running(&session)) {
        c1_terminal_stop(&session);
        return;
    }
    expect(c1_terminal_write(&session, command, sizeof(command) - 1U) == C1_STATUS_OK,
           "terminal service writes interactive input");
    deadline = test_milliseconds() + 5000;
    while (test_milliseconds() < deadline &&
           (c1_terminal_is_running(&session) || length == 0U)) {
        struct pollfd descriptor = {
            c1_terminal_fd(&session), c1_terminal_poll_events(&session), 0
        };
        int result = poll(&descriptor, 1U, 50);

        if (result > 0 && (descriptor.revents & POLLOUT) != 0) {
            (void)c1_terminal_flush(&session);
        }
        if (result > 0 && (descriptor.revents & (POLLIN | POLLHUP | POLLERR)) != 0 &&
            length + 1U < sizeof(output)) {
            ssize_t count = c1_terminal_read(&session,
                                             output + length,
                                             sizeof(output) - length - 1U);

            if (count > 0) {
                length += (size_t)count;
                output[length] = '\0';
            }
        }
    }
    expect(strstr(output, "PTY_OK:TTY 19 49") != NULL,
           "PTY shell has terminal stdin, stdout, stderr and the 19 by 49 window");
    expect(!c1_terminal_is_running(&session),
           "PTY shell exit is detected without a timeout kill");
    c1_terminal_stop(&session);
}

static void test_power_policy(void)
{
    c1_power_policy policy;
    c1_ui_state state = c1_ui_initial_state();
    int64_t now = 1000;
    int64_t non_desktop_locked_at;

    c1_power_policy_init(&policy, now);
    expect(policy.state == C1_POWER_ACTIVE,
           "power policy starts active");
    expect(c1_power_policy_timeout(&policy, now) == C1_POWER_IDLE_TIMEOUT_MS,
           "active policy schedules the five-minute idle deadline on the desktop");
    expect(c1_power_policy_tick(&policy,
                                now + C1_POWER_IDLE_TIMEOUT_MS - 1) ==
               C1_POWER_ACTION_NONE,
           "idle desktop remains active before five minutes");
    c1_power_policy_note_activity(&policy, now + C1_POWER_IDLE_TIMEOUT_MS - 1);
    expect(c1_power_policy_tick(&policy,
                                now + (2 * C1_POWER_IDLE_TIMEOUT_MS) - 2) ==
               C1_POWER_ACTION_NONE,
           "desktop input resets the idle deadline");
    expect(c1_power_policy_tick(&policy,
                                now + (2 * C1_POWER_IDLE_TIMEOUT_MS) - 1) ==
               C1_POWER_ACTION_ENTER_LOCK,
           "idle desktop requests lock at five minutes");
    expect(c1_ui_enter_lock(&state) && state.page == C1_UI_PAGE_LOCK,
           "idle desktop enters the wallpaper lock screen");
    expect(c1_power_policy_unlock(&policy, now + (2 * C1_POWER_IDLE_TIMEOUT_MS)) &&
               c1_ui_unlock(&state),
           "desktop idle-lock test returns to active state");

    state.page = C1_UI_PAGE_WIFI_PASSWORD;
    snprintf(state.secret, sizeof(state.secret), "%s", "temporary-password");
    state.secret_length = strlen(state.secret);
    state.terminal_symbol_picker = true;
    non_desktop_locked_at = now + (3 * C1_POWER_IDLE_TIMEOUT_MS);
    expect(c1_power_policy_tick(&policy, non_desktop_locked_at) ==
               C1_POWER_ACTION_ENTER_LOCK,
           "idle non-desktop page still requests lock");
    expect(c1_ui_enter_lock(&state) && state.page == C1_UI_PAGE_LOCK &&
               state.secret_length == 0U && state.secret[0] == '\0' &&
               !state.terminal_symbol_picker,
           "automatic lock clears transient input state");
    expect(c1_power_policy_tick(&policy,
                                non_desktop_locked_at +
                                    C1_POWER_LOCK_TIMEOUT_MS - 1) == C1_POWER_ACTION_NONE,
           "lock grace period delays suspend");
    expect(c1_power_policy_tick(&policy,
                                non_desktop_locked_at +
                                    C1_POWER_LOCK_TIMEOUT_MS) == C1_POWER_ACTION_SUSPEND,
           "locked policy requests suspend after grace period");

    c1_power_policy_suspend_failed(&policy, non_desktop_locked_at +
                                                C1_POWER_LOCK_TIMEOUT_MS);
    expect(policy.state == C1_POWER_LOCKED,
           "failed suspend returns to locked state");
    expect(c1_power_policy_tick(&policy,
                                non_desktop_locked_at +
                                    C1_POWER_LOCK_TIMEOUT_MS +
                                    C1_POWER_RETRY_DELAY_MS - 1) == C1_POWER_ACTION_NONE,
           "failed suspend uses bounded retry delay");

    expect(c1_power_policy_tick(&policy,
                                non_desktop_locked_at +
                                    C1_POWER_LOCK_TIMEOUT_MS +
                                    C1_POWER_RETRY_DELAY_MS) == C1_POWER_ACTION_SUSPEND,
           "failed suspend retries after the bounded delay");
    c1_power_policy_suspend_failed(&policy, now + 1000000);

    c1_power_policy_resumed(&policy, now + 1000000);
    expect(c1_power_policy_filter_wakeup(&policy, true),
           "resume suppresses the wake key press");
    expect(c1_power_policy_filter_wakeup(&policy, false),
           "resume consumes the wake key release");
    expect(!c1_power_policy_filter_wakeup(&policy, true),
           "a later wake key press is delivered");
    expect(c1_power_policy_unlock(&policy, now + 1000001) &&
               policy.state == C1_POWER_ACTIVE,
           "explicit unlock returns to active state");
    expect(c1_ui_unlock(&state) && state.page == C1_UI_PAGE_DESKTOP,
           "explicit UI unlock returns to desktop");

    c1_power_policy_init(&policy, now);
    expect(c1_power_policy_lock(&policy, now),
           "desktop can enter lock explicitly");
    c1_power_policy_suspend_unavailable(&policy);
    expect(c1_power_policy_tick(&policy,
                                now + C1_POWER_LOCK_TIMEOUT_MS +
                                    C1_POWER_RETRY_DELAY_MS) == C1_POWER_ACTION_NONE &&
               c1_power_policy_timeout(&policy, now) == -1,
           "unsupported suspend leaves locked policy indefinitely blocked");
}

static void test_stop_signal(void)
{
    c1_stop_reset();
    c1_stop_install();
    expect(!c1_stop_requested(), "stop state begins clear");
    expect(raise(SIGTERM) == 0 && c1_stop_requested(),
           "termination signal interrupts the event loop policy");
    c1_stop_reset();
    expect(!c1_stop_requested(), "stop state resets for a restart");
}

static void test_ndjson(void)
{
    static const char expected[] =
        "{\"category\":\"test\",\"event\":\"escape\",\"value\":\"a\\nb\",\"count\":7,\"ok\":true}\n";
    FILE *stream = tmpfile();
    c1_record record;
    char output[256] = {0};

    expect(stream != NULL, "temporary stream opens");
    if (stream == NULL) {
        return;
    }

    c1_record_init(&record, "test", "escape");
    expect(c1_record_add_text(&record, "value", "a\nb") == C1_STATUS_OK, "text field added");
    expect(c1_record_add_integer(&record, "count", 7) == C1_STATUS_OK, "integer field added");
    expect(c1_record_add_boolean(&record, "ok", true) == C1_STATUS_OK, "boolean field added");
    expect(c1_record_emit(c1_ndjson_sink(stream), &record) == C1_STATUS_OK, "record emitted");

    rewind(stream);
    expect(fgets(output, sizeof(output), stream) != NULL, "record read back");
    expect(strcmp(output, expected) == 0, "record is stable NDJSON");
    fclose(stream);
}

int main(void)
{
    test_display_frame();
    test_wifi_ssid_codec();
    test_ui();
    test_terminal_screen();
    test_terminal_pty();
    test_power_policy();
    test_stop_signal();
    test_ndjson();

    if (failures != 0) {
        fprintf(stderr, "%d test(s) failed\n", failures);
        return EXIT_FAILURE;
    }

    puts("all host tests passed");
    return EXIT_SUCCESS;
}