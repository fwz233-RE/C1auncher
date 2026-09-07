/* GEO QUIZ host 逻辑单测 — 直接包含 geo.c 访问静态状态 */
#include "../../src/games/geo.c"
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
    geo_on_key(&ev);
}
static void press_key(ccg_key k) {
    key_event_t ev = { k, 0, false };
    geo_on_key(&ev);
}
/* 键入答案(小写拼写, 验证大小写不敏感) */
static void type_answer(void) {
    const char *a = gq_answer();
    for (; *a; a++) press_char((char)(*a - 'A' + 'a'));
}

/* ---- 题库完整性: 30 组, 大写 ASCII, 无空格, ≤12, 首/首都各自唯一 ---- */
static void test_data(void) {
    int ok = 1;
    for (int i = 0; i < GQ_N; i++) {
        const char *c = gq_data[i].c;
        const char *a = gq_data[i].a;
        int cl = (int)strlen(c), al = (int)strlen(a);
        if (cl < 1 || al < 1 || cl > GQ_ANS_CAP || al > GQ_ANS_CAP) ok = 0;
        for (int k = 0; k < cl; k++) if (c[k] < 'A' || c[k] > 'Z') ok = 0;
        for (int k = 0; k < al; k++) if (a[k] < 'A' || a[k] > 'Z') ok = 0;
        for (int j = 0; j < i; j++) {
            if (strcmp(c, gq_data[j].c) == 0 || strcmp(a, gq_data[j].a) == 0) ok = 0;
        }
        if (strcmp(c, a) == 0) ok = 0;   /* 题目 != 答案(确定性) */
    }
    CHECK(ok, "30 pairs, uppercase A-Z, no space, len<=12, unique+deterministic");
}

/* ---- 新局初始化 ---- */
static void test_new_game(void) {
    rng_seed(&gq_rng, 42);
    gq_new_game();
    CHECK(gq_score == 0 && gq_streak == 0 && gq_right == 0, "score/streak/right reset");
    CHECK(gq_elapsed == 0u && !gq_over && gq_over_full == false, "timer/over reset");
    CHECK(!gq_rev && gq_ansn == 0, "forward mode, entry cleared");
    CHECK(gq_cur >= 0 && gq_cur < GQ_N, "valid question index");
    CHECK(gq_answer()[0] != 0, "deterministic answer non-empty");
    CHECK(gq_fb[0] == 0, "feedback cleared");
}

/* ---- 换题不连续重复(200 次全换) ---- */
static void test_no_repeat(void) {
    rng_seed(&gq_rng, 7);
    gq_cur = -1;
    int ok = 1;
    int prev = -2;
    for (int i = 0; i < 200; i++) {
        gq_next_q();
        if (gq_cur == prev) { ok = 0; break; }
        prev = gq_cur;
    }
    CHECK(ok, "no consecutive identical question over 200 gens");
}

/* ---- 输入: 大写转换/12 位上限/DEL 退格/重复忽略 ---- */
static void test_entry(void) {
    rng_seed(&gq_rng, 11);
    gq_new_game();
    gq_ansn = 0;
    press_char('p'); press_char('a'); press_char('r');
    CHECK(gq_ansn == 3 && gq_ans[0] == 'P' && gq_ans[1] == 'A' && gq_ans[2] == 'R',
          "letters stored uppercase");
    for (int i = 0; i < 12; i++) press_char('a');
    CHECK(gq_ansn == GQ_ANS_CAP, "entry capped at 12");
    press_key(K_DEL);
    CHECK(gq_ansn == GQ_ANS_CAP - 1, "DEL backspace");
    press_key(K_DEL);
    gq_ansn = 0;
    /* 重复事件忽略 */
    key_event_t ev = { K_CHAR, 'x', true };
    geo_on_key(&ev);
    CHECK(gq_ansn == 0, "repeat char ignored");
    ev.key = K_OK; ev.is_repeat = true;
    geo_on_key(&ev);
    CHECK(gq_score == 0, "repeat OK ignored");
    ev.key = K_DEL; ev.is_repeat = true;
    geo_on_key(&ev);
    CHECK(gq_ansn == 0, "repeat DEL ignored");
}

/* ---- 提交: 对 +5 / 错亮答案 / 空忽略 / 换题 ---- */
static void test_submit(void) {
    rng_seed(&gq_rng, 99);
    gq_new_game();
    /* 空提交: 忽略 */
    press_key(K_OK);
    CHECK(gq_score == 0 && gq_ansn == 0, "empty submit ignored");
    /* 错答案: 先取当前题答案(提交后换题, 反馈快照应在提交前取) */
    int q0 = gq_cur;
    char wrong_exp[30];
    snprintf(wrong_exp, sizeof wrong_exp, "WRONG: %s", gq_answer());
    press_char('a'); press_char('a'); press_char('a');
    press_key(K_OK);
    CHECK(gq_score == 0 && gq_streak == 0, "wrong answer no score, streak 0");
    CHECK(strcmp(gq_fb, wrong_exp) == 0, "wrong shows correct answer");
    CHECK(gq_ansn == 0 && gq_cur != q0, "entry cleared and question changed");
    /* 对答案(小写拼写) */
    int sol_cur = gq_cur;
    type_answer();
    CHECK(gq_ansn == (int)strlen(gq_answer()), "typed full answer");
    press_key(K_OK);
    CHECK(gq_score == GQ_SCORE, "correct +5");
    CHECK(gq_right == 1 && gq_streak == 1, "right/streak 1");
    CHECK(strcmp(gq_fb, "RIGHT +5") == 0, "RIGHT +5 feedback");
    CHECK(gq_ansn == 0 && gq_cur != sol_cur, "entry cleared, question changed");
}

/* ---- 连击: 连对 3 题提示, 错即清零 ---- */
static void test_combo(void) {
    rng_seed(&gq_rng, 5);
    gq_new_game();
    type_answer(); press_key(K_OK);
    type_answer(); press_key(K_OK);
    CHECK(gq_score == 10 && gq_streak == 2, "streak 2 after two correct");
    type_answer(); press_key(K_OK);
    CHECK(gq_score == 15 && gq_streak == 3, "streak 3 after three correct");
    CHECK(strcmp(gq_fb, "RIGHT +5 STREAK 3!") == 0, "combo hint at 3");
    type_answer(); press_key(K_OK);
    CHECK(gq_score == 20 && gq_streak == 4, "streak keeps growing");
    CHECK(strcmp(gq_fb, "RIGHT +5 STREAK 4!") == 0, "combo hint grows");
    /* 错 → 连击清零 */
    press_char('z'); press_char('z'); press_key(K_OK);
    CHECK(gq_streak == 0 && gq_score == 20, "wrong resets streak, score kept");
}

/* ---- N 命令/字母共用: 空输入且答案非 N 开头 → 切换; 否则输入 N ---- */
static void test_flip(void) {
    rng_seed(&gq_rng, 21);
    gq_new_game();
    /* 输入非空时 N 是字母 */
    gq_cur = 0;                       /* FRANCE -> PARIS */
    gq_rev = false;
    gq_ansn = 0;
    press_char('a'); press_char('n');
    CHECK(!gq_rev && gq_ansn == 2 && gq_ans[1] == 'N', "N types letter while typing");
    /* 空输入 + 答案不以 N 开头(PARIS) → 切换方向 */
    gq_ansn = 0;
    int q0 = gq_cur;
    press_char('n');
    CHECK(gq_rev, "N flips to reverse");
    CHECK(gq_ansn == 0 && gq_cur != q0, "flip clears entry and changes question");
    CHECK(gq_streak == 0, "flip resets streak");
    CHECK(strcmp(gq_fb, "MODE COUNTRY") == 0, "flip feedback COUNTRY");
    /* 反向: 答案是国名 */
    {
        const char *ans = gq_answer();
        int is_country = 0;
        for (int i = 0; i < GQ_N; i++)
            if (strcmp(ans, gq_data[i].c) == 0) is_country = 1;
        CHECK(is_country, "reverse answer is a country name");
    }
    /* 反向提交: 固定题目(ROMANIA -> BUCHAREST), 拼国名 */
    gq_cur = 20;
    gq_ansn = 0;
    type_answer(); press_key(K_OK);
    CHECK(gq_score == GQ_SCORE && gq_streak == 1, "reverse submit scores");
    /* 提交后已随机换题; 钉住一题(非 N 开头)再按 N, 确定化翻转判据 */
    gq_cur = 20;                        /* ROMANIA -> BUCHAREST, 非 N 开头 */
    gq_ansn = 0;
    press_char('n');
    CHECK(!gq_rev && strcmp(gq_fb, "MODE CAPITAL") == 0, "N flips back to capital");
}

/* ---- N 开头答案(NAIROBI 等): N 首字母照常输入, 不误切 ---- */
static void test_n_initial(void) {
    rng_seed(&gq_rng, 41);
    gq_new_game();
    gq_rev = false;
    for (int i = 0; i < GQ_N; i++) {
        if (gq_data[i].a[0] == 'N') {
            gq_cur = i;                        /* 直接指到 NAIROBI */
            gq_ansn = 0;
            press_char('n');                   /* 空输入 + N 开头 → 字母 */
            CHECK(gq_ansn == 1 && gq_ans[0] == 'N', "N initial types letter, no flip");
            CHECK(!gq_rev, "no flip on N-initial answer");
            return;
        }
    }
    CHECK(0, "test pool has an N-initial answer");
}

/* ---- 限时: 300 tick 到点, 封顶, 结束后 tick 无累积 ---- */
static void test_timer(void) {
    rng_seed(&gq_rng, 3);
    gq_new_game();
    for (int i = 0; i < 299; i++) geo_tick(0);
    CHECK(!gq_over && gq_elapsed == 29900u, "not over at 29.9s");
    geo_tick(0);
    CHECK(gq_over && gq_elapsed == GQ_GAME_MS, "over at 30.0s");
    CHECK(gq_ansn == 0, "entry cleared on time up");
    for (int i = 0; i < 50; i++) geo_tick(0);
    CHECK(gq_elapsed == GQ_GAME_MS, "no accumulation after over");
}

/* ---- 结束态按键: OK/N 重开, BACK/Q 退出 ---- */
static void test_over_keys(void) {
    rng_seed(&gq_rng, 33);
    gq_new_game();
    gq_over = true;
    press_key(K_OK);
    CHECK(!gq_over && gq_score == 0 && gq_right == 0, "OK restarts when over");
    gq_over = true;
    press_char('n');
    CHECK(!gq_over, "N restarts when over");
    s_exit_request = false;
    gq_over = true;
    press_key(K_BACK);
    CHECK(s_exit_request, "BACK quits when over");
    s_exit_request = false;
    gq_over = true;
    press_key(K_QUIT);
    CHECK(s_exit_request, "Q quits when over");
}

/* ---- 对局中退出 ---- */
static void test_quit(void) {
    rng_seed(&gq_rng, 17);
    gq_new_game();
    s_exit_request = false;
    press_key(K_QUIT);
    CHECK(s_exit_request, "Q exits mid-game");
}

/* ---- 渲染冒烟: 对局/结束两态绘制不崩溃, 有黑像素 ---- */
static int count_black(void) {
    int n = 0;
    for (unsigned i = 0; i < CCG_FRAME_BYTES; i++)
        for (int b = 0; b < 8; b++)
            if (g_fb[i] & (1u << b)) n++;
    return n;
}

static void test_render(void) {
    rng_seed(&gq_rng, 1);
    gq_new_game();
    gq_ansn = 0;
    press_char('p');
    geo_render();
    CHECK(count_black() > 500, "in-game frame has content");
    gq_over = true;
    gq_over_full = false;
    geo_render();                               /* 走 disp_force_full 分支 */
    CHECK(count_black() > 500, "over frame has content");
}

int main(void) {
    test_data();
    test_new_game();
    test_no_repeat();
    test_entry();
    test_submit();
    test_combo();
    test_flip();
    test_n_initial();
    test_timer();
    test_over_keys();
    test_quit();
    test_render();

    printf("%s\n", s_fail ? "TESTS FAILED" : "ALL TESTS PASSED");
    return s_fail ? 1 : 0;
}
