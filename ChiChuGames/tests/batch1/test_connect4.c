/* CONNECT 4 逻辑单测 — host 编译, 包含游戏 .c 直接访问静态状态 */
#include "../../src/games/connect4.c"
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

static void clear_board(void) {
    for (int r = 0; r < C4_ROWS; r++)
        for (int c = 0; c < C4_COLS; c++) c4_board[r][c] = 0;
    for (int c = 0; c < C4_COLS; c++) c4_col_h[c] = 0;
    c4_steps = 0;
    c4_over = false;
    c4_winner = 0;
    c4_turn = 1;
    c4_last_col = -1;
    c4_last_row = -1;
}

/* ---- 胜负判定: 横/竖/两斜 + 反例 ---- */
static void test_win(void) {
    clear_board();
    /* 横: 底行 0-3 黑 */
    c4_board[5][0] = c4_board[5][1] = c4_board[5][2] = c4_board[5][3] = 1;
    CHECK(c4_win_at(3, 5) && c4_win_at(0, 5) && c4_win_at(2, 5),
          "horizontal 4-in-row detected");
    /* 3 连不判胜 */
    clear_board();
    c4_board[5][0] = c4_board[5][1] = c4_board[5][2] = 1;
    c4_board[5][4] = 1;
    CHECK(!c4_win_at(2, 5) && !c4_win_at(5, 4), "3-in-row is not a win");
    /* 竖: 第 2 列 3..6 行(实际 row2..5) */
    clear_board();
    c4_board[2][3] = c4_board[3][3] = c4_board[4][3] = c4_board[5][3] = 2;
    CHECK(c4_win_at(3, 5) && c4_win_at(3, 2), "vertical 4-in-row detected");
    /* 斜 \ : row2/col0, row3/col1, row4/col2, row5/col3; 参数 (col,row) */
    clear_board();
    c4_board[2][0] = c4_board[3][1] = c4_board[4][2] = c4_board[5][3] = 1;
    CHECK(c4_win_at(3, 5) && c4_win_at(0, 2), "diag down-right detected");
    clear_board();
    c4_board[2][0] = c4_board[3][1] = c4_board[4][2] = 1;   /* 3 连 */
    CHECK(!c4_win_at(0, 2), "3 on diag is not a win");
    /* 斜 / : row5/col0, row4/col1, row3/col2, row2/col3; 参数 (col,row) */
    clear_board();
    c4_board[5][0] = c4_board[4][1] = c4_board[3][2] = c4_board[2][3] = 1;
    CHECK(c4_win_at(3, 2) && c4_win_at(0, 5), "diag up-right detected");
    /* 空格和无最后落子坐标的初始/满盘状态必须安全 */
    CHECK(!c4_win_at(0, 0), "empty cell never wins");
    CHECK(!c4_win_at(-1, -1), "missing last move never indexes board");
}

/* ---- 重力落子 + 列满防护 ---- */
static void test_gravity(void) {
    clear_board();
    c4_drop(0, 1);          /* 玩家黑 */
    CHECK(c4_board[5][0] == 1 && c4_col_h[0] == 1 && c4_steps == 1,
          "first drop lands at bottom row 5");
    CHECK(c4_turn == 2, "turn flips to AI after drop");
    c4_drop(0, 2);          /* AI 白叠上 */
    CHECK(c4_board[4][0] == 2 && c4_col_h[0] == 2,
          "second drop stacks above");
    /* 列满后 on_key 的 OK 必须无效果 */
    for (int r = 0; r < C4_ROWS; r++) c4_board[r][1] = (uint8_t)((r % 2) + 1);
    c4_col_h[1] = C4_ROWS;
    c4_steps = 6;
    c4_col = 1;
    c4_turn = 1;
    key_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.key = K_OK;
    connect4_on_key(&ev);
    CHECK(c4_col_h[1] == C4_ROWS && c4_steps == 6 && !c4_over,
          "OK on full column is a no-op");
}

/* ---- AI: 堵四(横/竖) ---- */
static void test_ai_block(void) {
    /* 玩家底行 0-2 三连, 轮到 AI: 唯一正确 = 堵第 3 列 */
    clear_board();
    c4_board[5][0] = c4_board[5][1] = c4_board[5][2] = 1;
    c4_col_h[0] = c4_col_h[1] = c4_col_h[2] = 1;
    c4_steps = 3;
    c4_turn = 2;
    int col = c4_ai_best();
    CHECK(col == 3, "AI blocks horizontal 4-threat (col 3)");
    c4_drop(col, 2);
    CHECK(c4_board[5][3] == 2, "block stone placed");
    CHECK(!c4_over, "block does not end game");

    /* 玩家竖 3 连(第 2 列 row3..5), 轮到 AI: 唯一正确 = 堵第 2 列 */
    clear_board();
    c4_board[3][2] = c4_board[4][2] = c4_board[5][2] = 1;
    c4_col_h[2] = 3;
    c4_steps = 3;
    c4_turn = 2;
    col = c4_ai_best();
    CHECK(col == 2, "AI blocks vertical 4-threat (col 2)");

    /* 玩家斜威胁: (4,0)(4,1)(4,2) 三连 + 空 (4,3) → 堵第 3 列 */
    clear_board();
    c4_board[4][0] = c4_board[4][1] = c4_board[4][2] = 1;
    c4_col_h[0] = c4_col_h[1] = c4_col_h[2] = 2;   /* 上面各还有一格 */
    c4_board[5][0] = c4_board[5][1] = c4_board[5][2] = 2;
    c4_steps = 6;
    c4_turn = 2;
    col = c4_ai_best();
    CHECK(col == 3, "AI blocks diagonal 4-threat (col 3)");
}

/* ---- AI: 抢胜/不送胜 ---- */
static void test_ai_win(void) {
    /* AI 底行 0-2 三连, 轮到 AI → 下第 3 列直接胜 */
    clear_board();
    c4_board[5][0] = c4_board[5][1] = c4_board[5][2] = 2;
    c4_col_h[0] = c4_col_h[1] = c4_col_h[2] = 1;
    c4_steps = 3;
    c4_turn = 2;
    int col = c4_ai_best();
    CHECK(col == 3, "AI takes its own winning move (col 3)");

    /* AI 竖 3 连也抢胜 */
    clear_board();
    c4_board[2][4] = c4_board[3][4] = c4_board[4][4] = 2;
    c4_col_h[4] = 3;
    c4_steps = 3;
    c4_turn = 2;
    col = c4_ai_best();
    CHECK(col == 4, "AI takes vertical winning move (col 4)");
}

/* ---- 玩家胜 + 结束态按键 ---- */
static void test_player_win(void) {
    clear_board();
    c4_board[5][0] = c4_board[5][1] = c4_board[5][2] = 1;
    c4_col_h[0] = c4_col_h[1] = c4_col_h[2] = 1;
    c4_steps = 3;
    c4_col = 3;
    c4_turn = 1;
    key_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.key = K_OK;
    connect4_on_key(&ev);
    CHECK(c4_over && c4_winner == 1, "player wins with 4th stone");
    CHECK(c4_board[5][3] == 1, "winning stone placed at col 3");
    CHECK(c4_steps == 4, "AI does not reply after win");
    /* 结束态: N/OK 重开, BACK 退出 */
    s_exit_request = false;
    ev.key = K_CHAR;
    ev.ch = 'n';
    connect4_on_key(&ev);
    CHECK(!c4_over && c4_steps == 0, "'n' after game over restarts");
    c4_over = true;
    ev.ch = 0;
    ev.key = K_BACK;
    connect4_on_key(&ev);
    CHECK(s_exit_request, "BACK after game over quits");
    s_exit_request = false;
}

/* ---- 满盘平局 + 死循环防护 ---- */
static void test_draw(void) {
    /* 全盘棋盘格(每 4 窗恒 2:2), 无四连 → 平局判定 */
    clear_board();
    for (int r = 0; r < C4_ROWS; r++)
        for (int c = 0; c < C4_COLS; c++)
            c4_board[r][c] = (uint8_t)(((r + c) & 1) ? 2 : 1);
    for (int c = 0; c < C4_COLS; c++) c4_col_h[c] = C4_ROWS;
    c4_steps = C4_COLS * C4_ROWS;
    CHECK(c4_check_end() && c4_over && c4_winner == 0, "full board = DRAW");
    /* 死循环防护: 满盘 negamax 必须立即返回 0 */
    int v = c4_negamax(C4_DEPTH, -(C4_WIN + C4_DEPTH) - 1,
                       (C4_WIN + C4_DEPTH) + 1, 2);
    CHECK(v == 0, "negamax on full board returns 0 (draw, no hang)");
    CHECK(c4_ai_best() == C4_COLS / 2, "ai_best on full board defensive col");
}

/* ---- 按键: 光标边界/重复忽略 ---- */
static void test_keys(void) {
    clear_board();
    c4_col = 0;
    key_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.key = K_LEFT;
    ev.is_repeat = true;
    connect4_on_key(&ev);
    CHECK(c4_col == 0, "cursor clamps at left edge");
    ev.key = K_RIGHT;
    connect4_on_key(&ev);
    CHECK(c4_col == 1, "cursor moves right");
    for (int i = 0; i < 10; i++) { ev.key = K_RIGHT; connect4_on_key(&ev); }
    CHECK(c4_col == C4_COLS - 1, "cursor clamps at right edge");
    /* 重复的确认键必须被忽略(不落子) */
    c4_col = 0;
    ev.key = K_OK;
    ev.is_repeat = true;
    connect4_on_key(&ev);
    CHECK(c4_steps == 0, "repeat OK ignored");
    /* WASD 移动 */
    ev.key = K_CHAR;
    ev.ch = 'a';
    ev.is_repeat = false;
    c4_col = 3;
    connect4_on_key(&ev);
    CHECK(c4_col == 2, "'a' moves left");
    ev.ch = 'd';
    c4_col = C4_COLS - 1;
    connect4_on_key(&ev);
    CHECK(c4_col == C4_COLS - 1, "'d' clamps at right edge");
}

/* ---- AI 随机性安全: 种子 0 也会被 rng_seed 纠正(xorshift 恒 0 死循环) ---- */
static void test_rng_guard(void) {
    rng_t r;
    rng_seed(&r, 0);
    CHECK(r.s != 0, "rng_seed(0) yields non-zero state");
    uint32_t a = rng_next(&r);
    uint32_t b = rng_next(&r);
    CHECK(a != 0 && b != 0, "xorshift does not lock to zero");
    /* 平手列随机在合法范围内 */
    rng_seed(&c4_rng, 12345);
    CHECK(rng_range(&c4_rng, 7) < 7, "tie-break range in bounds");
}

/* ---- 完整对局模拟: 确定性 AI + 固定策略, 必须终止 ---- */
static void test_full_game(void) {
    clear_board();
    c4_turn = 1;
    c4_col = C4_COLS / 2;
    c4_over = false;
    c4_steps = 0;
    rng_seed(&c4_rng, 1);          /* 固定种子 → 确定性 */
    int guard = 0;
    while (!c4_over && guard++ < 200) {
        if (c4_turn == 1) {
            int col = -1;
            for (int c = 0; c < C4_COLS; c++)
                if (c4_col_h[c] < C4_ROWS) { col = c; break; }
            CHECK(col >= 0, "player has a playable column on its turn");
            if (col < 0) break;
            c4_drop(col, 1);
        } else {
            int ac = c4_ai_best();
            CHECK(c4_col_h[ac] < C4_ROWS, "AI never picks a full column");
            c4_drop(ac, 2);
        }
        c4_check_end();
    }
    CHECK(c4_over, "full simulation terminates");
    CHECK(c4_winner == 1 || c4_winner == 2 || c4_winner == 0,
          "sane winner value");
    CHECK(c4_steps >= 7 && c4_steps <= C4_COLS * C4_ROWS,
          "steps within [7,42]");
    printf("  final: winner=%d steps=%d\n", c4_winner, c4_steps);
}

/* ---- 渲染冒烟: 黑子/白子/光标 XOR/结束全刷标志 ---- */
static void test_render(void) {
    clear_board();
    c4_board[5][0] = 1;                       /* 黑实心 */
    c4_board[4][1] = 2;                       /* 白空心 */
    c4_col_h[0] = 1;
    c4_col_h[1] = 1;
    c4_steps = 2;
    c4_col = 3;
    connect4_render();
    /* 黑子中心 (88,128) 黑 */
    CHECK(px(88, 128), "black stone center is black");
    /* 白子中心 (108,108) 白(空心), 环 (114,108) 黑 */
    CHECK(!px(108, 108), "white stone center is white (hollow)");
    CHECK(px(114, 108), "white stone ring is black");
    /* 光标列 3: 左框内缘 (139,50) 位于白底 → XOR 后黑 */
    CHECK(px(139, 50) && px(156, 100), "cursor border visible on white");
    /* 光标移到列 4 → 列 3 边框恢复白, 列 4 边框变黑 */
    c4_col = 4;
    connect4_render();
    CHECK(!px(139, 50), "old cursor erased after move");
    CHECK(px(159, 50), "new cursor drawn on col 4");
    /* 结束渲染: 置全刷标志 */
    c4_over = true;
    c4_over_full = false;
    c4_winner = 1;
    connect4_render();
    CHECK(c4_over_full, "over render sets force-full flag");
    /* HUD 结果: 黑字 "YOU WIN!" 起点 (2,2)('Y' 字形像素), 背景被清白 */
    CHECK(px(2, 2) && px(2, 3) && !px(52, 2), "HUD result text drawn");
    c4_over = false;
}

int main(void) {
    test_win();
    test_gravity();
    test_ai_block();
    test_ai_win();
    test_player_win();
    test_draw();
    test_keys();
    test_rng_guard();
    test_full_game();
    test_render();
    if (s_fail == 0) { printf("ALL CONNECT4 TESTS PASSED\n"); return 0; }
    printf("%d CONNECT4 TEST(S) FAILED\n", s_fail);
    return 1;
}
