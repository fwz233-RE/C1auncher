/* 24 GAME 逻辑单测 — host, 零平台依赖(驱动代码在 -DCHICHU_HOST 下空转) */
#include "../../src/config.h"
#include "../../src/gfx/canvas.h"
#include "../../src/rng.h"
#include "../../src/games/game24.c"
#include <stdio.h>

static int s_fail = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); s_fail++; } \
    else { printf("ok: %s\n", msg); } \
} while (0)

/* ---- host 框架 stub ---- */
bool s_exit_request = false;
void game_set_tick_interval(uint32_t ms) { (void)ms; }

/* 像素断言辅助 */
static bool px(int x, int y) {
    if (x < 0 || x >= (int)CCG_W || y < 0 || y >= (int)CCG_H) return false;
    int off = (y >> 3) * (int)CCG_W + x;
    return (g_fb[off] & (0x80 >> (y & 7))) != 0;
}

/* ---- 运算核心: 加减乘除 + 除法限制 ---- */
static void test_merge(void) {
    t4_cards[0] = 2; t4_cards[1] = 3;
    int res;
    CHECK(t4_merge(0, 1, 0, &res) && res == 5, "2+3=5");
    CHECK(t4_merge(0, 1, 1, &res) && res == -1, "2-3=-1");
    CHECK(t4_merge(1, 0, 1, &res) && res == 1, "3-2=1 (first minus second)");
    CHECK(t4_merge(0, 1, 2, &res) && res == 6, "2*3=6");
    t4_cards[0] = 8; t4_cards[1] = 2;
    CHECK(t4_merge(0, 1, 3, &res) && res == 4, "8/2=4 exact");
    t4_cards[0] = 7; t4_cards[1] = 2;
    CHECK(!t4_merge(0, 1, 3, &res), "7/2 rejected (non-integer)");
    t4_cards[0] = 5; t4_cards[1] = 0;
    CHECK(!t4_merge(0, 1, 3, &res), "division by zero rejected");
    t4_cards[0] = 0; t4_cards[1] = 5;
    CHECK(t4_merge(0, 1, 3, &res) && res == 0, "0/5=0 allowed");
    t4_cards[0] = 6; t4_cards[1] = 4;
    CHECK(t4_merge(0, 1, 2, &res) && res == 24, "6*4=24");
}

/* ---- 合并落盘 + 胜负判定 ---- */
static void test_apply_end(void) {
    t4_n = 4;
    t4_cards[0] = 6; t4_cards[1] = 6; t4_cards[2] = 6; t4_cards[3] = 6;
    t4_over = false; t4_won = false; t4_seln = 0; t4_phase = T4_PICK; t4_cx = 0;
    t4_apply(0, 1, 12);
    CHECK(t4_n == 3 && t4_cards[0] == 12 && t4_cards[1] == 6 && t4_cards[2] == 6,
          "merge writes result and shifts left");
    CHECK(t4_seln == 0 && t4_phase == T4_PICK, "selection cleared after merge");
    CHECK(!t4_over, "3 cards: not over");
    t4_apply(0, 1, 18);
    CHECK(t4_n == 2 && !t4_over, "2 cards: not over");
    t4_apply(0, 1, 24);
    CHECK(t4_n == 1 && t4_over && t4_won, "single 24 -> WIN");

    /* 反向下标顺序(a>b) */
    t4_n = 4; t4_over = false; t4_won = false; t4_cx = 3;
    t4_cards[0] = 6; t4_cards[1] = 6; t4_cards[2] = 6; t4_cards[3] = 6;
    t4_apply(1, 0, 12);
    CHECK(t4_cards[0] == 12 && t4_cards[1] == 6 && t4_cards[2] == 6,
          "apply with a>b index order works");
    CHECK(t4_cx == 2, "cursor clamps after removal");

    /* 中途出 24 不算胜 */
    t4_n = 3; t4_over = false; t4_won = false;
    t4_cards[0] = 6; t4_cards[1] = 4; t4_cards[2] = 3;
    t4_apply(0, 1, 24);
    CHECK(t4_n == 2 && !t4_over, "intermediate 24 with 2 cards: not over");

    /* 剩 1 张非 24 -> FAIL */
    t4_n = 2; t4_over = false; t4_won = false;
    t4_cards[0] = 7; t4_cards[1] = 5;
    t4_apply(0, 1, 12);
    CHECK(t4_over && !t4_won, "single non-24 -> FAIL");
}

/* ---- 选牌状态机 ---- */
static void test_selection(void) {
    t4_n = 4; t4_phase = T4_PICK; t4_seln = 0;
    t4_sel[0] = t4_sel[1] = -1; t4_opc = 0;
    t4_toggle_sel(0);
    CHECK(t4_seln == 1 && t4_sel[0] == 0 && t4_phase == T4_PICK,
          "first select stays in pick phase");
    t4_toggle_sel(2);
    CHECK(t4_seln == 2 && t4_sel[1] == 2 && t4_phase == T4_OP,
          "second select auto-enters op phase");
    t4_toggle_sel(2);
    CHECK(t4_seln == 1 && t4_sel[0] == 0 && t4_phase == T4_PICK,
          "deselect in op phase returns to pick");
    t4_toggle_sel(1);
    CHECK(t4_seln == 2 && t4_sel[0] == 0 && t4_sel[1] == 1 && t4_phase == T4_OP,
          "reselection works");
    t4_toggle_sel(3);
    CHECK(t4_seln == 2 && t4_sel[0] == 0 && t4_sel[1] == 3,
          "third toggle replaces last selection");
    t4_toggle_sel(0);
    CHECK(t4_seln == 1 && t4_sel[0] == 3 && t4_phase == T4_PICK,
          "deselect first keeps remaining");
    /* 越界下标被拒绝(牌减少后数字键仍安全) */
    t4_n = 3;
    t4_toggle_sel(3);
    CHECK(t4_seln == 1 && t4_sel[0] == 3, "out-of-range toggle rejected");
    t4_clear_sel();
    CHECK(t4_seln == 0 && t4_phase == T4_PICK, "clear selection");
}

/* ---- 发牌 ---- */
static void test_deal(void) {
    rng_seed(&t4_rng, 99);
    t4_deal();
    CHECK(t4_n == 4 && !t4_over && !t4_won && t4_phase == T4_PICK,
          "deal resets state");
    CHECK(t4_seln == 0 && t4_sel[0] == -1 && t4_sel[1] == -1,
          "deal clears selection");
    int ok = 1;
    for (int i = 0; i < 4; i++)
        if (t4_cards[i] < 1 || t4_cards[i] > 13) ok = 0;
    CHECK(ok, "dealt cards in 1..13");
    int c0 = t4_cards[0], c1 = t4_cards[1], c2 = t4_cards[2], c3 = t4_cards[3];
    rng_seed(&t4_rng, 99);
    t4_deal();
    CHECK(c0 == t4_cards[0] && c1 == t4_cards[1] &&
          c2 == t4_cards[2] && c3 == t4_cards[3],
          "deal deterministic per seed");
}

/* ---- 按键流: 选牌->运算->合并, 取消, 提示, 终局 ---- */
static void test_keys(void) {
    t4_n = 4; t4_over = false; t4_won = false; t4_over_full = false;
    t4_cards[0] = 6; t4_cards[1] = 6; t4_cards[2] = 6; t4_cards[3] = 6;
    t4_phase = T4_PICK; t4_cx = 0; t4_seln = 0;
    t4_sel[0] = t4_sel[1] = -1; t4_opc = 0; t4_msg = NULL;
    key_event_t ev = { K_NONE, 0, false };

    ev.key = K_OK; game24_on_key(&ev);
    CHECK(t4_seln == 1 && t4_sel[0] == 0, "OK selects card under cursor");
    ev.key = K_RIGHT; game24_on_key(&ev);
    CHECK(t4_cx == 1, "RIGHT moves card cursor");
    ev.key = K_OK; game24_on_key(&ev);
    CHECK(t4_seln == 2 && t4_phase == T4_OP, "two selects -> op phase");
    ev.key = K_RIGHT; game24_on_key(&ev);   /* + -> - */
    ev.key = K_RIGHT; game24_on_key(&ev);   /* - -> * */
    CHECK(t4_opc == 2, "op cursor moves to *");
    ev.key = K_OK; game24_on_key(&ev);
    CHECK(t4_n == 3 && t4_cards[0] == 36 && t4_phase == T4_PICK && !t4_over,
          "6*6 merged to 36, back to pick");
    CHECK(t4_seln == 0, "selection cleared via keys");

    /* 运算阶段 BACK = 取消, 保留选择 */
    ev.key = K_RIGHT; game24_on_key(&ev);   /* cx 1 -> 2 */
    ev.key = K_OK; game24_on_key(&ev);      /* 选牌2 */
    ev.key = K_LEFT; game24_on_key(&ev);    /* cx 2 -> 1 */
    ev.key = K_OK; game24_on_key(&ev);      /* 选牌1 -> op 阶段 */
    CHECK(t4_phase == T4_OP, "auto op phase via keys");
    ev.key = K_BACK; game24_on_key(&ev);
    CHECK(t4_phase == T4_PICK && t4_seln == 2,
          "BACK in op phase cancels to pick, keeps selection");

    /* 除法非整除: 该步无效 + 提示 */
    t4_n = 2; t4_cards[0] = 7; t4_cards[1] = 2;
    t4_phase = T4_OP; t4_opc = 3; t4_seln = 2;
    t4_sel[0] = 0; t4_sel[1] = 1; t4_msg = NULL;
    ev.key = K_OK; game24_on_key(&ev);
    CHECK(t4_n == 2 && t4_phase == T4_PICK && t4_msg != NULL,
          "non-whole division rejected with hint");
    CHECK(t4_seln == 2, "selection kept for retry after bad division");

    /* 除零拒绝(除数为 0) */
    t4_n = 2; t4_cards[0] = 5; t4_cards[1] = 0;
    t4_phase = T4_OP; t4_opc = 3; t4_seln = 2;
    t4_sel[0] = 0; t4_sel[1] = 1; t4_msg = NULL;
    ev.key = K_OK; game24_on_key(&ev);
    CHECK(t4_n == 2 && t4_msg != NULL && !t4_over, "division by zero rejected");

    /* 数字键快选 */
    t4_n = 4; t4_phase = T4_PICK; t4_seln = 0; t4_sel[0] = t4_sel[1] = -1;
    ev.key = K_CHAR; ev.ch = '3'; game24_on_key(&ev);
    CHECK(t4_seln == 1 && t4_sel[0] == 2, "digit 3 quick-selects card 3 (index 2)");
    ev.key = K_CHAR; ev.ch = '3'; game24_on_key(&ev);
    CHECK(t4_seln == 0, "digit toggles off");
    t4_n = 3; t4_seln = 0;
    ev.key = K_CHAR; ev.ch = '4'; game24_on_key(&ev);
    CHECK(t4_seln == 0, "digit beyond card count rejected");

    /* X 清选 / N 换牌 */
    t4_phase = T4_PICK; t4_seln = 2; t4_sel[0] = 0; t4_sel[1] = 1;
    ev.key = K_CHAR; ev.ch = 'x'; game24_on_key(&ev);
    CHECK(t4_seln == 0, "X clears selection");
    ev.key = K_CHAR; ev.ch = 'n'; game24_on_key(&ev);
    CHECK(t4_n == 4 && !t4_over && t4_phase == T4_PICK, "N deals new hand");

    /* 重复按键: 确认键忽略, 方向可响应 */
    t4_cx = 1; t4_phase = T4_PICK; t4_seln = 0;
    ev.key = K_OK; ev.is_repeat = true; game24_on_key(&ev);
    CHECK(t4_seln == 0, "repeated OK ignored");
    ev.key = K_RIGHT; ev.is_repeat = true; game24_on_key(&ev);
    CHECK(t4_cx == 2, "repeated RIGHT still moves cursor");

    /* 终局按键 */
    t4_over = true; t4_won = true; t4_over_full = false;
    ev.is_repeat = false; ev.key = K_CHAR; ev.ch = 'n'; game24_on_key(&ev);
    CHECK(t4_n == 4 && !t4_over && !t4_won, "N after win starts new hand");
    t4_over = true; t4_won = false; s_exit_request = false;
    ev.key = K_BACK; game24_on_key(&ev);
    CHECK(s_exit_request, "BACK after fail quits");
    s_exit_request = false;
    t4_over = true; t4_won = true;
    ev.key = K_OK; game24_on_key(&ev);
    CHECK(t4_n == 4 && !t4_over, "OK after win retries");
}

/* ---- 渲染: 边框/光标/反白/终局 HUD ---- */
static void test_render(void) {
    CHECK(T4_OY == 24 && T4_CARD_H == 80 && T4_OP_OY == 112 && T4_STATUS_Y == 134,
          "layout constants sane");
    CHECK(T4_OX == 8 && T4_OP_OX == 50, "rows centered");
    t4_n = 4; t4_phase = T4_PICK; t4_cx = 0; t4_over = false; t4_over_full = false;
    t4_cards[0] = 13; t4_cards[1] = 7; t4_cards[2] = 3; t4_cards[3] = 1;
    t4_seln = 0; t4_sel[0] = t4_sel[1] = -1; t4_opc = 0; t4_msg = NULL;
    game24_render();
    /* 光标内圈(底色)会盖住光标所在牌的上边框, 检查非光标牌的边框 */
    CHECK(px(T4_OX + 2 * (T4_CARD_W + T4_CARD_G), T4_OY), "card border renders");
    CHECK(px(T4_OX - 2, T4_OY - 2), "cursor outer ring black on unselected card");
    CHECK(!px(T4_OX + 32, T4_OY + 5), "unselected card interior white");
    CHECK(px(0, CCG_HUD_H - 1), "HUD separator line");
    CHECK(px(1, 0), "HUD title '24 GAME' renders");
    /* 选中反白 */
    t4_toggle_sel(0);
    game24_render();
    CHECK(px(T4_OX + 32, T4_OY + 5), "selected card interior black");
    CHECK(!px(T4_OX - 2, T4_OY - 2), "cursor outer ring white on selected (contrast)");
    /* 运算阶段: 光标在 op, 当前 op 反白 + 反色环, 其余正常 */
    t4_phase = T4_OP; t4_opc = 1;
    t4_seln = 2; t4_sel[0] = 0; t4_sel[1] = 1;
    game24_render();
    int op1x = T4_OP_OX + 1 * (T4_OP_W + T4_OP_G);
    int op0x = T4_OP_OX;
    CHECK(px(op1x + 2, T4_OP_OY + 2), "current op filled black");
    CHECK(!px(op0x + 2, T4_OP_OY + 2), "other op stays white");
    CHECK(!px(op1x - 2, T4_OP_OY - 2), "op cursor outer ring white (bg black)");
    CHECK(px(op1x - 1, T4_OP_OY - 1), "op cursor inner ring black");
    CHECK(px(T4_OX + 32, T4_OY + 5), "selected cards stay inverted in op phase");
    /* 终局: HUD 区清空 + 强制全刷一次 */
    t4_over = true; t4_won = true; t4_over_full = false;
    game24_render();
    CHECK(!px(0, 0), "over state clears HUD area");
    CHECK(t4_over_full, "force-full fired once on over");
    game24_render();
    CHECK(t4_over_full, "force-full not repeated");
}

int main(void) {
    test_merge();
    test_apply_end();
    test_selection();
    test_deal();
    test_keys();
    test_render();
    if (s_fail == 0) { printf("ALL TESTS PASSED\n"); return 0; }
    printf("%d TEST(S) FAILED\n", s_fail);
    return 1;
}
