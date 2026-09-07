#include "ui/render.h"

#include "display/frame.h"
#include "ui/canvas.h"
#include "ui/wallpaper.h"
#include "pkg/text.h" /* Reuse the bundled 16px UTF-8 bitmap font for SSIDs. */

#include <stdio.h>
#include <string.h>

#define C1_UI_KEYBOARD_COLUMNS 9U

static const char *keyboard_layer_label(c1_ui_keyboard_layer layer);

static void render_status_bar(uint8_t *frame, const c1_ui_status *status)
{
    char left[32];
    char right[24];

    snprintf(left, sizeof(left), "C1 / NETWORK");
    if (status->time_available && status->battery_available) {
        snprintf(right,
                 sizeof(right),
                 "%02u:%02u %u%%",
                 status->hour,
                 status->minute,
                 status->battery_percent);
    } else {
        snprintf(right, sizeof(right), "--:-- --%%");
    }

    c1_canvas_fill_rect(frame, 0U, 0U, C1_DISPLAY_WIDTH, 11U, true);
    c1_canvas_text(frame, 3U, 3U, left, 1U, false);
    c1_canvas_text(frame,
                   C1_DISPLAY_WIDTH - c1_canvas_text_width(right, 1U) - 3U,
                   3U,
                   right,
                   1U,
                   false);
}

static void render_title(uint8_t *frame, const char *title)
{
    c1_canvas_text(frame, 8U, 18U, title, 2U, true);
    c1_canvas_fill_rect(frame, 8U, 31U, C1_DISPLAY_WIDTH - 16U, 1U, true);
}

typedef struct {
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
} c1_desktop_cell;

static c1_desktop_cell desktop_cell(uint32_t column, uint32_t row)
{
    static const uint32_t x_positions[] = {0U, 100U, 199U};
    static const uint32_t widths[] = {98U, 97U, 97U};
    static const uint32_t y_positions[] = {0U, 52U, 103U};
    static const uint32_t heights[] = {50U, 49U, 49U};
    c1_desktop_cell cell = {
        x_positions[column],
        y_positions[row],
        widths[column],
        heights[row]
    };

    return cell;
}

static void render_desktop_cell_label(uint8_t *frame,
                                      uint32_t column,
                                      uint32_t row,
                                      const char *text,
                                      uint32_t x_scale,
                                      uint32_t y_scale,
                                      bool black)
{
    c1_desktop_cell cell = desktop_cell(column, row);
    uint32_t text_width = c1_canvas_text_width(text, x_scale) - x_scale;
    uint32_t text_height = 5U * y_scale;
    uint32_t x = cell.x + (cell.width - text_width) / 2U;
    uint32_t y = cell.y + (cell.height - text_height) / 2U;

    c1_canvas_text_scaled(frame, x, y, text, x_scale, y_scale, black);
}

static void render_desktop_cell_value(uint8_t *frame,
                                      uint32_t column,
                                      uint32_t row,
                                      const char *text)
{
    c1_desktop_cell cell = desktop_cell(column, row);
    uint32_t width_units = (uint32_t)strlen(text) * 4U - 1U;
    uint32_t x_scale = (cell.width - 2U) / width_units;
    uint32_t y_scale = (cell.height - 8U) / 5U;

    if (x_scale == 0U) {
        x_scale = 1U;
    }
    if (y_scale == 0U) {
        y_scale = 1U;
    }
    render_desktop_cell_label(frame, column, row, text, x_scale, y_scale, false);
}

static void render_desktop_update_cell(uint8_t *frame,
                                        const c1_ui_status *status)
{
    c1_desktop_cell cell = desktop_cell(1U, 1U);

    if (status->update_available) {
        render_desktop_cell_label(frame, 1U, 1U, "UPDATE", 4U, 8U, false);
        return;
    }
    c1_canvas_fill_rect(frame, cell.x + 46U, cell.y + 20U, 4U, 1U, false);
    c1_canvas_fill_rect(frame, cell.x + 44U, cell.y + 21U, 8U, 2U, false);
    c1_canvas_fill_rect(frame, cell.x + 43U, cell.y + 23U, 10U, 3U, false);
    c1_canvas_fill_rect(frame, cell.x + 44U, cell.y + 26U, 8U, 2U, false);
    c1_canvas_fill_rect(frame, cell.x + 46U, cell.y + 28U, 4U, 1U, false);
}

static bool split_ipv4_address(const char *address,
                               char *left,
                               size_t left_capacity,
                               char *right,
                               size_t right_capacity)
{
    const char *first;
    const char *second;
    const char *third;
    size_t left_length;
    size_t right_length;

    if (address == NULL || left == NULL || right == NULL) {
        return false;
    }
    first = strchr(address, '.');
    second = first != NULL ? strchr(first + 1, '.') : NULL;
    third = second != NULL ? strchr(second + 1, '.') : NULL;
    if (first == NULL || second == NULL || third == NULL || first == address ||
        second == first + 1 || third == second + 1 || third[1] == '\0' ||
        strchr(third + 1, '.') != NULL) {
        return false;
    }
    left_length = (size_t)(second - address);
    right_length = strlen(second + 1);
    if (left_length >= left_capacity || right_length >= right_capacity) {
        return false;
    }
    memcpy(left, address, left_length);
    left[left_length] = '\0';
    memcpy(right, second + 1, right_length + 1U);
    return true;
}

static void render_desktop(uint8_t *frame, const c1_ui_status *status)
{
    char ipv4_left[8];
    char ipv4_right[8];
    char battery[12];
    char time[12];

    c1_canvas_fill_rect(frame, 98U, 0U, 2U, C1_DISPLAY_HEIGHT, true);
    c1_canvas_fill_rect(frame, 197U, 0U, 2U, C1_DISPLAY_HEIGHT, true);
    c1_canvas_fill_rect(frame, 0U, 50U, C1_DISPLAY_WIDTH, 2U, true);
    c1_canvas_fill_rect(frame, 0U, 101U, C1_DISPLAY_WIDTH, 2U, true);

    c1_canvas_fill_rect(frame, 0U, 0U, 98U, 50U, true);
    c1_canvas_fill_rect(frame, 199U, 0U, 97U, 50U, true);
    c1_canvas_fill_rect(frame, 0U, 103U, 98U, 49U, true);
    c1_canvas_fill_rect(frame, 199U, 103U, 97U, 49U, true);
    c1_canvas_fill_rect(frame, 100U, 52U, 97U, 49U, true);
    render_desktop_cell_label(frame, 1U, 0U, "WI-FI", 4U, 8U, true);
    render_desktop_cell_label(frame, 0U, 1U, "APP", 7U, 8U, true);
    render_desktop_update_cell(frame, status);
    render_desktop_cell_label(frame, 2U, 1U, "TERMINAL", 3U, 8U, true);
    render_desktop_cell_label(frame, 1U, 2U, "DEVICE", 4U, 8U, true);

    if (status->battery_available) {
        snprintf(battery, sizeof(battery), "%u%%", status->battery_percent);
    } else {
        snprintf(battery, sizeof(battery), "--%%");
    }
    if (status->time_available) {
        snprintf(time, sizeof(time), "%02u:%02u", status->hour, status->minute);
    } else {
        snprintf(time, sizeof(time), "--:--");
    }
    if (status->wifi_connected &&
        split_ipv4_address(status->wifi_ipv4,
                           ipv4_left,
                           sizeof(ipv4_left),
                           ipv4_right,
                           sizeof(ipv4_right))) {
        render_desktop_cell_value(frame, 0U, 0U, ipv4_left);
        render_desktop_cell_value(frame, 2U, 0U, ipv4_right);
    }
    render_desktop_cell_value(frame, 0U, 2U, battery);
    render_desktop_cell_value(frame, 2U, 2U, time);
}

static unsigned int wifi_signal_bars(int signal_dbm)
{
    if (signal_dbm >= -55) {
        return 4U;
    }
    if (signal_dbm >= -67) {
        return 3U;
    }
    if (signal_dbm >= -75) {
        return 2U;
    }
    return 1U;
}

static void render_signal_icon(uint8_t *frame, uint32_t x, uint32_t y, int signal_dbm, bool black)
{
    unsigned int bars = wifi_signal_bars(signal_dbm);
    unsigned int index;

    for (index = 0U; index < bars; ++index) {
        uint32_t height = 3U + index * 3U;
        c1_canvas_fill_rect(frame, x + index * 4U, y + 12U - height, 3U, height, black);
    }
}

/* SSIDs are UTF-8 byte strings, not ASCII labels. Clip by glyph width so a
 * long Chinese name cannot disappear or overlap the security column. */
static void render_wifi_name(uint8_t *frame, int x, int y, const char *name,
                              int width, bool black)
{
    const char *label = c1pkg_text_width(name) > 0 ? name : "(unreadable name)";
    /* Retain the bundled font's full license in standalone launcher binaries,
     * including releases that distribute executables without sidecar files. */
    (void)c1pkg_font_license();
    if (c1pkg_text_width(label) > width) {
        c1pkg_text(frame, x, y, label, width - 24, black);
        c1pkg_text(frame, x + width - 24, y, "...", 24, black);
    } else {
        c1pkg_text(frame, x, y, label, width, black);
    }
}

static void render_wifi_network_row(uint8_t *frame,
                                    uint32_t y,
                                    const c1_ui_network *network,
                                    bool selected,
                                    bool connected)
{
    bool ink = !selected;
    const char *right_label = network->security != C1_WIFI_SECURITY_OPEN &&
                              network->security != C1_WIFI_SECURITY_WPA_PSK ? "N/A" :
                              connected ? "CURRENT" : network->saved ? "SAVED" :
                              (network->secured ? "LOCK" : "OPEN");
    uint32_t right_x;

    if (selected) {
        c1_canvas_fill_rect(frame, 8U, y, 280U, 21U, true);
    }
    render_signal_icon(frame, 14U, y + 4U, network->signal_dbm, ink);
    render_wifi_name(frame, 36, (int)y + 2, network->ssid, 208, ink);
    right_x = C1_DISPLAY_WIDTH - c1_canvas_text_width(right_label, 1U) - 13U;
    c1_canvas_text(frame, right_x, y + 9U, right_label, 1U, ink);
}

static void render_wifi_action_button(uint8_t *frame,
                                      uint32_t x,
                                      const char *label,
                                      bool selected)
{
    uint32_t text_width = c1_canvas_text_width(label, 1U);

    if (selected) {
        c1_canvas_fill_rect(frame, x, 37U, 138U, 17U, true);
        c1_canvas_text(frame, x + (138U - text_width) / 2U, 43U, label, 1U, false);
    } else {
        c1_canvas_text(frame, x + (138U - text_width) / 2U, 43U, label, 1U, true);
    }
}

static const char *wifi_progress_label(c1_wifi_phase phase)
{
    switch (phase) {
    case C1_WIFI_PHASE_PREPARING: return "PREPARING WI-FI";
    case C1_WIFI_PHASE_AUTHENTICATING: return "CHECKING PASSWORD";
    case C1_WIFI_PHASE_ACQUIRING_ADDRESS: return "GETTING ADDRESS";
    case C1_WIFI_PHASE_SAVING: return "SAVING CONNECTION";
    case C1_WIFI_PHASE_RESTORING: return "RESTORING NETWORK";
    default: return "CONNECTING...";
    }
}

static void render_wifi(uint8_t *frame, const c1_ui_state *state, const c1_ui_status *status)
{
    size_t selected_network = state->selection >= 2U ? state->selection - 2U : 0U;
    size_t first;
    size_t row;
    char heading[24];
    char range[40];
    char footer[48];

    if (status->network_count > 0U && selected_network >= status->network_count) {
        selected_network = status->network_count - 1U;
    }
    first = (selected_network / 3U) * 3U;
    render_title(frame, "WI-FI");
    if (status->wifi_stop_pending) {
        snprintf(heading, sizeof(heading), "STOP REQUESTED");
    } else if (status->wifi_busy) {
        snprintf(heading, sizeof(heading), "%s",
                 status->wifi_activity == C1_UI_ACTION_WIFI_CONNECT ? "CONNECTING" :
                 status->wifi_activity == C1_UI_ACTION_WIFI_DISABLE ? "TURNING OFF" : "SCANNING");
    } else if (status->wifi_connected) {
        snprintf(heading, sizeof(heading), "CONNECTED");
    } else {
        snprintf(heading, sizeof(heading), "%s", status->wifi_enabled ? "NOT CONNECTED" : "OFF");
    }
    c1_canvas_text(frame,
                   C1_DISPLAY_WIDTH - c1_canvas_text_width(heading, 1U) - 8U,
                   22U,
                   heading,
                   1U,
                   true);
    render_wifi_action_button(frame,
                              8U,
                              status->wifi_busy ? "WORKING..." : "SCAN / REFRESH",
                              state->selection == 0U);
    render_wifi_action_button(frame, 150U, "TURN WI-FI OFF", state->selection == 1U);

    if (status->wifi_busy) {
        const char *loading = status->wifi_stop_pending ? "STOP REQUESTED" :
            status->wifi_activity == C1_UI_ACTION_WIFI_CONNECT ? wifi_progress_label(status->wifi_phase) :
            status->wifi_activity == C1_UI_ACTION_WIFI_DISABLE ? "TURNING WI-FI OFF" : "FINDING NETWORKS";
        char detail[35];
        snprintf(detail, sizeof(detail), "%.34s",
                 status->wifi_activity == C1_UI_ACTION_WIFI_CONNECT ? state->selected_ssid : "PLEASE WAIT");
        c1_canvas_text(frame, 12U, 76U, loading, 2U, true);
        render_wifi_name(frame, 12, 96, detail, 272, true);
        c1_canvas_fill_rect(frame, 8U, 126U, 280U, 1U, true);
        c1_canvas_text(frame, 8U, 133U,
                       status->wifi_stop_pending ? "FINISHING SAFELY, THEN SWITCHING OFF" :
                       "RIGHT + OK: REQUEST WI-FI OFF", 1U, true);
        c1_canvas_text(frame, 8U, 145U, "BACK: LEAVE PAGE; TASK CONTINUES", 1U, true);
        return;
    }
    if (status->network_count == 0U) {
        c1_canvas_text(frame, 12U, 77U,
                       status->wifi_enabled ? "NO NETWORKS FOUND" : "WI-FI IS OFF", 2U, true);
        c1_canvas_text(frame, 12U, 99U, "SELECT SCAN TO FIND A NETWORK", 1U, true);
    }
    for (row = 0U; row < 3U && first + row < status->network_count; ++row) {
        size_t network_index = first + row;
        uint32_t y = 58U + (uint32_t)row * 22U;

        render_wifi_network_row(frame,
                                y,
                                &status->networks[network_index],
                                state->selection == network_index + 2U,
                                c1_ui_network_is_current(status, network_index));
    }

    if (state->wifi_notice[0] != '\0') {
        snprintf(footer, sizeof(footer), "%.46s", state->wifi_notice);
    } else if (status->wifi_message[0] != '\0') {
        snprintf(footer, sizeof(footer), "%.46s", status->wifi_message);
    } else if (status->wifi_connected) {
        snprintf(footer, sizeof(footer), "CONNECTED / %s", status->wifi_ipv4);
    } else {
        snprintf(footer, sizeof(footer), "SELECT A NETWORK TO CONNECT");
    }
    c1_canvas_fill_rect(frame, 8U, 126U, 280U, 1U, true);
    c1_canvas_text(frame, 8U, 133U, footer, 1U, true);
    c1_canvas_text(frame, 8U, 145U, "ARROWS: SELECT  OK: OPEN  BACK: HOME", 1U, true);
    if (status->network_count > 0U) {
        snprintf(range,
                 sizeof(range),
                 "%u-%u/%u",
                 (unsigned int)(first + 1U),
                 (unsigned int)(first + row),
                 (unsigned int)status->network_count);
        c1_canvas_text(frame,
                       C1_DISPLAY_WIDTH - c1_canvas_text_width(range, 1U) - 8U,
                       145U,
                       range,
                       1U,
                       true);
    }
}

static void render_terminal_symbols(uint8_t *frame, const c1_ui_state *state)
{
    unsigned int index;

    c1_canvas_fill_rect(frame, 8U, 96U, 280U, 48U, false);
    c1_canvas_stroke_rect(frame, 8U, 96U, 280U, 48U, 1U, true);
    c1_canvas_text(frame, 14U, 101U, "EXTRA SYMBOLS  ARROWS MOVE  ENTER INSERT", 1U, true);
    for (index = 0U; index < C1_UI_EXTENDED_SYMBOL_COUNT; ++index) {
        unsigned int column = index % 6U;
        unsigned int row = index / 6U;
        unsigned int x = 16U + column * 45U;
        unsigned int y = 112U + row * 14U;
        char label[2] = {c1_ui_extended_symbol(index), '\0'};
        bool selected = state->symbol_selection == index;

        c1_canvas_fill_rect(frame, x, y, 35U, 12U, selected);
        c1_canvas_stroke_rect(frame, x, y, 35U, 12U, 1U, true);
        c1_canvas_text(frame, x + 15U, y + 4U, label, 1U, !selected);
    }
}

static void render_terminal(uint8_t *frame,
                            const c1_ui_state *state,
                            c1_terminal_screen *terminal)
{
    const c1_terminal_cell *cells;
    unsigned int row;
    unsigned int column;

    c1_display_frame_clear(frame, false);
    if (terminal == NULL) {
        c1_canvas_text(frame, 56U, 70U, "STARTING ROOT TERMINAL", 2U, true);
        return;
    }
    cells = c1_terminal_screen_cells(terminal);
    if (cells == NULL) {
        c1_canvas_text(frame, 72U, 70U, "TERMINAL UNAVAILABLE", 2U, true);
        return;
    }
    for (row = 0U; row < C1_TERMINAL_ROWS; ++row) {
        for (column = 0U; column < C1_TERMINAL_COLUMNS; ++column) {
            const c1_terminal_cell *cell =
                &cells[row * C1_TERMINAL_COLUMNS + column];

            c1_canvas_terminal_cell(frame,
                                    column * 6U,
                                    row * 8U,
                                    cell->codepoint,
                                    cell->inverse,
                                    cell->underline,
                                    cell->bold);
        }
    }
    if (state->terminal_symbol_picker) {
        render_terminal_symbols(frame, state);
    }
}

static const char *keyboard_layer_label(c1_ui_keyboard_layer layer)
{
    switch (layer) {
    case C1_UI_KEYBOARD_LOWER:
        return "abc";
    case C1_UI_KEYBOARD_UPPER:
        return "ABC";
    case C1_UI_KEYBOARD_SYMBOLS:
        return "123#+";
    }
    return "?";
}

static void render_extended_symbols(uint8_t *frame, const c1_ui_state *state)
{
    uint32_t index;

    for (index = 0U; index < C1_UI_EXTENDED_SYMBOL_COUNT; ++index) {
        uint32_t column = index % 6U;
        uint32_t row = index / 6U;
        uint32_t x = 8U + column * 46U;
        uint32_t y = 96U + row * 21U;
        bool selected = state->symbol_selection == index;
        char label[2] = {c1_ui_extended_symbol(index), '\0'};
        uint32_t text_width = c1_canvas_text_width(label, 1U);

        if (selected) {
            c1_canvas_fill_rect(frame, x, y, 42U, 19U, true);
            c1_canvas_text(frame, x + (42U - text_width) / 2U, y + 6U, label, 1U, false);
        } else {
            c1_canvas_text(frame, x + (42U - text_width) / 2U, y + 6U, label, 1U, true);
        }
    }
}

static void render_input_controls(uint8_t *frame, const c1_ui_state *state)
{
    const char *layer = keyboard_layer_label(state->keyboard_layer);
    char hint[64];
    snprintf(hint, sizeof(hint), "%s  SHIFT: CHANGE MODE   TAB: %s", layer,
             state->secret_visible ? "HIDE" : "SHOW");
    c1_canvas_text(frame, 8U, 87U, hint, 1U, true);
    if (state->keyboard_layer == C1_UI_KEYBOARD_SYMBOLS) {
        render_extended_symbols(frame, state);
        c1_canvas_text(frame, 8U, 145U, "OK: SYMBOL  ENTER: CONNECT  BACK: CANCEL", 1U, true);
        return;
    }
    c1_canvas_fill_rect(frame, 8U, 103U, 280U, 23U, true);
    c1_canvas_text(frame, (C1_DISPLAY_WIDTH - c1_canvas_text_width("CONNECT", 2U)) / 2U,
                   110U, "CONNECT", 2U, false);
    c1_canvas_text(frame, 8U, 134U, "TYPE PASSWORD; DELETE TO CORRECT", 1U, true);
    c1_canvas_text(frame, 8U, 145U, "OK / ENTER: CONNECT   BACK: CANCEL", 1U, true);
}

static void render_password(uint8_t *frame, const c1_ui_state *state)
{
    char visible_secret[35];
    char range[32];
    size_t visible_length = state->secret_length < sizeof(visible_secret) - 1U
                                ? state->secret_length
                                : sizeof(visible_secret) - 1U;
    size_t first_visible = state->secret_length - visible_length;

    if (state->secret_visible) {
        memcpy(visible_secret, state->secret + first_visible, visible_length);
    } else {
        memset(visible_secret, '*', visible_length);
    }
    visible_secret[visible_length] = '\0';
    render_title(frame, "PASSWORD");
    const char *visibility = state->secret_visible ? "VISIBLE" : "HIDDEN";
    c1_canvas_text(frame, C1_DISPLAY_WIDTH - c1_canvas_text_width(visibility, 1U) - 8U,
                   22U, visibility, 1U, true);
    c1_canvas_text(frame, 8U, 39U, "NETWORK", 1U, true);
    render_wifi_name(frame, 48, 33, state->selected_ssid, 240, true);
    c1_canvas_stroke_rect(frame, 8U, 51U, 280U, 21U, 1U, true);
    c1_canvas_text(frame, 12U, 56U, visible_secret, 2U, true);
    if (first_visible > 0U) {
        snprintf(range,
                 sizeof(range),
                 "SHOWING %u-%u  LENGTH %u",
                 (unsigned int)(first_visible + 1U),
                 (unsigned int)state->secret_length,
                 (unsigned int)state->secret_length);
    } else {
        snprintf(range, sizeof(range), "LENGTH %u", (unsigned int)state->secret_length);
    }
    c1_canvas_text(frame, 8U, 75U,
                   state->wifi_notice[0] != '\0' ? state->wifi_notice : range, 1U, true);
    render_input_controls(frame, state);
}

void c1_ui_render(uint8_t *frame,
                  const c1_ui_state *state,
                  const c1_ui_status *status,
                  c1_terminal_screen *terminal)
{
    if (frame == NULL || state == NULL || status == NULL) {
        return;
    }

    c1_display_frame_clear(frame, false);
    if (state->page == C1_UI_PAGE_LOCK) {
        memcpy(frame, c1_wallpaper_frame, C1_DISPLAY_FRAME_BYTES);
        return;
    }
    if (state->page == C1_UI_PAGE_TERMINAL) {
        render_terminal(frame, state, terminal);
        return;
    }
    if (state->page == C1_UI_PAGE_DESKTOP) {
        render_desktop(frame, status);
        return;
    }
    render_status_bar(frame, status);
    switch (state->page) {
    case C1_UI_PAGE_DESKTOP:
        break;
    case C1_UI_PAGE_WIFI:
        render_wifi(frame, state, status);
        break;
    case C1_UI_PAGE_TERMINAL:
        render_terminal(frame, state, terminal);
        break;
    case C1_UI_PAGE_WIFI_PASSWORD:
        render_password(frame, state);
        break;
    case C1_UI_PAGE_LOCK:
        break;
    }
}