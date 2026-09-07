/* CHESS 逻辑单测 — host 编译, 包含游戏 .c 直接访问静态状态
 * 覆盖: 开局布局/各棋子走法合法性/落子与撤销(升变)/吃王胜负/
 *       无子可动胜负/步数和棋/AI 吃王与吃大子/键路径/渲染冒烟 */
#include "../../src/config.h"
#include "../../src/games/chess.c"
#include <stdio.h>
#include <string.h>

static int s_fail = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); s_fail++; } \
    else { printf("ok: %s\n", msg); } \
} while (0)

/* host 框架 stub */
bool s_exit_request = false;
void game_set_tick_interval(uint32_t ms) { (void)ms; }

static void ch_clear(void) {
    memset(ch_board, 0, sizeof ch_board);
    ch_turn = 1;
    ch_winner = 0;
    ch_sel = false;
    ch_sx = ch_sy = 0;
    ch_over = false;
    ch_over_full = false;
    ch_plies = 0;
    ch_ai_at = 0;
}

static void ch_put(int x, int y, int p) { ch_board[y][x] = (uint8_t)p; }

/* ---- 开局 ---- */
static void test_setup(void) {
    ch_new();
    int w = 0, b = 0;
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++) {
            if (ch_board[y][x] == 0) continue;
            if (ch_color(ch_board[y][x]) == 1) w++; else b++;
        }
    CHECK(w == 16 && b == 16, "opening: 16 pieces each side");
    int rows_ok = 1;
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++) {
            int p = ch_board[y][x];
            if (p == 0) continue;
            if (ch_color(p) == 1 && y < 6) rows_ok = 0;
            if (ch_color(p) == 2 && y > 1) rows_ok = 0;
        }
    CHECK(rows_ok, "white in rows 6-7, black in rows 0-1");
    CHECK(ch_turn == 1 && !ch_over && ch_plies == 0, "player moves first");
    CHECK(ch_has_moves(1) && ch_has_moves(2), "both sides have moves");
    CHECK(ch_board[7][0] == CH_WR && ch_board[7][4] == CH_WK &&
          ch_board[6][0] == CH_WP && ch_board[6][7] == CH_WP,
          "white back rank spot check");
    CHECK(ch_board[0][0] == CH_BR && ch_board[0][4] == CH_BK &&
          ch_board[1][0] == CH_BP && ch_board[1][7] == CH_BP,
          "black back rank spot check");
    CHECK(ch_color(CH_WK) == 1 && ch_color(CH_BQ) == 2, "color mapping");
    CHECK(ch_captured(1) == 0 && ch_captured(2) == 0, "no captures at start");
    CHECK(ch_material() == 0, "material even at start");
}

/* ---- 兵 ---- */
static void test_pawn(void) {
    ch_clear();
    ch_put(4, 6, CH_WP);
    CHECK(ch_legal(4, 6, 4, 5), "pawn 1-step forward");
    CHECK(ch_legal(4, 6, 4, 4), "pawn 2-step from start row");
    CHECK(!ch_legal(4, 6, 3, 5), "pawn diagonal into empty illegal");
    ch_put(3, 5, CH_BP);
    CHECK(ch_legal(4, 6, 3, 5), "pawn diagonal capture ok");
    ch_put(4, 5, CH_BP);
    CHECK(!ch_legal(4, 6, 4, 5), "pawn 1-step blocked");
    CHECK(!ch_legal(4, 6, 4, 4), "pawn 2-step blocked");
    ch_put(4, 5, CH_WP);           /* 覆盖为白兵 */
    CHECK(!ch_legal(4, 6, 4, 5), "pawn cannot capture own piece");
    ch_clear();
    ch_put(4, 4, CH_WP);
    CHECK(!ch_legal(4, 4, 4, 2), "pawn 2-step only from start row");
    ch_clear();
    ch_put(1, 1, CH_BP);
    CHECK(ch_legal(1, 1, 1, 2), "black pawn moves down");
    CHECK(!ch_legal(1, 1, 1, 0), "black pawn cannot move up");
    ch_clear();
    ch_put(4, 6, CH_WP);
    ch_put(4, 4, CH_BP);
    CHECK(!ch_legal(4, 6, 4, 4), "2-step blocked by enemy at midpoint");
    ch_clear();
    ch_put(4, 1, CH_WP);
    CHECK(ch_legal(4, 1, 4, 0), "pawn reaches promotion row");
}

/* ---- 马/象/车/后/王 ---- */
static void test_pieces(void) {
    ch_clear();
    ch_put(4, 4, CH_WN);
    CHECK(ch_legal(4, 4, 6, 5) && ch_legal(4, 4, 2, 3) &&
          ch_legal(4, 4, 5, 6) && ch_legal(4, 4, 2, 5),
          "knight L-moves");
    CHECK(!ch_legal(4, 4, 5, 4) && !ch_legal(4, 4, 4, 5), "knight not straight");
    ch_put(6, 5, CH_WN);
    CHECK(!ch_legal(4, 4, 6, 5), "knight blocked by own piece");
    ch_clear();
    ch_put(4, 4, CH_WB);
    CHECK(ch_legal(4, 4, 7, 7) && ch_legal(4, 4, 1, 1) &&
          ch_legal(4, 4, 6, 2), "bishop slides diagonally");
    ch_put(6, 6, CH_BP);
    CHECK(ch_legal(4, 4, 6, 6), "bishop captures enemy");
    CHECK(!ch_legal(4, 4, 7, 7), "bishop blocked beyond enemy");
    ch_clear();
    ch_put(0, 0, CH_WR);
    CHECK(ch_legal(0, 0, 0, 7) && ch_legal(0, 0, 7, 0), "rook slides lines");
    CHECK(!ch_legal(0, 0, 1, 1), "rook not diagonal");
    ch_put(0, 3, CH_BP);
    CHECK(ch_legal(0, 0, 0, 3) && !ch_legal(0, 0, 0, 4), "rook blocked by enemy");
    ch_clear();
    ch_put(3, 3, CH_WQ);
    CHECK(ch_legal(3, 3, 0, 0) && ch_legal(3, 3, 3, 7) &&
          ch_legal(3, 3, 7, 3) && ch_legal(3, 3, 0, 3), "queen all lines");
    ch_clear();
    ch_put(4, 4, CH_WK);
    CHECK(ch_legal(4, 4, 4, 5) && ch_legal(4, 4, 3, 3) &&
          ch_legal(4, 4, 5, 3), "king 1-step all dirs");
    CHECK(!ch_legal(4, 4, 4, 6), "king only 1 step");
    ch_put(4, 5, CH_BK);
    CHECK(ch_legal(4, 4, 4, 5), "king captures enemy king");
}

/* ---- 落子/撤销/升变 ---- */
static void test_make_undo(void) {
    int captured = -1, promo = -1;
    ch_clear();
    ch_put(4, 6, CH_WP);
    int r = ch_make(4, 6, 4, 5, &captured, &promo);
    CHECK(r == 0 && captured == 0 && promo == 0, "plain move returns no capture");
    CHECK(ch_board[5][4] == CH_WP && ch_board[6][4] == 0, "pawn moved");
    ch_unmake(4, 6, 4, 5, captured, promo);
    CHECK(ch_board[6][4] == CH_WP && ch_board[5][4] == 0, "undo restores");
    ch_clear();
    ch_put(4, 6, CH_WP);
    ch_put(3, 5, CH_BP);
    captured = ch_make(4, 6, 3, 5, &captured, &promo);
    CHECK(captured == CH_BP && ch_board[5][3] == CH_WP && ch_board[5][4] == 0,
          "capture removes enemy");
    ch_unmake(4, 6, 3, 5, captured, promo);
    CHECK(ch_board[6][4] == CH_WP && ch_board[5][3] == CH_BP,
          "undo capture restores both");
    ch_clear();
    ch_put(4, 1, CH_WP);
    captured = ch_make(4, 1, 4, 0, &captured, &promo);
    CHECK(promo == 1 && ch_board[0][4] == CH_WQ, "white pawn auto-queens");
    ch_unmake(4, 1, 4, 0, captured, promo);
    CHECK(ch_board[1][4] == CH_WP && ch_board[0][4] == 0, "undo promotion");
    ch_clear();
    ch_put(0, 6, CH_BP);
    captured = ch_make(0, 6, 0, 7, &captured, &promo);
    CHECK(promo == 1 && ch_board[7][0] == CH_BQ, "black pawn auto-queens");
    ch_unmake(0, 6, 0, 7, captured, promo);
    CHECK(ch_board[6][0] == CH_BP && ch_board[7][0] == 0, "undo black promotion");
}

/* ---- 吃王胜负 / 无子可动 / 和棋 ---- */
static void test_endings(void) {
    /* 玩家吃 AI 王 → 玩家胜 */
    ch_clear();
    ch_put(4, 4, CH_WK);
    ch_put(4, 5, CH_BK);
    ch_sel = true; ch_sx = 4; ch_sy = 4; ch_cx = 4; ch_cy = 5;
    ch_handle_ok();
    CHECK(ch_over && ch_winner == 1, "player wins capturing king");
    CHECK(ch_board[5][4] == CH_WK && ch_board[4][4] == 0,
          "capturing king lands on its square");
    /* AI 吃玩家王 → AI 胜 */
    ch_clear();
    ch_put(0, 0, CH_BK);
    ch_put(0, 1, CH_WK);
    ch_turn = 2;
    ch_ai_at = 0;
    rng_seed(&ch_rng, 5u);
    ch_ai_turn();
    CHECK(ch_over && ch_winner == 2, "AI wins capturing king");
    CHECK(ch_board[1][0] == CH_BK && ch_board[0][0] == 0, "AI king landed");
    /* 黑王被己方棋子困死 → 玩家走一步即胜 */
    ch_clear();
    ch_put(7, 7, CH_BK);
    ch_put(7, 6, CH_BP);
    ch_put(6, 7, CH_BP);
    ch_put(6, 6, CH_BP);
    ch_put(4, 4, CH_WK);
    CHECK(!ch_has_moves(2), "boxed black king has no move");
    ch_sel = true; ch_sx = 4; ch_sy = 4; ch_cx = 5; ch_cy = 4;
    ch_handle_ok();
    CHECK(ch_over && ch_winner == 1, "player wins when AI has no moves");
    /* 白王被困死 → AI 走一步即胜 */
    ch_clear();
    ch_put(0, 0, CH_WK);
    ch_put(0, 1, CH_WP);
    ch_put(1, 0, CH_WP);
    ch_put(1, 1, CH_WP);
    ch_put(7, 7, CH_BK);
    CHECK(!ch_has_moves(1), "boxed white king has no move");
    ch_turn = 2;
    ch_ai_at = 0;
    rng_seed(&ch_rng, 9u);
    ch_ai_turn();
    CHECK(ch_over && ch_winner == 2, "AI wins when player has no moves");
    /* AI 无路 → 玩家胜 */
    ch_clear();
    ch_put(7, 7, CH_BK);
    ch_put(7, 6, CH_BP);
    ch_put(6, 7, CH_BP);
    ch_put(6, 6, CH_BP);
    ch_put(4, 4, CH_WK);
    ch_turn = 2;
    ch_ai_at = 0;
    rng_seed(&ch_rng, 3u);
    ch_ai_turn();
    CHECK(ch_over && ch_winner == 1, "AI concedes when it has no moves");
    /* 步数封顶 → 和棋 */
    ch_clear();
    ch_put(4, 4, CH_WK);
    ch_put(7, 7, CH_BK);
    ch_plies = CH_PLIES_MAX - 1;
    ch_sel = true; ch_sx = 4; ch_sy = 4; ch_cx = 5; ch_cy = 4;
    ch_handle_ok();
    CHECK(ch_over && ch_winner == 3, "draw at ply cap");
}

/* ---- AI 启发式 ---- */
static void test_ai_behavior(void) {
    /* AI 王吃玩家王(必选将杀) */
    ch_clear();
    ch_put(0, 0, CH_BK);
    ch_put(0, 1, CH_WK);
    ch_turn = 2;
    rng_seed(&ch_rng, 11u);
    ch_ai_at = 0;
    ch_ai_turn();
    CHECK(ch_over && ch_winner == 2, "AI picks king capture");
    /* AI 车吃皇后优先于马吃兵 */
    ch_clear();
    ch_put(0, 0, CH_BR);
    ch_put(2, 0, CH_BN);
    ch_put(0, 7, CH_WQ);
    ch_put(1, 2, CH_WP);
    ch_put(7, 7, CH_WK);
    ch_turn = 2;
    rng_seed(&ch_rng, 42u);
    ch_ai_at = 0;
    ch_ai_turn();
    CHECK(!ch_over, "AI capture keeps game going");
    int q_ok = 1;
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++)
            if (ch_board[y][x] == CH_WQ) q_ok = 0;
    CHECK(ch_board[7][0] == CH_BR && q_ok, "AI captured the free queen");
    /* 残局加深: 王在安全位时搜索完成, 回到玩家 */
    ch_clear();
    ch_put(4, 4, CH_BQ);
    ch_put(7, 0, CH_WK);
    ch_put(0, 7, CH_BK);
    ch_turn = 2;
    rng_seed(&ch_rng, 7u);
    ch_ai_at = 0;
    ch_ai_turn();
    CHECK(!ch_over && ch_turn == 1, "endgame search completes, back to player");
}

/* ---- 键路径 ---- */
static void test_key_flow(void) {
    ch_new();
    key_event_t ev;
    memset(&ev, 0, sizeof ev);
    /* OK 选中自家兵 */
    ev.key = K_OK;
    chess_on_key(&ev);
    CHECK(ch_sel && ch_sx == 4 && ch_sy == 6, "OK selects own pawn");
    /* 方向键移动 + 改选自家象 */
    ev.key = K_RIGHT; chess_on_key(&ev);
    ev.key = K_DOWN; chess_on_key(&ev);
    CHECK(ch_cx == 5 && ch_cy == 7, "cursor moved to (5,7)");
    ev.key = K_OK;
    chess_on_key(&ev);
    CHECK(ch_sel && ch_sx == 5 && ch_sy == 7, "OK on own piece reselects");
    ev.key = K_OK;
    chess_on_key(&ev);
    CHECK(!ch_sel, "OK on selected square deselects");
    /* 选中兵走两步 */
    ev.key = K_UP; chess_on_key(&ev);
    CHECK(ch_cx == 5 && ch_cy == 6, "cursor on (5,6) pawn");
    ev.key = K_OK; chess_on_key(&ev);
    CHECK(ch_sel && ch_sx == 5 && ch_sy == 6, "selected pawn");
    ev.key = K_UP; chess_on_key(&ev);
    ev.key = K_UP; chess_on_key(&ev);
    ev.key = K_OK; chess_on_key(&ev);
    CHECK(ch_board[4][5] == CH_WP && ch_board[6][5] == 0, "pawn advanced 2");
    CHECK(!ch_sel && ch_turn == 2, "turn passed to AI");
    /* tick 推进 AI 回合 */
    ch_ai_at = 0;
    chess_tick(0);
    CHECK(ch_turn == 1 && !ch_over, "AI moved on tick");
    /* AI 回合未到点: OK 忽略 */
    ch_clear();
    ch_put(0, 0, CH_BK);
    ch_put(7, 7, CH_WK);
    ch_turn = 2;
    ch_ai_at = now_ms() + 10000;
    ev.key = K_OK;
    chess_on_key(&ev);
    CHECK(ch_turn == 2, "OK ignored during AI turn (before deadline)");
    /* 到点后按键推进 AI, 本键随后处理 */
    ch_ai_at = 0;
    ev.key = K_LEFT;
    chess_on_key(&ev);
    CHECK(ch_turn == 1, "key after deadline advances AI");
    /* 非法目标保持选择 */
    ch_clear();
    ch_put(4, 6, CH_WP);
    ch_sel = true; ch_sx = 4; ch_sy = 6; ch_cx = 3; ch_cy = 5;
    ev.key = K_OK;
    chess_on_key(&ev);
    CHECK(ch_sel && ch_board[5][3] == 0, "illegal move keeps selection");
    /* 重复 OK 忽略 */
    ch_clear();
    ch_put(4, 6, CH_WP);
    ch_cx = 4; ch_cy = 6;
    ev.is_repeat = true;
    ev.key = K_OK;
    chess_on_key(&ev);
    CHECK(!ch_sel, "repeat OK ignored");
    ev.is_repeat = false;
    ev.key = K_OK;
    chess_on_key(&ev);
    CHECK(ch_sel, "non-repeat OK selects");
    ev.key = K_OK;
    chess_on_key(&ev);
    CHECK(!ch_sel, "OK deselects");
    /* 'n' 重开 */
    ev.key = K_CHAR; ev.ch = 'n';
    chess_on_key(&ev);
    CHECK(ch_piece_count() == 32 && !ch_over, "'n' restarts game");
    /* Q 退出 */
    s_exit_request = false;
    ev.key = K_QUIT;
    chess_on_key(&ev);
    CHECK(s_exit_request, "Q exits to menu");
    s_exit_request = false;
    /* WASD 等效 */
    ch_new();
    ev.key = K_CHAR; ev.ch = 'w'; chess_on_key(&ev);
    CHECK(ch_cy == 5, "W moves up");
    ev.ch = 's'; chess_on_key(&ev);
    CHECK(ch_cy == 6, "S moves down");
    ev.ch = 'a'; chess_on_key(&ev);
    CHECK(ch_cx == 3, "A moves left");
    ev.ch = 'd'; chess_on_key(&ev);
    CHECK(ch_cx == 4, "D moves right");
    /* 方向键重复可响应 */
    ch_new();
    ev.key = K_LEFT;
    ev.is_repeat = true;
    chess_on_key(&ev);
    CHECK(ch_cx == 3, "repeat direction moves cursor");
    ev.is_repeat = false;
}

/* ---- 结束态按键 ---- */
static void test_over_keys(void) {
    ch_new();
    key_event_t ev;
    memset(&ev, 0, sizeof ev);
    ch_over = true; ch_winner = 2;
    ev.key = K_OK;
    chess_on_key(&ev);
    CHECK(!ch_over && ch_piece_count() == 32, "OK in over state retries");
    ch_over = true; ch_winner = 1;
    s_exit_request = false;
    ev.key = K_BACK;
    chess_on_key(&ev);
    CHECK(s_exit_request, "BACK in over state exits");
    s_exit_request = false;
    ev.is_repeat = true;
    ev.key = K_OK;
    chess_on_key(&ev);
    CHECK(ch_over, "repeat OK in over state ignored");
    ev.is_repeat = false;
}

/* ---- 渲染冒烟: 不越界不崩溃 ---- */
static void test_render_smoke(void) {
    ch_new();
    chess_render();
    int black = 0;
    for (unsigned i = 0; i < CCG_FRAME_BYTES; i++)
        for (int b = 0; b < 8; b++)
            if (g_fb[i] & (1u << b)) black++;
    CHECK(black > 800, "render draws board+pieces (many black px)");
    ch_sel = true; ch_sx = 4; ch_sy = 6;
    chess_render();
    ch_sel = false;
    ch_over = true; ch_winner = 2;
    chess_render();
    ch_over = false; ch_over_full = false;
    ch_turn = 2;
    ch_ai_at = now_ms() + 1000;
    chess_render();
    ch_turn = 1;
}

int main(void) {
    test_setup();
    test_pawn();
    test_pieces();
    test_make_undo();
    test_endings();
    test_ai_behavior();
    test_key_flow();
    test_over_keys();
    test_render_smoke();
    if (s_fail == 0) printf("ALL PASS\n");
    else printf("%d FAILURE(S)\n", s_fail);
    return s_fail ? 1 : 0;
}
