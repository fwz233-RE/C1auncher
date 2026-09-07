/* MATCHSTICK — 火柴棒等式: 移动一根火柴使等式成立
 * 数字以 7 段火柴绘制(0-9 七段码), 运算符 + - =
 * 内置 20 道题(每道给出错误等式 + 唯一一步正解, 离线穷举验证):
 *   光标左/右在 5 格(数字/运算符)间移动, 上/下在格内选段(槽位);
 *   OK 拿起(原位空心高亮, 格边框显示), 移到目标槽 OK 放下;
 *   放下后自动校验: 全部格字形合法且等式成立 → WIN(计步), N 换题
 * '=' 两根不可移动(动后字形必非法, 自动拒绝); 错误放置自动取消/保留手持
 * 误走一步后提示是否仍有一步正解(ms_count_solutions, 也用于测试唯一性)
 * 输入驱动(同 nim): 开局/结束全刷(disp_force_full 防重), 游戏内快刷
 *
 * games_table.c 建议条目(help <=5 行 + NULL):
 *   "MOVE ONE MATCH TO MAKE",
 *   "THE EQUATION TRUE.",
 *   "OK PICK UP, MOVE, OK",
 *   "TO DROP.  N NEW PUZZLE",
 *   NULL */
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
#include <stddef.h>

/* ---- 7 段位图: 槽位 0..6 = T UL UR M LL LR B ---- */
#define MS_T  (1u << 0)
#define MS_UL (1u << 1)
#define MS_UR (1u << 2)
#define MS_M  (1u << 3)
#define MS_LL (1u << 4)
#define MS_LR (1u << 5)
#define MS_B  (1u << 6)

#define MS_OP_PLUS  3u   /* '+' = 横 + 竖 */
#define MS_OP_MINUS 1u   /* '-' = 横 */
#define MS_OP_EQ    3u   /* '=' = 上横 + 下横 */

#define MS_NPUZ 20

enum { MS_PLAY = 0, MS_CARRY, MS_WIN };
enum { MS_ST_FIX = 0, MS_ST_HOLD, MS_ST_WRONG, MS_ST_NOHINT, MS_ST_INVALID,
       MS_ST_TAKEN, MS_ST_SAME, MS_ST_EQFIX };

/* ---- 静态状态(前缀 ms_) ---- */
static uint8_t ms_mask[5];    /* 当前 5 格字形(段位集) */
static int ms_state;          /* PLAY / CARRY / WIN */
static int ms_status;         /* 提示行 */
static int ms_cur_cell;       /* 光标格 0..4 */
static int ms_cur_slot;       /* 光标段槽位 */
static int ms_pc, ms_ps;      /* 手里这根来源(格, 槽) */
static int ms_moves;          /* 已用步数 */
static int ms_puzzle;         /* 当前题(洗牌后顺序下标) */
static bool ms_over_full;     /* 结束全刷防重 */
static int ms_order[MS_NPUZ]; /* 洗牌后的题序 */
static rng_t ms_rng;
static uint32_t ms_seed_cnt;
static uint8_t ms_cfg[2][5];  /* 解集去重暂存(<=2 即够) */

/* ---- 题库: 每题 = 错误等式 + 唯一一步正解(离线穷举验证) ---- */
typedef struct {
    uint8_t a, op, b, c;      /* 错误等式 a op b = c (op: 3='+' 1='-') */
    uint8_t pc, ps, dc, ds;   /* 解: 从 (pc,ps) 拿起, 放到 (dc,ds) */
} ms_puzzle_t;

static const uint8_t ms_digit[10] = {
    MS_T|MS_UL|MS_UR|MS_LL|MS_LR|MS_B,               /* 0 */
    MS_UR|MS_LR,                                     /* 1 */
    MS_T|MS_UR|MS_M|MS_LL|MS_B,                      /* 2 */
    MS_T|MS_UR|MS_M|MS_LR|MS_B,                      /* 3 */
    MS_UL|MS_UR|MS_M|MS_LR,                          /* 4 */
    MS_T|MS_UL|MS_M|MS_LR|MS_B,                      /* 5 */
    MS_T|MS_UL|MS_M|MS_LL|MS_LR|MS_B,                /* 6 */
    MS_T|MS_UR|MS_LR,                                /* 7 */
    MS_T|MS_UL|MS_UR|MS_M|MS_LL|MS_LR|MS_B,          /* 8 */
    MS_T|MS_UL|MS_UR|MS_M|MS_LR|MS_B,                /* 9 */
};

static const ms_puzzle_t ms_puzzles[MS_NPUZ] = {
    /* 1+5=2 -> 7-5=2  (op V -> T of 1) */
    {1, MS_OP_PLUS,  5, 2, 1, 1, 0, 0},
    /* 9+1=0 -> 9-1=8  (op V -> M of 0) */
    {9, MS_OP_PLUS,  1, 0, 1, 1, 4, 3},
    /* 1+1=6 -> 7-1=6  (op V -> T of 1) */
    {1, MS_OP_PLUS,  1, 6, 1, 1, 0, 0},
    /* 9+0=3 -> 9-0=9  (op V -> UL of 3) */
    {9, MS_OP_PLUS,  0, 3, 1, 1, 4, 1},
    /* 5+5=4 -> 9-5=4  (op V -> UR of 5) */
    {5, MS_OP_PLUS,  5, 4, 1, 1, 0, 2},
    /* 6+8=0 -> 8-8=0  (op V -> UR of 6) */
    {6, MS_OP_PLUS,  8, 0, 1, 1, 0, 2},
    /* 1-8=8 -> 1+8=9  (LL of 8 -> op V) */
    {1, MS_OP_MINUS, 8, 8, 4, 4, 1, 1},
    /* 4-9=9 -> 4+5=9  (UR of 9 -> op V) */
    {4, MS_OP_MINUS, 9, 9, 2, 2, 1, 1},
    /* 7-8=9 -> 1+8=9  (T of 7 -> op V) */
    {7, MS_OP_MINUS, 8, 9, 0, 0, 1, 1},
    /* 1-0=7 -> 1+0=1  (T of 7 -> op V) */
    {1, MS_OP_MINUS, 0, 7, 4, 0, 1, 1},
    /* 0-5=6 -> 0+5=5  (LL of 6 -> op V) */
    {0, MS_OP_MINUS, 5, 6, 4, 4, 1, 1},
    /* 3-7=8 -> 9-1=8  (T of 7 -> UL of 3) */
    {3, MS_OP_MINUS, 7, 8, 2, 0, 0, 1},
    /* 5-6=9 -> 9-6=3  (UL of 9 -> UR of 5) */
    {5, MS_OP_MINUS, 6, 9, 4, 1, 0, 2},
    /* 8+3=9 -> 0+9=9  (M of 8 -> UL of 3) */
    {8, MS_OP_PLUS,  3, 9, 0, 3, 2, 1},
    /* 9+8=3 -> 9+0=9  (M of 8 -> UL of 3) */
    {9, MS_OP_PLUS,  8, 3, 2, 3, 4, 1},
    /* 6-4=5 -> 9-4=5  (LL of 6 -> UR of 6) */
    {6, MS_OP_MINUS, 4, 5, 0, 4, 0, 2},
    /* 5+4=6 -> 5+4=9  (LL of 6 -> UR of 6) */
    {5, MS_OP_PLUS,  4, 6, 4, 4, 4, 2},
    /* 4-9=4 -> 4-0=4  (M of 9 -> LL of 9) */
    {4, MS_OP_MINUS, 9, 4, 2, 3, 2, 4},
    /* 4+3=6 -> 4+2=6  (LR of 3 -> LL of 3) */
    {4, MS_OP_PLUS,  3, 6, 2, 5, 2, 4},
    /* 3+3=0 -> 3+3=6  (UR of 0 -> M of 0) */
    {3, MS_OP_PLUS,  3, 0, 4, 2, 4, 3},
};

/* ---- 核心逻辑 ---- */

static int ms_digit_of(uint8_t m) {
    for (int i = 0; i < 10; i++)
        if (ms_digit[i] == m) return i;
    return -1;
}

/* 字形合法: 数字格 = 0-9 七段码; 运算符格 = '+'(3)/'-'(1); '=' 格 = 双横(3) */
static bool ms_glyph_ok(int cell, uint8_t m) {
    if (cell == 0 || cell == 2 || cell == 4) return ms_digit_of(m) >= 0;
    if (cell == 1) return m == MS_OP_PLUS || m == MS_OP_MINUS;
    return m == MS_OP_EQ;   /* cell 3 '=' */
}

static int ms_nslots(int cell) {
    return (cell == 0 || cell == 2 || cell == 4) ? 7 : 2;
}

/* 等式求值(前提: 5 格字形均合法) */
static bool ms_eval(const uint8_t m[5]) {
    int a = ms_digit_of(m[0]), b = ms_digit_of(m[2]), c = ms_digit_of(m[4]);
    if (a < 0 || b < 0 || c < 0) return false;
    if (m[1] == MS_OP_PLUS) return a + b == c;
    if (m[1] == MS_OP_MINUS) return a - b == c;
    return false;
}

/* 试算一步: 拿起 (pc,ps) 放到 (dc,ds); 结果字形合法则写入 out 并返回 true */
static bool ms_apply(const uint8_t m[5], int pc, int ps, int dc, int ds,
                     uint8_t out[5]) {
    uint8_t bit = (uint8_t)(1u << ps);
    if (!(m[pc] & bit)) return false;              /* 源槽没有火柴 */
    if (pc == dc && ps == ds) return false;        /* 原位放回 */
    uint8_t dbit = (uint8_t)(1u << ds);
    if (m[dc] & dbit) return false;                /* 目标槽已有火柴 */
    for (int i = 0; i < 5; i++) out[i] = m[i];
    out[pc] &= (uint8_t)~bit;
    out[dc] |= dbit;
    return ms_glyph_ok(pc, out[pc]) && ms_glyph_ok(dc, out[dc]);
}

/* 一步成立的解字形数(按结果字形去重; >=2 即返回 2, 唯一性判断够用)。
 * 供提示(误走后是否仍有一步正解)与测试(离线验证每题唯一解)共用 */
static int ms_count_solutions(const uint8_t m[5]) {
    int n = 0;
    uint8_t w[5];
    for (int pc = 0; pc < 5; pc++) {
        for (int ps = 0; ps < ms_nslots(pc); ps++) {
            uint8_t bit = (uint8_t)(1u << ps);
            if (!(m[pc] & bit)) continue;
            for (int dc = 0; dc < 5; dc++) {
                for (int ds = 0; ds < ms_nslots(dc); ds++) {
                    if (pc == dc && ps == ds) continue;
                    uint8_t dbit = (uint8_t)(1u << ds);
                    if (m[dc] & dbit) continue;
                    if (!ms_apply(m, pc, ps, dc, ds, w)) continue;
                    if (!ms_eval(w)) continue;
                    bool dup = false;
                    for (int k = 0; k < n; k++)
                        if (ms_cfg[k][0] == w[0] && ms_cfg[k][1] == w[1] &&
                            ms_cfg[k][2] == w[2] && ms_cfg[k][3] == w[3] &&
                            ms_cfg[k][4] == w[4]) { dup = true; break; }
                    if (dup) continue;
                    for (int i = 0; i < 5; i++) ms_cfg[n][i] = w[i];
                    n++;
                    if (n >= 2) return n;
                }
            }
        }
    }
    return n;
}

/* 测试辅助: 验证特定一步移动能直接成立(拿起 pc/ps, 放下 dc/ds) */
bool ms_try_move(const uint8_t m[5], int pc, int ps, int dc, int ds,
                 uint8_t out[5]) {
    if (!ms_apply(m, pc, ps, dc, ds, out)) return false;
    return ms_eval(out);
}

/* ---- 局流程 ---- */

static void ms_load_puzzle(int idx) {
    const ms_puzzle_t *p = &ms_puzzles[idx];
    ms_mask[0] = ms_digit[p->a];
    ms_mask[1] = p->op;
    ms_mask[2] = ms_digit[p->b];
    ms_mask[3] = MS_OP_EQ;
    ms_mask[4] = ms_digit[p->c];
    ms_moves = 0;
    ms_state = MS_PLAY;
    ms_status = MS_ST_FIX;
    ms_cur_cell = 0;
    ms_cur_slot = 0;
    ms_pc = -1;
    ms_ps = -1;
    ms_over_full = false;
}

static void ms_next_puzzle(void) {
    ms_puzzle = (ms_puzzle + 1) % MS_NPUZ;
    ms_load_puzzle(ms_puzzle);
}

static void ms_pause(void) {
    pause_sel_t sel;
    if (ui_pause_run(&sel)) {
        if (sel == PAUSE_RESTART) ms_load_puzzle(ms_puzzle);
    } else {
        s_exit_request = true;
    }
}

/* OK: 拿起 / 放下(放下后自动校验) */
static void ms_ok(void) {
    if (ms_state == MS_PLAY) {
        if (ms_cur_cell == 3) return;               /* '=' 两根不可动 */
        if (!(ms_mask[ms_cur_cell] & (1u << ms_cur_slot))) return;  /* 空槽 */
        ms_pc = ms_cur_cell;
        ms_ps = ms_cur_slot;
        ms_state = MS_CARRY;
        ms_status = MS_ST_HOLD;
        audio_select();                     /* 拿起火柴 */
        return;
    }
    /* MS_CARRY: 落子 */
    if (ms_cur_cell == 3) { ms_status = MS_ST_EQFIX; return; }
    if (ms_cur_cell == ms_pc && ms_cur_slot == ms_ps) {
        ms_status = MS_ST_SAME;                     /* 原位放回 = 取消 */
        ms_state = MS_PLAY;
        return;
    }
    if (ms_mask[ms_cur_cell] & (1u << ms_cur_slot)) {
        ms_status = MS_ST_TAKEN;                    /* 目标已有火柴 */
        ms_state = MS_PLAY;
        return;
    }
    uint8_t w[5];
    if (!ms_apply(ms_mask, ms_pc, ms_ps, ms_cur_cell, ms_cur_slot, w)) {
        ms_status = MS_ST_INVALID;                  /* 字形非法: 保持手持 */
        return;
    }
    for (int i = 0; i < 5; i++) ms_mask[i] = w[i];
    ms_moves++;
    ms_state = MS_PLAY;
    if (ms_eval(ms_mask)) {
        ms_state = MS_WIN;                          /* 等式成立 → 通关 */
        ms_over_full = false;
        audio_win();
        led_fx_set(LED_FX_WIN);
    } else if (ms_count_solutions(ms_mask) == 0) {
        ms_status = MS_ST_NOHINT;                   /* 已无一步正解 */
        audio_error();
    } else {
        ms_status = MS_ST_WRONG;
        audio_error();                              /* 放错位置 */
    }
}

/* ---- 渲染 ---- */

/* 格内各段条状几何(以格左上为原点): 横条 24x8, 竖条 8x12 */
static const int8_t ms_bar[7][4] = {
    {4,  4, 24,  8},   /* T  上横 */
    {4, 12,  8, 12},   /* UL 左上竖 */
    {20, 12,  8, 12},  /* UR 右上竖 */
    {4, 24, 24,  8},   /* M  中横 */
    {4, 32,  8, 12},   /* LL 左下竖 */
    {20, 32,  8, 12},  /* LR 右下竖 */
    {4, 44, 24,  8},   /* B  下横 */
};

#define MS_CELL_X(c) (48 + (c) * 42)
#define MS_CELL_Y 24

/* 段槽位 -> 条矩形; 无效槽返回 false */
static bool ms_seg_rect(int cell, int slot, int *x, int *y, int *w, int *h) {
    if (cell == 1) {
        if (slot == 0) { *x = 4;  *y = 24; *w = 24; *h = 8;  return true; }
        if (slot == 1) { *x = 12; *y = 8;  *w = 8;  *h = 40; return true; }
        return false;
    }
    if (cell == 3) {
        if (slot == 0) { *x = 4;  *y = 10; *w = 24; *h = 8; return true; }
        if (slot == 1) { *x = 4;  *y = 38; *w = 24; *h = 8; return true; }
        return false;
    }
    if (slot < 0 || slot > 6) return false;
    *x = ms_bar[slot][0];
    *y = ms_bar[slot][1];
    *w = ms_bar[slot][2];
    *h = ms_bar[slot][3];
    return true;
}

/* 画一个格的符号; 手里那根(搬运中)不画实心 */
static void ms_draw_glyph(int cell) {
    int ox = MS_CELL_X(cell);
    for (int s = 0; s < ms_nslots(cell); s++) {
        if (!(ms_mask[cell] & (1u << s))) continue;
        if (ms_state == MS_CARRY && cell == ms_pc && s == ms_ps) continue;
        int x, y, w, h;
        ms_seg_rect(cell, s, &x, &y, &w, &h);
        fb_fill_rect(ox + x, MS_CELL_Y + y, w, h, true);
    }
}

/* 光标: 当前格粗框(最后画盖住符号) + 选中段空心条 */
static void ms_draw_cursor(void) {
    int cx = MS_CELL_X(ms_cur_cell);
    fb_stroke_rect_thick(cx - 3, MS_CELL_Y - 3, 38, 62, 2, true);
    if (ms_state == MS_CARRY) {
        int px = MS_CELL_X(ms_pc);
        fb_stroke_rect(px - 2, MS_CELL_Y - 2, 36, 60, true);   /* 原格细框 */
        int x, y, w, h;
        ms_seg_rect(ms_pc, ms_ps, &x, &y, &w, &h);
        fb_stroke_rect_thick(px + x, MS_CELL_Y + y, w, h, 2, true); /* 原位空心 */
        if (ms_mask[ms_cur_cell] & (1u << ms_cur_slot)) {   /* 目标已有则空心提示 */
            ms_seg_rect(ms_cur_cell, ms_cur_slot, &x, &y, &w, &h);
            fb_stroke_rect_thick(cx + x, MS_CELL_Y + y, w, h, 2, true);
        }
    } else if (ms_mask[ms_cur_cell] & (1u << ms_cur_slot)) {
        int x, y, w, h;
        ms_seg_rect(ms_cur_cell, ms_cur_slot, &x, &y, &w, &h);
        fb_stroke_rect_thick(cx + x, MS_CELL_Y + y, w, h, 2, true);
    }
}

static const char *ms_status_text[8] = {
    "FIX THE EQUATION - MOVE ONE MATCH",
    "HOLDING - OK TO DROP",
    "STILL WRONG - KEEP TRYING",
    "NO ONE-MOVE FIX - UNDO",
    "INVALID - UNDONE",
    "SPOT OCCUPIED - CANCELLED",
    "NO MOVE - CANCELLED",
    "= IS FIXED - CANCELLED",
};

static void ms_draw_status(void) {
    const char *s = ms_status_text[ms_status];
    int tw = text_width(s);
    int sx = (CCG_W - tw) / 2;
    if (ms_state == MS_CARRY) {
        /* 手里火柴小图标(横/竖条) */
        int x, y, w, h;
        ms_seg_rect(ms_pc, ms_ps, &x, &y, &w, &h);
        bool horiz = h <= w;
        int iw = horiz ? 14 : 5;
        int ih = horiz ? 5 : 14;
        fb_fill_rect(sx - 16, 94 + (7 - ih) / 2, iw, ih, true);
    }
    fb_text(sx, 94, s, true);
}

static void ms_draw_hud_play(void) {
    fb_text(0, 0, "MATCH", true);
    char line[16];
    int m = 0;
    line[m++] = 'M';
    int v = ms_moves;
    if (v == 0) {
        line[m++] = '0';
    } else {
        char d[4];
        int k = 0;
        while (v > 0 && k < 3) { d[k++] = (char)('0' + v % 10); v /= 10; }
        while (k > 0) line[m++] = d[--k];
    }
    line[m++] = ' ';
    int pn = ms_puzzle + 1;
    if (pn >= 10) line[m++] = '1';
    line[m++] = (char)('0' + pn % 10);
    line[m++] = '/';
    line[m++] = '2';
    line[m++] = '0';
    line[m] = 0;
    fb_text(CCG_W - 2 - text_width(line), 0, line, true);
    fb_hline(0, CCG_HUD_H - 1, CCG_W, true);
}

static void ms_draw_hud_win(void) {
    fb_fill_rect(0, 0, CCG_W, CCG_HUD_H - 1, false);
    fb_text(2, 2, "SOLVED!", true);
    fb_text(CCG_W - 2 - text_width("OK/N:RETRY BACK:QUIT"), 2,
            "OK/N:RETRY BACK:QUIT", true);
    char line[16];
    int m = 0;
    const char *pre = "MOVES ";
    while (pre[m]) { line[m] = pre[m]; m++; }
    int v = ms_moves;
    if (v == 0) {
        line[m++] = '0';
    } else {
        char d[4];
        int k = 0;
        while (v > 0 && k < 3) { d[k++] = (char)('0' + v % 10); v /= 10; }
        while (k > 0) line[m++] = d[--k];
    }
    line[m] = 0;
    fb_text(2, 10, line, true);
    fb_text(CCG_W - 2 - text_width("N:NEW GAME"), 10, "N:NEW GAME", true);
}

void matchstick_render(void) {
    fb_clear(false);
    if (ms_state == MS_WIN) {
        ms_draw_hud_win();
        for (int i = 0; i < 5; i++) ms_draw_glyph(i);   /* 墙内: 成立的等式 */
        if (!ms_over_full) { ms_over_full = true; disp_force_full(); }
        return;
    }
    ms_draw_hud_play();
    for (int i = 0; i < 5; i++) ms_draw_glyph(i);
    ms_draw_cursor();
    ms_draw_status();
    fb_text_center(138, "ARROWS MOVE  OK PICK/DROP  N NEXT  Q QUIT", true);
}

/* ---- 输入 ---- */
void matchstick_on_key(const key_event_t *ev) {
    if (ev->is_repeat &&
        ev->key != K_UP && ev->key != K_DOWN &&
        ev->key != K_LEFT && ev->key != K_RIGHT)
        return;                          /* 确认键/字母忽略重复 */
    if (ms_state == MS_WIN) {
        if (ev->key == K_OK || (ev->key == K_CHAR && ev->ch == 'n'))
            ms_next_puzzle();
        else if (ev->key == K_BACK || ev->key == K_QUIT)
            s_exit_request = true;
        return;
    }
    switch (ev->key) {
    case K_LEFT:
        ms_cur_cell = (ms_cur_cell + 4) % 5;
        if (ms_cur_slot >= ms_nslots(ms_cur_cell)) ms_cur_slot = 0;
        break;
    case K_RIGHT:
        ms_cur_cell = (ms_cur_cell + 1) % 5;
        if (ms_cur_slot >= ms_nslots(ms_cur_cell)) ms_cur_slot = 0;
        break;
    case K_UP:
        ms_cur_slot = (ms_cur_slot + ms_nslots(ms_cur_cell) - 1) %
                      ms_nslots(ms_cur_cell);
        break;
    case K_DOWN:
        ms_cur_slot = (ms_cur_slot + 1) % ms_nslots(ms_cur_cell);
        break;
    case K_CHAR:
        if (ev->ch == 'a') {
            ms_cur_cell = (ms_cur_cell + 4) % 5;
            if (ms_cur_slot >= ms_nslots(ms_cur_cell)) ms_cur_slot = 0;
        } else if (ev->ch == 'd') {
            ms_cur_cell = (ms_cur_cell + 1) % 5;
            if (ms_cur_slot >= ms_nslots(ms_cur_cell)) ms_cur_slot = 0;
        } else if (ev->ch == 'w') {
            ms_cur_slot = (ms_cur_slot + ms_nslots(ms_cur_cell) - 1) %
                          ms_nslots(ms_cur_cell);
        } else if (ev->ch == 's') {
            ms_cur_slot = (ms_cur_slot + 1) % ms_nslots(ms_cur_cell);
        } else if (ev->ch == 'n') {
            ms_next_puzzle();
        } else if (ev->ch == 'q') {
            s_exit_request = true;
        }
        break;
    case K_OK:
        ms_ok();
        break;
    case K_BACK:
    case K_PAUSE:
        ms_pause();
        break;
    case K_QUIT:
        s_exit_request = true;
        break;
    default:
        break;
    }
}

/* ---- 框架 ---- */
void matchstick_enter(void) {
    rng_seed(&ms_rng, now_ms() ^ 0x4D53u ^ ((uint64_t)ms_seed_cnt++ << 32));
    for (int i = 0; i < MS_NPUZ; i++) ms_order[i] = i;
    for (int i = MS_NPUZ - 1; i > 0; i--) {     /* Fisher-Yates 洗牌 */
        int j = (int)rng_range(&ms_rng, (uint32_t)(i + 1));
        int t = ms_order[i];
        ms_order[i] = ms_order[j];
        ms_order[j] = t;
    }
    ms_puzzle = ms_order[0];
    ms_load_puzzle(ms_puzzle);
    matchstick_render();
    disp_full();
}

void matchstick_exit(void) {}

void matchstick_tick(uint64_t now) { (void)now; }
