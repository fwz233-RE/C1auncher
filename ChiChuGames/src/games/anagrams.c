/* ANAGRAMS — 字母重排拼词: 主词(6-9 字母)乱序显示为字母堆,
 * 光标在堆上移动, OK 选中字母进答案区, DEL/BACK 撤销(最后一位放回堆),
 * 拼出主词即赢(得分 10x 词长); 提示行实时显示已放对位置的字母数 RIGHT n/LEN
 * 键位: LEFT/RIGHT 移动光标, OK 选中, DEL/BACK 撤销, N 新局, P 暂停, Q 退出 */
#include "game.h"
#include "../config.h"
#include "../gfx/canvas.h"
#include "../gfx/font.h"
#include "../gfx/pattern.h"
#include "../platform/audio.h"
#include "../platform/display.h"
#include "../platform/input.h"
#include "../platform/led.h"
#include "../platform/time.h"
#include "../rng.h"
#include <string.h>

#define AN_MAX_LEN   9    /* 词表最长单词 */
#define AN_SLOT_W    24   /* 字母格宽(2x 字母 10px + 边距) */
#define AN_ANSWER_H  24
#define AN_POOL_H    28
#define AN_ANSWER_Y  24   /* 答案区行顶 */
#define AN_HINT_Y    52   /* RIGHT n/LEN 提示行 */
#define AN_POOL_Y    84   /* 字母堆行顶 */
#define AN_FOOT_Y    120  /* 底部按键提示 */

/* 内置词表 (30 x 6-9 字母, 全小写, 无重复) */
static const char an_words[][AN_MAX_LEN + 1] = {
    "planet", "garden", "window", "silver", "summer",
    "travel", "orange", "purple", "bridge", "forest",
    "jungle", "ladder", "market", "pocket", "shadow",
    "valley", "cabbage", "journey", "pumpkin", "dolphin",
    "diamond", "picture", "mountain", "sandwich", "question",
    "keyboard", "birthday", "weather", "waterfall", "chocolate",
};
#define AN_WORDS_N (sizeof(an_words) / sizeof(an_words[0]))

/* ---- 对局状态 ---- */
static char     an_target[AN_MAX_LEN + 1]; /* 谜底主词 */
static int      an_len;                    /* 词长 6..9 */
static char     an_pool[AN_MAX_LEN];       /* 乱序字母堆 */
static uint8_t  an_taken[AN_MAX_LEN];      /* 堆中字母是否已入答案 */
static char     an_ans[AN_MAX_LEN];        /* 答案区字母(按放置顺序) */
static int      an_from[AN_MAX_LEN];       /* 答案第 i 位来自的堆下标 */
static int      an_ans_len;                /* 已放字母数 0..an_len */
static int      an_curs;                   /* 字母堆光标 */
static int      an_score;                  /* 累计得分 */
static bool     an_over;
static bool     an_over_full;              /* 结束全刷只做一次 */
static rng_t    an_rng;
static uint64_t an_seed_cnt;               /* 换局换种子 */

void anagrams_render(void);

/* 堆洗牌: Fisher-Yates; 与主词全同则重洗(guard 限次, 不会死循环) */
static void an_shuffle(void) {
    int guard = 8;
    do {
        int i;
        for (i = an_len - 1; i > 0; i--) {
            uint32_t j = rng_range(&an_rng, (uint32_t)(i + 1));
            char t = an_pool[i];
            an_pool[i] = an_pool[j];
            an_pool[j] = t;
        }
    } while (memcmp(an_pool, an_target, (size_t)an_len) == 0 && --guard > 0);
}

/* 从词表随机抽词 + 洗牌开新局 */
static void an_new_round(void) {
    uint32_t idx = rng_range(&an_rng, (uint32_t)AN_WORDS_N);
    an_len = (int)strlen(an_words[idx]);
    memcpy(an_target, an_words[idx], (size_t)an_len + 1);
    memcpy(an_pool, an_target, (size_t)an_len);
    memset(an_taken, 0, sizeof(an_taken));
    memset(an_ans, 0, sizeof(an_ans));
    memset(an_from, 0, sizeof(an_from));
    an_ans_len = 0;
    an_curs = 0;
    an_over = false;
    an_over_full = false;
    an_shuffle();
}

/* 已放对位置的字母数(提示: RIGHT n/LEN) */
static int an_right_count(void) {
    int i, n = 0;
    for (i = 0; i < an_ans_len; i++)
        if (an_ans[i] == an_target[i]) n++;
    return n;
}

/* 光标移动 d 步并停在下一个可取字母上(自动跳过已取, 有界不绕死) */
static void an_move_cursor(int d) {
    int i;
    for (i = 0; i < an_len; i++) {
        an_curs = (an_curs + d + an_len) % an_len;
        if (!an_taken[an_curs]) return;
    }
}

/* OK: 光标字母进答案区; 放满且拼成主词即赢 */
static bool an_place(void) {
    if (an_over || an_ans_len >= an_len || an_taken[an_curs]) return false;
    an_from[an_ans_len] = an_curs;
    an_ans[an_ans_len] = an_pool[an_curs];
    an_ans_len++;
    an_taken[an_curs] = 1;
    if (an_ans_len >= an_len) {
        if (memcmp(an_ans, an_target, (size_t)an_len) == 0) {
            an_over = true;
            an_score += an_len * 10;
        }
        return true;
    }
    an_move_cursor(1);
    return true;
}

/* DEL/BACK: 撤销 — 答案最后一位放回字母堆, 光标回到该字母 */
static void an_undo(void) {
    int src;
    if (an_over || an_ans_len <= 0) return;
    an_ans_len--;
    src = an_from[an_ans_len];
    an_ans[an_ans_len] = 0;
    an_taken[src] = 0;
    an_curs = src;
}

void anagrams_enter(void) {
    an_score = 0;
    an_seed_cnt++;
    rng_seed(&an_rng, (uint64_t)now_ms() ^ ((uint64_t)an_seed_cnt << 32) ^ 0x41A9ULL);
    an_new_round();
    anagrams_render();
    disp_full();
}

void anagrams_exit(void) {}

void anagrams_tick(uint64_t now) {
    (void)now;   /* 本游戏无周期逻辑 */
}

/* 十进制整数写缓冲, 返回长度 */
static int an_itoa(char *b, int v) {
    char t[16];
    int i = 0, j;
    if (v == 0) { b[0] = '0'; b[1] = 0; return 1; }
    while (v > 0 && i < 14) { t[i++] = (char)('0' + v % 10); v /= 10; }
    for (j = 0; j < i; j++) b[j] = t[i - 1 - j];
    b[i] = 0;
    return i;
}

/* 格内 2x 字母: 10x14 居中 */
static void an_draw_letter(int x, int y, int h, char ch, bool inv) {
    char b[2];
    b[0] = ch;
    b[1] = 0;
    fb_text_scale2(x + (AN_SLOT_W - 10) / 2, y + (h - 14) / 2, b, inv);
}

void anagrams_render(void) {
    int i, x0;
    fb_clear(false);

    /* HUD 顶栏: 左 ANAGRAMS, 右词长; 结束改两行结果 */
    if (an_over) {
        char b[24];
        int n = 0;
        fb_fill_rect(0, 0, CCG_W, CCG_HUD_H - 1, false);
        fb_text(2, 1, "YOU WIN!", true);
        memcpy(b, "SCORE ", 6);
        n = 6;
        n += an_itoa(b + n, an_score);
        b[n++] = ' ';
        b[n++] = '+';
        n += an_itoa(b + n, an_len * 10);
        b[n] = 0;
        fb_text(2, 9, b, true);
        fb_text(CCG_W - 2 - text_width("OK/N:RETRY BACK:QUIT"), 1,
                "OK/N:RETRY BACK:QUIT", true);
        if (!an_over_full) {
            an_over_full = true;
            disp_force_full();
        }
    } else {
        char b[8];
        fb_text_scale2(2, 1, "ANAGRAMS", true);
        b[0] = 'L'; b[1] = 'E'; b[2] = 'N'; b[3] = ' ';
        b[4] = (char)('0' + an_len);
        b[5] = 0;
        fb_text_scale2(CCG_W - 2 - text_width(b) * 2, 1, b, true);
    }
    fb_hline(0, CCG_HUD_H - 1, CCG_W, true);

    x0 = (CCG_W - an_len * AN_SLOT_W) / 2;

    /* 答案区: 已放字母, 未放空格 + 下划线 */
    for (i = 0; i < an_len; i++) {
        int x = x0 + i * AN_SLOT_W;
        fb_stroke_rect(x, AN_ANSWER_Y, AN_SLOT_W, AN_ANSWER_H, true);
        if (i < an_ans_len) {
            an_draw_letter(x, AN_ANSWER_Y, AN_ANSWER_H, an_ans[i], false);
        } else {
            fb_hline(x + 4, AN_ANSWER_Y + AN_ANSWER_H - 4, AN_SLOT_W - 8, true);
        }
    }

    /* 提示行: 已放对位置的字母数 */
    if (!an_over) {
        char hb[16];
        int n = an_right_count();
        hb[0] = 'R'; hb[1] = 'I'; hb[2] = 'G'; hb[3] = 'H'; hb[4] = 'T';
        hb[5] = ' ';
        hb[6] = (char)('0' + n);
        hb[7] = '/';
        hb[8] = (char)('0' + an_len);
        hb[9] = 0;
        fb_text_center(AN_HINT_Y, hb, true);
    }

    /* 字母堆: 已取格点纹, 光标格黑底 */
    for (i = 0; i < an_len; i++) {
        int x = x0 + i * AN_SLOT_W;
        bool taken = an_taken[i] != 0;
        bool cur = (i == an_curs);
        if (cur) {
            fb_fill_rect(x, AN_POOL_Y, AN_SLOT_W, AN_POOL_H, true);
        } else if (taken) {
            fb_fill_tile(x, AN_POOL_Y, AN_SLOT_W, AN_POOL_H, pat_get(PAT_DOT_SPARSE));
            fb_stroke_rect(x, AN_POOL_Y, AN_SLOT_W, AN_POOL_H, true);
        } else {
            fb_stroke_rect(x, AN_POOL_Y, AN_SLOT_W, AN_POOL_H, true);
        }
        if (!taken) an_draw_letter(x, AN_POOL_Y, AN_POOL_H, an_pool[i], cur);
    }
    /* 光标(黑格白边)在所有格画完之后再画 */
    if (!an_over && an_ans_len < an_len) {
        int x = x0 + an_curs * AN_SLOT_W;
        fb_stroke_rect_thick(x + 1, AN_POOL_Y + 1, AN_SLOT_W - 2, AN_POOL_H - 2, 2, false);
    }

    /* 底部按键提示(墙内, 非结束提示) */
    if (!an_over) fb_text_center(AN_FOOT_Y, "OK:PICK DEL/BACK:UNDO N:NEW", true);
}

void anagrams_on_key(const key_event_t *ev) {
    /* 重复键只响应方向移动 */
    if (ev->is_repeat && ev->key != K_LEFT && ev->key != K_RIGHT) return;
    if (an_over) {
        if (ev->key == K_OK || (ev->key == K_CHAR && ev->ch == 'n'))
            an_new_round();
        else if (ev->key == K_BACK || ev->key == K_QUIT)
            s_exit_request = true;
        return;
    }
    switch (ev->key) {
    case K_LEFT:
        an_move_cursor(-1);
        audio_tick();
        break;
    case K_RIGHT:
        an_move_cursor(1);
        audio_tick();
        break;
    case K_OK:
        if (an_place()) {
            if (an_over) {
                audio_win();
                led_fx_set(LED_FX_WIN);
            } else {
                audio_select();
            }
        } else {
            audio_error();
        }
        break;
    case K_DEL:
    case K_BACK:
        an_undo();
        audio_move();
        break;
    case K_CHAR:
        if (ev->ch == 'n') { audio_select(); an_new_round(); }
        break;
    case K_PAUSE: {
        pause_sel_t sel;
        if (ui_pause_run(&sel)) {
            if (sel == PAUSE_RESTART) an_new_round();
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
