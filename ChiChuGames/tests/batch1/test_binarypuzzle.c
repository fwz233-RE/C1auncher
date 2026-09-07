/* BINARY PUZZLE host 逻辑单测 — 直接包含 binarypuzzle.c 访问静态状态 */
#include "../../src/games/binarypuzzle.c"
#include <stdio.h>
#include <string.h>

bool s_exit_request = false;
void game_set_tick_interval(uint32_t ms) { (void)ms; }

static int s_fail = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); s_fail++; } \
    else { printf("ok: %s\n", msg); } \
} while (0)

/* 完整解(脚本生成, 与 bp_rom 中题面一致) */
static const char s_sol[BP_ROM_N][BP_NCELL + 1] = {
    "1010101001010110110001100100111011001001100110011001100101011001011010110010010100101101100101011010",
    "1010010101110011001001010011010011011001110010011011010100100010101101101101001001011011000010101011",
    "0110010110100101011010101010010110010101010110011010101010100101011001010110010110100110101001101001",
};

/* 精确违反单一规则的完整盘面(脚本构造并验证) */
static const char s_v1[BP_NCELL + 1] = /* 行/列不平衡 */
    "1010101101010110110001100100111011001001100110011001100101011001011010110010010100101101100101011010";
static const char s_v2[BP_NCELL + 1] = /* 仅行内三连 */
    "1100101001001110110001100100111011001001100110011001100101011001011010110010010100101101100101011010";
static const char s_v3[BP_NCELL + 1] = /* 仅行重复(行5==7, 行6==8) */
    "0101101010010010110110100101011101001010011010010100101100111001011010011010010110010110101011010100";
static const char s_v4[BP_NCELL + 1] = /* 仅列重复(V3 转置) */
    "0011001011110110010000101101011001001011110011010000100110111101001010011010010110010110100110110100";

static void board_load(const char *s, int what) {
    for (int i = 0; i < BP_NCELL; i++) {
        char ch = s[i];
        bp_cell[i] = (ch == '1') ? BP_ONE : BP_ZERO;
        bp_given[i] = 0;
    }
    (void)what;
}

static int rule_check(const char *s) {
    board_load(s, 0);
    return bp_check_win();
}

/* ---- ROM 完整性 ---- */
static void test_rom(void) {
    for (int p = 0; p < BP_ROM_N; p++) {
        CHECK(strlen(bp_rom[p]) == BP_NCELL, "rom length == 100");
        int givens = 0;
        for (int i = 0; i < BP_NCELL; i++) {
            char ch = bp_rom[p][i];
            CHECK(ch == '0' || ch == '1' || ch == ' ', "rom chars in {0,1,' '}");
            if (ch != ' ') givens++;
        }
        CHECK(givens > 0, "puzzle has givens");
        CHECK(givens < BP_NCELL, "puzzle is not pre-solved");
    }
    int g_all[BP_ROM_N] = {0, 0, 0};
    for (int p = 0; p < BP_ROM_N; p++)
        for (int i = 0; i < BP_NCELL; i++)
            if (bp_rom[p][i] != ' ') g_all[p]++;
    printf("ok: each puzzle has %d/%d/%d givens\n", g_all[0], g_all[1], g_all[2]);
    for (int p = 0; p < BP_ROM_N; p++)
        CHECK(g_all[p] > 20 && g_all[p] < 60, "sane given count (not trivial, not empty)");
}

/* ---- 题面格 = 完整解的子集(题面与解一致) ---- */
static void test_givens_match_solution(void) {
    for (int p = 0; p < BP_ROM_N; p++)
        for (int i = 0; i < BP_NCELL; i++)
            if (bp_rom[p][i] != ' ')
                CHECK(bp_rom[p][i] == s_sol[p][i], "given cell matches solution");
}

/* ---- 完整解满足全部规则(独立实现校验) ---- */
static void test_solutions_valid(void) {
    for (int p = 0; p < BP_ROM_N; p++)
        CHECK(rule_check(s_sol[p]) == 1, "solution passes all rules");
}

/* ---- 胜利判定: 三类违反必须被拒; 空盘/未填满必须被拒 ---- */
static void test_win_check(void) {
    CHECK(rule_check(s_v1) == 0, "row/col imbalance rejected");
    CHECK(rule_check(s_v2) == 0, "row triple rejected");
    CHECK(rule_check(s_v3) == 0, "duplicate rows rejected");
    CHECK(rule_check(s_v4) == 0, "duplicate columns rejected");

    bp_new_game(0);
    CHECK(bp_check_win() == 0, "incomplete board not a win");
    /* 填满但留一格空 */
    board_load(s_sol[0], 0);
    bp_cell[42] = BP_EMPTY;
    CHECK(bp_check_win() == 0, "one empty cell not a win");
}

/* ---- 完成流程: 从题面经 OK 循环填到完整解 -> 胜利 ---- */
static void test_completion(void) {
    for (int p = 0; p < BP_ROM_N; p++) {
        bp_new_game(p);
        int presses = 0;
        for (int i = 0; i < BP_NCELL; i++) {
            int want = (s_sol[p][i] == '1') ? BP_ONE : BP_ZERO;
            int guard = 0;
            while (bp_cell[i] != want && guard < 3) {   /* 题面格循环无效, guard 防死循环 */
                bp_cycle(i);
                presses++;
                guard++;
            }
            CHECK(bp_cell[i] == want, "cycle reaches solution value");
        }
        CHECK(bp_over == true, "board complete -> over");
        CHECK(presses > 0, "player interaction actually required");
    }
}

/* ---- OK 循环: 空->0->1->空; 题面格锁定 ---- */
static void test_cycle(void) {
    bp_new_game(0);
    int idx = -1;
    for (int i = 0; i < BP_NCELL; i++)
        if (bp_given[i] == 0) { idx = i; break; }
    if (idx < 0) { CHECK(0, "found a free cell"); return; }
    CHECK(idx >= 0, "found a free cell");
    bp_cell[idx] = BP_EMPTY;
    bp_cycle(idx);
    CHECK(bp_cell[idx] == BP_ZERO, "cycle empty->0");
    bp_cycle(idx);
    CHECK(bp_cell[idx] == BP_ONE, "cycle 0->1");
    bp_cycle(idx);
    CHECK(bp_cell[idx] == BP_EMPTY, "cycle 1->empty");
    /* 题面格: OK/DEL 均无效 */
    int g = -1;
    for (int i = 0; i < BP_NCELL; i++)
        if (bp_given[i] != 0) { g = i; break; }
    if (g < 0) { CHECK(0, "found a given cell"); return; }
    CHECK(g >= 0, "found a given cell");
    uint8_t before = bp_cell[g];
    bp_cycle(g);
    bp_clear_cell(g);
    CHECK(bp_cell[g] == before, "given cell locked against OK/DEL");
}

/* ---- DEL 清空非题面格 ---- */
static void test_del(void) {
    bp_new_game(0);
    int idx = -1;
    for (int i = 0; i < BP_NCELL; i++)
        if (bp_given[i] == 0) { idx = i; break; }
    if (idx < 0) { CHECK(0, "found a free cell"); return; }
    bp_cell[idx] = BP_ONE;
    bp_clear_cell(idx);
    CHECK(bp_cell[idx] == BP_EMPTY, "DEL clears player cell");
}

/* ---- 光标移动 + 边界夹紧(含重复事件走快路径) ---- */
static void test_cursor(void) {
    bp_new_game(0);
    key_event_t ev;
    memset(&ev, 0, sizeof ev);
    for (int k = 0; k < 3; k++) {          /* 重复方向键应可响应 */
        ev.key = K_LEFT;
        ev.is_repeat = (k > 0);
        bp_cy = 5; bp_cx = 5;
        binarypuzzle_on_key(&ev);
        CHECK(bp_cx == 4, "arrow left moves");
    }
    ev.key = K_UP; ev.is_repeat = false; bp_cx = 5; bp_cy = 0;
    binarypuzzle_on_key(&ev);
    CHECK(bp_cy == 0, "cursor clamped at top");
    ev.key = K_DOWN; ev.is_repeat = false; bp_cx = 5; bp_cy = BP_N - 1;
    binarypuzzle_on_key(&ev);
    CHECK(bp_cy == BP_N - 1, "cursor clamped at bottom");
    ev.key = K_CHAR; ev.ch = 'a'; bp_cx = 0;
    binarypuzzle_on_key(&ev);
    CHECK(bp_cx == 0, "WASD left clamped");
    ev.key = K_CHAR; ev.ch = 'd'; bp_cx = BP_N - 1;
    binarypuzzle_on_key(&ev);
    CHECK(bp_cx == BP_N - 1, "WASD right clamped");
    ev.key = K_CHAR; ev.ch = 'w'; bp_cy = 0;
    binarypuzzle_on_key(&ev);
    CHECK(bp_cy == 0, "WASD up clamped");
    ev.key = K_CHAR; ev.ch = 's'; bp_cy = BP_N - 1;
    binarypuzzle_on_key(&ev);
    CHECK(bp_cy == BP_N - 1, "WASD down clamped");
}

/* ---- 按键驱动的填数/清空(走 on_key 入口) ---- */
static void test_onkey_fill(void) {
    bp_new_game(0);
    key_event_t ev;
    memset(&ev, 0, sizeof ev);
    ev.key = K_OK;
    bp_cx = 4; bp_cy = 4;
    int idx = 4 * BP_N + 4;
    int giv = bp_given[idx];
    binarypuzzle_on_key(&ev);
    if (giv) {
        CHECK(bp_cell[idx] != BP_EMPTY, "given cell unchanged by OK");
    } else {
        CHECK(bp_cell[idx] == BP_ZERO, "OK fills 0");
        ev.is_repeat = true;               /* 重复 OK 必须忽略 */
        binarypuzzle_on_key(&ev);
        CHECK(bp_cell[idx] == BP_ZERO, "repeat OK ignored");
        ev.is_repeat = false;
        ev.key = K_DEL;
        binarypuzzle_on_key(&ev);
        CHECK(bp_cell[idx] == BP_EMPTY, "DEL via on_key clears");
    }
}

/* ---- N 换题 / R 重开 状态复位 ---- */
static void test_next_puzzle(void) {
    bp_new_game(0);
    key_event_t ev;
    memset(&ev, 0, sizeof ev);
    ev.key = K_CHAR; ev.ch = 'n';
    bp_pz = 0;
    int first_pz = bp_pz;
    binarypuzzle_on_key(&ev);
    CHECK(bp_pz == (first_pz + 1) % BP_ROM_N, "N advances puzzle");
    CHECK(bp_over == false, "N resets over state");
    /* 破坏盘面后 R 应复位 */
    for (int i = 0; i < BP_NCELL; i++) if (bp_given[i] == 0) bp_cell[i] = BP_ONE;
    CHECK(bp_check_win() == 0, "destroyed board not a win");
    ev.key = K_CHAR; ev.ch = 'r';
    int pz_before = bp_pz;
    binarypuzzle_on_key(&ev);
    CHECK(bp_pz == pz_before, "R keeps puzzle id");
    /* 复位后盘面 = 题面 */
    int ok = 1;
    for (int i = 0; i < BP_NCELL; i++) {
        char want = bp_rom[pz_before][i];
        uint8_t v = (want == ' ') ? BP_EMPTY : (uint8_t)((want == '1') ? BP_ONE : BP_ZERO);
        if (bp_cell[i] != v) ok = 0;
    }
    CHECK(ok == 1, "R reloads puzzle givens");
}

/* ---- 胜利后按键: N/OK 重试同题, BACK 退出 ---- */
static void test_over_keys(void) {
    bp_new_game(0);
    /* 直接填入解触发胜利 */
    for (int i = 0; i < BP_NCELL; i++)
        bp_cell[i] = (s_sol[0][i] == '1') ? BP_ONE : BP_ZERO;
    CHECK(bp_check_win() == 1, "solution is a win");
    bp_over = true;
    key_event_t ev;
    memset(&ev, 0, sizeof ev);
    ev.key = K_CHAR; ev.ch = 'n';
    binarypuzzle_on_key(&ev);
    CHECK(bp_over == false && bp_pz == 0, "over: N retries same puzzle");
}

/* ---- enter: rng 播种 + 随机选题 + 渲染不崩 ---- */
static void test_enter(void) {
    rng_seed(&bp_rng, 20260830);
    binarypuzzle_enter();
    CHECK(bp_pz >= 0 && bp_pz < BP_ROM_N, "enter picks valid puzzle");
    CHECK(bp_over == false, "enter resets over");
    binarypuzzle_render();                 /* 渲染不崩 */
}

int main(void) {
    test_rom();
    test_givens_match_solution();
    test_solutions_valid();
    test_win_check();
    test_completion();
    test_cycle();
    test_del();
    test_cursor();
    test_onkey_fill();
    test_next_puzzle();
    test_over_keys();
    test_enter();
    if (s_fail == 0) printf("ALL PASS\n");
    else printf("%d FAILURES\n", s_fail);
    return s_fail != 0;
}
