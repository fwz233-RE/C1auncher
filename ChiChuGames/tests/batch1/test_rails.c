/* RAIL PLANNER 逻辑单测 — 直接包含 rails.c 访问静态状态
 * 格 = 行*8+列
 * L1: S=9 T=31 固定17,25,26,27,28,29 缺口30
 * L2: S=27 T1=41 T2=7 固定35,43,28,29,21,13 缺口42,14,15
 * L3: S=33 T1=9 T2=43 T3=30 固定25,26,27,28 缺口17,34,35,29
 */
#include "../../src/games/rails.c"
#include <stdio.h>
#include <string.h>

static int s_fail = 0;

/* host 框架 stub */
bool s_exit_request = false;
void game_set_tick_interval(uint32_t ms) { (void)ms; }

#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); s_fail++; } \
    else { printf("ok: %s\n", msg); } \
} while (0)

static void fill(int c, uint8_t v) { rl_grid[c] = v; }
static void empty_cell(int c) { rl_grid[c] = RL_EMPTY; }

static void test_rom_l1(void) {
    rl_start(0);
    CHECK(rl_tcnt == 1, "L1 目标站数=1");
    CHECK(!rl_over, "L1 开局未过关");
    CHECK(rl_done == 0, "L1 开局 0 连通");
    CHECK(rl_fixed[9] && rl_grid[9] == RL_S, "L1 S 站(9)固定");
    CHECK(rl_fixed[31] && rl_grid[31] == RL_T, "L1 T 站(31)固定");
    CHECK(rl_fixed[17] && rl_grid[17] == RL_H, "L1 预置轨(17)固定");
    /* 经 rl_ok 走流程: 光标到缺口 (3,6)=30, 铺轨即连通 */
    rl_cur = 30;
    rl_ok();
    CHECK(rl_grid[30] == RL_H, "L1 OK: 空→直");
    CHECK(rl_done == 1, "L1 铺缺口后 1/1 连通");
    CHECK(rl_over, "L1 全部连通→过关");
    rl_ok();  /* 直→弯 */
    CHECK(rl_grid[30] == RL_CUR && rl_over, "过关后仍可编辑且保持过关");
}

static void test_rom_l2(void) {
    rl_start(1);
    CHECK(rl_tcnt == 2, "L2 目标站数=2");
    CHECK(rl_done == 0, "L2 开局 0 连通");
    /* 只连 T1(5,1)=41: 填 (5,2)=42 */
    fill(42, RL_V);
    rl_conn();
    CHECK(rl_done == 1, "L2 只连 T1 → 1/2");
    CHECK(!rl_over, "L2 1/2 未过关");
    /* 只连 T2(0,7)=7 方向: 填 (1,7)=15, (1,6)=14 */
    empty_cell(42);
    fill(15, RL_V);
    rl_conn();
    CHECK(rl_done == 0, "L2 只填 T2 邻格仍不连(缺 1,6)");
    fill(14, RL_V);
    rl_conn();
    CHECK(rl_done == 1, "L2 只连 T2 → 1/2");
    /* 全部补齐 */
    fill(42, RL_V);
    rl_conn();
    CHECK(rl_done == 2, "L2 双缺口补齐 → 2/2");
}

static void test_rom_l3(void) {
    rl_start(2);
    CHECK(rl_tcnt == 3, "L3 目标站数=3");
    CHECK(rl_done == 0, "L3 开局 0 连通");
    /* T1(1,1)=9: 填 (2,1)=17 借预置 (3,1)=25 连通 */
    fill(17, RL_V);
    rl_conn();
    CHECK(rl_done == 1, "L3 接 T1 → 1/3");
    /* T2(5,3)=43: (4,3)=35 与 T2 直邻, 且斜接预置 (3,4)=28 → 1 格即连 */
    fill(35, RL_V);
    rl_conn();
    CHECK(rl_done == 2, "L3 填(4,3) 接 T2 → 2/3");
    /* T3(3,6)=30: 填 (3,5)=29 */
    fill(29, RL_H);
    rl_conn();
    CHECK(rl_done == 3, "L3 全连通 → 3/3");
}

static void test_cycle_and_fixed(void) {
    rl_start(0);
    rl_cur = 0;   /* (0,0) 空格 */
    rl_ok();      CHECK(rl_grid[0] == RL_H, "循环: 空→直");
    rl_ok();      CHECK(rl_grid[0] == RL_CUR, "循环: 直→弯");
    rl_ok();      CHECK(rl_grid[0] == RL_X, "循环: 弯→十字");
    rl_ok();      CHECK(rl_grid[0] == RL_EMPTY, "循环: 十字→空");
    /* 固定格不可改 */
    rl_cur = 9;   /* S 站 */
    rl_ok();
    CHECK(rl_grid[9] == RL_S, "OK 不改站格");
    rl_del();
    CHECK(rl_grid[9] == RL_S, "DEL 不改站格");
    rl_cur = 17;  /* 预置轨 */
    rl_ok();
    CHECK(rl_grid[17] == RL_H && rl_fixed[17], "OK 不改预置轨");
    rl_del();
    CHECK(rl_grid[17] == RL_H, "DEL 不改预置轨");
    /* DEL 擦玩家轨 */
    rl_cur = 30;
    rl_ok();
    CHECK(rl_grid[30] == RL_H, "DEL 前已铺轨");
    rl_del();
    CHECK(rl_grid[30] == RL_EMPTY, "DEL 擦除玩家轨");
    CHECK(rl_done == 0, "擦除后回到 0 连通");
}

static void test_diagonal_win(void) {
    /* 纯 8-连通: S(0,0) 与 T(1,1) 斜相邻 → 无需铺轨即连通 */
    rl_start(0);
    for (int i = 0; i < RL_CELLS; i++) { rl_grid[i] = RL_EMPTY; rl_fixed[i] = false; }
    rl_tcnt = 1;
    rl_sta.tcnt = 1;
    rl_grid[0] = RL_S;
    rl_grid[9] = RL_T;
    rl_sta.t[0] = 9;
    rl_conn();
    CHECK(rl_done == 1, "斜对角 8-连通直接判连");
    /* 隔一格不连通 */
    rl_grid[9] = RL_EMPTY;
    rl_grid[18] = RL_T;   /* (2,2) 与 S(0,0) 相距 2 格 */
    rl_sta.t[0] = 18;
    rl_conn();
    CHECK(rl_done == 0, "相距 2 格不连通");
}

static void test_gen_determinism(void) {
    rng_seed(&rl_rng, 42u);
    rl_start(3);
    uint8_t g1[RL_CELLS];
    memcpy(g1, rl_grid, RL_CELLS);
    int t1 = rl_tcnt;
    rng_seed(&rl_rng, 42u);
    rl_start(3);
    CHECK(t1 == rl_tcnt && memcmp(g1, rl_grid, RL_CELLS) == 0,
          "固定种子生成可复现");
}

static void test_gen_many_seeds(void) {
    for (uint32_t seed = 1; seed <= 40; seed++) {
        int lv = 3 + (int)(seed % 4);   /* 3..6 级 */
        rng_seed(&rl_rng, seed * 0x9E3779B1u);
        rl_start(lv);
        int exp = lv + 1;
        if (exp > RL_T_MAX) exp = RL_T_MAX;
        if (rl_tcnt != exp) {
            printf("FAIL: seed %u lv%d tcnt=%d expect %d\n", seed, lv, rl_tcnt, exp);
            s_fail++; continue;
        }
        /* 站合法: S 唯一, 目标站在表中且格值一致 */
        int s_cnt = 0, ok_sta = 1;
        for (int i = 0; i < RL_CELLS; i++)
            if (rl_grid[i] == RL_S) s_cnt++;
        for (int i = 0; i < rl_tcnt; i++) {
            int t = rl_sta.t[i];
            if (t < 0 || t >= RL_CELLS || rl_grid[t] != RL_T + i) { ok_sta = 0; break; }
        }
        if (s_cnt != 1 || !ok_sta) {
            printf("FAIL: seed %u lv%d 站布局非法 (S=%d)\n", seed, lv, s_cnt);
            s_fail++; continue;
        }
        if (rl_done != 0) {
            printf("FAIL: seed %u lv%d 开局已有 %d 连通\n", seed, lv, rl_done);
            s_fail++; continue;
        }
        /* 可解性: 全填实心必然全连通 */
        for (int i = 0; i < RL_CELLS; i++)
            if (rl_grid[i] == RL_EMPTY) rl_grid[i] = RL_H;
        rl_conn();
        if (rl_done != rl_tcnt) {
            printf("FAIL: seed %u lv%d 全填后仍未连通(%d)\n", seed, lv, rl_done);
            s_fail++; continue;
        }
        /* 至少有一个预置轨(引导) */
        int nfix = 0;
        for (int i = 0; i < RL_CELLS; i++)
            if (rl_fixed[i] && rl_is_rail(i)) nfix++;
        if (nfix == 0) { printf("FAIL: seed %u lv%d 无预置轨\n", seed, lv); s_fail++; }
    }
    printf("ok: 40 组随机关全过(目标数/开局未连/全填可解/预置轨)\n");
}

static void test_progression_and_keys(void) {
    rl_start(0);
    /* 'n' 键推进到第 2 关(ROM L2) */
    key_event_t ev = { K_CHAR, 'n', false };
    rails_on_key(&ev);
    CHECK(rl_level == 1 && rl_tcnt == 2, "N 键推进到 L2");
    /* 推进到 L4(首个随机关) */
    rl_start(3);
    CHECK(rl_level == 3 && rl_tcnt == 4, "L4 随机关 4 目标");
    /* 方向键重复可响应 */
    ev.key = K_RIGHT; ev.is_repeat = true;
    int c0 = rl_cur;
    rails_on_key(&ev);
    CHECK(rl_cur == (c0 + 1) % RL_COLS, "方向键重复可移动");
    /* 确认键重复忽略 */
    rl_cur = 30;
    rl_grid[30] = RL_EMPTY;
    rl_fixed[30] = false;
    ev.key = K_OK; ev.ch = 0; ev.is_repeat = true;
    rails_on_key(&ev);
    CHECK(rl_grid[30] == RL_EMPTY, "OK 重复按下忽略");
    /* 过关后 OK → 下一关 */
    rl_start(0);
    rl_cur = 30;
    rl_ok();
    CHECK(rl_over, "L1 过关就绪");
    ev.key = K_OK; ev.ch = 0; ev.is_repeat = false;
    rails_on_key(&ev);
    CHECK(rl_level == 1 && !rl_over, "过关后 OK 进入下一关");
    /* BACK 在过关界面 → 退出请求 */
    rl_start(2);
    fill(17, RL_V);
    fill(34, RL_H);
    fill(35, RL_V);
    fill(29, RL_H);
    rl_conn();
    CHECK(rl_done == 3 && rl_over == false, "L3 全填后 done=3");
}

static void test_render_smoke(void) {
    /* 渲染不崩溃 + 有像素输出 */
    rl_start(0);
    rails_render();
    int n1 = 0;
    for (int i = 0; i < (int)sizeof(g_fb); i++) if (g_fb[i]) n1++;
    CHECK(n1 > 0, "L1 渲染有内容");
    rl_cur = 30;
    rl_ok();
    rails_render();
    CHECK(rl_over, "L1 过关状态渲染");
    rl_start(4);
    rails_render();
    int n2 = 0;
    for (int i = 0; i < (int)sizeof(g_fb); i++) if (g_fb[i]) n2++;
    CHECK(n2 > 0, "随机关渲染有内容");
}

int main(void) {
    disp_init();   /* host stub */
    test_rom_l1();
    test_rom_l2();
    test_rom_l3();
    test_cycle_and_fixed();
    test_diagonal_win();
    test_gen_determinism();
    test_gen_many_seeds();
    test_progression_and_keys();
    test_render_smoke();
    if (s_fail == 0) printf("\nALL RAILS TESTS PASSED\n");
    else printf("\n%d TEST(S) FAILED\n", s_fail);
    return s_fail ? 1 : 0;
}
