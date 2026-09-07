/* MASTERMIND 逻辑单测 — host 编译, include 游戏源文件直接操作静态状态
 * 覆盖: 开局状态 / 谜底生成 / 反馈算法(限量/排序) / 键路径状态转换
 *       胜负判定 / 边界循环 / 重复键策略 / 结束态退出 */
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include "../../src/config.h"
#include "../../src/games/mastermind.c"

bool s_exit_request = false;

static int s_fail = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); s_fail++; } \
    else { printf("ok: %s\n", msg); } \
} while (0)

static void press_key(ccg_key k) {
    key_event_t ev = { k, 0, false };
    mastermind_on_key(&ev);
}

static void press_key_rep(ccg_key k) {
    key_event_t ev = { k, 0, true };
    mastermind_on_key(&ev);
}

static void press_char(char c) {
    key_event_t ev = { K_CHAR, (uint8_t)c, false };
    mastermind_on_key(&ev);
}

static void press_char_rep(char c) {
    key_event_t ev = { K_CHAR, (uint8_t)c, true };
    mastermind_on_key(&ev);
}

static void set_secret(int a, int b, int c, int d) {
    mm_secret[0] = a; mm_secret[1] = b; mm_secret[2] = c; mm_secret[3] = d;
}

/* ---- 开局状态 + 谜底生成 + rng 安全 ---- */
static void test_init_and_secret(void) {
    mastermind_enter();
    CHECK(mm_rows == 0 && !mm_over && !mm_won, "fresh game state");
    CHECK(mm_sel == 0, "cursor starts at cell 0");
    CHECK(mm_cur[0] == 0 && mm_cur[1] == 0 && mm_cur[2] == 0 && mm_cur[3] == 0,
          "current row all palette index 0");
    bool in_range = true;
    int i, it;
    for (it = 0; it < 500; it++) {
        mm_gen_secret();
        for (i = 0; i < MM_LEN; i++)
            if (mm_secret[i] < 0 || mm_secret[i] >= MM_PALETTE) in_range = false;
    }
    CHECK(in_range, "500 secrets all in [0,6)");
    /* rng 播种 0 不会恒 0(死循环防护), rng_range 不挂 */
    rng_seed(&mm_rng, 0);
    CHECK(mm_rng.s != 0, "rng_seed(0) yields nonzero state");
    for (it = 0; it < 1000; it++) {
        uint32_t v = rng_range(&mm_rng, MM_PALETTE);
        if (v >= MM_PALETTE) { in_range = false; break; }
    }
    CHECK(in_range, "rng_range(6) 1000x terminates, values < 6");
}

/* ---- 反馈算法: 位置对/符号对/限量/排序 ---- */
static void score_case(int *sec, int *cur, int e0, int e1, int e2, int e3,
                       const char *msg) {
    int i;
    for (i = 0; i < MM_LEN; i++) { mm_secret[i] = sec[i]; mm_cur[i] = cur[i]; }
    mm_rows = 0;
    mm_score_row();
    CHECK(mm_fb[0][0] == (uint8_t)e0 && mm_fb[0][1] == (uint8_t)e1 &&
          mm_fb[0][2] == (uint8_t)e2 && mm_fb[0][3] == (uint8_t)e3, msg);
}

static void test_scoring(void) {
    int sec[4], cur[4];
    sec[0] = 0; sec[1] = 1; sec[2] = 2; sec[3] = 3;
    cur[0] = 0; cur[1] = 1; cur[2] = 2; cur[3] = 3;
    score_case(sec, cur, 1, 1, 1, 1, "exact match -> 4 solid");
    cur[0] = 3; cur[1] = 2; cur[2] = 1; cur[3] = 0;
    score_case(sec, cur, 2, 2, 2, 2, "all colors, no position -> 4 hollow");
    cur[0] = 0; cur[1] = 0; cur[2] = 0; cur[3] = 0;
    score_case(sec, cur, 1, 0, 0, 0, "single match -> 1 solid");
    cur[0] = 1; cur[1] = 0; cur[2] = 2; cur[3] = 3;
    score_case(sec, cur, 1, 1, 2, 2, "solids before hollows order");
    /* 重复猜测受谜底名额限量 */
    sec[0] = 0; sec[1] = 0; sec[2] = 1; sec[3] = 1;
    cur[0] = 0; cur[1] = 0; cur[2] = 0; cur[3] = 0;
    score_case(sec, cur, 1, 1, 0, 0, "duplicate guess limited by secret");
    /* 全错 */
    sec[0] = 0; sec[1] = 1; sec[2] = 2; sec[3] = 3;
    cur[0] = 5; cur[1] = 5; cur[2] = 5; cur[3] = 5;
    score_case(sec, cur, 0, 0, 0, 0, "all wrong -> no feedback");
}

/* ---- 键路径: 选位/循环/提交/获胜 ---- */
static void test_flow_win(void) {
    mastermind_enter();
    set_secret(0, 1, 2, 3);
    CHECK(!mm_over && mm_rows == 0, "fresh game state");
    /* 用方向键逐格构造正确答案 */
    int i;
    for (i = 0; i < MM_LEN; i++) {
        while (mm_sel != i) press_key(K_RIGHT);
        while (mm_cur[i] != mm_secret[i]) press_key(K_UP);
    }
    CHECK(mm_sel == 3, "cursor moved to cell 3");
    CHECK(mm_cur[0] == 0 && mm_cur[1] == 1 && mm_cur[2] == 2 && mm_cur[3] == 3,
          "current row matches secret via keys");
    mastermind_render();               /* 中途渲染不崩 */
    press_key(K_OK);
    CHECK(mm_rows == 1, "submit advances row");
    CHECK(mm_over && mm_won, "correct guess wins");
    CHECK(mm_history[0][0] == 0 && mm_history[0][3] == 3, "guess stored");
    CHECK(mm_fb[0][0] == 1 && mm_fb[0][1] == 1 && mm_fb[0][2] == 1 &&
          mm_fb[0][3] == 1, "win row all solid feedback");
    mastermind_render();
    CHECK(mm_over_full, "over triggers force full refresh flag");
    /* 结束后 N 重开 */
    press_char('n');
    CHECK(!mm_over && mm_rows == 0 && !mm_won, "N restarts after win");
}

/* ---- 十个错误猜测判负 + 最后一手猜中也判胜 ---- */
static void test_lose_and_last_win(void) {
    mastermind_enter();
    set_secret(0, 1, 2, 3);
    int i, g;
    for (g = 0; g < 9; g++) {
        for (i = 0; i < MM_LEN; i++) mm_cur[i] = 5;   /* 全错 */
        press_key(K_OK);
        CHECK(mm_rows == g + 1, "wrong guess count advances");
        CHECK(!mm_over, "still playing after wrong guess");
    }
    for (i = 0; i < MM_LEN; i++) mm_cur[i] = 0;
    press_key(K_OK);                                   /* 第 10 手仍错 */
    CHECK(mm_rows == 10 && mm_over && !mm_won, "10th wrong guess loses");
    mastermind_render();                               /* 失败态渲染不崩 */
    /* OK 重开 */
    press_key(K_OK);
    CHECK(!mm_over && mm_rows == 0, "OK restarts after loss");
    /* 第 10 手猜中: 胜 */
    set_secret(0, 1, 2, 3);
    for (g = 0; g < 9; g++) {
        for (i = 0; i < MM_LEN; i++) mm_cur[i] = 5;
        press_key(K_OK);
    }
    for (i = 0; i < MM_LEN; i++) mm_cur[i] = (int)mm_secret[i];
    press_key(K_OK);
    CHECK(mm_over && mm_won && mm_rows == 10, "correct guess on last try wins");
}

/* ---- 边界循环 + 重复键策略 ---- */
static void test_bounds_and_repeat(void) {
    mastermind_enter();
    /* 循环上界 */
    mm_cur[0] = MM_PALETTE - 1;
    press_key(K_UP);
    CHECK(mm_cur[0] == 0, "UP wraps from top to 0");
    press_key(K_DOWN);
    CHECK(mm_cur[0] == MM_PALETTE - 1, "DOWN wraps from 0 to top");
    /* 光标循环 */
    mm_sel = 0;
    press_key(K_LEFT);
    CHECK(mm_sel == MM_LEN - 1, "LEFT wraps from 0 to last cell");
    press_key(K_RIGHT);
    CHECK(mm_sel == 0, "RIGHT wraps from last cell to 0");
    /* 方向键重复可响应 */
    mm_cur[0] = 0;
    press_key_rep(K_UP);
    CHECK(mm_cur[0] == 1, "repeat UP still cycles (long-press)");
    press_key_rep(K_DOWN);
    CHECK(mm_cur[0] == 0, "repeat DOWN still cycles");
    /* W/S 字母重复忽略 */
    press_char_rep('w');
    CHECK(mm_cur[0] == 0, "repeat letter w ignored");
    /* 确认键重复忽略 */
    mm_rows = 0;
    press_key_rep(K_OK);
    CHECK(mm_rows == 0, "repeat OK does not submit");
    /* 字母 WASD 单发生效 */
    press_char('w');
    CHECK(mm_cur[0] == 1, "letter w cycles up");
    press_char('s');
    CHECK(mm_cur[0] == 0, "letter s cycles down");
    press_char('a');
    CHECK(mm_sel == MM_LEN - 1, "letter a moves cursor left");
    press_char('d');
    CHECK(mm_sel == 0, "letter d moves cursor right");
    /* 其它字母忽略 */
    mm_rows = 0;
    press_char('x');
    CHECK(mm_rows == 0 && mm_cur[0] == 0, "irrelevant letter ignored");
}

/* ---- 结束态退出键 / 提示重置 ---- */
static void test_over_keys(void) {
    mastermind_enter();
    mm_over = true;
    mm_won = false;
    s_exit_request = false;
    press_key(K_QUIT);
    CHECK(s_exit_request, "QUIT exits when over");
    s_exit_request = false;
    press_key(K_BACK);
    CHECK(s_exit_request, "BACK exits when over");
    s_exit_request = false;
    press_key(K_OK);
    CHECK(!mm_over, "OK retries when over");
    mm_over = true;
    press_char('n');
    CHECK(!mm_over, "N retries when over");
    /* 非结束态 Q 退出 */
    s_exit_request = false;
    press_key(K_QUIT);
    CHECK(s_exit_request, "QUIT exits mid-game");
}

int main(void) {
    test_init_and_secret();
    test_scoring();
    test_flow_win();
    test_lose_and_last_win();
    test_bounds_and_repeat();
    test_over_keys();
    if (s_fail) { printf("\n%d FAILURE(S)\n", s_fail); return 1; }
    printf("\nALL MASTERMIND TESTS PASSED\n");
    return 0;
}
