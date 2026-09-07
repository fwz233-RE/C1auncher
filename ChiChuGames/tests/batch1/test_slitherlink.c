/* SLITHERLINK 逻辑单测 — host 编译运行
 * 包含游戏源码, 直接访问 sl_ 静态状态;
 * 唯一解用独立回调解验证(自带传播/剪枝, 不调用游戏判定函数) */
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#include "config.h"
#include "gfx/canvas.h"
#include "gfx/font.h"
#include "rng.h"
#include "platform/time.h"
#include "games/slitherlink.c"

/* host 框架 stub(main.c 不参与链接) */
bool s_exit_request = false;
void game_set_tick_interval(uint32_t ms) { (void)ms; }

static int s_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); s_fail++; } \
    else { printf("ok: %s\n", msg); } \
} while (0)

/* ================= 独立求解器(仅用 sl_rom 数据) ================= */
#define TN 5
#define TP 6
static int t_e[60];          /* -1 未定, 0 无线, 1 线 */
static int t_digit[TN][TN];
static long t_nodes;
static int t_sol[60];        /* 找到的第一个完整解 */

static int t_he(int y, int x) { return y * TN + x; }
static int t_ve(int y, int x) { return 30 + x * TN + y; }

/* 传播(格计数 + 格点度数规则), 新定边记入 prop(回溯时还原);
 * 返回新定边数, 死路返回 -1(此时已还原自身设置) */
static int t_propagate(int prop[60]) {
    int n = 0;
    int progress = 1;
    while (progress) {
        progress = 0;
        /* 格规则: has==need → 其余全 0; has+und==need → 其余全 1 */
        for (int j = 0; j < TN; j++) {
            for (int i = 0; i < TN; i++) {
                int ids[4];
                ids[0] = t_he(j, i);
                ids[1] = t_he(j + 1, i);
                ids[2] = t_ve(j, i);
                ids[3] = t_ve(j, i + 1);
                int has = 0, und = 0;
                for (int k = 0; k < 4; k++) {
                    if (t_e[ids[k]] < 0) und++;
                    else has += t_e[ids[k]];
                }
                int need = t_digit[j][i];
                if (und == 0 && has != need) {
                    while (n > 0) t_e[prop[--n]] = -1;
                    return -1;
                }
                if (has > need || has + und < need) {
                    while (n > 0) t_e[prop[--n]] = -1;
                    return -1;
                }
                if (und > 0 && has == need) {
                    for (int k = 0; k < 4; k++)
                        if (t_e[ids[k]] < 0) {
                            t_e[ids[k]] = 0;
                            prop[n++] = ids[k];
                        }
                    progress = 1;
                } else if (und > 0 && has + und == need) {
                    for (int k = 0; k < 4; k++)
                        if (t_e[ids[k]] < 0) {
                            t_e[ids[k]] = 1;
                            prop[n++] = ids[k];
                        }
                    progress = 1;
                }
            }
        }
        /* 格点规则: 度 2 → 其余邻边全 0; 度 1 且只剩 1 未定 → 必为 1 */
        for (int y = 0; y < TP && !progress; y++) {
            for (int x = 0; x < TP && !progress; x++) {
                int adj[4], an = 0;
                if (y < TN) adj[an++] = t_ve(y, x);
                if (y > 0) adj[an++] = t_ve(y - 1, x);
                if (x < TN) adj[an++] = t_he(y, x);
                if (x > 0) adj[an++] = t_he(y, x - 1);
                int d = 0, u = 0;
                for (int k = 0; k < an; k++) {
                    if (t_e[adj[k]] < 0) u++;
                    else d += t_e[adj[k]];
                }
                if (d > 2) {
                    while (n > 0) t_e[prop[--n]] = -1;
                    return -1;
                }
                if (u == 0) continue;
                if (d == 2) {
                    for (int k = 0; k < an; k++)
                        if (t_e[adj[k]] < 0) {
                            t_e[adj[k]] = 0;
                            prop[n++] = adj[k];
                        }
                    progress = 1;
                } else if (d == 1 && u == 1) {
                    for (int k = 0; k < an; k++)
                        if (t_e[adj[k]] < 0) {
                            t_e[adj[k]] = 1;
                            prop[n++] = adj[k];
                        }
                    progress = 1;
                }
            }
        }
    }
    return n;
}

/* 完整盘面判定: 数字 + 度数 0/2 + 单连通非空 */
static int t_loop_ok(void) {
    for (int j = 0; j < TN; j++)
        for (int i = 0; i < TN; i++) {
            int c = t_e[t_he(j, i)] + t_e[t_he(j + 1, i)] +
                    t_e[t_ve(j, i)] + t_e[t_ve(j, i + 1)];
            if (c != t_digit[j][i]) return 0;
        }
    int n = 0;
    for (int i = 0; i < 60; i++) if (t_e[i]) n++;
    if (n == 0) return 0;
    for (int y = 0; y < TP; y++)
        for (int x = 0; x < TP; x++) {
            int d = 0;
            if (y < TN) d += t_e[t_ve(y, x)];
            if (y > 0) d += t_e[t_ve(y - 1, x)];
            if (x < TN) d += t_e[t_he(y, x)];
            if (x > 0) d += t_e[t_he(y, x - 1)];
            if (d != 0 && d != 2) return 0;
        }
    int vis[60] = {0};
    int q[60];
    int first = -1;
    for (int i = 0; i < 60; i++) if (t_e[i]) { first = i; break; }
    int hd = 0, tl = 0;
    q[tl++] = first;
    vis[first] = 1;
    while (hd < tl) {
        int e = q[hd++];
        int ax, ay, bx, by;
        if (e < 30) { ax = e % TN; ay = e / TN; bx = ax + 1; by = ay; }
        else { int t = e - 30; ax = t / TN; ay = t % TN; bx = ax; by = ay + 1; }
        for (int f = 0; f < 60; f++) {
            if (vis[f] || !t_e[f]) continue;
            int fx, fy, gx, gy;
            if (f < 30) { fx = f % TN; fy = f / TN; gx = fx + 1; gy = fy; }
            else { int t2 = f - 30; fx = t2 / TN; fy = t2 % TN; gx = fx; gy = fy + 1; }
            if ((fx == ax && fy == ay) || (fx == bx && fy == by) ||
                (gx == ax && gy == ay) || (gx == bx && gy == by)) {
                vis[f] = 1;
                q[tl++] = f;
            }
        }
    }
    int cnt = 0;
    for (int i = 0; i < 60; i++) cnt += vis[i];
    return cnt == n;
}

/* 回溯: 找到 sol_cap 个解即停; 节点上限防爆炸 */
static int t_solve(int sol_cap, int *sols) {
    t_nodes++;
    if (t_nodes > 10000000L) return 1;
    if (*sols >= sol_cap) return 0;
    int prop[60];
    int pn = t_propagate(prop);
    if (pn < 0) return 0;
    int dead = 0;
    int e = -1;
    for (int i = 0; i < 60 && !dead; i++)
        if (t_e[i] < 0) { e = i; break; }
    if (e < 0) {
        if (t_loop_ok()) {
            if (*sols == 0)
                for (int i = 0; i < 60; i++) t_sol[i] = t_e[i];
            (*sols)++;
        }
    } else {
        t_e[e] = 0;
        t_solve(sol_cap, sols);
        if (*sols < sol_cap) {
            t_e[e] = 1;
            t_solve(sol_cap, sols);
        }
        t_e[e] = -1;
    }
    while (pn > 0) t_e[prop[--pn]] = -1;
    return 0;
}

/* 载入题目 p 到独立求解器 */
static void t_load(int p) {
    for (int i = 0; i < 25; i++) t_digit[i / 5][i % 5] = sl_rom[p][i] - '0';
    for (int i = 0; i < 60; i++) t_e[i] = -1;
    t_nodes = 0;
}

/* ================= 测试 ================= */

/* ROM 格式: 25 位, 0-4, 两两不同, 有非零 */
static void test_rom_format(void) {
    for (int p = 0; p < SL_ROM_N; p++) {
        int ok = 1;
        int nz = 0;
        for (int i = 0; i < 25; i++) {
            char c = sl_rom[p][i];
            if (c < '0' || c > '4') ok = 0;
            if (c != '0') nz++;
        }
        CHECK(ok == 1, "rom: digits only 0-4");
        CHECK(nz > 0, "rom: has nonzero digits");
    }
    int distinct = 1;
    for (int i = 0; i < SL_ROM_N && distinct; i++)
        for (int j = i + 1; j < SL_ROM_N && distinct; j++)
            if (strcmp(sl_rom[i], sl_rom[j]) == 0) distinct = 0;
    CHECK(distinct == 1, "rom: puzzles pairwise distinct");
}

/* 独立回调解: 每题恰好 1 解 */
static void test_unique_solutions(void) {
    for (int p = 0; p < SL_ROM_N; p++) {
        t_load(p);
        int sols = 0;
        t_solve(2, &sols);
        printf("  puzzle %d: solutions=%d nodes=%ld\n", p, sols, t_nodes);
        CHECK(t_nodes < 10000000L, "rom: solver finished within node cap");
        CHECK(sols == 1, "rom: puzzle has exactly one solution");
        CHECK(sl_rom[p][0] >= '0', "sanity");
    }
}

/* 解法经真实输入路径(逐边 OK)触发胜利 */
static void test_solution_wins_via_input(void) {
    for (int p = 0; p < SL_ROM_N; p++) {
        t_load(p);
        int sols = 0;
        t_solve(1, &sols);
        CHECK(sols == 1, "solver finds the solution");
        sl_new_game(p);
        for (int i = 0; i < 60; i++) sl_edge[i] = (uint8_t)t_sol[i];
        CHECK(sl_check_win(), "solution edge set passes win check");
        sl_new_game(p);
        for (int i = 0; i < 60; i++)
            if (t_sol[i] == 1) sl_toggle_edge(i);
        CHECK(sl_over, "toggling solution edges wins via game input");
        CHECK(sl_check_win(), "win flag consistent with check");
    }
}

/* 非胜利态: 空盘 / 单回路数字不符 / 开口 / 分支 */
static void test_win_negatives(void) {
    sl_new_game(0);
    CHECK(!sl_check_win(), "empty board not win");

    /* 单回路但数字不符: 外围一周(角=2 边=1 心=0, 与题面不符) */
    sl_new_game(0);
    for (int x = 0; x < 5; x++) {
        sl_edge[sl_hid(0, x)] = 1;
        sl_edge[sl_hid(5, x)] = 1;
    }
    for (int y = 0; y < 5; y++) {
        sl_edge[sl_vid(y, 0)] = 1;
        sl_edge[sl_vid(y, 5)] = 1;
    }
    CHECK(!sl_check_win(), "perimeter loop with wrong digits not win");

    /* 解法缺一边(开口) */
    t_load(0);
    int sols = 0;
    t_solve(1, &sols);
    sl_new_game(0);
    for (int i = 0; i < 60; i++) sl_edge[i] = (uint8_t)t_sol[i];
    int first_line = -1;
    for (int i = 0; i < 60; i++)
        if (t_sol[i]) { first_line = i; break; }
    sl_edge[first_line] = 0;
    CHECK(!sl_check_win(), "loop with one edge removed not win");

    /* 解法加一条无关边(分支/度数 1/3) */
    sl_new_game(0);
    for (int i = 0; i < 60; i++) sl_edge[i] = (uint8_t)t_sol[i];
    int extra = -1;
    for (int i = 0; i < 60; i++)
        if (!t_sol[i]) { extra = i; break; }
    sl_edge[extra] = 1;
    CHECK(!sl_check_win(), "loop with extra edge not win");
}

/* 边索引/端点助手 */
static void test_indexing(void) {
    CHECK(sl_hid(0, 0) == 0 && sl_hid(5, 4) == 29, "h edge ids");
    CHECK(sl_vid(0, 0) == 30 && sl_vid(4, 5) == 59, "v edge ids");
    int ax, ay, bx, by;
    sl_endpoints(0, &ax, &ay, &bx, &by);
    CHECK(ax == 0 && ay == 0 && bx == 1 && by == 0, "h endpoints");
    sl_endpoints(30, &ax, &ay, &bx, &by);
    CHECK(ax == 0 && ay == 0 && bx == 0 && by == 1, "v endpoints");
    sl_endpoints(59, &ax, &ay, &bx, &by);
    CHECK(ax == 5 && ay == 4 && bx == 5 && by == 5, "last v endpoints");
}

/* 光标/切换/边界行为 */
static void test_cursor_and_toggle(void) {
    sl_new_game(0);
    CHECK(sl_cx == 3 && sl_cy == 3, "cursor centered");
    key_event_t ev;
    ev.ch = 0;
    ev.is_repeat = false;

    /* 边循环: 空→线→点→空 */
    ev.key = K_OK;
    slitherlink_on_key(&ev);
    CHECK(sl_edge[sl_hid(3, 3)] == 1, "OK draws line");
    slitherlink_on_key(&ev);
    CHECK(sl_edge[sl_hid(3, 3)] == 2, "OK 2nd -> cross");
    slitherlink_on_key(&ev);
    CHECK(sl_edge[sl_hid(3, 3)] == 0, "OK 3rd -> empty");

    /* DEL/SPACE 切下方边 */
    ev.key = K_DEL;
    slitherlink_on_key(&ev);
    CHECK(sl_edge[sl_vid(3, 3)] == 1, "DEL draws vertical line");
    ev.key = K_SPACE;
    slitherlink_on_key(&ev);
    CHECK(sl_edge[sl_vid(3, 3)] == 2, "SPACE cycles vertical to cross");

    /* 方向移动与钳位 */
    ev.key = K_UP;
    slitherlink_on_key(&ev);
    CHECK(sl_cy == 2, "UP moves");
    for (int i = 0; i < 10; i++) { ev.key = K_LEFT; slitherlink_on_key(&ev); }
    CHECK(sl_cx == 0, "LEFT clamps at 0");
    for (int i = 0; i < 10; i++) { ev.key = K_RIGHT; slitherlink_on_key(&ev); }
    CHECK(sl_cx == 5, "RIGHT clamps at 5");

    /* 重复事件: 方向响应, 确认键忽略 */
    ev.key = K_RIGHT;
    ev.is_repeat = true;
    slitherlink_on_key(&ev);
    CHECK(sl_cx == 5, "repeat RIGHT still moves");
    ev.key = K_OK;
    slitherlink_on_key(&ev);
    CHECK(sl_edge[sl_hid(2, 4)] == 0, "repeat OK ignored");

    /* 最右列 OK 切左方边 (cy=2) */
    ev.is_repeat = false;
    slitherlink_on_key(&ev);
    CHECK(sl_edge[sl_hid(2, 4)] == 1, "OK at right border toggles left edge");

    /* 最下行 DEL 切上方边 */
    for (int i = 0; i < 3; i++) { ev.key = K_DOWN; slitherlink_on_key(&ev); }
    CHECK(sl_cy == 5, "DOWN to bottom");
    ev.key = K_DEL;
    slitherlink_on_key(&ev);
    CHECK(sl_edge[sl_vid(4, 5)] == 1, "DEL at bottom toggles edge above");

    /* WASD */
    ev.key = K_CHAR;
    ev.ch = 'w';
    slitherlink_on_key(&ev);
    CHECK(sl_cy == 4, "W moves up");
    ev.ch = 'd';
    slitherlink_on_key(&ev);
    CHECK(sl_cx == 5, "D clamps at right");
    ev.ch = 'a';
    slitherlink_on_key(&ev);
    CHECK(sl_cx == 4, "A moves left");
}

/* 胜利流程 + 结束后输入 */
static void test_over_state(void) {
    t_load(0);
    int sols = 0;
    t_solve(1, &sols);
    CHECK(sols == 1, "solver ok for over test");
    sl_new_game(0);
    int last = -1;
    for (int i = 0; i < 60; i++) if (t_sol[i]) last = i;
    for (int i = 0; i < 60; i++)
        if (t_sol[i] && i != last) sl_toggle_edge(i);
    CHECK(!sl_over, "not over before last edge");
    sl_toggle_edge(last);
    CHECK(sl_over, "last edge triggers win");

    key_event_t ev;
    ev.ch = 0;
    ev.is_repeat = false;
    int cx0 = sl_cx, cy0 = sl_cy;
    ev.key = K_LEFT;
    slitherlink_on_key(&ev);
    CHECK(sl_cx == cx0 && sl_cy == cy0, "input ignored when over");

    /* OK = 重开同一题 */
    ev.key = K_OK;
    slitherlink_on_key(&ev);
    CHECK(!sl_over, "OK at over retries");
    CHECK(sl_pz == 0, "retry keeps same puzzle");
    CHECK(sl_edge[sl_hid(0, 0)] == 0, "retry clears board");

    /* BACK/Q 退出 */
    sl_new_game(0);
    s_exit_request = false;
    ev.key = K_QUIT;
    slitherlink_on_key(&ev);
    CHECK(s_exit_request, "Q requests exit");
}

/* N 下一题 / R 重开 */
static void test_new_and_retry(void) {
    sl_new_game(0);
    key_event_t ev;
    ev.is_repeat = false;
    ev.ch = 0;
    ev.key = K_CHAR;
    ev.ch = 'n';
    slitherlink_on_key(&ev);
    CHECK(sl_pz == 1, "N advances puzzle");
    CHECK(sl_digit[0] == (uint8_t)(sl_rom[1][0] - '0'), "N loads next digits");
    ev.ch = 'n';
    slitherlink_on_key(&ev);
    ev.ch = 'n';
    slitherlink_on_key(&ev);
    CHECK(sl_pz == 0, "N wraps around");
    ev.ch = 'r';
    slitherlink_on_key(&ev);
    CHECK(sl_pz == 0, "R restarts same puzzle");
    CHECK(sl_edge[sl_hid(0, 0)] == 0, "R clears board");
}

/* 渲染冒烟: 有内容, 不越界 */
static void test_render_smoke(void) {
    sl_new_game(1);
    slitherlink_render();
    int black = 0;
    for (int i = 0; i < (int)CCG_FRAME_BYTES; i++) if (g_fb[i]) black++;
    CHECK(black > 100, "render draws content");
    /* 画一条边再渲染 */
    sl_toggle_edge(sl_hid(3, 3));
    slitherlink_render();
    black = 0;
    for (int i = 0; i < (int)CCG_FRAME_BYTES; i++) if (g_fb[i]) black++;
    CHECK(black > 100, "render with line ok");
}

int main(void) {
    test_rom_format();
    test_unique_solutions();
    test_solution_wins_via_input();
    test_win_negatives();
    test_indexing();
    test_cursor_and_toggle();
    test_over_state();
    test_new_and_retry();
    test_render_smoke();
    if (s_fail == 0) {
        printf("ALL SLITHERLINK TESTS PASSED\n");
        return 0;
    }
    printf("%d TEST(S) FAILED\n", s_fail);
    return 1;
}
