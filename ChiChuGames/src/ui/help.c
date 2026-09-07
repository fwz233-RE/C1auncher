/* 游戏说明页 */
#include "help.h"
#include "../app_runtime.h"
#include "../config.h"
#include "../gfx/canvas.h"
#include "../gfx/font.h"
#include "../platform/display.h"
#include "../platform/input.h"
#include "../platform/audio.h"

bool help_run(const game_desc_t *g) {
    fb_clear(false);
    fb_text_center(8, g->title, true);
    fb_hline(0, 18, CCG_W, true);
    int y = 30;
    for (int i = 0; i < 6 && g->help[i]; i++) {
        fb_text_center(y, g->help[i], true);
        y += 11;
    }
    fb_text_center(CCG_H - 10, "OK:START  BACK:MENU", true);
    disp_full();
    for (;;) {
        if (app_runtime_checkpoint()) return false;
        input_poll(250);
        if (app_runtime_checkpoint()) return false;
        key_event_t ev;
        while (input_get(&ev)) {
            if (ev.is_repeat) continue;
            if (ev.key == K_OK || ev.key == K_SPACE) { audio_select(); return true; }
            if (ev.key == K_BACK || ev.key == K_QUIT) { audio_move(); return false; }
        }
    }
}
