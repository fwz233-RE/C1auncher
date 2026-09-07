/* TIMES TRAINER — 乘法表训练: 60 秒限时快速乘法
 * 题目 2-9 随机乘法; 数字键输入答案, OK 提交
 * 答对 +5 连击加分(连击每加一档 +2, 封顶 +10), 答错显示正确答案
 * 等级制: 答对 10 题升一级, 数字范围变大:
 *   LV1 2-5 x 2-5 -> LV2 2-9 x 2-5 -> LV3 2-9 x 2-9 -> LV4(顶) 3-9 x 3-9
 * HUD 顶栏: 左 TIMES, 右 TIME + SCORE; 结束: 左上结果 + 右上 OK/N:RETRY BACK:QUIT
 * 题面与答案 3x 放大(15x21 大字), 光标为输入位反显块
 * 静态前缀 tt_; 像素坐标一律 int; 零 malloc; 全部按键忽略重复
 *
 * 集成提示(help[] 最多 5 行):
 *   "TIMES TRAINER", "QUICK MULTIPLICATION",
 *   "TYPE ANSWER, OK TO SUBMIT",
 *   "+5 PER RIGHT +COMBO, 10 RIGHT LEVEL UP",
 *   "60 SEC: DEL:BS  N:SKIP  P:PAUSE", NULL
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
#include <stdio.h>
#include <string.h>

void timestrain_render(void);

/* ---- 节奏(tick 100ms)与限时 ---- */
#define TT_TICK_MS 100u
#define TT_GAME_MS 60000u
#define TT_LEVEL_UP 10          /* 答对 10 题升级 */
#define TT_MAX_LVL 4            /* 最高难度等级 */
#define TT_ANS_CAP 2            /* 答案最多 2 位(9*9=81) */
#define TT_ANS_BUF 3
#define TT_COMBO_MAX 6          /* 连击档位封顶(封顶后加分固定 +10) */

/* ---- 布局(游戏区 y>=16) ---- */
#define TT_EXPR_Y 30            /* 题面 3x(21px 高) */
#define TT_ANS_Y 60             /* 答案行 3x */
#define TT_FB_Y 92              /* 反馈行(1x) */
#define TT_BAR_Y 112            /* 限时进度条 */
#define TT_BAR_H 6
#define TT_STAT_Y 130           /* 状态行(1x) */

static int tt_lvl;               /* 难度等级 1..TT_MAX_LVL */
static int tt_a, tt_b;           /* a x b */
static int tt_last_a, tt_last_b; /* 上一题(防连续重复) */
static char tt_ans[TT_ANS_BUF];  /* 已输入答案数字('0'-'9') */
static int tt_ansn;              /* 已输入位数 */
static int tt_score;
static int tt_right;             /* 本局累计答对(等级依据) */
static int tt_combo;             /* 当前连击(连续答对) */
static int tt_max_combo;         /* 本局最高连击 */
static uint32_t tt_elapsed;      /* 已进行 ms */
static bool tt_over;
static bool tt_over_full;        /* 结束全刷只做一次 */
static char tt_fb[40];           /* 反馈行(最后一次判定) */
static uint32_t tt_gens;         /* 换局计数(种子混合) */
static rng_t tt_rng;

/* 本局最高连击(结束页展示) */
static int tt_combo_peak(void) { return tt_max_combo; }

/* 连击加分: 连击第 1 题 0, 之后每档 +2, 封顶 +10 */
static int tt_combo_bonus(int combo) {
    if (combo <= 1) return 0;
    int b = (combo - 1) * 2;
    if (b > 10) b = 10;
    return b;
}

/* 生成一题(难度按当前等级); do-while 防与上一题重复, guard 防死循环 */
static void tt_make_q(void) {
    int a = 2, b = 2;
    int guard = 0;
    do {
        switch (tt_lvl) {
        case 1:                                /* 2-5 x 2-5 */
            a = (int)rng_range(&tt_rng, 4u) + 2;
            b = (int)rng_range(&tt_rng, 4u) + 2;
            break;
        case 2:                                /* 2-9 x 2-5 */
            a = (int)rng_range(&tt_rng, 8u) + 2;
            b = (int)rng_range(&tt_rng, 4u) + 2;
            break;
        case 3:                                /* 2-9 x 2-9 */
            a = (int)rng_range(&tt_rng, 8u) + 2;
            b = (int)rng_range(&tt_rng, 8u) + 2;
            break;
        default:                               /* LV4 顶: 3-9 x 3-9 */
            a = (int)rng_range(&tt_rng, 7u) + 3;
            b = (int)rng_range(&tt_rng, 7u) + 3;
            break;
        }
    } while (a == tt_last_a && b == tt_last_b && ++guard < 8);
    tt_a = a;
    tt_b = b;
    tt_last_a = a;
    tt_last_b = b;
}

/* 提交答案: 对=+5+连击加分且答对数+1(每 10 题升级), 错=显示正确答案 */
static void tt_submit(void) {
    if (tt_ansn <= 0) return;                  /* 未输入, 忽略 */
    int v = 0;
    for (int i = 0; i < tt_ansn; i++)
        v = v * 10 + (tt_ans[i] - '0');
    int old_lvl = tt_lvl;
    if (v == tt_a * tt_b) {
        tt_combo++;
        if (tt_combo > tt_max_combo) tt_max_combo = tt_combo;
        int gain = 5 + tt_combo_bonus(tt_combo);
        tt_score += gain;
        tt_right++;
        tt_lvl = tt_right / TT_LEVEL_UP + 1;
        if (tt_lvl > TT_MAX_LVL) tt_lvl = TT_MAX_LVL;
        if (tt_lvl != old_lvl)
            snprintf(tt_fb, sizeof(tt_fb), "RIGHT +%d LV UP! LV %d",
                     gain, tt_lvl);
        else
            snprintf(tt_fb, sizeof(tt_fb), "RIGHT +%d", gain);
        audio_clear();
    } else {
        tt_combo = 0;                          /* 连击中断 */
        snprintf(tt_fb, sizeof(tt_fb), "WRONG %dx%d=%d",
                 tt_a, tt_b, tt_a * tt_b);
        audio_error();
    }
    tt_ansn = 0;
    tt_make_q();
}

static void tt_new_game(void) {
    tt_gens++;
    rng_seed(&tt_rng, now_ms() ^ ((uint64_t)tt_gens * 0x9E3779B1u));
    tt_lvl = 1;
    tt_score = 0;
    tt_right = 0;
    tt_combo = 0;
    tt_max_combo = 0;
    tt_elapsed = 0;
    tt_ansn = 0;
    tt_over = false;
    tt_over_full = false;
    tt_fb[0] = 0;
    tt_last_a = -1;
    tt_last_b = -1;
    tt_make_q();
    timestrain_render();
    disp_full();
}

void timestrain_enter(void) { tt_new_game(); }

void timestrain_tick(uint64_t now) {
    (void)now;
    if (tt_over) return;
    tt_elapsed += TT_TICK_MS;
    if (tt_elapsed >= TT_GAME_MS) {
        tt_elapsed = TT_GAME_MS;
        tt_over = true;
        tt_ansn = 0;
        audio_lose();
        led_fx_set(LED_FX_LOSE);
    }
}

/* ---- 渲染 ---- */

/* 5x7 字形 3x 放大(每字符 15x21, 间距 3px; 字形 bit0=最左) */
static void tt_text3(int x, int y, const char *s, bool black) {
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
static int tt_text3_center(int y, const char *s, bool black) {
    int w = ((int)strlen(s) * FONT_ADV - 1) * 3;
    int x = (CCG_W - w) / 2;
    if (x < 0) x = 0;
    tt_text3(x, y, s, black);
    return x;
}

void timestrain_render(void) {
    fb_clear(false);
    char buf[32];

    if (tt_over) {
        /* HUD 两行: 左上结果 + 右上按键提示; 墙内只放结果展示 */
        fb_fill_rect(0, 0, CCG_W, CCG_HUD_H - 1, false);
        snprintf(buf, sizeof(buf), "SCORE %d", tt_score);
        fb_text(2, 2, buf, true);
        snprintf(buf, sizeof(buf), "RIGHT %d LV %d", tt_right, tt_lvl);
        fb_text(2, 9, buf, true);
        fb_text(CCG_W - 2 - text_width("OK/N:RETRY BACK:QUIT"), 2,
                "OK/N:RETRY BACK:QUIT", true);
        /* 墙内: 最终成绩大字 */
        tt_text3_center(TT_EXPR_Y, "FINAL SCORE", true);
        snprintf(buf, sizeof(buf), "%d", tt_score);
        tt_text3_center(TT_ANS_Y, buf, true);
        snprintf(buf, sizeof(buf), "BEST COMBO x%d", tt_combo_peak());
        fb_text_center(TT_FB_Y, buf, true);
        if (!tt_over_full) { tt_over_full = true; disp_force_full(); }
        return;
    }

    /* 题面: "a x b =" 3x 居中 */
    snprintf(buf, sizeof(buf), "%d x %d =", tt_a, tt_b);
    tt_text3_center(TT_EXPR_Y, buf, true);

    /* 答案行: 已输入数字 + 光标块 */
    if (tt_ansn > 0) {
        buf[0] = 0;
        for (int i = 0; i < tt_ansn && i < TT_ANS_BUF; i++) {
            char c[2] = { tt_ans[i], 0 };
            strncat(buf, c, sizeof(buf) - strlen(buf) - 1);
        }
        tt_text3_center(TT_ANS_Y, buf, true);
    }
    {
        int w = (tt_ansn * FONT_ADV - 1) * 3;   /* 已输入宽度 */
        int x = (CCG_W - w) / 2;
        if (x < 0) x = 0;
        fb_fill_rect(x + tt_ansn * FONT_ADV * 3 - 3, TT_ANS_Y, 3, FONT_H * 3, true);
    }

    /* 反馈行(最后一次判定) */
    if (tt_fb[0])
        fb_text(4, TT_FB_Y, tt_fb, true);

    /* 限时进度条(满格=60s) */
    fb_stroke_rect(2, TT_BAR_Y, CCG_W - 4, TT_BAR_H, true);
    {
        uint32_t rem = TT_GAME_MS - tt_elapsed;
        uint32_t bw = (CCG_W - 6u) * rem / TT_GAME_MS;
        if (bw > 0) fb_fill_rect(3, TT_BAR_Y + 1, (int)bw, TT_BAR_H - 2, true);
    }

    /* 状态行 */
    snprintf(buf, sizeof(buf), "LV %d RIGHT %d COMBO x%d",
             tt_lvl, tt_right, tt_combo);
    fb_text(2, TT_STAT_Y, buf, true);
    fb_text(CCG_W - 4 - text_width("OK:GO N:SKIP DEL:BS"), TT_STAT_Y,
            "OK:GO N:SKIP DEL:BS", true);

    /* HUD 顶栏: 左 TIMES, 右 TIME + SCORE */
    fb_text(0, 0, "TIMES", true);
    snprintf(buf, sizeof(buf), "TIME %u SCORE %d",
             (TT_GAME_MS - tt_elapsed) / 1000u, tt_score);
    fb_text(CCG_W - 4 - text_width(buf), 0, buf, true);
    fb_hline(0, CCG_HUD_H - 1, CCG_W, true);
}

/* ---- 输入 ---- */

void timestrain_on_key(const key_event_t *ev) {
    if (ev->is_repeat) return;                 /* 全部按键忽略重复 */
    if (tt_over) {
        if (ev->key == K_OK || (ev->key == K_CHAR && ev->ch == 'n'))
            tt_new_game();
        else if (ev->key == K_BACK || ev->key == K_QUIT)
            s_exit_request = true;
        return;
    }
    if (ev->key == K_CHAR) {
        if (ev->ch >= '0' && ev->ch <= '9') {
            if (tt_ansn < TT_ANS_CAP) { tt_ans[tt_ansn++] = (char)ev->ch; audio_tick(); }
        } else if (ev->ch == 'n') {
            audio_select();
            tt_ansn = 0;                       /* N: 跳过换题 */
            tt_make_q();
        }
        return;
    }
    switch (ev->key) {
    case K_OK:
        tt_submit();
        break;
    case K_DEL:
        if (tt_ansn > 0) { tt_ansn--; audio_move(); }
        break;
    case K_BACK:
    case K_PAUSE: {
        pause_sel_t sel;
        if (ui_pause_run(&sel)) {
            if (sel == PAUSE_RESTART) tt_new_game();
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

void timestrain_exit(void) {}
