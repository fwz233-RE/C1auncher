/* GAME OF LIFE 逻辑单测 — host cc 编译运行
 * 直接 #include 游戏源文件(静态状态可访问), 链接框架层源文件(-DCHICHU_HOST)
 * 覆盖: B3/S23 演化规则、边缘视为死、暂停/tick、按键语义、光标边界、
 *       重复键防护、随机填充、rng 播种安全、长跑不死循环、渲染像素 */
#include "../../src/games/life.c"

#include <stdio.h>
#include <string.h>

static int s_fail = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); s_fail++; } \
    else { printf("ok: %s\n", msg); } \
} while (0)

/* ---- host 框架 stub ---- */
bool s_exit_request = false;
void game_set_tick_interval(uint32_t ms) { (void)ms; }

static void lf_test_clear(void) {
    for (int i = 0; i < LF_CELLS; i++) lf_cells[i] = 0;
}

static int lf_test_alive(void) {
    int n = 0;
    for (int i = 0; i < LF_CELLS; i++)
        if (lf_cells[i]) n++;
    return n;
}

static void lf_set(int x, int y) { lf_cells[y * LF_COLS + x] = 1; }
static int  lf_get(int x, int y) { return lf_cells[y * LF_COLS + x]; }

/* ---- rng 安全: 种子 0 不得死循环; range 恒 < n ---- */
static void test_rng_safety(void) {
    rng_t r;
    rng_seed(&r, 0);                 /* 陷阱种子: 旧实现会恒 0 */
    int bad = 0;
    for (int i = 0; i < 2000; i++) {
        uint32_t v = rng_range(&r, 100u);
        if (v >= 100u) bad++;
        if (r.s == 0) bad++;         /* xorshift 状态 0 = 死循环 */
    }
    CHECK(bad == 0, "rng seed 0: 2000 draws in range, state nonzero");
}

/* ---- B3/S23 演化 ---- */
static void test_blinker(void) {
    lf_test_clear();
    lf_set(5, 5); lf_set(6, 5); lf_set(7, 5);   /* 横向闪烁器 */
    lf_gen = 0;
    lf_step();
    CHECK(lf_get(6, 4) && lf_get(6, 5) && lf_get(6, 6), "blinker -> vertical");
    CHECK(!lf_get(5, 5) && !lf_get(7, 5), "blinker ends died");
    CHECK(lf_test_alive() == 3 && lf_gen == 1, "blinker gen=1 count=3");
    lf_step();
    CHECK(lf_get(5, 5) && lf_get(6, 5) && lf_get(7, 5), "blinker -> horizontal");
    CHECK(!lf_get(6, 4) && !lf_get(6, 6), "vertical ends died");
    CHECK(lf_test_alive() == 3 && lf_gen == 2, "blinker gen=2 count=3");
}

static void test_block_still(void) {
    lf_test_clear();
    lf_set(10, 10); lf_set(11, 10); lf_set(10, 11); lf_set(11, 11);
    lf_gen = 0;
    lf_step();
    CHECK(lf_get(10, 10) && lf_get(11, 10) && lf_get(10, 11) && lf_get(11, 11),
          "2x2 block still life unchanged");
    CHECK(lf_test_alive() == 4 && lf_gen == 1, "block count=4 gen=1");
}

static void test_lone_dies(void) {
    lf_test_clear();
    lf_set(5, 5);
    lf_step();
    CHECK(lf_test_alive() == 0, "lone cell dies (0 neighbors)");
}

static void test_edge_is_dead(void) {
    /* 角上 3 格: 只有格内邻居计数, 角格 (0,0) 邻居=2 存活, (1,1) 出生 */
    lf_test_clear();
    lf_set(0, 0); lf_set(1, 0); lf_set(0, 1);
    lf_step();
    CHECK(lf_get(0, 0) && lf_get(1, 0) && lf_get(0, 1), "corner cells survive");
    CHECK(lf_get(1, 1), "diagonal gap born (3 neighbors, no wrap)");
    CHECK(lf_test_alive() == 4, "corner block -> full 2x2, no wrap cells");
}

static void test_overpopulation(void) {
    lf_test_clear();
    lf_set(5, 5); lf_set(6, 5); lf_set(7, 5);   /* 横排 3 */
    lf_set(6, 4); lf_set(6, 6);                 /* 加竖两头 = 十字 */
    lf_step();
    CHECK(!lf_get(6, 5), "center dies (4 neighbors)");
    CHECK(lf_get(5, 5) && lf_get(7, 5) && lf_get(6, 4) && lf_get(6, 6),
          "cross arms survive (3 neighbors)");
}

/* ---- tick: 暂停时不动 ---- */
static void test_tick_pause(void) {
    lf_test_clear();
    lf_set(5, 5);
    lf_running = true;
    lf_gen = 0;
    life_tick(1234);
    CHECK(lf_gen == 1 && lf_test_alive() == 0, "tick while running: one gen");
    lf_test_clear();
    lf_set(5, 5);
    lf_running = false;
    life_tick(1);
    life_tick(2);
    CHECK(lf_gen == 1 && lf_test_alive() == 1, "tick while paused: no change");
    lf_running = true;
}

/* ---- 按键语义 ---- */
static void test_keys(void) {
    key_event_t ev;
    memset(&ev, 0, sizeof(ev));

    /* OK 翻转 + 重复防护 */
    lf_test_clear();
    lf_cx = 3; lf_cy = 3;
    ev.key = K_OK; ev.is_repeat = false;
    life_on_key(&ev);
    CHECK(lf_get(3, 3) == 1, "OK flips cell on");
    life_on_key(&ev);
    CHECK(lf_get(3, 3) == 0, "OK flips cell off");
    ev.is_repeat = true;
    life_on_key(&ev);
    CHECK(lf_get(3, 3) == 0, "OK repeat ignored");
    ev.is_repeat = false;

    /* 方向重复可响应 */
    lf_cx = 10; lf_cy = 5;
    ev.key = K_LEFT; ev.is_repeat = true;
    life_on_key(&ev);
    CHECK(lf_cx == 9, "K_LEFT repeat moves cursor");
    ev.is_repeat = false;

    /* 光标边界 */
    lf_cx = 0; lf_cy = 0;
    ev.key = K_LEFT;  life_on_key(&ev);
    ev.key = K_UP;    life_on_key(&ev);
    CHECK(lf_cx == 0 && lf_cy == 0, "cursor clamped at top-left");
    lf_cx = LF_COLS - 1; lf_cy = LF_ROWS - 1;
    ev.key = K_RIGHT; life_on_key(&ev);
    ev.key = K_DOWN;  life_on_key(&ev);
    CHECK(lf_cx == LF_COLS - 1 && lf_cy == LF_ROWS - 1, "cursor clamped at bottom-right");
    lf_cx = 0;
    ev.key = K_CHAR; ev.ch = 'a'; life_on_key(&ev);
    CHECK(lf_cx == 0, "WASD-left clamped at edge");

    /* w/d 移动 */
    lf_cx = 5; lf_cy = 5;
    ev.key = K_CHAR; ev.ch = 'w'; life_on_key(&ev);
    CHECK(lf_cy == 4, "'w' moves up");
    ev.ch = 'd'; life_on_key(&ev);
    CHECK(lf_cx == 6, "'d' moves right");

    /* S 单步: 先停后走 */
    lf_test_clear();
    lf_set(10, 10); lf_set(11, 10); lf_set(10, 11); lf_set(11, 11);
    lf_running = true;
    lf_gen = 0;
    ev.key = K_CHAR; ev.ch = 's'; life_on_key(&ev);
    CHECK(!lf_running, "'s' pauses evolution");
    CHECK(lf_gen == 1, "'s' steps exactly one generation");
    CHECK(lf_test_alive() == 4, "'s' applies B3/S23 (still life kept)");

    /* P / K_PAUSE 切换 */
    ev.ch = 'p'; life_on_key(&ev);
    CHECK(lf_running, "'p' resumes");
    ev.key = K_PAUSE; life_on_key(&ev);
    CHECK(!lf_running, "K_PAUSE pauses");
    ev.key = K_PAUSE; life_on_key(&ev);
    CHECK(lf_running, "K_PAUSE resumes");
    ev.key = K_CHAR;

    /* C 清空 */
    ev.ch = 'c'; life_on_key(&ev);
    CHECK(lf_test_alive() == 0, "'c' clears all cells");
    CHECK(lf_gen == 0 && !lf_running, "'c' resets gen and pauses");

    /* R 随机填充 30% */
    lf_running = true;
    ev.ch = 'r'; life_on_key(&ev);
    CHECK(lf_test_alive() > 0, "'r' fills some cells");
    CHECK(lf_test_alive() < LF_CELLS, "'r' not full board");
    CHECK(lf_gen == 0, "'r' resets gen");
    CHECK(lf_running, "'r' keeps running state");

    /* N 随机新局 */
    ev.ch = 'n'; life_on_key(&ev);
    CHECK(lf_gen == 0 && lf_running, "'n' new game: gen=0 running");
    CHECK(lf_test_alive() > 0, "'n' random pattern non-empty");
    CHECK(lf_cx == LF_COLS / 2 && lf_cy == LF_ROWS / 2, "'n' cursor centered");

    /* Q 退出 */
    s_exit_request = false;
    ev.ch = 'q'; life_on_key(&ev);
    CHECK(s_exit_request, "'q' sets exit request");
    s_exit_request = false;
    ev.key = K_QUIT; life_on_key(&ev);
    CHECK(s_exit_request, "K_QUIT sets exit request");
    s_exit_request = false;
}

/* ---- 渲染: 活格像素 / 光标反色边框 / 暂停不崩溃 ---- */
static void test_render(void) {
    lf_test_clear();
    lf_set(0, 0);                       /* 实心格 */
    lf_cx = 20; lf_cy = 10;             /* 光标放远处(死格→黑边框) */
    lf_running = true;
    life_render();
    /* 格 (0,0) 中心像素 (OX+5, OY+5) = (33,23) */
    CHECK((g_fb[625] & 0x01) != 0, "live cell pixel drawn");
    /* 光标边框像素 (OX+20*10-2, OY+10*10-2) = (226,116) */
    CHECK((g_fb[4370] & 0x08) != 0, "cursor black border on dead cell");
    /* 活格在光标下 → 白边框 */
    lf_test_clear();
    lf_set(20, 10);
    life_render();
    CHECK((g_fb[4370] & 0x08) == 0, "cursor white border on live cell");
    /* 暂停态渲染不崩溃 */
    lf_running = false;
    life_render();
    CHECK(lf_running == false, "render ok while paused");
    lf_running = true;
}

/* ---- 长跑: 5000 代无死循环, 代数计数正确 ---- */
static void test_soak(void) {
    lf_new_game();
    lf_running = false;
    uint32_t start = lf_gen;
    for (int i = 0; i < 5000; i++) lf_step();
    CHECK(lf_gen == start + 5000, "soak 5000 gens: counter exact");
}

int main(void) {
    test_rng_safety();
    test_blinker();
    test_block_still();
    test_lone_dies();
    test_edge_is_dead();
    test_overpopulation();
    test_tick_pause();
    test_keys();
    test_render();
    test_soak();
    if (s_fail == 0) printf("ALL LIFE TESTS PASSED\n");
    else printf("%d FAILURES\n", s_fail);
    return s_fail ? 1 : 0;
}
