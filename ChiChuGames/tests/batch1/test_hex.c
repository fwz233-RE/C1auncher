/* HEX 逻辑单测 — host, 直接包含游戏源文件(静态状态可访问)
 * 链接 canvas/font/font_data/pattern/rng/time/ui_common/input/display (-DCHICHU_HOST) */
#include "../../src/config.h"
#include "../../src/gfx/canvas.h"
#include "../../src/games/hex.c"
#include <stdio.h>

static int s_fail = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); s_fail++; } \
    else { printf("ok: %s\n", msg); } \
} while (0)

/* ---- host 框架 stub ---- */
bool s_exit_request = false;
void game_set_tick_interval(uint32_t ms) { (void)ms; }

static bool px(int x, int y) {
    if (x < 0 || x >= (int)CCG_W || y < 0 || y >= (int)CCG_H) return false;
    int off = (y >> 3) * (int)CCG_W + x;
    return (g_fb[off] & (0x80 >> (y & 7))) != 0;
}

/* ---- 开局状态 ---- */
static void test_new(void) {
    hex_enter();
    CHECK(hx_cx == 5 && hx_cy == 5, "cursor at center");
    CHECK(hx_turn == 1 && !hx_ai_pending, "player moves first");
    CHECK(hx_moves == 0 && hx_lastr == -1, "empty board, no last move");
    CHECK(!hx_over && hx_winner == 0, "not over at start");
    int n = 0;
    for (int i = 0; i < HX_CELLS; i++) if (hx_board[i / HX_N][i % HX_N]) n++;
    CHECK(n == 0, "all 121 cells empty");
}

/* ---- 六邻接对称性与边界 ---- */
static void test_adjacency(void) {
    int cnt = 0;
    for (int r = 0; r < HX_N; r++)
        for (int c = 0; c < HX_N; c++) {
            const int8_t (*nb)[2] = hx_nb[r & 1];
            for (int k = 0; k < 6; k++) {
                int nr = r + nb[k][0], nc = c + nb[k][1];
                if (nr < 0 || nr >= HX_N || nc < 0 || nc >= HX_N) continue;
                cnt++;
                /* 反向邻接必须存在于邻居自己的表中 */
                bool found = false;
                const int8_t (*nb2)[2] = hx_nb[nr & 1];
                for (int k2 = 0; k2 < 6; k2++)
                    if (nr + nb2[k2][0] == r && nc + nb2[k2][1] == c) found = true;
                CHECK(found, "adjacency is symmetric");
            }
        }
    /* 内部格应有 6 邻, 角格应满足该行几何(偶数行角: 2-3 个) */
    int m = 0;
    for (int k = 0; k < 6; k++) {
        int nr = 5 + hx_nb[5 & 1][k][0], nc = 5 + hx_nb[5 & 1][k][1];
        if (nr >= 0 && nr < HX_N && nc >= 0 && nc < HX_N) m++;
    }
    CHECK(m == 6, "interior cell has 6 neighbors");
    CHECK(cnt > 0, "board has adjacency edges");
}

/* ---- 胜负判定 ---- */
static void test_win_check(void) {
    hex_enter();
    CHECK(!hx_check_win(5, 5), "single empty stone no win");
    /* 红整行连成 L-R 胜 */
    for (int c = 0; c < HX_N; c++) hx_board[5][c] = 1;
    CHECK(hx_check_win(5, 0), "red row touches left edge");
    CHECK(hx_check_win(5, 10), "red row touches right edge");
    CHECK(hx_check_win(5, 5), "red row connected");
    hex_enter();
    /* 红仅靠左边缘一串, 不构成连通 */
    hx_board[0][0] = hx_board[0][1] = hx_board[1][0] = 1;
    CHECK(!hx_check_win(0, 0), "left-only red cluster no win");
    hex_enter();
    /* 蓝整列连成 T-B 胜 */
    for (int r = 0; r < HX_N; r++) hx_board[r][5] = 2;
    CHECK(hx_check_win(0, 5), "blue column wins top-bottom");
    CHECK(hx_check_win(10, 5), "blue column wins from bottom");
    hex_enter();
    /* 斜对(0,0)与(1,1)不是六邻接: 不互邻且不连通 */
    hx_board[0][0] = 1;
    hx_board[1][1] = 1;
    bool n1 = false, n2 = false;
    for (int k = 0; k < 6; k++)
        if (1 + hx_nb[1 & 1][k][0] == 0 && 1 + hx_nb[1 & 1][k][1] == 0) n2 = true;
    for (int k = 0; k < 6; k++)
        if (0 + hx_nb[0 & 1][k][0] == 1 && 0 + hx_nb[0 & 1][k][1] == 1) n1 = true;
    CHECK(!n1 && !n2, "diagonal (0,0)-(1,1) not adjacent in hex topology");
    CHECK(!hx_check_win(0, 0), "two diagonal red stones no win");
}

/* ---- 玩家落子经 on_key + 回合流转 ---- */
static void test_player_move(void) {
    hex_enter();
    hx_cx = 3; hx_cy = 4;
    key_event_t ev = { K_OK, 0, false };
    hex_on_key(&ev);
    CHECK(hx_board[4][3] == 1, "OK places red stone");
    CHECK(hx_moves == 1 && hx_lastr == 4 && hx_lastc == 3 && hx_lastp == 1,
          "move counter + last move recorded");
    CHECK(hx_turn == 2 && hx_ai_pending, "player move hands turn to AI (pending)");
    CHECK(!hx_over, "no immediate win");
    /* AI 落子经 tick */
    hex_tick(0);
    CHECK(!hx_ai_pending && hx_turn == 1, "tick executes AI move, turn back to player");
    int blue = 0;
    for (int i = 0; i < HX_CELLS; i++) if (hx_board[i / HX_N][i % HX_N] == 2) blue++;
    CHECK(blue == 1 && hx_moves == 2, "exactly one blue stone after AI move");
    /* 重复 OK 忽略 */
    ev.is_repeat = true;
    ev.key = K_OK;
    hx_cx = 6; hx_cy = 4;
    hex_on_key(&ev);
    CHECK(hx_moves == 2, "repeated OK ignored");
    /* AI 回合中 OK 无效 */
    ev.is_repeat = false;
    hx_turn = 2;
    hx_cx = 6; hx_cy = 4;
    hex_on_key(&ev);
    CHECK(hx_moves == 2, "OK ignored during AI turn");
    /* 方向键响应重复 */
    ev.key = K_LEFT; ev.is_repeat = true;
    hex_on_key(&ev);
    CHECK(hx_cx == 5, "repeated LEFT moves cursor");
    ev.key = K_CHAR; ev.ch = 'd'; ev.is_repeat = true;
    hx_cx = 5;
    hex_on_key(&ev);
    CHECK(hx_cx == 5, "repeated letter ignored");
}

/* ---- AI: 一步取胜 ---- */
static void test_ai_win(void) {
    hex_enter();
    for (int r = 0; r < HX_N - 1; r++) hx_board[r][5] = 2;
    hx_turn = 2;
    hx_ai_pending = true;
    hex_tick(0);
    CHECK(hx_board[10][5] == 2, "AI completes top-bottom column");
    CHECK(hx_over && hx_winner == 2, "AI wins");
    CHECK(hx_winpath[10 * HX_N + 5] && hx_winpath[0 * HX_N + 5],
          "win path marked for highlight");
}

/* ---- AI: 堵玩家一步胜 ---- */
static void test_ai_block(void) {
    hex_enter();
    for (int c = 0; c < HX_N - 1; c++) hx_board[5][c] = 1;
    hx_turn = 2;
    hx_ai_pending = true;
    hex_tick(0);
    CHECK(hx_board[5][10] == 2, "AI blocks red's winning cell");
    CHECK(!hx_over && hx_winner == 0, "game continues after block");
}

/* ---- AI: 自己赢优先于堵 ---- */
static void test_ai_prefers_win(void) {
    hex_enter();
    for (int r = 0; r < HX_N - 1; r++) hx_board[r][5] = 2;
    for (int c = 0; c < HX_N - 1; c++) if (c != 5) hx_board[8][c] = 1; /* 红行留出蓝列 */
    hx_turn = 2;
    hx_ai_pending = true;
    hex_tick(0);
    CHECK(hx_board[10][5] == 2 && hx_over && hx_winner == 2,
          "AI takes own win over blocking");
}

/* ---- AI: 启发式落子(贴己方子 + 中心进度) ---- */
static void test_ai_heuristic(void) {
    hex_enter();
    hx_board[0][5] = 2;
    hx_turn = 2;
    hx_ai_pending = true;
    hex_tick(0);
    CHECK(hx_board[1][5] == 2, "AI extends from own stone down the center");
    CHECK(!hx_over, "heuristic move is not a win");
    /* 玩家 4 连威胁: AI 必须在红行端头附近落子 */
    hex_enter();
    for (int c = 0; c < 4; c++) hx_board[5][c] = 1;
    hx_turn = 2;
    hx_ai_pending = true;
    hex_tick(0);
    CHECK(hx_board[4][3] == 2 || hx_board[6][3] == 2 || hx_board[5][4] == 2 ||
          hx_board[4][4] == 2 || hx_board[6][4] == 2,
          "AI answers a 4-row threat at its tip");
}

/* ---- 玩家五连被堵后由启发式继续, 满盘防御分支 ---- */
static void test_draw_branch(void) {
    hex_enter();
    hx_board[0][0] = 1;
    hx_moves = HX_CELLS - 1;
    hx_commit(5, 5, 1);
    CHECK(hx_over && hx_winner == 0, "defensive draw branch when board full");
}

/* ---- 终局按键 ---- */
static void test_over_keys(void) {
    hex_enter();
    hx_over = true; hx_winner = 1; hx_over_full = false;
    key_event_t ev = { K_BACK, 0, false };
    s_exit_request = false;
    hex_on_key(&ev);
    CHECK(s_exit_request, "BACK at game over quits");
    s_exit_request = false;
    hx_over = true; hx_winner = 1;
    ev.key = K_OK;
    hex_on_key(&ev);
    CHECK(!hx_over && hx_moves == 0 && hx_winner == 0, "OK at game over restarts");
    hx_over = true; hx_winner = 2;
    ev.key = K_CHAR; ev.ch = 'n';
    hex_on_key(&ev);
    CHECK(!hx_over && hx_moves == 0, "N at game over restarts");
    /* N 在游戏中重开 */
    hx_board[3][3] = 1; hx_moves = 1;
    ev.key = K_CHAR; ev.ch = 'n';
    hex_on_key(&ev);
    CHECK(hx_moves == 0 && hx_board[3][3] == 0, "N mid-game restarts");
}

/* ---- 渲染: 布局/格子/棋子/光标反白/终局 HUD ---- */
static void test_render(void) {
    CHECK(HX_OX == 70 && HX_OY == 44, "board origin constants");
    CHECK(HX_OX + 10 * HX_CW + 2 * HX_HALF <= (int)CCG_W, "board fits width");
    CHECK(HX_OY + 10 * HX_HALF + 2 * HX_HALF <= (int)CCG_H, "board fits height");
    hex_enter();
    hex_render();
    CHECK(px(0, 0), "HUD title HEX renders");
    CHECK(px(0, CCG_HUD_H - 1), "HUD separator line");
    /* 空格 (0,0): 菱形 1px 轮廓, 内部白 */
    int cx0 = HX_OX + HX_HALF, cy0 = HX_OY + HX_HALF;
    CHECK(px(cx0, cy0 - HX_HALF), "cell (0,0) top tip black outline");
    CHECK(!px(cx0, cy0 - 2), "cell (0,0) interior white");
    /* 光标 (5,5): 反白格 = 黑底 + 白环(环在 |dx|+|dy|==6) */
    int ccx = HX_OX + HX_HALF + 5 * HX_CW + HX_HALF;
    int ccy = HX_OY + HX_HALF + 5 * HX_HALF;
    CHECK(px(ccx, ccy), "cursor cell black-filled");
    CHECK(!px(ccx + 6, ccy), "cursor ring pixel white");
    CHECK(!px(ccx + 8, ccy), "outside cursor diamond white");
    CHECK(!px(ccx, ccy + 6), "cursor bottom ring pixel white");
    /* 落红子后移开光标: 实心菱形 + 白心 */
    hx_board[5][5] = 1;
    hx_lastr = 5; hx_lastc = 5; hx_lastp = 1;
    hx_cx = 0; hx_cy = 0;
    hex_render();
    CHECK(!px(ccx, ccy), "player stone center is white dot");
    CHECK(px(ccx + 4, ccy), "player stone body black");
    /* 蓝子: 空心 + 黑心 */
    hx_board[5][5] = 0;
    hx_board[4][5] = 2;
    hx_lastr = 4; hx_lastc = 5; hx_lastp = 2;
    hex_render();
    int bx = HX_OX + HX_HALF + 5 * HX_CW, by = HX_OY + HX_HALF + 4 * HX_HALF;
    CHECK(px(bx, by - 7), "AI stone outline black");
    CHECK(!px(bx, by - 5), "AI stone hollow interior white");
    CHECK(px(bx, by), "AI stone center dot black");
    /* 终局: HUD 清空 + 结果文本 + 强制全刷一次 */
    hx_over = true; hx_winner = 1; hx_over_full = false;
    hex_render();
    CHECK(!px(0, 0), "over state clears HUD title");
    int ink = 0;
    for (int y = 2; y < 10; y++)
        for (int x = 2; x < 42; x++)
            if (px(x, y)) ink++;
    CHECK(ink > 0, "result text rendered in HUD");
    CHECK(hx_over_full, "force-full fired once on over");
    hex_render();
    CHECK(hx_over_full, "force-full not repeated");
}

int main(void) {
    test_new();
    test_adjacency();
    test_win_check();
    test_player_move();
    test_ai_win();
    test_ai_block();
    test_ai_prefers_win();
    test_ai_heuristic();
    test_draw_branch();
    test_over_keys();
    test_render();
    if (s_fail == 0) { printf("ALL TESTS PASSED\n"); return 0; }
    printf("%d TEST(S) FAILED\n", s_fail);
    return 1;
}
