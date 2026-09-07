/* GOLF SOLITAIRE 逻辑单测 — host, 零平台依赖(驱动代码在 -DCHICHU_HOST 下空转) */
#include "../../src/config.h"
#include "../../src/gfx/canvas.h"
#include "../../src/games/golf.c"
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

static bool px(int x, int y) {
    if (x < 0 || x >= (int)CCG_W || y < 0 || y >= (int)CCG_H) return false;
    int off = (y >> 3) * (int)CCG_W + x;
    return (g_fb[off] & (0x80 >> (y & 7))) != 0;
}

/* 牌编码助手: 点数 r, 花色 0 */
#define C(r) ((uint8_t)(r))

/* 清空并定制局面 */
static void fresh(void) {
    for (int c = 0; c < GO_COLS; c++)
        for (int r = 0; r < GO_ROWS; r++) go_tab[c][r] = 0;
    for (int c = 0; c < GO_COLS; c++) go_cols[c] = 0;
    go_stock_cnt = 0;
    go_waste_cnt = 0;
    go_moves = 0;
    go_sel = 0;
    go_over = false;
    go_win = false;
    go_over_full = false;
}

static void set_col(int c, uint8_t top) { go_tab[c][0] = top; go_cols[c] = 1; }
static void set_col2(int c, uint8_t top, uint8_t under) {
    go_tab[c][0] = top; go_tab[c][1] = under; go_cols[c] = 2;
}
static void set_waste(const uint8_t *w, int n) {
    for (int i = 0; i < n; i++) go_waste[i] = w[i];
    go_waste_cnt = n;
}
static void set_stock(const uint8_t *s, int n) {
    for (int i = 0; i < n; i++) go_stock[i] = s[i];
    go_stock_cnt = n;
}

static int count_tab(void) {
    int n = 0;
    for (int c = 0; c < GO_COLS; c++) n += go_cols[c];
    return n;
}

/* ---- 洗牌发牌: 52 张唯一, 分布正确, 同种子确定性 ---- */
static void test_deal(void) {
    rng_seed(&go_rng, 42);
    go_deal();
    CHECK(go_stock_cnt == GO_STOCK_MAX, "stock has 26 cards");
    CHECK(go_waste_cnt == 1, "waste starts with 1 card");
    CHECK(count_tab() == GO_TAB_CARDS, "tableau has 25 cards");
    for (int c = 0; c < GO_COLS; c++)
        CHECK(go_cols[c] == GO_ROWS, "every column has 5 cards");
    /* 52 张全局唯一 + 点数/花色合法(编码 suit<<4|rank, 最大码 61) */
    uint64_t seen = 0;
    int bad = 0;
    for (int c = 0; c < GO_COLS; c++)
        for (int r = 0; r < GO_ROWS; r++) {
            uint8_t v = go_tab[c][r];
            if (v == 0 || GO_RANK(v) < 1 || GO_RANK(v) > 13 || GO_SUIT(v) > 3) bad++;
            int idx = (GO_RANK(v) - 1) + 13 * GO_SUIT(v);
            if (seen & (1ull << idx)) bad++;
            seen |= 1ull << idx;
        }
    for (int i = 0; i < go_stock_cnt; i++) {
        uint8_t v = go_stock[i];
        if (v == 0 || GO_RANK(v) < 1 || GO_RANK(v) > 13 || GO_SUIT(v) > 3) bad++;
        int idx = (GO_RANK(v) - 1) + 13 * GO_SUIT(v);
        if (seen & (1ull << idx)) bad++;
        seen |= 1ull << idx;
    }
    for (int i = 0; i < go_waste_cnt; i++) {
        uint8_t v = go_waste[i];
        int idx = (GO_RANK(v) - 1) + 13 * GO_SUIT(v);
        if (seen & (1ull << idx)) bad++;
        seen |= 1ull << idx;
    }
    CHECK(bad == 0 && seen == 0xFFFFFFFFFFFFFull, "52 unique valid cards");
    CHECK(go_any_move() || go_stock_cnt > 0, "fresh deal not instantly dead");
    /* 同种子 → 同序列 */
    uint8_t snap[GO_COLS][GO_ROWS];
    memcpy(snap, go_tab, sizeof(snap));
    uint8_t ss[GO_STOCK_MAX];
    memcpy(ss, go_stock, sizeof(ss));
    uint8_t sw = go_waste[0];
    rng_seed(&go_rng, 42);
    go_deal();
    CHECK(memcmp(snap, go_tab, sizeof(snap)) == 0, "same seed -> same tableau");
    CHECK(memcmp(ss, go_stock, sizeof(ss)) == 0, "same seed -> same stock");
    CHECK(sw == go_waste[0], "same seed -> same waste");
    /* 不同种子 → 不同序列 */
    rng_seed(&go_rng, 43);
    go_deal();
    CHECK(memcmp(snap, go_tab, sizeof(snap)) != 0 || sw != go_waste[0],
          "different seed -> different deal");
}

/* ---- 点数相邻判定(差 1 或 K-A 环绕) ---- */
static void test_adj(void) {
    CHECK(go_adj(5, 6) && go_adj(6, 5), "consecutive ranks adjacent");
    CHECK(go_adj(1, 2) && go_adj(12, 13), "low/high consecutive adjacent");
    CHECK(go_adj(1, 13) && go_adj(13, 1), "A-K wrap adjacent");
    CHECK(!go_adj(5, 7) && !go_adj(1, 3), "gap 2 not adjacent");
    CHECK(!go_adj(13, 2), "K-2 not adjacent (no double wrap)");
    CHECK(!go_adj(4, 4), "same rank not adjacent");
    CHECK(!go_adj(1, 11) && !go_adj(2, 13), "edge cases");
}

/* ---- 列顶 → 废牌堆: 相邻可移, 不相邻拒绝, 环绕, 空废牌任意 ---- */
static void test_move_to_waste(void) {
    fresh();
    uint8_t w[] = { C(5) };
    set_waste(w, 1);
    set_col(0, C(6));
    go_sel = 0;
    key_event_t ev = { K_OK, 0, false };
    golf_on_key(&ev);
    CHECK(go_waste_cnt == 2 && go_waste[1] == C(6), "adjacent top moves to waste");
    CHECK(go_waste_top() == C(6), "waste top updated");
    CHECK(go_cols[0] == 0 && go_moves == 1, "column emptied, moves counted");

    fresh();
    w[0] = C(5);
    set_waste(w, 1);
    set_col(1, C(4));
    go_sel = 1;
    ev.key = K_OK;
    golf_on_key(&ev);
    CHECK(go_cols[1] == 0 && go_waste_top() == C(4), "rank 4 moves onto 5");

    fresh();
    w[0] = C(5);
    set_waste(w, 1);
    set_col(2, C(7));
    go_sel = 2;
    golf_on_key(&ev);
    CHECK(go_cols[2] == 1 && go_waste_cnt == 1 && go_moves == 0,
          "non-adjacent rank rejected");

    /* K-A 环绕 */
    fresh();
    w[0] = C(13);
    set_waste(w, 1);
    set_col(3, C(1));
    go_sel = 3;
    golf_on_key(&ev);
    CHECK(go_cols[3] == 0 && go_waste_top() == C(1), "A moves onto K (wrap)");
    fresh();
    w[0] = C(1);
    set_waste(w, 1);
    set_col(4, C(13));
    go_sel = 4;
    golf_on_key(&ev);
    CHECK(go_cols[4] == 0 && go_waste_top() == C(13), "K moves onto A (wrap)");

    /* 废牌堆空: 任意列顶可入 */
    fresh();
    set_col(0, C(9));
    go_sel = 0;
    golf_on_key(&ev);
    CHECK(go_cols[0] == 0 && go_waste_top() == C(9) && go_moves == 1,
          "empty waste accepts any card");

    /* 移走后翻开下一张(仍背面, 只是露出) */
    fresh();
    w[0] = C(5);
    set_waste(w, 1);
    set_col2(1, C(6), C(9));
    go_sel = 1;
    golf_on_key(&ev);
    CHECK(go_cols[1] == 1 && go_tab_top(1) == C(9), "reveals next card after move");
    CHECK(go_tab[1][1] == 0, "tail slot cleared after shift");
}

/* ---- 列空可放任意: 废牌堆顶 → 空列 ---- */
static void test_waste_to_empty_col(void) {
    fresh();
    uint8_t w[] = { C(4), C(8) };
    set_waste(w, 2);               /* 顶 = 8 */
    go_sel = 2;                    /* 空列 */
    key_event_t ev = { K_OK, 0, false };
    golf_on_key(&ev);
    CHECK(go_cols[2] == 1 && go_tab_top(2) == C(8), "waste top parked on empty col");
    CHECK(go_waste_cnt == 1 && go_waste_top() == C(4), "waste stack shrinks");
    CHECK(go_moves == 1, "park counts as a move");

    /* 废牌堆空 + 空列: 无事发生 */
    fresh();
    go_sel = 3;
    golf_on_key(&ev);
    CHECK(go_moves == 0 && go_cols[3] == 0, "empty waste + empty col: no-op");

    /* 非空列 + 废牌堆: 只能走列→废牌堆路径 */
    fresh();
    w[0] = C(7);
    set_waste(w, 1);
    set_col(0, C(7));
    go_sel = 0;
    golf_on_key(&ev);
    CHECK(go_cols[0] == 1 && go_waste_cnt == 1 && go_moves == 0,
          "non-adjacent card blocks both paths");
}

/* ---- 发牌堆翻牌: BACK 翻新废牌, 空堆无操作, 不计步 ---- */
static void test_draw(void) {
    fresh();
    uint8_t s[] = { C(9), C(4) };   /* 顶 = 9 */
    set_stock(s, 2);
    key_event_t ev = { K_BACK, 0, false };
    golf_on_key(&ev);
    CHECK(go_waste_cnt == 1 && go_waste_top() == C(9), "BACK draws stock top");
    CHECK(go_stock_cnt == 1 && go_stock[0] == C(4), "stock shrinks, next top shifts");
    CHECK(go_moves == 0, "draw not counted as move");

    fresh();
    golf_on_key(&ev);               /* 空发牌堆 */
    CHECK(go_waste_cnt == 0 && go_stock_cnt == 0, "empty stock: BACK no-op");

    /* 重复 BACK 忽略 */
    fresh();
    set_stock(s, 2);
    ev.is_repeat = true;
    golf_on_key(&ev);
    CHECK(go_stock_cnt == 2 && go_waste_cnt == 0, "repeated BACK ignored");
}

/* ---- 胜利: 清空最后一张列顶 ---- */
static void test_win(void) {
    fresh();
    uint8_t w[] = { C(3) };
    set_waste(w, 1);
    set_col(0, C(4));               /* 唯一剩牌, 相邻 */
    go_sel = 0;
    key_event_t ev = { K_OK, 0, false };
    golf_on_key(&ev);
    CHECK(count_tab() == 0, "last card removed");
    golf_render();                          /* 终局判定在 render 首行 */
    CHECK(go_over && go_win, "WIN when tableau cleared");
}

/* ---- 失败: 发牌堆耗尽且无法动(含空列可接废牌的救援判定) ---- */
static void test_fail(void) {
    /* 死局牌山: 废牌堆顶 5, 牌山顶 2,7,9,11,3 均不相邻, 无空列 */
    fresh();
    uint8_t w[] = { C(5) };
    set_waste(w, 1);
    set_col(0, C(2)); set_col(1, C(7)); set_col(2, C(9));
    set_col(3, C(11)); set_col(4, C(3));
    go_stock_cnt = 0;
    golf_render();                          /* 终局判定在 render 首行 */
    CHECK(go_over && !go_win, "deadlock with empty stock -> FAIL");
    CHECK(go_over_full, "force-full fired on fail");
    golf_render();
    CHECK(go_over_full, "force-full not repeated");

    /* 相邻牌存在 → 不死 */
    fresh();
    w[0] = C(5);
    set_waste(w, 1);
    set_col(0, C(6));
    go_stock_cnt = 0;
    golf_render();
    CHECK(!go_over, "adjacent card keeps game alive with empty stock");

    /* 空列可接废牌 → 不死 */
    fresh();
    w[0] = C(5);
    set_waste(w, 1);
    set_col(0, C(2)); set_col(1, C(7)); set_col(2, C(9)); set_col(3, C(11));
    go_stock_cnt = 0;                       /* 列 4 空 */
    golf_render();
    CHECK(!go_over, "empty column (waste can park) keeps game alive");

    /* 发牌堆还剩一张 → 翻牌机会 → 不死; 翻出致命牌 → 同帧判负。
     * 死局牌山: 顶 9, 牌山 1,4,6,12,2 互不相邻; 翻 10: |10-x| 均 >=2 且无空列 */
    fresh();
    w[0] = C(9);
    set_waste(w, 1);
    set_col(0, C(1)); set_col(1, C(4)); set_col(2, C(6));
    set_col(3, C(12)); set_col(4, C(2));
    go_stock_cnt = 0;
    golf_render();
    CHECK(go_over && !go_win, "rigged dead board fails");

    fresh();
    w[0] = C(9);
    set_waste(w, 1);
    set_col(0, C(1)); set_col(1, C(4)); set_col(2, C(6));
    set_col(3, C(12)); set_col(4, C(2));
    uint8_t s[] = { C(10) };
    set_stock(s, 1);
    golf_render();
    CHECK(!go_over, "stock remaining keeps game alive");
    key_event_t ev = { K_BACK, 0, false };
    golf_on_key(&ev);                       /* BACK 翻 10 */
    golf_render();
    CHECK(go_over && !go_win, "drawing dead card -> FAIL on next frame");
}

/* ---- 按键: 重复忽略/方向重复/WASD/钳制/N 重开/终局键 ---- */
static void test_keys(void) {
    fresh();
    uint8_t w[] = { C(5) };
    set_waste(w, 1);
    set_col(0, C(6));
    go_sel = 0;
    key_event_t ev = { K_OK, 0, true };
    golf_on_key(&ev);
    CHECK(go_moves == 0 && go_cols[0] == 1, "repeated OK ignored");

    uint8_t s[] = { C(9) };
    fresh();
    set_stock(s, 1);
    ev.key = K_BACK; ev.is_repeat = true;
    golf_on_key(&ev);
    CHECK(go_stock_cnt == 1, "repeated BACK ignored");

    fresh();
    go_sel = 0;
    ev.key = K_RIGHT; ev.is_repeat = true;
    golf_on_key(&ev);
    CHECK(go_sel == 1, "repeated RIGHT still moves cursor");
    ev.key = K_RIGHT; ev.is_repeat = false;
    golf_on_key(&ev);
    ev.key = K_RIGHT;
    golf_on_key(&ev);
    ev.key = K_RIGHT;
    golf_on_key(&ev);
    ev.key = K_RIGHT;
    golf_on_key(&ev);
    CHECK(go_sel == GO_COLS - 1, "cursor clamped at right edge");
    ev.key = K_LEFT;
    golf_on_key(&ev);
    for (int i = 0; i < 9; i++) { ev.key = K_LEFT; golf_on_key(&ev); }
    CHECK(go_sel == 0, "cursor clamped at left edge");

    ev.key = K_CHAR; ev.ch = 'd';
    golf_on_key(&ev);
    CHECK(go_sel == 1, "D moves right");
    ev.key = K_CHAR; ev.ch = 'a';
    golf_on_key(&ev);
    CHECK(go_sel == 0, "A moves left");
    ev.key = K_CHAR; ev.ch = 'w';
    golf_on_key(&ev);
    ev.key = K_CHAR; ev.ch = 's';
    golf_on_key(&ev);
    CHECK(go_sel == 0, "W/S no-op on single-row cursor");

    /* N 重开 */
    fresh();
    rng_seed(&go_rng, 7);
    go_deal();
    go_moves = 3;
    ev.key = K_CHAR; ev.ch = 'n';
    golf_on_key(&ev);
    CHECK(go_stock_cnt == GO_STOCK_MAX && go_moves == 0 && !go_over,
          "N restarts new game");

    /* 终局键: OK/N 重开, BACK/Q 退出, 重复忽略 */
    fresh();
    go_over = true; go_win = true; s_exit_request = false;
    ev.key = K_OK; ev.is_repeat = true;
    golf_on_key(&ev);
    CHECK(go_over, "repeated OK at game over ignored");
    ev.key = K_OK; ev.is_repeat = false;
    golf_on_key(&ev);
    CHECK(!go_over && go_stock_cnt == GO_STOCK_MAX, "OK at game over restarts");

    go_over = true; go_win = true;
    ev.key = K_CHAR; ev.ch = 'n';
    golf_on_key(&ev);
    CHECK(!go_over, "N at game over restarts");

    go_over = true; go_win = false; s_exit_request = false;
    ev.key = K_BACK;
    golf_on_key(&ev);
    CHECK(s_exit_request, "BACK at game over quits");
    s_exit_request = false;
    go_over = true; go_win = false;
    ev.key = K_QUIT;
    golf_on_key(&ev);
    CHECK(s_exit_request, "Q at game over quits");
}

/* ---- 渲染: HUD/牌面/背面/光标黑框/终局 HUD ---- */
static void test_render(void) {
    CHECK(GO_TAB_X + (GO_COLS - 1) * GO_PITCH_X + GO_CW <= (int)CCG_W,
          "tableau fits horizontally (295<=296)");
    CHECK(GO_TAB_Y + (GO_ROWS - 1) * GO_PITCH_Y + GO_CH <= (int)CCG_H,
          "tableau fits vertically (150<=152)");
    CHECK(GO_TAB_X - 2 >= 0 && GO_TAB_Y - 2 >= 0, "cursor frame in bounds");

    rng_seed(&go_rng, 1);
    go_deal();
    go_sel = 0; go_moves = 0; go_over = false; go_over_full = false;
    golf_render();
    {
        int title_ink = 0;
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 30; x++)
                if (px(x, y)) title_ink++;
        CHECK(title_ink > 0, "HUD title 'GOLF' renders");
    }
    CHECK(px(0, CCG_HUD_H - 1), "HUD separator line");
    /* 列 0 顶牌正面: 白底 + 黑边框 */
    CHECK(px(GO_TAB_X, GO_TAB_Y), "face-up card border black");
    CHECK(!px(GO_TAB_X + 1, GO_TAB_Y + 1), "face-up card interior white");
    /* 第 2 行背面斜纹: 边框黑, 内部有斜纹墨点 */
    int bx = GO_TAB_X + 1 * GO_PITCH_X;
    int by = GO_TAB_Y + 1 * GO_PITCH_Y;
    CHECK(px(bx, by), "face-down card border black");
    int ink = 0;
    for (int y = by + 1; y < by + GO_CH - 1; y++)
        for (int x = bx + 1; x < bx + GO_CW - 1; x++)
            if (px(x, y)) ink++;
    CHECK(ink > 0 && ink < 600, "face-down card has slash pattern, not solid");
    /* 发牌堆/废牌堆 */
    CHECK(px(GO_PILE_X, GO_PILE_Y), "stock pile border black");
    CHECK(px(GO_WASTE_X, GO_WASTE_Y), "waste pile border black");
    /* 光标: 选中列顶牌四周对称黑框(最后绘制) */
    int fx = GO_TAB_X + go_sel * GO_PITCH_X;
    CHECK(px(fx - 2, GO_TAB_Y + GO_CH / 2), "cursor frame left edge");
    CHECK(px(fx + GO_CW + 1, GO_TAB_Y + GO_CH / 2), "cursor frame right edge");
    CHECK(px(fx - 1, GO_TAB_Y - 2), "cursor frame top edge");
    CHECK(px(fx - 1, GO_TAB_Y + GO_CH + 1), "cursor frame bottom edge");
    CHECK(!px(fx - 3, GO_TAB_Y + GO_CH / 2), "no ink outside cursor frame");
    /* 光标移到列 1 */
    go_sel = 1;
    golf_render();
    CHECK(!px(fx - 2, GO_TAB_Y + GO_CH / 2), "old cursor frame cleared");
    int fx2 = GO_TAB_X + 1 * GO_PITCH_X;
    CHECK(px(fx2 - 2, GO_TAB_Y + GO_CH / 2), "cursor frame follows selection");

    /* 终局: HUD 区清空 + 结果文本 + 强制全刷一次 */
    go_over = true; go_win = true; go_over_full = false;
    golf_render();
    CHECK(!px(0, 0), "over state clears HUD area");
    int hud_ink = 0;
    for (int y = 2; y < 8; y++)
        for (int x = 2; x < 30; x++)
            if (px(x, y)) hud_ink++;
    CHECK(hud_ink > 0, "WIN! text rendered in HUD");
    CHECK(go_over_full, "force-full fired once on over");
    golf_render();
    CHECK(go_over_full, "force-full not repeated");
    go_win = false; go_over_full = false;
    golf_render();
    int fail_ink = 0;
    for (int y = 2; y < 8; y++)
        for (int x = 2; x < 50; x++)
            if (px(x, y)) fail_ink++;
    CHECK(fail_ink > 0, "NO MOVES text rendered in HUD");
}

int main(void) {
    test_deal();
    test_adj();
    test_move_to_waste();
    test_waste_to_empty_col();
    test_draw();
    test_win();
    test_fail();
    test_keys();
    test_render();
    if (s_fail == 0) { printf("ALL TESTS PASSED\n"); return 0; }
    printf("%d TEST(S) FAILED\n", s_fail);
    return 1;
}
