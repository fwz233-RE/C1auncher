/* SIMON 逻辑单测 — host 编译运行; 直接包含 simon.c 访问静态状态
 * 链接 canvas/font/font_data/pattern/rng/time/ui_common/input/display (-DCHICHU_HOST) */
#include "../../src/config.h"
#include "../../src/gfx/canvas.h"
#include "../../src/gfx/font.h"
#include "../../src/gfx/pattern.h"
#include "../../src/rng.h"
#include "../../src/platform/time.h"
#include "../../src/games/simon.c"
#include <stdio.h>
#include <string.h>

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

/* tile 平铺到 w*h 的黑点数 */
static int tile_black(const uint8_t *t, int w, int h) {
    int n = 0;
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
            if (t[j & 7] & (1u << (i & 7))) n++;
    return n;
}

static void test_layout(void) {
    CHECK(SM_MX == 85, "horizontal margins centered (85)");
    CHECK(SM_ROW0 == CCG_HUD_H + SM_MY && SM_ROW0 == 32, "row0 at y=32");
    CHECK(SM_ROW1 == 97, "row1 at y=97");
    CHECK(SM_ROW1 + SM_BTN == (int)CCG_H, "row1 bottom flush with screen");
    int x, y;
    for (int i = 0; i < 4; i++) {
        sm_zone_xy(i, &x, &y);
        CHECK(x >= 0 && x + SM_BTN <= (int)CCG_W, "zone fits width");
        CHECK(y >= CCG_HUD_H && y + SM_BTN <= (int)CCG_H, "zone fits height");
    }
    /* 2x2 无重叠 */
    for (int a = 0; a < 4; a++) {
        for (int b = a + 1; b < 4; b++) {
            int xa, ya, xb, yb;
            sm_zone_xy(a, &xa, &ya);
            sm_zone_xy(b, &xb, &yb);
            CHECK(xa + SM_BTN <= xb || xb + SM_BTN <= xa ||
                  ya + SM_BTN <= yb || yb + SM_BTN <= ya, "zones non-overlapping");
        }
    }
    /* 4 图案两两不同 */
    for (int a = 0; a < 3; a++)
        for (int b = a + 1; b < 4; b++)
            CHECK(memcmp(pat_get(sm_pat[a]), pat_get(sm_pat[b]), 8) != 0,
                  "zone patterns pairwise distinct");
}

static void test_new_game(void) {
    sm_new_game();
    CHECK(sm_len == 1, "sequence starts at length 1");
    CHECK(sm_seq[0] <= 3, "first zone in 0..3");
    CHECK(sm_state == SM_SHOW && !sm_waiting, "starts in show state");
    CHECK(sm_show_pos == 0, "show pos 0");
    CHECK(sm_score == 0, "score reset");
    CHECK(!sm_over && !sm_over_full, "over flags reset");
    CHECK(sm_cursor == sm_seq[0], "cursor pre-set to first zone");
    CHECK(sm_flash == 0xFFu, "no flash");
}

static void test_playback_timing(void) {
    sm_new_game();
    sm_len = 3;
    sm_seq[0] = 0; sm_seq[1] = 1; sm_seq[2] = 2;
    sm_state = SM_SHOW; sm_waiting = false;
    sm_show_pos = 0;
    sm_show_t0 = 1000;
    uint64_t now = 1000;
    simon_tick(now + 100);            /* 0.1s: 未到 0.6s */
    CHECK(sm_show_pos == 0 && sm_state == SM_SHOW, "no advance before 600ms");
    simon_tick(now + 600);            /* 到 0.6s: 步进 */
    CHECK(sm_show_pos == 1, "advance at 600ms");
    simon_tick(now + 1200);
    CHECK(sm_show_pos == 2, "advance to last at 1200ms");
    simon_tick(now + 1800);           /* 末步亮完 → 停顿 */
    CHECK(sm_waiting && sm_show_pos == 2, "gap phase after last step");
    simon_tick(now + 1900);           /* 停顿中 0.1s */
    CHECK(sm_state == SM_SHOW, "still show during gap");
    simon_tick(now + 2200);           /* 400ms gap 完 → 输入 */
    CHECK(sm_state == SM_INPUT, "enter input after gap");
    CHECK(sm_in_pos == 0 && sm_cursor == sm_seq[0], "input starts at seq[0]");
}

static void test_correct_round(void) {
    sm_new_game();
    sm_len = 4;
    sm_seq[0] = 1; sm_seq[1] = 0; sm_seq[2] = 3; sm_seq[3] = 2;
    sm_state = SM_INPUT;
    sm_in_pos = 0;
    sm_score = 0;
    /* 依序按 4 区 */
    for (int i = 0; i < 4; i++) {
        sm_cursor = sm_seq[sm_in_pos];
        sm_press();
        CHECK(sm_score == (uint16_t)(i + 1), "score +1 per correct press");
    }
    CHECK(sm_in_pos == 4, "in_pos consumed all 4 (len grew to 5)");
    CHECK(sm_state == SM_SHOW, "round complete -> replay");
    CHECK(sm_len == 5, "sequence extended to 5");
    CHECK(sm_show_pos == 0 && !sm_waiting, "playback restarted at 0");
}

static void test_wrong_press(void) {
    sm_new_game();
    sm_len = 2;
    sm_seq[0] = 2; sm_seq[1] = 0;
    sm_state = SM_INPUT;
    sm_in_pos = 0;
    sm_cursor = 1;                    /* 应按 2, 按了 1 */
    uint64_t before = now_ms();
    sm_press();
    CHECK(sm_over, "wrong press -> game over");
    CHECK(sm_flash == 1 && sm_flash_until > before, "wrong zone flashes");
    CHECK(sm_score == 0, "no score for wrong press");
    simon_tick(sm_flash_until + 1);   /* 闪烁到时熄灭 */
    CHECK(sm_flash == 0xFFu, "flash cleared by tick");
}

static void test_ok_ignored_during_show(void) {
    sm_new_game();
    sm_len = 3;
    sm_seq[0] = 1;
    sm_state = SM_SHOW;
    sm_waiting = false;
    sm_in_pos = 0;
    sm_cursor = 3;
    key_event_t ev = {0};
    ev.key = K_OK;
    simon_on_key(&ev);
    CHECK(sm_state == SM_SHOW && sm_score == 0, "OK ignored while showing");
}

static void test_cursor_moves(void) {
    sm_new_game();
    sm_state = SM_INPUT;
    key_event_t ev = {0};
    sm_cursor = 0;
    ev.key = K_LEFT; simon_on_key(&ev);
    CHECK(sm_cursor == 0, "LEFT clamped at 0");
    ev.key = K_UP; simon_on_key(&ev);
    CHECK(sm_cursor == 0, "UP clamped at 0");
    ev.key = K_RIGHT; simon_on_key(&ev);
    CHECK(sm_cursor == 1, "RIGHT to zone 1");
    ev.key = K_DOWN; simon_on_key(&ev);
    CHECK(sm_cursor == 3, "DOWN to zone 3");
    ev.key = K_CHAR; ev.ch = 'a'; simon_on_key(&ev);
    CHECK(sm_cursor == 2, "A to zone 2");
    ev.key = K_CHAR; ev.ch = 'w'; simon_on_key(&ev);
    CHECK(sm_cursor == 0, "W to zone 0");
    ev.key = K_CHAR; ev.ch = 's'; simon_on_key(&ev);
    CHECK(sm_cursor == 2, "S to zone 2");
    ev.key = K_CHAR; ev.ch = 'd'; simon_on_key(&ev);
    CHECK(sm_cursor == 3, "D to zone 3");
    ev.key = K_RIGHT; simon_on_key(&ev);
    CHECK(sm_cursor == 3, "RIGHT clamped at 3");
    ev.key = K_DOWN; simon_on_key(&ev);
    CHECK(sm_cursor == 3, "DOWN clamped at 3");
}

static void test_repeat_ignored(void) {
    sm_new_game();
    sm_len = 1;
    sm_seq[0] = 0;
    sm_state = SM_INPUT;
    sm_in_pos = 0;
    sm_cursor = 2;                    /* 按错区, 但事件是 repeat */
    key_event_t ev = {0};
    ev.key = K_OK; ev.is_repeat = true;
    simon_on_key(&ev);
    CHECK(sm_score == 0 && !sm_over, "repeat OK ignored");
    sm_cursor = 0;
    ev.key = K_OK; ev.is_repeat = true;
    simon_on_key(&ev);
    CHECK(sm_score == 0, "repeat OK does not confirm correct zone");
    /* 方向键重复允许 */
    ev.key = K_RIGHT; ev.is_repeat = true;
    simon_on_key(&ev);
    CHECK(sm_cursor == 1, "repeat direction still moves");
}

static void test_sequence_guard(void) {
    rng_seed(&sm_rng, 4242u);
    /* 200 轮: 以 2 连开场追加 1 个, 绝不允许出现 3 连 */
    int triples = 0;
    for (int round = 0; round < 200; round++) {
        sm_len = 2;
        sm_seq[0] = 1; sm_seq[1] = 1;
        sm_append();
        CHECK(sm_len == 3, "append accepted");
        uint8_t n = sm_len;
        if (sm_seq[n - 1] == sm_seq[n - 2] && sm_seq[n - 2] == sm_seq[n - 3])
            triples++;
        CHECK(sm_seq[n - 1] <= 3, "zones in 0..3");
    }
    CHECK(triples == 0, "no 3-in-a-row zones (200 appends)");
    /* 全长 60 序列整体扫描 */
    sm_len = 0;
    while (sm_len < SM_MAX) sm_append();
    int triples2 = 0;
    for (uint8_t i = 2; i < sm_len; i++)
        if (sm_seq[i] == sm_seq[i - 1] && sm_seq[i - 1] == sm_seq[i - 2])
            triples2++;
    CHECK(triples2 == 0 && sm_len == SM_MAX, "full-length sequence has no triples");
}

static void test_max_len_cap(void) {
    sm_new_game();
    sm_len = SM_MAX;
    for (int i = 0; i < SM_MAX; i++) sm_seq[i] = (uint8_t)(i & 3);
    sm_state = SM_INPUT;
    sm_in_pos = SM_MAX - 1;
    sm_cursor = sm_seq[SM_MAX - 1];
    sm_score = 0;
    sm_press();
    CHECK(sm_state == SM_SHOW, "round at max length replays");
    CHECK(sm_len == SM_MAX, "length capped at SM_MAX");
    CHECK(sm_score == 1, "last press still scores");
}

static void test_over_keys(void) {
    sm_new_game();
    sm_over = true;
    key_event_t ev = {0};
    s_exit_request = false;
    ev.key = K_QUIT; simon_on_key(&ev);
    CHECK(s_exit_request, "Q exits at game over");
    s_exit_request = false;
    ev.key = K_OK; simon_on_key(&ev);   /* OK 重开 */
    CHECK(!sm_over && sm_score == 0 && sm_len == 1, "OK restarts at over");
    sm_over = true;
    s_exit_request = false;
    ev.key = K_BACK; simon_on_key(&ev);
    CHECK(s_exit_request, "BACK exits at game over");
    s_exit_request = false;
    sm_over = true;
    ev.key = K_CHAR; ev.ch = 'n'; simon_on_key(&ev);
    CHECK(!sm_over && sm_len == 1, "N restarts at over");
    sm_over = false;
    ev.key = K_CHAR; ev.ch = 'n'; simon_on_key(&ev);   /* 游戏中 N 重开 */
    CHECK(sm_len == 1 && sm_score == 0, "N restarts mid-game");
}

static void test_render_zones(void) {
    sm_new_game();
    /* 强制: 输入态, 光标在区 3 → 区 3 反色, 其余正常 */
    sm_state = SM_INPUT;
    sm_cursor = 3;
    simon_render();
    int x, y;
    for (int i = 0; i < 4; i++) {
        sm_zone_xy(i, &x, &y);
        int inner = region_black(x + 3, y + 3, x + SM_BTN - 4, y + SM_BTN - 4);
        const uint8_t *t = pat_get(sm_pat[i]);
        int expect = (i == 3)
            ? (SM_BTN - 6) * (SM_BTN - 6) - tile_black(t, SM_BTN - 6, SM_BTN - 6)
            : tile_black(t, SM_BTN - 6, SM_BTN - 6);
        CHECK(inner == expect, "zone inner density matches (cursor inverted)");
        if (i == 3) {
            /* 光标双层边框 */
            CHECK(region_black(x - 2, y - 2, x + SM_BTN + 1, y - 1) >= 55,
                  "cursor top frame drawn");
        }
    }
}

static void test_render_hud(void) {
    sm_new_game();
    sm_state = SM_INPUT;
    sm_in_pos = 0;
    sm_len = 1;
    simon_render();
    CHECK(region_black(0, 0, 28, 7) > 0, "HUD title SIMON drawn");
    CHECK(region_black(0, 17, (int)CCG_W - 1, 24) > 0, "status line drawn");
    CHECK(region_black(0, CCG_HUD_H - 1, (int)CCG_W - 1, CCG_HUD_H - 1) > 0,
          "HUD separator line drawn");
    /* 游戏结束: HUD 清底重绘 FAIL 文本 + 右上提示 */
    sm_over = true;
    sm_score = 12;
    sm_flash = 0xFFu;
    sm_over_full = false;
    simon_render();
    CHECK(region_black(2, 2, 100, 9) > 0, "FAIL text drawn at over");
    CHECK(region_black((int)CCG_W - 130, 2, (int)CCG_W - 2, 9) > 0,
          "retry hint drawn at over");
    CHECK(sm_over_full, "force-full issued once at over");
    /* 墙内无游戏结束提示文字: HUD 之外仅 4 区图案 */
}

int main(void) {
    printf("== SIMON host tests ==\n");
    test_layout();
    test_new_game();
    test_playback_timing();
    test_correct_round();
    test_wrong_press();
    test_ok_ignored_during_show();
    test_cursor_moves();
    test_repeat_ignored();
    test_sequence_guard();
    test_max_len_cap();
    test_over_keys();
    test_render_zones();
    test_render_hud();
    printf(s_fail ? "== %d FAILURES ==\n" : "== ALL PASS ==\n", s_fail);
    return s_fail ? 1 : 0;
}
