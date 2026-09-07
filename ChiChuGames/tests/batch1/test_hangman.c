/* HANGMAN 逻辑测试 — host 编译运行; 含 hangman.c 直访静态 */
#include <stdio.h>
#include <string.h>

/* CHICHU_HOST 由命令行 -D 提供 */
#include "../../src/games/hangman.c"

static int s_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); s_fail++; } \
    else { printf("ok: %s\n", msg); } \
} while (0)

/* host stub */
bool s_exit_request = false;
void game_set_tick_interval(uint32_t ms) { (void)ms; }

/* 直接置局 */
static void test_set_word(const char *w) {
    hm_over = false; hm_won = false; hm_over_full = false; hm_err = 0;
    memset(hm_used, 0, sizeof(hm_used));
    memset(hm_revealed, 0, sizeof(hm_revealed));
    hm_len = (int)strlen(w);
    memcpy(hm_ans, w, (size_t)hm_len + 1);
}

static void send_char(char ch, bool rep) {
    key_event_t ev;
    ev.key = K_CHAR;
    ev.ch = (uint8_t)ch;
    ev.is_repeat = rep;
    hangman_on_key(&ev);
}

/* ---- 词表完整性 ---- */
static void test_words(void) {
    int i, j, count = (int)HM_WORDS_N;
    CHECK(count >= 100, "word count >= 100");
    for (i = 0; i < count; i++) {
        size_t l = strlen(hm_words[i]);
        if (l < 6 || l > 10) {
            CHECK(0, "word length 6..10");
            printf("  bad: %s len=%zu\n", hm_words[i], l);
        }
        for (j = 0; j < (int)l; j++)
            if (hm_words[i][j] < 'a' || hm_words[i][j] > 'z') {
                CHECK(0, "word lowercase");
                printf("  bad: %s\n", hm_words[i]);
                break;
            }
        for (j = i + 1; j < count; j++)
            if (strcmp(hm_words[i], hm_words[j]) == 0) {
                CHECK(0, "no duplicate words");
                printf("  dup: %s\n", hm_words[i]);
                break;
            }
    }
    printf("ok: %d words checked, lengths 6..10, unique\n", count);
}

/* ---- 猜对填所有位 + 胜 ---- */
static void test_win_banana(void) {
    test_set_word("banana");
    send_char('b', false);
    CHECK(hm_revealed[0] == 1, "b reveals pos0");
    CHECK(hm_err == 0, "correct guess no error");
    send_char('n', false);
    CHECK(hm_revealed[2] == 1 && hm_revealed[4] == 1, "n reveals pos2,pos4");
    CHECK(!hm_over, "not over yet");
    send_char('a', false);
    CHECK(hm_revealed[1] == 1 && hm_revealed[3] == 1 && hm_revealed[5] == 1,
          "a reveals all remaining");
    CHECK(hm_over && hm_won, "all revealed -> WIN");
}

/* ---- 6 错吊死 + 答对字母计数 ---- */
static void test_lose(void) {
    test_set_word("orange");
    send_char('z', false); send_char('q', false); send_char('j', false);
    send_char('x', false); send_char('v', false);
    CHECK(hm_err == 5 && !hm_over, "5 errors not over");
    send_char('k', false);
    CHECK(hm_err == 6 && hm_over && !hm_won, "6th error -> HANGED");
}

/* ---- 已猜字母忽略(不重复计错) ---- */
static void test_repeat_guess(void) {
    test_set_word("orange");
    send_char('z', false);
    send_char('z', false);
    send_char('z', true);           /* 重复事件也忽略 */
    CHECK(hm_err == 1, "repeat guess ignored");
    send_char('o', false);
    send_char('o', false);
    CHECK(hm_revealed[0] == 1 && hm_err == 1, "repeat correct ignored");
}

/* ---- 大小写归一 ---- */
static void test_case(void) {
    test_set_word("banana");
    send_char('B', false);
    CHECK(hm_revealed[0] == 1 && hm_used['b' - 'a'] == 1, "uppercase normalized");
    send_char('A', false);
    CHECK(hm_revealed[1] == 1 && hm_revealed[3] == 1 && hm_revealed[5] == 1,
          "uppercase A fills all positions");
    send_char('N', false);
    CHECK(hm_won && hm_over, "uppercase completes word");
}

/* ---- 非字母忽略 / 重复事件忽略 / 其他键 ---- */
static void test_noise(void) {
    test_set_word("orange");
    key_event_t ev;
    ev.is_repeat = true;
    ev.key = K_CHAR; ev.ch = 'o'; hangman_on_key(&ev);
    CHECK(hm_err == 0 && hm_revealed[0] == 0, "repeat letter ignored");
    ev.is_repeat = false;
    ev.key = K_CHAR; ev.ch = '1'; hangman_on_key(&ev);
    CHECK(hm_err == 0, "non-letter char ignored");
    ev.key = K_UP; ev.ch = 0; hangman_on_key(&ev);
    CHECK(hm_err == 0 && !hm_over, "direction key ignored");
}

/* ---- 字母全不同单词, 猜完必胜且错数未到 6 ---- */
static void test_win_fresh(void) {
    test_set_word("orange");
    const char *letters = "orange";
    int i;
    for (i = 0; i < 6; i++) send_char(letters[i], false);
    CHECK(hm_over && hm_won, "all distinct letters -> win");
    CHECK(hm_err == 0, "win with 0 errors");
}

/* ---- 结束状态按键: OK/N 新局, BACK 退出 ---- */
static void test_over_keys(void) {
    test_set_word("orange");
    send_char('z', false); send_char('q', false); send_char('j', false);
    send_char('x', false); send_char('v', false); send_char('k', false);
    CHECK(hm_over, "reached over state");
    s_exit_request = false;
    key_event_t ev;
    ev.is_repeat = false; ev.ch = 0;
    ev.key = K_OK; hangman_on_key(&ev);
    CHECK(!hm_over && hm_err == 0, "OK restarts game");
    test_set_word("orange");
    send_char('z', false); send_char('q', false); send_char('j', false);
    send_char('x', false); send_char('v', false); send_char('k', false);
    ev.key = K_CHAR; ev.ch = 'n'; hangman_on_key(&ev);
    CHECK(!hm_over && hm_err == 0, "'n' restarts game");
    test_set_word("orange");
    send_char('z', false); send_char('q', false); send_char('j', false);
    send_char('x', false); send_char('v', false); send_char('k', false);
    s_exit_request = false;
    ev.key = K_BACK; ev.ch = 0; hangman_on_key(&ev);
    CHECK(s_exit_request, "BACK quits after game over");
}

/* ---- 全模拟: 任一词按 a-z 全猜必然终止(无死循环) ---- */
static void test_termination(void) {
    int i;
    for (i = 0; i < (int)HM_WORDS_N; i++) {
        int g, guard = 0;
        test_set_word(hm_words[i]);
        for (g = 0; g < 26 && !hm_over; g++) {
            send_char((char)('a' + g), false);
            guard++;
        }
        if (!hm_over || guard > 26) {
            CHECK(0, "game always terminates");
            printf("  stuck on: %s\n", hm_words[i]);
            break;
        }
        CHECK(hm_won == (hm_err < 6), "end state consistent");
    }
    printf("ok: all %d words terminate\n", (int)HM_WORDS_N);
}

/* ---- 随机抽词: 多次 pick 全部合法 ---- */
static void test_pick(void) {
    int i;
    rng_seed(&hm_rng, 0x123456789ULL);   /* 显式固定种子 */
    for (i = 0; i < 2000; i++) {
        uint32_t idx = rng_range(&hm_rng, (uint32_t)HM_WORDS_N);
        if (idx >= HM_WORDS_N) { CHECK(0, "rng_range in range"); break; }
        hm_len = (int)strlen(hm_words[idx]);
        if (hm_len < 6 || hm_len > 10) { CHECK(0, "picked word valid"); break; }
    }
    printf("ok: 2000 picks in range\n");
}

/* ---- 渲染冒烟: 逐笔绘制 + 帧内像素 ---- */
static void test_render(void) {
    int i;
    /* err=0: 绞刑架空白 */
    test_set_word("orange");
    hangman_render();
    if (g_fb[(30 >> 3) * CCG_W + 30] & (0x80u >> (30 & 7)))
        CHECK(0, "no gallows at err=0");
    else
        printf("ok: no gallows at err=0\n");
    /* err=6: 竖杆(30,60) 与 头部(74,50) 均已绘制 */
    for (i = 0; i < 6; i++) send_char((char)('z' - i), false);
    hm_over = false;          /* 保持全画但去掉结束 HUD 影响 */
    hm_won = false;
    hm_over_full = false;
    hangman_render();
    if ((g_fb[(60 >> 3) * CCG_W + 30] & (0x80u >> (60 & 7))) == 0)
        CHECK(0, "post drawn at err=6");
    if ((g_fb[(50 >> 3) * CCG_W + 74] & (0x80u >> (50 & 7))) == 0)
        CHECK(0, "head drawn at err=6");
    printf("ok: gallows strokes drawn at err=6\n");
}

int main(void) {
    printf("== hangman logic tests ==\n");
    test_words();
    test_win_banana();
    test_lose();
    test_repeat_guess();
    test_case();
    test_noise();
    test_win_fresh();
    test_over_keys();
    test_termination();
    test_pick();
    test_render();
    if (s_fail) { printf("TOTAL FAIL: %d\n", s_fail); return 1; }
    printf("ALL PASS\n");
    return 0;
}
