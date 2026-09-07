/* WHACK-A-MOLE — 3x3 打地鼠(离散版, 墨水屏适配)
 * 地鼠随机在 9 洞中出现(每 0.8-1.5s 一只, 持续 1.2s);
 * 方向/WASD 移动光标, OK 击打当前洞: 有地鼠则得分+1
 * 60 秒限时(以 tick 计数), 时间到结算得分
 * HUD 顶栏: 左 WHACK, 右 SCORE n 与 TIME s
 * 无动画: 地鼠出现 = 洞里画 CG_MINE 3x 放大, 消失 = 空白
 * 静态前缀 wa_; 像素坐标一律 int; 零 malloc
 *
 * 集成提示(help[] 最多 5 行):
 *   "WHACK-A-MOLE", "ARROWS/WASD: MOVE  OK: HIT",
 *   "WHACK THE MOLE, SCORE BIG", "60 SECONDS ROUND", "P: PAUSE  N: NEW"
 */
#include "game.h"
#include "../config.h"
#include "../gfx/canvas.h"
#include "../gfx/font.h"
#include "../platform/display.h"
#include "../platform/input.h"
#include "../platform/time.h"
#include "../platform/audio.h"
#include "../platform/led.h"
#include "../rng.h"

/* ---- 布局: 3x3 洞, 格 64x40 ---- */
#define WA_CELL_W 64
#define WA_CELL_H 40
#define WA_GRID_OX ((CCG_W - 3 * WA_CELL_W) / 2)            /* 水平居中 52 */
#define WA_GRID_OY (CCG_HUD_H + (CCG_H - CCG_HUD_H - 3 * WA_CELL_H) / 2) /* 24 */

/* ---- 节奏(tick 100ms) ---- */
#define WA_TICK_MS 100u
#define WA_GAME_MS 60000u        /* 60 秒限时 */
#define WA_MOLE_LIFE 12          /* 地鼠持续 1.2s */
#define WA_NEXT_MIN 8            /* 下次生成间隔 0.8s .. 1.5s */
#define WA_NEXT_RANGE 8          /* +0..0.7s */

static uint8_t wa_cx, wa_cy;     /* 光标洞 0-2 */
static uint8_t wa_mole;          /* 地鼠所在洞 0-8, 9=无 */
static uint8_t wa_ttl;           /* 地鼠剩余 tick */
static uint8_t wa_next;          /* 距下次生成 tick */
static uint32_t wa_elapsed;      /* 已进行 ms */
static uint32_t wa_score;
static bool wa_over;             /* 时间到 */
static bool wa_over_full;        /* 结束全刷只做一次 */
static uint32_t wa_gens;         /* 换局计数(种子混合) */
static rng_t wa_rng;

void whack_render(void);

/* 手写数字追加(无 snprintf 依赖) */
static void wa_append_u32(char *buf, unsigned *n, uint32_t v, unsigned cap) {
    char tmp[12];
    unsigned len = 0;
    if (v == 0) tmp[len++] = '0';
    while (v && len < 11) { tmp[len++] = (char)('0' + v % 10u); v /= 10u; }
    while (len && *n < cap) buf[(*n)++] = tmp[--len];
}

/* 生成地鼠: 不与当前洞相同(带 guard 防死循环) */
static void wa_spawn(void) {
    uint8_t n;
    int guard = 0;
    do {
        n = (uint8_t)rng_range(&wa_rng, 9u);
    } while (wa_mole != 9 && n == wa_mole && ++guard < 16);
    wa_mole = n;
    wa_ttl = WA_MOLE_LIFE;
}

static void wa_new_game(void) {
    wa_gens++;
    rng_seed(&wa_rng, (uint64_t)now_ms() ^ ((uint64_t)wa_gens * 0x9E3779B1u));
    wa_cx = 1;
    wa_cy = 1;
    wa_mole = 9;
    wa_ttl = 0;
    wa_next = (uint8_t)(WA_NEXT_MIN + rng_range(&wa_rng, WA_NEXT_RANGE));
    wa_elapsed = 0;
    wa_score = 0;
    wa_over = false;
    wa_over_full = false;
    whack_render();
    disp_full();
}

void whack_enter(void) { wa_new_game(); }

void whack_tick(uint64_t now) {
    (void)now;
    if (wa_over) return;
    wa_elapsed += WA_TICK_MS;
    if (wa_elapsed >= WA_GAME_MS) {
        wa_over = true;          /* 时间到 → 结算 */
        wa_mole = 9;
        wa_ttl = 0;
        audio_lose();
        led_fx_set(LED_FX_LOSE);
        return;
    }
    if (wa_ttl > 0) {
        wa_ttl--;
        if (wa_ttl == 0) wa_mole = 9;   /* 地鼠缩回 */
    }
    if (wa_next > 0) wa_next--;
    if (wa_next == 0) {
        wa_spawn();
        wa_next = (uint8_t)(WA_NEXT_MIN + rng_range(&wa_rng, WA_NEXT_RANGE));
    }
}

/* OK 击打: 当前洞有地鼠 → +1 分并立即消失 */
static void wa_hit(void) {
    if (wa_over) return;
    uint8_t i = (uint8_t)(wa_cy * 3 + wa_cx);
    if (wa_mole == i && wa_ttl > 0) {
        wa_score++;
        wa_mole = 9;
        wa_ttl = 0;
        audio_clear();
    } else {
        audio_error();
    }
}

/* CG_MINE 符号 3x 放大(24x21), 作地鼠 */
static void wa_draw_mole(int ox, int oy) {
    for (int j = 0; j < 7; j++)
        for (int i = 0; i < 8; i++)
            if (font_symbols[CG_MINE][j] & (1u << i))
                fb_fill_rect(ox + i * 3, oy + j * 3, 3, 3, true);
}

void whack_render(void) {
    fb_clear(false);
    /* 3x3 洞: 白底 + 底部黑洞; 有地鼠则上方画符号 */
    for (int y = 0; y < 3; y++) {
        for (int x = 0; x < 3; x++) {
            int cx = WA_GRID_OX + x * WA_CELL_W;
            int cy = WA_GRID_OY + y * WA_CELL_H;
            /* 洞(底部深色条) */
            fb_fill_rect(cx + 19, cy + 29, 26, 8, true);
            if (wa_mole == (uint8_t)(y * 3 + x))
                wa_draw_mole(cx + 20, cy + 8);   /* 24x21, 悬于洞上方 */
        }
    }
    /* 格线 */
    for (int i = 1; i < 3; i++) {
        fb_vline(WA_GRID_OX + i * WA_CELL_W, WA_GRID_OY, 3 * WA_CELL_H, true);
        fb_hline(WA_GRID_OX, WA_GRID_OY + i * WA_CELL_H, 3 * WA_CELL_W, true);
    }
    /* 光标: 全格画完后画(四周对称双层对比边框) */
    {
        int cx = WA_GRID_OX + (int)wa_cx * WA_CELL_W;
        int cy = WA_GRID_OY + (int)wa_cy * WA_CELL_H;
        fb_stroke_rect_thick(cx - 2, cy - 2, WA_CELL_W + 4, WA_CELL_H + 4, 2, true);
        fb_stroke_rect(cx - 1, cy - 1, WA_CELL_W + 2, WA_CELL_H + 2, false);
    }
    /* HUD 顶栏 */
    if (wa_over) {
        fb_fill_rect(0, 0, CCG_W, CCG_HUD_H - 1, false);
        fb_text(2, 2, "SCORE", true);
        char sv[8];
        unsigned sn = 0;
        wa_append_u32(sv, &sn, wa_score, 7);
        sv[sn] = 0;
        fb_text(2 + text_width("SCORE"), 2, sv, true);
        fb_text(CCG_W - 2 - text_width("OK/N:RETRY BACK:QUIT"), 2,
                "OK/N:RETRY BACK:QUIT", true);
        if (!wa_over_full) { wa_over_full = true; disp_force_full(); }
    } else {
        fb_text(0, 0, "WHACK", true);
        char buf[28];
        unsigned n = 0;
        const char *p = "SCORE ";
        while (*p && n < 26) buf[n++] = *p++;
        wa_append_u32(buf, &n, wa_score, 26);
        p = " TIME ";
        while (*p && n < 26) buf[n++] = *p++;
        wa_append_u32(buf, &n, (WA_GAME_MS - wa_elapsed) / 1000u, 26);
        buf[n] = 0;
        fb_text(CCG_W - 4 - text_width(buf), 0, buf, true);
        fb_hline(0, CCG_HUD_H - 1, CCG_W, true);
    }
}

void whack_on_key(const key_event_t *ev) {
    bool dir = ev->key == K_UP || ev->key == K_DOWN ||
               ev->key == K_LEFT || ev->key == K_RIGHT;
    if (ev->is_repeat && !dir) return;   /* 确认键/字母忽略重复, 方向可重复 */
    if (wa_over) {
        if (ev->key == K_OK || (ev->key == K_CHAR && ev->ch == 'n'))
            whack_enter();
        else if (ev->key == K_BACK || ev->key == K_QUIT)
            s_exit_request = true;
        return;
    }
    switch (ev->key) {
    case K_UP: if (wa_cy > 0) wa_cy--; audio_tick(); break;
    case K_DOWN: if (wa_cy < 2) wa_cy++; audio_tick(); break;
    case K_LEFT: if (wa_cx > 0) wa_cx--; audio_tick(); break;
    case K_RIGHT: if (wa_cx < 2) wa_cx++; audio_tick(); break;
    case K_CHAR:
        switch (ev->ch) {
        case 'w': if (wa_cy > 0) wa_cy--; audio_tick(); break;
        case 's': if (wa_cy < 2) wa_cy++; audio_tick(); break;
        case 'a': if (wa_cx > 0) wa_cx--; audio_tick(); break;
        case 'd': if (wa_cx < 2) wa_cx++; audio_tick(); break;
        case 'n': audio_select(); whack_enter(); break;
        default: break;
        }
        break;
    case K_OK:
        wa_hit();
        break;
    case K_BACK:
    case K_PAUSE: {
        pause_sel_t sel;
        if (ui_pause_run(&sel)) {
            if (sel == PAUSE_RESTART) whack_enter();
        } else {
            s_exit_request = true;
        }
        break;
    }
    case K_QUIT: s_exit_request = true; break;
    default: break;
    }
}

void whack_exit(void) {}
