/* MAZE 逻辑单测 — 包含游戏源文件, 链接框架 host 实现
 * 覆盖: 生成有效性(连通/唯一路径/确定性)、撞墙/边界阻挡、步数、
 *       沿独立 BFS 路径走到终点判定、解法路径标记、键路径状态转换、重复键 */
#include "../../src/config.h"
#include "../../src/games/maze.c"
#include <stdio.h>
#include <string.h>

static int s_fail = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); s_fail++; } \
    else { fprintf(stderr, "ok: %s\n", msg); } \
} while (0)

/* 框架全局 stub(与 tests/test_logic.c 一致) */
bool s_exit_request = false;
void game_set_tick_interval(uint32_t ms) { (void)ms; }

/* 独立 BFS(不复用游戏内 mz_solve): from→to 步数, -1=不可达 */
static int mz_test_bfs(int from, int to, int16_t prev[MZ_N]) {
    int16_t q[MZ_N];
    int head = 0, tail = 0;
    int i;
    for (i = 0; i < MZ_N; i++) prev[i] = -1;
    q[tail++] = (int16_t)from;
    prev[from] = from;
    while (head < tail) {
        int c = q[head++];
        int cx = c % MZ_W, cy = c / MZ_W;
        if (c == to) break;
        for (i = 0; i < 4; i++) {
            int nx = cx + mz_dx[i], ny = cy + mz_dy[i];
            if (nx < 0 || ny < 0 || nx >= MZ_W || ny >= MZ_H) continue;
            int n = ny * MZ_W + nx;
            if (mz_g[n] || prev[n] != -1) continue;
            prev[n] = (int16_t)c;
            q[tail++] = (int16_t)n;
        }
    }
    if (prev[to] == -1) return -1;
    int d = 0;
    for (int c = to; c != from; c = prev[c]) d++;
    return d;
}

/* 生成: 300 个种子下 起点/终点必通、全连通、生成树性质(边数==路格数-1 → 唯一路径) */
static void test_generation(void) {
    int bad = 0;
    for (int seed = 1; seed <= 300; seed++) {
        rng_seed(&mz_rng, (uint64_t)seed);
        mz_generate();   /* 若有死循环, 测试在此挂死 = 防护失败 */
        if (mz_g[0] != 0 || mz_g[MZ_END] != 0) { bad++; continue; }
        int16_t prev[MZ_N];
        int d = mz_test_bfs(0, MZ_END, prev);
        if (d < 0) { bad++; continue; }
        int paths = 0, edges = 0;
        for (int i = 0; i < MZ_N; i++) if (!mz_g[i]) paths++;
        for (int y = 0; y < MZ_H; y++)
            for (int x = 0; x < MZ_W; x++) {
                int idx = y * MZ_W + x;
                if (mz_g[idx]) continue;
                if (x + 1 < MZ_W && !mz_g[idx + 1]) edges++;
                if (y + 1 < MZ_H && !mz_g[idx + MZ_W]) edges++;
            }
        if (edges != paths - 1) bad++;
        if (paths == 0) bad++;
    }
    CHECK(bad == 0, "300 seeds: start/end carved, connected, unique path");
}

/* 确定性: 同种子同迷宫 */
static void test_determinism(void) {
    uint8_t snap[MZ_N];
    rng_seed(&mz_rng, 777);
    mz_generate();
    memcpy(snap, mz_g, MZ_N);
    rng_seed(&mz_rng, 777);
    mz_generate();
    CHECK(memcmp(snap, mz_g, MZ_N) == 0, "same seed -> identical maze");
}

/* 撞墙与边界: 每个墙格从每个相邻路格推必须被挡; 边界向外推必须被挡 */
static void test_blocking(void) {
    rng_seed(&mz_rng, 42);
    mz_generate();
    int tested = 0, blocked = 0;
    for (int y = 0; y < MZ_H; y++)
        for (int x = 0; x < MZ_W; x++) {
            int w = y * MZ_W + x;
            if (!mz_g[w]) continue;
            for (int i = 0; i < 4; i++) {
                int nx = x + mz_dx[i], ny = y + mz_dy[i];
                if (nx < 0 || ny < 0 || nx >= MZ_W || ny >= MZ_H) continue;
                if (mz_g[ny * MZ_W + nx]) continue;
                mz_px = nx; mz_py = ny;
                mz_moves = 12345;
                mz_over = false;
                /* 玩家在墙的邻居格, 朝墙推 = 方向取反 */
                bool r = mz_move(-mz_dx[i], -mz_dy[i]);
                tested++;
                if (!r && mz_px == nx && mz_py == ny &&
                    mz_moves == 12345 && !mz_over) blocked++;
            }
        }
    CHECK(tested > 0 && blocked == tested, "all wall cells block from every side");

    int bcnt = 0, bok = 0;
    for (int y = 0; y < MZ_H; y++) {
        if (!mz_g[y * MZ_W]) {
            mz_px = 0; mz_py = y; mz_moves = 0; mz_over = false; bcnt++;
            if (!mz_move(-1, 0) && mz_px == 0 && mz_moves == 0) bok++;
        }
        if (!mz_g[y * MZ_W + (MZ_W - 1)]) {
            mz_px = MZ_W - 1; mz_py = y; mz_moves = 0; mz_over = false; bcnt++;
            if (!mz_move(1, 0) && mz_px == MZ_W - 1 && mz_moves == 0) bok++;
        }
    }
    for (int x = 0; x < MZ_W; x++) {
        if (!mz_g[x]) {
            mz_px = x; mz_py = 0; mz_moves = 0; mz_over = false; bcnt++;
            if (!mz_move(0, -1) && mz_py == 0 && mz_moves == 0) bok++;
        }
        if (!mz_g[(MZ_H - 1) * MZ_W + x]) {
            mz_px = x; mz_py = MZ_H - 1; mz_moves = 0; mz_over = false; bcnt++;
            if (!mz_move(0, 1) && mz_py == MZ_H - 1 && mz_moves == 0) bok++;
        }
    }
    CHECK(bcnt > 0 && bok == bcnt, "border path cells block outward moves");
}

/* 沿独立 BFS 路径用 mz_move 走到终点: 步数==路径长, 位置在终点, 触发胜利 */
static void test_walk_to_star(void) {
    rng_seed(&mz_rng, 42);
    mz_generate();
    int16_t prev[MZ_N];
    int d = mz_test_bfs(0, MZ_END, prev);
    CHECK(d > 0, "walk: maze has a path (dist > 0)");
    int path[MZ_N], len = 0;
    for (int c = MZ_END; c != 0; c = prev[c]) path[len++] = c;
    mz_px = 0; mz_py = 0; mz_moves = 0; mz_over = false; mz_show_sol = false;
    int ok = 1;
    for (int i = len - 1; i >= 0; i--) {
        int tx = path[i] % MZ_W, ty = path[i] / MZ_W;
        if (!mz_move(tx - mz_px, ty - mz_py)) { ok = 0; break; }
    }
    CHECK(ok && mz_moves == (uint32_t)d && mz_px == MZ_W - 1 &&
          mz_py == MZ_H - 1 && mz_over,
          "walk: step count == dist, stands on star, SOLVED triggered");
}

/* 解法: 标记格数 == 玩家到终点距离-1, 全为路格, 终点旁有标记, 玩家在终点时无标记 */
static void test_solve(void) {
    rng_seed(&mz_rng, 99);
    mz_generate();
    mz_px = 0; mz_py = 0; mz_over = false; mz_show_sol = true;
    mz_solve();
    int16_t prev[MZ_N];
    int d = mz_test_bfs(0, MZ_END, prev);
    CHECK(d >= 0, "solve: path exists");
    int cnt = 0, all_path = 1;
    for (int i = 0; i < MZ_N; i++) {
        if (mz_sol[i]) { cnt++; if (mz_g[i]) all_path = 0; }
    }
    CHECK(cnt == d - 1, "solve: marks every cell between player and star");
    CHECK(all_path, "solve: all marked cells are path cells");
    int near_end = 0;
    for (int i = 0; i < 4; i++) {
        int nx = (MZ_W - 1) + mz_dx[i], ny = (MZ_H - 1) + mz_dy[i];
        if (nx < 0 || ny < 0 || nx >= MZ_W || ny >= MZ_H) continue;
        if (!mz_g[ny * MZ_W + nx] && mz_sol[ny * MZ_W + nx]) near_end++;
    }
    CHECK(near_end >= 1, "solve: a neighbor of the star is marked");
    mz_px = MZ_W - 1; mz_py = MZ_H - 1;
    mz_solve();
    cnt = 0;
    for (int i = 0; i < MZ_N; i++) if (mz_sol[i]) cnt++;
    CHECK(cnt == 0, "solve: player on star marks nothing");
}

/* 键路径: 重复字母忽略、重复方向响应、WASD 下行、H 切换解法、N 重开、胜利态按键 */
static void test_keys(void) {
    rng_seed(&mz_rng, 5);
    mz_generate();
    mz_px = 0; mz_py = 0; mz_moves = 0; mz_over = false; mz_show_sol = false;
    uint32_t gen0 = mz_gens;

    key_event_t ev;
    ev.key = K_CHAR; ev.ch = 'n'; ev.is_repeat = true;
    maze_on_key(&ev);
    CHECK(mz_gens == gen0, "repeat 'n' ignored (no new game)");

    int rep_moved = 0;
    for (int i = 0; i < 4 && !rep_moved; i++) {
        int nx = mz_px + mz_dx[i], ny = mz_py + mz_dy[i];
        if (nx < 0 || ny < 0 || nx >= MZ_W || ny >= MZ_H) continue;
        if (mz_g[ny * MZ_W + nx]) continue;
        ev.key = (ccg_key)(K_UP + i);
        ev.ch = 0;
        ev.is_repeat = true;
        maze_on_key(&ev);
        rep_moved = (mz_px == nx && mz_py == ny);
    }
    CHECK(rep_moved, "repeat direction key does move");

    int did_wasd = 0;
    for (int y = 0; y < MZ_H - 1 && !did_wasd; y++)
        for (int x = 0; x < MZ_W && !did_wasd; x++) {
            int idx = y * MZ_W + x;
            if (mz_g[idx] || mz_g[idx + MZ_W]) continue;
            mz_px = x; mz_py = y; mz_moves = 100; mz_over = false;
            ev.key = K_CHAR; ev.ch = 's'; ev.is_repeat = false;
            maze_on_key(&ev);
            did_wasd = (mz_px == x && mz_py == y + 1 && mz_moves == 101);
        }
    CHECK(did_wasd, "WASD 's' moves down and counts a step");

    mz_show_sol = false;
    ev.key = K_CHAR; ev.ch = 'h'; ev.is_repeat = false;
    maze_on_key(&ev);
    CHECK(mz_show_sol, "'h' turns solution on");
    maze_on_key(&ev);
    CHECK(!mz_show_sol, "'h' again turns solution off");

    ev.key = K_CHAR; ev.ch = 'n'; ev.is_repeat = false;
    maze_on_key(&ev);
    CHECK(mz_gens == gen0 + 1 && mz_px == 0 && mz_py == 0 &&
          mz_moves == 0 && !mz_over && !mz_show_sol,
          "'n' starts a fresh maze");

    /* 胜利态: 移动忽略, OK 重开, BACK 退出 */
    mz_over = true; mz_px = 5; mz_py = 5; mz_moves = 9;
    ev.key = K_UP; ev.ch = 0; ev.is_repeat = false;
    maze_on_key(&ev);
    CHECK(mz_px == 5 && mz_py == 5 && mz_moves == 9, "over: movement ignored");

    s_exit_request = false;
    ev.key = K_OK;
    maze_on_key(&ev);
    CHECK(!mz_over && mz_px == 0 && mz_py == 0 && mz_moves == 0,
          "over: OK starts retry");

    mz_over = true;
    s_exit_request = false;
    ev.key = K_BACK;
    maze_on_key(&ev);
    CHECK(s_exit_request, "over: BACK requests quit");

    /* 游戏内 Q 退出 */
    mz_over = false;
    s_exit_request = false;
    ev.key = K_QUIT;
    maze_on_key(&ev);
    CHECK(s_exit_request, "in-game Q requests quit");
}

/* 大量换局: mz_new_game 内部重播种(now_ms 变化)也不会死循环 */
static void test_many_new_games(void) {
    for (int i = 0; i < 40; i++) {
        mz_new_game();
        int16_t prev[MZ_N];
        CHECK(mz_test_bfs(0, MZ_END, prev) >= 0, "new game always solvable");
    }
}

int main(void) {
    test_generation();
    test_determinism();
    test_blocking();
    test_walk_to_star();
    test_solve();
    test_keys();
    test_many_new_games();
    if (s_fail) {
        fprintf(stderr, "MAZE TESTS: %d FAILED\n", s_fail);
        return 1;
    }
    printf("MAZE TESTS: ALL PASSED\n");
    return 0;
}
