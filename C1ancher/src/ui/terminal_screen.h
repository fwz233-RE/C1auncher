#ifndef C1_UI_TERMINAL_SCREEN_H
#define C1_UI_TERMINAL_SCREEN_H

#include "core/status.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define C1_TERMINAL_COLUMNS 49U
#define C1_TERMINAL_ROWS 19U
#define C1_TERMINAL_SCROLLBACK 400U

#define C1_TERMINAL_MOD_SHIFT (1U << 0)
#define C1_TERMINAL_MOD_CONTROL (1U << 2)

typedef enum {
    C1_TERMINAL_KEY_UP = 0,
    C1_TERMINAL_KEY_DOWN,
    C1_TERMINAL_KEY_LEFT,
    C1_TERMINAL_KEY_RIGHT,
    C1_TERMINAL_KEY_HOME,
    C1_TERMINAL_KEY_DELETE,
    C1_TERMINAL_KEY_BACKSPACE,
    C1_TERMINAL_KEY_ENTER,
    C1_TERMINAL_KEY_PAGE_UP,
    C1_TERMINAL_KEY_PAGE_DOWN,
    C1_TERMINAL_KEY_ESCAPE
} c1_terminal_key;

typedef struct {
    uint32_t codepoint;
    bool inverse;
    bool underline;
    bool bold;
} c1_terminal_cell;

struct tsm_screen;
struct tsm_vte;

typedef struct {
    struct tsm_screen *screen;
    struct tsm_vte *vte;
    c1_terminal_cell cells[C1_TERMINAL_ROWS][C1_TERMINAL_COLUMNS];
    char reply[512];
    size_t reply_offset;
    size_t reply_length;
} c1_terminal_screen;

c1_status c1_terminal_screen_init(c1_terminal_screen *terminal);
void c1_terminal_screen_destroy(c1_terminal_screen *terminal);
void c1_terminal_screen_reset(c1_terminal_screen *terminal);
void c1_terminal_screen_feed(c1_terminal_screen *terminal, const void *bytes, size_t count);
const c1_terminal_cell *c1_terminal_screen_cells(c1_terminal_screen *terminal);
bool c1_terminal_screen_character(c1_terminal_screen *terminal,
                                  uint32_t codepoint,
                                  unsigned int modifiers);
bool c1_terminal_screen_special(c1_terminal_screen *terminal,
                                c1_terminal_key key,
                                unsigned int modifiers);
size_t c1_terminal_screen_take_reply(c1_terminal_screen *terminal, void *buffer, size_t capacity);
void c1_terminal_screen_scroll_up(c1_terminal_screen *terminal, unsigned int lines);
void c1_terminal_screen_scroll_down(c1_terminal_screen *terminal, unsigned int lines);
void c1_terminal_screen_scroll_page_up(c1_terminal_screen *terminal);
void c1_terminal_screen_scroll_page_down(c1_terminal_screen *terminal);
void c1_terminal_screen_scroll_reset(c1_terminal_screen *terminal);
unsigned int c1_terminal_screen_scroll_position(c1_terminal_screen *terminal);

#endif