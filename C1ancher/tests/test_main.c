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
#include "ui/focus.h"
#include "ui/model.h"
#include "ui/render.h"
#include "c1_ime_client.h"
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

    expect(state.page == C1_UI_PAGE_DESKTOP && state.selection == 0U,
           "desktop starts with the first of five entries selected");
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
    state.page = C1_UI_PAGE_SETTINGS; state.selection = C1_SETTING_UPDATE;
    transition = c1_ui_step(state, C1_UI_EVENT_ENTER, &status);
    expect(transition.state.page == C1_UI_PAGE_SETTINGS &&
               transition.action == C1_UI_ACTION_UPDATE_REFRESH,
           "online update setting checks in the background");
    status.wifi_connected = false;
    transition = c1_ui_step(state, C1_UI_EVENT_ENTER, &status);
    expect(transition.state.wifi_notice[0] && transition.action == C1_UI_ACTION_NONE,
           "offline update setting shows a no-network notice");
    transition = c1_ui_step(state, C1_UI_EVENT_ENTER, NULL);
    expect(transition.state.wifi_notice[0] && transition.action == C1_UI_ACTION_NONE,
           "missing status cannot start a network update");
    status.update_available = status.update_prepared = true;
    transition = c1_ui_step(state, C1_UI_EVENT_ENTER, &status);
    expect(transition.state.page == C1_UI_PAGE_TERMINAL &&
               transition.action == C1_UI_ACTION_TERMINAL_UPDATE,
           "prepared update can be explicitly confirmed offline");
    status.update_available = status.update_prepared = false;
    status.wifi_connected = true;
    status.wifi_ipv4[0] = '\0';
    transition = c1_ui_step(state, C1_UI_EVENT_ENTER, &status);
    expect(transition.action == C1_UI_ACTION_NONE, "update needs a live network address");
    snprintf(status.wifi_ipv4, sizeof(status.wifi_ipv4), "172.16.21.119");
    status.service_busy = true;
    transition = c1_ui_step(state, C1_UI_EVENT_ENTER, &status);
    expect(transition.action == C1_UI_ACTION_NONE, "busy settings cannot duplicate update checks");
    status.service_busy = false;
    state = c1_ui_initial_state();
    transition = c1_ui_step(state, C1_UI_EVENT_RIGHT, &status);
    expect(transition.state.page == C1_UI_PAGE_DESKTOP && transition.state.selection == 1U &&
               transition.action == C1_UI_ACTION_NONE, "desktop arrows select without launching");
    transition = c1_ui_step(state, C1_UI_EVENT_SELECT_NEXT, &status);
    expect(transition.state.selection == 0U, "the expression key has no desktop action");
    transition = c1_ui_step(transition.state, C1_UI_EVENT_DOWN, &status);
    transition = c1_ui_step(transition.state, C1_UI_EVENT_ENTER, &status);
    expect(transition.state.page == C1_UI_PAGE_TERMINAL && transition.action == C1_UI_ACTION_NONE,
           "confirm opens the selected terminal");
    state = c1_ui_reduce(transition.state, C1_UI_EVENT_HOME);
    expect(state.page == C1_UI_PAGE_DESKTOP && state.selection == 1U,
           "home restores the previous desktop selection");

    state = c1_ui_initial_state();
    transition = c1_ui_step(state, C1_UI_EVENT_LEFT, &status);
    expect(transition.state.page == C1_UI_PAGE_DESKTOP && transition.state.selection == 4U &&
               transition.action == C1_UI_ACTION_NONE,
           "left wraps to the last desktop entry without launching");
    status.wifi_connected = false;
    transition = c1_ui_step(state, C1_UI_EVENT_ENTER, &status);
    expect(transition.action == C1_UI_ACTION_TERMINAL_APP, "apps can be opened offline");
    transition = c1_ui_step(transition.state, C1_UI_EVENT_NONE, &status);
    expect(transition.action == C1_UI_ACTION_NONE, "apps only launches once");

    status.wifi_enabled = true;
    state = c1_ui_initial_state();
    state.selection = 2U;
    transition = c1_ui_step(state, C1_UI_EVENT_ENTER, &status);
    expect(transition.state.page == C1_UI_PAGE_WIFI && transition.action == C1_UI_ACTION_WIFI_SCAN,
           "confirming Wi-Fi starts a real scan action");
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
    c1pkg_text(page, 12, 64, "********", 272, 1);
    expect(frame_region_equal(desktop, page, 12U, 64U, 64U, 16U),
           "password is masked when visibility is disabled");
    transition = c1_ui_step(transition.state, C1_UI_EVENT_TOGGLE_SECRET, &status);
    c1_display_frame_clear(page, false);
    c1pkg_text(page, 12, 64, transition.state.secret, 272, 1);
    c1_ui_render(desktop, &transition.state, &status, NULL);
    expect(frame_region_equal(desktop, page, 12U, 64U, 64U, 16U),
           "Wi-Fi password input uses the shared readable bitmap font");

    status.wifi_connected = true;
    state = c1_ui_initial_state();
    c1_ui_render(desktop, &state, &status, NULL);
    expect(frame_pixel(desktop, 0U, 17U) && frame_pixel(desktop, 295U, 17U),
           "unified desktop reserves a one-pixel top-bar separator");
    const char *labels[] = {"应用", "终端", "Wi-Fi", "电池", "设置"};
    for (unsigned i = 0; i < 5; ++i) {
        unsigned x = 14U, y = 25U + i * 21U;
        c1_display_frame_clear(page, false);
        if (i == 0) c1_canvas_fill_rect(page, 6, 23, 284, 20, true);
        c1pkg_text(page, (int)x, (int)y, labels[i], 104, i != 0);
        expect(frame_region_equal(desktop, page, x, y, 104U, 16U),
               "all five entries render readable names without directional labels");
    }
    state = c1_ui_step(state, C1_UI_EVENT_SELECT_NEXT, &status).state;
    expect(state.selection == 0U, "expression key is ignored on the desktop");
    state = c1_ui_step(state, C1_UI_EVENT_DOWN, &status).state;
    c1_ui_render(desktop, &state, &status, NULL);
    expect(frame_pixel(desktop, 11U, 44U) && frame_pixel(desktop, 6U, 44U) &&
           frame_region_white_bounds(desktop, 6U, 44U, 284U, 20U,
                                     &ink_x, &ink_y, &ink_width, &ink_height),
           "selected row is rectangular with readable reversed text");
    state = c1_ui_initial_state();
    c1_ui_render(desktop, &state, &status, NULL);
    status.update_available = true;
    c1_ui_render(page, &state, &status, NULL);
    expect(!frame_region_equal(desktop, page, 130U, 107U, 150U, 20U),
           "prepared update is identified beside Settings");
    expect(frame_region_equal(desktop, page, 0U, 136U, 296U, 16U),
           "navigation hints stay stable while update status changes");
    status.update_available = status.update_prepared = false;

    c1_ui_render(connected_status, &state, &status, NULL);
    status.wifi_connected = false;
    status.wifi_connected_ssid[0] = '\0';
    status.wifi_ipv4[0] = '\0';
    c1_ui_render(generic_status, &state, &status, NULL);
    expect(frame_region_equal(connected_status, generic_status, 0U, 0U, 296U, 18U),
           "network state never adds a dash or Wi-Fi glyph to the top bar");
    c1_display_frame_clear(page, false);
    c1pkg_text(page, 232, 67, "无网络", 48, 1);
    expect(frame_region_equal(generic_status, page, 232U, 67U, 48U, 16U),
           "offline status is spelled out in the Wi-Fi desktop row");
    state = c1_ui_initial_state();
    status.daily_quote[0] = '\0';
    c1_ui_render(generic_status, &state, &status, NULL);
    c1_display_frame_clear(page, false);
    c1pkg_text(page, 6, 136, "Live free or die.", 284, 1);
    expect(frame_region_equal(generic_status, page, 0U, 136U, 296U, 16U),
           "offline desktop banner uses the Live free or die fallback");

    state = c1_ui_initial_state();
    strcpy(status.daily_quote, "中文原句");
    state.preferences.language = C1_LANGUAGE_ZH;
    c1_ui_render(page, &state, &status, NULL);
    state.preferences.language = C1_LANGUAGE_EN;
    c1_ui_render(desktop, &state, &status, NULL);
    expect(frame_region_equal(page, desktop, 0U, 134U, 296U, 18U),
           "desktop footer keeps one original quote in both UI languages");
    status.daily_quote[0] = '\0';

    state.page = C1_UI_PAGE_BATTERY;
    state.selection = 0U;
    status.battery_history.count = 3U;
    status.battery_history.samples[0] = (c1_battery_sample){1704067200LL, 72U, C1_BATTERY_DISCHARGING, false};
    status.battery_history.samples[1] = (c1_battery_sample){1704067260LL, 70U, C1_BATTERY_DISCHARGING, true};
    status.battery_history.samples[2] = (c1_battery_sample){1704067380LL, 68U, C1_BATTERY_PLUGGED, false};
    status.battery_history_now = 1704067380LL;
    for (unsigned language = C1_LANGUAGE_ZH; language <= C1_LANGUAGE_EN; ++language) {
        state.preferences.language = (c1_language)language;
        c1_ui_render(generic_status, &state, &status, NULL);
        expect(frame_region_bounds(generic_status, 30U, 56U, 250U, 58U,
                                   &ink_x, &ink_y, &ink_width, &ink_height),
               "battery page renders a bounded history graph in both languages");
    }
    state = c1_ui_initial_state();

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
    expect(!frame_region_equal(connected_status, generic_status, 12U, 64U, 8U, 16U),
           "connected network row has a visible connection marker");
    expect(!frame_region_equal(connected_status, generic_status, 0U, 21U, 296U, 16U),
           "Wi-Fi page heading identifies connection state");

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
    expect(frame_region_equal(page, connected_status, 8U, 58U, 280U, 66U),
           "background Wi-Fi activity preserves navigable network rows");
    expect(!frame_region_equal(page, connected_status, 0U, 136U, 296U, 16U),
           "background progress appears in the status area");
    status.wifi_busy = false;
    state = c1_ui_initial_state();
    transition = c1_ui_step(state, C1_UI_EVENT_DOWN, &status);
    expect(transition.state.page == C1_UI_PAGE_DESKTOP && transition.state.selection == 1U &&
               transition.action == C1_UI_ACTION_NONE,
           "desktop down selects the terminal without opening it");
    transition = c1_ui_step(transition.state, C1_UI_EVENT_ENTER, &status);
    expect(transition.state.page == C1_UI_PAGE_TERMINAL && transition.action == C1_UI_ACTION_NONE,
           "confirmation opens a clean shell without injecting a command");
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
    c1pkg_text(expected, 26, 64, "书房网络", 234, 1);
    expect(frame_region_equal(frame, expected, 26U, 64U, 234U, 16U),
           "Chinese SSIDs render complete bitmap glyphs instead of blank bytes");

    snprintf(status.networks[0].ssid, sizeof(status.networks[0].ssid), "Short");
    c1_ui_render(short_name, &state, &status, NULL);
    snprintf(status.networks[0].ssid, sizeof(status.networks[0].ssid), "01234567890123456789012345678901");
    c1_ui_render(frame, &state, &status, NULL);
    expect(frame_region_equal(frame, short_name, 267U, 62U, 21U, 21U) &&
           frame_region_equal(frame, short_name, 12U, 64U, 8U, 16U),
           "long SSIDs cannot overwrite signal or security indicators");
    c1_display_frame_clear(expected, false);
    c1pkg_text(expected, 244, 64, "..", 16, 1);
    expect(frame_region_equal(frame, expected, 244U, 64U, 16U, 16U),
           "long SSIDs have a visible pixel-budget ellipsis");

    state.selection = 5U;
    c1_ui_render(frame, &state, &status, NULL);
    c1_display_frame_clear(expected, false);
    c1_canvas_fill_rect(expected, 8U, 62U, 280U, 21U, true);
    c1pkg_text(expected, 26, 64, "Network-3", 234, 0);
    expect(frame_region_equal(frame, expected, 26U, 64U, 234U, 16U),
           "fourth network starts the next page with white text on black selection");
    expect(frame_region_equal(frame, expected, 8U, 84U, 280U, 43U),
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
    state.selection = 2U;
    next = c1_ui_step(state, C1_UI_EVENT_ENTER, &status);
    expect(next.state.page == C1_UI_PAGE_WIFI && next.action == C1_UI_ACTION_NONE &&
               next.state.wifi_notice[0] != '\0', "entering Wi-Fi during an update explains why no scan starts");
    state = next.state;
    state.selection = 1U;
    next = c1_ui_step(state, C1_UI_EVENT_ENTER, &status);
    expect(next.action == C1_UI_ACTION_WIFI_DISABLE, "turn off can be requested while worker is busy");
    status.wifi_busy = true;
    status.network_count = 1U;
    state.selection = 0U;
    next = c1_ui_step(state, C1_UI_EVENT_DOWN, &status);
    expect(next.state.selection == 2U, "busy view still permits browsing retained network rows");
    next = c1_ui_step(next.state, C1_UI_EVENT_ENTER, &status);
    expect(next.action == C1_UI_ACTION_NONE && next.state.wifi_notice[0],
           "browsing during work cannot start a second network transaction");
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

    expect(c1_terminal_screen_resize(&terminal, 37U, 8U) == C1_STATUS_OK,
           "terminal geometry fills the area below the shared header");
    c1_terminal_screen_reset(&terminal);
    c1_terminal_screen_feed(&terminal, "A中文", strlen("A中文"));
    state.page = C1_UI_PAGE_TERMINAL;
    c1_ui_render(frame, &state, &status, &terminal);
    uint8_t expected[C1_DISPLAY_FRAME_BYTES] = {0};
    c1pkg_text(expected, 0, 18, "A中文", 40, 1);
    expect(frame_region_equal(frame, expected, 0U, 18U, 40U, 16U),
           "terminal starts immediately below header with full-width Chinese glyphs");
    expect(frame_pixel(frame, 0U, 17U) && !frame_pixel(frame, 295U, 151U),
           "terminal has one top bar and no window border");
    const char *last_cell = "\033[?25l\033[8;37HZ";
    c1_terminal_screen_feed(&terminal, last_cell, strlen(last_cell));
    c1_ui_render(frame, &state, &status, &terminal);
    c1_display_frame_clear(expected, false);
    c1pkg_text(expected, 288, 130, "Z", 8, 1);
    expect(frame_region_equal(frame, expected, 288U, 130U, 8U, 16U),
           "last full-screen row and column are usable rather than clipped");
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
    expect(c1_power_policy_timeout(&policy, now) == -1 &&
               c1_power_policy_tick(&policy, now + C1_POWER_IDLE_TIMEOUT_MS) == C1_POWER_ACTION_NONE,
           "unknown external power leaves automatic idle locking disabled");
    c1_power_policy_set_external_power(&policy, true, false, now);
    expect(c1_power_policy_timeout(&policy, now) == C1_POWER_IDLE_TIMEOUT_MS,
           "known battery power schedules the configured idle deadline");

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
               C1_POWER_ACTION_NONE &&
               c1_power_policy_timeout(&gated_policy, now + 6000) == C1_POWER_IDLE_TIMEOUT_MS,
           "locked unplug starts a full new idle interval rather than only the power grace period");
    expect(c1_power_policy_tick(&gated_policy,
                                now + 6000 + C1_POWER_IDLE_TIMEOUT_MS) ==
               C1_POWER_ACTION_SUSPEND,
           "locked battery policy permits suspend after the complete unplug idle interval");
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
    expect(c1_ui_unlock(&state) && state.page == C1_UI_PAGE_WIFI,
           "unlock returns to the network list without restoring a cleared password");

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

static void test_input_overlay_bounds(void)
{
    uint8_t frame[C1_DISPLAY_FRAME_BYTES], before[C1_DISPLAY_FRAME_BYTES];
    c1_ui_state state = c1_ui_initial_state();
    c1_ui_status status = {0};
    struct c1_ime_response view = {0};
    strcpy(view.preedit, "nihao"); strcpy(view.candidates[0], "你好"); view.candidate_count = 1;
    state.page = C1_UI_PAGE_TERMINAL;
    c1_ui_render(frame, &state, &status, NULL); memcpy(before, frame, sizeof(frame));
    c1_ui_render_input(frame, &state, &view, true, false);
    bool above_unchanged = true, bottom_changed = false;
    for (unsigned y = 0; y < 152; ++y) for (unsigned x = 0; x < 296; ++x) {
        unsigned index = (y / 8U) * 296U + x;
        bool changed = ((frame[index] ^ before[index]) & (0x80U >> (y % 8U))) != 0;
        if (y < 119 && changed) above_unchanged = false;
        if (y >= 119 && changed) bottom_changed = true;
    }
    expect(above_unchanged && bottom_changed, "IME overlay only writes its reserved 33px region");
    state.page = C1_UI_PAGE_WIFI_PASSWORD;
    c1_ui_render(frame, &state, &status, NULL); memcpy(before, frame, sizeof(frame));
    c1_ui_render_input(frame, &state, &view, true, true);
    expect(memcmp(frame, before, sizeof(frame)) == 0, "IME overlay cannot paint over a password page");
}

static void test_focus_frame(void)
{
    uint8_t frame[C1_DISPLAY_FRAME_BYTES] = {0}, before[C1_DISPLAY_FRAME_BYTES];
    c1_ui_focus_frame(frame, 296, 152, 10, 10, 9, 9, true);
    expect(frame_pixel(frame, 10, 10) && frame_pixel(frame, 18, 18) &&
           !frame_pixel(frame, 14, 10) && !frame_pixel(frame, 10, 14) && !frame_pixel(frame, 14, 14),
           "focus uses four open corners, never an arrow or a closed square");
    memcpy(before, frame, sizeof(frame));
    c1_ui_focus_frame(frame, 296, 152, UINT32_MAX, UINT32_MAX, 9, 9, true);
    c1_ui_focus_frame(frame, 296, 152, 290, 148, 9, 9, true);
    c1_ui_focus_frame(frame, 296, 152, 1, 1, UINT32_MAX, UINT32_MAX, true);
    expect(!memcmp(frame, before, sizeof(frame)), "invalid focus dimensions cannot wrap or escape the bitmap");
    c1_ui_focus_frame(frame, 296, 152, 10, 10, 9, 9, false);
    memset(before, 0, sizeof(before));
    expect(!memcmp(frame, before, sizeof(frame)), "white focus uses the same geometry on an inverted row");
}

static void test_selected_rows_are_rectangular(void)
{
    uint8_t frame[C1_DISPLAY_FRAME_BYTES];
    c1_ui_state state = c1_ui_initial_state();
    c1_ui_status status = {0};
    state.page = C1_UI_PAGE_DESKTOP;
    state.selection = 0;
    c1_ui_render(frame, &state, &status, NULL);
    expect(frame_pixel(frame, 6, 23) && frame_pixel(frame, 289, 23) &&
           frame_pixel(frame, 6, 42) && frame_pixel(frame, 289, 42),
           "selected desktop row retains all four black corners");
    state.page = C1_UI_PAGE_WIFI;
    state.selection = 0;
    c1_ui_render(frame, &state, &status, NULL);
    expect(frame_pixel(frame, 8, 40) && frame_pixel(frame, 143, 40) &&
           frame_pixel(frame, 8, 57) && frame_pixel(frame, 143, 57),
           "selected Wi-Fi action retains all four black corners");
    status.network_count = 1;
    strcpy(status.networks[0].ssid, "test");
    state.selection = 2;
    c1_ui_render(frame, &state, &status, NULL);
    expect(frame_pixel(frame, 8, 62) && frame_pixel(frame, 287, 62) &&
           frame_pixel(frame, 8, 82) && frame_pixel(frame, 287, 82),
           "selected Wi-Fi network retains all four black corners");
    state.page = C1_UI_PAGE_SETTINGS;
    state.selection = 0;
    c1_ui_render(frame, &state, &status, NULL);
    expect(frame_pixel(frame, 8, 43) && frame_pixel(frame, 287, 43) &&
           frame_pixel(frame, 8, 63) && frame_pixel(frame, 287, 63),
           "selected settings row retains all four black corners");
}

static void test_battery_ui(void)
{
    struct { uint8_t before[16], frame[C1_DISPLAY_FRAME_BYTES], after[16]; } guarded;
    uint8_t expected[C1_DISPLAY_FRAME_BYTES], gap[C1_DISPLAY_FRAME_BYTES];
    c1_ui_state state = c1_ui_initial_state();
    c1_ui_status status = {0};
    state.page = C1_UI_PAGE_BATTERY;
    state.battery_view = C1_BATTERY_VIEW_MINUTE;
    status.time_available = true;
    status.battery_available = true; status.battery_percent = 72;
    status.battery_history.count = 2;
    status.battery_history_now = C1_BATTERY_MIN_TIME + 60;
    status.battery_history.samples[0] = (c1_battery_sample){C1_BATTERY_MIN_TIME, 72, C1_BATTERY_DISCHARGING, false};
    status.battery_history.samples[1] = (c1_battery_sample){C1_BATTERY_MIN_TIME + 60, 72, C1_BATTERY_PLUGGED, false};
    memset(&guarded, 0xa5, sizeof(guarded));
    c1_ui_render(guarded.frame, &state, &status, NULL);
    memcpy(gap, guarded.frame, sizeof(gap));
    c1_display_frame_clear(expected, false);
    c1pkg_text(expected, 6, 136, "01/01 08:01 72% 插电", 284, true);
    expect(frame_region_equal(gap, expected, 0, 136, 296, 16), "battery detail replaces the bottom legend with actual local sample time");
    status.battery_history.samples[1].connected = true;
    c1_ui_render(guarded.frame, &state, &status, NULL);
    /* Samples at x276 and x280, level72 at y64. Brackets are at y60/68;
     * x278,y64 distinguishes a connected segment from two isolated points. */
    expect(!frame_pixel(gap, 278, 64) && frame_pixel(guarded.frame, 278, 64),
           "a gap is never connected while continuously observed adjacent minutes are");
    c1_preferences before = state.preferences;
    state = c1_ui_step(state, C1_UI_EVENT_LEFT, &status).state;
    expect(state.selection == 1 && state.battery_selected_at == C1_BATTERY_MIN_TIME &&
           state.battery_view == C1_BATTERY_VIEW_MINUTE, "left pins the older real sample without changing scale");
    state = c1_ui_step(state, C1_UI_EVENT_LEFT, &status).state;
    expect(state.selection == 1, "older selection clamps at first observation");
    c1_ui_transition next = c1_ui_step(state, C1_UI_EVENT_ENTER, &status);
    expect(next.state.battery_view == C1_BATTERY_VIEW_MINUTE && next.state.battery_selected_at == state.battery_selected_at &&
           next.action == C1_UI_ACTION_NONE && !memcmp(&before, &next.state.preferences, sizeof(before)),
           "Enter does not switch the battery view or change selection, settings or services");
    next = c1_ui_step(next.state, C1_UI_EVENT_SELECT_NEXT, &status);
    expect(next.state.battery_view == C1_BATTERY_VIEW_MINUTE, "expression key does not switch the battery view");
    static const c1_ui_event views[] = {C1_UI_EVENT_VIEW_PREVIOUS, C1_UI_EVENT_VIEW_PREVIOUS,
        C1_UI_EVENT_VIEW_PREVIOUS, C1_UI_EVENT_VIEW_NEXT, C1_UI_EVENT_VIEW_NEXT, C1_UI_EVENT_VIEW_NEXT};
    static const c1_battery_view expected_views[] = {C1_BATTERY_VIEW_HOUR, C1_BATTERY_VIEW_DAY,
        C1_BATTERY_VIEW_DAY, C1_BATTERY_VIEW_HOUR, C1_BATTERY_VIEW_MINUTE, C1_BATTERY_VIEW_MINUTE};
    for (size_t i = 0; i < sizeof(views) / sizeof(*views); ++i) {
        next = c1_ui_step(next.state, views[i], &status);
        expect(next.state.battery_view == expected_views[i] &&
               next.state.battery_selected_at == state.battery_selected_at && next.state.selection == state.selection &&
               next.action == C1_UI_ACTION_NONE && !memcmp(&before, &next.state.preferences, sizeof(before)),
               "volume switches day/hour/minute with clamped ends, without moving the sample or changing settings");
        next = c1_ui_step(next.state, C1_UI_EVENT_ENTER, &status);
        expect(next.state.battery_view == expected_views[i] && next.state.battery_selected_at == state.battery_selected_at &&
               next.action == C1_UI_ACTION_NONE, "Enter leaves every scale and selected sample unchanged");
    }
    status.battery_history.samples[2] = (c1_battery_sample){C1_BATTERY_MIN_TIME + 120, 70, C1_BATTERY_DISCHARGING, true};
    status.battery_history.count = 3;
    expect(c1_ui_battery_selected(&state, &status) == 0, "background appends do not move a pinned timestamp");
    state = c1_ui_step(state, C1_UI_EVENT_RIGHT, &status).state;
    expect(state.selection == 1, "right selects the next newer minute");
    state = c1_ui_step(state, C1_UI_EVENT_RIGHT, &status).state;
    expect(state.selection == 0 && !state.battery_selected_at, "right at newest resumes live following");
    state = c1_ui_step(state, C1_UI_EVENT_RIGHT, &status).state;
    expect(state.selection == 0 && !state.battery_selected_at && state.battery_view == C1_BATTERY_VIEW_MINUTE,
           "right clamps at the newest sample without changing scale");
    status.battery_available = false;
    c1_ui_render(guarded.frame, &state, &status, NULL);
    c1_display_frame_clear(expected, false);
    c1pkg_text(expected, 6, 136, "01/01 08:02 70% 电池", 284, true);
    expect(frame_region_equal(expected, guarded.frame, 0, 136, 296, 16),
           "sensor failure does not erase real history or invent a current point");
    /* A full day's bounds, zoom/pan, selected endpoints and both languages. */
    status.battery_history.count = C1_BATTERY_HISTORY_POINTS;
    for (size_t i = 0; i < C1_BATTERY_HISTORY_POINTS; ++i)
        status.battery_history.samples[i] = (c1_battery_sample){C1_BATTERY_MIN_TIME + (int64_t)i * 60,
            (uint8_t)(i % 101U), C1_BATTERY_DISCHARGING, i != 0 && i != 50};
    status.battery_history_now = status.battery_history.samples[C1_BATTERY_HISTORY_POINTS - 1U].timestamp;
    for (unsigned language = 0; language <= C1_LANGUAGE_EN; ++language) {
        state.preferences.language = (c1_language)language;
        for (unsigned view = 0; view <= C1_BATTERY_VIEW_MINUTE; ++view) {
            state.battery_view = (c1_battery_view)view;
            for (unsigned endpoint = 0; endpoint < 2; ++endpoint) {
                state.battery_selected_at = endpoint ? C1_BATTERY_MIN_TIME : 0;
                c1_ui_render(guarded.frame, &state, &status, NULL);
                for (unsigned i = 0; i < 16; ++i)
                    expect(guarded.before[i] == 0xa5 && guarded.after[i] == 0xa5,
                           "full minute history and focus brackets preserve frame canaries");
            }
        }
    }
    status.battery_history.count = 0;
    for (unsigned language = 0; language <= C1_LANGUAGE_EN; ++language) {
        state.preferences.language = (c1_language)language;
        c1_ui_render(guarded.frame, &state, &status, NULL);
        c1_display_frame_clear(expected, false);
        c1pkg_text(expected, 178, 22, language ? "Vol +/-: view" : "音量选视图", 110, true);
        expect(frame_region_equal(expected, guarded.frame, 178, 22, 110, 16),
               "battery toolbar advertises volume view control in both languages");
        c1pkg_text(expected, 6, 136, language ? "Left/Right: sample  Vol +/-: view" : "左右选采样点  音量选日/时/分", 284, true);
        expect(frame_region_equal(expected, guarded.frame, 0, 136, 296, 16),
               "empty history footer advertises arrows for samples and volume for scale");
    }
    static const c1_ui_event empty_events[] = {C1_UI_EVENT_LEFT, C1_UI_EVENT_RIGHT, C1_UI_EVENT_ENTER};
    for (size_t i = 0; i < sizeof(empty_events) / sizeof(*empty_events); ++i) {
        next = c1_ui_step(state, empty_events[i], &status);
        expect(next.state.selection == 0 && !next.state.battery_selected_at && next.state.battery_view == state.battery_view &&
               next.action == C1_UI_ACTION_NONE, "empty history arrows and Enter are safe and never change scale");
    }
    expect(c1_ui_step(state, C1_UI_EVENT_VIEW_PREVIOUS, &status).state.battery_view == C1_BATTERY_VIEW_HOUR,
           "volume still changes views with empty history");
    state.page = C1_UI_PAGE_DESKTOP; state.selection = 1; state.preferences.terminal_enabled = false;
    expect(c1_ui_step(state, C1_UI_EVENT_ENTER, &status).state.page == C1_UI_PAGE_TERMINAL,
           "obsolete terminal switch cannot strand a user after removing the process page");
}


int main(void)
{
    test_display_frame();
    test_wifi_ssid_codec();
    test_wifi_interactions();
    test_wifi_saved_interactions();
    test_wifi_typography();
    test_ui();
    test_focus_frame();
    test_selected_rows_are_rectangular();
    test_battery_ui();
    test_input_overlay_bounds();
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