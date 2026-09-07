/* WORD CHAIN — 单词接龙: 玩家与 AI 交替出词,
 * 每词首字母必须等于上一词末字母; 词必须在内置 200 词词库且未用过.
 * AI 按首字母索引随机选可用词(简单启发式); AI 无词可接 = 玩家赢;
 * 玩家无词可接 / 出坏词(不在词库 / 首字母不接 / 已用过) = 玩家输.
 * 界面: HUD 左标题右链长; 游戏区 NEXT 行(所需首字母, 2x 大字) +
 * 最近 3 个词(首字母反白显示连接点) + 输入格(光标黑块).
 * 键位: A-Z 输入, OK 提交, DEL 退格, BACK/P 暂停, Q 退出; 结束后 N 新局 */
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
#include <string.h>

#define WC_WORDS_N    200
#define WC_MAX_LEN    10      /* 词长上限(词库最长 7) */

#define WC_CELL_W     12      /* 输入格宽 */
#define WC_CELL_H     20      /* 输入格高 */
#define WC_CELL_PITCH 14      /* 格间距 */
#define WC_INPUT_Y    78
#define WC_HINT_Y     116

/* 词库: 200 词, 全小写, 3-10 字母, 无重复, 按首字母 a-z 分组 */
static const char wc_words[WC_WORDS_N][WC_MAX_LEN + 1] = {
    /* a */ "above", "angle", "animal", "ant", "apple", "arrow", "autumn", "award",
    /* b */ "banana", "bird", "boat", "book", "brain", "brave", "bridge", "butter",
    /* c */ "cake", "cat", "chair", "city", "cloud", "color", "crown", "crystal",
    /* d */ "dance", "desk", "dog", "door", "dragon", "dream", "drink",
    /* e */ "eagle", "earth", "egg", "eight", "empty", "engine", "enter", "extra",
    /* f */ "field", "fire", "fish", "flower", "forest", "friend", "fruit", "funny",
    /* g */ "game", "garden", "girl", "glass", "gold", "grape", "green",
    /* h */ "hammer", "happy", "hat", "heart", "hill", "honey", "horse", "house",
    /* i */ "ice", "idea", "image", "inch", "inside", "iron", "island", "ivory",
    /* j */ "jacket", "jam", "jelly", "jewel", "jolly", "judge", "juice", "jump",
    /* k */ "kernel", "key", "king", "kite", "knife", "koala", "known",
    /* l */ "ladder", "lake", "lamp", "lemon", "letter", "light", "lion", "lucky",
    /* m */ "magic", "milk", "mirror", "money", "moon", "mother", "mouse", "music",
    /* n */ "nature", "nest", "night", "noise", "north", "nose", "number", "nurse",
    /* o */ "ocean", "offer", "onion", "open", "orange", "outfit", "owner", "owl",
    /* p */ "paper", "park", "pen", "piano", "plant", "pocket", "prize", "purple",
    /* q */ "quart", "queen", "quick", "quiet", "quite", "quiz",
    /* r */ "rabbit", "rain", "rat", "ready", "river", "robot", "rocket", "round",
    /* s */ "seven", "silver", "star", "stone", "street", "strong", "sugar", "sun",
    /* t */ "table", "tea", "think", "tiger", "tower", "train", "tree", "turtle",
    /* u */ "uncle", "under", "unique", "unit", "upper", "urban", "upside", "use",
    /* v */ "van", "value", "vase", "video", "village", "violet", "visit", "voice",
    /* w */ "watch", "water", "whale", "wind", "win", "winter", "world",
    /* x */ "xavier", "xenon", "xenia", "xeric", "xerox", "xylem",
    /* y */ "yacht", "year", "yellow", "yes", "yield", "yodel", "young", "yummy",
    /* z */ "zeal", "zebra", "zero", "zesty", "zinc", "zipper", "zone", "zoo",
};

/* 结束原因 */
typedef enum {
    WC_RS_LINK = 0,   /* 首字母不接上一词 */
    WC_RS_BAD,        /* 词不在词库 */
    WC_RS_USED,       /* 词已用过 */
    WC_RS_NOMOVE,     /* 无词可接 */
    WC_RS_AI_STUCK,   /* AI 无词可接 → 玩家赢 */
} wc_reason_t;

/* ---- 对局状态 ---- */
static int      wc_start[27];       /* 首字母 c 的词在词库起始下标; [26]=N */
static bool     wc_idx_ok;
static uint8_t  wc_chain[WC_WORDS_N];  /* 链上词下标(顺序) */
static uint8_t  wc_used[WC_WORDS_N];   /* 词是否已用过 */
static int      wc_chain_len;
static char     wc_last_c;          /* 上一词末字母 = 下一词必须的首字母 */
static char     wc_buf[WC_MAX_LEN + 1];
static int      wc_blen;
static bool     wc_over;
static bool     wc_won;
static int      wc_reason;
static bool     wc_over_full;       /* 结束全刷只做一次 */
static rng_t    wc_rng;
static uint64_t wc_seed_cnt;

/* 词库首字母索引: 词库按 a-z 分组, 记录每字母首词下标 */
static void wc_build_index(void) {
    int i, c;
    if (wc_idx_ok) return;
    wc_start[26] = WC_WORDS_N;
    for (c = 0; c < 26; c++) wc_start[c] = WC_WORDS_N;
    for (i = 0; i < WC_WORDS_N; i++) {
        c = wc_words[i][0] - 'a';
        if (c >= 0 && c < 26 && wc_start[c] == WC_WORDS_N) wc_start[c] = i;
    }
    for (c = 25; c >= 0; c--)       /* 缺字母回填为后一字母起点 */
        if (wc_start[c] == WC_WORDS_N) wc_start[c] = wc_start[c + 1];
    wc_idx_ok = true;
}

/* 十进制整数写缓冲, 返回长度 */
static int wc_itoa(char *b, int v) {
    char t[16];
    int i = 0, j;
    if (v == 0) { b[0] = '0'; b[1] = 0; return 1; }
    while (v > 0 && i < 14) { t[i++] = (char)('0' + v % 10); v /= 10; }
    for (j = 0; j < i; j++) b[j] = t[i - 1 - j];
    b[i] = 0;
    return i;
}

static void wc_draw_next(void);
static void wc_draw_chain(void);
static void wc_draw_input(void);
static void wc_draw_hint(void);
void wordchain_render(void);

/* 词库查找, 命中返回下标否则 -1 */
static int wc_find_word(const char *w) {
    int i;
    for (i = 0; i < WC_WORDS_N; i++)
        if (strcmp(wc_words[i], w) == 0) return i;
    return -1;
}

/* 以字母 c 开头且未用过的词数 */
static int wc_avail_for(char c) {
    int lo, hi, n = 0, i;
    if (c < 'a' || c > 'z') return 0;
    lo = wc_start[c - 'a'];
    hi = wc_start[c - 'a' + 1];
    for (i = lo; i < hi; i++) if (!wc_used[i]) n++;
    return n;
}

/* 出词入链; 新必需首字母 = 该词末字母 */
static void wc_place(int idx) {
    wc_chain[wc_chain_len] = (uint8_t)idx;
    wc_used[idx] = 1;
    wc_chain_len++;
    wc_last_c = wc_words[idx][(int)strlen(wc_words[idx]) - 1];
}

/* AI 回合: 该字母组内随机选未用过词; 无可接返回 false */
static bool wc_ai_move(void) {
    char c = wc_last_c;
    int lo, hi, n, pick = -1, guard, i;
    if (c < 'a' || c > 'z') return false;
    lo = wc_start[c - 'a'];
    hi = wc_start[c - 'a' + 1];
    n = hi - lo;
    if (n <= 0 || wc_avail_for(c) == 0) return false;
    guard = 64;
    do {
        pick = lo + (int)rng_range(&wc_rng, (uint32_t)n);
    } while (wc_used[pick] && --guard > 0);
    if (wc_used[pick]) {            /* guard 耗尽: 顺序取第一个可用 */
        for (i = lo; i < hi; i++)
            if (!wc_used[i]) { pick = i; break; }
    }
    wc_place(pick);
    return true;
}

static void wc_finish(bool won, int reason) {
    wc_over = true;
    wc_won = won;
    wc_reason = reason;
    wc_over_full = false;
    if (won) {
        audio_win();
        led_fx_set(LED_FX_WIN);
    } else {
        audio_lose();
        led_fx_set(LED_FX_LOSE);
    }
}

/* 结束原因 → 显示文本 */
static const char *wc_reason_str(void) {
    switch (wc_reason) {
    case WC_RS_LINK:    return "BAD LINK";
    case WC_RS_BAD:     return "BAD WORD";
    case WC_RS_USED:    return "USED WORD";
    case WC_RS_NOMOVE:  return "NO WORDS";
    case WC_RS_AI_STUCK: return "AI STUCK";
    default:            return "?";
    }
}

/* 新对局 */
static void wc_new_game(void) {
    wc_build_index();
    memset(wc_used, 0, sizeof(wc_used));
    wc_chain_len = 0;
    wc_blen = 0;
    wc_buf[0] = '\0';
    wc_last_c = 0;
    wc_over = false;
    wc_won = false;
    wc_reason = WC_RS_LINK;
    wc_over_full = false;
}

/* OK 提交: 校验首字母 / 词库 / 未用过, 通过则入链并轮到 AI */
static void wc_submit(void) {
    int idx;
    if (wc_over || wc_blen == 0) return;
    wc_buf[wc_blen] = '\0';
    if (wc_chain_len > 0 && wc_buf[0] != wc_last_c) {
        wc_finish(false, WC_RS_LINK);
        return;
    }
    idx = wc_find_word(wc_buf);
    if (idx < 0) { wc_finish(false, WC_RS_BAD); return; }
    if (wc_used[idx]) { wc_finish(false, WC_RS_USED); return; }
    wc_place(idx);
    wc_blen = 0;
    wc_buf[0] = '\0';
    audio_clear();
    if (!wc_ai_move()) {            /* AI 无词可接 → 玩家赢 */
        wc_finish(true, WC_RS_AI_STUCK);
        return;
    }
    if (wc_avail_for(wc_last_c) == 0)   /* 玩家无词可接 → 输 */
        wc_finish(false, WC_RS_NOMOVE);
}

void wordchain_enter(void) {
    wc_seed_cnt++;
    rng_seed(&wc_rng, (uint64_t)now_ms() ^ ((uint64_t)wc_seed_cnt << 32) ^ 0x7C0FULL);
    wc_new_game();
    wordchain_render();
    disp_full();
}

void wordchain_exit(void) {}

void wordchain_tick(uint64_t now) {
    (void)now;   /* 本游戏无周期逻辑 */
}

/* 所需首字母行: 2x 大字 + 标签 */
static void wc_draw_next(void) {
    char b[2];
    if (wc_chain_len == 0) {
        fb_text(6, 26, "FIRST WORD FREE", true);
        return;
    }
    fb_text(6, 26, "NEXT", true);
    fb_symbol(40, 25, CG_ARROW_RT, true);
    b[0] = (char)(wc_last_c - 'a' + 'A');
    b[1] = 0;
    fb_text_scale2(54, 19, b, true);
}

/* 当前链最后 3 词; 首字母反白显示连接点, 最新一行带箭头 */
static void wc_draw_chain(void) {
    int ci, r, first;
    first = wc_chain_len > 3 ? wc_chain_len - 3 : 0;
    r = 0;
    for (ci = first; ci < wc_chain_len; ci++, r++) {
        const char *w = wc_words[wc_chain[ci]];
        int len = (int)strlen(w);
        char up[WC_MAX_LEN + 1];
        char c1[2];
        char num[8];
        int nl, x = 6, y = 38 + r * 10, i;
        if (ci == wc_chain_len - 1) {
            fb_symbol(x, y + 1, CG_ARROW_RT, true);
            x += 12;
        }
        nl = wc_itoa(num, ci + 1);
        fb_text(x, y, num, true);
        x += nl * FONT_ADV;
        fb_text(x, y, ".", true);
        x += FONT_ADV + 2;
        for (i = 0; i < len; i++) up[i] = (char)(w[i] - 'a' + 'A');
        up[len] = 0;
        c1[0] = up[0];
        c1[1] = 0;
        fb_text_inv(x, y, c1);
        x += FONT_ADV;
        fb_text(x, y, up + 1, true);
    }
}

/* 输入格: 字母格 + 光标黑块(空位反色框) */
static void wc_draw_input(void) {
    int total = WC_MAX_LEN * WC_CELL_PITCH - 2;
    int x0 = (CCG_W - total) / 2;
    int y = WC_INPUT_Y;
    int i;
    for (i = 0; i < WC_MAX_LEN; i++) {
        int x = x0 + i * WC_CELL_PITCH;
        if (i == wc_blen) {
            fb_fill_rect(x, y, WC_CELL_W, WC_CELL_H, true);
            fb_stroke_rect(x + 1, y + 1, WC_CELL_W - 2, WC_CELL_H - 2, false);
        } else {
            fb_stroke_rect(x, y, WC_CELL_W, WC_CELL_H, true);
            if (i < wc_blen) {
                char b[2];
                b[0] = (char)(wc_buf[i] - 'a' + 'A');
                b[1] = 0;
                fb_text_scale2(x + 1, y + 3, b, true);
            }
        }
    }
}

static void wc_draw_hint(void) {
    fb_text_center(WC_HINT_Y, "A-Z TYPE  OK:PLAY  DEL:BSP  BACK:PAUSE", true);
}

void wordchain_render(void) {
    fb_clear(false);

    if (wc_over) {
        /* HUD 两行: 结果 / 原因 + CHAIN n, 右上重开提示 */
        char b[20];
        int n;
        fb_fill_rect(0, 0, CCG_W, CCG_HUD_H - 1, false);
        fb_text(2, 1, wc_won ? "YOU WIN!" : "YOU LOSE", true);
        fb_text(CCG_W - 2 - text_width("OK/N:RETRY BACK:QUIT"), 1,
                "OK/N:RETRY BACK:QUIT", true);
        fb_text(2, 9, wc_reason_str(), true);
        memcpy(b, "CHAIN ", 6);
        n = 6;
        n += wc_itoa(b + n, wc_chain_len);
        b[n] = 0;
        fb_text(CCG_W - 2 - text_width(b), 9, b, true);
        fb_hline(0, CCG_HUD_H - 1, CCG_W, true);
        if (!wc_over_full) {
            wc_over_full = true;
            disp_force_full();
        }
    } else {
        /* HUD: 左标题黑字, 右 CHAIN n */
        char b[16];
        int n;
        fb_text_scale2(2, 1, "WORD CHAIN", true);
        memcpy(b, "CHAIN ", 6);
        n = 6;
        n += wc_itoa(b + n, wc_chain_len);
        b[n] = 0;
        fb_text_scale2(CCG_W - 2 - text_width(b) * 2, 1, b, true);
        fb_hline(0, CCG_HUD_H - 1, CCG_W, true);
    }

    wc_draw_next();
    wc_draw_chain();
    if (!wc_over) {
        wc_draw_input();
        wc_draw_hint();
    }
}

void wordchain_on_key(const key_event_t *ev) {
    /* 长按 DEL 连续退格, 其余键忽略重复 */
    if (ev->is_repeat && ev->key != K_DEL) return;
    if (wc_over) {
        if (ev->key == K_OK || (ev->key == K_CHAR && ev->ch == 'n'))
            wc_new_game();
        else if (ev->key == K_BACK || ev->key == K_QUIT)
            s_exit_request = true;
        return;
    }
    switch (ev->key) {
    case K_CHAR: {
        char ch = (char)ev->ch;
        if (ch >= 'A' && ch <= 'Z') ch = (char)(ch - 'A' + 'a');   /* 大小写统一 */
        if (ch >= 'a' && ch <= 'z' && wc_blen < WC_MAX_LEN) {
            wc_buf[wc_blen++] = ch;
            audio_move();
        }
        break;
    }
    case K_DEL:
        if (wc_blen > 0) wc_buf[--wc_blen] = '\0';
        break;
    case K_OK:
        wc_submit();
        break;
    case K_PAUSE:
    case K_BACK: {
        pause_sel_t sel;
        if (ui_pause_run(&sel)) {
            if (sel == PAUSE_RESTART) wc_new_game();
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
