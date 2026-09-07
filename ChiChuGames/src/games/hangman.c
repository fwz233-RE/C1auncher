/* HANGMAN — 经典绞刑架猜词: 140 词词表(6-10 字母), 字母键直猜
 * 猜对填所有位; 猜错画一笔 + 计数, 6 错吊死显示答案
 * 键位: A-Z 直猜(大小写归一, 已猜忽略), BACK/P 暂停, Q 退出
 * 结束: OK/N 新局, BACK 退出 */
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
#include <string.h>

#define HM_MAX_LEN 10                 /* 最长单词 */
#define HM_MAX_ERR 6                  /* 吊死所需错误数 */
#define HM_SLOT_W  12                 /* 单词格宽(2x 字母 10px + 2px 间距) */
#define HM_SLOT_H  16
#define HM_WORD_Y  96                 /* 单词行顶 */
#define HM_WRONG_Y 126                /* 错字列表行 */

/* 内置词表 (140 x 6-10 字母, 全小写, 无重复) */
static const char hm_words[][HM_MAX_LEN + 1] = {
    "animal", "anchor", "answer", "autumn", "airport",
    "banana", "basket", "battery", "bicycle", "blanket",
    "bottle", "bridge", "bright", "brother", "butter",
    "butterfly", "birthday", "backpack", "baseball", "button",
    "cabbage", "cactus", "camera", "candle", "canyon",
    "captain", "castle", "celery", "channel", "cheese",
    "cherry", "chicken", "chocolate", "circle", "clever",
    "coffee", "computer", "corner", "cotton", "counter",
    "cousin", "cucumber", "customer", "danger", "desert",
    "diamond", "dinner",
    "dinosaur", "doctor", "dolphin", "dragon", "dragonfly",
    "elephant", "engine", "evening", "enormous", "feather",
    "flower", "forest", "football", "fortune", "fridge",
    "friend", "garden", "garlic", "gentle", "giraffe",
    "grandma", "grandpa", "guitar", "hamburger", "hammer",
    "harbor", "harvest", "headphone", "helicopter", "hollow",
    "hospital", "hundred", "important", "island", "jacket",
    "journey", "jungle", "kangaroo", "keyboard", "kingdom",
    "kitchen", "kitten", "ladder", "laptop", "lawyer",
    "lettuce", "library", "lobster", "machine", "magnet",
    "manager", "marble", "meadow", "microphone", "million",
    "mirror", "monkey", "monster", "morning", "mountain",
    "museum", "mushroom", "mustard", "national", "needle",
    "network", "octopus", "office", "orange", "oxygen",
    "palace", "pancake", "parents", "parrot", "pencil",
    "pepper", "picture", "pillow", "pineapple", "planet",
    "pocket", "popcorn", "possible", "potato", "pretty",
    "prisoner", "probably", "problem", "produce", "program",
    "project", "promise", "pumpkin", "purple", "question",
    "rabbit", "rainbow", "rocket", "saddle", "sailor",
    "sandwich", "school", "secret", "service", "shadow",
    "silver", "sister", "soccer", "soldier", "something",
    "special", "spider", "spinach", "spring", "square",
    "statue", "strawberry", "stream", "student", "summer",
    "sunflower", "sunset", "sweater", "teacher", "temple",
    "terrible", "thunder", "together", "tomato", "tomorrow",
    "tongue", "travel", "treasure", "triangle", "tunnel",
    "turtle", "umbrella", "universe", "valley", "vegetable",
    "victory", "violet", "violin", "volcano", "wallet",
    "walnut", "waterfall", "weather", "welcome", "window",
    "winter", "wizard", "yellow", "yesterday", "village",
};
#define HM_WORDS_N (sizeof(hm_words) / sizeof(hm_words[0]))

/* ---- 对局状态 ---- */
static char    hm_ans[HM_MAX_LEN + 1]; /* 谜底 */
static int     hm_len;                 /* 单词长度 6..10 */
static uint8_t hm_revealed[HM_MAX_LEN];/* 各位是否已揭示 */
static uint8_t hm_used[26];            /* 已猜字母 */
static int     hm_err;                 /* 错误数 0..6 */
static bool    hm_over;
static bool    hm_won;
static bool    hm_over_full;           /* 结束全刷只做一次 */
static rng_t   hm_rng;
static uint64_t hm_seed_cnt;           /* 换局换种子 */

void hangman_render(void);

/* 字母 k(0-25) 是否在谜底中 */
static bool hm_in_word(int k) {
    int i;
    for (i = 0; i < hm_len; i++)
        if (hm_ans[i] == (char)('a' + k)) return true;
    return false;
}

/* 从词表随机抽谜底 */
static void hm_pick_word(void) {
    /* rng_range 内部有 n<=1 早退 guard, 不会死循环 */
    uint32_t idx = rng_range(&hm_rng, (uint32_t)HM_WORDS_N);
    hm_len = (int)strlen(hm_words[idx]);
    memcpy(hm_ans, hm_words[idx], (size_t)hm_len + 1);
}

/* 猜一个字母: 已猜忽略; 对则全位填入, 错则一笔 + 计数 */
static void hm_guess(char ch) {
    int i, k = ch - 'a';
    if (k < 0 || k >= 26 || hm_used[k]) return;
    hm_used[k] = 1;
    if (hm_in_word(k)) {
        for (i = 0; i < hm_len; i++)
            if (hm_ans[i] == ch) hm_revealed[i] = 1;
        for (i = 0; i < hm_len; i++)
            if (!hm_revealed[i]) { audio_clear(); return; }   /* 未全揭示 */
        hm_over = true;
        hm_won = true;
        audio_win();
        led_fx_set(LED_FX_WIN);
    } else {
        hm_err++;
        if (hm_err >= HM_MAX_ERR) {
            hm_over = true;
            hm_won = false;
            audio_lose();
            led_fx_set(LED_FX_LOSE);
        } else {
            audio_error();
        }
    }
}

void hangman_enter(void) {
    memset(hm_ans, 0, sizeof(hm_ans));
    memset(hm_revealed, 0, sizeof(hm_revealed));
    memset(hm_used, 0, sizeof(hm_used));
    hm_len = 0;
    hm_err = 0;
    hm_over = false;
    hm_won = false;
    hm_over_full = false;
    hm_seed_cnt++;
    rng_seed(&hm_rng, (uint64_t)now_ms() ^ ((uint64_t)hm_seed_cnt << 32) ^ 0xE577ULL);
    hm_pick_word();
    hangman_render();
    disp_full();
}

void hangman_exit(void) {}

void hangman_tick(uint64_t now) {
    (void)now;   /* 本游戏无周期逻辑 */
}

/* 整数 DDA 画线(无浮点) */
static void hm_line(int x0, int y0, int x1, int y1) {
    int dx = x1 - x0;
    int dy = y1 - y0;
    int steps = dx < 0 ? -dx : dx;
    int sdy = dy < 0 ? -dy : dy;
    int i;
    if (sdy > steps) steps = sdy;
    if (steps == 0) { fb_pixel(x0, y0, true); return; }
    for (i = 0; i <= steps; i++)
        fb_pixel(x0 + dx * i / steps, y0 + dy * i / steps, true);
}

/* 实心圆盘(整数运算) */
static void hm_disk(int cx, int cy, int r) {
    int dy, dx;
    for (dy = -r; dy <= r; dy++)
        for (dx = -r; dx <= r; dx++)
            if (dx * dx + dy * dy <= r * r)
                fb_pixel(cx + dx, cy + dy, true);
}

/* 绞刑架: 6 笔 — 架/横/绳/头/身/腿, 按错误数逐笔绘制 */
static void hm_draw_gallows(void) {
    if (hm_err >= 1) {                 /* 1 架: 竖杆 + 底座 */
        fb_vline(30, 28, 80, true);    /* y 28..107 */
        fb_hline(26, 107, 12, true);
        fb_hline(26, 106, 12, true);
    }
    if (hm_err >= 2) {                 /* 2 横: 顶梁 */
        fb_hline(30, 28, 44, true);    /* x 30..73 */
    }
    if (hm_err >= 3) {                 /* 3 绳 */
        fb_vline(74, 28, 12, true);    /* y 28..39 */
    }
    if (hm_err >= 4) {                 /* 4 头 */
        hm_disk(74, 53, 9);
    }
    if (hm_err >= 5) {                 /* 5 身: 躯干 + 双臂 */
        fb_vline(74, 62, 22, true);    /* y 62..83 */
        hm_line(74, 67, 65, 72);
        hm_line(74, 67, 83, 72);
    }
    if (hm_err >= 6) {                 /* 6 腿 */
        hm_line(74, 83, 65, 94);
        hm_line(74, 83, 83, 94);
    }
}

/* 单词空格: 已揭示 2x 字母, 未揭示下划线 */
static void hm_draw_word(void) {
    int i, w = hm_len * HM_SLOT_W - 2;
    int x0 = (CCG_W - w) / 2;
    for (i = 0; i < hm_len; i++) {
        int x = x0 + i * HM_SLOT_W;
        if (hm_revealed[i]) {
            char b[2];
            b[0] = hm_ans[i];
            b[1] = 0;
            fb_text_scale2(x, HM_WORD_Y, b, true);
        } else {
            fb_hline(x + 2, HM_WORD_Y + HM_SLOT_H - 3, HM_SLOT_W - 4, true);
        }
    }
}

/* 已猜错字母列表(按字母序) */
static void hm_draw_wrong(void) {
    char buf[40];
    int n = 0, k;
    memcpy(buf, "WRONG:", 6);
    n = 6;
    for (k = 0; k < 26; k++) {
        if (hm_used[k] && !hm_in_word(k)) {
            buf[n++] = ' ';
            buf[n++] = (char)('a' + k);
        }
    }
    buf[n] = 0;
    if (n > 6) fb_text_center(HM_WRONG_Y, buf, true);
}

void hangman_render(void) {
    fb_clear(false);

    /* HUD 顶栏: 黑字白底 */
    if (hm_over) {
        char b[24];
        int i = 0;
        fb_fill_rect(0, 0, CCG_W, CCG_HUD_H - 1, false);
        fb_text(2, 1, hm_won ? "YOU WIN!" : "HANGED", true);
        if (!hm_won) {
            static const char lab[] = "ANSWER:";
            memcpy(b, lab, sizeof(lab) - 1);
            i = (int)sizeof(lab) - 1;
            memcpy(b + i, hm_ans, (size_t)hm_len);
            b[i + hm_len] = 0;
            fb_text(2, 9, b, true);
        }
        fb_text(CCG_W - 2 - text_width("OK/N:RETRY BACK:QUIT"), 1,
                "OK/N:RETRY BACK:QUIT", true);
        if (!hm_over_full) {
            hm_over_full = true;
            disp_force_full();
        }
    } else {
        char buf[8];
        fb_text_scale2(2, 1, "HANGMAN", true);
        buf[0] = 'E'; buf[1] = 'R'; buf[2] = 'R'; buf[3] = ' ';
        buf[4] = (char)('0' + hm_err);
        buf[5] = '/'; buf[6] = '6'; buf[7] = 0;
        fb_text_scale2(CCG_W - 2 - text_width(buf) * 2, 1, buf, true);
    }
    fb_hline(0, CCG_HUD_H - 1, CCG_W, true);

    /* 墙内: 绞刑架 + 单词 + 错字 */
    hm_draw_gallows();
    hm_draw_word();
    hm_draw_wrong();
}

void hangman_on_key(const key_event_t *ev) {
    if (ev->is_repeat) return;         /* 字母/确认键重复一律忽略 */
    if (hm_over) {
        if (ev->key == K_OK || (ev->key == K_CHAR && ev->ch == 'n'))
            hangman_enter();
        else if (ev->key == K_BACK || ev->key == K_QUIT)
            s_exit_request = true;
        return;
    }
    switch (ev->key) {
    case K_CHAR: {
        char ch = (char)ev->ch;
        if (ch >= 'A' && ch <= 'Z') ch = (char)(ch - 'A' + 'a');
        if (ch >= 'a' && ch <= 'z') hm_guess(ch);
        break;
    }
    case K_BACK:
    case K_PAUSE: {
        pause_sel_t sel;
        if (ui_pause_run(&sel)) {
            if (sel == PAUSE_RESTART) hangman_enter();
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
