/* NURIKABE 主机逻辑单测 — 包含游戏 .c 与独立 C 求解器, 断言:
 *  - 3 个 ROM 谜题唯一解(求解器全量枚举 == 1, 找第 2 解即停)
 *  - 胜判定: 解盘→true; 各类违规→false(岛尺寸/无数字岛/2x2 黑块/墙断开)
 *  - 交互: OK 黑/白切换、DEL 清点、数字格锁定、N 换题、结束画面 OK
 *  - HUD: ISLES 计数; 谜题数据合法性
 */
#include <stdio.h>
#include <string.h>

#include "../../src/games/nurikabe.c"
#include "../support/nk_solver.c"

/* 框架符号(host stub, main.c 提供) */
bool s_exit_request = false;
void game_set_tick_interval(uint32_t ms) { (void)ms; }

static int s_fail = 0;
static int s_pass = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); s_fail++; } \
    else { printf("ok: %s\n", msg); s_pass++; } \
} while (0)

/* 3 题对应解(W=岛 B=墙) — 生成脚本输出, 与谜题一致(脚本已验证) */
static const char *nk_sol[3][8] = {
    { "WWWBWBWW", "BBBBBBBB", "WBWBWWWB", "BBBBWBBB",
      "WBWBWBWB", "WBWBBBBB", "WBWBWBWB", "BBBBBBBB" },
    { "WWWBWBWW", "BBWBBBBB", "WBWBWWWB", "BBWBWBBB",
      "WBWBWBWB", "WBWBWBBB", "WBWBWBWB", "BBBBBBBB" },
    { "BBBBBBBB", "WWWWWWWB", "BBBBBBWB", "WWWWWBWB",
      "BBWBBBBB", "WBWBWWWB", "BBBBBBBB", "WWWBWBWB" },
};

/* 把解盘装入玩家盘面 */
static void load_sol(int p) {
    nk_start((uint8_t)p);
    for (int r = 0; r < 8; r++)
        for (int c = 0; c < 8; c++)
            nk_cell[r * 8 + c] = (nk_sol[p][r][c] == 'B') ? NK_BLACK : NK_WHITE;
}

/* ---- 谜题数据合法性 + 解盘一致性 ---- */
static void test_data_sanity(void) {
    int clues[3] = { 0 }, whites[3] = { 0 }, ok_chars = 1, ok_clue_white = 1;
    for (int p = 0; p < 3; p++) {
        for (int r = 0; r < 8; r++) {
            for (int c = 0; c < 8; c++) {
                char pc = nk_puz[p][r][c];
                if (pc >= '1' && pc <= '9') {
                    clues[p] += pc - '0';
                    if (nk_sol[p][r][c] != 'W') ok_clue_white = 0;
                } else if (pc != '.') {
                    ok_chars = 0;
                }
                if (nk_sol[p][r][c] == 'W') whites[p]++;
            }
        }
    }
    CHECK(ok_chars, "puzzle cells are digits 1-9 or dots");
    CHECK(ok_clue_white, "clue cells are white in solutions");
    CHECK(clues[0] == whites[0] && clues[1] == whites[1] &&
          clues[2] == whites[2], "clue sum == white cell count");
}

/* ---- 3 题唯一解(独立求解器) ---- */
static void test_unique_solutions(void) {
    for (int p = 0; p < 3; p++) {
        const char *rows[8];
        for (int r = 0; r < 8; r++) rows[r] = nk_puz[p][r];
        nks_t s;
        long nodes = 0;
        int cnt = nks_solve(&s, rows, 2, &nodes);   /* 找到第 2 解即停 */
        char msg[64];
        snprintf(msg, sizeof msg, "puzzle %d unique (solutions=%d)", p + 1, cnt);
        CHECK(cnt == 1, msg);
    }
}

/* ---- 胜判定 ---- */
static void test_win(void) {
    char msg[64];
    for (int p = 0; p < 3; p++) {
        load_sol(p);
        snprintf(msg, sizeof msg, "puzzle %d solution -> solved", p + 1);
        CHECK(nk_solved(), msg);
    }
    /* 未填满 */
    load_sol(0);
    nk_cell[0] = NK_UNKNOWN;
    CHECK(!nk_solved(), "unknown cell left -> not solved");
    /* 墙变白 → 岛超编 */
    load_sol(0);
    nk_cell[1 * 8 + 0] = NK_WHITE;      /* (1,0) 解为黑 */
    CHECK(!nk_solved(), "wall->white breaks island size");
    /* 岛变黑 → 岛缩水 */
    load_sol(0);
    nk_cell[0] = NK_BLACK;              /* 数字格(3)变黑 */
    CHECK(!nk_solved(), "island->black shrinks island");
    /* 2x2 黑块 */
    load_sol(1);
    nk_cell[0] = NK_BLACK;
    nk_cell[1] = NK_BLACK;
    nk_cell[8] = NK_BLACK;
    nk_cell[9] = NK_BLACK;              /* (0,0)(0,1)(1,0)(1,1) 全黑 */
    CHECK(!nk_solved(), "2x2 black block rejected");
    /* 无数字白区 */
    load_sol(0);
    nk_cell[3 * 8 + 3] = NK_WHITE;      /* (3,3) 四周皆墙, 孤立无数字 */
    CHECK(!nk_solved(), "unnumbered island rejected");
    /* 两数字岛相触: 第 2 题 (2,1) 墙变白, 桥接 (2,0) 与 (2,2) 两个 1 岛 */
    load_sol(1);
    nk_cell[2 * 8 + 1] = NK_WHITE;
    CHECK(!nk_solved(), "touching clue regions rejected");
    /* 墙断开(仅墙连通违规, 岛与 2x2 均合法) */
    {
        static const char *split[8] = {
            "WWBBWBWW", "BWWBBBBB", "WBWBWWWB", "BBWBWBBB",
            "WBWBWBWB", "WBWBWBBB", "WBWBWBWB", "BBBBBBBB" };
        nk_start(1);
        for (int r = 0; r < 8; r++)
            for (int c = 0; c < 8; c++)
                nk_cell[r * 8 + c] = (split[r][c] == 'B') ? NK_BLACK : NK_WHITE;
        CHECK(!nk_solved(), "split wall rejected (islands still valid)");
    }
}

/* ---- 交互 ---- */
static void test_interaction(void) {
    /* OK 黑/白切换, DEL 清点 */
    nk_start(0);
    nk_cx = 2;
    nk_cy = 0;                  /* (0,2) 非数字格 */
    nk_toggle();
    CHECK(nk_cell[2] == NK_BLACK, "OK on unknown -> black");
    nk_toggle();
    CHECK(nk_cell[2] == NK_WHITE, "OK black -> white");
    nk_toggle();
    CHECK(nk_cell[2] == NK_BLACK, "OK white -> black");
    nk_clear_cell();
    CHECK(nk_cell[2] == NK_UNKNOWN, "DEL -> unknown");
    /* 数字格锁定 */
    nk_cx = 0;
    nk_cy = 0;                  /* (0,0) = 3 */
    nk_toggle();
    CHECK(nk_cell[0] == NK_WHITE, "clue cell locked by OK");
    nk_clear_cell();
    CHECK(nk_cell[0] == NK_WHITE, "clue cell locked by DEL");
    /* 方向键边界 */
    nk_start(0);
    nk_cx = 7;
    nk_cy = 7;
    nurikabe_on_key(&(key_event_t){ K_RIGHT, 0, false });
    CHECK(nk_cx == 7 && nk_cy == 7, "right clamped at edge");
    nurikabe_on_key(&(key_event_t){ K_UP, 0, false });
    CHECK(nk_cy == 6, "up moves cursor");
    /* 重复确认键忽略 */
    nurikabe_on_key(&(key_event_t){ K_OK, 0, true });
    CHECK(nk_cell[7 * 8 + 7] == NK_UNKNOWN, "repeat OK ignored");
    /* N 换题 */
    nurikabe_on_key(&(key_event_t){ K_CHAR, 'n', false });
    CHECK(nk_idx == 1, "N advances puzzle");
    /* 完成 → 结束画面 → OK 下一题 */
    load_sol(1);
    nk_cell[2] = NK_UNKNOWN;    /* (0,2) 解为白 → 清未知 */
    nk_cx = 2;
    nk_cy = 0;
    nk_toggle();                /* 未知 → 黑 */
    nk_toggle();                /* 黑 → 白 = 完成 */
    CHECK(nk_over, "solving triggers over state");
    nurikabe_on_key(&(key_event_t){ K_OK, 0, false });
    CHECK(!nk_over && nk_idx == 2, "over-screen OK -> next puzzle");
}

/* ---- HUD ISLES 计数 ---- */
static void test_isles_hud(void) {
    load_sol(0);
    CHECK(nk_isles_ok() == 11, "all 11 islands counted on solution 1");
    nk_cell[0] = NK_BLACK;      /* 破坏 (0,0) 3 岛 */
    CHECK(nk_isles_ok() == 10, "broken island not counted");
    /* 开局即满足的岛计入: 第 1 题有 6 个 1 岛(单格即满) */
    nk_start(0);
    CHECK(nk_isles_ok() == 6, "empty board counts size-1 islands");
}

int main(void) {
    test_data_sanity();
    test_unique_solutions();
    test_win();
    test_interaction();
    test_isles_hud();
    printf("\n%d passed, %d failed\n", s_pass, s_fail);
    return s_fail ? 1 : 0;
}
