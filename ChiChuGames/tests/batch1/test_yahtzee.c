/* YAHTZEE host 逻辑单测: 直接包含游戏源, 断言核心逻辑 */
#include "../../src/games/yahtzee.c"
#include <stdio.h>

static int s_fail = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); s_fail++; } \
    else { printf("ok: %s\n", msg); } \
} while (0)

/* ---- 框架 stub(host) ---- */
bool s_exit_request = false;
void game_set_tick_interval(uint32_t ms) { (void)ms; }

static void set_dice(const int v[5]) {
    for (int i = 0; i < 5; i++) yz_dice[i] = v[i];
}

/* 1) 各类别得分计算 */
static void test_scoring(void) {
    int a[5] = { 1, 1, 1, 2, 2 };
    set_dice(a);
    CHECK(yz_calc(0) == 3, "ones 11122 = 3");
    CHECK(yz_calc(1) == 4, "twos 11122 = 4");
    CHECK(yz_calc(2) == 0, "threes 11122 = 0");
    CHECK(yz_calc(5) == 0, "sixes 11122 = 0");
    CHECK(yz_calc(6) == 7, "3kind 11122 = 7");
    CHECK(yz_calc(7) == 0, "4kind 11122 = 0");
    CHECK(yz_calc(8) == 25, "full 11122 = 25");
    CHECK(yz_calc(9) == 0, "small 11122 = 0");
    CHECK(yz_calc(10) == 0, "large 11122 = 0");
    CHECK(yz_calc(11) == 0, "yahtz 11122 = 0");
    CHECK(yz_calc(12) == 7, "chance 11122 = 7");

    int b[5] = { 6, 6, 6, 6, 6 };
    set_dice(b);
    CHECK(yz_calc(5) == 30, "sixes 66666 = 30");
    CHECK(yz_calc(6) == 30, "3kind 66666 = 30");
    CHECK(yz_calc(7) == 30, "4kind 66666 = 30");
    CHECK(yz_calc(8) == 0, "full 66666 = 0 (strict 3+2)");
    CHECK(yz_calc(9) == 0, "small 66666 = 0");
    CHECK(yz_calc(10) == 0, "large 66666 = 0");
    CHECK(yz_calc(11) == 50, "yahtz 66666 = 50");
    CHECK(yz_calc(12) == 30, "chance 66666 = 30");

    int c[5] = { 1, 2, 3, 4, 6 };
    set_dice(c);
    CHECK(yz_calc(9) == 30, "small 12346 = 30");
    CHECK(yz_calc(10) == 0, "large 12346 = 0");
    CHECK(yz_calc(1) == 2, "twos 12346 = 2");

    int d[5] = { 2, 3, 4, 5, 6 };
    set_dice(d);
    CHECK(yz_calc(9) == 30, "small 23456 = 30");
    CHECK(yz_calc(10) == 40, "large 23456 = 40");

    int e[5] = { 1, 2, 3, 4, 5 };
    set_dice(e);
    CHECK(yz_calc(10) == 40, "large 12345 = 40");

    int f[5] = { 1, 3, 4, 5, 6 };
    set_dice(f);
    CHECK(yz_calc(9) == 30, "small 13456 = 30 (3456)");

    int g[5] = { 2, 2, 3, 3, 3 };
    set_dice(g);
    CHECK(yz_calc(6) == 13, "3kind 22333 = 13");
    CHECK(yz_calc(8) == 25, "full 22333 = 25");
    CHECK(yz_calc(11) == 0, "yahtz 22333 = 0");
}

/* 2) 掷骰: 值域/锁定保留/三次上限/同种子确定性 */
static void test_roll(void) {
    rng_seed(&yz_rng, 12345);
    yz_new_game();
    CHECK(yz_phase == YZ_PH_DICE, "new game phase DICE");
    CHECK(yz_rolls == 1, "new game first roll done");
    CHECK(yz_turn == 0, "new game turn 0");
    CHECK(yz_over_full == false, "over_full reset");
    for (int i = 0; i < 5; i++)
        CHECK(yz_dice[i] >= 1 && yz_dice[i] <= 6, "dice in 1..6");

    int before[5];
    for (int i = 0; i < 5; i++) { before[i] = yz_dice[i]; yz_hold[i] = true; }
    yz_roll();
    CHECK(yz_rolls == 2, "second roll");
    int same = 1;
    for (int i = 0; i < 5; i++) if (yz_dice[i] != before[i]) same = 0;
    CHECK(same, "held dice preserved across roll");

    yz_roll();
    CHECK(yz_rolls == 3 && yz_phase == YZ_PH_SCORE, "3rd roll auto -> SCORE");
    int after3 = yz_rolls;
    yz_roll();
    CHECK(yz_rolls == after3, "roll over cap is no-op");

    rng_seed(&yz_rng, 777);
    yz_new_game();
    int d1[5];
    for (int i = 0; i < 5; i++) d1[i] = yz_dice[i];
    rng_seed(&yz_rng, 777);
    yz_new_game();
    int same2 = 1;
    for (int i = 0; i < 5; i++) if (yz_dice[i] != d1[i]) same2 = 0;
    CHECK(same2, "same seed -> same dice");
}

/* 3) 确认计分流程: 填类别/回合推进/跑满 13 轮结算 */
static void test_confirm_flow(void) {
    rng_seed(&yz_rng, 999);
    yz_new_game();
    yz_phase = YZ_PH_SCORE;
    yz_cat = 0;
    yz_confirm();
    CHECK(yz_score[0] >= 0, "category 0 filled");
    CHECK(yz_turn == 1, "turn advanced");
    CHECK(yz_phase == YZ_PH_DICE, "new turn auto-roll");
    CHECK(yz_rolls == 1, "new turn roll 1");
    int held = 0;
    for (int i = 0; i < 5; i++) if (yz_hold[i]) held = 1;
    CHECK(!held, "holds cleared for new turn");

    int s0 = yz_score[0];
    yz_phase = YZ_PH_SCORE;
    yz_cat = 0;
    yz_confirm();
    CHECK(yz_score[0] == s0 && yz_turn == 1, "filled category not overwritten");

    int guard = 0;
    while (yz_turn < 13 && guard++ < 20) {
        yz_phase = YZ_PH_SCORE;
        yz_cat = yz_turn;
        yz_confirm();
    }
    CHECK(yz_turn == 13, "13 turns played");
    CHECK(yz_phase == YZ_PH_OVER, "phase OVER after 13 turns");
    int all = 1;
    for (int i = 0; i < 13; i++) if (yz_score[i] < 0) all = 0;
    CHECK(all, "all 13 categories filled");
}

/* 4) 上区奖励与总分 */
static void test_bonus_total(void) {
    for (int i = 0; i < 13; i++) yz_score[i] = -1;
    yz_score[0] = 10; yz_score[1] = 10; yz_score[2] = 10;
    yz_score[3] = 10; yz_score[4] = 10; yz_score[5] = 13;
    CHECK(yz_upper_sum() == 63, "upper sum 63");
    CHECK(yz_total() == 98, "total 98 (63 + 35 bonus)");
    yz_score[5] = 12;
    CHECK(yz_upper_sum() == 62 && yz_total() == 62, "no bonus below 63");
    for (int i = 0; i < 6; i++) yz_score[i] = 3;
    CHECK(yz_upper_sum() == 18 && yz_total() == 18, "no bonus for low upper");
    yz_score[0] = 10; yz_score[1] = 10; yz_score[2] = 10;
    yz_score[3] = 10; yz_score[4] = 10; yz_score[5] = 13;   /* 上区 = 63 */
    yz_score[6] = 25; yz_score[7] = 30; yz_score[8] = 25; yz_score[9] = 30;
    yz_score[10] = 40; yz_score[11] = 50; yz_score[12] = 20;
    CHECK(yz_total() == 63 + 220 + 35, "total = upper + lower + bonus");
}

/* 5) 按键语义 */
static void test_keys(void) {
    rng_seed(&yz_rng, 555);
    yz_new_game();
    key_event_t ev = { K_LEFT, 0, true };
    int c0 = yz_cur;
    yahtzee_on_key(&ev);
    CHECK(yz_cur == (c0 + 4) % 5, "repeat LEFT moves die cursor");

    ev.key = K_OK; ev.is_repeat = false;
    yahtzee_on_key(&ev);
    CHECK(yz_hold[yz_cur], "OK locks selected die");
    ev.is_repeat = true;
    yahtzee_on_key(&ev);
    CHECK(yz_hold[yz_cur], "repeat OK ignored");
    ev.is_repeat = false;
    yahtzee_on_key(&ev);
    CHECK(!yz_hold[yz_cur], "OK toggles unlock");

    ev.key = K_CHAR; ev.ch = 'w';
    int r1 = yz_rolls;
    yahtzee_on_key(&ev);
    CHECK(yz_rolls == r1 + 1, "w rolls dice");

    ev.ch = 's';
    yahtzee_on_key(&ev);
    CHECK(yz_phase == YZ_PH_SCORE, "s enters SCORE phase");

    ev.key = K_UP; ev.ch = 0;
    int cat0 = yz_cat;
    yahtzee_on_key(&ev);
    CHECK(yz_cat == (cat0 + 12) % 13, "UP moves category up in SCORE");

    ev.key = K_BACK;
    yahtzee_on_key(&ev);
    CHECK(yz_phase == YZ_PH_DICE, "BACK returns DICE from SCORE");

    ev.key = K_CHAR; ev.ch = 'n';
    yahtzee_on_key(&ev);
    CHECK(yz_turn == 0 && yz_phase == YZ_PH_DICE, "n starts new game");

    ev.key = K_OK; ev.ch = 0;
    yahtzee_on_key(&ev);   /* 锁定第 0 骰 */
    yz_phase = YZ_PH_OVER;
    s_exit_request = false;
    ev.key = K_BACK;
    yahtzee_on_key(&ev);
    CHECK(s_exit_request, "BACK in OVER quits");
}

/* 6) 渲染不崩溃 + 结算全刷一次 */
static void test_render_over(void) {
    rng_seed(&yz_rng, 42);
    yz_new_game();
    yahtzee_render();
    CHECK(yz_over_full == false, "in-game render no force full");
    for (int i = 0; i < 13; i++) yz_score[i] = i;
    yz_phase = YZ_PH_OVER;
    yz_over_full = false;
    yahtzee_render();
    CHECK(yz_over_full, "over render sets full-refresh flag");
    yahtzee_render();
    CHECK(yz_over_full, "over flag stays set (single full)");
}

int main(void) {
    test_scoring();
    test_roll();
    test_confirm_flow();
    test_bonus_total();
    test_keys();
    test_render_over();
    if (s_fail) { printf("FAILED: %d check(s)\n", s_fail); return 1; }
    printf("ALL PASS\n");
    return 0;
}
