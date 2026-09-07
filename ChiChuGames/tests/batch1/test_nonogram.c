/* NONOGRAM 逻辑测试 — 包含游戏源文件, 直接访问 ng_* 静态状态
 * 覆盖: 图案提示与行程编码双向一致(5 张全查 + 硬编码抽查)、涂错计数、
 *       OK 循环/X 标记/黑格转叉、胜负判定(严格: 多余黑格/叉在图案格上不算赢)、
 *       N 换图循环、解后 OK 换下一图、光标移动/钳制/WASD/重复键过滤、退出、
 *       渲染冒烟(格线/黑格/叉/光标/HUD/SOLVED 提示)。
 * 无随机路径(图案固定), 无 do-while 随机循环, 无需死循环防护断言 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* 框架在 main.c 定义, 测试不链接 main.c */
bool s_exit_request = false;

#include "../../src/games/nonogram.c"

/* ---- 测试侧独立行程编码(与游戏实现同规则但独立代码) ---- */
static int ng_test_runs(const char *row, int out[NG_N]) {
    int n = 0, run = 0;
    for (int c = 0; c <= NG_N; c++) {
        if (c < NG_N && row[c] == 'X') {
            run++;
        } else if (run) {
            if (n < NG_N) out[n++] = run;
            run = 0;
        }
    }
    return n;
}

static const char *ng_row_of(int puz, int r) {
    return ng_puz[puz * NG_N + r];
}

static void test_hints_crosscheck(void) {
    for (int puz = 0; puz < NG_PUZZLES; puz++) {
        ng_start_puz((uint8_t)puz);
        assert(ng_puz_idx == puz);
        int rows[NG_N][NG_N], cols[NG_N][NG_N];
        int rn[NG_N], cn[NG_N];
        for (int r = 0; r < NG_N; r++) {
            rn[r] = ng_test_runs(ng_row_of(puz, r), rows[r]);
            /* 行: 游戏提示 == 独立行程编码 */
            assert(ng_rhint_n[r] == rn[r]);
            for (int i = 0; i < rn[r]; i++)
                assert(ng_rhint[r][i] == rows[r][i]);
        }
        for (int c = 0; c < NG_N; c++) {
            char col[NG_N + 1];
            for (int r = 0; r < NG_N; r++) col[r] = ng_row_of(puz, r)[c];
            col[NG_N] = 0;
            cn[c] = ng_test_runs(col, cols[c]);
            assert(ng_chint_n[c] == cn[c]);
            for (int i = 0; i < cn[c]; i++)
                assert(ng_chint[c][i] == cols[c][i]);
            /* 布局约束: 列至多 2 组(顶部提示带 2 行), 行至多 5 组 */
            assert(cn[c] <= 2);
            assert(rn[c] <= 5);
            for (int i = 0; i < cn[c]; i++) assert(cols[c][i] >= 1 && cols[c][i] <= 10);
        }
        for (int r = 0; r < NG_N; r++)
            for (int i = 0; i < rn[r]; i++) assert(rows[r][i] >= 1 && rows[r][i] <= 10);
        /* 行列黑格总数守恒 */
        int s1 = 0, s2 = 0, nb = 0;
        for (int r = 0; r < NG_N; r++) {
            for (int i = 0; i < rn[r]; i++) s1 += rows[r][i];
            for (int i = 0; i < cn[r]; i++) s2 += cols[r][i];
            for (int c = 0; c < NG_N; c++) if (ng_row_of(puz, r)[c] == 'X') nb++;
        }
        assert(s1 == nb && s2 == nb);
        printf("test_hints_crosscheck PASS (puzzle %d, black=%d)\n", puz, nb);
    }
    /* 硬编码抽查(与 /tmp/verify_ng.py 输出一致) */
    ng_start_puz(0);  /* HEART */
    assert(ng_rhint_n[0] == 2 && ng_rhint[0][0] == 2 && ng_rhint[0][1] == 2);
    assert(ng_rhint_n[9] == 0);                      /* 全空行无提示 */
    assert(ng_chint_n[0] == 1 && ng_chint[0][0] == 4);
    assert(ng_chint_n[2] == 1 && ng_chint[2][0] == 7);
    ng_start_puz(1);  /* TREE */
    assert(ng_rhint_n[4] == 1 && ng_rhint[4][0] == 4);
    assert(ng_chint_n[4] == 1 && ng_chint[4][0] == 10);  /* 双位 10 */
    ng_start_puz(2);  /* HOUSE */
    assert(ng_rhint_n[7] == 3 && ng_rhint[7][0] == 2 && ng_rhint[7][1] == 2 && ng_rhint[7][2] == 2);
    ng_start_puz(3);  /* FISH */
    assert(ng_chint_n[2] == 2 && ng_chint[2][0] == 2 && ng_chint[2][1] == 2);
    assert(ng_rhint_n[4] == 2 && ng_rhint[4][0] == 2 && ng_rhint[4][1] == 7);
    ng_start_puz(4);  /* STAR */
    assert(ng_rhint_n[5] == 3 && ng_rhint[5][0] == 2 && ng_rhint[5][1] == 4 && ng_rhint[5][2] == 2);
    assert(ng_chint_n[2] == 2 && ng_chint[2][0] == 1 && ng_chint[2][1] == 1);
    printf("test_hints_spotcheck PASS\n");
}

static void move_to(int x, int y) {
    key_event_t ev;
    memset(&ev, 0, sizeof ev);
    while ((int)ng_cx < x) { ev.key = K_RIGHT; nonogram_on_key(&ev); }
    while ((int)ng_cx > x) { ev.key = K_LEFT; nonogram_on_key(&ev); }
    while ((int)ng_cy < y) { ev.key = K_DOWN; nonogram_on_key(&ev); }
    while ((int)ng_cy > y) { ev.key = K_UP; nonogram_on_key(&ev); }
    assert(ng_cx == x && ng_cy == y);
}

static void press_ok(void) {
    key_event_t ev;
    memset(&ev, 0, sizeof ev);
    ev.key = K_OK;
    nonogram_on_key(&ev);
}

static void press_x(void) {
    key_event_t ev;
    memset(&ev, 0, sizeof ev);
    ev.key = K_CHAR;
    ev.ch = 'x';
    nonogram_on_key(&ev);
}

static void test_paint_wrong_x(void) {
    ng_start_puz(0);   /* HEART: (0,0) 是空格 */
    move_to(0, 0);
    assert(ng_wrong == 0);
    press_ok();        /* 涂错 → ERR 1 */
    assert(ng_cell[0] == NG_BLACK && ng_wrong == 1 && !ng_over);
    press_ok();        /* 再按取消 → ERR 0 */
    assert(ng_cell[0] == NG_EMPTY && ng_wrong == 0);
    /* 涂正确格 (1,1) → 不计数 */
    move_to(1, 1);
    press_ok();
    assert(ng_cell[11] == NG_BLACK && ng_wrong == 0);
    /* 叉: 空格标叉 → 再按取消 */
    move_to(0, 0);
    press_x();
    assert(ng_cell[0] == NG_X && ng_wrong == 0);
    press_x();
    assert(ng_cell[0] == NG_EMPTY);
    /* 黑格上标叉: 表达"此处应为空" → 黑格移除, 错数重算 */
    assert(ng_cell[11] == NG_BLACK);
    move_to(1, 1);
    press_x();
    assert(ng_cell[11] == NG_X && ng_wrong == 0);
    /* OK 键在叉格上直接变黑 */
    press_ok();
    assert(ng_cell[11] == NG_BLACK && ng_wrong == 0);
    /* Delete 键与 X 键等价 */
    move_to(0, 0);
    {
        key_event_t ev;
        memset(&ev, 0, sizeof ev);
        ev.key = K_DEL;
        nonogram_on_key(&ev);
    }
    assert(ng_cell[0] == NG_X);
    printf("test_paint_wrong_x PASS\n");
}

static void test_solve_strict(void) {
    /* 解 HEART: 全对 → SOLVED */
    ng_start_puz(0);
    for (int r = 0; r < NG_N; r++)
        for (int c = 0; c < NG_N; c++)
            if (ng_puz[r][c] == 'X') { move_to(c, r); press_ok(); }
    assert(ng_over);
    assert(ng_wrong == 0);
    for (int i = 0; i < 100; i++)
        assert(ng_cell[i] == (ng_puz[i / NG_N][i % NG_N] == 'X' ? NG_BLACK : NG_EMPTY));
    printf("test_solve PASS\n");
    /* 严格性 1: 缺一格 + 多余错黑格都不算赢 */
    ng_start_puz(0);
    for (int r = 0; r < NG_N; r++)
        for (int c = 0; c < NG_N; c++) {
            if (ng_puz[r][c] != 'X') continue;
            if (r == 0 && c == 1) continue;   /* 缺一格 (1,0) */
            move_to(c, r);
            press_ok();
        }
    assert(!ng_over);  /* 缺一格不算赢 */
    move_to(0, 9);     /* 图案空格 (9,0) 涂黑 → 多余 */
    press_ok();
    assert(!ng_over && ng_wrong == 1);
    press_ok();        /* 取消多余 */
    assert(!ng_over && ng_wrong == 0);
    move_to(1, 0);
    press_ok();        /* 补上缺失的图案格 → 全对 → 赢 */
    assert(ng_over && ng_wrong == 0);
    /* 严格性 2: 叉打在图案格上不算赢; 叉在空格上不影响赢 */
    ng_start_puz(0);
    move_to(1, 0);
    press_x();         /* (1,0) 是图案格, 标叉 */
    for (int r = 0; r < NG_N; r++)
        for (int c = 0; c < NG_N; c++) {
            if (ng_puz[r][c] != 'X') continue;
            if (r == 0 && c == 1) continue;
            move_to(c, r);
            press_ok();
        }
    assert(!ng_over);  /* 图案格上是叉, 不算赢 */
    move_to(1, 0);
    press_x();         /* 取消叉 → 空 */
    assert(!ng_over);
    press_ok();        /* 补黑 → 全对 → 赢 */
    assert(ng_over);
    ng_start_puz(0);   /* 叉在空格上不挡赢 */
    move_to(0, 0);
    press_x();         /* (0,0) 是空格, 标叉 */
    for (int r = 0; r < NG_N; r++)
        for (int c = 0; c < NG_N; c++)
            if (ng_puz[r][c] == 'X') { move_to(c, r); press_ok(); }
    assert(ng_over && ng_wrong == 0);   /* 空格上的叉保留, 仍算赢 */
    printf("test_solve_strict PASS\n");
}

static void test_next_picture(void) {
    ng_start_puz(0);
    ng_cell[0] = NG_BLACK;
    ng_wrong = ng_count_wrong();
    key_event_t ev;
    memset(&ev, 0, sizeof ev);
    ev.key = K_CHAR;
    ev.ch = 'n';
    nonogram_on_key(&ev);            /* N 换图 0→1 */
    assert(ng_puz_idx == 1 && !ng_over);
    assert(ng_cell[0] == NG_EMPTY && ng_wrong == 0);
    assert(ng_rhint_n[4] == 1 && ng_rhint[4][0] == 4);  /* TREE 提示已换 */
    for (int i = 0; i < 4; i++) {
        nonogram_on_key(&ev);        /* 1→2→3→4→0 循环 */
    }
    assert(ng_puz_idx == 0);
    assert(ng_rhint_n[0] == 2);      /* 回到 HEART 提示 */
    printf("test_next_picture PASS\n");
}

static void test_over_next_retry_quit(void) {
    /* 解完 → OK 换下一图 */
    ng_start_puz(0);
    for (int r = 0; r < NG_N; r++)
        for (int c = 0; c < NG_N; c++)
            if (ng_puz[r][c] == 'X') { move_to(c, r); press_ok(); }
    assert(ng_over);
    key_event_t ev;
    memset(&ev, 0, sizeof ev);
    ev.key = K_OK;
    nonogram_on_key(&ev);
    assert(!ng_over && ng_puz_idx == 1);   /* 换到 TREE */
    /* 解完 → N 同样换图 */
    ng_start_puz(1);
    for (int r = 0; r < NG_N; r++)
        for (int c = 0; c < NG_N; c++)
            if (ng_puz[NG_N + r][c] == 'X') { move_to(c, r); press_ok(); }
    assert(ng_over);
    ev.key = K_CHAR;
    ev.ch = 'n';
    nonogram_on_key(&ev);
    assert(!ng_over && ng_puz_idx == 2);
    /* 解后方向键不生效(未换图) */
    ng_start_puz(2);
    for (int r = 0; r < NG_N; r++)
        for (int c = 0; c < NG_N; c++)
            if (ng_puz[2 * NG_N + r][c] == 'X') { move_to(c, r); press_ok(); }
    assert(ng_over);
    ev.key = K_RIGHT;
    nonogram_on_key(&ev);
    assert(ng_puz_idx == 2 && ng_over);
    /* 解后 BACK 退出 */
    s_exit_request = false;
    ev.key = K_BACK;
    nonogram_on_key(&ev);
    assert(s_exit_request);
    printf("test_over_next_retry_quit PASS\n");
}

static void test_cursor_repeat(void) {
    ng_start_puz(0);
    key_event_t ev;
    memset(&ev, 0, sizeof ev);
    for (int i = 0; i < 20; i++) { ev.key = K_RIGHT; nonogram_on_key(&ev); }
    assert(ng_cx == 9);
    for (int i = 0; i < 20; i++) { ev.key = K_LEFT; nonogram_on_key(&ev); }
    assert(ng_cx == 0);
    for (int i = 0; i < 20; i++) { ev.key = K_UP; nonogram_on_key(&ev); }
    assert(ng_cy == 0);
    for (int i = 0; i < 20; i++) { ev.key = K_DOWN; nonogram_on_key(&ev); }
    assert(ng_cy == 9);
    /* WASD */
    ev.key = K_CHAR; ev.ch = 'w'; nonogram_on_key(&ev);
    assert(ng_cy == 8);
    ev.key = K_CHAR; ev.ch = 'a'; nonogram_on_key(&ev);
    assert(ng_cx == 0);
    ev.key = K_CHAR; ev.ch = 'a'; nonogram_on_key(&ev);
    assert(ng_cx == 0);
    /* 重复键: 确认键/字母忽略, 方向可响应 */
    ng_start_puz(0);
    ev.key = K_OK; ev.is_repeat = true;
    nonogram_on_key(&ev);
    assert(ng_cell[0] == NG_EMPTY && ng_wrong == 0);
    ev.key = K_CHAR; ev.ch = 'x'; ev.is_repeat = true;
    nonogram_on_key(&ev);
    assert(ng_cell[0] == NG_EMPTY);
    ev.key = K_CHAR; ev.ch = 'n'; ev.is_repeat = true;
    nonogram_on_key(&ev);
    assert(ng_puz_idx == 0);
    ev.key = K_RIGHT; ev.is_repeat = true;
    nonogram_on_key(&ev);
    assert(ng_cx == 1);
    printf("test_cursor_repeat PASS\n");
}

static void test_quit(void) {
    s_exit_request = false;
    key_event_t ev;
    memset(&ev, 0, sizeof ev);
    ev.key = K_QUIT;
    nonogram_on_key(&ev);
    assert(s_exit_request);
    printf("test_quit PASS\n");
}

/* ---- 渲染冒烟: 直接查帧缓冲 g_fb(黑=1) ---- */
static int ng_px(int x, int y) {
    if (x < 0 || x >= (int)CCG_W || y < 0 || y >= (int)CCG_H) return 0;
    int off = (y >> 3) * CCG_W + x;
    return (g_fb[off] >> (7 - (y & 7))) & 1;
}

static int ng_rect_has_black(int x0, int y0, int x1, int y1) {
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++)
            if (ng_px(x, y)) return 1;
    return 0;
}

static void test_render(void) {
    /* HEART, 光标 (0,0), (0,0) 空格 + 叉, (1,1) 黑格 */
    ng_start_puz(0);
    move_to(0, 0);
    press_x();              /* (0,0) = 叉 */
    move_to(1, 1);
    press_ok();             /* (1,1) = 黑 */
    move_to(0, 0);
    ng_over = false;
    nonogram_render();
    /* 黑格 (1,1) 中心黑 */
    assert(ng_px(NG_GRID_X + 12 + 6, NG_GRID_Y + 12 + 6) == 1);
    /* 格线: x=88 竖线, y=30 横线(边界行) */
    assert(ng_px(NG_GRID_X, NG_GRID_Y + 30) == 1);
    assert(ng_px(NG_GRID_X + 60, NG_GRID_Y) == 1);
    /* 叉: (0,0) 对角线上有黑点 */
    assert(ng_px(NG_GRID_X + 4, NG_GRID_Y + 4) == 1);
    assert(ng_px(NG_GRID_X + 7, NG_GRID_Y + 4) == 1);
    /* 光标: 空格上黑边框(左/上 2px 外扩) */
    assert(ng_px(NG_GRID_X - 1, NG_GRID_Y + 6) == 1);
    assert(ng_px(NG_GRID_X + 6, NG_GRID_Y - 1) == 1);
    assert(ng_px(NG_GRID_X - 2, NG_GRID_Y + 6) == 1);
    /* HUD: 左 "NONOGRAM", 右 "<名> ERR n", 分隔线 */
    assert(ng_rect_has_black(0, 0, 60, 7));
    assert(ng_rect_has_black(CCG_W - 90, 0, CCG_W - 4, 7));
    assert(ng_px(150, CCG_HUD_H - 1) == 1);
    /* 列提示贴网格行有字(y=23..29), 行提示右对齐区有字 */
    assert(ng_rect_has_black(NG_GRID_X, NG_HINT_L2_Y, NG_GRID_X + 120, NG_HINT_L2_Y + 7));
    assert(ng_rect_has_black(0, NG_GRID_Y + 3, NG_HINT_RIGHT, NG_GRID_Y + 10));
    /* 胜利态 HUD: SOLVED! + 右侧提示, 原标签被覆盖 */
    ng_start_puz(0);
    for (int r = 0; r < NG_N; r++)
        for (int c = 0; c < NG_N; c++)
            if (ng_puz[r][c] == 'X') { move_to(c, r); press_ok(); }
    assert(ng_over);
    ng_over_full = true;   /* host 跳过 disp_force_full */
    nonogram_render();
    assert(ng_rect_has_black(2, 2, 2 + 50, 2 + 7));       /* "SOLVED!" */
    assert(ng_rect_has_black(160, 2, CCG_W - 2, 9));      /* "OK/N:NEXT BACK:QUIT" */
    printf("test_render PASS\n");
}

int main(void) {
    printf("NONOGRAM logic tests\n");
    test_hints_crosscheck();
    test_paint_wrong_x();
    test_solve_strict();
    test_next_picture();
    test_over_next_retry_quit();
    test_cursor_repeat();
    test_render();
    test_quit();
    printf("ALL TESTS PASSED\n");
    return 0;
}
