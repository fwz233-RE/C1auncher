#include "core/power_policy.h"
#include "core/record.h"
#include "core/status.h"
#include "display/frame.h"
#include "hal/linux/led.h"
#include "platform/stop.h"
#include "platform/update_health.h"
#include "services/terminal.h"
#include "services/wifi.h"
#include "ui/canvas.h"
#include "ui/model.h"
#include "ui/render.h"
#include "pkg/text.h"
#include "ui/wallpaper.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
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

static bool frame_region_color_bounds(const uint8_t *frame,
                                      uint32_t x,
                                      uint32_t y,
                                      uint32_t width,
                                      uint32_t height,
                                      bool black,
                                      uint32_t *ink_x,
                                      uint32_t *ink_y,
                                      uint32_t *ink_width,
                                      uint32_t *ink_height)
{
    uint32_t min_x = x + width;
    uint32_t min_y = y + height;
    uint32_t max_x = x;
    uint32_t max_y = y;
    uint32_t row;
    uint32_t column;
    bool found = false;

    for (row = y; row < y + height; ++row) {
        for (column = x; column < x + width; ++column) {
            if (frame_pixel(frame, column, row) == black) {
                if (!found || column < min_x) min_x = column;
                if (!found || column > max_x) max_x = column;
                if (!found || row < min_y) min_y = row;
                if (!found || row > max_y) max_y = row;
                found = true;
            }
        }
    }
    if (found) {
        *ink_x = min_x;
        *ink_y = min_y;
        *ink_width = max_x - min_x + 1U;
        *ink_height = max_y - min_y + 1U;
    }
    return found;
}

static bool frame_region_bounds(const uint8_t *frame,
                                uint32_t x,
                                uint32_t y,
                                uint32_t width,
                                uint32_t height,
                                uint32_t *ink_x,
                                uint32_t *ink_y,
                                uint32_t *ink_width,
                                uint32_t *ink_height)
{
    return frame_region_color_bounds(frame, x, y, width, height, true,
                                     ink_x, ink_y, ink_width, ink_height);
}

static bool frame_region_white_bounds(const uint8_t *frame,
                                      uint32_t x,
                                      uint32_t y,
                                      uint32_t width,
                                      uint32_t height,
                                      uint32_t *ink_x,
                                      uint32_t *ink_y,
                                      uint32_t *ink_width,
                                      uint32_t *ink_height)
{
    return frame_region_color_bounds(frame, x, y, width, height, false,
                                     ink_x, ink_y, ink_width, ink_height);
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
        .wifi_ipv4 = "172.16.21.119",
        .networks = {{"TEST-NETWORK", -48, true}},
        .time_available = true,
        .hour = 23U,
        .minute = 5U
    };
    c1_ui_transition transition;
    uint8_t desktop[C1_DISPLAY_FRAME_BYTES];
    uint8_t page[C1_DISPLAY_FRAME_BYTES];
    uint8_t connected_status[C1_DISPLAY_FRAME_BYTES];
    uint8_t generic_status[C1_DISPLAY_FRAME_BYTES];
    uint32_t ink_x;
    uint32_t ink_y;
    uint32_t ink_width;
    uint32_t ink_height;

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
               transition.action == C1_UI_ACTION_UPDATE_REFRESH,
           "online center Enter checks in the background without opening a terminal");
    transition = c1_ui_step(state, C1_UI_EVENT_ENTER, NULL);
    expect(memcmp(&transition.state, &state, sizeof(state)) == 0 &&
               transition.action == C1_UI_ACTION_NONE,
           "missing network status makes center Enter a no-op");
    status.wifi_connected = false;
    transition = c1_ui_step(state, C1_UI_EVENT_ENTER, &status);
    expect(memcmp(&transition.state, &state, sizeof(state)) == 0 &&
               transition.action == C1_UI_ACTION_NONE,
           "offline center Enter preserves the desktop without any action");
    status.update_available = true;
    transition = c1_ui_step(state, C1_UI_EVENT_ENTER, &status);
    expect(transition.state.page == C1_UI_PAGE_DESKTOP &&
               transition.action == C1_UI_ACTION_NONE,
           "offline center Enter remains a no-op even with a cached prepared update");
    status.wifi_connected = true;
    status.wifi_ipv4[0] = '\0';
    transition = c1_ui_step(state, C1_UI_EVENT_ENTER, &status);
    expect(transition.state.page == C1_UI_PAGE_DESKTOP &&
               transition.action == C1_UI_ACTION_NONE,
           "stale connected status without a live IP cannot open the updater");
    snprintf(status.wifi_ipv4, sizeof(status.wifi_ipv4), "172.16.21.119");
    transition = c1_ui_step(state, C1_UI_EVENT_ENTER, &status);
    expect(transition.state.page == C1_UI_PAGE_TERMINAL &&
               transition.state.selection == 0U &&
               transition.action == C1_UI_ACTION_TERMINAL_UPDATE,
           "online center Enter opens only a prepared update for confirmation");
    status.update_available = false;
    state = c1_ui_reduce(transition.state, C1_UI_EVENT_HOME);
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
    expect(transition.state.page == C1_UI_PAGE_TERMINAL &&
               transition.action == C1_UI_ACTION_TERMINAL_APP,
           "left opens APP in the shared terminal");
    state = transition.state;
    transition = c1_ui_step(state, C1_UI_EVENT_NONE, &status);
    expect(transition.state.page == C1_UI_PAGE_TERMINAL &&
               transition.action == C1_UI_ACTION_NONE,
           "APP direction requests c1pkg only for its entry event");

    status.wifi_connected = false;
    status.wifi_enabled = true;
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
    status.networks[0].security = C1_WIFI_SECURITY_WPA_PSK;
    transition = c1_ui_step(transition.state, C1_UI_EVENT_ENTER, &status);
    expect(transition.state.page == C1_UI_PAGE_WIFI_PASSWORD &&
               transition.action == C1_UI_ACTION_NONE,
           "secured network selection opens the password input page");
    expect(transition.state.secret_visible, "password entry starts visible by default");
    transition = c1_ui_step(transition.state, C1_UI_EVENT_TOGGLE_SECRET, &status);
    expect(!transition.state.secret_visible, "Tab can hide the initially visible password");
    transition = c1_ui_step(transition.state, C1_UI_EVENT_TOGGLE_SECRET, &status);
    expect(transition.state.secret_visible, "Tab can reveal the password again");
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
    expect(transition.state.secret_length == 0U && transition.action == C1_UI_ACTION_NONE &&
               strcmp(transition.state.wifi_notice, "USE 8-63 CHARACTERS") == 0,
           "OK validates an empty password on the input page without starting work");
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
    expect(transition.action == C1_UI_ACTION_NONE && transition.state.secret_length == 2U,
           "short password is retained and rejected before creating a worker");
    for (unsigned int i = 0; i < 6U; ++i) c1_ui_secret_append(&transition.state, 'a');
    transition = c1_ui_step(transition.state, C1_UI_EVENT_SUBMIT, &status);
    expect(transition.action == C1_UI_ACTION_WIFI_CONNECT, "physical Enter requests Wi-Fi connection");
    transition.state.keyboard_layer = C1_UI_KEYBOARD_LOWER;
    transition = c1_ui_step(transition.state, C1_UI_EVENT_ENTER, &status);
    expect(transition.action == C1_UI_ACTION_WIFI_CONNECT, "center OK also submits a valid password");
    c1_ui_render(desktop, &transition.state, &status, NULL);
    c1_display_frame_clear(page, false);
    c1_canvas_text(page, 12U, 56U, "********", 2U, true);
    expect(frame_region_equal(desktop, page, 12U, 56U, 64U, 10U),
           "password is masked when visibility is disabled");
    transition = c1_ui_step(transition.state, C1_UI_EVENT_TOGGLE_SECRET, &status);
    c1_display_frame_clear(page, false);
    c1_canvas_text(page, 12U, 56U, transition.state.secret, 2U, true);
    c1_ui_render(desktop, &transition.state, &status, NULL);
    expect(frame_region_equal(desktop, page, 12U, 56U, 16U, 10U),
           "Wi-Fi password input displays entered characters at double size");

    status.wifi_connected = true;
    state = c1_ui_initial_state();
    c1_ui_render(desktop, &state, &status, NULL);
    expect(frame_region_bounds(desktop, 100U, 0U, 97U, 50U,
                               &ink_x, &ink_y, &ink_width, &ink_height) &&
               ink_x == 110U && ink_y == 5U && ink_width == 76U && ink_height == 40U,
           "desktop WI-FI label is visually centered and fills most of its direction cell");
    expect(frame_region_bounds(desktop, 0U, 52U, 98U, 49U,
                               &ink_x, &ink_y, &ink_width, &ink_height) &&
               ink_x == 10U && ink_y == 56U && ink_width == 77U && ink_height == 40U,
           "desktop APP label is visually centered and fills most of its direction cell");
    expect(frame_region_bounds(desktop, 199U, 52U, 97U, 49U,
                               &ink_x, &ink_y, &ink_width, &ink_height) &&
               ink_x == 201U && ink_y == 56U && ink_width == 93U && ink_height == 40U,
           "desktop TERMINAL label is visually centered and fills most of its direction cell");
    expect(frame_region_bounds(desktop, 100U, 103U, 97U, 49U,
                               &ink_x, &ink_y, &ink_width, &ink_height) &&
               ink_x == 102U && ink_y == 107U && ink_width == 92U && ink_height == 40U,
           "desktop DEVICE label is visually centered and fills most of its direction cell");
    expect(frame_pixel(desktop, 100U, 52U), "center desktop cell is locked black");
    expect(frame_region_white_bounds(desktop, 100U, 52U, 97U, 49U,
                                     &ink_x, &ink_y, &ink_width, &ink_height) &&
               ink_x == 143U && ink_y == 72U && ink_width == 10U && ink_height == 9U,
           "desktop center cell shows the original no-update dot");
    status.update_available = true;
    c1_ui_render(desktop, &state, &status, NULL);
    expect(frame_region_white_bounds(desktop, 100U, 52U, 97U, 49U,
                                     &ink_x, &ink_y, &ink_width, &ink_height) &&
               ink_x == 102U && ink_y == 56U && ink_width == 92U && ink_height == 40U,
           "desktop center cell expands UPDATE to fill its grid cell");
    status.update_available = false;
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

    expect(frame_region_white_bounds(desktop, 0U, 0U, 98U, 50U,
                                     &ink_x, &ink_y, &ink_width, &ink_height) &&
               ink_x == 3U && ink_y == 5U && ink_width == 92U && ink_height == 40U,
           "desktop IPv4 prefix fills the top-left status cell");
    expect(frame_region_white_bounds(desktop, 199U, 0U, 97U, 50U,
                                     &ink_x, &ink_y, &ink_width, &ink_height) &&
               ink_x == 201U && ink_y == 5U && ink_width == 92U && ink_height == 40U,
           "desktop IPv4 suffix fills the top-right status cell");
    expect(frame_region_white_bounds(desktop, 0U, 103U, 98U, 49U,
                                     &ink_x, &ink_y, &ink_width, &ink_height) &&
               ink_x == 5U && ink_y == 107U && ink_width == 88U && ink_height == 40U,
           "desktop battery value fills the bottom-left status cell");
    expect(frame_region_white_bounds(desktop, 199U, 103U, 97U, 49U,
                                     &ink_x, &ink_y, &ink_width, &ink_height) &&
               ink_x == 200U && ink_y == 107U && ink_width == 95U && ink_height == 40U,
           "desktop time fills the bottom-right status cell");

    c1_ui_render(connected_status, &state, &status, NULL);
    status.wifi_connected = false;
    status.wifi_connected_ssid[0] = '\0';
    status.wifi_ipv4[0] = '\0';
    c1_ui_render(generic_status, &state, &status, NULL);
    expect(!frame_region_equal(connected_status, generic_status, 0U, 0U, 98U, 50U) &&
               !frame_region_equal(connected_status, generic_status, 199U, 0U, 97U, 50U),
           "disconnecting clears both IPv4 halves from the desktop");
    expect(!frame_region_white_bounds(generic_status, 0U, 0U, 98U, 50U,
                                      &ink_x, &ink_y, &ink_width, &ink_height) &&
               !frame_region_white_bounds(generic_status, 199U, 0U, 97U, 50U,
                                          &ink_x, &ink_y, &ink_width, &ink_height),
           "disconnected top status cells remain completely blank");

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
    expect(!frame_region_equal(connected_status, generic_status, 8U, 133U, 180U, 7U),
           "Wi-Fi page footer identifies the connected address");

    status.wifi_connected = true;
    snprintf(status.wifi_connected_ssid,
             sizeof(status.wifi_connected_ssid),
             "%s",
             "TEST-NETWORK");
    status.wifi_busy = true;
    c1_ui_render(page, &state, &status, NULL);
    expect(frame_region_bounds(page, 12U, 76U, 260U, 12U,
                               &ink_x, &ink_y, &ink_width, &ink_height),
           "Wi-Fi scanning state renders readable text rather than a fake progress bar");
    expect(!frame_region_equal(page, connected_status, 8U, 58U, 280U, 66U),
           "busy page replaces actionable stale network rows");
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

static void test_wifi_typography(void)
{
    c1_ui_state state = c1_ui_initial_state();
    c1_ui_status status = {0};
    uint8_t frame[C1_DISPLAY_FRAME_BYTES], expected[C1_DISPLAY_FRAME_BYTES];
    uint8_t short_name[C1_DISPLAY_FRAME_BYTES];
    state.page = C1_UI_PAGE_WIFI;
    state.selection = 0U;
    status.wifi_enabled = true;
    status.network_count = 4U;
    for (size_t i = 0; i < status.network_count; ++i) {
        snprintf(status.networks[i].ssid, sizeof(status.networks[i].ssid), "Network-%u", (unsigned int)i);
        status.networks[i].security = C1_WIFI_SECURITY_WPA_PSK;
        status.networks[i].secured = true;
    }
    snprintf(status.networks[0].ssid, sizeof(status.networks[0].ssid), "书房网络");
    c1_ui_render(frame, &state, &status, NULL);
    c1_display_frame_clear(expected, false);
    c1pkg_text(expected, 36, 60, "书房网络", 208, 1);
    expect(frame_region_equal(frame, expected, 36U, 60U, 208U, 16U),
           "Chinese SSIDs render complete bitmap glyphs instead of blank bytes");

    snprintf(status.networks[0].ssid, sizeof(status.networks[0].ssid), "Short");
    c1_ui_render(short_name, &state, &status, NULL);
    snprintf(status.networks[0].ssid, sizeof(status.networks[0].ssid), "01234567890123456789012345678901");
    c1_ui_render(frame, &state, &status, NULL);
    expect(frame_region_equal(frame, short_name, 248U, 58U, 40U, 21U),
           "long SSIDs cannot overwrite the network security label");
    c1_display_frame_clear(expected, false);
    c1pkg_text(expected, 220, 60, "...", 24, 1);
    expect(frame_region_equal(frame, expected, 220U, 60U, 24U, 16U),
           "long SSIDs have a visible pixel-budget ellipsis");

    state.selection = 5U;
    c1_ui_render(frame, &state, &status, NULL);
    c1_display_frame_clear(expected, false);
    c1_canvas_fill_rect(expected, 8U, 58U, 280U, 21U, true);
    c1pkg_text(expected, 36, 60, "Network-3", 208, 0);
    expect(frame_region_equal(frame, expected, 36U, 60U, 208U, 16U),
           "fourth network starts the next page with white text on black selection");
    expect(frame_region_equal(frame, expected, 8U, 80U, 280U, 43U),
           "last page does not retain stale rows from previous page");
}

static void test_wifi_saved_interactions(void)
{
    c1_ui_state state = c1_ui_initial_state();
    state.page = C1_UI_PAGE_WIFI;
    state.selection = 2U;
    c1_ui_status status = {0};
    status.wifi_enabled = true;
    status.network_count = 3U;
    status.networks[0] = (c1_ui_network){"Guest", -40, false, C1_WIFI_SECURITY_OPEN, false};
    status.networks[1] = (c1_ui_network){"Home", -50, true, C1_WIFI_SECURITY_WPA_PSK, true};
    status.networks[2] = (c1_ui_network){"Office", -60, true, C1_WIFI_SECURITY_WPA_PSK, true};
    c1_ui_transition next = c1_ui_autoconnect(state, &status);
    expect(next.action == C1_UI_ACTION_WIFI_CONNECT && next.state.selected_saved &&
               strcmp(next.state.selected_ssid, "Home") == 0 && next.state.secret_length == 0U,
           "fresh scan automatically selects strongest saved network without exposing credentials");
    status.wifi_connected = true;
    expect(c1_ui_autoconnect(state, &status).action == C1_UI_ACTION_NONE,
           "automatic reconnect never replaces an existing live connection");
    status.wifi_connected = false;
    status.wifi_stop_pending = true;
    expect(c1_ui_autoconnect(state, &status).action == C1_UI_ACTION_NONE,
           "pending turn off takes precedence over saved auto-connect");
    status.wifi_stop_pending = false;
    status.service_busy = true;
    expect(c1_ui_autoconnect(state, &status).action == C1_UI_ACTION_NONE,
           "auto-connect cannot race an existing worker");
    status.service_busy = false;
    state.page = C1_UI_PAGE_DESKTOP;
    expect(c1_ui_autoconnect(state, &status).action == C1_UI_ACTION_NONE,
           "leaving Wi-Fi before scan completion prevents automatic connection");
    state.page = C1_UI_PAGE_WIFI;
    state.selection = 3U;
    next = c1_ui_step(state, C1_UI_EVENT_ENTER, &status);
    expect(next.action == C1_UI_ACTION_WIFI_CONNECT && next.state.selected_saved &&
               next.state.page == C1_UI_PAGE_WIFI && next.state.secret_length == 0U,
           "manual saved network selection connects directly instead of opening password page");
    status.networks[1].saved = false;
    next = c1_ui_step(state, C1_UI_EVENT_ENTER, &status);
    expect(next.action == C1_UI_ACTION_NONE && !next.state.selected_saved &&
               next.state.page == C1_UI_PAGE_WIFI_PASSWORD && next.state.secret_visible,
           "unremembered secured networks still open visible password input");
    status.networks[2].saved = false;
    expect(c1_ui_autoconnect(state, &status).action == C1_UI_ACTION_NONE,
           "no saved credentials means no automatic connection to an arbitrary open network");
}

static void test_wifi_interactions(void)
{
    c1_ui_state state = c1_ui_initial_state();
    c1_ui_status status = {0};
    c1_ui_transition next;
    status.service_busy = true;
    next = c1_ui_step(state, C1_UI_EVENT_UP, &status);
    expect(next.state.page == C1_UI_PAGE_WIFI && next.action == C1_UI_ACTION_NONE &&
               next.state.wifi_notice[0] != '\0', "entering Wi-Fi during an update explains why no scan starts");
    state = next.state;
    state.selection = 1U;
    next = c1_ui_step(state, C1_UI_EVENT_ENTER, &status);
    expect(next.action == C1_UI_ACTION_WIFI_DISABLE, "turn off can be requested while worker is busy");
    status.wifi_busy = true;
    state.selection = 0U;
    next = c1_ui_step(state, C1_UI_EVENT_DOWN, &status);
    expect(next.state.selection == 0U, "busy view cannot navigate hidden network rows");
    status.service_busy = false;
    status.wifi_busy = false;
    status.wifi_connected = true;
    status.network_count = 1U;
    snprintf(status.networks[0].ssid, sizeof(status.networks[0].ssid), "HOME");
    snprintf(status.wifi_connected_ssid, sizeof(status.wifi_connected_ssid), "HOME");
    status.networks[0].secured = true;
    status.networks[0].security = C1_WIFI_SECURITY_WPA_PSK;
    state.selection = 2U;
    next = c1_ui_step(state, C1_UI_EVENT_ENTER, &status);
    expect(next.state.page == C1_UI_PAGE_WIFI && next.action == C1_UI_ACTION_NONE &&
               strstr(next.state.wifi_notice, "ALREADY CONNECTED") != NULL,
           "selecting current network does not ask for its password again");
    status.network_count = 2U;
    status.networks[1] = status.networks[0];
    status.networks[1].security = C1_WIFI_SECURITY_OPEN;
    status.networks[1].secured = false;
    expect(!c1_ui_network_is_current(&status, 0U) && !c1_ui_network_is_current(&status, 1U),
           "same-name networks with different security are not both marked current");
    state.selection = 3U;
    next = c1_ui_step(state, C1_UI_EVENT_ENTER, &status);
    expect(next.action == C1_UI_ACTION_WIFI_CONNECT &&
               next.state.selected_security == C1_WIFI_SECURITY_OPEN,
           "SSID-only connected status cannot swallow selection of another security type");
    status.network_count = 1U;
    state.selection = 2U;
    status.wifi_connected = false;
    status.networks[0].security = C1_WIFI_SECURITY_ENTERPRISE;
    next = c1_ui_step(state, C1_UI_EVENT_ENTER, &status);
    expect(next.action == C1_UI_ACTION_NONE && next.state.page == C1_UI_PAGE_WIFI &&
               strstr(next.state.wifi_notice, "NOT SUPPORTED") != NULL,
           "enterprise security is explained rather than treated as a PSK password");
    status.networks[0].security = C1_WIFI_SECURITY_WPA_PSK;
    next = c1_ui_step(state, C1_UI_EVENT_ENTER, &status);
    expect(next.state.selected_security == C1_WIFI_SECURITY_WPA_PSK,
           "selected authentication type is retained for the connection worker");
    state.page = C1_UI_PAGE_WIFI_PASSWORD;
    snprintf(state.secret, sizeof(state.secret), "password123");
    state.secret_length = strlen(state.secret);
    state.secret_visible = true;
    status.service_busy = true;
    next = c1_ui_step(state, C1_UI_EVENT_ENTER, &status);
    expect(next.action == C1_UI_ACTION_NONE && next.state.secret_length == state.secret_length,
           "busy connection submit retains password for retry");
    next = c1_ui_step(next.state, C1_UI_EVENT_BACK, &status);
    expect(next.state.page == C1_UI_PAGE_WIFI && next.state.secret_length == 0U &&
               !next.state.secret_visible, "cancelling clears password and resets visibility");
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
    expect(c1_terminal_screen_special(&terminal, C1_TERMINAL_KEY_PAGE_UP, 0U),
           "terminal screen accepts the APP previous-list key");
    count = c1_terminal_screen_take_reply(&terminal, reply, sizeof(reply));
    expect(count == 4U && memcmp(reply, "\033[5~", 4U) == 0,
           "APP previous-list key produces Page Up for volume minus");
    expect(c1_terminal_screen_special(&terminal, C1_TERMINAL_KEY_PAGE_DOWN, 0U),
           "terminal screen accepts the APP next-list key");
    count = c1_terminal_screen_take_reply(&terminal, reply, sizeof(reply));
    expect(count == 4U && memcmp(reply, "\033[6~", 4U) == 0,
           "APP next-list key produces Page Down for volume plus");
    expect(c1_terminal_screen_special(&terminal, C1_TERMINAL_KEY_ENTER, 0U),
           "terminal screen accepts the APP Enter and OK confirmation key");
    count = c1_terminal_screen_take_reply(&terminal, reply, sizeof(reply));
    expect(count == 1U && reply[0] == '\r',
           "APP Enter and OK confirmation produces carriage return");

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
    deadline = test_milliseconds() + 2000;
    while (test_milliseconds() < deadline && !c1_terminal_shell_is_foreground(&session)) {
        struct timespec pause = {0, 10000000L};
        (void)nanosleep(&pause, NULL);
    }
    expect(c1_terminal_shell_is_foreground(&session),
           "terminal detects the interactive shell as PTY foreground owner");
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
    c1_power_policy gated_policy;
    c1_ui_state state = c1_ui_initial_state();
    int64_t now = 1000;
    int64_t non_desktop_locked_at;

    c1_power_policy_init(&policy, now);
    expect(policy.state == C1_POWER_ACTIVE,
           "power policy starts active");
    expect(c1_power_policy_timeout(&policy, now) == C1_POWER_IDLE_TIMEOUT_MS,
           "active policy schedules the five-minute idle deadline on the desktop");

    c1_power_policy_init(&gated_policy, now);
    expect(c1_power_policy_lock(&gated_policy, now),
           "external-power gate test enters lock state");
    expect(c1_power_policy_tick(&gated_policy,
                                now + C1_POWER_LOCK_TIMEOUT_MS) == C1_POWER_ACTION_NONE &&
               c1_power_policy_timeout(&gated_policy, now) == -1,
           "unknown external power fails closed without a suspend deadline");
    c1_power_policy_set_external_power(&gated_policy, true, true, now);
    expect(c1_power_policy_tick(&gated_policy,
                                now + C1_POWER_LOCK_TIMEOUT_MS) == C1_POWER_ACTION_NONE &&
               c1_power_policy_timeout(&gated_policy, now) == -1,
           "online external power blocks suspend without periodic policy wakeups");
    c1_power_policy_set_external_power(&gated_policy, true, false, now + 1000);
    expect(c1_power_policy_tick(&gated_policy,
                                now + 1000 + C1_POWER_EXTERNAL_OFFLINE_DELAY_MS - 1) ==
               C1_POWER_ACTION_NONE,
           "newly offline power waits for a stable unplug interval");
    c1_power_policy_set_external_power(&gated_policy, true, true, now + 5000);
    c1_power_policy_set_external_power(&gated_policy, true, false, now + 6000);
    expect(c1_power_policy_tick(&gated_policy,
                                now + 1000 + C1_POWER_EXTERNAL_OFFLINE_DELAY_MS) ==
               C1_POWER_ACTION_NONE,
           "an online transition cancels the previous unplug interval");
    expect(c1_power_policy_tick(&gated_policy,
                                now + 6000 + C1_POWER_EXTERNAL_OFFLINE_DELAY_MS) ==
               C1_POWER_ACTION_SUSPEND,
           "stable offline power permits suspend after twenty seconds while locked");
    c1_power_policy_suspend_cancelled(&gated_policy);
    expect(gated_policy.state == C1_POWER_LOCKED && gated_policy.suspend_retry_at < 0,
           "late power recheck cancellation returns to locked state without retry delay");
    c1_power_policy_restore_failed(&gated_policy, now + 30000);
    expect(gated_policy.suspend_disabled &&
               c1_power_policy_timeout(&gated_policy, now + 30000) == -1 &&
               c1_power_policy_filter_wakeup(&gated_policy, true),
           "restore failure disables later suspend attempts and suppresses wake-key leakage");

    c1_power_policy_set_external_power(&policy, true, false, now);
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

static bool test_file_write(const char *path, const char *value)
{
    FILE *stream = fopen(path, "w");

    if (stream == NULL) {
        return false;
    }
    if (fputs(value, stream) == EOF) {
        fclose(stream);
        return false;
    }
    return fclose(stream) == 0;
}

static bool test_file_read(const char *path, char *value, size_t value_size)
{
    FILE *stream = fopen(path, "r");
    size_t length;

    if (stream == NULL || fgets(value, (int)value_size, stream) == NULL) {
        if (stream != NULL) {
            fclose(stream);
        }
        return false;
    }
    fclose(stream);
    length = strlen(value);
    while (length > 0U && (value[length - 1U] == '\n' || value[length - 1U] == '\r')) {
        value[--length] = '\0';
    }
    return true;
}

static void test_led_chaser(void)
{
    char root[] = "/tmp/c1-led-test-XXXXXX";
    c1_linux_led_chaser chaser;
    unsigned int index;
    bool files_ready = true;
    char *created = mkdtemp(root);

    expect(created != NULL, "LED test directory is created");
    if (created == NULL) {
        return;
    }
    for (index = 0U; index < C1_LED_CHASER_COUNT; ++index) {
        char directory[320];
        char path[352];
        char brightness[16];

        (void)snprintf(directory, sizeof(directory), "%s/led%u", root, index + 2U);
        files_ready = files_ready && mkdir(directory, 0700) == 0;
        (void)snprintf(path, sizeof(path), "%s/trigger", directory);
        files_ready = files_ready && test_file_write(path, "none [timer] heartbeat\n");
        (void)snprintf(path, sizeof(path), "%s/brightness", directory);
        (void)snprintf(brightness, sizeof(brightness), "%u\n", index + 10U);
        files_ready = files_ready && test_file_write(path, brightness);
        (void)snprintf(path, sizeof(path), "%s/delay_on", directory);
        files_ready = files_ready && test_file_write(path, "500\n");
        (void)snprintf(path, sizeof(path), "%s/delay_off", directory);
        files_ready = files_ready && test_file_write(path, "500\n");
    }
    expect(files_ready, "LED test sysfs files are created");
    if (files_ready) {
        char path[352];
        char value[64];

        expect(c1_linux_led_chaser_start(&chaser, root, 1000),
               "LED feedback takes temporary control of all four lights");
        (void)snprintf(path, sizeof(path), "%s/led2/brightness", root);
        expect(test_file_read(path, value, sizeof(value)) && strcmp(value, "0") == 0,
               "LED feedback starts quiet without a periodic deadline");
        expect(c1_linux_led_chaser_timeout(&chaser, 1000) == -1,
               "quiet LED feedback does not wake the event loop");
        c1_linux_led_chaser_pulse(&chaser, 1000);
        expect(test_file_read(path, value, sizeof(value)) && strcmp(value, "255") == 0,
               "input feedback starts with the first light on");
        expect(c1_linux_led_chaser_timeout(&chaser, 1000) == 180,
               "active LED feedback exposes only its short pulse deadline");
        c1_linux_led_chaser_tick(&chaser, 1180);
        expect(test_file_read(path, value, sizeof(value)) && strcmp(value, "0") == 0,
               "LED feedback turns the previous light off");
        (void)snprintf(path, sizeof(path), "%s/led3/brightness", root);
        expect(test_file_read(path, value, sizeof(value)) && strcmp(value, "255") == 0,
               "LED feedback advances to the next light");
        c1_linux_led_chaser_tick(&chaser, 1720);
        expect(test_file_read(path, value, sizeof(value)) && strcmp(value, "0") == 0 &&
                   c1_linux_led_chaser_timeout(&chaser, 1720) == -1,
               "LED feedback turns fully off after one short cycle");
        c1_linux_led_chaser_pulse(&chaser, 2000);
        c1_linux_led_chaser_quiet(&chaser);
        expect(c1_linux_led_chaser_timeout(&chaser, 2000) == -1,
               "lock mode immediately cancels LED feedback");
        c1_linux_led_chaser_stop(&chaser);
        (void)snprintf(path, sizeof(path), "%s/led2/trigger", root);
        expect(test_file_read(path, value, sizeof(value)) && strcmp(value, "timer") == 0,
               "LED chaser restores the original trigger");
        (void)snprintf(path, sizeof(path), "%s/led2/brightness", root);
        expect(test_file_read(path, value, sizeof(value)) && strcmp(value, "10") == 0,
               "LED chaser restores the original brightness");
    }
    for (index = 0U; index < C1_LED_CHASER_COUNT; ++index) {
        char directory[320];
        char path[352];
        static const char *attributes[] = {"trigger", "brightness", "delay_on", "delay_off"};
        size_t attribute;

        (void)snprintf(directory, sizeof(directory), "%s/led%u", root, index + 2U);
        for (attribute = 0U; attribute < sizeof(attributes) / sizeof(attributes[0]); ++attribute) {
            (void)snprintf(path, sizeof(path), "%s/%s", directory, attributes[attribute]);
            (void)unlink(path);
        }
        (void)rmdir(directory);
    }
    (void)rmdir(root);
}

static void test_update_health_policy(void)
{
    struct c1_update_state state;

    memset(&state, 0, sizeof(state));
    state.phase = C1_UPDATE_PENDING_BOOT;
    expect(c1_update_health_should_probe(&state),
           "pending release requires one health probe");
    state.phase = C1_UPDATE_CONFIRMED;
    expect(!c1_update_health_should_probe(&state),
           "confirmed release skips candidate health probes");
    expect(!c1_update_health_should_probe(NULL),
           "missing update state cannot authorize a health probe");
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
    test_wifi_interactions();
    test_wifi_saved_interactions();
    test_wifi_typography();
    test_ui();
    test_terminal_screen();
    test_terminal_pty();
    test_power_policy();
    test_led_chaser();
    test_update_health_policy();
    test_stop_signal();
    test_ndjson();

    if (failures != 0) {
        fprintf(stderr, "%d test(s) failed\n", failures);
        return EXIT_FAILURE;
    }

    puts("all host tests passed");
    return EXIT_SUCCESS;
}