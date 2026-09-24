#ifndef C1_UI_MODEL_H
#define C1_UI_MODEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "services/wifi.h"
#include "services/battery.h"
#include "ui/preferences.h"

#define C1_UI_MAX_NETWORKS 8U
#define C1_UI_SSID_CAPACITY 33U
#define C1_UI_SECRET_CAPACITY 64U
#define C1_UI_IP_CAPACITY 16U
#define C1_UI_MESSAGE_CAPACITY 64U
#define C1_UI_EXTENDED_SYMBOL_COUNT 12U

typedef enum {
    C1_UI_PAGE_DESKTOP = 0,
    C1_UI_PAGE_WIFI = 1,
    C1_UI_PAGE_TERMINAL = 3,
    C1_UI_PAGE_WIFI_PASSWORD = 4,
    C1_UI_PAGE_LOCK = 5,
    C1_UI_PAGE_BATTERY = 6,
    C1_UI_PAGE_SETTINGS = 7,
    C1_UI_PAGE_LOCK_TEXT = 8,
    C1_UI_PAGE_ABOUT = 9
} c1_ui_page;

typedef enum {
    C1_UI_EVENT_NONE = 0,
    C1_UI_EVENT_UP = 1,
    C1_UI_EVENT_DOWN = 2,
    C1_UI_EVENT_LEFT = 3,
    C1_UI_EVENT_RIGHT = 4,
    C1_UI_EVENT_ENTER = 5,
    C1_UI_EVENT_SUBMIT = 6,
    C1_UI_EVENT_BACK = 7,
    C1_UI_EVENT_HOME = 8,
    C1_UI_EVENT_TOGGLE_SECRET = 9,
    C1_UI_EVENT_BATTERY = 10,
    C1_UI_EVENT_SETTINGS = 11,
    C1_UI_EVENT_SELECT_NEXT = 12, /* Physical expression key (Linux KEY_TAB). */
    C1_UI_EVENT_VIEW_PREVIOUS = 13, /* Battery: previous scale, volume down. */
    C1_UI_EVENT_VIEW_NEXT = 14 /* Battery: next scale, volume up. */
} c1_ui_event;

typedef enum {
    C1_UI_ACTION_NONE = 0,
    C1_UI_ACTION_WIFI_SCAN = 1,
    C1_UI_ACTION_WIFI_CONNECT = 2,
    C1_UI_ACTION_WIFI_DISABLE = 3,
    C1_UI_ACTION_TERMINAL_APP = 7,
    C1_UI_ACTION_TERMINAL_UPDATE = 8,
    C1_UI_ACTION_UPDATE_REFRESH = 9
} c1_ui_action;

typedef enum {
    C1_UI_KEYBOARD_LOWER = 0,
    C1_UI_KEYBOARD_UPPER = 1,
    C1_UI_KEYBOARD_SYMBOLS = 2
} c1_ui_keyboard_layer;

typedef struct {
    char ssid[C1_UI_SSID_CAPACITY];
    int signal_dbm;
    bool secured;
    c1_wifi_security security;
    bool saved;
} c1_ui_network;

typedef enum {
    C1_BATTERY_VIEW_DAY = 0,
    C1_BATTERY_VIEW_HOUR = 1,
    C1_BATTERY_VIEW_MINUTE = 2
} c1_battery_view;

typedef struct {
    c1_ui_page page;
    uint32_t selection;
    c1_battery_view battery_view;
    int64_t battery_selected_at; /* Zero follows newest; otherwise a real sample timestamp. */
    uint32_t symbol_selection;
    c1_ui_keyboard_layer keyboard_layer;
    bool terminal_symbol_picker;
    bool secret_visible;
    uint32_t desktop_selection; /* Retained while visiting another page. */
    c1_ui_action terminal_action; /* Title of the currently hosted application. */
    c1_ui_page return_page;
    uint32_t return_selection;
    bool frozen_lock;
    c1_preferences preferences;
    /* Editable copy; only ENTER on LOCK_TEXT commits it to preferences. */
    char lock_text_draft[C1_LOCK_TEXT_BYTES];
    size_t lock_text_cursor; /* Byte offset at a complete UTF-8 code point boundary. */
    char wifi_notice[C1_UI_MESSAGE_CAPACITY];
    char selected_ssid[C1_UI_SSID_CAPACITY];
    c1_wifi_security selected_security;
    bool selected_saved;
    char secret[C1_UI_SECRET_CAPACITY];
    size_t secret_length;
} c1_ui_state;

typedef struct {
    bool battery_available;
    uint32_t battery_percent;
    bool external_power_known;
    bool external_power;
    c1_battery_history battery_history;
    int64_t battery_history_now;
    bool wifi_connected;
    bool wifi_busy;
    bool wifi_enabled;
    bool service_busy;
    c1_ui_action wifi_activity;
    c1_wifi_phase wifi_phase;
    bool wifi_stop_pending;
    c1_ui_network networks[C1_UI_MAX_NETWORKS];
    size_t network_count;
    char wifi_connected_ssid[C1_UI_SSID_CAPACITY];
    char wifi_ipv4[C1_UI_IP_CAPACITY];
    char wifi_message[C1_UI_MESSAGE_CAPACITY];
    bool time_available;
    uint32_t hour;
    uint32_t minute;
    uint32_t year, month, day, weekday;
    bool update_available;
    bool update_prepared;
    unsigned new_applications;
    char daily_quote[193];
    bool time_sync_running;
    bool time_sync_ok;
} c1_ui_status;

typedef struct {
    c1_ui_state state;
    c1_ui_action action;
} c1_ui_transition;

/* Oldest-to-newest sample index; zero on empty. Pinned samples survive appends. */
size_t c1_ui_battery_selected(const c1_ui_state *state, const c1_ui_status *status);
c1_ui_state c1_ui_initial_state(void);
c1_ui_state c1_ui_reduce(c1_ui_state state, c1_ui_event event);
c1_ui_transition c1_ui_autoconnect(c1_ui_state state, const c1_ui_status *status);
c1_ui_transition c1_ui_step(c1_ui_state state, c1_ui_event event, const c1_ui_status *status);
char c1_ui_extended_symbol(uint32_t selection);
c1_ui_keyboard_layer c1_ui_keyboard_next_layer(c1_ui_keyboard_layer layer);
char c1_ui_physical_character(c1_ui_keyboard_layer layer, char letter, bool shift_chord);
bool c1_ui_is_password_page(c1_ui_page page);
bool c1_ui_enter_lock(c1_ui_state *state);
bool c1_ui_unlock(c1_ui_state *state);
bool c1_ui_secret_append(c1_ui_state *state, char character);
bool c1_ui_secret_delete(c1_ui_state *state);
/* Insert complete printable UTF-8 at the cursor atomically (legacy append name). */
bool c1_ui_lock_text_append_utf8(c1_ui_state *state, const char *text);
bool c1_ui_lock_text_append_ascii(c1_ui_state *state, char character);
bool c1_ui_lock_text_delete(c1_ui_state *state);
bool c1_ui_lock_text_move(c1_ui_state *state, int direction);
/* Clamp stale offsets to the draft and a complete code point boundary. */
size_t c1_ui_lock_text_cursor_offset(const c1_ui_state *state);
/* SSID-only status cannot distinguish same-name networks with different
 * security. Only mark an unambiguous scanned row as already connected. */
bool c1_ui_network_is_current(const c1_ui_status *status, size_t index);
void c1_ui_clear_secret(c1_ui_state *state);

#endif