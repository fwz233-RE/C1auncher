/* WORD CHAIN 逻辑测试 — host 编译运行; 含 wordchain.c 直访静态
 * 链接 canvas/font/font_data/pattern/rng/time/ui_common/input/display (-DCHICHU_HOST) */
#include <stdio.h>
#include <string.h>

#include "../../src/games/wordchain.c"

static int s_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); s_fail++; } \
    else { printf("ok: %s\n", msg); } \
} while (0)

/* 框架全局 stub(main.c 定义) */
bool s_exit_request = false;
void game_set_tick_interval(uint32_t ms) { (void)ms; }

static void send_key(ccg_key k, uint8_t ch, bool rep) {
    key_event_t ev;
    ev.key = k;
    ev.ch = ch;
    ev.is_repeat = rep;
    wordchain_on_key(&ev);
}

static void type_word(const char *w) {
    int i;
    for (i = 0; w[i]; i++) send_key(K_CHAR, (uint8_t)w[i], false);
}

static void press_ok(void) { send_key(K_OK, 0, false); }

/* 置链: 链上放 words[] 个词, 末字母设为 last */
static void set_chain(const char *word, char last) {
    wc_new_game();
    type_word(word);
    press_ok();
    /* 上面会触发 AI, 清掉重放单链状态 */
    wc_new_game();
    wc_chain[0] = (uint8_t)wc_find_word(word);
    wc_used[wc_chain[0]] = 1;
    wc_chain_len = 1;
    wc_last_c = last;
    wc_over = false;
    wc_won = false;
    wc_blen = 0;
    wc_buf[0] = '\0';
}

static void mark_group(char c) {
    int i;
    for (i = wc_start[c - 'a']; i < wc_start[c - 'a' + 1]; i++) wc_used[i] = 1;
}

static int fb_pix(int x, int y) {
    if (x < 0 || x >= (int)CCG_W || y < 0 || y >= (int)CCG_H) return 0;
    return (g_fb[(y >> 3) * (int)CCG_W + x] >> (7 - (y & 7))) & 1;
}

static int region_black(int x0, int y0, int x1, int y1) {
    int n = 0, x, y;
    for (y = y0; y <= y1; y++)
        for (x = x0; x <= x1; x++)
            if (fb_pix(x, y)) n++;
    return n;
}

/* ---- 词库完整性: 200 词, 3-10 字母, 全小写, 无重复, a-z 分组排序 ---- */
static void test_dict(void) {
    int i, j;
    CHECK(WC_WORDS_N == 200, "exactly 200 words");
    for (i = 0; i < WC_WORDS_N; i++) {
        size_t l = strlen(wc_words[i]);
        if (l < 3 || l > (size_t)WC_MAX_LEN) {
            CHECK(0, "word length 3..10");
            printf("  bad: %s len=%zu\n", wc_words[i], l);
        }
        for (j = 0; j < (int)l; j++)
            if (wc_words[i][j] < 'a' || wc_words[i][j] > 'z') {
                CHECK(0, "word lowercase");
                printf("  bad: %s\n", wc_words[i]);
                break;
            }
        for (j = i + 1; j < WC_WORDS_N; j++)
            if (strcmp(wc_words[i], wc_words[j]) == 0) {
                CHECK(0, "no duplicate words");
                printf("  dup: %s\n", wc_words[i]);
                break;
            }
    }
    printf("ok: %d words unique, len 3..10, lowercase\n", WC_WORDS_N);
}

/* ---- 首字母索引表 ---- */
static void test_index(void) {
    int c, i;
    wc_build_index();
    CHECK(wc_start[26] == WC_WORDS_N, "start[26] == N");
    CHECK(wc_start[0] == 0, "a-group starts at 0");
    for (c = 0; c < 26; c++) {
        if (wc_start[c] < 0 || wc_start[c] >= WC_WORDS_N) {
            CHECK(0, "start in range");
            printf("  bad start[%d]=%d\n", c, wc_start[c]);
            continue;
        }
        if (c > 0 && wc_start[c] < wc_start[c - 1]) {
            CHECK(0, "start nondecreasing");
        }
        for (i = wc_start[c]; i < wc_start[c + 1]; i++)
            if (wc_words[i][0] != (char)('a' + c)) {
                CHECK(0, "group first letter matches");
                printf("  %s not in group %c\n", wc_words[i], 'a' + c);
                break;
            }
        if (wc_start[c] == wc_start[c + 1])
            printf("  note: no words for '%c'\n", 'a' + c);
    }
    printf("ok: index table covers all 26 letters\n");
}

/* ---- 新局重置 ---- */
static void test_newgame(void) {
    int i, used = 0;
    wc_new_game();
    for (i = 0; i < WC_WORDS_N; i++) if (wc_used[i]) used++;
    CHECK(used == 0, "all words unused");
    CHECK(wc_chain_len == 0 && wc_blen == 0, "chain and buffer empty");
    CHECK(!wc_over && !wc_won, "not over");
    CHECK(wc_last_c == 0, "no required letter yet");
    CHECK(wc_find_word("apple") >= 0, "apple in dict");
    CHECK(wc_find_word("zzzz") < 0, "junk not in dict");
}

/* ---- 输入/退格/大小写/上限 ---- */
static void test_typing(void) {
    wc_new_game();
    type_word("appl");
    CHECK(wc_blen == 4 && strcmp(wc_buf, "appl") == 0, "type 4 letters");
    send_key(K_CHAR, (uint8_t)'E', false);
    CHECK(wc_blen == 5 && wc_buf[4] == 'e', "uppercase normalized");
    send_key(K_DEL, 0, false);
    CHECK(wc_blen == 4 && wc_buf[4] == '\0', "DEL removes last");
    send_key(K_DEL, 0, true);
    CHECK(wc_blen == 3, "repeat DEL keeps deleting");
    wc_blen = 0; wc_buf[0] = '\0';
    type_word("abcdefghijklmnop");      /* 超长输入截断 */
    CHECK(wc_blen == WC_MAX_LEN, "buffer capped at WC_MAX_LEN");
    CHECK(wc_buf[WC_MAX_LEN] == '\0', "buffer terminated");
    /* 非字母忽略 */
    send_key(K_CHAR, (uint8_t)'1', false);
    send_key(K_CHAR, (uint8_t)'.', false);
    CHECK(wc_blen == WC_MAX_LEN, "non-letter ignored");
    send_key(K_OK, 0, true);
    CHECK(!wc_over, "repeat OK ignored");
}

/* ---- 首词自由 + AI 应答合法 ---- */
static void test_first_and_ai(void) {
    wc_new_game();
    rng_seed(&wc_rng, 0x1234ULL);
    type_word("apple");
    press_ok();
    CHECK(!wc_over, "valid first word not over");
    CHECK(wc_chain_len == 2, "chain grows to 2 (player + AI)");
    CHECK(wc_chain[0] == wc_find_word("apple"), "chain[0] is apple");
    CHECK(wc_words[wc_chain[1]][0] == 'e', "AI word starts with 'e' (apple ends e)");
    CHECK(wc_used[wc_chain[1]] == 1, "AI word marked used");
    CHECK(wc_used[wc_chain[0]] == 1, "player word marked used");
    CHECK(wc_last_c == wc_words[wc_chain[1]][strlen(wc_words[wc_chain[1]]) - 1],
          "last letter updated to AI word's last");
}

/* ---- 坏链: 首字母不接 ---- */
static void test_bad_link(void) {
    set_chain("apple", 'e');
    type_word("book");          /* 'b' != 'e' */
    press_ok();
    CHECK(wc_over && !wc_won, "wrong first letter -> lose");
    CHECK(wc_reason == WC_RS_LINK, "reason LINK");
}

/* ---- 坏词: 不在词库(首字母正确) ---- */
static void test_bad_word(void) {
    set_chain("apple", 'e');
    type_word("eabcd");         /* 以 e 开头但不在词库 */
    press_ok();
    CHECK(wc_over && !wc_won, "not in dict -> lose");
    CHECK(wc_reason == WC_RS_BAD, "reason BAD");
    /* 空缓冲提交不触发 */
    wc_new_game();
    press_ok();
    CHECK(!wc_over, "empty submit ignored");
}

/* ---- 已用过 ---- */
static void test_used_word(void) {
    set_chain("apple", 'a');
    type_word("apple");         /* 'a' 匹配但已用过 */
    press_ok();
    CHECK(wc_over && !wc_won, "used word -> lose");
    CHECK(wc_reason == WC_RS_USED, "reason USED");
}

/* ---- AI 无词可接 -> 玩家赢 ---- */
static void test_ai_stuck(void) {
    int i;
    wc_new_game();
    mark_group('o');            /* 用光所有 o 开头词 */
    type_word("zoo");           /* 首词自由; zoo 以 o 结尾 */
    press_ok();
    CHECK(wc_over && wc_won, "AI stuck -> player wins");
    CHECK(wc_reason == WC_RS_AI_STUCK, "reason AI_STUCK");
    CHECK(wc_chain_len == 1, "chain = zoo only (AI placed nothing)");
    for (i = 0; i < WC_WORDS_N; i++)
        if (wc_used[i] && wc_words[i][0] != 'o' && strcmp(wc_words[i], "zoo") != 0) {
            CHECK(0, "only o-group and zoo used");
            break;
        }
}

/* ---- 玩家无词可接 -> 输 ---- */
static void test_player_stuck(void) {
    wc_new_game();
    mark_group('g');
    mark_group('m');
    mark_group('e');
    wc_used[wc_find_word("egg")] = 0;   /* e 组仅留 egg */
    type_word("apple");                 /* 以 e 结尾 */
    press_ok();
    /* 同步流程: 提交后 AI 只能出 egg, 玩家 g 组全空 → 立即判负 */
    CHECK(wc_over && !wc_won, "player has no g-word -> lose");
    CHECK(wc_reason == WC_RS_NOMOVE, "reason NOMOVE");
    CHECK(wc_chain_len == 2, "chain = apple, egg (player never placed)");
    CHECK(strcmp(wc_words[wc_chain[1]], "egg") == 0, "AI forced to egg");
    CHECK(wc_chain[0] == wc_find_word("apple"), "chain[0] = apple");
}

/* ---- 结束态按键: OK/N 新局, BACK 退出 ---- */
static void test_over_keys(void) {
    set_chain("apple", 'e');
    type_word("book");
    press_ok();
    CHECK(wc_over, "reached over state");
    s_exit_request = false;
    send_key(K_OK, 0, false);
    CHECK(!wc_over && wc_chain_len == 0, "OK restarts");
    set_chain("apple", 'e');
    type_word("book");
    press_ok();
    send_key(K_CHAR, (uint8_t)'n', false);
    CHECK(!wc_over && wc_chain_len == 0, "'n' restarts");
    set_chain("apple", 'e');
    type_word("book");
    press_ok();
    s_exit_request = false;
    send_key(K_BACK, 0, false);
    CHECK(s_exit_request, "BACK quits after game over");
}

/* ---- 暂停键(进行中) ---- */
static void test_pause_key(void) {
    /* ui_pause_run 阻塞等待输入, 不进入; 验证 K_QUIT 常规退出路径 */
    s_exit_request = false;
    wc_new_game();
    send_key(K_QUIT, 0, false);
    CHECK(s_exit_request, "Q quits during play");
}

/* ---- 全模拟: 贪心玩家 vs AI, 多种子, 必然终止且链始终合法 ---- */
static void test_sim(void) {
    int seed;
    for (seed = 1; seed <= 6; seed++) {
        int steps = 0, ok = 1;
        wc_new_game();
        rng_seed(&wc_rng, (uint32_t)seed * 0x9E3779B9U + 7U);
        while (!wc_over && steps < 400) {
            int i, pick = -1;
            if (wc_chain_len == 0) {
                pick = 0;                       /* 任意首词 */
            } else {
                int lo = wc_start[wc_last_c - 'a'];
                int hi = wc_start[wc_last_c - 'a' + 1];
                for (i = lo; i < hi; i++)
                    if (!wc_used[i]) { pick = i; break; }
            }
            if (pick < 0) { ok = 0; break; }    /* 理论不可达(NOMOVE 已判负) */
            type_word(wc_words[pick]);
            press_ok();
            /* 校验链链接 */
            if (wc_chain_len > 1) {
                const char *a = wc_words[wc_chain[wc_chain_len - 2]];
                const char *b = wc_words[wc_chain[wc_chain_len - 1]];
                if (b[0] != a[strlen(a) - 1]) ok = 0;
            }
            steps++;            if (wc_over) {
                CHECK(wc_reason == WC_RS_AI_STUCK || wc_reason == WC_RS_NOMOVE,
                      "sim ends with AI_STUCK or NOMOVE only");
            }
        }
        if (!ok || steps >= 400 || !wc_over) {
            CHECK(0, "simulation terminated cleanly");
            printf("  seed=%d steps=%d over=%d\n", seed, steps, wc_over);
            continue;
        }
        CHECK(wc_won == (wc_reason == WC_RS_AI_STUCK), "win iff AI stuck");
        CHECK(wc_chain_len >= 2, "chain at least 2 long");
        CHECK(wc_chain_len <= WC_WORDS_N, "chain bounded by dict size");
        printf("ok: sim seed=%d end=%s chain=%d\n", seed,
               wc_won ? "WIN" : "LOSE", wc_chain_len);
    }
}

/* ---- 渲染冒烟 ---- */
static void test_render(void) {
    int x0 = (CCG_W - (WC_MAX_LEN * WC_CELL_PITCH - 2)) / 2;
    wc_new_game();
    wordchain_render();
    CHECK(fb_pix(100, CCG_HUD_H - 1) == 1, "HUD separator drawn");
    CHECK(fb_pix(x0 + 2, WC_INPUT_Y + 2) == 1, "cursor cell black at pos 0");
    CHECK(fb_pix(x0 + 5, WC_INPUT_Y + 10) == 1, "cursor cell filled");
    CHECK(fb_pix(50, 40) == 0, "chain area empty before words");
    send_key(K_CHAR, (uint8_t)'a', false);
    wordchain_render();
    CHECK(fb_pix(x0 + WC_CELL_PITCH + 2, WC_INPUT_Y + 2) == 1,
          "cursor moved to cell 1");
    CHECK(region_black(x0 + 1, WC_INPUT_Y + 1, x0 + WC_CELL_W - 2,
                       WC_INPUT_Y + WC_CELL_H - 2) > 0,
          "typed letter drawn in cell 0");
    /* 结束渲染 */
    set_chain("apple", 'e');
    type_word("book");
    press_ok();
    CHECK(wc_over, "over state for render");
    wc_over_full = false;
    wordchain_render();
    CHECK(wc_over_full, "over render requests force full once");
    CHECK(region_black(2, 1, 60, 8) > 0, "result text drawn in HUD");
    CHECK(region_black(CCG_W - 140, 1, CCG_W - 2, 8) > 0,
          "retry hint drawn in HUD");
    CHECK(region_black(6, 38, 120, 45) > 0, "chain row drawn");
}

int main(void) {
    printf("== wordchain logic tests ==\n");
    test_dict();
    test_index();
    test_newgame();
    test_typing();
    test_first_and_ai();
    test_bad_link();
    test_bad_word();
    test_used_word();
    test_ai_stuck();
    test_player_stuck();
    test_over_keys();
    test_pause_key();
    test_sim();
    test_render();
    if (s_fail) { printf("TOTAL FAIL: %d\n", s_fail); return 1; }
    printf("ALL PASS\n");
    return 0;
}
