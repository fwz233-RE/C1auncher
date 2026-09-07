/* KLOTSKI host 逻辑测试 — include 游戏 .c 直接访问静态状态
 * 覆盖: 开局布局/边界/步数/光标回退/胜利判定/BFS 可解性+解法重放/
 * 随机压测不变式(死循环防护) */
#include "../src/config.h"
#include "../src/games/klotski.c"
#include <stdio.h>
#include <string.h>

/* main.c 提供, host 测试 stub */
bool s_exit_request = false;
void game_set_tick_interval(uint32_t ms) { (void)ms; }

static int s_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); s_fail++; } \
    else { printf("ok: %s\n", msg); } \
} while (0)

/* ---- 不变式: 18 子界内无重叠、2 空位、光标在空位 ---- */
static int kk_invariants_ok(void) {
    for (int p = 0; p < KK_PIECES; p++) {
        if (kk_px[p] < 0 || kk_py[p] < 0 ||
            kk_px[p] + kk_pw[p] > KK_COLS || kk_py[p] + kk_ph[p] > KK_ROWS)
            return 0;
    }
    int occ = 0, empty = 0;
    for (int i = 0; i < KK_COLS * KK_ROWS; i++) {
        if (kk_grid[i]) occ++; else empty++;
    }
    if (occ != 18 || empty != 2) return 0;
    if (kk_grid[kk_ay * KK_COLS + kk_ax] != 0) return 0;
    return 1;
}

/* ---- 开局布局 ---- */
static void test_init(void) {
    klotski_enter();
    CHECK(kk_grid[0 * KK_COLS + 1] == 1 && kk_grid[0 * KK_COLS + 2] == 1 &&
          kk_grid[1 * KK_COLS + 1] == 1 && kk_grid[1 * KK_COLS + 2] == 1,
          "cao 2x2 at (1,0)");
    CHECK(kk_px[KK_P_CAO] == 1 && kk_py[KK_P_CAO] == 0, "cao anchor (1,0)");
    CHECK(kk_grid[2 * KK_COLS + 1] == 2 && kk_grid[2 * KK_COLS + 2] == 2,
          "guan horizontal 2x1 row2");
    CHECK(kk_grid[0 * KK_COLS + 0] == 3 && kk_grid[1 * KK_COLS + 0] == 3,
          "zhangfei vertical col0");
    CHECK(kk_grid[0 * KK_COLS + 3] == 4 && kk_grid[1 * KK_COLS + 3] == 4,
          "zhaoyun vertical col3");
    CHECK(kk_grid[2 * KK_COLS + 4] == 6 && kk_grid[3 * KK_COLS + 4] == 6,
          "huangzhong vertical col4");
    CHECK(kk_grid[3 * KK_COLS + 1] == 9 && kk_grid[3 * KK_COLS + 2] == 10,
          "soldiers bottom-middle");
    CHECK(kk_grid[2 * KK_COLS + 3] == 0 && kk_grid[3 * KK_COLS + 3] == 0,
          "two blanks (3,2)(3,3)");
    CHECK(kk_invariants_ok(), "init invariants");
    CHECK(kk_ax == 3 && kk_ay == 2, "anchor on first blank (3,2)");
    CHECK(kk_moves == 0 && !kk_over, "moves 0, not over");
    CHECK(!kk_solved(), "init not solved");
}

/* ---- 方向语义(15 谜一致): 按 DOWN = 空位正上方的棋子下滑入空位;
 * 边界/被挡不可动不计步; 锚位失败回退另一空位 ---- */
static void test_blocked(void) {
    klotski_enter();
    /* 直接单测 kk_slide_at 边界 */
    CHECK(kk_slide_at(3, 2, 0) == 0, "slide UP: cell below blank is blank");
    CHECK(kk_slide_at(0, 0, 0) == 0, "slide UP at top edge: piece leaves board");
    CHECK(kk_slide_at(0, 0, 3) == 0, "slide RIGHT at left edge: no piece");
    CHECK(kk_slide_at(3, 2, 3) == 1, "slide RIGHT: guan into (3,2)");
    klotski_enter();                        /* 复位 */
    /* 开局按 UP: 空位上方无棋子(下方是空位/边界), 不动不计步 */
    CHECK(kk_press(0) == 0 && kk_moves == 0, "UP blocked, no move counted");
    /* 按 DOWN: 空位正上方的赵云下滑入 (3,2) */
    CHECK(kk_press(1) == 1 && kk_moves == 1, "DOWN: zhaoyun slides in");
    CHECK(kk_grid[1 * KK_COLS + 3] == 4 && kk_grid[2 * KK_COLS + 3] == 4 &&
          kk_grid[0 * KK_COLS + 3] == 0,
          "zhaoyun now (3,1)(3,2), (3,0) blank");
    CHECK(kk_ax == 3 && kk_ay == 0, "anchor follows to (3,0)");
    /* 按 RIGHT: 锚位左侧是曹操, 被赵云(3,1)挡住不可右滑 ->
     * 回退另一空位, 卒 (2,3) 右滑入 (3,3) */
    CHECK(kk_press(3) == 1 && kk_moves == 2, "RIGHT via other blank: soldier");
    CHECK(kk_grid[0 * KK_COLS + 1] == 1 && kk_grid[1 * KK_COLS + 1] == 1,
          "cao untouched (anchor slide was blocked)");
    CHECK(kk_grid[3 * KK_COLS + 3] == 10 && kk_grid[3 * KK_COLS + 2] == 0,
          "soldier slid into (3,3)");
    CHECK(kk_invariants_ok(), "invariants after opening");
}

/* ---- 回退优先级: 锚位可动时绝不动另一空位侧 ---- */
static void test_fallback_order(void) {
    /* 构造: 空位 (2,0)[锚] 和 (4,0); 按 RIGHT 时锚位卒和另一空位侧
     * 卒都可右滑, 只允许锚位卒动(另一空位侧卒原地不动) */
    kk_px[0] = 1; kk_py[0] = 1;             /* cao (1,1) */
    kk_px[1] = 1; kk_py[1] = 3;             /* guan (1,3) */
    kk_px[2] = 0; kk_py[2] = 0;             /* zf (0,0) */
    kk_px[3] = 3; kk_py[3] = 1;             /* zy (3,1) */
    kk_px[4] = 4; kk_py[4] = 1;             /* mc (4,1) */
    kk_px[5] = 0; kk_py[5] = 2;             /* hz (0,2) */
    kk_px[6] = 1; kk_py[6] = 0;             /* s1 (1,0) 锚位左侧 */
    kk_px[7] = 3; kk_py[7] = 0;             /* s2 (3,0) 另一空位左侧 */
    kk_px[8] = 3; kk_py[8] = 3;             /* s3 (3,3) */
    kk_px[9] = 4; kk_py[9] = 3;             /* s4 (4,3) */
    kk_rebuild();
    kk_ax = 2; kk_ay = 0;
    kk_moves = 0; kk_over = false;
    CHECK(kk_invariants_ok(), "manual state valid");
    CHECK(kk_grid[0 * KK_COLS + 2] == 0 && kk_grid[0 * KK_COLS + 4] == 0,
          "blanks (2,0)(4,0)");
    CHECK(kk_press(3) == 1 && kk_moves == 1, "RIGHT moves anchor-side soldier");
    CHECK(kk_grid[0 * KK_COLS + 2] == 7 && kk_grid[0 * KK_COLS + 1] == 0,
          "s1 slid to (2,0)");
    CHECK(kk_grid[0 * KK_COLS + 3] == 8, "s2 untouched at (3,0)");
    CHECK(kk_invariants_ok(), "invariants after anchor-first move");
}

/* ---- 胜利判定与 kk_over 状态机 ---- */
static void test_solved(void) {
    /* 距胜利一步: 曹操 (1,1), 空位 (1,3)(2,3) 在其正下方 */
    kk_px[0] = 1; kk_py[0] = 1;
    kk_px[1] = 3; kk_py[1] = 2;             /* guan (3,2)(4,2) 横 */
    kk_px[2] = 0; kk_py[2] = 0;             /* zf (0,0) */
    kk_px[3] = 4; kk_py[3] = 0;             /* zy (4,0) */
    kk_px[4] = 0; kk_py[4] = 2;             /* mc (0,2) */
    kk_px[5] = 3; kk_py[5] = 0;             /* hz (3,0) */
    kk_px[6] = 1; kk_py[6] = 0;             /* s1 (1,0) */
    kk_px[7] = 2; kk_py[7] = 0;             /* s2 (2,0) */
    kk_px[8] = 3; kk_py[8] = 3;             /* s3 (3,3) */
    kk_px[9] = 4; kk_py[9] = 3;             /* s4 (4,3) */
    kk_rebuild();
    kk_anchor_first();
    kk_moves = 0; kk_over = false;
    CHECK(kk_invariants_ok(), "near-solved state valid");
    CHECK(kk_grid[3 * KK_COLS + 1] == 0 && kk_grid[3 * KK_COLS + 2] == 0,
          "blanks right below cao");
    CHECK(!kk_solved(), "near-solved not yet solved");
    CHECK(kk_press(1) == 1 && kk_moves == 1, "DOWN slides cao to exit");
    CHECK(kk_solved(), "solved after cao reaches bottom");
    CHECK(kk_over, "kk_over set on solve");
    /* 已胜利布局直接判定 */
    kk_px[0] = 1; kk_py[0] = 2;             /* cao (1,2) = 出口行 */
    kk_rebuild();
    CHECK(kk_solved(), "solved layout detected");
    kk_px[0] = 1; kk_py[0] = 1;             /* 差一行 */
    kk_rebuild();
    CHECK(!kk_solved(), "one row short not solved");
}

/* ---- 随机压测: 不变式 + 无死循环 + 步数一致 ---- */
static void test_random(void) {
    uint32_t fz = 12345u;
    klotski_enter();
    uint32_t local = 0;
    for (int i = 0; i < 10000; i++) {
        fz = fz * 1664525u + 1013904223u;
        int dir = (int)(fz % 4);
        int r = kk_press(dir);              /* 每次调用必然返回, 无死循环 */
        if (r) local++;
        if (!kk_invariants_ok()) {
            CHECK(0, "random press invariant");
            return;
        }
        if (kk_over) {
            CHECK(kk_solved(), "over implies solved");
            break;
        }
    }
    CHECK(kk_moves == local, "moves count consistent");
    CHECK(kk_invariants_ok(), "random invariants (10000 presses)");
    printf("  (random run ended: moves=%u solved=%d)\n",
           (unsigned)kk_moves, kk_over ? 1 : 0);
}

/* ---- BFS 可解性 + 最优解重放 (防呆: 状态数上限即死循环防护) ---- */
#define KK_BFS_MAX (1u << 20)
static uint64_t s_q[KK_BFS_MAX];
static uint32_t s_par[KK_BFS_MAX];
static uint8_t s_mv[KK_BFS_MAX];            /* ex(3b)|ey(2b)|dir(2b) */
static uint64_t s_vis[KK_BFS_MAX];
static uint8_t s_vused[KK_BFS_MAX];

static uint64_t kk_state_key(void) {
    uint64_t k = 0;
    int b = 0;
    for (int p = 0; p < KK_PIECES; p++) {
        int xb = (kk_pw[p] == 2) ? 2 : 3;
        k |= (uint64_t)(kk_px[p] & 7u) << b; b += xb;
        k |= (uint64_t)(kk_py[p] & 3u) << b; b += 2;
    }
    return k;
}

static void kk_state_set(uint64_t k) {
    int b = 0;
    for (int p = 0; p < KK_PIECES; p++) {
        int xb = (kk_pw[p] == 2) ? 2 : 3;
        kk_px[p] = (int)((k >> b) & ((1u << xb) - 1u)); b += xb;
        kk_py[p] = (int)((k >> b) & 3u); b += 2;
    }
    kk_rebuild();
}

static int kk_vis_add(uint64_t key) {
    size_t h = (size_t)((key * 0x9e3779b97f4a7c15ULL) >> 44) & (KK_BFS_MAX - 1);
    while (s_vused[h]) {
        if (s_vis[h] == key) return 0;
        h = (h + 1) & (KK_BFS_MAX - 1);
    }
    s_vused[h] = 1;
    s_vis[h] = key;
    return 1;
}

/* BFS 找胜利态, 返回该状态在队列中的下标; -1=不可解; -2=状态数超上限 */
static long kk_bfs(void) {
    memset(s_vused, 0, sizeof(s_vused));
    memset(s_vis, 0, sizeof(s_vis));
    klotski_enter();
    uint64_t start = kk_state_key();
    size_t head = 0, tail = 0;
    s_q[tail++] = start;
    kk_vis_add(start);
    s_par[0] = 0xFFFFFFFFu;
    s_mv[0] = 0;
    while (head < tail) {
        uint64_t cur = s_q[head];
        if ((cur & 3u) == 1u && ((cur >> 2) & 3u) == 2u) return (long)head;
        kk_state_set(cur);
        int ex[2], ey[2], ne = 0;
        for (int i = 0; i < KK_COLS * KK_ROWS && ne < 2; i++)
            if (kk_grid[i] == 0) { ex[ne] = i % KK_COLS; ey[ne] = i / KK_COLS; ne++; }
        int px0[KK_PIECES], py0[KK_PIECES];
        memcpy(px0, kk_px, sizeof(px0));
        memcpy(py0, kk_py, sizeof(py0));
        for (int e = 0; e < ne; e++)
            for (int d = 0; d < 4; d++) {
                if (!kk_slide_at(ex[e], ey[e], d)) continue;
                uint64_t nk = kk_state_key();
                memcpy(kk_px, px0, sizeof(kk_px));
                memcpy(kk_py, py0, sizeof(kk_py));
                kk_rebuild();
                if (!kk_vis_add(nk)) continue;
                if (tail >= KK_BFS_MAX) return -2;
                s_par[tail] = (uint32_t)head;
                /* 位域: ex(3b) | ey(2b)<<3 | d(2b)<<5 —— 互不重叠 */
                s_mv[tail] = (uint8_t)(ex[e] | (ey[e] << 3) | (d << 5));
                s_q[tail++] = nk;
            }
        head++;
    }
    return -1;
}

static void test_bfs_solvable(void) {
    long sol = kk_bfs();
    CHECK(sol >= 0, "BFS: solvable (not stuck/infinite)");
    if (sol < 0) {
        printf("  (bfs result=%ld)\n", sol);
        return;
    }
    /* 回溯出步序列, 重放(直接按 BFS 记录的空位+方向滑) */
    static uint32_t s_seq[KK_BFS_MAX];
    uint32_t *seq = s_seq;
    int len = 0;
    long i = sol;
    while (i != 0) {
        seq[len++] = s_mv[i];
        i = (long)s_par[i];
        if (len >= (int)KK_BFS_MAX) break;
    }
    CHECK(len > 0 && len < 400, "solution depth sane");
    klotski_enter();
    for (int s = len - 1; s >= 0; s--) {
        int ex = seq[s] & 7, ey = (seq[s] >> 3) & 3, d = (seq[s] >> 5) & 3;
        if (!kk_slide_at(ex, ey, d)) {
            CHECK(0, "replay slide should always succeed");
            return;
        }
        kk_anchor_first();                  /* 与 kk_press 同步: 移动后光标归位 */
        if (!kk_invariants_ok()) {
            CHECK(0, "replay invariant");
            return;
        }
    }
    CHECK(kk_solved(), "replayed full solution reaches solved");
    printf("  (optimal solution length = %d moves)\n", len);
}

int main(void) {
    printf("== klotski host tests ==\n");
    test_init();
    test_blocked();
    test_fallback_order();
    test_solved();
    test_random();
    test_bfs_solvable();
    if (s_fail == 0) {
        printf("ALL PASS\n");
        return 0;
    }
    printf("%d FAILED\n", s_fail);
    return 1;
}
