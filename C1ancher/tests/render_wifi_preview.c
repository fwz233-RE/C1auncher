/* Render real 296x152 UI frames with deterministic, non-sensitive fixtures. */
#include "ui/render.h"
#include "display/frame.h"
#include <stdio.h>
#include <string.h>

static int save(const char *directory, const char *name, c1_ui_state *state, c1_ui_status *status)
{
    uint8_t frame[C1_DISPLAY_FRAME_BYTES];
    char path[1024];
    if (snprintf(path, sizeof(path), "%s/%s.pbm", directory, name) >= (int)sizeof(path)) return 1;
    c1_ui_render(frame, state, status, NULL);
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
