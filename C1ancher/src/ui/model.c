#include "ui/model.h"

#include <stdio.h>
#include <string.h>

#define C1_UI_EXTENDED_SYMBOL_COLUMNS 6U

static void secure_clear(void *memory, size_t size)
{
    volatile unsigned char *cursor = memory;

    while (size-- > 0U) {
        *cursor++ = 0U;
    }
}

c1_ui_state c1_ui_initial_state(void)
{
    c1_ui_state state;

    memset(&state, 0, sizeof(state));
    state.page = C1_UI_PAGE_DESKTOP;
    state.selection = 0U;
    state.battery_view = C1_BATTERY_VIEW_DAY;
    state.preferences = c1_preferences_default();
    return state;
}

void c1_ui_clear_secret(c1_ui_state *state)
{
    if (state == NULL) {
        return;
    }
    secure_clear(state->secret, sizeof(state->secret));
    state->secret_length = 0U;
    state->secret_visible = false;
}

bool c1_ui_is_password_page(c1_ui_page page)
{
    return page == C1_UI_PAGE_WIFI_PASSWORD;
}

bool c1_ui_enter_lock(c1_ui_state *state)
{
    if (state == NULL || state->page == C1_UI_PAGE_LOCK) {
        return false;
    }
    bool sensitive = c1_ui_is_password_page(state->page) || state->secret_length != 0U ||
                     state->secret_visible;
    /* Scan the entire buffer: a cleared first byte or stale length must not
     * allow a previously drawn password to remain on a frozen display. */
    for (size_t i = 0; i < sizeof(state->secret); ++i) {
        if (state->secret[i] != '\0') sensitive = true;
    }
    state->return_page = state->page == C1_UI_PAGE_WIFI_PASSWORD ? C1_UI_PAGE_WIFI : state->page;
    state->return_selection = state->page == C1_UI_PAGE_WIFI_PASSWORD ? 0U : state->selection;
    state->frozen_lock = state->preferences.lock_style == C1_LOCK_FREEZE && !sensitive;
    c1_ui_clear_secret(state);
    state->page = C1_UI_PAGE_LOCK;
    state->selection = 4U;
    state->symbol_selection = 0U;
    state->keyboard_layer = C1_UI_KEYBOARD_LOWER;
    state->terminal_symbol_picker = false;
    state->selected_ssid[0] = '\0';
    return true;
}

bool c1_ui_unlock(c1_ui_state *state)
{
    if (state == NULL || state->page != C1_UI_PAGE_LOCK) {
        return false;
    }
    state->page = state->return_page;
    state->selection = state->return_selection;
    state->frozen_lock = false;
    return true;
}

bool c1_ui_secret_append(c1_ui_state *state, char character)
{
    unsigned char value = (unsigned char)character;

    if (state == NULL || !c1_ui_is_password_page(state->page) || value < 32U || value > 126U ||
        state->secret_length + 1U >= sizeof(state->secret)) {
        return false;
    }
    state->secret[state->secret_length++] = character;
    state->secret[state->secret_length] = '\0';
    state->wifi_notice[0] = '\0';
    return true;
}

bool c1_ui_secret_delete(c1_ui_state *state)
{
    if (state == NULL || !c1_ui_is_password_page(state->page) || state->secret_length == 0U) {
        return false;
    }
    state->secret[--state->secret_length] = '\0';
    state->wifi_notice[0] = '\0';
    return true;
}

size_t c1_ui_lock_text_cursor_offset(const c1_ui_state *state)
{
    if (state == NULL) return 0U;
    size_t length = strnlen(state->lock_text_draft, sizeof(state->lock_text_draft));
    size_t cursor = state->lock_text_cursor < length ? state->lock_text_cursor : length;
    while (cursor > 0U && cursor < length &&
           ((unsigned char)state->lock_text_draft[cursor] & 0xc0U) == 0x80U) --cursor;
    return cursor;
}

bool c1_ui_lock_text_move(c1_ui_state *state, int direction)
{
    if (state == NULL || state->page != C1_UI_PAGE_LOCK_TEXT || direction == 0 ||
        !c1_preferences_valid_text(state->lock_text_draft)) return false;
    size_t cursor = c1_ui_lock_text_cursor_offset(state);
    size_t length = strlen(state->lock_text_draft);
    if (direction < 0) {
        if (cursor == 0U) return false;
        --cursor;
        while (cursor > 0U && ((unsigned char)state->lock_text_draft[cursor] & 0xc0U) == 0x80U) --cursor;
    } else {
        if (cursor == length) return false;
        ++cursor;
        while (cursor < length && ((unsigned char)state->lock_text_draft[cursor] & 0xc0U) == 0x80U) ++cursor;
    }
    state->lock_text_cursor = cursor;
    return true;
}

bool c1_ui_lock_text_append_utf8(c1_ui_state *state, const char *text)
{
    if (state == NULL || state->page != C1_UI_PAGE_LOCK_TEXT ||
        !c1_preferences_valid_text(text)) return false;
    size_t length = strnlen(state->lock_text_draft, sizeof(state->lock_text_draft));
    if (length >= sizeof(state->lock_text_draft) ||
        (length != 0U && !c1_preferences_valid_text(state->lock_text_draft))) return false;
    size_t added = strlen(text); /* The validator already checked the bound. */
    if (added >= sizeof(state->lock_text_draft) - length) return false;

    /* Build before mutating state, also allowing text to alias the draft. */
    char draft[C1_LOCK_TEXT_BYTES] = {0};
    size_t cursor = c1_ui_lock_text_cursor_offset(state);
    memcpy(draft, state->lock_text_draft, cursor);
    memcpy(draft + cursor, text, added);
    memcpy(draft + cursor + added, state->lock_text_draft + cursor, length - cursor + 1U);
    memcpy(state->lock_text_draft, draft, sizeof(draft));
    state->lock_text_cursor = cursor + added;
    state->wifi_notice[0] = '\0';
    return true;
}

bool c1_ui_lock_text_append_ascii(c1_ui_state *state, char character)
{
    unsigned char value = (unsigned char)character;
    if (value < 32U || value > 126U) return false;
    char text[] = {character, '\0'};
    return c1_ui_lock_text_append_utf8(state, text);
}

bool c1_ui_lock_text_delete(c1_ui_state *state)
{
    if (state == NULL || state->page != C1_UI_PAGE_LOCK_TEXT ||
        !c1_preferences_valid_text(state->lock_text_draft)) return false;
    size_t length = strlen(state->lock_text_draft);
    size_t cursor = c1_ui_lock_text_cursor_offset(state);
    if (cursor == 0U) return false;
    size_t start = cursor - 1U;
    while (start > 0U && ((unsigned char)state->lock_text_draft[start] & 0xc0U) == 0x80U) --start;
    memmove(state->lock_text_draft + start, state->lock_text_draft + cursor, length - cursor + 1U);
    memset(state->lock_text_draft + length - (cursor - start) + 1U, 0,
           sizeof(state->lock_text_draft) - (length - (cursor - start) + 1U));
    state->lock_text_cursor = start;
    state->wifi_notice[0] = '\0';
    return true;
}


char c1_ui_extended_symbol(uint32_t selection)
{
    static const char symbols[] = "!+=[]{}<>\\|_";

    if (selection >= C1_UI_EXTENDED_SYMBOL_COUNT) {
        return '\0';
    }
    return symbols[selection];
}

c1_ui_keyboard_layer c1_ui_keyboard_next_layer(c1_ui_keyboard_layer layer)
{
    return (c1_ui_keyboard_layer)(((unsigned int)layer + 1U) % 3U);
}

char c1_ui_physical_character(c1_ui_keyboard_layer layer, char letter, bool shift_chord)
{
    static const char letters[] = "qwertyuiopasdfghjklzxcvbnm";
    static const char keycap_symbols[] = "1234567890'@#$%&*().-/?:;,";
    const char *position = strchr(letters, letter);

    if (position == NULL) {
        return '\0';
    }
    if (shift_chord || layer == C1_UI_KEYBOARD_SYMBOLS) {
        return keycap_symbols[position - letters];
    }
    if (layer == C1_UI_KEYBOARD_UPPER) {
        return (char)(letter - 'a' + 'A');
    }
    return letter;
}

/* Every desktop row is selected first; only confirm launches it. */
static c1_ui_transition activate_desktop(c1_ui_state state, const c1_ui_status *status)
{
    c1_ui_transition next = {state, C1_UI_ACTION_NONE};
    next.state.desktop_selection = state.selection < 5U ? state.selection : 0U;
    next.state.selection = 0U;
    next.state.wifi_notice[0] = '\0';
    switch (next.state.desktop_selection) {
    case 0:
        next.state.page = C1_UI_PAGE_TERMINAL;
        next.state.terminal_action = next.action = C1_UI_ACTION_TERMINAL_APP;
        break;
    case 1:
        next.state.page = C1_UI_PAGE_TERMINAL;
        next.state.terminal_action = C1_UI_ACTION_NONE;
        break;
    case 2:
        next.state.page = C1_UI_PAGE_WIFI;
        if (status == NULL || !status->service_busy) next.action = C1_UI_ACTION_WIFI_SCAN;
        else snprintf(next.state.wifi_notice, sizeof(next.state.wifi_notice), "%s",
                      status->wifi_busy ? "WI-FI TASK ALREADY RUNNING" :
                                         "UPDATE CHECK RUNNING; TRY SCAN LATER");
        break;
    case 3:
        next.state.page = C1_UI_PAGE_BATTERY;
        next.state.battery_selected_at = 0;
        break;
    case 4: next.state.page = C1_UI_PAGE_SETTINGS; break;
    }
    return next;
}

static void move_extended_symbol(c1_ui_state *state, c1_ui_event event)
{
    uint32_t selection = state->symbol_selection;

    switch (event) {
    case C1_UI_EVENT_LEFT:
        if (selection % C1_UI_EXTENDED_SYMBOL_COLUMNS != 0U) {
            --selection;
        }
        break;
    case C1_UI_EVENT_RIGHT:
        if (selection % C1_UI_EXTENDED_SYMBOL_COLUMNS != C1_UI_EXTENDED_SYMBOL_COLUMNS - 1U &&
            selection + 1U < C1_UI_EXTENDED_SYMBOL_COUNT) {
            ++selection;
        }
        break;
    case C1_UI_EVENT_UP:
        if (selection >= C1_UI_EXTENDED_SYMBOL_COLUMNS) {
            selection -= C1_UI_EXTENDED_SYMBOL_COLUMNS;
        }
        break;
    case C1_UI_EVENT_DOWN:
        if (selection + C1_UI_EXTENDED_SYMBOL_COLUMNS < C1_UI_EXTENDED_SYMBOL_COUNT) {
            selection += C1_UI_EXTENDED_SYMBOL_COLUMNS;
        }
        break;
    default:
        break;
    }
    state->symbol_selection = selection;
}

static void activate_extended_symbol(c1_ui_state *state)
{
    char character = c1_ui_extended_symbol(state->symbol_selection);

    if (character != '\0') {
        c1_ui_secret_append(state, character);
    }
}

bool c1_ui_network_is_current(const c1_ui_status *status, size_t index)
{
    if (status == NULL || !status->wifi_connected || index >= status->network_count ||
        index >= C1_UI_MAX_NETWORKS ||
        strcmp(status->wifi_connected_ssid, status->networks[index].ssid) != 0) return false;
    for (size_t i = 0; i < status->network_count && i < C1_UI_MAX_NETWORKS; ++i) {
        if (strcmp(status->networks[i].ssid, status->networks[index].ssid) == 0 &&
            status->networks[i].security != status->networks[index].security) return false;
    }
    return true;
}

/* Called once after a successful fresh scan, never by periodic redraws.
 * Retain a live connection; otherwise choose the strongest visible saved row. */
c1_ui_transition c1_ui_autoconnect(c1_ui_state state, const c1_ui_status *status)
{
    c1_ui_transition next = {state, C1_UI_ACTION_NONE};
    if (status == NULL || state.page != C1_UI_PAGE_WIFI || status->wifi_connected ||
        status->service_busy || status->wifi_stop_pending) return next;
    for (size_t i = 0; i < status->network_count && i < C1_UI_MAX_NETWORKS; ++i) {
        if (!status->networks[i].saved ||
            (status->networks[i].security != C1_WIFI_SECURITY_OPEN &&
             status->networks[i].security != C1_WIFI_SECURITY_WPA_PSK)) continue;
        c1_ui_clear_secret(&next.state);
        next.state.selected_saved = true;
        next.state.selected_security = status->networks[i].security;
        snprintf(next.state.selected_ssid, sizeof(next.state.selected_ssid), "%s", status->networks[i].ssid);
        next.state.selection = 0U;
        next.state.wifi_notice[0] = '\0';
        next.action = C1_UI_ACTION_WIFI_CONNECT;
        break;
    }
    return next;
}

size_t c1_ui_battery_selected(const c1_ui_state *state, const c1_ui_status *status)
{
    size_t count = status ? status->battery_history.count : 0U;
    if (count > C1_BATTERY_HISTORY_POINTS) count = C1_BATTERY_HISTORY_POINTS;
    if (!count || !state) return 0U;
    if (!state->battery_selected_at) return count - 1U;
    /* Choose the first remaining point at/after an expired pinned timestamp. */
    for (size_t i = 0; i < count; ++i)
        if (status->battery_history.samples[i].timestamp >= state->battery_selected_at) return i;
    return count - 1U;
}

c1_ui_transition c1_ui_step(c1_ui_state state, c1_ui_event event, const c1_ui_status *status)
{
    c1_ui_transition transition = {state, C1_UI_ACTION_NONE};
    size_t network_count = status != NULL && status->network_count < C1_UI_MAX_NETWORKS
        ? status->network_count : status != NULL ? C1_UI_MAX_NETWORKS : 0U;

    if (state.page == C1_UI_PAGE_LOCK) return transition;
    if (event == C1_UI_EVENT_BATTERY || event == C1_UI_EVENT_SETTINGS) {
        c1_ui_clear_secret(&transition.state);
        if (state.page == C1_UI_PAGE_LOCK_TEXT) {
            memset(transition.state.lock_text_draft, 0, sizeof(transition.state.lock_text_draft));
            transition.state.lock_text_cursor = 0U;
        }
        transition.state.page = event == C1_UI_EVENT_BATTERY ? C1_UI_PAGE_BATTERY : C1_UI_PAGE_SETTINGS;
        if (event == C1_UI_EVENT_BATTERY) transition.state.battery_selected_at = 0;
        transition.state.selection = 0;
        transition.state.wifi_notice[0] = 0;
        return transition;
    }
    if (state.page == C1_UI_PAGE_BATTERY) transition.state.wifi_notice[0] = '\0';
    if (event == C1_UI_EVENT_HOME) {
        c1_ui_clear_secret(&transition.state);
        if (state.page == C1_UI_PAGE_LOCK_TEXT) {
            memset(transition.state.lock_text_draft, 0, sizeof(transition.state.lock_text_draft));
            transition.state.lock_text_cursor = 0U;
            transition.state.wifi_notice[0] = '\0';
        }
        transition.state.page = C1_UI_PAGE_DESKTOP;
        transition.state.selection = state.desktop_selection < 5U ? state.desktop_selection : 0U;
        return transition;
    }
    if (event == C1_UI_EVENT_BACK) {
        c1_ui_clear_secret(&transition.state);
        if (state.page == C1_UI_PAGE_WIFI_PASSWORD) {
            transition.state.page = C1_UI_PAGE_WIFI;
            transition.state.selection = 0U;
        } else if (state.page == C1_UI_PAGE_ABOUT) {
            transition.state.page = C1_UI_PAGE_SETTINGS;
            transition.state.selection = C1_SETTING_ABOUT;
        } else if (state.page == C1_UI_PAGE_LOCK_TEXT) {
            memset(transition.state.lock_text_draft, 0, sizeof(transition.state.lock_text_draft));
            transition.state.lock_text_cursor = 0U;
            transition.state.wifi_notice[0] = '\0';
            transition.state.page = C1_UI_PAGE_SETTINGS;
            transition.state.selection = C1_SETTING_LOCK_TEXT;
        } else {
            transition.state.page = C1_UI_PAGE_DESKTOP;
            transition.state.selection = state.desktop_selection < 5U ? state.desktop_selection : 0U;
        }
        return transition;
    }
    if (state.page == C1_UI_PAGE_LOCK_TEXT) {
        if (event == C1_UI_EVENT_LEFT || event == C1_UI_EVENT_RIGHT)
            (void)c1_ui_lock_text_move(&transition.state, event == C1_UI_EVENT_LEFT ? -1 : 1);
        if (event == C1_UI_EVENT_ENTER) {
            if (!c1_preferences_valid_text(state.lock_text_draft)) {
                snprintf(transition.state.wifi_notice, sizeof(transition.state.wifi_notice), "%s",
                         c1_ui_tr(state.preferences.language,
                                  "请输入有效的非空文字", "ENTER VALID NONEMPTY TEXT"));
                return transition;
            }
            memcpy(transition.state.preferences.lock_text, state.lock_text_draft,
                   sizeof(transition.state.preferences.lock_text));
            memset(transition.state.lock_text_draft, 0, sizeof(transition.state.lock_text_draft));
            transition.state.lock_text_cursor = 0U;
            transition.state.wifi_notice[0] = '\0';
            transition.state.page = C1_UI_PAGE_SETTINGS;
            transition.state.selection = C1_SETTING_LOCK_TEXT;
        }
        return transition;
    }
    if (state.page == C1_UI_PAGE_ABOUT) return transition;
    if (state.page == C1_UI_PAGE_BATTERY) {
        size_t count = status ? status->battery_history.count : 0U;
        if (count > C1_BATTERY_HISTORY_POINTS) count = C1_BATTERY_HISTORY_POINTS;
        if (event == C1_UI_EVENT_VIEW_PREVIOUS && state.battery_view > C1_BATTERY_VIEW_DAY)
            transition.state.battery_view = (c1_battery_view)(state.battery_view - 1);
        if (event == C1_UI_EVENT_VIEW_NEXT && state.battery_view < C1_BATTERY_VIEW_MINUTE)
            transition.state.battery_view = (c1_battery_view)(state.battery_view + 1);
        if (!count) {
            transition.state.selection = 0;
            transition.state.battery_selected_at = 0;
        } else {
            size_t selected = c1_ui_battery_selected(&state, status);
            if ((event == C1_UI_EVENT_LEFT || event == C1_UI_EVENT_UP) && selected) {
                --selected;
                transition.state.battery_selected_at = status->battery_history.samples[selected].timestamp;
            }
            if (event == C1_UI_EVENT_RIGHT || event == C1_UI_EVENT_DOWN) {
                if (selected + 1U < count) ++selected;
                transition.state.battery_selected_at = selected + 1U == count ? 0 :
                    status->battery_history.samples[selected].timestamp;
            }
            transition.state.selection = (uint32_t)(count - 1U - selected);
        }
        return transition; /* Read-only: Left/Right choose samples; volume chooses scale. */
    }
    if (state.page == C1_UI_PAGE_SETTINGS) {
        if (event == C1_UI_EVENT_UP && state.selection) --transition.state.selection;
        if (event == C1_UI_EVENT_DOWN && state.selection + 1U < C1_SETTING_COUNT) ++transition.state.selection;
        if (event == C1_UI_EVENT_SELECT_NEXT) transition.state.selection = (state.selection + 1U) % C1_SETTING_COUNT;
        if (state.selection == C1_SETTING_ABOUT) {
            if (event == C1_UI_EVENT_ENTER) { transition.state.page = C1_UI_PAGE_ABOUT; transition.state.selection = 0; }
            return transition;
        }
        if (state.selection == C1_SETTING_UPDATE) {
            /* Core updates are explicit settings actions, not desktop shortcuts. */
            if (event != C1_UI_EVENT_ENTER) return transition;
            if (status != NULL && status->service_busy) {
                snprintf(transition.state.wifi_notice, sizeof(transition.state.wifi_notice), "%s",
                         c1_ui_tr(state.preferences.language, "任务进行中，请稍候", "Task in progress"));
            } else if (status != NULL && status->update_prepared) {
                transition.state.page = C1_UI_PAGE_TERMINAL;
                transition.state.selection = 0U;
                transition.state.terminal_action = transition.action = C1_UI_ACTION_TERMINAL_UPDATE;
            } else if (status == NULL || !status->wifi_connected || !status->wifi_ipv4[0]) {
                snprintf(transition.state.wifi_notice, sizeof(transition.state.wifi_notice), "%s",
                         c1_ui_tr(state.preferences.language, "无网络，请先连接 Wi-Fi", "No network. Connect Wi-Fi first."));
            } else {
                transition.state.wifi_notice[0] = '\0';
                transition.action = C1_UI_ACTION_UPDATE_REFRESH;
            }
            return transition;
        }
        if (event == C1_UI_EVENT_LEFT || event == C1_UI_EVENT_RIGHT || event == C1_UI_EVENT_ENTER) {
            if (state.selection == C1_SETTING_LOCK_TEXT) {
                memset(transition.state.lock_text_draft, 0, sizeof(transition.state.lock_text_draft));
                if (c1_preferences_valid_text(state.preferences.lock_text))
                    memcpy(transition.state.lock_text_draft, state.preferences.lock_text,
                           strlen(state.preferences.lock_text) + 1U);
                transition.state.lock_text_cursor = strlen(transition.state.lock_text_draft);
                transition.state.wifi_notice[0] = '\0';
                transition.state.page = C1_UI_PAGE_LOCK_TEXT;
            } else {
                c1_preferences_cycle(&transition.state.preferences, state.selection,
                                     event == C1_UI_EVENT_LEFT ? -1 : 1);
            }
        }
        return transition;
    }
    if (state.page == C1_UI_PAGE_DESKTOP) {
        /* Navigation selects; only confirm launches. Removing the old
         * directional launch shortcuts must not disable the physical D-pad. */
        if (event == C1_UI_EVENT_ENTER) return activate_desktop(state, status);
        if (event == C1_UI_EVENT_DOWN || event == C1_UI_EVENT_RIGHT)
            transition.state.selection = (state.selection + 1U) % 5U;
        else if (event == C1_UI_EVENT_UP || event == C1_UI_EVENT_LEFT)
            transition.state.selection = (state.selection + 4U) % 5U;
        if (event == C1_UI_EVENT_UP || event == C1_UI_EVENT_DOWN || event == C1_UI_EVENT_LEFT || event == C1_UI_EVENT_RIGHT)
            transition.state.wifi_notice[0] = '\0';
        transition.state.desktop_selection = transition.state.selection;
        return transition;
    }
    if (state.page == C1_UI_PAGE_WIFI) {
        size_t item_count = network_count + 2U;

        if (transition.state.selection >= item_count) {
            transition.state.selection = network_count > 0U ? (uint32_t)(item_count - 1U) : 0U;
        }
        if (event == C1_UI_EVENT_SELECT_NEXT)
            transition.state.selection = (transition.state.selection + 1U) % (uint32_t)item_count;
        if (event == C1_UI_EVENT_LEFT && transition.state.selection == 1U) {
            transition.state.selection = 0U;
        } else if (event == C1_UI_EVENT_RIGHT && transition.state.selection == 0U) {
            transition.state.selection = 1U;
        } else if (event == C1_UI_EVENT_UP && transition.state.selection >= 2U) {
            transition.state.selection = transition.state.selection == 2U
                                             ? 0U
                                             : transition.state.selection - 1U;
        } else if (event == C1_UI_EVENT_DOWN) {
            if (transition.state.selection < 2U && network_count > 0U) {
                transition.state.selection = 2U;
            } else if (transition.state.selection >= 2U && transition.state.selection + 1U < item_count) {
                ++transition.state.selection;
            }
        } else if (event == C1_UI_EVENT_ENTER) {
            transition.state.wifi_notice[0] = '\0';
            if (status != NULL && status->service_busy && transition.state.selection != 1U) {
                snprintf(transition.state.wifi_notice, sizeof(transition.state.wifi_notice),
                         "BUSY - WAIT OR SELECT TURN OFF");
                return transition;
            }
            if (transition.state.selection == 0U) {
                transition.action = C1_UI_ACTION_WIFI_SCAN;
            } else if (transition.state.selection == 1U) {
                transition.action = C1_UI_ACTION_WIFI_DISABLE;
            } else if (transition.state.selection < item_count && status != NULL) {
                size_t network_index = transition.state.selection - 2U;
                transition.state.selected_security = status->networks[network_index].security;
                transition.state.selected_saved = status->networks[network_index].saved;
                if (transition.state.selected_security != C1_WIFI_SECURITY_OPEN &&
                    transition.state.selected_security != C1_WIFI_SECURITY_WPA_PSK) {
                    snprintf(transition.state.wifi_notice, sizeof(transition.state.wifi_notice),
                             "THIS SECURITY TYPE IS NOT SUPPORTED");
                    return transition;
                }

                if (c1_ui_network_is_current(status, network_index)) {
                    snprintf(transition.state.wifi_notice, sizeof(transition.state.wifi_notice),
                             "ALREADY CONNECTED TO THIS NETWORK");
                    return transition;
                }
                snprintf(transition.state.selected_ssid,
                         sizeof(transition.state.selected_ssid),
                         "%s",
                         status->networks[network_index].ssid);
                c1_ui_clear_secret(&transition.state);
                transition.state.symbol_selection = 0U;
                transition.state.keyboard_layer = C1_UI_KEYBOARD_LOWER;
                if (status->networks[network_index].secured && !transition.state.selected_saved) {
                    transition.state.page = C1_UI_PAGE_WIFI_PASSWORD;
                    transition.state.secret_visible = true;
                } else {
                    transition.action = C1_UI_ACTION_WIFI_CONNECT;
                }
            }
        }
        return transition;
    }
    if (state.page == C1_UI_PAGE_WIFI_PASSWORD) {
        if (event == C1_UI_EVENT_TOGGLE_SECRET) {
            transition.state.secret_visible = !state.secret_visible;
        } else if (event == C1_UI_EVENT_SUBMIT ||
                   (event == C1_UI_EVENT_ENTER && state.keyboard_layer != C1_UI_KEYBOARD_SYMBOLS)) {
            if (status != NULL && status->service_busy) {
                snprintf(transition.state.wifi_notice, sizeof(transition.state.wifi_notice),
                         "BUSY - YOUR PASSWORD IS KEPT");
            } else if (state.secret_length < 8U || state.secret_length > 63U) {
                snprintf(transition.state.wifi_notice, sizeof(transition.state.wifi_notice),
                         "USE 8-63 CHARACTERS");
            } else {
                transition.state.wifi_notice[0] = '\0';
                transition.action = C1_UI_ACTION_WIFI_CONNECT;
            }
        } else if (state.keyboard_layer == C1_UI_KEYBOARD_SYMBOLS) {
            if (event == C1_UI_EVENT_ENTER) {
                activate_extended_symbol(&transition.state);
            } else {
                move_extended_symbol(&transition.state, event);
            }
        }
    }
    return transition;
}

c1_ui_state c1_ui_reduce(c1_ui_state state, c1_ui_event event)
{
    return c1_ui_step(state, event, NULL).state;
}