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
    return true;
}

bool c1_ui_secret_delete(c1_ui_state *state)
{
    if (state == NULL || !c1_ui_is_password_page(state->page) || state->secret_length == 0U) {
        return false;
    }
    state->secret[--state->secret_length] = '\0';
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
        state.page = C1_UI_PAGE_SSH;
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
            transition.action = C1_UI_ACTION_WIFI_SCAN;
        } else if (event == C1_UI_EVENT_DOWN) {
            transition.action = C1_UI_ACTION_TERMINAL_NEOFETCH;
        }
        return transition;
    }
    if (state.page == C1_UI_PAGE_WIFI) {
        size_t item_count = network_count + 2U;

        if (transition.state.selection >= item_count) {
            transition.state.selection = network_count > 0U ? (uint32_t)(item_count - 1U) : 0U;
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
            if (transition.state.selection == 0U) {
                transition.action = C1_UI_ACTION_WIFI_SCAN;
            } else if (transition.state.selection == 1U) {
                transition.action = C1_UI_ACTION_WIFI_DISABLE;
            } else if (transition.state.selection < item_count && status != NULL) {
                size_t network_index = transition.state.selection - 2U;

                snprintf(transition.state.selected_ssid,
                         sizeof(transition.state.selected_ssid),
                         "%s",
                         status->networks[network_index].ssid);
                c1_ui_clear_secret(&transition.state);
                transition.state.symbol_selection = 0U;
                transition.state.keyboard_layer = C1_UI_KEYBOARD_LOWER;
                transition.state.page = C1_UI_PAGE_WIFI_PASSWORD;
            }
        }
        return transition;
    }
    if (state.page == C1_UI_PAGE_SSH && event == C1_UI_EVENT_ENTER) {
        transition.action = status != NULL && status->ssh_enabled
                                ? C1_UI_ACTION_SSH_DISABLE
                                : C1_UI_ACTION_SSH_ENABLE;
        return transition;
    }
    if (state.page == C1_UI_PAGE_WIFI_PASSWORD) {
        if (event == C1_UI_EVENT_SUBMIT) {
            transition.action = C1_UI_ACTION_WIFI_CONNECT;
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