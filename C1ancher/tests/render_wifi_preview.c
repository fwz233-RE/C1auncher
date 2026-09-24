/* Render real 296x152 UI frames with deterministic, non-sensitive fixtures. */
#include "ui/render.h"
#include "ui/chrome.h"
#include "display/frame.h"
#include "c1_ime_client.h"
static bool preview_input;
static const struct c1_ime_response preview_candidates = {
    .flags = C1_IME_READY | C1_IME_CHINESE | C1_IME_COMPOSING,
    .candidate_count = 5, .preedit = "nihao", .candidates = {"你好", "拟好", "你", "尼", "呢"}
};
#include <stdio.h>
#include <string.h>

static int save(const char *directory, const char *name, c1_ui_state *state, c1_ui_status *status)
{
    uint8_t frame[C1_DISPLAY_FRAME_BYTES];
    char path[1024];
    if (snprintf(path, sizeof(path), "%s/%s.pbm", directory, name) >= (int)sizeof(path)) return 1;
    c1_terminal_screen terminal;
    if (state->page == C1_UI_PAGE_TERMINAL) {
        if (c1_terminal_screen_init(&terminal) != C1_STATUS_OK) return 1;
        unsigned columns = C1_CHROME_TERMINAL_COLUMNS;
        unsigned rows = preview_input ? C1_CHROME_TERMINAL_INPUT_ROWS : C1_CHROME_TERMINAL_ROWS;
        c1_terminal_screen_resize(&terminal, columns, rows);
        const char *sample = "root@C1:~# echo 你好世界\r\n你好世界\r\nroot@C1:~# ls /storage\r\nBook  Music  Pic\r\nroot@C1:~# ";
        c1_terminal_screen_feed(&terminal, sample, strlen(sample));
        c1_ui_render(frame, state, status, &terminal);
        c1_terminal_screen_destroy(&terminal);
    } else c1_ui_render(frame, state, status, NULL);
    if (preview_input) c1_ui_render_input(frame, state, &preview_candidates, true, false);
    FILE *output = fopen(path, "wb");
    if (!output) return 1;
    fprintf(output, "P4\n%u %u\n", C1_DISPLAY_WIDTH, C1_DISPLAY_HEIGHT);
    int failed = 0;
    for (uint32_t y = 0; y < C1_DISPLAY_HEIGHT; ++y) {
        for (uint32_t x = 0; x < C1_DISPLAY_WIDTH; x += 8U) {
            unsigned char byte = 0;
            for (uint32_t bit = 0; bit < 8U; ++bit) {
                size_t offset = (y / 8U) * C1_DISPLAY_WIDTH + x + bit;
                if (frame[offset] & (0x80U >> (y % 8U))) byte |= 0x80U >> bit;
            }
            if (fputc(byte, output) == EOF) failed = 1;
        }
    }
    return fclose(output) != 0 || failed;
}
int main(int argc, char **argv)
{
    if (argc != 2) return 2;
    c1_ui_state state = c1_ui_initial_state();
    c1_ui_status status = {0};
    state.page = C1_UI_PAGE_WIFI;
    state.selection = 3;
    status.wifi_enabled = true;
    status.wifi_connected = true;
    status.battery_available = status.time_available = true;
    status.battery_percent = 86; status.hour = 21; status.minute = 51;
    status.year = 2026; status.month = 9; status.day = 18; status.weekday = 5;
    state.page = C1_UI_PAGE_DESKTOP;
    state.selection = 0U;
    status.wifi_connected = false;
    if (save(argv[1], "desktop-zh", &state, &status)) return 1;
    state.preferences.language = C1_LANGUAGE_EN;
    if (save(argv[1], "desktop-en", &state, &status)) return 1;
    state.preferences.language = C1_LANGUAGE_ZH;
    state.page = C1_UI_PAGE_TERMINAL;
    if (save(argv[1], "terminal-fullscreen", &state, &status)) return 1;
    preview_input = true;
    if (save(argv[1], "terminal-chinese-input", &state, &status)) return 1;
    preview_input = false;
    state.page = C1_UI_PAGE_LOCK_TEXT;
    snprintf(state.lock_text_draft, sizeof(state.lock_text_draft), "愿你拥有安静的一天");
    preview_input = true;
    if (save(argv[1], "lock-text-editor", &state, &status)) return 1;
    preview_input = false;
    state.page = C1_UI_PAGE_BATTERY;
    status.battery_history.count = C1_BATTERY_HISTORY_POINTS;
    for (unsigned i = 0U; i < status.battery_history.count; ++i) {
        c1_battery_sample *point = &status.battery_history.samples[i];
        point->timestamp = 1790020800LL + (int64_t)i * 60LL;
        point->percent = (uint8_t)(i < 1000U ? 95U - i / 20U : 45U + (i - 1000U) / 10U);
        point->power = i < 1000U ? C1_BATTERY_DISCHARGING : C1_BATTERY_PLUGGED;
        point->connected = i != 0 && (i < 480U || i > 540U);
    }
    status.battery_history_now = status.battery_history.samples[status.battery_history.count - 1U].timestamp;
    state.battery_selected_at = status.battery_history.samples[1080].timestamp;
    if (save(argv[1], "battery-history", &state, &status)) return 1;
    state.preferences.language = C1_LANGUAGE_EN;
    if (save(argv[1], "battery-history-en", &state, &status)) return 1;
    state.battery_view = C1_BATTERY_VIEW_HOUR;
    if (save(argv[1], "battery-hours-en", &state, &status)) return 1;
    state.preferences.language = C1_LANGUAGE_ZH;
    if (save(argv[1], "battery-hours", &state, &status)) return 1;
    state.battery_view = C1_BATTERY_VIEW_MINUTE;
    if (save(argv[1], "battery-minutes", &state, &status)) return 1;
    state.battery_view = C1_BATTERY_VIEW_DAY;
    state.battery_selected_at = 0;
    status.battery_history.count = 1;
    if (save(argv[1], "battery-single", &state, &status)) return 1;
    status.battery_history.count = 0;
    if (save(argv[1], "battery-empty", &state, &status)) return 1;
    state.page = C1_UI_PAGE_SETTINGS; state.selection = C1_SETTING_POWER_MODE;
    if (save(argv[1], "settings-power", &state, &status)) return 1;
    for (unsigned mode = 0U; mode <= C1_POWER_MODE_PERFORMANCE; ++mode) {
        char name[64];
        state.preferences.power_mode = (c1_power_mode)mode;
        snprintf(name, sizeof(name), "settings-power-%u", mode);
        if (save(argv[1], name, &state, &status)) return 1;
    }
    state.preferences.power_mode = C1_POWER_MODE_STANDARD;
    state.selection = C1_SETTING_UPDATE;
    if (save(argv[1], "settings-update", &state, &status)) return 1;
    state.selection = C1_SETTING_POWER_MODE;
    state.preferences.language = C1_LANGUAGE_EN;
    if (save(argv[1], "settings-en", &state, &status)) return 1;
    state.preferences.language = C1_LANGUAGE_ZH;
    state.selection = C1_SETTING_LOCK_STYLE;
    if (save(argv[1], "settings-lock", &state, &status)) return 1;
    state.page = C1_UI_PAGE_LOCK; state.preferences.lock_style = C1_LOCK_TEXT;
    if (save(argv[1], "lock-text", &state, &status)) return 1;
    strcpy(state.preferences.lock_text, "千里之行始于足下坚持自己热爱的生活");
    if (save(argv[1], "lock-text-long", &state, &status)) return 1;
    strcpy(state.preferences.lock_text, "Live free or die.");
    if (save(argv[1], "lock-text-english", &state, &status)) return 1;
    for (unsigned i = 0U; i < 64U; ++i) memcpy(state.preferences.lock_text + i * 3U, "中", 3U);
    state.preferences.lock_text[192] = 0;
    if (save(argv[1], "lock-text-maximum", &state, &status)) return 1;
    state.preferences.lock_style = C1_LOCK_CALENDAR;
    if (save(argv[1], "lock-calendar", &state, &status)) return 1;
    state.preferences.lock_style = C1_LOCK_WALLPAPER;
    state.page = C1_UI_PAGE_WIFI; state.selection = 3;
    status.wifi_connected = true;
    status.network_count = 4;
    const char *names[] = {"Home Studio", "Reading Room", "Cafe Guest", "书房网络"};
    for (size_t i = 0; i < 4; ++i) {
        snprintf(status.networks[i].ssid, sizeof(status.networks[i].ssid), "%s", names[i]);
        status.networks[i].signal_dbm = -45 - (int)i * 12;
        status.networks[i].secured = i != 2;
        status.networks[i].security = i != 2 ? C1_WIFI_SECURITY_WPA_PSK : C1_WIFI_SECURITY_OPEN;
        status.networks[i].saved = i < 2;
    }
    snprintf(status.wifi_connected_ssid, sizeof(status.wifi_connected_ssid), "Home Studio");
    snprintf(status.wifi_ipv4, sizeof(status.wifi_ipv4), "192.168.1.42");
    if (save(argv[1], "wifi-list", &state, &status)) return 1;
    status.network_count = 6;
    status.networks[4] = (c1_ui_network){"Very-Long-Network-Name-0123456789", -71, true, C1_WIFI_SECURITY_WPA_PSK, false};
    status.networks[5] = (c1_ui_network){"Enterprise", -78, true, C1_WIFI_SECURITY_ENTERPRISE, false};
    state.selection = 5;
    if (save(argv[1], "wifi-more-networks", &state, &status)) return 1;
    state.selection = 0;
    status.wifi_busy = status.service_busy = true;
    status.wifi_activity = C1_UI_ACTION_WIFI_CONNECT;
    status.wifi_phase = C1_WIFI_PHASE_AUTHENTICATING;
    snprintf(state.selected_ssid, sizeof(state.selected_ssid), "Reading Room");
    if (save(argv[1], "wifi-connecting", &state, &status)) return 1;
    status.wifi_stop_pending = true;
    status.wifi_phase = C1_WIFI_PHASE_RESTORING;
    state.selection = 1;
    if (save(argv[1], "wifi-stopping", &state, &status)) return 1;
    status.wifi_stop_pending = false;
    state.selection = 0;
    status.wifi_busy = status.service_busy = status.wifi_connected = status.wifi_enabled = false;
    status.network_count = 0;
    if (save(argv[1], "wifi-off", &state, &status)) return 1;
    state.page = C1_UI_PAGE_WIFI_PASSWORD;
    state.secret_visible = true;
    snprintf(state.secret, sizeof(state.secret), "example123");
    state.secret_length = strlen(state.secret);
    if (save(argv[1], "wifi-password", &state, &status)) return 1;
    state.keyboard_layer = C1_UI_KEYBOARD_SYMBOLS;
    if (save(argv[1], "wifi-symbols", &state, &status)) return 1;
    state.keyboard_layer = C1_UI_KEYBOARD_LOWER;
    snprintf(state.wifi_notice, sizeof(state.wifi_notice), "USE 8-63 CHARACTERS");
    if (save(argv[1], "wifi-validation", &state, &status)) return 1;
    return 0;
}
