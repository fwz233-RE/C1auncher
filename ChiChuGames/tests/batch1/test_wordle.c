/* WORDLE 逻辑单测 — host 编译, include 游戏源文件直接操作静态状态 */
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include "../../src/config.h"
#include "../../src/games/wordle.c"

bool s_exit_request = false;

static int s_fail = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); s_fail++; } \
    else { printf("ok: %s\n", msg); } \
} while (0)

static void press_key(ccg_key k) {
    key_event_t ev = { k, 0, false };
    wordle_on_key(&ev);
}

static void press_char(char c) {
    key_event_t ev = { K_CHAR, (uint8_t)c, false };
    wordle_on_key(&ev);
}

static void type_word(const char *s) {
    int i;
    for (i = 0; i < WD_LEN; i++) press_char(s[i]);
}

/* ---- 词表完整性 + 谜底抽取 ---- */
static void test_dictionary(void) {
    unsigned n = (unsigned)WD_WORDS_N;
    CHECK(n == 200, "dictionary has 200 words");
    CHECK(wd_in_words("apple", strlen("apple")), "apple in dictionary");
    CHECK(wd_in_words("about", strlen("about")), "about in dictionary");
    CHECK(!wd_in_words("zzzzz", strlen("zzzzz")), "zzzzz rejected");
    CHECK(!wd_in_words("app", strlen("app")), "short word rejected");
    bool all_ok = true;
    unsigned i;
    for (i = 0; i < n; i++) {
        int j;
        for (j = 0; j < WD_LEN; j++) {
            char c = wd_words[i][j];
            if (c < 'a' || c > 'z') all_ok = false;
        }
        if (wd_words[i][WD_LEN] != 0) all_ok = false;
        for (j = 0; j < (int)i; j++)
            if (strcmp(wd_words[i], wd_words[j]) == 0) all_ok = false;
    }
    CHECK(all_ok, "all words lowercase 5-letter, NUL-terminated, unique");
    rng_seed(&wd_rng, 42);
    bool ans_ok = true;
    for (i = 0; i < 100; i++) {
        wd_pick_answer();
        if (!wd_in_words(wd_ans, strlen(wd_ans))) ans_ok = false;
    }
    CHECK(ans_ok, "100 picked answers always in dictionary");
    CHECK(wd_ans[WD_LEN] == 0, "answer NUL-terminated");
}

/* ---- 着色算法(含重复字母限量) ---- */
static void score_guess(const char *ans, const char *guess) {
    memcpy(wd_ans, ans, WD_LEN + 1);
    wd_row = 0;
    memcpy(wd_cur, guess, WD_LEN);
    memset(wd_col, 0, sizeof(wd_col));   /* 每局行颜色须清零(模拟新局) */
    wd_score_row();
}

static int rc(int c) { return wd_col[0][c]; }

static void test_scoring(void) {
    score_guess("apple", "apply");
    CHECK(rc(0) == 2 && rc(1) == 2 && rc(2) == 2 && rc(3) == 2 && rc(4) == 0,
          "apply vs apple -> GGGGX");
    score_guess("level", "levee");
    CHECK(rc(0) == 2 && rc(1) == 2 && rc(2) == 2 && rc(3) == 2 && rc(4) == 0,
          "levee vs level -> GGGGX");
    score_guess("peter", "erupt");
    CHECK(rc(0) == 1 && rc(1) == 1 && rc(2) == 0 && rc(3) == 1 && rc(4) == 1,
          "erupt vs peter -> YYYXY");
    score_guess("mummy", "mamma");
    CHECK(rc(0) == 2 && rc(1) == 0 && rc(2) == 2 && rc(3) == 2 && rc(4) == 0,
          "mamma vs mummy triple dup -> GXGGX");
    score_guess("crane", "crane");
    CHECK(rc(0) == 2 && rc(1) == 2 && rc(2) == 2 && rc(3) == 2 && rc(4) == 2,
          "exact match all green");
    score_guess("bloom", "kebab");
    CHECK(rc(0) == 0 && rc(1) == 0 && rc(2) == 1 && rc(3) == 0 && rc(4) == 0,
          "kebab vs bloom -> XXYXX (duplicate b limited)");
}

/* ---- 输入/提交/获胜流程 ---- */
static void test_flow_win(void) {
    wordle_enter();
    memcpy(wd_ans, "apple", WD_LEN + 1);
    CHECK(!wd_over && wd_row == 0 && wd_cur_len == 0, "fresh game state");
    type_word("about");
    CHECK(wd_cur_len == WD_LEN, "five letters typed");
    press_char('x');                 /* 第 6 个字母 */
    CHECK(wd_cur_len == WD_LEN && wd_cur[4] == 't', "extra letter ignored");
    press_key(K_OK);
    CHECK(wd_row == 1 && !wd_over, "submit advances row");
    CHECK(wd_g[0][0] == 'a' && wd_g[0][4] == 't', "guess stored in grid");
    CHECK(wd_cur_len == 0, "current input reset after submit");
    wordle_render();                 /* 中途渲染不崩 */
    type_word("apple");
    press_key(K_OK);
    CHECK(wd_over && wd_won, "correct word wins");
    CHECK(wd_row == 2, "win counts as attempt");
    CHECK(wd_col[1][0] == 2 && wd_col[1][4] == 2, "win row colored green");
    CHECK(wd_g[1][0] == 'a' && wd_g[1][4] == 'e', "win word stored");
    wordle_render();
    CHECK(wd_over_full, "over triggers force full refresh flag");
    /* 结束后 N 重开 */
    press_char('n');
    CHECK(!wd_over && wd_row == 0 && !wd_won, "N restarts after win");
}

/* ---- INVALID 判定 + 退格 ---- */
static void test_invalid(void) {
    wordle_enter();
    memcpy(wd_ans, "apple", WD_LEN + 1);
    type_word("abou");
    press_key(K_OK);
    CHECK(wd_row == 0 && wd_msg_ticks == WD_MSG_TICKS, "short submit invalid");
    wordle_tick(0);
    wordle_tick(0);
    CHECK(wd_msg_ticks == 0, "invalid message expires via ticks");
    type_word("zzzzz");
    press_key(K_OK);
    CHECK(wd_row == 0 && wd_msg_ticks == WD_MSG_TICKS, "non-word submit invalid");
    CHECK(wd_cur_len == WD_LEN, "failed submit keeps letters");
    type_word("apple");
    CHECK(wd_cur[4] == 'z', "row full: new letters ignored");
    press_key(K_DEL);
    CHECK(wd_cur_len == 4, "DEL removes last letter");
    press_key(K_DEL); press_key(K_DEL); press_key(K_DEL); press_key(K_DEL);
    CHECK(wd_cur_len == 0, "DEL empties row");
    press_key(K_DEL);
    CHECK(wd_cur_len == 0, "DEL on empty row harmless");
    type_word("apple");
    press_key(K_OK);
    CHECK(wd_over && wd_won, "recovery: correct word still wins");
}

/* ---- 六次失败展示答案 ---- */
static void test_lose(void) {
    wordle_enter();
    memcpy(wd_ans, "apple", WD_LEN + 1);
    static const char *guesses[WD_ROWS] = { "about", "chair", "beach",
                                            "cloud", "dance", "eager" };
    int i;
    for (i = 0; i < WD_ROWS; i++) {
        type_word(guesses[i]);
        press_key(K_OK);
        CHECK(wd_row == i + 1, "guess count advances");
        CHECK(wd_g[i][0] == guesses[i][0], "guess i stored");
    }
    CHECK(wd_over && !wd_won, "six wrong guesses lose");
    CHECK(wd_row == WD_ROWS, "all attempts used");
    CHECK(strcmp(wd_ans, "apple") == 0, "answer kept for reveal");
    CHECK(wd_col[5][0] == 1 && wd_col[5][1] == 1 && wd_col[5][2] == 0 &&
          wd_col[5][3] == 0 && wd_col[5][4] == 0, "last row eager vs apple colors");
    wordle_render();                 /* 失败态渲染不崩 */
    /* 结束后 OK 重开 */
    press_key(K_OK);
    CHECK(!wd_over && wd_row == 0, "OK restarts after loss");
}

/* ---- 大小写统一 / 重复忽略 ---- */
static void test_case_and_repeat(void) {
    wordle_enter();
    memcpy(wd_ans, "apple", WD_LEN + 1);
    key_event_t up = { K_CHAR, 'A', false };
    wordle_on_key(&up);
    CHECK(wd_cur_len == 1 && wd_cur[0] == 'a', "uppercase A normalized to a");
    key_event_t dig = { K_CHAR, '3', false };
    wordle_on_key(&dig);
    CHECK(wd_cur_len == 1, "digit ignored");
    key_event_t rep = { K_CHAR, 'p', true };
    wordle_on_key(&rep);
    CHECK(wd_cur_len == 1, "repeat letter ignored");
    key_event_t delrep = { K_DEL, 0, true };
    wordle_on_key(&delrep);
    CHECK(wd_cur_len == 0, "repeat DEL still backspaces (long-press)");
    type_word("apple");
    press_key(K_OK);
    CHECK(wd_over && wd_won, "game still winnable after case/repeat path");
}

/* ---- 结束态退出键 ---- */
static void test_quit_over(void) {
    wordle_enter();
    wd_over = true;
    wd_won = false;
    s_exit_request = false;
    key_event_t q = { K_QUIT, 0, false };
    wordle_on_key(&q);
    CHECK(s_exit_request, "QUIT exits when over");
    s_exit_request = false;
    key_event_t b = { K_BACK, 0, false };
    wordle_on_key(&b);
    CHECK(s_exit_request, "BACK exits when over");
}

int main(void) {
    test_dictionary();
    test_scoring();
    test_flow_win();
    test_invalid();
    test_lose();
    test_case_and_repeat();
    test_quit_over();
    if (s_fail) { printf("\n%d FAILURE(S)\n", s_fail); return 1; }
    printf("\nALL WORDLE TESTS PASSED\n");
    return 0;
}
