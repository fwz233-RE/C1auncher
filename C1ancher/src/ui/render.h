#ifndef C1_UI_RENDER_H
#define C1_UI_RENDER_H

#include "ui/model.h"
#include "ui/terminal_screen.h"

#include <stdint.h>

void c1_ui_render(uint8_t *frame,
                  const c1_ui_state *state,
                  const c1_ui_status *status,
                  c1_terminal_screen *terminal);

#endif