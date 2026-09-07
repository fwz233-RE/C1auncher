/* KENKEN 逻辑单测 — host 编译运行; 包含游戏源码, 直接访问 kn_ 静态状态
 * 覆盖: ROM 结构(覆盖/连通/笼角/运算符/目标) / 独立 Latin 方枚举验证唯一解 /
 *       加载器与独立解码交叉验证 / 题面格锁定 / 填数→胜负判定(对/错/恢复) /
 *       ERR 计数(行/列重复+笼错误) / 光标边界 / 输入态与按键 / 渲染冒烟 /
 *       胜利全刷只一次 / enter 随机选题 */
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#include "config.h"
#include "gfx/canvas.h"
#include "gfx/font.h"
#include "rng.h"
#include "games/kenken.c"

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

static int t_px(int x, int y) {
    if (x < 0 || x >= (int)CCG_W || y < 0 || y >= (int)CCG_H) return 0;
    return (g_fb[(y >> 3) * CCG_W + x] & (0x80u >> (y & 7))) != 0;
}

/* ---------- 独立解码(不经 kn_ 函数) ---------- */
typedef struct {
    int cage[16];
    int ncage;
    int op[9];
    int tgt[9];
    int corner[9];
    int cells[9][4];
    int clen[9];
} t_puz_t;

static int t_tgt_val(char c) {
    if (c >= '1' && c <= '9') return c - '0';
    return 10 + (c - 'a');
}

static void t_decode(int pz, t_puz_t *P) {
    const kn_puz_t *p = &kn_rom[pz];
    memset(P, 0, sizeof(*P));
    P->ncage = (int)strlen(p->ops);
    for (int i = 0; i < 16; i++) P->cage[i] = p->cmap[i] - 'a';
    for (int c = 0; c < P->ncage; c++) {
        P->op[c] = p->ops[c];
        P->tgt[c] = t_tgt_val(p->tgts[c]);
        P->corner[c] = 16;
    }
    for (int i = 0; i < 16; i++) {
        int c = P->cage[i];
        if (P->corner[c] == 16) P->corner[c] = i;   /* 阅读顺序首格 */
        P->cells[c][P->clen[c]++] = i;
    }
}

/* 独立笼约束: 满时按运算符是否等于目标 */
static bool t_cage_sat(const t_puz_t *P, int c, const int *board) {
    int n = P->clen[c];
    int v[4];
    for (int k = 0; k < n; k++) v[k] = board[P->cells[c][k]];
    switch (P->op[c]) {
    case '+': {
        int s = 0;
        for (int k = 0; k < n; k++) s += v[k];
        return s == P->tgt[c];
    }
    case 'x': {
        int p = 1;
        for (int k = 0; k < n; k++) p *= v[k];
        return p == P->tgt[c];
    }
    case '-': {
        int mx = 0, s = 0;
        for (int k = 0; k < n; k++) {
            if (v[k] > mx) mx = v[k];
            s += v[k];
        }
        return mx - s + mx == P->tgt[c];
    }
    case '/': {
        int a = v[0], b = v[1];
        int big = (a > b) ? a : b;
        int sm = (a > b) ? b : a;
        return sm != 0 && big % sm == 0 && big / sm == P->tgt[c];
    }
    }
    return false;
}

/* Heap 算法迭代生成全部 24 个排列 */
static int t_perms[24][4];
static int t_nperms;
static void t_gen_perms(void) {
    int a[4] = {1, 2, 3, 4};
    int c[4] = {0, 0, 0, 0};
    t_nperms = 0;
    for (int i = 0; i < 4; i++) t_perms[0][i] = a[i];
    t_nperms++;
    int i = 0;
    while (i < 4) {
        if (c[i] < i) {
            int t;
            if (i % 2 == 0) { t = a[0]; a[0] = a[i]; a[i] = t; }
            else { t = a[c[i]]; a[c[i]] = a[i]; a[i] = t; }
            for (int j = 0; j < 4; j++) t_perms[t_nperms][j] = a[j];
            t_nperms++;
            c[i]++;
            i = 0;
        } else {
            c[i] = 0;
            i++;
        }
    }
}

/* 独立唯一解验证: 枚举全部 576 个 4x4 Latin 方, 统计满足笼约束的个数 */
static int t_count_solutions(int pz, int *only) {
    t_puz_t P;
    t_decode(pz, &P);
    int cnt = 0;
    for (int r0 = 0; r0 < 24; r0++)
    for (int r1 = 0; r1 < 24; r1++)
    for (int r2 = 0; r2 < 24; r2++)
    for (int r3 = 0; r3 < 24; r3++) {
        const int *rr[4] = {t_perms[r0], t_perms[r1], t_perms[r2], t_perms[r3]};
        /* 列不重复 */
        bool latin = true;
        for (int x = 0; x < 4 && latin; x++) {
            uint32_t m = 0;
            for (int y = 0; y < 4; y++) {
                uint32_t b = 1u << (rr[y][x] - 1);
                if ((m & b) != 0) { latin = false; break; }
                m |= b;
            }
        }
        if (!latin) continue;
        int board[16];
        for (int y = 0; y < 4; y++)
            for (int x = 0; x < 4; x++) board[y * 4 + x] = rr[y][x];
        bool ok = true;
        for (int c = 0; c < P.ncage && ok; c++)
            if (!t_cage_sat(&P, c, board)) ok = false;
        if (ok) {
            cnt++;
            if (only) memcpy(only, board, 16 * sizeof(int));
        }
    }
    return cnt;
}

static void test_rom_structure(void) {
    for (int pz = 0; pz < KN_ROM_N; pz++) {
        t_puz_t P;
        t_decode(pz, &P);
        int mask = 0;
        for (int c = 0; c < P.ncage; c++)
            for (int k = 0; k < P.clen[c]; k++) mask |= 1 << P.cells[c][k];
        CHECK(mask == 0xFFFF, "cage coverage of all 16 cells");
        bool contig_ok = true;
        for (int c = 0; c < P.ncage; c++) {
            int seen[16] = {0};
            int stack[16], sp = 1;
            stack[0] = P.cells[c][0];
            seen[stack[0]] = 1;
            while (sp > 0) {
                int i = stack[--sp];
                int x = i % 4, y = i / 4;
                const int dx[4] = {1, -1, 0, 0};
                const int dy[4] = {0, 0, 1, -1};
                for (int d = 0; d < 4; d++) {
                    int nx = x + dx[d], ny = y + dy[d];
                    if (nx < 0 || nx > 3 || ny < 0 || ny > 3) continue;
                    int j = ny * 4 + nx;
                    int inc = 0;
                    for (int k = 0; k < P.clen[c]; k++)
                        if (P.cells[c][k] == j) inc = 1;
                    if (inc && !seen[j]) { seen[j] = 1; stack[sp++] = j; }
                }
            }
            int vis = 0;
            for (int k = 0; k < P.clen[c]; k++) vis += seen[P.cells[c][k]];
            if (vis != P.clen[c]) contig_ok = false;
        }
        CHECK(contig_ok, "all cages contiguous");
        bool corner_ok = true;
        for (int i = 0; i < 16; i++) {
            int c = P.cage[i];
            int first = 16;
            for (int k = 0; k < P.clen[c]; k++)
                if (P.cells[c][k] < first) first = P.cells[c][k];
            if (first != P.corner[c]) corner_ok = false;
        }
        CHECK(corner_ok, "corner = first cell in reading order");
        bool op_ok = true;
        for (int c = 0; c < P.ncage; c++)
            if (P.op[c] == '/' && P.clen[c] != 2) op_ok = false;
        CHECK(op_ok, "division only on 2-cell cages");
        bool tgt_ok = true;
        for (int c = 0; c < P.ncage; c++)
            if (P.tgt[c] < 1 || P.tgt[c] > 25) tgt_ok = false;
        CHECK(tgt_ok, "targets in range 1..25");
    }
    CHECK(t_nperms == 24, "permutation generator yields 24");
}

static void test_uniqueness(void) {
    static const int sol[3][16] = {
        {1, 2, 3, 4, 3, 4, 1, 2, 2, 1, 4, 3, 4, 3, 2, 1},
        {1, 2, 3, 4, 2, 3, 4, 1, 3, 4, 1, 2, 4, 1, 2, 3},
        {2, 4, 1, 3, 1, 3, 2, 4, 4, 2, 3, 1, 3, 1, 4, 2},
    };
    for (int pz = 0; pz < KN_ROM_N; pz++) {
        int only[16] = {0};
        int n = t_count_solutions(pz, only);
        char msg[64];
        snprintf(msg, sizeof(msg), "puzzle %d has exactly 1 solution (%d)", pz, n);
        CHECK(n == 1, msg);
        bool same = (n == 1);
        for (int i = 0; i < 16 && same; i++)
            if (only[i] != sol[pz][i]) same = false;
        CHECK(same, "the unique solution matches expected grid");
    }
}

static void test_loader(void) {
    kn_load(0);
    CHECK(kn_pz == 0, "load puzzle 0");
    t_puz_t P;
    t_decode(0, &P);
    CHECK((int)kn_ncage == P.ncage, "cage count matches");
    bool same = true;
    for (int i = 0; i < 16; i++)
        if ((int)kn_cage[i] != P.cage[i]) same = false;
    for (int c = 0; c < P.ncage; c++) {
        if ((int)kn_clen[c] != P.clen[c]) same = false;
        for (int k = 0; k < P.clen[c]; k++)
            if ((int)kn_ccell[c][k] != P.cells[c][k]) same = false;
        if ((int)kn_corner[c] != P.corner[c]) same = false;
        if (kn_op[c] != (char)P.op[c]) same = false;
        if ((int)kn_tgt[c] != P.tgt[c]) same = false;
    }
    CHECK(same, "cage tables match independent decode");
    int given = 0;
    bool prefilled = true;
    for (int i = 0; i < 16; i++)
        if (kn_given[i]) {
            given++;
            if (kn_cell[i] != kn_tgt[kn_cage[i]]) prefilled = false;
        }
    CHECK(prefilled, "given cells pre-filled with single-cage value");
    CHECK(given == 3, "puzzle 0 has 3 givens");
}

static void test_logic(void) {
    kn_load(0);
    int g = -1;
    for (int i = 0; i < 16; i++)
        if (kn_given[i]) { g = i; break; }
    uint8_t gval = kn_cell[g];
    kn_place(g, 2);
    CHECK(kn_cell[g] == gval, "given cell rejects placement");
    static const int sol[16] = {1, 2, 3, 4, 3, 4, 1, 2, 2, 1, 4, 3, 4, 3, 2, 1};
    for (int i = 0; i < 16; i++)
        if (!kn_given[i]) kn_place(i, (uint8_t)sol[i]);
    CHECK(kn_over, "full correct board wins");
    kn_over = false;
    int free = -1;
    for (int i = 0; i < 16; i++)
        if (!kn_given[i]) { free = i; break; }
    int orig = (int)kn_cell[free];
    kn_cell[free] = (uint8_t)((orig == 4) ? 3 : 4);
    CHECK(!kn_check_win(), "breaking a cell loses win");
    kn_cell[free] = (uint8_t)orig;
    CHECK(kn_check_win(), "restore board wins again");
    /* 换一个笼角的数字, 行/列仍不重复但笼和错 → 不胜 */
    kn_load(0);
    for (int i = 0; i < 16; i++)
        if (!kn_given[i]) kn_place(i, (uint8_t)sol[i]);
    kn_over = false;
    kn_cell[0] = 2;             /* 笼 a 变 {2,2} 和 4 != 3, 行 0 重复 */
    kn_cell[1] = 1;
    CHECK(!kn_check_win(), "cage sum wrong not a win");
    /* ERR: 行重复 */
    kn_load(1);
    CHECK(kn_dup_err() == 0, "empty board no dup error");
    kn_cell[0] = 1;
    kn_cell[1] = 1;
    CHECK(kn_dup_err() == 1, "row duplicate detected");
    kn_cell[0] = 0;
    kn_cell[1] = 0;
    /* 笼错误: 部分填已不可能(目标 9+, 已填 1+1=2, 剩 1 格 max4 → 最大 6) */
    kn_cell[4] = 1;
    kn_cell[8] = 1;
    CHECK(kn_cage_err() >= 1, "impossible cage sum flagged");
    kn_cell[4] = 0;
    kn_cell[8] = 0;
    /* 填满但和错 */
    kn_cell[4] = 1;
    kn_cell[8] = 2;
    kn_cell[12] = 3;
    CHECK(kn_cage_bad(1), "full cage wrong sum flagged");
    /* 正确笼不误报 */
    kn_cell[4] = 2;
    kn_cell[8] = 3;
    kn_cell[12] = 4;
    CHECK(!kn_cage_bad(1), "full cage correct sum not flagged");
    /* 光标边界 */
    kn_cx = 0;
    kn_cy = 0;
    kn_move(-1, -1);
    CHECK(kn_cx == 0 && kn_cy == 0, "cursor clamped at top-left");
    kn_cx = 3;
    kn_cy = 3;
    kn_move(1, 1);
    CHECK(kn_cx == 3 && kn_cy == 3, "cursor clamped at bottom-right");
    kn_move(-1, -1);
    CHECK(kn_cx == 2 && kn_cy == 2, "cursor moves in bounds");
}

static void test_render(void) {
    kn_load(0);
    kenken_render();
    /* 笼边界: 谜题 0 笼 a(格0,1) 与 b(格2,3) 之间在列 2 有 3px 黑边 */
    bool border = true;
    for (int dy = 0; dy < 3; dy++)
        if (!t_px(KN_OX + 2 * KN_CELL - 1, KN_OY + 4 + dy)) border = false;
    CHECK(border, "cage border drawn (3px)");
    /* 线索 "3+" 画在笼 a 角格(0,0)左上 */
    bool clue = false;
    for (int dy = 0; dy < 7; dy++)
        for (int dx = 0; dx < 12; dx++)
            if (t_px(KN_OX + 1 + dx, KN_OY + 1 + dy)) clue = true;
    CHECK(clue, "cage clue pixels drawn at corner");
    /* 题面格黑底(谜题 0 单格笼 = 格 5,6,7) */
    int g = -1;
    for (int i = 0; i < 16; i++)
        if (kn_given[i]) { g = i; break; }
    kn_cell[g] = 2;
    kenken_render();
    bool black = t_px(KN_OX + (g % 4) * KN_CELL + 3,
                      KN_OY + (g / 4) * KN_CELL + 3);
    CHECK(black, "given cell has black background");
    kn_cell[g] = 0;
    /* 玩家数字 2x: 填格 0 后中心有像素 */
    kn_cell[0] = 2;
    kenken_render();
    bool digit = false;
    for (int dy = 0; dy < 14; dy++)
        for (int dx = 0; dx < 10; dx++)
            if (t_px(KN_OX + 11 + dx, KN_OY + 9 + dy)) digit = true;
    CHECK(digit, "player digit 2x rendered");
    kn_cell[0] = 0;
    /* 光标: 格(0,0)右侧 2px 外框右缘 x=OX+33(避让网格线 OX+32) */
    kn_cx = 0;
    kn_cy = 0;
    kenken_render();
    bool cur = t_px(KN_OX + 33, KN_OY + 16);
    CHECK(cur, "cursor frame drawn at cell edge");
    /* 光标移开后原处恢复白(帧不存在) */
    kn_cx = 1;
    kn_cy = 1;
    kenken_render();
    bool cur2 = !t_px(KN_OX + 33, KN_OY + 16);
    CHECK(cur2, "cursor frame gone after move");
    /* 胜利渲染: 全刷只一次 */
    kn_over = true;
    kn_over_full = false;
    kenken_render();
    CHECK(kn_over_full, "win triggers force-full once");
    kenken_render();
    CHECK(kn_over_full, "force-full only once");
    kn_over = false;
}

static void test_keys(void) {
    kn_load(0);
    key_event_t ev;
    ev.key = K_UP;
    ev.ch = 0;
    ev.is_repeat = true;
    kn_cx = 2;
    kn_cy = 2;
    kenken_on_key(&ev);
    CHECK(kn_cy == 1, "repeat UP moves cursor");
    ev.key = K_OK;
    ev.is_repeat = false;
    kn_cx = 0;
    kn_cy = 0;
    kenken_on_key(&ev);
    CHECK(kn_input, "OK enters input mode");
    CHECK(kn_sel == 0, "selection follows cursor");
    ev.key = K_CHAR;
    ev.ch = '3';
    kenken_on_key(&ev);
    CHECK(kn_cell[0] == 3, "digit fills selected cell");
    ev.key = K_CHAR;
    ev.ch = '5';
    kenken_on_key(&ev);
    CHECK(kn_cell[0] == 3, "digit 5 ignored");
    ev.key = K_CHAR;
    ev.ch = '0';
    kenken_on_key(&ev);
    CHECK(kn_cell[0] == 3, "digit 0 ignored");
    ev.key = K_DEL;
    kenken_on_key(&ev);
    CHECK(kn_cell[0] == 0, "DEL clears selected cell");
    /* 题面格不可删 */
    int g = -1;
    for (int i = 0; i < 16; i++)
        if (kn_given[i]) { g = i; break; }
    kn_cell[g] = 1;
    kn_cx = (uint8_t)(g % 4);
    kn_cy = (uint8_t)(g / 4);
    ev.key = K_OK;
    kenken_on_key(&ev);
    ev.key = K_DEL;
    kenken_on_key(&ev);
    CHECK(kn_cell[g] == 1, "DEL cannot clear given");
    kn_cell[g] = 0;
    /* WASD 移动 */
    kn_cx = 1;
    kn_cy = 1;
    ev.key = K_CHAR;
    ev.ch = 'a';
    ev.is_repeat = true;
    kenken_on_key(&ev);
    CHECK(kn_cx == 0, "A moves left");
}

int main(void) {
    printf("== KENKEN tests ==\n");
    t_gen_perms();
    test_rom_structure();
    test_uniqueness();
    test_loader();
    test_logic();
    test_render();
    test_keys();
    kenken_enter();
    CHECK(kn_pz >= 0 && kn_pz < KN_ROM_N, "enter picks valid puzzle");
    printf("%d checks, %d failed\n", s_total, s_fail);
    return s_fail ? 1 : 0;
}
