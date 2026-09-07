#include "ui_common.h"
#include "../app_runtime.h"
#include "../config.h"
#include "../gfx/canvas.h"
#include "../gfx/font.h"
#include "../platform/display.h"
#include "../platform/input.h"
#include "../platform/audio.h"

void hud_draw(const char *title, uint32_t score) {
    /* 顶栏: 黑字标题 + 右对齐分数 + 分隔线 */
    fb_text(0, 0, title, true);
    char buf[16];
    unsigned i = 0;
    uint32_t v = score;
    if (v == 0) { buf[0] = '0'; i = 1; }
    while (v && i < 14) { buf[i++] = (char)('0' + v % 10); v /= 10; }
    buf[i] = 0;
    /* 反转数字 */
    char rev[16];
    unsigned len = i;
    for (unsigned j = 0; j < len; j++) rev[j] = buf[len - 1 - j];
    rev[len] = 0;
    char full[24];
    const char *label = "SCORE ";
    unsigned n = 0;
    while (label[n]) { full[n] = label[n]; n++; }
    for (unsigned j = 0; j < len; j++) full[n++] = rev[j];
    full[n] = 0;
    int x = CCG_W - text_width(full) - 4;
    fb_text(x, 0, full, true);
    fb_hline(0, CCG_HUD_H - 1, CCG_W, true);
}

void msg_center(const char *line1, const char *line2) {
    int w = text_width(line1);
    if (line2) {
        int w2 = text_width(line2);
        if (w2 > w) w = w2;
    }
    w += 8;
    int h = (line2 ? 20 : 13);
    int x = (CCG_W - w) / 2;
    int y = (CCG_H - h) / 2;
    fb_stroke_rect_thick(x, y, w, h, 2, true);
    fb_text_center(y + 4, line1, true);
    if (line2) fb_text_center(y + 13, line2, true);
}

/* ---- 暂停覆盖层 ---- */
static bool s_pause_active;
static pause_sel_t s_pause_sel;

static void pause_draw(void) {
    static const char *items[PAUSE_ITEMS] = { "RESUME", "RESTART", "QUIT" };
    /* 半屏中央面板 */
    int w = 120, h = 3 * 10 + 14;
    int x = (CCG_W - w) / 2, y = (CCG_H - h) / 2;
    fb_fill_rect(x - 2, y - 2, w + 4, h + 4, false);   /* 白底盖住游戏画面 */
    fb_stroke_rect_thick(x - 2, y - 2, w + 4, h + 4, 2, true);
    fb_text_center(y, "PAUSED", true);
    for (int i = 0; i < PAUSE_ITEMS; i++) {
        if (i == (int)s_pause_sel) {
            fb_text_inv(x + 8, y + 12 + i * 10, items[i]);
        } else {
            fb_text(x + 8, y + 12 + i * 10, items[i], true);
        }
    }
}

bool ui_pause_run(pause_sel_t *sel) {
    s_pause_active = true;
    s_pause_sel = PAUSE_RESUME;
    pause_draw();
    disp_full();
    for (;;) {
        if (app_runtime_checkpoint()) {
            s_pause_active = false;
            *sel = PAUSE_QUIT;
            return false;
        }
        int timeout = 200;
        input_poll(timeout);
        if (app_runtime_checkpoint()) {
            s_pause_active = false;
            *sel = PAUSE_QUIT;
            return false;
        }
        key_event_t ev;
        while (input_get(&ev)) {
            if (ev.is_repeat) continue;
            switch (ev.key) {
            case K_UP: s_pause_sel = (pause_sel_t)((s_pause_sel + PAUSE_ITEMS - 1) % PAUSE_ITEMS); audio_tick(); pause_draw(); disp_fast(); break;
            case K_DOWN: s_pause_sel = (pause_sel_t)((s_pause_sel + 1) % PAUSE_ITEMS); audio_tick(); pause_draw(); disp_fast(); break;
            case K_OK:
            case K_SPACE:
                audio_select();
                s_pause_active = false;
                *sel = s_pause_sel;
                /* QUIT 与面板内按 BACK 语义一致(返回 false): 游戏侧 else →
                 * s_exit_request → 返回主菜单. (86 款游戏均已按此结构编写) */
                return s_pause_sel != PAUSE_QUIT;
            case K_BACK:
            case K_QUIT:
                s_pause_active = false;
                *sel = PAUSE_QUIT;
                return false;
            default: break;
            }
        }
    }
}
