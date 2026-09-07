/* MEMORY DIGITS host 逻辑测试: 直接包含 memdigits.c, 断言核心逻辑 */
#include "../../src/games/memdigits.c"
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
    memdigits_on_key(&ev);
}

static void press_key(ccg_key k) {
    key_event_t ev = { k, 0, false };
    memdigits_on_key(&ev);
}

/* 填充正确/错误答案(直接驱动静态状态) */
static void fill_correct(void) {
    md_in = 0;
    for (int i = 0; i < md_len; i++) md_input[i] = md_target[i];
    md_in = md_len;
}

static void fill_wrong(void) {
    md_in = 0;
    for (int i = 0; i < md_len; i++)
        md_input[i] = (char)('0' + (md_target[i] - '0' + 1) % 10);
    md_in = md_len;
}

/* 从 FEEDBACK/SHOW 推进到 INPUT 输入窗口(不依赖真实时钟) */
static void to_input(void) {
    int guard = 0;
    while (md_state != MD_INPUT && !md_over && ++guard < 6) {
        md_until = 0;
        memdigits_tick(1);
    }
}

/* ---- 开局初始化 ---- */
static void test_new_game(void) {
    md_new_game();
    CHECK(md_level == 1, "new game level 1");
    CHECK(md_len == 3, "level 1 -> 3 digits");
    CHECK(md_state == MD_SHOW && !md_over, "new game starts in SHOW");
    CHECK(md_score == 0 && md_correct == 0, "score/correct reset");
    CHECK(md_streak == 0 && md_wrong == 0, "streaks reset");
    CHECK(md_in == 0 && md_prev_len == 3, "input empty, prev recorded");
    CHECK(md_target[0] >= '0' && md_target[0] <= '9', "target digits 0-9");
}

/* ---- 题目生成: 各等级位数与数字范围 ---- */
static void test_gen(void) {
    rng_seed(&md_rng, 42);
    int ok = 1;
    for (int lvl = 1; lvl <= MD_MAX_LVL; lvl++) {
        md_level = lvl;
        md_prev_len = -1;
        for (int i = 0; i < 300; i++) {
            md_make_q();
            int want = lvl + MD_MIN_LEN > MD_MAX ? MD_MAX : lvl + MD_MIN_LEN;
            if (md_len != want) ok = 0;
            for (int j = 0; j < md_len; j++)
                if (md_target[j] < '0' || md_target[j] > '9') ok = 0;
            if (md_in != 0) ok = 0;
            if (!ok) { printf("lvl=%d len=%d\n", lvl, md_len); break; }
        }
    }
    CHECK(ok, "question gen valid for all levels (len=level+2, digits 0-9)");
}

/* ---- 连续题目不重复(do-while guard) ---- */
static void test_no_dupe(void) {
    rng_seed(&md_rng, 7);
    md_level = 2;
    md_prev_len = -1;
    md_make_q();
    int ok = 1;
    for (int i = 0; i < 300; i++) {
        char prev[MD_MAX];
        int plen = md_len;
        memcpy(prev, md_target, MD_MAX);
        md_make_q();
        if (md_len == plen && memcmp(prev, md_target, (size_t)md_len) == 0) ok = 0;
    }
    CHECK(ok, "consecutive questions never identical (guard loop)");
}

/* ---- md_same 辅助函数 ---- */
static void test_same(void) {
    md_len = 3;
    md_target[0] = '1'; md_target[1] = '2'; md_target[2] = '3';
    md_prev[0] = '1';   md_prev[1] = '2';   md_prev[2] = '3';
    CHECK(md_same(), "md_same true on equal strings");
    md_prev[2] = '4';
    CHECK(!md_same(), "md_same false on differing strings");
}

/* ---- 升级流程: 3 连对升 1 级, 分数 = 正确数 x 等级 ---- */
static void test_level_up(void) {
    rng_seed(&md_rng, 99);
    md_new_game();
    md_prev_len = -1;              /* 重定种子后重新出题 */
    md_make_q();
    for (int i = 0; i < 3; i++) {
        md_state = MD_INPUT;
        fill_correct();
        md_submit();
        CHECK(md_state == MD_FEEDBACK, "submit enters feedback");
        to_input();
    }
    CHECK(md_level == 2, "3 correct in a row -> level 2");
    CHECK(md_len == 4, "level 2 -> 4 digits");
    CHECK(md_correct == 3 && md_score == 3 * 2, "score = correct x level");
    CHECK(md_streak == 0, "streak reset after level up");
    CHECK(md_wrong == 0, "wrong streak untouched by corrects");
}

/* ---- 连对不足 3 不升级, 分数随正确数累加 ---- */
static void test_partial_streak(void) {
    rng_seed(&md_rng, 3);
    md_new_game();
    md_prev_len = -1;
    md_make_q();
    md_state = MD_INPUT;
    fill_correct();
    md_submit();
    CHECK(md_level == 1 && md_streak == 1 && md_score == 1, "1 correct no level up");
    to_input();
    fill_correct();
    md_submit();
    CHECK(md_level == 1 && md_streak == 2 && md_score == 2, "2 correct still level 1");
}

/* ---- 答错: 降级 + 连错计数; 答对清零连错 ---- */
static void test_wrong_flow(void) {
    rng_seed(&md_rng, 5);
    md_new_game();
    md_prev_len = -1;
    md_make_q();
    /* 升到 2 级: score 应为 3*2=6 */
    for (int i = 0; i < 3; i++) {
        md_state = MD_INPUT;
        fill_correct();
        md_submit();
        to_input();
    }
    CHECK(md_level == 2 && md_score == 6, "at level 2 score 6");
    md_state = MD_INPUT;
    fill_wrong();
    md_submit();
    CHECK(!md_over && md_wrong == 1, "1st wrong: not over, wrong=1");
    CHECK(md_level == 1, "wrong -> level down");
    CHECK(md_score == 3, "score recomputed = correct x level");
    CHECK(md_streak == 0, "wrong resets correct streak");
    /* 答对 → 连错清零 */
    to_input();
    fill_correct();
    md_submit();
    CHECK(md_wrong == 0, "correct clears wrong streak");
    CHECK(md_score == 4, "score back up with correct");
}

/* ---- FAIL: 同等级 2 连错 ---- */
static void test_fail(void) {
    rng_seed(&md_rng, 11);
    md_new_game();
    md_prev_len = -1;
    md_make_q();
    md_state = MD_INPUT;
    fill_wrong();
    md_submit();
    CHECK(!md_over && md_wrong == 1, "first wrong not fatal");
    to_input();
    fill_wrong();
    md_submit();
    CHECK(md_over, "2 consecutive wrongs -> FAIL");
    CHECK(md_state == MD_FEEDBACK, "over flagged at submit");
}

/* ---- 输入边界: 位数上限/退格/展示期拒收/半程提交忽略 ---- */
static void test_input_edges(void) {
    rng_seed(&md_rng, 13);
    md_new_game();
    md_prev_len = -1;
    md_make_q();
    /* 展示期输入被拒 */
    md_state = MD_SHOW;
    press_char('5');
    CHECK(md_in == 0, "no input during SHOW");
    /* 输入期(等级 1 目标 3 位) */
    md_state = MD_INPUT;
    press_char('0');
    press_char('9');
    press_char('8');
    CHECK(md_in == md_len && md_len == 3 && md_input[0] == '0' &&
          md_input[2] == '8', "digits entered via keys");
    /* 满位后再输被拒 */
    press_char('7');
    CHECK(md_in == md_len, "typing capped at full length");
    /* 退格 */
    press_key(K_DEL);
    CHECK(md_in == md_len - 1, "backspace removes last digit");
    /* 半程提交忽略 */
    md_submit();
    CHECK(md_state == MD_INPUT, "partial submit ignored");
    /* 填满再验满位拒绝 */
    md_in = md_len;
    press_char('1');
    CHECK(md_in == md_len, "typing capped at full length");
    /* 重复键忽略 */
    {
        key_event_t rep = { K_OK, 0, true };
        md_state = MD_INPUT;
        memdigits_on_key(&rep);
        CHECK(md_state == MD_INPUT, "repeat key ignored (no submit)");
    }
}

/* ---- 满位提交走 on_key 通路 ---- */
static void test_key_submit(void) {
    rng_seed(&md_rng, 17);
    md_new_game();
    md_prev_len = -1;
    md_make_q();
    md_state = MD_INPUT;
    fill_correct();
    press_key(K_OK);
    CHECK(md_state == MD_FEEDBACK && md_correct == 1, "OK submits via on_key");
}

/* ---- tick 状态机: SHOW->INPUT->(FEEDBACK)->SHOW ---- */
static void test_tick(void) {
    md_new_game();
    md_until = 0;
    memdigits_tick(1000);
    CHECK(md_state == MD_INPUT, "show expires -> input");
    md_state = MD_FEEDBACK;
    md_until = 0;
    memdigits_tick(1);
    CHECK(md_state == MD_SHOW && md_in == 0, "feedback expires -> new show");
    md_until = 0;
    memdigits_tick(2);
    CHECK(md_state == MD_INPUT, "second show expires -> input");
    /* over 时 tick 不动作 */
    md_over = true;
    md_state = MD_SHOW;
    memdigits_tick(3);
    CHECK(md_state == MD_SHOW, "tick inert when over");
}

/* ---- 等级上限 8(10 位数字), 不越界升级 ---- */
static void test_max_level(void) {
    rng_seed(&md_rng, 23);
    md_new_game();
    md_level = MD_MAX_LVL;
    md_prev_len = -1;
    md_make_q();
    CHECK(md_len == 10, "max level -> 10 digits");
    for (int i = 0; i < 3; i++) {
        md_state = MD_INPUT;
        fill_correct();
        md_submit();
        to_input();
    }
    CHECK(md_level == MD_MAX_LVL, "no level up beyond max");
    CHECK(md_correct == 3 && md_score == 3 * MD_MAX_LVL, "score capped level still counts");
    /* 满位提交与最多 10 位输入一致 */
    md_in = md_len;
    press_char('9');
    CHECK(md_in == md_len, "10 digit cap respected");
}

/* ---- 结束界面按键: OK/N 重开, BACK/QUIT 退出 ---- */
static void test_over_keys(void) {
    rng_seed(&md_rng, 29);
    md_new_game();
    md_over = true;
    s_exit_request = false;
    press_key(K_OK);
    CHECK(!md_over && md_level == 1 && md_score == 0, "OK on over -> new game");
    md_over = true;
    press_key(K_BACK);
    CHECK(s_exit_request, "BACK on over -> exit request");
    s_exit_request = false;
    md_over = true;
    press_char('n');
    CHECK(!md_over, "N on over -> new game");
    /* 游戏中 N 重开 */
    press_char('n');
    CHECK(md_level == 1 && md_score == 0, "N during play -> restart");
}

int main(void) {
    test_new_game();
    test_gen();
    test_no_dupe();
    test_same();
    test_level_up();
    test_partial_streak();
    test_wrong_flow();
    test_fail();
    test_input_edges();
    test_key_submit();
    test_tick();
    test_max_level();
    test_over_keys();
    if (s_fail) {
        printf("\n%d FAILURE(S)\n", s_fail);
        return 1;
    }
    printf("\nALL TESTS PASSED\n");
    return 0;
}
