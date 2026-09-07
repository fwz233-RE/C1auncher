/* POKER DRAW 逻辑测试 — host 编译运行; 含 pokerdraw.c 直访静态 */
#include <stdio.h>
#include <string.h>

/* CHICHU_HOST 由命令行 -D 提供 */
#include "../../src/games/pokerdraw.c"

static int s_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); s_fail++; } \
    else { printf("ok: %s\n", msg); } \
} while (0)

/* host stub */
bool s_exit_request = false;
void game_set_tick_interval(uint32_t ms) { (void)ms; }

static void key(ccg_key k, uint8_t ch, bool rep) {
    key_event_t ev;
    ev.key = k; ev.ch = ch; ev.is_repeat = rep;
    pokerdraw_on_key(&ev);
}

/* 已知牌型手牌 (id = 花色*13 + 点数) */
static const int8_t H_SF[5]      = { 8, 9, 10, 11, 12 };   /* 同花顺 9-K */
static const int8_t H_FOUR[5]    = { 6, 19, 32, 45, 1 };   /* 四条 7 + 2 */
static const int8_t H_FULL[5]    = { 8, 21, 34, 4, 17 };   /* 葫芦 10-5 */
static const int8_t H_FLUSH[5]   = { 15, 18, 21, 24, 25 }; /* 同花(杂顺) */
static const int8_t H_STR[5]     = { 1, 15, 29, 4, 5 };    /* 顺子 2-6 */
static const int8_t H_THREE[5]   = { 9, 22, 35, 3, 1 };    /* 三条 10 */
static const int8_t H_2P[5]      = { 10, 23, 4, 17, 7 };   /* 两对 J-5 */
static const int8_t H_PAIR[5]    = { 11, 24, 3, 8, 2 };    /* 一对 Q */
static const int8_t H_HIGH[5]    = { 12, 23, 7, 30, 41 };  /* 高牌 K */

/* ---- 牌型判定 ---- */
static void test_types(void) {
    CHECK(pd_hand_type(H_SF) == PD_SF, "straight flush detected");
    CHECK(pd_hand_type(H_FOUR) == PD_FOUR, "four of a kind detected");
    CHECK(pd_hand_type(H_FULL) == PD_FULL, "full house detected");
    CHECK(pd_hand_type(H_FLUSH) == PD_FLUSH, "flush detected");
    CHECK(pd_hand_type(H_STR) == PD_STRAIGHT, "straight detected");
    CHECK(pd_hand_type(H_THREE) == PD_THREE, "three of a kind detected");
    CHECK(pd_hand_type(H_2P) == PD_TWOPAIR, "two pair detected");
    CHECK(pd_hand_type(H_PAIR) == PD_PAIR, "one pair detected");
    CHECK(pd_hand_type(H_HIGH) == PD_HIGH, "high card detected");

    const int8_t wheel[5] = { 0, 14, 28, 3, 4 };   /* A2345 混花色 */
    CHECK(pd_hand_type(wheel) == PD_STRAIGHT, "A2345 is a (wheel) straight");
    const int8_t broadway[5] = { 12, 24, 36, 9, 13 }; /* KQJ10A 不是顺 */
    CHECK(pd_hand_type(broadway) == PD_HIGH, "KQJ10A is NOT a straight");
}

/* ---- 大小排序 ---- */
static void test_order(void) {
    const int8_t *chain[9] = { H_SF, H_FOUR, H_FULL, H_FLUSH,
                               H_STR, H_THREE, H_2P, H_PAIR, H_HIGH };
    for (int i = 0; i < 8; i++)
        CHECK(pd_compare(chain[i], chain[i + 1]) > 0,
              "hand rank chain strictly ordered");
    CHECK(pd_compare(H_SF, H_SF) == 0, "identical hands tie");

    /* 两对: 高对 10 胜 9 */
    const int8_t p2lo[5] = { 9, 22, 4, 17, 7 };
    CHECK(pd_compare(H_2P, p2lo) > 0, "higher top pair wins two-pair");
    /* 一对: 相同对 + 高踢脚胜 */
    const int8_t pair_k1[5] = { 11, 24, 3, 8, 1 };
    CHECK(pd_compare(H_PAIR, pair_k1) > 0, "higher kicker wins pair");
    /* 同花: 高踢脚胜 */
    const int8_t flush_lo[5] = { 25, 24, 21, 18, 14 };
    CHECK(pd_compare(H_FLUSH, flush_lo) > 0, "higher kicker wins flush");
    /* 顺子: 高张胜 */
    const int8_t str_low[5] = { 0, 14, 28, 3, 4 };
    CHECK(pd_compare(H_STR, str_low) > 0, "higher straight wins");
    /* 同点数不同花色平手 */
    const int8_t p2_s2[5] = { 10, 36, 4, 30, 7 };
    CHECK(pd_compare(H_2P, p2_s2) == 0, "same ranks, diff suits tie");
}

/* ---- 发牌/换牌 ---- */
static void test_deal(void) {
    rng_seed(&pd_rng, 42);
    pd_new_game();
    CHECK(pd_phase == PD_PH_SELECT && pd_ndisc == 0 && pd_cur == 0,
          "new game starts in select phase");
    int seen[52];
    for (int i = 0; i < 52; i++) seen[i] = 0;
    for (int w = 0; w < 2; w++)
        for (int i = 0; i < 5; i++) {
            int c = pd_hand[w][i];
            CHECK(c >= 0 && c < 52, "card id in range");
            seen[c]++;
        }
    int dup = 0;
    for (int i = 0; i < 52; i++) if (seen[i] > 1) dup++;
    CHECK(dup == 0, "10 dealt cards all distinct");
}

/* ---- 玩家标记与光标 ---- */
static void test_mark(void) {
    pd_new_game();
    /* 光标循环 0..5 */
    key(K_LEFT, 0, false);
    CHECK(pd_cur == 5, "left wraps to DRAW button");
    key(K_RIGHT, 0, false);
    CHECK(pd_cur == 0, "right wraps back to card 0");
    key(K_CHAR, 'd', false);
    CHECK(pd_cur == 1, "d moves right");
    key(K_CHAR, 'a', false);
    CHECK(pd_cur == 0, "a moves left");
    /* OK 标记/取消 */
    key(K_OK, 0, false);
    CHECK(pd_disc[0] && pd_ndisc == 1, "OK marks card");
    key(K_OK, 0, false);
    CHECK(!pd_disc[0] && pd_ndisc == 0, "OK unmarks card");
    /* 确认键长按重复忽略 */
    key(K_OK, 0, true);
    CHECK(pd_ndisc == 0, "repeated OK ignored");
    /* 最多 3 张 */
    key(K_OK, 0, false);
    key(K_RIGHT, 0, false);
    key(K_OK, 0, false);
    key(K_RIGHT, 0, false);
    key(K_OK, 0, false);
    key(K_RIGHT, 0, false);
    key(K_OK, 0, false);
    CHECK(pd_ndisc == 3, "4th mark refused (max 3)");
    CHECK(!pd_disc[3], "card 4 stays unmarked");
    key(K_OK, 0, false);               /* 在已标记的 3 号位上再按 OK 取消 */
    CHECK(pd_ndisc == 3, "marked card unmark only via its own slot");
    key(K_LEFT, 0, false);             /* 回到已标记的 2 号位 */
    key(K_OK, 0, false);
    CHECK(pd_ndisc == 2 && !pd_disc[2], "unmark frees a slot");
}

/* ---- 玩家换牌确定性与阶段流转 ---- */
static void test_draw_flow(void) {
    for (int i = 0; i < 52; i++) pd_deck[i] = (int8_t)i;
    pd_pos = 0;
    pd_phase = PD_PH_SELECT;
    pd_ndisc = 0;
    pd_cur = 0;
    for (int i = 0; i < 5; i++) pd_disc[i] = false;
    pd_deal();                          /* 玩家 {0,2,4,6,8}, AI {1,3,5,7,9} */
    CHECK(pd_hand[0][0] == 0 && pd_hand[0][1] == 2 && pd_hand[0][4] == 8,
          "controlled deal player hand");
    pd_disc[0] = pd_disc[1] = pd_disc[4] = true;
    pd_ndisc = 3;
    pd_cur = 5;
    key(K_OK, 0, false);                /* 光标在 DRAW 上确认 */
    CHECK(pd_phase == PD_PH_AIDRAW, "OK on DRAW confirms exchange");
    CHECK(pd_hand[0][0] == 10 && pd_hand[0][1] == 11 && pd_hand[0][2] == 4 &&
          pd_hand[0][3] == 6 && pd_hand[0][4] == 12,
          "player cards replaced from deck");
    /* tick 驱动 AI 换牌 -> 比牌 */
    pokerdraw_tick(pd_ai_time + 1000);
    CHECK(pd_phase == PD_PH_SHOW, "tick advances to showdown");
    CHECK(pd_result == 1 || pd_result == 0 || pd_result == -1,
          "result is valid win/lose/tie");
}

/* ---- AI 换牌策略 ---- */
static void test_ai_discard(void) {
    /* 对子保留, 单牌全换 */
    pd_hand[1][0] = 6; pd_hand[1][1] = 19; pd_hand[1][2] = 12;
    pd_hand[1][3] = 10; pd_hand[1][4] = 4;              /* 对 7 + K,J,5 */
    pd_deck[10] = 0; pd_deck[11] = 1; pd_deck[12] = 2;
    pd_pos = 10;
    pd_phase = PD_PH_AIDRAW;
    pd_ai_time = 0;
    pokerdraw_tick(1000);
    CHECK(pd_hand[1][0] == 6 && pd_hand[1][1] == 19 &&
          pd_hand[1][2] == 0 && pd_hand[1][3] == 1 && pd_hand[1][4] == 2,
          "AI keeps pair, discards all singles");
    /* 全单牌: 保留最高 2 张, 换 3 张低牌 */
    pd_hand[1][0] = 12; pd_hand[1][1] = 10; pd_hand[1][2] = 8;
    pd_hand[1][3] = 5; pd_hand[1][4] = 2;               /* K,J,9,6,3 */
    pd_deck[10] = 0; pd_deck[11] = 1; pd_deck[12] = 2;
    pd_pos = 10;
    pd_phase = PD_PH_AIDRAW;
    pd_ai_time = 0;
    pokerdraw_tick(1000);
    CHECK(pd_hand[1][0] == 12 && pd_hand[1][1] == 10 &&
          pd_hand[1][2] == 0 && pd_hand[1][3] == 1 && pd_hand[1][4] == 2,
          "AI keeps 2 highest singles when 5 high cards");
    /* 葫芦不换 */
    pd_hand[1][0] = 8; pd_hand[1][1] = 21; pd_hand[1][2] = 34;
    pd_hand[1][3] = 4; pd_hand[1][4] = 17;
    pd_pos = 52;                        /* 无牌可抽也不该触发 */
    pd_phase = PD_PH_AIDRAW;
    pd_ai_time = 0;
    pokerdraw_tick(1000);
    CHECK(pd_hand[1][0] == 8 && pd_hand[1][1] == 21 && pd_hand[1][2] == 34 &&
          pd_hand[1][3] == 4 && pd_hand[1][4] == 17,
          "AI full house draws nothing");
}

/* ---- 计分 ---- */
static void test_score(void) {
    pd_win = 0;
    pd_loss = 0;
    for (int i = 0; i < 5; i++) {
        pd_hand[0][i] = H_FOUR[i];
        pd_hand[1][i] = H_PAIR[i];
    }
    pd_phase = PD_PH_AIDRAW;
    pd_ai_time = 0;
    for (int i = 0; i < 52; i++) pd_deck[i] = (int8_t)i;
    pd_pos = 10;
    pokerdraw_tick(1000);
    CHECK(pd_result == 1 && pd_win == 1 && pd_loss == 0,
          "four-of-kind beats pair -> win counted");
    for (int i = 0; i < 5; i++) {
        pd_hand[0][i] = H_HIGH[i];
        pd_hand[1][i] = H_FULL[i];
    }
    pd_phase = PD_PH_AIDRAW;
    pd_ai_time = 0;
    pokerdraw_tick(1000);
    CHECK(pd_result == -1 && pd_win == 1 && pd_loss == 1,
          "high card loses to full house -> loss counted");
}

/* ---- 比牌后按键 ---- */
static void test_show_keys(void) {
    pd_phase = PD_PH_SHOW;
    s_exit_request = false;
    key(K_BACK, 0, false);
    CHECK(s_exit_request, "BACK at showdown quits");
    s_exit_request = false;
    pd_phase = PD_PH_SHOW;
    pd_ndisc = 2;
    key(K_OK, 0, false);
    CHECK(pd_phase == PD_PH_SELECT && pd_ndisc == 0,
          "OK at showdown starts new round");
    pd_ndisc = 2;
    key(K_CHAR, 'n', false);
    CHECK(pd_phase == PD_PH_SELECT && pd_ndisc == 0,
          "N at showdown starts new round");
    key(K_QUIT, 0, false);
    CHECK(s_exit_request, "Q quits at any phase");
}

/* ---- 渲染冒烟 ---- */
static void test_render(void) {
    pd_new_game();
    pd_over_full = false;
    fb_clear(false);
    pokerdraw_render();
    int black = 0;
    for (unsigned i = 0; i < CCG_FRAME_BYTES; i++)
        if (g_fb[i]) black++;
    CHECK(black > 0, "select phase renders pixels");
    pd_phase = PD_PH_AIDRAW;
    pd_ai_time = 0;
    fb_clear(false);
    pokerdraw_render();
    CHECK(!pd_over_full, "AI draw phase: no over full-refresh");
    pd_phase = PD_PH_SHOW;
    pd_result = 1;
    pd_over_full = false;
    fb_clear(false);
    pokerdraw_render();
    CHECK(pd_over_full, "showdown triggers one force-full");
    fb_clear(false);
    pokerdraw_render();
    black = 0;
    for (unsigned i = 0; i < CCG_FRAME_BYTES; i++)
        if (g_fb[i]) black++;
    CHECK(black > 0, "showdown renders pixels");
}

int main(void) {
    test_types();
    test_order();
    test_deal();
    test_mark();
    test_draw_flow();
    test_ai_discard();
    test_score();
    test_show_keys();
    test_render();
    if (s_fail) { printf("\n%d FAILURES\n", s_fail); return 1; }
    printf("\nALL TESTS PASSED\n");
    return 0;
}
