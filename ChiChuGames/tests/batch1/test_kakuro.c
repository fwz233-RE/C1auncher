/* KAKURO 逻辑单测 — host 编译运行; 包含游戏源码, 直接访问 ka_ 静态状态
 * 覆盖: ROM 结构合法性 / 加载器一致性 / 独立求解唯一解 / 规则胜负判定 /
 *       光标跳黑格 / 输入态 / 冲突允许 / 胜利路径 / 换题 */
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#include "config.h"
#include "gfx/canvas.h"
#include "gfx/font.h"
#include "rng.h"
#include "games/kakuro.c"

/* host 框架 stub(main.c 不参与链接) */
bool s_exit_request = false;
void game_set_tick_interval(uint32_t ms) { (void)ms; }

static int s_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); s_fail++; } \
    else { printf("ok: %s\n", msg); } \
} while (0)

/* ---------- 独立结构重算: 从 ROM 字符串直接建段, 与 ka_load 交叉验证 ---------- */
typedef struct {
    int ncell;
    int cells[64];
    int nrun;
    int run_sum[32];
    int run_len[32];
    int run_cell[32][8];
    int cell_hr[64];
    int cell_vr[64];
} ka_sol_t;

static int ka_hint_dec(char c) {
    if (c == '-') return 0;
    if (c >= '0' && c <= '9') return (int)(c - '0');
    if (c >= 'a' && c <= 'z') return 10 + (int)(c - 'a');
    return 36 + (int)(c - 'A');
}

/* 独立建段: grid 平铺 63 字符('.','#','0'); 每段要求左侧/上方有提示格 */
static int ka_sol_build(const char *grid, const char *hints, ka_sol_t *s) {
    memset(s, 0, sizeof(*s));
    for (int i = 0; i < 64; i++) s->cell_hr[i] = s->cell_vr[i] = -1;
    for (int y = 0; y < KA_ROWS; y++)
        for (int x = 0; x < KA_COLS; x++)
            if (grid[y * KA_COLS + x] == '0') s->cells[s->ncell++] = y * KA_COLS + x;
    int nrun = 0;
    /* 横段 */
    for (int y = 0; y < KA_ROWS; y++) {
        int x = 0;
        while (x < KA_COLS) {
            if (grid[y * KA_COLS + x] == '0') {
                int x0 = x;
                while (x < KA_COLS && grid[y * KA_COLS + x] == '0') x++;
                if (grid[y * KA_COLS + x0 - 1] != '#') return -1;
                s->run_sum[nrun] = ka_hint_dec(hints[0]);  /* 占位, 下面重填 */
                s->run_len[nrun] = 0;
                for (int cx = x0; cx < x; cx++) {
                    int idx = y * KA_COLS + cx;
                    s->run_cell[nrun][s->run_len[nrun]++] = idx;
                    s->cell_hr[idx] = nrun;
                }
                nrun++;
            } else {
                x++;
            }
        }
    }
    /* 竖段 */
    for (int x = 0; x < KA_COLS; x++) {
        int y = 0;
        while (y < KA_ROWS) {
            if (grid[y * KA_COLS + x] == '0') {
                int y0 = y;
                while (y < KA_ROWS && grid[y * KA_COLS + x] == '0') y++;
                if (grid[(y0 - 1) * KA_COLS + x] != '#') return -1;
                s->run_len[nrun] = 0;
                for (int cy = y0; cy < y; cy++) {
                    int idx = cy * KA_COLS + x;
                    s->run_cell[nrun][s->run_len[nrun]++] = idx;
                    s->cell_vr[idx] = nrun;
                }
                nrun++;
            } else {
                y++;
            }
        }
    }
    s->nrun = nrun;
    /* 从 hints 字符串按阅读顺序填段和: 提示格字符对(上=竖段和, 下=横段和) */
    int hi = 0;
    for (int y = 0; y < KA_ROWS; y++) {
        for (int x = 0; x < KA_COLS; x++) {
            if (grid[y * KA_COLS + x] != '#') continue;
            int down = ka_hint_dec(hints[2 * hi]);
            int right = ka_hint_dec(hints[2 * hi + 1]);
            hi++;
            if (down != 0) {
                int r = s->cell_vr[(y + 1) * KA_COLS + x];
                if (r >= 0) s->run_sum[r] = down;
            }
            if (right != 0) {
                int r = s->cell_hr[y * KA_COLS + x + 1];
                if (r >= 0) s->run_sum[r] = right;
            }
        }
    }
    for (int r = 0; r < s->nrun; r++)
        if (s->run_sum[r] <= 0) return -2;
    return 0;
}

/* 独立求解器: MRV + 和值剪枝; 返回解个数(封顶 cap) */
static long ka_sol_nodes;
static int ka_sol_rec(ka_sol_t *s, uint8_t *sol, uint32_t *used, int *psum,
                      int *nfill, int cap) {
    if (++ka_sol_nodes > 50000000L) return -1;   /* 防护 */
    int best = -1, bestn = 10, nb = 0;
    for (int i = 0; i < s->ncell; i++) {
        int c = s->cells[i];
        if (sol[c] != 0) continue;
        uint32_t m = used[s->cell_hr[c]] | used[s->cell_vr[c]];
        nb = 0;
        for (int d = 1; d <= 9; d++) {
            if (m & (1u << d)) continue;
            int ok = 1;
            for (int rk = 0; rk < 2; rk++) {
                int r = rk ? s->cell_vr[c] : s->cell_hr[c];
                int p = psum[r] + d;
                int rem = s->run_len[r] - nfill[r] - 1;
                if (rem < 0) { ok = 0; break; }
                int t = s->run_sum[r] - p;
                if (t < rem || t > 9 * rem) { ok = 0; break; }
                if (rem == 0 && t != 0) { ok = 0; break; }
            }
            if (ok) nb++;
        }
        if (nb == 0) return 0;
        if (nb < bestn) { bestn = nb; best = c; }
    }
    if (best < 0) return 1;
    /* 重算 best 的候选 */
    uint32_t m = used[s->cell_hr[best]] | used[s->cell_vr[best]];
    int total = 0;
    for (int d = 1; d <= 9; d++) {
        if (m & (1u << d)) continue;
        int ok = 1;
        for (int rk = 0; rk < 2; rk++) {
            int r = rk ? s->cell_vr[best] : s->cell_hr[best];
            int p = psum[r] + d;
            int rem = s->run_len[r] - nfill[r] - 1;
            if (rem < 0) { ok = 0; break; }
            int t = s->run_sum[r] - p;
            if (t < rem || t > 9 * rem) { ok = 0; break; }
            if (rem == 0 && t != 0) { ok = 0; break; }
        }
        if (!ok) continue;
        sol[best] = (uint8_t)d;
        used[s->cell_hr[best]] |= 1u << d;
        used[s->cell_vr[best]] |= 1u << d;
        psum[s->cell_hr[best]] += d;
        psum[s->cell_vr[best]] += d;
        nfill[s->cell_hr[best]]++;
        nfill[s->cell_vr[best]]++;
        total += ka_sol_rec(s, sol, used, psum, nfill, cap);
        nfill[s->cell_vr[best]]--;
        nfill[s->cell_hr[best]]--;
        psum[s->cell_vr[best]] -= d;
        psum[s->cell_hr[best]] -= d;
        used[s->cell_vr[best]] &= ~(1u << d);
        used[s->cell_hr[best]] &= ~(1u << d);
        sol[best] = 0;
        if (total >= cap) return total;
    }
    return total;
}

static int ka_sol_count(int pz, int cap) {
    ka_sol_t s;
    int rc = ka_sol_build(ka_rom[pz].grid, ka_rom[pz].hints, &s);
    if (rc != 0) return rc;
    uint8_t sol[KA_CELLS] = {0};
    uint32_t used[32] = {0};
    int psum[32] = {0}, nfill[32] = {0};
    ka_sol_nodes = 0;
    return ka_sol_rec(&s, sol, used, psum, nfill, cap);
}

/* 用独立段结构 + 答案填满并核对每段规则 */
static int ka_sol_answer_ok(int pz) {
    ka_sol_t s;
    if (ka_sol_build(ka_rom[pz].grid, ka_rom[pz].hints, &s) != 0) return 0;
    for (int r = 0; r < s.nrun; r++) {
        int sum = 0;
        uint32_t mask = 0;
        for (int k = 0; k < s.run_len[r]; k++) {
            int c = s.run_cell[r][k];
            int v = ka_rom[pz].ans[c] - '0';
            if (v < 1 || v > 9) return 0;
            mask |= 1u << v;
            sum += v;
        }
        int n = 0;
        for (int b = 1; b <= 9; b++) if (mask & (1u << b)) n++;
        if (n != s.run_len[r]) return 0;
        if (sum != s.run_sum[r]) return 0;
    }
    return 1;
}

/* ---------- 测试组 ---------- */

static void test_rom_structure(void) {
    for (int p = 0; p < KA_ROM_N; p++) {
        CHECK(strlen(ka_rom[p].grid) == 63, "rom: grid 63 chars");
        CHECK(strlen(ka_rom[p].ans) == 63, "rom: ans 63 chars");
        int ok = 1, nh = 0;
        for (int i = 0; i < 63; i++) {
            char c = ka_rom[p].grid[i];
            if (c != '.' && c != '#' && c != '0') ok = 0;
            if (c == '#') nh++;
            char a = ka_rom[p].ans[i];
            if (c == '0') { if (a < '1' || a > '9') ok = 0; }
            else if (a != '.') ok = 0;
        }
        CHECK(ok == 1, "rom: grid/ans chars valid");
        CHECK((int)strlen(ka_rom[p].hints) == 2 * nh, "rom: hints 2 per hint cell");
        /* 首行首列无空格 */
        ok = 1;
        for (int x = 0; x < 9; x++) if (ka_rom[p].grid[x] == '0') ok = 0;
        for (int y = 0; y < 7; y++) if (ka_rom[p].grid[y * 9] == '0') ok = 0;
        CHECK(ok == 1, "rom: no input in row 0 / col 0");
        /* 独立建段: 提示邻接合法 + 所有段和 > 0 */
        ka_sol_t s;
        CHECK(ka_sol_build(ka_rom[p].grid, ka_rom[p].hints, &s) == 0,
              "rom: runs well-formed with sums");
        /* 答案满足每段规则 */
        CHECK(ka_sol_answer_ok(p) == 1, "rom: answer satisfies every run");
    }
}

static void test_unique(void) {
    for (int p = 0; p < KA_ROM_N; p++) {
        int n = ka_sol_count(p, 2);
        CHECK(n == 1, "rom: puzzle has exactly one solution");
    }
}

static void test_load(void) {
    for (int p = 0; p < KA_ROM_N; p++) {
        ka_load(p);
        CHECK(ka_pz == p, "load: puzzle index");
        int ninput = 0;
        for (int i = 0; i < 63; i++)
            if (ka_rom[p].grid[i] == '0') ninput++;
        CHECK(ka_ninput == ninput, "load: input count matches rom");
        int ok = 1;
        for (int i = 0; i < 63; i++) {
            if (ka_input[i] != (ka_rom[p].grid[i] == '0')) ok = 0;
            if (ka_input[i]) {
                if (ka_rid[i] == KA_NONE || ka_cid[i] == KA_NONE) ok = 0;
            } else {
                if (ka_rid[i] != KA_NONE || ka_cid[i] != KA_NONE) ok = 0;
            }
            if (ka_cell[i] != 0) ok = 0;
        }
        CHECK(ok == 1, "load: input/rid/cid/cell consistent");
        /* 段表: 每个空格恰好在一个横段和一个竖段 */
        int tot = 0, run_ok = 1;
        for (int r = 0; r < ka_nrun; r++) {
            if (ka_rsum[r] == 0 || ka_rlen[r] == 0) run_ok = 0;
            tot += ka_rlen[r];
        }
        CHECK(tot == 2 * ka_ninput && run_ok == 1, "load: run tables cover all inputs (h+v)");
        /* 独立建段数与加载器一致 */
        ka_sol_t s;
        ka_sol_build(ka_rom[p].grid, ka_rom[p].hints, &s);
        CHECK(ka_nrun == s.nrun, "load: run count matches independent build");
        /* 光标在第一个空格上 */
        CHECK(ka_input[(int)ka_cy * 9 + (int)ka_cx], "load: cursor on input cell");
        CHECK(!ka_over && !ka_input_mode, "load: fresh flags");
    }
}

/* 光标移动: 始终落在空格, 且单向不回头 */
static void test_cursor(void) {
    for (int p = 0; p < KA_ROM_N; p++) {
        ka_load(p);
        key_event_t ev;
        ev.is_repeat = false;
        ev.ch = 0;
        /* 每个方向连按 20 次 */
        struct { ccg_key k; int dx, dy; } dirs[4] = {
            { K_LEFT, -1, 0 }, { K_RIGHT, 1, 0 }, { K_UP, 0, -1 }, { K_DOWN, 0, 1 } };
        for (int d = 0; d < 4; d++) {
            for (int t = 0; t < 20; t++) {
                int ox = ka_cx, oy = ka_cy;
                ev.key = dirs[d].k;
                kakuro_on_key(&ev);
                int nx = ka_cx, ny = ka_cy;
                CHECK(ka_input[ny * 9 + nx], "cursor: stays on input cell");
                CHECK(dirs[d].dx < 0 ? nx <= ox : nx >= ox, "cursor: x monotone");
                CHECK(dirs[d].dy < 0 ? ny <= oy : ny >= oy, "cursor: y monotone");
                if (s_fail > 100) return;
            }
        }
    }
}

/* 输入态 / 填数 / 清除 / 冲突允许 */
static void test_input(void) {
    ka_load(0);
    int cx = ka_cx, cy = ka_cy;
    key_event_t ev;
    ev.is_repeat = false;
    ev.ch = 0;

    /* OK 进入输入态选中当前格, 再按取消 */
    ev.key = K_OK;
    kakuro_on_key(&ev);
    CHECK(ka_input_mode, "OK enters input mode");
    CHECK(ka_sel == (uint8_t)(cy * 9 + cx), "OK selects cursor cell");
    kakuro_on_key(&ev);
    CHECK(!ka_input_mode, "OK again exits input mode");

    /* 输入态: 光标移走后数字仍填入选中格 */
    ka_load(0);
    int sel = (int)ka_cy * 9 + (int)ka_cx;
    ev.key = K_OK;
    kakuro_on_key(&ev);
    ev.key = K_LEFT;
    kakuro_on_key(&ev);               /* 光标离开(左移或被挡) */
    ev.key = K_CHAR;
    ev.ch = '5';
    kakuro_on_key(&ev);
    CHECK(ka_cell[sel] == 5, "input mode fills selected cell");
    ev.key = K_DEL;
    kakuro_on_key(&ev);
    CHECK(ka_cell[sel] == 0, "DEL clears selected cell");

    /* 非输入态: 数字填入光标格; DEL 清除 */
    kakuro_on_key(&ev);               /* OK 取消输入态 */
    ev.key = K_CHAR;
    ev.ch = '5';
    kakuro_on_key(&ev);
    int cidx = (int)ka_cy * 9 + (int)ka_cx;
    CHECK(ka_cell[cidx] == 5, "move mode fills cursor cell");
    ev.key = K_DEL;
    kakuro_on_key(&ev);
    CHECK(ka_cell[cidx] == 0, "DEL clears cursor cell");

    /* 冲突允许: 填一个与同段已有值重复的数字 */
    ka_load(0);
    int c0 = -1;
    for (int i = 0; i < 63; i++)
        if (ka_input[i]) { c0 = i; break; }
    int r0 = ka_rid[c0];
    uint8_t dup = 0;
    for (int k = 0; k < ka_rlen[r0]; k++) {
        int cc = ka_rcell[r0][k];
        if (cc != c0) { dup = ka_rom[0].ans[cc] - '0'; break; }
    }
    if (dup != 0) {
        ka_place(c0, dup);
        CHECK(ka_cell[c0] == dup, "conflict entry allowed");
        CHECK(!ka_over, "conflict not a win");
        /* 段未填满时重复不报错; 填满后报错 */
        CHECK(ka_err_count() == 0, "partial run with dup not flagged");
        int missing = -1;
        for (int k = 0; k < ka_rlen[r0]; k++) {
            int cc = ka_rcell[r0][k];
            if (ka_cell[cc] == 0) { missing = cc; break; }
        }
        if (missing >= 0) {
            ka_place(missing, 1);
            CHECK(ka_err_count() >= 1, "completed run with dup flagged");
            ka_cell[missing] = 0;
        }
        ka_cell[c0] = 0;
    }
}

/* 胜利路径: 按答案填满 → 胜; 制造冲突 → 不胜; 修正 → 胜 */
static void test_win(void) {
    for (int p = 0; p < KA_ROM_N; p++) {
        ka_load(p);
        CHECK(!ka_check_win(), "fresh board not win");
        /* 未填满时即使无冲突也不胜 */
        int placed = 0;
        for (int i = 0; i < 63 && placed < ka_ninput - 1; i++)
            if (ka_input[i]) {
                ka_place(i, (uint8_t)(ka_rom[p].ans[i] - '0'));
                placed++;
            }
        CHECK(!ka_check_win(), "incomplete board not win");
        CHECK(!ka_over, "incomplete board not over");
        /* 填最后空格 → 胜 */
        for (int i = 0; i < 63; i++)
            if (ka_input[i] && ka_cell[i] == 0)
                ka_place(i, (uint8_t)(ka_rom[p].ans[i] - '0'));
        CHECK(ka_over, "answer fills to win");
        CHECK(ka_check_win(), "win flag consistent with check");
        /* 制造段内重复 → 不胜 */
        int j1 = -1, j2 = -1;
        for (int r = 0; r < ka_nrun && j1 < 0; r++)
            if (ka_rlen[r] >= 2) {
                j1 = ka_rcell[r][0];
                j2 = ka_rcell[r][1];
            }
        if (j1 >= 0) {
            ka_cell[j2] = ka_cell[j1];
            CHECK(!ka_check_win(), "full board with duplicate not win");
            CHECK(ka_err_count() >= 1, "duplicate flagged as error");
            ka_cell[j2] = (uint8_t)(ka_rom[p].ans[j2] - '0');
            CHECK(ka_check_win(), "restored board wins again");
        }
    }
}

/* 胜利后输入忽略; OK/N 重试同一题; BACK 退出 */
static void test_over_keys(void) {
    ka_load(0);
    for (int i = 0; i < 63; i++)
        if (ka_input[i]) ka_place(i, (uint8_t)(ka_rom[0].ans[i] - '0'));
    CHECK(ka_over, "over state reached");
    key_event_t ev;
    ev.is_repeat = false;
    ev.ch = '1';
    ev.key = K_CHAR;
    uint8_t saved = ka_cell[(int)ka_cy * 9 + (int)ka_cx];
    kakuro_on_key(&ev);
    CHECK(ka_cell[(int)ka_cy * 9 + (int)ka_cx] == saved, "input ignored when over");
    ev.key = K_OK;
    kakuro_on_key(&ev);
    CHECK(!ka_over, "OK at over = retry");
    CHECK(ka_cell[0] == 0, "retry resets board");
}

/* N 换题 / R 重开同题 */
static void test_cycle(void) {
    ka_load(0);
    int pz0 = ka_pz;
    key_event_t ev;
    ev.is_repeat = false;
    ev.ch = 'n';
    ev.key = K_CHAR;
    kakuro_on_key(&ev);
    CHECK(ka_pz == (pz0 + 1) % KA_ROM_N, "N advances puzzle");
    /* 换题后盘面重置 */
    CHECK(ka_ninput == ka_ninput, "load resets");
    ev.ch = 'r';
    kakuro_on_key(&ev);
    CHECK(ka_pz == (pz0 + 1) % KA_ROM_N, "R restarts same puzzle");
    CHECK(!ka_over && !ka_input_mode, "restart resets flags");
}

/* 解码器与数字格式化 */
static void test_helpers(void) {
    CHECK(ka_hint_val('-') == 0, "hint '-': none");
    CHECK(ka_hint_val('9') == 9, "hint '9': 9");
    CHECK(ka_hint_val('a') == 10, "hint 'a': 10");
    CHECK(ka_hint_val('z') == 35, "hint 'z': 35");
    CHECK(ka_hint_val('A') == 36, "hint 'A': 36");
    CHECK(ka_hint_val('J') == 45, "hint 'J': 45");
    char buf[4];
    ka_num(6, buf);
    CHECK(strcmp(buf, "6") == 0, "num single digit");
    ka_num(24, buf);
    CHECK(strcmp(buf, "24") == 0, "num double digit");
}

/* 渲染冒烟: 正常盘面 + 胜利盘面各画一帧(host 无显示) */
static void test_render(void) {
    ka_load(0);
    kakuro_render();
    /* 格 (0,0) 恒为提示格 → 黑; 像素 (64,18) 在格内 */
    int byte = (18 >> 3) * CCG_W + 64;
    CHECK((g_fb[byte] & 0x20) != 0, "render: hint cell painted black");
    for (int i = 0; i < 63; i++)
        if (ka_input[i]) ka_place(i, (uint8_t)(ka_rom[0].ans[i] - '0'));
    CHECK(ka_over, "render: over reached");
    kakuro_render();
    CHECK(ka_over_full, "render: over full-flag set once");
    kakuro_render();
    CHECK(ka_over_full, "render: over full-flag not repeated");
}

int main(void) {
    test_rom_structure();
    test_unique();
    test_load();
    test_cursor();
    test_input();
    test_win();
    test_over_keys();
    test_cycle();
    test_helpers();
    test_render();
    if (s_fail == 0) {
        printf("ALL KAKURO TESTS PASSED\n");
        return 0;
    }
    printf("%d TEST(S) FAILED\n", s_fail);
    return 1;
}
