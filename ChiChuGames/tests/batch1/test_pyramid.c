/* PYRAMID host 逻辑测试 — include 游戏 .c 直接访问静态状态
 * 覆盖: 发牌完整性/暴露规则/光标导航/配对与 13 判定/取消/K 单除/
 * 废牌堆配对/翻牌与回收/死局判定/覆盖牌不可选/同种子确定性/随机压测 */
#include "../src/config.h"
#include "../src/games/pyramid.c"
#include <stdio.h>
#include <string.h>

/* main.c 提供, host 测试 stub */
bool s_exit_request = false;
void game_set_tick_interval(uint32_t ms) { (void)ms; }

static int s_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); s_fail++; } \
    else { printf("ok: %s\n", msg); } \
} while (0)

/* ---- 发牌完整性: 52 张 (值,花色) 无重复, 布局正确 ---- */
static void test_deal(void) {
    rng_seed(&py_rng, 12345);
    py_deal();
    CHECK(py_left() == 28, "28 cards in pyramid");
    CHECK(py_stock_i == 0, "stock ready");
    CHECK(py_waste_n == 0, "waste empty");
    int seen[4][13];
    memset(seen, 0, sizeof(seen));
    int ok = 1;
    for (int i = 0; i < 28; i++) {
        if (py_val[i] < 1 || py_val[i] > 13) ok = 0;
        if (py_suit[i] < 0 || py_suit[i] > 3) ok = 0;
        seen[py_suit[i]][py_val[i] - 1]++;
    }
    for (int i = 0; i < 24; i++) {
        if (py_stock[i] < 1 || py_stock[i] > 13) ok = 0;
        if (py_stock_s[i] < 0 || py_stock_s[i] > 3) ok = 0;
        seen[py_stock_s[i]][py_stock[i] - 1]++;
    }
    for (int s = 0; s < 4 && ok; s++)
        for (int v = 0; v < 13; v++)
            if (seen[s][v] != 1) ok = 0;
    CHECK(ok, "52 unique (value,suit) cards");
    CHECK(!py_over && !py_won, "not over after deal");
    CHECK(py_selp == -1 && !py_selw, "no selection after deal");
}

/* ---- 暴露规则: 开局仅底行 7 张暴露; 移除下层后上层露出 ---- */
static void test_exposed(void) {
    rng_seed(&py_rng, 7);
    py_deal();
    int n = 0;
    for (int i = 0; i < 28; i++)
        if (py_exposed(i)) n++;
    CHECK(n == 7, "only 7 bottom cards exposed initially");
    /* 移除 (6,0)(6,1) 后 (5,0)=15 露出 */
    py_val[21] = 0; py_val[22] = 0;
    CHECK(py_exposed(15), "row5 col0 exposed after below removed");
    CHECK(!py_exposed(16), "row5 col1 still covered");
    py_val[23] = 0;
    CHECK(py_exposed(16), "row5 col1 exposed after below removed");
    CHECK(py_exposed(24), "bottom row always exposed");
    /* 两层: (4,0) 需 (5,0)(5,1) 全清 */
    py_val[15] = 0; py_val[16] = 0;
    CHECK(py_exposed(10), "row4 col0 exposed after row5 cleared");
}

/* ---- 光标导航边界 ---- */
static void test_cursor(void) {
    rng_seed(&py_rng, 9);
    py_deal();
    CHECK(py_cz == 0 && py_cr == 6 && py_cc == 0, "cursor starts row6 col0");
    py_cursor(2);
    CHECK(py_cr == 6 && py_cc == 0, "left at col0 stays");
    py_cursor(3);
    CHECK(py_cc == 1, "right to col1");
    py_cursor(0);
    CHECK(py_cr == 5 && py_cc == 1, "up to row5 col1 (clamped)");
    py_cursor(0);
    CHECK(py_cr == 4 && py_cc == 1, "up to row4 col1");
    py_cursor(1);
    py_cursor(1);
    CHECK(py_cr == 6 && py_cc == 1, "down back to row6 col1");
    py_cursor(1);
    CHECK(py_cz == 1 && py_bc == 1, "down from row6 to waste slot");
    py_cursor(2);
    CHECK(py_bc == 0, "left to stock slot");
    py_cursor(2);
    CHECK(py_bc == 0, "left at stock slot stays");
    py_cursor(3);
    CHECK(py_bc == 1, "right back to waste");
    py_cursor(0);
    CHECK(py_cz == 0 && py_cr == 6 && py_cc == 1, "up back to row6 col1");
    py_cursor(1);                   /* 底部 DOWN 无动作 */
    CHECK(py_cz == 1, "down at bottom row stays");
    /* 底行右移到右端后回金字塔, 列钳位 */
    py_cursor(3);
    CHECK(py_bc == 1, "right at waste stays");
}

/* ---- 配对移除: 两张暴露牌凑 13 ---- */
static void test_pair(void) {
    rng_seed(&py_rng, 3);
    py_deal();
    for (int i = 0; i < 28; i++) py_val[i] = 0;
    py_val[21] = 5; py_val[22] = 8;
    py_over = false; py_won = false;
    py_ok_py(21);
    CHECK(py_selp == 21, "first OK selects");
    CHECK(py_val[21] == 5, "not removed yet");
    py_ok_py(22);
    CHECK(py_val[21] == 0 && py_val[22] == 0, "pair 5+8 removed");
    CHECK(py_over && py_won, "empty pyramid wins");
    CHECK(py_selp == -1, "selection cleared after removal");
}

/* ---- 取消选择 与 非 13 配对不删除 ---- */
static void test_cancel(void) {
    rng_seed(&py_rng, 3);
    py_deal();
    for (int i = 0; i < 28; i++) py_val[i] = 0;
    py_val[21] = 5; py_val[22] = 9;
    py_over = false; py_won = false;
    py_ok_py(21);
    py_ok_py(21);
    CHECK(py_selp == -1, "second OK cancels non-K");
    CHECK(py_val[21] == 5 && py_val[22] == 9, "cards intact after cancel");
    py_ok_py(21);
    py_ok_py(22);
    CHECK(py_val[21] == 5 && py_val[22] == 9, "14 pair: nothing removed");
    CHECK(py_selp == -1, "failed pair clears selection");
}

/* ---- K 单独移除(重复 OK), 非 K 重复 OK 仅取消 ---- */
static void test_king(void) {
    rng_seed(&py_rng, 5);
    py_deal();
    for (int i = 0; i < 28; i++) py_val[i] = 0;
    py_val[21] = 13;
    py_over = false; py_won = false;
    py_ok_py(21);
    CHECK(py_selp == 21, "K selected on first OK");
    py_ok_py(21);
    CHECK(py_val[21] == 0, "K removed on second OK");
    CHECK(py_over && py_won, "single K removal wins");
    py_val[21] = 10;
    py_over = false; py_won = false;
    py_ok_py(21);
    py_ok_py(21);
    CHECK(py_val[21] == 10, "non-K not removed by second OK");
}

/* ---- 废牌堆配对与 K 丢弃 ---- */
static void test_waste_pair(void) {
    rng_seed(&py_rng, 11);
    py_deal();
    for (int i = 0; i < 28; i++) py_val[i] = 0;
    py_val[21] = 5; py_val[22] = 8;
    py_over = false; py_won = false;
    py_stock_i = 0;
    py_stock[0] = 8; py_stock_s[0] = 0;
    py_stock[1] = 8; py_stock_s[1] = 0;
    py_waste_n = 0;
    py_flip();
    CHECK(py_waste_n == 1 && py_waste[0] == 8, "flip moves stock top to waste");
    CHECK(py_stock_i == 1, "stock consumed by flip");
    py_ok_waste();
    CHECK(py_selw, "waste top selected");
    py_ok_py(21);
    CHECK(py_val[21] == 0, "pyramid card removed with waste pair");
    CHECK(py_waste_n == 0, "waste top discarded");
    py_ok_waste();
    CHECK(!py_selw, "OK on empty waste is no-op");
    py_flip();
    CHECK(py_waste_n == 1 && py_waste[0] == 8, "second flip");
    py_ok_py(22);
    CHECK(py_selp == 22, "pyramid selected first");
    py_ok_waste();
    CHECK(py_val[22] == 8, "16 pair not removed");
    CHECK(py_selp == -1 && !py_selw, "failed waste pair clears both");
    py_waste[0] = 13; py_waste_s[0] = 1; py_waste_n = 1;
    py_ok_waste();
    CHECK(py_selw, "waste K selected");
    py_ok_waste();
    CHECK(py_waste_n == 0, "waste K discarded alone");
}

/* ---- 发牌堆耗尽 + 回收 ---- */
static void test_recycle(void) {
    rng_seed(&py_rng, 13);
    py_deal();
    py_stock_i = 22;
    py_stock[22] = 1; py_stock_s[22] = 0;
    py_stock[23] = 2; py_stock_s[23] = 0;
    py_waste_n = 2;
    py_waste[0] = 7; py_waste_s[0] = 0;
    py_waste[1] = 9; py_waste_s[1] = 0;
    CHECK(py_flip() && py_stock_i == 23 && py_waste_n == 3 && py_waste[2] == 1,
          "flip #1");
    CHECK(py_flip() && py_stock_i == 24 && py_waste[3] == 2, "flip #2 exhausts");
    CHECK(py_flip() && py_stock_i == 0 && py_waste_n == 0, "recycle to stock");
    CHECK(py_stock[0] == 2 && py_stock[1] == 1, "recycle order reversed");
    CHECK(py_flip() && py_waste[0] == 2, "recycled card dealt back");
    py_stock_i = 24;
    py_waste_n = 0;
    CHECK(!py_flip(), "flip with empty stock and waste returns false");
}

/* ---- 死局判定 ---- */
static void test_dead(void) {
    rng_seed(&py_rng, 17);
    py_deal();
    for (int i = 0; i < 28; i++) py_val[i] = 0;
    py_val[21] = 2; py_val[22] = 4;    /* 2+4=6, 无 K, 无法配 13 */
    py_over = false; py_won = false;
    py_stock_i = 24; py_waste_n = 0;
    py_after_move();
    CHECK(py_over && !py_won, "dead end detected");
    py_over = false;
    py_stock_i = 23;
    py_after_move();
    CHECK(!py_over, "stock remaining: not dead yet");
    py_stock_i = 24;
    py_val[21] = 5; py_val[22] = 8;
    py_over = false;
    py_after_move();
    CHECK(!py_over, "playable pair: not dead");
    py_val[21] = 13; py_val[22] = 0;
    py_over = false;
    py_after_move();
    CHECK(!py_over, "exposed K removable: not dead");
    py_val[21] = 2; py_val[22] = 4;
    py_waste_n = 1; py_waste[0] = 13; py_waste_s[0] = 0;
    py_over = false;
    py_after_move();
    CHECK(!py_over, "waste-top K discardable: not dead");
    py_waste[0] = 9;
    py_over = false;
    py_after_move();
    CHECK(!py_over, "waste-top pairs with 4: not dead");
    /* 可配对被压住: (5,0)=8 被 (6,0)(6,1) 压住, (5,2)=5 暴露但配 13 的 8 不可用 */
    py_waste_n = 0;
    py_val[21] = 2; py_val[22] = 4;
    py_val[15] = 8; py_val[16] = 0;
    py_val[17] = 5;
    py_over = false;
    py_after_move();
    CHECK(py_over, "covered pair not playable: dead");
    CHECK(py_won == false, "dead end is not a win");
}

/* ---- 被压住的牌不可选 ---- */
static void test_covered_unselectable(void) {
    rng_seed(&py_rng, 19);
    py_deal();
    py_ok_py(15);
    CHECK(py_selp == -1, "covered card not selectable");
    py_ok_py(15);
    CHECK(py_selp == -1, "covered card still not selectable");
    py_ok_py(21);
    CHECK(py_selp == 21, "exposed card selectable");
    py_ok_py(21);
    CHECK(py_selp == -1, "deselect works");
}

/* ---- 发牌堆翻出顺序与发牌堆一致 ---- */
static void test_stock_order(void) {
    rng_seed(&py_rng, 555);
    py_deal();
    int order[24];
    for (int i = 0; i < 24; i++) {
        CHECK(py_flip(), "flip succeeds");
        order[i] = py_waste[py_waste_n - 1];
    }
    CHECK(py_stock_i == 24 && py_waste_n == 24, "all 24 dealt to waste");
    int ok = 1;
    for (int i = 0; i < 24; i++)
        if (order[i] != py_stock[i]) ok = 0;
    CHECK(ok, "deal order matches stock order");
    CHECK(!py_over, "full pyramid: no dead end after dealing all");
}

/* ---- 同种子确定性 ---- */
static void test_deterministic(void) {
    rng_seed(&py_rng, 31337);
    py_deal();
    int v1[28], s1[28], st1[24];
    memcpy(v1, py_val, sizeof(v1));
    memcpy(s1, py_suit, sizeof(s1));
    memcpy(st1, py_stock, sizeof(st1));
    rng_seed(&py_rng, 31337);
    py_deal();
    CHECK(memcmp(v1, py_val, sizeof(v1)) == 0, "same seed: same pyramid");
    CHECK(memcmp(s1, py_suit, sizeof(s1)) == 0, "same seed: same suits");
    CHECK(memcmp(st1, py_stock, sizeof(st1)) == 0, "same seed: same stock");
}

/* ---- 随机压测: 状态不变式 + 无死循环 ---- */
static void test_random(void) {
    uint32_t fz = 987654u;
    rng_seed(&py_rng, 4242);
    py_deal();
    int steps = 0;
    for (int i = 0; i < 20000; i++) {
        fz = fz * 1664525u + 1013904223u;
        switch (fz % 6) {
        case 0: py_cursor((int)(fz % 4)); break;
        case 1:
            if (py_cz == 0)
                py_ok_py(py_idx(py_cr, py_cc));
            else if (py_bc == 1)
                py_ok_waste();
            else
                py_flip();
            break;
        case 2: py_flip(); break;
        case 3: py_ok_waste(); break;
        default: py_ok_py((int)(fz % 28)); break;
        }
        int left = 0, ok = 1;
        for (int k = 0; k < 28; k++) {
            if (py_val[k]) {
                left++;
                if (py_val[k] < 1 || py_val[k] > 13) ok = 0;
            }
        }
        if (py_stock_i < 0 || py_stock_i > 24) ok = 0;
        if (py_waste_n < 0 || py_waste_n > 24) ok = 0;
        if (py_selp >= 0 && py_val[py_selp] == 0) ok = 0;
        if (py_cr < 0 || py_cr > 6 || py_cc < 0 || py_cc > py_cr) ok = 0;
        if (py_won && !py_over) ok = 0;
        if (!ok) {
            CHECK(0, "random invariant violated");
            return;
        }
        if (py_over) {
            CHECK(py_won == (left == 0), "over state consistent");
            break;
        }
        steps++;
    }
    CHECK(steps > 0, "random run stable");
    printf("  (random run: %d steps, over=%d won=%d)\n",
           steps, py_over ? 1 : 0, py_won ? 1 : 0);
}

int main(void) {
    printf("== pyramid host tests ==\n");
    test_deal();
    test_exposed();
    test_cursor();
    test_pair();
    test_cancel();
    test_king();
    test_waste_pair();
    test_recycle();
    test_dead();
    test_covered_unselectable();
    test_stock_order();
    test_deterministic();
    test_random();
    if (s_fail == 0) {
        printf("ALL PASS\n");
        return 0;
    }
    printf("%d FAILED\n", s_fail);
    return 1;
}
