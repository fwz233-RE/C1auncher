/* TENTS — 帐篷: 每棵树旁配一顶相邻帐篷
 *
 * 规则: 每棵树恰有 1 顶上下左右相邻的帐篷; 帐篷互不相邻(含对角);
 *       每行/每列帐篷数 = 边沿数字(顶/右)。全部满足 → SOLVED。
 *
 * 布局(296x152):
 *   y 0..15    HUD 顶栏(左 TENTS, 右 "<树名> TENT k/N ERR n")
 *   y 17..23   列提示数字(每列居中于格上方)
 *   y 24..150  7x7 格 18px/格 126x126(x 85..211); 行提示右对齐 x=222
 * 格内符号: 树=实心方块(内缩 2px); 帐篷=实心三角(顶点朝上);
 *           草=三点标记。光标=反色 2px 边框(全部格画完后最后画)。
 *
 * 谜题: 3 个内置(6/9/10 树), 行/列帐篷数由唯一解推导。
 *       唯一解性经生成脚本(独立回溯) + 单测(测试侧独立求解器)双重验证。
 *
 * 操作: 方向/WASD 移光标; OK 循环 空→帐篷→草→空(树上无效);
 *       Delete/X 清格; N 换下一题; BACK 暂停; Q 退出。
 *
 * 静态前缀 tn_; 像素坐标一律 int; 零 malloc。
 *
 * 集成提示(help[] ≤5 行, games_table.c 填):
 *   "EACH TREE NEEDS ONE ADJACENT TENT",
 *   "TENTS NEVER TOUCH (NOT DIAGONAL)",
 *   "MATCH ROW/COL COUNTS ON EDGES",
 *   "OK: EMPTY>TENT>GRASS  DEL: CLEAR",
 *   "ARROWS/WASD MOVE  N: NEXT", NULL
 */
#include "game.h"
#include "../config.h"
#include "../gfx/canvas.h"
#include "../gfx/font.h"
#include "../platform/display.h"
#include "../platform/input.h"
#include "../platform/audio.h"
#include "../platform/led.h"
#include <string.h>

#define TN_N 7                 /* 7x7 盘面 */
#define TN_CELL 18             /* 格 18px */
#define TN_GRID (TN_N * TN_CELL)            /* 126 */
#define TN_GRID_X ((CCG_W - TN_GRID) / 2)   /* 85, 水平居中 */
#define TN_GRID_Y 24           /* 列提示带(17..23)下方 */
#define TN_HINT_Y 17           /* 列提示数字行 */
#define TN_HINT_X 222          /* 行提示右对齐边 */
#define TN_PUZZLES 3

enum { TN_EMPTY = 0, TN_TENT, TN_GRASS };

/* 树布局: 'T'=树 '.'=空地. 每谜题行/列帐篷数(边沿数字)由唯一解推导 */
static const char tn_trees[TN_PUZZLES][TN_N][TN_N + 1] = {
    /* PINE — 6 树 (易) */
    {
        ".......",
        ".T...T.",
        "..T....",
        ".......",
        ".......",
        "....T..",
        "...TT..",
    },
    /* MAPLE — 9 树 (中) */
    {
        "....TT.",
        ".......",
        ".....T.",
        "T.T....",
        "T......",
        "T...T..",
        "....T..",
    },
    /* BIRCH — 10 树 (难) */
    {
        "...T..T",
        "....T..",
        "..T...T",
        ".......",
        "...T...",
        ".T.T..T",
        "...T...",
    },
};

static const uint8_t tn_rcnt[TN_PUZZLES][TN_N] = {
    { 2, 0, 1, 0, 1, 0, 2 },   /* PINE */
    { 2, 0, 3, 0, 2, 0, 2 },   /* MAPLE */
    { 2, 0, 2, 1, 1, 2, 2 },   /* BIRCH */
};

static const uint8_t tn_ccnt[TN_PUZZLES][TN_N] = {
    { 0, 1, 1, 1, 1, 2, 0 },   /* PINE */
    { 2, 1, 1, 2, 2, 0, 1 },   /* MAPLE */
    { 1, 1, 3, 0, 2, 1, 2 },   /* BIRCH */
};

static const char *tn_names[TN_PUZZLES] = { "PINE", "MAPLE", "BIRCH" };

static uint8_t tn_cell[TN_N * TN_N];  /* 玩家盘面 */
static int tn_cx, tn_cy;              /* 光标 0..6 */
static uint8_t tn_puz;
static uint8_t tn_ntree;              /* 当前谜题树数 */
static bool tn_over;
static bool tn_over_full;             /* 结束全刷只做一次 */

void tents_render(void);              /* enter 先于 render 定义 */

static bool tn_is_tree(int r, int c) {
    return tn_trees[tn_puz][r][c] == 'T';
}

static int tn_count_tents(void) {
    int n = 0;
    for (int i = 0; i < TN_N * TN_N; i++)
        if (tn_cell[i] == TN_TENT) n++;
    return n;
}

/* 8 邻域相触帐篷对数(每对计 1) */
static int tn_touch_pairs(void) {
    int n = 0;
    for (int r = 0; r < TN_N; r++) {
        for (int c = 0; c < TN_N; c++) {
            if (tn_cell[r * TN_N + c] != TN_TENT) continue;
            for (int dr = -1; dr <= 1; dr++) {
                for (int dc = -1; dc <= 1; dc++) {
                    if (dr == 0 && dc == 0) continue;
                    int nr = r + dr, nc = c + dc;
                    if (nr < 0 || nr >= TN_N || nc < 0 || nc >= TN_N) continue;
                    if (tn_cell[nr * TN_N + nc] == TN_TENT) n++;
                }
            }
        }
    }
    return n / 2;
}

/* 邻树帐篷数 != 1 的树数 */
static int tn_tree_viol(void) {
    int n = 0;
    for (int r = 0; r < TN_N; r++) {
        for (int c = 0; c < TN_N; c++) {
            if (!tn_is_tree(r, c)) continue;
            int t = 0;
            if (r > 0 && tn_cell[(r - 1) * TN_N + c] == TN_TENT) t++;
            if (r < TN_N - 1 && tn_cell[(r + 1) * TN_N + c] == TN_TENT) t++;
            if (c > 0 && tn_cell[r * TN_N + c - 1] == TN_TENT) t++;
            if (c < TN_N - 1 && tn_cell[r * TN_N + c + 1] == TN_TENT) t++;
            if (t != 1) n++;
        }
    }
    return n;
}

/* 不邻任何树的帐篷数(孤儿帐篷) */
static int tn_tent_orphan(void) {
    int n = 0;
    for (int r = 0; r < TN_N; r++) {
        for (int c = 0; c < TN_N; c++) {
            if (tn_cell[r * TN_N + c] != TN_TENT) continue;
            int t = 0;
            if (r > 0 && tn_is_tree(r - 1, c)) t++;
            if (r < TN_N - 1 && tn_is_tree(r + 1, c)) t++;
            if (c > 0 && tn_is_tree(r, c - 1)) t++;
            if (c < TN_N - 1 && tn_is_tree(r, c + 1)) t++;
            if (t == 0) n++;
        }
    }
    return n;
}

/* 行/列帐篷数与边沿数字不符的行+列数 */
static int tn_count_viol(void) {
    int n = 0;
    for (int r = 0; r < TN_N; r++) {
        int t = 0;
        for (int c = 0; c < TN_N; c++)
            if (tn_cell[r * TN_N + c] == TN_TENT) t++;
        if (t != (int)tn_rcnt[tn_puz][r]) n++;
    }
    for (int c = 0; c < TN_N; c++) {
        int t = 0;
        for (int r = 0; r < TN_N; r++)
            if (tn_cell[r * TN_N + c] == TN_TENT) t++;
        if (t != (int)tn_ccnt[tn_puz][c]) n++;
    }
    return n;
}

static int tn_errors(void) {
    return tn_touch_pairs() + tn_tree_viol() + tn_tent_orphan() + tn_count_viol();
}

/* 完成判定: 帐篷数=树数, 无相触, 每树恰 1 邻帐篷, 无孤儿帐篷,
 * 行/列数全部符合(计数相等 + 每树恰 1 + 无孤儿 ⇒ 一一配对) */
static bool tn_solved(void) {
    return tn_count_tents() == (int)tn_ntree &&
           tn_touch_pairs() == 0 && tn_tree_viol() == 0 &&
           tn_tent_orphan() == 0 && tn_count_viol() == 0;
}

static void tents_start_puz(uint8_t idx) {
    tn_puz = (uint8_t)(idx % TN_PUZZLES);
    memset(tn_cell, 0, sizeof tn_cell);
    tn_cx = 0;
    tn_cy = 0;
    tn_ntree = 0;
    for (int r = 0; r < TN_N; r++)
        for (int c = 0; c < TN_N; c++)
            if (tn_trees[tn_puz][r][c] == 'T') tn_ntree++;
    tn_over = false;
    tn_over_full = false;
}

/* OK: 空→帐篷→草→空; 树上无效 */
static void tn_cycle(void) {
    int r = tn_cy, c = tn_cx;
    if (tn_is_tree(r, c)) { audio_error(); return; }   /* 树上不能放 */
    uint8_t v = tn_cell[r * TN_N + c];
    if (v == TN_EMPTY) v = TN_TENT;
    else if (v == TN_TENT) v = TN_GRASS;
    else v = TN_EMPTY;
    tn_cell[r * TN_N + c] = v;
    if (!tn_over && tn_solved()) {
        tn_over = true;
        tn_over_full = false;
        audio_win();
        led_fx_set(LED_FX_WIN);
    } else {
        audio_move();                     /* 循环放置 */
    }
}

/* Delete/X: 清格 */
static void tn_set_empty(void) {
    tn_cell[tn_cy * TN_N + tn_cx] = TN_EMPTY;
    audio_move();                         /* 清格 */
}

void tents_enter(void) {
    tents_start_puz(0);
    tents_render();
    disp_full();
}

void tents_exit(void) {}

void tents_tick(uint64_t now) { (void)now; }

/* ---- 数字格式化(≤3 位, 无 stdio) ---- */
static unsigned tn_fmt_num(char *out, int v) {
    char tmp[4];
    unsigned len = 0;
    if (v == 0) tmp[len++] = '0';
    while (v && len < 3) { tmp[len++] = (char)('0' + v % 10); v /= 10; }
    unsigned n = 0;
    while (len) out[n++] = tmp[--len];
    out[n] = 0;
    return n;
}

static void tn_put(char *buf, unsigned *n, unsigned cap, const char *s) {
    while (*s && *n < cap) buf[(*n)++] = *s++;
}

static void tn_put_num(char *buf, unsigned *n, unsigned cap, int v) {
    char tmp[4];
    unsigned len = tn_fmt_num(tmp, v);
    unsigned i = 0;
    while (i < len && *n < cap) buf[(*n)++] = tmp[i++];
}

/* 列提示: 每列数字水平居中于格上方 */
static void tn_draw_col_hints(void) {
    for (int c = 0; c < TN_N; c++) {
        char b[4];
        tn_fmt_num(b, (int)tn_ccnt[tn_puz][c]);
        int w = text_width(b);
        fb_text(TN_GRID_X + c * TN_CELL + (TN_CELL - w) / 2, TN_HINT_Y, b, true);
    }
}

/* 行提示: 右对齐贴网格, 每行垂直居中 */
static void tn_draw_row_hints(void) {
    for (int r = 0; r < TN_N; r++) {
        char b[4];
        tn_fmt_num(b, (int)tn_rcnt[tn_puz][r]);
        int y = TN_GRID_Y + r * TN_CELL + (TN_CELL - 7) / 2;
        fb_text(TN_HINT_X - text_width(b), y, b, true);
    }
}

/* 帐篷: 实心三角, 顶点朝上, 内缩 1px 不压格线 */
static void tn_draw_tent(int cx, int cy) {
    for (int dy = 0; dy <= 13; dy++) {
        int hw = (dy * 7 + 6) / 13;   /* 0..7, 底宽 15 */
        fb_hline(cx + 9 - hw, cy + 2 + dy, hw * 2 + 1, true);
    }
}

static void tn_draw_cells(void) {
    for (int r = 0; r < TN_N; r++) {
        for (int c = 0; c < TN_N; c++) {
            int cx = TN_GRID_X + c * TN_CELL;
            int cy = TN_GRID_Y + r * TN_CELL;
            if (tn_is_tree(r, c)) {
                fb_fill_rect(cx + 2, cy + 2, TN_CELL - 4, TN_CELL - 4, true);
            } else if (tn_cell[r * TN_N + c] == TN_TENT) {
                tn_draw_tent(cx, cy);
            } else if (tn_cell[r * TN_N + c] == TN_GRASS) {
                fb_pixel(cx + 5, cy + 9, true);
                fb_pixel(cx + 9, cy + 9, true);
                fb_pixel(cx + 13, cy + 9, true);
            }
        }
    }
}

static void tn_draw_grid(void) {
    for (int i = 0; i <= TN_N; i++) {
        fb_vline(TN_GRID_X + i * TN_CELL, TN_GRID_Y, TN_GRID, true);
        fb_hline(TN_GRID_X, TN_GRID_Y + i * TN_CELL, TN_GRID, true);
    }
}

/* 光标: 反色 2px 边框, 所有格画完后最后画(黑块白边/白块黑边) */
static void tn_draw_cursor(void) {
    int cx = TN_GRID_X + tn_cx * TN_CELL;
    int cy = TN_GRID_Y + tn_cy * TN_CELL;
    bool dark = tn_is_tree(tn_cy, tn_cx) ||
                tn_cell[tn_cy * TN_N + tn_cx] == TN_TENT;
    fb_stroke_rect_thick(cx - 2, cy - 2, TN_CELL + 4, TN_CELL + 4, 2, !dark);
}

static void tn_draw_hud(void) {
    if (tn_over) {
        fb_fill_rect(0, 0, CCG_W, CCG_HUD_H - 1, false);
        fb_text(2, 2, "SOLVED!", true);
        fb_text(CCG_W - 2 - text_width("OK/N:NEXT BACK:QUIT"), 2,
                "OK/N:NEXT BACK:QUIT", true);
        if (!tn_over_full) { tn_over_full = true; disp_force_full(); }
        return;
    }
    fb_text(0, 0, "TENTS", true);
    /* 右标签: "<树名> TENT k/N ERR n" */
    char buf[32];
    unsigned n = 0;
    tn_put(buf, &n, sizeof buf, tn_names[tn_puz]);
    tn_put(buf, &n, sizeof buf, " TENT ");
    tn_put_num(buf, &n, sizeof buf, tn_count_tents());
    tn_put(buf, &n, sizeof buf, "/");
    tn_put_num(buf, &n, sizeof buf, (int)tn_ntree);
    int err = tn_errors();
    if (err > 0) {
        tn_put(buf, &n, sizeof buf, " ERR ");
        tn_put_num(buf, &n, sizeof buf, err);
    }
    buf[n] = 0;
    fb_text(CCG_W - 4 - text_width(buf), 0, buf, true);
    fb_hline(0, CCG_HUD_H - 1, CCG_W, true);
}

void tents_render(void) {
    fb_clear(false);
    tn_draw_col_hints();
    tn_draw_row_hints();
    tn_draw_cells();
    tn_draw_grid();
    tn_draw_cursor();
    tn_draw_hud();
}

void tents_on_key(const key_event_t *ev) {
    bool dir = ev->key == K_UP || ev->key == K_DOWN ||
               ev->key == K_LEFT || ev->key == K_RIGHT;
    if (ev->is_repeat && !dir) return;   /* 确认键/字母忽略重复, 方向可重复 */
    if (tn_over) {
        if (ev->key == K_OK || (ev->key == K_CHAR && ev->ch == 'n'))
            tents_start_puz((uint8_t)(tn_puz + 1));   /* 换题 */
        else if (ev->key == K_BACK || ev->key == K_QUIT)
            s_exit_request = true;
        return;
    }
    switch (ev->key) {
    case K_UP:    if (tn_cy > 0) tn_cy--; break;
    case K_DOWN:  if (tn_cy < TN_N - 1) tn_cy++; break;
    case K_LEFT:  if (tn_cx > 0) tn_cx--; break;
    case K_RIGHT: if (tn_cx < TN_N - 1) tn_cx++; break;
    case K_DEL: tn_set_empty(); break;
    case K_OK: tn_cycle(); break;
    case K_CHAR:
        switch (ev->ch) {
        case 'w': if (tn_cy > 0) tn_cy--; break;
        case 's': if (tn_cy < TN_N - 1) tn_cy++; break;
        case 'a': if (tn_cx > 0) tn_cx--; break;
        case 'd': if (tn_cx < TN_N - 1) tn_cx++; break;
        case 'x': tn_set_empty(); break;
        case 'n': tents_start_puz((uint8_t)(tn_puz + 1)); break;   /* N 换题 */
        default: break;
        }
        break;
    case K_BACK:
    case K_PAUSE: {
        pause_sel_t sel;
        if (ui_pause_run(&sel)) {
            if (sel == PAUSE_RESTART) tents_start_puz(tn_puz);
        } else {
            s_exit_request = true;
        }
        break;
    }
    case K_QUIT: s_exit_request = true; break;
    default: break;
    }
}
