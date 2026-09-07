/* MAHJONG MATCH — host 逻辑测试
 * include 游戏 .c 直接访问 mj_* 静态; 链接 canvas/font/font_data/pattern/
 * rng/time/ui_common/input/display (-DCHICHU_HOST) */
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#include "../src/gfx/canvas.h"
#include "../src/gfx/font.h"
#include "../src/rng.h"
#include "../src/games/mahjongmatch.c"

bool s_exit_request = false;
void game_set_tick_interval(uint32_t ms) { (void)ms; }

static int s_fail = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); s_fail++; } \
    else { printf("ok: %s\n", msg); } \
} while (0)

/* 1. 洗牌: 每种图案恰好 2 张, 共 36 张, 并复位选择 */
static void test_shuffle(void) {
    rng_seed(&mj_rng, 42);
    bool ok = true;
    for (int iter = 0; iter < 8; iter++) {
        mj_shuffle();
        int cnt[19];
        memset(cnt, 0, sizeof cnt);
        for (int i = 0; i < 36; i++) {
            if (mj_board[i] < 1 || mj_board[i] > 18) ok = false;
            cnt[mj_board[i]]++;
        }
        for (int t = 1; t <= 18; t++)
            if (cnt[t] != 2) ok = false;
    }
    CHECK(ok, "shuffle x8: 36 tiles, each of 18 types exactly twice");
    CHECK(mj_sel == -1 && mj_cx == 0 && mj_cy == 0, "shuffle resets sel/cursor");
}

/* 2. 可消规则: 边缘/孤立/单侧开放 free, 被夹与空格不 free */
static void test_free(void) {
    memset(mj_board, 0, sizeof mj_board);
    mj_board[0] = 1;
    CHECK(mj_free(0), "left edge tile is free");
    mj_board[5] = 2;
    CHECK(mj_free(5), "right edge tile is free");
    mj_board[2] = 3;
    CHECK(mj_free(2), "isolated middle tile is free");
    mj_board[4] = 5;
    CHECK(mj_free(4), "one open side (right) is enough");
    mj_board[1] = 4;
    CHECK(!mj_free(1), "sandwiched tile is locked");
    CHECK(!mj_free(10), "empty cell not free");
    mj_board[30] = 6;
    CHECK(mj_free(30), "bottom-left edge tile free");
}

/* 3. OK 流程: 选/消/取消/消错/被夹/空格 */
static void test_ok(void) {
    memset(mj_board, 0, sizeof mj_board);
    mj_board[0] = 1;   /* (0,0) 与 (1,1) 一对 */
    mj_board[7] = 1;
    mj_board[1] = 2;   /* (1,0); 与 (2,0) 同图案 */
    mj_board[2] = 2;
    mj_board[30] = 6;  /* (0,5) 独立 */
    mj_moves = 0;
    mj_pairs_left = 3;
    mj_over = false;
    mj_sel = -1;
    mj_cx = 0; mj_cy = 0;

    mj_ok();
    CHECK(mj_sel == 0 && mj_status == MJ_MSG_SEL, "first OK selects tile");
    mj_cx = 1; mj_cy = 1;
    mj_ok();
    CHECK(mj_board[0] == 0 && mj_board[7] == 0, "matching pair removed");
    CHECK(mj_moves == 1 && mj_pairs_left == 2, "moves/pairs counted");
    CHECK(mj_sel == -1 && mj_status == MJ_MSG_MATCH, "match hint, sel cleared");

    mj_cx = 1; mj_cy = 0;
    mj_ok();
    CHECK(mj_sel == 1, "OK on new tile selects");
    mj_ok();
    CHECK(mj_sel == -1, "OK on same tile deselects");
    CHECK(mj_board[1] == 2 && mj_moves == 1, "deselect removes nothing");

    mj_cx = 0; mj_cy = 5;
    mj_ok();
    CHECK(mj_sel == 30, "select (0,5)");
    mj_cx = 2; mj_cy = 0;
    mj_ok();
    CHECK(mj_board[30] == 6 && mj_board[2] == 2, "mismatch removes nothing");
    CHECK(mj_sel == -1 && mj_status == MJ_MSG_WRONG, "mismatch hint, sel cleared");

    /* 同图案但 (1,0) 被夹 → LOCK: 先放回左挡牌 */
    mj_board[0] = 7;
    mj_cx = 1; mj_cy = 0;
    mj_ok();
    CHECK(mj_sel == 1, "select sandwiched tile");
    mj_cx = 2; mj_cy = 0;
    mj_ok();
    CHECK(mj_board[1] == 2 && mj_board[2] == 2 && mj_moves == 1,
          "locked pair not removed");
    CHECK(mj_sel == -1 && mj_status == MJ_MSG_LOCK, "lock hint, sel cleared");

    mj_cx = 3; mj_cy = 3;
    mj_ok();
    CHECK(mj_status == MJ_MSG_EMPTY && mj_sel == -1, "empty cell hint");
}

/* 4. 死局检测 */
static void test_deadlock(void) {
    memset(mj_board, 0, sizeof mj_board);
    /* A A B B 单行: 每对的第二张必被夹 → 无任何可消对 */
    mj_board[0] = 1;
    mj_board[1] = 1;
    mj_board[2] = 2;
    mj_board[3] = 2;
    CHECK(!mj_has_move(), "AABB single row is deadlocked");
    /* A B B 行: A 单独, B 对中 idx1 被夹 → 同样死局 */
    memset(mj_board, 0, sizeof mj_board);
    mj_board[0] = 1;
    mj_board[1] = 2;
    mj_board[2] = 2;
    CHECK(!mj_has_move(), "ABB single row is deadlocked");
    mj_board[0] = 0;
    CHECK(mj_has_move(), "freeing the row end creates a move");

    memset(mj_board, 0, sizeof mj_board);
    mj_board[0] = 3;   /* (0,0) */
    mj_board[7] = 3;   /* (1,1) */
    CHECK(mj_has_move(), "diagonal pair is a move");
    mj_board[3] = 4;   /* (3,0) */
    mj_board[4] = 4;   /* (4,0) */
    CHECK(mj_has_move(), "edge-side pair is a move");
}

/* 可构造全清盘: 每行两端一对/次内一对/中间一对, 行优先贪心必全清 */
static void place_solvable(void) {
    memset(mj_board, 0, sizeof mj_board);
    for (int r = 0; r < 6; r++) {
        int base = r * 3;
        mj_board[r * 6 + 0] = (uint8_t)(base + 1);
        mj_board[r * 6 + 5] = (uint8_t)(base + 1);
        mj_board[r * 6 + 1] = (uint8_t)(base + 2);
        mj_board[r * 6 + 4] = (uint8_t)(base + 2);
        mj_board[r * 6 + 2] = (uint8_t)(base + 3);
        mj_board[r * 6 + 3] = (uint8_t)(base + 3);
    }
}

/* 5. 胜利路径: 通过真实 mj_ok 输入流程消除全部 18 对 */
static void test_win(void) {
    place_solvable();
    mj_moves = 0;
    mj_pairs_left = 18;
    mj_over = false;
    mj_sel = -1;
    mj_cx = 0; mj_cy = 0;
    int guard = 0;
    while (!mj_over && guard < 100) {
        int a = -1, b = -1;
        for (int i = 0; i < 36 && a < 0; i++) {
            if (mj_board[i] == 0 || !mj_free(i)) continue;
            for (int j = i + 1; j < 36; j++) {
                if (mj_board[j] == mj_board[i] && mj_free(j)) { a = i; b = j; break; }
            }
        }
        if (a < 0) break;
        mj_cx = a % 6; mj_cy = a / 6;
        mj_ok();
        mj_cx = b % 6; mj_cy = b / 6;
        mj_ok();
        guard++;
    }
    CHECK(mj_over, "win flagged when all cleared");
    CHECK(mj_pairs_left == 0, "pairs left is 0 at win");
    CHECK(mj_moves == 18, "moves == 18 pairs at win");
    int left = 0;
    for (int i = 0; i < 36; i++)
        if (mj_board[i] != 0) left++;
    CHECK(left == 0, "board fully empty at win");
}

/* 6. 随机对局属性测试: 要么全清, 要么真死局; 计数/状态一致 */
static void test_sim(void) {
    bool consistent = true;
    int wins = 0, stuck = 0;
    for (int seed = 1; seed <= 30; seed++) {
        rng_seed(&mj_rng, (uint64_t)seed * 0x9E3779B97F4A7C15ull);
        mj_new_game();
        int guard = 0;
        while (mj_pairs_left > 0 && guard < 64) {
            int a = -1, b = -1;
            for (int i = 0; i < 36 && a < 0; i++) {
                if (mj_board[i] == 0 || !mj_free(i)) continue;
                for (int j = i + 1; j < 36; j++) {
                    if (mj_board[j] == mj_board[i] && mj_free(j)) { a = i; b = j; break; }
                }
            }
            if (a < 0) break;
            mj_cx = a % 6; mj_cy = a / 6;
            mj_ok();
            mj_cx = b % 6; mj_cy = b / 6;
            mj_ok();
            guard++;
        }
        int left = 0;
        for (int i = 0; i < 36; i++)
            if (mj_board[i] != 0) left++;
        if (left != mj_pairs_left * 2) consistent = false;
        if (mj_moves + mj_pairs_left != 18) consistent = false;
        if (mj_pairs_left == 0) {
            if (!mj_over) consistent = false;
            wins++;
        } else {
            if (mj_has_move()) consistent = false;
            if (mj_status != MJ_MSG_DEAD) consistent = false;
            stuck++;
        }
    }
    CHECK(consistent, "sim: all 30 seeds internally consistent");
    CHECK(wins > 0, "at least one seed clears fully");
    CHECK(stuck > 0, "at least one seed hits deadlock");
    printf("info: sim %d wins / %d deadlocks / 30 seeds\n", wins, stuck);
}

int main(void) {
    test_shuffle();
    test_free();
    test_ok();
    test_deadlock();
    test_win();
    test_sim();
    if (s_fail == 0) {
        printf("ALL PASS\n");
        return 0;
    }
    printf("%d FAILURES\n", s_fail);
    return 1;
}
