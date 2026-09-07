/* LIAR'S DICE 逻辑单测 — host, 包含游戏源码 + 链接框架宿主桩 */
#include "../../src/config.h"
#include "../../src/gfx/canvas.h"
#include "../../src/gfx/font.h"
#include "../../src/platform/input.h"
#include "../../src/rng.h"
#include "../../src/games/liarsdice.c"
#include <stdio.h>
#include <string.h>

static int s_fail = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); s_fail++; } \
    else { printf("ok: %s\n", msg); } \
} while (0)

/* main.c 未链接, 补桩 */
bool s_exit_request = false;
void game_set_tick_interval(uint32_t ms) { (void)ms; }

/* ---- 像素断言辅助 ---- */
static bool px(int x, int y) {
    if (x < 0 || x >= (int)CCG_W || y < 0 || y >= (int)CCG_H) return false;
    int off = (y >> 3) * (int)CCG_W + x;
    return (g_fb[off] & (0x80 >> (y & 7))) != 0;
}

/* 行带 [x0,x1) 内是否有黑像素 */
static bool band_black(int y, int x0, int x1) {
    for (int x = x0; x < x1; x++)
        if (px(x, y)) return true;
    return false;
}

static void ld_key(ccg_key key, char ch) {
    key_event_t ev;
    ev.key = key;
    ev.ch = (uint8_t)ch;
    ev.is_repeat = false;
    liarsdice_on_key(&ev);
}

/* ---- 辅助: 摆定局面 ---- */
static void setup(int pn, int an, const int *pd, const int *ad,
                  int q, int f, int bidder) {
    ld_pn = pn;
    ld_an = an;
    for (int i = 0; i < 3; i++) {
        ld_pd[i] = (i < pn) ? pd[i] : 0;
        ld_ad[i] = (i < an) ? ad[i] : 0;
    }
    ld_cur_q = q;
    ld_cur_f = f;
    ld_bidder = bidder;
    ld_in_q = 0;
    ld_in_f = 0;
    ld_hint = LD_HINT_NONE;
}

/* ---- 1. 叫点合法性 ---- */
static void test_bid_ok(void) {
    setup(3, 3, (int[]){5,5,1}, (int[]){5,2,3}, 0, 0, 0);
    CHECK(ld_bid_ok(1, 1), "open bid 1 ONES valid");
    CHECK(ld_bid_ok(6, 6), "open bid 6 SIXES valid");
    CHECK(!ld_bid_ok(7, 1), "q over total rejected");
    CHECK(!ld_bid_ok(0, 3), "q=0 rejected");
    CHECK(!ld_bid_ok(3, 0), "f=0 rejected");
    ld_cur_q = 3; ld_cur_f = 5;
    CHECK(ld_bid_ok(4, 1), "higher qty any face valid");
    CHECK(ld_bid_ok(3, 6), "same qty higher face valid");
    CHECK(!ld_bid_ok(3, 5), "same bid rejected");
    CHECK(!ld_bid_ok(3, 4), "same qty lower face rejected");
    CHECK(!ld_bid_ok(2, 6), "lower qty rejected");
    ld_cur_q = 6; ld_cur_f = 6;
    CHECK(!ld_bid_ok(7, 1), "beyond max bid rejected");
    CHECK(!ld_bid_ok(6, 6), "max bid itself cannot repeat");
}

/* ---- 2. 玩家输入与提交 ---- */
static void test_entry(void) {
    setup(3, 3, (int[]){5,5,1}, (int[]){5,2,3}, 0, 0, 0);
    ld_phase = LD_PH_BID;
    ld_key(K_CHAR, '3');
    CHECK(ld_in_q == 3, "first digit sets qty");
    ld_key(K_CHAR, '5');
    CHECK(ld_in_f == 5, "second digit sets face");
    ld_key(K_DEL, 0);
    CHECK(ld_in_q == 0 && ld_in_f == 0, "DEL clears entry");
    ld_key(K_CHAR, '3');
    ld_key(K_CHAR, '5');
    ld_key(K_OK, 0);
    CHECK(ld_cur_q == 3 && ld_cur_f == 5, "OK submits bid");
    CHECK(ld_bidder == 0, "bidder recorded as player");
    CHECK(ld_phase == LD_PH_AI, "player bid -> AI turn");
    CHECK(ld_in_q == 0 && ld_in_f == 0, "entry cleared after submit");

    /* 输入不完整 */
    setup(3, 3, (int[]){5,5,1}, (int[]){5,2,3}, 0, 0, 0);
    ld_phase = LD_PH_BID;
    ld_key(K_CHAR, '2');
    ld_key(K_OK, 0);
    CHECK(ld_phase == LD_PH_BID && ld_hint == LD_HINT_INC,
          "incomplete entry -> hint, stays BID");

    /* 叫点太低 */
    setup(3, 3, (int[]){5,5,1}, (int[]){5,2,3}, 4, 1, 1);
    ld_phase = LD_PH_BID;
    ld_key(K_CHAR, '3');
    ld_key(K_CHAR, '6');
    ld_key(K_OK, 0);
    CHECK(ld_phase == LD_PH_BID && ld_hint == LD_HINT_LOW,
          "too-low bid -> hint, stays BID");
    CHECK(ld_cur_q == 4 && ld_cur_f == 1, "current bid unchanged");

    /* 超过总骰数(总骰=3 时输 4) */
    setup(1, 2, (int[]){5}, (int[]){1,2}, 2, 2, 1);
    ld_phase = LD_PH_BID;
    ld_key(K_CHAR, '4');
    ld_key(K_CHAR, '3');
    ld_key(K_OK, 0);
    CHECK(ld_phase == LD_PH_BID && ld_hint == LD_HINT_HIGH,
          "q over total -> HIGH hint");

    /* 无叫点时质疑被拒 */
    setup(3, 3, (int[]){5,5,1}, (int[]){5,2,3}, 0, 0, 0);
    ld_phase = LD_PH_BID;
    ld_key(K_SPACE, 0);
    CHECK(ld_phase == LD_PH_BID && ld_hint == LD_HINT_NOBID,
          "SPACE with no bid -> hint only");

    /* 重复键忽略 */
    setup(3, 3, (int[]){5,5,1}, (int[]){5,2,3}, 2, 2, 1);
    ld_phase = LD_PH_BID;
    key_event_t ev;
    ev.key = K_OK; ev.ch = 0; ev.is_repeat = true;
    liarsdice_on_key(&ev);
    CHECK(ld_phase == LD_PH_BID, "repeat OK ignored");

    /* 输入已满再按数字忽略 */
    setup(3, 3, (int[]){5,5,1}, (int[]){5,2,3}, 0, 0, 0);
    ld_phase = LD_PH_BID;
    ld_key(K_CHAR, '3');
    ld_key(K_CHAR, '5');
    ld_key(K_CHAR, '2');
    CHECK(ld_in_q == 3 && ld_in_f == 5, "digits ignored when entry full");
}

/* ---- 3. 质疑开盅与失骰 ---- */
static void test_challenge(void) {
    /* 玩家质疑 AI 叫 3 FIVES: 全场恰好 3 个 -> 足, 质疑方(玩家)失骰 */
    setup(3, 3, (int[]){5,5,1}, (int[]){5,2,3}, 3, 5, 1);
    ld_phase = LD_PH_BID;
    ld_key(K_SPACE, 0);
    CHECK(ld_phase == LD_PH_REVEAL, "player call opens reveal");
    CHECK(ld_reveal_cnt == 3 && ld_reveal_q == 3, "count snapshot 3/3");
    CHECK(ld_loser == 0 && ld_pn == 2 && ld_an == 3,
          "bid met -> caller (player) loses die");

    /* 质疑失败: 全场 2 个不足 3 -> 叫方(AI)失骰 */
    setup(3, 3, (int[]){5,5,1}, (int[]){1,2,3}, 3, 5, 1);
    ld_phase = LD_PH_BID;
    ld_key(K_BACK, 0);
    CHECK(ld_phase == LD_PH_REVEAL && ld_reveal_cnt == 2, "count 2/3");
    CHECK(ld_loser == 1 && ld_an == 2 && ld_pn == 3,
          "bid not met -> bidder (AI) loses die");

    /* AI 叫 4 FOURS 玩家质疑: 全场 4 个 -> 足, 质疑方(玩家)失骰 */
    setup(3, 3, (int[]){4,4,4}, (int[]){4,1,1}, 4, 4, 1);
    ld_phase = LD_PH_BID;
    ld_key(K_SPACE, 0);
    CHECK(ld_reveal_cnt == 4 && ld_loser == 0 && ld_pn == 2,
          "count 4/4 -> caller (player) loses die");

    /* AI 叫 4 FOURS 玩家质疑: 全场 3 个不足 -> 叫方(AI)失骰 */
    setup(3, 3, (int[]){4,4,4}, (int[]){1,1,1}, 4, 4, 1);
    ld_phase = LD_PH_BID;
    ld_key(K_SPACE, 0);
    CHECK(ld_reveal_cnt == 3 && ld_loser == 1 && ld_an == 2,
          "count 3/4 -> bidder (AI) loses die");
}

/* ---- 4. AI 决策 ---- */
static void test_ai(void) {
    /* 开局叫: 自己的优势点数 */
    rng_seed(&ld_rng, 1);
    setup(3, 3, (int[]){1,2,3}, (int[]){6,6,2}, 0, 0, 0);
    ld_phase = LD_PH_AI;
    liarsdice_tick(0);
    CHECK(ld_phase == LD_PH_BID && ld_bidder == 1, "AI opens round");
    CHECK(ld_cur_f == 6, "AI opens with its best face");
    CHECK(ld_cur_q >= 2 && ld_cur_q <= 3, "AI qty = own count (+1 push)");

    /* 加叫: 安全路径, 无随机依赖 */
    setup(3, 3, (int[]){1,2,3}, (int[]){3,3,1}, 2, 4, 0);
    ld_phase = LD_PH_AI;
    liarsdice_tick(0);
    CHECK(ld_phase == LD_PH_BID, "AI raises safely");
    CHECK(ld_cur_q == 3 && ld_cur_f == 3, "AI raises to 3 THREES");
    CHECK(ld_cur_q > 2 || (ld_cur_q == 2 && ld_cur_f > 4),
          "AI bid strictly beats previous (2,4)");

    /* 高叫点: 无法安全加叫 -> 质疑或最小加叫(两种结果都合法) */
    rng_seed(&ld_rng, 3);
    setup(3, 3, (int[]){1,1,1}, (int[]){1,1,2}, 5, 6, 0);
    ld_phase = LD_PH_AI;
    liarsdice_tick(0);
    if (ld_phase == LD_PH_REVEAL) {
        CHECK(ld_loser == 0 && ld_pn == 2,
              "AI challenged: 6s count<5 -> bidder (player) loses");
    } else {
        CHECK(ld_phase == LD_PH_BID, "AI chose minimal raise");
        CHECK(ld_cur_q == 6 && ld_cur_f == 1, "minimal raise = 6 ONES");
        CHECK(ld_bid_ok(ld_cur_q, ld_cur_f), "minimal raise is legal");
    }

    /* 顶到极限 (6,6): 无路可加 -> 必定质疑, 无随机依赖 */
    setup(3, 3, (int[]){6,6,6}, (int[]){1,2,3}, 6, 6, 0);
    ld_phase = LD_PH_AI;
    liarsdice_tick(0);
    CHECK(ld_phase == LD_PH_REVEAL, "bid 6 SIXES -> AI must challenge");
    CHECK(ld_reveal_cnt == 3 && ld_loser == 0 && ld_pn == 2,
          "count 3/6 -> bidder (player) loses");

    /* 确定性: 同种子同决策序列 */
    {
        int a1[8], a2[8];
        rng_seed(&ld_rng, 7);
        setup(3, 3, (int[]){1,1,1}, (int[]){1,1,2}, 5, 6, 0);
        ld_phase = LD_PH_AI; liarsdice_tick(0);
        a1[0] = ld_cur_q; a1[1] = ld_cur_f; a1[2] = (int)ld_phase; a1[3] = ld_loser;
        setup(3, 3, (int[]){1,2,3}, (int[]){6,6,2}, 0, 0, 0);
        ld_phase = LD_PH_AI; liarsdice_tick(0);
        a1[4] = ld_cur_q; a1[5] = ld_cur_f; a1[6] = (int)ld_phase; a1[7] = ld_loser;

        rng_seed(&ld_rng, 7);
        setup(3, 3, (int[]){1,1,1}, (int[]){1,1,2}, 5, 6, 0);
        ld_phase = LD_PH_AI; liarsdice_tick(0);
        a2[0] = ld_cur_q; a2[1] = ld_cur_f; a2[2] = (int)ld_phase; a2[3] = ld_loser;
        setup(3, 3, (int[]){1,2,3}, (int[]){6,6,2}, 0, 0, 0);
        ld_phase = LD_PH_AI; liarsdice_tick(0);
        a2[4] = ld_cur_q; a2[5] = ld_cur_f; a2[6] = (int)ld_phase; a2[7] = ld_loser;
        CHECK(memcmp(a1, a2, sizeof a1) == 0, "same seed -> identical AI series");
    }
}

/* ---- 5. 回合流转与胜负 ---- */
static void test_flow(void) {
    /* 玩家开 2 SIXES -> AI 必加叫 3 SIXES -> 玩家质疑(不足, AI 失骰) */
    setup(3, 3, (int[]){1,2,3}, (int[]){6,6,1}, 0, 0, 0);
    ld_phase = LD_PH_BID;
    ld_key(K_CHAR, '2'); ld_key(K_CHAR, '6');
    ld_key(K_OK, 0);
    CHECK(ld_phase == LD_PH_AI && ld_cur_q == 2 && ld_cur_f == 6,
          "player opens 2 SIXES");
    liarsdice_tick(0);
    CHECK(ld_phase == LD_PH_BID && ld_bidder == 1, "AI raises");
    CHECK(ld_cur_q == 3 && ld_cur_f == 6, "AI raises to 3 SIXES");
    CHECK(ld_cur_q > 2 || (ld_cur_q == 2 && ld_cur_f > 6),
          "AI raise beats 2 SIXES");

    ld_phase = LD_PH_BID;
    ld_key(K_SPACE, 0);
    CHECK(ld_phase == LD_PH_REVEAL && ld_loser == 1 && ld_an == 2,
          "player calls, 6s count 2<3 -> AI loses die");
    ld_key(K_OK, 0);
    CHECK(ld_phase == LD_PH_AI, "loser (AI) starts next round");
    CHECK(ld_cur_q == 0 && ld_cur_f == 0, "new round: bid reset");
    CHECK(ld_an == 2 && ld_pn == 3, "new round: dice counts kept");
    for (int i = 0; i < 3; i++) {
        CHECK(ld_pd[i] >= 1 && ld_pd[i] <= 6, "player dice rerolled in 1..6");
        if (i < 2) CHECK(ld_ad[i] >= 1 && ld_ad[i] <= 6, "AI dice rerolled in 1..6");
    }
    rng_seed(&ld_rng, 6);
    liarsdice_tick(0);
    CHECK(ld_phase == LD_PH_BID && ld_bidder == 1 && ld_cur_q > 0,
          "AI opens the new round");

    /* 玩家质疑失手 -> 玩家失骰 -> 玩家先叫 */
    setup(2, 2, (int[]){1,2}, (int[]){1,3}, 1, 1, 1);
    ld_phase = LD_PH_BID;
    ld_key(K_SPACE, 0);                    /* count 1s = 2 >= 1 -> caller loses */
    CHECK(ld_loser == 0 && ld_pn == 1, "player loses die on wrong call");
    ld_key(K_OK, 0);
    CHECK(ld_phase == LD_PH_BID, "loser (player) opens next round");

    /* 终局: 玩家骰数归 0(AI 叫 1 FIVE 足, 玩家质疑失手) */
    setup(1, 2, (int[]){5}, (int[]){5,5}, 1, 5, 1);
    ld_wins = 0; ld_losses = 0;
    ld_phase = LD_PH_BID;
    ld_key(K_SPACE, 0);                    /* count 5s = 3 >= 1 -> caller loses */
    CHECK(ld_phase == LD_PH_REVEAL && ld_pn == 0, "player out of dice");
    CHECK(ld_winner == 1 && ld_losses == 1, "AI wins, loss recorded");
    ld_key(K_OK, 0);
    CHECK(ld_phase == LD_PH_OVER, "reveal OK -> OVER");
    CHECK(!ld_over_full, "force-full flag not yet set");
    ld_key(K_OK, 0);
    CHECK(ld_pn == 3 && ld_an == 3 && ld_phase == LD_PH_BID,
          "OK in OVER starts new game");
    CHECK(ld_over_full == false && ld_winner == 0, "over state cleared");
    CHECK(ld_wins == 0 && ld_losses == 1, "record kept across retry");

    /* 终局: AI 骰数归 0(AI 叫 2 THREES 不足, 玩家质疑成功) */
    setup(2, 1, (int[]){1,1}, (int[]){3}, 2, 3, 1);
    ld_wins = 0; ld_losses = 0;
    ld_phase = LD_PH_BID;
    ld_key(K_SPACE, 0);                    /* count 3s = 1 < 2 -> bidder loses */
    CHECK(ld_phase == LD_PH_REVEAL && ld_an == 0, "AI out of dice");
    CHECK(ld_winner == 0 && ld_wins == 1, "player wins, win recorded");
    ld_key(K_OK, 0);
    CHECK(ld_phase == LD_PH_OVER, "AI zero -> OVER");
    ld_key(K_OK, 0);
    CHECK(ld_phase == LD_PH_BID && ld_pn == 3, "retry resets dice");

    /* OVER 中 BACK 退出 */
    ld_phase = LD_PH_OVER;
    s_exit_request = false;
    ld_key(K_BACK, 0);
    CHECK(s_exit_request, "BACK in OVER requests exit");

    /* N 新局(游戏中) */
    ld_phase = LD_PH_AI;
    ld_key(K_CHAR, 'n');
    CHECK(ld_phase == LD_PH_BID && ld_pn == 3 && ld_an == 3,
          "N mid-AI-turn starts new game");

    /* 非 AI 阶段 tick 空转 */
    ld_phase = LD_PH_BID;
    liarsdice_tick(0);
    CHECK(ld_phase == LD_PH_BID, "tick no-op outside AI phase");
}

/* ---- 6. 渲染冒烟 ---- */
static void test_render(void) {
    /* 对局中: AI 行隐藏黑盒, 玩家行亮面 */
    setup(3, 3, (int[]){1,2,3}, (int[]){1,2,3}, 3, 5, 1);
    ld_phase = LD_PH_BID;
    ld_in_q = 0; ld_in_f = 0;
    liarsdice_render();
    CHECK(px(LD_X0 + 30, LD_AI_Y + 30), "AI die hidden box filled black");
    CHECK(px(LD_X0, LD_PL_Y), "player die border drawn");
    CHECK(px(LD_X0 + 14, LD_PL_Y + 14), "player die face 1 center pip black");
    CHECK(!px(LD_X0 + 6, LD_PL_Y + 6), "player die corner white (face 1)");

    /* 开盅: AI 行亮面 */
    setup(3, 3, (int[]){5,5,1}, (int[]){1,2,3}, 3, 5, 1);
    ld_phase = LD_PH_REVEAL;
    ld_reveal_q = 3; ld_reveal_f = 5; ld_reveal_cnt = 2; ld_loser = 0; ld_bidder = 1;
    liarsdice_render();
    CHECK(px(LD_X0 + 14, LD_AI_Y + 14), "reveal: AI die face 1 center pip black");
    CHECK(!px(LD_X0 + 40 + 18, LD_AI_Y + 18),
          "reveal: AI die 2 face 2 center white (not a hidden box)");
    CHECK(px(LD_X0 + 40 + 6, LD_AI_Y + 6),
          "reveal: AI die 2 face 2 corner pip black");

    /* 终局: HUD 两行提示 + 墙内无文字 + AI 骰已亮 */
    setup(1, 2, (int[]){5}, (int[]){1,2}, 0, 0, 0);
    ld_phase = LD_PH_OVER;
    ld_winner = 1;
    ld_over_full = false;
    liarsdice_render();
    CHECK(ld_over_full, "OVER render triggers force-full flag");
    CHECK(band_black(2, 150, 296), "OVER HUD right label drawn");
    CHECK(!band_black(70, 0, 296), "OVER wall middle band empty (no text)");
    CHECK(!band_black(140, 0, 296), "OVER wall footer empty");
    CHECK(px(LD_X0 + 14, LD_AI_Y + 14), "OVER: AI die face revealed");

    /* 标题与 HUD 战绩 */
    setup(3, 3, (int[]){5,5,1}, (int[]){5,2,3}, 0, 0, 0);
    ld_phase = LD_PH_BID;
    liarsdice_render();
    CHECK(band_black(3, 0, 60), "HUD title 'LIAR'S DICE' drawn");
    CHECK(band_black(3, 240, 296), "HUD W/L record drawn");
}

int main(void) {
    printf("== LIAR'S DICE host tests ==\n");
    test_bid_ok();
    test_entry();
    test_challenge();
    test_ai();
    test_flow();
    test_render();
    if (s_fail) { printf("RESULT: %d FAILURE(S)\n", s_fail); return 1; }
    printf("RESULT: ALL PASS\n");
    return 0;
}
