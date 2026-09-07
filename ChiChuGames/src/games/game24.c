/* 24 GAME — 选牌合并版
 * 4 张牌(1-13), 选两张 + 运算符(+ - * /) 合并成一张新牌
 * 除法仅允许整除(结果非整或除零 → 提示并退回选牌)
 * 剩 1 张且 =24 → WIN; 剩 1 张非 24 → FAIL(N 换牌)
 * 输入驱动(无 tick): 方向/OK 选牌选运算, BACK 取消(选牌阶段=暂停), P 暂停, N 换牌
 * 建议帮助页(集成到 games_table.c, 本文件不持有 desc):
 *   { "24 GAME", "MAKE 24 FROM 4 CARDS", "OK: SELECT TWO CARDS",
 *     "THEN PICK + - * /", "DIV: WHOLE ONLY  N: NEW", NULL }
 */
#include "game.h"
#include "../config.h"
#include "../gfx/canvas.h"
#include "../gfx/font.h"
#include "../platform/display.h"
#include "../platform/input.h"
#include "../platform/audio.h"
#include "../platform/led.h"
#include "../platform/time.h"
#include "../rng.h"
#include <stdio.h>

void game24_render(void);   /* enter 前向声明 */

#define T4_CARD_W 64
#define T4_CARD_H 80
#define T4_CARD_G 8
#define T4_OX ((CCG_W - (4 * T4_CARD_W + 3 * T4_CARD_G)) / 2)
#define T4_OY 24
#define T4_OP_W 40
#define T4_OP_H 18
#define T4_OP_G 12
#define T4_OP_OX ((CCG_W - (4 * T4_OP_W + 3 * T4_OP_G)) / 2)
#define T4_OP_OY 112
#define T4_STATUS_Y (T4_OP_OY + T4_OP_H + 4)
#define T4_MAX 8          /* 牌位容量(实际 ≤4) */

typedef enum { T4_PICK, T4_OP, T4_OVER } t4_phase_t;

static int t4_cards[T4_MAX];        /* 当前牌(0..n-1) */
static int t4_n;                    /* 剩余牌数 */
static int t4_cx;                   /* 牌光标 0..3 */
static int t4_sel[2];               /* 选中的牌下标, -1=无 */
static int t4_seln;                 /* 已选张数 0..2 */
static t4_phase_t t4_phase;
static int t4_opc;                  /* 运算符光标 0..3 */
static bool t4_over, t4_won, t4_over_full;
static const char *t4_msg;          /* 底部提示, NULL=默认指引 */
static rng_t t4_rng;
static const char t4_ops[4] = { '+', '-', '*', '/' };

/* 对两张牌做运算: 成功返回 true, *res 存结果
 * 除法: 除零或不能整除 → false(该步无效) */
static bool t4_merge(int a, int b, int op, int *res) {
    int x = t4_cards[a], y = t4_cards[b];
    switch (op) {
    case 0: *res = x + y; return true;
    case 1: *res = x - y; return true;
    case 2: *res = x * y; return true;
    default:
        if (y == 0 || x % y != 0) return false;
        *res = x / y;
        return true;
    }
}

/* 合并落盘: 结果写 a, 移除 b, 左移补齐 */
static void t4_apply(int a, int b, int res) {
    t4_cards[a] = res;
    for (int i = b; i < t4_n - 1; i++) t4_cards[i] = t4_cards[i + 1];
    t4_n--;
    t4_seln = 0;
    t4_sel[0] = t4_sel[1] = -1;
    t4_phase = T4_PICK;
    t4_msg = NULL;
    if (t4_cx > t4_n - 1) t4_cx = t4_n - 1;
    if (t4_n == 1) {              /* 剩 1 张定胜负 */
        t4_over = true;
        t4_won = (t4_cards[0] == 24);
        if (t4_won) {
            audio_win();
            led_fx_set(LED_FX_WIN);
        } else {
            audio_lose();
            led_fx_set(LED_FX_LOSE);
        }
    }
}

/* 选中/取消一张牌; 选满 2 张自动进入运算阶段 */
static void t4_toggle_sel(int i) {
    if (i < 0 || i >= t4_n) return;
    for (int k = 0; k < t4_seln; k++) {
        if (t4_sel[k] == i) {            /* 取消 */
            for (int j = k; j < t4_seln - 1; j++) t4_sel[j] = t4_sel[j + 1];
            t4_seln--;
            t4_sel[t4_seln] = -1;
            if (t4_phase == T4_OP) t4_phase = T4_PICK;
            return;
        }
    }
    if (t4_seln < 2) {
        t4_sel[t4_seln++] = i;
    } else {
        t4_sel[1] = i;                   /* 已满: 替换最后一张 */
    }
    audio_select();
    if (t4_seln == 2 && t4_phase == T4_PICK) {
        t4_phase = T4_OP;
        t4_opc = 0;
    }
}

static void t4_clear_sel(void) {
    t4_sel[0] = t4_sel[1] = -1;
    t4_seln = 0;
    if (t4_phase == T4_OP) t4_phase = T4_PICK;
    t4_msg = NULL;
}

static void t4_cursor_left(void) {
    t4_msg = NULL;
    if (t4_phase == T4_PICK) {
        if (t4_cx > 0) t4_cx--;
    } else if (t4_phase == T4_OP) {
        if (t4_opc > 0) t4_opc--;
    }
}

static void t4_cursor_right(void) {
    t4_msg = NULL;
    if (t4_phase == T4_PICK) {
        if (t4_cx < t4_n - 1) t4_cx++;
    } else if (t4_phase == T4_OP) {
        if (t4_opc < 3) t4_opc++;
    }
}

static void t4_deal(void) {
    for (int i = 0; i < 4; i++)
        t4_cards[i] = (int)rng_range(&t4_rng, 13) + 1;
    t4_n = 4;
    t4_cx = 0;
    t4_seln = 0;
    t4_sel[0] = t4_sel[1] = -1;
    t4_phase = T4_PICK;
    t4_opc = 0;
    t4_over = false;
    t4_won = false;
    t4_over_full = false;
    t4_msg = NULL;
}

void game24_enter(void) {
    rng_seed(&t4_rng, now_ms() ^ 0x24A7u);
    t4_deal();
    game24_render();
    disp_full();
}

void game24_tick(uint64_t now) { (void)now; }

void game24_exit(void) {}

/* ---- 渲染 ---- */

static bool t4_is_sel(int i) {
    for (int k = 0; k < t4_seln; k++)
        if (t4_sel[k] == i) return true;
    return false;
}

static void t4_draw_card(int x, int y, int val, bool sel) {
    char buf[12];
    snprintf(buf, sizeof(buf), "%d", val);
    int tw = text_width(buf) * 2;               /* scale2: 每字符 12px */
    int tx = x + (T4_CARD_W - tw) / 2;
    int ty = y + (T4_CARD_H - 14) / 2;
    if (sel) {
        fb_fill_rect(x, y, T4_CARD_W, T4_CARD_H, true);
        fb_text_scale2(tx, ty, buf, false);
    } else {
        fb_stroke_rect(x, y, T4_CARD_W, T4_CARD_H, true);
        fb_text_scale2(tx, ty, buf, true);
    }
}

/* 光标: 外圈 1px 反色 + 内圈 2px 同色, 黑/白底均可见 */
static void t4_draw_cursor(int ci) {
    int x = T4_OX + ci * (T4_CARD_W + T4_CARD_G);
    bool bg = t4_is_sel(ci);
    fb_stroke_rect(x - 2, T4_OY - 2, T4_CARD_W + 4, T4_CARD_H + 4, !bg);
    fb_stroke_rect_thick(x - 1, T4_OY - 1, T4_CARD_W + 2, T4_CARD_H + 2, 2, bg);
}

static void t4_draw_ops(void) {
    for (int i = 0; i < 4; i++) {
        int x = T4_OP_OX + i * (T4_OP_W + T4_OP_G);
        char c[2] = { t4_ops[i], '\0' };
        int tx = x + (T4_OP_W - text_width(c)) / 2;
        int ty = T4_OP_OY + (T4_OP_H - FONT_H) / 2;
        if (t4_phase == T4_OP && t4_opc == i) {
            fb_fill_rect(x, T4_OP_OY, T4_OP_W, T4_OP_H, true);
            fb_text(tx, ty, c, false);
            fb_stroke_rect(x - 2, T4_OP_OY - 2, T4_OP_W + 4, T4_OP_H + 4, false);
            fb_stroke_rect_thick(x - 1, T4_OP_OY - 1, T4_OP_W + 2, T4_OP_H + 2, 2, true);
        } else {
            fb_stroke_rect(x, T4_OP_OY, T4_OP_W, T4_OP_H, true);
            fb_text(tx, ty, c, true);
        }
    }
}

static void t4_draw_status(void) {
    const char *s;
    if (t4_msg) s = t4_msg;
    else if (t4_phase == T4_OP) s = "PICK OP FOR SELECTED";
    else if (t4_seln == 1) s = "SELECT ONE MORE CARD";
    else s = "SELECT TWO CARDS";
    fb_text(2, T4_STATUS_Y, s, true);
    fb_text(CCG_W - 2 - text_width("N:NEW"), T4_STATUS_Y, "N:NEW", true);
}

static void t4_draw_hud(void) {
    char buf[24];
    snprintf(buf, sizeof(buf), "CARDS %d", t4_n);
    fb_text(CCG_W - text_width(buf) - 4, 0, buf, true);
    fb_text(0, 0, "24 GAME", true);
    fb_hline(0, CCG_HUD_H - 1, CCG_W, true);
}

void game24_render(void) {
    fb_clear(false);
    for (int i = 0; i < 4; i++) {
        if (i >= t4_n) continue;
        t4_draw_card(T4_OX + i * (T4_CARD_W + T4_CARD_G), T4_OY,
                     t4_cards[i], t4_is_sel(i));
    }
    if (!t4_over) {
        t4_draw_ops();
        if (t4_phase == T4_PICK) t4_draw_cursor(t4_cx);
        t4_draw_status();
    }
    if (t4_over) {
        /* HUD 两行: 左上结果 + 右上按键提示; 墙内不放提示 */
        fb_fill_rect(0, 0, CCG_W, CCG_HUD_H - 1, false);
        fb_text(2, 2, t4_won ? "YOU WIN!" : "NO MOVES", true);
        fb_text(CCG_W - 2 - text_width("OK/N:RETRY BACK:QUIT"), 2,
                "OK/N:RETRY BACK:QUIT", true);
        if (!t4_over_full) { t4_over_full = true; disp_force_full(); }
    } else {
        t4_draw_hud();
    }
}

/* ---- 输入 ---- */

void game24_on_key(const key_event_t *ev) {
    if (ev->is_repeat) {
        /* 方向键重复可响应; 确认键/字母一律忽略重复 */
        if (!t4_over && (ev->key == K_LEFT || ev->key == K_RIGHT)) {
            if (ev->key == K_LEFT) t4_cursor_left();
            else t4_cursor_right();
        }
        return;
    }
    if (t4_over) {
        if (ev->key == K_OK || (ev->key == K_CHAR && ev->ch == 'n'))
            game24_enter();
        else if (ev->key == K_BACK || ev->key == K_QUIT)
            s_exit_request = true;
        return;
    }
    switch (ev->key) {
    case K_LEFT:  t4_cursor_left();  break;
    case K_RIGHT: t4_cursor_right(); break;
    case K_OK:
        if (t4_phase == T4_PICK) {
            t4_msg = NULL;
            t4_toggle_sel(t4_cx);
        } else if (t4_phase == T4_OP) {
            int a = t4_sel[0], b = t4_sel[1];
            if (a >= 0 && b >= 0 && a != b) {
                int res;
                if (t4_merge(a, b, t4_opc, &res)) {
                    t4_apply(a, b, res);
                } else {
                    /* 除法非整除/除零: 该步无效, 提示并退回选牌 */
                    t4_msg = (t4_opc == 3) ? "DIV: WHOLE RESULT ONLY"
                                           : "BAD OP";
                    t4_phase = T4_PICK;
                    audio_error();
                }
            }
        }
        break;
    case K_BACK:
        if (t4_phase == T4_OP) {
            t4_phase = T4_PICK;      /* 运算阶段 BACK = 取消, 保留选择 */
            t4_msg = NULL;
        } else {
            pause_sel_t sel;
            if (ui_pause_run(&sel)) {
                if (sel == PAUSE_RESTART) game24_enter();
            } else {
                s_exit_request = true;
            }
        }
        break;
    case K_PAUSE: {
        pause_sel_t sel;
        if (ui_pause_run(&sel)) {
            if (sel == PAUSE_RESTART) game24_enter();
        } else {
            s_exit_request = true;
        }
        break;
    }
    case K_DEL:
        t4_clear_sel();
        break;
    case K_QUIT:
        s_exit_request = true;
        break;
    case K_CHAR:
        if (ev->ch == 'a') t4_cursor_left();
        else if (ev->ch == 'd') t4_cursor_right();
        else if (ev->ch == 'n') game24_enter();
        else if (ev->ch == 'x') t4_clear_sel();
        else if (ev->ch >= '1' && ev->ch <= '4')
            t4_toggle_sel((int)(ev->ch - '1'));
        break;
    default:
        break;
    }
}
