/* TOWER DEFENSE 逻辑单测 — 包含游戏源文件, 链接框架 host 实现
 * 覆盖: 路径表合法性、建塔规则(路径/占用/金币/边界)、敌人推进、塔攻击
 *       与金币、基地守卫(踏入基地那 tick 可被击杀)、无塔必败、弱配置必败、
 *       全配置通关(平衡验证)、波次推进与喘息、菜单禁用跳过、光标钳制 */
#include "../../src/config.h"
#include "../../src/games/towerdefense.c"
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

static void td_sim_reset(void) {
    td_gold = TD_START_GOLD;
    td_wave = 1;
    td_spawned = 0;
    td_stick = 0;
    td_break = 0;
    td_tn = 0;
    td_over = false;
    td_win = false;
    td_over_full = false;
    td_menu_open = false;
    for (int i = 0; i < TD_MAX_EN; i++) {
        td_epos[i] = -1;
        td_ehp[i] = 0;
    }
    td_cx = 5;
    td_cy = 4;
}

/* 跑 max_ticks 个回合; 每回合前按优先级尝试建塔(金币够就买) */
static void td_sim_run(const int plan[][3], int plan_len, int max_ticks) {
    for (int tick = 0; tick < max_ticks && !td_over; tick++) {
        for (int k = 0; k < plan_len; k++)
            (void)td_place_tower(plan[k][0], plan[k][1], plan[k][2]);
        td_tick_turn();
    }
}

/* ---- 路径表合法性 ---- */
static void test_path(void) {
    int bad = 0;
    uint8_t seen[TD_COLS * TD_ROWS] = { 0 };
    if (td_px(0) != 0 || td_py(0) != 0) bad++;                       /* 起点左上 */
    if (td_px(TD_PATH_LEN - 1) != 11 || td_py(TD_PATH_LEN - 1) != 6) bad++; /* 终点基地 */
    for (int s = 0; s < TD_PATH_LEN; s++) {
        int c = td_path[s];
        if (c < 0 || c >= TD_COLS * TD_ROWS || seen[c]) { bad++; continue; }
        seen[c] = 1;
        if (s > 0) {                                                /* 相邻 1 步 */
            int dx = td_px(s) - td_px(s - 1);
            int dy = td_py(s) - td_py(s - 1);
            if (dx < 0) dx = -dx;
            if (dy < 0) dy = -dy;
            if (dx + dy != 1) bad++;
        }
    }
    CHECK(bad == 0 && TD_PATH_LEN == 28, "path: 28 cells, start TL, end BR, adjacent, unique");
}

/* ---- 建塔规则 ---- */
static void test_placement(void) {
    td_sim_reset();                                  /* gold 20 */
    CHECK(td_place_tower(4, 1, 3), "place T3 at (4,1) ok");
    CHECK(td_gold == 0, "T3 costs 20");
    CHECK(td_tn == 1, "tower count 1");
    CHECK(!td_place_tower(4, 1, 1), "occupied cell rejected");
    CHECK(!td_place_tower(0, 0, 1), "path cell rejected");
    CHECK(!td_place_tower(0, 3, 1), "path cell rejected (row3)");
    CHECK(!td_place_tower(4, 1, 0), "type 0 rejected");
    CHECK(!td_place_tower(4, 1, 4), "type 4 rejected");
    CHECK(!td_place_tower(-1, 2, 1), "out of bounds rejected");
    td_sim_reset();                                  /* gold 20 */
    CHECK(td_place_tower(5, 4, 1), "T1 $5 ok");
    CHECK(td_gold == 15, "T1 costs 5");
    CHECK(!td_place_tower(5, 1, 3), "T3 $20 > gold 15 rejected");
    td_gold = 94;
    td_tn = 0;
    for (int i = 0; i < TD_MAX_TOWERS; i++) {
        int fx = (i % 8) + ((i % 8) >= 5 ? 1 : 0);
        int fy = i < 8 ? 1 : 5;
        CHECK(td_place_tower(fx, fy, 1), "fill to capacity");
    }
    CHECK(!td_place_tower(1, 6, 1), "capacity cap enforced");
}

/* ---- 无塔必败: 首敌 tick4 出生, 28 格, 约 tick 31 进基地 ---- */
static void test_no_tower_defeat(void) {
    td_sim_reset();
    int ticks = 0;
    for (ticks = 0; ticks < 60 && !td_over; ticks++) td_tick_turn();
    CHECK(td_over && !td_win, "no towers: enemy reaches base -> DEFEAT");
    CHECK(ticks == 30, "defeat exactly at tick 30 (spawn 4 + 26 steps)");
    CHECK(td_gold == TD_START_GOLD, "no kills: gold untouched");
}

/* ---- 单塔清前几波: T3 (4,1) 杀敌得金币, 波次推进 + 喘息 ---- */
static void test_single_tower_early_waves(void) {
    td_sim_reset();
    CHECK(td_place_tower(4, 1, 3), "place T3");
    int ticks = 0;
    for (ticks = 0; ticks < 200 && !td_over && td_wave <= 3; ticks++)
        td_tick_turn();
    CHECK(!td_over, "single T3 survives waves 1-3");
    CHECK(td_wave == 4 || td_break > 0, "wave advanced to 4");
    CHECK(td_gold >= TD_START_GOLD - 20 + 4 * TD_KILL_GOLD,
          "kill gold credited (wave1: 4 kills)");
    /* 喘息期间不生成敌人 */
    if (td_wave >= 2) {
        int prev_wave = td_wave;
        int prev_break = td_break;
        int prev_spawned = td_spawned;
        td_tick_turn();
        CHECK(td_spawned == prev_spawned,
              "no spawn during wave break");
        CHECK(td_break == prev_break - 1, "break ticks count down");
        (void)prev_wave;
    }
}

/* ---- 弱配置(单 T3)必败于后期高 HP 波 ---- */
static void test_weak_build_loses(void) {
    static const int plan[][3] = { { 4, 1, 3 } };
    td_sim_reset();
    td_sim_run(plan, 1, 2000);
    CHECK(td_over && !td_win, "single T3: late waves overrun -> DEFEAT");
    fprintf(stderr, "  (weak build failed at wave %d, %d enemies killed)\n",
            td_wave, td_spawned - (td_wave > 1 ? 0 : 0));
}

/* ---- 全配置通关(平衡验证): 7 塔 $85 <= 总收入 $94 ---- */
static void test_full_build_wins(void) {
    static const int plan[][3] = {
        { 4, 1, 3 },   /* T3 中路枢纽 */
        { 9, 5, 3 },   /* T3 基地侧 */
        { 0, 1, 2 },   /* T2 出生侧 */
        { 7, 1, 2 },   /* T2 上走廊 */
        { 7, 4, 2 },   /* T2 下走廊 */
        { 11, 1, 2 },  /* T2 末端 */
        { 10, 5, 1 },  /* T1 基地贴身 */
    };
    int cost = 0;
    for (int k = 0; k < 7; k++) cost += td_cost[plan[k][2]];
    CHECK(cost <= TD_START_GOLD + 37 * TD_KILL_GOLD,
          "plan affordable: $85 <= $94");
    td_sim_reset();
    td_sim_run(plan, 7, 3000);
    fprintf(stderr, "  (end state: over=%d win=%d wave=%d spawned=%d gold=%d towers=%d)\n",
            td_over, td_win, td_wave, td_spawned, td_gold, td_tn);
    CHECK(td_over && td_win, "full build clears 5 waves -> VICTORY");
    CHECK(td_gold >= 0, "gold never negative");
    /* 终局后回合不再推进 */
    td_tick_turn();
    CHECK(td_over && td_win, "post-win ticks are no-ops");
}

/* ---- 基地守卫: 塔可在敌人踏入基地那一 tick 击杀 ---- */
static void test_base_guard(void) {
    td_sim_reset();                            /* gold 20 */
    CHECK(td_place_tower(10, 5, 1), "place T1 at (10,5)");
    td_epos[0] = TD_PATH_LEN - 2;              /* 26: 基地前一格 (11,5) */
    td_ehp[0] = 1;
    td_spawned = 4;                            /* 假装波1已生成完 */
    td_tick_turn();
    CHECK(!td_over, "T1 kills HP1 enemy on base cell -> saved");
    CHECK(td_gold == 20 - 5 + TD_KILL_GOLD, "kill gold credited on base save");
    /* 无守卫: 高 HP 敌人进基地即败 */
    td_sim_reset();
    td_epos[0] = TD_PATH_LEN - 2;
    td_ehp[0] = 99;
    td_tick_turn();
    CHECK(td_over && !td_win, "unguarded high-HP enemy enters base -> DEFEAT");
}

/* ---- 波清推进: 清波后 wave++ 且 break=6, break 结束开始下一波 ---- */
static void test_wave_advance(void) {
    td_sim_reset();
    td_gold = 100;
    CHECK(td_place_tower(4, 1, 3), "place T3");
    td_epos[0] = 3; td_ehp[0] = 1;              /* 手摆一个即将到射程的敌人 */
    td_spawned = 4;                             /* 波1已全生成 */
    td_break = 0;
    for (int i = 1; i < TD_MAX_EN; i++) td_epos[i] = -1;
    int ticks = 0;
    while (ticks < 40 && td_wave == 1) { td_tick_turn(); ticks++; }
    CHECK(td_wave == 2, "wave 1 cleared -> wave 2");
    CHECK(td_break == TD_BREAK_TICKS, "break set to 6");
    for (int i = 0; i < TD_BREAK_TICKS; i++) td_tick_turn();
    CHECK(td_wave == 2 && td_spawned == 0, "no spawn during break");
    for (int i = 0; i < 4; i++) td_tick_turn();
    CHECK(td_spawned == 1 && td_wave == 2, "wave 2 starts after break (interval 4)");
}

/* ---- 菜单: 金币不足的条目被跳过 ---- */
static void test_menu_skip(void) {
    td_gold = 4;
    td_menu_sel = -1;
    CHECK(td_menu_step(1) == 3, "gold<5: jumps straight to CANCEL");
    td_gold = 5;
    td_menu_sel = -1;
    CHECK(td_menu_step(1) == 0, "gold=5: only T1 selectable");
    td_gold = 15;
    td_menu_sel = -1;
    CHECK(td_menu_step(1) == 0, "gold=15: lands on T1");
    td_menu_sel = 1;
    CHECK(td_menu_step(1) == 3, "gold=15: T3 unaffordable -> skip to CANCEL");
    td_gold = 20;
    td_menu_sel = -1;
    CHECK(td_menu_step(1) == 0, "gold=20: all selectable, lands on T1");
}

/* ---- 光标钳制 ---- */
static void test_cursor(void) {
    td_sim_reset();
    td_cx = 0; td_cy = 0;
    td_move(0, -1); td_move(-1, 0);
    CHECK(td_cx == 0 && td_cy == 0, "cursor clamps at top-left");
    td_cx = TD_COLS - 1; td_cy = TD_ROWS - 1;
    td_move(0, 1); td_move(1, 0);
    CHECK(td_cx == TD_COLS - 1 && td_cy == TD_ROWS - 1, "cursor clamps at bottom-right");
}

int main(void) {
    test_path();
    test_placement();
    test_no_tower_defeat();
    test_single_tower_early_waves();
    test_weak_build_loses();
    test_full_build_wins();
    test_base_guard();
    test_wave_advance();
    test_menu_skip();
    test_cursor();
    if (s_fail == 0) printf("ALL TESTS PASSED\n");
    else printf("%d TEST(S) FAILED\n", s_fail);
    return s_fail != 0;
}
