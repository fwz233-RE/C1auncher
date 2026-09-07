/* PRIME RUSH 逻辑单测 — host 编译运行; 直接包含 primerush.c 访问静态状态
 * 链接 canvas/font/font_data/pattern/rng/time/ui_common/input/display (-DCHICHU_HOST) */
#include "../../src/config.h"
#include "../../src/gfx/canvas.h"
#include "../../src/gfx/font.h"
#include "../../src/gfx/pattern.h"
#include "../../src/rng.h"
#include "../../src/platform/time.h"
#include "../../src/games/primerush.c"
#include <stdio.h>
#include <string.h>

static int s_fail = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); s_fail++; } \
    else { printf("ok: %s\n", msg); } \
} while (0)

/* 框架全局 stub(main.c 定义) */
bool s_exit_request = false;
void game_set_tick_interval(uint32_t ms) { (void)ms; }

static int fb_pix(int x, int y) {
    if (x < 0 || x >= (int)CCG_W || y < 0 || y >= (int)CCG_H) return 0;
    return (g_fb[(y >> 3) * (int)CCG_W + x] >> (7 - (y & 7))) & 1;
}

static int region_black(int x0, int y0, int x1, int y1) {
    int n = 0;
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++)
            if (fb_pix(x, y)) n++;
    return n;
}

/* 手动注入一道题(绕过随机) */
static void set_question(int n, bool prime) {
    pr_num = n;
    pr_ans = prime;
    pr_last = 0;
    pr_state = PR_PLAY;
}

static void test_is_prime(void) {
    static const int primes[] = {2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31,
                                 37, 41, 53, 97, 101, 997};
    static const int comps[] = {1, 4, 6, 8, 9, 10, 15, 21, 25, 49, 100,
                                121, 961, 999, 1000};
    int ok = 1;
    for (unsigned i = 0; i < sizeof(primes) / sizeof(primes[0]); i++)
        if (!pr_is_prime(primes[i])) ok = 0;
    for (unsigned i = 0; i < sizeof(comps) / sizeof(comps[0]); i++)
        if (pr_is_prime(comps[i])) ok = 0;
    CHECK(ok, "trial division: all listed primes/composites classified");
    CHECK(pr_is_prime(2) && !pr_is_prime(1) && !pr_is_prime(0), "edges: 2 prime, 1/0 not");
    CHECK(!pr_is_prime(961) && !pr_is_prime(1000), "sqrt bound: 31^2 composite, 1000 composite");
}

static void test_new_game(void) {
    pr_new_game();
    CHECK(pr_state == PR_PLAY, "new game state PLAY");
    CHECK(pr_score == 0 && pr_correct == 0 && pr_wrong == 0, "score/counts reset");
    CHECK(pr_time_ms == PR_GAME_MS, "60s timer set");
    CHECK(pr_num >= PR_MIN_N && pr_num <= PR_MAX_N, "first number in 2..1000");
    CHECK(pr_ans == pr_is_prime(pr_num), "cached answer matches trial division");
    CHECK(!pr_over_full, "over_full flag reset");
    CHECK(pr_fbk_ticks == 0, "feedback ticks reset");
}

static void test_range_and_mix(void) {
    rng_seed(&pr_rng, 12345u);
    pr_last = 0;
    int primes_seen = 0, comps_seen = 0, dup = 0, prev = -1, bad = 0;
    for (int i = 0; i < 300; i++) {
        pr_next();
        if (pr_num < PR_MIN_N || pr_num > PR_MAX_N) bad++;
        if (pr_ans != pr_is_prime(pr_num)) bad++;
        if (pr_num == prev) dup++;
        prev = pr_num;
        if (pr_ans) primes_seen++; else comps_seen++;
    }
    CHECK(bad == 0, "300 draws all in range with consistent answer");
    CHECK(dup == 0, "no consecutive duplicate numbers");
    CHECK(primes_seen > 20 && comps_seen > 20, "prime/composite mix both represented");
}

static void test_answer_correct(void) {
    pr_new_game();
    pr_score = 5; pr_correct = 2; pr_wrong = 1;
    set_question(17, true);
    pr_answer(true);
    CHECK(pr_score == 6 && pr_correct == 3 && pr_wrong == 1, "prime yes: +1, state PLAY");
    CHECK(pr_state == PR_PLAY, "correct answer advances immediately");
    set_question(15, false);
    pr_answer(false);
    CHECK(pr_score == 7 && pr_correct == 4, "composite no: +1");
}

static void test_answer_wrong(void) {
    pr_new_game();
    pr_score = 8; pr_wrong = 0;
    set_question(15, false);
    pr_answer(true);
    CHECK(pr_score == 6 && pr_wrong == 1, "wrong: -2 points");
    CHECK(pr_state == PR_FBK, "wrong answer enters feedback state");
    CHECK(pr_fbk_ticks == PR_FBK_TICKS, "feedback lasts 12 ticks (1.2s)");
    CHECK(strstr(pr_fbk, "15") && strstr(pr_fbk, "= 3 x 5") && strstr(pr_fbk, "NOT PRIME"),
          "composite feedback shows factorization");
    set_question(17, true);
    pr_answer(false);
    CHECK(strcmp(pr_fbk, "17 IS PRIME") == 0, "prime feedback shows IS PRIME");
    /* 分数下限 0 */
    pr_score = 1;
    set_question(20, false);
    pr_answer(true);
    CHECK(pr_score == 0, "score clamps at 0");
    pr_score = 0;
    set_question(20, false);
    pr_answer(true);
    CHECK(pr_score == 0, "score never underflows");
    /* FBK 态忽略作答 */
    pr_score = 5;
    pr_state = PR_FBK;
    pr_answer(true);
    CHECK(pr_score == 5 && pr_state == PR_FBK, "answers ignored during feedback");
}

static void test_feedback_advance(void) {
    pr_new_game();
    int before = pr_num;
    pr_state = PR_FBK;
    pr_fbk_ticks = 1;
    pr_time_ms = 50000;
    primerush_tick(0);
    CHECK(pr_state == PR_PLAY, "feedback expires into PLAY");
    CHECK(pr_num != before, "new number after feedback");
    CHECK(pr_time_ms == 49900, "timer keeps running during feedback");
    /* 正常 12 tick 流程 */
    set_question(21, false);
    pr_answer(true);
    pr_time_ms = 30000;
    for (int i = 0; i < PR_FBK_TICKS; i++) primerush_tick(0);
    CHECK(pr_state == PR_PLAY, "12 ticks later auto-advance");
    CHECK(pr_time_ms == 30000u - PR_TICK_MS * PR_FBK_TICKS, "12 ticks consumed");
}

static void test_timer_over(void) {
    pr_new_game();
    pr_score = 30; pr_best = 10;
    pr_time_ms = 200;
    primerush_tick(0);
    CHECK(pr_time_ms == 100 && pr_state == PR_PLAY, "countdown tick 1");
    primerush_tick(0);
    CHECK(pr_state == PR_OVER && pr_best == 30, "time up: OVER, best updated");
    uint32_t t = pr_time_ms;
    primerush_tick(0);
    CHECK(pr_state == PR_OVER && pr_time_ms == t, "ticks ignored after OVER");
    /* 新纪录 */
    pr_score = 5; pr_best = 50;
    pr_state = PR_PLAY; pr_time_ms = 100;
    primerush_tick(0);
    CHECK(pr_state == PR_OVER && pr_best == 50, "best not lowered by worse score");
}

static void test_over_keys(void) {
    pr_new_game();
    pr_state = PR_OVER;
    key_event_t ev = {0};
    ev.key = K_OK;
    primerush_on_key(&ev);
    CHECK(pr_state == PR_PLAY && pr_score == 0, "OK at over restarts");
    pr_state = PR_OVER;
    ev.key = K_CHAR; ev.ch = 'n';
    primerush_on_key(&ev);
    CHECK(pr_state == PR_PLAY, "N at over restarts");
    pr_state = PR_OVER;
    s_exit_request = false;
    ev.key = K_BACK;
    primerush_on_key(&ev);
    CHECK(s_exit_request, "BACK at over quits");
    s_exit_request = false;
    ev.key = K_QUIT;
    primerush_on_key(&ev);
    CHECK(s_exit_request, "Q at over quits");
}

static void test_play_keys(void) {
    pr_new_game();
    set_question(17, true);
    key_event_t ev = {0};
    ev.is_repeat = true; ev.key = K_OK;
    primerush_on_key(&ev);
    CHECK(pr_state == PR_PLAY && pr_num == 17, "repeat OK ignored");
    ev.is_repeat = false;
    ev.key = K_SPACE;
    primerush_on_key(&ev);
    CHECK(pr_state == PR_FBK && pr_wrong == 1, "SPACE answers not-prime");
    set_question(15, false);
    ev.key = K_BACK;
    primerush_on_key(&ev);
    CHECK(pr_correct == 1 && pr_state == PR_PLAY, "BACK answers not-prime correctly");
    s_exit_request = false;
    ev.key = K_QUIT;
    primerush_on_key(&ev);
    CHECK(s_exit_request, "Q during play quits");
}

static void test_render(void) {
    pr_new_game();
    set_question(997, true);
    primerush_render();
    CHECK(fb_pix(10, 120) == 0, "background cleared white");
    CHECK(fb_pix(PR_BOX_X, PR_BOX_Y) == 1, "number box frame drawn");
    CHECK(region_black(PR_BOX_X + 30, PR_NUM_Y, PR_BOX_X + PR_BOX_W - 30, PR_NUM_Y + 27) > 20,
          "4x number glyphs visible");
    CHECK(region_black(0, 0, 60, 6) > 10, "HUD title PRIME RUSH rendered");
    CHECK(region_black(180, 0, 294, 6) > 10, "HUD SCORE/TIME rendered");
    CHECK(region_black(0, PR_HINT_Y, CCG_W - 1, PR_HINT_Y + 6) > 10, "key hint line rendered");
    /* 反馈态: WRONG 行 + 正确答案行 */
    set_question(15, false);
    pr_answer(true);
    primerush_render();
    CHECK(region_black(0, PR_Q_Y, CCG_W - 1, PR_Q_Y + 13) > 10, "WRONG -2 line rendered");
    CHECK(region_black(0, PR_FBK_Y, CCG_W - 1, PR_FBK_Y + 13) > 10, "answer feedback line rendered");
    /* 结束态: HUD 清底 + 左结果右提示, 无墙内干扰 */
    pr_state = PR_OVER;
    pr_over_full = false;
    pr_score = 25; pr_best = 40;
    pr_correct = 12; pr_wrong = 3;
    primerush_render();
    CHECK(region_black(2, 1, 110, 6) > 10, "over HUD: score+best text");
    CHECK(region_black(172, 1, 294, 6) > 10, "over HUD: RETRY/QUIT hint");
    CHECK(fb_pix(0, 0) == 0 && fb_pix(150, 0) == 0, "over HUD cleared background");
    CHECK(pr_over_full, "over full refresh triggered once");
}

int main(void) {
    disp_init();                 /* host stub: 无设备 */
    test_is_prime();
    test_new_game();
    test_range_and_mix();
    test_answer_correct();
    test_answer_wrong();
    test_feedback_advance();
    test_timer_over();
    test_over_keys();
    test_play_keys();
    test_render();
    if (s_fail) {
        printf("FAILED: %d check(s)\n", s_fail);
        return 1;
    }
    printf("ALL PASS\n");
    return 0;
}
