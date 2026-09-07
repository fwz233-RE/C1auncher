/* SPIDER 逻辑单测 — host 编译, 包含游戏 .c 直接访问静态状态 */
#include "../../src/games/spider.c"
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

static void clear_all(void) {
    memset(sp_cnt, 0, sizeof(sp_cnt));
    sp_stock_n = 0;
    sp_sel = -1;
    sp_over = false;
    sp_stuck = false;
    sp_runs = 0;
    sp_moves = 0;
    sp_deals = 0;
}

/* 把列 c 置为给定 rank 序列(自底向上) */
static void set_col(int c, const int *ranks, int n) {
    for (int i = 0; i < n; i++) sp_col[c][i] = (uint8_t)(ranks[i] - 1);
    sp_cnt[c] = (uint8_t)n;
}

/* ---- 发牌: 列大小/发牌堆/全牌唯一 ---- */
static void test_deal(void) {
    rng_seed(&sp_rng, 42);
    sp_reset();
    int seen[SP_DECK] = { 0 };
    int total = 0;
    for (int c = 0; c < SP_NCOL; c++) {
        int expect = (c < 4) ? 6 : 5;
        CHECK(sp_cnt[c] == expect, "column size 6/6/6/6/5x6");
        total += sp_cnt[c];
        for (int k = 0; k < sp_cnt[c]; k++) seen[sp_col[c][k]] = 1;
    }
    CHECK(total == 54, "54 cards dealt to columns");
    CHECK(sp_stock_n == 50, "50 cards in stock");
    for (int i = 0; i < sp_stock_n; i++) seen[sp_stock[i]] = 1;
    int cnt = 0;
    for (int i = 0; i < SP_DECK; i++) cnt += seen[i];
    CHECK(cnt == SP_DECK, "all 104 cards present exactly once");
}

/* ---- rank 映射 ---- */
static void test_rank(void) {
    CHECK(sp_rank(0) == 1, "value 0 -> A");
    CHECK(sp_rank(12) == 13, "value 12 -> K");
    CHECK(sp_rank(13) == 1, "value 13 -> A (2nd suit)");
    CHECK(sp_rank(103) == 13, "value 103 -> K (8th suit)");
}

/* ---- 移动规则 ---- */
static void test_move(void) {
    clear_all();
    int k1[1] = { 13 }, f1[1] = { 4 }, t1[1] = { 2 }, th1[1] = { 3 };
    set_col(0, k1, 1);                              /* col0: K */
    set_col(1, f1, 1);                              /* col1: 4 */
    CHECK(!sp_try_move(0, 1), "K onto 4 rejected");
    CHECK(sp_cnt[0] == 1 && sp_cnt[1] == 1, "failed move leaves piles");
    CHECK(sp_moves == 0, "failed move not counted");
    CHECK(sp_try_move(1, 5), "any card onto empty column ok");
    CHECK(sp_cnt[1] == 0 && sp_cnt[5] == 1, "4 moved onto empty column");
    CHECK(sp_moves == 1, "move counted");
    CHECK(!sp_try_move(0, 0), "self move rejected");
    CHECK(!sp_try_move(0, 5), "K onto 4 rejected again");
    set_col(2, t1, 1);                              /* col2: 2 */
    CHECK(!sp_try_move(2, 5), "2 onto 4 (skip rank) rejected");
    CHECK(!sp_try_move(2, 0), "2 onto K rejected");
    set_col(3, th1, 1);                             /* col3: 3 */
    CHECK(sp_try_move(2, 3), "2 onto 3 ok (decrease by 1)");
    CHECK(sp_cnt[2] == 0 && sp_cnt[3] == 2, "2 stacked on 3");
    CHECK(!sp_try_move(2, 3), "empty source rejected");
    CHECK(sp_moves == 2, "move counter counts 2");
}

/* ---- 完整 K..A 段自动清除 ---- */
static void test_runs(void) {
    clear_all();
    int ka[12], a1[1] = { 1 }, f1[1] = { 5 };
    for (int i = 0; i < 12; i++) ka[i] = 13 - i;          /* K..2 */
    set_col(0, ka, 12);
    set_col(1, a1, 1);
    set_col(2, f1, 1);                                    /* 留一张避免误判 WIN */
    CHECK(sp_try_move(1, 0), "A onto 2 ok");
    CHECK(sp_cnt[0] == 0 && sp_cnt[1] == 0, "13-card K..A run auto-cleared");
    CHECK(sp_runs == 1, "runs counter incremented");
    CHECK(!sp_over, "not win yet (col2 remains)");
    /* 非完整段不清理 */
    clear_all();
    int bad[13] = { 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 3 };  /* 倒数第 2 张错 */
    set_col(0, bad, 13);
    sp_collect_runs();
    CHECK(sp_cnt[0] == 13 && sp_runs == 0, "broken run not cleared");
    /* 完整段在列底(顶 13 张不完整)不清理 */
    clear_all();
    int below[15] = { 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 9, 7 };
    set_col(0, below, 15);
    sp_collect_runs();
    CHECK(sp_cnt[0] == 15 && sp_runs == 0, "run below top not cleared");
}

/* ---- 补发一列(每列 +1) ---- */
static void test_deal_op(void) {
    clear_all();
    for (int c = 0; c < SP_NCOL; c++) { sp_cnt[c] = 1; sp_col[c][0] = (uint8_t)(c + 1); }
    sp_stock_n = 20;
    for (int i = 0; i < 20; i++) sp_stock[i] = (uint8_t)(100 - i);
    sp_deal();
    CHECK(sp_stock_n == 10, "deal consumes 10 from stock");
    CHECK(sp_deals == 1, "deal counter");
    for (int c = 0; c < SP_NCOL; c++) CHECK(sp_cnt[c] == 2, "every column +1");
    sp_deal();
    CHECK(sp_stock_n == 0 && sp_deals == 2, "second deal");
    sp_deal();
    CHECK(sp_stock_n == 0 && sp_deals == 2, "no deal when stock < 10");
    CHECK(!sp_over, "not stuck/over after two deals");
    /* 补发完成 K..A 段: col0 已有 K..2, 取牌顺序 col0 先拿 sp_stock[9] = A */
    clear_all();
    int ka[12];
    for (int i = 0; i < 12; i++) ka[i] = 13 - i;
    set_col(0, ka, 12);
    sp_stock_n = 10;
    for (int i = 0; i < 10; i++) sp_stock[i] = (uint8_t)(i + 20);   /* rank 21..30 */
    sp_stock[9] = 0;                                                  /* A -> col0 */
    sp_deal();
    CHECK(sp_cnt[0] == 0 && sp_runs == 1, "deal completes K..A run");
    CHECK(!sp_over, "not win after one run");
}

/* ---- 胜利: 全部列清空 ---- */
static void test_win(void) {
    clear_all();
    int ka[12], a1[1] = { 1 };
    for (int i = 0; i < 12; i++) ka[i] = 13 - i;
    set_col(0, ka, 12);
    set_col(1, a1, 1);
    CHECK(sp_try_move(1, 0), "final move ok");
    CHECK(sp_over && !sp_stuck, "all columns empty -> WIN");
    CHECK(sp_runs == 1, "1 run collected on win");
}

/* ---- 死局检测(发牌堆空且无合法移动) ---- */
static void test_stuck(void) {
    clear_all();
    /* 10 列全非空且无相邻差 1: 顶集 {2,5,8,11,13} x2 */
    int r2a[1] = { 2 }, r5a[1] = { 5 }, r8a[1] = { 8 }, r11a[1] = { 11 }, r13a[1] = { 13 };
    set_col(0, r2a, 1);
    set_col(1, r5a, 1);
    set_col(2, r8a, 1);
    set_col(3, r11a, 1);
    set_col(4, r13a, 1);
    set_col(5, r2a, 1);
    set_col(6, r5a, 1);
    set_col(7, r8a, 1);
    set_col(8, r11a, 1);
    set_col(9, r13a, 1);
    sp_check_stuck();
    CHECK(sp_over && sp_stuck, "tops no adjacency no empties -> STUCK");
    /* 有空列则不算死局 */
    clear_all();
    set_col(0, r2a, 1);
    set_col(1, r5a, 1);
    sp_check_stuck();
    CHECK(!sp_over, "empty column available -> not stuck");
    /* 有相邻则不算死局 */
    clear_all();
    int r3a[1] = { 3 };
    set_col(0, r2a, 1);
    set_col(1, r3a, 1);
    set_col(2, r8a, 1);
    sp_check_stuck();
    CHECK(!sp_over, "adjacent tops 2/3 -> not stuck");
    /* 真实路径: 补发后发牌堆空且无棋 */
    clear_all();
    int seq[10] = { 1, 4, 7, 1, 4, 7, 1, 4, 7, 1 };   /* 取牌顺序 col9..col0 */
    sp_stock_n = 10;
    for (int i = 0; i < 10; i++) sp_stock[i] = (uint8_t)(seq[i] - 1);
    sp_deal();
    CHECK(sp_over && sp_stuck, "deal empties stock with no moves -> STUCK");
    CHECK(sp_runs == 0, "no runs on stuck deal");
}

/* ---- 光标/按键流程(公开 API) ---- */
static void test_keys(void) {
    key_event_t ev;
    /* 光标移动与钳位 */
    clear_all();
    sp_cur = 0;
    ev.key = K_LEFT; ev.ch = 0; ev.is_repeat = false;
    spider_on_key(&ev);
    CHECK(sp_cur == 0, "LEFT at col0 clamps");
    sp_cur = 9;
    ev.key = K_RIGHT;
    spider_on_key(&ev);
    CHECK(sp_cur == 9, "RIGHT at col9 clamps");
    ev.key = K_CHAR; ev.ch = 'a';
    spider_on_key(&ev);
    CHECK(sp_cur == 8, "A moves left");
    ev.key = K_CHAR; ev.ch = 'd';
    spider_on_key(&ev);
    CHECK(sp_cur == 9, "D moves right");
    /* 选中 -> 移动 -> 取消 */
    clear_all();
    int r2[1] = { 2 }, r3[1] = { 3 };
    set_col(0, r2, 1);
    set_col(1, r3, 1);
    sp_cur = 0;
    ev.key = K_OK;
    spider_on_key(&ev);
    CHECK(sp_sel == 0, "OK selects top card");
    ev.key = K_OK;
    spider_on_key(&ev);
    CHECK(sp_sel == -1, "OK same cell cancels selection");
    ev.key = K_OK;
    spider_on_key(&ev);
    CHECK(sp_sel == 0, "re-select");
    ev.key = K_RIGHT;
    spider_on_key(&ev);
    CHECK(sp_cur == 1, "cursor to col1");
    ev.key = K_OK;
    spider_on_key(&ev);
    CHECK(sp_sel == -1 && sp_cnt[0] == 0 && sp_cnt[1] == 2, "2 moved onto 3");
    /* 非法目标: 牌不动, 选中放下, 游戏不结束 */
    clear_all();
    int r5[1] = { 5 }, r8b[1] = { 8 };
    set_col(0, r2, 1);
    set_col(1, r5, 1);
    set_col(2, r8b, 1);
    sp_stock_n = 0;
    sp_cur = 0;
    ev.key = K_OK;
    spider_on_key(&ev);
    ev.key = K_RIGHT;
    spider_on_key(&ev);
    ev.key = K_OK;                                   /* 2 上 5 非法 */
    spider_on_key(&ev);
    CHECK(sp_cnt[0] == 1 && sp_cnt[1] == 1, "illegal move leaves piles");
    CHECK(sp_sel == -1, "selection dropped after failed move");
    CHECK(!sp_over, "failed move does not end game");
    /* 成功移动后死局: 移后顶集 {5,2,7,13,9,11,7,9,11,13} 无相邻差 1 且无空列 */
    clear_all();
    int c0[2] = { 5, 2 };                            /* col0: 底 5, 顶 2 */
    int c3a[1] = { 3 }, c7a[1] = { 7 }, c13a[1] = { 13 };
    int c9a[1] = { 9 }, c11a[1] = { 11 };
    set_col(0, c0, 2);
    set_col(1, c3a, 1);
    set_col(2, c7a, 1);
    set_col(3, c13a, 1);
    set_col(4, c9a, 1);
    set_col(5, c11a, 1);
    set_col(6, c7a, 1);
    set_col(7, c9a, 1);
    set_col(8, c11a, 1);
    set_col(9, c13a, 1);
    sp_stock_n = 0;
    sp_cur = 0;
    ev.key = K_OK;                                   /* 选中 2 */
    spider_on_key(&ev);
    ev.key = K_RIGHT;
    spider_on_key(&ev);
    ev.key = K_OK;                                   /* 2 上 3: 合法 */
    spider_on_key(&ev);
    CHECK(sp_cnt[0] == 1 && sp_cnt[1] == 2, "2 moved onto 3");
    CHECK(sp_over && sp_stuck, "successful move into deadlock -> STUCK");
    /* BACK 补发 */
    clear_all();
    sp_stock_n = 10;
    for (int i = 0; i < 10; i++) sp_stock[i] = (uint8_t)i;
    ev.key = K_BACK;
    spider_on_key(&ev);
    CHECK(sp_stock_n == 0 && sp_deals == 1, "BACK deals one row");
    for (int c = 0; c < SP_NCOL; c++) CHECK(sp_cnt[c] == 1, "each column +1 after BACK");
    /* 重复确认键忽略 */
    clear_all();
    ev.key = K_OK; ev.is_repeat = true;
    spider_on_key(&ev);
    CHECK(sp_sel == -1, "repeat OK ignored");
    /* N 新局 / Q 退出 */
    clear_all();
    sp_moves = 7;
    ev.is_repeat = false;
    ev.key = K_CHAR; ev.ch = 'n';
    spider_on_key(&ev);
    CHECK(sp_moves == 0 && sp_cnt[0] == 6, "N restarts a new game");
    s_exit_request = false;
    ev.key = K_QUIT;
    spider_on_key(&ev);
    CHECK(s_exit_request, "Q requests exit");
    /* 终局按键 */
    sp_over = true;
    s_exit_request = false;
    ev.key = K_BACK;
    spider_on_key(&ev);
    CHECK(s_exit_request, "over: BACK quits");
    sp_over = true;
    ev.key = K_OK;
    spider_on_key(&ev);
    CHECK(!sp_over, "over: OK restarts");
    sp_over = true;
    ev.key = K_OK; ev.is_repeat = true;
    spider_on_key(&ev);
    CHECK(sp_over, "over: repeat OK ignored");
}

int main(void) {
    test_deal();
    test_rank();
    test_move();
    test_runs();
    test_deal_op();
    test_win();
    test_stuck();
    test_keys();
    if (s_fail) { printf("TOTAL FAIL: %d\n", s_fail); return 1; }
    printf("ALL PASS\n");
    return 0;
}
