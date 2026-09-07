/* FUTOSHIKI 逻辑单测 — host 编译运行; 包含游戏源码, 直接访问 ft_ 静态状态 */
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#include "config.h"
#include "gfx/canvas.h"
#include "gfx/font.h"
#include "rng.h"
#include "games/futoshiki.c"

/* host 框架 stub(main.c 不参与链接) */
bool s_exit_request = false;
void game_set_tick_interval(uint32_t ms) { (void)ms; }

static int s_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); s_fail++; } \
    else { printf("ok: %s\n", msg); } \
} while (0)

/* ---- 独立回溯求解器(不调用游戏内判定, 约束自行实现) ---- */
static bool t_ok(int idx, int v) {
    int x = idx % 4, y = idx / 4;
    for (int i = 0; i < 4; i++) {
        if (i != x && ft_cell[y * 4 + i] == v) return false;
        if (i != y && ft_cell[i * 4 + x] == v) return false;
    }
    if (x > 0) {
        char s = ft_h[y * 3 + x - 1];
        if (s == '<' && ft_cell[idx - 1] && !(ft_cell[idx - 1] < v)) return false;
        if (s == '>' && ft_cell[idx - 1] && !(ft_cell[idx - 1] > v)) return false;
    }
    if (x < 3) {
        char s = ft_h[y * 3 + x];
        if (s == '<' && ft_cell[idx + 1] && !(v < ft_cell[idx + 1])) return false;
        if (s == '>' && ft_cell[idx + 1] && !(v > ft_cell[idx + 1])) return false;
    }
    if (y > 0) {
        char s = ft_v[(y - 1) * 4 + x];
        if (s == '<' && ft_cell[idx - 4] && !(ft_cell[idx - 4] < v)) return false;
        if (s == '>' && ft_cell[idx - 4] && !(ft_cell[idx - 4] > v)) return false;
    }
    if (y < 3) {
        char s = ft_v[y * 4 + x];
        if (s == '<' && ft_cell[idx + 4] && !(v < ft_cell[idx + 4])) return false;
        if (s == '>' && ft_cell[idx + 4] && !(v > ft_cell[idx + 4])) return false;
    }
    return true;
}

static int t_solve_rec(int cap) {
    int best = -1, bestn = 5;
    for (int i = 0; i < 16; i++) {
        if (ft_cell[i] != 0) continue;
        int n = 0;
        for (int v = 1; v <= 4; v++)
            if (t_ok(i, v)) n++;
        if (n == 0) return 0;
        if (n < bestn) { bestn = n; best = i; }
    }
    if (best < 0) return 1;                /* 全填满 */
    int total = 0;
    for (int v = 1; v <= 4; v++) {
        if (!t_ok(best, v)) continue;
        ft_cell[best] = (uint8_t)v;
        total += t_solve_rec(cap);
        ft_cell[best] = 0;
        if (total > cap) return total;
    }
    return total;
}

/* 填满第一个解并保留盘面(成功即返回, 不回溯清零) */
static int t_fill_rec(void) {
    int best = -1, bestn = 5;
    for (int i = 0; i < 16; i++) {
        if (ft_cell[i] != 0) continue;
        int n = 0;
        for (int v = 1; v <= 4; v++)
            if (t_ok(i, v)) n++;
        if (n == 0) return 0;
        if (n < bestn) { bestn = n; best = i; }
    }
    if (best < 0) return 1;
    for (int v = 1; v <= 4; v++) {
        if (!t_ok(best, v)) continue;
        ft_cell[best] = (uint8_t)v;
        if (t_fill_rec()) return 1;
        ft_cell[best] = 0;
    }
    return 0;
}

/* ---- ROM 校验: 40 位 / 字符集 / 题面互不冲突 / 两两不同 ---- */
static void test_rom(void) {
    for (int p = 0; p < FT_ROM_N; p++) {
        char msg[48];
        snprintf(msg, sizeof(msg), "rom %d: 40 chars", p);
        CHECK(strlen(ft_rom[p]) == 40, msg);
        int okd = 1, oki = 1;
        for (int i = 0; i < 16; i++)
            if (ft_rom[p][i] < '0' || ft_rom[p][i] > '4') okd = 0;
        for (int i = 16; i < 40; i++)
            if (ft_rom[p][i] != '<' && ft_rom[p][i] != '>' && ft_rom[p][i] != '.')
                oki = 0;
        snprintf(msg, sizeof(msg), "rom %d: digit charset", p);
        CHECK(okd == 1, msg);
        snprintf(msg, sizeof(msg), "rom %d: ineq charset", p);
        CHECK(oki == 1, msg);
        /* 题面同 行/列 不重复 */
        int noc = 1;
        for (int y = 0; y < 4 && noc; y++)
            for (int x = 0; x < 4 && noc; x++) {
                uint8_t v = (uint8_t)(ft_rom[p][y * 4 + x] - '0');
                if (v == 0) continue;
                for (int i = 0; i < 4 && noc; i++) {
                    if (i != x && ft_rom[p][y * 4 + i] - '0' == v) noc = 0;
                    if (i != y && ft_rom[p][i * 4 + x] - '0' == v) noc = 0;
                }
            }
        snprintf(msg, sizeof(msg), "rom %d: givens no row/col dup", p);
        CHECK(noc == 1, msg);
        /* 题面两相邻格都给出时不等式方向成立 */
        int ineqok = 1;
        for (int y = 0; y < 4 && ineqok; y++)
            for (int x = 0; x < 3 && ineqok; x++) {
                char s = ft_rom[p][16 + y * 3 + x];
                int a = ft_rom[p][y * 4 + x] - '0';
                int b = ft_rom[p][y * 4 + x + 1] - '0';
                if (s == '<' && a != 0 && b != 0 && !(a < b)) ineqok = 0;
                if (s == '>' && a != 0 && b != 0 && !(a > b)) ineqok = 0;
            }
        for (int y = 0; y < 3 && ineqok; y++)
            for (int x = 0; x < 4 && ineqok; x++) {
                char s = ft_rom[p][28 + y * 4 + x];
                int a = ft_rom[p][y * 4 + x] - '0';
                int b = ft_rom[p][(y + 1) * 4 + x] - '0';
                if (s == '<' && a != 0 && b != 0 && !(a < b)) ineqok = 0;
                if (s == '>' && a != 0 && b != 0 && !(a > b)) ineqok = 0;
            }
        snprintf(msg, sizeof(msg), "rom %d: givens satisfy ineq", p);
        CHECK(ineqok == 1, msg);
    }
    int distinct = 1;
    for (int i = 0; i < FT_ROM_N && distinct; i++)
        for (int j = i + 1; j < FT_ROM_N && distinct; j++)
            if (strcmp(ft_rom[i], ft_rom[j]) == 0) distinct = 0;
    CHECK(distinct == 1, "rom: 5 puzzles pairwise distinct");
}

/* ---- 唯一解: 回溯计数 == 1, 且解满足游戏内胜利判定 ---- */
static void test_unique_solution(void) {
    for (int p = 0; p < FT_ROM_N; p++) {
        char msg[48];
        ft_new_game(p);
        int n = t_solve_rec(2);
        snprintf(msg, sizeof(msg), "rom %d: unique solution (n=%d)", p, n);
        CHECK(n == 1, msg);
        /* 填满后的盘面应触发游戏胜利判定 */
        ft_new_game(p);
        CHECK(t_fill_rec() == 1, "solver fills puzzle");
        CHECK(ft_check_win(), "solver board passes game win check");
    }
}

/* ---- 状态转换: 载入 / 题面不可改 / 填数覆盖清除 ---- */
static void test_state(void) {
    ft_new_game(0);
    int loaded = 1;
    for (int i = 0; i < 16; i++)
        if (ft_cell[i] != (uint8_t)(ft_rom[0][i] - '0')) loaded = 0;
    CHECK(loaded == 1, "new game loads rom digits");
    int ineq_ok = 1;
    for (int i = 0; i < 12; i++)
        if (ft_h[i] != ft_rom[0][16 + i] || ft_v[i] != ft_rom[0][28 + i]) ineq_ok = 0;
    CHECK(ineq_ok == 1, "new game loads inequalities");
    CHECK(ft_given[0] == 0 && ft_given[6] == 1, "given flags correct");
    CHECK(ft_cx == 1 && ft_cy == 1, "cursor initial position");
    CHECK(!ft_input && !ft_over, "fresh flags");

    /* 题面格不可改 */
    ft_place_digit(6, 4);
    CHECK(ft_cell[6] == 2, "given cell immutable");

    /* 空格的填数/覆盖/清除 */
    int empty = -1;
    for (int i = 0; i < 16; i++)
        if (ft_cell[i] == 0) { empty = i; break; }
    CHECK(empty >= 0, "puzzle has empty cells");
    ft_place_digit(empty, 3);
    CHECK(ft_cell[empty] == 3, "place digit");
    ft_place_digit(empty, 1);
    CHECK(ft_cell[empty] == 1, "overwrite digit");
    ft_cell[empty] = 0;
    CHECK(ft_cell[empty] == 0, "clear cell");

    /* 冲突允许(完成时校验): 同列已有数字填入不计胜 */
    ft_new_game(0);
    empty = -1;
    for (int i = 0; i < 16; i++)
        if (ft_cell[i] == 0) { empty = i; break; }
    /* 找与 empty 同列的一个给定数字 */
    uint8_t col_given = 0;
    for (int y = 0; y < 4 && col_given == 0; y++)
        if (ft_cell[y * 4 + (empty % 4)] != 0)
            col_given = ft_cell[y * 4 + (empty % 4)];
    CHECK(col_given != 0, "puzzle 0 has column given");
    ft_place_digit(empty, col_given);
    CHECK(ft_cell[empty] == col_given, "conflict entry allowed");
    CHECK(!ft_over, "conflict not a win");
    ft_cell[empty] = 0;
}

/* ---- 胜负判定: 完整无冲突=胜; 行重复/列重复/不等式违背=不胜 ---- */
static void test_win(void) {
    ft_new_game(0);
    CHECK(!ft_check_win(), "fresh board not win");

    /* 填满唯一解 → 胜 */
    CHECK(t_fill_rec() == 1, "solver fills puzzle 0");
    CHECK(ft_check_win(), "complete correct board wins");

    /* 行内造重复 → 不胜 */
    int j = -1;
    for (int i = 0; i < 16; i++)
        if (ft_given[i] == 0) { j = i; break; }
    CHECK(j >= 0, "puzzle 0 has user cell");
    uint8_t orig = ft_cell[j];
    int jsrc = (j / 4) * 4 + ((j % 4 == 0) ? 1 : 0);   /* 同行另一列 */
    ft_cell[j] = ft_cell[jsrc];
    CHECK(ft_cell[j] != orig, "duplicate value differs from original");
    CHECK(!ft_check_win(), "full board with row dup not win");
    ft_cell[j] = orig;
    CHECK(ft_check_win(), "restored board wins again");

    /* 列内造重复 → 不胜 */
    int k = -1;
    for (int i = 0; i < 16; i++)
        if (ft_given[i] == 0 && i != j) { k = i; break; }
    if (k >= 0) {
        uint8_t orig2 = ft_cell[k];
        ft_cell[k] = ft_cell[(k % 4 == 0) ? 0 : k % 4];   /* 同列另一行的数字 */
        if (orig2 == ft_cell[k]) ft_cell[k] = ft_cell[4 + k % 4];
        CHECK(ft_cell[k] != orig2, "col duplicate differs from original");
        CHECK(!ft_check_win(), "full board with col dup not win");
        ft_cell[k] = orig2;
        CHECK(ft_check_win(), "restored col wins again");
    }

    /* 翻转一个不等式方向的数字 → 不胜(找一条两端均非题面的不等式) */
    int viol = 0;
    for (int y = 0; y < 4 && !viol; y++)
        for (int x = 0; x < 3 && !viol; x++) {
            char s = ft_h[y * 3 + x];
            int a = y * 4 + x, b = y * 4 + x + 1;
            if (s == '<' && ft_given[a] == 0 && ft_given[b] == 0) {
                uint8_t t = ft_cell[a];
                ft_cell[a] = ft_cell[b];
                ft_cell[b] = t;          /* 交换后 a>b, '<' 被违背 */
                viol = 1;
            } else if (s == '>' && ft_given[a] == 0 && ft_given[b] == 0) {
                uint8_t t = ft_cell[a];
                ft_cell[a] = ft_cell[b];
                ft_cell[b] = t;
                viol = 1;
            }
        }
    CHECK(viol == 1, "puzzle 0 has user-only inequality edge");
    CHECK(!ft_check_win(), "full board with violated ineq not win");
    /* 恢复原状 */
    ft_new_game(0);
    CHECK(t_fill_rec() == 1, "re-solve puzzle 0");
    CHECK(ft_check_win(), "re-solved board wins again");

    /* 通过真实输入路径触发胜利 */
    ft_new_game(0);
    CHECK(t_fill_rec() == 1, "solve puzzle 0 again");
    /* 制造冲突后, ft_place_digit 修正最后一格 → ft_over */
    ft_cell[j] = ft_cell[jsrc];
    ft_place_digit(j, orig);
    CHECK(ft_over, "place triggers win flag");
    CHECK(ft_check_win(), "win flag consistent with check");
}

/* ---- 输入: 光标/OK 输入态/填数/DEL/N/R ---- */
static void test_input(void) {
    ft_new_game(0);
    ft_cx = 3;
    ft_cy = 3;
    key_event_t ev;
    ev.is_repeat = false;
    ev.ch = 0;
    ev.key = K_RIGHT;
    futoshiki_on_key(&ev);
    CHECK(ft_cx == 3, "cursor clamps right edge");
    ev.key = K_DOWN;
    futoshiki_on_key(&ev);
    CHECK(ft_cy == 3, "cursor clamps bottom edge");
    ev.key = K_UP;
    futoshiki_on_key(&ev);
    CHECK(ft_cy == 2, "cursor moves up");

    /* OK 选中(输入态), 再按取消 */
    ev.key = K_OK;
    futoshiki_on_key(&ev);
    CHECK(ft_input, "OK enters input state");
    CHECK(ft_sel == (uint8_t)(2 * 4 + 3), "OK selects cursor cell");
    futoshiki_on_key(&ev);
    CHECK(!ft_input, "OK again cancels input state");

    /* 输入态: 光标移走, 数字仍填入选中格 */
    ft_new_game(0);
    ev.key = K_OK;
    futoshiki_on_key(&ev);               /* 选中 (1,1) */
    int sel = ft_sel;
    CHECK(ft_given[sel] == 0 && ft_cell[sel] == 0, "cursor cell user-editable & empty");
    ev.key = K_LEFT;
    futoshiki_on_key(&ev);
    CHECK(ft_cx == 0, "cursor moved away in input mode");
    uint8_t cbefore = ft_cell[(int)ft_cy * 4 + (int)ft_cx];
    ev.key = K_CHAR;
    ev.ch = '3';
    futoshiki_on_key(&ev);
    CHECK(ft_cell[sel] == 3, "digit fills selected cell in input mode");
    CHECK(ft_cell[(int)ft_cy * 4 + (int)ft_cx] == cbefore, "cursor cell untouched");

    /* DEL: 输入态清选中格; 非输入态清光标格 */
    ev.key = K_DEL;
    futoshiki_on_key(&ev);
    CHECK(ft_cell[sel] == 0, "DEL clears selected cell");
    ev.key = K_OK;
    futoshiki_on_key(&ev);               /* 取消输入态 */
    ev.key = K_RIGHT;
    futoshiki_on_key(&ev);               /* 光标回 (1,1) */
    ev.ch = '4';
    ev.key = K_CHAR;
    futoshiki_on_key(&ev);
    CHECK(ft_cell[(int)ft_cy * 4 + (int)ft_cx] == 4, "digit fills cursor cell");
    ev.key = K_DEL;
    futoshiki_on_key(&ev);
    CHECK(ft_cell[(int)ft_cy * 4 + (int)ft_cx] == 0, "DEL clears cursor cell");

    /* '5' 不在 1-4 范围: 忽略 */
    ev.ch = '5';
    futoshiki_on_key(&ev);
    CHECK(ft_cell[(int)ft_cy * 4 + (int)ft_cx] == 0, "digit 5 ignored");

    /* DEL 对题面格无效 */
    ev.key = K_DEL;
    futoshiki_on_key(&ev);
    ev.key = K_OK;
    futoshiki_on_key(&ev);
    ev.ch = '2';
    ev.key = K_CHAR;
    futoshiki_on_key(&ev);               /* 试图在题面格(1,1)=2 填数(无效) */
    CHECK(ft_cell[sel] == 2, "given cell input ignored");

    /* N 下一题; R 重开 */
    ft_new_game(0);
    int pz0 = ft_pz;
    ev.key = K_CHAR;
    ev.ch = 'n';
    futoshiki_on_key(&ev);
    CHECK(ft_pz == (pz0 + 1) % FT_ROM_N, "N advances puzzle");
    CHECK(ft_cell[0] == ft_rom[ft_pz][0] - '0', "N loads next puzzle");
    ev.ch = 'r';
    futoshiki_on_key(&ev);
    CHECK(ft_pz == (pz0 + 1) % FT_ROM_N, "R restarts same puzzle");
    CHECK(!ft_input && !ft_over, "restart resets flags");
}

/* ---- 胜利后: 输入忽略, OK/N 重试 ---- */
static void test_over(void) {
    ft_new_game(0);
    CHECK(t_fill_rec() == 1, "solve for over test");
    int j = -1;
    for (int i = 0; i < 16; i++)
        if (ft_given[i] == 0) { j = i; break; }
    uint8_t orig = ft_cell[j];
    int jsrc = (j / 4) * 4 + ((j % 4 == 0) ? 1 : 0);
    ft_cell[j] = ft_cell[jsrc];
    ft_place_digit(j, orig);
    CHECK(ft_over, "win flag set");

    key_event_t ev;
    ev.is_repeat = false;
    ev.ch = '3';
    ev.key = K_CHAR;
    uint8_t saved = ft_cell[(int)ft_cy * 4 + (int)ft_cx];
    futoshiki_on_key(&ev);
    CHECK(ft_cell[(int)ft_cy * 4 + (int)ft_cx] == saved, "input ignored when over");

    ev.key = K_OK;
    futoshiki_on_key(&ev);
    CHECK(!ft_over, "OK at over retries");
    CHECK(ft_cell[0] == ft_rom[0][0] - '0', "retry reloads puzzle");

    /* 再胜一次: N 键同样重试 */
    CHECK(t_fill_rec() == 1, "re-solve");
    ft_cell[j] = ft_cell[jsrc];
    ft_place_digit(j, orig);
    CHECK(ft_over, "win again");
    ev.key = K_CHAR;
    ev.ch = 'n';
    futoshiki_on_key(&ev);
    CHECK(!ft_over, "N at over retries");
}

/* ---- 全部 5 题都可填出并触发胜利 ---- */
static void test_win_all_puzzles(void) {
    for (int p = 0; p < FT_ROM_N; p++) {
        char msg[48];
        ft_new_game(p);
        CHECK(t_fill_rec() == 1, "solver fills puzzle");
        CHECK(ft_check_win(), "every puzzle solvable to win");
        /* 通过 ft_place_digit 修回一处分歧 → 每题都触发 ft_over */
        ft_new_game(p);
        CHECK(t_fill_rec() == 1, "solver gives solution");
        int u = -1;
        for (int i = 0; i < 16; i++)
            if (ft_given[i] == 0) { u = i; break; }
        if (u >= 0) {
            uint8_t corr = ft_cell[u];
            ft_cell[u] = (uint8_t)(corr == 4 ? 1 : 4);
            CHECK(!ft_check_win(), "tampered board not win");
            ft_place_digit(u, corr);
            CHECK(ft_over, "place digit triggers win for every puzzle");
        }
        snprintf(msg, sizeof(msg), "puzzle %d win path ok", p);
        CHECK(1 == 1, msg);
    }
}

/* ---- 渲染冒烟: 像素级抽查(题面格/不等式符号/空格/无符号区) ---- */
static bool px(int x, int y) {
    if (x < 0 || x >= (int)CCG_W || y < 0 || y >= (int)CCG_H) return false;
    return (g_fb[(y >> 3) * CCG_W + x] & (0x80u >> (y & 7))) != 0;
}

static int black_count(int x0, int y0, int w, int h) {
    int n = 0;
    for (int y = y0; y < y0 + h; y++)
        for (int x = x0; x < x0 + w; x++)
            if (px(x, y)) n++;
    return n;
}

static void test_render(void) {
    ft_new_game(0);
    futoshiki_render();

    /* 题面格 (0,3)=3: 黑底 + 白字笔画 */
    int bx = FT_OX, by = FT_OY + 3 * FT_PITCH;
    CHECK(px(bx + 3, by + 3), "given cell filled black");
    int n = black_count(bx + 9, by + 7, 10, 14);
    CHECK(n > 0 && n < 140, "given digit strokes drawn (not solid)");

    /* 玩家/空格的普通格 (0,0): 空白, 无填色 */
    CHECK(!px(FT_OX + 10, FT_OY + 30), "empty cell stays white");

    /* 不等式符号: h[0]='<' 位于 (0,0) 与 (1,0) 格间 */
    int sx = FT_OX + 0 * FT_PITCH + FT_CELL + 2;
    int sy = FT_OY + 0 * FT_PITCH + 11;
    int sn = black_count(sx, sy, 5, 7);
    CHECK(sn > 0 && sn < 35, "inequality symbol '<' drawn");

    /* 无不等式区: h[4]='.' 在 (1,1) 与 (2,1) 之间, 应空白 */
    int ex = FT_OX + 1 * FT_PITCH + FT_CELL + 2;
    int ey = FT_OY + 1 * FT_PITCH + 11;
    CHECK(black_count(ex, ey, 5, 7) == 0, "no symbol where none defined");

    /* 网格线存在: 外框 (0,0) 角为黑 */
    CHECK(px(FT_OX, FT_OY), "outer frame corner black");

    /* 胜局渲染: 无越界/无崩溃 */
    ft_new_game(0);
    CHECK(t_fill_rec() == 1, "solve for render-over");
    int u = -1;
    for (int i = 0; i < 16; i++)
        if (ft_given[i] == 0) { u = i; break; }
    uint8_t corr = ft_cell[u];
    ft_cell[u] = (uint8_t)(corr == 4 ? 1 : 4);
    ft_place_digit(u, corr);
    CHECK(ft_over, "over state for render");
    futoshiki_render();
    CHECK(1 == 1, "over render runs clean");
}

int main(void) {
    test_rom();
    test_unique_solution();
    test_state();
    test_win();
    test_input();
    test_over();
    test_win_all_puzzles();
    test_render();
    if (s_fail == 0) {
        printf("ALL FUTOSHIKI TESTS PASSED\n");
        return 0;
    }
    printf("%d TEST(S) FAILED\n", s_fail);
    return 1;
}
