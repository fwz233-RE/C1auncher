/* PEG SOLITAIRE 逻辑单测 — host, 零平台依赖(驱动代码在 -DCHICHU_HOST 下空转) */
#include "../../src/config.h"
#include "../../src/gfx/canvas.h"
#include "../../src/games/peg.c"
#include <stdio.h>

static int s_fail = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); s_fail++; } \
    else { printf("ok: %s\n", msg); } \
} while (0)

/* ---- host 框架 stub ---- */
bool s_exit_request = false;
void game_set_tick_interval(uint32_t ms) { (void)ms; }

/* 像素断言辅助 */
static bool px(int x, int y) {
    if (x < 0 || x >= (int)CCG_W || y < 0 || y >= (int)CCG_H) return false;
    int off = (y >> 3) * (int)CCG_W + x;
    return (g_fb[off] & (0x80 >> (y & 7))) != 0;
}

/* 棋盘下标统一 pg_board[y][x]; 以下助手以 (x,y) 坐标操作 */
static int count_pegs(void) {
    int n = 0;
    for (int y = 0; y < PG_N; y++)
        for (int x = 0; x < PG_N; x++)
            if (pg_board[y][x]) n++;
    return n;
}

static int count_holes(void) {
    int n = 0;
    for (int y = 0; y < PG_N; y++)
        for (int x = 0; x < PG_N; x++)
            if (pg_is_hole(x, y)) n++;
    return n;
}

static void set_board(void) {
    for (int y = 0; y < PG_N; y++)
        for (int x = 0; x < PG_N; x++) pg_board[y][x] = 0;
}

static void put_peg(int x, int y) { pg_board[y][x] = 1; }

/* ---- 开局: 33 孔 32 子, 中心空 ---- */
static void test_new(void) {
    pg_new();
    CHECK(count_holes() == 33, "English board has 33 holes");
    CHECK(count_pegs() == 32 && pg_remain == 32, "opening has 32 pegs");
    CHECK(pg_board[3][3] == 0, "center hole empty");
    CHECK(pg_board[0][0] == 0 && pg_board[0][6] == 0 &&
          pg_board[6][0] == 0 && pg_board[6][6] == 0, "four corners empty");
    CHECK(pg_board[1][2] == 1 && pg_board[2][0] == 1, "cross arms filled");
    CHECK(pg_cx == 3 && pg_cy == 3 && !pg_sel, "cursor at center, no selection");
    CHECK(!pg_over && !pg_stuck && !pg_perfect, "not over at start");
}

/* ---- 跳吃合法性: 正交 2 格/中继/目标空/越界防护 ---- */
static void test_validity(void) {
    pg_new();
    CHECK(pg_valid_jump(3, 1, 3, 3), "vertical jump over (3,2) valid");
    pg_board[3][3] = 1; pg_board[1][3] = 0;   /* 中心放子, (3,1) 腾空 */
    CHECK(pg_valid_jump(3, 3, 3, 1), "reverse direction valid");
    pg_new();
    CHECK(pg_valid_jump(1, 3, 3, 3), "horizontal jump valid");
    CHECK(!pg_valid_jump(2, 1, 3, 2), "diagonal rejected");
    CHECK(!pg_valid_jump(3, 2, 3, 3), "distance 1 rejected");
    CHECK(!pg_valid_jump(3, 0, 3, 3), "distance 3 rejected");
    CHECK(!pg_valid_jump(3, 3, 3, 3), "zero distance rejected");
    pg_board[1][3] = 0;                       /* (3,1) 源无子 */
    CHECK(!pg_valid_jump(3, 1, 3, 3), "empty source rejected");
    pg_board[1][3] = 1;
    pg_board[2][3] = 0;                       /* (3,2) 中被跳子缺失 */
    CHECK(!pg_valid_jump(3, 1, 3, 3), "missing jumped peg rejected");
    pg_board[2][3] = 1;
    pg_board[3][3] = 1;                       /* (3,3) 目标有子 */
    CHECK(!pg_valid_jump(3, 1, 3, 3), "occupied target rejected");
    pg_board[3][3] = 0;
    CHECK(!pg_valid_jump(2, 0, 0, 0), "non-hole target rejected");
    /* 越界目标必须拒绝且不越界读(越界/死循环防护) */
    CHECK(!pg_valid_jump(0, 3, -2, 3), "out-of-bounds target (left) rejected");
    CHECK(!pg_valid_jump(2, 3, 8, 3), "out-of-bounds target (right) rejected");
    CHECK(!pg_valid_jump(3, 3, 3, 7), "out-of-bounds target (bottom) rejected");
    CHECK(pg_has_move(), "fresh board has moves");
}

/* ---- 完整一跳: 选中->移动->跳吃, 计数/棋盘/光标 ---- */
static void test_move(void) {
    pg_new();
    pg_cx = 3; pg_cy = 1;
    key_event_t ev = { K_OK, 0, false };
    peg_on_key(&ev);
    CHECK(pg_sel && pg_selx == 3 && pg_sely == 1, "OK selects peg at cursor");
    pg_cx = 3; pg_cy = 3;
    ev.key = K_OK;
    peg_on_key(&ev);
    CHECK(!pg_sel, "selection cleared after jump");
    CHECK(pg_remain == 31 && count_pegs() == 31, "31 pegs after one jump");
    CHECK(pg_board[1][3] == 0 && pg_board[2][3] == 0 && pg_board[3][3] == 1,
          "source+middle removed, target has peg");
    CHECK(pg_cx == 3 && pg_cy == 3, "cursor follows landing cell");
    CHECK(!pg_over, "still playing after first jump");
}

/* ---- BACK 取消选中: 局面不变 ---- */
static void test_cancel(void) {
    pg_new();
    pg_cx = 3; pg_cy = 1;
    key_event_t ev = { K_OK, 0, false };
    peg_on_key(&ev);
    CHECK(pg_sel, "selected before cancel");
    ev.key = K_BACK;
    peg_on_key(&ev);
    CHECK(!pg_sel, "BACK cancels selection");
    CHECK(pg_remain == 32 && count_pegs() == 32, "board unchanged by cancel");
}

/* ---- OK 在另一颗子上 = 换选; 换选后可正常跳 ---- */
static void test_reselect(void) {
    pg_new();
    pg_cx = 3; pg_cy = 1;
    key_event_t ev = { K_OK, 0, false };
    peg_on_key(&ev);
    CHECK(pg_sel && pg_selx == 3 && pg_sely == 1, "OK selects (3,1)");
    pg_cx = 3; pg_cy = 2;          /* 光标移到邻格棋子 */
    ev.key = K_OK;
    peg_on_key(&ev);
    CHECK(pg_selx == 3 && pg_sely == 2 && count_pegs() == 32,
          "OK on adjacent peg reselects, no move");
    /* 换选后跳吃: (3,2) 选中, (2,2) 中继, (1,2) 空 */
    set_board();
    put_peg(3, 2); put_peg(2, 2);
    pg_remain = 2;
    pg_cx = 3; pg_cy = 2; pg_sel = true; pg_selx = 3; pg_sely = 2;
    pg_over = false; pg_stuck = false; pg_perfect = false;
    pg_cx = 1; pg_cy = 2;
    ev.key = K_OK;
    peg_on_key(&ev);
    CHECK(pg_remain == 1 && pg_board[2][1] == 1 &&
          pg_board[2][2] == 0 && pg_board[2][3] == 0,
          "jump from selected peg (3,2)->(1,2) over (2,2)");
}

/* ---- 非法目标: 无操作, 选择保留; 有子目标=换选 ---- */
static void test_invalid_target(void) {
    pg_new();
    pg_cx = 3; pg_cy = 1;
    key_event_t ev = { K_OK, 0, false };
    peg_on_key(&ev);
    CHECK(pg_sel, "selected (3,1)");
    pg_cx = 1; pg_cy = 1;          /* 非孔位, 空 */
    ev.key = K_OK;
    peg_on_key(&ev);
    CHECK(pg_sel && pg_remain == 32 && count_pegs() == 32,
          "OK on non-hole keeps selection, no move");
    pg_cx = 2; pg_cy = 0;          /* 有子但斜角不可达 */
    ev.key = K_OK;
    peg_on_key(&ev);
    CHECK(pg_selx == 2 && pg_sely == 0 && count_pegs() == 32,
          "diagonal peg reselects, no move");
    pg_cx = 4; pg_cy = 0;          /* (2,0) 的 2 距目标 (4,0) 有子 */
    ev.key = K_OK;
    peg_on_key(&ev);
    CHECK(pg_selx == 4 && pg_sely == 0 && count_pegs() == 32,
          "occupied 2-away target reselects instead of jumping");
}

/* ---- 胜利: 剩 1 子, 中心 = PERFECT, 非中心 = SOLVED ---- */
static void test_win(void) {
    set_board();
    put_peg(3, 1); put_peg(3, 2);          /* 跳 (3,1)->(3,3) 吃 (3,2) */
    pg_remain = 2;
    pg_cx = 3; pg_cy = 1; pg_sel = true; pg_selx = 3; pg_sely = 1;
    pg_over = false; pg_stuck = false; pg_perfect = false; pg_over_full = false;
    key_event_t ev = { K_OK, 0, false };
    pg_cx = 3; pg_cy = 3;
    peg_on_key(&ev);
    CHECK(pg_remain == 1 && count_pegs() == 1, "single peg left");
    CHECK(pg_over && !pg_stuck && pg_perfect, "win at center -> PERFECT");
    CHECK(pg_board[3][3] == 1 && !pg_sel, "winning peg at center, selection cleared");

    set_board();
    put_peg(2, 3); put_peg(3, 3);          /* 跳 (2,3)->(4,3) 吃 (3,3) */
    pg_remain = 2;
    pg_cx = 2; pg_cy = 3; pg_sel = true; pg_selx = 2; pg_sely = 3;
    pg_over = false; pg_stuck = false; pg_perfect = false; pg_over_full = false;
    pg_cx = 4; pg_cy = 3;
    peg_on_key(&ev);
    CHECK(pg_over && !pg_perfect && pg_remain == 1 && pg_board[3][4] == 1,
          "win off-center -> SOLVED (not PERFECT)");
}

/* ---- 死局: 跳后剩 2 子且无合法跳 -> STUCK ---- */
static void test_stuck(void) {
    set_board();
    put_peg(3, 0); put_peg(3, 1); put_peg(3, 4);
    pg_remain = 3;
    pg_cx = 3; pg_cy = 0; pg_sel = true; pg_selx = 3; pg_sely = 0;
    pg_over = false; pg_stuck = false; pg_perfect = false; pg_over_full = false;
    key_event_t ev = { K_OK, 0, false };
    pg_cx = 3; pg_cy = 2;          /* (3,0) 跳 (3,2) 吃 (3,1), 余 (3,2)(3,4) */
    peg_on_key(&ev);
    CHECK(pg_remain == 2 && count_pegs() == 2, "2 pegs after jump");
    CHECK(pg_over && pg_stuck && !pg_perfect, "no moves -> STUCK");
    CHECK(!pg_has_move(), "stuck board has no moves");
    /* 双子隔空(子-空-子)无法互跳 */
    set_board();
    put_peg(3, 1); put_peg(3, 3);
    CHECK(!pg_has_move(), "peg-empty-peg pair: no moves");
}

/* ---- 按键: 重复忽略/方向重复/WASD/边界钳制/N 重开/终局键 ---- */
static void test_keys(void) {
    pg_new();
    pg_cx = 3; pg_cy = 3;
    key_event_t ev = { K_OK, 0, true };
    peg_on_key(&ev);
    CHECK(!pg_sel, "repeated OK ignored");
    ev.key = K_RIGHT; ev.is_repeat = true;
    peg_on_key(&ev);
    CHECK(pg_cx == 4, "repeated RIGHT still moves cursor");
    ev.is_repeat = true; ev.key = K_BACK;
    peg_on_key(&ev);
    CHECK(!s_exit_request, "repeated BACK ignored");

    ev.is_repeat = false;
    pg_cx = 3; pg_cy = 3;
    ev.key = K_CHAR; ev.ch = 'w'; peg_on_key(&ev);
    CHECK(pg_cy == 2, "W moves up");
    ev.key = K_CHAR; ev.ch = 's'; peg_on_key(&ev);
    CHECK(pg_cy == 3, "S moves down");
    ev.key = K_CHAR; ev.ch = 'a'; peg_on_key(&ev);
    CHECK(pg_cx == 2, "A moves left");
    ev.key = K_CHAR; ev.ch = 'd'; peg_on_key(&ev);
    CHECK(pg_cx == 3, "D moves right");
    pg_cx = 0; pg_cy = 0;
    ev.key = K_LEFT; peg_on_key(&ev);
    ev.key = K_UP; peg_on_key(&ev);
    CHECK(pg_cx == 0 && pg_cy == 0, "cursor clamped at top-left");
    pg_cx = 6; pg_cy = 6;
    ev.key = K_RIGHT; peg_on_key(&ev);
    ev.key = K_DOWN; peg_on_key(&ev);
    CHECK(pg_cx == 6 && pg_cy == 6, "cursor clamped at bottom-right");

    /* N 重开 */
    pg_board[0][3] = 0; pg_remain = 31;
    ev.key = K_CHAR; ev.ch = 'n'; peg_on_key(&ev);
    CHECK(pg_remain == 32 && count_pegs() == 32 && pg_board[0][3] == 1,
          "N restarts new game");

    /* 终局键: BACK 退出, OK/N 重开 */
    pg_over = true; pg_stuck = true; s_exit_request = false;
    ev.key = K_BACK; ev.ch = 0; peg_on_key(&ev);
    CHECK(s_exit_request, "BACK at game over quits");
    s_exit_request = false;
    pg_over = true; pg_stuck = true;
    ev.key = K_OK; peg_on_key(&ev);
    CHECK(pg_remain == 32 && !pg_over && !pg_stuck, "OK at game over restarts");
    pg_over = true; pg_stuck = true; pg_over_full = false;
    ev.key = K_CHAR; ev.ch = 'n'; peg_on_key(&ev);
    CHECK(!pg_over && pg_remain == 32, "N at game over restarts");
}

/* ---- 渲染: 布局常量/棋子圆/选中反白/光标/终局 HUD ---- */
static void test_render(void) {
    CHECK(PG_BOARD == 133, "board 133px");
    CHECK(PG_OX == 81 && PG_OY == 17, "board centered (x=81,y=17)");
    CHECK(PG_OY + PG_BOARD <= (int)CCG_H, "board fits vertically (150<=152)");
    pg_new();
    peg_render();
    CHECK(px(2, 2), "HUD title 'PEG' renders");
    CHECK(px(0, CCG_HUD_H - 1), "HUD separator line");
    /* 空中心孔: 小点(半径 3); 有子孔: 大圆(半径 7) */
    int cx = PG_OX + 3 * PG_CELL + PG_CELL / 2;      /* 147 */
    int cyc = PG_OY + 3 * PG_CELL + PG_CELL / 2;     /* 83 */
    CHECK(px(cx, cyc), "center hole dot black");
    CHECK(!px(cx, cyc + 4), "dot radius <= 3 (no peg)");
    int cy_p = PG_OY + 2 * PG_CELL + PG_CELL / 2;    /* 64: peg at (3,2) */
    CHECK(px(cx, cy_p) && px(cx, cy_p + 4), "peg circle radius 7 black");
    /* 选中: 整格反白 + 白子 + 白边框 */
    pg_cx = 3; pg_cy = 1;
    key_event_t ev = { K_OK, 0, false };
    peg_on_key(&ev);
    peg_render();
    int sxo = PG_OX + 3 * PG_CELL;                   /* 138 */
    int syo = PG_OY + 1 * PG_CELL;                   /* 36 */
    CHECK(px(sxo + 3, syo + 4), "selected cell interior black");
    CHECK(!px(sxo, syo + 9), "selected cell border white (inverted)");
    CHECK(!px(sxo + 9, syo + 9), "selected peg drawn white");
    /* 光标移开: 选中格保持反白, 光标黑边 */
    pg_cx = 2; pg_cy = 1;
    peg_render();
    CHECK(!px(sxo + 9, syo + 9), "selection persists after cursor moves");
    int cxo = PG_OX + 2 * PG_CELL;                   /* 119 */
    CHECK(px(cxo, syo + 9), "cursor black border on normal cell");
    CHECK(!px(cxo - 2, syo + 9), "no content left of cursor cell");
    /* 终局: HUD 区清空 + 结果文本 + 强制全刷一次 */
    pg_over = true; pg_stuck = true; pg_over_full = false;
    peg_render();
    CHECK(!px(0, 0), "over state clears HUD area");
    int ink = 0;
    for (int y = 2; y < 10; y++)
        for (int x = 2; x < 42; x++)
            if (px(x, y)) ink++;
    CHECK(ink > 0, "result text rendered in HUD");
    CHECK(pg_over_full, "force-full fired once on over");
    peg_render();
    CHECK(pg_over_full, "force-full not repeated");
}

int main(void) {
    test_new();
    test_validity();
    test_move();
    test_cancel();
    test_reselect();
    test_invalid_target();
    test_win();
    test_stuck();
    test_keys();
    test_render();
    if (s_fail == 0) { printf("ALL TESTS PASSED\n"); return 0; }
    printf("%d TEST(S) FAILED\n", s_fail);
    return 1;
}
