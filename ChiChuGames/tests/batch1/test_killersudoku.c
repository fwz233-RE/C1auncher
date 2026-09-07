/* KILLER SUDOKU 逻辑单测 — host 编译运行; 包含游戏源码, 直接访问 ks_ 静态状态
 * 覆盖: ROM 结构合法性 / 加载器与独立解码交叉验证 / 独立求解器唯一解 /
 *       题面格锁定 / 笼冲突(超和/重复) / 候选掩码(笼和+行冲突) /
 *       胜负判定(正确/破坏/恢复) / 光标与输入态 / 换题重开 / 渲染冒烟 */
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#include "config.h"
#include "gfx/canvas.h"
#include "gfx/font.h"
#include "rng.h"
#include "games/killersudoku.c"

/* host 框架 stub(main.c 不参与链接) */
bool s_exit_request = false;
void game_set_tick_interval(uint32_t ms) { (void)ms; }

static int s_fail = 0;
static int s_total = 0;
#define CHECK(cond, msg) do { \
    s_total++; \
    if (!(cond)) { printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); s_fail++; } \
    else { printf("ok: %s\n", msg); } \
} while (0)

/* g_fb 像素读取(帧公式: 黑=1) */
static int ks_pixel_probe(int x, int y) {
    if (x < 0 || x >= (int)CCG_W || y < 0 || y >= (int)CCG_H) return 0;
    return (g_fb[(y >> 3) * CCG_W + x] & (0x80u >> (y & 7))) != 0;
}

/* ---------- 独立解码(不经 ks_ 函数) ---------- */
static int t_cage_id(char c) {
    if (c >= 'a' && c <= 'z') return c - 'a';
    return 26 + (c - 'A');
}
static int t_sum_val(char c) {
    if (c >= '1' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'z') return 10 + (c - 'a');
    return 36 + (c - 'A');
}

typedef struct {
    int sol[81];
    int giv[81];
    int cage[81];
    int ncage;
    int sum[32];
    int corner[32];
    int cells[32][9];
    int clen[32];
} t_puz_t;

static void t_decode(int pz, t_puz_t *P) {
    const ks_puz_t *p = &ks_rom[pz];
    int nc = 0;
    memset(P, 0, sizeof(*P));
    for (int i = 0; i < 81; i++) {
        P->sol[i] = p->sol[i] - '0';
        P->giv[i] = (p->giv[i] != '.') ? (p->giv[i] - '0') : 0;
        P->cage[i] = t_cage_id(p->cmap[i]);
        if (P->cage[i] + 1 > nc) nc = P->cage[i] + 1;
    }
    P->ncage = nc;
    for (int c = 0; c < nc; c++) {
        P->sum[c] = t_sum_val(p->sums[c]);
        P->corner[c] = -1;
    }
    for (int i = 0; i < 81; i++) {
        int c = P->cage[i];
        if (P->corner[c] < 0) P->corner[c] = i;
        P->cells[c][P->clen[c]++] = i;
    }
}

/* ---------- 独立求解器(与离线 ks_solve.c 独立实现): 最受限格回溯,
 * 行/列/宫/笼掩码增量维护; -1 = 超过节点预算 ---------- */
#define T_BUDGET 60000000
static long long t_nodes = 0;

static int w_grid[81];
static uint32_t w_rowm[9], w_colm[9], w_boxm[9];
static uint32_t w_cdig[32];
static int w_cnow[32];
static int w_cleft[32];
static int w_count;
static int w_exhausted;
static t_puz_t w_P;
static int w_limit;

/* 单元级掩码(行/列/宫/笼): 回溯撤消安全(不变量保证无交叉源),
 * 行/列/宫/笼掩码增量维护; 候选 = 剩余可行位 & 笼和预算 */
static void t_solve_rec(void) {
    if (w_exhausted) return;
    if (w_count >= w_limit) return;
    t_nodes++;
    if (t_nodes > T_BUDGET) {
        w_exhausted = 1;
        return;
    }
    int best = -1, bestn = 10;
    uint32_t bestm = 0;
    for (int i = 0; i < 81; i++) {
        if (w_grid[i]) continue;
        int c = w_P.cage[i];
        int s = w_cnow[c];
        int left = w_cleft[c];
        int lo = w_P.sum[c] - s - 9 * (left - 1);
        if (lo < 1) lo = 1;
        int hi = w_P.sum[c] - s - (left - 1);
        if (hi > 9) hi = 9;
        if (lo > hi) return;
        uint32_t rngm = ((1u << hi) - 1u) ^ ((1u << (lo - 1)) - 1u);
        uint32_t m = ~(w_rowm[i / 9] | w_colm[i % 9] |
                       w_boxm[(i / 9 / 3) * 3 + (i % 9) / 3] | w_cdig[c]) &
                      rngm & 0x1FFu;
        if (m == 0) return;
        int n = 0;
        for (uint32_t t = m; t; t &= t - 1) n++;
        if (n < bestn) {
            bestn = n;
            best = i;
            bestm = m;
            if (n == 1) break;
        }
    }
    if (best < 0) {
        w_count++;
        return;
    }
    for (uint32_t t = bestm; t; t &= t - 1) {
        int bit = __builtin_ctz(t);
        uint32_t m = 1u << bit;
        int r = best / 9, cc = best % 9;
        int c = w_P.cage[best];
        w_grid[best] = bit + 1;
        w_rowm[r] |= m;
        w_colm[cc] |= m;
        w_boxm[(r / 3) * 3 + cc / 3] |= m;
        w_cdig[c] |= m;
        w_cnow[c] += bit + 1;
        w_cleft[c]--;
        t_solve_rec();
        w_cleft[c]++;
        w_cnow[c] -= bit + 1;
        w_cdig[c] &= ~m;
        w_boxm[(r / 3) * 3 + cc / 3] &= ~m;
        w_colm[cc] &= ~m;
        w_rowm[r] &= ~m;
        w_grid[best] = 0;
        if (w_count >= w_limit) return;
    }
}

/* 解数统计: giv_fixed=1 时使用 ROM 题面格; limit=1/2/... */
static int t_count_solutions(int pz, int giv_fixed, int limit) {
    t_decode(pz, &w_P);
    memset(w_grid, 0, sizeof(w_grid));
    memset(w_rowm, 0, sizeof(w_rowm));
    memset(w_colm, 0, sizeof(w_colm));
    memset(w_boxm, 0, sizeof(w_boxm));
    memset(w_cdig, 0, sizeof(w_cdig));
    for (int c = 0; c < w_P.ncage; c++) {
        w_cnow[c] = 0;
        w_cleft[c] = w_P.clen[c];
    }
    for (int i = 0; i < 81; i++) {
        int d = (giv_fixed && w_P.giv[i]) ? w_P.giv[i] : 0;
        if (d) {
            uint32_t m = 1u << (d - 1);
            w_rowm[i / 9] |= m;
            w_colm[i % 9] |= m;
            w_boxm[(i / 9 / 3) * 3 + (i % 9) / 3] |= m;
            w_cdig[w_P.cage[i]] |= m;
            w_grid[i] = d;
            w_cnow[w_P.cage[i]] += d;
            w_cleft[w_P.cage[i]]--;
        }
    }
    w_count = 0;
    w_exhausted = 0;
    w_limit = limit;
    t_solve_rec();
    if (w_exhausted) return -1;
    return w_count;
}

/* ---------- ROM 结构校验(独立实现) ---------- */
static void test_rom_structure(void) {
    for (int pz = 0; pz < KS_ROM_N; pz++) {
        char msg[64];
        t_puz_t P;
        t_decode(pz, &P);
        snprintf(msg, sizeof(msg), "P%d sums total 405", pz + 1);
        int tot = 0;
        for (int c = 0; c < P.ncage; c++) tot += P.sum[c];
        CHECK(tot == 405, msg);
        snprintf(msg, sizeof(msg), "P%d solution rows/cols/boxes valid", pz + 1);
        bool ok = true;
        for (int r = 0; r < 9; r++) {
            uint32_t m = 0;
            for (int c = 0; c < 9; c++) m |= 1u << (P.sol[r * 9 + c] - 1);
            if (m != 0x1FF) ok = false;
        }
        for (int c = 0; c < 9; c++) {
            uint32_t m = 0;
            for (int r = 0; r < 9; r++) m |= 1u << (P.sol[r * 9 + c] - 1);
            if (m != 0x1FF) ok = false;
        }
        for (int by = 0; by < 9; by += 3)
            for (int bx = 0; bx < 9; bx += 3) {
                uint32_t m = 0;
                for (int a = 0; a < 3; a++)
                    for (int b = 0; b < 3; b++)
                        m |= 1u << (P.sol[(by + a) * 9 + bx + b] - 1);
                if (m != 0x1FF) ok = false;
            }
        CHECK(ok, msg);
        snprintf(msg, sizeof(msg), "P%d givens match solution", pz + 1);
        ok = true;
        for (int i = 0; i < 81; i++)
            if (P.giv[i] && P.giv[i] != P.sol[i]) ok = false;
        CHECK(ok, msg);
        snprintf(msg, sizeof(msg), "P%d cages cover all 81 cells", pz + 1);
        int tcells = 0;
        for (int c = 0; c < P.ncage; c++) tcells += P.clen[c];
        CHECK(tcells == 81, msg);
        snprintf(msg, sizeof(msg), "P%d cages connected sizes 2-9", pz + 1);
        ok = true;
        for (int c = 0; c < P.ncage; c++) {
            if (P.clen[c] < 2 || P.clen[c] > 9) ok = false;
            int seen2[81] = {0};
            int stack[9], sp = 0;
            stack[sp++] = P.cells[c][0];
            seen2[P.cells[c][0]] = 1;
            while (sp > 0) {
                int cur = stack[--sp];
                int r = cur / 9, cc = cur % 9;
                for (int d = 0; d < 4; d++) {
                    int nr = r + (d == 0) - (d == 1);
                    int nc = cc + (d == 2) - (d == 3);
                    if (nr < 0 || nr > 8 || nc < 0 || nc > 8) continue;
                    int nb = nr * 9 + nc;
                    if (P.cage[nb] == c && !seen2[nb]) {
                        seen2[nb] = 1;
                        stack[sp++] = nb;
                    }
                }
            }
            int vis = 0;
            for (int i = 0; i < 81; i++)
                if (P.cage[i] == c && seen2[i]) vis++;
            if (vis != P.clen[c]) ok = false;
        }
        CHECK(ok, msg);
        snprintf(msg, sizeof(msg), "P%d cage sums match solution, no cage dup",
                 pz + 1);
        ok = true;
        for (int c = 0; c < P.ncage; c++) {
            int s = 0;
            uint32_t m = 0;
            for (int k = 0; k < P.clen[c]; k++) {
                int d = P.sol[P.cells[c][k]];
                s += d;
                if (m & (1u << (d - 1))) ok = false;
                m |= 1u << (d - 1);
            }
            if (s != P.sum[c]) ok = false;
        }
        CHECK(ok, msg);
        snprintf(msg, sizeof(msg), "P%d no given on cage corner", pz + 1);
        ok = true;
        for (int c = 0; c < P.ncage; c++)
            if (P.giv[P.corner[c]]) ok = false;
        CHECK(ok, msg);
        snprintf(msg, sizeof(msg), "P%d sums string matches cage count", pz + 1);
        CHECK((int)strlen(ks_rom[pz].sums) == P.ncage, msg);
        snprintf(msg, sizeof(msg), "P%d givens do not pre-break a cage", pz + 1);
        ok = true;
        for (int i = 0; i < 81; i++) {
            if (!P.giv[i]) continue;
            int c = P.cage[i];
            int empty = P.clen[c] - 1;
            if (P.giv[i] + empty > P.sum[c]) ok = false;
            if (P.giv[i] + 9 * empty < P.sum[c]) ok = false;
        }
        CHECK(ok, msg);
    }
}

/* 加载器与独立解码交叉验证 */
static void test_loader(void) {
    for (int pz = 0; pz < KS_ROM_N; pz++) {
        char msg[64];
        ks_load(pz);
        t_puz_t P;
        t_decode(pz, &P);
        CHECK(ks_ncage == (uint8_t)P.ncage, "loader cage count");
        bool ok = true;
        for (int i = 0; i < 81; i++) {
            if (ks_cage[i] != (uint8_t)P.cage[i]) ok = false;
            if (ks_given[i] != (uint8_t)(P.giv[i] != 0)) ok = false;
            if (ks_cell[i] != (uint8_t)P.giv[i]) ok = false;
        }
        for (int c = 0; c < P.ncage; c++) {
            if (ks_csum[c] != (uint8_t)P.sum[c]) ok = false;
            if (ks_clen[c] != (uint8_t)P.clen[c]) ok = false;
            if (ks_corner[c] != (uint8_t)P.corner[c]) ok = false;
            for (int k = 0; k < P.clen[c]; k++)
                if (ks_ccell[c][k] != (uint8_t)P.cells[c][k]) ok = false;
        }
        snprintf(msg, sizeof(msg), "P%d loader matches independent decode",
                 pz + 1);
        CHECK(ok, msg);
    }
}

/* 独立求解器: 每题唯一解 */
static void test_uniqueness(void) {
    for (int pz = 0; pz < KS_ROM_N; pz++) {
        char msg[64];
        t_nodes = 0;
        int n = t_count_solutions(pz, 1, 2);
        snprintf(msg, sizeof(msg), "P%d unique (count=%d, nodes=%lld)",
                 pz + 1, n, t_nodes);
        CHECK(n == 1, msg);
    }
    /* 求解器健全性: 全题面(=完整解) 恰好 1 解 */
    for (int pz = 0; pz < KS_ROM_N; pz++) {
        char msg[64];
        t_puz_t P;
        t_decode(pz, &P);
        int full[81];
        for (int i = 0; i < 81; i++) full[i] = P.sol[i];
        /* 把解填入全局工作区后计数(不依赖 ROM giv) */
        memset(w_grid, 0, sizeof(w_grid));
        memset(w_rowm, 0, sizeof(w_rowm));
        memset(w_colm, 0, sizeof(w_colm));
        memset(w_boxm, 0, sizeof(w_boxm));
        memset(w_cdig, 0, sizeof(w_cdig));
        memcpy(&w_P, &P, sizeof(w_P));
        for (int c = 0; c < w_P.ncage; c++) {
            w_cnow[c] = 0;
            w_cleft[c] = w_P.clen[c];
        }
        for (int i = 0; i < 81; i++) {
            uint32_t m = 1u << (full[i] - 1);
            w_rowm[i / 9] |= m;
            w_colm[i % 9] |= m;
            w_boxm[(i / 9 / 3) * 3 + (i % 9) / 3] |= m;
            w_cdig[w_P.cage[i]] |= m;
            w_grid[i] = full[i];
            w_cnow[w_P.cage[i]] += full[i];
            w_cleft[w_P.cage[i]]--;
        }
        w_count = 0;
        w_exhausted = 0;
        w_limit = 1;
        t_solve_rec();
        snprintf(msg, sizeof(msg), "P%d full grid solves to 1", pz + 1);
        CHECK(w_count == 1 && !w_exhausted, msg);
    }
}

/* ---------- 游戏逻辑 ---------- */
static void test_logic(void) {
    ks_load(0);
    CHECK(ks_pz == 0, "load pz 0");
    CHECK(ks_ncage == 28, "P1 cage count 28");
    CHECK(ks_fill_count() == 3, "P1 starts with 3 givens");
    int fresh_err = ks_err_count();
    CHECK(fresh_err == 0, "fresh board has no bad cage");

    /* 题面格锁定 */
    int gi = -1;
    for (int i = 0; i < 81; i++)
        if (ks_given[i]) { gi = i; break; }
    CHECK(gi >= 0, "P1 has a given");
    {
        int before = ks_cell[gi];
        ks_place(gi, (uint8_t)(before == 9 ? 1 : before + 1));
        CHECK(ks_cell[gi] == before, "given cell locked");
    }

    /* 笼冲突: P1 笼 d(2 格, 和 4) */
    uint8_t cd = 255;
    for (uint8_t c = 0; c < ks_ncage; c++)
        if (ks_csum[c] == 4 && ks_clen[c] == 2) { cd = c; break; }
    CHECK(cd != 255, "found 2-cell sum-4 cage in P1");
    if (cd != 255) {
        int a = ks_ccell[cd][0];
        int b = ks_ccell[cd][1];
        ks_place(a, 4);
        CHECK(ks_cage_bad(cd), "over-target cage flagged bad");
        CHECK(ks_err_count() == fresh_err + 1, "ERR +1 for bad cage");
        ks_place(b, 1);                      /* 4+1=5, 满, 目标 4 */
        CHECK(ks_cage_bad(cd), "full cage sum mismatch flagged bad");
        ks_place(a, 3);                      /* 3+1=4 正确 */
        CHECK(!ks_cage_bad(cd), "correct cage not bad");
        ks_place(b, 3);                      /* 3+3=6 笼内重复 */
        CHECK(ks_cage_bad(cd), "cage duplicate flagged bad");
        ks_cell[a] = 0;
        ks_cell[b] = 0;
        CHECK(ks_err_count() == fresh_err, "ERR back to baseline");
    }

    /* 候选掩码: 空盘 {1,2,3}; 笼内已填 3 => {1}; 行冲突 3 => {1,2} */
    if (cd != 255) {
        int a = ks_ccell[cd][0];
        int b = ks_ccell[cd][1];
        ks_load(0);
        CHECK(ks_cand_mask(a) == 0x07u, "empty sum-4 cage candidates {1,2,3}");
        ks_place(b, 3);
        CHECK(ks_cand_mask(a) == 0x01u, "cage partner 3 forces {1}");
        ks_place(a, 1);
        ks_cell[a] = 0;
        ks_cell[b] = 0;
        int rr = a / 9;
        int other = -1;
        for (int x = 0; x < 9; x++) {
            int i = rr * 9 + x;
            if (i != a && i != b && !ks_given[i]) { other = i; break; }
        }
        if (other >= 0) {
            ks_place(other, 3);
            CHECK(ks_cand_mask(a) == 0x03u, "row conflict excludes 3");
            ks_cell[other] = 0;
        }
        /* 笼内重复排除 */
        ks_place(b, 3);
        ks_place(a, 3);
        CHECK(ks_cand_mask(b) == 0x00u, "dup + full cage no candidates");
        ks_cell[a] = 0;
        ks_cell[b] = 0;
    }

    /* 胜利路径: 按解填充 -> over */
    {
        t_puz_t P;
        t_decode(0, &P);
        ks_load(0);
        for (int i = 0; i < 81; i++)
            if (!ks_given[i]) ks_place(i, (uint8_t)P.sol[i]);
        CHECK(ks_over, "filling solution wins");
        CHECK(ks_fill_count() == 81, "board full after solution");
    }

    /* 胜利判定: 正确 -> true; 交换 2 格笼数字 -> false; 恢复 -> true */
    {
        t_puz_t P;
        t_decode(1, &P);
        ks_load(1);
        for (int i = 0; i < 81; i++)
            if (!ks_given[i]) ks_cell[i] = (uint8_t)P.sol[i];
        CHECK(ks_check_win(), "complete correct board wins");
        bool done = false;
        for (uint8_t c = 0; c < ks_ncage && !done; c++) {
            if (ks_clen[c] == 2) {
                int a = ks_ccell[c][0];
                int b = ks_ccell[c][1];
                uint8_t ta = ks_cell[a];
                uint8_t tb = ks_cell[b];
                ks_cell[a] = tb;
                ks_cell[b] = ta;
                CHECK(!ks_check_win(), "swapped cage digits not a win");
                ks_cell[a] = ta;
                ks_cell[b] = tb;
                done = true;
            }
        }
        CHECK(ks_check_win(), "restored board wins again");
    }

    /* 光标边界 + 输入态 + 键位 */
    {
        key_event_t ev;
        memset(&ev, 0, sizeof(ev));
        ks_load(0);
        for (int i = 0; i < 20; i++) {
            ev.key = K_LEFT;
            killersudoku_on_key(&ev);
        }
        CHECK(ks_cx == 0, "cursor clamped left");
        for (int i = 0; i < 20; i++) {
            ev.key = K_RIGHT;
            killersudoku_on_key(&ev);
        }
        CHECK(ks_cx == 8, "cursor clamped right");
        for (int i = 0; i < 20; i++) {
            ev.key = K_DOWN;
            killersudoku_on_key(&ev);
        }
        CHECK(ks_cy == 8, "cursor clamped down");
        ev.key = K_UP;
        killersudoku_on_key(&ev);
        ev.key = K_OK;
        killersudoku_on_key(&ev);
        CHECK(ks_input, "OK enters input mode");
        CHECK(ks_sel == (uint8_t)(ks_cy * 9 + ks_cx), "sel == cursor cell");
        int sel = ks_sel;
        ev.key = K_CHAR;
        ev.ch = '5';
        ev.is_repeat = true;
        killersudoku_on_key(&ev);
        CHECK(ks_cell[sel] == 0, "repeat digit ignored");
        ev.is_repeat = false;
        killersudoku_on_key(&ev);
        if (ks_given[sel] == 0)
            CHECK(ks_cell[sel] == 5, "digit placed on selected cell");
        else
            CHECK(ks_cell[sel] == 0, "digit not placed on given");
        ev.key = K_DEL;
        killersudoku_on_key(&ev);
        CHECK(ks_cell[sel] == 0, "DEL clears selected cell");
        ev.key = K_OK;
        killersudoku_on_key(&ev);
        CHECK(!ks_input, "OK again exits input mode");
        ev.key = K_CHAR;
        ev.ch = 'a';
        killersudoku_on_key(&ev);
        ev.ch = 'w';
        killersudoku_on_key(&ev);
        CHECK(ks_cx == 7 && ks_cy == 6, "WASD moves cursor");
        ev.ch = 'n';
        killersudoku_on_key(&ev);
        CHECK(ks_pz == 1, "N advances puzzle");
        ev.ch = 'r';
        killersudoku_on_key(&ev);
        CHECK(ks_pz == 1, "R restarts same puzzle");
        ev.key = K_PAUSE;
        killersudoku_on_key(&ev);
        CHECK(ks_cand, "P toggles candidates on");
        killersudoku_on_key(&ev);
        CHECK(!ks_cand, "P toggles candidates off");
        ev.key = K_QUIT;
        ev.is_repeat = true;
        killersudoku_on_key(&ev);
        ev.is_repeat = false;
        killersudoku_on_key(&ev);
        CHECK(s_exit_request, "Q sets exit request");
        s_exit_request = false;
    }
}

/* 渲染冒烟 + 像素探测 */
static void test_render(void) {
    ks_load(0);
    killersudoku_render();
    CHECK(ks_pixel_probe(KS_OX, KS_OY) == 1, "outer border top-left black");
    CHECK(ks_pixel_probe(100, CCG_HUD_H - 1) == 1, "HUD hline drawn");
    int gi = -1;
    for (int i = 0; i < 81; i++)
        if (ks_given[i]) { gi = i; break; }
    if (gi >= 0)
        CHECK(ks_pixel_probe(KS_OX + (gi % 9) * KS_CELL + 5,
                             KS_OY + (gi / 9) * KS_CELL + 5) == 1,
              "given cell black fill");
    {
        bool any = false;
        for (int yy = KS_OY + 1; yy < KS_OY + 9; yy++)
            for (int xx = KS_OX + 1; xx < KS_OX + 12; xx++)
                if (ks_pixel_probe(xx, yy)) { any = true; break; }
        CHECK(any, "cage sum text drawn at corner");
    }
    {
        bool white = true;
        for (int yy = KS_OY + 5; yy < KS_OY + 9; yy++)
            for (int xx = KS_OX + KS_CELL + 5; xx < KS_OX + KS_CELL + 9; xx++)
                if (ks_pixel_probe(xx, yy)) white = false;
        CHECK(white, "empty non-corner cell (1,0) center white");
    }
    /* 数字渲染: 填一格后 2x 数字像素 */
    {
        int idx = 9;
        ks_place(idx, 8);
        killersudoku_render();
        bool any = false;
        for (int yy = KS_OY + 2; yy < KS_OY + 13; yy++)
            for (int xx = KS_OX + 2; xx < KS_OX + 12; xx++)
                if (ks_pixel_probe(xx, yy)) { any = true; break; }
        CHECK(any, "2x digit rendered in cell");
        ks_cell[idx] = 0;
    }
    /* 候选模式 + 胜利渲染 */
    ks_cand = true;
    killersudoku_render();
    ks_cand = false;
    ks_over = true;
    ks_over_full = false;
    killersudoku_render();
    CHECK(ks_over_full, "win triggers force-full once");
    killersudoku_render();
    CHECK(ks_over_full, "force-full only once");
    ks_over = false;
}

int main(void) {
    printf("== KILLER SUDOKU tests ==\n");
    test_rom_structure();
    test_loader();
    test_uniqueness();
    test_logic();
    test_render();
    printf("%d checks, %d failed\n", s_total, s_fail);
    return s_fail ? 1 : 0;
}
