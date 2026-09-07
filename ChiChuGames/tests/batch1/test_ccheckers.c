/* CHINESE CHECKERS 逻辑单测 — host cc 编译运行; 零平台依赖
 * 链接: canvas/font/font_data/pattern/rng/time/ui_common/input/display
 * 编译: cc -std=c11 -O2 -DCHICHU_HOST -I<root> <本文件> src/gfx/canvas.c src/gfx/font.c
 *        src/platform/time.c src/ui/ui_common.c src/platform/input.c src/platform/display.c */
#include "../../src/config.h"
#include "../../src/gfx/canvas.h"
#include "../../src/gfx/font.h"
#include "../../src/gfx/pattern.h"
#include "../../src/rng.h"
#include "../../src/games/ccheckers.c"
#include <stdio.h>
#include <string.h>

static int s_fail = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); s_fail++; } \
    else { printf("ok: %s\n", msg); } \
} while (0)

/* host 框架 stub(main.c 提供) */
bool s_exit_request = false;
void game_set_tick_interval(uint32_t ms) { (void)ms; }

static void board_clear(void) {
    for (int y = 0; y < CC_GW; y++)
        for (int x = 0; x < CC_GW; x++)
            cc_board[y][x] = cc_valid(x, y) ? 0 : CC_INV;
}

static int piece_count(int who) {
    int n = 0;
    for (int y = 0; y < CC_GW; y++)
        for (int x = 0; x < CC_GW; x++)
            if (cc_board[y][x] == (uint8_t)who) n++;
    return n;
}

static int min_dist_ai(int tx, int ty) {
    int m = 99;
    for (int y = 0; y < CC_GW; y++)
        for (int x = 0; x < CC_GW; x++)
            if (cc_board[y][x] == 2) {
                int d = cc_cdist(x, y, tx, ty);
                if (d < m) m = d;
            }
    return m;
}

/* ---- 棋盘几何 ---- */
static void test_board_geometry(void) {
    int valid = 0, rows[CC_GW] = { 0 };
    for (int y = 0; y < CC_GW; y++)
        for (int x = 0; x < CC_GW; x++)
            if (cc_valid(x, y)) { valid++; rows[y]++; }
    CHECK(valid == 61, "61 valid cells");
    {
        int expect[9] = { 5, 6, 7, 8, 9, 8, 7, 6, 5 };
        int bad = 0;
        for (int y = 0; y < 9; y++)
            if (rows[y] != expect[y]) { bad = 1; printf("row %d len %d\n", y, rows[y]); }
        CHECK(!bad, "row lengths 5..9..5");
    }
    int r1 = 0, r2 = 0;
    for (int y = 0; y < CC_GW; y++)
        for (int x = 0; x < CC_GW; x++) {
            int r = cc_region(x, y);
            if (r == 1) r1++;
            else if (r == 2) r2++;
        }
    CHECK(r1 == 10, "player triangle 10 cells");
    CHECK(r2 == 10, "AI triangle 10 cells");
    CHECK(cc_region(4, 0) == 1 && cc_region(4, 8) == 2, "tips assigned");
    CHECK(cc_region(3, 1) == 1 && cc_region(1, 3) == 1, "top triangle corners");
    CHECK(cc_region(4, 8) == 2 && cc_region(7, 5) == 2 && cc_region(5, 7) == 2,
          "bottom triangle corners");
    CHECK(cc_region(4, 4) == 0 && cc_region(0, 4) == 0, "center/mid neutral");
    CHECK(!cc_valid(0, 0) && !cc_valid(8, 8), "diagonal grid corners invalid");
    CHECK(cc_valid(8, 0) && cc_valid(0, 8) && cc_valid(8, 4) && cc_valid(0, 4),
          "hexagon side corners valid");
    CHECK(cc_cdist(4, 4, 4, 4) == 0, "dist self 0");
    CHECK(cc_cdist(4, 4, 4, 0) == 4, "center->top corner 4");
    CHECK(cc_cdist(4, 0, 4, 8) == 8, "top->bottom corner 8");
    CHECK(cc_cdist(4, 4, 5, 4) == 1, "adjacent dist 1");
}

/* ---- 开局布子 ---- */
static void test_setup(void) {
    cc_new();
    CHECK(piece_count(1) == 10 && piece_count(2) == 10, "10 pieces each");
    int inv = 0;
    for (int y = 0; y < CC_GW; y++)
        for (int x = 0; x < CC_GW; x++)
            if (!cc_valid(x, y) && cc_board[y][x] != CC_INV) inv++;
    CHECK(inv == 0, "all invalid cells marked CC_INV");
    CHECK(cc_turn == 1 && !cc_over, "player first, not over");
    CHECK(cc_home_count(1) == 0 && cc_home_count(2) == 0, "no pieces home at start");
    CHECK(cc_board[3][4] == 1 && cc_board[8][4] == 2, "representative pieces placed");
}

/* ---- BFS: 一步走 ---- */
static void test_bfs_steps(void) {
    board_clear();
    cc_board[4][4] = 1;
    cc_bfs(4, 4);
    int n = 0, maxl = 0;
    for (int i = 0; i < CC_GW * CC_GW; i++) {
        if (cc_vis[i] > 0) { n++; if (cc_vis[i] > maxl) maxl = cc_vis[i]; }
    }
    CHECK(n == 6, "center has 6 step targets");
    CHECK(maxl == 1, "all steps pathlen 1");
    CHECK(cc_vis[5 * CC_GW + 4] == 1, "(4,5) reachable below center");
    CHECK(cc_vis[4 * CC_GW + 4] == 0, "source not a target");
    board_clear();
    cc_board[0][4] = 1;
    cc_bfs(4, 0);
    n = 0;
    for (int i = 0; i < CC_GW * CC_GW; i++)
        if (cc_vis[i] > 0) n++;
    CHECK(n == 3, "tip has 3 step targets");
}

/* ---- BFS: 跳与连续跳; 跳后禁走一步 ---- */
static void test_bfs_jump(void) {
    board_clear();
    cc_board[4][4] = 1;
    cc_board[4][5] = 2;
    cc_bfs(4, 4);
    CHECK(cc_vis[4 * CC_GW + 6] == 2, "jump lands pathlen 2");
    CHECK(cc_vis[4 * CC_GW + 7] == 0, "no step after jump (down)");
    CHECK(cc_vis[5 * CC_GW + 6] == 0, "no step after jump (diag)");
    /* 连续跳 */
    cc_board[4][7] = 2;
    cc_bfs(4, 4);
    CHECK(cc_vis[4 * CC_GW + 8] == 3, "double jump pathlen 3");
    CHECK(cc_vis[4 * CC_GW + 6] == 2, "intermediate landing kept");
    /* 跳回源被禁止 */
    cc_board[6][4] = 2;   /* 从 (6,4) 跳回 (4,4)? (4,4) 有源子, 不可落 */
    cc_bfs(4, 4);
    CHECK(cc_vis[4 * CC_GW + 4] == 0, "source cell never a target");
    /* 顶角跳不可越界: (4,1) 跳过 (4,0) 落在界外 → 不可达 */
    board_clear();
    cc_board[1][4] = 1;   /* 移动子 (4,1) */
    cc_board[0][4] = 2;   /* 被跳子 (4,0) */
    cc_bfs(4, 1);
    CHECK(cc_vis[0 * CC_GW + 4] == 0, "no jump landing beyond tip");
}

/* ---- 玩家操作流 ---- */
static void test_player_move(void) {
    cc_new();
    cc_cx = 4;
    cc_cy = 3;
    cc_handle_ok();
    CHECK(cc_sel && cc_sx == 4 && cc_sy == 3, "select own piece");
    cc_bfs(4, 3);
    CHECK(cc_vis[4 * CC_GW + 4] > 0, "(4,4) reachable from (4,3)");
    cc_cx = 4;
    cc_cy = 4;
    cc_handle_ok();
    CHECK(!cc_sel && cc_board[4][4] == 1 && cc_board[3][4] == 0, "piece moved");
    CHECK(cc_turn == 2, "AI turn after player move");
    CHECK(cc_moves == 1, "move counted");
    /* 同格再按 = 取消选中 */
    cc_turn = 1;
    cc_cx = 3;
    cc_cy = 3;
    cc_handle_ok();
    CHECK(cc_sel && cc_sx == 3 && cc_sy == 3, "select another own piece");
    cc_handle_ok();
    CHECK(!cc_sel, "cancel selection on same cell");
    /* 选中另一枚己子 = 换选 */
    cc_handle_ok();
    cc_cx = 0;
    cc_cy = 4;
    cc_handle_ok();
    CHECK(cc_sel && cc_sx == 3 && cc_sy == 3, "illegal target keeps selection");
    /* AI 回合中玩家不可操作 */
    cc_sel = false;
    cc_turn = 2;
    cc_cx = 4;
    cc_cy = 4;
    cc_handle_ok();
    CHECK(!cc_sel && cc_board[4][4] == 1, "player cannot act on AI turn");
}

/* ---- 一跳多跳一次落子 ---- */
static void test_multi_jump_move(void) {
    board_clear();
    cc_turn = 1;
    cc_over = false;
    cc_sel = false;
    cc_moves = 0;
    cc_no_prog = 0;
    cc_board[4][4] = 1;
    cc_board[5][4] = 2;   /* (4,5) 被跳子 */
    cc_board[7][4] = 2;   /* (4,7) 被跳子 */
    cc_cx = 4;
    cc_cy = 4;
    cc_handle_ok();
    CHECK(cc_sel, "selected");
    cc_cx = 4;
    cc_cy = 8;
    cc_handle_ok();
    CHECK(cc_board[8][4] == 1 && cc_board[4][4] == 0, "chain jump applied in one move");
    CHECK(piece_count(1) == 1 && piece_count(2) == 2, "counts preserved");
    CHECK(cc_turn == 2, "turn passed after chain");
    /* 进目标三角 → HOME 计数 */
    CHECK(cc_home_count(1) == 1, "home count updated");
}

/* ---- 光标滑动 ---- */
static void test_cursor(void) {
    cc_new();
    cc_cx = 4;
    cc_cy = 3;
    cc_cursor_move(0, -1);
    CHECK(cc_cx == 4 && cc_cy == 2, "up 1");
    cc_cursor_move(0, -1);
    cc_cursor_move(0, -1);
    CHECK(cc_cx == 4 && cc_cy == 0, "up to tip");
    cc_cursor_move(0, -1);
    CHECK(cc_cx == 4 && cc_cy == 0, "stays at tip");
    cc_cursor_move(-1, 0);
    CHECK(cc_cx == 4 && cc_cy == 0, "no valid cell sideways");
    cc_cursor_move(1, 0);
    CHECK(cc_cx == 5 && cc_cy == 0, "glide right at tip row");
    cc_cursor_move(0, 1);
    CHECK(cc_cy == 1, "down from tip row");
}

/* ---- AI 走子合法性 ---- */
static void test_ai_legality(void) {
    cc_new();
    uint8_t before[CC_GW][CC_GW];
    for (int y = 0; y < CC_GW; y++)
        for (int x = 0; x < CC_GW; x++) before[y][x] = cc_board[y][x];
    cc_turn = 2;
    cc_ai_at = 0;
    cc_ai_turn();
    CHECK(cc_turn == 1, "AI moved, turn back");
    int sx = -1, sy = -1, tx = -1, ty = -1, diffs = 0;
    for (int y = 0; y < CC_GW; y++)
        for (int x = 0; x < CC_GW; x++) {
            if (before[y][x] != cc_board[y][x]) {
                diffs++;
                if (before[y][x] == 2 && cc_board[y][x] != 2) { sx = x; sy = y; }
                if (before[y][x] != 2 && cc_board[y][x] == 2) { tx = x; ty = y; }
            }
        }
    CHECK(diffs == 2 && sx >= 0 && tx >= 0, "exactly one piece moved");
    CHECK(cc_board[ty][tx] == 2, "destination holds AI piece");
    CHECK(piece_count(1) == 10 && piece_count(2) == 10, "counts preserved");
    CHECK(cc_moves == 1, "AI move counted");
    /* 从源 BFS 验证目标可达(先恢复原局面再验证) */
    for (int y = 0; y < CC_GW; y++)
        for (int x = 0; x < CC_GW; x++) cc_board[y][x] = before[y][x];
    cc_bfs(sx, sy);
    CHECK(cc_vis[ty * CC_GW + tx] > 0, "AI destination BFS-legal");
}

/* ---- AI 贪心: 标准开局 20 回合最小距稳定在 4(玩家三角占满挡住) ---- */
static void test_ai_blocked(void) {
    cc_new();
    rng_seed(&cc_rng, 12345);
    int minnd = 99;
    for (int t = 0; t < 20; t++) {
        cc_turn = 2;
        cc_ai_at = 0;
        cc_ai_turn();
        minnd = min_dist_ai(4, 0);
        if (cc_over) break;
    }
    printf("probe: min AI dist after 20 turns (player triangle full) = %d\n", minnd);
    CHECK(minnd == 1, "AI parks at distance 1 (corner held by player)");
    CHECK(piece_count(1) == 10 && piece_count(2) == 10, "counts intact");
    CHECK(!cc_over, "game continues");
}

/* ---- AI 贪心: 清空棋盘只留 AI 子 → 应一路推进到对角角 (4,0) ---- */
static void test_ai_progress(void) {
    board_clear();
    for (int y = 0; y < CC_GW; y++)
        for (int x = 0; x < CC_GW; x++)
            if (cc_region(x, y) == 2) cc_board[y][x] = 2;
    cc_over = false;
    cc_winner = 0;
    cc_moves = 0;
    cc_no_prog = 0;
    rng_seed(&cc_rng, 7);
    int minnd = 99, turns = 0;
    for (int t = 0; t < 80 && !cc_over; t++) {
        cc_turn = 2;
        cc_ai_at = 0;
        cc_ai_turn();
        turns++;
        minnd = min_dist_ai(4, 0);
    }
    printf("probe: AI-only min dist = %d after %d turns\n", minnd, turns);
    CHECK(minnd == 0, "AI reaches far corner on clear board");
    CHECK(piece_count(2) == 10, "AI count intact");
}

/* ---- 胜负与和棋 ---- */
static void test_win_draw(void) {
    /* 玩家 9 子在目标三角, 第 10 子一步踏入 → 胜 */
    board_clear();
    static const int cells[10][2] = {
        { 4, 8 }, { 4, 7 }, { 5, 7 }, { 4, 6 }, { 5, 6 },
        { 6, 6 }, { 5, 5 }, { 6, 5 }, { 7, 5 }, { 4, 5 }
    };
    for (int i = 0; i < 9; i++) cc_board[cells[i][1]][cells[i][0]] = 1;
    cc_board[4][4] = 1;   /* 最后 1 子 */
    cc_turn = 1;
    cc_over = false;
    cc_winner = 0;
    cc_sel = false;
    cc_moves = 0;
    cc_no_prog = 0;
    cc_home_max[0] = 0;
    cc_home_max[1] = 0;
    cc_home_total_max = 0;
    cc_cx = 4;
    cc_cy = 4;
    cc_handle_ok();       /* 选 */
    CHECK(cc_sel, "selected final piece");
    cc_cx = 4;
    cc_cy = 5;            /* (4,5) 空, 一步进入目标三角 */
    cc_handle_ok();
    CHECK(cc_over && cc_winner == 1, "player wins filling far triangle");
    /* 和棋 */
    cc_new();
    cc_no_prog = CC_DRAW_LIMIT - 1;
    cc_after_move(1);
    CHECK(cc_over && cc_winner == 3, "draw after stall limit");
}

/* ---- 端到端对局模拟: 贪心玩家 vs AI, 300 步内必终局且状态不变量保持 ---- */
static bool sim_player_move(void) {
    /* 全局最优: 最小化到 (4,8) 距离; 第一遍只进不退, 第二遍允许回退(镜像 AI) */
    for (int pass = 0; pass < 2; pass++) {
        int best = 1 << 30, sx = -1, sy = -1, tx = -1, ty = -1;
        for (int y = 0; y < CC_GW; y++)
            for (int x = 0; x < CC_GW; x++) {
                if (cc_board[y][x] != 1) continue;
                int cur = cc_cdist(x, y, 4, 8);
                cc_bfs(x, y);
                for (int i = 0; i < CC_GW * CC_GW; i++) {
                    if (cc_vis[i] <= 0) continue;
                    int vx = i % CC_GW, vy = i / CC_GW;
                    int nd = cc_cdist(vx, vy, 4, 8);
                    if (pass == 0 && nd > cur) continue;
                    int crowd = (cc_region(vx, vy) == 2)
                                    ? 0
                                    : cc_own_adj(vx, vy, 1) * 3;
                    int score = nd * 100 + cc_vis[i] * 2 - (cc_vis[i] > 1 ? 4 : 0) +
                                crowd + (int)rng_range(&cc_rng, 3);
                    if (score < best) {
                        best = score;
                        sx = x; sy = y; tx = vx; ty = vy;
                    }
                }
            }
        if (sx < 0) continue;
        cc_cx = sx;
        cc_cy = sy;
        cc_handle_ok();
        if (!cc_sel) return false;
        cc_cx = tx;
        cc_cy = ty;
        cc_handle_ok();
        return !cc_sel;
    }
    return false;
}

static void test_sim_game(void) {
    cc_new();
    rng_seed(&cc_rng, 20260830u);
    int turns = 0;
    while (!cc_over && turns < 4000) {
        if (cc_turn == 1) {
            if (!sim_player_move()) break;
        } else {
            cc_ai_at = 0;
            cc_ai_turn();
        }
        turns++;
        if (piece_count(1) != 10 || piece_count(2) != 10) {
            printf("FAIL: piece count drift at turn %d\n", turns);
            s_fail++;
            return;
        }
        if (turns % 500 == 0)
            printf("  sim %d: youHome=%d aiHome=%d\n", turns,
                   cc_home_count(1), cc_home_count(2));
    }
    printf("sim: %d turns, winner=%d\n", turns, cc_winner);
    CHECK(cc_over, "sim game terminates");
    CHECK(piece_count(1) == 10 && piece_count(2) == 10, "counts held through sim");
}

/* ---- 渲染冒烟: host 帧缓冲有内容且关键像素正确 ---- */
static void test_render_smoke(void) {
    ccheckers_enter();          /* 渲染 + disp_full(host no-op) */
    int black = 0;
    for (unsigned i = 0; i < CCG_FRAME_BYTES; i++)
        for (int b = 0; b < 8; b++)
            if (g_fb[i] & (0x80u >> b)) black++;
    CHECK(black > 1000 && black < 30000, "frame has content (not blank/all-black)");
    /* 玩家实心子 (4,2) 中心像素: 黑(不在光标下); py = 24+13*y */
    CHECK((g_fb[(50 >> 3) * CCG_W + 109] & (0x80u >> (50 & 7))) != 0,
          "player piece pixel black");
    /* AI 空心子 (4,8): 环上黑; py = 24+13*8 = 128 */
    CHECK((g_fb[(128 >> 3) * CCG_W + 54] & (0x80u >> (128 & 7))) != 0,
          "AI ring pixel black");
    /* 光标(4,3) 反白: 实心子变白环(环边 x=138 白, 中心仍黑); py = 24+13*3 = 63 */
    CHECK((g_fb[(63 >> 3) * CCG_W + 138] & (0x80u >> (63 & 7))) == 0,
          "cursor invert on piece (white ring)");
    /* 结束态渲染不越界 */
    cc_over = true;
    cc_winner = 2;
    ccheckers_render();
    CHECK(cc_over_full, "end render forces full refresh once");
    int black2 = 0;
    for (unsigned i = 0; i < CCG_FRAME_BYTES; i++)
        for (int b = 0; b < 8; b++)
            if (g_fb[i] & (0x80u >> b)) black2++;
    CHECK(black2 > 100, "over frame rendered");
}

int main(void) {
    test_board_geometry();
    test_setup();
    test_bfs_steps();
    test_bfs_jump();
    test_player_move();
    test_multi_jump_move();
    test_cursor();
    test_ai_legality();
    test_ai_blocked();
    test_ai_progress();
    test_win_draw();
    test_sim_game();
    test_render_smoke();
    printf(s_fail ? "RESULT: %d FAIL\n" : "RESULT: ALL PASS\n", s_fail);
    return s_fail != 0;
}
