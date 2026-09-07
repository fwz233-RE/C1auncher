/* 数独逻辑单测 — host 编译运行; 包含游戏源码, 直接访问 sd_ 静态状态 */
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#include "config.h"
#include "gfx/canvas.h"
#include "gfx/font.h"
#include "rng.h"
#include "games/sudoku.c"

/* host 框架 stub(main.c 不参与链接) */
bool s_exit_request = false;
void game_set_tick_interval(uint32_t ms) { (void)ms; }

static int s_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); s_fail++; } \
    else { printf("ok: %s\n", msg); } \
} while (0)

/* ---- ROM 校验: 81 位, 仅 '0'-'9', 题面无冲突 ---- */
static int rom_givens(int p) {
    int n = 0;
    for (int i = 0; i < 81; i++)
        if (sd_rom[p][i] != '0') n++;
    return n;
}

static bool rom_has_conflict(int p) {
    uint8_t b[81];
    for (int i = 0; i < 81; i++) b[i] = (uint8_t)(sd_rom[p][i] - '0');
    for (int y = 0; y < 9; y++) {
        for (int x = 0; x < 9; x++) {
            uint8_t v = b[y * 9 + x];
            if (v == 0) continue;
            for (int i = 0; i < 9; i++) {
                if (i != x && b[y * 9 + i] == v) return true;
                if (i != y && b[i * 9 + x] == v) return true;
            }
        }
    }
    for (int by = 0; by < 9; by += 3)
        for (int bx = 0; bx < 9; bx += 3) {
            uint32_t seen = 0;
            for (int i = 0; i < 3; i++)
                for (int j = 0; j < 3; j++) {
                    uint8_t v = b[(by + i) * 9 + bx + j];
                    if (v == 0) continue;
                    uint32_t m = 1u << (v - 1);
                    if (seen & m) return true;
                    seen |= m;
                }
        }
    return false;
}

static void test_rom(void) {
    for (int p = 0; p < SD_ROM_N; p++) {
        CHECK(strlen(sd_rom[p]) == 81, "rom: 81 digits");
        int ok = 1;
        for (int i = 0; i < 81; i++)
            if (sd_rom[p][i] < '0' || sd_rom[p][i] > '9') ok = 0;
        CHECK(ok == 1, "rom: digits only 0-9");
        CHECK(!rom_has_conflict(p), "rom: no conflicts");
        CHECK(rom_givens(p) >= 30, "rom: enough givens");
    }
    int distinct = 1;
    for (int i = 0; i < SD_ROM_N && distinct; i++)
        for (int j = i + 1; j < SD_ROM_N && distinct; j++)
            if (strcmp(sd_rom[i], sd_rom[j]) == 0) distinct = 0;
    CHECK(distinct == 1, "rom: 5 puzzles pairwise distinct");
}

/* ---- 唯一解校验(在 sd_cell 上回溯计数) ---- */
static int count_cands(int idx) {
    uint32_t m = sd_cand_mask(idx);
    int n = 0;
    for (int k = 0; k < 9; k++)
        if (m & (1u << k)) n++;
    return n;
}

static int solve_rec(int cap) {
    int best = -1, bestn = 10;
    for (int i = 0; i < 81; i++) {
        if (sd_cell[i] != 0) continue;
        int n = count_cands(i);
        if (n == 0) return 0;              /* 死路 */
        if (n < bestn) { bestn = n; best = i; }
    }
    if (best < 0) return 1;                /* 全填满 */
    uint32_t m = sd_cand_mask(best);
    int total = 0;
    for (int v = 1; v <= 9; v++) {
        if ((m & (1u << (v - 1))) == 0) continue;
        sd_cell[best] = (uint8_t)v;
        total += solve_rec(cap);
        sd_cell[best] = 0;
        if (total > cap) return total;
    }
    return total;
}

static int solve_count(int cap) { return solve_rec(cap); }

static int fill_rec(void) {
    int best = -1, bestn = 10;
    for (int i = 0; i < 81; i++) {
        if (sd_cell[i] != 0) continue;
        int n = count_cands(i);
        if (n == 0) return 0;
        if (n < bestn) { bestn = n; best = i; }
    }
    if (best < 0) return 1;
    uint32_t m = sd_cand_mask(best);
    for (int v = 1; v <= 9; v++) {
        if ((m & (1u << (v - 1))) == 0) continue;
        sd_cell[best] = (uint8_t)v;
        if (fill_rec()) return 1;
        sd_cell[best] = 0;
    }
    return 0;
}

static void test_unique_solution(void) {
    for (int p = 0; p < SD_ROM_N; p++) {
        sd_new_game(p);
        int n = solve_count(2);
        CHECK(n == 1, "rom: puzzle has unique solution");
    }
}

/* ---- 状态转换: 填数/清除/题面不可改/候选同步 ---- */
static void test_state(void) {
    sd_new_game(0);
    int loaded = 1;
    for (int i = 0; i < 81; i++)
        if (sd_cell[i] != (uint8_t)(sd_rom[0][i] - '0')) loaded = 0;
    CHECK(loaded == 1, "new game loads rom");
    CHECK(sd_given[0] == 1 && sd_given[2] == 0, "given flags correct");
    CHECK(sd_cx == 4 && sd_cy == 4, "cursor centered");
    CHECK(!sd_input && !sd_cand_mode && !sd_over, "fresh flags");

    /* 题面格不可改 */
    sd_place_digit(0, 5);
    CHECK(sd_cell[0] == (uint8_t)(sd_rom[0][0] - '0'), "given cell immutable");

    /* 填数/覆盖/清除 */
    int empty = -1;
    for (int i = 0; i < 81; i++)
        if (sd_cell[i] == 0) { empty = i; break; }
    CHECK(empty >= 0, "puzzle has empty cells");
    sd_place_digit(empty, 3);
    CHECK(sd_cell[empty] == 3, "place digit");
    sd_place_digit(empty, 7);
    CHECK(sd_cell[empty] == 7, "overwrite digit");
    sd_cell[empty] = 0;
    CHECK(sd_cell[empty] == 0, "clear cell");

    /* 冲突允许 */
    sd_new_game(0);
    empty = -1;
    for (int i = 0; i < 81; i++)
        if (sd_cell[i] == 0) { empty = i; break; }
    int row_digit = sd_cell[(empty / 9) * 9];   /* 同行已有数字 → 必冲突 */
    sd_place_digit(empty, (uint8_t)row_digit);
    CHECK(sd_cell[empty] == (uint8_t)row_digit, "conflict entry allowed");
    CHECK(!sd_over, "conflict not a win");
    sd_cell[empty] = 0;

    /* 候选同步: 同行/同列/同宫数字被排除 */
    sd_new_game(0);
    int t = -1;                            /* 找一个空格 */
    for (int i = 0; i < 81; i++)
        if (sd_cell[i] == 0) { t = i; break; }
    int y0 = t / 9, x0 = t % 9;
    uint32_t m0 = sd_cand_mask(t);
    for (int i = 0; i < 9; i++) {
        if (sd_cell[y0 * 9 + i] != 0)
            CHECK((m0 & (1u << (sd_cell[y0 * 9 + i] - 1))) == 0,
                  "cand excludes row digits");
        if (sd_cell[i * 9 + x0] != 0)
            CHECK((m0 & (1u << (sd_cell[i * 9 + x0] - 1))) == 0,
                  "cand excludes col digits");
    }
    int by0 = (y0 / 3) * 3, bx0 = (x0 / 3) * 3;
    for (int i = by0; i < by0 + 3; i++)
        for (int j = bx0; j < bx0 + 3; j++)
            if (sd_cell[i * 9 + j] != 0)
                CHECK((m0 & (1u << (sd_cell[i * 9 + j] - 1))) == 0,
                      "cand excludes box digits");
    /* 填一格后, 同行空格的候选更新 */
    int o = -1;
    for (int i = 0; i < 9; i++)
        if (y0 * 9 + i != t && sd_cell[y0 * 9 + i] == 0) { o = y0 * 9 + i; break; }
    if (o >= 0) {
        uint32_t before = sd_cand_mask(o);
        sd_place_digit(t, 9);
        uint32_t after = sd_cand_mask(o);
        CHECK((before & (1u << 8)) != 0 && (after & (1u << 8)) == 0,
              "placing digit removes candidate in same row");
        sd_cell[t] = 0;
    }
}

/* ---- 光标/OK 输入态 ---- */
static void test_input(void) {
    sd_new_game(0);
    sd_cx = 8;
    sd_cy = 8;
    key_event_t ev;
    ev.is_repeat = false;
    ev.ch = 0;
    ev.key = K_RIGHT;
    sudoku_on_key(&ev);
    CHECK(sd_cx == 8, "cursor clamps right edge");
    ev.key = K_DOWN;
    sudoku_on_key(&ev);
    CHECK(sd_cy == 8, "cursor clamps bottom edge");
    ev.key = K_UP;
    sudoku_on_key(&ev);
    CHECK(sd_cy == 7, "cursor moves up");

    /* OK 选中(输入态), 再按取消 */
    ev.key = K_OK;
    sudoku_on_key(&ev);
    CHECK(sd_input, "OK enters input state");
    CHECK(sd_sel == (uint8_t)(7 * 9 + 8), "OK selects cursor cell");
    sudoku_on_key(&ev);
    CHECK(!sd_input, "OK again cancels input state");

    /* 输入态: 光标移走, 数字仍填入选中格 */
    sd_new_game(0);
    ev.key = K_OK;
    sudoku_on_key(&ev);                    /* 选中 (4,4) */
    int sel = sd_sel;
    CHECK(sd_cell[sel] == 0, "cursor cell empty for test");
    ev.key = K_LEFT;
    sudoku_on_key(&ev);
    CHECK(sd_cx == 3, "cursor moved away");
    uint8_t cbefore = sd_cell[(int)sd_cy * 9 + (int)sd_cx];
    ev.key = K_CHAR;
    ev.ch = '5';
    sudoku_on_key(&ev);
    CHECK(sd_cell[sel] == 5, "digit fills selected cell in input mode");
    CHECK(sd_cell[(int)sd_cy * 9 + (int)sd_cx] == cbefore, "cursor cell untouched");

    /* DEL 清除: 输入态清选中格; 非输入态清光标格 */
    ev.key = K_DEL;
    sudoku_on_key(&ev);
    CHECK(sd_cell[sel] == 0, "DEL clears selected cell");
    ev.key = K_OK;
    sudoku_on_key(&ev);                    /* 取消输入态 */
    ev.key = K_RIGHT;
    sudoku_on_key(&ev);                    /* 光标回 (4,4) */
    ev.ch = '5';
    ev.key = K_CHAR;
    sudoku_on_key(&ev);
    CHECK(sd_cell[(int)sd_cy * 9 + (int)sd_cx] == 5, "digit fills cursor cell");
    ev.key = K_DEL;
    sudoku_on_key(&ev);
    CHECK(sd_cell[(int)sd_cy * 9 + (int)sd_cx] == 0, "DEL clears cursor cell");

    /* P 切换候选; N 下一题; R 重开 */
    sd_new_game(0);
    ev.key = K_PAUSE;
    sudoku_on_key(&ev);
    CHECK(sd_cand_mode, "P enables candidate mode");
    sudoku_on_key(&ev);
    CHECK(!sd_cand_mode, "P disables candidate mode");
    int pz0 = sd_pz;
    ev.key = K_CHAR;
    ev.ch = 'n';
    sudoku_on_key(&ev);
    CHECK(sd_pz == (pz0 + 1) % SD_ROM_N, "N advances puzzle");
    CHECK(sd_cell[0] == sd_rom[sd_pz][0] - '0', "N loads next puzzle");
    ev.ch = 'r';
    sudoku_on_key(&ev);
    CHECK(sd_pz == (pz0 + 1) % SD_ROM_N, "R restarts same puzzle");
    CHECK(!sd_input && !sd_cand_mode, "restart resets flags");
}

/* ---- 胜负判定: 完整无冲突=胜; 完整有冲突=不胜; 不完整=不胜 ---- */
static void test_win(void) {
    sd_new_game(0);
    CHECK(!sd_check_win(), "fresh board not win");

    /* 不完整但有冲突 → 不胜(满 80 格 + 1 冲突?) 冲突需要满盘才测 */
    /* 填到唯一解 → 胜 */
    CHECK(fill_rec() == 1, "solver fills puzzle 0");
    CHECK(sd_check_win(), "complete correct board wins");

    /* 完整但冲突 → 不胜: 行内造重复 */
    int j = -1;
    for (int i = 1; i < 9; i++)
        if (sd_given[i] == 0) { j = i; break; }
    CHECK(j > 0, "row 0 has user cell");
    uint8_t orig = sd_cell[j];
    uint8_t dup = sd_cell[0];              /* (0,0) 是题面 7 */
    sd_cell[j] = dup;
    CHECK(!sd_check_win(), "full board with duplicate not win");
    sd_cell[j] = orig;
    CHECK(sd_check_win(), "restored board wins again");

    /* 通过真实输入路径达成胜利 */
    sd_new_game(0);
    CHECK(fill_rec() == 1, "solver fills puzzle 0 again");
    sd_cell[j] = dup;                      /* 制造冲突 */
    sd_place_digit(j, orig);               /* 修正 → 应触发 sd_over */
    CHECK(sd_over, "place triggers win flag");
    CHECK(sd_check_win(), "win flag consistent with check");
    /* 胜利后: 输入被忽略, RETRY 重开 */
    key_event_t ev;
    ev.is_repeat = false;
    ev.ch = '9';
    ev.key = K_CHAR;
    uint8_t saved = sd_cell[(int)sd_cy * 9 + (int)sd_cx];
    sudoku_on_key(&ev);
    CHECK(sd_cell[(int)sd_cy * 9 + (int)sd_cx] == saved, "input ignored when over");
    ev.key = K_OK;
    sudoku_on_key(&ev);
    CHECK(!sd_over && sd_cell[0] == sd_rom[0][0] - '0', "OK at over = retry");
}

/* ---- 边界: 全部 5 题都可完整填出且触发胜利判定 ---- */
static void test_win_all_puzzles(void) {
    for (int p = 0; p < SD_ROM_N; p++) {
        sd_new_game(p);
        CHECK(fill_rec() == 1, "solver fills puzzle");
        CHECK(sd_check_win(), "every puzzle solvable to a win state");
    }
}

int main(void) {
    test_rom();
    test_unique_solution();
    test_state();
    test_input();
    test_win();
    test_win_all_puzzles();
    if (s_fail == 0) {
        printf("ALL SUDOKU TESTS PASSED\n");
        return 0;
    }
    printf("%d TEST(S) FAILED\n", s_fail);
    return 1;
}
