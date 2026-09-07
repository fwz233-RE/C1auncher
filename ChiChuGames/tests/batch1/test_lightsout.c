/* LIGHTS OUT 逻辑测试 — 包含游戏源文件, 直接访问 lo_* 静态状态
 * 覆盖: 按压翻转(角/边/中心/恒等)、最少步数求解器(GF(2), BFS 深度 11
 * 交叉验证 + 小盘面精确值 + 奇偶守恒)、25x25 矩阵秩、生成可解性、
 * 胜负判定(回放解法)、重开(N)、退出(Q)、光标移动/钳制、重复键过滤 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 框架在 main.c 定义, 测试不链接 main.c */
bool s_exit_request = false;

#include "../../src/games/lightsout.c"

/* ---- BFS 距离表(从全灭反向, 深度 11) ---- */
#define LO_BFS_DEPTH 11u
static uint8_t *lo_dist;
static uint32_t *lo_queue;
static uint32_t lo_press_pat[25];
static uint32_t lo_bfs_states;

static uint32_t lo_count_on(const uint8_t *b) {
    uint32_t n = 0;
    for (int i = 0; i < 25; i++) n += b[i];
    return n;
}

static uint32_t lo_bitcount(uint32_t v) {
    uint32_t w = 0;
    while (v) { v &= v - 1; w++; }
    return w;
}

static void lo_build_press_pat(void) {
    uint8_t b[25];
    for (int i = 0; i < 25; i++) {
        memset(b, 0, sizeof b);
        lo_press_at(i, b);
        uint32_t p = 0;
        for (int j = 0; j < 25; j++)
            if (b[j]) p |= 1u << j;
        lo_press_pat[i] = p;
    }
    /* 十字邻接数: 角 3 / 边 4 / 中心 5 */
    assert(lo_bitcount(lo_press_pat[0]) == 3);
    assert(lo_bitcount(lo_press_pat[2]) == 4);
    assert(lo_bitcount(lo_press_pat[10]) == 4);
    assert(lo_bitcount(lo_press_pat[12]) == 5);
    assert(lo_bitcount(lo_press_pat[24]) == 3);
}

static void lo_bfs(void) {
    lo_dist = malloc(1u << 25);
    lo_queue = malloc((1u << 24) * sizeof(uint32_t));   /* 深度 11 共 ~11.6M 状态 */
    assert(lo_dist && lo_queue);
    memset(lo_dist, 0xFF, 1u << 25);
    uint32_t head = 0, tail = 0;
    lo_queue[tail++] = 0;
    lo_dist[0] = 0;
    while (head < tail) {
        uint32_t s = lo_queue[head++];
        if (lo_dist[s] >= LO_BFS_DEPTH) continue;
        for (int i = 0; i < 25; i++) {
            uint32_t ns = s ^ lo_press_pat[i];
            if (lo_dist[ns] == 0xFF) {
                lo_dist[ns] = (uint8_t)(lo_dist[s] + 1);
                lo_queue[tail++] = ns;
            }
        }
    }
    lo_bfs_states = tail;
}

static uint32_t lo_board_state(void) {
    uint32_t st = 0;
    for (int i = 0; i < 25; i++)
        if (lo_g[i]) st |= 1u << i;
    return st;
}

/* 可复现生成: presses 次随机按压(可记录各格), 结果必可解 */
static void lo_gen_board(uint64_t seed, uint32_t presses, uint32_t *cells) {
    rng_t r;
    rng_seed(&r, seed);
    memset(lo_g, 0, sizeof lo_g);
    for (uint32_t i = 0; i < presses; i++) {
        uint32_t c = rng_range(&r, 25u);
        lo_press_at((int)c, lo_g);
        if (cells) cells[i] = c;
    }
}

static void test_toggle(void) {
    uint8_t b[25];
    /* 左上角 (0,0): 自身 + 右 + 下 */
    memset(b, 0, sizeof b);
    lo_press_at(0, b);
    assert(b[0] == 1 && b[1] == 1 && b[5] == 1);
    assert(b[2] == 0 && b[6] == 0 && b[10] == 0);
    assert(lo_count_on(b) == 3);
    /* 同格两次 = 恒等 */
    lo_press_at(0, b);
    assert(lo_count_on(b) == 0);
    /* 上边缘 (2,0): 1,2,3,7 */
    memset(b, 0, sizeof b);
    lo_press_at(2, b);
    assert(b[1] && b[2] && b[3] && b[7]);
    assert(lo_count_on(b) == 4);
    /* 中心 (2,2): 7,11,12,13,17 */
    memset(b, 0, sizeof b);
    lo_press_at(12, b);
    assert(b[7] && b[11] && b[12] && b[13] && b[17]);
    assert(lo_count_on(b) == 5);
    /* 右下角 (4,4): 19,23,24 */
    memset(b, 0, sizeof b);
    lo_press_at(24, b);
    assert(b[19] && b[23] && b[24]);
    assert(lo_count_on(b) == 3);
    printf("test_toggle PASS\n");
}

static void test_solver_exact(void) {
    uint8_t b[25];
    memset(b, 0, sizeof b);
    assert(lo_solve_min(b) == 0);          /* 全灭 = 0 步 */
    for (int i = 0; i < 25; i++) {
        memset(b, 0, sizeof b);
        lo_press_at(i, b);
        assert(lo_solve_min(b) == 1);      /* 单格 = 1 步 */
    }
    for (int i = 0; i < 24; i++) {
        memset(b, 0, sizeof b);
        lo_press_at(i, b);
        lo_press_at(i + 1, b);
        assert(lo_solve_min(b) == 2);      /* 相邻两格 = 2 步 */
    }
    memset(b, 0, sizeof b);
    lo_press_at(12, b);
    lo_press_at(12, b);
    assert(lo_solve_min(b) == 0);          /* 两次同格 = 回全灭 */
    printf("test_solver_exact PASS\n");
}

static void test_rank(void) {
    /* 25x25 十字翻转矩阵秩 = 23(零空间维 2), 决定求解器枚举 4 个候选 */
    uint32_t a[25];
    memcpy(a, lo_press_pat, sizeof a);
    int piv = 0;
    for (int col = 0; col < 25 && piv < 25; col++) {
        int r = -1;
        for (int i = piv; i < 25; i++)
            if (a[i] & (1u << col)) { r = i; break; }
        if (r < 0) continue;
        uint32_t t = a[piv];
        a[piv] = a[r];
        a[r] = t;
        for (int i = 0; i < 25; i++)
            if (i != piv && (a[i] & (1u << col))) a[i] ^= a[piv];
        piv++;
    }
    assert(piv == 23);
    printf("test_rank PASS (rank=23)\n");
}

static void test_bfs_cross(void) {
    int exact = 0, beyond = 0;
    for (int t = 0; t < 300; t++) {
        uint32_t presses = 30u + (uint32_t)(t % 31u);
        lo_gen_board((uint64_t)t + 1, presses, NULL);
        uint32_t min = lo_solve_min(lo_g);
        uint32_t st = lo_board_state();
        assert(min >= 1);                          /* 非全灭 */
        assert(min <= presses);                    /* 不劣于生成序列 */
        assert((min & 1u) == (presses & 1u));      /* 奇偶守恒(零空间向量偶重量) */
        if (lo_dist[st] != 0xFF) {
            assert(min == lo_dist[st]);            /* 与 BFS 精确距离一致 */
            exact++;
        } else {
            assert(min > LO_BFS_DEPTH);            /* 超 BFS 深度则距离更大 */
            beyond++;
        }
    }
    printf("test_bfs_cross PASS (exact=%d beyond-depth=%d)\n", exact, beyond);
}

static void test_win_and_restart(void) {
    uint32_t cells[64];
    uint32_t presses = 37;
    lo_gen_board(4242, presses, cells);
    assert(lo_solve_min(lo_g) >= 1);
    /* 回放按压序列 → 全灭 → 胜利 */
    lo_moves = 0;
    lo_over = false;
    lo_over_full = false;
    key_event_t ev;
    memset(&ev, 0, sizeof ev);
    ev.key = K_OK;
    uint32_t played = 0;
    for (uint32_t i = 0; i < presses; i++) {
        lo_cx = (uint8_t)(cells[i] % 5u);
        lo_cy = (uint8_t)(cells[i] / 5u);
        lightsout_on_key(&ev);
        played++;
        if (lo_over) break;
    }
    assert(lo_over);
    assert(lo_moves == played);
    assert(lo_solve_min(lo_g) == 0);
    printf("test_win PASS (moves=%u)\n", lo_moves);
    /* 胜利态按 N → 重开新局 */
    s_exit_request = false;
    ev.key = K_CHAR;
    ev.ch = 'n';
    ev.is_repeat = false;
    lightsout_on_key(&ev);
    assert(!lo_over);
    assert(lo_moves == 0);
    assert(lo_min >= 1);
    assert(lo_min == lo_solve_min(lo_g));
    printf("test_restart PASS\n");
}

static void test_cursor_repeat(void) {
    key_event_t ev;
    memset(&ev, 0, sizeof ev);
    lo_cx = 2;
    lo_cy = 2;
    for (int i = 0; i < 10; i++) { ev.key = K_RIGHT; lightsout_on_key(&ev); }
    assert(lo_cx == 4);
    for (int i = 0; i < 10; i++) { ev.key = K_LEFT; lightsout_on_key(&ev); }
    assert(lo_cx == 0);
    for (int i = 0; i < 10; i++) { ev.key = K_UP; lightsout_on_key(&ev); }
    assert(lo_cy == 0);
    for (int i = 0; i < 10; i++) { ev.key = K_DOWN; lightsout_on_key(&ev); }
    assert(lo_cy == 4);
    /* WASD */
    ev.key = K_CHAR; ev.ch = 'w'; lightsout_on_key(&ev);
    assert(lo_cy == 3);
    ev.key = K_CHAR; ev.ch = 'a'; lightsout_on_key(&ev);
    assert(lo_cx == 0);
    ev.key = K_CHAR; ev.ch = 'a'; lightsout_on_key(&ev);
    assert(lo_cx == 0);   /* 左边界钳制 */
    /* 重复键: 确认/字母忽略, 方向可响应 */
    lo_moves = 0;
    ev.key = K_OK; ev.is_repeat = true;
    lightsout_on_key(&ev);
    assert(lo_moves == 0);
    ev.key = K_CHAR; ev.ch = 'n'; ev.is_repeat = true;
    lightsout_on_key(&ev);
    assert(lo_moves == 0 && !lo_over);
    ev.key = K_RIGHT; ev.is_repeat = true;
    lightsout_on_key(&ev);
    assert(lo_cx == 1);
    printf("test_cursor_repeat PASS\n");
}

static void test_generation(void) {
    for (int t = 0; t < 40; t++) {
        lo_new_game();
        assert(!lo_over);
        assert(lo_moves == 0);
        assert(lo_cx == 2 && lo_cy == 2);
        assert(lo_min >= 1);
        assert(lo_min <= 60);              /* 生成按压 ≤60 次 */
        assert(lo_min == lo_solve_min(lo_g));
    }
    printf("test_generation PASS\n");
}

static void test_quit(void) {
    s_exit_request = false;
    key_event_t ev;
    memset(&ev, 0, sizeof ev);
    ev.key = K_QUIT;
    lightsout_on_key(&ev);
    assert(s_exit_request);
    printf("test_quit PASS\n");
}

/* ---- 渲染冒烟: 直接查帧缓冲 g_fb(黑=1) ---- */
static int lo_px(int x, int y) {
    int off = (y >> 3) * CCG_W + x;
    return (g_fb[off] >> (7 - (y & 7))) & 1;
}

static int lo_rect_has_black(int x0, int y0, int x1, int y1) {
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++)
            if (lo_px(x, y)) return 1;
    return 0;
}

static void test_render(void) {
    /* 已知盘面: 仅 (0,0) 亮, 光标在其上 */
    memset(lo_g, 0, sizeof lo_g);
    lo_g[0] = 1;
    lo_cx = 0;
    lo_cy = 0;
    lo_moves = 7;
    lo_min = 5;
    lo_over = false;
    lightsout_render();
    /* 亮格: 白底 + 中央黑圆点 */
    assert(lo_px(LO_OX + 2, LO_OY + 2) == 0);          /* 白底角落 */
    assert(lo_px(LO_OX + 13, LO_OY + 13) == 1);        /* 黑圆点 */
    /* 灭格 (0,1): 黑底 */
    assert(lo_px(LO_OX + 13, LO_OY + 26 + 13) == 1);
    /* 光标: 亮格反色黑边框 */
    assert(lo_px(LO_OX - 1, LO_OY + 13) == 1);
    assert(lo_px(LO_OX - 2, LO_OY + 13) == 1);
    /* HUD 顶栏: 左标题 + 右数值 + 底部分隔线 */
    assert(lo_rect_has_black(0, 0, 50, 7));            /* "LIGHTS OUT" */
    assert(lo_rect_has_black(CCG_W - 90, 0, CCG_W - 4, 7));  /* "MOVES 7 MIN 5" */
    assert(lo_px(150, CCG_HUD_H - 1) == 1);            /* 分隔线 */
    /* 胜利态 HUD */
    lo_over = true;
    lo_over_full = true;   /* 跳过 disp_force_full(host 无碍, 省时) */
    lightsout_render();
    assert(lo_rect_has_black(2, 2, 2 + 40, 2 + 7));    /* "SOLVED!" */
    assert(lo_rect_has_black(170, 2, 290, 9));         /* "OK/N:RETRY BACK:QUIT" */
    lo_over = false;
    printf("test_render PASS\n");
}

int main(void) {
    printf("LIGHTS OUT logic tests\n");
    lo_build_press_pat();
    test_toggle();
    test_solver_exact();
    test_rank();
    lo_bfs();
    printf("BFS built: depth %u, %u states\n", (unsigned)LO_BFS_DEPTH, (unsigned)lo_bfs_states);
    test_bfs_cross();
    test_generation();
    test_win_and_restart();
    test_cursor_repeat();
    test_render();
    test_quit();
    free(lo_dist);
    free(lo_queue);
    printf("ALL TESTS PASSED\n");
    return 0;
}
