/* SKYSCRAPERS 逻辑单测 — host 编译运行; 包含游戏源码, 直接访问 sk_ 静态状态
 * 覆盖: ROM 结构合法性 / 独立穷举验证唯一解(全部 576 个 4 阶拉丁方) /
 *       加载器与线索解析 / 可见数规则 / 行列入违例判定 / 胜负判定 /
 *       光标移动与环绕 / 输入态循环填数 / 直接填数与清空 / 换题重开 /
 *       胜利态按键(N retry / BACK quit) */
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#include "config.h"
#include "gfx/canvas.h"
#include "gfx/font.h"
#include "rng.h"
#include "games/skyscrapers.c"

/* host 框架 stub(main.c 不参与链接) */
bool s_exit_request = false;
void game_set_tick_interval(uint32_t ms) { (void)ms; }

static int s_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); s_fail++; } \
    else { printf("ok: %s\n", msg); } \
} while (0)

/* ---------- 独立可见数(测试自身实现, 与 sk_visible 交叉验证) ---------- */
static int t_vis(const int *line) {
    int maxh = 0, n = 0;
    for (int i = 0; i < 4; i++) {
        if (line[i] > maxh) { maxh = line[i]; n++; }
    }
    return n;
}

/* ---------- 独立穷举求解器: 枚举 24^4 行排列, 拉丁约束 + 线索过滤 ---------- */
static int s_perm_count;
static int s_perm[24][4];

static void s_perm_init(void) {
    s_perm_count = 0;
    /* 手动枚举 24 个排列 */
    for (int a = 0; a < 4; a++) {
        for (int b = 0; b < 4; b++) if (b != a) {
            for (int c = 0; c < 4; c++) if (c != a && c != b) {
                for (int d = 0; d < 4; d++) if (d != a && d != b && d != c) {
                    s_perm[s_perm_count][0] = a + 1;
                    s_perm[s_perm_count][1] = b + 1;
                    s_perm[s_perm_count][2] = c + 1;
                    s_perm[s_perm_count][3] = d + 1;
                    s_perm_count++;
                }
            }
        }
    }
}

static bool t_latin_ok(int g[4][4]) {
    for (int r = 0; r < 4; r++)
        if (g[r][0] == g[r][1] || g[r][0] == g[r][2] || g[r][0] == g[r][3] ||
            g[r][1] == g[r][2] || g[r][1] == g[r][3] || g[r][2] == g[r][3])
            return false;
    for (int c = 0; c < 4; c++) {
        int m = 0;
        for (int r = 0; r < 4; r++) m |= 1 << g[r][c];
        if (m != 0x1E) return false;
    }
    return true;
}

/* 线索数组 cl[16] 0=无; 上[0-3] 下[4-7] 左[8-11] 右[12-15] */
static bool t_clue_ok(int g[4][4], const uint8_t cl[16]) {
    for (int c = 0; c < 4; c++) {
        int col[4];
        for (int r = 0; r < 4; r++) col[r] = g[r][c];
        if (cl[c] && t_vis(col) != cl[c]) return false;
        int rev[4];
        for (int r = 0; r < 4; r++) rev[r] = col[3 - r];
        if (cl[4 + c] && t_vis(rev) != cl[4 + c]) return false;
        if (cl[8 + c] && t_vis(g[c]) != cl[8 + c]) return false;
        int rrev[4];
        for (int i = 0; i < 4; i++) rrev[i] = g[c][3 - i];
        if (cl[12 + c] && t_vis(rrev) != cl[12 + c]) return false;
    }
    return true;
}

static int t_solve_count(const uint8_t cl[16], int cap) {
    int count = 0;
    for (int p0 = 0; p0 < 24 && count < cap; p0++) {
        for (int p1 = 0; p1 < 24 && count < cap; p1++) {
            for (int p2 = 0; p2 < 24 && count < cap; p2++) {
                for (int p3 = 0; p3 < 24 && count < cap; p3++) {
                    int g[4][4];
                    for (int i = 0; i < 4; i++) {
                        g[0][i] = s_perm[p0][i];
                        g[1][i] = s_perm[p1][i];
                        g[2][i] = s_perm[p2][i];
                        g[3][i] = s_perm[p3][i];
                    }
                    if (t_latin_ok(g) && t_clue_ok(g, cl)) count++;
                }
            }
        }
    }
    return count;
}

/* ---------- 测试组 ---------- */

static void test_rom_structure(void) {
    s_perm_init();
    CHECK(s_perm_count == 24, "perm: 24 row permutations generated");
    for (int p = 0; p < SK_ROM_N; p++) {
        CHECK(strlen(sk_rom[p].sol) == 16, "rom: sol 16 chars");
        CHECK(strlen(sk_rom[p].clu) == 16, "rom: clu 16 chars");
        int ok = 1;
        for (int i = 0; i < 16; i++) {
            char s = sk_rom[p].sol[i];
            char c = sk_rom[p].clu[i];
            if (s < '1' || s > '4') ok = 0;
            if (c < '0' || c > '4') ok = 0;
        }
        CHECK(ok == 1, "rom: sol '1'-'4', clu '0'-'4'");
        /* 答案是拉丁方 */
        int g[4][4];
        for (int r = 0; r < 4; r++)
            for (int c = 0; c < 4; c++)
                g[r][c] = sk_rom[p].sol[r * 4 + c] - '0';
        CHECK(t_latin_ok(g), "rom: solution is a Latin square");
        /* 答案满足自身全部线索 */
        uint8_t cl[16];
        for (int i = 0; i < 16; i++) cl[i] = (uint8_t)(sk_rom[p].clu[i] - '0');
        CHECK(t_clue_ok(g, cl), "rom: solution matches every clue");
    }
}

static void test_unique(void) {
    for (int p = 0; p < SK_ROM_N; p++) {
        uint8_t cl[16];
        for (int i = 0; i < 16; i++) cl[i] = (uint8_t)(sk_rom[p].clu[i] - '0');
        CHECK(t_solve_count(cl, 2) == 1, "rom: exactly one solution (exhaustive)");
    }
}

static void test_load(void) {
    for (int p = 0; p < SK_ROM_N; p++) {
        sk_load(p);
        CHECK(sk_pz == p, "load: puzzle index");
        int allzero = 1;
        for (int i = 0; i < 16; i++) {
            if (sk_cell[i] != 0) allzero = 0;
            CHECK(sk_clue[i] == (uint8_t)(sk_rom[p].clu[i] - '0'),
                  "load: clue parsed");
        }
        CHECK(allzero == 1, "load: board cleared");
        CHECK(!sk_over && !sk_sel, "load: fresh state");
        CHECK(sk_cx == 0 && sk_cy == 0, "load: cursor at 0,0");
    }
    sk_load(-1);  /* 负索引环绕 */
    CHECK(sk_pz == SK_ROM_N - 1, "load: negative wraps");
}

static void test_visible_rule(void) {
    uint8_t l1[4] = {4, 1, 2, 3};       /* 4 遮住其余 */
    CHECK(sk_visible(l1) == 1, "visible: 4123 -> 1");
    uint8_t l2[4] = {1, 2, 3, 4};       /* 递增全可见 */
    CHECK(sk_visible(l2) == 4, "visible: 1234 -> 4");
    uint8_t l3[4] = {3, 1, 4, 2};
    CHECK(sk_visible(l3) == 2, "visible: 3142 -> 2");
    uint8_t l4[4] = {2, 4, 1, 3};
    CHECK(sk_visible(l4) == 2, "visible: 2413 -> 2");
}

static void test_win_check(void) {
    sk_load(0);
    CHECK(sk_check_win() == false, "win: empty board no win");
    /* 答案填满 -> 胜 */
    for (int i = 0; i < 16; i++)
        sk_cell[i] = (uint8_t)(sk_rom[0].sol[i] - '0');
    CHECK(sk_check_win() == true, "win: solution wins");
    CHECK(sk_err_count() == 0, "win: zero violations");
    /* 换一个合法拉丁方(不同解) -> 有违例 */
    sk_load(0);
    sk_cell[0] = 1; sk_cell[1] = 2; sk_cell[2] = 3; sk_cell[3] = 4;
    sk_cell[4] = 2; sk_cell[5] = 1; sk_cell[6] = 4; sk_cell[7] = 3;
    sk_cell[8] = 3; sk_cell[9] = 4; sk_cell[10] = 1; sk_cell[11] = 2;
    sk_cell[12] = 4; sk_cell[13] = 3; sk_cell[14] = 2; sk_cell[15] = 1;
    CHECK(sk_check_win() == false, "win: wrong latin square loses");
    CHECK(sk_err_count() > 0, "win: violations counted");
    /* 行内重复 */
    sk_load(0);
    sk_cell[0] = 1; sk_cell[1] = 1; sk_cell[2] = 3; sk_cell[3] = 4;
    CHECK(sk_row_bad(0) == true, "bad: duplicate in row");
    CHECK(sk_row_bad(1) == false, "bad: empty row not bad");
    /* 未填满不判 */
    sk_load(0);
    sk_cell[0] = 4; sk_cell[1] = 1; sk_cell[2] = 2;
    CHECK(sk_row_bad(0) == false, "bad: incomplete row not judged");
}

static void test_place_and_cycle(void) {
    sk_load(0);
    sk_place(0, 3);
    CHECK(sk_cell[0] == 3, "place: fill digit");
    sk_place(0, 3);                      /* 相同值 no-op */
    CHECK(sk_cell[0] == 3, "place: same value no-op");
    sk_place(0, 0);                      /* 清空 */
    CHECK(sk_cell[0] == 0, "place: clear");
    /* 循环 1->2->3->4->1 */
    sk_place(0, 1);
    sk_cycle(1);  CHECK(sk_cell[0] == 2, "cycle: up 1->2");
    sk_cycle(1);  CHECK(sk_cell[0] == 3, "cycle: up 2->3");
    sk_cycle(1);  CHECK(sk_cell[0] == 4, "cycle: up 3->4");
    sk_cycle(1);  CHECK(sk_cell[0] == 1, "cycle: up 4->1 wrap");
    sk_cycle(-1); CHECK(sk_cell[0] == 4, "cycle: down 1->4");
    sk_place(0, 0);
    sk_cycle(1);  CHECK(sk_cell[0] == 1, "cycle: empty starts at 1");
}

static void test_cursor(void) {
    sk_load(0);
    sk_move(1, 0);   CHECK(sk_cx == 1 && sk_cy == 0, "move: right");
    sk_move(0, 1);   CHECK(sk_cx == 1 && sk_cy == 1, "move: down");
    sk_move(-1, 0);  CHECK(sk_cx == 0 && sk_cy == 1, "move: left");
    sk_move(-1, 0);  CHECK(sk_cx == 3 && sk_cy == 1, "move: wraps left");
    sk_move(0, 1);   CHECK(sk_cx == 3 && sk_cy == 2, "move: down 2");
    sk_move(0, 1);   CHECK(sk_cx == 3 && sk_cy == 3, "move: down 3");
    sk_move(0, 1);   CHECK(sk_cx == 3 && sk_cy == 0, "move: wraps down");
    sk_move(1, 0);   CHECK(sk_cx == 0 && sk_cy == 0, "move: wraps right");
}

static void test_keys(void) {
    key_event_t ev;
    memset(&ev, 0, sizeof(ev));

    sk_load(0);
    /* 方向键移光标 */
    ev.key = K_RIGHT; ev.is_repeat = true;
    skyscrapers_on_key(&ev);
    CHECK(sk_cx == 1, "key: repeat right moves");
    /* OK 选中 */
    ev.key = K_OK; ev.is_repeat = false;
    skyscrapers_on_key(&ev);
    CHECK(sk_sel == true, "key: OK selects");
    /* 选中态 UP 循环 */
    ev.key = K_UP; ev.is_repeat = false;
    skyscrapers_on_key(&ev);
    CHECK(sk_cell[1] == 1, "key: selected up cycles to 1");
    /* 选中态 LEFT/RIGHT 移动选中格 */
    ev.key = K_RIGHT; ev.is_repeat = true;
    skyscrapers_on_key(&ev);
    CHECK(sk_cx == 2, "key: selected right moves sel");
    ev.key = K_UP; ev.is_repeat = false;
    skyscrapers_on_key(&ev);
    CHECK(sk_cell[2] == 1, "key: cycle applies to new sel");
    /* OK 取消选中 */
    ev.key = K_OK; ev.is_repeat = false;
    skyscrapers_on_key(&ev);
    CHECK(sk_sel == false, "key: OK deselects");
    /* 未选中态 UP 移光标 */
    ev.key = K_UP; ev.is_repeat = true;
    skyscrapers_on_key(&ev);
    CHECK(sk_cy == 3, "key: unselected up moves cursor");
    /* 数字直接填 */
    ev.key = K_CHAR; ev.ch = '4'; ev.is_repeat = true;
    skyscrapers_on_key(&ev);
    CHECK(sk_cell[3 * 4 + 2] == 0, "key: repeat digit ignored");
    ev.is_repeat = false;
    skyscrapers_on_key(&ev);
    CHECK(sk_cell[3 * 4 + 2] == 4, "key: digit fills cursor cell");
    /* DEL 清空 */
    ev.key = K_DEL; ev.is_repeat = false;
    skyscrapers_on_key(&ev);
    CHECK(sk_cell[3 * 4 + 2] == 0, "key: DEL clears");
    /* 字母确认键忽略 repeat */
    ev.key = K_CHAR; ev.ch = 'n'; ev.is_repeat = true;
    int pz_before = sk_pz;
    skyscrapers_on_key(&ev);
    CHECK(sk_pz == pz_before, "key: repeat N ignored");
    ev.is_repeat = false;
    skyscrapers_on_key(&ev);
    CHECK(sk_pz == (pz_before + 1) % SK_ROM_N, "key: N next puzzle");
    /* R 重开当前题 */
    ev.key = K_CHAR; ev.ch = 'r'; ev.is_repeat = false;
    skyscrapers_on_key(&ev);
    CHECK(sk_pz == (pz_before + 1) % SK_ROM_N, "key: R keeps puzzle");
    CHECK(sk_filled() == false, "key: R clears board");
    /* WASD 等效 */
    ev.key = K_CHAR; ev.ch = 'd'; ev.is_repeat = true;
    skyscrapers_on_key(&ev);
    CHECK(sk_cx == 1, "key: d moves right");
    /* Q 退出 */
    s_exit_request = false;
    ev.key = K_QUIT; ev.is_repeat = false;
    skyscrapers_on_key(&ev);
    CHECK(s_exit_request == true, "key: Q quits");
}

static void test_win_flow(void) {
    key_event_t ev;
    memset(&ev, 0, sizeof(ev));

    sk_load(0);
    /* 用键逐步填满答案 */
    for (int i = 0; i < 16; i++) {
        sk_cx = i % 4;
        sk_cy = i / 4;
        ev.key = K_CHAR;
        ev.ch = sk_rom[0].sol[i];
        ev.is_repeat = false;
        skyscrapers_on_key(&ev);
    }
    CHECK(sk_over == true, "win: filling solution wins");
    CHECK(sk_check_win() == true, "win: state consistent");

    /* 胜利态: OK -> retry 同题并清盘 */
    ev.key = K_OK; ev.is_repeat = false;
    skyscrapers_on_key(&ev);
    CHECK(sk_over == false, "win: OK retries");
    CHECK(sk_filled() == false, "win: retry clears board");
    CHECK(sk_pz == 0, "win: retry same puzzle");

    /* 再胜, BACK -> 退出 */
    for (int i = 0; i < 16; i++) {
        sk_cx = i % 4;
        sk_cy = i / 4;
        ev.key = K_CHAR;
        ev.ch = sk_rom[0].sol[i];
        ev.is_repeat = false;
        skyscrapers_on_key(&ev);
    }
    CHECK(sk_over == true, "win: solution wins again");
    s_exit_request = false;
    ev.key = K_BACK; ev.is_repeat = false;
    skyscrapers_on_key(&ev);
    CHECK(s_exit_request == true, "win: BACK quits");

    /* 胜利态 Q 也退出 */
    sk_load(0);
    for (int i = 0; i < 16; i++) {
        sk_cx = i % 4;
        sk_cy = i / 4;
        ev.key = K_CHAR;
        ev.ch = sk_rom[0].sol[i];
        ev.is_repeat = false;
        skyscrapers_on_key(&ev);
    }
    s_exit_request = false;
    ev.key = K_QUIT; ev.is_repeat = false;
    skyscrapers_on_key(&ev);
    CHECK(s_exit_request == true, "win: Q quits");
}

static void test_err_display_count(void) {
    sk_load(1);
    /* 全部填 1: 行重复, 列重复, 可见数错误 */
    for (int i = 0; i < 16; i++) sk_cell[i] = 1;
    CHECK(sk_err_count() >= 8, "err: all-ones counts all 8 lines");
    /* 部分填: 不判 */
    sk_load(2);
    sk_cell[0] = 2; sk_cell[1] = 1; sk_cell[2] = 4; sk_cell[3] = 3;
    CHECK(sk_err_count() == 0, "err: complete row only");
}

int main(void) {
    test_rom_structure();
    test_unique();
    test_load();
    test_visible_rule();
    test_win_check();
    test_place_and_cycle();
    test_cursor();
    test_keys();
    test_win_flow();
    test_err_display_count();
    if (s_fail == 0) printf("ALL TESTS PASSED\n");
    else printf("%d TEST(S) FAILED\n", s_fail);
    return s_fail ? 1 : 0;
}
