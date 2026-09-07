/* GEO QUIZ — 国家↔首都拼写猜谜
 * 显示国家名, 拼写首都名(字母键), OK 提交自动判分: 对 +5 并清空换题,
 * 错亮出正确答案并清空; 30 秒限时(进度条), 连对连击提示(STREAK n!);
 * N 切换正/反向(猜首都 ↔ 猜国家, 输入为空时生效), 切换后清空输入并换题;
 * DEL 退格; OK 提交; BACK/P 暂停; Q 退出; 结束 OK/N 再来, BACK/Q 退出
 * 静态前缀 gq_; 全部按键忽略重复; 零 malloc; 像素坐标 int;
 * 题库 30 组 static const, 首尾不重复(do-while + guard 兜底强制换题)
 * 集成提示: tick_interval_ms = 100 (每 tick 计时 100ms, 共 30s)
 * help[]:
 *   "GEO QUIZ",
 *   "TYPE THE CAPITAL, +5 PER RIGHT",
 *   "30 SECONDS - 3 IN A ROW = COMBO",
 *   "N: FLIP CAPITAL/COUNTRY (EMPTY)",
 *   "DEL: BS  OK: SUBMIT", NULL
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

void geo_render(void);

#define GQ_N        30      /* 题库题数 */
#define GQ_ANS_CAP  12      /* 答案上限(最长 KUALALUMPUR=11) */
#define GQ_TICK_MS  100u    /* 计时粒度(与 desc.tick_interval_ms 对应) */
#define GQ_GAME_MS  30000u  /* 限时 30 秒 */
#define GQ_SCORE    5       /* 答对得分 */
#define GQ_COMBO    3       /* 连对 3 题触发连击提示 */

typedef struct { const char *c; const char *a; } gq_pair_t;

/* 30 组国家→首都(全大写 ASCII, 无空格, 答案唯一确定) */
static const gq_pair_t gq_data[GQ_N] = {
    {"FRANCE","PARIS"},     {"JAPAN","TOKYO"},      {"GERMANY","BERLIN"},
    {"ITALY","ROME"},       {"SPAIN","MADRID"},     {"PORTUGAL","LISBON"},
    {"RUSSIA","MOSCOW"},    {"CHINA","BEIJING"},    {"INDIA","DELHI"},
    {"BRAZIL","BRASILIA"},  {"EGYPT","CAIRO"},      {"TURKEY","ANKARA"},
    {"GREECE","ATHENS"},    {"POLAND","WARSAW"},    {"NORWAY","OSLO"},
    {"FINLAND","HELSINKI"}, {"SWEDEN","STOCKHOLM"}, {"IRELAND","DUBLIN"},
    {"AUSTRIA","VIENNA"},   {"HUNGARY","BUDAPEST"}, {"ROMANIA","BUCHAREST"},
    {"THAILAND","BANGKOK"}, {"VIETNAM","HANOI"},    {"INDONESIA","JAKARTA"},
    {"MALAYSIA","KUALALUMPUR"}, {"AUSTRALIA","CANBERRA"}, {"CANADA","OTTAWA"},
    {"NIGERIA","ABUJA"},    {"KENYA","NAIROBI"},    {"CUBA","HAVANA"},
};

/* ---- 对局状态 ---- */
static int gq_cur;               /* 当前题号 */
static bool gq_rev;              /* 反向: 显示首都猜国家 */
static char gq_ans[GQ_ANS_CAP + 1]; /* 已拼答案(大写) */
static int gq_ansn;              /* 已输入字母数 */
static int gq_score;
static int gq_streak;            /* 连对数 */
static int gq_right;             /* 本局总答对数 */
static uint32_t gq_elapsed;      /* 已进行 ms */
static bool gq_over;
static bool gq_over_full;        /* 结束全刷只做一次 */
static char gq_fb[30];           /* 反馈行(最后一次判定) */
static uint32_t gq_gens;         /* 换局计数(种子混合) */
static rng_t gq_rng;

/* 当前题的目标答案 */
static const char *gq_answer(void) {
    return gq_rev ? gq_data[gq_cur].c : gq_data[gq_cur].a;
}

/* 当前题的题干(显示用) */
static const char *gq_prompt(void) {
    return gq_rev ? gq_data[gq_cur].a : gq_data[gq_cur].c;
}

/* 换题: do-while 防与上一题相同, guard 防死循环, 兜底强制换题 */
static void gq_next_q(void) {
    int idx = gq_cur, guard = 0;
    do {
        idx = (int)rng_range(&gq_rng, GQ_N);
    } while (idx == gq_cur && ++guard < 8);
    if (idx == gq_cur) idx = (gq_cur + 1) % GQ_N;   /* 理论兜底: 保证换题 */
    gq_cur = idx;
    gq_ansn = 0;
}

/* 提交: 对 +5 连击, 错亮答案; 判完清空换题 */
static void gq_submit(void) {
    if (gq_ansn <= 0) return;                /* 未输入, 忽略 */
    gq_ans[gq_ansn] = 0;
    if (strcmp(gq_ans, gq_answer()) == 0) {
        gq_score += GQ_SCORE;
        gq_right++;
        gq_streak++;
        if (gq_streak >= GQ_COMBO)
            snprintf(gq_fb, sizeof gq_fb, "RIGHT +%d STREAK %d!", GQ_SCORE, gq_streak);
        else
            snprintf(gq_fb, sizeof gq_fb, "RIGHT +%d", GQ_SCORE);
        audio_clear();
    } else {
        gq_streak = 0;
        snprintf(gq_fb, sizeof gq_fb, "WRONG: %s", gq_answer());
        audio_error();
    }
    gq_ansn = 0;
    gq_next_q();
}

/* 切换正/反方向: 连击清零, 换题 */
static void gq_flip(void) {
    gq_rev = !gq_rev;
    gq_streak = 0;
    snprintf(gq_fb, sizeof gq_fb, "MODE %s", gq_rev ? "COUNTRY" : "CAPITAL");
    gq_next_q();
}

static void gq_new_game(void) {
    gq_gens++;
    rng_seed(&gq_rng, now_ms() ^ ((uint64_t)gq_gens * 0x9E3779B1u));
    gq_cur = -1;
    gq_rev = false;
    gq_ansn = 0;
    gq_score = 0;
    gq_streak = 0;
    gq_right = 0;
    gq_elapsed = 0;
    gq_over = false;
    gq_over_full = false;
    gq_fb[0] = 0;
    gq_next_q();
    geo_render();
    disp_full();
}

void geo_enter(void) { gq_new_game(); }

void geo_tick(uint64_t now) {
    (void)now;
    if (gq_over) return;
    gq_elapsed += GQ_TICK_MS;
    if (gq_elapsed >= GQ_GAME_MS) {
        gq_elapsed = GQ_GAME_MS;
        gq_over = true;
        gq_ansn = 0;
        audio_lose();
        led_fx_set(LED_FX_LOSE);
    }
}

/* ---- 渲染 ---- */

/* 5x7 字形 3x 放大(每字符 15x21, 间距 3px; 字形 bit0=最左) */
static void gq_text3(int x, int y, const char *s, bool black) {
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
static int gq_text3_center(int y, const char *s, bool black) {
    int w = ((int)strlen(s) * FONT_ADV - 1) * 3;
    int x = (CCG_W - w) / 2;
    if (x < 0) x = 0;
    gq_text3(x, y, s, black);
    return x;
}

void geo_render(void) {
    fb_clear(false);
    char buf[32];

    if (gq_over) {
        /* HUD 两行: 左上结果 + 右上按键提示; 墙内只放结果展示 */
        fb_fill_rect(0, 0, CCG_W, CCG_HUD_H - 1, false);
        snprintf(buf, sizeof buf, "SCORE %d", gq_score);
        fb_text(2, 2, buf, true);
        snprintf(buf, sizeof buf, "RIGHT %d", gq_right);
        fb_text(2, 9, buf, true);
        fb_text(CCG_W - 2 - text_width("OK/N:RETRY BACK:QUIT"), 2,
                "OK/N:RETRY BACK:QUIT", true);
        /* 墙内: 最终成绩大字 */
        gq_text3_center(28, "FINAL SCORE", true);
        snprintf(buf, sizeof buf, "%d", gq_score);
        gq_text3_center(56, buf, true);
        if (!gq_over_full) { gq_over_full = true; disp_force_full(); }
        return;
    }

    /* 题干标签: CAPITAL OF <国家> / COUNTRY OF <首都> */
    {
        int n = 0;
        static const char pre_cap[] = "CAPITAL OF ";
        static const char pre_cnt[] = "COUNTRY OF ";
        const char *pre = gq_rev ? pre_cnt : pre_cap;
        const char *p = gq_prompt();
        while (pre[n]) { buf[n] = pre[n]; n++; }
        while (*p && n < (int)sizeof(buf) - 2) buf[n++] = *p++;
        buf[n] = 0;
        fb_text_center(18, buf, true);
    }

    /* 题干大字(3x) */
    gq_text3_center(30, gq_prompt(), true);

    /* 答案行(3x): 已拼字母 + 光标块 */
    if (gq_ansn > 0) {
        buf[0] = 0;
        for (int i = 0; i < gq_ansn && i < GQ_ANS_CAP; i++) {
            char c[2] = { gq_ans[i], 0 };
            strncat(buf, c, sizeof(buf) - strlen(buf) - 1);
        }
        gq_text3_center(62, buf, true);
    }
    {
        int w = (gq_ansn * FONT_ADV - 1) * 3;   /* 已拼宽度 */
        int x = (CCG_W - w) / 2;
        if (x < 0) x = 0;
        fb_fill_rect(x + gq_ansn * FONT_ADV * 3 - 3, 62, 3, FONT_H * 3, true);
    }

    /* 反馈行(最后一次判定) */
    if (gq_fb[0])
        fb_text_center(92, gq_fb, true);

    /* 限时进度条(满格=30s) */
    fb_stroke_rect(2, 106, CCG_W - 4, 6, true);
    {
        uint32_t rem = GQ_GAME_MS - gq_elapsed;
        uint32_t bw = (CCG_W - 6u) * rem / GQ_GAME_MS;
        if (bw > 0u) fb_fill_rect(3, 107, (int)bw, 4, true);
    }

    /* 状态行 */
    snprintf(buf, sizeof buf, "STREAK %d", gq_streak);
    fb_text(2, 126, buf, true);
    fb_text(CCG_W - 4 - text_width("OK:GO N:FLIP DEL:BS"), 126,
            "OK:GO N:FLIP DEL:BS", true);

    /* HUD 顶栏: 左标题, 右 TIME + SCORE; 黑字白底 */
    fb_text(0, 0, "GEO QUIZ", true);
    snprintf(buf, sizeof buf, "TIME %u SCORE %d",
             (GQ_GAME_MS - gq_elapsed) / 1000u, gq_score);
    fb_text(CCG_W - 4 - text_width(buf), 0, buf, true);
    fb_hline(0, CCG_HUD_H - 1, CCG_W, true);
}

/* ---- 输入 ---- */

void geo_on_key(const key_event_t *ev) {
    if (ev->is_repeat) return;                 /* 全部按键忽略重复 */
    if (gq_over) {
        if (ev->key == K_OK || (ev->key == K_CHAR && ev->ch == 'n'))
            gq_new_game();
        else if (ev->key == K_BACK || ev->key == K_QUIT)
            s_exit_request = true;
        return;
    }
    if (ev->key == K_CHAR) {
        char ch = (char)ev->ch;
        /* N 命令与字母 N 共用一键: 未输入且答案不以 N 开头时切换方向,
         * 其余情况(拼写中含 N)输入字母 — 保证答案永远可拼、方向可切 */
        if (ch == 'n' && gq_ansn == 0 && gq_answer()[0] != 'N') {
            audio_select();
            gq_flip();
        } else if (ch >= 'a' && ch <= 'z') {
            if (gq_ansn < GQ_ANS_CAP)
                gq_ans[gq_ansn++] = (char)(ch - 'a' + 'A');  /* 存大写 */
        }
        return;
    }
    switch (ev->key) {
    case K_OK:
        gq_submit();
        break;
    case K_DEL:
        if (gq_ansn > 0) { gq_ansn--; audio_move(); }
        break;
    case K_BACK:
    case K_PAUSE: {
        pause_sel_t sel;
        if (ui_pause_run(&sel)) {
            if (sel == PAUSE_RESTART) gq_new_game();
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

void geo_exit(void) {}
