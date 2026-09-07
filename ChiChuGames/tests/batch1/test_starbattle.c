/* Star Battle 逻辑单测 — host cc 编译运行
 * 覆盖: ROM 数据(区域划分/唯一解/题面星)独立验证 + 游戏逻辑(胜负/计数/键位) */
#include "../../src/games/starbattle.c"
#include <stdio.h>
#include <string.h>

bool s_exit_request = false;
void game_set_tick_interval(uint32_t ms) { (void)ms; }

static int s_fail = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); s_fail++; } \
    else { printf("ok: %s\n", msg); } \
} while (0)

/* ---- 独立求解器(行主序回溯, 与游戏代码完全独立) ---- */
static uint8_t s_work[SB_NCELL];
static uint8_t s_capture[SB_NCELL];
static int s_count;
static int s_colcnt[SB_N];
static int s_regcnt[SB_N];

static void solve_rec(int r) {
    int a, b, j;
    int fixed[2], nf = 0;
    if (s_count > 1) return;
    if (r == SB_N) {
        s_count++;
        if (s_count == 1) memcpy(s_capture, s_work, sizeof s_work);
        return;
    }
    for (a = 0; a < SB_N; a++)
        if (s_work[r * SB_N + a]) { fixed[nf++] = a; }
    for (a = 0; a < SB_N; a++)
        for (b = a + 1; b < SB_N; b++) {
            if (nf == 2 && !(a == fixed[0] && b == fixed[1])) continue;
            if (nf == 1 && !(a == fixed[0] || b == fixed[0])) continue;
            if (b == a + 1) continue;               /* 行内相邻 */
            if (r > 0) {                            /* 与上一行相邻(含对角) */
                int pa = -1, pb = -1;
                for (j = 0; j < SB_N; j++)
                    if (s_work[(r - 1) * SB_N + j]) {
                        if (pa < 0) pa = j; else pb = j;
                    }
                if (a >= pa - 1 && a <= pa + 1) continue;
                if (b >= pa - 1 && b <= pa + 1) continue;
                if (a >= pb - 1 && a <= pb + 1) continue;
                if (b >= pb - 1 && b <= pb + 1) continue;
            }
            if (s_colcnt[a] + 1 > 2 || s_colcnt[b] + 1 > 2) continue;
            {
                int ra = (int)(sb_region[r * SB_N + a] - 'A');
                int rb = (int)(sb_region[r * SB_N + b] - 'A');
                /* 同行同区域两星: 计数 +2, 上限仍为 2 */
                if (ra == rb) {
                    if (s_regcnt[ra] + 2 > 2) continue;
                    s_regcnt[ra] += 2;
                } else {
                    if (s_regcnt[ra] + 1 > 2 || s_regcnt[rb] + 1 > 2) continue;
                    s_regcnt[ra]++;
                    s_regcnt[rb]++;
                }
                s_colcnt[a]++; s_colcnt[b]++;
                s_work[r * SB_N + a] = 1;
                s_work[r * SB_N + b] = 1;
                solve_rec(r + 1);
                s_work[r * SB_N + a] = 0;
                s_work[r * SB_N + b] = 0;
                s_colcnt[a]--; s_colcnt[b]--;
                if (ra == rb) s_regcnt[ra] -= 2;
                else { s_regcnt[ra]--; s_regcnt[rb]--; }
            }
            if (s_count > 1) return;
        }
}

static int count_solutions(const uint8_t *given) {
    int i;
    memset(s_work, 0, sizeof s_work);
    for (i = 0; i < SB_NCELL; i++)
        if (given[i]) s_work[i] = 1;
    memset(s_colcnt, 0, sizeof s_colcnt);
    memset(s_regcnt, 0, sizeof s_regcnt);
    s_count = 0;
    solve_rec(0);
    return s_count;
}

static int capture_matches_sol(void) {
    int i;
    for (i = 0; i < SB_NCELL; i++)
        if ((s_capture[i] != 0) != (sb_sol[i] == '#')) return 0;
    return 1;
}

/* ---- 1. 区域划分: 64 格全覆盖, 每区域 8 格, 4-连通 ---- */
static void test_tiling(void) {
    int cnt[SB_N], bad = 0, i, k;
    memset(cnt, 0, sizeof cnt);
    for (i = 0; i < SB_NCELL; i++) {
        char ch = sb_region[i];
        if (ch < 'A' || ch > 'H') bad++;
        else cnt[ch - 'A']++;
    }
    CHECK(bad == 0, "region chars all in A-H");
    for (i = 0; i < SB_N; i++)
        CHECK(cnt[i] == SB_N, "each region has 8 cells");
    for (k = 0; k < SB_N; k++) {
        int start = -1;
        int stack[SB_NCELL], seen[SB_NCELL];
        int top = 0, n = 0;
        for (i = 0; i < SB_NCELL; i++)
            if (sb_region[i] - 'A' == k) { start = i; break; }
        memset(seen, 0, sizeof seen);
        seen[start] = 1;
        stack[top++] = start;
        while (top > 0) {
            static const int dir[4][2] = { {1,0},{-1,0},{0,1},{0,-1} };
            int idx = stack[--top];
            int r = idx >> 3, c = idx & 7, d;
            n++;
            for (d = 0; d < 4; d++) {
                int nr = r + dir[d][0], nc = c + dir[d][1], nidx;
                if (nr < 0 || nr >= SB_N || nc < 0 || nc >= SB_N) continue;
                nidx = nr * SB_N + nc;
                if (seen[nidx]) continue;
                if (sb_region[nidx] - 'A' != k) continue;
                seen[nidx] = 1;
                stack[top++] = nidx;
            }
        }
        CHECK(n == SB_N, "region is 4-connected");
    }
}

/* ---- 2. 唯一解: 16 星, 行/列/区域各 2, 无相邻星 ---- */
static void test_solution(void) {
    int i, r, c, k, n, bad;
    CHECK(strlen(sb_sol) == 64, "solution length 64");
    n = 0;
    for (i = 0; i < SB_NCELL; i++)
        if (sb_sol[i] == '#') n++;
    CHECK(n == 16, "solution has 16 stars");
    for (r = 0; r < SB_N; r++) {
        n = 0;
        for (c = 0; c < SB_N; c++)
            if (sb_sol[r * SB_N + c] == '#') n++;
        CHECK(n == 2, "each row has 2 stars");
    }
    for (c = 0; c < SB_N; c++) {
        n = 0;
        for (r = 0; r < SB_N; r++)
            if (sb_sol[r * SB_N + c] == '#') n++;
        CHECK(n == 2, "each col has 2 stars");
    }
    for (k = 0; k < SB_N; k++) {
        n = 0;
        for (i = 0; i < SB_NCELL; i++)
            if (sb_sol[i] == '#' && sb_region[i] - 'A' == k) n++;
        CHECK(n == 2, "each region has 2 stars");
    }
    bad = 0;
    for (r = 0; r < SB_N; r++)
        for (c = 0; c < SB_N; c++) {
            int dr, dc;
            if (sb_sol[r * SB_N + c] != '#') continue;
            for (dr = -1; dr <= 1; dr++)
                for (dc = -1; dc <= 1; dc++) {
                    int nr = r + dr, nc = c + dc;
                    if (dr == 0 && dc == 0) continue;
                    if (nr < 0 || nr >= SB_N || nc < 0 || nc >= SB_N) continue;
                    if (sb_sol[nr * SB_N + nc] == '#') bad++;
                }
        }
    CHECK(bad == 0, "no adjacent stars in solution");
}

/* ---- 3. 题面星 + 唯一性: 每题仅一个合法完成且等于 ROM 解 ---- */
static void test_givens(void) {
    uint8_t g[SB_NCELL];
    int p, j, i, bad;
    for (p = 0; p < SB_PUZ_N; p++) {
        memset(g, 0, sizeof g);
        for (j = 0; j < SB_PUZ_N && sb_giv[p][j] != 255; j++) {
            int idx = (int)sb_giv[p][j];
            CHECK(idx >= 0 && idx < SB_NCELL, "given index in range");
            CHECK(sb_sol[idx] == '#', "given is a solution star");
            g[idx] = 1;
        }
        bad = 0;
        for (i = 0; i < SB_NCELL; i++) {
            int dr, dc;
            if (!g[i]) continue;
            for (dr = -1; dr <= 1; dr++)
                for (dc = -1; dc <= 1; dc++) {
                    int nr = (i >> 3) + dr, nc = (i & 7) + dc;
                    if (dr == 0 && dc == 0) continue;
                    if (nr < 0 || nr >= SB_N || nc < 0 || nc >= SB_N) continue;
                    if (g[nr * SB_N + nc]) bad++;
                }
        }
        CHECK(bad == 0, "givens not mutually adjacent");
        CHECK(count_solutions(g) == 1, "puzzle has unique completion");
        CHECK(capture_matches_sol(), "unique completion equals ROM solution");
    }
    memset(g, 0, sizeof g);
    CHECK(count_solutions(g) == 1, "empty board also unique");
    CHECK(capture_matches_sol(), "empty completion equals ROM solution");
}

/* ---- 4. 游戏逻辑 ---- */
static void test_game_logic(void) {
    int i, r, c;
    key_event_t ev;

    /* 新局: 题面星预置并锁定 */
    sb_new_game(0);
    CHECK(!sb_over, "new game not over");
    CHECK(!sb_check_win(), "empty-ish board not win");
    for (i = 0; i < SB_NCELL; i++) {
        if (sb_given[i]) {
            CHECK(sb_star[i] == 1, "given star pre-placed");
        } else {
            CHECK(sb_star[i] == 0, "non-given cell empty");
        }
    }
    CHECK(sb_count_line(0, (int)sb_giv[0][0] >> 3) == 1, "row count 1");
    CHECK(sb_count_line(1, (int)(sb_giv[0][0] & 7)) == 1, "col count 1");
    CHECK(sb_count_line(2, (int)(sb_region[sb_giv[0][0]] - 'A')) == 1,
          "region count 1");
    {
        int g = (int)sb_giv[0][0];
        uint8_t before = sb_star[g];
        sb_toggle(g);
        CHECK(sb_star[g] == before, "given protected from toggle");
        sb_clear(g);
        CHECK(sb_star[g] == before, "given protected from clear");
    }

    /* 光标边界 + WASD */
    sb_cx = 0;
    sb_cy = 0;
    memset(&ev, 0, sizeof ev);
    ev.key = K_UP;
    starbattle_on_key(&ev);
    CHECK(sb_cy == 0, "cursor clamped at top");
    ev.key = K_LEFT;
    starbattle_on_key(&ev);
    CHECK(sb_cx == 0, "cursor clamped at left");
    ev.key = K_DOWN;
    starbattle_on_key(&ev);
    ev.key = K_RIGHT;
    starbattle_on_key(&ev);
    CHECK(sb_cx == 1 && sb_cy == 1, "cursor moves");
    ev.key = K_CHAR;
    ev.ch = 'a';
    starbattle_on_key(&ev);
    CHECK(sb_cx == 0, "WASD left");
    ev.key = K_CHAR;
    ev.ch = 's';
    starbattle_on_key(&ev);
    CHECK(sb_cy == 2, "WASD down");

    /* OK 放/移除星; 重复 OK 忽略 */
    sb_cx = 0;
    sb_cy = 0;
    memset(&ev, 0, sizeof ev);
    ev.key = K_OK;
    starbattle_on_key(&ev);
    CHECK(sb_star[0] == 1, "OK places star");
    ev.is_repeat = true;
    starbattle_on_key(&ev);
    CHECK(sb_star[0] == 1, "OK repeat ignored");
    ev.is_repeat = false;
    ev.key = K_DEL;
    starbattle_on_key(&ev);
    CHECK(sb_star[0] == 0, "DEL clears star");

    /* 差最后一星未胜, 补上即胜 */
    sb_new_game(0);
    {
        int nong = 0, last = -1, placed = 0;
        for (i = 0; i < SB_NCELL; i++)
            if (sb_sol[i] == '#' && !sb_given[i]) { nong++; last = i; }
        for (i = 0; i < SB_NCELL; i++) {
            if (sb_sol[i] == '#' && !sb_given[i]) {
                if (placed < nong - 1) sb_toggle(i);
                placed++;
            }
        }
        CHECK(!sb_over, "one star short not win");
        sb_toggle(last);
        CHECK(sb_over, "last star wins");
        CHECK(sb_check_win(), "check_win true on solution");
    }

    /* 移除一星即失胜 */
    {
        int g = -1;
        for (i = 0; i < SB_NCELL; i++)
            if (sb_sol[i] == '#' && !sb_given[i]) { g = i; break; }
        sb_toggle(g);
        CHECK(!sb_check_win(), "removing a star breaks win");
        sb_toggle(g);
    }

    /* 计数全对但相邻违例 -> 不算胜 (独立构造的 16 星布局) */
    {
        static const int badset[16] = { 0, 1, 11, 12, 21, 22, 24, 31,
                                        35, 39, 42, 45, 49, 50, 60, 62 };
        sb_new_game(0);
        memset(sb_star, 0, sizeof sb_star);
        for (i = 0; i < 16; i++) sb_star[badset[i]] = 1;
        for (r = 0; r < SB_N; r++)
            CHECK(sb_count_line(0, r) == 2, "badset rows all 2");
        for (c = 0; c < SB_N; c++)
            CHECK(sb_count_line(1, c) == 2, "badset cols all 2");
        for (r = 0; r < SB_N; r++)
            CHECK(sb_count_line(2, r) == 2, "badset regions all 2");
        CHECK(!sb_check_win(), "adjacency violation blocks win");
    }

    /* N 换题 / R 重开同一题 */
    sb_new_game(0);
    memset(&ev, 0, sizeof ev);
    ev.key = K_CHAR;
    ev.ch = 'n';
    starbattle_on_key(&ev);
    CHECK(sb_pz == 1, "N cycles to next puzzle");
    CHECK(sb_star[(int)sb_giv[1][0]] == 1 && sb_given[(int)sb_giv[1][0]] == 1,
          "new puzzle givens loaded");
    ev.ch = 'r';
    starbattle_on_key(&ev);
    CHECK(sb_pz == 1, "R restarts same puzzle");

    /* 胜利后 OK/N 重开, Q 退出 */
    sb_new_game(0);
    for (i = 0; i < SB_NCELL; i++)
        if (sb_sol[i] == '#' && !sb_given[i]) sb_toggle(i);
    CHECK(sb_over, "solved before over-key tests");
    memset(&ev, 0, sizeof ev);
    ev.key = K_OK;
    starbattle_on_key(&ev);
    CHECK(!sb_over && sb_pz == 0, "OK after win restarts same puzzle");
    for (i = 0; i < SB_NCELL; i++)
        if (sb_sol[i] == '#' && !sb_given[i]) sb_toggle(i);
    ev.key = K_QUIT;
    s_exit_request = false;
    starbattle_on_key(&ev);
    CHECK(s_exit_request, "Q after win requests exit");
    s_exit_request = false;

    /* 游戏中 Q 退出 */
    sb_new_game(0);
    memset(&ev, 0, sizeof ev);
    ev.key = K_QUIT;
    starbattle_on_key(&ev);
    CHECK(s_exit_request, "Q in game requests exit");
    s_exit_request = false;
}

int main(void) {
    test_tiling();
    test_solution();
    test_givens();
    test_game_logic();
    printf(s_fail == 0 ? "ALL PASS (%d checks)\n" : "FAILURES: %d\n", s_fail);
    return s_fail;
}
