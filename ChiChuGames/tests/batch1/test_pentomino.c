/* PENTOMINO host 逻辑测试 — 直接包含 pentomino.c 访问静态状态
 * cc -DCHICHU_HOST -Isrc -Isrc/gfx test_pentomino.c 源.c 列表 */
#include "../src/config.h"
#include "../src/gfx/canvas.h"
#include "../src/gfx/font.h"
#include "../src/gfx/pattern.h"
#include "../src/rng.h"
#include "../src/games/pentomino.c"
#include <stdio.h>
#include <string.h>

static int s_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); s_fail++; } \
    else { printf("ok: %s\n", msg); } \
} while (0)

bool s_exit_request = false;
void game_set_tick_interval(uint32_t ms) { (void)ms; }

/* 模拟按键事件 */
static key_event_t ev_key(ccg_key k) { key_event_t e; e.key = k; e.ch = 0; e.is_repeat = false; return e; }
static key_event_t ev_char(uint8_t ch) { key_event_t e; e.key = K_CHAR; e.ch = ch; e.is_repeat = false; return e; }

/* 解出的棋盘上每种块出现 5 次 */
static void check_board_ok(void) {
    int cnt[12];
    memset(cnt, 0, sizeof(cnt));
    for (int i = 0; i < 60; i++) {
        if (pn_sb[i] == 0) continue;
        cnt[pn_sb[i] - 1]++;
    }
    for (int i = 0; i < 12; i++)
        CHECK(cnt[i] == 5, "solver uses each piece exactly 5 cells");
}

int main(void) {
    /* ---- 初始化 ---- */
    pentomino_enter();
    CHECK(pn_oris_done, "oris generated on enter");
    CHECK(pn_placed_count() == 0, "fresh board empty");

    /* ---- 朝向属性 ---- */
    {
        int sum = 0;
        for (int p = 0; p < 12; p++) {
            CHECK(pn_orin[p] >= 1 && pn_orin[p] <= 8, "orin in [1,8]");
            sum += pn_orin[p];
            for (int oi = 0; oi < pn_orin[p]; oi++) {
                const pn_ori_t *o = &pn_ori[p][oi];
                int minx = 99, miny = 99, maxx = -1, maxy = -1;
                for (int i = 0; i < 5; i++) {
                    int x = o->c[i] & 7, y = o->c[i] >> 3;
                    if (x < minx) minx = x;
                    if (y < miny) miny = y;
                    if (x > maxx) maxx = x;
                    if (y > maxy) maxy = y;
                }
                CHECK(minx == 0 && miny == 0, "orientation canonical");
                CHECK(maxx < o->w && maxy < o->h, "cells within w/h");
            }
        }
        CHECK(sum > 30, "orientation total plausible");
        printf("    orientation counts:");
        for (int p = 0; p < 12; p++) printf(" %c:%d", pn_letter[p], (int)pn_orin[p]);
        printf("\n");
    }
    CHECK(pn_orin[1] == 2, "I has 2 orientations");
    CHECK(pn_orin[5] == 4, "T has 4 orientations");
    CHECK(pn_orin[9] == 1, "X has 1 orientation");
    CHECK(pn_orin[6] == 4, "U has 4 orientations");
    CHECK(pn_orin[8] == 4, "W has 4 orientations");
    {
        int sum = 0;
        for (int p = 0; p < 12; p++) sum += pn_orin[p];
        CHECK(sum == 63, "total orientations = classic 63");
    }

    /* 12 块基准互不相同 */
    {
        int distinct = 0;
        for (int p = 0; p < 12; p++) {
            bool seen = false;
            for (int q = 0; q < p && !seen; q++) {
                bool same = (pn_base[p].w == pn_base[q].w && pn_base[p].h == pn_base[q].h);
                for (int i = 0; same && i < 5; i++) same = (pn_base[p].c[i] == pn_base[q].c[i]);
                if (same) seen = true;
            }
            if (!seen) distinct++;
        }
        CHECK(distinct == 12, "12 distinct base pieces");
    }

    /* ---- 求解器: 6x10 完整覆盖 ---- */
    {
        bool ok = pn_solve();
        CHECK(ok, "6x10 tiling found");
        if (ok) {
            check_board_ok();
            int empty = 0;
            for (int i = 0; i < 60; i++) if (pn_sb[i] == 0) empty++;
            CHECK(empty == 0, "board fully covered");
            printf("    solver nodes: %u\n", (unsigned)pn_sb_nodes);
        }
    }

    /* ---- 放置/移除逻辑 ---- */
    pn_start();
    pn_cur = 1;                    /* I */
    pn_cx = 0; pn_cy = 0;
    CHECK(pn_fits(pn_cell, 1, 0, 0, 0), "horizontal I fits at (0,0)");
    pn_ok_action();
    CHECK(pn_used[1] == 1, "I placed");
    CHECK(pn_cell[0] == 2 && pn_cell[4] == 2, "I cells (0..4,0) filled");
    CHECK(!pn_fits(pn_cell, 1, 1, 0, 0), "vertical I overlaps placed I");
    CHECK(pn_cur == 2, "auto-advance to next piece");
    CHECK(pn_px[1] == 0 && pn_py[1] == 0, "I origin recorded");
    /* 重叠再放: 落点被占 -> 走移除分支 */
    pn_ok_action();
    CHECK(pn_used[1] == 0, "OK on occupied piece removes it");
    CHECK(pn_cur == 1, "removed piece becomes current");
    CHECK(pn_cell[0] == 0, "cells cleared after removal");
    /* 越界不放 */
    pn_cx = 5; pn_cy = 9;
    CHECK(!pn_fits(pn_cell, 1, 0, pn_cx, pn_cy), "I does not fit at (5,9)");
    /* DEL 移除 */
    pn_ok_action();               /* 放到 (5,9) 失败 -> 空, 无操作 */
    CHECK(pn_used[1] == 0, "no-op when nothing to remove");

    /* ---- 旋转 ---- */
    pn_cur = 9;                    /* X: 仅 1 种朝向, R 原地循环 */
    key_event_t e = ev_char('r');
    pentomino_on_key(&e);
    CHECK(pn_curori[9] == 0, "R wraps X orientation (1 distinct)");
    pn_cur = 1;                    /* I: 2 朝向 */
    e = ev_char('r');
    pentomino_on_key(&e);
    CHECK(pn_curori[1] == 1, "R rotates I");

    /* ---- 方向键 / 切换块 ---- */
    pn_start();
    e = ev_key(K_UP);
    pentomino_on_key(&e);
    CHECK(pn_cur == 11, "UP switches to previous piece (Z)");
    e = ev_key(K_DOWN);
    pentomino_on_key(&e);
    CHECK(pn_cur == 0, "DOWN switches to next piece");
    e = ev_key(K_RIGHT);
    pentomino_on_key(&e);
    CHECK(pn_cx == 3, "RIGHT moves cursor");
    e = ev_key(K_LEFT);
    pentomino_on_key(&e);
    CHECK(pn_cx == 2, "LEFT moves cursor");
    e = ev_char('s');
    pentomino_on_key(&e);
    CHECK(pn_cy == 5, "WASD-S moves cursor down");
    e = ev_char('w');
    pentomino_on_key(&e);
    CHECK(pn_cy == 4, "WASD-W moves cursor up");
    /* 确认键忽略 repeat */
    pn_cx = 0; pn_cy = 0; pn_cur = 1;
    e = ev_key(K_OK); e.is_repeat = true;
    pentomino_on_key(&e);
    CHECK(pn_used[1] == 0, "repeated OK ignored");

    /* ---- 解到底 -> WIN ---- */
    pn_start();
    pn_solve_action();
    CHECK(pn_over, "solve fills board -> win");
    CHECK(pn_placed_count() == 12, "all 12 pieces placed after solve");
    /* 从胜局棋盘移除 F: 光标放在一块真正属于 F 的格上 */
    pn_over = false;
    {
        int fx = -1, fy = -1;
        for (int c = 0; c < 60 && fx < 0; c++)
            if (pn_cell[c] == 1) { fx = c % 6; fy = c / 6; }
        pn_cx = fx; pn_cy = fy;
        pn_remove_at_cursor();
        CHECK(pn_used[0] == 0, "remove works on solved board");
        CHECK(pn_cell[fy * 6 + fx] == 0, "piece cell cleared");
        CHECK(pn_placed_count() == 11, "other pieces intact after remove");
    }
    /* N 新局 */
    e = ev_char('n');
    pentomino_on_key(&e);
    CHECK(!pn_over && pn_placed_count() == 0, "N restarts fresh");
    /* 胜后按键: OK 重开 */
    pn_over = true;
    e = ev_key(K_OK);
    pentomino_on_key(&e);
    CHECK(!pn_over && pn_placed_count() == 0, "OK after win restarts");

    /* ---- 渲染冒烟: HUD 分隔线与棋盘外框有墨 ---- */
    fb_clear(false);
    pentomino_render();
    {
        int ink = 0;
        for (int i = 0; i < (int)CCG_FRAME_BYTES; i++) if (g_fb[i]) ink++;
        CHECK(ink > 200, "render produces visible frame");
        /* 侧栏字母表第 4 列(Z, 屏内 x<=265)有墨 */
        int zink = 0;
        for (int y = 114; y <= 145 && zink == 0; y++)
            for (int x = 223; x <= 265 && zink == 0; x++) {
                int off = (y >> 3) * 296 + x;
                if (g_fb[off] & (0x80u >> (y & 7))) zink = 1;
            }
        CHECK(zink == 1, "letter grid last column renders on-screen");
    }

    if (s_fail == 0) printf("\nALL TESTS PASSED\n");
    else printf("\n%d TEST(S) FAILED\n", s_fail);
    return s_fail;
}
