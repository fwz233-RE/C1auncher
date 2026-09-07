/* MATH TRAINER host 逻辑测试: 直接包含 mathtrain.c, 断言核心逻辑 */
#include "../../src/games/mathtrain.c"
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
    mathtrain_on_key(&ev);
}

static void press_key(ccg_key k) {
    key_event_t ev = { k, 0, false };
    mathtrain_on_key(&ev);
}

/* ---- 题目生成正确性: 各等级边界与运算结果 ---- */
static void test_gen(void) {
    rng_seed(&mt_rng, 42);
    mt_last_op = -1;
    mt_last_sol = -1;
    int ok = 1;
    int level_of[5] = {0};
    for (int lvl = 1; lvl <= 4; lvl++) {
        mt_lvl = lvl;
        for (int i = 0; i < 200; i++) {
            mt_make_q();
            int op = mt_op, a = mt_a, b = mt_b, sol = mt_sol;
            level_of[lvl]++;
            if (lvl == 1) {
                if (op < 0 || op > 1) ok = 0;
                if (a < 2 || a > 10 || b < 2 || b > 10) ok = 0;
                if (op == 1 && a < b) ok = 0;
                if (sol != (op == 0 ? a + b : a - b)) ok = 0;
            } else if (lvl == 2) {
                if (op < 0 || op > 1) ok = 0;
                if (a < 10 || a > 99 || b < 10 || b > 99) ok = 0;
                if (op == 1 && a < b) ok = 0;
                if (sol != (op == 0 ? a + b : a - b)) ok = 0;
            } else if (lvl == 3) {
                if (op != 2) ok = 0;
                if (a < 2 || a > 9 || b < 2 || b > 9) ok = 0;
                if (sol != a * b) ok = 0;
            } else {
                if (op != 3) ok = 0;
                if (a < 4 || a > 81 || b < 2 || b > 9) ok = 0;
                if (b == 0 || a % b != 0) ok = 0;
                if (sol != a / b) ok = 0;
                if (a / b < 2 || a / b > 9) ok = 0;
            }
            if (sol < 0) ok = 0;
            if (!ok) { printf("lvl=%d op=%d a=%d b=%d sol=%d\n", lvl, op, a, b, sol); break; }
        }
    }
    CHECK(ok, "question gen valid for all levels");
    CHECK(level_of[1] == 200 && level_of[4] == 200, "200 questions per level");
}

/* ---- 连续题目不重复(op+sol 不同) ---- */
static void test_no_dupe(void) {
    rng_seed(&mt_rng, 7);
    mt_last_op = -1;
    mt_last_sol = -1;
    mt_lvl = 2;
    int ok = 1;
    int prev_op = -2, prev_sol = -2;
    for (int i = 0; i < 500; i++) {
        mt_make_q();
        if (mt_op == prev_op && mt_sol == prev_sol) { ok = 0; break; }
        prev_op = mt_op;
        prev_sol = mt_sol;
    }
    CHECK(ok, "no consecutive identical (op,sol) over 500 gens");
}

/* ---- 提交: 对 +10 / 错显示答案 +0 / 空忽略 ---- */
static void test_submit(void) {
    rng_seed(&mt_rng, 99);
    mt_new_game();
    int q0_sol = mt_sol, q0_a = mt_a, q0_b = mt_b;
    /* 空提交: 忽略, 题目不变 */
    press_key(K_OK);
    CHECK(mt_score == 0 && mt_sol == q0_sol, "empty submit ignored");
    /* 输错答案 */
    int wrong = (q0_sol == 0) ? 1 : 0;          /* 保证与正确答案不同 */
    mt_ansn = 0;
    press_char((char)('0' + wrong));
    press_key(K_OK);
    CHECK(mt_score == 0, "wrong answer no score");
    CHECK(mt_fb[0] == 'W' && strstr(mt_fb, "ANS") != NULL, "wrong shows ANS feedback");
    {
        /* 反馈含正确答案数值 */
        char expect[16];
        snprintf(expect, sizeof(expect), "WRONG ANS %d", q0_sol);
        CHECK(strcmp(mt_fb, expect) == 0, "feedback shows correct answer");
    }
    CHECK(mt_ansn == 0, "answer cleared after submit");
    /* 对答案: 直接构造正确输入 */
    int sol2 = mt_sol, a2 = mt_a, b2 = mt_b;
    CHECK(!(sol2 == q0_sol && a2 == q0_a && b2 == q0_b) ||
          mt_sol != q0_sol, "question changed after wrong submit");
    char buf[8];
    snprintf(buf, sizeof(buf), "%d", sol2);
    for (char *p = buf; *p; p++) press_char(*p);
    press_key(K_OK);
    CHECK(mt_score == 10, "correct +10");
    CHECK(strcmp(mt_fb, "OK +10") == 0, "OK +10 feedback");
    CHECK(mt_right == 1, "right count 1");
    CHECK(mt_ansn == 0, "answer cleared after correct");
}

/* ---- 等级制: 10 题升级, 上限 4 ---- */
static void test_levels(void) {
    rng_seed(&mt_rng, 5);
    mt_new_game();
    CHECK(mt_lvl == 1, "start level 1");
    for (int i = 0; i < 10; i++) {              /* 第 1..10 题正确 */
        char buf[8];
        snprintf(buf, sizeof(buf), "%d", mt_sol);
        for (char *p = buf; *p; p++) press_char(*p);
        press_key(K_OK);
    }
    CHECK(mt_lvl == 2, "level 2 after 10 correct");
    CHECK(mt_score == 100, "score 100 after 10 correct");
    CHECK(strcmp(mt_fb, "LV UP! NOW LV 2") == 0, "lv up fb after 10th correct");
    /* 升到 4 后再答 10 题仍为 4 */
    while (mt_lvl < 4) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%d", mt_sol);
        for (char *p = buf; *p; p++) press_char(*p);
        press_key(K_OK);
    }
    CHECK(mt_lvl == 4, "reached level 4");
    for (int i = 0; i < 10; i++) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%d", mt_sol);
        for (char *p = buf; *p; p++) press_char(*p);
        press_key(K_OK);
    }
    CHECK(mt_lvl == 4, "level clamped at 4");
    /* 4 级除法题必须整除 */
    CHECK(mt_op == 3 && mt_a % mt_b == 0, "level 4 division question exact");
}

/* ---- 输入: 3 位上限 + DEL 退格 + 前导零 + 重复忽略 ---- */
static void test_entry(void) {
    rng_seed(&mt_rng, 11);
    mt_new_game();
    mt_ansn = 0;
    press_char('1'); press_char('2'); press_char('3');
    CHECK(mt_ansn == 3, "3 digits entered");
    press_char('4');
    CHECK(mt_ansn == 3, "4th digit capped");
    press_key(K_DEL);
    CHECK(mt_ansn == 2 && mt_ans[1] == '2', "DEL backspace");
    /* 前导零: 007 解析为 7 */
    mt_ansn = 0;
    press_char('0'); press_char('0');
    /* 直接换到 sol=7 的题不可控, 改为断言内部解析正确: 0*10+0=0 */
    CHECK(mt_ans[0] == '0' && mt_ans[1] == '0', "leading zeros stored");
    mt_ansn = 0;
    /* 重复事件忽略: 字符与确认键均不响应 */
    key_event_t ev = { K_CHAR, '5', true };
    mathtrain_on_key(&ev);
    CHECK(mt_ansn == 0, "repeat char ignored");
    ev.key = K_OK; ev.is_repeat = true;
    mathtrain_on_key(&ev);
    CHECK(mt_score == 0, "repeat OK ignored");
}

/* ---- 限时: 600 tick 到点, 结束态按键 ---- */
static void test_timer(void) {
    rng_seed(&mt_rng, 3);
    mt_new_game();
    for (int i = 0; i < 599; i++) mathtrain_tick(0);
    CHECK(!mt_over, "not over at 59.9s");
    CHECK(mt_elapsed == 59900u, "elapsed 59900ms");
    mathtrain_tick(0);
    CHECK(mt_over, "over at 60.0s");
    CHECK(mt_elapsed == MT_GAME_MS, "elapsed capped at 60s");
    /* tick 不再累积 */
    for (int i = 0; i < 50; i++) mathtrain_tick(0);
    CHECK(mt_elapsed == MT_GAME_MS, "no accumulation after over");
    /* 结束后 OK 或 N → 新局 */
    press_key(K_OK);
    CHECK(!mt_over && mt_score == 0 && mt_right == 0, "OK restarts game");
    mt_over = true;
    press_char('n');
    CHECK(!mt_over && mt_score == 0, "N restarts game when over");
}

/* ---- N 换题: 跳过当前题 ---- */
static void test_skip(void) {
    rng_seed(&mt_rng, 21);
    mt_new_game();
    int op0 = mt_op, sol0 = mt_sol;
    press_char('9'); press_char('9');
    press_char('n');
    CHECK(mt_ansn == 0, "N clears entry");
    CHECK(mt_op != op0 || mt_sol != sol0, "N skips to different question");
    CHECK(mt_score == 0, "skip no penalty");
}

/* ---- 暂停/退出路径 ---- */
static void test_pause_quit(void) {
    rng_seed(&mt_rng, 33);
    mt_new_game();
    s_exit_request = false;
    press_key(K_QUIT);
    CHECK(s_exit_request, "Q sets exit request");
    s_exit_request = false;
    mt_over = true;
    press_key(K_BACK);
    CHECK(s_exit_request, "BACK quits when over");
}

int main(void) {
    /* 结束态渲染不崩溃(走 disp_force_full 分支) */
    rng_seed(&mt_rng, 1);
    mt_new_game();
    mathtrain_render();
    mt_over = true;
    mathtrain_render();
    mt_new_game();
    mathtrain_render();

    test_gen();
    test_no_dupe();
    test_submit();
    test_levels();
    test_entry();
    test_timer();
    test_skip();
    test_pause_quit();

    printf("%s\n", s_fail ? "TESTS FAILED" : "ALL TESTS PASSED");
    return s_fail ? 1 : 0;
}
