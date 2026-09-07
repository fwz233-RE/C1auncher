/* TIMES TRAINER host 逻辑单测 — include 游戏 .c, 断言核心逻辑 */
#include "../src/games/timestrain.c"
#include <stdio.h>
#include <string.h>

/* ---- host 框架 stub ---- */
bool s_exit_request = false;
void game_set_tick_interval(uint32_t ms) { (void)ms; }

static int s_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); s_fail++; } \
    else { printf("ok: %s\n", msg); } \
} while (0)

/* 输入答案并提交(模拟数字键+OK) */
static void tt_type_answer(int v) {
    char tmp[4];
    int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v > 0) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
    tt_ansn = n;
    for (int i = 0; i < n; i++) tt_ans[i] = tmp[n - 1 - i];
    tt_submit();
}

static void test_ranges(void) {
    /* 各等级数字范围 */
    rng_seed(&tt_rng, 7);
    tt_lvl = 1; tt_make_q();
    CHECK(tt_a >= 2 && tt_a <= 5 && tt_b >= 2 && tt_b <= 5, "LV1 range 2-5 x 2-5");
    CHECK(tt_a * tt_b == tt_a * tt_b && tt_a * tt_b >= 4 && tt_a * tt_b <= 25,
          "LV1 product in 4..25");
    tt_lvl = 2; tt_make_q();
    CHECK(tt_a >= 2 && tt_a <= 9 && tt_b >= 2 && tt_b <= 5, "LV2 range 2-9 x 2-5");
    tt_lvl = 3; tt_make_q();
    CHECK(tt_a >= 2 && tt_a <= 9 && tt_b >= 2 && tt_b <= 9, "LV3 range 2-9 x 2-9");
    CHECK(tt_a * tt_b <= 81, "LV3 product <= 81");
    tt_lvl = 4; tt_make_q();
    CHECK(tt_a >= 3 && tt_a <= 9 && tt_b >= 3 && tt_b <= 9, "LV4 range 3-9 x 3-9");
}

static void test_digits(void) {
    /* 数字追加/封顶/退格 */
    rng_seed(&tt_rng, 11);
    tt_new_game();
    tt_ansn = 0;
    tt_ans[tt_ansn++] = '1';                    /* 模拟按键 1 */
    tt_ans[tt_ansn++] = '2';
    CHECK(tt_ansn == 2 && tt_ans[0] == '1' && tt_ans[1] == '2',
          "digits appended");
    if (tt_ansn < TT_ANS_CAP) tt_ans[tt_ansn++] = '3';  /* 超 2 位丢弃 */
    CHECK(tt_ansn == 2, "digit cap at 2");
    tt_ansn--;                                   /* DEL 退格 */
    CHECK(tt_ansn == 1, "backspace");
    tt_ansn = 0;
    tt_submit();                                 /* 空输入提交: 忽略 */
    CHECK(tt_score == 0 && tt_right == 0, "empty submit ignored");
}

static void test_combo_scoring(void) {
    /* 连击加分: 1:0 2:2 3:4 4:6 5:8 6+:10 */
    CHECK(tt_combo_bonus(0) == 0 && tt_combo_bonus(1) == 0, "combo bonus 0/1 = 0");
    CHECK(tt_combo_bonus(2) == 2 && tt_combo_bonus(3) == 4, "combo bonus 2/3");
    CHECK(tt_combo_bonus(5) == 8 && tt_combo_bonus(6) == 10, "combo bonus 5/6");
    CHECK(tt_combo_bonus(9) == 10, "combo bonus cap at 10");
}

static void test_play_and_levelup(void) {
    /* 固定种子完整对局: 前 10 题全对 -> 升 LV2 */
    rng_seed(&tt_rng, 1234);
    tt_lvl = 1;
    tt_score = 0; tt_right = 0; tt_combo = 0; tt_max_combo = 0;
    tt_last_a = -1; tt_last_b = -1;
    tt_fb[0] = 0;
    for (int i = 0; i < 10; i++) {
        tt_make_q();
        CHECK(tt_a >= 2 && tt_a <= 5 && tt_b >= 2 && tt_b <= 5, "play LV1 range");
        tt_type_answer(tt_a * tt_b);
    }
    CHECK(tt_right == 10, "10 correct");
    CHECK(tt_lvl == 2, "level up to LV2 after 10 right");
    CHECK(tt_combo == 10, "combo 10 in a row");
    CHECK(tt_max_combo == 10, "max combo recorded");
    /* 分数: 10 题全对, 连击加分 0+2+4+6+8+10+10+10+10+10=70, 基础 50 -> 120 */
    CHECK(tt_score == 120, "score 120 with combo bonuses");
    /* 对题反馈含 RIGHT */
    CHECK(strstr(tt_fb, "RIGHT") != NULL, "feedback RIGHT");
}

static void test_wrong_answer(void) {
    /* 答错: 连击清零, 反馈显示正确答案, 换新题 */
    rng_seed(&tt_rng, 99);
    tt_lvl = 3;
    tt_score = 0; tt_right = 0; tt_combo = 5; tt_max_combo = 5;
    tt_last_a = -1; tt_last_b = -1;
    tt_fb[0] = 0;
    tt_make_q();
    int olda = tt_a, oldb = tt_b;
    tt_type_answer(olda * oldb + 1);             /* 故意答错 */
    CHECK(tt_combo == 0, "wrong resets combo");
    CHECK(tt_right == 0, "wrong does not count right");
    CHECK(tt_score == 0, "wrong no score");
    {
        char expect[32];
        snprintf(expect, sizeof(expect), "WRONG %dx%d=%d", olda, oldb, olda * oldb);
        CHECK(strcmp(tt_fb, expect) == 0, "feedback shows correct answer");
    }
    CHECK(!(tt_a == olda && tt_b == oldb), "new question generated");
    /* 答对后再答错: 反馈切回 WRONG */
    tt_type_answer(tt_a * tt_b);
    CHECK(strstr(tt_fb, "RIGHT") != NULL, "feedback RIGHT after right");
    int c2 = tt_combo;
    tt_type_answer(tt_a * tt_b + 2);
    CHECK(tt_combo == 0 && c2 > 0, "combo streak broken");
}

static void test_level_cap(void) {
    /* 等级封顶 LV4, 再答对不超 */
    rng_seed(&tt_rng, 5);
    tt_lvl = 4; tt_right = 40; tt_combo = 3;
    tt_last_a = -1; tt_last_b = -1;
    tt_make_q();
    tt_type_answer(tt_a * tt_b);
    CHECK(tt_lvl == 4, "level capped at 4");
    CHECK(tt_right == 41, "right still counts at cap");
}

static void test_timer(void) {
    /* 600 tick = 60s 到点 -> over */
    tt_over = false;
    tt_elapsed = 0;
    tt_ansn = 1;
    for (int i = 0; i < 599; i++) timestrain_tick(0);
    CHECK(!tt_over && tt_elapsed == TT_GAME_MS - TT_TICK_MS, "not over before 60s");
    timestrain_tick(0);
    CHECK(tt_over, "over at 60s");
    CHECK(tt_elapsed == TT_GAME_MS, "elapsed clamped to 60s");
    CHECK(tt_ansn == 0, "answer cleared at over");
    timestrain_tick(0);                          /* over 后 tick 无效 */
    CHECK(tt_elapsed == TT_GAME_MS, "tick ignored after over");
}

static void test_retry_and_exit(void) {
    /* 结束页 OK 新局 / BACK 退出 */
    tt_over = true;
    tt_score = 42; tt_right = 7; tt_lvl = 2;
    key_event_t ev = { K_OK, 0, false };
    timestrain_on_key(&ev);
    CHECK(!tt_over && tt_score == 0 && tt_right == 0 && tt_lvl == 1,
          "retry resets game");
    s_exit_request = false;
    tt_over = true;
    ev.key = K_QUIT;
    timestrain_on_key(&ev);
    CHECK(s_exit_request, "quit sets exit request");
}

static void test_onkey_flow(void) {
    /* 按键流: 数字->OK, 重复忽略, N 换题, 数字超位 */
    rng_seed(&tt_rng, 77);
    tt_new_game();
    int a = tt_a, b = tt_b;
    key_event_t ev = { K_CHAR, 0, false };
    ev.ch = (char)('0' + (a * b) / 10);
    ev.is_repeat = true;                         /* 重复按键忽略 */
    timestrain_on_key(&ev);
    CHECK(tt_ansn == 0, "repeat key ignored");
    ev.is_repeat = false;
    timestrain_on_key(&ev);
    CHECK(tt_ansn == 1, "first digit entered");
    ev.ch = (char)('0' + (a * b) % 10);
    timestrain_on_key(&ev);
    CHECK(tt_ansn == 2, "second digit entered");
    ev.ch = '5';                                 /* 第 3 位被丢弃 */
    timestrain_on_key(&ev);
    CHECK(tt_ansn == 2, "third digit capped");
    ev.key = K_OK; ev.ch = 0;
    timestrain_on_key(&ev);
    CHECK(tt_right == 1 && tt_score == 5, "OK submits correct answer");
    /* N 换题: 已输入清空, 出新题 */
    tt_ansn = 1;
    ev.key = K_CHAR; ev.ch = 'n';
    timestrain_on_key(&ev);
    CHECK(tt_ansn == 0, "N clears input");
    CHECK(tt_a >= 2 && tt_a <= 5 && tt_b >= 2 && tt_b <= 5, "N new question LV1");
}

int main(void) {
    test_ranges();
    test_digits();
    test_combo_scoring();
    test_play_and_levelup();
    test_wrong_answer();
    test_level_cap();
    test_timer();
    test_retry_and_exit();
    test_onkey_flow();
    if (s_fail == 0) { printf("ALL TESTS PASSED\n"); return 0; }
    printf("%d FAILURES\n", s_fail);
    return 1;
}
