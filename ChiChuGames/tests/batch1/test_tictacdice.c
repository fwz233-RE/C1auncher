/* TIC-TAC-DICE 逻辑单测 — host 编译, 包含游戏 .c 直接访问静态状态 */
#include "../../src/games/tictacdice.c"
#include <stdio.h>
#include <string.h>

static int s_fail = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); s_fail++; } \
    else { printf("ok: %s\n", msg); } \
} while (0)

/* ---- host 框架 stub ---- */
bool s_exit_request = false;
void game_set_tick_interval(uint32_t ms) { (void)ms; }

static bool px(int x, int y) {
    int off = (y >> 3) * (int)CCG_W + x;
    return (g_fb[off] & (uint8_t)(0x80 >> (y & 7))) != 0;
}

static void reset(void) {
    td_new_round();
    td_pwins = td_awins = td_draws = 0;
    td_rng.s = 0x12345678u;
}

static key_event_t press(ccg_key k) {
    key_event_t ev;
    memset(&ev, 0, sizeof ev);
    ev.key = k;
    return ev;
}

/* 找种子: 下一次 rng_range(&r,n) 的结果为 want */
static uint32_t seed_for_range(uint32_t n, uint32_t want) {
    rng_t r;
    for (uint32_t s = 1; s < 1000000u; s++) {
        r.s = s;
        if (rng_range(&r, n) == want) return s;
    }
    return 1;
}

/* ---- 胜负判定 ---- */
static void test_win(void) {
    uint8_t b[3][3];
    memset(b, 0, sizeof b);
    b[0][0] = b[0][1] = b[0][2] = 1;
    CHECK(td_check_win(b) == 1, "row 3-in-line wins");
    memset(b, 0, sizeof b);
    b[0][1] = b[1][1] = b[2][1] = 2;
    CHECK(td_check_win(b) == 2, "col 3-in-line wins");
    memset(b, 0, sizeof b);
    b[0][0] = b[1][1] = b[2][2] = 1;
    CHECK(td_check_win(b) == 1, "diag \\ wins");
    memset(b, 0, sizeof b);
    b[0][2] = b[1][1] = b[2][0] = 2;
    CHECK(td_check_win(b) == 2, "diag / wins");
    memset(b, 0, sizeof b);
    b[0][0] = 1; b[1][1] = 2; b[2][2] = 1;
    CHECK(td_check_win(b) == 0, "mixed marks not a win");
    memset(b, 0, sizeof b);
    b[1][0] = b[1][1] = 2; b[1][2] = 1;
    CHECK(td_check_win(b) == 0, "2-in-a-row not a win");
    memset(b, 0, sizeof b);
    CHECK(td_check_win(b) == 0, "empty board safe");
}

/* ---- 掷骰→选列→落子→AI 流程 ---- */
static void test_flow(void) {
    reset();
    key_event_t ev = press(K_OK);
    tictacdice_on_key(&ev);                    /* 掷骰 */
    CHECK(td_phase == TD_PH_SELECT, "OK rolls and enters select");
    CHECK(td_row >= 0 && td_row <= 2, "rolled row in range");
    CHECK(td_die == td_row + 1, "die face matches row");
    CHECK(td_col == 1, "cursor defaults to center col");
    ev = press(K_RIGHT);
    tictacdice_on_key(&ev);
    CHECK(td_col == 2, "RIGHT moves cursor");
    ev = press(K_LEFT);
    tictacdice_on_key(&ev);
    ev = press(K_LEFT);
    tictacdice_on_key(&ev);
    CHECK(td_col == 0, "LEFT wraps to col 0");
    ev = press(K_OK);
    tictacdice_on_key(&ev);                    /* 落子 */
    CHECK(td_board[td_row][0] == 1, "X placed in rolled row");
    CHECK(td_phase == TD_PH_AI_ROLL, "player move hands over to AI");
    tictacdice_tick(0);                        /* AI 掷骰 */
    CHECK(td_phase == TD_PH_AI_PLACE || td_phase == TD_PH_AI_ROLL,
          "AI roll sets next phase");
    if (td_phase == TD_PH_AI_PLACE) tictacdice_tick(0);
    CHECK(td_phase == TD_PH_ROLL, "AI completes move, back to player");
    int stones = 0;
    for (int y = 0; y < 3; y++)
        for (int x = 0; x < 3; x++)
            if (td_board[y][x]) stones++;
    CHECK(stones == 2, "one X and one O on board");
    /* AI 思考中按键无效 */
    td_phase = TD_PH_AI_PLACE;
    ev = press(K_OK);
    tictacdice_on_key(&ev);
    CHECK(td_phase == TD_PH_AI_PLACE, "keys ignored while AI thinks");
}

/* ---- 满行跳回合 ---- */
static void test_skip(void) {
    reset();
    td_board[0][0] = 2; td_board[0][1] = 1; td_board[0][2] = 2;  /* 满行但不连三 */
    td_rng.s = seed_for_range(3, 0);                        /* 强制掷到行 0 */
    key_event_t ev = press(K_OK);
    tictacdice_on_key(&ev);
    CHECK(td_phase == TD_PH_AI_ROLL, "rolling a full row skips player turn");
    CHECK(td_board[0][0] == 2 && td_board[0][1] == 1 && td_board[0][2] == 2,
          "skip places no stone");
    /* AI 掷到满行 → AI 同样跳过, 回到玩家(直接构造 AI 回合) */
    reset();
    td_board[0][0] = 2; td_board[0][1] = 1; td_board[0][2] = 2;
    td_phase = TD_PH_AI_ROLL;
    td_rng.s = seed_for_range(3, 0);
    tictacdice_tick(0);
    CHECK(td_phase == TD_PH_ROLL, "AI skip on full row returns to player");
    CHECK(td_board[0][0] == 2 && td_board[0][1] == 1 && td_board[0][2] == 2,
          "AI skip places no stone");
}

/* ---- AI 启发式 ---- */
static void test_ai_heuristic(void) {
    reset();
    td_row = 0;
    td_board[0][0] = 2; td_board[0][1] = 2;
    CHECK(td_ai_pick() == 2, "AI takes its own winning move");
    reset();
    td_row = 0;
    td_board[0][0] = 1; td_board[0][1] = 1;
    CHECK(td_ai_pick() == 2, "AI blocks player threat");
    reset();
    td_row = 0;
    td_board[0][0] = 1;
    CHECK(td_ai_pick() == 1, "AI prefers center when no threat");
    reset();
    td_row = 0;
    td_board[0][0] = td_board[0][1] = td_board[0][2] = 1;
    CHECK(td_ai_pick() == -1, "full row yields -1");
}

/* ---- 终局: 胜/平 + 战绩 + 重开 ---- */
static void test_end(void) {
    reset();
    td_phase = TD_PH_SELECT;
    td_row = 1; td_col = 2;
    td_board[1][0] = 1; td_board[1][1] = 1;          /* 玩家行 1 双连 */
    td_board[0][0] = 2; td_board[2][2] = 2;
    key_event_t ev = press(K_OK);
    tictacdice_on_key(&ev);
    CHECK(td_phase == TD_PH_OVER && td_winner == 1, "player 3-in-line ends game");
    CHECK(td_pwins == 1 && td_awins == 0 && td_draws == 0, "player win recorded");
    ev = press(K_OK);                                 /* OVER 时 OK = 重开 */
    tictacdice_on_key(&ev);
    CHECK(td_phase == TD_PH_ROLL, "OK at over starts new round");
    CHECK(td_pwins == 1, "match record persists across rounds");
    CHECK(td_board[1][2] == 0 && td_die == 0, "new round board reset");

    reset();                                          /* 平局 */
    td_board[0][0] = 1; td_board[0][1] = 2; td_board[0][2] = 1;
    td_board[1][0] = 2; td_board[1][1] = 1; td_board[1][2] = 2;
    td_board[2][0] = 2; td_board[2][1] = 1; td_board[2][2] = 2;
    td_phase = TD_PH_AI_PLACE;
    td_check_end();
    CHECK(td_phase == TD_PH_OVER && td_winner == 3, "full board without line is draw");
    CHECK(td_draws == 1 && td_pwins == 0 && td_awins == 0, "draw recorded");
    td_check_end();                                   /* 重复调用不重复记 */
    CHECK(td_draws == 1, "record counted once");
}

/* ---- 完整随机对局: 终止性 + 结果一致性 ---- */
static void test_full_game(void) {
    reset();
    td_rng.s = 42u;
    int guard = 0;
    while (td_phase != TD_PH_OVER && guard++ < 500) {
        if (td_phase == TD_PH_ROLL || td_phase == TD_PH_SELECT) {
            if (td_phase == TD_PH_SELECT) {                 /* 光标移到该行首个空列 */
                for (int c = 0; c < 3; c++)
                    if (!td_board[td_row][c]) { td_col = c; break; }
            }
            key_event_t ev = press(K_OK);
            tictacdice_on_key(&ev);
        } else {
            tictacdice_tick(0);
        }
    }
    CHECK(td_phase == TD_PH_OVER, "full game terminates (no skip deadlock)");
    CHECK(td_pwins + td_awins + td_draws == 1, "exactly one result recorded");
    if (td_winner == 1)
        CHECK(td_check_win(td_board) == 1, "player win consistent with board");
    else if (td_winner == 2)
        CHECK(td_check_win(td_board) == 2, "AI win consistent with board");
    else {
        int empty = 0;
        for (int y = 0; y < 3; y++)
            for (int x = 0; x < 3; x++)
                if (!td_board[y][x]) empty++;
        CHECK(empty == 0, "draw implies full board");
    }
    int xc = 0, oc = 0;
    for (int y = 0; y < 3; y++)
        for (int x = 0; x < 3; x++)
            if (td_board[y][x] == 1) xc++;
            else if (td_board[y][x] == 2) oc++;
    CHECK(xc == oc || xc == oc + 1, "X and O counts balanced (X first)");
}

/* ---- 渲染冒烟: HUD/反白行/光标环/骰子 ---- */
static void test_render(void) {
    reset();
    td_phase = TD_PH_SELECT;
    td_row = 0; td_col = 1;
    td_die = 3;
    td_board[0][0] = 1;
    tictacdice_render();
    CHECK(px(2, 0) == 1, "HUD title pixel black");
    CHECK(px(0, CCG_HUD_H - 1) == 1, "HUD divider black");
    CHECK(px(TD_OX, TD_OY + TD_CELL) == 1, "non-required row border black");
    CHECK(px(TD_OX + 1, TD_OY + 1) == 1, "required row inverted (black bg)");
    CHECK(px(TD_OX + 44 - 2, TD_OY + 1) == 1, "cursor ring drawn after cells");
    CHECK(px(TD_DIE_X + 8 + 2 * 12, TD_DIE_Y + 8 + 2 * 12) == 1,
          "die pip bottom-right for face 3");
    td_phase = TD_PH_ROLL;
    tictacdice_render();
    CHECK(px(TD_OX + 1, TD_OY + 1) == 0, "non-required row white");
    td_phase = TD_PH_OVER;
    td_winner = 1;
    tictacdice_render();
    CHECK(px(2, 2) == 1, "over banner drawn in HUD");
}

int main(void) {
    test_win();
    test_flow();
    test_skip();
    test_ai_heuristic();
    test_end();
    test_full_game();
    test_render();
    if (s_fail) {
        printf("TICTACDICE: %d FAILED\n", s_fail);
        return 1;
    }
    printf("TICTACDICE: ALL TESTS PASSED\n");
    return 0;
}
