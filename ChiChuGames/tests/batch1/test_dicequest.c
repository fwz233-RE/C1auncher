/* DICE QUEST 逻辑单测 — host, 直接包含游戏源文件(静态状态可访问)
 * 链接 canvas/font/font_data/pattern/rng/time/ui_common/input/display (-DCHICHU_HOST)
 * cc -std=c11 -O2 -Wall -Wextra -DCHICHU_HOST -I<root>/src -I<root>/src/gfx
 *    /tmp/test_dicequest.c <root>/src/gfx/canvas.c <root>/src/gfx/font.c
 *    <root>/src/gfx/font_data.c <root>/src/gfx/pattern.c <root>/src/rng.c
 *    <root>/src/platform/time.c <root>/src/ui/ui_common.c
 *    <root>/src/platform/input.c <root>/src/platform/display.c -o /tmp/test_dicequest
 */
#include "../../src/config.h"
#include "../../src/gfx/canvas.h"
#include "../../src/games/dicequest.c"
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

static int g_hit_seed = -1, g_win_seed = -1, g_gold_seed = -1, g_key_seed = -1;

static bool px(int x, int y) {
    if (x < 0 || x >= (int)CCG_W || y < 0 || y >= (int)CCG_H) return false;
    int off = (y >> 3) * (int)CCG_W + x;
    return (g_fb[off] & (uint8_t)(0x80 >> (y & 7))) != 0;
}

static key_event_t press(ccg_key k) {
    key_event_t ev;
    memset(&ev, 0, sizeof ev);
    ev.key = k;
    return ev;
}

static key_event_t char_key(char c) {
    key_event_t ev;
    memset(&ev, 0, sizeof ev);
    ev.key = K_CHAR;
    ev.ch = (uint8_t)c;
    return ev;
}

/* 手摆固定关卡: 玩家(0,0) 宝石(1,0) 门(2,0) 事件(0,1); 其余事件禁用 */
static void manual_level(void) {
    dq_px = 0; dq_py = 0;
    dq_gx = 1; dq_gy = 0;
    dq_dx = 2; dq_dy = 0;
    dq_evx[0] = 0; dq_evy[0] = 1;
    dq_evx[1] = -1; dq_evy[1] = -1;
    dq_evx[2] = -1; dq_evy[2] = -1;
    dq_have_gem = false;
    dq_hp = DQ_MAX_HP;
    dq_die = 0;
    dq_lv = 1;
    dq_gold = 0;
    dq_keys = 0;
    dq_over = false;
    dq_win = false;
    dq_msg[0] = 0;
}

/* 关卡落点不变量: 全在界内, 互不重叠, 不在玩家起点 */
static bool place_ok(void) {
    int sx[6] = { dq_dx, dq_gx, dq_evx[0], dq_evx[1], dq_evx[2], 0 };
    int sy[6] = { dq_dy, dq_gy, dq_evy[0], dq_evy[1], dq_evy[2], 0 };
    for (int i = 0; i < 5; i++) {
        if (sx[i] < 0 || sx[i] >= DQ_COLS || sy[i] < 0 || sy[i] >= DQ_ROWS) return false;
        if (sx[i] == 0 && sy[i] == 0) return false;
        for (int j = i + 1; j < 6; j++)
            if (sx[i] == sx[j] && sy[i] == sy[j]) return false;
    }
    return true;
}

/* ---- 1. 关卡生成 ---- */
static void send(key_event_t ev) { dicequest_on_key(&ev); }

static void test_place(void) {
    rng_seed(&dq_rng, 12345);
    int ok_all = 1;
    for (int lv = 0; lv < 30; lv++) {
        dq_new_level();
        if (!place_ok()) ok_all = 0;
        if (dq_px != 0 || dq_py != 0) ok_all = 0;
        if (dq_hp != DQ_MAX_HP || dq_have_gem || dq_die != 0) ok_all = 0;
    }
    CHECK(ok_all, "30 random levels: placement/state invariants");
}

/* ---- 2. 移动边界与宝石 ---- */
static void test_move(void) {
    manual_level();
    dq_try_move(0, -1);                 /* 上出界 */
    CHECK(dq_px == 0 && dq_py == 0, "blocked at top edge");
    dq_try_move(-1, 0);                 /* 左出界 */
    CHECK(dq_px == 0 && dq_py == 0, "blocked at left edge");
    dq_try_move(1, 0);                  /* → 宝石 */
    CHECK(dq_px == 1 && dq_py == 0, "walked onto gem");
    CHECK(dq_have_gem, "gem collected on step");
    CHECK(dq_gold == 10, "gem gives +10 gold");
    CHECK(strstr(dq_msg, "GEM") != NULL, "gem message shown");
    /* 已收宝石后回到原宝石格: 无重复奖励 */
    dq_try_move(-1, 0);
    dq_try_move(1, 0);
    CHECK(dq_gold == 10, "gem not re-collected");
    /* 走空格 */
    dq_try_move(0, 1);
    CHECK(dq_px == 1 && dq_py == 1, "walked onto empty tile");
}

/* ---- 3. 门: 锁/开 ---- */
static void test_door(void) {
    manual_level();
    dq_px = 1; dq_py = 0;
    dq_try_move(1, 0);                  /* 门(未收宝石) */
    CHECK(dq_px == 2 && dq_py == 0, "door tile walkable");
    CHECK(dq_lv == 1, "locked door keeps level");
    CHECK(strstr(dq_msg, "NEEDS GEM") != NULL, "locked door message");
    /* 收宝石 → 门 → 过关 */
    dq_px = 1; dq_py = 0;
    dq_have_gem = true;
    dq_keys = 2;
    dq_gold = 10;
    dq_try_move(1, 0);
    CHECK(dq_lv == 2, "open door advances level");
    CHECK(dq_gold == 20, "keys bonus: 2 keys x5 +10 gold");
    CHECK(dq_keys == 0, "keys consumed on door pass");
    CHECK(dq_px == 0 && dq_py == 0, "player reset on new level");
    CHECK(dq_hp == DQ_MAX_HP, "hp refilled on new level");
    CHECK(!dq_have_gem, "gem reset on new level");
    CHECK(place_ok(), "new level placement ok");
    CHECK(strstr(dq_msg, "KEYS +10") != NULL, "keys bonus message");
    /* 无钥匙过关: 普通消息 */
    manual_level();
    dq_px = 1; dq_py = 0;
    dq_have_gem = true;
    dq_try_move(1, 0);
    CHECK(dq_lv == 2 && strcmp(dq_msg, "LEVEL UP!") == 0, "plain level-up message");
}

/* ---- 4. 胜利: 第 10 关过关 ---- */
static void test_win(void) {
    manual_level();
    dq_lv = DQ_MAX_LV;
    dq_px = 1; dq_py = 0;
    dq_have_gem = true;
    dq_gold = 42;
    dq_try_move(1, 0);
    CHECK(dq_over && dq_win, "win after clearing level 10");
    CHECK(dq_lv == DQ_MAX_LV + 1, "level counter past max");
    dicequest_render();
    CHECK(dq_over_full, "over triggers force-full once");
    dicequest_render();
    CHECK(dq_over_full, "force-full not repeated");
}

/* ---- 5. 事件掷骰四类结果 ---- */
static void test_rolls(void) {
    for (uint32_t s = 1; s < 300000u &&
         (g_hit_seed < 0 || g_win_seed < 0 || g_gold_seed < 0 || g_key_seed < 0); s++) {
        dq_rng.s = s;
        dq_hp = 5; dq_gold = 0; dq_keys = 0; dq_over = false;
        dq_do_roll();
        if (dq_hp == 4 && g_hit_seed < 0) g_hit_seed = (int)s;
        if (dq_gold == 3 && g_win_seed < 0) g_win_seed = (int)s;
        if (dq_gold == 2 && g_gold_seed < 0) g_gold_seed = (int)s;
        if (dq_keys == 1 && g_key_seed < 0) g_key_seed = (int)s;
    }
    CHECK(g_hit_seed > 0, "found enemy-hit seed");
    CHECK(g_win_seed > 0, "found enemy-win seed");
    CHECK(g_gold_seed > 0, "found gold seed");
    CHECK(g_key_seed > 0, "found key seed");

    dq_rng.s = (uint32_t)g_hit_seed;
    dq_hp = 5; dq_gold = 0; dq_keys = 0; dq_over = false;
    dq_do_roll();
    CHECK(dq_hp == 4 && dq_gold == 0 && dq_keys == 0, "1-2 enemy: battle 1-3 -> -1 hp");
    CHECK(dq_die >= 1 && dq_die <= 3, "battle die 1-3 on hit");
    CHECK(strstr(dq_msg, "ENEMY") != NULL, "enemy hit message");

    dq_rng.s = (uint32_t)g_win_seed;
    dq_hp = 5; dq_gold = 0; dq_keys = 0;
    dq_do_roll();
    CHECK(dq_hp == 5 && dq_gold == 3, "1-2 enemy: battle 4-6 -> +3 gold");
    CHECK(dq_die >= 4 && dq_die <= 6, "battle die 4-6 on win");
    CHECK(strstr(dq_msg, "WIN") != NULL, "enemy win message");

    dq_rng.s = (uint32_t)g_gold_seed;
    dq_hp = 5; dq_gold = 0; dq_keys = 0;
    dq_do_roll();
    CHECK(dq_gold == 2 && dq_hp == 5 && dq_keys == 0, "3-4 roll -> +2 gold");
    CHECK(dq_die >= 3 && dq_die <= 4, "gold roll die 3-4");
    CHECK(strcmp(dq_msg, "GOLD +2") == 0, "gold message exact");

    dq_rng.s = (uint32_t)g_key_seed;
    dq_hp = 5; dq_gold = 0; dq_keys = 0;
    dq_do_roll();
    CHECK(dq_keys == 1 && dq_gold == 0 && dq_hp == 5, "5-6 roll -> +1 key");
    CHECK(dq_die >= 5 && dq_die <= 6, "key roll die 5-6");
    CHECK(strcmp(dq_msg, "KEY +1") == 0, "key message exact");
}

/* ---- 6. 生命 0 → FAIL ---- */
static void test_death(void) {
    manual_level();
    dq_rng.s = (uint32_t)g_hit_seed;
    dq_hp = 1;
    dq_do_roll();
    CHECK(dq_hp == 0 && dq_over && !dq_win, "hp 0 -> FAILED");
    CHECK(!dq_over_full, "fail pending full refresh");
    dicequest_render();
    CHECK(dq_over_full, "fail forces full once");
}

/* ---- 7. 事件格可重复触发 ---- */
static void test_retrigger(void) {
    manual_level();                     /* 事件在 (0,1) */
    dq_rng.s = (uint32_t)g_gold_seed;
    dq_try_move(0, 1);                  /* 踩入事件 */
    CHECK(dq_gold == 2, "event triggers on first step");
    dq_rng.s = (uint32_t)g_key_seed;
    dq_try_move(0, 1);                  /* → (0,2) 空格 */
    CHECK(dq_keys == 0, "empty tile no trigger");
    dq_try_move(0, -1);                 /* 再踩事件 → 再掷 */
    CHECK(dq_keys == 1 && dq_gold == 2, "event re-triggers on re-entry");
}

/* ---- 8. 结束键位 ---- */
static void test_over_keys(void) {
    manual_level();
    dq_over = true;
    dq_win = false;
    send(press(K_OK));
    CHECK(!dq_over && dq_lv == 1 && dq_hp == DQ_MAX_HP && dq_gold == 0,
          "OK retries fresh game");
    CHECK(dq_px == 0 && dq_py == 0 && !dq_have_gem, "retry resets level state");

    manual_level();
    dq_over = true;
    s_exit_request = false;
    send(press(K_BACK));
    CHECK(s_exit_request, "BACK in over exits");

    s_exit_request = false;
    send(press(K_QUIT));
    CHECK(s_exit_request, "Q in over exits");

    /* 游戏内 Q 退出 / N 新局 */
    s_exit_request = false;
    manual_level();
    send(press(K_QUIT));
    CHECK(s_exit_request, "Q quits mid-game");

    manual_level();
    send(char_key('n'));
    CHECK(dq_lv == 1 && dq_hp == DQ_MAX_HP && dq_gold == 0, "N restarts mid-game");
}

/* ---- 9. 方向键/WASD 映射 + 重复忽略 ---- */
static void test_keys(void) {
    manual_level();                     /* 玩家(0,0) 宝石(1,0) 门(2,0) */
    dq_dx = 7; dq_dy = 4;               /* 门挪远, 避免 K_RIGHT 踩宝石后开门过关 */
    send(press(K_RIGHT));               /* → 宝石 */
    CHECK(dq_px == 1 && dq_py == 0, "K_RIGHT moves (onto gem)");
    key_event_t rep = press(K_RIGHT);
    rep.is_repeat = true;
    dicequest_on_key(&rep);             /* 重复方向 → 仍移动 */
    CHECK(dq_px == 2 && dq_py == 0, "repeat right moves again");
    CHECK(dq_lv == 1, "no door advance on repeat move");

    manual_level();
    send(press(K_RIGHT));
    send(press(K_DOWN));
    CHECK(dq_px == 1 && dq_py == 1, "arrow walk path");

    manual_level();
    send(char_key('d'));
    CHECK(dq_px == 1 && dq_py == 0, "WASD d moves");
    key_event_t rep2 = char_key('a');
    rep2.is_repeat = true;
    dicequest_on_key(&rep2);
    CHECK(dq_px == 1 && dq_py == 0, "repeat char ignored");
    send(char_key('a'));
    CHECK(dq_px == 0 && dq_py == 0, "WASD a moves once");

    key_event_t okr = press(K_OK);
    okr.is_repeat = true;
    dicequest_on_key(&okr);
    CHECK(dq_px == 0 && dq_py == 0, "repeat OK ignored");
}

/* ---- 10. HUD 统计串与消息缓冲 ---- */
static void test_strings(void) {
    manual_level();
    dq_lv = 10; dq_hp = 5; dq_gold = 123;
    char st[32];
    dq_stats(st, (int)sizeof st);
    CHECK(strcmp(st, "LV 10 HP 5 GOLD 123") == 0, "stats without key");
    dq_keys = 3;
    dq_stats(st, (int)sizeof st);
    CHECK(strcmp(st, "LV 10 HP 5 GOLD 123 KEY 3") == 0, "stats with key");

    dq_msg_set_int("LEVEL UP! KEYS +", 15);
    CHECK(strcmp(dq_msg, "LEVEL UP! KEYS +15") == 0, "msg set_int");
    dq_msg_set("ENEMY! WIN +3 GOLD");
    CHECK(strcmp(dq_msg, "ENEMY! WIN +3 GOLD") == 0, "msg set");
}

/* ---- 11. 渲染冒烟: 图标像素位置 ---- */
static void test_render(void) {
    manual_level();                     /* 玩家(0,0) 宝石(1,0) 门(2,0) 事件(0,1) */
    dq_have_gem = false;
    dq_die = 0;
    dicequest_render();
    int hx = DQ_OX + 5, hy = DQ_OY + 3;         /* 玩家头 */
    CHECK(px(hx, hy), "player head pixel");
    CHECK(px(hx + 1, hy + 5), "player head interior pixel");
    int gx = DQ_OX + DQ_CELL + 12, gy = DQ_OY + 2;  /* 宝石尖端 */
    CHECK(px(gx, gy), "gem tip pixel");
    int gx2 = DQ_OX + DQ_CELL + 3, gy2 = DQ_OY + 11; /* 宝石最宽行 */
    CHECK(px(gx2, gy2), "gem widest row pixel");
    int dx = DQ_OX + 2 * DQ_CELL + 19, dy = DQ_OY + 20; /* 锁门实心角 */
    CHECK(px(dx, dy), "locked door body pixel");
    int khx = DQ_OX + 2 * DQ_CELL + 10, khy = DQ_OY + 9; /* 锁孔白 */
    CHECK(!px(khx, khy), "door keyhole cutout white");
    CHECK(px(DQ_OX, DQ_OY), "board border pixel");
    CHECK(px(100, CCG_HUD_H - 1), "HUD divider pixel");
    /* 门开渲染: 描边门 */
    dq_have_gem = true;
    dicequest_render();
    CHECK(!px(DQ_OX + 2 * DQ_CELL + 8, DQ_OY + 15), "open door body white (outline)");
    /* 骰子面 4 画 3 点 */
    dq_have_gem = false;
    dq_die = 4;
    dicequest_render();
    int p1x = 252 + 4 + 0 * 6 + 1, p1y = 64 + 4 + 0 * 6 + 1;
    int p2x = 252 + 4 + 2 * 6 + 1, p2y = 64 + 4 + 2 * 6 + 1;
    int c3x = 252 + 4 + 1 * 6 + 1, c3y = 64 + 4 + 1 * 6 + 1;
    CHECK(px(p1x, p1y) && px(p2x, p2y) && !px(c3x, c3y), "die face 4 pips correct");
    /* 玩家移动后渲染 */
    dq_px = 3; dq_py = 2;
    dicequest_render();
    int mhx = DQ_OX + 3 * DQ_CELL + 5, mhy = DQ_OY + 2 * DQ_CELL + 3;
    CHECK(px(mhx, mhy), "player redrawn at new cell");
}

int main(void) {
    test_place();
    test_move();
    test_door();
    test_win();
    test_rolls();
    test_death();
    test_retrigger();
    test_over_keys();
    test_keys();
    test_strings();
    test_render();
    if (s_fail == 0) { printf("ALL TESTS PASSED\n"); return 0; }
    printf("%d TEST(S) FAILED\n", s_fail);
    return 1;
}
