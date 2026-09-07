/* CHECKERS 逻辑单测 — host 编译, 包含游戏 .c 直接访问静态状态
 * 覆盖: 开局布局/兵移动/跳吃/升王/王滑动与吃/胜负(无路/无子/和)/AI 贪心/键路径/渲染冒烟 */
#include "../../src/config.h"
#include "../../src/games/checkers.c"
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

static void ck_clear(void) {
    memset(ck_board, 0, sizeof ck_board);
    ck_turn = CK_P1;
    ck_winner = 0;
    ck_sel = false;
    ck_sx = ck_sy = 0;
    ck_over = false;
    ck_over_full = false;
    ck_moves_since_cap = 0;
    ck_ai_at = 0;
}

static void ck_put(int x, int y, int p) {
    ck_board[y][x] = (uint8_t)p;
}

/* ---- 开局 ---- */
static void test_setup(void) {
    ck_new();
    CHECK(ck_piece_count(CK_P1) == 12 && ck_piece_count(CK_P2) == 12,
          "opening: 12 pieces each side");
    CHECK(ck_turn == CK_P1 && !ck_over, "player moves first");
    int dark_ok = 1, rows_ok = 1;
    for (int y = 0; y < CK_N; y++)
        for (int x = 0; x < CK_N; x++) {
            int p = ck_board[y][x];
            if (p == 0) continue;
            if (((x + y) & 1) == 0) dark_ok = 0;
            if (p == CK_P1 && y > 2) rows_ok = 0;
            if (p == CK_P2 && y < 5) rows_ok = 0;
        }
    CHECK(dark_ok, "pieces only on dark squares");
    CHECK(rows_ok, "player top 3 rows, AI bottom 3 rows");
    CHECK(ck_has_move(CK_P1) && ck_has_move(CK_P2), "both sides have moves");
    CHECK(ck_board[1][0] == CK_P1 && ck_board[6][7] == CK_P2, "spot-check pieces");
}

/* ---- 兵移动规则 ---- */
static void test_man_move(void) {
    ck_clear();
    ck_put(1, 2, 1);
    int mx = -1, my = -1;
    CHECK(ck_move_kind(1, 2, 0, 3, &mx, &my) == 1, "man forward-left ok");
    CHECK(ck_move_kind(1, 2, 2, 3, &mx, &my) == 1, "man forward-right ok");
    CHECK(ck_move_kind(1, 2, 0, 1, &mx, &my) == 0, "man cannot move backward");
    CHECK(ck_move_kind(1, 2, 2, 1, &mx, &my) == 0, "man cannot move backward 2");
    CHECK(ck_move_kind(1, 2, 2, 2, &mx, &my) == 0, "man cannot move horizontally");
    CHECK(ck_move_kind(1, 2, 1, 3, &mx, &my) == 0, "man cannot move straight");
    ck_put(0, 3, 2);
    CHECK(ck_move_kind(1, 2, 0, 3, &mx, &my) == 0, "blocked forward move illegal");
    ck_put(0, 3, 1);
    CHECK(ck_move_kind(1, 2, 0, 3, &mx, &my) == 0, "own piece blocks move");
    ck_clear();
    ck_put(6, 5, 2);
    CHECK(ck_move_kind(6, 5, 7, 4, &mx, &my) == 1, "AI man moves up-forward");
    CHECK(ck_move_kind(6, 5, 5, 6, &mx, &my) == 0, "AI man cannot move down");
}

/* ---- 跳吃 ---- */
static void test_capture(void) {
    ck_clear();
    ck_put(2, 3, 1);
    ck_put(3, 4, 2);
    int mx = -1, my = -1;
    CHECK(ck_move_kind(2, 3, 4, 5, &mx, &my) == 2, "jump over enemy is capture");
    CHECK(mx == 3 && my == 4, "captured square reported");
    ck_apply(2, 3, 4, 5);
    CHECK(ck_board[5][4] == CK_P1 && ck_board[4][3] == 0,
          "capture removes enemy and lands");
    CHECK(ck_moves_since_cap == 0, "capture resets no-capture counter");
    ck_clear();
    ck_put(2, 3, 1);
    CHECK(ck_move_kind(2, 3, 4, 5, &mx, &my) == 0, "no jump without enemy");
    ck_clear();
    ck_put(2, 3, 1);
    ck_put(3, 4, 1);
    CHECK(ck_move_kind(2, 3, 4, 5, &mx, &my) == 0, "cannot jump own piece");
    ck_clear();
    ck_put(7, 6, 2);
    ck_put(6, 5, 1);
    CHECK(ck_move_kind(7, 6, 5, 4, &mx, &my) == 2 && mx == 6 && my == 5,
          "AI capture up-left");
    ck_clear();
    ck_put(0, 1, 1);
    ck_put(1, 2, 2);
    CHECK(ck_move_kind(0, 1, 2, 3, &mx, &my) == 2 && mx == 1 && my == 2,
          "capture near left edge");
    ck_clear();
    ck_put(2, 3, 1);
    ck_put(3, 4, 2);
    ck_apply(2, 3, 4, 5);
    ck_moves_since_cap = 5;
    ck_apply(4, 5, 3, 6);      /* 普通移动 */
    CHECK(ck_moves_since_cap == 6, "non-capture move increments counter");
}

/* ---- 升王与王规则 ---- */
static void test_king(void) {
    ck_clear();
    ck_put(1, 6, CK_P1);
    ck_apply(1, 6, 2, 7);
    CHECK(ck_board[7][2] == CK_K1, "player man reaching last row promotes");
    ck_clear();
    ck_put(0, 1, CK_P2);
    ck_apply(0, 1, 1, 0);
    CHECK(ck_board[0][1] == CK_K2, "AI man reaching top row promotes");
    ck_clear();
    ck_put(2, 3, CK_K1);
    int mx = -1, my = -1;
    CHECK(ck_move_kind(2, 3, 5, 6, &mx, &my) == 1, "king slides any distance");
    CHECK(ck_move_kind(2, 3, 3, 4, &mx, &my) == 1, "king single step ok");
    ck_put(3, 4, CK_P1);
    CHECK(ck_move_kind(2, 3, 5, 6, &mx, &my) == 0, "king blocked by own piece");
    CHECK(ck_move_kind(2, 3, 3, 4, &mx, &my) == 0, "king cannot land on own piece");
    ck_clear();
    ck_put(2, 3, CK_K1);
    ck_put(4, 5, CK_P2);
    CHECK(ck_move_kind(2, 3, 5, 6, &mx, &my) == 2 && mx == 4 && my == 5,
          "king capture landing just beyond");
    CHECK(ck_move_kind(2, 3, 6, 7, &mx, &my) == 2 && mx == 4 && my == 5,
          "king capture landing far beyond");
    ck_put(5, 6, CK_P2);
    CHECK(ck_move_kind(2, 3, 6, 7, &mx, &my) == 0, "king cannot jump two enemies");
    ck_clear();
    ck_put(2, 3, CK_K1);
    ck_put(3, 4, CK_P1);
    ck_put(4, 5, CK_P2);
    CHECK(ck_move_kind(2, 3, 5, 6, &mx, &my) == 0,
          "king cannot jump over own piece first");
    ck_clear();
    ck_put(5, 4, CK_K2);
    CHECK(ck_move_kind(5, 4, 2, 1, &mx, &my) == 1, "AI king moves any direction");
    ck_clear();
    ck_put(2, 3, CK_K1);
    ck_put(4, 5, CK_P2);
    ck_apply(2, 3, 5, 6);
    CHECK(ck_board[5][4] == 0 && ck_board[6][5] == CK_K1,
          "king capture via apply removes enemy");
}

/* ---- 胜负判定 ---- */
static void test_endings(void) {
    /* 玩家落子后 AI 无路 → 玩家胜 */
    ck_clear();
    ck_put(2, 5, CK_P1);
    ck_put(1, 0, CK_P2);             /* 顶行 AI 兵: 只能向上 → 无路 */
    CHECK(!ck_has_move(CK_P2), "cornered AI man has no move");
    ck_sel = true; ck_sx = 2; ck_sy = 5; ck_cx = 3; ck_cy = 6;
    ck_handle_ok();
    CHECK(ck_over && ck_winner == CK_P1, "player wins when AI has no move");
    CHECK(ck_board[6][3] == CK_P1, "player piece landed");
    /* AI 无子 → 玩家胜 */
    ck_clear();
    ck_put(2, 5, CK_P1);
    ck_sel = true; ck_sx = 2; ck_sy = 5; ck_cx = 3; ck_cy = 6;
    ck_handle_ok();
    CHECK(ck_over && ck_winner == CK_P1, "player wins when AI has no pieces");
    /* AI 走子后玩家无路 → AI 胜(底行兵无前进方向) */
    ck_clear();
    ck_put(0, 7, CK_P1);         /* 底行兵: 目标全出界, 永无路 */
    ck_put(4, 5, CK_P2);
    CHECK(!ck_has_move(CK_P1), "bottom-row man has no move");
    ck_turn = CK_P2;
    rng_seed(&ck_rng, 11u);
    ck_ai_turn();
    CHECK(ck_over && ck_winner == CK_P2, "AI wins when player has no move");
    /* 和棋: 无吃步数达限 */
    ck_clear();
    ck_put(2, 5, CK_P1);
    ck_put(5, 2, CK_P2);
    ck_moves_since_cap = CK_DRAW_LIMIT - 1;
    ck_sel = true; ck_sx = 2; ck_sy = 5; ck_cx = 3; ck_cy = 6;
    ck_handle_ok();
    CHECK(ck_over && ck_winner == 3, "draw after no-capture limit");
}

/* ---- AI 启发式 ---- */
static void test_ai_behavior(void) {
    /* 有跳吃必吃(贪心) */
    ck_clear();
    ck_put(5, 2, CK_P2);
    ck_put(6, 1, CK_P1);
    ck_put(3, 5, CK_P2);
    rng_seed(&ck_rng, 42u);
    ck_turn = CK_P2;
    CHECK(ck_ai_move(), "AI has a move");
    CHECK(ck_board[0][7] == CK_K2 && ck_board[1][6] == 0,
          "AI chose the capture (promoting on last row), enemy removed");
    CHECK(ck_board[2][5] == 0, "AI left its origin");
    /* AI 升王优先 */
    ck_clear();
    ck_put(0, 1, CK_P2);
    rng_seed(&ck_rng, 7u);
    CHECK(ck_ai_move(), "AI moves toward promotion");
    int king = 0;
    for (int x = 0; x < CK_N; x++)
        if (ck_board[0][x] == CK_K2) king = 1;
    CHECK(king, "AI man promoted to king on top row");
    /* 王 AI 可吃远子 */
    ck_clear();
    ck_put(5, 4, CK_K2);
    ck_put(2, 1, CK_P1);
    rng_seed(&ck_rng, 3u);
    CHECK(ck_ai_move(), "AI king moves");
    CHECK(ck_board[0][1] == CK_K2 && ck_board[1][2] == 0,
          "AI king captured distant piece");
}

/* ---- 键路径 ---- */
static void test_key_flow(void) {
    ck_new();
    key_event_t ev;
    memset(&ev, 0, sizeof ev);
    /* OK 选中自家棋子 */
    ev.key = K_OK;
    checkers_on_key(&ev);
    CHECK(ck_sel && ck_sx == 1 && ck_sy == 0, "OK selects own piece");
    /* 方向键移动 + OK 改选(2,1)上已有己方棋子 */
    ev.key = K_RIGHT; checkers_on_key(&ev);
    ev.key = K_DOWN; checkers_on_key(&ev);
    CHECK(ck_cx == 2 && ck_cy == 1, "cursor moved to (2,1)");
    ev.key = K_OK;
    checkers_on_key(&ev);
    CHECK(ck_sel && ck_sx == 2 && ck_sy == 1, "OK on own piece re-selects");
    /* 同格再 OK 取消选择 */
    ev.key = K_OK;
    checkers_on_key(&ev);
    CHECK(!ck_sel, "OK on selected square deselects");
    /* 选中 (3,2) 并落子到 (4,3) */
    ev.key = K_RIGHT; checkers_on_key(&ev);
    ev.key = K_DOWN; checkers_on_key(&ev);
    ev.key = K_OK; checkers_on_key(&ev);
    CHECK(ck_sel && ck_sx == 3 && ck_sy == 2, "OK selects (3,2)");
    ev.key = K_RIGHT; checkers_on_key(&ev);
    ev.key = K_DOWN; checkers_on_key(&ev);
    ev.key = K_OK; checkers_on_key(&ev);
    CHECK(ck_board[3][4] == CK_P1 && ck_board[2][3] == 0, "player moved to (4,3)");
    CHECK(!ck_sel && ck_turn == CK_P2, "turn passed to AI");
    /* tick 推进 AI 回合 */
    ck_ai_at = 0;
    checkers_tick(0);
    CHECK(ck_turn == CK_P1 && !ck_over, "AI moved on tick, back to player");
    /* AI 回合未到点: OK 忽略 */
    ck_clear();
    ck_put(2, 5, CK_P1);
    ck_put(5, 2, CK_P2);
    ck_put(6, 1, CK_P1);         /* AI 唯一好棋: 吃 (6,1) 至 (7,0) 升王 */
    ck_turn = CK_P2;
    ck_ai_at = now_ms() + 10000;
    ev.key = K_OK;
    checkers_on_key(&ev);
    CHECK(ck_turn == CK_P2, "OK ignored during AI turn (before deadline)");
    /* 到点后按键推进 AI, 本键随后处理 */
    ck_ai_at = 0;
    ev.key = K_LEFT;
    checkers_on_key(&ev);
    CHECK(ck_turn == CK_P1, "key after deadline advances AI");
    CHECK(ck_board[0][7] == CK_K2 && ck_board[1][6] == 0, "AI captured on key");
    CHECK(ck_cx == 6 && ck_cy == 0, "cursor lands at AI square, then key moves it");
    /* 重复的 OK 忽略 */
    ck_clear();
    ck_put(1, 2, CK_P1);
    ev.is_repeat = true;
    ev.key = K_OK;
    checkers_on_key(&ev);
    CHECK(!ck_sel, "repeat OK ignored");
    ev.is_repeat = false;
    /* 光标到该子, 再 OK 选中 */
    ck_cx = 1; ck_cy = 2;
    checkers_on_key(&ev);
    CHECK(ck_sel, "non-repeat OK selects");
    /* 非法目标保持选择 */
    ck_sel = true; ck_sx = 1; ck_sy = 2;
    ev.key = K_DOWN; checkers_on_key(&ev);   /* 光标到 (1,3): 直线移动非法 */
    ev.key = K_OK; checkers_on_key(&ev);
    CHECK(ck_sel && ck_board[3][1] == 0, "illegal move keeps selection");
    /* 'n' 重开 */
    ck_clear();
    ev.key = K_CHAR; ev.ch = 'n';
    checkers_on_key(&ev);
    CHECK(ck_piece_count(CK_P1) == 12 && !ck_over, "'n' restarts game");
    /* Q 退出 */
    s_exit_request = false;
    ev.key = K_QUIT;
    checkers_on_key(&ev);
    CHECK(s_exit_request, "Q exits to menu");
    s_exit_request = false;
    /* WASD 等效 */
    ck_new();
    ev.key = K_CHAR; ev.ch = 'w'; checkers_on_key(&ev);
    CHECK(ck_cy == 0, "W clamps at top row");
    ev.ch = 's'; checkers_on_key(&ev);
    CHECK(ck_cy == 1, "S moves down");
    ev.ch = 'a'; checkers_on_key(&ev);
    CHECK(ck_cx == 0, "A moves left");
    ev.ch = 'd'; checkers_on_key(&ev);
    CHECK(ck_cx == 1, "D moves right");
}

/* ---- 结束态按键 ---- */
static void test_over_keys(void) {
    ck_new();
    key_event_t ev;
    memset(&ev, 0, sizeof ev);
    ck_over = true; ck_winner = CK_P2;
    ev.key = K_OK;
    checkers_on_key(&ev);
    CHECK(!ck_over && ck_piece_count(CK_P1) == 12, "OK in over state retries");
    ck_over = true; ck_winner = CK_P1;
    s_exit_request = false;
    ev.key = K_BACK;
    checkers_on_key(&ev);
    CHECK(s_exit_request, "BACK in over state exits");
    s_exit_request = false;
    ev.is_repeat = true;
    ev.key = K_OK;
    checkers_on_key(&ev);
    CHECK(ck_over, "repeat OK in over state ignored");
    ev.is_repeat = false;
}

/* ---- 渲染冒烟: 不越界不崩溃 ---- */
static void test_render_smoke(void) {
    ck_new();
    checkers_render();
    int black = 0;
    for (unsigned i = 0; i < CCG_FRAME_BYTES; i++)
        for (int b = 0; b < 8; b++)
            if (g_fb[i] & (1u << b)) black++;
    CHECK(black > 2000, "render draws board+pieces (many black px)");
    ck_sel = true; ck_sx = 1; ck_sy = 0;
    checkers_render();
    ck_sel = false;
    ck_over = true; ck_winner = 2;
    checkers_render();
    ck_over = false; ck_over_full = false;
}

int main(void) {
    test_setup();
    test_man_move();
    test_capture();
    test_king();
    test_endings();
    test_ai_behavior();
    test_key_flow();
    test_over_keys();
    test_render_smoke();
    if (s_fail == 0) printf("ALL PASS\n");
    else printf("%d FAILURE(S)\n", s_fail);
    return s_fail ? 1 : 0;
}
