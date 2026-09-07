/* BEE FARM 逻辑单测 — host, 直接包含游戏源文件(静态状态可访问)
 * 链接 canvas/font/font_data/pattern/rng/time/ui_common/input/display (-DCHICHU_HOST)
 * cc -std=c11 -O2 -Wall -Wextra -DCHICHU_HOST -I<root>/src -I<root>/src/gfx \
 *    /tmp/test_beefarm.c <root>/src/gfx/canvas.c <root>/src/gfx/font.c \
 *    <root>/src/gfx/font_data.c <root>/src/gfx/pattern.c <root>/src/rng.c \
 *    <root>/src/platform/time.c <root>/src/ui/ui_common.c \
 *    <root>/src/platform/input.c <root>/src/platform/display.c -o /tmp/test_beefarm
 */
#include "../../src/config.h"
#include "../../src/gfx/canvas.h"
#include "../../src/games/beefarm.c"
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

static bool px(int x, int y) {
    if (x < 0 || x >= (int)CCG_W || y < 0 || y >= (int)CCG_H) return false;
    int off = (y >> 3) * (int)CCG_W + x;
    return (g_fb[off] & (uint8_t)(0x80 >> (y & 7))) != 0;
}

static key_event_t press(ccg_key k) {
    key_event_t ev;
    memset(&ev, 0, sizeof ev);
    ev.key = k;
    return ev;
}

static key_event_t ch(char c) {
    key_event_t ev;
    memset(&ev, 0, sizeof ev);
    ev.key = K_CHAR;
    ev.ch = (uint8_t)c;
    return ev;
}

static void at(int x, int y) { bf_cx = x; bf_cy = y; }

static void send(key_event_t ev) { beefarm_on_key(&ev); }

/* ---- test: 开局状态 ---- */
static void test_reset(void) {
    bf_reset();
    CHECK(bf_grid[2][1] == BF_HIVE && bf_grid[2][5] == BF_HIVE, "two hives placed");
    CHECK(bf_grid[0][0] == BF_EMPTY, "rest of board empty");
    CHECK(bf_turn == 0, "turn 0");
    CHECK(bf_honey == 0, "honey 0");
    CHECK(bf_ms == BF_TURN_MS, "countdown full");
    CHECK(bf_sel == -1 && bf_over == BF_PLAY, "no selection, playing");
}

/* ---- test: 种花 = 一回合 ---- */
static void test_plant(void) {
    bf_reset();
    at(3, 3);
    send(press(K_OK));
    CHECK(bf_grid[3][3] == BF_FLOWER, "flower planted");
    CHECK(bf_age[3][3] == 1, "flower aged once at turn end");
    CHECK(bf_turn == 1, "plant costs one turn");
    /* OK on flower: no-op, no turn */
    int t = bf_turn;
    send(press(K_OK));
    CHECK(bf_turn == t && bf_grid[3][3] == BF_FLOWER, "OK on flower is no-op");
    /* SPACE 等效 OK: 种花 */
    bf_reset();
    at(0, 4);
    send(press(K_SPACE));
    CHECK(bf_grid[4][0] == BF_FLOWER && bf_turn == 1, "space plants a flower");
}

/* ---- test: 枯萎: 存活 5 回合末, 第 6 回合末移除 ---- */
static void test_wilt(void) {
    bf_reset();
    at(3, 3);
    send(press(K_OK));          /* turn 1, age 1 */
    for (int i = 0; i < 4; i++) bf_end_turn();   /* ages 2,3,4,5 */
    CHECK(bf_grid[3][3] == BF_FLOWER && bf_age[3][3] == 5, "flower alive at age 5");
    bf_end_turn();                          /* age 6 -> wilt */
    CHECK(bf_grid[3][3] == BF_EMPTY, "flower wilted after 6 turn-ends");
    CHECK(bf_turn == 6, "turn count intact");
}

/* ---- test: 产蜜节奏与上限 ---- */
static void test_production(void) {
    bf_reset();
    /* 蜂箱 A=(1,2); 在其 8 邻种 5 花 */
    static const int fx[5] = { 0, 1, 2, 0, 2 };
    static const int fy[5] = { 1, 1, 1, 2, 2 };
    for (int i = 0; i < 5; i++) { bf_grid[fy[i]][fx[i]] = BF_FLOWER; }
    bf_turn = 2;
    bf_end_turn();                          /* turn 3 -> produce */
    CHECK(bf_stock[2][1] == 5, "5 flowers -> 5 honey");
    CHECK(bf_turn == 3, "turn 3");
    bf_end_turn();                          /* turn 4: 无产蜜 */
    CHECK(bf_stock[2][1] == 5, "no production off schedule");
    /* 上限: 8 邻全花仍只产 5 */
    bf_reset();
    for (int y = 0; y < BF_ROWS; y++)
        for (int x = 0; x < BF_COLS; x++)
            if (x >= 0 && x <= 2 && y >= 1 && y <= 3 && !(x == 1 && y == 2))
                bf_grid[y][x] = BF_FLOWER;  /* 8 邻满 */
    bf_turn = 2;
    bf_end_turn();
    CHECK(bf_stock[2][1] == 5, "cap at 5 even with 8 flowers");
}

/* ---- test: 收蜜(自动, 不耗回合) ---- */
static void test_harvest(void) {
    bf_reset();
    bf_stock[2][1] = 7;
    at(1, 2);
    send(press(K_OK));
    CHECK(bf_honey == 7, "harvest collects stock");
    CHECK(bf_stock[2][1] == 0, "stock cleared");
    CHECK(bf_turn == 0, "harvest costs no turn");
    /* 空库存蜂箱 OK: 无动作 */
    bf_reset();
    at(1, 2);
    send(press(K_OK));
    CHECK(bf_honey == 0 && bf_turn == 0, "OK on empty hive is no-op");
}

/* ---- test: 搬蜂箱 ---- */
static void test_move_hive(void) {
    bf_reset();
    bf_stock[2][1] = 7;
    at(1, 2);
    send(ch('b'));               /* 选中 */
    CHECK(bf_sel == 2 * BF_COLS + 1, "hive selected");
    CHECK(bf_turn == 0, "selecting costs no turn");
    at(6, 4);
    send(ch('b'));               /* 移动到空格 */
    CHECK(bf_grid[4][6] == BF_HIVE, "hive moved");
    CHECK(bf_grid[2][1] == BF_EMPTY, "source cleared");
    CHECK(bf_stock[4][6] == 7 && bf_stock[2][1] == 0, "stock carried");
    CHECK(bf_sel == -1 && bf_turn == 1, "move costs a turn");
    /* 选中后对花再按 B: 取消, 不耗回合 */
    bf_reset();
    bf_grid[3][3] = BF_FLOWER;
    at(1, 2);
    send(ch('b'));
    at(3, 3);
    send(ch('b'));
    CHECK(bf_sel == -1 && bf_turn == 0, "B on flower cancels selection");
}

/* ---- test: 胜负判定 ---- */
static void test_winlose(void) {
    /* 收蜜达标 -> 即时胜(无需回合结束) */
    bf_reset();
    bf_honey = 29;
    bf_stock[2][5] = 4;
    at(5, 2);
    send(press(K_OK));
    CHECK(bf_over == BF_WIN, "honey >= 30 wins at harvest");
    /* 第 25 回合末仍不足 -> 负; 但收蜜达标优先 */
    bf_reset();
    bf_honey = 29;
    bf_stock[2][5] = 4;
    bf_turn = 24;
    at(5, 2);
    send(press(K_OK));
    CHECK(bf_over == BF_WIN, "win checked before turn cap");
    bf_reset();
    bf_turn = 24;
    bf_end_turn();
    CHECK(bf_turn == 25 && bf_over == BF_LOSE, "out of turns loses");
    /* 回合末收蜜达标也可胜 */
    bf_reset();
    bf_honey = 30;
    bf_turn = 24;
    bf_end_turn();
    CHECK(bf_over == BF_WIN, "goal reached by turn end wins");
}

/* ---- test: 倒计时 tick ---- */
static void test_countdown(void) {
    bf_reset();
    CHECK(bf_ms == BF_TURN_MS, "countdown starts full");
    for (int i = 0; i < 14; i++) beefarm_tick(0);
    CHECK(bf_ms == 1000 && bf_turn == 0, "14 ticks left, no turn yet");
    beefarm_tick(0);
    CHECK(bf_turn == 1 && bf_ms == BF_TURN_MS, "15th tick auto-ends turn");
    /* 结束后 tick 无动作 */
    bf_over = BF_LOSE;
    bf_ms = 500;
    beefarm_tick(0);
    CHECK(bf_ms == 500 && bf_turn == 1, "tick frozen after game over");
}

/* ---- test: 光标移动与边界 ---- */
static void test_cursor(void) {
    bf_reset();
    bf_cx = 0; bf_cy = 0;
    send(press(K_LEFT));
    send(press(K_UP));
    CHECK(bf_cx == 0 && bf_cy == 0, "cursor clamped at top-left");
    bf_cx = BF_COLS - 1; bf_cy = BF_ROWS - 1;
    send(press(K_RIGHT));
    send(press(K_DOWN));
    CHECK(bf_cx == BF_COLS - 1 && bf_cy == BF_ROWS - 1, "cursor clamped at bottom-right");
    bf_cx = 3; bf_cy = 2;
    send(ch('a')); send(ch('w'));
    CHECK(bf_cx == 2 && bf_cy == 1, "wasd moves cursor");
    send(ch('s')); send(ch('d'));
    CHECK(bf_cx == 3 && bf_cy == 2, "wasd back to start");
    /* 确认键忽略重复 */
    bf_reset();
    key_event_t rep = press(K_OK);
    rep.is_repeat = true;
    at(3, 3);
    beefarm_on_key(&rep);
    CHECK(bf_turn == 0, "repeat OK ignored");
}

/* ---- test: 结束状态按键 ---- */
static void test_over_keys(void) {
    bf_reset();
    bf_over = BF_WIN;
    s_exit_request = false;
    send(press(K_BACK));
    CHECK(s_exit_request, "BACK quits when over");
    s_exit_request = false;
    send(press(K_OK));
    CHECK(bf_over == BF_PLAY && bf_turn == 0, "OK restarts when over");
}

/* ---- test: 渲染冒烟 + 关键像素 ---- */
static void test_render(void) {
    bf_reset();
    beefarm_render();
    /* 蜂箱 A (1,2): 黑箱内点 (87,75) */
    CHECK(px(83 + 4, 71 + 4), "hive box black");
    /* 倒计时条顶部黑色(满 15s) */
    CHECK(px(BF_BAR_X, BF_Y0), "countdown bar full at start");
    /* 光标白环切断网格线 */
    CHECK(!px(57 + 3 * 26 + 1, 19 + 2 * 26 + 1), "cursor ring white");
    /* 种花后花心黑点 */
    bf_grid[0][0] = BF_FLOWER;
    bf_age[0][0] = 3;
    bf_grid[0][1] = BF_FLOWER;
    bf_age[0][1] = 5;
    beefarm_render();
    CHECK(px(59 + 3 * 3, 21 + 3 * 3), "flower center pixel");
    CHECK(px(59 + 3, 21), "flower ring pixel");
    /* 花龄点 */
    CHECK(px(61, 42), "flower age dot shown");
    CHECK(!px(76, 42), "no dot beyond age");
    /* 选中蜂箱: 整格反黑 */
    bf_sel = 2 * BF_COLS + 1;
    beefarm_render();
    CHECK(px(83, 71), "selected hive cell black");
    CHECK(!px(83 + 4, 71 + 8), "selected hive slit white");
    /* 结束画面渲染 */
    bf_over = BF_WIN;
    bf_over_full = false;
    beefarm_render();
    CHECK(!px(0, 0), "HUD cleared on game over");
    {
        int dark = 0;
        for (int xx = 0; xx < 150; xx++)
            if (px(xx, 2)) dark++;
        CHECK(dark > 5, "over message text present");
    }
    CHECK(px(BF_X0, BF_Y0), "board still visible under overlay");
}

/* ---- test: 最优策略模拟(整局) — 验证 25 回合 30 蜜可达 ----
 * 策略: 每回合在蜂箱 A 邻格种花; 顺路(模拟每 5 回合)回蜂箱自动收蜜。
 * 收蜜不耗回合, 中途达标即胜, 循环须在结束态停止。 */
static void test_strategy_win(void) {
    static const int nx[8] = { 0, 1, 2, 0, 2, 0, 1, 2 };
    static const int ny[8] = { 1, 1, 1, 2, 2, 3, 3, 3 };
    bf_reset();
    int plant_fail = 0;
    for (int act = 1; act <= 25 && bf_over == BF_PLAY; act++) {
        if (act % 5 == 0) {
            at(1, 2);
            send(press(K_OK));        /* 顺路自动收蜜 */
        }
        if (bf_over != BF_PLAY) break;
        int planted = 0;
        for (int i = 0; i < 8 && !planted; i++)
            if (bf_grid[ny[i]][nx[i]] == BF_EMPTY) {
                at(nx[i], ny[i]);
                send(press(K_OK));
                planted = 1;
            }
        if (!planted) plant_fail++;
    }
    CHECK(plant_fail == 0, "strategy always finds a free flower cell");
    CHECK(bf_honey >= BF_GOAL, "plant-every-turn strategy reaches 30");
    CHECK(bf_over == BF_WIN, "strategy wins");
}

int main(void) {
    printf("=== BEE FARM logic tests ===\n");
    test_reset();
    test_plant();
    test_wilt();
    test_production();
    test_harvest();
    test_move_hive();
    test_winlose();
    test_countdown();
    test_cursor();
    test_over_keys();
    test_render();
    test_strategy_win();
    if (s_fail) { printf("FAILED: %d checks\n", s_fail); return 1; }
    printf("ALL PASS\n");
    return 0;
}
