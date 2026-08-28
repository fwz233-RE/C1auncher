#include "ui/render.h"

#include "display/frame.h"
#include "ui/canvas.h"
#include "ui/wallpaper.h"

#include <stdio.h>
#include <string.h>

#define C1_UI_KEYBOARD_COLUMNS 9U

static const char *keyboard_layer_label(c1_ui_keyboard_layer layer);

static void render_status_bar(uint8_t *frame, const c1_ui_status *status)
{
    char left[32];
    char right[24];

    snprintf(left,
             sizeof(left),
             "C1 WIFI %s",
             status->wifi_connected ? "CONNECTED" : "OFF");
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

static void render_desktop_cell_text(uint8_t *frame,
                                     uint32_t column,
                                     uint32_t row,
                                     const char *text,
                                     uint32_t scale,
                                     bool black)
{
    static const uint32_t x_positions[] = {0U, 100U, 199U};
    static const uint32_t widths[] = {98U, 97U, 97U};
    static const uint32_t y_positions[] = {0U, 52U, 103U};
    static const uint32_t heights[] = {50U, 49U, 49U};
    uint32_t text_width = c1_canvas_text_width(text, scale);
    uint32_t x = x_positions[column] + (widths[column] - text_width) / 2U;
    uint32_t y = y_positions[row] + (heights[row] - 7U * scale) / 2U + 1U;

    c1_canvas_text(frame, x, y, text, scale, black);
}

static void render_desktop_origin(uint8_t *frame)
{
    c1_canvas_fill_rect(frame, 146U, 72U, 4U, 1U, false);
    c1_canvas_fill_rect(frame, 144U, 73U, 8U, 2U, false);
    c1_canvas_fill_rect(frame, 143U, 75U, 10U, 3U, false);
    c1_canvas_fill_rect(frame, 144U, 78U, 8U, 2U, false);
    c1_canvas_fill_rect(frame, 146U, 80U, 4U, 1U, false);
}

static void render_desktop(uint8_t *frame, const c1_ui_status *status)
{
    char wifi[12];
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
    render_desktop_cell_text(frame, 1U, 0U, "WI-FI", 2U, true);
    render_desktop_cell_text(frame, 0U, 1U, "APP", 2U, true);
    render_desktop_origin(frame);
    render_desktop_cell_text(frame, 2U, 1U, "TERMINAL", 2U, true);
    render_desktop_cell_text(frame, 1U, 2U, "DEVICE", 2U, true);

    snprintf(wifi, sizeof(wifi), "%s", status->wifi_connected ? "WIFI ON" : "WIFI OFF");
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
    render_desktop_cell_text(frame, 0U, 0U, wifi, 2U, false);
    render_desktop_cell_text(frame, 0U, 2U, battery, 3U, false);
    render_desktop_cell_text(frame, 2U, 2U, time, 3U, false);
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

static void render_wifi_network_row(uint8_t *frame,
                                    uint32_t y,
                                    const c1_ui_network *network,
                                    bool selected,
                                    bool connected)
{
    bool ink = !selected;
    char ssid[49];
    const char *right_label = connected ? "CONNECTED" : (network->secured ? "LOCK" : "OPEN");
    uint32_t right_x;

    snprintf(ssid, sizeof(ssid), "%.48s", network->ssid);
    if (selected) {
        c1_canvas_fill_rect(frame, 8U, y, 280U, 19U, true);
    } else {
        c1_canvas_fill_rect(frame, 8U, y + 18U, 280U, 1U, true);
    }
    render_signal_icon(frame, 13U, y + 3U, network->signal_dbm, ink);
    c1_canvas_text(frame, 32U, y + 7U, ssid, 1U, ink);
    right_x = C1_DISPLAY_WIDTH - c1_canvas_text_width(right_label, 1U) - 13U;
    c1_canvas_text(frame, right_x, y + 7U, right_label, 1U, ink);
}

static void render_wifi_action_button(uint8_t *frame,
                                      uint32_t x,
                                      const char *label,
                                      bool selected)
{
    uint32_t text_width = c1_canvas_text_width(label, 1U);

    if (selected) {
        c1_canvas_fill_rect(frame, x, 36U, 138U, 20U, true);
        c1_canvas_text(frame, x + (138U - text_width) / 2U, 43U, label, 1U, false);
    } else {
        c1_canvas_stroke_rect(frame, x, 36U, 138U, 20U, 1U, true);
        c1_canvas_text(frame, x + (138U - text_width) / 2U, 43U, label, 1U, true);
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
    first = selected_network > 1U ? selected_network - 1U : 0U;
    render_title(frame, "WI-FI");
    if (status->wifi_busy) {
        snprintf(heading, sizeof(heading), "SCANNING...");
    } else if (status->wifi_connected) {
        snprintf(heading, sizeof(heading), "CONNECTED");
    } else {
        snprintf(heading, sizeof(heading), "%u NETWORKS", (unsigned int)status->network_count);
    }
    c1_canvas_text(frame,
                   C1_DISPLAY_WIDTH - c1_canvas_text_width(heading, 1U) - 8U,
                   22U,
                   heading,
                   1U,
                   true);
    render_wifi_action_button(frame,
                              8U,
                              status->wifi_busy ? "SCANNING..." : "SCAN WI-FI",
                              state->selection == 0U);
    render_wifi_action_button(frame, 150U, "TURN WI-FI OFF", state->selection == 1U);

    if (status->wifi_busy) {
        const char *loading = "SCANNING WI-FI";
        const char *waiting = "PLEASE WAIT";
        uint32_t loading_width = c1_canvas_text_width(loading, 2U);
        uint32_t waiting_width = c1_canvas_text_width(waiting, 1U);

        c1_canvas_text(frame, (C1_DISPLAY_WIDTH - loading_width) / 2U, 79U, loading, 2U, true);
        c1_canvas_text(frame, (C1_DISPLAY_WIDTH - waiting_width) / 2U, 104U, waiting, 1U, true);
        c1_canvas_stroke_rect(frame, 62U, 120U, 172U, 11U, 1U, true);
        c1_canvas_fill_rect(frame, 66U, 124U, 32U, 3U, true);
        c1_canvas_fill_rect(frame, 106U, 124U, 32U, 3U, true);
        c1_canvas_fill_rect(frame, 146U, 124U, 32U, 3U, true);
        c1_canvas_fill_rect(frame, 186U, 124U, 32U, 3U, true);
        return;
    }
    if (status->network_count == 0U) {
        c1_canvas_text(frame, 86U, 91U, "NO NETWORKS FOUND", 1U, true);
    }
    for (row = 0U; row < 4U && first + row < status->network_count; ++row) {
        size_t network_index = first + row;
        uint32_t y = 59U + (uint32_t)row * 21U;

        render_wifi_network_row(frame,
                                y,
                                &status->networks[network_index],
                                state->selection == network_index + 2U,
                                status->wifi_connected &&
                                    strcmp(status->wifi_connected_ssid,
                                           status->networks[network_index].ssid) == 0);
    }

    if (status->wifi_connected) {
        snprintf(footer,
                 sizeof(footer),
                 "CONNECTED TO %.28s",
                 status->wifi_connected_ssid[0] != '\0' ? status->wifi_connected_ssid : "WI-FI");
    } else if (status->wifi_message[0] != '\0') {
        snprintf(footer, sizeof(footer), "%.42s", status->wifi_message);
    } else {
        snprintf(footer, sizeof(footer), "SELECT NETWORK AND PRESS ENTER");
    }
    c1_canvas_text(frame, 8U, 145U, footer, 1U, true);
    if (status->network_count > 0U && !status->wifi_connected) {
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

    c1_canvas_text(frame, 84U, 81U, "KEYCAP: SHIFT + LETTER", 1U, true);
    c1_canvas_text(frame, 84U, 95U, "EXTRA: ARROWS + OK", 1U, true);
    for (index = 0U; index < C1_UI_EXTENDED_SYMBOL_COUNT; ++index) {
        uint32_t column = index % 6U;
        uint32_t row = index / 6U;
        uint32_t x = 8U + column * 46U;
        uint32_t y = 108U + row * 22U;
        bool selected = state->symbol_selection == index;
        char label[2] = {c1_ui_extended_symbol(index), '\0'};
        uint32_t text_width = c1_canvas_text_width(label, 1U);

        if (selected) {
            c1_canvas_fill_rect(frame, x, y, 42U, 19U, true);
            c1_canvas_text(frame, x + (42U - text_width) / 2U, y + 6U, label, 1U, false);
        } else {
            c1_canvas_stroke_rect(frame, x, y, 42U, 19U, 1U, true);
            c1_canvas_text(frame, x + (42U - text_width) / 2U, y + 6U, label, 1U, true);
        }
    }
}

static void render_input_controls(uint8_t *frame, const c1_ui_state *state)
{
    const char *layer = keyboard_layer_label(state->keyboard_layer);
    uint32_t layer_width = c1_canvas_text_width(layer, 2U);

    c1_canvas_stroke_rect(frame, 8U, 79U, 68U, 29U, 2U, true);
    c1_canvas_text(frame, 42U - layer_width / 2U, 88U, layer, 2U, true);
    if (state->keyboard_layer == C1_UI_KEYBOARD_SYMBOLS) {
        render_extended_symbols(frame, state);
        return;
    }
    c1_canvas_text(frame, 88U, 82U, "TYPE ON PHYSICAL KEYBOARD", 1U, true);
    c1_canvas_text(frame, 88U, 99U, "SHIFT  CHANGE LAYER", 1U, true);
    c1_canvas_text(frame, 18U, 130U, "DELETE  ERASE", 1U, true);
    c1_canvas_text(frame, 111U, 130U, "SPACE  BLANK", 1U, true);
    c1_canvas_text(frame, 204U, 130U, "ENTER  SUBMIT", 1U, true);
}

static void render_password(uint8_t *frame, const c1_ui_state *state)
{
    char visible_secret[35];
    char network[52];
    char range[32];
    size_t visible_length = state->secret_length < sizeof(visible_secret) - 1U
                                ? state->secret_length
                                : sizeof(visible_secret) - 1U;
    size_t first_visible = state->secret_length - visible_length;

    memcpy(visible_secret, state->secret + first_visible, visible_length);
    visible_secret[visible_length] = '\0';
    render_title(frame, "WI-FI PASSWORD");
    c1_canvas_text(frame,
                   C1_DISPLAY_WIDTH - c1_canvas_text_width("ENTER CONNECT", 1U) - 8U,
                   22U,
                   "ENTER CONNECT",
                   1U,
                   true);
    snprintf(network, sizeof(network), "NETWORK  %.42s", state->selected_ssid);
    c1_canvas_text(frame, 8U, 38U, network, 1U, true);
    c1_canvas_stroke_rect(frame, 8U, 47U, 280U, 21U, 1U, true);
    c1_canvas_text(frame, 12U, 52U, visible_secret, 2U, true);
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
    c1_canvas_text(frame, 8U, 71U, range, 1U, true);
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