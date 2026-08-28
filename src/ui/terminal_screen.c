#include "ui/terminal_screen.h"

#include "libtsm.h"
#include "xkbcommon/xkbcommon-keysyms.h"

#include <string.h>

static void queue_reply(struct tsm_vte *vte, const char *bytes, size_t count, void *data)
{
    c1_terminal_screen *terminal = data;
    size_t available;

    (void)vte;
    if (terminal == NULL || bytes == NULL || count == 0U) {
        return;
    }
    if (terminal->reply_offset > 0U &&
        terminal->reply_offset + terminal->reply_length + count > sizeof(terminal->reply)) {
        memmove(terminal->reply,
                terminal->reply + terminal->reply_offset,
                terminal->reply_length);
        terminal->reply_offset = 0U;
    }
    available = sizeof(terminal->reply) - terminal->reply_offset - terminal->reply_length;
    if (count > available) {
        count = available;
    }
    memcpy(terminal->reply + terminal->reply_offset + terminal->reply_length, bytes, count);
    terminal->reply_length += count;
}

c1_status c1_terminal_screen_init(c1_terminal_screen *terminal)
{
    int result;

    if (terminal == NULL) {
        return C1_STATUS_INVALID_ARGUMENT;
    }
    memset(terminal, 0, sizeof(*terminal));
    result = tsm_screen_new(&terminal->screen, NULL, NULL);
    if (result != 0) {
        return C1_STATUS_UNAVAILABLE;
    }
    result = tsm_screen_resize(terminal->screen, C1_TERMINAL_COLUMNS, C1_TERMINAL_ROWS);
    if (result != 0) {
        c1_terminal_screen_destroy(terminal);
        return C1_STATUS_UNAVAILABLE;
    }
    tsm_screen_set_max_sb(terminal->screen, C1_TERMINAL_SCROLLBACK);
    result = tsm_vte_new(&terminal->vte,
                         terminal->screen,
                         queue_reply,
                         terminal,
                         NULL,
                         NULL);
    if (result != 0) {
        c1_terminal_screen_destroy(terminal);
        return C1_STATUS_UNAVAILABLE;
    }
    tsm_vte_set_backspace_sends_delete(terminal->vte, true);
    return C1_STATUS_OK;
}

void c1_terminal_screen_destroy(c1_terminal_screen *terminal)
{
    if (terminal == NULL) {
        return;
    }
    if (terminal->vte != NULL) {
        tsm_vte_unref(terminal->vte);
    }
    if (terminal->screen != NULL) {
        tsm_screen_unref(terminal->screen);
    }
    memset(terminal, 0, sizeof(*terminal));
}

void c1_terminal_screen_reset(c1_terminal_screen *terminal)
{
    if (terminal == NULL || terminal->vte == NULL) {
        return;
    }
    tsm_vte_hard_reset(terminal->vte);
    tsm_screen_clear_sb(terminal->screen);
    terminal->reply_offset = 0U;
    terminal->reply_length = 0U;
}

void c1_terminal_screen_feed(c1_terminal_screen *terminal, const void *bytes, size_t count)
{
    if (terminal == NULL || terminal->vte == NULL || bytes == NULL || count == 0U) {
        return;
    }
    tsm_vte_input(terminal->vte, bytes, count);
}

static int collect_cell(struct tsm_screen *screen,
                        uint64_t id,
                        const uint32_t *characters,
                        size_t length,
                        unsigned int width,
                        unsigned int column,
                        unsigned int row,
                        const struct tsm_screen_attr *attributes,
                        tsm_age_t age,
                        void *data)
{
    c1_terminal_screen *terminal = data;
    c1_terminal_cell *cell;

    (void)screen;
    (void)id;
    (void)width;
    (void)age;
    if (terminal == NULL || column >= C1_TERMINAL_COLUMNS || row >= C1_TERMINAL_ROWS) {
        return 0;
    }
    cell = &terminal->cells[row][column];
    cell->codepoint = length > 0U && characters != NULL ? characters[0] : (uint32_t)' ';
    cell->inverse = attributes != NULL && attributes->inverse;
    cell->underline = attributes != NULL && attributes->underline;
    cell->bold = attributes != NULL && attributes->bold;
    return 0;
}

const c1_terminal_cell *c1_terminal_screen_cells(c1_terminal_screen *terminal)
{
    if (terminal == NULL || terminal->screen == NULL) {
        return NULL;
    }
    memset(terminal->cells, 0, sizeof(terminal->cells));
    (void)tsm_screen_draw(terminal->screen, collect_cell, terminal);
    return &terminal->cells[0][0];
}

bool c1_terminal_screen_character(c1_terminal_screen *terminal,
                                  uint32_t codepoint,
                                  unsigned int modifiers)
{
    uint32_t ascii;

    if (terminal == NULL || terminal->vte == NULL || codepoint == 0U) {
        return false;
    }
    ascii = codepoint < 128U ? codepoint : TSM_VTE_INVALID;
    return tsm_vte_handle_keyboard(terminal->vte,
                                   codepoint,
                                   ascii,
                                   modifiers,
                                   codepoint);
}

static uint32_t key_symbol(c1_terminal_key key)
{
    switch (key) {
    case C1_TERMINAL_KEY_UP: return XKB_KEY_Up;
    case C1_TERMINAL_KEY_DOWN: return XKB_KEY_Down;
    case C1_TERMINAL_KEY_LEFT: return XKB_KEY_Left;
    case C1_TERMINAL_KEY_RIGHT: return XKB_KEY_Right;
    case C1_TERMINAL_KEY_HOME: return XKB_KEY_Home;
    case C1_TERMINAL_KEY_DELETE: return XKB_KEY_Delete;
    case C1_TERMINAL_KEY_BACKSPACE: return XKB_KEY_BackSpace;
    case C1_TERMINAL_KEY_ENTER: return XKB_KEY_Return;
    case C1_TERMINAL_KEY_PAGE_UP: return XKB_KEY_Page_Up;
    case C1_TERMINAL_KEY_PAGE_DOWN: return XKB_KEY_Page_Down;
    case C1_TERMINAL_KEY_ESCAPE: return XKB_KEY_Escape;
    }
    return XKB_KEY_VoidSymbol;
}

bool c1_terminal_screen_special(c1_terminal_screen *terminal,
                                c1_terminal_key key,
                                unsigned int modifiers)
{
    uint32_t symbol;

    if (terminal == NULL || terminal->vte == NULL) {
        return false;
    }
    symbol = key_symbol(key);
    if (symbol == XKB_KEY_VoidSymbol) {
        return false;
    }
    return tsm_vte_handle_keyboard(terminal->vte,
                                   symbol,
                                   TSM_VTE_INVALID,
                                   modifiers,
                                   TSM_VTE_INVALID);
}

size_t c1_terminal_screen_take_reply(c1_terminal_screen *terminal, void *buffer, size_t capacity)
{
    size_t count;

    if (terminal == NULL || buffer == NULL || capacity == 0U || terminal->reply_length == 0U) {
        return 0U;
    }
    count = terminal->reply_length < capacity ? terminal->reply_length : capacity;
    memcpy(buffer, terminal->reply + terminal->reply_offset, count);
    terminal->reply_offset += count;
    terminal->reply_length -= count;
    if (terminal->reply_length == 0U) {
        terminal->reply_offset = 0U;
    }
    return count;
}

void c1_terminal_screen_scroll_up(c1_terminal_screen *terminal, unsigned int lines)
{
    if (terminal != NULL && terminal->screen != NULL) {
        tsm_screen_sb_up(terminal->screen, lines);
    }
}

void c1_terminal_screen_scroll_down(c1_terminal_screen *terminal, unsigned int lines)
{
    if (terminal != NULL && terminal->screen != NULL) {
        tsm_screen_sb_down(terminal->screen, lines);
    }
}

void c1_terminal_screen_scroll_page_up(c1_terminal_screen *terminal)
{
    if (terminal != NULL && terminal->screen != NULL) {
        tsm_screen_sb_page_up(terminal->screen, 1U);
    }
}

void c1_terminal_screen_scroll_page_down(c1_terminal_screen *terminal)
{
    if (terminal != NULL && terminal->screen != NULL) {
        tsm_screen_sb_page_down(terminal->screen, 1U);
    }
}

void c1_terminal_screen_scroll_reset(c1_terminal_screen *terminal)
{
    if (terminal != NULL && terminal->screen != NULL) {
        tsm_screen_sb_reset(terminal->screen);
    }
}

unsigned int c1_terminal_screen_scroll_position(c1_terminal_screen *terminal)
{
    return terminal != NULL && terminal->screen != NULL
               ? tsm_screen_sb_get_line_pos(terminal->screen)
               : 0U;
}