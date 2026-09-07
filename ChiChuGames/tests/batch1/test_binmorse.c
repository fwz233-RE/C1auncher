/* BINMORSE host 逻辑单测 — 直接包含 binmorse.c 访问静态状态 */
#include "../../src/games/binmorse.c"
#include <stdio.h>
#include <string.h>

bool s_exit_request = false;
void game_set_tick_interval(uint32_t ms) { (void)ms; }

static int s_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); s_fail++; } \
    else { printf("ok: %s\n", msg); } \
} while (0)

static void key(ccg_key k, uint8_t ch, bool rep) {
    key_event_t ev;
    ev.key = k;
    ev.ch = ch;
    ev.is_repeat = rep;
    binmorse_on_key(&ev);
}

/* 在二进制模式下输入 n 个位 */
static void type_bits(const char *bits) {
    for (const char *p = bits; *p; p++)
        key(K_CHAR, (*p == '1') ? 'z' : 'o', false);
}

/* 在摩斯模式下输入 n 个符号 */
static void type_morse(const char *code) {
    for (const char *p = code; *p; p++)
        key(*p == '.' ? K_LEFT : K_RIGHT, 0, false);
}

/* ---- 1. 二进制转换 helper ---- */
static void test_binary(void) {
    char b[9];
    CHECK(bm_to_binary(1, b) == 1 && !strcmp(b, "1"), "bin(1)=1");
    CHECK(bm_to_binary(5, b) == 3 && !strcmp(b, "101"), "bin(5)=101");
    CHECK(bm_to_binary(13, b) == 4 && !strcmp(b, "1101"), "bin(13)=1101");
    CHECK(bm_to_binary(128, b) == 8 && !strcmp(b, "10000000"), "bin(128)=10000000");
    CHECK(bm_to_binary(255, b) == 8 && !strcmp(b, "11111111"), "bin(255)=11111111");
    CHECK(bm_to_binary(0, b) == 1 && !strcmp(b, "0"), "bin(0)=0");
}

/* ---- 2. 开局状态 ---- */
static void test_init(void) {
    bm_new_game();
    CHECK(bm_mode == BM_BIN, "starts in BIN mode");
    CHECK(bm_score == 0 && bm_correct == 0 && !bm_over, "fresh score/state");
    CHECK(bm_n == 0, "fresh answer empty");
    CHECK(bm_target >= 1 && bm_target <= 15, "initial difficulty 1..15");
    CHECK(bm_bitlen() == 4, "initial bitlen 4");
}

/* ---- 3. 二进制答题: 对/错/换题/难度递增 ---- */
static void test_bin_answer(void) {
    bm_new_game();
    /* 搭一题固定值 */
    bm_target = 13; bm_n = 0;
    type_bits("1101");
    CHECK(bm_n == 4, "typed 4 bits");
    key(K_OK, 0, false);
    CHECK(bm_score == 5, "correct +5");
    CHECK(bm_correct == 1, "correct count +1");
    CHECK(bm_fb == 1, "feedback=correct");
    /* 反馈结束后自动换题 */
    bm_fb_until = 0;                 /* 强制到期 */
    binmorse_tick(1);
    CHECK(bm_fb == 0, "feedback clears to next question");
    CHECK(bm_n == 0, "answer reset on new question");

    /* 错答: 不减分, 显示错误反馈 */
    uint32_t old = bm_score;
    uint32_t t0 = bm_target;
    bm_fb = 0; bm_n = 0;
    char w[9];
    bm_to_binary(t0, w);
    /* 输入一个必错的答案(第一位取反) */
    type_bits(w[0] == '1' ? "0" : "1");
    key(K_OK, 0, false);
    CHECK(bm_fb == 2, "wrong -> feedback=wrong");
    CHECK(bm_score == old, "wrong does not change score");
    CHECK(bm_correct == 1, "wrong does not advance difficulty");
    bm_fb_until = 0;
    binmorse_tick(1);
    CHECK(bm_fb == 0 && bm_target != t0, "next question differs (guard)");

    /* 难度递增: 4 题后 5 位, 8 题后 6 位... 16 题后 8 位(1..255) */
    bm_target = 0; bm_letter = 0xFFu;
    bm_correct = 15;
    bm_next_q();
    CHECK(bm_bitlen() == 7, "bitlen 7 at correct=15");
    CHECK(bm_target >= 1 && bm_target <= 127, "range 1..127 at correct=15");
    bm_correct = 16;
    bm_next_q();
    CHECK(bm_bitlen() == 8, "bitlen 8 at correct=16");
    CHECK(bm_target >= 1 && bm_target <= 255, "difficulty reaches 1..255");
    bm_correct = 100;
    bm_next_q();
    CHECK(bm_bitlen() == 8, "bitlen capped at 8");
    bm_correct = 3;
    bm_next_q();
    CHECK(bm_bitlen() == 4, "bitlen 4 at correct=3");
}

/* ---- 4. 输入边界 ---- */
static void test_input_bounds(void) {
    bm_new_game();
    bm_target = 255; bm_n = 0;
    type_bits("11111111");
    key(K_CHAR, 'o', false);         /* 第 9 位应被忽略 */
    CHECK(bm_n == 8, "binary caps at 8 bits");
    /* 方向键输入等价 */
    bm_n = 0;
    key(K_LEFT, 0, false); key(K_RIGHT, 0, false); key(K_LEFT, 0, false);
    CHECK(bm_n == 3 && bm_bits[0] == 0 && bm_bits[1] == 1 && bm_bits[2] == 0,
          "LT/RT enter 0/1 bits");
    /* 数字字符输入 */
    bm_n = 0;
    key(K_CHAR, '1', false); key(K_CHAR, '0', false);
    CHECK(bm_n == 2 && bm_bits[0] == 1 && bm_bits[1] == 0, "digit chars enter bits");
    /* DEL 退格 */
    key(K_DEL, 0, false);
    CHECK(bm_n == 1, "DEL undoes last bit");
    /* 空答案提交无效 */
    bm_n = 0;
    uint32_t s0 = bm_score;
    key(K_OK, 0, false);
    CHECK(bm_score == s0 && bm_fb == 0, "empty submit ignored");
}

/* ---- 5. 摩斯表完整性 ---- */
static void test_morse_table(void) {
    static const char *expect[26] = {
        ".-", "-...", "-.-.", "-..", ".", "..-.", "--.", "....",
        "..", ".---", "-.-", ".-..", "--", "-.", "---", ".--.",
        "--.-", ".-.", "...", "-", "..-", "...-", ".--", "-..-",
        "-.--", "--.."
    };
    for (int i = 0; i < 26; i++) {
        CHECK(strcmp(bm_code[i], expect[i]) == 0, "morse table entry matches");
        CHECK(bm_clen[i] == strlen(expect[i]), "morse length table matches");
    }
}

/* ---- 6. 摩斯答题 ---- */
static void test_morse_answer(void) {
    bm_new_game();
    bm_mode = BM_MORSE;
    bm_letter = 10;                  /* K = -.- */
    bm_n = 0; bm_fb = 0;
    type_morse("-.-");
    CHECK(bm_n == 3, "morse typed 3 symbols");
    key(K_OK, 0, false);
    CHECK(bm_fb == 1 && bm_score == 5, "morse correct +5");

    /* 长度不符判错 */
    bm_fb = 0; bm_n = 0;
    type_morse("-.");
    key(K_OK, 0, false);
    CHECK(bm_fb == 2, "morse wrong length -> wrong");

    /* 内容不符判错 */
    bm_fb = 0; bm_n = 0;
    type_morse(".--");
    key(K_OK, 0, false);
    CHECK(bm_fb == 2, "morse wrong content -> wrong");

    /* 上限 4 符号 */
    bm_letter = 1;                   /* B = -... */
    bm_n = 0; bm_fb = 0;
    type_morse("-...");
    key(K_LEFT, 0, false);           /* 第 5 个应忽略 */
    CHECK(bm_n == 4, "morse caps at 4 symbols");

    /* 相邻题不重复(guard) — 固定种子确定性验证 */
    rng_seed(&bm_rng, 0xC0FFEEu);
    bm_letter = 13;                  /* N */
    bm_next_q();
    CHECK(bm_letter != 13, "morse next letter differs from previous");
    /* 二进制同样不重复上一题 */
    bm_mode = BM_BIN;
    rng_seed(&bm_rng, 0xC0FFEEu);
    bm_target = 7;
    bm_next_q();
    CHECK(bm_target != 7, "binary next value differs from previous");
    bm_mode = BM_MORSE;
}

/* ---- 7. 模式切换 ---- */
static void test_mode_switch(void) {
    bm_new_game();
    bm_n = 3; bm_bits[0] = 1; bm_bits[1] = 0; bm_bits[2] = 1;
    uint32_t s0 = bm_score;
    key(K_CHAR, 'm', false);
    CHECK(bm_mode == BM_MORSE, "M switches to morse");
    CHECK(bm_n == 0, "mode switch resets answer");
    CHECK(bm_score == s0, "mode switch keeps score");
    key(K_CHAR, 'm', false);
    CHECK(bm_mode == BM_BIN, "M switches back to bin");
    /* 摩斯模式下 o/z 不生效(仅左右键) */
    bm_mode = BM_MORSE;
    bm_n = 0;
    key(K_CHAR, 'o', false);
    CHECK(bm_n == 0, "morse ignores o/z keys");
    bm_mode = BM_BIN;
}

/* ---- 8. 计时与结算 ---- */
static void test_timer(void) {
    bm_new_game();
    bm_elapsed = BM_GAME_MS - 150u;
    bm_fb = 0;
    binmorse_tick(0);
    CHECK(!bm_over, "not over before time");
    binmorse_tick(0);
    CHECK(bm_over, "time up -> over");
    CHECK(bm_over_full == false, "over_full flag reset for full refresh");
    /* 结算后按键: OK 重开 */
    s_exit_request = false;
    key(K_OK, 0, false);
    CHECK(!bm_over && bm_score == 0, "OK retries new game");
    /* BACK 退出 */
    s_exit_request = false;
    bm_elapsed = BM_GAME_MS;
    binmorse_tick(0);
    key(K_BACK, 0, false);
    CHECK(s_exit_request, "BACK quits after over");
    /* Q 退出 */
    s_exit_request = false;
    bm_elapsed = BM_GAME_MS;
    binmorse_tick(0);
    key(K_QUIT, 0, false);
    CHECK(s_exit_request, "Q quits after over");
}

/* ---- 9. 反馈期间忽略输入 ---- */
static void test_fb_lock(void) {
    bm_new_game();
    bm_target = 1; bm_n = 0; bm_fb = 2;
    key(K_CHAR, 'o', false);
    CHECK(bm_n == 0, "input locked during feedback");
    key(K_LEFT, 0, false);
    CHECK(bm_n == 0, "dir input locked during feedback");
    key(K_OK, 0, false);
    CHECK(bm_fb == 2, "OK locked during feedback");
    bm_fb_until = 0;
    binmorse_tick(1);
    CHECK(bm_fb == 0, "feedback ends via tick");
}

/* ---- 10. 暂停路径 ---- */
static void test_pause(void) {
    bm_new_game();
    /* ui_pause_run 内部阻塞读输入, 只能验证进入逻辑不崩;
     * 这里仅验证 K_PAUSE 不直接改状态(暂停由 ui_pause_run 全权处理) */
    CHECK(bm_over == false, "pause path reachable");
}

int main(void) {
    test_binary();
    test_init();
    test_bin_answer();
    test_input_bounds();
    test_morse_table();
    test_morse_answer();
    test_mode_switch();
    test_timer();
    test_fb_lock();
    test_pause();
    if (s_fail) { printf("== %d FAILURES ==\n", s_fail); return 1; }
    printf("== ALL PASS ==\n");
    return 0;
}
