/* KLONDIKE — 经典接龙纸牌(简化版: 单张移动, 发牌堆翻牌)
 * 布局: 顶行 = 发牌堆/废牌堆/4 目标堆, 下方 7 列表格(第 i 列 i 张, 顶牌翻开)
 * 规则: 废牌堆顶或表格翻开牌 → 目标堆(同花色递增)或表格(红黑交替递减, K 可落空列)
 * 操作: 方向/WASD 移动光标(顶行 6 格/表格 7 列), OK 选中/移动(选中高亮),
 *       OK(发牌堆) 或 BACK 翻牌, N 新局, Q 退出, P 暂停; 52 张全入目标堆 = WIN
 * 静态前缀: kl_ (测试包含冲突隔离) */
#include "game.h"
#include "../config.h"
#include "../gfx/canvas.h"
#include "../gfx/font.h"
#include "../gfx/pattern.h"
#include "../platform/display.h"
#include "../rng.h"
#include "../platform/time.h"
#include "../platform/audio.h"
#include "../platform/led.h"

/* ---- 布局: 24x32 牌, 列距 28, 7 列横向居中 ---- */
#define KL_CW 24
#define KL_CH 32
#define KL_CS 28
#define KL_OX 50                       /* (296 - (7*28-4)) / 2 */
#define KL_OY_TOP 20                   /* 顶行(发牌/废牌/目标) */
#define KL_OY_TAB 56                   /* 表格区顶 */
#define KL_TAB_H ((int)CCG_H - KL_OY_TAB) /* 96 */
#define KL_FU_STEP 12                  /* 翻开牌重叠: 露出下张点数 */
#define KL_FD_STEP 2                   /* 扣牌重叠: 只露条 */

#define KL_SUIT_HEART 0
#define KL_SUIT_DIAMOND 1
#define KL_SUIT_SPADE 2
#define KL_SUIT_CLUB 3

#define KL_SRC_WASTE 7                 /* 选中源: 0..6=表格列, 7=废牌堆 */
#define KL_SRC_DECK 8                  /* 发牌堆(OK=翻牌, 非移动源) */

static uint8_t kl_deck[52];
static int kl_deck_n;
static uint8_t kl_waste[52];
static int kl_waste_n;
static uint8_t kl_found[4][13];
static int kl_found_n[4];
static uint8_t kl_tab[7][52];
static int kl_tab_n[7];
static int kl_tab_fd[7];               /* 堆底扣牌数(翻开数为 n-fd) */
static int kl_row, kl_col;             /* 光标: 0=顶行(6 格) 1=表格(7 列) */
static int kl_sel_src;                 /* 选中源, -1=无 */
static uint32_t kl_moves;
static bool kl_over, kl_over_full;
static rng_t kl_rng;

static const char *kl_rank_str[13] = { "A", "2", "3", "4", "5", "6", "7", "8",
                                       "9", "10", "J", "Q", "K" };
static const int kl_suit_sym[4] = { CG_HEART, CG_DIAMOND, CG_SPADE, CG_CLUB };

static int kl_rank(int card) { return card % 13 + 1; }
static int kl_suit(int card) { return card / 13; }
static int kl_color(int card) { return card / 13 < 2 ? 1 : 0; }  /* 红/黑 */

/* ---- 发牌: 7 列 1..7 张(顶牌翻开), 余 24 张入发牌堆 ---- */
static void kl_deal(void) {
    uint8_t deck[52];
    int i, c, k, p = 0;
    for (i = 0; i < 52; i++) deck[i] = (uint8_t)i;
    for (i = 51; i > 0; i--) {
        uint32_t j = rng_range(&kl_rng, (uint32_t)i + 1);
        uint8_t t = deck[i];
        deck[i] = deck[j];
        deck[j] = t;
    }
    for (c = 0; c < 7; c++) {
        kl_tab_n[c] = c + 1;
        kl_tab_fd[c] = c;
        for (k = 0; k <= c; k++) kl_tab[c][k] = deck[p++];
    }
    kl_deck_n = 52 - p;
    for (i = 0; i < kl_deck_n; i++) kl_deck[i] = deck[p + i];
}

static void kl_reset_state(void) {
    kl_waste_n = 0;
    int f;
    for (f = 0; f < 4; f++) kl_found_n[f] = 0;
    kl_row = 1;
    kl_col = 0;
    kl_sel_src = -1;
    kl_moves = 0;
    kl_over = false;
    kl_over_full = false;
}

/* ---- 规则判定 ---- */
static bool kl_can_give(int card, int dst_col, int dst_found) {
    if (dst_col >= 0) {
        int n = kl_tab_n[dst_col];
        if (n == 0) return kl_rank(card) == 13;        /* 空列只收 K */
        int top = kl_tab[dst_col][n - 1];
        return kl_color(card) != kl_color(top) &&
               kl_rank(card) == kl_rank(top) - 1;
    }
    if (dst_found >= 0) {
        int n = kl_found_n[dst_found];
        if (n == 0) return kl_suit(card) == dst_found && kl_rank(card) == 1;
        return kl_suit(card) == dst_found &&
               kl_rank(card) == n + 1;
    }
    return false;
}

/* src: 0..6=表格列, 7=废牌堆; 目的: dst_col>=0 表格列, 否则 dst_found 目标堆 */
static bool kl_move(int src, int dst_col, int dst_found) {
    if (dst_col >= 0 && src < 7 && dst_col == src) return false;
    int card = (src == KL_SRC_WASTE) ? kl_waste[kl_waste_n - 1]
                                     : kl_tab[src][kl_tab_n[src] - 1];
    if (!kl_can_give(card, dst_col, dst_found)) return false;
    if (src == KL_SRC_WASTE) {
        kl_waste_n--;
    } else {
        kl_tab_n[src]--;
        if (kl_tab_fd[src] > 0 && kl_tab_fd[src] == kl_tab_n[src])
            kl_tab_fd[src]--;                          /* 露出的扣牌翻开 */
    }
    if (dst_col >= 0) {
        kl_tab[dst_col][kl_tab_n[dst_col]++] = (uint8_t)card;
    } else {
        kl_found[dst_found][kl_found_n[dst_found]++] = (uint8_t)card;
    }
    kl_moves++;
    int won = 1, f;
    for (f = 0; f < 4; f++)
        if (kl_found_n[f] != 13) { won = 0; break; }
    if (won) {
        kl_over = true;
        audio_win();
        led_fx_set(LED_FX_WIN);
    } else {
        audio_move();
    }
    return true;
}

/* 发牌堆翻一张到废牌堆; 发牌堆空时把废牌堆反序回收 */
static void kl_flip(void) {
    if (kl_over) return;
    if (kl_deck_n > 0) {
        kl_waste[kl_waste_n++] = kl_deck[--kl_deck_n];
    } else if (kl_waste_n > 0) {
        int i;
        for (i = 0; i < kl_waste_n; i++)
            kl_deck[i] = kl_waste[kl_waste_n - 1 - i];
        kl_deck_n = kl_waste_n;
        kl_waste_n = 0;
    } else {
        return;
    }
    kl_moves++;
    audio_select();
}

/* ---- 光标导航: 顶行 6 格(0 发牌 1 废牌 2..5 目标), 表格 7 列 ---- */
static void kl_cursor_move(const key_event_t *ev) {
    int dx = 0, dy = 0;
    switch (ev->key) {
    case K_LEFT: dx = -1; break;
    case K_RIGHT: dx = 1; break;
    case K_UP: dy = -1; break;
    case K_DOWN: dy = 1; break;
    case K_CHAR:
        if (ev->ch == 'a') dx = -1;
        else if (ev->ch == 'd') dx = 1;
        else if (ev->ch == 'w') dy = -1;
        else if (ev->ch == 's') dy = 1;
        break;
    default: break;
    }
    if (dx) {
        int slots = (kl_row == 0) ? 6 : 7;
        kl_col = (kl_col + dx + slots) % slots;
    }
    if (dy) {
        kl_row = dy < 0 ? 0 : 1;
        if (kl_row == 0 && kl_col > 5) kl_col = 5;
    }
}

/* 光标格对应的可移动源: -1=无, 0..6=表格列, 7=废牌堆, 8=发牌堆(翻牌) */
static int kl_src_at_cursor(void) {
    if (kl_row == 0) {
        if (kl_col == 0) return KL_SRC_DECK;
        if (kl_col == 1 && kl_waste_n > 0) return KL_SRC_WASTE;
        return -1;
    }
    if (kl_tab_n[kl_col] > 0 && kl_tab_fd[kl_col] < kl_tab_n[kl_col])
        return kl_col;
    return -1;
}

/* 光标格的移动目的: dst_col>=0 表格列 / dst_found>=0 目标堆 / 否则不可达 */
static void kl_dst_at_cursor(int *dst_col, int *dst_found) {
    *dst_col = -1;
    *dst_found = -1;
    if (kl_row == 0) {
        if (kl_col >= 2) *dst_found = kl_col - 2;
    } else {
        *dst_col = kl_col;
    }
}

/* ---- 绘制 ---- */
static void kl_draw_suit2(int x, int y, int suit, bool black) {
    const uint8_t *g = font_symbols[kl_suit_sym[suit]];
    int j, i;
    for (j = 0; j < FONT_H; j++)
        for (i = 0; i < FONT_W; i++)
            if (g[j] & (1u << i))
                fb_fill_rect(x + i * 2, y + j * 2, 2, 2, black);
}

static void kl_draw_card(int x, int y, int card, bool faceup, bool invert) {
    fb_fill_rect(x, y, KL_CW, KL_CH, !invert);
    fb_stroke_rect(x, y, KL_CW, KL_CH, invert);
    if (faceup) {
        fb_text(x + 2, y + 2, kl_rank_str[kl_rank(card) - 1], invert);
        kl_draw_suit2(x + (KL_CW - 10) / 2, y + 11, kl_suit(card), invert);
    } else if (!invert) {
        fb_fill_tile(x + 3, y + 3, KL_CW - 6, KL_CH - 6, pat_get(PAT_CROSS));
    }
}

/* 空格(空列/空目标/空发牌堆): 细框, 光标格反白 */
static void kl_draw_slot(int x, int y, bool cursor) {
    if (cursor) {
        fb_fill_rect(x, y, KL_CW, KL_CH, true);
        fb_stroke_rect(x, y, KL_CW, KL_CH, false);
    } else {
        fb_stroke_rect(x, y, KL_CW, KL_CH, true);
    }
}

/* 一列表格: 顶牌锚在区域底部向上叠; 超高的旧牌从堆底裁掉 */
static void kl_draw_pile(int c, int x) {
    int n = kl_tab_n[c], fd = kl_tab_fd[c];
    bool cur = (kl_row == 1 && kl_col == c);
    if (n == 0) { kl_draw_slot(x, KL_OY_TAB, cur); return; }
    int fu = n - fd;                    /* 翻开张数 */
    int fu_shown = fu;
    while (fu_shown > 1 && KL_CH + (fu_shown - 1) * KL_FU_STEP > KL_TAB_H)
        fu_shown--;
    int fd_avail = KL_TAB_H - (KL_CH + (fu_shown - 1) * KL_FU_STEP);
    int fd_shown = 0;
    if (fd > 0) {
        fd_shown = fd;
        while (fd_shown > 1 && KL_CH + (fd_shown - 1) * KL_FD_STEP > fd_avail)
            fd_shown--;
        if (KL_CH > fd_avail) fd_shown = fd_avail / KL_FD_STEP;  /* 只露条 */
        if (fd_shown > fd) fd_shown = fd;
    }
    int y_top = KL_OY_TAB + KL_TAB_H - KL_CH;
    int base_fd = y_top - (fu_shown - 1) * KL_FU_STEP - KL_CH;
    int j;
    /* 扣牌(最旧先画, 最靠上) */
    for (j = 0; j < fd_shown; j++)
        kl_draw_card(x, base_fd - (fd_shown - 1 - j) * KL_FD_STEP,
                     kl_tab[c][fd - fd_shown + j], false, false);
    /* 翻开(旧的先画, 顶牌最后盖在最上) */
    for (j = fu_shown - 1; j >= 0; j--)
        kl_draw_card(x, y_top - j * KL_FU_STEP,
                     kl_tab[c][n - 1 - j], true, cur && j == 0);
}

static void kl_draw_top(void) {
    int x, f;
    bool cur;
    x = KL_OX;
    cur = (kl_row == 0 && kl_col == 0);
    if (kl_deck_n > 0) kl_draw_card(x, KL_OY_TOP, kl_deck[kl_deck_n - 1], false, cur);
    else kl_draw_slot(x, KL_OY_TOP, cur);
    x = KL_OX + KL_CS;
    cur = (kl_row == 0 && kl_col == 1);
    if (kl_waste_n > 0) kl_draw_card(x, KL_OY_TOP, kl_waste[kl_waste_n - 1], true, cur);
    else kl_draw_slot(x, KL_OY_TOP, cur);
    for (f = 0; f < 4; f++) {
        x = KL_OX + (f + 2) * KL_CS;
        cur = (kl_row == 0 && kl_col == f + 2);
        if (kl_found_n[f] > 0)
            kl_draw_card(x, KL_OY_TOP, kl_found[f][kl_found_n[f] - 1], true, cur);
        else
            kl_draw_slot(x, KL_OY_TOP, cur);
    }
}

/* 选中源高亮: 粗黑框(在所有牌画完后画) */
static void kl_draw_sel(void) {
    int x = -1, y = -1;
    if (kl_sel_src == KL_SRC_WASTE && kl_waste_n > 0) {
        x = KL_OX + KL_CS;
        y = KL_OY_TOP;
    } else if (kl_sel_src >= 0 && kl_sel_src < 7 && kl_tab_n[kl_sel_src] > 0) {
        x = KL_OX + kl_sel_src * KL_CS;
        y = KL_OY_TAB + KL_TAB_H - KL_CH;
    }
    if (x >= 0)
        fb_stroke_rect_thick(x - 2, y - 2, KL_CW + 4, KL_CH + 4, 2, true);
}

static void kl_hud(void) {
    char buf[16], rev[16];
    unsigned i = 0, j, len;
    uint32_t v = kl_moves;
    int total;
    fb_text(0, 0, "KLONDIKE", true);
    if (v == 0) buf[i++] = '0';
    while (v && i < 14) { buf[i++] = (char)('0' + v % 10); v /= 10; }
    len = i;
    for (j = 0; j < len; j++) rev[j] = buf[len - 1 - j];
    rev[len] = 0;
    total = text_width("MOVES") + text_width(rev);
    fb_text(CCG_W - 2 - total, 0, "MOVES", true);
    fb_text(CCG_W - 2 - total + text_width("MOVES"), 0, rev, true);
    fb_hline(0, CCG_HUD_H - 1, CCG_W, true);
}

void klondike_enter(void);

void klondike_render(void) {
    fb_clear(false);
    kl_draw_top();
    kl_draw_pile(0, KL_OX);
    kl_draw_pile(1, KL_OX + KL_CS);
    kl_draw_pile(2, KL_OX + 2 * KL_CS);
    kl_draw_pile(3, KL_OX + 3 * KL_CS);
    kl_draw_pile(4, KL_OX + 4 * KL_CS);
    kl_draw_pile(5, KL_OX + 5 * KL_CS);
    kl_draw_pile(6, KL_OX + 6 * KL_CS);
    kl_draw_sel();
    kl_hud();
    if (kl_over) {
        fb_fill_rect(0, 0, CCG_W, CCG_HUD_H - 1, false);
        fb_text(2, 2, "WIN! 52 CARDS", true);
        fb_text(CCG_W - 2 - text_width("OK/N:RETRY BACK:QUIT"), 2,
                "OK/N:RETRY BACK:QUIT", true);
        if (!kl_over_full) { kl_over_full = true; disp_force_full(); }
    }
}

void klondike_on_key(const key_event_t *ev) {
    if (ev->is_repeat) { kl_cursor_move(ev); return; }
    if (kl_over) {
        if (ev->key == K_OK || (ev->key == K_CHAR && ev->ch == 'n'))
            klondike_enter();
        else if (ev->key == K_BACK || ev->key == K_QUIT)
            s_exit_request = true;
        return;
    }
    kl_cursor_move(ev);
    switch (ev->key) {
    case K_OK:
        if (kl_sel_src >= 0) {
            int dcol, dfound;
            kl_dst_at_cursor(&dcol, &dfound);
            if (dcol == kl_sel_src ||
                (kl_sel_src == KL_SRC_WASTE && kl_row == 0 && kl_col == 1)) {
                kl_sel_src = -1;                     /* 同格再按: 取消 */
            } else if (!kl_move(kl_sel_src, dcol, dfound)) {
                audio_error();
                kl_sel_src = -1;                     /* 移动失败: 取消 */
                int ns = kl_src_at_cursor();         /* 新格若是源则改选 */
                if (ns >= 0 && ns != KL_SRC_DECK) kl_sel_src = ns;
            } else {
                kl_sel_src = -1;
            }
        } else {
            int src = kl_src_at_cursor();
            if (src == KL_SRC_DECK) kl_flip();
            else if (src >= 0) kl_sel_src = src;
        }
        break;
    case K_BACK:
        kl_flip();
        break;
    case K_PAUSE: {
        pause_sel_t sel;
        if (ui_pause_run(&sel)) {
            if (sel == PAUSE_RESTART) klondike_enter();
        } else {
            s_exit_request = true;
        }
        break;
    }
    case K_CHAR:
        if (ev->ch == 'n') klondike_enter();
        break;
    case K_QUIT:
        s_exit_request = true;
        break;
    default:
        break;
    }
}

void klondike_enter(void) {
    rng_seed(&kl_rng, now_ms() ^ 0x6B21u);
    kl_deal();
    kl_reset_state();
    klondike_render();
    disp_full();
}

void klondike_tick(uint64_t now) { (void)now; }
void klondike_exit(void) {}
