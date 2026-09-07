/* ISOLA host 逻辑单测 — 直接包含 isola.c, 断言规则/AI/胜负 */
#include "../../src/games/isola.c"
#include <stdio.h>
#include <string.h>

static int s_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); s_fail++; } \
    else { printf("ok: %s\n", msg); } \
} while (0)

/* 框架 stub */
bool s_exit_request = false;
void game_set_tick_interval(uint32_t ms) { (void)ms; }

/* 纯函数机动计算(独立于全局, 用于 AI 最优性校验) */
static int mob_of(uint8_t b[IS_N][IS_N], int x, int y, int oppx, int oppy) {
    int n = 0;
    for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++) {
            if (dx == 0 && dy == 0) continue;
            int nx = x + dx, ny = y + dy;
            if (nx < 0 || nx >= IS_N || ny < 0 || ny >= IS_N) continue;
            if (b[ny][nx]) continue;
            if (nx == oppx && ny == oppy) continue;
            n++;
        }
    return n;
}
static int score_of(uint8_t b[IS_N][IS_N], int px, int py, int ax, int ay) {
    return 5 * mob_of(b, ax, ay, px, py) - 6 * mob_of(b, px, py, ax, ay);
}
/* 在给定局面下 AI 最优可达分(1 层枚举, 与 is_ai_play 同式) */
static int ai_best_of(uint8_t b[IS_N][IS_N], int px, int py, int ax, int ay) {
    int best = -1000000;
    uint8_t t[IS_N][IS_N];
    memcpy(t, b, sizeof(t));
    for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++) {
            if (dx == 0 && dy == 0) continue;
            int mx = ax + dx, my = ay + dy;
            if (mx < 0 || mx >= IS_N || my < 0 || my >= IS_N) continue;
            if (t[my][mx]) continue;
            if (mx == px && my == py) continue;
            bool any = false;
            for (int ry = 0; ry < IS_N; ry++)
                for (int rx = 0; rx < IS_N; rx++) {
                    if (t[ry][rx]) continue;
                    if ((rx == px && ry == py) || (rx == mx && ry == my)) continue;
                    any = true;
                    t[ry][rx] = 1;
                    int sc = score_of(t, px, py, mx, my);
                    t[ry][rx] = 0;
                    if (sc > best) best = sc;
                }
            if (!any) {
                int sc = score_of(t, px, py, mx, my);
                if (sc > best) best = sc;
            }
        }
    return best;
}

static void test_new_game(void) {
    isola_enter();
    CHECK(is_px == 0 && is_py == 3, "player piece at left middle");
    CHECK(is_ax == 6 && is_ay == 3, "AI piece at right middle");
    int gone = 0;
    for (int y = 0; y < IS_N; y++)
        for (int x = 0; x < IS_N; x++)
            if (is_board[y][x]) gone++;
    CHECK(gone == 0, "all cells floor at start");
    CHECK(is_phase == IS_PH_MOVE && is_turn == IS_PLAYER, "player starts, MOVE mode");
    CHECK(!is_over, "not over at start");
    CHECK(is_player_mobility() == 5, "corner start mobility 5");
    CHECK(is_valid_move_target(is_cx, is_cy), "cursor snapped to valid target");
    CHECK(is_rng.s != 0, "rng seeded non-zero");
}

/* 拆除(x,y)辅助: 棋盘下标 board[y][x] */
static void rm(int x, int y) { is_board[y][x] = 1; }

static void test_move_targets(void) {
    is_new();
    CHECK(is_valid_move_target(1, 3), "(1,3) adjacent floor is target");
    CHECK(is_valid_move_target(1, 4), "(1,4) diagonal is target");
    CHECK(!is_valid_move_target(0, 3), "own cell not a target");
    CHECK(!is_valid_move_target(0, 0), "far cell not a target");
    CHECK(!is_valid_move_target(6, 3), "AI cell not a target");
    /* 拆除格不是目标 */
    rm(1, 2);
    CHECK(!is_valid_move_target(1, 2), "removed cell not a target");
    CHECK(!is_removable(1, 2), "removed cell not removable");
    CHECK(!is_removable(0, 3), "own piece cell not removable");
    CHECK(!is_removable(6, 3), "AI piece cell not removable");
    CHECK(is_removable(2, 3), "plain floor removable");
    /* 拆除邻格削减机动 */
    CHECK(is_player_mobility() == 4, "removing neighbor cuts mobility");
}

static void test_player_turn_win(void) {
    /* 玩家 (0,1) AI (2,2); 围住 AI 只剩 (3,2):
     * 玩家移 (0,1)->(1,1), 拆 (3,2) -> AI 无路 -> 玩家胜 */
    is_new();
    is_px = 0; is_py = 1;
    is_ax = 2; is_ay = 2;
    for (int y = 0; y < IS_N; y++)
        for (int x = 0; x < IS_N; x++) is_board[y][x] = 0;
    rm(0, 0); rm(0, 2); rm(1, 0);
    rm(1, 2); rm(1, 3); rm(2, 1);
    rm(2, 3); rm(3, 1); rm(3, 3);
    is_phase = IS_PH_MOVE;
    CHECK(is_player_mobility() == 1 && is_ai_mobility() == 2,
          "setup mobilities 1 / 2");
    /* 非法确认: 光标在非目标格 -> 无动作 */
    is_cx = 0; is_cy = 1;
    {
        key_event_t ev = { K_OK, 0, false };
        isola_on_key(&ev);
    }
    CHECK(is_px == 0 && is_py == 1 && is_phase == IS_PH_MOVE,
          "OK on non-target is no-op");
    /* 合法移动 */
    is_cx = 1; is_cy = 1;
    {
        key_event_t ev = { K_OK, 0, false };
        isola_on_key(&ev);
    }
    CHECK(is_px == 1 && is_py == 1, "player moved to (1,1)");
    CHECK(is_phase == IS_PH_REMOVE, "auto-switch to REMOVE after move");
    /* 拆除 (3,2): AI 无路 -> 玩家胜 */
    is_cx = 3; is_cy = 2;
    {
        key_event_t ev = { K_OK, 0, false };
        isola_on_key(&ev);
    }
    CHECK(is_board[2][3] == 1, "cell (3,2) removed");
    CHECK(is_over && is_winner == IS_PLAYER, "AI trapped -> player wins");
    CHECK(is_ai_mobility() == 0, "AI mobility zero after trap");
}

static void test_ai_optimal(void) {
    /* 玩家 (0,3) AI (3,3) 开放局面: AI 落子合法 + 恰拆 1 格 + 分数最优 */
    is_new();
    is_px = 0; is_py = 3;
    is_ax = 3; is_ay = 3;
    uint8_t b0[IS_N][IS_N];
    memcpy(b0, is_board, sizeof(b0));
    int exp_best = ai_best_of(b0, is_px, is_py, is_ax, is_ay);
    CHECK(is_ai_mobility() == 8, "center AI mobility 8");
    int lax = is_ax, lay = is_ay;
    is_ai_play();
    /* 落点合法 */
    int dx = is_ax - lax, dy = is_ay - lay;
    CHECK(dx >= -1 && dx <= 1 && dy >= -1 && dy <= 1 && (dx || dy),
          "AI moved one step");
    CHECK(is_board[is_ay][is_ax] == 0, "AI not on removed cell");
    CHECK(!(is_ax == is_px && is_ay == is_py), "AI not on player cell");
    /* 恰好拆掉 1 格, 且非棋所在 */
    int diff = 0, drx = -1, dry = -1;
    for (int y = 0; y < IS_N; y++)
        for (int x = 0; x < IS_N; x++) {
            if (b0[y][x] == 0 && is_board[y][x] == 1) {
                diff++;
                drx = x; dry = y;
            }
        }
    CHECK(diff == 1, "exactly one cell removed");
    CHECK(!(drx == is_px && dry == is_py) && !(drx == is_ax && dry == is_ay),
          "removed cell under no piece");
    /* 最优性: 所选组合分数 == 穷举最优 */
    uint8_t b1[IS_N][IS_N];
    memcpy(b1, b0, sizeof(b1));
    b1[dry][drx] = 1;
    int got = score_of(b1, is_px, is_py, is_ax, is_ay);
    CHECK(got == exp_best, "AI picked an optimal (move,remove) pair");
    /* 再次运行(确定性局面): 依然最优 */
    memcpy(is_board, b0, sizeof(b0));
    is_ax = 3; is_ay = 3;
    is_ai_play();
    CHECK(is_ai_mobility() >= 0, "AI replay no crash");
}

static void test_ai_wins_trap(void) {
    /* 玩家 (2,2) 可走 (1,1)/(3,2); AI (0,1):
     * AI 移 (1,1) 占 (1,1) 并拆 (3,2) -> 玩家无路 -> AI 胜 */
    is_new();
    is_px = 2; is_py = 2;
    is_ax = 0; is_ay = 1;
    for (int y = 0; y < IS_N; y++)
        for (int x = 0; x < IS_N; x++) is_board[y][x] = 0;
    rm(1, 2); rm(1, 3); rm(2, 1);
    rm(2, 3); rm(3, 1); rm(3, 3);
    is_phase = IS_PH_THINK;
    is_turn = IS_AI;
    is_think_at = 0;
    CHECK(is_player_mobility() == 2, "player has two escapes");
    CHECK(is_ai_mobility() == 4, "AI mobility 4");
    isola_tick(2000);                       /* 已过 IS_AI_MS 思考期 */
    CHECK(is_over, "tick ended the game");
    CHECK(is_winner == IS_AI, "AI traps player -> AI wins");
    CHECK(is_player_mobility() == 0, "player mobility zero");
    CHECK(is_ax == 1 && is_ay == 1, "AI moved to (1,1)");
    CHECK(is_board[2][3] == 1, "AI removed (3,2)");
    /* 思考未满时 tick 不应落子 */
    is_new();
    is_phase = IS_PH_THINK;
    is_think_at = 1000000;
    isola_tick(1000000 + 100);
    CHECK(is_phase == IS_PH_THINK, "tick before delay does nothing");
}

static void test_removal_noop(void) {
    /* REMOVE 模式: 光标在已拆格/棋格上按 OK 必须无动作 */
    is_new();
    is_px = 0; is_py = 3;
    is_ax = 6; is_ay = 3;
    for (int y = 0; y < IS_N; y++)
        for (int x = 0; x < IS_N; x++) is_board[y][x] = 0;
    rm(3, 2);
    is_phase = IS_PH_REMOVE;
    is_cx = 3; is_cy = 2;                    /* 已拆格 */
    {
        key_event_t ev = { K_OK, 0, false };
        isola_on_key(&ev);
    }
    CHECK(is_phase == IS_PH_REMOVE && !is_over, "OK on removed cell no-op");
    is_cx = 0; is_cy = 3;                    /* 棋格 */
    {
        key_event_t ev = { K_OK, 0, false };
        isola_on_key(&ev);
    }
    CHECK(is_phase == IS_PH_REMOVE && !is_over, "OK on own piece no-op");
    /* 合法拆除: 切 AI 思考 */
    is_cx = 1; is_cy = 3;
    {
        key_event_t ev = { K_OK, 0, false };
        isola_on_key(&ev);
    }
    CHECK(is_board[3][1] == 1, "valid removal executed");
    CHECK(is_phase == IS_PH_THINK, "valid removal -> AI think");
}

static void test_directions(void) {
    is_new();
    CHECK(is_cx == 0 && is_cy == 2, "cursor snapped to first target (0,2)");
    {
        key_event_t ev = { K_LEFT, 0, true };      /* 方向键重复允许 */
        isola_on_key(&ev);
    }
    CHECK(is_cx == 0 && is_cy == 2, "left at edge stays");
    {
        key_event_t ev = { K_DOWN, 0, true };
        isola_on_key(&ev);
    }
    CHECK(is_cx == 0 && is_cy == 3, "down repeat moves cursor to (0,3)");
    {
        key_event_t ev = { K_OK, 0, true };        /* 确认键重复必须忽略 */
        isola_on_key(&ev);
    }
    CHECK(is_px == 0 && is_py == 3, "repeat OK ignored");
    {
        key_event_t ev = { K_CHAR, 'd', false };
        isola_on_key(&ev);
    }
    CHECK(is_cx == 1 && is_cy == 3, "char d moves cursor to (1,3)");
}

int main(void) {
    test_new_game();
    test_move_targets();
    test_player_turn_win();
    test_ai_optimal();
    test_ai_wins_trap();
    test_removal_noop();
    test_directions();
    if (s_fail == 0) { printf("ALL ISOLA TESTS PASSED\n"); return 0; }
    printf("%d TEST(S) FAILED\n", s_fail);
    return 1;
}
