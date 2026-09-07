/* FIFTEEN 逻辑单测 — 包含游戏源文件, 链接框架 host 实现
 * 覆盖: 滑动语义/边界、胜负判定、键路径状态转换、洗牌可解性、重复键 */
#include "../src/config.h"
#include "../src/games/fifteen.c"
#include <stdio.h>
#include <string.h>

static int s_fail = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); s_fail++; } \
    else { fprintf(stderr, "ok: %s\n", msg); } \
} while (0)

/* 框架全局 stub(与 tests/test_logic.c 一致) */
bool s_exit_request = false;
void game_set_tick_interval(uint32_t ms) { (void)ms; }

static void fp_set_solved(void) {
    for (int i = 0; i < 16; i++) fp_board[i] = (uint8_t)((i + 1) & 15);
    fp_moves = 0;
    fp_over = false;
    fp_over_full = false;
}

/* 可解性判定: 逆序数奇偶 == 空格行(自底, 0 起)奇偶 */
static int fp_parity_solvable(const uint8_t b[16]) {
    int inv = 0, blank_row = 0;
    for (int i = 0; i < 16; i++) {
        if (b[i] == 0) { blank_row = i / 4; continue; }
        for (int j = i + 1; j < 16; j++)
            if (b[j] != 0 && b[j] < b[i]) inv++;
    }
    return (inv & 1) == ((3 - blank_row) & 1);
}

static int fp_perm_ok(const uint8_t b[16]) {
    int seen[16] = {0};
    for (int i = 0; i < 16; i++) {
        if (b[i] > 15) return 0;
        seen[b[i]]++;
    }
    for (int i = 0; i < 16; i++)
        if (seen[i] != 1) return 0;
    return 1;
}

/* ---- 滑动语义与边界 ----
 * 方向键 = 数字滑入空格的方向(朝空格方向滑动):
 * 按 UP 空格正下方数字上滑入空格; 空格在边界则该方向无效。 */
static void test_slide_mechanics(void) {
    fp_set_solved();   /* 空格在 (3,3) = 索引 15, 底行最右列 */
    CHECK(fp_slide_dir(0) == 0, "UP at bottom row: no move");
    CHECK(fp_slide_dir(2) == 0, "LEFT at rightmost col: no move");
    CHECK(fp_moves == 0, "no moves counted for boundary presses");

    /* RIGHT: 空格左侧(索引 14, 值 15)右滑入空格 */
    CHECK(fp_slide_dir(3) == 1, "RIGHT slides tile left of gap into it");
    CHECK(fp_board[14] == 0 && fp_board[15] == 15, "after RIGHT: gap at idx14, 15 at idx15");

    /* LEFT 还原 */
    CHECK(fp_slide_dir(2) == 1, "LEFT restores");
    CHECK(fp_solved(), "board solved again after undo");

    /* DOWN: 空格正上方(索引 11, 值 12)下滑入空格 */
    CHECK(fp_slide_dir(1) == 1, "DOWN slides tile above gap down");
    CHECK(fp_board[11] == 0 && fp_board[15] == 12, "after DOWN: gap at idx11, 12 at idx15");
    CHECK(fp_slide_dir(0) == 1, "UP restores");
    CHECK(fp_solved(), "board solved again after UP undo");

    /* 空格移到左上角: 直接构造 0 在 (0,0) */
    fp_board[0] = 0;
    for (int i = 1; i < 16; i++) fp_board[i] = (uint8_t)i;
    CHECK(fp_slide_dir(1) == 0, "DOWN at top row: no move");
    CHECK(fp_slide_dir(3) == 0, "RIGHT at leftmost col: no move");
    CHECK(fp_slide_dir(0) == 1, "UP slides tile below gap (val 4) up");
    CHECK(fp_board[0] == 4 && fp_board[4] == 0, "after UP at corner: 4 at idx0, gap at idx4");
    /* 注意: UP 之后空格已到 idx4, 再按 LEFT 滑动的是 idx4 右侧的 5 */
    CHECK(fp_slide_dir(2) == 1, "LEFT slides tile right of gap into it");
    CHECK(fp_board[4] == 5 && fp_board[5] == 0, "after LEFT at corner: 5 at idx4, gap at idx5");
}

/* ---- 胜负判定 ---- */
static void test_win_detect(void) {
    fp_set_solved();
    CHECK(fp_solved(), "solved board detected");
    fp_slide_dir(3);
    CHECK(!fp_solved(), "one slide away from solved is not solved");
    fp_slide_dir(2);
    CHECK(fp_solved(), "back to solved detected");
}

/* ---- 通过 on_key 的完整状态转换 ---- */
static void test_key_flow(void) {
    /* 近完成态: [1..14, 空格, 15], 按 LEFT 一步完成(15 左滑入空格) */
    for (int i = 0; i < 14; i++) fp_board[i] = (uint8_t)(i + 1);
    fp_board[14] = 0;
    fp_board[15] = 15;
    fp_moves = 0;
    fp_over = false;
    fp_over_full = false;

    key_event_t ev;
    ev.ch = 0;
    ev.is_repeat = false;
    ev.key = K_LEFT;
    fifteen_on_key(&ev);
    CHECK(fp_over && fp_solved() && fp_moves == 1, "K_LEFT solves: over+solved, moves=1");

    /* 结束态: 重复 OK 不触发重开 */
    ev.is_repeat = true;
    fifteen_on_key(&ev);
    CHECK(fp_over, "repeat OK in over state ignored");
    ev.is_repeat = false;

    /* 结束态: BACK 请求退出 */
    s_exit_request = false;
    ev.key = K_BACK;
    fifteen_on_key(&ev);
    CHECK(s_exit_request, "BACK in over state exits");

    /* 结束态: N 重开新局 */
    s_exit_request = false;
    ev.key = K_CHAR;
    ev.ch = 'n';
    fifteen_on_key(&ev);
    CHECK(!fp_over && fp_moves == 0 && fp_perm_ok(fp_board),
          "N in over state starts new game (moves reset)");
    CHECK(fp_parity_solvable(fp_board), "new game is solvable (parity invariant)");

    /* 游戏中: 字母 N 重复键被忽略 */
    fp_set_solved();
    ev.key = K_CHAR;
    ev.ch = 'n';
    ev.is_repeat = true;
    fifteen_on_key(&ev);
    CHECK(fp_solved() && fp_moves == 0, "repeat 'n' ignored in game");
    ev.is_repeat = false;

    /* 游戏中: WASD 与方向键等效 */
    fp_set_solved();
    ev.key = K_CHAR;
    ev.ch = 'd';   /* 同 RIGHT: 空格(3,3)左侧 15 右滑入空格 */
    fifteen_on_key(&ev);
    CHECK(fp_board[14] == 0 && fp_board[15] == 15 && fp_moves == 1,
          "WASD 'd' slides like K_RIGHT");

    /* 游戏中: 方向键重复可响应(长按连滑) */
    fp_set_solved();
    ev.key = K_RIGHT;
    ev.is_repeat = true;
    fifteen_on_key(&ev);
    CHECK(fp_moves == 1 && !fp_solved(), "repeat direction slides (repeatable)");
    ev.is_repeat = false;

    /* 游戏中: N 重开 */
    fp_set_solved();
    fp_slide_dir(3);
    ev.key = K_CHAR;
    ev.ch = 'n';
    fifteen_on_key(&ev);
    CHECK(!fp_solved() && fp_moves == 0 && fp_perm_ok(fp_board), "N restarts mid-game");

    /* 游戏中: Q 退出 */
    s_exit_request = false;
    ev.key = K_QUIT;
    fifteen_on_key(&ev);
    CHECK(s_exit_request, "Q quits to menu");
    s_exit_request = false;
}

/* ---- 洗牌: 多次新局保持置换合法性 + 可解性 ---- */
static void test_shuffle_legality(void) {
    rng_seed(&fp_rng, 20260829u);
    for (int k = 0; k < 30; k++) {
        fifteen_enter();   /* 内部 now_ms() 播种, 仍验证性质而非具体盘面 */
        CHECK(fp_perm_ok(fp_board), "shuffle result is a valid permutation");
        CHECK(fp_parity_solvable(fp_board), "shuffle result is solvable");
        CHECK(!fp_solved(), "shuffle never yields solved board");
        CHECK(fp_moves == 0 && !fp_over, "new game resets moves/over");
    }
}

/* ---- 渲染冒烟: 棋盘/数字/完成态不越界不崩溃 ---- */
static void test_render_smoke(void) {
    fp_set_solved();
    fifteen_render();
    int black = 0;
    for (unsigned i = 0; i < CCG_FRAME_BYTES; i++)
        for (int b = 0; b < 8; b++)
            if (g_fb[i] & (1u << b)) black++;
    CHECK(black > 200, "render draws board grid + 15 numbers (black px > 200)");

    fp_over = true;
    fifteen_render();
    black = 0;
    for (unsigned i = 0; i < CCG_FRAME_BYTES; i++)
        for (int b = 0; b < 8; b++)
            if (g_fb[i] & (1u << b)) black++;
    CHECK(black > 50, "solved-state render draws HUD text (black px > 50)");
    fp_over = false;
}

int main(void) {
    test_slide_mechanics();
    test_win_detect();
    test_key_flow();
    test_shuffle_legality();
    test_render_smoke();
    if (s_fail == 0) printf("ALL PASS\n");
    else printf("%d FAILURE(S)\n", s_fail);
    return s_fail ? 1 : 0;
}
