/* FREECELL host 逻辑单测 — include freecell.c 直测静态状态 */
#include "../src/config.h"
#include "../src/gfx/canvas.h"
#include "../src/gfx/pattern.h"
#include "../src/rng.h"
#include "../src/games/freecell.c"
#include <stdio.h>
#include <string.h>

/* ---- host 框架 stub ---- */
bool s_exit_request = false;
void game_set_tick_interval(uint32_t ms) { (void)ms; }

static int s_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); s_fail++; } \
    else { printf("ok: %s\n", msg); } \
} while (0)

static void fc_blank(void) {
    for (int i = 0; i < FC_NS; i++) { fc_cell[i] = -1; fc_found[i] = -1; }
    for (int c = 0; c < FC_NCOL; c++) fc_cnt[c] = 0;
    fc_sel_kind = -1;
    fc_moves = 0;
    fc_over = false;
    fc_stuck = false;
}

static void fc_put_col(int c, const uint8_t *cards, int n) {
    fc_cnt[c] = 0;
    for (int i = 0; i < n; i++) fc_col[c][i] = cards[i];
    fc_cnt[c] = (uint8_t)n;
}

static void key(ccg_key k, uint8_t ch, bool rep) {
    key_event_t e;
    e.key = k; e.ch = ch; e.is_repeat = rep;
    freecell_on_key(&e);
}

static void test_deal(void) {
    rng_seed(&fc_rng, 12345);
    freecell_reset();
    int exp[8] = { 7, 6, 7, 6, 7, 6, 7, 6 };
    int sum = 0;
    for (int c = 0; c < 8; c++) {
        CHECK(fc_cnt[c] == exp[c], "deal column counts 7/6/7/6/7/6/7/6");
        sum += fc_cnt[c];
    }
    CHECK(sum == 52, "deal total 52");
    int seen[52] = { 0 };
    for (int c = 0; c < 8; c++)
        for (int i = 0; i < fc_cnt[c]; i++) seen[fc_col[c][i]]++;
    int bad = 0;
    for (int i = 0; i < 52; i++) if (seen[i] != 1) bad++;
    CHECK(bad == 0, "deal has every card exactly once");
    int all = 1;
    for (int i = 0; i < FC_NS; i++) if (fc_cell[i] != -1 || fc_found[i] != -1) all = 0;
    CHECK(all, "cells and foundations start empty");
    CHECK(fc_moves == 0 && !fc_over, "moves 0, not over");
}

static void test_seed(void) {
    rng_seed(&fc_rng, 1); freecell_reset();
    uint8_t a[8][52]; uint8_t an[8];
    memcpy(a, fc_col, sizeof(a)); memcpy(an, fc_cnt, sizeof(fc_cnt));
    rng_seed(&fc_rng, 2); freecell_reset();
    int diff = 0;
    for (int c = 0; c < 8 && !diff; c++)
        for (int i = 0; i < fc_cnt[c] && !diff; i++)
            if (fc_col[c][i] != a[c][i]) diff = 1;
    CHECK(diff, "different seeds give different deals");
    rng_seed(&fc_rng, 1); freecell_reset();
    int same = 1;
    for (int c = 0; c < 8 && same; c++) {
        if (fc_cnt[c] != an[c]) { same = 0; break; }
        for (int i = 0; i < fc_cnt[c] && same; i++)
            if (fc_col[c][i] != a[c][i]) same = 0;
    }
    CHECK(same, "same seed gives identical deal");
}

static void test_stack_layout(void) {
    int bad = 0;
    for (int n = 0; n <= 52; n++) {
        fc_blank();
        fc_cnt[0] = (uint8_t)n;
        int ov = fc_overlap(n);
        int stack_h = FC_CH + (n > 0 ? (n - 1) * ov : 0);
        int y0 = FC_TB_Y0 + (FC_TB_SPAN - stack_h) / 2;
        if (n > 0 && (ov < 1 || ov > 8)) bad++;
        if (y0 < FC_TB_Y0 || y0 + stack_h - 1 > FC_TB_BOT) bad++;
        int ty = fc_col_top_y(0);
        if (ty != y0 + (n > 0 ? (n - 1) * ov : 0)) bad++;
    }
    CHECK(bad == 0, "stack layout in bounds for 0..52 cards");
}

static void test_move_rules(void) {
    /* A: 目标堆 */
    fc_blank();
    fc_cell[0] = 0;                                   /* AH */
    CHECK(fc_try_move(0, 0, 0, 4), "Ace to empty foundation");
    CHECK(fc_found[0] == 0 && fc_cell[0] == -1 && fc_moves == 1, "foundation gets ace");
    fc_blank();
    fc_found[0] = 0;
    fc_put_col(0, (uint8_t[]){ 1 }, 1);               /* 2H */
    CHECK(fc_try_move(1, 0, 0, 4), "2H onto AH");
    CHECK(fc_found[0] == 1 && fc_cnt[0] == 0, "2H stacked");
    fc_blank();
    fc_found[0] = 0;
    fc_put_col(0, (uint8_t[]){ 2 }, 1);               /* 3H */
    CHECK(!fc_try_move(1, 0, 0, 4), "3H onto AH rejected (rank skip)");
    fc_blank();
    fc_found[0] = 0;
    fc_put_col(0, (uint8_t[]){ 28 }, 1);              /* 3S */
    CHECK(!fc_try_move(1, 0, 0, 4), "wrong suit rejected");
    fc_blank();
    fc_put_col(0, (uint8_t[]){ 1 }, 1);               /* 2H to empty foundation */
    CHECK(!fc_try_move(1, 0, 0, 4), "non-ace to empty foundation rejected");
    fc_blank();
    fc_found[0] = 12;                                 /* KH */
    fc_put_col(0, (uint8_t[]){ 0 }, 1);               /* AH */
    CHECK(!fc_try_move(1, 0, 0, 4), "ace onto king rejected");
    /* B: 列间红黑交替递减 (5S=30, 6S=31, 6H=5, 7H=6, 5H=4) */
    fc_blank();
    fc_put_col(0, (uint8_t[]){ 30 }, 1);              /* 5S 黑 */
    fc_put_col(1, (uint8_t[]){ 5 }, 1);               /* 6H 红 */
    CHECK(fc_try_move(1, 0, 1, 1), "5S onto 6H (alt color, rank-1)");
    CHECK(fc_cnt[0] == 0 && fc_cnt[1] == 2 && fc_col[1][1] == 30, "column move executed");
    fc_blank();
    fc_put_col(0, (uint8_t[]){ 30 }, 1);
    fc_put_col(1, (uint8_t[]){ 31 }, 1);              /* 6S 黑 */
    CHECK(!fc_try_move(1, 0, 1, 1), "same color rejected");
    fc_blank();
    fc_put_col(0, (uint8_t[]){ 30 }, 1);
    fc_put_col(1, (uint8_t[]){ 6 }, 1);               /* 7H */
    CHECK(!fc_try_move(1, 0, 1, 1), "rank gap rejected");
    fc_blank();
    fc_put_col(0, (uint8_t[]){ 30 }, 1);
    fc_put_col(1, (uint8_t[]){ 4 }, 1);               /* 5H */
    CHECK(!fc_try_move(1, 0, 1, 1), "equal rank rejected");
    fc_blank();
    fc_put_col(0, (uint8_t[]){ 30 }, 1);              /* 空列任意 */
    CHECK(fc_try_move(1, 0, 1, 1), "any card to empty column");
    fc_blank();
    fc_cell[0] = 6;                                   /* 7H */
    CHECK(fc_try_move(0, 0, 1, 1), "cell card to empty column");
    /* C: 空当 */
    fc_blank();
    fc_cell[0] = 0;
    fc_cell[1] = 14;
    CHECK(!fc_try_move(0, 0, 0, 1), "occupied cell rejected");
    fc_blank();
    fc_cell[0] = 0;
    CHECK(fc_try_move(0, 0, 0, 1), "any card to empty cell");
    /* D: 目标堆出牌 */
    fc_blank();
    fc_found[0] = 12;                                 /* KH 回列 */
    CHECK(fc_try_move(0, 4, 1, 1), "foundation card back to column");
    CHECK(fc_found[0] == -1 && fc_col[1][0] == 12, "foundation cleared");
    fc_blank();
    fc_found[0] = 0;                                  /* AH 回空当 */
    CHECK(fc_try_move(0, 4, 0, 1), "foundation ace to cell");
    fc_blank();
    fc_found[0] = 0; fc_found[1] = 1;
    CHECK(!fc_try_move(0, 4, 0, 5), "AH onto 2H foundation rejected");
    /* E: 自身/空源 */
    fc_blank();
    fc_cell[0] = 0;
    CHECK(!fc_try_move(0, 0, 0, 0), "self move rejected");
    fc_blank();
    CHECK(!fc_try_move(1, 3, 0, 0), "empty column source rejected");
}

static void test_win(void) {
    fc_blank();
    fc_found[0] = 12; fc_found[1] = 25; fc_found[2] = 38; fc_found[3] = 51;
    fc_check_end();
    CHECK(fc_over && !fc_stuck, "four kings -> WIN");
    fc_blank();
    fc_found[0] = 11; fc_found[1] = 25; fc_found[2] = 38; fc_found[3] = 51;
    fc_check_end();
    CHECK(!fc_over, "three kings + queen -> not win");
}

static void test_stuck(void) {
    fc_blank();
    for (int c = 0; c < 8; c++) fc_put_col(c, (uint8_t[]){ 40 }, 1);  /* 8x 2C */
    for (int i = 0; i < FC_NS; i++) fc_cell[i] = 41;                  /* 4x 3C 满 */
    fc_check_end();
    CHECK(fc_over && fc_stuck, "deadlock detected (cells full, 8x 2C tops)");
    fc_blank();
    for (int c = 0; c < 8; c++) fc_put_col(c, (uint8_t[]){ 40 }, 1);
    for (int i = 0; i < FC_NS; i++) fc_cell[i] = 41;
    fc_cell[0] = -1;
    fc_check_end();
    CHECK(!fc_over, "not stuck while a cell is free");
    rng_seed(&fc_rng, 99);
    freecell_reset();
    fc_check_end();
    CHECK(!fc_over, "fresh deal is never stuck");
}

static void test_cursor(void) {
    freecell_reset();
    CHECK(fc_cur_kind == 0 && fc_cur_col == 0, "cursor starts top-left");
    key(K_DOWN, 0, false);
    CHECK(fc_cur_kind == 1 && fc_cur_col == 0, "down -> tableau");
    key(K_RIGHT, 0, false); key(K_RIGHT, 0, false);
    CHECK(fc_cur_col == 2, "right x2");
    key(K_UP, 0, false);
    CHECK(fc_cur_kind == 0 && fc_cur_col == 2, "up -> top row same col");
    key(K_UP, 0, false);
    CHECK(fc_cur_kind == 0, "up at top stays");
    key(K_DOWN, 0, false); key(K_DOWN, 0, false); key(K_DOWN, 0, false);
    CHECK(fc_cur_kind == 1, "down at tableau stays");
    for (int i = 0; i < 10; i++) key(K_RIGHT, 0, false);
    CHECK(fc_cur_col == 7, "right clamps at 7");
    key(K_CHAR, 'w', false);
    CHECK(fc_cur_kind == 0 && fc_cur_col == 7, "w -> top");
    key(K_CHAR, 'a', false);
    CHECK(fc_cur_col == 6, "a -> left");
    key(K_CHAR, 'd', false);
    CHECK(fc_cur_col == 7, "d -> right");
    key(K_CHAR, 's', false);
    CHECK(fc_cur_kind == 1 && fc_cur_col == 7, "s -> tableau");
    key(K_UP, 0, true);
    CHECK(fc_cur_kind == 0, "repeat direction still moves");
    key(K_OK, 0, true);
    CHECK(fc_sel_kind == -1, "repeat OK ignored");
    uint32_t mv = fc_moves;
    key(K_CHAR, 'n', false);
    int sum = 0;
    for (int c = 0; c < 8; c++) sum += fc_cnt[c];
    CHECK(sum == 52 && fc_moves == 0 && fc_cur_col == 0 && fc_cur_kind == 0,
          "'n' starts new deal");
    (void)mv;
}

static void test_ok_flow(void) {
    fc_blank();
    fc_cell[0] = 0;                       /* AH in cell 0 */
    fc_cur_kind = 0; fc_cur_col = 0;
    key(K_OK, 0, false);
    CHECK(fc_sel_kind == 0 && fc_sel_idx == 0, "OK picks card");
    key(K_OK, 0, false);
    CHECK(fc_sel_kind == -1, "OK again cancels in place");
    key(K_OK, 0, false);
    CHECK(fc_sel_kind == 0, "re-picked");
    key(K_RIGHT, 0, false);               /* cell 1 空 */
    key(K_OK, 0, false);
    CHECK(fc_cell[0] == -1 && fc_cell[1] == 0 && fc_sel_kind == -1 && fc_moves == 1,
          "cell to cell move + move count");
    key(K_OK, 0, false);                  /* 拾起 AH */
    key(K_RIGHT, 0, false);               /* cell 2 */
    key(K_RIGHT, 0, false);               /* cell 3 */
    key(K_RIGHT, 0, false);               /* 到目标堆 0 (顶部行列 4..7) */
    key(K_OK, 0, false);
    CHECK(fc_found[0] == 0 && fc_cell[1] == -1 && fc_moves == 2, "ace to foundation");
    key(K_OK, 0, false);                  /* 再拾起 (found 0) */
    key(K_LEFT, 0, false);                /* cell 3 */
    key(K_LEFT, 0, false);                /* cell 2 */
    key(K_LEFT, 0, false);                /* cell 1 空 */
    key(K_OK, 0, false);
    CHECK(fc_cell[1] == 0 && fc_found[0] == -1 && fc_moves == 3, "foundation card back out");
    /* 非法落点: 放下不计数 */
    fc_blank();
    fc_put_col(0, (uint8_t[]){ 30 }, 1);  /* 5S */
    fc_cell[0] = 5;                       /* 6H 占住空当 0 */
    fc_cur_kind = 1; fc_cur_col = 0;
    key(K_OK, 0, false);                  /* 拾 5S */
    key(K_UP, 0, false);                  /* 光标到 cell 0(有牌) */
    key(K_OK, 0, false);
    CHECK(fc_sel_kind == -1 && fc_moves == 0 && fc_cnt[0] == 1,
          "invalid drop deselects, no move counted");
    fc_blank();
    fc_put_col(0, (uint8_t[]){ 30 }, 1);
    fc_cur_kind = 1; fc_cur_col = 0;
    key(K_OK, 0, false);
    key(K_UP, 0, false);                  /* cell 0 空 */
    key(K_OK, 0, false);
    CHECK(fc_cell[0] == 30 && fc_cnt[0] == 0 && fc_moves == 1, "col top to empty cell");
    /* 空源 OK 无动作 */
    fc_blank();
    fc_cur_kind = 0; fc_cur_col = 3;
    key(K_OK, 0, false);
    CHECK(fc_sel_kind == -1, "OK on empty slot ignored");
}

static void test_over_keys(void) {
    fc_blank();
    fc_found[0] = 12; fc_found[1] = 25; fc_found[2] = 38; fc_found[3] = 51;
    fc_check_end();
    CHECK(fc_over, "over set before restart");
    key(K_OK, 0, false);
    int sum = 0;
    for (int c = 0; c < 8; c++) sum += fc_cnt[c];
    CHECK(!fc_over && sum == 52 && fc_moves == 0, "OK at game over restarts");
    fc_blank();
    fc_over = true;
    s_exit_request = false;
    key(K_QUIT, 0, false);
    CHECK(s_exit_request, "QUIT at game over exits");
    s_exit_request = false;
}

static void test_render_smoke(void) {
    rng_seed(&fc_rng, 777);
    freecell_reset();
    /* 选中一张牌 + 光标在右下角, 覆盖光标/反白路径 */
    fc_sel_kind = 1; fc_sel_idx = 5;
    fc_cur_kind = 1; fc_cur_col = 7;
    freecell_render();
    int nonz = 0;
    for (size_t i = 0; i < CCG_FRAME_BYTES; i++) if (g_fb[i]) nonz++;
    CHECK(nonz > 0, "render produces frame");
    fc_over = true;
    fc_stuck = true;
    freecell_render();
    CHECK(nonz > 0, "render at stuck state");
    fc_over = false;
    fc_stuck = false;
    /* 空列槽 + 顶部空槽渲染 */
    fc_blank();
    freecell_render();
    CHECK(nonz > 0, "render all-empty board");
}

int main(void) {
    test_deal();
    test_seed();
    test_stack_layout();
    test_move_rules();
    test_win();
    test_stuck();
    test_cursor();
    test_ok_flow();
    test_over_keys();
    test_render_smoke();
    if (s_fail == 0) printf("\nALL PASS\n");
    else printf("\n%d FAILURES\n", s_fail);
    return s_fail ? 1 : 0;
}
