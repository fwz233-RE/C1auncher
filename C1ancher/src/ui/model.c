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
    state.selection = 4U;
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
    state->page = C1_UI_PAGE_DESKTOP;
    state->selection = 4U;
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

static c1_ui_state reduce_desktop(c1_ui_state state, c1_ui_event event)
{
    switch (event) {
    case C1_UI_EVENT_UP:
        state.page = C1_UI_PAGE_WIFI;
        state.selection = 0U;
        break;
    case C1_UI_EVENT_DOWN:
        state.page = C1_UI_PAGE_TERMINAL;
        state.selection = 0U;
        break;
    case C1_UI_EVENT_LEFT:
        state.page = C1_UI_PAGE_TERMINAL;
        state.selection = 0U;
        break;
    case C1_UI_EVENT_RIGHT:
        state.page = C1_UI_PAGE_TERMINAL;
        state.selection = 0U;
        break;
    default:
        state.selection = 4U;
        break;
    }
    return state;
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

c1_ui_transition c1_ui_step(c1_ui_state state, c1_ui_event event, const c1_ui_status *status)
{
    c1_ui_transition transition = {state, C1_UI_ACTION_NONE};
    size_t network_count = status != NULL ? status->network_count : 0U;

    if (event == C1_UI_EVENT_HOME) {
        c1_ui_clear_secret(&transition.state);
        transition.state.page = C1_UI_PAGE_DESKTOP;
        transition.state.selection = 4U;
        return transition;
    }
    if (event == C1_UI_EVENT_BACK) {
        c1_ui_clear_secret(&transition.state);
        if (state.page == C1_UI_PAGE_WIFI_PASSWORD) {
            transition.state.page = C1_UI_PAGE_WIFI;
            transition.state.selection = 0U;
        } else {
            transition.state.page = C1_UI_PAGE_DESKTOP;
            transition.state.selection = 4U;
        }
        return transition;
    }
    if (state.page == C1_UI_PAGE_DESKTOP) {
        transition.state = reduce_desktop(state, event);
        if (event == C1_UI_EVENT_UP) {
            transition.state.wifi_notice[0] = '\0';
            if (status == NULL || !status->service_busy) {
                transition.action = C1_UI_ACTION_WIFI_SCAN;
            } else {
                snprintf(transition.state.wifi_notice, sizeof(transition.state.wifi_notice),
                         status->wifi_busy ? "WI-FI TASK ALREADY RUNNING" :
                         "UPDATE CHECK RUNNING; TRY SCAN LATER");
            }
        } else if (event == C1_UI_EVENT_DOWN) {
            transition.action = C1_UI_ACTION_TERMINAL_NEOFETCH;
        } else if (event == C1_UI_EVENT_LEFT) {
            transition.action = C1_UI_ACTION_TERMINAL_APP;
        } else if (event == C1_UI_EVENT_ENTER && status != NULL &&
                   status->wifi_connected && status->wifi_ipv4[0] != '\0') {
            /* Check/download in the existing background service worker. Never
             * enter a terminal just to discover an offline or current release.
             * A later explicit press on UPDATE confirms a prepared release. */
            if (status->update_available) {
                transition.state.page = C1_UI_PAGE_TERMINAL;
                transition.state.selection = 0U;
                transition.action = C1_UI_ACTION_TERMINAL_UPDATE;
            } else {
                transition.action = C1_UI_ACTION_UPDATE_REFRESH;
            }
        }
        return transition;
    }
    if (state.page == C1_UI_PAGE_WIFI) {
        size_t item_count = network_count + 2U;

        if (transition.state.selection >= item_count) {
            transition.state.selection = network_count > 0U ? (uint32_t)(item_count - 1U) : 0U;
        }
        if (status != NULL && status->wifi_busy &&
            (event == C1_UI_EVENT_DOWN || event == C1_UI_EVENT_UP)) {
            transition.state.selection = 0U;
            return transition;
        }
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