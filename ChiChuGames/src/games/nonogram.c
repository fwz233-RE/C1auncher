/* NONOGRAM — 数织: 按行/列数字提示涂格成画
 *
 * 布局(296x152):
 *   y 0..15   HUD 顶栏(左 NONOGRAM, 右 "<图名> ERR n")
 *   y 16..22  列提示第 1 行(两组的上一组)
 *   y 23..29  列提示第 2 行(贴网格; 单组列也画在这行)
 *   y 30..149 10x10 格, 12px/格, 120x120 水平居中(x 88..207)
 *   x 0..85   行提示(右对齐, 每行垂直居中)
 * 注: 顶部提示带需两行(列可含 2 组), 网格因此从 y=30 起
 *  (设计稿 "y 从 18 起" 指游戏区从 HUD 下方开始, 提示带占 16..29)
 *
 * 操作: 方向/WASD 移光标; OK 涂黑(再按取消); X 键/Delete 标记叉(空位);
 *       N 换图; 涂错格数(HUD ERR)实时统计; 全对无多余 → SOLVED!
 *
 * 图案与提示正确性验证方法(三路互验):
 *  1. 开发期脚本(见 /tmp/verify_ng.py 思路): 独立重算每行/列连续黑格段
 *     (行程编码), 断言与图案逐位一致, 并约束 列<=2组 行<=5组 值1..10;
 *  2. 本文件 ng_compute_hints 与脚本同规则(图案 'X' 扫描),
 *     下方注释中贴出脚本输出的已验证提示表;
 *  3. 单测 tests 的 test_hints_crosscheck 在运行时用测试侧独立实现
 *     重算 5 张图所有行列提示, 与 ng_rhint/ng_chint 逐位对比。
 *
 * 已验证提示表(10 列):
 *  HEART rows [2 2][4 4][10][10][10][8][6][4][2][-]  cols [4][6][7][7][7][7][7][7][6][4]
 *  TREE  rows [2][2][2][4][4][6][6][8][8][10]        cols [1][3][5][7][10][10][7][5][3][1]
 *  HOUSE rows [2][4][6][8][10][8][8][2 2 2][2 2 2][2 2 2]
 *                                                   cols [1][7][8][6][10][10][6][8][7][1]
 *  FISH  rows [4][5][7][9][2 7][2 7][9][7][5][4]    cols [2][4][2 2][8][10][10][10][10][6][4]
 *  STAR  rows [2][2][4][4][10][2 4 2][10][4][4][2]  cols [3][3][1 1][7][10][10][7][1 1][3][3]
 *
 * 静态前缀 ng_; 像素坐标一律 int; 零 malloc。
 *
 * 集成提示(help[] 最多 5 行):
 *   "ARROWS/WASD: MOVE  OK: PAINT",
 *   "X OR DEL: MARK EMPTY",
 *   "N: NEXT PICTURE",
 *   "FILL ALL SHADED CELLS",
 *   "BACK: PAUSE  P: PAUSE", NULL
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

#define NG_N 10                    /* 10x10 盘面 */
#define NG_CELL 12                 /* 格 12px */
#define NG_PUZZLES 5
#define NG_GRID_X ((CCG_W - NG_N * NG_CELL) / 2)  /* 88, 水平居中 */
#define NG_GRID_Y 30               /* 列提示带(16/23)下方 */
#define NG_HINT_L1_Y 16            /* 列提示第 1 行(两组的上一组) */
#define NG_HINT_L2_Y 23            /* 列提示第 2 行(贴网格) */
#define NG_HINT_RIGHT 85           /* 行提示右对齐边 */

enum { NG_EMPTY = 0, NG_BLACK, NG_X };   /* 格状态 */

/* 5 张 10x10 图案: 'X'=黑格 '.'=空格
 * 每张的行/列提示与图案一致性由上方所述脚本+单测验证 */
static const char ng_puz[NG_PUZZLES * NG_N][NG_N + 1] = {
    /* HEART */
    ".XX....XX.",
    "XXXX..XXXX",
    "XXXXXXXXXX",
    "XXXXXXXXXX",
    "XXXXXXXXXX",
    ".XXXXXXXX.",
    "..XXXXXX..",
    "...XXXX...",
    "....XX....",
    "..........",
    /* TREE */
    "....XX....",
    "....XX....",
    "....XX....",
    "...XXXX...",
    "...XXXX...",
    "..XXXXXX..",
    "..XXXXXX..",
    ".XXXXXXXX.",
    ".XXXXXXXX.",
    "XXXXXXXXXX",
    /* HOUSE */
    "....XX....",
    "...XXXX...",
    "..XXXXXX..",
    ".XXXXXXXX.",
    "XXXXXXXXXX",
    ".XXXXXXXX.",
    ".XXXXXXXX.",
    ".XX.XX.XX.",
    ".XX.XX.XX.",
    ".XX.XX.XX.",
    /* FISH */
    "....XXXX..",
    "...XXXXX..",
    "..XXXXXXX.",
    ".XXXXXXXXX",
    "XX.XXXXXXX",
    "XX.XXXXXXX",
    ".XXXXXXXXX",
    "..XXXXXXX.",
    "...XXXXX..",
    "....XXXX..",
    /* STAR */
    "....XX....",
    "....XX....",
    "...XXXX...",
    "...XXXX...",
    "XXXXXXXXXX",
    "XX.XXXX.XX",
    "XXXXXXXXXX",
    "...XXXX...",
    "...XXXX...",
    "....XX....",
};

static const char *ng_names[NG_PUZZLES] = {
    "HEART", "TREE", "HOUSE", "FISH", "STAR"
};

void nonogram_render(void);   /* enter 先于 render 定义 */

static uint8_t ng_cell[NG_N * NG_N];  /* 玩家盘面 */
static uint8_t ng_rhint[NG_N][5];     /* 行提示(最多 5 组) */
static uint8_t ng_rhint_n[NG_N];
static uint8_t ng_chint[NG_N][2];     /* 列提示(最多 2 组) */
static uint8_t ng_chint_n[NG_N];
static uint8_t ng_cx, ng_cy;          /* 光标 0..9 */
static uint8_t ng_puz_idx;
static uint32_t ng_wrong;             /* 涂错黑格数 */
static bool ng_over;
static bool ng_over_full;             /* 结束全刷只做一次 */

/* 扫描图案行/列, 生成连续黑格段提示(行程编码)
 * n<5 / n<2 为防御上限: 10 格内行至多 5 段, 列按布局约束至多 2 段
 * (5 张图案经脚本+单测验证不越界) */
static void ng_compute_hints(void) {
    for (int r = 0; r < NG_N; r++) {
        const char *p = ng_puz[ng_puz_idx * NG_N + r];
        int n = 0, run = 0;
        for (int c = 0; c <= NG_N; c++) {
            if (c < NG_N && p[c] == 'X') {
                run++;
            } else if (run) {
                if (n < 5) ng_rhint[r][n++] = (uint8_t)run;
                run = 0;
            }
        }
        ng_rhint_n[r] = (uint8_t)n;
    }
    for (int c = 0; c < NG_N; c++) {
        int n = 0, run = 0;
        for (int r = 0; r <= NG_N; r++) {
            if (r < NG_N && ng_puz[ng_puz_idx * NG_N + r][c] == 'X') {
                run++;
            } else if (run) {
                if (n < 2) ng_chint[c][n++] = (uint8_t)run;
                run = 0;
            }
        }
        ng_chint_n[c] = (uint8_t)n;
    }
}

/* 涂错黑格数: 黑格且非图案黑格 */
static uint32_t ng_count_wrong(void) {
    uint32_t w = 0;
    for (int r = 0; r < NG_N; r++) {
        const char *p = ng_puz[ng_puz_idx * NG_N + r];
        for (int c = 0; c < NG_N; c++)
            if (ng_cell[r * NG_N + c] == NG_BLACK && p[c] != 'X') w++;
    }
    return w;
}

/* 胜负: 所有图案黑格被涂黑, 且无多余黑格(叉只允许在空格上) */
static bool ng_solved(void) {
    for (int r = 0; r < NG_N; r++) {
        const char *p = ng_puz[ng_puz_idx * NG_N + r];
        for (int c = 0; c < NG_N; c++) {
            if (p[c] == 'X') {
                if (ng_cell[r * NG_N + c] != NG_BLACK) return false;
            } else if (ng_cell[r * NG_N + c] == NG_BLACK) {
                return false;
            }
        }
    }
    return true;
}

static void ng_start_puz(uint8_t idx) {
    ng_puz_idx = (uint8_t)(idx % NG_PUZZLES);
    memset(ng_cell, 0, sizeof ng_cell);
    ng_cx = 0;
    ng_cy = 0;
    ng_wrong = 0;
    ng_over = false;
    ng_over_full = false;
    ng_compute_hints();
}

/* OK: 涂黑, 再按取消; 叉格上按 OK 直接变黑 */
static void ng_paint(void) {
    int i = (int)ng_cy * NG_N + (int)ng_cx;
    if (ng_cell[i] == NG_BLACK) {
        ng_cell[i] = NG_EMPTY;
    } else {
        ng_cell[i] = NG_BLACK;
    }
    ng_wrong = ng_count_wrong();
    if (!ng_over && ng_solved()) {
        ng_over = true;
        ng_over_full = false;
        audio_win();               /* 涂满即胜 */
        led_fx_set(LED_FX_WIN);
    } else {
        audio_select();            /* 落子 */
    }
}

/* X/Delete: 空位标叉, 再按取消; 黑格上标叉=表达"此处应为空" */
static void ng_toggle_x(void) {
    int i = (int)ng_cy * NG_N + (int)ng_cx;
    if (ng_cell[i] == NG_X) {
        ng_cell[i] = NG_EMPTY;
    } else {
        ng_cell[i] = NG_X;
    }
    ng_wrong = ng_count_wrong();
    if (!ng_over && ng_solved()) {
        ng_over = true;
        ng_over_full = false;
        audio_win();               /* 标叉完成即胜 */
        led_fx_set(LED_FX_WIN);
    } else {
        audio_select();            /* 标叉 */
    }
}

void nonogram_enter(void) {
    ng_start_puz(0);
    nonogram_render();
    disp_full();
}

void nonogram_exit(void) {}

void nonogram_tick(uint64_t now) { (void)now; }

/* ---- 提示绘制 ---- */
static int ng_fmt_num(char *out, int v) {
    char tmp[3];
    int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v && n < 2) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
    int len = 0;
    while (n) out[len++] = tmp[--n];
    out[len] = 0;
    return len;
}

static int ng_num_width(int v) {
    char b[4];
    ng_fmt_num(b, v);
    return text_width(b);
}

/* 行提示: 右对齐贴网格, 组间 2px, 每行垂直居中 */
static void ng_draw_row_hints(void) {
    for (int r = 0; r < NG_N; r++) {
        int n = ng_rhint_n[r];
        if (n == 0) continue;
        int w = 0;
        for (int i = 0; i < n; i++) w += ng_num_width(ng_rhint[r][i]);
        w += (n - 1) * 2;
        int x = NG_HINT_RIGHT - w;
        int y = NG_GRID_Y + 3 + r * NG_CELL;
        for (int i = 0; i < n; i++) {
            char b[4];
            ng_fmt_num(b, ng_rhint[r][i]);
            fb_text(x, y, b, true);
            x += ng_num_width(ng_rhint[r][i]) + 2;
        }
    }
}

/* 列提示: 组自上而下, 最后一组贴网格, 每列水平居中 */
static void ng_draw_col_hints(void) {
    for (int c = 0; c < NG_N; c++) {
        int n = ng_chint_n[c];
        for (int i = 0; i < n; i++) {
            char b[4];
            ng_fmt_num(b, ng_chint[c][i]);
            int w = text_width(b);
            int y = (i == n - 1) ? NG_HINT_L2_Y : NG_HINT_L1_Y;
            fb_text(NG_GRID_X + c * NG_CELL + (NG_CELL - w) / 2, y, b, true);
        }
    }
}

static void ng_draw_grid(void) {
    for (int r = 0; r < NG_N; r++) {
        for (int c = 0; c < NG_N; c++) {
            int i = r * NG_N + c;
            int cx = NG_GRID_X + c * NG_CELL;
            int cy = NG_GRID_Y + r * NG_CELL;
            if (ng_cell[i] == NG_BLACK) {
                fb_fill_rect(cx, cy, NG_CELL, NG_CELL, true);
            } else if (ng_cell[i] == NG_X) {
                /* 对角斜叉(留 1px 边距, 不压格线) */
                for (int k = 1; k < NG_CELL - 1; k++) {
                    fb_pixel(cx + k, cy + k, true);
                    fb_pixel(cx + NG_CELL - 1 - k, cy + k, true);
                }
            }
        }
    }
    /* 格线: 黑格上融合, 空格上分隔 */
    for (int i = 0; i <= NG_N; i++) {
        fb_vline(NG_GRID_X + i * NG_CELL, NG_GRID_Y, NG_N * NG_CELL, true);
        fb_hline(NG_GRID_X, NG_GRID_Y + i * NG_CELL, NG_N * NG_CELL, true);
    }
}

void nonogram_render(void) {
    fb_clear(false);
    ng_draw_col_hints();
    ng_draw_row_hints();
    ng_draw_grid();
    /* 光标: 反色 2px 边框, 所有格画完后最后画(黑格白边/白格黑边) */
    {
        int ccx = NG_GRID_X + (int)ng_cx * NG_CELL;
        int ccy = NG_GRID_Y + (int)ng_cy * NG_CELL;
        bool inv = ng_cell[(int)ng_cy * NG_N + (int)ng_cx] != NG_BLACK;
        fb_stroke_rect_thick(ccx - 2, ccy - 2, NG_CELL + 4, NG_CELL + 4, 2, inv);
    }
    if (ng_over) {
        fb_fill_rect(0, 0, CCG_W, CCG_HUD_H - 1, false);
        fb_text(2, 2, "SOLVED!", true);
        fb_text(CCG_W - 2 - text_width("OK/N:NEXT BACK:QUIT"), 2,
                "OK/N:NEXT BACK:QUIT", true);
        if (!ng_over_full) { ng_over_full = true; disp_force_full(); }
    } else {
        fb_text(0, 0, "NONOGRAM", true);
        /* 右标签: "<图名> ERR n" */
        char buf[24];
        unsigned n = 0;
        const char *nm = ng_names[ng_puz_idx];
        while (*nm && n < 23) buf[n++] = *nm++;
        const char *e = " ERR ";
        while (*e && n < 23) buf[n++] = *e++;
        char tmp[12];
        unsigned len = 0;
        uint32_t v = ng_wrong;
        if (v == 0) tmp[len++] = '0';
        while (v && len < 10) { tmp[len++] = (char)('0' + v % 10u); v /= 10u; }
        while (len && n < 23) buf[n++] = tmp[--len];
        buf[n] = 0;
        fb_text(CCG_W - 4 - text_width(buf), 0, buf, true);
        fb_hline(0, CCG_HUD_H - 1, CCG_W, true);
    }
}

void nonogram_on_key(const key_event_t *ev) {
    bool dir = ev->key == K_UP || ev->key == K_DOWN ||
               ev->key == K_LEFT || ev->key == K_RIGHT;
    if (ev->is_repeat && !dir) return;   /* 确认键/字母忽略重复, 方向可重复 */
    if (ng_over) {
        if (ev->key == K_OK || (ev->key == K_CHAR && ev->ch == 'n'))
            ng_start_puz((uint8_t)(ng_puz_idx + 1));   /* 换图 */
        else if (ev->key == K_BACK || ev->key == K_QUIT)
            s_exit_request = true;
        return;
    }
    switch (ev->key) {
    case K_UP: if (ng_cy > 0) { ng_cy--; if (!ev->is_repeat) audio_move(); } break;
    case K_DOWN: if (ng_cy < NG_N - 1) { ng_cy++; if (!ev->is_repeat) audio_move(); } break;
    case K_LEFT: if (ng_cx > 0) { ng_cx--; if (!ev->is_repeat) audio_move(); } break;
    case K_RIGHT: if (ng_cx < NG_N - 1) { ng_cx++; if (!ev->is_repeat) audio_move(); } break;
    case K_DEL: ng_toggle_x(); break;
    case K_OK: ng_paint(); break;
    case K_CHAR:
        switch (ev->ch) {
        case 'w': if (ng_cy > 0) { ng_cy--; if (!ev->is_repeat) audio_move(); } break;
        case 's': if (ng_cy < NG_N - 1) { ng_cy++; if (!ev->is_repeat) audio_move(); } break;
        case 'a': if (ng_cx > 0) { ng_cx--; if (!ev->is_repeat) audio_move(); } break;
        case 'd': if (ng_cx < NG_N - 1) { ng_cx++; if (!ev->is_repeat) audio_move(); } break;
        case 'x': ng_toggle_x(); break;
        case 'n': ng_start_puz((uint8_t)(ng_puz_idx + 1)); break;   /* N 换图 */
        default: break;
        }
        break;
    case K_BACK:
    case K_PAUSE: {
        pause_sel_t sel;
        if (ui_pause_run(&sel)) {
            if (sel == PAUSE_RESTART) ng_start_puz(ng_puz_idx);
        } else {
            s_exit_request = true;
        }
        break;
    }
    case K_QUIT: s_exit_request = true; break;
    default: break;
    }
}
