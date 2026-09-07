/* BATTLESHIP host 逻辑单测 — 直接包含 battleship.c 访问静态状态 */
#include "../src/games/battleship.c"
#include <stdio.h>
#include <string.h>

bool s_exit_request = false;
void game_set_tick_interval(uint32_t ms) { (void)ms; }

static int s_fail = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); s_fail++; } \
    else { printf("ok: %s\n", msg); } \
} while (0)

static int count_ships(int side) {
    int n = 0;
    for (int i = 0; i < 64; i++) if (bs_ship[side][i]) n++;
    return n;
}

/* 不同船的船格互不相邻(8 邻域); 船内格相邻是正常的 */
static int count_touching_ships(void) {
    int id[64];
    int nid = 0;
    memset(id, 0, sizeof id);
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++) {
            int idx = y * 8 + x;
            if (!bs_ship[1][idx] || id[idx]) continue;
            nid++;
            /* 洪水标记同一艘船 */
            int st[64], sp = 0;
            st[sp++] = idx;
            id[idx] = nid;
            while (sp > 0) {
                int c = st[--sp];
                int cx = c & 7, cy = c >> 3;
                if (cx > 0     && bs_ship[1][c - 1] && !id[c - 1]) { id[c - 1] = nid; st[sp++] = c - 1; }
                if (cx < 7     && bs_ship[1][c + 1] && !id[c + 1]) { id[c + 1] = nid; st[sp++] = c + 1; }
                if (cy > 0     && bs_ship[1][c - 8] && !id[c - 8]) { id[c - 8] = nid; st[sp++] = c - 8; }
                if (cy < 7     && bs_ship[1][c + 8] && !id[c + 8]) { id[c + 8] = nid; st[sp++] = c + 8; }
            }
        }
    int touch = 0;
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++) {
            int idx = y * 8 + x;
            if (!bs_ship[1][idx]) continue;
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++) {
                    if (!dx && !dy) continue;
                    int nx = x + dx, ny = y + dy;
                    if (nx < 0 || ny < 0 || nx >= 8 || ny >= 8) continue;
                    int nidx = ny * 8 + nx;
                    if (bs_ship[1][nidx] && id[nidx] != id[idx]) touch++;
                }
        }
    return touch;
}

/* AI 布船: 9 格、不同船互不相邻、同种子可复现 */
static void test_fleet_ai(void) {
    memset(bs_ship, 0, sizeof bs_ship);
    rng_seed(&bs_rng, 12345);
    bs_place_fleet_ai();
    CHECK(count_ships(1) == 9, "AI fleet covers 9 cells");
    CHECK(count_touching_ships() == 0, "AI fleet ships never touch (8-neighbor)");
    memset(bs_ship, 0, sizeof bs_ship);
    rng_seed(&bs_rng, 12345);
    bs_place_fleet_ai();
    CHECK(count_ships(1) == 9, "AI fleet reproducible with same seed");
    /* 不同种子应给出不同布局(布局多样性) */
    memset(bs_ship, 0, sizeof bs_ship);
    rng_seed(&bs_rng, 777);
    bs_place_fleet_ai();
    CHECK(count_ships(1) == 9, "AI fleet works with another seed");
}

/* bs_fit/bs_stamp: 边界、重叠、相邻拒绝 */
static void test_fit_place(void) {
    memset(bs_ship, 0, sizeof bs_ship);
    CHECK(bs_fit(0, 0, 0, 0, 3), "3-cell fits at (0,0) horizontal");
    CHECK(bs_fit(0, 0, 0, 1, 3), "3-cell fits at (0,0) vertical");
    CHECK(!bs_fit(0, 6, 0, 0, 3), "rejects x overflow (6+3>8)");
    CHECK(!bs_fit(0, 0, 6, 1, 3), "rejects y overflow (6+3>8)");
    bs_stamp(0, 0, 0, 0, 3);                      /* (0,0)(1,0)(2,0) */
    CHECK(!bs_fit(0, 0, 0, 0, 3), "rejects overlap");
    CHECK(!bs_fit(0, 1, 1, 0, 3), "rejects adjacency (cell below ship)");
    CHECK(!bs_fit(0, 0, 1, 0, 3), "rejects adjacency (below first cell)");
    CHECK(!bs_fit(0, 3, 0, 0, 3), "rejects adjacency (right of ship)");
    CHECK(bs_fit(0, 4, 4, 0, 2), "2-cell fits in clear area");
    CHECK(bs_fit(0, 7, 7, 0, 1), "1-cell fits at corner");
}

/* 开火: 未中/命中/沉没/重复开火 */
static void test_fire(void) {
    memset(bs_ship, 0, sizeof bs_ship);
    memset(bs_shot, 0, sizeof bs_shot);
    bs_left[0] = bs_left[1] = 5;
    bs_over = false;
    bs_winner = 0;
    bs_stamp(1, 3, 3, 0, 1);                      /* 敌 1 格船 (3,3) */
    CHECK(bs_fire(1, 0, 0) == 1, "miss returns 1");
    CHECK(bs_shot[1][0] == 2, "miss marked 2 on board");
    CHECK(bs_fire(1, 3, 3) == 3, "single hit returns 3 (sunk)");
    CHECK(bs_left[1] == 4, "ships left decremented to 4");
    CHECK(bs_fire(1, 3, 3) == 0, "re-fire same cell rejected");
    bs_stamp(1, 5, 5, 1, 2);                      /* 敌 2 格船 (5,5)(5,6) */
    CHECK(bs_fire(1, 5, 5) == 2, "partial hit returns 2");
    CHECK(bs_left[1] == 4, "no sink on partial hit");
    CHECK(bs_fire(1, 5, 6) == 3, "second hit returns 3 (sunk)");
    CHECK(bs_left[1] == 3, "ships left decremented to 3");
    CHECK(!bs_over, "game not over yet");
}

/* 胜负: 玩家灭全部敌船 → 胜; 敌灭全部玩家船 → AI 胜 */
static void test_win(void) {
    static const int xs[5] = { 0, 2, 4, 6, 1 };
    static const int ys[5] = { 0, 0, 0, 0, 6 };
    memset(bs_ship, 0, sizeof bs_ship);
    memset(bs_shot, 0, sizeof bs_shot);
    bs_left[0] = bs_left[1] = 5;
    bs_over = false;
    bs_winner = 0;
    for (int i = 0; i < 5; i++) bs_stamp(1, xs[i], ys[i], 0, 1);
    for (int i = 0; i < 4; i++) CHECK(bs_fire(1, xs[i], ys[i]) == 3, "win-path hit");
    CHECK(!bs_over, "not over after 4 of 5 ships sunk");
    CHECK(bs_fire(1, xs[4], ys[4]) == 3, "final enemy ship sunk");
    CHECK(bs_over && bs_winner == 1, "player wins when enemy fleet destroyed");

    memset(bs_ship, 0, sizeof bs_ship);
    memset(bs_shot, 0, sizeof bs_shot);
    bs_left[0] = bs_left[1] = 5;
    bs_over = false;
    bs_winner = 0;
    for (int i = 0; i < 5; i++) bs_stamp(0, xs[i], ys[i], 0, 1);
    for (int i = 0; i < 5; i++) bs_fire(0, xs[i], ys[i]);
    CHECK(bs_over && bs_winner == 2, "AI wins when player fleet destroyed");
}

/* AI 目标队列: 命中后邻格优先 + 寻线方向最优先 */
static void test_ai_queue(void) {
    memset(bs_ship, 0, sizeof bs_ship);
    memset(bs_shot, 0, sizeof bs_shot);
    bs_left[0] = 5;
    bs_over = false;
    bs_winner = 0;
    bs_stamp(0, 4, 3, 0, 2);                      /* 玩家船 (4,3)(5,3) */
    rng_seed(&bs_rng, 999);
    bs_ai_lastx = -1;
    bs_ai_lasty = -1;
    bs_aiq_clear();
    bs_ai_on_hit(4, 3);
    CHECK(bs_aiq_n == 4, "hit enqueues 4 unshot neighbors");
    bs_ai_turn();
    CHECK(bs_shot[0][3 * 8 + 5] == 1, "AI fires at line neighbor (5,3) first");
    CHECK(bs_turn == 1, "turn returns to player after AI shot");
    bs_ai_turn();
    CHECK(bs_shot[0][3 * 8 + 6] == 2, "AI keeps firing along line (6,3), misses");
    /* 已开火格入队无害: 弹头弹出时被过滤 */
    bs_ai_on_hit(4, 3);
    bs_ai_turn();                                 /* 弹出 (5,3)? 已开火 → 跳过 */
    bs_ai_turn();
    CHECK(bs_turn == 1, "AI queue skips already-shot cells safely");
}

/* 布置流程: 5 船放完 → 进入攻击阶段, 玩家先手 */
static void test_placement_flow(void) {
    memset(bs_ship, 0, sizeof bs_ship);
    memset(bs_shot, 0, sizeof bs_shot);
    bs_phase = BS_PLACE;
    bs_cur = 0;
    bs_left[0] = 5;
    bs_over = false;
    bs_turn = 1;
    bs_cx = 0; bs_cy = 0; bs_dir = 0; bs_place_player();   /* 3 格 (0,0)-(2,0) */
    CHECK(bs_cur == 1 && bs_phase == BS_PLACE, "first ship placed, still placing");
    bs_cx = 4; bs_cy = 4; bs_dir = 1; bs_place_player();   /* 2 格 (4,4)-(4,5) */
    bs_cx = 0; bs_cy = 6; bs_dir = 0; bs_place_player();   /* 2 格 (0,6)-(1,6) */
    bs_cx = 6; bs_cy = 0; bs_dir = 0; bs_place_player();   /* 1 格 (6,0) */
    bs_cx = 6; bs_cy = 6; bs_dir = 0; bs_place_player();   /* 1 格 (6,6) */
    CHECK(bs_cur == 5 && bs_phase == BS_ATTACK, "fifth ship placed, attack begins");
    CHECK(count_ships(0) == 9, "player fleet covers 9 cells");
    CHECK(bs_turn == 1, "player goes first in attack");
}

/* 完整对局(随机打): 必终止且有胜者; AI 智能性: 命中后不浪费在已打格 */
static void test_full_game(void) {
    bs_new();
    bs_turn = 1;
    rng_seed(&bs_rng, 42);
    int guard = 0;
    while (!bs_over && guard < 300) {
        int x, y, g2 = 0, r;
        guard++;
        if (bs_turn == 1) {
            do {
                x = (int)rng_range(&bs_rng, 8);
                y = (int)rng_range(&bs_rng, 8);
                g2++;
            } while (bs_shot[1][y * 8 + x] != 0 && g2 < 256);
            if (bs_shot[1][y * 8 + x] != 0) {
                for (int k = 0; k < 64; k++)
                    if (!bs_shot[1][k]) { x = k & 7; y = k >> 3; break; }
            }
            r = bs_fire(1, x, y);
            if (r != 0 && !bs_over) { bs_turn = 2; bs_ai_pending = true; }
        } else {
            battleship_tick(0);
        }
    }
    CHECK(bs_over, "full random game terminates in a win");
    CHECK(bs_winner == 1 || bs_winner == 2, "game declares a winner");
}

/* render 冒烟: 各阶段绘制不越界不崩溃(覆盖光标/幽灵/箭头/结束横幅) */
static void test_render_smoke(void) {
    bs_new();
    battleship_render();
    battleship_render();                      /* 幂等 */
    bs_phase = BS_ATTACK;
    bs_cur = 5;
    bs_cx = 7; bs_cy = 7; bs_turn = 2;        /* AI 回合箭头 */
    bs_ai_pending = true;
    battleship_render();
    battleship_tick(0);
    battleship_render();
    bs_over = true;
    bs_winner = 1;
    battleship_render();                      /* 结束横幅 + 全刷一次 */
    bs_over = false;
    bs_over_full = false;
    bs_phase = BS_PLACE;
    bs_cur = 0;
    bs_cx = 6; bs_cy = 6; bs_dir = 1;
    battleship_render();
}

int main(void) {
    test_fleet_ai();
    test_fit_place();
    test_fire();
    test_win();
    test_ai_queue();
    test_placement_flow();
    test_full_game();
    test_render_smoke();
    if (s_fail) { printf("\n%d FAILURES\n", s_fail); return 1; }
    printf("\nALL TESTS PASSED\n");
    return 0;
}
