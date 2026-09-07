/* 逻辑单测 — 包含 citybuilder.c, 直接访问 cb_ 静态状态
 * 编译: cc -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror -DCHICHU_HOST \
 *       -Isrc -Isrc/gfx /tmp/test_citybuilder.c \
 *       /tmp/ccb/{display,input,time,canvas,font,font_data,pattern,rng,ui_common}.o -o /tmp/ccb/test_citybuilder */
#include "games/citybuilder.c"
#include <string.h>
#include <stdio.h>

/* 宿主 stub: 框架全局 */
bool s_exit_request = false;
void game_set_tick_interval(uint32_t ms) { (void)ms; }

static int s_fail = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); s_fail++; } \
    else { printf("ok: %s\n", msg); } \
} while (0)

/* 手工重置(不触发 render/disp) */
static void reset(void) {
    for (int y = 0; y < CB_ROWS; y++)
        for (int x = 0; x < CB_COLS; x++)
            cb_b[y][x] = 0;
    cb_cx = CB_COLS / 2;
    cb_cy = CB_ROWS / 2;
    cb_sel = CB_H;
    cb_turns = 0;
    cb_score = 0;
    cb_over = false;
    cb_over_full = false;
}

static void place_at(int x, int y) { cb_cx = x; cb_cy = y; cb_place(); }
static void remove_at(int x, int y) { cb_cx = x; cb_cy = y; cb_remove(); }

static void test_enter(void) {
    citybuilder_enter();
    int filled = 0;
    for (int y = 0; y < CB_ROWS; y++)
        for (int x = 0; x < CB_COLS; x++)
            if (cb_b[y][x]) filled++;
    CHECK(filled == 0, "enter: board empty");
    CHECK(cb_turns == 0 && cb_score == 0, "enter: turns/score zero");
    CHECK(!cb_over, "enter: not over");
    CHECK(cb_cx == 5 && cb_cy == 2, "enter: cursor center");
    CHECK(cb_sel == CB_H, "enter: default select H");
}

static void test_pair_rules(void) {
    reset();
    cb_sel = CB_H;
    place_at(0, 0);                              /* H */
    CHECK(cb_score == 0 && cb_turns == 1, "single H scores 0");
    cb_sel = CB_P;
    place_at(1, 0);                              /* P 邻 H +2 */
    CHECK(cb_score == 2, "H+P adjacent = +2");
    place_at(0, 1);                              /* P 邻 H +2 */
    CHECK(cb_score == 4, "H+P twice = +4");
    cb_sel = CB_F;
    place_at(2, 0);                              /* F 邻 P -1 */
    CHECK(cb_score == 3, "F+P adjacent = -1");
}

static void test_pair_symmetry(void) {
    reset();
    place_at(0, 0);                              /* P */
    cb_sel = CB_P;
    place_at(1, 0);
    CHECK(cb_score == 2, "P+H = +2 (symmetric)");
    reset();
    cb_sel = CB_H; place_at(0, 0);
    cb_sel = CB_F; place_at(1, 0);
    CHECK(cb_score == -2, "H+F = -2");
    reset();
    cb_sel = CB_H; place_at(0, 0);
    cb_sel = CB_S; place_at(1, 0);
    CHECK(cb_score == 1, "S+H = +1");
    reset();
    cb_sel = CB_S; place_at(0, 0);
    cb_sel = CB_F; place_at(1, 0);
    CHECK(cb_score == 1, "S+F = +1");
    /* 表对称性 */
    int asym = 0;
    for (int a = 1; a <= 4; a++)
        for (int b = 1; b <= 4; b++)
            if (cb_pair[a][b] != cb_pair[b][a]) asym++;
    CHECK(asym == 0, "pair table symmetric");
}

static void test_remove(void) {
    reset();
    cb_sel = CB_H;
    place_at(0, 0);                              /* H */
    cb_sel = CB_P;
    place_at(1, 0);                              /* P -> +2 */
    CHECK(cb_score == 2 && cb_turns == 2, "setup 2 buildings");
    remove_at(1, 0);
    CHECK(cb_score == 0 && cb_turns == 2, "remove frees score, turn not refunded");
    place_at(1, 0);                              /* 重新放置消耗新回合 */
    CHECK(cb_score == 2 && cb_turns == 3, "re-place costs a turn");
    remove_at(9, 4);                             /* 空位拆除 no-op */
    CHECK(cb_score == 2 && cb_turns == 3, "remove empty is no-op");
}

static void test_occupied_noop(void) {
    reset();
    place_at(0, 0);
    place_at(0, 0);                              /* 占用格再放 */
    CHECK(cb_turns == 1 && cb_score == 0, "place on occupied is no-op");
}

static void test_turn_limit(void) {
    reset();
    for (int i = 0; i < CB_TURNS; i++) place_at(i % CB_COLS, i / CB_COLS);
    CHECK(cb_turns == CB_TURNS && cb_over, "20th placement ends game");
    place_at(9, 4);
    CHECK(cb_turns == CB_TURNS, "place after over is no-op");
    citybuilder_enter();
    CHECK(!cb_over && cb_turns == 0, "enter after over resets (retry)");
}

static void test_sel_cycle(void) {
    reset();
    cb_sel = CB_H;
    cb_sel = cb_sel % 4 + 1;
    CHECK(cb_sel == CB_S, "SPACE cycle H->S");
    cb_sel = cb_sel % 4 + 1;
    CHECK(cb_sel == CB_P, "SPACE cycle S->P");
    cb_sel = cb_sel % 4 + 1;
    CHECK(cb_sel == CB_F, "SPACE cycle P->F");
    cb_sel = cb_sel % 4 + 1;
    CHECK(cb_sel == CB_H, "SPACE cycle F->H");
    key_event_t ev = { K_CHAR, 'p', false };
    citybuilder_on_key(&ev);
    CHECK(cb_sel == CB_P, "letter p selects park");
    ev.ch = '2';
    citybuilder_on_key(&ev);
    CHECK(cb_sel == CB_S, "digit 2 selects shop");
    ev.ch = 'f';
    citybuilder_on_key(&ev);
    CHECK(cb_sel == CB_F, "letter f selects factory");
}

static void test_cursor(void) {
    reset();
    key_event_t ev;
    for (int i = 0; i < 7; i++) { ev.key = K_RIGHT; ev.is_repeat = false; citybuilder_on_key(&ev); }
    CHECK(cb_cx == 9, "right clamps at 9");
    ev.key = K_RIGHT; ev.is_repeat = true; citybuilder_on_key(&ev);
    CHECK(cb_cx == 9, "right repeat still clamped");
    for (int i = 0; i < 12; i++) { ev.key = K_LEFT; ev.is_repeat = false; citybuilder_on_key(&ev); }
    CHECK(cb_cx == 0, "left clamps at 0");
    for (int i = 0; i < 2; i++) { ev.key = K_UP; citybuilder_on_key(&ev); }
    CHECK(cb_cy == 0, "up clamps at 0");
    for (int i = 0; i < 9; i++) { ev.key = K_DOWN; citybuilder_on_key(&ev); }
    CHECK(cb_cy == 4, "down clamps at 4");
    /* WASD */
    ev.key = K_CHAR; ev.ch = 'd'; citybuilder_on_key(&ev);
    CHECK(cb_cx == 1, "d moves right");
    ev.ch = 'a'; citybuilder_on_key(&ev);
    CHECK(cb_cx == 0, "a moves left");
    ev.ch = 'w'; citybuilder_on_key(&ev);
    CHECK(cb_cy == 3, "w moves up");
    ev.ch = 's'; citybuilder_on_key(&ev);
    CHECK(cb_sel == CB_S, "s selects shop (building letter)");
}

static void test_repeat_ignore(void) {
    reset();
    key_event_t ev = { K_OK, 0, true };
    citybuilder_on_key(&ev);
    CHECK(cb_turns == 0, "repeat OK ignored");
    ev.key = K_CHAR; ev.ch = 'h'; ev.is_repeat = true;
    citybuilder_on_key(&ev);
    CHECK(cb_sel == CB_H, "repeat letter ignored");
}

static void test_checkerboard(void) {
    reset();
    /* 4x5 H/P 棋盘格: 31 对 H-P 邻接, 每对 +2 -> 62 */
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 5; x++) {
            cb_cx = x; cb_cy = y;
            cb_sel = ((x + y) & 1) ? CB_P : CB_H;
            cb_place();
        }
    }
    CHECK(cb_turns == 20 && cb_over, "checkerboard uses all 20 turns");
    CHECK(cb_score == 62, "checkerboard H/P scores 62");
}

static void test_rating(void) {
    CHECK(strcmp(cb_rating(62), "URBAN UTOPIA") == 0, "rating 62 -> UTOPIA");
    CHECK(strcmp(cb_rating(40), "URBAN UTOPIA") == 0, "rating 40 -> UTOPIA");
    CHECK(strcmp(cb_rating(25), "THRIVING CITY") == 0, "rating 25 -> THRIVING");
    CHECK(strcmp(cb_rating(12), "A GROWING TOWN") == 0, "rating 12 -> GROWING");
    CHECK(strcmp(cb_rating(0), "FIRST STEPS") == 0, "rating 0 -> FIRST STEPS");
    CHECK(strcmp(cb_rating(-9), "URBAN RUINS") == 0, "rating -9 -> RUINS");
}

static void test_hud_fmt(void) {
    char b[24];
    cb_hud(b, 3, 12);
    CHECK(strcmp(b, "T3 S12") == 0, "hud T3 S12");
    cb_hud(b, 20, 62);
    CHECK(strcmp(b, "T20 S62") == 0, "hud T20 S62");
    cb_hud(b, 0, 0);
    CHECK(strcmp(b, "T0 S0") == 0, "hud T0 S0");
    cb_hud(b, 0, -9);
    CHECK(strcmp(b, "T0 S-9") == 0, "hud negative score");
}

static void test_quit_key(void) {
    reset();
    s_exit_request = false;
    key_event_t ev = { K_QUIT, 0, false };
    citybuilder_on_key(&ev);
    CHECK(s_exit_request, "K_QUIT sets exit request");
}

int main(void) {
    test_enter();
    test_pair_rules();
    test_pair_symmetry();
    test_remove();
    test_occupied_noop();
    test_turn_limit();
    test_sel_cycle();
    test_cursor();
    test_repeat_ignore();
    test_checkerboard();
    test_rating();
    test_hud_fmt();
    test_quit_key();
    if (s_fail) { printf("RESULT: %d FAIL\n", s_fail); return 1; }
    printf("RESULT: ALL OK\n");
    return 0;
}
