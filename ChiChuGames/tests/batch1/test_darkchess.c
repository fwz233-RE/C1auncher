/* 暗棋 DARK CHESS — host 逻辑单测: 直接包含 src/games/darkchess.c
 * 覆盖: 克制矩阵(含循环 兵>将) / 洗牌分布 / 翻开归属 / 吃子与目标选择 /
 * AI 贪心(吃能赢的、避开送子) / 无行动判负 / tick 调度 / EV 数学 / 终局压力 */
#include "../../src/games/darkchess.c"
#include <stdio.h>
#include <string.h>

bool s_exit_request = false;   /* main.c 提供, host stub */

static int s_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); s_fail++; } \
    else { printf("ok: %s\n", msg); } \
} while (0)

static void clear_board(void) {
    for (int r = 0; r < DC_ROWS; r++)
        for (int c = 0; c < DC_COLS; c++) dc_cell[r][c] = 0;
}

/* ---- 1. 克制矩阵 ---- */
static void test_beats(void) {
    CHECK(dc_beats(DC_J, DC_S), "Jiang beats Shi");
    CHECK(dc_beats(DC_J, DC_X), "Jiang beats Xiang");
    CHECK(dc_beats(DC_J, DC_M), "Jiang beats Ma");
    CHECK(dc_beats(DC_J, DC_C), "Jiang beats Ju");
    CHECK(dc_beats(DC_J, DC_P), "Jiang beats Pao");
    CHECK(!dc_beats(DC_J, DC_B), "Jiang cannot beat Bing");
    CHECK(!dc_beats(DC_J, DC_J), "Jiang vs Jiang no capture");
    CHECK(dc_beats(DC_S, DC_B), "Shi beats Bing");
    CHECK(dc_beats(DC_X, DC_M) && dc_beats(DC_M, DC_C), "Xiang>Ma>Ju chain");
    CHECK(dc_beats(DC_C, DC_P) && dc_beats(DC_P, DC_B), "Ju>Pao>Bing chain");
    CHECK(!dc_beats(DC_X, DC_J), "Xiang cannot beat Jiang");
    CHECK(!dc_beats(DC_C, DC_M), "Ju cannot beat Ma");
    CHECK(!dc_beats(DC_P, DC_P), "Pao vs Pao no capture");
    CHECK(dc_beats(DC_B, DC_J), "Bing beats Jiang (cycle)");
    CHECK(!dc_beats(DC_B, DC_S), "Bing cannot beat Shi");
    CHECK(!dc_beats(DC_B, DC_B), "Bing vs Bing no capture");
}

/* ---- 2. 洗牌: 32 全背面 + 分布 + 种子差异 ---- */
static void test_new(void) {
    int cnt[7] = { 0, 0, 0, 0, 0, 0, 0 };
    uint8_t snap[DC_ROWS][DC_COLS];
    rng_seed(&dc_rng, 12345);
    dc_new();
    int face = 0;
    for (int r = 0; r < DC_ROWS; r++)
        for (int c = 0; c < DC_COLS; c++) {
            uint8_t v = dc_cell[r][c];
            snap[r][c] = v;
            if (v & DC_FACE) face++;
            if (v & (DC_OWN_P | DC_OWN_A)) CHECK(0, "no owner at setup");
            cnt[v & DC_TYPE_MASK]++;
        }
    CHECK(face == 32, "all 32 face down at setup");
    CHECK(cnt[DC_J] == 2 && cnt[DC_S] == 4 && cnt[DC_X] == 4 &&
          cnt[DC_M] == 4 && cnt[DC_C] == 4 && cnt[DC_P] == 4 &&
          cnt[DC_B] == 10, "piece distribution J2 S4 X4 M4 C4 P4 B10");
    CHECK(dc_cx == 0 && dc_cy == 0 && !dc_over, "cursor + fresh state");
    rng_seed(&dc_rng, 999);
    dc_new();
    int diff = 0;
    for (int r = 0; r < DC_ROWS; r++)
        for (int c = 0; c < DC_COLS; c++)
            if (dc_cell[r][c] != snap[r][c]) diff++;
    CHECK(diff > 0, "different seed => different layout");
}

/* ---- 3. 翻开: 归翻出方 + 排定 AI 回合 ---- */
static void test_flip(void) {
    rng_seed(&dc_rng, 7);
    dc_new();
    dc_cx = 0;
    dc_cy = 0;
    int t0 = (int)(dc_cell[0][0] & DC_TYPE_MASK);
    CHECK(dc_player_act(), "player flips own cell");
    CHECK(dc_cell[0][0] == (uint8_t)(t0 | DC_OWN_P), "flipped piece owned by player");
    CHECK(dc_ai_pending && dc_ai_at > now_ms(), "AI turn scheduled with delay");
    /* 面板上未翻开的子不能被吃 */
    CHECK(!dc_can_eat((uint8_t)(DC_J | DC_FACE), DC_B, 1),
          "face-down piece cannot be eaten (even by Bing)");
}

/* ---- 4. 吃子: 选价值最高的目标, 子移入敌格 ---- */
static void test_capture(void) {
    clear_board();
    dc_cell[0][0] = (uint8_t)(DC_C | DC_OWN_P);   /* 玩家车 */
    dc_cell[0][1] = (uint8_t)(DC_P | DC_OWN_A);   /* AI 炮 (120) */
    dc_cell[0][2] = (uint8_t)(DC_B | DC_OWN_A);   /* AI 兵 (10) */
    dc_cell[1][0] = (uint8_t)(DC_S | DC_FACE);    /* AI 可翻, 防立刻终局 */
    dc_cx = 0;
    dc_cy = 0;
    CHECK(dc_player_act(), "player capture succeeds");
    CHECK(dc_cell[0][0] == 0, "source cell vacated");
    CHECK(dc_cell[0][1] == (uint8_t)(DC_C | DC_OWN_P),
          "captured highest-value target (Pao, not Bing)");
    CHECK(dc_cell[0][2] == (uint8_t)(DC_B | DC_OWN_A), "Bing untouched");
    CHECK(dc_ai_pending, "AI turn after capture");
    CHECK(!dc_hl_ok, "AI highlight cleared on player action");
}

/* ---- 5. 非法操作 ---- */
static void test_illegal(void) {
    clear_board();
    dc_cell[0][0] = (uint8_t)(DC_M | DC_OWN_P);
    dc_cell[0][1] = (uint8_t)(DC_C | DC_OWN_P);
    dc_cx = 0;
    dc_cy = 0;
    CHECK(!dc_player_act(), "cannot capture own piece");
    CHECK(!dc_hl_ok, "no action leaves board unchanged");
    dc_cell[0][0] = (uint8_t)(DC_J | DC_OWN_A);
    CHECK(!dc_player_act(), "cannot move enemy piece");
    clear_board();
    CHECK(!dc_player_act(), "empty cell: no action");
}

/* ---- 6. AI 贪心: 兵吃将(收益 600 > 翻子期望) ---- */
static void test_ai_capture(void) {
    clear_board();
    dc_cell[0][0] = (uint8_t)(DC_B | DC_OWN_A);   /* AI 兵 */
    dc_cell[0][1] = (uint8_t)(DC_J | DC_OWN_P);   /* 玩家将 */
    dc_cell[3][7] = (uint8_t)(DC_S | DC_FACE);
    dc_ai_move();
    CHECK(dc_cell[0][0] == 0, "AI vacates source");
    CHECK(dc_cell[0][1] == (uint8_t)(DC_B | DC_OWN_A), "AI Bing ate Jiang");
    CHECK(dc_hl_ok && dc_hlx == 1 && dc_hly == 0, "AI highlight on capture cell");
    CHECK(!dc_over, "game continues (face-down remains for player)");
}

/* ---- 7. AI 避开送子: 车吃兵会被马反吃(10-300<0) → 改翻子 ---- */
static void test_ai_avoid_suicide(void) {
    clear_board();
    dc_cell[0][0] = (uint8_t)(DC_C | DC_OWN_A);   /* AI 车 */
    dc_cell[0][1] = (uint8_t)(DC_B | DC_OWN_P);   /* 玩家兵(可吃) */
    dc_cell[1][1] = (uint8_t)(DC_M | DC_OWN_P);   /* 玩家马(反吃车) */
    for (int r = 0; r < DC_ROWS; r++)
        for (int c = 0; c < DC_COLS; c++)
            if (!dc_cell[r][c]) dc_cell[r][c] = (uint8_t)(DC_B | DC_FACE);
    int before = dc_count(2);              /* AI 车已归 AI */
    dc_ai_move();
    CHECK(dc_cell[0][1] == (uint8_t)(DC_B | DC_OWN_P),
          "AI refused suicidal capture of Bing");
    CHECK(dc_count(2) == before + 1, "AI flipped instead of capturing");
    CHECK(dc_hl_ok, "AI highlight set on flipped cell");
    CHECK(!dc_over, "game continues");
}

/* ---- 8. 吃光玩家最后一子 + 玩家无行动 → AI 胜 ---- */
static void test_ai_win(void) {
    clear_board();
    dc_cell[0][0] = (uint8_t)(DC_B | DC_OWN_P);   /* 玩家唯一子 */
    dc_cell[0][1] = (uint8_t)(DC_C | DC_OWN_A);   /* AI 车 */
    dc_ai_move();
    CHECK(dc_over && dc_winner == 2, "AI wins: player has no pieces/actions");
    CHECK(dc_cell[0][0] == (uint8_t)(DC_C | DC_OWN_A), "AI piece moved onto last cell");
    CHECK(dc_cell[0][1] == 0, "target cell vacated");
}

/* ---- 9. AI 无行动 → 玩家胜 ---- */
static void test_ai_stuck(void) {
    clear_board();
    dc_cell[0][0] = (uint8_t)(DC_B | DC_OWN_P);   /* 玩家兵 */
    dc_cell[3][7] = (uint8_t)(DC_C | DC_OWN_A);   /* AI 车(孤立) */
    dc_ai_move();
    CHECK(dc_over && dc_winner == 1, "player wins: AI has no action");
}

/* ---- 10. tick 调度: 延时前不动, 到期才行动 ---- */
static void test_tick(void) {
    rng_seed(&dc_rng, 42);
    dc_new();
    dc_cx = 0;
    dc_cy = 0;
    CHECK(dc_player_act(), "player flips");
    CHECK(dc_ai_pending, "AI pending after player action");
    darkchess_tick(now_ms() + 100);
    CHECK(dc_ai_pending, "AI not moved before delay elapses");
    CHECK(dc_count(2) == 0, "board unchanged before delay");
    darkchess_tick(now_ms() + 2000);
    CHECK(!dc_ai_pending, "AI moved after delay");
    CHECK(dc_count(2) == 1, "AI owns exactly one revealed piece (flip)");
}

/* ---- 11. 翻子期望值 EV 数学 ---- */
static void test_flip_ev(void) {
    int cnt1[7] = { 0, 0, 0, 0, 0, 0, 1 };          /* 只剩 1 兵 */
    CHECK(dc_flip_ev(DC_J, 1, cnt1, 10) == 600,
          "flip vs Jiang: Bing kills it -> EV 600");
    CHECK(dc_flip_ev(DC_S, 1, cnt1, 10) == -10,
          "flip vs Shi with only Bings left -> always lose, EV -avg");
    int cnt2[7] = { 1, 0, 0, 0, 0, 0, 9 };          /* 1 将 + 9 兵 */
    CHECK(dc_flip_ev(DC_J, 10, cnt2, 69) == 533,
          "mixed: EV=(9*600-1*69)/10=533");
    CHECK(dc_flip_ev(DC_C, 10, cnt2, 69) == -32,
          "mixed vs Ju: only Jiang beats it -> (300-9*69)/10=-32");
}

/* ---- 12. has_action / 边界 ---- */
static void test_has_action(void) {
    clear_board();
    CHECK(!dc_has_action(1) && !dc_has_action(2), "empty board: no action");
    dc_cell[0][0] = (uint8_t)(DC_B | DC_FACE);
    CHECK(dc_has_action(1) && dc_has_action(2), "face-down cell: flip possible");
    clear_board();
    dc_cell[0][0] = (uint8_t)(DC_B | DC_OWN_P);
    dc_cell[0][1] = (uint8_t)(DC_J | DC_OWN_A);
    CHECK(dc_has_action(1), "Bing adjacent to Jiang: capture possible");
    clear_board();
    dc_cell[0][0] = (uint8_t)(DC_B | DC_OWN_P);
    dc_cell[0][1] = (uint8_t)(DC_C | DC_OWN_A);
    CHECK(!dc_has_action(1), "Bing next to Ju: cannot capture, no flip");
}

/* ---- 13. 计数文本 + 渲染冒烟 ---- */
static void test_render_smoke(void) {
    char buf[16];
    clear_board();
    for (int c = 0; c < 7; c++) dc_cell[0][c] = (uint8_t)(DC_B | DC_OWN_P);
    for (int c = 0; c < 3; c++) dc_cell[1][c] = (uint8_t)(DC_S | DC_OWN_A);
    dc_counts(buf);
    CHECK(strcmp(buf, "YOU 7  AI 3") == 0, "HUD counts format");
    dc_ai_pending = true;
    darkchess_render();
    dc_ai_pending = false;
    darkchess_render();
    dc_over = true;
    dc_winner = 1;
    dc_over_full = true;   /* host 下跳过 disp_force_full */
    darkchess_render();
    CHECK(1, "render smoke: mid-game / AI turn / over all draw");
}

/* ---- 14. 完整对局压力: 必然终止、胜负合法 ---- */
static void test_full_game(void) {
    rng_seed(&dc_rng, 2026);
    dc_new();
    int turns = 0;
    while (!dc_over && turns < 300) {
        bool acted = false;
        for (int r = 0; r < DC_ROWS && !acted; r++)
            for (int c = 0; c < DC_COLS && !acted; c++) {
                dc_cx = c;
                dc_cy = r;
                if (dc_player_act()) acted = true;
            }
        CHECK(acted, "player always finds an action while game running");
        if (!acted) break;
        while (dc_ai_pending && !dc_over) darkchess_tick(now_ms() + 1000);
        turns++;
    }
    CHECK(dc_over, "full game terminates");
    CHECK(dc_winner == 1 || dc_winner == 2, "winner is player or AI");
    CHECK(turns < 300, "terminates within turn cap");
    printf("full game ended in %d turns (winner=%d)\n", turns, dc_winner);
}

int main(void) {
    test_beats();
    test_new();
    test_flip();
    test_capture();
    test_illegal();
    test_ai_capture();
    test_ai_avoid_suicide();
    test_ai_win();
    test_ai_stuck();
    test_tick();
    test_flip_ev();
    test_has_action();
    test_render_smoke();
    test_full_game();
    if (s_fail == 0) printf("ALL TESTS PASSED\n");
    else printf("%d FAILURES\n", s_fail);
    return s_fail ? 1 : 0;
}
