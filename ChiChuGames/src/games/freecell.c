/* 空当接龙 FREECELL — 8 列 52 张全翻开 + 4 空当单元 + 4 目标堆
 * 规则: 单元/列顶牌可移目标堆(同花色递增)、列间(红黑交替递减)、空单元(任意一张);
 *       空列可放任意。整叠移动不做(仅单张)。光标跨区域移动, OK 选中/放置(选中反白)。
 * 布局: 牌 22x30, 列距 35, 顶部一行 8 槽(左 4 空当 + 右 4 目标), 下方 8 列堆叠居中 */
#include "game.h"
#include "../config.h"
#include "../gfx/canvas.h"
#include "../gfx/font.h"
#include "../platform/display.h"
#include "../platform/input.h"
#include "../rng.h"
#include "../platform/time.h"
#include "../platform/audio.h"
#include "../platform/led.h"

#define FC_CW 22            /* 牌宽 */
#define FC_CH 30            /* 牌高 */
#define FC_PITCH 35         /* 列距 */
#define FC_X0 8             /* 8 列 8*35=280 居中: (296-280)/2 */
#define FC_TOP_Y 19         /* 顶部单元/目标行 y */
#define FC_TB_Y0 53         /* 列区顶 */
#define FC_TB_BOT 149       /* 列区底(牌底贴此, 屏幕 152 留 2px) */
#define FC_TB_SPAN (FC_TB_BOT - FC_TB_Y0 + 1)   /* 97 */
#define FC_NS 4             /* 空当/目标数 */
#define FC_NCOL 8
#define FC_MAX 52

static uint8_t fc_col[FC_NCOL][FC_MAX];   /* 列堆叠, 0=底 */
static uint8_t fc_cnt[FC_NCOL];           /* 每列张数 */
static int8_t fc_cell[FC_NS];             /* 空当单元, -1 空 */
static int8_t fc_found[FC_NS];            /* 目标堆顶牌, -1 空 */
static uint8_t fc_cur_kind;               /* 光标: 0=顶部行(空当+目标) 1=列 */
static uint8_t fc_cur_col;
static int8_t fc_sel_kind;                /* 选中源: -1 无; 0=顶部行 1=列 */
static int8_t fc_sel_idx;
static uint32_t fc_moves;
static bool fc_over, fc_over_full, fc_stuck;
static rng_t fc_rng;

void freecell_render(void);

static uint8_t fc_suit(uint8_t c) { return c / 13; }   /* 0红心 1方块 2黑桃 3梅花 */
static uint8_t fc_rank(uint8_t c) { return c % 13; }   /* 0=A .. 12=K */
static bool fc_red(uint8_t c) { return fc_suit(c) < 2; }

/* 堆叠重叠量: 张数越多重叠越小, 保证整列始终落在列区内 */
static int fc_overlap(int n) {
    if (n <= 1) return 8;
    int o = (FC_TB_SPAN - FC_CH) / (n - 1);
    if (o > 8) o = 8;
    if (o < 1) o = 1;
    return o;
}

/* 列顶牌 y(空列 = 落牌槽 y) */
static int fc_col_top_y(int c) {
    int n = fc_cnt[c];
    int ov = fc_overlap(n);
    int stack_h = FC_CH + (n > 0 ? (n - 1) * ov : 0);
    return FC_TB_Y0 + (FC_TB_SPAN - stack_h) / 2 + (n > 0 ? (n - 1) * ov : 0);
}

static void fc_target_rect(uint8_t kind, uint8_t idx, int *x, int *y) {
    *x = FC_X0 + idx * FC_PITCH;
    *y = (kind == 0) ? FC_TOP_Y : fc_col_top_y(idx);
}

/* 花色符号: 红(红心/方块)黑底白花, 黑(黑桃/梅花)黑花; 反白时整体翻转 */
static void fc_glyph(int x, int y, uint8_t suit, int scale, bool invert) {
    extern const uint8_t font_symbols[CG_COUNT][7];
    int cg = suit == 0 ? CG_HEART : suit == 1 ? CG_DIAMOND
            : suit == 2 ? CG_SPADE : CG_CLUB;
    const uint8_t *g = font_symbols[cg];
    bool box = fc_red(suit) != invert;
    if (box) fb_fill_rect(x, y, FONT_W * scale, FONT_H * scale, true);
    for (int j = 0; j < FONT_H; j++)
        for (int i = 0; i < FONT_W; i++)
            if (g[j] & (1u << i))
                fb_fill_rect(x + i * scale, y + j * scale, scale, scale, !box);
}

/* 牌面 22x30: 左上点数+花色, 中央 2x 花色, 黑花色底部色条 */
static void fc_draw_card(int x, int y, uint8_t card, bool invert) {
    uint8_t s = fc_suit(card);
    int r = (int)fc_rank(card) + 1;      /* 1..13 */
    if (invert) fb_fill_rect(x, y, FC_CW, FC_CH, true);
    fb_stroke_rect(x, y, FC_CW, FC_CH, !invert);
    char rbuf[3];
    if (r == 1) { rbuf[0] = 'A'; rbuf[1] = 0; }
    else if (r == 11) { rbuf[0] = 'J'; rbuf[1] = 0; }
    else if (r == 12) { rbuf[0] = 'Q'; rbuf[1] = 0; }
    else if (r == 13) { rbuf[0] = 'K'; rbuf[1] = 0; }
    else if (r == 10) { rbuf[0] = '1'; rbuf[1] = '0'; rbuf[2] = 0; }
    else { rbuf[0] = (char)('0' + r); rbuf[1] = 0; }
    fb_text(x + 1, y + 1, rbuf, !invert);
    fc_glyph(x + 2 + text_width(rbuf), y + 1, s, 1, invert);
    fc_glyph(x + (FC_CW - FONT_W * 2) / 2, y + 9, s, 2, invert);
    if (s >= 2) fb_fill_rect(x + 1, y + FC_CH - 3, FC_CW - 2, 2, !invert);
}

/* 空槽: 空心双框 */
static void fc_slot(int x, int y) {
    fb_stroke_rect(x, y, FC_CW, FC_CH, true);
    fb_stroke_rect(x + 2, y + 2, FC_CW - 4, FC_CH - 4, true);
}

/* 取源槽顶牌; 返回是否有牌 */
static bool fc_pick_card(uint8_t kind, uint8_t idx, uint8_t *card) {
    if (kind == 0) {
        if (idx < FC_NS) {
            if (fc_cell[idx] < 0) return false;
            *card = (uint8_t)fc_cell[idx];
        } else {
            if (fc_found[idx - FC_NS] < 0) return false;
            *card = (uint8_t)fc_found[idx - FC_NS];
        }
        return true;
    }
    if (fc_cnt[idx] == 0) return false;
    *card = fc_col[idx][fc_cnt[idx] - 1];
    return true;
}

/* 落牌合法性(纯检查, 不修改状态) */
static bool fc_can_place(uint8_t card, uint8_t dk, uint8_t di) {
    uint8_t s = fc_suit(card), r = fc_rank(card);
    if (dk == 0) {
        if (di < FC_NS) return fc_cell[di] < 0;          /* 空当: 任意一张 */
        int8_t t = fc_found[di - FC_NS];                 /* 目标: 同花色递增 */
        if (t < 0) return r == 0;                        /* 只收 A */
        return fc_suit((uint8_t)t) == s && fc_rank((uint8_t)t) == r - 1;
    }
    if (fc_cnt[di] == 0) return true;                    /* 空列: 任意 */
    uint8_t t = fc_col[di][fc_cnt[di] - 1];
    return fc_red(t) != fc_red(card) && fc_rank(t) == r + 1;  /* 红黑交替递减 */
}

/* 单张移动: 成功返回 true */
static bool fc_try_move(uint8_t sk, uint8_t si, uint8_t dk, uint8_t di) {
    uint8_t card;
    if (!fc_pick_card(sk, si, &card)) return false;
    if (sk == dk && si == di) return false;
    if (!fc_can_place(card, dk, di)) return false;
    /* 移除源 */
    if (sk == 0) {
        if (si < FC_NS) fc_cell[si] = -1;
        else fc_found[si - FC_NS] = -1;
    } else {
        fc_cnt[si]--;
    }
    /* 放入目标 */
    if (dk == 0) {
        if (di < FC_NS) fc_cell[di] = (int8_t)card;
        else fc_found[di - FC_NS] = (int8_t)card;
    } else {
        fc_col[di][fc_cnt[di]++] = card;
    }
    fc_moves++;
    return true;
}

/* 胜利(4 目标堆到 K)或死局(无任何合法单张移动)检测 */
static void fc_check_end(void) {
    int won = 1;
    for (int i = 0; i < FC_NS; i++)
        if (fc_found[i] < 0 || fc_rank((uint8_t)fc_found[i]) != 12) { won = 0; break; }
    if (won) {
        fc_over = true;
        fc_stuck = false;
        audio_win();                   /* 四堆到 K */
        led_fx_set(LED_FX_WIN);
        return;
    }
    for (uint8_t sk = 0; sk < 2; sk++)
        for (uint8_t si = 0; si < (sk == 0 ? 8 : FC_NCOL); si++) {
            uint8_t card;
            if (!fc_pick_card(sk, si, &card)) continue;
            for (uint8_t dk = 0; dk < 2; dk++)
                for (uint8_t di = 0; di < (dk == 0 ? 8 : FC_NCOL); di++) {
                    if (sk == dk && si == di) continue;
                    if (fc_can_place(card, dk, di)) return;   /* 还有棋 */
                }
        }
    fc_over = true;
    fc_stuck = true;
    audio_lose();                      /* 死局 */
    led_fx_set(LED_FX_LOSE);
}

static void fc_do_ok(void) {
    if (fc_sel_kind < 0) {
        uint8_t card;
        if (fc_pick_card(fc_cur_kind, fc_cur_col, &card)) {
            fc_sel_kind = (int8_t)fc_cur_kind;
            fc_sel_idx = (int8_t)fc_cur_col;
            audio_select();            /* 选中牌 */
        }
        return;
    }
    if (fc_sel_kind == (int8_t)fc_cur_kind && fc_sel_idx == (int8_t)fc_cur_col) {
        fc_sel_kind = -1;              /* 原地取消 */
        return;
    }
    if (fc_try_move((uint8_t)fc_sel_kind, (uint8_t)fc_sel_idx, fc_cur_kind, fc_cur_col)) {
        fc_check_end();
    } else {
        audio_error();                 /* 非法放置 */
    }
    fc_sel_kind = -1;                  /* 无论成败, 放下 */
}

/* 洗牌 52 张, 按 7/6/7/6/7/6/7/6 发入 8 列 */
static void freecell_reset(void) {
    for (int i = 0; i < FC_NS; i++) { fc_cell[i] = -1; fc_found[i] = -1; }
    for (int c = 0; c < FC_NCOL; c++) fc_cnt[c] = 0;
    uint8_t deck[FC_MAX];
    for (int i = 0; i < FC_MAX; i++) deck[i] = (uint8_t)i;
    for (int i = FC_MAX - 1; i > 0; i--) {        /* Fisher-Yates */
        uint32_t j = rng_range(&fc_rng, (uint32_t)i + 1);
        uint8_t t = deck[i];
        deck[i] = deck[j];
        deck[j] = t;
    }
    int k = 0;
    for (int c = 0; c < FC_NCOL; c++) {
        int n = (c % 2 == 0) ? 7 : 6;             /* 前 4 列 7 张, 后 4 列 6 张 */
        for (int r = 0; r < n; r++) fc_col[c][fc_cnt[c]++] = deck[k++];
    }
    fc_cur_kind = 0;
    fc_cur_col = 0;
    fc_sel_kind = -1;
    fc_moves = 0;
    fc_over = false;
    fc_stuck = false;
    fc_over_full = false;
}

void freecell_enter(void) {
    uint64_t e = now_ms() ^ 0xFCE3u;
    rng_seed(&fc_rng, e ? e : 0xFCE3u);           /* 种子 0 会死循环 */
    freecell_reset();
    freecell_render();
    disp_full();
}

void freecell_render(void) {
    fb_clear(false);
    /* 顶部一行: 左 4 空当 + 右 4 目标(列 4..7 对齐下方第 5..8 列) */
    for (int i = 0; i < FC_NS; i++) {
        int x = FC_X0 + i * FC_PITCH;
        bool sel = fc_sel_kind == 0 && fc_sel_idx == i;
        if (fc_cell[i] >= 0) fc_draw_card(x, FC_TOP_Y, (uint8_t)fc_cell[i], sel);
        else fc_slot(x, FC_TOP_Y);
    }
    for (int i = 0; i < FC_NS; i++) {
        int x = FC_X0 + (i + FC_NS) * FC_PITCH;
        bool sel = fc_sel_kind == 0 && fc_sel_idx == i + FC_NS;
        if (fc_found[i] >= 0) fc_draw_card(x, FC_TOP_Y, (uint8_t)fc_found[i], sel);
        else fc_slot(x, FC_TOP_Y);
    }
    /* 8 列: 底->顶, 整列在列区内垂直居中 */
    for (int c = 0; c < FC_NCOL; c++) {
        int n = fc_cnt[c];
        int ov = fc_overlap(n);
        int stack_h = FC_CH + (n > 0 ? (n - 1) * ov : 0);
        int y0 = FC_TB_Y0 + (FC_TB_SPAN - stack_h) / 2;
        int x = FC_X0 + c * FC_PITCH;
        for (int i = 0; i < n; i++)
            fc_draw_card(x, y0 + i * ov, fc_col[c][i],
                         fc_sel_kind == 1 && fc_sel_idx == c);
        if (n == 0) fc_slot(x, y0);
    }
    /* 光标: 对称外框(所有格子画完后最后画) */
    {
        int cx, cy;
        fc_target_rect(fc_cur_kind, fc_cur_col, &cx, &cy);
        fb_stroke_rect_thick(cx - 2, cy - 2, FC_CW + 4, FC_CH + 4, 2, true);
    }
    /* HUD 顶栏: 左标题黑字, 右 MOVES + 数值(黑字白底) */
    fb_text(0, 0, "FREECELL", true);
    char mv[16];
    const char *pre = "MOVES ";
    unsigned mi = 0;
    while (pre[mi]) { mv[mi] = pre[mi]; mi++; }
    uint32_t v = fc_moves;
    char rev[12];
    unsigned ri = 0;
    if (v == 0) { rev[ri++] = '0'; }
    while (v && ri < 10) { rev[ri++] = (char)('0' + v % 10); v /= 10; }
    while (ri > 0) mv[mi++] = rev[--ri];
    mv[mi] = 0;
    fb_text(CCG_W - 2 - text_width(mv), 0, mv, true);
    fb_hline(0, CCG_HUD_H - 1, CCG_W, true);
    if (fc_over) {
        /* 结束: HUD 区两行提示 */
        fb_fill_rect(0, 0, CCG_W, CCG_HUD_H - 1, false);
        fb_text(2, 2, fc_stuck ? "NO MOVES!" : "WIN!", true);
        fb_text(CCG_W - 2 - text_width("OK/N:RETRY BACK:QUIT"), 2,
                "OK/N:RETRY BACK:QUIT", true);
        if (!fc_over_full) { fc_over_full = true; disp_force_full(); }
    }
}

void freecell_on_key(const key_event_t *ev) {
    if (fc_over) {
        if (ev->is_repeat) return;
        if (ev->key == K_OK || (ev->key == K_CHAR && ev->ch == 'n'))
            freecell_enter();
        else if (ev->key == K_BACK || ev->key == K_QUIT)
            s_exit_request = true;
        return;
    }
    /* 方向/WASD 光标跨区域移动(方向键重复可响应) */
    bool l = ev->key == K_LEFT || (ev->key == K_CHAR && ev->ch == 'a');
    bool r = ev->key == K_RIGHT || (ev->key == K_CHAR && ev->ch == 'd');
    bool u = ev->key == K_UP || (ev->key == K_CHAR && ev->ch == 'w');
    bool d = ev->key == K_DOWN || (ev->key == K_CHAR && ev->ch == 's');
    if (l && fc_cur_col > 0) { fc_cur_col--; return; }
    if (r && fc_cur_col < FC_NCOL - 1) { fc_cur_col++; return; }
    if (u && fc_cur_kind == 1) { fc_cur_kind = 0; return; }
    if (d && fc_cur_kind == 0) { fc_cur_kind = 1; return; }
    if (ev->is_repeat) return;                    /* 确认键/字母忽略重复 */
    switch (ev->key) {
    case K_OK:
        fc_do_ok();
        break;
    case K_CHAR:
        if (ev->ch == 'n') freecell_enter();
        break;
    case K_PAUSE:
    case K_BACK: {
        pause_sel_t sel;
        if (ui_pause_run(&sel)) {
            if (sel == PAUSE_RESTART) freecell_enter();
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

void freecell_tick(uint64_t now) { (void)now; }
void freecell_exit(void) {}
