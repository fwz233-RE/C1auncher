/* 逻辑单测 — host cc 编译运行; 零平台依赖
 * 链接 canvas/font/font_data/pattern/rng/time/ui_common/input/display */
#include "../src/config.h"
#include "../src/gfx/canvas.h"
#include "../src/gfx/pattern.h"
#include "../src/rng.h"
#include "../src/games/bullscows.c"
#include <stdio.h>
#include <string.h>

static int s_fail = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); s_fail++; } \
    else { printf("ok: %s\n", msg); } \
} while (0)

/* ---- host 框架 stub(main.c 提供, 此处补上) ---- */
bool s_exit_request = false;
void game_set_tick_interval(uint32_t ms) { (void)ms; }

static key_event_t ev(int key, char ch, bool rep) {
    key_event_t e;
    e.key = (ccg_key)key;
    e.ch = (uint8_t)ch;
    e.is_repeat = rep;
    return e;
}

/* ---- test: 谜底生成 ---- */
static void test_secret(void) {
    int i;
    rng_seed(&bc_rng, 12345);
    bc_gen_secret();
    for (i = 0; i < BC_LEN; i++) {
        int j;
        CHECK(bc_secret[i] >= 0 && bc_secret[i] <= 9, "secret digit in 0..9");
        for (j = i + 1; j < BC_LEN; j++)
            CHECK(bc_secret[i] != bc_secret[j], "secret digits unique");
    }
    rng_seed(&bc_rng, 999);
    bc_gen_secret();
    for (i = 0; i < BC_LEN; i++) {
        int j;
        CHECK(bc_secret[i] >= 0 && bc_secret[i] <= 9, "secret2 digit in 0..9");
        for (j = i + 1; j < BC_LEN; j++)
            CHECK(bc_secret[i] != bc_secret[j], "secret2 digits unique");
    }
}

/* ---- test: 反馈计分 ---- */
static void test_score(void) {
    int bulls, cows;
    int sec[4] = { 1, 2, 3, 4 };
    int cur[4];

    cur[0] = 1; cur[1] = 2; cur[2] = 3; cur[3] = 4;
    bc_score(cur, sec, &bulls, &cows);
    CHECK(bulls == 4 && cows == 0, "exact match -> 4B0C");

    cur[0] = 4; cur[1] = 3; cur[2] = 2; cur[3] = 1;
    bc_score(cur, sec, &bulls, &cows);
    CHECK(bulls == 0 && cows == 4, "all wrong pos -> 0B4C");

    cur[0] = 5; cur[1] = 6; cur[2] = 7; cur[3] = 8;
    bc_score(cur, sec, &bulls, &cows);
    CHECK(bulls == 0 && cows == 0, "no overlap -> 0B0C");

    cur[0] = 1; cur[1] = 1; cur[2] = 1; cur[3] = 1;
    bc_score(cur, sec, &bulls, &cows);
    CHECK(bulls == 1 && cows == 0, "repeated guess digit -> 1B0C");

    cur[0] = 2; cur[1] = 0; cur[2] = 0; cur[3] = 0;
    bc_score(cur, sec, &bulls, &cows);
    CHECK(bulls == 0 && cows == 1, "single misplaced digit -> 0B1C");

    sec[0] = 5; sec[1] = 0; sec[2] = 7; sec[3] = 3;
    cur[0] = 0; cur[1] = 5; cur[2] = 7; cur[3] = 3;
    bc_score(cur, sec, &bulls, &cows);
    CHECK(bulls == 2 && cows == 2, "swap pair -> 2B2C");

    sec[0] = 0; sec[1] = 1; sec[2] = 2; sec[3] = 3;
    cur[0] = 3; cur[1] = 2; cur[2] = 1; cur[3] = 0;
    bc_score(cur, sec, &bulls, &cows);
    CHECK(bulls == 0 && cows == 4, "reverse -> 0B4C");

    sec[0] = 1; sec[1] = 2; sec[2] = 3; sec[3] = 4;
    cur[0] = 1; cur[1] = 3; cur[2] = 2; cur[3] = 4;
    bc_score(cur, sec, &bulls, &cows);
    CHECK(bulls == 2 && cows == 2, "partial match -> 2B2C");
}

/* ---- test: 提交胜利 ---- */
static void test_submit_win(void) {
    bc_secret[0] = 3; bc_secret[1] = 1; bc_secret[2] = 4; bc_secret[3] = 2;
    bc_cur[0] = 3; bc_cur[1] = 1; bc_cur[2] = 4; bc_cur[3] = 2;
    bc_rows = 0; bc_over = false; bc_won = false;
    bc_submit();
    CHECK(bc_rows == 1, "submit increments rows");
    CHECK(bc_over && bc_won, "4 bulls -> win");
    CHECK(bc_bulls[0] == 4 && bc_cows[0] == 0, "win row feedback 4B0C");
    CHECK(bc_hist[0][0] == 3 && bc_hist[0][3] == 2, "history stores guess");
}

/* ---- test: 10 次用完判负 ---- */
static void test_submit_fail(void) {
    int i;
    bc_secret[0] = 9; bc_secret[1] = 8; bc_secret[2] = 7; bc_secret[3] = 6;
    bc_rows = 0; bc_over = false; bc_won = false;
    for (i = 0; i < BC_ATTEMPTS; i++) {
        bc_cur[0] = 0; bc_cur[1] = 1; bc_cur[2] = 2; bc_cur[3] = 3;
        bc_submit();
        if (i < BC_ATTEMPTS - 1)
            CHECK(!bc_over, "not over before last try");
    }
    CHECK(bc_rows == BC_ATTEMPTS, "10 rows after 10 submits");
    CHECK(bc_over && !bc_won, "10 tries no win -> fail");
}

/* ---- test: 按键输入 ---- */
static void test_keys(void) {
    key_event_t e;

    bc_rows = 0; bc_over = false; bc_won = false; bc_sel = 0;
    bc_cur[0] = bc_cur[1] = bc_cur[2] = bc_cur[3] = 0;

    e = ev(K_RIGHT, 0, false);
    bullscows_on_key(&e);
    CHECK(bc_sel == 1, "RIGHT moves cursor");
    e = ev(K_LEFT, 0, false);
    bullscows_on_key(&e);
    CHECK(bc_sel == 0, "LEFT moves cursor");
    e = ev(K_LEFT, 0, false);
    bullscows_on_key(&e);
    CHECK(bc_sel == 3, "LEFT wraps to last");
    e = ev(K_UP, 0, false);
    bullscows_on_key(&e);
    CHECK(bc_cur[3] == 1, "UP cycles digit up");
    e = ev(K_UP, 0, true);   /* 方向重复可响应 */
    bullscows_on_key(&e);
    CHECK(bc_cur[3] == 2, "UP repeat still cycles");
    e = ev(K_DOWN, 0, false);
    bullscows_on_key(&e);
    CHECK(bc_cur[3] == 1, "DOWN cycles digit down");
    {   /* 再按 10 次 DOWN: 1 -> 1 (0..9 循环) */
        int k;
        for (k = 0; k < 10; k++) {
            e = ev(K_DOWN, 0, false);
            bullscows_on_key(&e);
        }
    }
    CHECK(bc_cur[3] == 1, "DOWN wraps 0<->9");

    e = ev(K_CHAR, 'a', false);
    bullscows_on_key(&e);
    CHECK(bc_sel == 2, "A moves left");
    e = ev(K_CHAR, 'w', false);
    bullscows_on_key(&e);
    CHECK(bc_cur[2] == 1, "W cycles up");
    e = ev(K_CHAR, 's', false);
    bullscows_on_key(&e);
    CHECK(bc_cur[2] == 0, "S cycles down");
    e = ev(K_CHAR, 'd', false);
    bullscows_on_key(&e);
    CHECK(bc_sel == 3, "D moves right");

    /* OK 提交重复忽略 */
    bc_secret[0] = 5; bc_secret[1] = 6; bc_secret[2] = 7; bc_secret[3] = 8;
    bc_rows = 0; bc_over = false;
    e = ev(K_OK, 0, true);
    bullscows_on_key(&e);
    CHECK(bc_rows == 0, "OK repeat ignored during play");

    /* 命中: OK 提交胜利 */
    bc_cur[0] = 5; bc_cur[1] = 6; bc_cur[2] = 7; bc_cur[3] = 8;
    e = ev(K_OK, 0, false);
    bullscows_on_key(&e);
    CHECK(bc_rows == 1 && bc_over && bc_won, "OK submits and wins");

    /* 结束态: OK 开新局 */
    e = ev(K_OK, 0, false);
    bullscows_on_key(&e);
    CHECK(!bc_over && bc_rows == 0, "OK at game over starts new game");

    /* 结束态: N 重复忽略, N 开新局 */
    bc_over = true;
    e = ev(K_CHAR, 'n', true);
    bullscows_on_key(&e);
    CHECK(bc_over, "N repeat ignored at game over");
    e = ev(K_CHAR, 'n', false);
    bullscows_on_key(&e);
    CHECK(!bc_over && bc_rows == 0, "N at game over starts new game");

    /* 游戏中 N 重复忽略 */
    e = ev(K_CHAR, 'n', true);
    bullscows_on_key(&e);
    CHECK(bc_rows == 0, "N repeat ignored during play");

    /* 未认出的字母不动作 */
    e = ev(K_CHAR, 'x', false);
    bullscows_on_key(&e);
    CHECK(bc_rows == 0 && bc_sel == 0, "unknown char ignored");
}

/* ---- test: 真实对局流程 ---- */
static void test_flow(void) {
    bc_secret[0] = 4; bc_secret[1] = 2; bc_secret[2] = 0; bc_secret[3] = 9;
    bc_rows = 0; bc_over = false; bc_won = false;

    bc_cur[0] = 0; bc_cur[1] = 1; bc_cur[2] = 2; bc_cur[3] = 3;
    bc_submit();
    CHECK(bc_bulls[0] == 0 && bc_cows[0] == 2, "guess1 -> 0B2C");
    CHECK(!bc_over, "guess1 not over");

    bc_cur[0] = 4; bc_cur[1] = 2; bc_cur[2] = 0; bc_cur[3] = 9;
    bc_submit();
    CHECK(bc_over && bc_won && bc_rows == 2, "guess2 wins");
    CHECK(bc_bulls[1] == 4 && bc_cows[1] == 0, "guess2 feedback 4B0C");
}

/* ---- test: enter 重置状态 ---- */
static void test_enter(void) {
    int i;
    bc_rows = 7; bc_over = true; bc_won = true;
    bc_sel = 2;
    bc_cur[0] = 5;
    bc_hist[0][0] = 9;
    bullscows_enter();
    CHECK(bc_rows == 0 && !bc_over && !bc_won, "enter resets state");
    CHECK(bc_sel == 0, "enter resets cursor");
    CHECK(bc_cur[0] == 0, "enter resets digits");
    CHECK(bc_over_full == false, "enter resets over-full flag");
    for (i = 0; i < BC_LEN; i++) {
        int j;
        CHECK(bc_secret[i] >= 0 && bc_secret[i] <= 9, "enter secret in 0..9");
        for (j = i + 1; j < BC_LEN; j++)
            CHECK(bc_secret[i] != bc_secret[j], "enter secret unique");
    }
}

int main(void) {
    test_secret();
    test_score();
    test_submit_win();
    test_submit_fail();
    test_keys();
    test_flow();
    test_enter();
    if (s_fail) {
        printf("FAILED: %d check(s)\n", s_fail);
        return 1;
    }
    printf("ALL TESTS PASSED\n");
    return 0;
}
