/* BOGGLE 逻辑单测 — host cc 编译运行; 直接 include 游戏源文件访问静态状态 */
#include "../../src/games/boggle.c"
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

/* 词库查找辅助(测试用) */
static int bo_dict_in(const char *w) {
    int i;
    for (i = 0; i < BO_DICT_N; i++)
        if (strcmp(bo_dict[i], w) == 0) return 1;
    return 0;
}

/* 词库完整性: 100 词, 3-7 字母, 小写, 仅字母池字母, 无重复 */
static void test_dict(void) {
    static const char *pool = "aeiounrstdlbbcmfghkpvwj";
    int i, j, n = BO_DICT_N;
    CHECK(n == 100, "dict has 100 words");
    for (i = 0; i < n; i++) {
        size_t len = strlen(bo_dict[i]);
        if (len < 3 || len > 7) {
            CHECK(0, "dict word length in 3..7");
            continue;
        }
        for (j = 0; j < (int)len; j++) {
            if (bo_dict[i][j] < 'a' || bo_dict[i][j] > 'z') {
                CHECK(0, "dict word lowercase");
                break;
            }
            if (strchr(pool, bo_dict[i][j]) == NULL) {
                CHECK(0, "dict word letters all in board pool");
                break;
            }
        }
        for (j = i + 1; j < n; j++)
            if (strcmp(bo_dict[i], bo_dict[j]) == 0)
                CHECK(0, "dict words unique");
    }
    /* 关键测试词确实在词库里 */
    CHECK(strcmp(bo_dict[0], "ace") == 0, "dict[0] is ace");
    CHECK(bo_dict_in("cat"), "cat in dict");
    CHECK(!bo_dict_in("cac"), "cac not in dict");
}

static void test_scoring(void) {
    CHECK(bo_score_of(3) == 1, "score 3 -> 1");
    CHECK(bo_score_of(4) == 2, "score 4 -> 2");
    CHECK(bo_score_of(5) == 3, "score 5 -> 3");
    CHECK(bo_score_of(6) == 5, "score 6 -> 5");
    CHECK(bo_score_of(7) == 5, "score 7 -> 5");
}

/* 棋盘生成: 字母都在池内, 且保证至少一个词库词可拼出(多个种子) */
static void test_board_gen(void) {
    static const char *pool = "aeiounrstdlbbcmfghkpvwj";
    int seed;
    for (seed = 1; seed <= 10; seed++) {
        int i;
        rng_seed(&bo_rng, (uint64_t)seed);
        bo_new_board();
        for (i = 0; i < 16; i++) {
            if (strchr(pool, bo_board[i]) == NULL) {
                CHECK(0, "board letters all from pool");
                return;
            }
        }
        if (!bo_board_findable()) {
            CHECK(0, "every board has >=1 dict word");
            return;
        }
    }
    printf("ok: board gen valid across 10 seeds\n");
}

/* 相邻/同格规则 + 首字母 OK=提交 */
static void test_select_rules(void) {
    int i;
    bo_over = false;
    bo_clear_word();
    bo_score = 0;
    bo_found_n = 0;
    /* 棋盘: (0,0)c (0,1)a (1,1)t 拼出 cat */
    for (i = 0; i < 16; i++) bo_board[i] = 'x';
    bo_board[0] = 'c';
    bo_board[1] = 'a';
    bo_board[5] = 't';

    /* 对角相邻合法: C -> A(右), A -> T(左下) */
    bo_select_cell(0, 0);
    bo_select_cell(0, 1);
    bo_select_cell(1, 1);
    CHECK(bo_len == 3, "C-A-T path accepted (adjacent incl diagonal)");
    CHECK(strcmp(bo_word, "cat") == 0, "word is cat");

    /* 非相邻拒绝 */
    bo_clear_word();
    bo_select_cell(0, 0);
    bo_select_cell(2, 2);
    CHECK(bo_len == 1, "non-adjacent cell rejected");

    /* 同格两次拒绝 */
    bo_clear_word();
    bo_select_cell(0, 0);
    bo_select_cell(0, 0);
    CHECK(bo_len == 1, "same cell twice rejected");

    /* 首字母上 OK = 提交 */
    bo_clear_word();
    bo_select_cell(0, 0);
    bo_select_cell(0, 1);
    bo_select_cell(1, 1);
    bo_select_cell(0, 0);
    CHECK(bo_found_n == 1, "OK on first letter submits");
    CHECK(bo_score == 1, "cat scored 1");
    CHECK(bo_len == 0, "word cleared after submit");
}

static void test_submit_cases(void) {
    int i;
    bo_over = false;
    bo_clear_word();
    bo_score = 0;
    bo_found_n = 0;
    bo_msg_ttl = 0;
    for (i = 0; i < 16; i++) bo_board[i] = 'x';
    bo_board[0] = 'c';   /* (0,0) */
    bo_board[1] = 'a';   /* (0,1) */
    bo_board[5] = 't';   /* (1,1) */
    bo_board[6] = 'r';   /* (1,2) */
    bo_board[7] = 'k';   /* (1,3) */

    /* 首次 cat 命中: +1 分 */
    bo_select_cell(0, 0);
    bo_select_cell(0, 1);
    bo_select_cell(1, 1);
    bo_submit();
    CHECK(bo_found_n == 1 && bo_score == 1, "cat accepted +1");
    CHECK(strcmp(bo_msg, "cat+1") == 0, "cat+1 message");

    /* 重复词: 再拼一次 → ALREADY, 不重复计分 */
    bo_select_cell(0, 0);
    bo_select_cell(0, 1);
    bo_select_cell(1, 1);
    bo_submit();
    CHECK(bo_found_n == 1 && bo_score == 1, "duplicate cat rejected");
    CHECK(strcmp(bo_msg, "ALREADY") == 0, "dup message ALREADY");

    /* 4 字母词 bark(词库有 "bark")→ 2 分; 重摆棋盘 */
    for (i = 0; i < 16; i++) bo_board[i] = 'x';
    bo_board[0] = 'b';   /* (0,0) */
    bo_board[1] = 'a';   /* (0,1) */
    bo_board[5] = 'r';   /* (1,1) */
    bo_board[6] = 'k';   /* (1,2) */
    bo_clear_word();
    bo_select_cell(0, 0);
    bo_select_cell(0, 1);
    bo_select_cell(1, 1);
    bo_select_cell(1, 2);
    CHECK(bo_len == 4, "bark path built");
    bo_submit();
    CHECK(bo_found_n == 2, "bark found too");
    CHECK(bo_score == 3, "cat(1) + bark(2) = 3");
    CHECK(strcmp(bo_msg, "bark+2") == 0, "bark+2 message");

    /* 非词: cac */
    bo_clear_word();
    bo_board[0] = 'c';
    bo_board[2] = 'c';
    bo_select_cell(0, 0);
    bo_select_cell(0, 1);
    bo_select_cell(0, 2);
    bo_submit();
    CHECK(bo_found_n == 2, "cac (not a word) rejected");
    CHECK(bo_score == 3, "score unchanged for non-word");
    CHECK(strcmp(bo_msg, "NOT A WORD") == 0, "not-a-word message");

    /* 过短: 2 字母提交 */
    bo_clear_word();
    bo_select_cell(0, 0);
    bo_select_cell(0, 1);
    bo_submit();
    CHECK(bo_len == 0 && bo_found_n == 2 && bo_score == 3,
          "too-short submit rejected");
    CHECK(strcmp(bo_msg, "TOO SHORT") == 0, "too-short message");
}

static void test_undo(void) {
    bo_over = false;
    bo_clear_word();
    bo_board[0] = 'c';
    bo_board[1] = 'a';
    bo_board[5] = 't';
    bo_select_cell(0, 0);
    bo_select_cell(0, 1);
    bo_select_cell(1, 1);
    CHECK(bo_len == 3, "3 letters selected");
    bo_undo();
    CHECK(bo_len == 2, "undo -> 2 letters");
    CHECK(bo_cur_r == 1 && bo_cur_c == 1, "cursor back on removed cell");
    CHECK(strcmp(bo_word, "ca") == 0, "word truncated to ca");
    bo_undo();
    bo_undo();
    CHECK(bo_len == 0, "undo all -> empty");
}

static void test_cursor_wrap(void) {
    bo_over = false;
    bo_cur_r = 0;
    bo_cur_c = 3;
    bo_move_cursor(0, 1);
    CHECK(bo_cur_c == 0, "cursor wraps right");
    bo_move_cursor(-1, 0);
    CHECK(bo_cur_r == 3, "cursor wraps up");
    bo_move_cursor(1, -1);
    CHECK(bo_cur_r == 0 && bo_cur_c == 3, "cursor wraps down/left");
}

static void test_timer(void) {
    bo_over = false;
    bo_elapsed = BO_GAME_MS - 200;   /* 剩 0.2s */
    bo_msg_ttl = 2;
    boggle_tick(0);
    CHECK(!bo_over, "not over with 0.2s left");
    CHECK(bo_elapsed == BO_GAME_MS - 100, "elapsed +100ms");
    CHECK(bo_msg_ttl == 1, "msg ttl decrements");
    CHECK(bo_time_left() == 0, "time left rounds to 0");
    boggle_tick(0);
    CHECK(bo_over, "over when time expires");
    CHECK(bo_len == 0, "word cleared at end");
}

/* 端到端: enter + 一次完整得分回合 + 结束态渲染路径(host 帧缓冲) */
static void test_enter_flow(void) {
    int i;
    boggle_enter();
    CHECK(!bo_over, "enter starts new game");
    CHECK(bo_score == 0 && bo_found_n == 0, "fresh score");
    CHECK(bo_board_findable(), "enter board is playable");
    /* 直接在棋盘上摆 cat 模拟找到单词 */
    bo_board[0] = 'c';
    bo_board[1] = 'a';
    bo_board[5] = 't';
    bo_select_cell(0, 0);
    bo_select_cell(0, 1);
    bo_select_cell(1, 1);
    bo_submit();
    CHECK(bo_score == 1 && bo_found_n == 1, "found cat in flow");
    /* 时间走完 → 结束渲染(含 disp_force_full 一次) */
    bo_over_full = false;
    bo_elapsed = BO_GAME_MS - 50;
    boggle_tick(0);
    CHECK(bo_over, "flow: time up ends round");
    boggle_render();
    CHECK(bo_over_full, "over-full flag set after render");
    for (i = 0; i < 10; i++) boggle_render();
    (void)i;
    /* 结束态按 OK 重开 */
    {
        key_event_t ev;
        memset(&ev, 0, sizeof(ev));
        ev.key = K_OK;
        boggle_on_key(&ev);
        CHECK(!bo_over, "OK retries new round");
    }
    /* 结束态按 BACK 退出(先重开再直接进入结束态) */
    {
        key_event_t ev;
        memset(&ev, 0, sizeof(ev));
        bo_over = true;
        bo_over_full = true;
        ev.key = K_BACK;
        boggle_on_key(&ev);
        CHECK(s_exit_request, "BACK quits from game over");
        s_exit_request = false;
    }
    printf("ok: enter/render/over/retry flow exercised\n");
}

int main(void) {
    test_dict();
    test_scoring();
    test_board_gen();
    test_select_rules();
    test_submit_cases();
    test_undo();
    test_cursor_wrap();
    test_timer();
    test_enter_flow();
    if (s_fail) {
        printf("TOTAL FAILURES: %d\n", s_fail);
        return 1;
    }
    printf("ALL TESTS PASSED\n");
    return 0;
}
