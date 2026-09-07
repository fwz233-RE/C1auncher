/* CLOCK QUIZ — 读钟面: 画线绘制模拟时钟(半径 56 圆 + 时针分针),
 * 玩家输入 24h 制时间 HHMM(四位数字), OK 提交: 对 = +10 分
 * 30 秒限时一轮; 答错进入"展示答案"阶段(2 秒)显示正确时间, 自动换题
 * HUD 顶栏: 左 CLOCK, 右 TIME + SCORE; 结束: 左上结果 + 右上 OK/N:RETRY BACK:QUIT
 * 布局: 钟面居中 (148,84) r=56, 数字输入槽(3x "HH:MM")在钟面内下方,
 *   限时进度条在钟面上方, 反馈行在钟面下方
 * 静态前缀 cq_; 像素坐标一律 int; 零 malloc; 全部按键忽略重复
 *
 * 集成提示(help[] 最多 5 行):
 *   "CLOCK QUIZ", "READ THE ANALOG CLOCK",
 *   "TYPE 24H TIME HHMM, OK SUBMIT", "CORRECT +10, 30 SEC ROUND",
 *   "DEL:BACKSPACE  N:SKIP", NULL
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
#include <stdlib.h>
#include <string.h>

void clockquiz_render(void);

/* ---- 节奏与限时 ---- */
#define CQ_TICK_MS 1000u         /* 面板实测 tick 需 >=700ms */
#define CQ_GAME_MS 30000u        /* 30 秒一轮 */
#define CQ_ANS_MS 2000u          /* 答错后展示正确答案时长 */
#define CQ_ANS_CAP 4             /* HHMM 四位 */

/* ---- 布局(游戏区 y>=16) ---- */
#define CQ_CX 148                /* 钟面圆心 */
#define CQ_CY 84
#define CQ_R 56                  /* 钟面半径 */
#define CQ_SLOT_Y 93             /* 3x 输入槽行(钟面内下方) */
#define CQ_SLOT_X0 (CQ_CX - (5 * FONT_ADV * 3 - 3) / 2)   /* 5 槽位居中 */
#define CQ_BAR_Y 17              /* 限时进度条 */
#define CQ_FB_Y 143              /* 反馈行(1x) */
#define CQ_OVER_Y1 42            /* 结束: FINAL SCORE */
#define CQ_OVER_Y2 70            /* 结束: 分数 3x */
#define CQ_OVER_Y3 122           /* 结束: RIGHT n OF m */

typedef enum {
    CQ_PH_PLAY = 0,              /* 正常答题 */
    CQ_PH_ANS,                   /* 答错展示答案(自动换题) */
    CQ_PH_OVER                   /* 一轮结束 */
} cq_phase_t;

static rng_t cq_rng;
static int cq_hour;              /* 当前钟面时间 1..23 */
static int cq_min;               /* 0/15/30/45 */
static int cq_last_h, cq_last_m; /* 上一题(防连续重复) */
static char cq_ans[4];           /* 已输入数字 */
static int cq_ansn;
static int cq_score;
static int cq_right;             /* 答对题数 */
static int cq_total;             /* 已提交题数 */
static uint32_t cq_elapsed;      /* 已进行 ms */
static int cq_ans_left;          /* 展示答案剩余 ms */
static cq_phase_t cq_phase;
static bool cq_over_full;        /* 结束全刷只做一次 */
static char cq_fb[24];           /* 反馈行(最后一次判定) */
static uint32_t cq_gens;         /* 换局计数(种子混合) */

/* ---- sin(deg)*64, 360 项(.rodata 360B); cos 用 cq_sin[(d+90)%360] ---- */
static const int8_t cq_sin[360] = {
      0,   1,   2,   3,   4,   6,   7,   8,   9,  10,  11,  12,
     13,  14,  15,  17,  18,  19,  20,  21,  22,  23,  24,  25,
     26,  27,  28,  29,  30,  31,  32,  33,  34,  35,  36,  37,
     38,  39,  39,  40,  41,  42,  43,  44,  44,  45,  46,  47,
     48,  48,  49,  50,  50,  51,  52,  52,  53,  54,  54,  55,
     55,  56,  57,  57,  58,  58,  58,  59,  59,  60,  60,  61,
     61,  61,  62,  62,  62,  62,  63,  63,  63,  63,  63,  64,
     64,  64,  64,  64,  64,  64,  64,  64,  64,  64,  64,  64,
     64,  64,  63,  63,  63,  63,  63,  62,  62,  62,  62,  61,
     61,  61,  60,  60,  59,  59,  58,  58,  58,  57,  57,  56,
     55,  55,  54,  54,  53,  52,  52,  51,  50,  50,  49,  48,
     48,  47,  46,  45,  44,  44,  43,  42,  41,  40,  39,  39,
     38,  37,  36,  35,  34,  33,  32,  31,  30,  29,  28,  27,
     26,  25,  24,  23,  22,  21,  20,  19,  18,  17,  15,  14,
     13,  12,  11,  10,   9,   8,   7,   6,   4,   3,   2,   1,
      0,  -1,  -2,  -3,  -4,  -6,  -7,  -8,  -9, -10, -11, -12,
    -13, -14, -15, -17, -18, -19, -20, -21, -22, -23, -24, -25,
    -26, -27, -28, -29, -30, -31, -32, -33, -34, -35, -36, -37,
    -38, -39, -39, -40, -41, -42, -43, -44, -44, -45, -46, -47,
    -48, -48, -49, -50, -50, -51, -52, -52, -53, -54, -54, -55,
    -55, -56, -57, -57, -58, -58, -58, -59, -59, -60, -60, -61,
    -61, -61, -62, -62, -62, -62, -63, -63, -63, -63, -63, -64,
    -64, -64, -64, -64, -64, -64, -64, -64, -64, -64, -64, -64,
    -64, -64, -63, -63, -63, -63, -63, -62, -62, -62, -62, -61,
    -61, -61, -60, -60, -59, -59, -58, -58, -58, -57, -57, -56,
    -55, -55, -54, -54, -53, -52, -52, -51, -50, -50, -49, -48,
    -48, -47, -46, -45, -44, -44, -43, -42, -41, -40, -39, -39,
    -38, -37, -36, -35, -34, -33, -32, -31, -30, -29, -28, -27,
    -26, -25, -24, -23, -22, -21, -20, -19, -18, -17, -15, -14,
    -13, -12, -11, -10,  -9,  -8,  -7,  -6,  -4,  -3,  -2,  -1,
};

/* 指针角度(顺时针从 12 点起, 度): 时针含分钟偏移(分钟/2, 精确到 0.5 度) */
static int cq_hand_deg(int hour, int min, bool is_hour) {
    if (is_hour) return (hour % 12) * 30 + min / 2;
    return min * 6;
}

/* ---- 基础画线 ---- */
static void cq_line(int x0, int y0, int x1, int y1, bool black) {
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        fb_pixel(x0, y0, black);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

/* 中点圆算法(8 向对称) */
static void cq_circle(int cx, int cy, int r, bool black) {
    int x = 0, y = r, d = 3 - 2 * r;
    while (x <= y) {
        fb_pixel(cx + x, cy + y, black);
        fb_pixel(cx - x, cy + y, black);
        fb_pixel(cx + x, cy - y, black);
        fb_pixel(cx - x, cy - y, black);
        fb_pixel(cx + y, cy + x, black);
        fb_pixel(cx - y, cy + x, black);
        fb_pixel(cx + y, cy - x, black);
        fb_pixel(cx - y, cy - x, black);
        if (d < 0) d += 4 * x + 6;
        else { d += 4 * (x - y) + 10; y--; }
        x++;
    }
}

/* 指针: 从圆心指向 (sx,-sy)/64 方向 len 像素; hw=半宽(1 根=1px) */
static void cq_hand(int cx, int cy, int sx, int sy, int len, int hw, bool black) {
    int ex = cx + sx * len / 64;
    int ey = cy - sy * len / 64;
    int dx = abs(ex - cx), dy = abs(ey - cy);
    int ox = 0, oy = 0;
    if (dx > dy) oy = 1;                /* 横向线: 上下加粗 */
    else if (dy > dx) ox = 1;           /* 纵向线: 左右加粗 */
    else { ox = 1; oy = 1; }            /* 45 度: 对角加粗 */
    for (int k = -hw; k <= hw; k++)
        cq_line(cx + ox * k, cy + oy * k, ex + ox * k, ey + oy * k, black);
}

/* 轴心: 实心小圆(r=2) */
static void cq_hub(int cx, int cy) {
    fb_fill_rect(cx - 2, cy - 2, 5, 1, true);
    fb_fill_rect(cx - 1, cy - 1, 3, 1, true);
    fb_fill_rect(cx, cy, 1, 1, true);
    fb_fill_rect(cx - 1, cy + 1, 3, 1, true);
    fb_fill_rect(cx - 2, cy + 2, 5, 1, true);
}

/* 钟面: 圆 + 12 刻度(3/6/9/12 加长) + 数字 12/3/6/9 + 时针分针 + 轴心 */
static void cq_draw_clock(int hour, int min) {
    int cx = CQ_CX, cy = CQ_CY, r = CQ_R;
    cq_circle(cx, cy, r, true);
    for (int i = 0; i < 12; i++) {
        int deg = i * 30;
        int t = (deg % 90 == 0) ? 10 : 6;
        int sx = cq_sin[deg], sy = cq_sin[(deg + 90) % 360];
        cq_line(cx + sx * (r - t) / 64, cy - sy * (r - t) / 64,
                cx + sx * r / 64, cy - sy * r / 64, true);
    }
    fb_text(cx - 5, cy - r + 3, "12", true);
    fb_text(cx + r - 14, cy - 3, "3", true);
    fb_text(cx - 2, cy + r - 10, "6", true);
    fb_text(cx - r + 9, cy - 3, "9", true);
    int dh = cq_hand_deg(hour, min, true);
    cq_hand(cx, cy, cq_sin[dh], cq_sin[(dh + 90) % 360], 30, 1, true);
    int dm = cq_hand_deg(hour, min, false);
    cq_hand(cx, cy, cq_sin[dm], cq_sin[(dm + 90) % 360], 42, 0, true);
    cq_hub(cx, cy);
}

/* ---- 5x7 字形 3x 放大(每字符 15x21, 间距 3px) ---- */
static void cq_text3(int x, int y, const char *s, bool black) {
    for (const char *p = s; *p; p++) {
        const uint8_t *g = font_glyph5x7[(unsigned char)*p];
        for (int j = 0; j < FONT_H; j++)
            for (int i = 0; i < FONT_W; i++)
                if (g[j] & (1u << i))
                    fb_fill_rect(x + i * 3, y + j * 3, 3, 3, black);
        x += FONT_ADV * 3;
    }
}

static void cq_text3_center(int y, const char *s, bool black) {
    int w = ((int)strlen(s) * FONT_ADV - 1) * 3;
    int x = (CCG_W - w) / 2;
    if (x < 0) x = 0;
    cq_text3(x, y, s, black);
}

/* 输入槽: 5 槽位 "HH:MM" 3x; 空槽 '_', 下一输入位反白光标 */
static void cq_draw_slots(void) {
    char disp[5];
    if (cq_phase == CQ_PH_ANS) {           /* 展示正确答案 */
        disp[0] = (char)('0' + cq_hour / 10);
        disp[1] = (char)('0' + cq_hour % 10);
        disp[2] = ':';
        disp[3] = (char)('0' + cq_min / 10);
        disp[4] = (char)('0' + cq_min % 10);
    } else {
        disp[0] = cq_ansn > 0 ? cq_ans[0] : '_';
        disp[1] = cq_ansn > 1 ? cq_ans[1] : '_';
        disp[2] = ':';
        disp[3] = cq_ansn > 2 ? cq_ans[2] : '_';
        disp[4] = cq_ansn > 3 ? cq_ans[3] : '_';
    }
    for (int i = 0; i < 5; i++) {
        int x = CQ_SLOT_X0 + i * (FONT_ADV * 3);
        if (i == 2) {
            cq_text3(x, CQ_SLOT_Y, ":", true);
            continue;
        }
        int di = (i < 2) ? i : i - 1;      /* 槽位 -> 输入位 */
        char c[2] = { disp[i], 0 };
        if (cq_phase == CQ_PH_PLAY && di == cq_ansn) {
            fb_fill_rect(x, CQ_SLOT_Y, FONT_W * 3, FONT_H * 3, true);
            cq_text3(x, CQ_SLOT_Y, c, false);
        } else {
            cq_text3(x, CQ_SLOT_Y, c, true);
        }
    }
}

/* ---- 题目生成: 24h 小时 1..23, 分钟整点/半点/刻钟混合 ---- */
static void cq_make_q(void) {
    int guard = 0;
    do {
        cq_hour = (int)rng_range(&cq_rng, 23u) + 1;   /* 1..23 */
        cq_min = (int)rng_range(&cq_rng, 4u) * 15;    /* 0/15/30/45 */
    } while (cq_hour == cq_last_h && cq_min == cq_last_m && ++guard < 8);
    cq_last_h = cq_hour;
    cq_last_m = cq_min;
}

/* 提交: 对 = +10; 错 = 进入展示答案阶段(2 秒后自动换题) */
static void cq_submit(void) {
    if (cq_ansn < CQ_ANS_CAP) {
        snprintf(cq_fb, sizeof(cq_fb), "NEED 4 DIGITS");
        audio_error();
        return;
    }
    int h = (cq_ans[0] - '0') * 10 + (cq_ans[1] - '0');
    int m = (cq_ans[2] - '0') * 10 + (cq_ans[3] - '0');
    cq_total++;
    cq_ansn = 0;
    if (h == cq_hour && m == cq_min) {
        cq_score += 10;
        cq_right++;
        snprintf(cq_fb, sizeof(cq_fb), "OK +10");
        cq_make_q();
        audio_clear();               /* 答对 */
    } else {
        snprintf(cq_fb, sizeof(cq_fb), "WRONG %02d:%02d", cq_hour, cq_min);
        cq_phase = CQ_PH_ANS;
        cq_ans_left = CQ_ANS_MS;
        audio_error();               /* 答错 */
    }
}

static void cq_new_game(void) {
    cq_gens++;
    rng_seed(&cq_rng, now_ms() ^ ((uint64_t)cq_gens * 0x9E3779B1u));
    cq_score = 0;
    cq_right = 0;
    cq_total = 0;
    cq_elapsed = 0;
    cq_ansn = 0;
    cq_phase = CQ_PH_PLAY;
    cq_over_full = false;
    cq_fb[0] = 0;
    cq_last_h = -1;
    cq_last_m = -1;
    cq_make_q();
    clockquiz_render();
    disp_full();
}

void clockquiz_enter(void) { cq_new_game(); }

void clockquiz_tick(uint64_t now) {
    (void)now;
    if (cq_phase == CQ_PH_OVER) return;
    if (cq_phase == CQ_PH_ANS) {
        cq_ans_left -= (int)CQ_TICK_MS;
        if (cq_ans_left <= 0) {
            cq_phase = CQ_PH_PLAY;
            cq_make_q();
        }
    }
    cq_elapsed += CQ_TICK_MS;
    if (cq_elapsed >= CQ_GAME_MS) {
        cq_elapsed = CQ_GAME_MS;
        cq_phase = CQ_PH_OVER;
        cq_over_full = false;
        audio_lose();                /* 一轮超时结束 */
        led_fx_set(LED_FX_LOSE);
    }
}

/* ---- 渲染 ---- */
void clockquiz_render(void) {
    fb_clear(false);
    char buf[32];

    if (cq_phase == CQ_PH_OVER) {
        /* HUD 两行: 左上结果 + 右上按键提示; 墙内只放最终成绩 */
        fb_fill_rect(0, 0, CCG_W, CCG_HUD_H - 1, false);
        snprintf(buf, sizeof(buf), "SCORE %d", cq_score);
        fb_text(2, 2, buf, true);
        snprintf(buf, sizeof(buf), "RIGHT %d OF %d", cq_right, cq_total);
        fb_text(2, 9, buf, true);
        fb_text(CCG_W - 2 - text_width("OK/N:RETRY BACK:QUIT"), 2,
                "OK/N:RETRY BACK:QUIT", true);
        cq_text3_center(CQ_OVER_Y1, "FINAL SCORE", true);
        snprintf(buf, sizeof(buf), "%d", cq_score);
        cq_text3_center(CQ_OVER_Y2, buf, true);
        snprintf(buf, sizeof(buf), "RIGHT %d OF %d", cq_right, cq_total);
        fb_text_center(CQ_OVER_Y3, buf, true);
        if (!cq_over_full) { cq_over_full = true; disp_force_full(); }
        return;
    }

    /* HUD 顶栏: 左 CLOCK, 右 TIME + SCORE */
    fb_text(0, 0, "CLOCK", true);
    snprintf(buf, sizeof(buf), "TIME %u SCORE %d",
             (CQ_GAME_MS - cq_elapsed) / 1000u, cq_score);
    fb_text(CCG_W - 4 - text_width(buf), 0, buf, true);
    fb_hline(0, CCG_HUD_H - 1, CCG_W, true);

    /* 限时进度条(满格=30s) */
    fb_stroke_rect(2, CQ_BAR_Y, CCG_W - 4, 4, true);
    {
        uint32_t rem = CQ_GAME_MS - cq_elapsed;
        uint32_t bw = (CCG_W - 6u) * rem / CQ_GAME_MS;
        if (bw > 0) fb_fill_rect(3, CQ_BAR_Y + 1, (int)bw, 2, true);
    }

    /* 钟面 + 输入槽 */
    cq_draw_clock(cq_hour, cq_min);
    cq_draw_slots();

    /* 反馈行 + 按键提示 */
    if (cq_fb[0]) fb_text(2, CQ_FB_Y, cq_fb, true);
    fb_text(CCG_W - 4 - text_width("OK:GO N:SKIP DEL:BS"), CQ_FB_Y,
            "OK:GO N:SKIP DEL:BS", true);
}

/* ---- 输入 ---- */
void clockquiz_on_key(const key_event_t *ev) {
    if (ev->is_repeat) return;                 /* 全部按键忽略重复 */
    if (cq_phase == CQ_PH_OVER) {
        if (ev->key == K_OK || (ev->key == K_CHAR && ev->ch == 'n'))
            cq_new_game();
        else if (ev->key == K_BACK || ev->key == K_QUIT)
            s_exit_request = true;
        return;
    }
    if (ev->key == K_CHAR) {
        if (ev->ch >= '0' && ev->ch <= '9') {
            if (cq_phase == CQ_PH_PLAY && cq_ansn < CQ_ANS_CAP)
                cq_ans[cq_ansn++] = (char)ev->ch;
        } else if (ev->ch == 'n') {
            if (cq_phase == CQ_PH_ANS) { cq_phase = CQ_PH_PLAY; cq_make_q(); }
            else { cq_ansn = 0; cq_make_q(); }
        }
        return;
    }
    switch (ev->key) {
    case K_OK:
        if (cq_phase == CQ_PH_ANS) { cq_phase = CQ_PH_PLAY; cq_make_q(); }
        else cq_submit();
        break;
    case K_DEL:
        if (cq_phase == CQ_PH_PLAY && cq_ansn > 0) cq_ansn--;
        break;
    case K_BACK:
    case K_PAUSE: {
        pause_sel_t sel;
        if (ui_pause_run(&sel)) {
            if (sel == PAUSE_RESTART) cq_new_game();
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

void clockquiz_exit(void) {}
