/* NUMBERLINK host 逻辑测试: 谜题数据 / 设计解 / 独立回溯求解 / 游戏规则
 * 链接 canvas font font_data pattern rng time ui_common input display (-DCHICHU_HOST) */
#include "../src/config.h"
#include "../src/gfx/canvas.h"
#include "../src/gfx/pattern.h"
#include "../src/rng.h"
#include "../src/games/numberlink.c"
#include <stdio.h>
#include <string.h>

static int s_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); s_fail++; } \
    else { printf("ok: %s\n", msg); } \
} while (0)

/* host stub */
bool s_exit_request = false;
void game_set_tick_interval(uint32_t ms) { (void)ms; }

/* 导出函数(测试经 on_key 驱动) */
void numberlink_on_key(const key_event_t *ev);
void numberlink_render(void);

/* ---- 设计解完整盘面(手工设计, 全填充) ---- */
static const uint8_t sol[3][48] = {
    /* L1: 条状分隔区 */
    { 1,1,1,4,4,2,2,2,
      3,3,1,6,4,2,5,5,
      3,3,1,6,4,2,5,5,
      3,3,1,6,4,2,5,5,
      3,3,1,6,4,2,5,5,
      3,3,1,6,6,2,5,5 },
    /* L2: 螺旋 */
    { 1,1,1,1,1,1,1,1,
      3,4,4,4,4,4,4,2,
      3,5,6,6,6,6,4,2,
      3,5,6,6,6,6,4,2,
      3,5,5,5,5,5,5,2,
      3,3,3,3,2,2,2,2 },
    /* L3: 中区交错 */
    { 1,1,3,3,4,4,2,2,
      1,1,5,3,6,4,2,2,
      1,1,5,3,6,4,2,2,
      1,1,5,3,6,4,2,2,
      1,1,5,3,6,4,2,2,
      1,1,5,5,6,6,2,2 },
};

static int sol_connected(const uint8_t *s, int p) {
    int a = -1, b = -1;
    for (int i = 0; i < 48; i++)
        if (s[i] == p) { if (a < 0) a = i; else b = i; }
    if (a < 0 || b < 0) return 0;
    uint8_t seen[48];
    uint8_t q[48];
    int qh = 0, qt = 0;
    memset(seen, 0, 48);
    seen[a] = 1;
    q[qt++] = (uint8_t)a;
    while (qh < qt) {
        int c = q[qh++];
        if (c == b) return 1;
        int x = c % 8, y = c / 8;
        if (y > 0 && !seen[c - 8] && s[c - 8] == p) { seen[c - 8] = 1; q[qt++] = (uint8_t)(c - 8); }
        if (y < 5 && !seen[c + 8] && s[c + 8] == p) { seen[c + 8] = 1; q[qt++] = (uint8_t)(c + 8); }
        if (x > 0 && !seen[c - 1] && s[c - 1] == p) { seen[c - 1] = 1; q[qt++] = (uint8_t)(c - 1); }
        if (x < 7 && !seen[c + 1] && s[c + 1] == p) { seen[c + 1] = 1; q[qt++] = (uint8_t)(c + 1); }
    }
    return 0;
}

static void test_puzzle_data(void) {
    for (int lvl = 0; lvl < NL_LEVELS; lvl++) {
        for (int i = 0; i < NL_PAIRS * 2; i++) {
            CHECK(nl_puzzles[lvl][i] < NL_CELLS, "endpoint in range");
            for (int j = i + 1; j < NL_PAIRS * 2; j++)
                CHECK(nl_puzzles[lvl][i] != nl_puzzles[lvl][j], "endpoints distinct");
        }
        for (int p = 0; p < NL_PAIRS; p++) {
            int a = nl_puzzles[lvl][p * 2], b = nl_puzzles[lvl][p * 2 + 1];
            int dist = (a / 8 > b / 8 ? a / 8 - b / 8 : b / 8 - a / 8) +
                       (a % 8 > b % 8 ? a % 8 - b % 8 : b % 8 - a % 8);
            CHECK(dist > 1, "pair endpoints not adjacent (no trivial pair)");
        }
    }
}

static void test_design_solutions(void) {
    for (int lvl = 0; lvl < NL_LEVELS; lvl++) {
        const uint8_t *s = sol[lvl];
        int covered = 0;
        for (int c = 0; c < 48; c++)
            if (s[c] >= 1 && s[c] <= 6) covered++;
        CHECK(covered == 48, "design: full cover");
        for (int p = 1; p <= 6; p++) {
            CHECK(s[nl_puzzles[lvl][(p - 1) * 2]] == p &&
                  s[nl_puzzles[lvl][(p - 1) * 2 + 1]] == p, "design: endpoints match");
            CHECK(sol_connected(s, p), "design: pair connected");
        }
    }
}

/* ---- 独立回溯求解器: 从端点数据出发找全填充解 ---- */
static const uint8_t *s_ep;
static int s_nodes;
static int s_cap;

static int has_empty_nbr(const uint8_t *g, int c) {
    int x = c % 8, y = c / 8;
    if (y > 0 && g[c - 8] == 0) return 1;
    if (y < 5 && g[c + 8] == 0) return 1;
    if (x > 0 && g[c - 1] == 0) return 1;
    if (x < 7 && g[c + 1] == 0) return 1;
    return 0;
}

/* 孤立空格的剪枝: 无空格邻居且不在任何未完成路径头的旁边 → 永远填不上 */
static int stranded(const uint8_t *g, const uint8_t *head, const uint8_t *done) {
    for (int c = 0; c < 48; c++) {
        if (g[c] != 0) continue;
        if (has_empty_nbr(g, c)) continue;
        int reach = 0;
        for (int p = 0; p < NL_PAIRS; p++) {
            if (done[p]) continue;
            int h = head[p];
            int hx = h % 8, hy = h / 8;
            if ((hx > 0 && h - 1 == c) || (hx < 7 && h + 1 == c) ||
                (hy > 0 && h - 8 == c) || (hy < 5 && h + 8 == c)) { reach = 1; break; }
        }
        if (!reach) return 1;
    }
    return 0;
}

static int solve_rec(uint8_t *g, const uint8_t *head, const uint8_t *done, int empty,
                     uint8_t *out) {
    if (++s_nodes > s_cap) return -1;
    if (empty == 0) {
        /* 全填满: 若全部完成则成功(此时在回溯解绑前拷出解);
         * 否则还有"走上端点"的结束步可走, 落入下方 */
        int alldone = 1;
        for (int p = 0; p < NL_PAIRS; p++)
            if (!done[p]) { alldone = 0; break; }
        if (alldone) { memcpy(out, g, 48); return 1; }
    }
    if (stranded(g, head, done)) return 0;
    /* MRV: 挑可走步数最少的未完成路径头 */
    uint8_t mv[NL_PAIRS][4];   /* 内部格可达 4 邻 */
    int mn[NL_PAIRS];
    int best = -1, best_n = 99;
    for (int p = 0; p < NL_PAIRS; p++) {
        mn[p] = 0;
        if (done[p]) continue;
        int h = head[p];
        int hx = h % 8, hy = h / 8;
        int cand[4];
        int k = 0;
        if (hy > 0) cand[k++] = h - 8;
        if (hy < 5) cand[k++] = h + 8;
        if (hx > 0) cand[k++] = h - 1;
        if (hx < 7) cand[k++] = h + 1;
        for (int i = 0; i < k; i++) {
            int c = cand[i];
            if (g[c] == 0) mv[p][mn[p]++] = (uint8_t)c;
            else if (g[c] == p + 1 && c == s_ep[p * 2 + 1]) mv[p][mn[p]++] = (uint8_t)c;
        }
        if (mn[p] == 0) return 0;
        if (mn[p] < best_n) { best_n = mn[p]; best = p; }
    }
    if (best < 0) return 0;
    for (int i = 0; i < best_n; i++) {
        int c = mv[best][i];
        int was_empty = (g[c] == 0);
        g[c] = (uint8_t)(best + 1);
        uint8_t h2[6], d2[6];
        memcpy(h2, head, 6);
        memcpy(d2, done, 6);
        if (c == s_ep[best * 2 + 1]) d2[best] = 1; else h2[best] = (uint8_t)c;
        int r = solve_rec(g, h2, d2, empty - (was_empty ? 1 : 0), out);
        g[c] = (uint8_t)(was_empty ? 0 : best + 1);
        if (r == 1) return 1;
        if (r == -1) return -1;
    }
    return 0;
}

/* 返回 1 找到全填充解; 0 无解; -1 超节点上限 */
static int solve_puzzle(int lvl, uint8_t *out) {
    uint8_t g[48];
    memset(g, 0, 48);
    for (int i = 0; i < NL_PAIRS * 2; i++) g[nl_puzzles[lvl][i]] = (uint8_t)(i / 2 + 1);
    uint8_t head[6], done[6];
    int empty = 0;
    for (int p = 0; p < NL_PAIRS; p++) { head[p] = nl_puzzles[lvl][p * 2]; done[p] = 0; }
    for (int c = 0; c < 48; c++) if (!g[c]) empty++;
    s_ep = nl_puzzles[lvl];
    s_nodes = 0;
    s_cap = 400000;
    int r = solve_rec(g, head, done, empty, out);
    return r;
}

static void test_solver_all(void) {
    for (int lvl = 0; lvl < NL_LEVELS; lvl++) {
        uint8_t out[48];
        int r = solve_puzzle(lvl, out);
        char msg[64];
        snprintf(msg, sizeof msg, "solver L%d finds full-cover solution", lvl + 1);
        if (r == 1) {
            /* 求出的解必须是合法解: 全填充 + 每对连通 */
            int covered = 0, ok = 1;
            for (int c = 0; c < 48; c++)
                if (out[c] >= 1 && out[c] <= 6) covered++; else ok = 0;
            for (int p = 1; p <= 6; p++)
                if (!sol_connected(out, p)) ok = 0;
            CHECK(ok && covered == 48, msg);
            printf("  (L%d solver nodes=%d)\n", lvl + 1, s_nodes);
        } else {
            snprintf(msg, sizeof msg, "solver L%d failed r=%d", lvl + 1, r);
            CHECK(0, msg);
        }
    }
}

/* ---- 按键驱动(走游戏 API) ---- */
static void press(ccg_key k) {
    key_event_t ev;
    ev.key = k;
    ev.ch = 0;
    ev.is_repeat = false;
    numberlink_on_key(&ev);
}

static void press_repeat(ccg_key k) {
    key_event_t ev;
    ev.key = k;
    ev.ch = 0;
    ev.is_repeat = true;
    numberlink_on_key(&ev);
}

static void press_char(char ch) {
    key_event_t ev;
    ev.key = K_CHAR;
    ev.ch = (uint8_t)ch;
    ev.is_repeat = false;
    numberlink_on_key(&ev);
}

/* 从当前光标端点开始: OK 起笔, 按 dirs 走到另一端(自动连接) */
static void walk(const char *dirs) {
    press(K_OK);
    for (const char *d = dirs; *d; d++) {
        switch (*d) {
        case 'R': press(K_RIGHT); break;
        case 'L': press(K_LEFT); break;
        case 'U': press(K_UP); break;
        case 'D': press(K_DOWN); break;
        default: break;
        }
    }
}

/* 3 关的全部解走法(方向串 + 起点格) */
static const char *walk_dirs[3][6] = {
    { "RRDDDDD", "LLDDDDD", "RDLDRDLDR", "RDDDD", "RDLDRDLDR", "DDDDR" },
    { "RRRRRRR", "DDDDLLL", "LLLUUUU", "RRRRRDD", "LLLLLUU", "DRURDRU" },
    { "RDLDRDLDRDL", "RDLDRDLDRDL", "RDDDD", "RDDDD", "DDDDR", "DDDDR" },
};
static const uint8_t walk_start[3][6] = {
    { 0, 7, 8, 3, 14, 11 },
    { 0, 15, 43, 9, 38, 18 },
    { 0, 6, 2, 4, 10, 12 },
};

static void solve_with_keys(int lvl) {
    nl_start((uint8_t)lvl);
    for (int p = 0; p < NL_PAIRS; p++) {
        nl_cur = walk_start[lvl][p];
        walk(walk_dirs[lvl][p]);
        CHECK(nl_pairs_done() == p + 1, "pairs done count during solve");
    }
}

static void test_game_solve_all(void) {
    solve_with_keys(0);
    CHECK(nl_over, "L1 solved, over set");
    CHECK(nl_full_cover(), "L1 full cover after solve");
    solve_with_keys(1);
    CHECK(nl_over, "L2 solved");
    CHECK(nl_full_cover(), "L2 full cover after solve");
    solve_with_keys(2);
    CHECK(nl_over, "L3 solved");
    CHECK(nl_full_cover(), "L3 full cover after solve");
}

static void test_game_rules(void) {
    nl_start(0);
    CHECK(!nl_over && nl_pairs_done() == 0, "fresh board: 0 pairs, not over");
    /* WASD 字母移动 */
    press_char('d');
    CHECK(nl_cur == 1, "char d moves cursor");
    press_char('a');
    CHECK(nl_cur == 0, "char a moves cursor");
    press_char('w');
    CHECK(nl_cur == 0, "char w clamped at top edge");
    /* 重复键: 方向可重复, OK 忽略重复 */
    press_repeat(K_RIGHT);
    CHECK(nl_cur == 1, "repeat arrow moves cursor");
    press_repeat(K_OK);
    CHECK(!nl_drawing, "repeat OK ignored");
    /* 越界钳制 */
    for (int i = 0; i < 20; i++) press(K_UP);
    CHECK(nl_cur == 1, "cursor clamped at top edge");
    /* 画线: DEL 退格 */
    nl_cur = 0;
    press(K_OK);
    CHECK(nl_drawing && nl_plen == 1, "OK starts drawing");
    press(K_RIGHT);
    press(K_RIGHT);
    CHECK(nl_plen == 3 && nl_cur == 2, "path extended");
    CHECK(nl_grid[1] == 1 && nl_grid[2] == 1, "path cells marked");
    press(K_DEL);
    CHECK(nl_plen == 2 && nl_cur == 1 && nl_grid[2] == 0, "DEL retreats one");
    press(K_DEL);
    CHECK(nl_plen == 1 && nl_cur == 0, "DEL to start");
    press(K_DEL);
    CHECK(!nl_drawing, "DEL at start cancels drawing");
    /* OK 中途 → 路径作废 (画 R,D,D → (0,1),(1,1),(2,1)) */
    press(K_OK);
    press(K_RIGHT);
    press(K_DOWN);
    press(K_DOWN);
    CHECK(nl_plen == 4, "path len 4");
    press(K_OK);
    CHECK(!nl_drawing && nl_plen == 0, "OK mid-path discards");
    CHECK(nl_grid[1] == 0 && nl_grid[9] == 0 && nl_grid[17] == 0 &&
          nl_pairs_done() == 0, "discard cleared");
    /* 阻挡: 走到别的配对格/路径格被拦 */
    nl_cur = 0;
    walk("RRDDDDD");                       /* 配对1 完成 */
    CHECK(nl_pairs_done() == 1, "pair1 committed");
    nl_cur = 7;                            /* 配对2 起点 (0,7) */
    press(K_OK);
    press(K_LEFT);                         /* (0,6) 空 */
    press(K_LEFT);                         /* (0,5) 空 */
    press(K_LEFT);                         /* (0,4) 空 */
    press(K_DOWN);                         /* (1,4) 空 */
    press(K_DOWN);                         /* (2,4) 空 */
    press(K_LEFT);                         /* (2,3) 空 */
    press(K_LEFT);                         /* (2,2) 是配对1路径 → 拦 */
    CHECK(nl_cur == 2 * 8 + 3 && nl_grid[2 * 8 + 2] == 1, "blocked by other pair path");
    for (int i = 0; i < 7; i++) press(K_DEL);   /* 退回起点后取消 */
    CHECK(!nl_drawing, "retreat chain cancels");
    /* DEL 撤销上一条完成路径 */
    CHECK(nl_last_pair == 1, "last pair recorded");
    press(K_DEL);
    CHECK(nl_pairs_done() == 0 && nl_last_pair == 0, "DEL in idle undoes last path");
    CHECK(nl_grid[0] == 1 && nl_grid[42] == 1, "endpoints remain fixed");
    CHECK(nl_grid[1] == 0 && nl_grid[10] == 0, "path cells cleared");
    /* 已连接配对: OK 重画(清旧路径) */
    nl_cur = 0;
    walk("RRDDDDD");
    CHECK(nl_pairs_done() == 1, "pair1 redone");
    nl_cur = 42;
    press(K_OK);
    CHECK(nl_drawing && nl_pairs_done() == 0, "redraw clears old path");
    CHECK(nl_grid[1] == 0, "old path cells gone");
    press(K_DEL);
    CHECK(!nl_drawing, "redraw cancelled by DEL");
    /* 从另一端也能画(对称性): (5,2) 起笔 */
    nl_cur = 42;
    walk("UUUUULL");                       /* (4,2)(3,2)(2,2)(1,2)(0,2)(0,1)(0,0) */
    CHECK(nl_pairs_done() == 1, "pair1 drawn from other end");
}

static void test_game_over_keys(void) {
    solve_with_keys(0);
    CHECK(nl_over, "over state");
    /* 结束态: BACK 退出 */
    s_exit_request = false;
    press(K_BACK);
    CHECK(s_exit_request, "over: BACK requests exit");
    /* 结束态: OK 重开本关 */
    s_exit_request = false;
    nl_start(0);
    solve_with_keys(0);
    press(K_OK);
    CHECK(!nl_over && nl_pairs_done() == 0 && nl_level == 0, "over: OK retries same level");
    /* 游戏内 N 换关 */
    press_char('n');
    CHECK(nl_level == 1 && nl_pairs_done() == 0, "N advances to next level");
    press_char('n');
    press_char('n');
    CHECK(nl_level == 0, "N cycles back to level 1");
}

int main(void) {
    printf("== puzzle data ==\n");
    test_puzzle_data();
    printf("== design solutions ==\n");
    test_design_solutions();
    printf("== backtracking solver ==\n");
    test_solver_all();
    printf("== game solve via keys ==\n");
    test_game_solve_all();
    printf("== rules ==\n");
    test_game_rules();
    printf("== over-state keys ==\n");
    test_game_over_keys();
    if (s_fail) {
        printf("%d FAILURE(S)\n", s_fail);
        return 1;
    }
    printf("ALL PASS\n");
    return 0;
}
