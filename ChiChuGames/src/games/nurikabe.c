/* NURIKABE — 数墙: 8x8 岛与墙
 *
 * 布局(296x152):
 *   y 0..15    HUD 顶栏(左 NURIKABE, 右 "P<n> ISLES k" k=已满足岛数)
 *   y 16..151  8x8 格, 17px/格, 136x136, 水平居中 (x 80..215)
 *
 * 操作: 方向/WASD 移光标; OK 黑/白切换(数字格锁定); DEL 清为未知(点);
 *       N 下一题; BACK 暂停菜单; P 暂停; Q 退出
 * 完成: 无未知格 + 墙连通成一片 + 无 2x2 黑块 + 每岛格数=数字 → SOLVED
 *
 * 内置 3 题: 由生成脚本构造并验证唯一解(见 ROM 表下方注释), 难度递进。
 * 静态前缀 nk_; 像素坐标一律 int; 零 malloc; ASCII 文本。
 *
 * 集成提示(help[] 最多 5 行 + NULL, games_table.c 使用):
 *   "MAKE ISLANDS & ONE WALL",
 *   "ARROWS/WASD MOVE  OK: B/W TOGGLE",
 *   "DEL: UNKNOWN DOT  N: NEXT PUZZLE",
 *   "ISLAND SIZE = ITS NUMBER",
 *   "WALL CONNECTED, NO 2X2 BLACK", NULL
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

#define NK_N 8                   /* 8x8 盘面 */
#define NK_PUZZLES 3
#define NK_CELL 17               /* 格 17px */
#define NK_GRID (NK_N * NK_CELL) /* 136 */
#define NK_X ((CCG_W - NK_GRID) / 2)  /* 80, 水平居中 */
#define NK_Y CCG_HUD_H           /* 16, HUD 下方 */

enum { NK_UNKNOWN = 0, NK_WHITE, NK_BLACK };

/* ROM 谜题: 数字=该岛格数(<=9), '.'=待定.
 * 唯一解验证(C 求解器全量枚举, 2 解即中止):
 *   p1: 998 节点    p2: 12775 节点    p3: 143747 节点 */
static const char nk_puz[NK_PUZZLES][NK_N][NK_N + 1] = {
    { "3...1.2.",
      "........",
      "1.1.5...",
      "........",
      "3.3...1.",
      "........",
      "....1.1.",
      "........" },
    { "9...1.2.",
      "........",
      "1...7...",
      "........",
      "3.....1.",
      "........",
      "......1.",
      "........" },
    { "........",
      "9.......",
      "........",
      "7.......",
      "........",
      "1...3...",
      "........",
      "3...1.1." },
};

static uint8_t nk_cell[NK_N * NK_N];  /* 玩家盘面: 未知/白/黑 */
static int nk_cx, nk_cy;              /* 光标 0..7 */
static uint8_t nk_idx;                /* 当前题 0..2 */
static bool nk_over;
static bool nk_over_full;             /* 结束全刷只做一次 */

/* 从 idx 泛洪 4 连通白格; out[] 填格序, 返回格数 */
static int nk_flood_white(uint8_t seen[64], int idx, int out[64]) {
    int q[64], head = 0, tail = 0;
    int n = 0;
    q[tail++] = idx;
    seen[idx] = 1;
    while (head < tail) {
        int i = q[head++];
        out[n++] = i;
        int x = i % NK_N, y = i / NK_N;
        if (x > 0 && !seen[i - 1] && nk_cell[i - 1] == NK_WHITE) {
            seen[i - 1] = 1;
            q[tail++] = i - 1;
        }
        if (x < NK_N - 1 && !seen[i + 1] && nk_cell[i + 1] == NK_WHITE) {
            seen[i + 1] = 1;
            q[tail++] = i + 1;
        }
        if (y > 0 && !seen[i - NK_N] && nk_cell[i - NK_N] == NK_WHITE) {
            seen[i - NK_N] = 1;
            q[tail++] = i - NK_N;
        }
        if (y < NK_N - 1 && !seen[i + NK_N] && nk_cell[i + NK_N] == NK_WHITE) {
            seen[i + NK_N] = 1;
            q[tail++] = i + NK_N;
        }
    }
    return n;
}

/* 全部规则校验(盘面必须已填满):
 * 岛: 每个白区恰含 1 个数字且格数=数字;
 * 墙: 无 2x2 黑块, 且连通成一片 */
static bool nk_check(void) {
    uint8_t seen[64] = { 0 };
    for (int i = 0; i < 64; i++) {
        if (nk_cell[i] == NK_WHITE && !seen[i]) {
            int comp[64];
            int n = nk_flood_white(seen, i, comp);
            int clue = 0;
            for (int k = 0; k < n; k++) {
                char pc = nk_puz[nk_idx][comp[k] / NK_N][comp[k] % NK_N];
                if (pc != '.') {
                    if (clue) return false;        /* 两数字岛相触(斜角除外) */
                    clue = pc - '0';
                }
            }
            if (clue == 0) return false;           /* 无数字的白区 */
            if (n != clue) return false;           /* 岛格数 != 数字 */
        }
    }
    /* 无 2x2 黑块 */
    for (int y = 0; y < NK_N - 1; y++) {
        for (int x = 0; x < NK_N - 1; x++) {
            if (nk_cell[y * NK_N + x] == NK_BLACK &&
                nk_cell[y * NK_N + x + 1] == NK_BLACK &&
                nk_cell[(y + 1) * NK_N + x] == NK_BLACK &&
                nk_cell[(y + 1) * NK_N + x + 1] == NK_BLACK)
                return false;
        }
    }
    /* 墙连通成一片 */
    int bcount = 0, start = -1;
    for (int i = 0; i < 64; i++)
        if (nk_cell[i] == NK_BLACK) {
            bcount++;
            if (start < 0) start = i;
        }
    if (bcount == 0) return false;
    {
        uint8_t bseen[64] = { 0 };
        int q[64], head = 0, tail = 0, reached = 0;
        q[tail++] = start;
        bseen[start] = 1;
        while (head < tail) {
            int i = q[head++];
            reached++;
            int x = i % NK_N, y = i / NK_N;
            if (x > 0 && !bseen[i - 1] && nk_cell[i - 1] == NK_BLACK) {
                bseen[i - 1] = 1;
                q[tail++] = i - 1;
            }
            if (x < NK_N - 1 && !bseen[i + 1] && nk_cell[i + 1] == NK_BLACK) {
                bseen[i + 1] = 1;
                q[tail++] = i + 1;
            }
            if (y > 0 && !bseen[i - NK_N] && nk_cell[i - NK_N] == NK_BLACK) {
                bseen[i - NK_N] = 1;
                q[tail++] = i - NK_N;
            }
            if (y < NK_N - 1 && !bseen[i + NK_N] && nk_cell[i + NK_N] == NK_BLACK) {
                bseen[i + NK_N] = 1;
                q[tail++] = i + NK_N;
            }
        }
        if (reached != bcount) return false;
    }
    return true;
}

/* 已满足的数字岛数(HUD 反馈): 白区含 1 个数字且格数=数字 */
static int nk_isles_ok(void) {
    uint8_t seen[64] = { 0 };
    int ok = 0;
    for (int i = 0; i < 64; i++) {
        if (nk_cell[i] == NK_WHITE && !seen[i]) {
            int comp[64];
            int n = nk_flood_white(seen, i, comp);
            int clue = 0;
            for (int k = 0; k < n; k++) {
                char pc = nk_puz[nk_idx][comp[k] / NK_N][comp[k] % NK_N];
                if (pc != '.') {
                    if (clue) break;
                    clue = pc - '0';
                }
            }
            if (clue && n == clue) ok++;
        }
    }
    return ok;
}

/* 胜: 无未知格且全部规则满足 */
static bool nk_solved(void) {
    for (int i = 0; i < 64; i++)
        if (nk_cell[i] == NK_UNKNOWN) return false;
    return nk_check();
}

static void nk_start(uint8_t idx) {
    nk_idx = (uint8_t)(idx % NK_PUZZLES);
    /* 数字格恒为白(锁定, 渲染直接画数字); 其余格未知 */
    for (int i = 0; i < NK_N * NK_N; i++)
        nk_cell[i] = (nk_puz[nk_idx][i / NK_N][i % NK_N] != '.') ?
                     NK_WHITE : NK_UNKNOWN;
    nk_cx = 0;
    nk_cy = 0;
    nk_over = false;
    nk_over_full = false;
}

/* OK: 黑/白切换(未知→黑→白→黑); 数字格锁定 */
static void nk_toggle(void) {
    int i = nk_cy * NK_N + nk_cx;
    if (nk_puz[nk_idx][nk_cy][nk_cx] != '.') { audio_error(); return; }
    nk_cell[i] = (nk_cell[i] == NK_BLACK) ? NK_WHITE : NK_BLACK;
    if (!nk_over && nk_solved()) {
        nk_over = true;
        nk_over_full = false;
        audio_win();               /* 全部规则满足 */
        led_fx_set(LED_FX_WIN);
    } else {
        audio_select();            /* 填格 */
    }
}

/* DEL: 清为未知(点); 数字格锁定 */
static void nk_clear_cell(void) {
    int i = nk_cy * NK_N + nk_cx;
    if (nk_puz[nk_idx][nk_cy][nk_cx] != '.') { audio_error(); return; }
    nk_cell[i] = NK_UNKNOWN;
    if (!nk_over && nk_solved()) {
        nk_over = true;
        nk_over_full = false;
        audio_win();               /* 清格完成即胜 */
        led_fx_set(LED_FX_WIN);
    } else {
        audio_select();            /* 清为未知 */
    }
}

void nurikabe_render(void);   /* enter 先于 render 定义 */

void nurikabe_enter(void) {
    nk_start(0);
    nurikabe_render();
    disp_full();
}

void nurikabe_exit(void) {}

void nurikabe_tick(uint64_t now) { (void)now; }

void nurikabe_render(void) {
    fb_clear(false);
    /* 格线(先画, 黑格覆盖后成为实心墙) */
    for (int i = 0; i <= NK_N; i++) {
        fb_vline(NK_X + i * NK_CELL, NK_Y, NK_GRID, true);
        fb_hline(NK_X, NK_Y + i * NK_CELL, NK_GRID, true);
    }
    /* 格内容 */
    for (int r = 0; r < NK_N; r++) {
        for (int c = 0; c < NK_N; c++) {
            int i = r * NK_N + c;
            int cx = NK_X + c * NK_CELL;
            int cy = NK_Y + r * NK_CELL;
            char pc = nk_puz[nk_idx][r][c];
            if (pc != '.') {
                /* 数字格: 2x 大字居中(10x14 在 17px 格内) */
                char buf[2] = { pc, 0 };
                fb_text_scale2(cx + (NK_CELL - 10) / 2, cy + (NK_CELL - 14) / 2,
                               buf, true);
            } else if (nk_cell[i] == NK_BLACK) {
                fb_fill_rect(cx, cy, NK_CELL, NK_CELL, true);   /* 实心墙 */
            } else if (nk_cell[i] == NK_UNKNOWN) {
                /* 未知: 中心 5x5 点 */
                fb_fill_rect(cx + (NK_CELL - 5) / 2, cy + (NK_CELL - 5) / 2,
                             5, 5, true);
            }
        }
    }
    /* 光标: 反色 2px 边框, 所有格画完后最后画 */
    {
        int ccx = NK_X + nk_cx * NK_CELL;
        int ccy = NK_Y + nk_cy * NK_CELL;
        bool inv = nk_cell[nk_cy * NK_N + nk_cx] != NK_BLACK;
        fb_stroke_rect_thick(ccx - 2, ccy - 2, NK_CELL + 4, NK_CELL + 4, 2, inv);
    }
    if (nk_over) {
        fb_fill_rect(0, 0, CCG_W, CCG_HUD_H - 1, false);
        fb_text(2, 2, "SOLVED!", true);
        fb_text(CCG_W - 2 - text_width("OK/N:NEXT BACK:QUIT"), 2,
                "OK/N:NEXT BACK:QUIT", true);
        if (!nk_over_full) { nk_over_full = true; disp_force_full(); }
    } else {
        fb_text(0, 0, "NURIKABE", true);
        /* 右标签: "P<n> ISLES k" */
        char buf[24];
        unsigned n = 0;
        buf[n++] = 'P';
        buf[n++] = (char)('1' + nk_idx);
        buf[n++] = ' ';
        const char *l = "ISLES ";
        while (*l && n < 22) buf[n++] = *l++;
        int ok = nk_isles_ok();
        char tmp[12];
        unsigned len = 0;
        if (ok == 0) tmp[len++] = '0';
        while (ok && len < 10) { tmp[len++] = (char)('0' + ok % 10); ok /= 10; }
        while (len && n < 22) buf[n++] = tmp[--len];
        buf[n] = 0;
        fb_text(CCG_W - 4 - text_width(buf), 0, buf, true);
        fb_hline(0, CCG_HUD_H - 1, CCG_W, true);
    }
}

void nurikabe_on_key(const key_event_t *ev) {
    bool dir = ev->key == K_UP || ev->key == K_DOWN ||
               ev->key == K_LEFT || ev->key == K_RIGHT;
    if (ev->is_repeat && !dir) return;   /* 确认键/字母忽略重复, 方向可重复 */
    if (nk_over) {
        if (ev->key == K_OK || (ev->key == K_CHAR && ev->ch == 'n'))
            nk_start((uint8_t)(nk_idx + 1));   /* 下一题 */
        else if (ev->key == K_BACK || ev->key == K_QUIT)
            s_exit_request = true;
        return;
    }
    switch (ev->key) {
    case K_UP: if (nk_cy > 0) { nk_cy--; if (!ev->is_repeat) audio_move(); } break;
    case K_DOWN: if (nk_cy < NK_N - 1) { nk_cy++; if (!ev->is_repeat) audio_move(); } break;
    case K_LEFT: if (nk_cx > 0) { nk_cx--; if (!ev->is_repeat) audio_move(); } break;
    case K_RIGHT: if (nk_cx < NK_N - 1) { nk_cx++; if (!ev->is_repeat) audio_move(); } break;
    case K_OK: nk_toggle(); break;
    case K_DEL: nk_clear_cell(); break;
    case K_CHAR:
        switch (ev->ch) {
        case 'w': if (nk_cy > 0) { nk_cy--; if (!ev->is_repeat) audio_move(); } break;
        case 's': if (nk_cy < NK_N - 1) { nk_cy++; if (!ev->is_repeat) audio_move(); } break;
        case 'a': if (nk_cx > 0) { nk_cx--; if (!ev->is_repeat) audio_move(); } break;
        case 'd': if (nk_cx < NK_N - 1) { nk_cx++; if (!ev->is_repeat) audio_move(); } break;
        case 'n': nk_start((uint8_t)(nk_idx + 1)); break;   /* N 下一题 */
        default: break;
        }
        break;
    case K_BACK:
    case K_PAUSE: {
        pause_sel_t sel;
        if (ui_pause_run(&sel)) {
            if (sel == PAUSE_RESTART) nk_start(nk_idx);
        } else {
            s_exit_request = true;
        }
        break;
    }
    case K_QUIT: s_exit_request = true; break;
    default: break;
    }
}
