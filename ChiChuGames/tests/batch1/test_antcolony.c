/* ANT COLONY 逻辑单测 — host cc 编译运行; 零平台依赖 */
#include "../src/config.h"
#include "../src/gfx/canvas.h"
#include "../src/gfx/font.h"
#include "../src/gfx/pattern.h"
#include "../src/rng.h"
#include "../src/platform/display.h"
#include "../src/games/antcolony.c"
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

/* 干净空棋盘 + 默认状态(不重播种, 测试自管 rng) */
static void ac_reset_board(void) {
    for (int y = 0; y < AC_ROWS; y++)
        for (int x = 0; x < AC_COLS; x++) {
            ac_cell[y][x] = AC_EMPTY;
            ac_grave[y][x] = false;
        }
    ac_food = 0; ac_turn = 0;
    ac_ants = 1; ac_active = 0;
    ac_ax = AC_NX; ac_ay = AC_NY; ac_age = 0;
    ac_path_len = 0;
    ac_over = false; ac_win = false; ac_over_full = false;
}

/* ---- test: BFS 最短路 ---- */
static void test_bfs(void) {
    ac_reset_board();
    int p[AC_CELLS];
    int len = ac_bfs_path(3, 2, p);
    CHECK(len == 5, "no-rock BFS length = manhattan(5)");
    CHECK(p[0] == 2 * AC_COLS + 3, "path starts at food cell (3,2)");
    /* 每步正交相邻, 终点与蚁穴相邻 */
    bool adj = true, end = false;
    for (int i = 1; i < len; i++) {
        int a = p[i - 1], b = p[i];
        int d = ((a % AC_COLS) - (b % AC_COLS)) * ((a % AC_COLS) - (b % AC_COLS)) +
                ((a / AC_COLS) - (b / AC_COLS)) * ((a / AC_COLS) - (b / AC_COLS));
        if (d != 1) adj = false;
    }
    CHECK(adj, "path cells orthogonally adjacent");
    {
        int last = p[len - 1];
        end = (last == AC_COLS + 0) || (last == 0 * AC_COLS + 1);
    }
    CHECK(end, "path ends adjacent to nest");
    CHECK(ac_bfs_path(AC_NX, AC_NY, p) == 0, "nest-to-nest path len 0");
    /* 蚁穴被岩石围死 → 不可达 */
    ac_cell[0][1] = AC_ROCK;
    ac_cell[1][0] = AC_ROCK;
    CHECK(ac_bfs_path(3, 3, p) == -1, "enclosed nest unreachable (-1)");
    /* 岩石绕行: 路径更长且不上岩石 */
    ac_reset_board();
    ac_cell[0][2] = AC_ROCK;   /* 挡住 (2,0) */
    len = ac_bfs_path(3, 0, p);
    CHECK(len == 5, "rock detour: dist (3,0)=3 blocked -> len 5");
    for (int i = 0; i < len; i++) {
        int cell = p[i];
        CHECK(ac_cell[cell / AC_COLS][cell % AC_COLS] != AC_ROCK, "path avoids rocks");
    }
}

/* ---- test: 随机布局合法性(200 种子) ---- */
static void test_layout(void) {
    for (uint64_t seed = 1; seed <= 200; seed++) {
        rng_seed(&ac_rng, seed);
        ac_reset_board();
        ac_place_rocks();
        ac_place_food();
        int rocks = 0;
        for (int y = 0; y < AC_ROWS; y++)
            for (int x = 0; x < AC_COLS; x++) {
                if (ac_cell[y][x] == AC_ROCK) {
                    rocks++;
                    CHECK(!(x == AC_NX && y == AC_NY), "no rock on nest");
                    CHECK(!(x == 1 && y == 0) && !(x == 0 && y == 1),
                          "nest exits kept open");
                }
            }
        CHECK(rocks == AC_ROCKS, "exactly 4 rocks");
        CHECK(ac_cell[ac_fy][ac_fx] != AC_ROCK, "food not on rock");
        CHECK(!(ac_fx == AC_NX && ac_fy == AC_NY), "food not on nest");
        int d = ac_bfs_path(ac_fx, ac_fy, ac_tmp);
        CHECK(d >= 1 && d <= AC_MAX_DIST, "food reachable within life limit");
    }
}

/* ---- test: 行走/拾取/回巢 ---- */
static void test_step(void) {
    ac_reset_board();
    ac_fx = 2; ac_fy = 0;           /* 食物固定在 (2,0) */
    CHECK(ac_step(1, 0), "step right moves");
    CHECK(ac_ax == 1 && ac_ay == 0, "ant at (1,0)");
    CHECK(ac_turn == 1 && ac_age == 1, "turn+age incremented");
    CHECK(ac_step(1, 0), "step onto food");
    CHECK(ac_food == AC_PILE, "food +pile on landing");
    CHECK(ac_ax == AC_NX && ac_ay == AC_NY, "auto-return to nest");
    CHECK(ac_age == 0, "age reset after return");
    CHECK(ac_path_len == 2, "return path len 2 for food (2,0)");
    CHECK(ac_path[0] == 2 && ac_path[1] == 1, "path = (2,0)->(1,0)->nest");
    /* 撞墙: 不移动不耗回合 */
    int t = ac_turn;
    CHECK(!ac_step(0, -1), "wall step blocked");
    CHECK(ac_turn == t && ac_ax == AC_NX, "blocked step costs nothing");
    /* 撞岩石: 不移动不耗回合 */
    ac_cell[0][1] = AC_ROCK;        /* (1,0) 落岩 */
    CHECK(!ac_step(1, 0), "rock step blocked");
    CHECK(ac_turn == t, "rock step costs nothing");
    ac_cell[0][1] = AC_EMPTY;
    /* 新行走清除旧回巢路径 */
    CHECK(ac_step(0, 1), "step down after return");
    CHECK(ac_path_len == 0, "old return path cleared on new walk");
    /* 新食物堆落点合法(拾取后已重刷) */
    CHECK(ac_cell[ac_fy][ac_fx] != AC_ROCK, "respawned food not on rock");
    int d = ac_bfs_path(ac_fx, ac_fy, ac_tmp);
    CHECK(d >= 1 && d <= AC_MAX_DIST, "respawned food reachable");
}

/* ---- test: 寿命 ---- */
static void test_life(void) {
    ac_reset_board();
    ac_fx = 7; ac_fy = 5;           /* 食物放最远 */
    ac_age = AC_ANT_LIFE - 1;       /* 11 步后走第 12 步 */
    CHECK(ac_step(1, 0), "12th step happens");
    CHECK(ac_grave[0][1], "death marker set at (1,0)");
    CHECK(ac_ax == AC_NX && ac_ay == AC_NY, "dead ant back at nest");
    CHECK(ac_age == 0, "age reset after death");
    CHECK(ac_food == 0, "no food gained on death");
}

/* ---- test: 胜负判定 ---- */
static void test_end(void) {
    ac_reset_board();
    ac_fx = 1; ac_fy = 0;
    ac_food = AC_WIN_FOOD - AC_PILE;       /* 10, 还差一垛 */
    ac_turn = AC_TURN_MAX - 1;             /* 恰好最后一回合 */
    CHECK(ac_step(1, 0), "last-turn food step");
    CHECK(ac_food == AC_WIN_FOOD, "food reaches 15");
    CHECK(ac_over && ac_win, "WIN on food reach within turn limit");

    ac_reset_board();
    ac_fx = 7; ac_fy = 5;
    ac_food = 5;
    ac_turn = AC_TURN_MAX - 1;
    CHECK(ac_step(1, 0), "last-turn plain step");
    CHECK(ac_turn == AC_TURN_MAX, "turn max reached");
    CHECK(ac_over && !ac_win, "LOSE on turn exhaustion");
}

/* ---- test: 蚂蚁解锁/轮换 ---- */
static void test_ants(void) {
    ac_reset_board();
    ac_fx = 1; ac_fy = 0;
    ac_food = AC_ANTS2 - AC_PILE;          /* 5, 拾取后到 10 */
    CHECK(ac_step(1, 0), "food step for unlock");
    CHECK(ac_ants == 2, "2nd ant unlocked at 10 food");
    ac_active = 0;
    ac_ant_back();
    CHECK(ac_active == 1, "ant rotation 0->1");
    ac_ant_back();
    CHECK(ac_active == 0, "ant rotation 1->0");
    /* 第 3 只: 20 食物(测试绕过 15 胜利直接置数) */
    ac_reset_board();
    ac_fx = 1; ac_fy = 0;
    ac_food = AC_ANTS3 - AC_PILE;          /* 15 */
    ac_ants = 2;
    CHECK(ac_step(1, 0), "food step for 3rd ant");
    CHECK(ac_ants == 3, "3rd ant unlocked at 20 food (capped at 3)");
    CHECK(ac_food == AC_ANTS3, "food 20");
    CHECK(ac_over && ac_win, "20>=15 also wins");
}

/* ---- test: 按键 ---- */
static void test_keys(void) {
    ac_reset_board();
    ac_fx = 7; ac_fy = 5;
    key_event_t ev;
    /* 重复的方向键可移动 */
    ev.key = K_RIGHT; ev.ch = 0; ev.is_repeat = true;
    antcolony_on_key(&ev);
    CHECK(ac_ax == 1 && ac_turn == 1, "repeat direction moves ant");
    /* 重复的确认键必须忽略 */
    ev.key = K_OK; ev.is_repeat = true;
    antcolony_on_key(&ev);
    CHECK(ac_turn == 1, "repeat OK ignored");
    /* WASD 移动 */
    ev.key = K_CHAR; ev.ch = 's'; ev.is_repeat = false;
    antcolony_on_key(&ev);
    CHECK(ac_ay == 1, "S moves ant down");
    /* 游戏结束: 非重开键不重置 */
    ac_over = true; ac_win = true;
    ev.key = K_LEFT; ev.is_repeat = false;
    antcolony_on_key(&ev);
    CHECK(ac_over, "over state ignores movement");
    ev.key = K_BACK;
    antcolony_on_key(&ev);
    CHECK(s_exit_request, "BACK at over -> exit request");
    s_exit_request = false;
}

/* ---- test: 渲染冒烟(各状态不崩溃) ---- */
static void test_render(void) {
    ac_new();
    antcolony_render();
    CHECK(ac_food == 0 && ac_turn == 0 && ac_ants == 1, "fresh game state");
    CHECK(!ac_over_full, "over full not yet fired");
    /* 带路径渲染 */
    ac_reset_board();
    ac_fx = 2; ac_fy = 0;
    ac_step(1, 0);
    ac_step(1, 0);                          /* 拾取 → 有回巢路径 */
    antcolony_render();
    CHECK(ac_path_len > 0, "path visible after pickup");
    /* 结束态渲染: 触发一次强制全刷 */
    ac_over = true; ac_win = true;
    antcolony_render();
    CHECK(ac_over_full, "over state fires force-full once");
    antcolony_render();
    CHECK(ac_over_full, "force-full fired only once");
    ac_reset_board();
}

int main(void) {
    test_bfs();
    test_layout();
    test_step();
    test_life();
    test_end();
    test_ants();
    test_keys();
    test_render();
    if (s_fail) { printf("TOTAL FAIL: %d\n", s_fail); return 1; }
    printf("ALL TESTS PASSED\n");
    return 0;
}
