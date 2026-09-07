/* BLACKJACK 逻辑测试 — host 编译运行; 含 blackjack.c 直访静态 */
#include <stdio.h>
#include <string.h>

/* CHICHU_HOST 由命令行 -D 提供 */
#include "../../src/games/blackjack.c"

static int s_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); s_fail++; } \
    else { printf("ok: %s\n", msg); } \
} while (0)

/* host stub */
bool s_exit_request = false;
void game_set_tick_interval(uint32_t ms) { (void)ms; }

static bool px(int x, int y) {
    return (g_fb[(y >> 3) * CCG_W + x] & (0x80u >> (y & 7))) != 0;
}

static void reset_state(void) {
    s_exit_request = false;
    bj_win = 0; bj_loss = 0; bj_ties = 0;
    bj_pos = 0; bj_phase = BJ_PH_PLAY; bj_hole = true; bj_res = BJ_RES_PUSH;
}

/* 控制发牌: 前 4 张按序进两家 */
static void seed_deck4(int a, int b, int c, int d) {
    bj_deck[0] = (int8_t)a; bj_deck[1] = (int8_t)b;
    bj_deck[2] = (int8_t)c; bj_deck[3] = (int8_t)d;
    bj_pos = 0;
}

static void key(ccg_key k, uint8_t ch, bool rep) {
    key_event_t ev;
    ev.key = k; ev.ch = ch; ev.is_repeat = rep;
    blackjack_on_key(&ev);
}

/* ---- 牌值: A=11, 2-9 面值, 10/J/Q/K=10 ---- */
static void test_values(void) {
    CHECK(bj_rank_value(0) == 11, "A = 11");
    CHECK(bj_rank_value(1) == 2 && bj_rank_value(8) == 9, "2-9 face value");
    CHECK(bj_rank_value(9) == 10 && bj_rank_value(10) == 10 &&
          bj_rank_value(11) == 10 && bj_rank_value(12) == 10,
          "10/J/Q/K = 10");
}

/* ---- 手牌合计与软 A ---- */
static void test_total(void) {
    int8_t h[8];
    h[0] = 0; h[1] = 0;                     /* A A */
    CHECK(bj_total(h, 2) == 12, "A+A = 12 (one soft)");
    h[2] = 8;                               /* +9♠ (值 9) */
    CHECK(bj_total(h, 3) == 21, "A+A+9 = 21 (soft)");
    h[3] = 12;                              /* +K */
    CHECK(bj_total(h, 4) == 21, "A+A+9+K = 21 (soft after 22)");
    int8_t s[5] = { 0, 0, 0, 8, 8 };        /* A A A 9 9 */
    CHECK(bj_total(s, 5) == 21, "A+A+A+9+9 = 21 (all soft)");
    int8_t g[2] = { 0, 12 };
    CHECK(bj_total(g, 2) == 21, "A+K = 21");
    int8_t f[2] = { 12, 12 };
    CHECK(bj_total(f, 2) == 20, "K+K = 20");
    int8_t e[2] = { 9, 9 };
    CHECK(bj_total(e, 2) == 20, "10+10 = 20");
}

/* ---- 洗牌: 52 张各一次(置换) ---- */
static void test_shuffle(void) {
    rng_seed(&bj_rng, 12345);
    bj_shuffle();
    int seen[52] = {0};
    for (int i = 0; i < 52; i++) {
        int c = bj_deck[i];
        if (c < 0 || c > 51) { CHECK(0, "card id in range"); return; }
        seen[c]++;
    }
    int ok = 1;
    for (int i = 0; i < 52; i++) if (seen[i] != 1) ok = 0;
    CHECK(ok, "shuffle is a permutation of 52");
    CHECK(bj_pos == 0, "shuffle resets pos");
}

/* ---- 发牌: 52 张内无重复, 用尽自动重洗 ---- */
static void test_draw_cycle(void) {
    rng_seed(&bj_rng, 999);
    bj_shuffle();
    int seen[52] = {0};
    for (int i = 0; i < 52; i++) seen[bj_draw()]++;
    int ok = 1;
    for (int i = 0; i < 52; i++) if (seen[i] != 1) ok = 0;
    CHECK(ok, "52 draws give each card once");
    CHECK(bj_pos == 52, "pos exhausted");
    int c = bj_draw();                      /* 触发重洗 */
    CHECK(c >= 0 && c <= 51, "draw past end reshuffles");
    CHECK(bj_pos == 1, "reshuffle restarts pos");
}

/* ---- 自然 21: 玩家黑杰克 ---- */
static void test_natural_bj(void) {
    reset_state();
    seed_deck4(0, 6, 25, 1);                /* 玩家 A♠ K♥, 庄家 7♠ 2♠ */
    bj_deal();
    CHECK(bj_phase == BJ_PH_OVER, "player BJ ends round");
    CHECK(bj_res == BJ_RES_BJ, "player BJ result");
    CHECK(bj_win == 1 && bj_loss == 0, "BJ counts win");
    CHECK(!bj_hole, "hole revealed on BJ");
}

/* ---- 自然 21: 庄家黑杰克 ---- */
static void test_dealer_bj(void) {
    reset_state();
    seed_deck4(5, 0, 2, 25);                /* 庄家 A♠ K♥ */
    bj_deal();
    CHECK(bj_phase == BJ_PH_OVER && bj_res == BJ_RES_DBJ, "dealer BJ result");
    CHECK(bj_loss == 1, "dealer BJ counts loss");
}

/* ---- 自然 21: 双黑杰克 = 平局 ---- */
static void test_double_bj(void) {
    reset_state();
    seed_deck4(0, 25, 25, 0);               /* 两家 A+K */
    bj_deal();
    CHECK(bj_phase == BJ_PH_OVER && bj_res == BJ_RES_PUSH, "double BJ = push");
    CHECK(bj_ties == 1 && bj_win == 0 && bj_loss == 0, "push counts tie");
}

/* ---- 玩家爆牌: HIT 到 22+ 立即结算 ---- */
static void test_player_bust(void) {
    reset_state();
    bj_n[0] = 2; bj_n[1] = 2;
    bj_hand[0][0] = 5; bj_hand[0][1] = 6;   /* 6+7 = 13 */
    bj_hand[1][0] = 6; bj_hand[1][1] = 7;
    bj_phase = BJ_PH_PLAY;
    bj_deck[0] = 9; bj_pos = 0;             /* 下一张 10♠ */
    bj_hit();
    CHECK(bj_n[0] == 3 && bj_total(bj_hand[0], 3) == 23, "hit drew to 23");
    CHECK(bj_phase == BJ_PH_OVER && bj_res == BJ_RES_PBUST, "bust ends round");
    CHECK(bj_loss == 1, "bust counts loss");
    /* 庄家不抽牌 */
    CHECK(bj_n[1] == 2, "dealer idle after player bust");
}

/* ---- 玩家 STAND: 庄家 17 停 ---- */
static void test_dealer_stands_17(void) {
    reset_state();
    bj_n[0] = 2; bj_n[1] = 2;
    bj_hand[0][0] = 12; bj_hand[0][1] = 12; /* 20 */
    bj_hand[1][0] = 6; bj_hand[1][1] = 12;  /* 7+10 = 17 */
    bj_phase = BJ_PH_PLAY;
    bj_stand();
    CHECK(bj_phase == BJ_PH_DEALER && !bj_hole, "stand reveals hole");
    blackjack_tick(0);
    CHECK(bj_n[1] == 2, "dealer at 17 does not draw");
    CHECK(bj_phase == BJ_PH_OVER && bj_res == BJ_RES_WIN, "20 beats 17");
    CHECK(bj_win == 1, "win counted");
    blackjack_tick(0);                      /* 结束后 tick 无副作用 */
    CHECK(bj_n[1] == 2 && bj_phase == BJ_PH_OVER, "post-over tick no-op");
}

/* ---- 庄家逐张抽到 17+ 再结算 ---- */
static void test_dealer_draws(void) {
    reset_state();
    bj_n[0] = 2; bj_n[1] = 2;
    bj_hand[0][0] = 12; bj_hand[0][1] = 12; /* 20 */
    bj_hand[1][0] = 5; bj_hand[1][1] = 6;   /* 6+7 = 13 */
    bj_phase = BJ_PH_DEALER;
    bj_deck[0] = 4; bj_pos = 0;             /* 下一张 5♠ → 13+5 = 18 */
    blackjack_tick(0);
    CHECK(bj_n[1] == 3 && bj_total(bj_hand[1], 3) == 18, "dealer drew to 18");
    CHECK(bj_phase == BJ_PH_OVER, "dealer >= 17 ends round");
    CHECK(bj_res == BJ_RES_WIN, "20 beats 18");
}

/* ---- 庄家爆牌: 玩家胜 ---- */
static void test_dealer_bust(void) {
    reset_state();
    bj_n[0] = 2; bj_n[1] = 2;
    bj_hand[0][0] = 12; bj_hand[0][1] = 7;  /* 10+8 = 18 */
    bj_hand[1][0] = 7; bj_hand[1][1] = 6;   /* 8+7 = 15 */
    bj_phase = BJ_PH_DEALER;
    bj_deck[0] = 9; bj_pos = 0;             /* 15+10 = 25 爆 */
    blackjack_tick(0);
    CHECK(bj_res == BJ_RES_DBUST && bj_win == 1, "dealer bust = player win");
    CHECK(bj_phase == BJ_PH_OVER, "bust ends round");
}

/* ---- 比点: 庄家 19 赢玩家 17 ---- */
static void test_compare_lose(void) {
    reset_state();
    bj_n[0] = 2; bj_n[1] = 2;
    bj_hand[0][0] = 8; bj_hand[0][1] = 7;   /* 17 */
    bj_hand[1][0] = 12; bj_hand[1][1] = 8;  /* 19 */
    bj_phase = BJ_PH_DEALER;
    bj_deck[0] = 4; bj_pos = 0;
    blackjack_tick(0);                      /* 庄家 19 已停 */
    CHECK(bj_phase == BJ_PH_OVER && bj_res == BJ_RES_LOSE, "19 beats 17");
    CHECK(bj_loss == 1, "loss counted");
}

/* ---- 平局比点 ---- */
static void test_compare_push(void) {
    reset_state();
    bj_n[0] = 2; bj_n[1] = 2;
    bj_hand[0][0] = 12; bj_hand[0][1] = 8;  /* 19 */
    bj_hand[1][0] = 9; bj_hand[1][1] = 8;   /* 19 */
    bj_phase = BJ_PH_DEALER;
    bj_deck[0] = 4; bj_pos = 0;
    blackjack_tick(0);
    CHECK(bj_res == BJ_RES_PUSH && bj_ties == 1, "equal totals = push");
}

/* ---- 按键: OK=HIT, BACK/SPACE=STAND, N=新局, 忽略 repeat ---- */
static void test_keys(void) {
    reset_state();
    bj_n[0] = 2; bj_n[1] = 2;
    bj_hand[0][0] = 7; bj_hand[0][1] = 6;   /* 13 */
    bj_hand[1][0] = 6; bj_hand[1][1] = 7;
    bj_phase = BJ_PH_PLAY;
    bj_deck[0] = 4; bj_pos = 0;             /* 下一张 5♠ */
    key(K_OK, 0, true);                     /* repeat 必须忽略 */
    CHECK(bj_n[0] == 2, "repeat OK ignored");
    key(K_OK, 0, false);
    CHECK(bj_n[0] == 3, "OK = HIT");
    key(K_BACK, 0, false);                  /* STAND 退回合 */
    CHECK(bj_phase == BJ_PH_DEALER, "BACK = STAND");
    bj_phase = BJ_PH_PLAY;
    key(K_SPACE, 0, false);
    CHECK(bj_phase == BJ_PH_DEALER, "SPACE = STAND");
    /* 'h' / 's' 字母快捷键 */
    bj_phase = BJ_PH_PLAY;
    bj_n[0] = 2;
    bj_deck[0] = 3; bj_pos = 0;
    key(K_CHAR, 'h', false);
    CHECK(bj_n[0] == 3, "h = HIT");
    key(K_CHAR, 's', false);
    CHECK(bj_phase == BJ_PH_DEALER, "s = STAND");
    /* 'n' 任意时刻重开 */
    seed_deck4(1, 6, 2, 7);                /* 玩家 2♠ 3♠, 庄家 7♠ 8♠(无 21) */
    bj_phase = BJ_PH_PLAY;
    key(K_CHAR, 'n', false);
    CHECK(bj_phase == BJ_PH_PLAY && bj_n[0] == 2 && bj_n[1] == 2,
          "n starts fresh round");
    /* 结算态: OK/N 重开, BACK 退出 */
    seed_deck4(1, 6, 2, 7);                /* 无 21, 避免隔离污染 */
    bj_phase = BJ_PH_OVER;
    bj_n[0] = 2; bj_n[1] = 2;
    key(K_OK, 0, false);
    CHECK(bj_phase == BJ_PH_PLAY, "over + OK = new round");
    bj_phase = BJ_PH_OVER;
    key(K_BACK, 0, false);
    CHECK(s_exit_request, "over + BACK = quit");
    /* Q 退出 */
    reset_state();
    key(K_QUIT, 0, false);
    CHECK(s_exit_request, "Q = quit");
    /* 玩家手牌上限保护 */
    reset_state();
    bj_n[0] = BJ_MAX_HAND;
    bj_phase = BJ_PH_PLAY;
    key(K_OK, 0, false);
    CHECK(bj_n[0] == BJ_MAX_HAND, "hit capped at MAX_HAND");
    /* 庄家回合中 HIT/STAND 无效 */
    bj_phase = BJ_PH_DEALER;
    bj_n[0] = BJ_MAX_HAND - 1;
    key(K_OK, 0, false);
    key(K_SPACE, 0, false);
    CHECK(bj_n[0] == BJ_MAX_HAND - 1 && bj_phase == BJ_PH_DEALER,
          "hit/stand ignored during dealer phase");
}

/* ---- 渲染冒烟: 牌面/牌背/合计像素 ---- */
static void test_render(void) {
    reset_state();
    bj_n[0] = 2; bj_n[1] = 2;
    bj_hand[0][0] = 0; bj_hand[0][1] = 25;  /* A♠ K♥ = 21 */
    bj_hand[1][0] = 7; bj_hand[1][1] = 6;   /* 7♠ 6♠ */
    bj_phase = BJ_PH_PLAY;
    bj_hole = true;
    blackjack_render();
    CHECK(px(6, 67), "player card border at (6,67)");
    CHECK(px(17, 69), "A rank pixel on player card 0");
    CHECK(px(17, 84), "spade suit pixel on player card 0");
    CHECK(px(6, 25), "dealer upcard border at (6,25)");
    /* 暗牌未揭: 庄家合计 '?' (2x) 左上角像素 */
    CHECK(px(66, 34), "hidden hole shows ? total");
    /* 揭牌后合计 '=' */
    bj_hole = false;
    bj_phase = BJ_PH_DEALER;
    blackjack_render();
    CHECK(!px(66, 34), "revealed hole shows = total");
    /* 结束态渲染不崩 */
    bj_phase = BJ_PH_OVER;
    bj_res = BJ_RES_WIN;
    blackjack_render();
    CHECK(px(6, 104), "status line drawn at (6,104)");
}

int main(void) {
    printf("== blackjack logic tests ==\n");
    test_values();
    test_total();
    test_shuffle();
    test_draw_cycle();
    test_natural_bj();
    test_dealer_bj();
    test_double_bj();
    test_player_bust();
    test_dealer_stands_17();
    test_dealer_draws();
    test_dealer_bust();
    test_compare_lose();
    test_compare_push();
    test_keys();
    test_render();
    if (s_fail) { printf("TOTAL FAIL: %d\n", s_fail); return 1; }
    printf("ALL PASS\n");
    return 0;
}
