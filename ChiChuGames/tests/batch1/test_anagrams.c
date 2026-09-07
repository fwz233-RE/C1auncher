/* ANAGRAMS host 逻辑单测 — 直接包含 anagrams.c 访问静态状态 */
#include "../../src/games/anagrams.c"
#include <stdio.h>
#include <string.h>

bool s_exit_request = false;
void game_set_tick_interval(uint32_t ms) { (void)ms; }

static int s_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); s_fail++; } \
    else { printf("ok: %s\n", msg); } \
} while (0)

/* 直接搭一局(绕开随机抽词): 固定 target/pool */
static void set_round(const char *word, const char *pool) {
    an_len = (int)strlen(word);
    memcpy(an_target, word, (size_t)an_len + 1);
    memcpy(an_pool, pool, (size_t)an_len);
    memset(an_taken, 0, sizeof(an_taken));
    memset(an_ans, 0, sizeof(an_ans));
    memset(an_from, 0, sizeof(an_from));
    an_ans_len = 0;
    an_curs = 0;
    an_over = false;
    an_over_full = false;
}

/* 在堆中找第 skip 个未取且等于 ch 的下标 */
static int find_pool(char ch, int skip) {
    int i, seen = 0;
    for (i = 0; i < an_len; i++) {
        if (an_pool[i] == ch && !an_taken[i]) {
            if (seen == skip) return i;
            seen++;
        }
    }
    return -1;
}

/* 按 target 顺序把前 n 个字母放入答案区 */
static void place_prefix(int n) {
    int i;
    for (i = 0; i < n; i++) {
        int idx = find_pool(an_target[i], 0);
        if (idx < 0) { printf("no pool letter for %c\n", an_target[i]); s_fail++; return; }
        an_curs = idx;
        an_place();
    }
}

static void key(ccg_key k, uint8_t ch, bool rep) {
    key_event_t ev;
    ev.key = k;
    ev.ch = ch;
    ev.is_repeat = rep;
    anagrams_on_key(&ev);
}

/* ---- 词表完整性 ---- */
static void test_wordlist(void) {
    unsigned i, j;
    CHECK(AN_WORDS_N == 30, "wordlist: 30 words");
    for (i = 0; i < AN_WORDS_N; i++) {
        int len = (int)strlen(an_words[i]);
        CHECK(len >= 6 && len <= 9, "wordlist: length 6..9");
        for (j = 0; j < (unsigned)len; j++)
            if (an_words[i][j] < 'a' || an_words[i][j] > 'z') {
                CHECK(0, "wordlist: lowercase only");
                break;
            }
    }
    for (i = 0; i < AN_WORDS_N; i++)
        for (j = i + 1; j < AN_WORDS_N; j++)
            if (strcmp(an_words[i], an_words[j]) == 0)
                CHECK(0, "wordlist: no duplicates");
}

/* ---- 洗牌: 多次随机局, 堆必为主词排列且不为原序 ---- */
static void test_shuffle(void) {
    int round;
    rng_seed(&an_rng, 12345ULL);
    for (round = 0; round < 200; round++) {
        int i;
        an_new_round();
        /* 长度一致 */
        CHECK(an_len == (int)strlen(an_target), "shuffle: len matches");
        /* 堆是 target 的排列(字符多重集相等) */
        {
            int pc[26] = { 0 }, tc[26] = { 0 };
            for (i = 0; i < an_len; i++) {
                pc[an_pool[i] - 'a']++;
                tc[an_target[i] - 'a']++;
            }
            for (i = 0; i < 26; i++)
                if (pc[i] != tc[i]) CHECK(0, "shuffle: pool is a permutation");
        }
        /* 洗牌不得与原序全同 */
        if (memcmp(an_pool, an_target, (size_t)an_len) == 0)
            CHECK(0, "shuffle: never identical to target");
        /* 新局初始状态 */
        CHECK(an_ans_len == 0 && an_curs == 0 && !an_over, "shuffle: fresh state");
    }
}

/* ---- 拼词胜负 + 得分 ---- */
static void test_solve_win(void) {
    set_round("planet", "natelp");
    an_score = 40;
    place_prefix(6);
    CHECK(an_over, "win: over after full correct word");
    CHECK(an_score == 40 + 60, "win: score += 10 x len");
    CHECK(an_ans_len == an_len && memcmp(an_ans, an_target, (size_t)an_len) == 0,
          "win: answer holds target");
}

/* ---- 重复字母词(重排拼对仍赢) ---- */
static void test_duplicate_letters(void) {
    set_round("cabbage", "cabbage");
    place_prefix(7);
    CHECK(an_over, "dup: solve cabbage with duplicate b/a");
    /* 反向排列则放满未赢, 撤销后还能重拼 */
    set_round("cabbage", "bbacaeg");
    an_score = 0;
    {
        int i;
        for (i = an_len - 1; i >= 0; i--) {
            an_curs = find_pool(an_target[i], 0);
            if (an_curs < 0) { CHECK(0, "dup: pool letter found"); return; }
            an_place();
        }
    }
    CHECK(an_ans_len == an_len && !an_over, "dup: full but wrong order not over");
    an_undo();
    CHECK(an_ans_len == an_len - 1, "dup: undo works after full wrong");
}

/* ---- 提示: RIGHT 计数(含重复字母) ---- */
static void test_right_count(void) {
    set_round("orange", "oearng");   /* 乱序 */
    an_curs = find_pool('o', 0);
    an_place();                      /* 第 0 位放 o -> 对 1 */
    CHECK(an_right_count() == 1, "right: first letter correct");
    an_curs = find_pool('e', 0);
    an_place();                      /* 第 1 位放 e -> 目标第 1 位是 r, 错 */
    CHECK(an_right_count() == 1, "right: second letter wrong");
    /* 直接构造含重复字母的答案验证逐位计数 */
    set_round("cabbage", "cabbage");
    an_ans[0] = 'c'; an_ans[1] = 'b'; an_ans[2] = 'a'; an_ans[3] = 'b';
    an_ans_len = 4;
    /* target: c a b b a g e -> c 对, a 错(b), b 错(a), b 对 => 2 */
    CHECK(an_right_count() == 2, "right: duplicate-aware per-position count");
}

/* ---- 撤销: 字母回堆, 光标回位 ---- */
static void test_undo(void) {
    set_round("travel", "evlrat");
    an_curs = find_pool('v', 0);
    an_place();
    CHECK(an_ans_len == 1 && an_ans[0] == 'v', "undo: placed v");
    an_undo();
    CHECK(an_ans_len == 0, "undo: ans back to 0");
    CHECK(!an_taken[an_from[0]] && an_taken[find_pool('v', 0)] == 0, "undo: letter returned");
    CHECK(an_curs == find_pool('v', 0), "undo: cursor back to letter");
    an_undo();                          /* 空答案再撤销: 无变化 */
    CHECK(an_ans_len == 0, "undo: empty undo no-op");
    /* DEL 与 BACK 等效 */
    an_curs = find_pool('t', 0);
    an_place();
    key(K_BACK, 0, false);
    CHECK(an_ans_len == 0, "undo: K_BACK undoes");
    an_curs = find_pool('t', 0);
    an_place();
    key(K_DEL, 0, false);
    CHECK(an_ans_len == 0, "undo: K_DEL undoes");
}

/* ---- 光标: 移动跳已取、环绕、全取时有界 ---- */
static void test_cursor(void) {
    set_round("window", "wndiow");
    an_curs = 3;
    an_move_cursor(-1);
    CHECK(an_curs == 2, "cursor: left wraps to 2");
    an_move_cursor(-1);
    CHECK(an_curs == 1, "cursor: left to 1");
    /* 取走 1,4 后移动自动跳过已取 */
    an_curs = 1;
    an_place();
    an_curs = 4;
    an_place();
    an_curs = 4;
    an_move_cursor(1);
    CHECK(!an_taken[an_curs], "cursor: skips taken slot");
    /* 全部取走后移动有界且不动 */
    set_round("window", "wndiow");
    {
        int start, i;
        for (i = 0; i < an_len; i++) {
            an_curs = i;
            an_place();
        }
        start = an_curs;
        an_move_cursor(1);
        CHECK(an_curs == start, "cursor: bounded when all taken");
        CHECK(an_ans_len == an_len && !an_over, "cursor: full wrong not over");
    }
}

/* ---- 键处理 ---- */
static void test_keys(void) {
    set_round("orange", "oearng");
    /* OK 重复忽略 */
    an_curs = 0;
    key(K_OK, 0, true);
    CHECK(an_ans_len == 0, "keys: repeated OK ignored");
    /* LEFT/RIGHT 重复可响应 */
    an_curs = 2;
    key(K_RIGHT, 0, true);
    CHECK(an_curs == 3, "keys: repeated RIGHT moves");
    /* N 新局 */
    key(K_CHAR, 'n', false);
    CHECK(an_ans_len == 0 && !an_over && an_len >= 6, "keys: N starts new round");
    /* K_CHAR 其他字母忽略 */
    key(K_CHAR, 'x', false);
    CHECK(an_ans_len == 0, "keys: stray letter ignored");
    /* Q 退出 */
    s_exit_request = false;
    key(K_QUIT, 0, false);
    CHECK(s_exit_request, "keys: Q quits");
}

/* ---- 结束态按键: OK/N 重开, BACK 退出 ---- */
static void test_over_keys(void) {
    set_round("planet", "natelp");
    place_prefix(6);
    CHECK(an_over, "over: solved");
    s_exit_request = false;
    key(K_BACK, 0, false);
    CHECK(s_exit_request, "over: BACK quits");
    key(K_OK, 0, false);
    CHECK(!an_over && an_ans_len == 0, "over: OK retries");
    /* 再赢一次, N 重开 */
    place_prefix(6);
    CHECK(an_over, "over: won again");
    key(K_CHAR, 'n', false);
    CHECK(!an_over && an_ans_len == 0, "over: N retries");
}

/* ---- enter 重置得分与种子换局 ---- */
static void test_enter(void) {
    uint64_t s0;
    an_score = 999;
    an_seed_cnt = 7;
    rng_seed(&an_rng, 1ULL);
    anagrams_enter();
    CHECK(an_score == 0, "enter: score reset");
    CHECK(an_len >= 6 && an_len <= 9 && an_ans_len == 0, "enter: fresh round");
    /* 相同种子序列换局: 二次 enter 得不同词(概率性, 用固定种子验证可复现即可) */
    s0 = an_seed_cnt;
    anagrams_enter();
    CHECK(an_seed_cnt == s0 + 1, "enter: seed counter increments");
}

int main(void) {
    test_wordlist();
    test_shuffle();
    test_solve_win();
    test_duplicate_letters();
    test_right_count();
    test_undo();
    test_cursor();
    test_keys();
    test_over_keys();
    test_enter();
    printf("\n%s (%d failures)\n", s_fail ? "FAILED" : "ALL PASS", s_fail);
    return s_fail ? 1 : 0;
}
