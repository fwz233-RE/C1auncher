#ifndef C1_UI_RENDER_H
#define C1_UI_RENDER_H

#include "ui/model.h"
#include "ui/terminal_screen.h"

#include <stdint.h>

#define C1_LOCK_TEXT_MAX_LINES 9U

typedef struct {
    unsigned font_height;
    unsigned line_count;
    unsigned block_height;
    size_t offsets[C1_LOCK_TEXT_MAX_LINES];
    size_t lengths[C1_LOCK_TEXT_MAX_LINES];
    unsigned widths[C1_LOCK_TEXT_MAX_LINES];
} c1_lock_text_layout;

/* Largest whole-pixel font height that fits the complete valid UTF-8 text in
 * 284x144 pixels. Whitespace is a soft line boundary; no glyph is truncated. */
bool c1_ui_layout_lock_text(const char *text, c1_lock_text_layout *layout);

#define C1_LOCK_TEXT_EDIT_WIDTH 264U
typedef struct {
    size_t offset, length; /* Complete code points in the visible single-line window. */
    unsigned cursor_x; /* Pixel offset; a two-pixel caret gap separates prefix/suffix. */
} c1_lock_text_edit_layout;
bool c1_ui_layout_lock_text_edit(const c1_ui_state *state, c1_lock_text_edit_layout *layout);

void c1_ui_render(uint8_t *frame,
                  const c1_ui_state *state,
                  const c1_ui_status *status,
                  c1_terminal_screen *terminal);

struct c1_ime_response;
void c1_ui_render_input(uint8_t *frame, const c1_ui_state *state,
                        const struct c1_ime_response *view, bool enabled, bool failed);

#endif