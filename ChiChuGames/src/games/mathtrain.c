/* MATH TRAINER — 心算训练: 60 秒限时四则运算
 * 数字键输入答案, OK 提交: 对=+10 分, 错=显示正确答案; N 换题/新局
 * 等级制: 答对 10 题升一级(难度递增):
 *   LV1 加减小值(2..10) -> LV2 加大值(10..99) -> LV3 乘法 -> LV4 除法整除
 * HUD 顶栏: 左 MATH, 右 TIME + SCORE; 结束: 左上结果 + 右上 OK/N:RETRY BACK:QUIT
 * 题面与答案 3x 放大(15x21 大字), 光标为输入位反显块
 * 静态前缀 mt_; 像素坐标一律 int; 零 malloc; 全部按键忽略重复
 *
 * 集成提示(help[] 最多 5 行):
 *   "MATH TRAINER", "TYPE ANSWER, OK TO SUBMIT",
 *   "CORRECT +10, 10 IN A ROW LEVEL UP", "60 SECONDS ROUND",
 *   "DEL: BACKSPACE  N: SKIP", NULL
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
#include <string.h>

void mathtrain_render(void);

/* ---- 节奏(tick 100ms)与限时 ---- */
#define MT_TICK_MS 100u
#define MT_GAME_MS 60000u
#define MT_LEVEL_UP 10          /* 答对 10 题升级 */
#define MT_MAX_LVL 4            /* 最高难度等级 */
#define MT_ANS_CAP 3            /* 答案最多 3 位(99+99=198) */
#define MT_ANS_BUF 4

/* ---- 布局(游戏区 y>=16) ---- */
#define MT_EXPR_Y 26            /* 题面 3x(21px 高) */
#define MT_ANS_Y 56             /* 答案行 3x */
#define MT_FB_Y 88              /* 反馈行(1x) */
#define MT_BAR_Y 108            /* 限时进度条 */
#define MT_BAR_H 6
#define MT_STAT_Y 128           /* 状态行(1x) */

static int mt_lvl;               /* 难度等级 1..MT_MAX_LVL */
static int mt_op;                /* 0+ 1- 2* 3/ */
static int mt_a, mt_b, mt_sol;   /* a OP b = sol */
static int mt_last_op, mt_last_sol; /* 上一题(防连续重复) */
static char mt_ans[MT_ANS_BUF];  /* 已输入答案数字('0'-'9') */
static int mt_ansn;              /* 已输入位数 */
static int mt_score;
static int mt_right;             /* 本局累计答对(等级依据) */
static uint32_t mt_elapsed;      /* 已进行 ms */
static bool mt_over;
static bool mt_over_full;        /* 结束全刷只做一次 */
static char mt_fb[28];           /* 反馈行(最后一次判定) */
static uint32_t mt_gens;         /* 换局计数(种子混合) */
static rng_t mt_rng;

/* 生成一题(难度按当前等级); do-while 防与上一题重复, guard 防死循环 */
static void mt_make_q(void) {
    int op = 0, a = 0, b = 0, sol = 0;
    int guard = 0;
    do {
        switch (mt_lvl) {
        case 1:                                /* 加减小值 2..10 */
            op = (int)rng_range(&mt_rng, 2u);
            a = (int)rng_range(&mt_rng, 9u) + 2;
            b = (int)rng_range(&mt_rng, 9u) + 2;
            break;
        case 2:                                /* 加大值 10..99 */
            op = (int)rng_range(&mt_rng, 2u);
            a = (int)rng_range(&mt_rng, 90u) + 10;
            b = (int)rng_range(&mt_rng, 90u) + 10;
            break;
        case 3:                                /* 乘法 2..9 x 2..9 */
            op = 2;
            a = (int)rng_range(&mt_rng, 8u) + 2;
            b = (int)rng_range(&mt_rng, 8u) + 2;
            break;
        default:                               /* 除法整除: 被除数=商*除数 */
            op = 3;
            b = (int)rng_range(&mt_rng, 8u) + 2;
            a = ((int)rng_range(&mt_rng, 8u) + 2) * b;
            break;
        }
        if (op == 1 && a < b) { int t = a; a = b; b = t; }  /* 差非负 */
        if (op == 0) sol = a + b;
        else if (op == 1) sol = a - b;
        else if (op == 2) sol = a * b;
        else sol = a / b;
    } while (op == mt_last_op && sol == mt_last_sol && ++guard < 8);
    mt_op = op;
    mt_a = a;
    mt_b = b;
    mt_sol = sol;
    mt_last_op = op;
    mt_last_sol = sol;
}

/* 提交答案: 对=+10 且答对数+1(每 10 题升级), 错=显示正确答案 */
static void mt_submit(void) {
    if (mt_ansn <= 0) return;                  /* 未输入, 忽略 */
    int v = 0;
    for (int i = 0; i < mt_ansn; i++)
        v = v * 10 + (mt_ans[i] - '0');
    int old_lvl = mt_lvl;
    if (v == mt_sol) {
        mt_score += 10;
        mt_right++;
        mt_lvl = mt_right / MT_LEVEL_UP + 1;
        if (mt_lvl > MT_MAX_LVL) mt_lvl = MT_MAX_LVL;
        if (mt_lvl != old_lvl)
            snprintf(mt_fb, sizeof(mt_fb), "LV UP! NOW LV %d", mt_lvl);
        else
            snprintf(mt_fb, sizeof(mt_fb), "OK +10");
        audio_clear();
    } else {
        snprintf(mt_fb, sizeof(mt_fb), "WRONG ANS %d", mt_sol);
        audio_error();
    }
    mt_ansn = 0;
    mt_make_q();
}

static void mt_new_game(void) {
    mt_gens++;
    rng_seed(&mt_rng, now_ms() ^ ((uint64_t)mt_gens * 0x9E3779B1u));
    mt_lvl = 1;
    mt_score = 0;
    mt_right = 0;
    mt_elapsed = 0;
    mt_ansn = 0;
    mt_over = false;
    mt_over_full = false;
    mt_fb[0] = 0;
    mt_last_op = -1;
    mt_last_sol = -1;
    mt_make_q();
    mathtrain_render();
    disp_full();
}

void mathtrain_enter(void) { mt_new_game(); }

void mathtrain_tick(uint64_t now) {
    (void)now;
    if (mt_over) return;
    mt_elapsed += MT_TICK_MS;
    if (mt_elapsed >= MT_GAME_MS) {
        mt_elapsed = MT_GAME_MS;
        mt_over = true;
        mt_ansn = 0;
        audio_lose();
        led_fx_set(LED_FX_LOSE);
    }
}

/* ---- 渲染 ---- */

/* 5x7 字形 3x 放大(每字符 15x21, 间距 3px; 字形 bit0=最左) */
static void mt_text3(int x, int y, const char *s, bool black) {
    for (const char *p = s; *p; p++) {
        const uint8_t *g = font_glyph5x7[(unsigned char)*p];
        for (int j = 0; j < FONT_H; j++)
            for (int i = 0; i < FONT_W; i++)
                if (g[j] & (1u << i))
                    fb_fill_rect(x + i * 3, y + j * 3, 3, 3, black);
        x += FONT_ADV * 3;
    }
}

/* 居中绘制字符串(3x), 返回起始 x */
static int mt_text3_center(int y, const char *s, bool black) {
    int w = ((int)strlen(s) * FONT_ADV - 1) * 3;
    int x = (CCG_W - w) / 2;
    if (x < 0) x = 0;
    mt_text3(x, y, s, black);
    return x;
}

void mathtrain_render(void) {
    fb_clear(false);
    char buf[32];

    if (mt_over) {
        /* HUD 两行: 左上结果 + 右上按键提示; 墙内只放结果展示 */
        fb_fill_rect(0, 0, CCG_W, CCG_HUD_H - 1, false);
        snprintf(buf, sizeof(buf), "SCORE %d", mt_score);
        fb_text(2, 2, buf, true);
        snprintf(buf, sizeof(buf), "RIGHT %d LV %d", mt_right, mt_lvl);
        fb_text(2, 9, buf, true);
        fb_text(CCG_W - 2 - text_width("OK/N:RETRY BACK:QUIT"), 2,
                "OK/N:RETRY BACK:QUIT", true);
        /* 墙内: 最终成绩大字 */
        mt_text3_center(MT_EXPR_Y, "FINAL SCORE", true);
        snprintf(buf, sizeof(buf), "%d", mt_score);
        mt_text3_center(MT_ANS_Y, buf, true);
        if (!mt_over_full) { mt_over_full = true; disp_force_full(); }
        return;
    }

    /* 题面: "a + b =" 3x 居中 */
    static const char mt_opch[4] = { '+', '-', '*', '/' };
    snprintf(buf, sizeof(buf), "%d %c %d =", mt_a, mt_opch[mt_op], mt_b);
    mt_text3_center(MT_EXPR_Y, buf, true);

    /* 答案行: 已输入数字 + 光标块 */
    if (mt_ansn > 0) {
        buf[0] = 0;
        for (int i = 0; i < mt_ansn && i < MT_ANS_BUF; i++) {
            char c[2] = { mt_ans[i], 0 };
            strncat(buf, c, sizeof(buf) - strlen(buf) - 1);
        }
        mt_text3_center(MT_ANS_Y, buf, true);
    }
    {
        int w = (mt_ansn * FONT_ADV - 1) * 3;   /* 已输入宽度 */
        int x = (CCG_W - w) / 2;
        if (x < 0) x = 0;
        fb_fill_rect(x + mt_ansn * FONT_ADV * 3 - 3, MT_ANS_Y, 3, FONT_H * 3, true);
    }

    /* 反馈行(最后一次判定) */
    if (mt_fb[0])
        fb_text(4, MT_FB_Y, mt_fb, true);

    /* 限时进度条(满格=60s) */
    fb_stroke_rect(2, MT_BAR_Y, CCG_W - 4, MT_BAR_H, true);
    {
        uint32_t rem = MT_GAME_MS - mt_elapsed;
        uint32_t bw = (CCG_W - 6u) * rem / MT_GAME_MS;
        if (bw > 0) fb_fill_rect(3, MT_BAR_Y + 1, (int)bw, MT_BAR_H - 2, true);
    }

    /* 状态行 */
    snprintf(buf, sizeof(buf), "LV %d RIGHT %d", mt_lvl, mt_right);
    fb_text(2, MT_STAT_Y, buf, true);
    fb_text(CCG_W - 4 - text_width("OK:GO N:SKIP DEL:BS"), MT_STAT_Y,
            "OK:GO N:SKIP DEL:BS", true);

    /* HUD 顶栏: 左 MATH, 右 TIME + SCORE */
    fb_text(0, 0, "MATH", true);
    snprintf(buf, sizeof(buf), "TIME %u SCORE %d",
             (MT_GAME_MS - mt_elapsed) / 1000u, mt_score);
    fb_text(CCG_W - 4 - text_width(buf), 0, buf, true);
    fb_hline(0, CCG_HUD_H - 1, CCG_W, true);
}

/* ---- 输入 ---- */

void mathtrain_on_key(const key_event_t *ev) {
    if (ev->is_repeat) return;                 /* 全部按键忽略重复 */
    if (mt_over) {
        if (ev->key == K_OK || (ev->key == K_CHAR && ev->ch == 'n'))
            mt_new_game();
        else if (ev->key == K_BACK || ev->key == K_QUIT)
            s_exit_request = true;
        return;
    }
    if (ev->key == K_CHAR) {
        if (ev->ch >= '0' && ev->ch <= '9') {
            if (mt_ansn < MT_ANS_CAP) { mt_ans[mt_ansn++] = (char)ev->ch; audio_tick(); }
        } else if (ev->ch == 'n') {
            audio_select();
            mt_ansn = 0;                       /* N: 跳过换题 */
            mt_make_q();
        }
        return;
    }
    switch (ev->key) {
    case K_OK:
        mt_submit();
        break;
    case K_DEL:
        if (mt_ansn > 0) { mt_ansn--; audio_move(); }
        break;
    case K_BACK:
    case K_PAUSE: {
        pause_sel_t sel;
        if (ui_pause_run(&sel)) {
            if (sel == PAUSE_RESTART) mt_new_game();
        } else {
            s_exit_request = true;
        }
        break;
    }
    case K_QUIT:
        s_exit_request = true;
        break;
    default:
        break;
    }
}

void mathtrain_exit(void) {}
