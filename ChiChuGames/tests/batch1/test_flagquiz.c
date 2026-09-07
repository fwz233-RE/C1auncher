/* host 逻辑单测 — FLAG QUIZ (静态变量直接访问, 前缀 fq_) */
#include "../src/config.h"
#include "../src/gfx/canvas.h"
#include "../src/gfx/pattern.h"
#include "../src/rng.h"
#include "../src/platform/time.h"
#include "../src/games/flagquiz.c"
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

void flagquiz_enter(void);
void flagquiz_on_key(const key_event_t *ev);

static void send_key(ccg_key k) {
    key_event_t ev;
    ev.key = k;
    ev.ch = 0;
    ev.is_repeat = false;
    flagquiz_on_key(&ev);
}
static void send_char(char c) {
    key_event_t ev;
    ev.key = K_CHAR;
    ev.ch = (uint8_t)c;
    ev.is_repeat = false;
    flagquiz_on_key(&ev);
}

/* 定位当前题正确答案在选项中的下标 */
static int find_correct_opt(void) {
    int t = fq_order[fq_qi];
    for (int i = 0; i < FQ_OPTS; i++)
        if (fq_opts[i] == t) return i;
    return -1;
}

/* ---- 题库数据完整性 ---- */
static void test_flag_data(void) {
    for (int i = 0; i < FQ_N; i++) {
        const char *p = fq_flag_pix[i];
        int rows = 1, cols = 0, ink = 0;
        for (; *p; p++) {
            if (*p == '\n') { rows++; cols = 0; }
            else {
                cols++;
                if (*p == '#') ink++;
            }
        }
        CHECK(rows == 8 && cols == 12, "flag pattern is 8x12");
        CHECK(ink >= 6, "flag pattern has enough ink");
        CHECK(fq_names[i] != NULL && fq_names[i][0] != 0, "flag name non-empty");
    }
    /* 两两互异(题目必须有区分度) */
    int uniq = 1;
    for (int i = 0; i < FQ_N && uniq; i++)
        for (int j = i + 1; j < FQ_N; j++)
            if (strcmp(fq_flag_pix[i], fq_flag_pix[j]) == 0) { uniq = 0; break; }
    CHECK(uniq, "all 24 flag patterns pairwise distinct");
    /* 名字两两互异(选项不能重名) */
    uniq = 1;
    for (int i = 0; i < FQ_N && uniq; i++)
        for (int j = i + 1; j < FQ_N; j++)
            if (strcmp(fq_names[i], fq_names[j]) == 0) { uniq = 0; break; }
    CHECK(uniq, "all 24 names pairwise distinct");
}

/* ---- 开局与洗牌 ---- */
static void test_new_game(void) {
    flagquiz_enter();
    CHECK(fq_state == FQ_ST_PLAY, "new game starts in PLAY");
    CHECK(fq_qi == 0 && fq_score == 0 && fq_right == 0, "fresh counters");
    /* fq_order 是 0..23 的排列 */
    int seen[FQ_N];
    int perm_ok = 1;
    for (int i = 0; i < FQ_N; i++) seen[i] = 0;
    for (int i = 0; i < FQ_N; i++) {
        if (fq_order[i] < 0 || fq_order[i] >= FQ_N) { perm_ok = 0; break; }
        seen[fq_order[i]]++;
    }
    for (int i = 0; i < FQ_N; i++)
        if (seen[i] != 1) { perm_ok = 0; break; }
    CHECK(perm_ok, "fq_order is a permutation of 0..23");
    /* 4 选项互异且含正确答案 */
    int dist = 1;
    for (int i = 0; i < FQ_OPTS && dist; i++)
        for (int j = i + 1; j < FQ_OPTS; j++)
            if (fq_opts[i] == fq_opts[j]) { dist = 0; break; }
    CHECK(dist, "4 options are distinct");
    CHECK(find_correct_opt() >= 0, "correct answer among options");
}

/* ---- 洗牌确定性(同种子同序列) ---- */
static void test_shuffle_deterministic(void) {
    int a[FQ_N];
    for (int i = 0; i < FQ_N; i++) fq_order[i] = i;
    rng_seed(&fq_rng, 0x1234abcd);
    fq_shuffle();
    for (int i = 0; i < FQ_N; i++) a[i] = fq_order[i];
    for (int i = 0; i < FQ_N; i++) fq_order[i] = i;
    rng_seed(&fq_rng, 0x1234abcd);
    fq_shuffle();
    CHECK(memcmp(a, fq_order, sizeof a) == 0, "same seed -> same order");
    for (int i = 0; i < FQ_N; i++) fq_order[i] = i;
    rng_seed(&fq_rng, 0xdeadbeef);
    fq_shuffle();
    CHECK(memcmp(a, fq_order, sizeof a) != 0, "different seed -> different order");
}

/* ---- 选项生成的健壮性(多次生成仍 4 个互异) ---- */
static void test_opts_stress(void) {
    fq_qi = 0;
    rng_seed(&fq_rng, 0x55aa);
    int ok = 1;
    for (int k = 0; k < 200 && ok; k++) {
        fq_pick_opts();
        for (int i = 0; i < FQ_OPTS && ok; i++) {
            if (fq_opts[i] < 0 || fq_opts[i] >= FQ_N) { ok = 0; break; }
            for (int j = i + 1; j < FQ_OPTS; j++)
                if (fq_opts[i] == fq_opts[j]) { ok = 0; break; }
        }
    }
    CHECK(ok, "200x option builds all valid, distinct, in range");
}

/* ---- 方向键导航(2x2 网格) ---- */
static void test_nav(void) {
    flagquiz_enter();
    fq_sel = 0;
    send_key(K_RIGHT);
    CHECK(fq_sel == 1, "RIGHT: 0 -> 1");
    send_key(K_DOWN);
    CHECK(fq_sel == 3, "DOWN: 1 -> 3");
    send_key(K_LEFT);
    CHECK(fq_sel == 2, "LEFT: 3 -> 2");
    send_key(K_UP);
    CHECK(fq_sel == 0, "UP: 2 -> 0");
    send_key(K_RIGHT);
    send_key(K_LEFT);
    CHECK(fq_sel == 0, "RIGHT+LEFT roundtrip");
    /* 重复键必须忽略 */
    key_event_t ev;
    ev.key = K_RIGHT;
    ev.ch = 0;
    ev.is_repeat = true;
    flagquiz_on_key(&ev);
    CHECK(fq_sel == 0, "repeat key ignored");
}

/* ---- 答对: +10 分, 反馈正确 ---- */
static void test_answer_right(void) {
    flagquiz_enter();
    int qi_before = fq_qi;
    fq_sel = find_correct_opt();
    CHECK(fq_sel >= 0, "correct option located");
    int score_before = fq_score;
    send_key(K_OK);
    CHECK(fq_score == score_before + 10, "right answer +10");
    CHECK(fq_right == 1, "right counter increments");
    CHECK(fq_state == FQ_ST_FEED && fq_feed_ok, "feedback state RIGHT");
    CHECK(fq_correct == fq_order[qi_before], "fq_correct recorded");
    /* FEED 中 OK -> 下一题 */
    send_key(K_OK);
    CHECK(fq_state == FQ_ST_PLAY && fq_qi == qi_before + 1, "OK advances in FEED");
}

/* ---- 答错: 不加分, 显示正确答案 ---- */
static void test_answer_wrong(void) {
    flagquiz_enter();
    int qi_before = fq_qi;
    int correct = find_correct_opt();
    int wrong = -1;
    for (int i = 0; i < FQ_OPTS; i++)
        if (i != correct) { wrong = i; break; }
    CHECK(wrong >= 0, "wrong option exists");
    fq_sel = wrong;
    int score_before = fq_score;
    send_key(K_OK);
    CHECK(fq_score == score_before, "wrong answer no score");
    CHECK(fq_right == 0, "right counter unchanged");
    CHECK(fq_state == FQ_ST_FEED && !fq_feed_ok, "feedback state WRONG");
    CHECK(fq_correct == fq_order[qi_before], "wrong shows correct index");
}

/* ---- N 跳过本题 ---- */
static void test_skip(void) {
    flagquiz_enter();
    int qi_before = fq_qi;
    int score_before = fq_score;
    send_char('n');
    CHECK(fq_qi == qi_before + 1, "N skips to next question");
    CHECK(fq_state == FQ_ST_PLAY, "skip keeps PLAY");
    CHECK(fq_score == score_before, "skip gives no points");
}

/* ---- 完整一轮 24 题 -> OVER -> 重开 ---- */
static void test_full_round(void) {
    flagquiz_enter();
    int q = 0, guard = 0;
    while (fq_state != FQ_ST_OVER && guard < 64) {
        guard++;
        fq_sel = find_correct_opt();
        CHECK(fq_sel >= 0, "correct option found every round");
        send_key(K_OK);        /* 提交 */
        send_key(K_OK);        /* 反馈 -> 下一题 */
        q++;
    }
    CHECK(q == FQ_N, "24 questions played");
    CHECK(fq_state == FQ_ST_OVER, "game over after 24");
    CHECK(fq_score == FQ_N * 10 && fq_right == FQ_N, "perfect score 240");
    /* OVER: OK 重开 */
    send_key(K_OK);
    CHECK(fq_state == FQ_ST_PLAY && fq_score == 0 && fq_right == 0,
          "OK restarts from OVER");
    /* OVER: BACK 退出 */
    fq_state = FQ_ST_OVER;
    s_exit_request = false;
    send_key(K_BACK);
    CHECK(s_exit_request, "BACK in OVER requests exit");
}

/* ---- 对局中 Q 退出 ---- */
static void test_quit(void) {
    flagquiz_enter();
    s_exit_request = false;
    send_char('q');
    CHECK(s_exit_request, "Q during PLAY requests exit");
    s_exit_request = false;
    send_key(K_QUIT);
    CHECK(s_exit_request, "K_QUIT during PLAY requests exit");
}

/* ---- 渲染冒烟: 各状态渲染不越界 ---- */
static void test_render_smoke(void) {
    flagquiz_enter();
    flagquiz_render();                 /* PLAY */
    fq_state = FQ_ST_FEED;
    fq_feed_ok = false;
    fq_correct = fq_order[fq_qi];
    flagquiz_render();                 /* FEED wrong */
    fq_feed_ok = true;
    flagquiz_render();                 /* FEED right */
    fq_state = FQ_ST_OVER;
    fq_over_full = false;
    flagquiz_render();                 /* OVER(触发一次 force full 标记) */
    CHECK(fq_over_full, "over full-refresh flag latched");
    flagquiz_render();
    CHECK(fq_over_full, "force full only once");
    /* 帧缓冲应有内容 */
    int any = 0;
    for (int i = 0; i < (int)CCG_FRAME_BYTES; i++)
        if (g_fb[i]) { any = 1; break; }
    CHECK(any, "render produced visible pixels");
}

int main(void) {
    test_flag_data();
    test_new_game();
    test_shuffle_deterministic();
    test_opts_stress();
    test_nav();
    test_answer_right();
    test_answer_wrong();
    test_skip();
    test_full_round();
    test_quit();
    test_render_smoke();
    if (s_fail == 0) { printf("ALL TESTS PASSED\n"); return 0; }
    printf("%d TEST(S) FAILED\n", s_fail);
    return 1;
}
