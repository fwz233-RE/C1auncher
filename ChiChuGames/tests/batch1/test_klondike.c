/* KLONDIKE 逻辑单测 — host 编译, 包含游戏 .c 直接访问静态状态 */
#include "../../src/games/klondike.c"
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

/* 测试用牌: suit 0=H 1=D 2=S 3=C, rank 1=A..13=K */
static int ck(int suit, int rank) { return suit * 13 + (rank - 1); }

static void clear_tab(void) {
    memset(kl_tab_n, 0, sizeof(kl_tab_n));
    memset(kl_tab_fd, 0, sizeof(kl_tab_fd));
}

/* ---- 发牌: 列大小/扣牌数/全牌唯一 ---- */
static void test_deal(void) {
    rng_seed(&kl_rng, 42);
    kl_deal();
    int total = 0, c, k, cnt = 0, i;
    int seen[52] = { 0 };
    for (c = 0; c < 7; c++) {
        CHECK(kl_tab_n[c] == c + 1, "column i has i+1 cards");
        CHECK(kl_tab_fd[c] == c, "column i has i face-down");
        total += kl_tab_n[c];
        for (k = 0; k < kl_tab_n[c]; k++) seen[kl_tab[c][k]] = 1;
    }
    CHECK(total == 28, "28 cards dealt to tableau");
    CHECK(kl_deck_n == 24, "24 cards left in deck");
    for (i = 0; i < kl_deck_n; i++) seen[kl_deck[i]] = 1;
    for (i = 0; i < 52; i++) cnt += seen[i];
    CHECK(cnt == 52, "all 52 cards present exactly once");
}

/* ---- 落牌规则 ---- */
static void test_can_give(void) {
    kl_reset_state();
    clear_tab();
    CHECK(kl_can_give(ck(KL_SUIT_HEART, 1), -1, 0), "empty found takes A");
    CHECK(!kl_can_give(ck(KL_SUIT_HEART, 2), -1, 0), "empty found rejects 2");
    CHECK(!kl_can_give(ck(KL_SUIT_SPADE, 1), -1, 1), "found rejects wrong suit");
    kl_found_n[0] = 5;
    kl_found[0][4] = (uint8_t)ck(KL_SUIT_HEART, 5);
    CHECK(kl_can_give(ck(KL_SUIT_HEART, 6), -1, 0), "found accepts same suit n+1");
    CHECK(!kl_can_give(ck(KL_SUIT_HEART, 7), -1, 0), "found rejects rank skip");
    CHECK(!kl_can_give(ck(KL_SUIT_DIAMOND, 6), -1, 0), "found rejects other suit");
    kl_reset_state();
    clear_tab();
    CHECK(kl_can_give(ck(KL_SUIT_CLUB, 13), 3, -1), "empty col takes K");
    CHECK(!kl_can_give(ck(KL_SUIT_CLUB, 12), 3, -1), "empty col rejects Q");
    kl_tab_n[4] = 1;
    kl_tab[4][0] = (uint8_t)ck(KL_SUIT_SPADE, 9);
    CHECK(kl_can_give(ck(KL_SUIT_HEART, 8), 4, -1), "red 8 on black 9 ok");
    CHECK(!kl_can_give(ck(KL_SUIT_CLUB, 8), 4, -1), "same color rejected");
    CHECK(!kl_can_give(ck(KL_SUIT_HEART, 7), 4, -1), "wrong rank rejected");
    CHECK(!kl_can_give(ck(KL_SUIT_HEART, 8), -1, -1), "deck/waste dest rejected");
}

/* ---- 移动执行与状态转移 ---- */
static void test_move(void) {
    kl_reset_state();
    clear_tab();
    kl_waste_n = 1;
    kl_waste[0] = (uint8_t)ck(KL_SUIT_DIAMOND, 1);
    CHECK(kl_move(KL_SRC_WASTE, -1, 1), "waste A to foundation");
    CHECK(kl_found_n[1] == 1 &&
          kl_found[1][0] == (uint8_t)ck(KL_SUIT_DIAMOND, 1), "foundation holds A");
    CHECK(kl_waste_n == 0, "waste consumed");
    CHECK(!kl_over, "not over yet");
    kl_tab_n[0] = 1; kl_tab[0][0] = (uint8_t)ck(KL_SUIT_SPADE, 2);
    kl_tab_n[1] = 1; kl_tab[1][0] = (uint8_t)ck(KL_SUIT_HEART, 3);
    CHECK(kl_move(0, 1, -1), "2S onto 3H");
    CHECK(kl_tab_n[0] == 0, "source column emptied");
    CHECK(kl_tab_n[1] == 2 && kl_tab[1][1] == (uint8_t)ck(KL_SUIT_SPADE, 2),
          "2S stacked on 3H");
    CHECK(!kl_move(1, 1, -1), "self move rejected");
    kl_tab_n[2] = 1; kl_tab[2][0] = (uint8_t)ck(KL_SUIT_CLUB, 13);
    CHECK(kl_move(2, 0, -1), "K to empty column");
    int before = kl_tab_n[0];
    CHECK(!kl_move(0, -1, -1), "invalid dest rejected");
    CHECK(kl_tab_n[0] == before, "no mutation on rejected move");
    CHECK(kl_moves == 3, "move counter counts 3");
}

/* ---- 移走顶牌后扣牌翻开 ---- */
static void test_fd_flip(void) {
    kl_reset_state();
    clear_tab();
    kl_tab_n[0] = 3; kl_tab_fd[0] = 2;
    kl_tab[0][0] = (uint8_t)ck(KL_SUIT_CLUB, 5);
    kl_tab[0][1] = (uint8_t)ck(KL_SUIT_SPADE, 6);
    kl_tab[0][2] = (uint8_t)ck(KL_SUIT_HEART, 7);
    kl_tab_n[1] = 1; kl_tab[1][0] = (uint8_t)ck(KL_SUIT_SPADE, 8);
    CHECK(kl_move(0, 1, -1), "move 7H onto 8S");
    CHECK(kl_tab_n[0] == 2 && kl_tab_fd[0] == 1, "next face-down flipped up");
    kl_tab_n[2] = 1; kl_tab[2][0] = (uint8_t)ck(KL_SUIT_HEART, 7);
    CHECK(kl_move(0, 2, -1), "move 6S onto 7H");
    CHECK(kl_tab_n[0] == 1 && kl_tab_fd[0] == 0, "last card flipped up");
    CHECK(kl_tab[0][0] == (uint8_t)ck(KL_SUIT_CLUB, 5), "bottom card kept");
}

/* ---- 发牌堆翻牌与回收 ---- */
static void test_flip(void) {
    kl_reset_state();
    kl_deck_n = 3;
    kl_deck[0] = 1; kl_deck[1] = 2; kl_deck[2] = 3;
    kl_flip();
    CHECK(kl_waste_n == 1 && kl_waste[0] == 3, "flip moves deck top to waste");
    kl_flip();
    CHECK(kl_waste_n == 2 && kl_waste[1] == 2, "second flip");
    kl_flip();
    CHECK(kl_deck_n == 0 && kl_waste_n == 3, "deck exhausted");
    kl_flip();
    CHECK(kl_deck_n == 3 && kl_deck[0] == 1 && kl_deck[2] == 3 && kl_waste_n == 0,
          "recycle reverses waste into deck");
    kl_deck_n = 0; kl_waste_n = 0;
    kl_flip();
    CHECK(kl_deck_n == 0 && kl_waste_n == 0, "flip on both empty is no-op");
}

/* ---- 胜利判定 ---- */
static void test_win(void) {
    kl_reset_state();
    clear_tab();
    kl_found_n[0] = kl_found_n[1] = kl_found_n[2] = 13;
    kl_found_n[3] = 12;
    kl_waste_n = 1;
    kl_waste[0] = (uint8_t)ck(KL_SUIT_CLUB, 13);
    CHECK(kl_move(KL_SRC_WASTE, -1, 3), "last club K to foundation");
    CHECK(kl_over, "all 52 in foundations -> WIN");
}

/* ---- 光标导航与源/目的解析 ---- */
static void test_cursor(void) {
    key_event_t ev;
    int dcol, dfound;
    kl_reset_state();
    clear_tab();
    kl_row = 1; kl_col = 6;
    ev.key = K_UP; ev.ch = 0; ev.is_repeat = false;
    kl_cursor_move(&ev);
    CHECK(kl_row == 0 && kl_col == 5, "UP from col6 clamps to top col5");
    ev.key = K_LEFT;
    kl_cursor_move(&ev);
    CHECK(kl_col == 4, "LEFT moves on top row");
    ev.key = K_LEFT;
    kl_cursor_move(&ev);
    kl_cursor_move(&ev);
    kl_cursor_move(&ev);
    kl_cursor_move(&ev);
    kl_cursor_move(&ev);
    CHECK(kl_col == 5, "top row wraps 4 -> 3 -> 2 -> 1 -> 0 -> 5");
    ev.key = K_DOWN;
    kl_cursor_move(&ev);
    CHECK(kl_row == 1 && kl_col == 5, "DOWN back to tableau");
    ev.key = K_RIGHT;
    kl_cursor_move(&ev);
    CHECK(kl_col == 6, "tableau right from col5 -> col6");
    /* WASD */
    ev.key = K_CHAR; ev.ch = 'd';
    kl_cursor_move(&ev);
    CHECK(kl_col == 0, "D wraps 6 -> 0");
    ev.key = K_CHAR; ev.ch = 'w';
    kl_cursor_move(&ev);
    CHECK(kl_row == 0 && kl_col == 0, "W moves up to top row col0");
    /* 源解析 */
    kl_reset_state();
    clear_tab();
    CHECK(kl_src_at_cursor() == -1, "empty column is no source");
    kl_tab_n[0] = 1; kl_tab[0][0] = 1;
    CHECK(kl_src_at_cursor() == 0, "face-up top is a source");
    kl_tab_fd[0] = 1;
    CHECK(kl_src_at_cursor() == -1, "face-down top is no source");
    kl_tab_fd[0] = 0;
    kl_row = 0; kl_col = 1;
    CHECK(kl_src_at_cursor() == -1, "empty waste is no source");
    kl_waste_n = 1; kl_waste[0] = 2;
    CHECK(kl_src_at_cursor() == KL_SRC_WASTE, "waste top is a source");
    kl_row = 0; kl_col = 0;
    CHECK(kl_src_at_cursor() == KL_SRC_DECK, "deck resolves as flip source");
    /* 目的解析 */
    kl_row = 0; kl_col = 3;
    kl_dst_at_cursor(&dcol, &dfound);
    CHECK(dcol == -1 && dfound == 1, "top col3 -> foundation 1");
    kl_row = 0; kl_col = 0;
    kl_dst_at_cursor(&dcol, &dfound);
    CHECK(dcol == -1 && dfound == -1, "deck is not a dest");
    kl_row = 1; kl_col = 5;
    kl_dst_at_cursor(&dcol, &dfound);
    CHECK(dcol == 5 && dfound == -1, "tableau col5 is a dest");
}

/* ---- 按键流程(公开 API) ---- */
static void test_on_key(void) {
    key_event_t ev;
    kl_reset_state();
    clear_tab();
    kl_deck_n = 1; kl_deck[0] = 5;
    kl_row = 0; kl_col = 0;
    ev.key = K_OK; ev.ch = 0; ev.is_repeat = false;
    klondike_on_key(&ev);
    CHECK(kl_waste_n == 1 && kl_waste[0] == 5, "OK on deck flips a card");
    kl_deck_n = 1; kl_deck[0] = 6;
    ev.key = K_BACK;
    klondike_on_key(&ev);
    CHECK(kl_waste_n == 2 && kl_waste[1] == 6, "BACK flips a card");
    kl_deck_n = 1; kl_deck[0] = 7;
    ev.key = K_OK; ev.is_repeat = true;
    klondike_on_key(&ev);
    CHECK(kl_waste_n == 2, "repeat OK is ignored");
    ev.is_repeat = false;
    /* 选中 + 移动: 光标在表格 col0(2S 翻开), col1 顶 3H */
    kl_reset_state();
    clear_tab();
    kl_tab_n[0] = 1; kl_tab[0][0] = (uint8_t)ck(KL_SUIT_SPADE, 2);
    kl_tab_n[1] = 1; kl_tab[1][0] = (uint8_t)ck(KL_SUIT_HEART, 3);
    ev.key = K_OK;
    klondike_on_key(&ev);
    CHECK(kl_sel_src == 0, "OK selects column 0");
    ev.key = K_RIGHT;
    klondike_on_key(&ev);
    CHECK(kl_col == 1, "cursor moved to col1");
    ev.key = K_OK;
    klondike_on_key(&ev);
    CHECK(kl_sel_src == -1 && kl_tab_n[1] == 2, "OK on col1 moves 2S onto 3H");
    /* 同格再按: 取消选择 */
    ev.key = K_OK;                          /* 光标仍在 col1(顶 2S) */
    klondike_on_key(&ev);
    CHECK(kl_sel_src == 1, "OK selects col1 again");
    ev.key = K_OK;
    klondike_on_key(&ev);
    CHECK(kl_sel_src == -1, "OK on same cell cancels selection");
    /* 非法目标: 取消并改选新格 */
    kl_tab_n[2] = 1; kl_tab[2][0] = (uint8_t)ck(KL_SUIT_CLUB, 3);  /* 黑 3 */
    ev.key = K_OK;                          /* 选中 col1(顶 2S) */
    klondike_on_key(&ev);
    CHECK(kl_sel_src == 1, "selected 2S again");
    ev.key = K_RIGHT;
    klondike_on_key(&ev);
    ev.key = K_OK;                          /* 2S(黑) 不能上 3C(黑) */
    klondike_on_key(&ev);
    CHECK(kl_tab_n[2] == 1 && kl_tab_n[1] == 2, "invalid move leaves piles untouched");
    CHECK(kl_sel_src == 2, "after failed move, new cell (3C) gets selected");
    /* 废牌堆: 选中后同格再按取消 */
    kl_waste_n = 1; kl_waste[0] = (uint8_t)ck(KL_SUIT_SPADE, 9);
    kl_row = 0; kl_col = 1;
    ev.key = K_OK;
    klondike_on_key(&ev);
    CHECK(kl_sel_src == KL_SRC_WASTE, "OK selects waste top");
    ev.key = K_OK;
    klondike_on_key(&ev);
    CHECK(kl_sel_src == -1, "OK on waste again cancels selection");
    /* 废牌堆 → 目标堆: 选中废牌堆后移到目标堆 2 并 OK */
    kl_found_n[2] = 8; kl_found[2][7] = (uint8_t)ck(KL_SUIT_SPADE, 8);
    ev.key = K_OK;
    klondike_on_key(&ev);
    CHECK(kl_sel_src == KL_SRC_WASTE, "waste selected for move");
    ev.key = K_UP;
    klondike_on_key(&ev);
    CHECK(kl_row == 0 && kl_col == 1, "UP stays on waste slot");
    ev.key = K_RIGHT;
    klondike_on_key(&ev);
    CHECK(kl_col == 2, "RIGHT to foundation 0 slot");
    ev.key = K_RIGHT;
    klondike_on_key(&ev);
    CHECK(kl_col == 3, "RIGHT to foundation 1 slot");
    ev.key = K_RIGHT;
    klondike_on_key(&ev);
    CHECK(kl_col == 4, "RIGHT to foundation 2 slot");
    ev.key = K_OK;
    klondike_on_key(&ev);
    CHECK(kl_found_n[2] == 9 && kl_found[2][8] == (uint8_t)ck(KL_SUIT_SPADE, 9),
          "waste 9S onto spade foundation 9th");
    CHECK(kl_waste_n == 0 && kl_sel_src == -1, "waste consumed after move");
    /* Q 退出 */
    s_exit_request = false;
    ev.key = K_QUIT;
    klondike_on_key(&ev);
    CHECK(s_exit_request, "Q requests exit");
    /* 终局按键 */
    kl_over = true;
    s_exit_request = false;
    ev.key = K_BACK;
    klondike_on_key(&ev);
    CHECK(s_exit_request, "over: BACK quits");
    kl_over = true;
    ev.key = K_OK;
    klondike_on_key(&ev);
    CHECK(!kl_over, "over: OK restarts");
}

int main(void) {
    test_deal();
    test_can_give();
    test_move();
    test_fd_flip();
    test_flip();
    test_win();
    test_cursor();
    test_on_key();
    if (s_fail) { printf("TOTAL FAIL: %d\n", s_fail); return 1; }
    printf("ALL PASS\n");
    return 0;
}
