/* MUSHROOM GARDEN 逻辑单测 — host cc 编译, 断言核心逻辑 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* host 框架 stub(game.h 需要) */
bool s_exit_request = false;
void game_set_tick_interval(uint32_t ms) { (void)ms; }

#include "games/mushgarden.c"

static int s_fail = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); s_fail++; } \
    else { printf("ok: %s\n", msg); } \
} while (0)

/* 重置对局状态(不重新播种 rng) */
static void reset(void) {
    memset(mg_age, 0, sizeof(mg_age));
    mg_cx = 0; mg_cy = 0;
    mg_turn = 1;
    mg_score = 0;
    mg_msg = MG_MSG_NONE;
    mg_state = MG_ST_PLAY;
    mg_over_full = false;
}

/* 找第一个使 rng_range(&r,10) 落在 [lo,hi] 的种子(滚动骰子) */
static uint32_t seed_with_roll(uint32_t lo, uint32_t hi) {
    for (uint32_t s = 1; s < 1000000; s++) {
        rng_t r;
        rng_seed(&r, s);
        uint32_t v = rng_range(&r, 10);
        if (v >= lo && v <= hi) return s;
    }
    return 0;
}

/* 前两次掷骰(回合5与回合10)都落在 [lo,hi] 的种子 */
static uint32_t seed_with_roll2(uint32_t lo, uint32_t hi) {
    for (uint32_t s = 1; s < 1000000; s++) {
        rng_t r;
        rng_seed(&r, s);
        uint32_t v1 = rng_range(&r, 10);
        uint32_t v2 = rng_range(&r, 10);
        if (v1 >= lo && v1 <= hi && v2 >= lo && v2 <= hi) return s;
    }
    return 0;
}

/* 事件掷骰落空的种子: 每 5 回合事件=无事, 得到纯净生长时间线 */
static uint32_t s_none_seed;
static uint32_t s_none2_seed;   /* 前两次掷骰(回合5,10)都落空 */

/* ---- 阶段映射与倒计时 ---- */
static void test_stage(void) {
    CHECK(mg_stage(0) == 0, "stage: empty=0");
    CHECK(mg_stage(1) == 1 && mg_stage(2) == 1, "stage: seed ages 1-2");
    CHECK(mg_stage(3) == 2 && mg_stage(4) == 2, "stage: shoot ages 3-4");
    CHECK(mg_stage(5) == 3 && mg_stage(6) == 3, "stage: mushroom ages 5-6");
    CHECK(mg_stage(7) == 4 && mg_stage(12) == 4, "stage: ready ages 7-12");
    CHECK(mg_countdown_val(1) == 6 && mg_countdown_val(2) == 5 &&
          mg_countdown_val(6) == 1, "countdown: growing 6..1");
    CHECK(mg_countdown_val(7) == 6 && mg_countdown_val(9) == 4 &&
          mg_countdown_val(12) == 1, "countdown: ready rot 6..1");
}

/* ---- 生长时间线: 种下第 1 回合, 第 7 回合可收获, 第 13 回合腐烂 ---- */
static void test_growth_timeline(void) {
    reset();
    rng_seed(&mg_rng, s_none_seed);             /* 事件全部落空 */
    mg_act();                                   /* turn1: 种 */
    CHECK(mg_age[0] == 2 && mg_turn == 2, "plant: age2 after turn, turn advances");
    CHECK(mg_score == 0 && mg_msg == MG_MSG_NONE, "plant: no score change");
    for (int k = 0; k < 4; k++) mg_act();      /* turns 2-5: 等待 */
    CHECK(mg_age[0] == 6 && mg_stage(6) == 3 &&
          mg_countdown_val(6) == 1, "turn5: stage3 mushroom, cd=1");
    CHECK(mg_msg == MG_MSG_NONE, "turn5: event rolled none");
    mg_act();                                   /* turn6: wait -> ready */
    CHECK(mg_age[0] == 7 && mg_stage(7) == 4, "turn6 end: ready (stage4)");
    CHECK(mg_turn == 7, "ready at turn 7");
}

/* ---- 收获: 第 7 回合起可收 +2, 清格, 回合推进 ---- */
static void test_harvest(void) {
    reset();
    rng_seed(&mg_rng, s_none_seed);
    mg_act();                                   /* plant */
    for (int k = 0; k < 4; k++) mg_act();       /* turns 2-5: age 6 */
    CHECK(mg_stage(mg_age[0]) == 3 &&
          mg_countdown_val(mg_age[0]) == 1, "pre-harvest: ready next turn");
    mg_act();                                   /* turn6: wait -> ready */
    CHECK(mg_stage(mg_age[0]) == 4 && mg_turn == 7, "ready at turn 7");
    int turn_before = mg_turn;
    mg_act();                                   /* harvest */
    CHECK(mg_score == 2, "harvest: +2");
    CHECK(mg_age[0] == 0, "harvest: cell cleared");
    CHECK(mg_turn == turn_before + 1, "harvest: turn advances");
    CHECK(mg_msg == MG_MSG_HARVEST, "harvest: HUD msg +2");
}

/* ---- 腐烂: 第 7-12 回合可收, 第 12 回合末腐烂 -1 ----
 * 注意: OK 落在可收获格=收获, 无法"等待"跳过, 故腐烂测试全程把光标
 * 放在 0 号格以外的空格(会种下诱饵, 但诱饵种得晚, 第 12 回合末只有
 * cell0 达到 age13)。回合 5/10 事件用双落空种子隔离。 */
static void test_rot(void) {
    reset();
    rng_seed(&mg_rng, s_none2_seed);
    mg_act();                                   /* act1: 种 cell0 */
    for (int k = 0; k < 10; k++) {              /* acts 2-11: 光标在别处 */
        mg_cx = (k + 1) % MG_COLS;
        mg_cy = ((k + 1) / MG_COLS) % MG_ROWS;
        mg_act();
    }
    CHECK(mg_age[0] == 12 && mg_stage(12) == 4 &&
          mg_countdown_val(12) == 1, "turn12: last ready round (cd=1)");
    CHECK(mg_turn == 12, "act11 end: turn 12");
    int s0 = mg_score;
    mg_cx = 1; mg_cy = 1;                       /* act12: 别处行动 */
    mg_act();
    CHECK(mg_age[0] == 0, "rot: cell cleared");
    CHECK(mg_score == s0 - 1, "rot: exactly -1 (only cell0 rots)");
    CHECK(mg_msg == MG_MSG_ROT, "rot: HUD msg ROT -1");
}

/* ---- 雨水: 所有蘑菇 +1 阶段(age+2, 封顶 12 不直接腐烂) ---- */
static void test_rain(void) {
    uint32_t s = seed_with_roll(0, 2);
    CHECK(s != 0, "rain seed found");
    reset();
    mg_age[0] = 1; mg_age[1] = 5; mg_age[2] = 12;   /* 种子/成菇/临界 */
    rng_seed(&mg_rng, s);
    mg_event();
    CHECK(mg_msg == MG_MSG_RAIN, "rain: msg set");
    CHECK(mg_age[0] == 3 && mg_age[1] == 7, "rain: +1 stage (age+2)");
    CHECK(mg_age[2] == 12, "rain: capped at 12, no instant rot");
    CHECK(mg_age[3] == 0 && mg_age[29] == 0, "rain: empty cells stay empty");
    CHECK(mg_score == 0, "rain: no score change");
}

/* ---- 虫害: 随机 1 个蘑菇 -1 阶段(age-2, 不低于种子); 空园无效果 ---- */
static void test_pest(void) {
    uint32_t s = seed_with_roll(3, 4);
    CHECK(s != 0, "pest seed found");
    reset();
    mg_age[0] = 1; mg_age[1] = 5; mg_age[2] = 7;    /* 种子会被钳制在 1 */
    rng_seed(&mg_rng, s);
    mg_event();
    CHECK(mg_msg == MG_MSG_PEST, "pest: msg set");
    CHECK(mg_age[0] >= 1 && mg_age[1] >= 1 && mg_age[2] >= 1, "pest: no destroy");
    CHECK(mg_age[0] + mg_age[1] + mg_age[2] == 11, "pest: exactly one cell -2");
    CHECK(mg_score == 0, "pest: no score change");
    /* 空园: 无事发生, 无消息 */
    reset();
    rng_seed(&mg_rng, s);
    mg_event();
    CHECK(mg_msg == MG_MSG_NONE, "pest: empty garden no-op");
    /* 单蘑菇: 必中 */
    reset();
    mg_age[10] = 4;
    rng_seed(&mg_rng, s);
    mg_event();
    CHECK(mg_age[10] == 2, "pest: single mushroom always hit");
}

/* ---- 事件节奏: 第 5 回合末掷骰(带雨种子 → RAIN) ---- */
static void test_event_cadence(void) {
    uint32_t s = seed_with_roll(0, 2);
    reset();
    rng_seed(&mg_rng, s);
    mg_act();                                   /* turn1 plant */
    for (int k = 0; k < 3; k++) mg_act();       /* turns 2-4 */
    CHECK(mg_msg == MG_MSG_NONE, "cadence: no event before turn 5");
    mg_act();                                   /* turn5 end: roll */
    CHECK(mg_msg == MG_MSG_RAIN, "cadence: event fires on turn 5");
    CHECK(mg_turn == 6, "cadence: turn advanced");
    CHECK(mg_age[0] == 8, "cadence: rain boosted age 6->8");
}

/* ---- 等待: OK 在生长中的蘑菇 = 等待, 回合推进无其他效果 ---- */
static void test_wait(void) {
    reset();
    mg_act();                                   /* plant */
    int age_before = mg_age[0];
    int turn_before = mg_turn;
    mg_act();                                   /* wait on growing */
    CHECK(mg_age[0] == age_before + 1, "wait: mushroom still grows one stage step");
    CHECK(mg_turn == turn_before + 1, "wait: turn advances");
    CHECK(mg_score == 0, "wait: no score change");
    CHECK(mg_age[0] != 0, "wait: cell not cleared");
}

/* ---- 光标环绕 + 重复键语义 ---- */
static void test_cursor(void) {
    reset();
    key_event_t ev;
    ev.is_repeat = false;
    ev.key = K_LEFT;  mushgarden_on_key(&ev);
    CHECK(mg_cx == MG_COLS - 1 && mg_cy == 0, "cursor: left wraps");
    ev.key = K_UP;    mushgarden_on_key(&ev);
    CHECK(mg_cx == MG_COLS - 1 && mg_cy == MG_ROWS - 1, "cursor: up wraps");
    ev.key = K_RIGHT; mushgarden_on_key(&ev);
    CHECK(mg_cx == 0, "cursor: right wraps");
    ev.key = K_DOWN;  mushgarden_on_key(&ev);
    CHECK(mg_cy == 0, "cursor: down wraps");
    ev.is_repeat = true;
    ev.key = K_RIGHT; mushgarden_on_key(&ev);
    CHECK(mg_cx == 1, "cursor: direction responds to repeat");
    ev.is_repeat = true;
    ev.key = K_OK;    mushgarden_on_key(&ev);
    CHECK(mg_turn == 1 && mg_age[0] == 0, "confirm: repeat OK ignored");
}

/* ---- 退出键 ---- */
static void test_quit(void) {
    reset();
    s_exit_request = false;
    key_event_t ev = { K_QUIT, 0, false };
    mushgarden_on_key(&ev);
    CHECK(s_exit_request, "quit: K_QUIT exits");
    reset();
    s_exit_request = false;
    key_event_t ev2 = { K_CHAR, 'q', false };
    mushgarden_on_key(&ev2);
    CHECK(s_exit_request, "quit: 'q' exits");
    reset();
    s_exit_request = false;
    key_event_t ev3 = { K_CHAR, 'x', false };
    mushgarden_on_key(&ev3);
    CHECK(!s_exit_request && mg_turn == 1, "quit: other keys ignored");
}

/* ---- 完整 25 回合: 独立镜像模拟交叉验证(同种子同策略, 分数/年龄/结束态) ---- */
static void test_full_game(void) {
    uint32_t s = seed_with_roll(0, 2);
    reset();
    rng_seed(&mg_rng, s);
    /* 镜像: 自己的年龄表/分数/rng, 完整复刻生长+事件规则 */
    rng_t mr;
    rng_seed(&mr, s);
    int m_age[MG_N];
    memset(m_age, 0, sizeof(m_age));
    int m_score = 0;
    for (int i = 0; i < 25; i++) {
        int idx = (i % MG_COLS) + ((i / MG_COLS) % MG_ROWS) * MG_COLS;
        mg_cx = i % MG_COLS;
        mg_cy = (i / MG_COLS) % MG_ROWS;
        /* 行动前: 镜像与游戏的当前格状态必须一致 */
        int ms = (m_age[idx] <= 0) ? 0 : ((m_age[idx] + 1) / 2);
        if (ms > 4) ms = 4;
        if (ms != mg_stage(mg_age[idx])) {
            printf("FAIL: mirror stage divergence at act %d\n", i + 1);
            s_fail++;
        }
        if (m_age[idx] == 0) m_age[idx] = 1;
        else if (ms == 4) { m_score += 2; m_age[idx] = 0; }
        mg_act();
        /* 镜像回合末: 生长+腐烂 */
        for (int c = 0; c < MG_N; c++) {
            if (m_age[c] == 0) continue;
            m_age[c]++;
            if (m_age[c] >= MG_ROT) { m_age[c] = 0; m_score--; }
        }
        /* 镜像事件(与游戏同序调用 rng) */
        if ((i + 1) % 5 == 0) {
            uint32_t roll = rng_range(&mr, 10);
            if (roll <= 2) {
                for (int c = 0; c < MG_N; c++)
                    if (m_age[c]) {
                        m_age[c] += 2;
                        if (m_age[c] > MG_ROT - 1) m_age[c] = MG_ROT - 1;
                    }
            } else if (roll == 3 || roll == 4) {
                int n = 0;
                for (int c = 0; c < MG_N; c++) if (m_age[c]) n++;
                if (n) {
                    uint32_t k = rng_range(&mr, (uint32_t)n);
                    for (int c = 0; c < MG_N; c++) {
                        if (m_age[c] == 0) continue;
                        if (k == 0) { m_age[c] -= 2; if (m_age[c] < 1) m_age[c] = 1; break; }
                        k--;
                    }
                }
            }
        }
    }
    CHECK(mg_state == MG_ST_OVER, "full: game over after 25 turns");
    CHECK(mg_turn == 26, "full: turn counter 26");
    CHECK(mg_score == m_score, "full: score matches mirror");
    for (int i = 0; i < MG_N; i++)
        if (mg_age[i] != m_age[i]) {
            printf("FAIL: age[%d]=%d mirror=%d\n", i, mg_age[i], m_age[i]);
            s_fail++;
        }
    printf("      (full game score=%d)\n", mg_score);
    /* 结束渲染不崩溃, 全刷标志只置一次 */
    mg_over_full = false;
    mushgarden_render();
    CHECK(mg_over_full, "full: over render sets full flag");
    /* OVER 态 OK -> 新局 */
    reset();
    mg_state = MG_ST_OVER;
    key_event_t ev = { K_OK, 0, false };
    mushgarden_on_key(&ev);
    CHECK(mg_state == MG_ST_PLAY && mg_turn == 1 && mg_score == 0,
          "over: OK restarts");
    /* OVER 态 BACK -> 退出 */
    reset();
    mg_state = MG_ST_OVER;
    s_exit_request = false;
    key_event_t ev2 = { K_BACK, 0, false };
    mushgarden_on_key(&ev2);
    CHECK(s_exit_request, "over: BACK quits");
}

/* ---- 渲染冒烟: 种植/收获/光标路径绘制不越界不崩溃 ---- */
static void test_render_smoke(void) {
    reset();
    mg_age[0] = 2; mg_age[13] = 5; mg_age[27] = 9; mg_age[14] = 12;
    mg_cx = 0; mg_cy = 0;
    mushgarden_render();
    int nset = 0;
    for (int i = 0; i < (int)CCG_FRAME_BYTES; i++)
        if (g_fb[i] != 0) nset++;
    CHECK(nset > 0, "render: draws something (HUD+grid+art)");
    mg_cx = 4; mg_cy = 2;           /* 光标在可收获格上 */
    mushgarden_render();
    mg_cx = 5; mg_cy = 4;           /* 光标在空格上 */
    mushgarden_render();
    CHECK(mg_over_full == false, "render: play state no force full");
}

int main(void) {
    s_none_seed = seed_with_roll(5, 9);         /* 事件=无的种子 */
    s_none2_seed = seed_with_roll2(5, 9);       /* 回合5/10都=无 */
    if (s_none_seed == 0 || s_none2_seed == 0) {
        printf("FAIL: cannot find none seed\n");
        return 1;
    }
    test_stage();
    test_growth_timeline();
    test_harvest();
    test_rot();
    test_rain();
    test_pest();
    test_event_cadence();
    test_wait();
    test_cursor();
    test_quit();
    test_full_game();
    test_render_smoke();
    if (s_fail) { printf("RESULT: %d FAIL\n", s_fail); return 1; }
    printf("RESULT: ALL PASS\n");
    return 0;
}
