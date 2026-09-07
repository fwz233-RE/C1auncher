/* SPIDER SOLITAIRE — 单花色简化蜘蛛纸牌 (C1-Slim 墨水屏)
 * 104 张(8 套 A..K, 全黑桃): 前 4 列 6 张、后 6 列 5 张(共 54), 发牌堆 50 张
 * 规则: 只移单张顶牌; 目标列空可放任意, 否则目标顶牌须比所移牌大 1(递减堆叠);
 *       完整 K..A 13 张降序段自动移出游戏(蜘蛛核心机制——不减牌则列数守恒,
 *       全部列清空将不可达); 全部列清空 = WIN(即收齐 8 组);
 *       发牌堆空且无任何合法移动 = STUCK
 * 操作: 左右/WASD 选列, OK 选中顶牌/移动放置(再按同列取消), BACK 补发一列
 *       (每列 +1, 10 张/次, 共 5 次), N 新局, P 暂停, Q 退出
 * 显示: 10 列 22x30 牌底对齐, 每列只画顶牌 + 顶部张数(1x), 光标粗框, 选中反白
 * 静态前缀: sp_ */
#include "game.h"
#include "../config.h"
#include "../gfx/canvas.h"
#include "../gfx/font.h"
#include "../platform/display.h"
#include "../rng.h"
#include "../platform/time.h"
#include "../platform/audio.h"
#include "../platform/led.h"

#define SP_NCOL 10          /* 列数 */
#define SP_NRUN 13          /* 完整降序段长度 */
#define SP_DECK 104         /* 8 套 * 13 */
#define SP_CW 22            /* 牌宽 */
#define SP_CH 30            /* 牌高 */
#define SP_PITCH 28         /* 列距 */
#define SP_X0 11            /* (296 - (10*22+9*6)) / 2 */
#define SP_CNT_Y 20         /* 张数文字 y */
#define SP_CARD_Y 116       /* 顶牌 y(底对齐, 116+30=146) */

static uint8_t sp_col[SP_NCOL][SP_DECK];   /* 列堆叠, 0=底 */
static uint8_t sp_cnt[SP_NCOL];            /* 每列张数 */
static uint8_t sp_stock[50];               /* 发牌堆 */
static uint8_t sp_stock_n;
static uint8_t sp_cur;                     /* 光标列 0..9 */
static int8_t sp_sel;                      /* 选中源列, -1 无 */
static uint32_t sp_moves;
static uint8_t sp_runs;                    /* 已清除完整段数(0..8) */
static uint8_t sp_deals;                   /* 已补发次数(0..5) */
static bool sp_over, sp_stuck, sp_over_full;
static rng_t sp_rng;

static const char *sp_rank_str[13] = { "A", "2", "3", "4", "5", "6", "7", "8",
                                       "9", "10", "J", "Q", "K" };

static uint8_t sp_rank(uint8_t c) { return (uint8_t)(c % 13 + 1); }   /* 1=A..13=K */

/* 无符号十进制转字符串 */
static void sp_utoa(char *out, unsigned cap, uint32_t v) {
    char rev[12];
    unsigned n = 0;
    if (v == 0) rev[n++] = '0';
    while (v > 0 && n < cap - 1) { rev[n++] = (char)('0' + v % 10); v /= 10; }
    unsigned i = 0;
    while (n > 0 && i < cap - 1) out[i++] = rev[--n];
    out[i] = 0;
}

/* 洗牌 104 张; 前 4 列 6 张, 后 6 列 5 张, 余 50 张入发牌堆 */
static void sp_reset(void) {
    uint8_t deck[SP_DECK];
    for (int i = 0; i < SP_DECK; i++) deck[i] = (uint8_t)i;
    for (int i = SP_DECK - 1; i > 0; i--) {          /* Fisher-Yates */
        uint32_t j = rng_range(&sp_rng, (uint32_t)i + 1);
        uint8_t t = deck[i];
        deck[i] = deck[j];
        deck[j] = t;
    }
    int k = 0;
    for (int c = 0; c < SP_NCOL; c++) {
        int n = (c < 4) ? 6 : 5;
        sp_cnt[c] = (uint8_t)n;
        for (int r = 0; r < n; r++) sp_col[c][r] = deck[k++];
    }
    sp_stock_n = (uint8_t)(SP_DECK - k);             /* 50 */
    for (int i = 0; i < (int)sp_stock_n; i++) sp_stock[i] = deck[k + i];
    sp_cur = 0;
    sp_sel = -1;
    sp_moves = 0;
    sp_runs = 0;
    sp_deals = 0;
    sp_over = false;
    sp_stuck = false;
    sp_over_full = false;
}

/* 完整 K..A 降序 13 段自动清除(只认列顶 13 张); 返回是否清过 */
static bool sp_collect_runs(void) {
    bool any = false;
    for (int c = 0; c < SP_NCOL; c++) {
        while (sp_cnt[c] >= SP_NRUN) {
            int base = (int)sp_cnt[c] - SP_NRUN;
            bool ok = true;
            for (int k = 0; k < SP_NRUN; k++) {
                if (sp_rank(sp_col[c][base + k]) != (uint8_t)(SP_NRUN - k)) { ok = false; break; }
            }
            if (!ok) break;
            sp_cnt[c] = (uint8_t)base;
            sp_runs++;
            any = true;
        }
    }
    return any;
}

static bool sp_all_empty(void) {
    for (int c = 0; c < SP_NCOL; c++)
        if (sp_cnt[c] > 0) return false;
    return true;
}

/* 发牌堆空时检测死局: 无任何单张可移(含空列目标) */
static void sp_check_stuck(void) {
    for (int s = 0; s < SP_NCOL; s++) {
        if (sp_cnt[s] == 0) continue;
        uint8_t card = sp_col[s][sp_cnt[s] - 1];
        for (int d = 0; d < SP_NCOL; d++) {
            if (s == d) continue;
            if (sp_cnt[d] == 0) return;                       /* 空列可落 */
            if (sp_rank(card) == sp_rank(sp_col[d][sp_cnt[d] - 1]) - 1) return;
        }
    }
    sp_over = true;
    sp_stuck = true;
}

/* 单张移动: 目标空或目标顶牌 = 所移牌 + 1; 成功后清段/胜负/死局检查 */
static bool sp_try_move(uint8_t src, uint8_t dst) {
    if (src == dst || sp_cnt[src] == 0) return false;
    uint8_t card = sp_col[src][sp_cnt[src] - 1];
    if (sp_cnt[dst] > 0) {
        uint8_t top = sp_col[dst][sp_cnt[dst] - 1];
        if (sp_rank(card) != sp_rank(top) - 1) { audio_error(); return false; }  /* 递减 1 */
    }
    sp_cnt[src]--;
    sp_col[dst][sp_cnt[dst]++] = card;
    sp_moves++;
    if (sp_collect_runs()) audio_clear();       /* 收齐 K..A 一段 */
    if (sp_all_empty()) {
        sp_over = true; sp_stuck = false;       /* 全部列清空: 胜 */
        audio_win();
        led_fx_set(LED_FX_WIN);
        return true;
    }
    if (sp_stock_n == 0) {
        sp_check_stuck();
        if (sp_over) {                          /* 死局: 负 */
            audio_lose();
            led_fx_set(LED_FX_LOSE);
        }
    }
    return true;
}

/* BACK: 补发一列, 每列 +1(需发牌堆 >= 10 张) */
static void sp_deal(void) {
    if (sp_over || sp_stock_n < SP_NCOL) return;
    for (int c = 0; c < SP_NCOL; c++)
        sp_col[c][sp_cnt[c]++] = sp_stock[--sp_stock_n];
    sp_deals++;
    sp_moves++;
    sp_collect_runs();
    if (sp_all_empty()) { sp_over = true; sp_stuck = false; return; }
    if (sp_stock_n == 0) sp_check_stuck();
}

/* OK: 无选中则选中当前列顶牌, 已选中则移动(同列再按取消) */
static void sp_do_ok(void) {
    if (sp_sel < 0) {
        if (sp_cnt[sp_cur] > 0) sp_sel = (int8_t)sp_cur;
        return;
    }
    if (sp_sel == (int8_t)sp_cur) { sp_sel = -1; return; }
    sp_try_move((uint8_t)sp_sel, sp_cur);
    sp_sel = -1;
}

/* ---- 绘制 ---- */
static void sp_glyph(int x, int y, int scale, bool black) {
    const uint8_t *g = font_symbols[CG_SPADE];
    for (int j = 0; j < FONT_H; j++)
        for (int i = 0; i < FONT_W; i++)
            if (g[j] & (1u << i))
                fb_fill_rect(x + i * scale, y + j * scale, scale, scale, black);
}

/* 牌面 22x30: 左上点数+黑桃, 中央 2x 黑桃 */
static void sp_draw_card(int x, int y, uint8_t card, bool invert) {
    if (invert) fb_fill_rect(x, y, SP_CW, SP_CH, true);
    fb_stroke_rect(x, y, SP_CW, SP_CH, !invert);
    const char *rs = sp_rank_str[sp_rank(card) - 1];
    fb_text(x + 2, y + 2, rs, !invert);
    sp_glyph(x + 3 + text_width(rs), y + 2, 1, !invert);
    sp_glyph(x + (SP_CW - FONT_W * 2) / 2, y + 9, 2, !invert);
}

/* 空列槽: 细框(光标时反白) */
static void sp_slot(int x, int y, bool cursor) {
    if (cursor) {
        fb_fill_rect(x, y, SP_CW, SP_CH, true);
        fb_stroke_rect(x, y, SP_CW, SP_CH, false);
    } else {
        fb_stroke_rect(x, y, SP_CW, SP_CH, true);
    }
}

static void sp_draw_column(int c) {
    int x = SP_X0 + c * SP_PITCH;
    bool cur = sp_cur == c;
    int n = sp_cnt[c];
    if (n == 0) { sp_slot(x, SP_CARD_Y, cur); return; }
    /* 张数(光标列反白) */
    char num[4];
    sp_utoa(num, sizeof num, (uint32_t)n);
    int w = text_width(num);
    int nx = x + (SP_CW - w) / 2;
    if (cur) fb_fill_rect(nx - 1, SP_CNT_Y, w + 2, FONT_H, true);
    fb_text(nx, SP_CNT_Y, num, !cur);
    /* 堆栈连线: 张数下沿到牌顶 */
    fb_vline(x + SP_CW / 2 - 1, SP_CNT_Y + FONT_H + 2,
             SP_CARD_Y - SP_CNT_Y - FONT_H - 3, true);
    /* 顶牌(选中反白) */
    sp_draw_card(x, SP_CARD_Y, sp_col[c][n - 1], sp_sel == c);
}

/* 光标: 粗黑框(所有格绘制完后最后画) */
static void sp_draw_cursor(void) {
    int x = SP_X0 + sp_cur * SP_PITCH;
    fb_stroke_rect_thick(x - 2, SP_CARD_Y - 2, SP_CW + 4, SP_CH + 4, 2, true);
}

static void sp_hud(void) {
    fb_text(0, 0, "SPIDER", true);
    char s[48];
    char nbuf[12];
    const char *p;
    unsigned i = 0;
    p = "MOVES ";
    while (*p) s[i++] = *p++;
    sp_utoa(nbuf, sizeof nbuf, sp_moves);
    p = nbuf;
    while (*p && i < sizeof s - 1) s[i++] = *p++;
    p = " RUNS ";
    while (*p) s[i++] = *p++;
    sp_utoa(nbuf, sizeof nbuf, sp_runs);
    p = nbuf;
    while (*p && i < sizeof s - 1) s[i++] = *p++;
    s[i++] = '/';
    s[i++] = '8';
    p = " DEAL ";
    while (*p) s[i++] = *p++;
    sp_utoa(nbuf, sizeof nbuf, (uint32_t)(sp_stock_n / SP_NCOL));
    p = nbuf;
    while (*p && i < sizeof s - 1) s[i++] = *p++;
    s[i] = 0;
    fb_text(CCG_W - 2 - text_width(s), 0, s, true);
    fb_hline(0, CCG_HUD_H - 1, CCG_W, true);
}

void spider_render(void);

void spider_enter(void) {
    rng_seed(&sp_rng, now_ms() ^ 0x5EEDu);
    sp_reset();
    spider_render();
    disp_full();
}

void spider_render(void) {
    fb_clear(false);
    for (int c = 0; c < SP_NCOL; c++) sp_draw_column(c);
    sp_draw_cursor();
    sp_hud();
    if (sp_over) {
        /* 结束: HUD 区两行提示(墙内不放文字) */
        fb_fill_rect(0, 0, CCG_W, CCG_HUD_H - 1, false);
        fb_text(2, 2, sp_stuck ? "STUCK! NO MOVES" : "WIN! ALL CLEARED", true);
        fb_text(CCG_W - 2 - text_width("OK/N:RETRY BACK:QUIT"), 2,
                "OK/N:RETRY BACK:QUIT", true);
        if (!sp_over_full) { sp_over_full = true; disp_force_full(); }
    }
}

void spider_on_key(const key_event_t *ev) {
    if (sp_over) {
        if (ev->is_repeat) return;
        if (ev->key == K_OK || (ev->key == K_CHAR && ev->ch == 'n'))
            spider_enter();
        else if (ev->key == K_BACK || ev->key == K_QUIT)
            s_exit_request = true;
        return;
    }
    /* 方向/WASD 移列(重复可响应) */
    bool l = ev->key == K_LEFT || (ev->key == K_CHAR && ev->ch == 'a');
    bool r = ev->key == K_RIGHT || (ev->key == K_CHAR && ev->ch == 'd');
    if (l && sp_cur > 0) { sp_cur--; return; }
    if (r && sp_cur < SP_NCOL - 1) { sp_cur++; return; }
    if (ev->is_repeat) return;                /* 确认键/字母忽略重复 */
    switch (ev->key) {
    case K_OK:
        sp_do_ok();
        break;
    case K_BACK:
        sp_deal();
        break;
    case K_CHAR:
        if (ev->ch == 'n') spider_enter();
        break;
    case K_PAUSE: {
        pause_sel_t sel;
        if (ui_pause_run(&sel)) {
            if (sel == PAUSE_RESTART) spider_enter();
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

void spider_tick(uint64_t now) { (void)now; }
void spider_exit(void) {}
