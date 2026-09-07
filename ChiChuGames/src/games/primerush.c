/* PRIME RUSH — 质数速判
 * 60 秒限时: 每题显示 2-1000 随机数, 玩家判断是否质数:
 *   OK/ENTER = 是质数;  BACK/SPACE = 不是质数
 * 答对 +1 分立即下一题; 答错扣 2 分(不低于 0), 显示 1.2s 正确答案
 * (质数显示 "N IS PRIME", 合数含因数分解 "N = d x (N/d) NOT PRIME") 后自动下一题
 * 质/合 50/50 混合抽取(避免纯均匀下 83% 都是"否"的退化玩法)
 * 质数判定内置试除: 2 与 3..sqrt(n) 奇数(试除素数表等价, n<=1000 最多 ~31 次)
 * 静态前缀 pr_; 像素坐标一律 int; 零 malloc; ASCII 文本
 *
 * 集成提示(help[] 最多 5 行):
 *   "PRIME RUSH", "QUICK PRIME DETECTION",
 *   "OK=PRIME  BACK/SPACE=NOT PRIME",
 *   "60 SECONDS: CORRECT +1, WRONG -2",
 *   "P:PAUSE  N:NEW  Q:QUIT"
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

/* ---- 节奏与规则 ---- */
#define PR_TICK_MS 100u          /* 与 WA 同款 100ms 离散 tick */
#define PR_GAME_MS 60000u        /* 60 秒限时 */
#define PR_FBK_TICKS 12          /* 答错显示正确答案 1.2s */
#define PR_MIN_N 2
#define PR_MAX_N 1000
#define PR_4X_ADV 24             /* 4x 大数字逐字步进: 5*4 + 4 */

/* ---- 布局 ---- */
#define PR_BOX_W 120             /* 题目数字框 */
#define PR_BOX_H 36
#define PR_BOX_X ((CCG_W - PR_BOX_W) / 2)   /* 88 */
#define PR_BOX_Y (CCG_HUD_H)                 /* 16 */
#define PR_NUM_Y (PR_BOX_Y + 4)              /* 20: 4x 数字(28 高) */
#define PR_Q_Y 56               /* "PRIME?" / "WRONG -2" 2x */
#define PR_FBK_Y 74             /* 正确答案 2x */
#define PR_STATS_Y 100          /* RIGHT/WRONG 计数 5x7 */
#define PR_HINT_Y 144           /* 键位提示 5x7 */

enum { PR_PLAY = 0, PR_FBK = 1, PR_OVER = 2 };

static rng_t pr_rng;
static int pr_state;             /* PLAY/FBK/OVER */
static int pr_num;               /* 当前题目 2-1000 */
static int pr_last;              /* 上一题(防连续重复), 0=无 */
static bool pr_ans;              /* 当前题目是否为质数 */
static uint32_t pr_score;
static uint32_t pr_best;         /* 会话内历史最高 */
static uint32_t pr_correct;
static uint32_t pr_wrong;
static uint32_t pr_time_ms;      /* 剩余毫秒 */
static uint32_t pr_gens;         /* 换局计数(种子混合) */
static uint8_t pr_fbk_ticks;     /* 反馈剩余 tick */
static bool pr_over_full;        /* 结束全刷只做一次 */
static char pr_fbk[32];          /* 答错反馈行(正确答案) */

void primerush_render(void);

/* 手写数字追加(无 snprintf 依赖) */
static void pr_append_u32(char *buf, unsigned *n, uint32_t v, unsigned cap) {
    char tmp[12];
    unsigned len = 0;
    if (v == 0) tmp[len++] = '0';
    while (v && len < 11) { tmp[len++] = (char)('0' + v % 10u); v /= 10u; }
    while (len && *n < cap) buf[(*n)++] = tmp[--len];
}

/* 试除判定: 2 与 3..sqrt(n) 奇数; n <= 1000 最多 ~31 次 */
static bool pr_is_prime(int n) {
    int d;
    if (n < 2) return false;
    if (n % 2 == 0) return n == 2;
    for (d = 3; d * d <= n; d += 2)
        if (n % d == 0) return false;
    return true;
}

/* 抽取 2-1000 随机数; want_prime=1 偏向质数, 0 偏向合数(50/50 混合)。
 * do-while 带 guard 防死循环, 兜底接受任意数(正确性以实测为准) */
static void pr_pick(int want_prime) {
    int guard = 0;
    do {
        pr_num = (int)(PR_MIN_N +
                       rng_range(&pr_rng, (uint32_t)(PR_MAX_N - PR_MIN_N + 1)));
    } while (pr_is_prime(pr_num) != (want_prime != 0) && ++guard < 128);
}

/* 下一题: 50/50 质合混合, 不与上一题重复(带 guard) */
static void pr_next(void) {
    int want = (int)rng_range(&pr_rng, 2u);
    int guard = 0;
    do {
        pr_pick(want);
    } while (pr_num == pr_last && ++guard < 16);
    pr_last = pr_num;
    pr_ans = pr_is_prime(pr_num);
    pr_state = PR_PLAY;
}

/* 合成答错反馈行: 质数 → "N IS PRIME"; 合数 → "N = d x (N/d) NOT PRIME" */
static void pr_make_fbk(void) {
    unsigned n = 0;
    const char *s;
    pr_append_u32(pr_fbk, &n, (uint32_t)pr_num, 30);
    if (pr_ans) {
        s = " IS PRIME";
        while (*s && n < 30) pr_fbk[n++] = *s++;
    } else {
        int d = 2;
        while (d * d <= pr_num && pr_num % d != 0) d++;   /* 最小真因子 */
        s = " = ";
        while (*s && n < 30) pr_fbk[n++] = *s++;
        pr_append_u32(pr_fbk, &n, (uint32_t)d, 30);
        s = " x ";
        while (*s && n < 30) pr_fbk[n++] = *s++;
        pr_append_u32(pr_fbk, &n, (uint32_t)(pr_num / d), 30);
        s = " NOT PRIME";
        while (*s && n < 30) pr_fbk[n++] = *s++;
    }
    pr_fbk[n] = 0;
}

/* 作答: say_prime=true 认为"是质数" */
static void pr_answer(bool say_prime) {
    if (pr_state != PR_PLAY) return;
    if (say_prime == pr_ans) {
        pr_score++;
        pr_correct++;
        audio_clear();           /* 答对得分 */
        pr_next();               /* 答对立即下一题 */
    } else {
        pr_score = (pr_score >= 2u) ? (pr_score - 2u) : 0u;
        pr_wrong++;
        pr_make_fbk();
        pr_state = PR_FBK;
        pr_fbk_ticks = PR_FBK_TICKS;
        audio_error();           /* 答错 */
    }
}

static void pr_new_game(void) {
    pr_gens++;
    rng_seed(&pr_rng, (uint64_t)now_ms() ^ ((uint64_t)pr_gens * 0x9E3779B1u));
    pr_score = 0;
    pr_correct = 0;
    pr_wrong = 0;
    pr_time_ms = PR_GAME_MS;
    pr_last = 0;
    pr_state = PR_PLAY;
    pr_fbk_ticks = 0;
    pr_over_full = false;
    pr_next();
    primerush_render();
    disp_full();
}

void primerush_enter(void) { pr_new_game(); }

void primerush_tick(uint64_t now) {
    (void)now;
    if (pr_state == PR_OVER) return;
    /* 倒计时(PLAY 与 FBK 均进行, 答错展示也占用时间) */
    if (pr_time_ms >= PR_TICK_MS) pr_time_ms -= PR_TICK_MS; else pr_time_ms = 0;
    if (pr_time_ms == 0) {       /* 时间到 → 结算 */
        pr_state = PR_OVER;
        pr_over_full = false;
        if (pr_score > pr_best) pr_best = pr_score;
        audio_lose();            /* 超时结束 */
        led_fx_set(LED_FX_LOSE);
        return;
    }
    if (pr_state == PR_FBK && pr_fbk_ticks > 0) {
        pr_fbk_ticks--;
        if (pr_fbk_ticks == 0) pr_next();   /* 展示完毕自动下一题 */
    }
}

/* ---- 绘制 ---- */

/* 2x 居中文本 */
static void pr_text_center2(int y, const char *s) {
    fb_text_scale2((CCG_W - text_width(s) * 2) / 2, y, s, true);
}

static int pr_digits(uint32_t v) {
    int d = 1;
    while (v >= 10u) { v /= 10u; d++; }
    return d;
}

/* 4x 大数字(5x7 → 20x28/字), 居中于框内 */
static void pr_draw_num4_center(int y, uint32_t v) {
    char tmp[6];
    unsigned len = 0;
    int x = PR_BOX_X + (PR_BOX_W - (pr_digits(v) * PR_4X_ADV - 4)) / 2;
    if (v == 0) tmp[len++] = '0';
    while (v && len < 5) { tmp[len++] = (char)('0' + v % 10u); v /= 10u; }
    while (len) {
        unsigned char c = (unsigned char)tmp[--len];
        if (c >= 0x30 && c <= 0x39) {
            for (int j = 0; j < FONT_H; j++)
                for (int i = 0; i < FONT_W; i++)
                    if (font_glyph5x7[c][j] & (1u << i))
                        fb_fill_rect(x + i * 4, y + j * 4, 4, 4, true);
        }
        x += PR_4X_ADV;
    }
}

/* HUD 顶栏(进行中): 左标题, 右 SCORE n TIME s */
static void pr_draw_hud(void) {
    char buf[32];
    unsigned n = 0;
    const char *p = "SCORE ";
    while (*p && n < 30) buf[n++] = *p++;
    pr_append_u32(buf, &n, pr_score, 30);
    p = " TIME ";
    while (*p && n < 30) buf[n++] = *p++;
    pr_append_u32(buf, &n, (pr_time_ms + 999u) / 1000u, 30);
    buf[n] = 0;
    fb_text(CCG_W - 4 - text_width(buf), 0, buf, true);
}

/* HUD(结束态): 左上 SCORE n BEST m, 右上 OK/N:RETRY BACK:QUIT */
static void pr_draw_over_hud(void) {
    char buf[28];
    unsigned n = 0;
    const char *p = "SCORE ";
    while (*p && n < 26) buf[n++] = *p++;
    pr_append_u32(buf, &n, pr_score, 26);
    p = " BEST ";
    while (*p && n < 26) buf[n++] = *p++;
    pr_append_u32(buf, &n, pr_best, 26);
    buf[n] = 0;
    fb_text(2, 2, buf, true);
    fb_text(CCG_W - 2 - text_width("OK/N:RETRY BACK:QUIT"), 2,
            "OK/N:RETRY BACK:QUIT", true);
}

/* 答对/答错计数行 */
static void pr_draw_stats(void) {
    char buf[24];
    unsigned n = 0;
    const char *p = "RIGHT ";
    while (*p && n < 22) buf[n++] = *p++;
    pr_append_u32(buf, &n, pr_correct, 22);
    p = "  WRONG ";
    while (*p && n < 22) buf[n++] = *p++;
    pr_append_u32(buf, &n, pr_wrong, 22);
    buf[n] = 0;
    fb_text_center(PR_STATS_Y, buf, true);
}

void primerush_render(void) {
    fb_clear(false);
    if (pr_state == PR_OVER) {
        /* HUD: 清底 + 左上结果 + 右上操作提示; 全刷一次 */
        fb_fill_rect(0, 0, CCG_W, CCG_HUD_H - 1, false);
        pr_draw_over_hud();
        pr_text_center2(24, "GAME OVER");
        fb_stroke_rect_thick(PR_BOX_X, 42, PR_BOX_W, PR_BOX_H, 2, true);
        pr_draw_num4_center(48, pr_score);
        {
            char buf[24];
            unsigned n = 0;
            const char *p = "RIGHT ";
            while (*p && n < 22) buf[n++] = *p++;
            pr_append_u32(buf, &n, pr_correct, 22);
            p = "  WRONG ";
            while (*p && n < 22) buf[n++] = *p++;
            pr_append_u32(buf, &n, pr_wrong, 22);
            buf[n] = 0;
            pr_text_center2(88, buf);
        }
        if (!pr_over_full) { pr_over_full = true; disp_force_full(); }
        return;
    }
    /* 进行中: HUD + 题目 */
    fb_text(0, 0, "PRIME RUSH", true);
    pr_draw_hud();
    fb_hline(0, CCG_HUD_H - 1, CCG_W, true);
    /* 题目数字框 */
    fb_stroke_rect_thick(PR_BOX_X, PR_BOX_Y, PR_BOX_W, PR_BOX_H, 2, true);
    pr_draw_num4_center(PR_NUM_Y, (uint32_t)pr_num);
    if (pr_state == PR_FBK) {
        pr_text_center2(PR_Q_Y, "WRONG -2");
        pr_text_center2(PR_FBK_Y, pr_fbk);
    } else {
        pr_text_center2(PR_Q_Y, "PRIME?");
    }
    pr_draw_stats();
    fb_text_center(PR_HINT_Y, "OK=PRIME  BACK/SPACE=NO  P:PAUSE  Q:QUIT", true);
}

void primerush_on_key(const key_event_t *ev) {
    if (ev->is_repeat) return;   /* 本游戏无方向键, 全部按键忽略重复 */
    if (pr_state == PR_OVER) {
        if (ev->key == K_OK || (ev->key == K_CHAR && ev->ch == 'n')) {
            audio_select();
            primerush_enter();
        }
        else if (ev->key == K_BACK || ev->key == K_QUIT)
            s_exit_request = true;
        else if (ev->key == K_PAUSE) {
            pause_sel_t sel;
            if (ui_pause_run(&sel)) {
                if (sel == PAUSE_RESTART) primerush_enter();
            } else {
                s_exit_request = true;
            }
        }
        return;
    }
    switch (ev->key) {
    case K_OK:                   /* 是质数 */
        pr_answer(true);
        break;
    case K_BACK:                 /* 不是质数(BACK 兼任否定键, 暂停走 P) */
    case K_SPACE:
        pr_answer(false);
        break;
    case K_PAUSE: {
        pause_sel_t sel;
        if (ui_pause_run(&sel)) {
            if (sel == PAUSE_RESTART) primerush_enter();
        } else {
            s_exit_request = true;
        }
        break;
    }
    case K_CHAR:
        if (ev->ch == 'n') { audio_select(); primerush_enter(); }
        break;
    case K_QUIT:
        s_exit_request = true;
        break;
    default:
        break;
    }
}

void primerush_exit(void) {}
