/* BACKGAMMON 逻辑单测 — host 编译, 包含游戏 .c 直接访问静态状态 */
#include "../../src/games/backgammon.c"
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
    int off = (y >> 3) * (int)CCG_W + x;
    return (g_fb[off] & (uint8_t)(0x80 >> (y & 7))) != 0;
}

static int count_black(int x0, int y0, int x1, int y1) {
    int n = 0;
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++)
            if (px(x, y)) n++;
    return n;
}

static void reset_boards(void) {
    for (int i = 0; i < BG_N; i++) { bg_p[i] = 0; bg_a[i] = 0; }
    bg_pfin = bg_afin = 0;
    bg_over = false;
    bg_over_full = false;
    bg_winner = 0;
}

/* ---- 落子合法性 ---- */
static void test_legal(void) {
    reset_boards();
    /* 玩家 10 → +6 = 16: 空 → 合法 */
    CHECK(bg_legal(10, 6, true), "player empty landing legal");
    /* 目标格对方单子: 可同格 */
    bg_a[16] = 1;
    CHECK(bg_legal(10, 6, true), "player landing on single AI legal");
    /* 对方 2+ 成墙: 阻挡 */
    bg_a[16] = 2;
    CHECK(!bg_legal(10, 6, true), "player blocked by 2-stack");
    /* 中间格有对方墙不影响(可越过) */
    bg_a[16] = 0;
    bg_a[12] = 2;
    CHECK(bg_legal(10, 6, true), "player jumps over wall cell");
    /* 越过终点恒合法 */
    bg_a[16] = 2;
    CHECK(bg_legal(21, 6, true), "player overshoot finish always legal");
    /* 最后一格(23)被墙: 2 号骰不能落 */
    bg_a[23] = 2;
    CHECK(!bg_legal(21, 2, true), "player cannot land on wall at last cell");
    bg_a[23] = 1;
    CHECK(bg_legal(21, 2, true), "player lands last cell with single AI");
    /* AI 侧镜像 */
    reset_boards();
    CHECK(bg_legal(10, 6, false), "AI empty landing legal");
    bg_p[4] = 1;
    CHECK(bg_legal(10, 6, false), "AI landing on single player legal");
    bg_p[4] = 2;
    CHECK(!bg_legal(10, 6, false), "AI blocked by player 2-stack");
    bg_p[4] = 0;
    CHECK(bg_legal(2, 6, false), "AI overshoot finish always legal");
}

/* ---- 玩家用骰选择(最大合法 → 次大 → 强制跳) ---- */
static void test_piece_die(void) {
    reset_boards();
    bg_p[5] = 1;
    bg_rem[0] = 4; bg_rem[1] = 3;
    bg_a[9] = 2;                                   /* 大骰 4 → 9 被挡 */
    CHECK(bg_piece_die(5) == 3, "die: falls back to smaller legal die");
    bg_a[8] = 2;                                   /* 两骰都挡 → 强制跳大骰 */
    CHECK(bg_piece_die(5) == 4, "die: forced jump with max die");
    bg_a[8] = 0; bg_a[9] = 0;
    bg_p[22] = 1;
    CHECK(bg_piece_die(22) == 4, "die: finish move always legal");
    bg_rem[0] = 3; bg_rem[1] = 3;
    CHECK(bg_piece_die(22) == 3, "die: double roll works");
    /* 只剩一颗骰时 lo=0 不得被取为 0(共享格回归 bug) */
    bg_rem[0] = 0; bg_rem[1] = 3;
    bg_a[5 + 3] = 2;                               /* 3 → 8 被挡 */
    bg_a[5] = 1;                                   /* 共享格 */
    CHECK(bg_piece_die(5) == 3, "die: single die forces jump, never 0");
}

/* ---- 玩家回合流程: OK 掷骰 → 选子移动 ×2 → AI 回合 ---- */
static void test_player_flow(void) {
    backgammon_enter();
    CHECK(bg_phase == BG_PH_ROLL, "enter: phase ROLL");
    CHECK(bg_p[0] == BG_PER && bg_a[BG_LAST] == BG_PER, "enter: 6 pieces each end");
    CHECK(bg_pfin == 0 && bg_afin == 0, "enter: no finishers");
    /* OK 掷骰 */
    key_event_t ev = { K_OK, 0, false };
    backgammon_on_key(&ev);
    CHECK(bg_phase == BG_PH_SELECT, "OK: roll -> SELECT");
    CHECK(bg_dice[0] >= 1 && bg_dice[0] <= 6 && bg_dice[1] >= 1 && bg_dice[1] <= 6,
          "OK: dice in 1..6");
    CHECK(bg_rem[0] == bg_dice[0] && bg_rem[1] == bg_dice[1], "OK: rem == dice");
    CHECK(bg_p[bg_cur] > 0, "OK: cursor snapped to a piece");
    /* 重复事件必须忽略 */
    bg_phase = BG_PH_ROLL;
    key_event_t rep = { K_OK, 0, true };
    backgammon_on_key(&rep);
    CHECK(bg_phase == BG_PH_ROLL, "repeat OK ignored");
    /* 单子在场, 确定骰面 {4,3} */
    reset_boards();
    bg_p[0] = 1;
    bg_pfin = 5;
    bg_rem[0] = 4; bg_rem[1] = 3;
    bg_dice[0] = 4; bg_dice[1] = 3;
    bg_phase = BG_PH_SELECT;
    bg_cur = 0;
    ev.key = K_OK;
    backgammon_on_key(&ev);
    CHECK(bg_p[4] == 1 && bg_p[0] == 0, "move: piece advanced by max die 4");
    CHECK(bg_rem[0] == 0 && bg_rem[1] == 3, "move: max die consumed");
    CHECK(bg_phase == BG_PH_SELECT, "move: still SELECT with die left");
    CHECK(bg_cur == 4, "move: cursor snapped to new position");
    backgammon_on_key(&ev);
    CHECK(bg_p[7] == 1 && bg_p[4] == 0, "move: second die advances to 7");
    CHECK(bg_rem[0] == 0 && bg_rem[1] == 0, "move: both dice used");
    CHECK(bg_phase == BG_PH_AI, "move: turn passes to AI");
}

/* ---- 玩家获胜判定 ---- */
static void test_player_win(void) {
    reset_boards();
    bg_p[23] = 2;
    bg_pfin = 4;
    bg_rem[0] = 6; bg_rem[1] = 1;
    bg_dice[0] = 6; bg_dice[1] = 1;
    bg_phase = BG_PH_SELECT;
    bg_cur = 23;
    key_event_t ev = { K_OK, 0, false };
    backgammon_on_key(&ev);
    CHECK(bg_pfin == 5 && bg_p[23] == 1, "win: one piece bears off");
    CHECK(bg_rem[0] == 0 && bg_rem[1] == 1, "win: die 6 used first");
    CHECK(bg_cur == 23, "win: cursor wraps back to last piece");
    backgammon_on_key(&ev);
    CHECK(bg_pfin == 6, "win: all 6 finished");
    CHECK(bg_over && bg_winner == 1 && bg_phase == BG_PH_OVER, "win: YOU WIN");
    /* 结束帧全刷标志 */
    backgammon_render();
    CHECK(bg_over_full, "win: over forced-full once");
    /* 结束后 OK = 重开 */
    backgammon_on_key(&ev);
    CHECK(!bg_over && bg_phase == BG_PH_ROLL && bg_p[0] == BG_PER,
          "win: OK restarts game");
    /* 结束后 BACK = 退出 */
    bg_over = true;
    bg_winner = 1;
    bg_phase = BG_PH_OVER;
    s_exit_request = false;
    key_event_t bk = { K_BACK, 0, false };
    backgammon_on_key(&bk);
    CHECK(s_exit_request, "win: BACK quits");
}

/* ---- 被墙围死也必然推进(强制跳, 无僵局) ---- */
static void test_wall_jump(void) {
    reset_boards();
    bg_p[10] = 1;
    bg_pfin = 5;
    bg_a[11] = 2; bg_a[12] = 2; bg_a[13] = 2; bg_a[14] = 2; bg_a[15] = 2;
    bg_rem[0] = 1; bg_rem[1] = 1;
    bg_dice[0] = 1; bg_dice[1] = 1;
    bg_phase = BG_PH_SELECT;
    bg_cur = 10;
    key_event_t ev = { K_OK, 0, false };
    backgammon_on_key(&ev);
    CHECK(bg_p[11] == 1, "wall: forced jump over wall (die 1)");
    CHECK(bg_rem[0] == 0 && bg_rem[1] == 1, "wall: one die used");
    CHECK(bg_phase == BG_PH_SELECT, "wall: still moving");
    backgammon_on_key(&ev);
    CHECK(bg_p[12] == 1, "wall: second forced jump");
    CHECK(bg_rem[0] == 0 && bg_rem[1] == 0, "wall: both dice used");
    CHECK(bg_phase == BG_PH_AI, "wall: game always advances, no deadlock");
}

/* ---- AI 单步启发式 ---- */
static void test_ai_step(void) {
    reset_boards();
    /* 优先把进度最深的子前移(用大骰) */
    bg_a[22] = 1;
    bg_rem[0] = 6; bg_rem[1] = 1;
    bg_ai_move_best();
    CHECK(bg_a[16] == 1 && bg_a[22] == 0, "ai: moves deepest piece by 6");
    CHECK(bg_rem[0] == 0 && bg_rem[1] == 1, "ai: die 6 consumed");
    /* 能越线时必越线: 前瞻用小骰先行一步, 大骰收尾越线(两骰都不浪费) */
    reset_boards();
    bg_a[5] = 1;
    bg_rem[0] = 6; bg_rem[1] = 1;
    bg_ai_move_best();
    CHECK(bg_a[4] == 1 && bg_a[5] == 0 && bg_afin == 0, "ai: small die advances first");
    CHECK(bg_rem[0] == 6 && bg_rem[1] == 0, "ai: die 1 consumed first");
    bg_ai_move_best();
    CHECK(bg_afin == 1 && bg_a[4] == 0, "ai: bears off with big die");
    /* 避开玩家墙(罚分), 走合法格 */
    reset_boards();
    bg_a[10] = 1;
    bg_p[4] = 2;
    bg_rem[0] = 2; bg_rem[1] = 6;
    bg_ai_move_best();
    CHECK(bg_a[8] == 1 && bg_a[10] == 0, "ai: avoids player wall, lands at 8");
    CHECK(bg_rem[0] == 0 && bg_rem[1] == 6, "ai: used die 2");
    /* 1 层前瞻: 用小骰完成, 大骰留给续走 */
    reset_boards();
    bg_a[3] = 1; bg_a[20] = 1;
    bg_rem[0] = 1; bg_rem[1] = 6;
    bg_ai_move_best();
    CHECK(bg_a[2] == 1, "ai: lookahead moves deep piece by small die");
    CHECK(bg_rem[0] == 0 && bg_rem[1] == 6, "ai: small die consumed");
    bg_ai_move_best();
    CHECK(bg_afin == 1, "ai: follow-up bears off with big die");
}

/* ---- AI 整回合 tick 驱动 ---- */
static void test_ai_turn_ticks(void) {
    backgammon_enter();
    bg_phase = BG_PH_AI;
    bg_ai_stage = 0;
    bg_rem[0] = 0; bg_rem[1] = 0;
    backgammon_tick(0);
    CHECK(bg_ai_stage == 1, "ai turn: tick rolls dice");
    CHECK(bg_dice[0] >= 1 && bg_dice[0] <= 6 && bg_dice[1] >= 1 && bg_dice[1] <= 6,
          "ai turn: dice in 1..6");
    CHECK(bg_rem[0] == bg_dice[0] && bg_rem[1] == bg_dice[1], "ai turn: rem set");
    /* 确定骰面: {2,5} */
    bg_rem[0] = 2; bg_rem[1] = 5;
    bg_dice[0] = 2; bg_dice[1] = 5;
    backgammon_tick(0);
    CHECK(bg_a[18] == 1 && bg_a[23] == 5, "ai turn: 23 -> 18 with die 5");
    backgammon_tick(0);
    CHECK(bg_a[16] == 1 && bg_a[18] == 0, "ai turn: die 2 advances deeper piece");
    backgammon_tick(0);
    CHECK(bg_phase == BG_PH_ROLL, "ai turn: back to player ROLL");
    /* 玩家回合中 tick 不做事 */
    bg_rem[0] = 4; bg_rem[1] = 2;
    backgammon_tick(0);
    CHECK(bg_rem[0] == 4 && bg_rem[1] == 2, "ai turn: tick no-op on player turn");
}

/* ---- AI 整局获胜 ---- */
static void test_ai_win(void) {
    reset_boards();
    bg_a[5] = 1; bg_a[4] = 2; bg_a[3] = 1; bg_a[2] = 1; bg_a[1] = 1;
    bg_phase = BG_PH_AI;
    bg_ai_stage = 1;
    bg_rem[0] = 6; bg_rem[1] = 1;
    int turns = 0;
    while (!bg_over && turns < 30) {
        while (bg_phase == BG_PH_AI && !bg_over) backgammon_tick(0);
        turns++;
        if (!bg_over) {
            bg_phase = BG_PH_AI;
            bg_ai_stage = 1;
            bg_rem[0] = 6; bg_rem[1] = 1;
        }
    }
    CHECK(bg_over && bg_winner == 2, "ai win: game ends AI WINS");
    CHECK(bg_afin == BG_PER, "ai win: all 6 AI pieces finished");
    CHECK(turns < 30, "ai win: terminates quickly");
}

/* ---- 光标环绕吸附 ---- */
static void test_cursor(void) {
    reset_boards();
    bg_p[5] = 1; bg_p[10] = 1; bg_p[20] = 1;
    bg_pfin = 3;
    bg_phase = BG_PH_SELECT;
    bg_rem[0] = 2; bg_rem[1] = 2;
    bg_cur = 20;
    bg_cursor_step(1);
    CHECK(bg_cur == 5, "cursor: wraps forward to next piece");
    bg_cursor_step(-1);
    CHECK(bg_cur == 20, "cursor: wraps backward to prev piece");
    bg_cur = 10;
    bg_cursor_step(1);
    CHECK(bg_cur == 20, "cursor: forward to next piece");
    /* 光标永远停在有子的格 */
    for (int i = 0; i < 10; i++) {
        bg_cursor_step(i % 2 ? 1 : -1);
        if (bg_p[bg_cur] == 0) { CHECK(0, "cursor: always on a piece"); return; }
    }
    CHECK(1, "cursor: always on a piece");
}

/* ---- 渲染冒烟 + 关键像素 ---- */
static void test_render(void) {
    backgammon_enter();                       /* enter 内部已 render */
    /* 起点格(0)有 6 子: 画了 2x 数字 */
    CHECK(count_black(5, 19, 14, 36) > 0, "render: start cell shows pieces");
    /* 空格(5)内部无墨点 */
    CHECK(count_black(66, 20, 73, 35) == 0, "render: empty cell clean");
    /* 光标: 反色格整格涂黑 */
    bg_phase = BG_PH_SELECT;
    bg_cur = 0;
    backgammon_render();
    CHECK(px(6, 20), "render: cursor cell filled black");
    bg_phase = BG_PH_ROLL;
    backgammon_render();
    CHECK(!px(6, 20), "render: cursor cleared outside SELECT");
    /* 骰子显示 */
    bg_rem[0] = 0; bg_rem[1] = 3;
    bg_dice[0] = 4; bg_dice[1] = 3;
    backgammon_render();
    CHECK(px(BG_DIE_X1 + 3, BG_DIE_Y + 3), "render: used die cross drawn");
    CHECK(count_black(BG_DIE_X2 + 1, BG_DIE_Y + 1, BG_DIE_X2 + BG_DIE - 2,
                      BG_DIE_Y + BG_DIE - 2) > 0, "render: available die filled");
    /* 结束 HUD */
    reset_boards();
    bg_over = true;
    bg_winner = 1;
    bg_phase = BG_PH_OVER;
    backgammon_render();
    CHECK(bg_over_full, "render: over triggers forced full");
}

int main(void) {
    test_legal();
    test_piece_die();
    test_player_flow();
    test_player_win();
    test_wall_jump();
    test_ai_step();
    test_ai_turn_ticks();
    test_ai_win();
    test_cursor();
    test_render();
    if (s_fail == 0) printf("ALL TESTS PASSED\n");
    else printf("%d TEST(S) FAILED\n", s_fail);
    return s_fail ? 1 : 0;
}
