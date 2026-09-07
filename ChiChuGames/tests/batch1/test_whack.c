/* WHACK-A-MOLE 逻辑单测 — host 编译运行; 直接包含 whack.c 访问静态状态
 * 链接 canvas/font/font_data/pattern/rng/time/ui_common/input/display (-DCHICHU_HOST) */
#include "../../src/config.h"
#include "../../src/gfx/canvas.h"
#include "../../src/gfx/pattern.h"
#include "../../src/rng.h"
#include "../../src/platform/time.h"
#include "../../src/games/whack.c"
#include <stdio.h>

static int s_fail = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); s_fail++; } \
    else { printf("ok: %s\n", msg); } \
} while (0)

/* 框架全局 stub(main.c 定义) */
bool s_exit_request = false;
void game_set_tick_interval(uint32_t ms) { (void)ms; }

static int fb_pix(int x, int y) {
    if (x < 0 || x >= (int)CCG_W || y < 0 || y >= (int)CCG_H) return 0;
    return (g_fb[(y >> 3) * (int)CCG_W + x] >> (7 - (y & 7))) & 1;
}

static int region_black(int x0, int y0, int x1, int y1) {
    int n = 0;
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++)
            if (fb_pix(x, y)) n++;
    return n;
}

static void test_layout(void) {
    CHECK(WA_GRID_OX == 52 && WA_GRID_OY == 24, "grid origin centered below HUD");
    CHECK(WA_GRID_OX + 3 * WA_CELL_W <= (int)CCG_W, "grid fits width");
    CHECK(WA_GRID_OY + 3 * WA_CELL_H <= (int)CCG_H, "grid fits height");
    CHECK(WA_GRID_OY >= CCG_HUD_H, "grid below HUD");
}

static void test_new_game(void) {
    wa_new_game();
    CHECK(wa_cx == 1 && wa_cy == 1, "cursor starts center");
    CHECK(wa_mole == 9 && wa_ttl == 0, "no mole at start");
    CHECK(wa_score == 0 && wa_elapsed == 0 && !wa_over, "score/time reset");
    CHECK(!wa_over_full, "over_full flag reset");
    CHECK(wa_next >= WA_NEXT_MIN && wa_next <= WA_NEXT_MIN + WA_NEXT_RANGE - 1,
          "first spawn delay within 0.8-1.5s");
}

static void test_spawn_and_lifetime(void) {
    rng_seed(&wa_rng, 42u);
    wa_new_game();
    wa_elapsed = 0;
    wa_mole = 9;
    wa_ttl = 0;
    wa_next = 1;                 /* 下一 tick 生成 */
    whack_tick(0);
    CHECK(wa_elapsed == 100, "elapsed advances by tick");
    CHECK(wa_mole <= 8 && wa_ttl == WA_MOLE_LIFE, "mole spawned with 1.2s life");
    CHECK(wa_next >= WA_NEXT_MIN && wa_next <= WA_NEXT_MIN + WA_NEXT_RANGE - 1,
          "next spawn delay set");
    uint8_t hole = wa_mole;
    wa_next = WA_NEXT_MIN + WA_NEXT_RANGE - 1;   /* 推迟下次生成, 隔离寿命测试 */
    for (int i = 0; i < WA_MOLE_LIFE - 1; i++) whack_tick(0);
    CHECK(wa_mole == hole && wa_ttl == 1, "mole alive until final tick");
    whack_tick(0);
    CHECK(wa_mole == 9 && wa_ttl == 0, "mole disappears after 1.2s");
    /* 替换行为: 场上有地鼠时到点 → 换洞生成 + 寿命重置(单地鼠模型) */
    rng_seed(&wa_rng, 9u);
    wa_elapsed = 0;
    wa_mole = 2;
    wa_ttl = 3;
    wa_next = 1;
    whack_tick(0);
    CHECK(wa_mole != 2 && wa_ttl == WA_MOLE_LIFE, "spawn replaces active mole in another hole");
}

static void test_spawn_distinct_hole(void) {
    rng_seed(&wa_rng, 7u);
    wa_new_game();
    uint8_t prev = 5;
    int same = 0;
    for (int i = 0; i < 300; i++) {
        wa_elapsed = 0;
        wa_mole = prev;
        wa_ttl = 3;              /* 地鼠仍在场 → 新洞必须不同 */
        wa_next = 1;
        whack_tick(0);
        if (wa_mole == prev && wa_ttl == WA_MOLE_LIFE) same++;
        prev = wa_mole;
    }
    CHECK(same == 0, "spawn never reuses the active hole (300 runs)");
}

static void test_hit(void) {
    wa_new_game();
    wa_cx = 2; wa_cy = 1;        /* 洞 5 */
    wa_mole = 5; wa_ttl = 5;
    wa_score = 0;
    wa_hit();
    CHECK(wa_score == 1, "hit scores +1");
    CHECK(wa_mole == 9 && wa_ttl == 0, "hit clears mole immediately");
    wa_mole = 5; wa_ttl = 5;
    wa_cx = 0; wa_cy = 0;        /* 打到空洞 */
    wa_hit();
    CHECK(wa_score == 1 && wa_mole == 5 && wa_ttl == 5, "empty hole: no score, mole stays");
    wa_cx = 0; wa_cy = 0;
    wa_mole = 8; wa_ttl = 3;     /* 地鼠在别的洞 */
    wa_hit();
    CHECK(wa_score == 1 && wa_mole == 8, "mole elsewhere: not hit");
    wa_over = true;              /* 时间到后击打无效 */
    wa_mole = 1; wa_ttl = 9;
    wa_hit();
    CHECK(wa_score == 1 && wa_mole == 1 && wa_ttl == 9, "no scoring after time up");
    wa_over = false;
}

static void test_cursor_keys(void) {
    wa_new_game();
    wa_cx = 0; wa_cy = 0;
    key_event_t ev = {0};
    ev.key = K_LEFT; whack_on_key(&ev);
    CHECK(wa_cx == 0, "LEFT clamped at 0");
    ev.key = K_UP; whack_on_key(&ev);
    CHECK(wa_cy == 0, "UP clamped at 0");
    ev.key = K_RIGHT; whack_on_key(&ev);
    CHECK(wa_cx == 1, "RIGHT moves");
    ev.key = K_DOWN; whack_on_key(&ev);
    CHECK(wa_cy == 1, "DOWN moves");
    ev.key = K_CHAR; ev.ch = 'a'; whack_on_key(&ev);
    CHECK(wa_cx == 0, "A moves left");
    ev.key = K_CHAR; ev.ch = 's'; whack_on_key(&ev);
    CHECK(wa_cy == 2, "S moves down");
    ev.key = K_CHAR; ev.ch = 'd'; whack_on_key(&ev);
    CHECK(wa_cx == 1, "D moves right");
    ev.key = K_CHAR; ev.ch = 'w'; whack_on_key(&ev);
    CHECK(wa_cy == 1, "W moves up");
    wa_cx = 2; wa_cy = 2;
    ev.key = K_RIGHT; whack_on_key(&ev);
    ev.key = K_DOWN; whack_on_key(&ev);
    CHECK(wa_cx == 2 && wa_cy == 2, "RIGHT/DOWN clamped at 2");
    ev.key = K_CHAR; ev.ch = 'd'; whack_on_key(&ev);
    ev.key = K_CHAR; ev.ch = 's'; whack_on_key(&ev);
    CHECK(wa_cx == 2 && wa_cy == 2, "D/S clamped at 2");
    /* 重复: 确认键/字母忽略, 方向键响应 */
    wa_score = 0;
    wa_mole = (uint8_t)(wa_cy * 3 + wa_cx);
    wa_ttl = 5;
    ev.key = K_OK; ev.is_repeat = true; whack_on_key(&ev);
    CHECK(wa_score == 0 && wa_ttl == 5, "repeat OK ignored");
    ev.key = K_CHAR; ev.ch = 'n'; whack_on_key(&ev);
    CHECK(!wa_over && wa_score == 0, "repeat N ignored");
    ev.key = K_LEFT; whack_on_key(&ev);
    CHECK(wa_cx == 1, "repeat direction moves cursor");
    ev.is_repeat = false;
}

static void test_time_over(void) {
    rng_seed(&wa_rng, 3u);
    wa_new_game();
    wa_elapsed = 59000;
    wa_score = 5;
    wa_next = 1;
    wa_mole = 9; wa_ttl = 0;
    for (int i = 0; i < 10; i++) whack_tick(0);
    CHECK(wa_elapsed == 60000 && wa_over, "game over exactly at 60s");
    CHECK(wa_mole == 9 && wa_ttl == 0, "mole cleared at time up");
    uint32_t sc = wa_score;
    whack_tick(0);
    CHECK(wa_elapsed == 60000 && wa_score == sc, "tick after over is no-op");
    /* 结算: OK/N 重开, BACK/Q 退出 */
    key_event_t ev = {0};
    ev.key = K_OK;
    whack_on_key(&ev);
    CHECK(!wa_over && wa_score == 0 && wa_elapsed == 0, "OK at over restarts");
    s_exit_request = false;
    wa_over = true;
    ev.key = K_BACK;
    whack_on_key(&ev);
    CHECK(s_exit_request, "BACK at over quits");
    s_exit_request = false;
    ev.key = K_QUIT;
    whack_on_key(&ev);
    CHECK(s_exit_request, "Q at over quits");
    s_exit_request = false;
}

static void test_hud_time(void) {
    wa_new_game();
    wa_elapsed = 0;
    CHECK((WA_GAME_MS - wa_elapsed) / 1000u == 60, "TIME starts at 60");
    wa_elapsed = 12300;
    CHECK((WA_GAME_MS - wa_elapsed) / 1000u == 47, "TIME counts down");
}

static void test_render(void) {
    wa_new_game();
    wa_cx = 1; wa_cy = 1;
    wa_mole = 9; wa_ttl = 0;
    whack_render();
    CHECK(fb_pix(WA_GRID_OX + 21, WA_GRID_OY + 31) == 1, "hole dark bar visible");
    CHECK(fb_pix(WA_GRID_OX + 10, WA_GRID_OY + 10) == 0, "cell background white");
    int mx = WA_GRID_OX + WA_CELL_W + 20 + 9;   /* 洞 4(1,1) 地鼠区像素 */
    int my = WA_GRID_OY + WA_CELL_H + 8;
    CHECK(fb_pix(mx, my) == 0, "no mole: mole area blank");
    wa_mole = 4; wa_ttl = 5;
    whack_render();
    CHECK(fb_pix(mx, my) == 1, "mole (CG_MINE x3) drawn in hole 4");
    CHECK(fb_pix(WA_GRID_OX + 20 + 9, WA_GRID_OY + 8) == 0, "hole 0 mole area still blank");
    CHECK(fb_pix(WA_GRID_OX + WA_CELL_W - 2, WA_GRID_OY + WA_CELL_H - 2) == 1,
          "cursor frame drawn after cells");
    CHECK(fb_pix(0, CCG_HUD_H - 1) == 1, "HUD divider line");
    CHECK(region_black(0, 0, 35, 6) > 10, "HUD title WHACK rendered");
    /* 结束态: HUD 清底 + 左 SCORE + 右提示 */
    wa_over = true; wa_over_full = false;
    whack_render();
    CHECK(fb_pix(0, 0) == 0 && region_black(2, 1, 60, 6) > 5, "over HUD: cleared + SCORE text");
    CHECK(region_black(168, 1, 294, 6) > 10, "over HUD: RETRY/QUIT hint");
    wa_over = false;
}

int main(void) {
    disp_init();                 /* host stub: 无设备 */
    test_layout();
    test_new_game();
    test_spawn_and_lifetime();
    test_spawn_distinct_hole();
    test_hit();
    test_cursor_keys();
    test_time_over();
    test_hud_time();
    test_render();
    if (s_fail) {
        printf("FAILED: %d check(s)\n", s_fail);
        return 1;
    }
    printf("ALL PASS\n");
    return 0;
}
