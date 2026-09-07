/* TENTS 逻辑单测 — host cc 编译运行; 零平台依赖
 *
 * 覆盖:
 *   1. 谜题数据完整性: 树串与预期板面镜像一致; 树数 == 行和 == 列和;
 *      行/列数在 [0,7] 内; 行长为 7。
 *   2. 唯一解: 测试侧独立回溯求解器(与游戏判定实现完全不同的代码结构,
 *      约束: 每树恰 1 邻帐篷 / 帐篷 8 邻不相触 / 无孤儿帐篷 / 行/列数)
 *      对 3 个谜题断言解数 == 1, 且 == 预期板面。
 *   3. tn_cycle 状态机: 空→帐篷→草→空; 树上无效; 清格。
 *   4. tn_solved / tn_errors: 完整解=赢; 缺帐篷/多余帐篷/相触/孤儿
 *      =不赢且报错; 草不干扰。
 *   5. on_key 全流程: 重复过滤(确认键忽略/方向可用)、光标移动+边界钳制、
 *      WASD、'x'/DEL 清、'n' 换题(2→0 环绕)、解后 OK 换题、BACK/Q 退出。
 *   6. 渲染冒烟: 光标边框、空格外黑内白、树方块、格线、HUD 分隔线、
 *      帐篷顶点、帐篷上光标白边、解后 HUD 清底 + SOLVED 文本。
 */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

/* 框架全局(main.c 定义), 测试不链接 main.c */
bool s_exit_request = false;

#include "../../src/games/tents.c"

static int s_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); s_fail++; } \
    else { printf("ok: %s\n", msg); } \
} while (0)

/* 预期板面(树+帐篷, 'T'=树 '^'=帐篷): 与生成脚本输出一致 */
static const char t_sol[TN_PUZZLES][TN_N][TN_N + 1] = {
    {   /* PINE — 6 树 */
        ".^...^.",
        ".T...T.",
        "..T^...",
        ".......",
        "....^..",
        "....T..",
        "..^TT^.",
    },
    {   /* MAPLE — 9 树 */
        "...^TT^",
        ".......",
        "^.^.^T.",
        "T.T....",
        "T^..^..",
        "T...T..",
        "^..^T..",
    },
    {   /* BIRCH — 10 树 */
        "..^T.^T",
        "....T..",
        ".^T.^.T",
        "......^",
        "..^T...",
        "^T.T^.T",
        "..^T..^",
    },
};

/* ---- 1. 数据完整性 ---- */
static void test_data(void) {
    for (int p = 0; p < TN_PUZZLES; p++) {
        int nt = 0, rs = 0, cs = 0;
        for (int r = 0; r < TN_N; r++) {
            CHECK(strlen(tn_trees[p][r]) == (size_t)TN_N, "tree row length is 7");
            for (int c = 0; c < TN_N; c++) {
                char e = (t_sol[p][r][c] == 'T') ? 'T' : '.';
                CHECK(tn_trees[p][r][c] == e, "tree string mirrors intended board");
                if (t_sol[p][r][c] == 'T') nt++;
            }
            rs += (int)tn_rcnt[p][r];
            cs += (int)tn_ccnt[p][r];
            CHECK(tn_rcnt[p][r] <= 7 && tn_ccnt[p][r] <= 7, "edge counts within 0..7");
        }
        CHECK(nt == rs && nt == cs, "tree count == row sum == col sum");
    }
}

/* ---- 2. 独立回溯求解器(唯一解验证) ---- */
static int t_nt;                        /* 树数 */
static int t_cand[TN_N * TN_N];         /* 候选格: 非树且 4 邻有树 */
static int t_cand_n;
static int t_sol_cnt;
static char t_best[TN_N * TN_N];        /* 求解盘面: 1=帐篷 */
static char t_keep[TN_N * TN_N];        /* 找到的首个完整解(搜索回溯会销毁 t_best) */
static char t_rowc[TN_N], t_colc[TN_N]; /* 求解中行/列帐篷计数 */

static void t_solve_rec(int k, int placed) {
    if (t_sol_cnt >= 2) return;
    if (placed == t_nt) {
        /* 完整校验: 每树恰 1 个 4 邻帐篷 */
        for (int r = 0; r < TN_N; r++) {
            for (int c = 0; c < TN_N; c++) {
                if (tn_trees[tn_puz][r][c] != 'T') continue;
                int t = 0;
                if (r > 0 && t_best[(r - 1) * TN_N + c]) t++;
                if (r < TN_N - 1 && t_best[(r + 1) * TN_N + c]) t++;
                if (c > 0 && t_best[r * TN_N + c - 1]) t++;
                if (c < TN_N - 1 && t_best[r * TN_N + c + 1]) t++;
                if (t != 1) return;
            }
        }
        /* 行/列帐篷数必须与边沿数字精确相等(搜索中只做了上限剪枝) */
        for (int rr = 0; rr < TN_N; rr++) {
            int tc = 0;
            for (int cc = 0; cc < TN_N; cc++)
                if (t_best[rr * TN_N + cc]) tc++;
            if (tc != (int)tn_rcnt[tn_puz][rr]) return;
        }
        for (int cc = 0; cc < TN_N; cc++) {
            int tc = 0;
            for (int rr = 0; rr < TN_N; rr++)
                if (t_best[rr * TN_N + cc]) tc++;
            if (tc != (int)tn_ccnt[tn_puz][cc]) return;
        }
        t_sol_cnt++;
        if (t_sol_cnt == 1) memcpy(t_keep, t_best, sizeof t_keep);
        return;
    }
    for (int i = k; i < t_cand_n; i++) {
        int cell = t_cand[i];
        int r = cell / TN_N, c = cell % TN_N;
        /* 8 邻不得有帐篷 */
        bool touch = false;
        for (int dr = -1; dr <= 1 && !touch; dr++) {
            for (int dc = -1; dc <= 1; dc++) {
                if (dr == 0 && dc == 0) continue;
                int nr = r + dr, nc = c + dc;
                if (nr < 0 || nr >= TN_N || nc < 0 || nc >= TN_N) continue;
                if (t_best[nr * TN_N + nc]) { touch = true; break; }
            }
        }
        if (touch) continue;
        /* 行/列计数不超上限 */
        if (t_rowc[r] >= (int)tn_rcnt[tn_puz][r]) continue;
        if (t_colc[c] >= (int)tn_ccnt[tn_puz][c]) continue;
        t_best[cell] = 1;
        t_rowc[r]++;
        t_colc[c]++;
        /* 任何树 4 邻帐篷数不得超过 1(配对规则只看 4 邻, 对角帐篷
         * 只受帐篷-帐篷不相触约束, 不影响树的配对计数) */
        bool ok = true;
        for (int dr = -1; dr <= 1 && ok; dr++) {
            for (int dc = -1; dc <= 1; dc++) {
                if (dr == 0 && dc == 0) continue;
                int nr = r + dr, nc = c + dc;
                if (nr < 0 || nr >= TN_N || nc < 0 || nc >= TN_N) continue;
                if (tn_trees[tn_puz][nr][nc] != 'T') continue;
                int t = 0;
                if (nr > 0 && t_best[(nr - 1) * TN_N + nc]) t++;
                if (nr < TN_N - 1 && t_best[(nr + 1) * TN_N + nc]) t++;
                if (nc > 0 && t_best[nr * TN_N + nc - 1]) t++;
                if (nc < TN_N - 1 && t_best[nr * TN_N + nc + 1]) t++;
                if (t > 1) { ok = false; break; }
            }
        }
        if (ok) t_solve_rec(i + 1, placed + 1);
        t_rowc[r]--;
        t_colc[c]--;
        t_best[cell] = 0;
        if (t_sol_cnt >= 2) return;
    }
}

static void test_unique(void) {
    for (int p = 0; p < TN_PUZZLES; p++) {
        tents_start_puz((uint8_t)p);
        t_nt = (int)tn_ntree;
        t_cand_n = 0;
        for (int r = 0; r < TN_N; r++) {
            for (int c = 0; c < TN_N; c++) {
                if (tn_is_tree(r, c)) continue;
                bool adj = false;
                for (int dr = -1; dr <= 1 && !adj; dr++) {
                    for (int dc = -1; dc <= 1; dc++) {
                        if (dr == 0 && dc == 0) continue;
                        int nr = r + dr, nc = c + dc;
                        if (nr < 0 || nr >= TN_N || nc < 0 || nc >= TN_N) continue;
                        if (tn_is_tree(nr, nc)) { adj = true; break; }
                    }
                }
                if (adj) t_cand[t_cand_n++] = r * TN_N + c;
            }
        }
        memset(t_best, 0, sizeof t_best);
        memset(t_rowc, 0, sizeof t_rowc);
        memset(t_colc, 0, sizeof t_colc);
        t_sol_cnt = 0;
        t_solve_rec(0, 0);
        CHECK(t_sol_cnt == 1, "puzzle has exactly one solution");
        bool match = true;
        for (int r = 0; r < TN_N && match; r++) {
            for (int c = 0; c < TN_N; c++) {
                bool expect = t_sol[p][r][c] == '^';
                if (expect != (t_keep[r * TN_N + c] != 0)) { match = false; break; }
            }
        }
        CHECK(match, "unique solution equals intended board");
    }
}

/* ---- 3. OK 循环状态机 ---- */
static void test_cycle(void) {
    tents_start_puz(0);
    tn_cx = 0;
    tn_cy = 0;
    tn_cycle();
    CHECK(tn_cell[0] == TN_TENT, "OK: empty -> tent");
    tn_cycle();
    CHECK(tn_cell[0] == TN_GRASS, "OK: tent -> grass");
    tn_cycle();
    CHECK(tn_cell[0] == TN_EMPTY, "OK: grass -> empty");
    tn_cx = 1;
    tn_cy = 1;                    /* PINE 树 */
    tn_cycle();
    CHECK(tn_cell[1 * TN_N + 1] == TN_EMPTY, "tree cell rejects cycle");
    tn_cx = 0;
    tn_cy = 0;
    tn_cycle();                   /* -> TENT */
    tn_set_empty();
    CHECK(tn_cell[0] == TN_EMPTY, "set_empty clears cell");
}

/* ---- 4. 胜负判定 ---- */
static void t_place_sol(void) {
    tents_start_puz(0);
    for (int r = 0; r < TN_N; r++)
        for (int c = 0; c < TN_N; c++)
            if (t_sol[0][r][c] == '^') tn_cell[r * TN_N + c] = TN_TENT;
}

static void test_solved(void) {
    t_place_sol();
    CHECK(tn_solved(), "full solution is solved");
    CHECK(tn_errors() == 0, "zero errors on solution");
    CHECK(tn_count_tents() == (int)tn_ntree, "tent count == tree count");
    /* 缺一顶 */
    tn_cell[0 * TN_N + 1] = TN_EMPTY;
    CHECK(!tn_solved(), "missing tent not solved");
    CHECK(tn_errors() > 0, "missing tent reported as error");
    CHECK(tn_tree_viol() == 1, "tree (1,1) loses its tent");
    /* 补回, 加一顶孤儿帐篷 (0,0) */
    tn_cell[0 * TN_N + 1] = TN_TENT;
    tn_cell[0 * TN_N + 0] = TN_TENT;
    CHECK(!tn_solved(), "extra orphan tent not solved");
    CHECK(tn_tent_orphan() == 1, "orphan tent detected");
    CHECK(tn_count_viol() >= 2, "row+col overcount detected");
    tn_cell[0 * TN_N + 0] = TN_EMPTY;
    /* 相触: 两顶 8 邻帐篷 */
    tn_cell[0 * TN_N + 0] = TN_TENT;   /* 与 (0,1) 横向相触 */
    CHECK(tn_touch_pairs() == 1, "touching pair detected");
    CHECK(!tn_solved(), "touching tents not solved");
    tn_cell[0 * TN_N + 0] = TN_EMPTY;
    /* 草不干扰胜负 */
    tn_cell[0 * TN_N + 0] = TN_GRASS;
    CHECK(tn_solved(), "grass does not break a solution");
    tn_cell[0 * TN_N + 0] = TN_EMPTY;
    /* 纯孤儿盘面: 单顶帐篷不邻树 */
    memset(tn_cell, 0, sizeof tn_cell);
    tn_cell[0 * TN_N + 0] = TN_TENT;
    CHECK(!tn_solved(), "lone orphan tent not solved");
    CHECK(tn_tent_orphan() == 1, "lone orphan detected");
}

/* ---- 5. on_key 全流程 ---- */
static void test_keys(void) {
    tents_start_puz(0);
    key_event_t ev;
    ev.ch = 0;
    /* 确认键重复被忽略 */
    ev.key = K_OK;
    ev.is_repeat = true;
    tents_on_key(&ev);
    CHECK(tn_cell[0] == TN_EMPTY, "repeat OK ignored");
    ev.is_repeat = false;
    tents_on_key(&ev);
    CHECK(tn_cell[0] == TN_TENT, "OK places tent");
    /* 方向键重复可用 */
    ev.key = K_RIGHT;
    ev.is_repeat = true;
    tents_on_key(&ev);
    CHECK(tn_cx == 1, "repeat RIGHT moves cursor");
    /* 边界钳制 */
    ev.is_repeat = false;
    for (int i = 0; i < 20; i++) { ev.key = K_RIGHT; tents_on_key(&ev); }
    CHECK(tn_cx == TN_N - 1, "cursor clamps at right edge");
    ev.key = K_DOWN;
    tents_on_key(&ev);
    CHECK(tn_cy == 1, "DOWN moves cursor");
    for (int i = 0; i < 20; i++) { ev.key = K_DOWN; tents_on_key(&ev); }
    CHECK(tn_cy == TN_N - 1, "cursor clamps at bottom edge");
    for (int i = 0; i < 20; i++) { ev.key = K_UP; tents_on_key(&ev); }
    CHECK(tn_cy == 0, "cursor clamps at top edge");
    for (int i = 0; i < 20; i++) { ev.key = K_LEFT; tents_on_key(&ev); }
    CHECK(tn_cx == 0, "cursor clamps at left edge");
    /* WASD */
    ev.key = K_CHAR;
    ev.ch = 's';
    tents_on_key(&ev);
    CHECK(tn_cy == 1, "s moves down");
    ev.ch = 'd';
    tents_on_key(&ev);
    CHECK(tn_cx == 1, "d moves right");
    ev.ch = 'w';
    tents_on_key(&ev);
    CHECK(tn_cy == 0, "w moves up");
    ev.ch = 'a';
    tents_on_key(&ev);
    CHECK(tn_cx == 0, "a moves left");
    /* 'x' 与 DEL 清格 */
    ev.key = K_OK;
    tents_on_key(&ev);            /* (0,0) -> TENT */
    ev.key = K_CHAR;
    ev.ch = 'x';
    tents_on_key(&ev);
    CHECK(tn_cell[0] == TN_EMPTY, "x clears cell");
    ev.key = K_OK;
    tents_on_key(&ev);            /* -> TENT */
    ev.key = K_DEL;
    ev.ch = 0;
    tents_on_key(&ev);
    CHECK(tn_cell[0] == TN_EMPTY, "DEL clears cell");
    /* 'n' 换题 + 环绕 */
    ev.key = K_CHAR;
    ev.ch = 'n';
    tents_on_key(&ev);
    CHECK(tn_puz == 1 && tn_cx == 0 && tn_cy == 0, "n advances to next puzzle");
    tents_start_puz(2);
    ev.ch = 'n';
    tents_on_key(&ev);
    CHECK(tn_puz == 0, "puzzle wraps 2 -> 0");
}

/* ---- 5b. 解后流程 ---- */
static void test_solve_flow(void) {
    tents_start_puz(0);
    key_event_t ev = { K_OK, 0, false };
    /* 用 on_key 摆放 PINE 全部 6 顶帐篷(光标直接定位) */
    static const int sol_rc[6][2] = {
        { 0, 1 }, { 0, 5 }, { 2, 3 }, { 4, 4 }, { 6, 2 }, { 6, 5 }
    };
    for (int i = 0; i < 6; i++) {
        tn_cx = sol_rc[i][1];
        tn_cy = sol_rc[i][0];
        ev.key = K_OK;
        tents_on_key(&ev);
    }
    CHECK(tn_over, "all tents placed via keys -> over");
    CHECK(tn_errors() == 0, "zero errors when solved via keys");
    /* 解后 OK -> 下一题 */
    ev.key = K_OK;
    tents_on_key(&ev);
    CHECK(tn_puz == 1 && !tn_over, "solved OK -> next puzzle");
    /* 解后 BACK / Q -> 退出 */
    t_place_sol();
    tn_over = true;
    s_exit_request = false;
    ev.key = K_BACK;
    tents_on_key(&ev);
    CHECK(s_exit_request, "solved BACK quits");
    s_exit_request = false;
    ev.key = K_QUIT;
    tents_on_key(&ev);
    CHECK(s_exit_request, "solved Q quits");
    /* 游戏中 Q -> 退出 */
    tents_start_puz(0);
    s_exit_request = false;
    ev.key = K_QUIT;
    tents_on_key(&ev);
    CHECK(s_exit_request, "in-game Q quits");
}

/* ---- 6. 渲染冒烟 ---- */
static int t_px(int x, int y) {
    if (x < 0 || x >= (int)CCG_W || y < 0 || y >= (int)CCG_H) return -1;
    return (g_fb[(y >> 3) * (int)CCG_W + x] & (0x80 >> (y & 7))) ? 1 : 0;
}

static int t_region_black(int x0, int y0, int x1, int y1) {
    int n = 0;
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++)
            if (t_px(x, y)) n++;
    return n;
}

static void test_render(void) {
    tents_start_puz(0);
    tents_render();
    /* 光标 (0,0) 空格: 黑边外框, 格内白 */
    CHECK(t_px(TN_GRID_X - 2, TN_GRID_Y - 2) == 1, "cursor corner black on empty");
    CHECK(t_px(TN_GRID_X + 1, TN_GRID_Y + 1) == 0, "empty cell interior white");
    /* 树方块 (1,1) 实心 */
    CHECK(t_px(TN_GRID_X + 1 * TN_CELL + 9, TN_GRID_Y + 1 * TN_CELL + 9) == 1,
          "tree square black");
    /* 格线与 HUD 分隔线 */
    CHECK(t_px(TN_GRID_X, TN_GRID_Y) == 1, "grid line corner black");
    CHECK(t_px(100, CCG_HUD_H - 1) == 1, "HUD underline black");
    /* 列/行提示数字占位(数字字符在网格外沿绘制, 非空白) */
    CHECK(t_region_black(TN_GRID_X - 10, TN_HINT_Y, TN_GRID_X + TN_GRID + 10,
                         TN_HINT_Y + 6) > 0, "column hints drawn");
    CHECK(t_region_black(TN_HINT_X - 20, TN_GRID_Y, TN_HINT_X + 20,
                         TN_GRID_Y + TN_GRID - 1) > 0, "row hints drawn");
    /* 帐篷: 顶点黑、顶点行两侧白 */
    tents_start_puz(0);
    tn_cx = 0;
    tn_cy = 0;
    tn_cycle();                   /* (0,0) -> TENT */
    tents_render();
    CHECK(t_px(TN_GRID_X + 9, TN_GRID_Y + 2) == 1, "tent apex black");
    CHECK(t_px(TN_GRID_X + 3, TN_GRID_Y + 2) == 0, "tent apex row flank white");
    CHECK(t_px(TN_GRID_X - 2, TN_GRID_Y - 2) == 0, "cursor white border over tent");
    /* 草标记: 中间点黑, 上沿白 */
    tn_cycle();                   /* -> GRASS */
    tents_render();
    CHECK(t_px(TN_GRID_X + 9, TN_GRID_Y + 9) == 1, "grass dot black");
    CHECK(t_px(TN_GRID_X - 2, TN_GRID_Y - 2) == 1, "cursor black over grass");
    /* 解后: HUD 清底(无分隔线) + SOLVED 文本 */
    t_place_sol();
    tn_over = true;
    tents_render();
    CHECK(t_px(100, CCG_HUD_H - 1) == 0, "solved HUD underline cleared");
    CHECK(t_region_black(2, 2, 64, 8) > 0, "SOLVED text drawn in HUD");
    CHECK(tn_over_full, "full-refresh flag set on first solved render");
}

int main(void) {
    test_data();
    test_unique();
    test_cycle();
    test_solved();
    test_keys();
    test_solve_flow();
    test_render();
    if (s_fail == 0) { printf("ALL TESTS PASSED\n"); return 0; }
    printf("%d TEST(S) FAILED\n", s_fail);
    return 1;
}
