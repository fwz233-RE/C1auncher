/* RIDDLE QUIZ host 逻辑单测 — 断言核心逻辑
 * 编译: cc -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror -DCHICHU_HOST \
 *        -D_GNU_SOURCE -D_POSIX_C_SOURCE=200809L -Isrc -Isrc/gfx \
 *        /tmp/test_riddle.c src/gfx/canvas.c src/gfx/font.c src/gfx/font_data.c \
 *        src/gfx/pattern.c src/rng.c src/platform/time.c src/ui/ui_common.c \
 *        src/platform/input.c src/platform/display.c -o /tmp/test_riddle */
#include "../src/config.h"
#include "../src/gfx/canvas.h"
#include "../src/gfx/pattern.h"
#include "../src/rng.h"
#include "../src/games/riddle.c"
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

static key_event_t evk(ccg_key k) {
    key_event_t e; e.key = k; e.ch = 0; e.is_repeat = false; return e;
}
static key_event_t evc(char c) {
    key_event_t e; e.key = K_CHAR; e.ch = (uint8_t)c; e.is_repeat = false; return e;
}
static key_event_t evr(ccg_key k) {
    key_event_t e; e.key = k; e.ch = 0; e.is_repeat = true; return e;
}
static void press_ev(key_event_t e) { riddle_on_key(&e); }

/* 定位某答案的题号 */
static int find_q(const char *ans) {
    int i;
    for (i = 0; i < RQ_N; i++)
        if (strcmp(rq_riddles[i].a, ans) == 0) return i;
    return -1;
}

static void test_bank(void) {
    int i, j;
    CHECK(RQ_N == 30, "bank has 30 riddles");
    for (i = 0; i < RQ_N; i++) {
        char bad[64];
        const char *q = rq_riddles[i].q;
        const char *a = rq_riddles[i].a;
        snprintf(bad, sizeof(bad), "riddle %d q<=60", i);
        CHECK((int)strlen(q) <= 60, bad);
        snprintf(bad, sizeof(bad), "riddle %d a<=12", i);
        CHECK((int)strlen(a) <= 12, bad);
        CHECK(strlen(a) > 0, "answer non-empty");
        for (j = 0; a[j]; j++) {
            snprintf(bad, sizeof(bad), "riddle %d answer lowercase", i);
            CHECK(a[j] >= 'a' && a[j] <= 'z', bad);
        }
        /* 谜面单词都不超过折行宽度 */
        {
            int w = 0, m = 0;
            for (j = 0; ; j++) {
                if (!q[j] || q[j] == ' ') {
                    if (w > m) m = w;
                    if (!q[j]) break;
                    w = 0;
                } else w++;
            }
            snprintf(bad, sizeof(bad), "riddle %d word<=%d", i, RQ_LINE);
            CHECK(m <= RQ_LINE, bad);
        }
    }
    /* 答案两两不同(确定性强) */
    for (i = 0; i < RQ_N; i++)
        for (j = i + 1; j < RQ_N; j++) {
            char bad[64];
            snprintf(bad, sizeof(bad), "answers %d/%d distinct", i, j);
            CHECK(strcmp(rq_riddles[i].a, rq_riddles[j].a) != 0, bad);
        }
}

static void test_shuffle(void) {
    int i, seen[RQ_N], seed;
    for (i = 0; i < RQ_N; i++) seen[i] = 0;
    rng_seed(&rq_rng, 12345);
    rq_shuffle();
    for (i = 0; i < RQ_N; i++) seen[rq_order[i]]++;
    for (i = 0; i < RQ_N; i++) {
        char bad[64];
        snprintf(bad, sizeof(bad), "order %d exactly once", i);
        CHECK(seen[i] == 1, bad);
    }
    /* 同种子同序列(可复现) */
    {
        uint8_t a[RQ_N], b[RQ_N];
        rng_seed(&rq_rng, 999);
        rq_shuffle();
        memcpy(a, rq_order, sizeof(a));
        rng_seed(&rq_rng, 999);
        rq_shuffle();
        memcpy(b, rq_order, sizeof(b));
        CHECK(memcmp(a, b, sizeof(a)) == 0, "same seed -> same order");
    }
    /* 换种子序列不同(至少有一处不同) */
    for (seed = 1; seed < 50; seed++) {
        uint8_t a[RQ_N];
        rng_seed(&rq_rng, 12345);
        rq_shuffle();
        memcpy(a, rq_order, sizeof(a));
        rng_seed(&rq_rng, (uint64_t)seed);
        rq_shuffle();
        if (memcmp(a, rq_order, sizeof(a)) != 0) break;
    }
    CHECK(seed < 50, "different seed -> different order");
}

static void test_typing(void) {
    /* 字母输入 / 超长截断 / 退格 */
    rq_phase = RQ_PLAY; rq_ans_len = 0; rq_ans[0] = 0;
    press_ev(evc('c'));
    press_ev(evc('l'));
    press_ev(evc('o'));
    CHECK(rq_ans_len == 3 && strcmp(rq_ans, "clo") == 0, "letters append");
    press_ev(evr(K_DEL));               /* 重复 DEL 允许 */
    CHECK(rq_ans_len == 2 && strcmp(rq_ans, "cl") == 0, "repeat DEL backspace");
    press_ev(evk(K_DEL));
    CHECK(rq_ans_len == 1 && strcmp(rq_ans, "c") == 0, "DEL backspace");
    press_ev(evk(K_DEL));
    CHECK(rq_ans_len == 0, "DEL empties buffer");
    press_ev(evk(K_DEL));               /* 空缓冲退格无副作用 */
    CHECK(rq_ans_len == 0, "DEL on empty ignored");
    press_ev(evc('k'));
    while (rq_ans_len < RQ_ANS_MAX) press_ev(evc('x'));
    CHECK(rq_ans_len == RQ_ANS_MAX, "answer capped at 12");
    press_ev(evc('y'));
    CHECK(rq_ans_len == RQ_ANS_MAX && rq_ans[RQ_ANS_MAX - 1] == 'x',
          "13th char ignored");
    /* 大写归一 */
    rq_ans_len = 0; rq_ans[0] = 0;
    press_ev(evc('A'));
    press_ev(evc('B'));
    CHECK(strcmp(rq_ans, "ab") == 0, "uppercase normalized");
    /* 非字母忽略 */
    rq_ans_len = 0; rq_ans[0] = 0;
    press_ev(evc('0'));
    press_ev(evk(K_SPACE));
    CHECK(rq_ans_len == 0, "non-letter keys ignored");
}

static void test_submit(void) {
    int ci = find_q("clock");
    /* 答对: +10, 连对, 即时反馈 */
    rng_seed(&rq_rng, 7);
    rq_shuffle();
    rq_order[0] = (uint8_t)ci;
    rq_idx = 0; rq_ans_len = 0; rq_ans[0] = 0;
    rq_score = 0; rq_streak = 0; rq_max_streak = 0; rq_correct = 0;
    rq_phase = RQ_PLAY; rq_fb_until = 0;
    strcpy(rq_ans, "clock"); rq_ans_len = 5;
    rq_submit();
    CHECK(rq_phase == RQ_FB, "correct -> FB phase");
    CHECK(rq_fb_ok && !rq_fb_skip, "correct feedback flag");
    CHECK(rq_score == RQ_SCORE, "correct +10");
    CHECK(rq_streak == 1 && rq_max_streak == 1 && rq_correct == 1,
          "streak/correct tracked");
    CHECK(rq_fb_until > 0, "feedback timer set");
    /* 反馈期 OK 进入下一题 */
    press_ev(evk(K_OK));
    CHECK(rq_phase == RQ_PLAY && rq_idx == 1 && rq_ans_len == 0,
          "OK advances past feedback");
    /* 答错: 清连对, 亮答案 */
    rq_order[1] = (uint8_t)ci;
    strcpy(rq_ans, "watch"); rq_ans_len = 5;
    rq_submit();
    CHECK(rq_phase == RQ_FB && !rq_fb_ok, "wrong -> FB, not ok");
    CHECK(rq_score == RQ_SCORE && rq_streak == 0, "wrong: no score, streak reset");
    CHECK(rq_correct == 1, "correct count unchanged on wrong");
    {
        char fb[40];
        rq_fb_line(fb, sizeof(fb));
        CHECK(strcmp(fb, "WRONG! ANSWER: clock") == 0, "wrong feedback shows answer");
    }
    /* 空答案提交忽略 */
    rq_advance();
    CHECK(rq_phase == RQ_PLAY, "OK on FB advances (2)");
    rq_ans_len = 0; rq_ans[0] = 0;
    rq_submit();
    CHECK(rq_phase == RQ_PLAY, "empty submit ignored");
    /* 反馈自动推进(tick 超时) */
    rq_order[rq_idx] = (uint8_t)ci;
    strcpy(rq_ans, "clock"); rq_ans_len = 5;
    rq_submit();
    CHECK(rq_phase == RQ_FB, "fb before tick");
    rq_fb_until = 0;                       /* 模拟超时 */
    riddle_tick(1000);
    CHECK(rq_phase == RQ_PLAY, "tick timeout auto-advances");
    /* 连对累加 + 错误重置(当前题固定为 clock) */
    rq_order[rq_idx] = (uint8_t)ci;
    strcpy(rq_ans, "clock"); rq_ans_len = 5;
    rq_submit(); rq_fb_until = 0; riddle_tick(1000);
    rq_order[rq_idx] = (uint8_t)ci;
    strcpy(rq_ans, "clock"); rq_ans_len = 5;
    rq_submit(); rq_fb_until = 0; riddle_tick(1000);
    rq_order[rq_idx] = (uint8_t)ci;
    strcpy(rq_ans, "clock"); rq_ans_len = 5;
    rq_submit(); rq_fb_until = 0; riddle_tick(1000);
    CHECK(rq_streak == 4 && rq_max_streak == 4 && rq_correct == 5,
          "streak accumulates over 3 wins");
    rq_order[rq_idx] = (uint8_t)ci;
    strcpy(rq_ans, "zzzz"); rq_ans_len = 4;
    rq_submit();
    CHECK(rq_streak == 0 && rq_score == 50, "wrong resets streak, score kept");
}

static void test_skip(void) {
    int ci = find_q("clock");
    rng_seed(&rq_rng, 1);
    rq_shuffle();
    rq_order[0] = (uint8_t)ci;
    rq_idx = 0; rq_ans_len = 0; rq_ans[0] = 0;
    rq_score = 10; rq_streak = 3; rq_correct = 3;
    rq_phase = RQ_PLAY; rq_fb_until = 0;
    /* 空答案按 N -> 跳过亮答案, 清连对, 不得分 */
    press_ev(evc('n'));
    CHECK(rq_phase == RQ_FB, "N with empty buffer skips");
    CHECK(rq_fb_skip && !rq_fb_ok, "skip feedback flag");
    CHECK(rq_streak == 0 && rq_score == 10 && rq_correct == 3,
          "skip: no score, streak reset");
    {
        char fb[40];
        rq_fb_line(fb, sizeof(fb));
        CHECK(strcmp(fb, "SKIPPED. ANSWER: clock") == 0, "skip feedback shows answer");
    }
    rq_advance();
    CHECK(rq_phase == RQ_PLAY, "OK advances after skip");
    /* n 开头的答案: 空缓冲 N 正常输入(防打不出首字母) */
    ci = find_q("needle");
    CHECK(ci >= 0, "needle in bank");
    rq_order[1] = (uint8_t)ci;
    rq_idx = 1; rq_ans_len = 0; rq_ans[0] = 0;
    rq_phase = RQ_PLAY;
    press_ev(evc('n'));
    CHECK(rq_phase == RQ_PLAY && rq_ans_len == 1 && rq_ans[0] == 'n',
          "N types when answer starts with n");
    press_ev(evc('e'));
    CHECK(strcmp(rq_ans, "ne") == 0, "n-answer typeable");
    /* 缓冲非空时 N 是普通字母 */
    rq_idx = 0; rq_ans_len = 0; rq_ans[0] = 0;
    rq_phase = RQ_PLAY;
    press_ev(evc('c'));
    press_ev(evc('n'));
    CHECK(rq_phase == RQ_PLAY && strcmp(rq_ans, "cn") == 0,
          "N with non-empty buffer types");
    /* 反馈期字母输入被忽略 */
    rq_fb_until = 0; rq_phase = RQ_FB;
    press_ev(evc('a'));
    CHECK(rq_phase == RQ_FB && rq_ans_len == 2, "letters ignored during FB");
}

static void test_finish(void) {
    /* 30 题答完 -> 总结面 */
    rq_idx = RQ_N - 1; rq_phase = RQ_PLAY;
    rq_ans_len = 0; rq_ans[0] = 0; rq_complete = false;
    rq_advance();
    CHECK(rq_phase == RQ_OVER && rq_complete, "30 done -> OVER, complete");
    CHECK(rq_idx == RQ_N, "idx past end");
    /* 总结面 OK/N 重开 */
    press_ev(evk(K_OK));
    CHECK(rq_idx == 0 && rq_phase == RQ_PLAY && rq_score == 0,
          "OK on OVER restarts");
    /* Q 主动结束 -> 总结面(未答完) */
    rq_idx = 12; rq_phase = RQ_PLAY; rq_score = 40; rq_correct = 4;
    press_ev(evk(K_QUIT));
    CHECK(rq_phase == RQ_OVER && !rq_complete, "Q mid-game -> OVER, not complete");
    CHECK(rq_score == 40 && rq_correct == 4, "partial stats kept");
    /* 总结面 BACK/Q 退出 */
    s_exit_request = false;
    press_ev(evk(K_QUIT));
    CHECK(s_exit_request, "Q on OVER exits to menu");
    s_exit_request = false;
    press_ev(evk(K_BACK));
    CHECK(s_exit_request, "BACK on OVER exits to menu");
    /* 对局中 BACK/P 暂停(RESTART 走 enter; 此处只验不崩溃 + 不退出) */
    s_exit_request = false;
    riddle_enter();
    CHECK(rq_phase == RQ_PLAY && !s_exit_request, "enter resets to PLAY");
}

static void test_render(void) {
    /* 各阶段渲染不崩溃且画出了内容 */
    unsigned i, nblack;
    riddle_enter();
    riddle_render();
    for (i = 0, nblack = 0; i < CCG_FRAME_BYTES; i++)
        if (g_fb[i]) nblack++;
    CHECK(nblack > 0, "PLAY render draws pixels");
    rq_phase = RQ_FB; rq_fb_ok = true; rq_fb_skip = false;
    riddle_render();
    for (i = 0, nblack = 0; i < CCG_FRAME_BYTES; i++)
        if (g_fb[i]) nblack++;
    CHECK(nblack > 0, "FB render draws pixels");
    rq_phase = RQ_OVER; rq_complete = true; rq_score = 300;
    riddle_render();
    for (i = 0, nblack = 0; i < CCG_FRAME_BYTES; i++)
        if (g_fb[i]) nblack++;
    CHECK(nblack > 0, "OVER render draws pixels");
    /* 分数格式化 */
    {
        char b[8];
        rq_putint(b, 0);
        CHECK(strcmp(b, "0") == 0, "putint 0");
        rq_putint(b, 300);
        CHECK(strcmp(b, "300") == 0, "putint 300");
        rq_putint(b, 5);
        CHECK(strcmp(b, "5") == 0, "putint 5");
    }
}

static void test_wrap(void) {
    int i;
    char lines[2][RQ_LINE + 1];
    /* 最长谜面两行折行, 行宽不超限, 拼接还原原文 */
    for (i = 0; i < RQ_N; i++) {
        char bad[80];
        rq_order[0] = (uint8_t)i;
        rq_idx = 0;
        rq_wrap(lines);
        snprintf(bad, sizeof(bad), "riddle %d line0 <=%d", i, RQ_LINE);
        CHECK((int)strlen(lines[0]) <= RQ_LINE, bad);
        snprintf(bad, sizeof(bad), "riddle %d line1 <=%d", i, RQ_LINE);
        CHECK((int)strlen(lines[1]) <= RQ_LINE, bad);
        if (lines[1][0]) {
            char joined[130];
            int k = 0, j = 0;
            while (lines[0][j]) joined[k++] = lines[0][j++];
            joined[k++] = ' ';
            j = 0;
            while (lines[1][j]) joined[k++] = lines[1][j++];
            joined[k] = 0;
            snprintf(bad, sizeof(bad), "riddle %d wrap joins to original", i);
            CHECK(strcmp(joined, rq_cur_q()) == 0, bad);
        }
    }
    /* 提示行: 可跳过显示 N:SKIP; 不可跳过省略 */
    rq_order[0] = (uint8_t)find_q("clock");
    rq_idx = 0; rq_phase = RQ_PLAY;
    {
        char hb[32];
        rq_hint_line(hb, sizeof(hb));
        CHECK(strstr(hb, "N:SKIP") != NULL, "hint shows N:SKIP when skipable");
    }
    rq_order[0] = (uint8_t)find_q("needle");
    {
        char hb[32];
        rq_hint_line(hb, sizeof(hb));
        CHECK(strstr(hb, "N:SKIP") == NULL, "hint hides N:SKIP for n-answer");
    }
}

int main(void) {
    test_bank();
    test_shuffle();
    test_typing();
    test_submit();
    test_skip();
    test_finish();
    test_wrap();
    test_render();
    if (s_fail == 0) printf("ALL TESTS PASSED\n");
    else printf("%d FAILURES\n", s_fail);
    return s_fail ? 1 : 0;
}
