#ifndef C1_UI_MODEL_H
#define C1_UI_MODEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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
    C1_UI_PAGE_LOCK = 5
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
    C1_UI_EVENT_HOME = 8
} c1_ui_event;

typedef enum {
    C1_UI_ACTION_NONE = 0,
    C1_UI_ACTION_WIFI_SCAN = 1,
    C1_UI_ACTION_WIFI_CONNECT = 2,
    C1_UI_ACTION_WIFI_DISABLE = 3,
    C1_UI_ACTION_TERMINAL_NEOFETCH = 6,
    C1_UI_ACTION_TERMINAL_APP = 7
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
} c1_ui_network;

typedef struct {
    c1_ui_page page;
    uint32_t selection;
    uint32_t symbol_selection;
    c1_ui_keyboard_layer keyboard_layer;
    bool terminal_symbol_picker;
    char selected_ssid[C1_UI_SSID_CAPACITY];
    char secret[C1_UI_SECRET_CAPACITY];
    size_t secret_length;
} c1_ui_state;

typedef struct {
    bool battery_available;
    uint32_t battery_percent;
    bool wifi_connected;
    bool wifi_busy;
    c1_ui_network networks[C1_UI_MAX_NETWORKS];
    size_t network_count;
    char wifi_connected_ssid[C1_UI_SSID_CAPACITY];
    char wifi_ipv4[C1_UI_IP_CAPACITY];
    char wifi_message[C1_UI_MESSAGE_CAPACITY];
    bool time_available;
    uint32_t hour;
    uint32_t minute;
} c1_ui_status;

typedef struct {
    c1_ui_state state;
    c1_ui_action action;
} c1_ui_transition;

c1_ui_state c1_ui_initial_state(void);
c1_ui_state c1_ui_reduce(c1_ui_state state, c1_ui_event event);
c1_ui_transition c1_ui_step(c1_ui_state state, c1_ui_event event, const c1_ui_status *status);
char c1_ui_extended_symbol(uint32_t selection);
c1_ui_keyboard_layer c1_ui_keyboard_next_layer(c1_ui_keyboard_layer layer);
char c1_ui_physical_character(c1_ui_keyboard_layer layer, char letter, bool shift_chord);
bool c1_ui_is_password_page(c1_ui_page page);
bool c1_ui_enter_lock(c1_ui_state *state);
bool c1_ui_unlock(c1_ui_state *state);
bool c1_ui_secret_append(c1_ui_state *state, char character);
bool c1_ui_secret_delete(c1_ui_state *state);
void c1_ui_clear_secret(c1_ui_state *state);

#endif