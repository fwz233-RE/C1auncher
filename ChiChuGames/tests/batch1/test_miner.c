/* MINER host 逻辑单测 — 直接包含 miner.c 访问静态状态 */
#include "../../src/games/miner.c"
#include <stdio.h>
#include <string.h>

bool s_exit_request = false;
void game_set_tick_interval(uint32_t ms) { (void)ms; }

static int s_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); s_fail++; } \
    else { printf("ok: %s\n", msg); } \
} while (0)

static void key(ccg_key k, uint8_t ch, bool rep) {
    key_event_t ev;
    ev.key = k;
    ev.ch = ch;
    ev.is_repeat = rep;
    miner_on_key(&ev);
}

/* ---- 层生成: 配比/出生点/揭示/密度递增/确定性 ---- */
static void test_gen(void) {
    mn_layer = 2;                    /* L3: 9煤 8铁 3金 */
    rng_seed(&mn_rng, 42);
    mn_gen_layer();
    int c = 0, i = 0, g = 0, e = 0;
    for (int k = 0; k < MN_CELLS; k++) {
        if (mn_cell[k] == MN_O_COAL) c++;
        else if (mn_cell[k] == MN_O_IRON) i++;
        else if (mn_cell[k] == MN_O_GOLD) g++;
        else e++;
    }
    CHECK(c == 9 && i == 8 && g == 3, "L3 ore counts 9/8/3");
    CHECK(e == 45 - 20, "L3 empty count 25");
    CHECK(mn_cell[mn_idx(0, 4)] == MN_O_NONE, "start cell (0,4) empty");
    CHECK(mn_rev[mn_idx(0, 4)] == 1, "start cell revealed");
    CHECK(mn_ores_left == 20, "ores_left == 20");
    CHECK(mn_px == 0 && mn_py == 4, "player starts (0,4)");
    /* 密度随深度递增 */
    int tot[MN_LAYERS];
    for (int L = 0; L < MN_LAYERS; L++) {
        mn_layer = L;
        rng_seed(&mn_rng, 42 + (uint32_t)L);
        mn_gen_layer();
        int n = 0;
        for (int k = 0; k < MN_CELLS; k++)
            if (mn_cell[k] != MN_O_NONE) n++;
        tot[L] = n;
        CHECK(mn_cell[mn_idx(0, 4)] == MN_O_NONE, "spawn protected on every layer");
    }
    CHECK(tot[0] == 12 && tot[1] == 16 && tot[2] == 20 &&
          tot[3] == 24 && tot[4] == 28, "ore density 12/16/20/24/28");
}

/* 确定性: 同种子生成两次布局完全一致 */
static void test_determinism(void) {
    static uint8_t a[MN_CELLS], b[MN_CELLS];
    mn_layer = 4;
    rng_seed(&mn_rng, 777);
    mn_gen_layer();
    memcpy(a, mn_cell, sizeof(a));
    rng_seed(&mn_rng, 777);
    mn_gen_layer();
    memcpy(b, mn_cell, sizeof(b));
    CHECK(memcmp(a, b, sizeof(a)) == 0, "same seed -> same layout");
}

/* ---- 移动 / 体力 / 边界 ---- */
static void test_move(void) {
    mn_layer = 0;
    rng_seed(&mn_rng, 7);
    mn_gen_layer();
    mn_state = MN_ST_PLAY;
    mn_moves = 0;
    mn_hp_now = 5;
    key(K_RIGHT, 0, false);
    CHECK(mn_px == 1 && mn_py == 4, "move right");
    CHECK(mn_rev[mn_idx(1, 4)] == 1, "move reveals destination");
    key(K_LEFT, 0, false);
    key(K_LEFT, 0, false);           /* 左边界挡 */
    CHECK(mn_px == 0 && mn_moves == 2, "left edge blocked");
    /* 每 10 步 -1 体力 */
    int hp0 = mn_hp_now;
    mn_moves = 8;
    key(K_RIGHT, 0, false);
    key(K_RIGHT, 0, false);
    CHECK(mn_moves == 10 && mn_hp_now == hp0 - 1, "stamina -1 at 10 moves");
    CHECK(mn_state == MN_ST_PLAY, "still playing at hp>0");
    /* 体力 0 → 结束 */
    mn_hp_now = 1;
    mn_moves = 9;
    key(K_RIGHT, 0, false);
    CHECK(mn_state == MN_ST_OVER && !mn_win, "stamina 0 -> lose over");
    /* 重复方向键可重复移动; 重复确认键忽略 */
    mn_state = MN_ST_PLAY;
    mn_hp_now = 30;
    mn_moves = 0;
    key(K_RIGHT, 0, true);
    CHECK(mn_px == 4, "repeat dir moves");
    key(K_OK, 0, true);
    CHECK(mn_state == MN_ST_PLAY, "repeat OK ignored");
    /* WASD 移动 */
    key(K_CHAR, 'w', false);
    CHECK(mn_py == 3, "w moves up");
}

/* ---- 挖矿: 煤/铁耐久/裂痕/铲子伤害 ---- */
static void test_dig(void) {
    mn_layer = 0;
    rng_seed(&mn_rng, 99);
    mn_gen_layer();
    mn_state = MN_ST_PLAY;
    mn_gold = 0;
    mn_shovel = 0;
    /* 煤 1 击 */
    mn_cell[mn_idx(1, 4)] = MN_O_COAL;
    mn_hp[mn_idx(1, 4)] = 1;
    mn_rev[mn_idx(1, 4)] = 1;
    mn_px = 1; mn_py = 4;
    int left = mn_ores_left;
    key(K_OK, 0, false);
    CHECK(mn_gold == 1, "coal +1 gold");
    CHECK(mn_hp[mn_idx(1, 4)] == 0, "coal dug in 1 hit");
    CHECK(mn_ores_left == left - 1, "ores_left decremented");
    /* 挖空地无效果 */
    mn_px = 0; mn_py = 4;
    int g0 = mn_gold;
    key(K_OK, 0, false);
    CHECK(mn_gold == g0, "dig empty no-op");
    /* 铁 2 击: 第 1 击裂痕, 第 2 击 +3 */
    mn_cell[mn_idx(2, 0)] = MN_O_IRON;
    mn_hp[mn_idx(2, 0)] = 2;
    mn_rev[mn_idx(2, 0)] = 1;
    mn_px = 2; mn_py = 0;
    key(K_OK, 0, false);
    CHECK(mn_hp[mn_idx(2, 0)] == 1 && mn_crack[mn_idx(2, 0)] == 1,
          "iron hit1 -> crack");
    key(K_OK, 0, false);
    CHECK(mn_gold == g0 + 3, "iron +3 gold");
    CHECK(mn_crack[mn_idx(2, 0)] == 0, "crack cleared when dug");
    /* 金 3 击(无铲子) */
    mn_cell[mn_idx(3, 0)] = MN_O_GOLD;
    mn_hp[mn_idx(3, 0)] = 3;
    mn_rev[mn_idx(3, 0)] = 1;
    mn_px = 3; mn_py = 0;
    g0 = mn_gold;
    key(K_OK, 0, false);
    key(K_OK, 0, false);
    CHECK(mn_gold == g0, "gold not dug after 2 hits");
    key(K_OK, 0, false);
    CHECK(mn_gold == g0 + 8, "gold +8 after 3 hits");
    /* 铲子 +1 伤害: 铁 1 击 */
    mn_shovel = 1;
    mn_cell[mn_idx(4, 0)] = MN_O_IRON;
    mn_hp[mn_idx(4, 0)] = 2;
    mn_rev[mn_idx(4, 0)] = 1;
    mn_px = 4; mn_py = 0;
    g0 = mn_gold;
    key(K_OK, 0, false);
    CHECK(mn_gold == g0 + 3, "shovel1 iron 1-hit");
    /* 铲子 +2: 金 1 击 */
    mn_shovel = 2;
    mn_cell[mn_idx(5, 0)] = MN_O_GOLD;
    mn_hp[mn_idx(5, 0)] = 3;
    mn_rev[mn_idx(5, 0)] = 1;
    mn_px = 5; mn_py = 0;
    g0 = mn_gold;
    key(K_OK, 0, false);
    CHECK(mn_gold == g0 + 8, "shovel2 gold 1-hit");
}

/* ---- 炸弹: 3x3 清空 / 边界夹取 / 无弹无效 ---- */
static void test_bomb(void) {
    mn_layer = 0;
    rng_seed(&mn_rng, 123);
    mn_gen_layer();
    mn_state = MN_ST_PLAY;
    mn_gold = 0;
    mn_bombs = 1;
    /* 清空全图, 玩家 (4,2) 周围 3x3 摆满煤 + 远处 1 块煤 */
    for (int k = 0; k < MN_CELLS; k++) {
        mn_cell[k] = MN_O_NONE;
        mn_hp[k] = 0;
        mn_rev[k] = 0;
    }
    int n = 0;
    for (int y = 1; y <= 3; y++)
        for (int x = 3; x <= 5; x++) {
            mn_cell[mn_idx(x, y)] = MN_O_COAL;
            mn_hp[mn_idx(x, y)] = 1;
            n++;
        }
    mn_cell[mn_idx(8, 0)] = MN_O_COAL; mn_hp[mn_idx(8, 0)] = 1;   /* 3x3 外 */
    n++;
    mn_ores_left = n;
    mn_px = 4; mn_py = 2;
    key(K_CHAR, 'b', false);
    CHECK(mn_bombs == 0, "bomb consumed");
    CHECK(mn_gold == n - 1, "bomb clears 3x3 (+1 each)");
    CHECK(mn_ores_left == 1, "ore outside 3x3 survives");
    CHECK(mn_state == MN_ST_PLAY, "layer not done yet");
    /* 挖掉最后一块 → 层结束 +1 炸弹 */
    mn_px = 8; mn_py = 0;
    key(K_OK, 0, false);
    CHECK(mn_state == MN_ST_NEXT && mn_layer == 1, "layer done -> next");
    CHECK(mn_bombs == 1, "next layer grants +1 bomb");
    /* 无弹无效 */
    mn_bombs = 0;
    mn_state = MN_ST_PLAY;
    mn_gold = 0;
    key(K_CHAR, 'b', false);
    CHECK(mn_gold == 0, "no bomb -> no effect");
    /* 角落夹取: 玩家 (0,0), 只清 4 格 */
    mn_layer = 0;
    rng_seed(&mn_rng, 321);
    mn_gen_layer();
    mn_state = MN_ST_PLAY;
    mn_bombs = 1;
    mn_gold = 0;
    for (int k = 0; k < MN_CELLS; k++) {
        mn_cell[k] = MN_O_NONE;
        mn_hp[k] = 0;
    }
    mn_cell[mn_idx(0, 0)] = MN_O_COAL; mn_hp[mn_idx(0, 0)] = 1;
    mn_cell[mn_idx(1, 0)] = MN_O_COAL; mn_hp[mn_idx(1, 0)] = 1;
    mn_cell[mn_idx(0, 1)] = MN_O_COAL; mn_hp[mn_idx(0, 1)] = 1;
    mn_cell[mn_idx(1, 1)] = MN_O_COAL; mn_hp[mn_idx(1, 1)] = 1;
    mn_ores_left = 4;
    mn_px = 0; mn_py = 0;
    key(K_CHAR, 'b', false);
    CHECK(mn_gold == 4, "corner bomb clears clamped 4 cells");
    CHECK(mn_hp[mn_idx(1, 1)] == 0, "corner bomb cell cleared");
    CHECK(mn_state == MN_ST_NEXT, "corner bomb also finishes layer");
}

/* ---- 层推进 / 胜利 ---- */
static void test_win(void) {
    mn_layer = 4;                    /* 第 5 层 */
    rng_seed(&mn_rng, 6);
    mn_gen_layer();
    mn_state = MN_ST_PLAY;
    mn_bombs = 3;                    /* 到上限 */
    for (int k = 0; k < MN_CELLS; k++) { mn_cell[k] = MN_O_NONE; mn_hp[k] = 0; }
    mn_cell[mn_idx(1, 4)] = MN_O_COAL;
    mn_hp[mn_idx(1, 4)] = 1;
    mn_rev[mn_idx(1, 4)] = 1;
    mn_ores_left = 1;
    mn_px = 1; mn_py = 4;
    mn_gold = 123;
    key(K_OK, 0, false);
    CHECK(mn_state == MN_ST_OVER && mn_win, "win after clearing layer 5");
    CHECK(mn_bombs == 3, "bomb cap holds (no +1 past 3)");
    /* OK/N 重开: 状态全重置 */
    key(K_OK, 0, false);
    CHECK(mn_state == MN_ST_PLAY && mn_layer == 0, "retry -> layer 1");
    CHECK(mn_gold == 0 && mn_hp_now == MN_MAX_HP && mn_moves == 0 &&
          mn_shovel == 0 && !mn_lamp && mn_bombs == 1 && !mn_win,
          "retry resets all progress");
    CHECK(mn_cell[mn_idx(0, 4)] == MN_O_NONE, "retry start cell empty");
}

/* ---- 商店: 购买/限价/满级/关闭 ---- */
static void test_shop(void) {
    mn_state = MN_ST_PLAY;
    mn_gold = 30;
    mn_shovel = 0;
    mn_lamp = false;
    mn_bombs = 1;
    key(K_SPACE, 0, false);
    CHECK(mn_state == MN_ST_SHOP, "SPACE opens shop");
    /* 铲子 1 级 5g */
    mn_sel = 0;
    key(K_OK, 0, false);
    CHECK(mn_shovel == 1 && mn_gold == 25, "shovel lvl1 cost 5");
    /* 钱不够买不动 */
    mn_gold = 4;
    mn_sel = 0;
    key(K_OK, 0, false);
    CHECK(mn_shovel == 1 && mn_gold == 4, "too poor to buy");
    /* 矿灯 12g, 一次性 */
    mn_gold = 12;
    mn_sel = 1;
    key(K_OK, 0, false);
    CHECK(mn_lamp && mn_gold == 0, "lamp bought for 12");
    key(K_OK, 0, false);
    CHECK(mn_lamp, "lamp not rebuyable");
    /* 炸弹 10g */
    mn_gold = 10;
    mn_sel = 2;
    key(K_OK, 0, false);
    CHECK(mn_bombs == 2 && mn_gold == 0, "bomb +1 for 10");
    /* BACK 行关闭 */
    mn_sel = 3;
    key(K_OK, 0, false);
    CHECK(mn_state == MN_ST_PLAY, "BACK item closes shop");
    /* 铲子满级后不可再买 */
    mn_gold = 100;
    mn_shovel = 2;
    key(K_SPACE, 0, false);
    mn_sel = 0;
    key(K_OK, 0, false);
    CHECK(mn_shovel == 2 && mn_gold == 100, "shovel maxed");
    /* BACK 键直接关店 */
    key(K_BACK, 0, false);
    CHECK(mn_state == MN_ST_PLAY, "BACK key closes shop");
}

/* ---- 矿灯: 移动揭示邻格 vs 无灯只揭示自身 ---- */
static void test_lamp(void) {
    mn_layer = 0;
    rng_seed(&mn_rng, 11);
    mn_gen_layer();
    mn_state = MN_ST_PLAY;
    mn_moves = 0;
    mn_hp_now = 30;
    mn_lamp = true;
    for (int k = 0; k < MN_CELLS; k++) mn_rev[k] = 0;
    mn_px = 4; mn_py = 2;
    key(K_RIGHT, 0, false);
    int cnt = 0;
    for (int k = 0; k < MN_CELLS; k++) if (mn_rev[k]) cnt++;
    CHECK(cnt == 9, "lamp reveals 3x3 around move target");
    mn_lamp = false;
    for (int k = 0; k < MN_CELLS; k++) mn_rev[k] = 0;
    key(K_LEFT, 0, false);
    cnt = 0;
    for (int k = 0; k < MN_CELLS; k++) if (mn_rev[k]) cnt++;
    CHECK(cnt == 1, "no lamp reveals only self");
}

/* ---- 渲染冒烟: 各状态可渲染且有内容; 结束画面只全刷一次 ---- */
static void test_render(void) {
    mn_layer = 0;
    rng_seed(&mn_rng, 13);
    mn_gen_layer();
    mn_state = MN_ST_PLAY;
    mn_gold = 7;
    mn_hp_now = 25;
    mn_over_full = false;
    miner_render();
    int nz = 0;
    for (unsigned k = 0; k < CCG_FRAME_BYTES; k++)
        if (g_fb[k]) nz++;
    CHECK(nz > 100, "PLAY render draws frame");
    mn_state = MN_ST_SHOP;
    miner_render();
    nz = 0;
    for (unsigned k = 0; k < CCG_FRAME_BYTES; k++)
        if (g_fb[k]) nz++;
    CHECK(nz > 100, "SHOP render draws frame");
    mn_state = MN_ST_NEXT;
    miner_render();
    nz = 0;
    for (unsigned k = 0; k < CCG_FRAME_BYTES; k++)
        if (g_fb[k]) nz++;
    CHECK(nz > 100, "NEXT render draws frame");
    mn_state = MN_ST_OVER;
    mn_win = false;
    mn_over_full = false;
    miner_render();
    CHECK(mn_over_full, "OVER render triggers one-time force full flag");
}

int main(void) {
    printf("== MINER logic tests ==\n");
    test_gen();
    test_determinism();
    test_move();
    test_dig();
    test_bomb();
    test_win();
    test_shop();
    test_lamp();
    test_render();
    if (s_fail) {
        printf("RESULT: %d FAILED\n", s_fail);
        return 1;
    }
    printf("RESULT: ALL PASS\n");
    return 0;
}
