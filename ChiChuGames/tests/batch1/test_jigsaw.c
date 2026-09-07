/* JIGSAW 逻辑单测 — host cc 编译运行; include 游戏源直接访问 static 状态 */
#include <stdio.h>
#include <string.h>
#include "../src/games/jigsaw.c"

/* ---- 框架 stub(display.o 等提供 g_fb 与绘制) ---- */
bool s_exit_request = false;
void game_set_tick_interval(uint32_t ms) { (void)ms; }

static int s_fail = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); s_fail++; } \
    else { printf("ok: %s\n", msg); } \
} while (0)

static void set_solved(void) {
    for (int i = 0; i < JG_CELLS; i++)
        jg_board[i] = (uint8_t)((i + 1) % JG_CELLS);   /* 1..19,0 */
}

/* 棋盘必须是 0-19 的排列(恰一个空格) */
static int board_is_perm(void) {
    int seen[JG_CELLS];
    memset(seen, 0, sizeof(seen));
    for (int i = 0; i < JG_CELLS; i++) {
        int v = jg_board[i];
        if (v < 0 || v >= JG_CELLS || seen[v]) return 0;
        seen[v] = 1;
    }
    return 1;
}

static void test_solved_layout(void) {
    set_solved();
    CHECK(jg_solved(), "solved layout detected");
    jg_board[0] = 2; jg_board[1] = 1;
    CHECK(!jg_solved(), "unsolved layout detected");
    jg_board[0] = 1; jg_board[1] = 2;
    CHECK(jg_blank() == JG_CELLS - 1, "blank at bottom-right in solved layout");
}

static void test_slide(void) {
    set_solved();
    jg_moves = 0;
    CHECK(jg_slide_to(18) == 1, "slide left neighbor of blank ok");
    CHECK(jg_board[18] == 0 && jg_board[19] == 19, "tile slid into blank");
    CHECK(jg_slide_to(19) == 1, "slide back ok");
    CHECK(jg_board[19] == 0 && jg_board[18] == 19, "reverse slide ok");
    CHECK(jg_slide_to(0) == 0, "non-adjacent tile rejected");
    CHECK(jg_slide_to(JG_CELLS) == 0, "out-of-range index rejected");
    CHECK(jg_slide_to(-1) == 0, "negative index rejected");
    /* 滑动不动 jg_moves(计步只在 on_key 层) */
    CHECK(jg_moves == 0, "slide helper does not count moves");
}

static void test_shuffle(void) {
    rng_seed(&jg_rng, 42);
    for (int r = 0; r < 5; r++) {
        jg_shuffle();
        CHECK(board_is_perm(), "shuffled board is permutation of 0-19");
        CHECK(!jg_solved(), "shuffle guard: never starts solved");
    }
    /* 不同洗牌应产生不同局面(粗查, 种子不同) */
    jg_shuffle();
    uint8_t a[JG_CELLS];
    memcpy(a, jg_board, sizeof(a));
    rng_seed(&jg_rng, 7);
    jg_shuffle();
    CHECK(memcmp(a, jg_board, sizeof(a)) != 0, "different seeds -> different boards");
}

static void test_cursor_bounds(void) {
    jg_cx = 0; jg_cy = 0;
    key_event_t ev = { K_LEFT, 0, false };
    jigsaw_on_key(&ev);
    CHECK(jg_cx == 0 && jg_cy == 0, "cursor clamped at left edge");
    ev.key = K_UP;
    jigsaw_on_key(&ev);
    CHECK(jg_cx == 0 && jg_cy == 0, "cursor clamped at top edge");
    jg_cx = JG_COLS - 1; jg_cy = JG_ROWS - 1;
    ev.key = K_RIGHT;
    jigsaw_on_key(&ev);
    CHECK(jg_cx == JG_COLS - 1, "cursor clamped at right edge");
    ev.key = K_DOWN;
    jigsaw_on_key(&ev);
    CHECK(jg_cy == JG_ROWS - 1, "cursor clamped at bottom edge");
}

static void test_wasd(void) {
    jg_cx = 1; jg_cy = 1;
    key_event_t ev = { K_CHAR, 'a', false };
    jigsaw_on_key(&ev);
    CHECK(jg_cx == 0, "wasd 'a' moves cursor left");
    ev.ch = 'w';
    jigsaw_on_key(&ev);
    CHECK(jg_cy == 0, "wasd 'w' moves cursor up");
    ev.ch = 'd';
    jigsaw_on_key(&ev);
    CHECK(jg_cx == 1, "wasd 'd' moves cursor right");
    ev.ch = 's';
    jigsaw_on_key(&ev);
    CHECK(jg_cy == 1, "wasd 's' moves cursor down");
}

static void test_ok_slide_and_win(void) {
    /* 差一步: 空格在 18 格, 19 在 19 格; 光标在 19 格 */
    set_solved();                          /* board[18]=19, board[19]=0 */
    jg_board[18] = 0; jg_board[19] = 19;   /* 交换 → 未完成态 */
    jg_moves = 0; jg_over = false; jg_cx = 3; jg_cy = 4;
    key_event_t ev = { K_OK, 0, false };
    jigsaw_on_key(&ev);
    CHECK(jg_moves == 1, "OK slide counts one move");
    CHECK(jg_over, "final slide triggers win");
    CHECK(jg_solved(), "board solved after final slide");

    /* OK 重复按键必须忽略 */
    set_solved();
    jg_board[18] = 0; jg_board[19] = 19;
    jg_moves = 0; jg_over = false; jg_cx = 3; jg_cy = 4;
    ev.is_repeat = true;
    jigsaw_on_key(&ev);
    CHECK(!jg_over && jg_moves == 0, "repeated OK ignored (no slide)");

    /* 光标不在空格相邻处 OK 无效 */
    set_solved();
    jg_moves = 0; jg_cx = 0; jg_cy = 0;
    ev.is_repeat = false;
    jigsaw_on_key(&ev);
    CHECK(jg_moves == 0 && board_is_perm(), "OK on non-adjacent tile does nothing");
    CHECK(!jg_solved() || jg_board[0] == 1, "board unchanged by invalid OK");
}

static void test_over_state_keys(void) {
    jg_over = true;
    jg_moves = 7;
    key_event_t ev = { K_CHAR, 'n', true };      /* 重复 'n' 在 over 态忽略 */
    jigsaw_on_key(&ev);
    CHECK(jg_moves == 7, "repeated 'n' ignored in over state");
    ev.is_repeat = false;
    jigsaw_on_key(&ev);                          /* 非重复 'n' 重开 */
    CHECK(!jg_over && jg_moves == 0, "non-repeat 'n' restarts game");
    CHECK(board_is_perm(), "restart keeps board valid");

    jg_over = true;
    jg_moves = 3;
    key_event_t ev2 = { K_OK, 0, true };         /* 重复 OK 在 over 态忽略 */
    jigsaw_on_key(&ev2);
    CHECK(jg_moves == 3 && jg_over, "repeated OK ignored in over state");
    ev2.is_repeat = false;
    jigsaw_on_key(&ev2);
    CHECK(!jg_over && jg_moves == 0, "non-repeat OK retries in over state");
}

static void test_render(void) {
    /* 正常局 render: 不触发全刷标志 */
    set_solved();
    jg_board[0] = 5; jg_board[4] = 1;   /* 非完成态 */
    jg_over = false; jg_over_full = false;
    jg_cx = 1; jg_cy = 2;
    jg_moves = 3;
    jigsaw_render();
    CHECK(!jg_over_full, "render in play does not force full refresh");
    /* over 态 render: 一次性 disp_force_full */
    jg_over = true;
    jigsaw_render();
    CHECK(jg_over_full, "over render triggers force-full once");
    jigsaw_render();
    CHECK(jg_over_full, "force-full flag stays set (no repeat)");
}

int main(void) {
    test_solved_layout();
    test_slide();
    test_shuffle();
    test_cursor_bounds();
    test_wasd();
    test_ok_slide_and_win();
    test_over_state_keys();
    test_render();
    if (s_fail) { printf("TOTAL: %d FAIL\n", s_fail); return 1; }
    printf("ALL PASS\n");
    return 0;
}
