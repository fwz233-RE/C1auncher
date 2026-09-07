/* 未完成游戏的占位实现 — "COMING SOON" 页 */
#include "game.h"
#include "../config.h"
#include "../gfx/canvas.h"
#include "../gfx/font.h"
#include "../platform/display.h"
#include "../platform/input.h"

static void stub_render_title(const char *title) {
    fb_clear(false);   /* 全帧重绘 */
    fb_text_scale2((CCG_W - text_width(title) * 2) / 2, 40, title, true);
    fb_text_center(70, "COMING SOON", true);
    fb_text_center(90, "OK:RETURN", true);
    hud_draw(title, 0);
}

static void stub_enter(const char *title) {
    stub_render_title(title);
    disp_full();
}

static void stub_on_key(const key_event_t *ev) {
    if (ev->is_repeat) return;
    if (ev->key == K_OK || ev->key == K_BACK || ev->key == K_QUIT)
        s_exit_request = true;
}

#define STUB_GAME(NAME, TITLE, TAG, HELP0, HELP1, HELP2, HELP3, HELP4)        \
    void NAME##_enter(void) { stub_enter(TITLE); }                            \
    void NAME##_on_key(const key_event_t *ev) { stub_on_key(ev); }            \
    void NAME##_render(void) { stub_render_title(TITLE); }                    \
    void NAME##_tick(uint64_t now) { (void)now; }                             \
    void NAME##_exit(void) {}

STUB_GAME(gomoku_s, "GOMOKU", "FIVE IN A ROW",
          "GOMOKU", "COMING SOON", NULL, NULL, NULL)

#undef STUB_GAME
