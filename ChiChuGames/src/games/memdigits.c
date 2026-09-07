/* MEMORY DIGITS — 记忆数字串复述游戏(墨水屏版)
 * 显示一串数字(长度 = 等级+2, 3..10 位)1000ms 后消失, 玩家输入复述:
 *   数字键输入, OK 提交, Delete 退格; 输入时逐位即时反馈:
 *     对 = 反白格(黑格白字), 错 = 灰格(棋盘格底), 光标为下一位竖条
 * 等级制: 连续 3 题对升 1 级(数字变长); 错 1 题降 1 级; 2 连错 FAIL
 * 分数 = 正确数 × 当前等级(等级变化时实时重算)
 * HUD: 左 MEMDIGITS 右 LEVEL n SCORE n; 开局/结束 disp_full, 游戏内 disp_fast
 * tick 100ms 驱动 显示→输入→反馈 状态机
 * 静态前缀 md_; 像素坐标一律 int; 零 malloc; 全部按键忽略重复
 *
 * 集成提示(help[] 最多 5 行):
 *   "MEMORY DIGITS", "WATCH THE DIGIT STRING,",
 *   "THEN TYPE IT BACK", "OK SUBMIT  DEL BACKSPACE",
 *   "3 RIGHT = LEVEL UP", NULL
 */
#include "game.h"
#include "../config.h"
#include "../gfx/canvas.h"
#include "../gfx/font.h"
#include "../gfx/pattern.h"
#include "../platform/display.h"
#include "../platform/input.h"
#include "../platform/audio.h"
#include "../platform/led.h"
#include "../platform/time.h"
#include "../rng.h"
#include <stdio.h>
#include <string.h>

void memdigits_render(void);

/* ---- 节奏与规则(集成时 tick_interval_ms 设 100) ---- */
#define MD_SHOW_MS     1000u      /* 数字串展示时长 */
#define MD_FB_MS       800u       /* 判定反馈时长 */
#define MD_LVUP_NEED   3          /* 连续答对升级数 */
#define MD_MAX_LVL     8          /* 最高等级(10 位数字) */
#define MD_MIN_LEN     2          /* 位数 = 等级+2 (3..10) */
#define MD_MAX         10         /* 最大位数 */

enum { MD_SHOW = 0, MD_INPUT, MD_FEEDBACK };

/* ---- 布局(游戏区 y>=16) ---- */
#define MD_LABEL_Y    18          /* 标签行(1x) */
#define MD_CELL_Y     28          /* 数字格顶(格 25px 高) */
#define MD_CELL_W     18          /* 格宽(字形 15px + 边距) */
#define MD_CELL_H     25
#define MD_CELL_ADV   20          /* 格间距 */
#define MD_STATUS_Y   134         /* 底部状态行(1x) */

static char     md_target[MD_MAX];    /* 目标数字串 */
static char     md_prev[MD_MAX];      /* 上一题(防连续重复) */
static int      md_prev_len;          /* 上一题长度, -1=无 */
static int      md_len;               /* 当前位数 = 等级+2 */
static char     md_input[MD_MAX];     /* 已输入数字 */
static int      md_in;                /* 已输入位数 */
static int      md_level;             /* 1..MD_MAX_LVL */
static int      md_streak;            /* 连续答对(>=3 升级) */
static int      md_correct;           /* 累计答对 */
static int      md_wrong;             /* 连续答错(>=2 FAIL) */
static int      md_score;             /* 正确数 × 等级 */
static int      md_state;             /* MD_SHOW/INPUT/FEEDBACK */
static bool     md_over;
static bool     md_over_full;         /* 结束全刷只做一次 */
static uint64_t md_until;             /* 当前状态截止时刻 */
static char     md_fb[40];            /* 判定反馈文本 */
static uint32_t md_gens;              /* 新局计数(种子混合) */
static rng_t    md_rng;

/* ---- 随机题: 逐位 0-9; do-while 防与上一题全同, guard 防死循环 ---- */
static bool md_same(void) {
    for (int i = 0; i < md_len; i++)
        if (md_target[i] != md_prev[i]) return false;
    return true;
}

static void md_make_q(void) {
    md_len = md_level + MD_MIN_LEN;
    if (md_len > MD_MAX) md_len = MD_MAX;
    int guard = 0;
    do {
        for (int i = 0; i < MD_MAX; i++)
            md_target[i] = (char)('0' + (int)rng_range(&md_rng, 10u));
    } while (md_len == md_prev_len && md_same() && ++guard < 16);
    md_prev_len = md_len;
    for (int i = 0; i < MD_MAX; i++) md_prev[i] = md_target[i];
    md_in = 0;
}

static void md_new_game(void) {
    md_gens++;
    rng_seed(&md_rng, now_ms() ^ ((uint64_t)md_gens * 0x9E3779B1u));
    md_level = 1;
    md_correct = 0;
    md_streak = 0;
    md_wrong = 0;
    md_score = 0;
    md_prev_len = -1;
    md_over = false;
    md_over_full = false;
    md_fb[0] = 0;
    md_make_q();
    md_state = MD_SHOW;
    md_until = now_ms() + MD_SHOW_MS;
    memdigits_render();
    disp_full();
}

void memdigits_enter(void) { md_new_game(); }

void memdigits_tick(uint64_t now) {
    if (md_over) return;
    if (md_state == MD_SHOW && now >= md_until) {
        md_state = MD_INPUT;               /* 数字串消失 → 输入窗口 */
    } else if (md_state == MD_FEEDBACK && now >= md_until) {
        md_make_q();                       /* 判定结束 → 下一题 */
        md_state = MD_SHOW;
        md_until = now + MD_SHOW_MS;
    }
}

/* ---- 输入 ---- */
static void md_enter_digit(char c) {
    if (md_over || md_state != MD_INPUT) return;
    if (md_in < md_len) md_input[md_in++] = c;
}

static void md_backspace(void) {
    if (md_over || md_state != MD_INPUT) return;
    if (md_in > 0) md_in--;
}

/* OK 提交(须输入满位): 逐位比对 → 对/错分支 */
static void md_submit(void) {
    if (md_over || md_state != MD_INPUT || md_in != md_len) return;
    bool ok = true;
    for (int i = 0; i < md_len; i++)
        if (md_input[i] != md_target[i]) { ok = false; break; }
    md_state = MD_FEEDBACK;
    md_until = now_ms() + MD_FB_MS;
    if (ok) {
        md_correct++;
        md_wrong = 0;                        /* 答对清零连错 */
        md_streak++;
        if (md_streak >= MD_LVUP_NEED) {
            md_streak = 0;
            if (md_level < MD_MAX_LVL) {
                md_level++;
                snprintf(md_fb, sizeof(md_fb), "LEVEL UP! NOW LV %d", md_level);
            } else {
                snprintf(md_fb, sizeof(md_fb), "PERFECT! MAX LV %d", md_level);
            }
        } else {
            snprintf(md_fb, sizeof(md_fb), "CORRECT %d IN A ROW", md_streak);
        }
        audio_clear();
    } else {
        md_wrong++;
        md_streak = 0;
        if (md_wrong >= 2) {                 /* 同等级 2 连错 → FAIL */
            md_over = true;
            audio_lose();
            led_fx_set(LED_FX_LOSE);
            return;
        }
        if (md_level > 1) md_level--;        /* 错 1 题降 1 级 */
        snprintf(md_fb, sizeof(md_fb), "WRONG! LV %d", md_level);
        audio_error();
    }
    md_score = md_correct * md_level;        /* 分数 = 正确数 × 等级 */
}

void memdigits_on_key(const key_event_t *ev) {
    if (ev->is_repeat) return;               /* 全部按键忽略重复 */
    if (md_over) {
        if (ev->key == K_OK || (ev->key == K_CHAR && ev->ch == 'n'))
            md_new_game();
        else if (ev->key == K_BACK || ev->key == K_QUIT)
            s_exit_request = true;
        return;
    }
    if (ev->key == K_CHAR) {
        if (ev->ch >= '0' && ev->ch <= '9') {
            md_enter_digit((char)ev->ch);
            audio_tick();
        } else if (ev->ch == 'n') {
            audio_select();
            md_new_game();
        }
        return;
    }
    switch (ev->key) {
    case K_OK:
        md_submit();
        break;
    case K_DEL:
        md_backspace();
        audio_move();
        break;
    case K_BACK:
    case K_PAUSE: {
        pause_sel_t sel;
        if (ui_pause_run(&sel)) {
            if (sel == PAUSE_RESTART) md_new_game();
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

/* ---- 渲染 ---- */

/* 5x7 字形 3x 放大; white=true 时写白像素(黑格反白用) */
static void md_glyph3(int x, int y, char c, bool white) {
    const uint8_t *g = font_glyph5x7[(unsigned char)c];
    for (int j = 0; j < FONT_H; j++)
        for (int i = 0; i < FONT_W; i++)
            if (g[j] & (1u << i))
                fb_fill_rect(x + i * 3, y + j * 3, 3, 3, white);
}

static void md_text3(int x, int y, const char *s, bool black) {
    for (const char *p = s; *p; p++) {
        md_glyph3(x, y, *p, black);
        x += FONT_ADV * 3;
    }
}

static void md_text3_center(int y, const char *s, bool black) {
    int w = ((int)strlen(s) * FONT_ADV - 1) * 3;
    int x = (CCG_W - w) / 2;
    if (x < 0) x = 0;
    md_text3(x, y, s, black);
}

void memdigits_render(void) {
    fb_clear(false);
    char buf[32];

    if (md_over) {
        /* HUD 两行: 左上结果 + 右上按键提示; 墙内只放结果展示 */
        fb_fill_rect(0, 0, CCG_W, CCG_HUD_H - 1, false);
        snprintf(buf, sizeof(buf), "FAIL SCORE %d", md_score);
        fb_text(2, 2, buf, true);
        snprintf(buf, sizeof(buf), "RIGHT %d  LV %d", md_correct, md_level);
        fb_text(2, 9, buf, true);
        fb_text(CCG_W - 2 - text_width("OK/N:RETRY BACK:QUIT"), 2,
                "OK/N:RETRY BACK:QUIT", true);
        /* 墙内: 最终成绩大字 + 出错题答案 */
        md_text3_center(40, "GAME OVER", true);
        snprintf(buf, sizeof(buf), "%d", md_score);
        md_text3_center(72, buf, true);
        {
            char ans[MD_MAX + 1];
            int n = md_len < MD_MAX ? md_len : MD_MAX;
            for (int i = 0; i < n; i++) ans[i] = md_target[i];
            ans[n] = 0;
            snprintf(buf, sizeof(buf), "ANS %s", ans);
            fb_text_center(100, buf, true);
        }
        if (!md_over_full) { md_over_full = true; disp_force_full(); }
        return;
    }

    /* 标签行(状态指示) */
    const char *label;
    if (md_state == MD_SHOW)
        label = "MEMORIZE";
    else if (md_state == MD_INPUT)
        label = (md_in == md_len) ? "PRESS OK" : "TYPE THE DIGITS";
    else
        label = md_fb;
    fb_text_center(MD_LABEL_Y, label, true);

    /* 数字区: 展示期纯大字; 输入/反馈期逐格反馈(对=反白 错=灰格) */
    if (md_state == MD_SHOW) {
        char digits[MD_MAX + 1];
        for (int i = 0; i < md_len; i++) digits[i] = md_target[i];
        digits[md_len] = 0;
        md_text3_center(MD_CELL_Y + 2, digits, true);
    } else {
        int x0 = (CCG_W - (md_len * MD_CELL_ADV - 2)) / 2;
        for (int i = 0; i < md_in; i++) {
            int x = x0 + i * MD_CELL_ADV;
            if (md_input[i] == md_target[i]) {
                fb_fill_rect(x, MD_CELL_Y, MD_CELL_W, MD_CELL_H, true);
                md_glyph3(x + 1, MD_CELL_Y + 2, md_input[i], false);
            } else {
                fb_fill_tile(x, MD_CELL_Y, MD_CELL_W, MD_CELL_H,
                             pat_get(PAT_DOT_DENSE));
                md_glyph3(x + 1, MD_CELL_Y + 2, md_input[i], true);
            }
        }
        /* 光标: 全部格画完后的下一位竖条(输入态) */
        if (md_state == MD_INPUT && md_in < md_len)
            fb_fill_rect(x0 + md_in * MD_CELL_ADV, MD_CELL_Y, 3, MD_CELL_H, true);
    }

    /* 底部状态行 */
    snprintf(buf, sizeof(buf), "STREAK %d/%d", md_streak, MD_LVUP_NEED);
    fb_text(2, MD_STATUS_Y, buf, true);
    fb_text(CCG_W - 4 - text_width("DEL:BS OK:GO N:NEW"), MD_STATUS_Y,
            "DEL:BS OK:GO N:NEW", true);

    /* HUD 顶栏: 左 MEMDIGITS, 右 LEVEL n SCORE n */
    fb_text(0, 0, "MEMDIGITS", true);
    snprintf(buf, sizeof(buf), "LEVEL %d SCORE %d", md_level, md_score);
    fb_text(CCG_W - 4 - text_width(buf), 0, buf, true);
    fb_hline(0, CCG_HUD_H - 1, CCG_W, true);
}

void memdigits_exit(void) {}
