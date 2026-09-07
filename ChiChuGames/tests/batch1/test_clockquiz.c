/* CLOCK QUIZ host 逻辑测试: 直接包含 clockquiz.c, 断言核心逻辑 */
#include "../../src/games/clockquiz.c"
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

static void press_char(char c) {
    key_event_t ev = { K_CHAR, (uint8_t)c, false };
    clockquiz_on_key(&ev);
}

static void press_key(ccg_key k) {
    key_event_t ev = { k, 0, false };
    clockquiz_on_key(&ev);
}

/* 输入当前题目的正确答案 */
static void type_answer(void) {
    char buf[8];
    snprintf(buf, sizeof(buf), "%02d%02d", cq_hour, cq_min);
    for (char *p = buf; *p; p++) press_char(*p);
}

/* 构造一个必然错误的答案 */
static void type_wrong(void) {
    char buf[8];
    snprintf(buf, sizeof(buf), "%02d%02d", (cq_hour + 1) % 24,
             (cq_min + 15) % 60);
    for (char *p = buf; *p; p++) press_char(*p);
}

/* ---- 指针角度映射: 确定性 + 12h 时针含分钟偏移 ---- */
static void test_hand_deg(void) {
    CHECK(cq_hand_deg(12, 0, true) == 0, "12:00 hour hand 0 deg");
    CHECK(cq_hand_deg(3, 0, true) == 90, "3:00 hour hand 90 deg");
    CHECK(cq_hand_deg(6, 30, true) == 195, "6:30 hour hand 180+15 deg");
    CHECK(cq_hand_deg(18, 45, true) == 202, "18:45 hour hand 180+22 deg");
    CHECK(cq_hand_deg(9, 0, true) == 270, "9:00 hour hand 270 deg");
    CHECK(cq_hand_deg(13, 0, true) == 30, "13:00 hour hand = 1 o'clock");
    CHECK(cq_hand_deg(5, 15, true) == 157, "5:15 hour hand 150+7 deg");
    CHECK(cq_hand_deg(0, 15, false) == 90, "minute 15 -> 90 deg");
    CHECK(cq_hand_deg(0, 30, false) == 180, "minute 30 -> 180 deg");
    CHECK(cq_hand_deg(0, 45, false) == 270, "minute 45 -> 270 deg");
    CHECK(cq_hand_deg(0, 0, false) == 0, "minute 0 -> 0 deg");
    /* sin 表关键值 + cos 复用 sin(d+90) */
    CHECK(cq_sin[0] == 0 && cq_sin[90] == 64, "sin 0/90 table");
    CHECK(cq_sin[180] == 0 && cq_sin[270] == -64, "sin 180/270 table");
    CHECK(cq_sin[30] == 32 && cq_sin[150] == 32, "sin 30/150 table");
    CHECK(cq_sin[(0 + 90) % 360] == 64, "cos 0 via sin(90)");
    CHECK(cq_sin[(90 + 90) % 360] == 0, "cos 90 via sin(180)");
    CHECK(cq_sin[(180 + 90) % 360] == -64, "cos 180 via sin(270)");
}

/* ---- 题目生成: 小时 1..23, 分钟 {0,15,30,45}, 连续不重复 ---- */
static void test_gen(void) {
    rng_seed(&cq_rng, 42);
    cq_last_h = -1;
    cq_last_m = -1;
    int ok = 1, min_ok[4] = {0};
    int prev_h = -2, prev_m = -2;
    for (int i = 0; i < 500; i++) {
        cq_make_q();
        if (cq_hour < 1 || cq_hour > 23) ok = 0;
        if (cq_min != 0 && cq_min != 15 && cq_min != 30 && cq_min != 45) ok = 0;
        if (cq_min % 15 != 0) ok = 0;
        min_ok[cq_min / 15]++;
        if (cq_hour == prev_h && cq_min == prev_m) { ok = 0; break; }
        prev_h = cq_hour;
        prev_m = cq_min;
    }
    CHECK(ok, "question valid over 500 gens");
    CHECK(min_ok[0] > 0 && min_ok[1] > 0 && min_ok[2] > 0 && min_ok[3] > 0,
          "all quarter-minute buckets seen");
}

/* ---- 提交: 空/不足四位 / 对 +10 / 错展示答案 ---- */
static void test_submit(void) {
    rng_seed(&cq_rng, 99);
    cq_new_game();
    int q0_h = cq_hour, q0_m = cq_min;
    /* 空提交: 忽略, 题目不变 */
    press_key(K_OK);
    CHECK(cq_score == 0 && cq_hour == q0_h && cq_min == q0_m,
          "empty submit ignored");
    /* 不足四位: NEED 4 DIGITS, 输入保留 */
    press_char('1'); press_char('2');
    press_key(K_OK);
    CHECK(cq_score == 0 && cq_ansn == 2, "partial submit keeps entry");
    CHECK(strcmp(cq_fb, "NEED 4 DIGITS") == 0, "NEED 4 DIGITS feedback");
    cq_ansn = 0;
    /* 正确答案: +10, 换题, 反馈 OK +10 */
    type_answer();
    CHECK(cq_ansn == 4, "4 digits entered");
    press_key(K_OK);
    CHECK(cq_score == 10, "correct +10");
    CHECK(cq_right == 1 && cq_total == 1, "right/total counters");
    CHECK(cq_phase == CQ_PH_PLAY && cq_ansn == 0, "play phase, entry cleared");
    CHECK(strcmp(cq_fb, "OK +10") == 0, "OK +10 feedback");
    CHECK(cq_hour != q0_h || cq_min != q0_m, "question advanced after correct");
    /* 错答案: 进入 ANS 阶段, 反馈含正确时间 */
    type_wrong();
    press_key(K_OK);
    CHECK(cq_phase == CQ_PH_ANS, "wrong answer enters ANS phase");
    CHECK(cq_ans_left == CQ_ANS_MS, "ANS timer 2000ms");
    CHECK(cq_score == 10, "wrong no score");
    {
        char expect[24];
        snprintf(expect, sizeof(expect), "WRONG %02d:%02d", cq_hour, cq_min);
        CHECK(strcmp(cq_fb, expect) == 0, "feedback shows correct time");
    }
    CHECK(cq_ansn == 0, "entry cleared after wrong");
}

/* ---- ANS 阶段: 2 tick 自动换题; OK/N 提前换题; 数字输入忽略 ---- */
static void test_ans_phase(void) {
    rng_seed(&cq_rng, 7);
    cq_new_game();
    type_wrong();
    press_key(K_OK);
    CHECK(cq_phase == CQ_PH_ANS, "ANS entered");
    int h0 = cq_hour, m0 = cq_min;
    clockquiz_tick(0);
    CHECK(cq_phase == CQ_PH_ANS && cq_ans_left == 1000,
          "ANS persists after 1 tick");
    press_char('5');                       /* ANS 期间数字忽略 */
    CHECK(cq_ansn == 0, "digits ignored during ANS");
    CHECK(cq_hour == h0 && cq_min == m0, "question frozen during ANS");
    clockquiz_tick(0);
    CHECK(cq_phase == CQ_PH_PLAY, "ANS auto-advances after 2s");
    CHECK(cq_hour != h0 || cq_min != m0, "new question after ANS");
    /* OK 提前换题 */
    type_wrong();
    press_key(K_OK);
    h0 = cq_hour; m0 = cq_min;
    press_key(K_OK);
    CHECK(cq_phase == CQ_PH_PLAY && (cq_hour != h0 || cq_min != m0),
          "OK skips ANS early");
    /* N 提前换题 */
    type_wrong();
    press_key(K_OK);
    h0 = cq_hour; m0 = cq_min;
    press_char('n');
    CHECK(cq_phase == CQ_PH_PLAY && (cq_hour != h0 || cq_min != m0),
          "N skips ANS early");
}

/* ---- 输入: 4 位上限 + DEL 退格 + 重复忽略 + N 跳过 ---- */
static void test_entry(void) {
    rng_seed(&cq_rng, 11);
    cq_new_game();
    press_char('1'); press_char('2'); press_char('3'); press_char('4');
    CHECK(cq_ansn == 4 && cq_ans[3] == '4', "4 digits entered");
    press_char('5');
    CHECK(cq_ansn == 4, "5th digit capped");
    press_key(K_DEL);
    CHECK(cq_ansn == 3 && cq_ans[2] == '3', "DEL backspace");
    press_key(K_DEL); press_key(K_DEL); press_key(K_DEL);
    CHECK(cq_ansn == 0, "DEL to empty");
    press_key(K_DEL);
    CHECK(cq_ansn == 0, "DEL at empty no-op");
    /* 前导零: 07 45 被原样存储 */
    press_char('0'); press_char('7');
    CHECK(cq_ans[0] == '0' && cq_ans[1] == '7', "leading zeros stored");
    cq_ansn = 0;
    /* 重复事件忽略 */
    key_event_t ev = { K_CHAR, '3', true };
    clockquiz_on_key(&ev);
    CHECK(cq_ansn == 0, "repeat char ignored");
    ev.key = K_OK; ev.is_repeat = true;
    clockquiz_on_key(&ev);
    CHECK(cq_score == 0, "repeat OK ignored");
    /* N 跳过: 清输入换题, 无惩罚 */
    int h0 = cq_hour, m0 = cq_min;
    press_char('9'); press_char('9');
    press_char('n');
    CHECK(cq_ansn == 0, "N clears entry");
    CHECK(cq_hour != h0 || cq_min != m0, "N skips to different question");
    CHECK(cq_score == 0, "skip no penalty");
}

/* ---- 限时: 30 tick 到点, 结束态按键 ---- */
static void test_timer(void) {
    rng_seed(&cq_rng, 3);
    cq_new_game();
    for (int i = 0; i < 29; i++) clockquiz_tick(0);
    CHECK(cq_phase == CQ_PH_PLAY, "not over at 29s");
    CHECK(cq_elapsed == 29000u, "elapsed 29000ms");
    clockquiz_tick(0);
    CHECK(cq_phase == CQ_PH_OVER, "over at 30s");
    CHECK(cq_elapsed == CQ_GAME_MS, "elapsed capped at 30s");
    for (int i = 0; i < 20; i++) clockquiz_tick(0);
    CHECK(cq_elapsed == CQ_GAME_MS && cq_phase == CQ_PH_OVER,
          "no accumulation after over");
    /* 结束: 输入忽略 */
    press_char('5');
    CHECK(cq_ansn == 0, "digits ignored when over");
    /* 结束后 OK -> 新局 */
    press_key(K_OK);
    CHECK(cq_phase == CQ_PH_PLAY && cq_score == 0 && cq_right == 0,
          "OK restarts game");
    cq_phase = CQ_PH_OVER;
    press_char('n');
    CHECK(cq_phase == CQ_PH_PLAY && cq_score == 0, "N restarts game when over");
    /* 结束后 BACK/Q -> 退出 */
    cq_phase = CQ_PH_OVER;
    s_exit_request = false;
    press_key(K_BACK);
    CHECK(s_exit_request, "BACK quits when over");
    cq_phase = CQ_PH_OVER;
    s_exit_request = false;
    press_key(K_QUIT);
    CHECK(s_exit_request, "Q quits when over");
}

/* ---- 分数累计 + 限时内 ANS 仍计时 ---- */
static void test_score_time(void) {
    rng_seed(&cq_rng, 21);
    cq_new_game();
    for (int i = 0; i < 3; i++) {
        type_answer();
        press_key(K_OK);
    }
    CHECK(cq_score == 30 && cq_right == 3 && cq_total == 3,
          "3 correct = 30 points");
    /* 错答案期间 tick 仍消耗本轮时间 */
    cq_new_game();
    type_wrong();
    press_key(K_OK);
    clockquiz_tick(0);
    CHECK(cq_elapsed == 1000u, "round timer runs during ANS");
    CHECK(cq_score == 0 && cq_total == 1, "wrong counts as answered");
    /* 答错计入 total 但不计 right */
    CHECK(cq_right == 0, "wrong not counted right");
}

/* ---- 渲染冒烟: 三个阶段均不崩溃(含结束全刷路径) ---- */
static void test_render(void) {
    rng_seed(&cq_rng, 1);
    cq_new_game();
    clockquiz_render();
    cq_phase = CQ_PH_ANS;
    clockquiz_render();
    cq_phase = CQ_PH_OVER;
    clockquiz_render();
    CHECK(cq_over_full, "over full-flag set");
    cq_phase = CQ_PH_PLAY;
    clockquiz_render();
    CHECK(!cq_over_full || cq_phase == CQ_PH_PLAY, "play render ok");
}

/* ---- 游戏中 Q 退出 ---- */
static void test_quit_play(void) {
    rng_seed(&cq_rng, 33);
    cq_new_game();
    s_exit_request = false;
    press_key(K_QUIT);
    CHECK(s_exit_request, "Q exits during play");
}

int main(void) {
    test_hand_deg();
    test_gen();
    test_submit();
    test_ans_phase();
    test_entry();
    test_timer();
    test_score_time();
    test_render();
    test_quit_play();

    printf("%s\n", s_fail ? "TESTS FAILED" : "ALL TESTS PASSED");
    return s_fail ? 1 : 0;
}
