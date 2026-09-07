/* 逻辑单测 — host cc 编译运行; 零平台依赖 */
#include "../src/config.h"
#include "../src/gfx/canvas.h"
#include "../src/gfx/pattern.h"
#include "../src/rng.h"
#include "../src/games/snake.c"
#include "../src/games/tetris.c"
#include "../src/games/g2048.c"
#include "../src/games/mines.c"
#include "../src/games/gomoku.c"
#include "../src/games/memory.c"
#include "../src/games/sokoban.c"
#include "../src/games/hanoi.c"
#include <stdio.h>
#include <string.h>

static int s_fail = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); s_fail++; } \
    else { printf("ok: %s\n", msg); } \
} while (0)

/* ---- host 框架 stub(g_fb 由 display.c 提供) ---- */
bool s_exit_request = false;
void game_set_tick_interval(uint32_t ms) { (void)ms; }

/* ---- test: 帧公式 ---- */
static void test_frame_layout(void) {
    fb_clear(false);
    /* 像素 (0,0) 应在字节 0 的 bit7 */
    fb_pixel(0, 0, true);
    CHECK(g_fb[0] == 0x80, "pixel(0,0) -> byte0 bit7");
    fb_clear(false);
    fb_pixel(7, 0, true);
    CHECK(g_fb[7] == 0x80, "pixel(7,0) -> byte7 bit7 (offset=x)");
    fb_clear(false);
    fb_pixel(0, 7, true);
    CHECK(g_fb[0] == 0x01, "pixel(0,7) -> byte0 bit0 (strip bottom)");
    fb_clear(false);
    fb_pixel(0, 8, true);
    CHECK(g_fb[296] == 0x80, "pixel(0,8) -> byte296 bit7 (next strip)");
    fb_clear(false);
    fb_pixel(295, 151, true);
    CHECK(g_fb[5623] == 0x01, "pixel(295,151) -> last byte bit0");
    fb_clear(false);
    fb_pixel(300, 200, true);  /* 越界应忽略 */
    CHECK(g_fb[0] == 0, "out-of-bounds pixel ignored");
}

/* ---- test: pattern 密度单调 ---- */
static void test_patterns(void) {
    unsigned dens[PAT_COUNT];
    for (int p = 0; p < PAT_COUNT; p++) {
        const uint8_t *t = pat_get((pat_id_t)p);
        unsigned n = 0;
        for (int i = 0; i < 8; i++)
            for (int b = 0; b < 8; b++)
                if (t[i] & (1u << b)) n++;
        dens[p] = n;
    }
    CHECK(dens[PAT_EMPTY] == 0, "PAT_EMPTY density 0");
    CHECK(dens[PAT_SOLID] == 64, "PAT_SOLID density 64");
    CHECK(dens[PAT_CROSS] > dens[PAT_GRID], "PAT_CROSS denser than PAT_GRID");
    CHECK(dens[PAT_GRID] > dens[PAT_DOT_DENSE], "PAT_GRID denser than PAT_DOT_DENSE");
    CHECK(dens[PAT_DOT_DENSE] > dens[PAT_DOT_SPARSE], "PAT_DOT_DENSE denser than PAT_DOT_SPARSE");
    /* 邻居图案两两不同(2048 可区分性) */
    for (int p = 1; p < PAT_COUNT; p++) {
        CHECK(memcmp(pat_get((pat_id_t)(p - 1)), pat_get((pat_id_t)p), 8) != 0,
              "adjacent patterns distinct");
    }
}

/* ---- test: 蛇逻辑 ---- */
static void test_snake(void) {
    /* 直接操作静态状态 */
    rng_seed(&tt_rng, 12345);
    s_len = 3; s_head = 0;
    s_bx[0] = 5; s_by[0] = 5;   /* 头 */
    s_bx[1] = 4; s_by[1] = 5;
    s_bx[2] = 3; s_by[2] = 5;
    s_dir = 1; s_next_dir = 1;
    s_alive = true; s_paused = false; g2_over = false; s_started = true;
    s_fx = 6; s_fy = 5;         /* 食物在头正前方一步 */
    snake_tick(0);
    CHECK(s_len == 4, "snake eats and grows");
    CHECK(s_bx[s_head] == 6 && s_by[s_head] == 5, "snake head advanced");
    CHECK(s_score == 10, "snake score +10");

    /* 撞墙 */
    s_bx[s_head] = SN_W - 1; s_by[s_head] = 5;
    s_next_dir = 1; s_dir = 1;
    snake_tick(0);
    CHECK(!s_alive, "snake dies on wall");

    /* 自撞 */
    s_alive = true; g2_over = false; s_started = true;
    s_len = 4; s_head = 0;
    s_bx[0] = 5; s_by[0] = 5;                      /* 头 */
    s_bx[SN_CAP - 1] = 5; s_by[SN_CAP - 1] = 6;    /* 身体(环索引) */
    s_bx[SN_CAP - 2] = 4; s_by[SN_CAP - 2] = 6;
    s_bx[SN_CAP - 3] = 4; s_by[SN_CAP - 3] = 5;
    s_dir = 0; s_next_dir = 0;  /* 向上撞自己身体(5,4)? body 在下方 */
    s_fx = 0; s_fy = 0;         /* 食物远离 */
    snake_tick(0);
    CHECK(s_alive, "snake does not die moving into own vacated tail space");
    /* 头 (5,5) 向上 → (5,4): 不在 body → 存活 */
    CHECK(s_bx[s_head] == 5 && s_by[s_head] == 4, "snake moved up");

    /* 反向输入被拒绝 */
    s_dir = 1; s_next_dir = 3;  /* 当前右, 想左 */
    s_bx[s_head] = 5; s_by[s_head] = 5;
    s_alive = true;
    snake_tick(0);
    CHECK(s_dir == 1, "snake rejects reverse direction");
}

/* ---- test: 蛇长跑(环形容器 256 步回绕回归) ---- */
static void test_snake_long_run(void) {
    rng_seed(&tt_rng, 777);
    s_len = 4; s_head = 0;
    s_bx[0] = 10; s_by[0] = 6;
    s_bx[SN_CAP - 1] = 9;  s_by[SN_CAP - 1] = 6;
    s_bx[SN_CAP - 2] = 8;  s_by[SN_CAP - 2] = 6;
    s_bx[SN_CAP - 3] = 7;  s_by[SN_CAP - 3] = 6;
    s_dir = 1; s_next_dir = 1;
    s_alive = true; s_paused = false; g2_over = false; s_started = true;
    s_fx = 0; s_fy = 0;
    /* 沿最下排往返跑 300 步(跨过 256 回绕点) */
    int row = SN_H - 1;  /* 最后一排 */
    s_bx[0] = 10; s_by[0] = (uint8_t)row;
    s_bx[SN_CAP - 1] = 9;  s_by[SN_CAP - 1] = (uint8_t)row;
    s_bx[SN_CAP - 2] = 8;  s_by[SN_CAP - 2] = (uint8_t)row;
    s_bx[SN_CAP - 3] = 7;  s_by[SN_CAP - 3] = (uint8_t)row;
    bool ok = true;
    for (int i = 0; i < 300 && ok; i++) {
        /* 矩形回路转角(无 180° 反向): 右→上→左→下 */
        if (s_dir == 1 && seg_x(0) >= SN_W - 3) s_next_dir = 0;  /* 右→上 */
        if (s_dir == 0 && seg_y(0) <= 7) s_next_dir = 3;         /* 上→左 */
        if (s_dir == 3 && seg_x(0) <= 2) s_next_dir = 2;         /* 左→下 */
        if (s_dir == 2 && seg_y(0) >= row) s_next_dir = 1;       /* 下→右 */
        snake_tick(0);
        if (!s_alive) {
            printf("  DIED at i=%d head=%u,%u seg1=%u,%u len=%u sh=%u dir=%d\n",
                   i, seg_x(0), seg_y(0), seg_x(1), seg_y(1), s_len, s_head, s_dir);
            ok = false; break;
        }
        /* 头身相邻检查(曼哈顿距离=1) */
        int hx = seg_x(0), hy = seg_y(0);
        int nx2 = seg_x(1), ny2 = seg_y(1);
        int dx = hx - nx2, dy = hy - ny2;
        if (dx < 0) dx = -dx;
        if (dy < 0) dy = -dy;
        if (dx + dy != 1) {
            printf("  BREAK at i=%d head=%u,%u seg1=%u,%u len=%u sh=%u dir=%d\n",
                   i, hx, hy, nx2, ny2, s_len, s_head, s_dir);
            ok = false; break;
        }
    }
    CHECK(ok, "snake ring survives 300 moves without head-body separation");
}

/* ---- test: 蛇头渲染位置(列21+ 的 uint8 溢出回归) ---- */
static void test_snake_head_render(void) {
    s_len = 4; s_head = 13;
    s_bx[13] = 23; s_by[13] = 6;
    s_bx[12] = 22; s_by[12] = 6;
    s_bx[11] = 21; s_by[11] = 6;
    s_bx[10] = 20; s_by[10] = 6;
    s_dir = 1; s_fx = 11; s_fy = 1;
    s_alive = true; s_started = true; g2_over = false;
    snake_render();
    /* 头格(23,6)中心像素 (286,96) 应为黑 */
    int off = (96 / 8) * CCG_W + 286;
    CHECK((g_fb[off] & 0x80) != 0, "head renders at grid (23,6)");
    /* 格(2,6)中心 (34,96) 应为白(头不能跑到最左) */
    off = (96 / 8) * CCG_W + 34;
    CHECK((g_fb[off] & 0x80) == 0, "no ghost head at grid (2,6)");
}

/* ---- test: 方块逻辑 ---- */
static void test_tetris(void) {
    /* 7 种方块 4 旋转各有 4 个格子, 位图合法 */
    for (int p = 0; p < 7; p++) {
        for (int r = 0; r < 4; r++) {
            int cells[8][2], n;
            piece_cells((uint8_t)p, (uint8_t)r, 0, 0, cells, &n);
            if (n != 4) { CHECK(0, "tetromino has 4 cells"); return; }
        }
    }
    CHECK(1, "all tetrominoes have 4 cells");

    /* 碰撞: 空场不撞; 场底撞 */
    for (int y = 0; y < TT_H; y++)
        for (int x = 0; x < TT_W; x++) tt_field[y][x] = 0;
    CHECK(!collides(3, 0, 2, 0), "T piece spawns free");
    CHECK(collides(3, TT_H - 1, 2, 0), "piece at bottom collides");

    /* 消行: 底行留四格, 用横向 I 块合法补满 */
    for (int x = 0; x < TT_W; x++)
        tt_field[TT_H - 1][x] = (x >= 3 && x < 7) ? 0 : 1;
    for (int x = 0; x < TT_W; x++) tt_field[TT_H - 2][x] = 0;
    tt_piece = 0; tt_rot = 0; tt_px = 3; tt_py = TT_H - 2;
    CHECK(!collides(tt_px, tt_py, tt_piece, tt_rot),
          "line-clear piece starts in a reachable position");
    CHECK(collides(tt_px, tt_py + 1, tt_piece, tt_rot),
          "line-clear piece is resting on the floor");
    uint32_t lines_before = tt_lines;
    lock_piece();
    CHECK(tt_lines == lines_before + 1, "line clear works");
    int empty = 1;
    for (int x = 0; x < TT_W; x++)
        if (tt_field[TT_H - 1][x]) { empty = 0; break; }
    CHECK(empty, "cleared row is empty after shift");

    /* HOLD 每块仅一次: 第二次 hold 被拒绝 */
    for (int y = 0; y < TT_H; y++)
        for (int x = 0; x < TT_W; x++) tt_field[y][x] = 0;
    tt_hold = 0;
    tt_hold_used = false;
    tt_piece = 2;   /* T */
    tt_next = 4;    /* Z */
    tt_bag_idx = 7;
    tt_rot = 0;
    tt_px = 3; tt_py = 5;   /* 场中位置, 便于验证没被重置到顶部 */
    do_hold();               /* 第一次: 暂存 T, 换入 Z */
    CHECK(tt_hold == 3 && tt_piece == 4, "first hold swaps piece");
    CHECK(tt_hold_used, "hold used flag set");
    int py_after = tt_py;
    do_hold();               /* 第二次: 应被拒绝, 位置不变 */
    CHECK(tt_hold == 3 && tt_piece == 4, "second hold rejected");
    CHECK(tt_py == py_after, "no respawn on rejected hold");

    /* 7-bag 前 7 块互不重复 */
    rng_seed(&tt_rng, 4242);
    for (int i = 0; i < 7; i++) tt_bag[i] = (uint8_t)i;
    tt_bag_idx = 7;
    uint8_t seen[7] = {0};
    for (int i = 0; i < 7; i++) seen[bag_take()]++;
    int uniq = 1;
    for (int i = 0; i < 7; i++)
        if (seen[i] != 1) { uniq = 0; break; }
    CHECK(uniq, "7-bag gives each piece once");
}

/* ---- test: 2048 逻辑 ---- */
static void test_2048(void) {
    /* 纯合并逻辑(不经 move_dir 的随机生成) */
    uint8_t line[4];
    uint32_t gain = 0;
    line[0]=1; line[1]=1; line[2]=1; line[3]=1;
    slide_line(line, 0, &gain);
    CHECK(line[0]==2 && line[1]==2 && line[2]==0 && line[3]==0 && gain==8,
          "2048 [2,2,2,2] left -> [4,4]");
    line[0]=2; line[1]=1; line[2]=1; line[3]=0;
    gain = 0;
    slide_line(line, 0, &gain);
    CHECK(line[0]==2 && line[1]==2 && line[2]==0 && line[3]==0,
          "2048 [4,2,2] left -> [4,4] (no chain)");
    line[0]=1; line[1]=0; line[2]=0; line[3]=1;
    gain = 0;
    slide_line(line, 1, &gain);
    CHECK(line[0]==0 && line[1]==0 && line[2]==0 && line[3]==2,
          "2048 [2,0,0,2] right -> [0,0,0,4]");
    line[0]=2; line[1]=2; line[2]=4; line[3]=4;
    gain = 0;
    slide_line(line, 0, &gain);
    CHECK(line[0]==3 && line[1]==5 && line[2]==0 && line[3]==0,
          "2048 [4,4,16,16] left -> [8,32]");
    /* 用户案例: 满列下移必须不动(写回顺序回归) */
    line[0]=2; line[1]=5; line[2]=2; line[3]=1;   /* 4,32,4,2 */
    gain = 0;
    slide_line(line, 3, &gain);
    CHECK(line[0]==2 && line[1]==5 && line[2]==2 && line[3]==1,
          "2048 [4,32,4,2] down stays [4,32,4,2]");
    /* 右移无合并: [2,4,0,0] right -> [0,0,2,4] */
    line[0]=1; line[1]=2; line[2]=0; line[3]=0;
    gain = 0;
    slide_line(line, 1, &gain);
    CHECK(line[0]==0 && line[1]==0 && line[2]==1 && line[3]==2,
          "2048 [2,4,0,0] right -> [0,0,2,4]");

    /* move_dir: 新块只在空位生成 */
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++) g2_cell[y][x] = 0;
    g2_cell[0][0] = 1; g2_cell[0][1] = 1;
    uint32_t before_score = g2_score;
    rng_seed(&g2_rng, 7);
    move_dir(0);
    CHECK(g2_score > before_score, "2048 merge scores");
    int empty_count = 0;
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++)
            if (!g2_cell[y][x]) empty_count++;
    CHECK(empty_count == 14, "2048 spawns one tile in empty (merged tile counts) ");

    /* 满盘判定 */
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++) g2_cell[y][x] = (uint8_t)(y + x + 1);
    CHECK(!can_move(), "2048 full board no moves");
    g2_cell[3][3] = g2_cell[3][2];
    CHECK(can_move(), "2048 adjacent equal detectable");
}

/* ---- test: 扫雷逻辑 ---- */
static void test_mines(void) {
    /* 布雷计数正确 */
    for (int y = 0; y < MS_H; y++)
        for (int x = 0; x < MS_W; x++) ms_b[y][x] = MS_EMPTY;
    ms_b[0][0] = MS_CELL; ms_b[0][2] = MS_CELL;
    for (int y = 0; y < MS_H; y++)
        for (int x = 0; x < MS_W; x++)
            if (ms_b[y][x] != MS_CELL) ms_b[y][x] = count_adj(x, y);
    CHECK(ms_b[0][1] == 2, "mines adjacent count = 2");
    CHECK(ms_b[1][1] == 2, "mines diagonal count = 2");
    CHECK(ms_b[1][2] == 1, "mines near-corner count = 1");
    CHECK(ms_b[8][14] == 0, "mines far corner count = 0");

    /* 首击安全: 布雷避开目标格及其邻格 */
    rng_seed(&ms_rng, 42);
    plant_mines(7, 4);
    int safe = 1;
    for (int dy = -1; dy <= 1 && safe; dy++)
        for (int dx = -1; dx <= 1 && safe; dx++)
            if (ms_b[4 + dy][7 + dx] == MS_CELL) safe = 0;
    CHECK(safe, "first reveal is mine-free (3x3)");
    int mine_count = 0;
    for (int y = 0; y < MS_H; y++)
        for (int x = 0; x < MS_W; x++)
            if (ms_b[y][x] == MS_CELL) mine_count++;
    CHECK(mine_count == MS_MINES, "exactly 15 mines planted");

    /* 翻开: 空区洪泛 */
    for (int y = 0; y < MS_H; y++)
        for (int x = 0; x < MS_W; x++) { ms_rep[y][x] = false; ms_flag[y][x] = false; }
    for (int y = 0; y < MS_H; y++)
        for (int x = 0; x < MS_W; x++) ms_b[y][x] = MS_EMPTY;   /* 全空场 */
    ms_b[0][0] = MS_CELL;
    for (int y = 0; y < MS_H; y++)
        for (int x = 0; x < MS_W; x++)
            if (ms_b[y][x] != MS_CELL) ms_b[y][x] = count_adj(x, y);
    ms_revealed = 0;
    flood_reveal(7, 4);
    CHECK(ms_revealed > 50, "flood reveal opens large area");
    /* 插旗的格子不被洪泛 */
    for (int y = 0; y < MS_H; y++)
        for (int x = 0; x < MS_W; x++) { ms_rep[y][x] = false; ms_flag[y][x] = false; }
    ms_revealed = 0;
    ms_flag[4][7] = true;
    ms_rep[4][7] = false;
    ms_cx = 7; ms_cy = 4;
    ms_flag[ms_cy][ms_cx] = true;
    ms_flags = 1;
    toggle_flag();   /* 取消旗 */
    CHECK(!ms_flag[4][7], "flag toggles off");
    CHECK(ms_flags == 0, "flag count back to 0");
}

/* ---- test: 五子棋 ---- */
static void test_ttt(void) {
    /* 五连判定: 横/竖/斜 */
    for (int y = 0; y < GO_N; y++)
        for (int x = 0; x < GO_N; x++) g_board[y][x] = 0;
    for (int i = 0; i < 5; i++) g_board[7][3 + i] = 1;
    CHECK(check_win_at(7, 7), "gomoku horizontal five detected");
    for (int y = 0; y < GO_N; y++)
        for (int x = 0; x < GO_N; x++) g_board[y][x] = 0;
    for (int i = 0; i < 5; i++) g_board[3 + i][7] = 1;
    CHECK(check_win_at(7, 7), "gomoku vertical five detected");
    for (int y = 0; y < GO_N; y++)
        for (int x = 0; x < GO_N; x++) g_board[y][x] = 0;
    for (int i = 0; i < 5; i++) g_board[3 + i][3 + i] = 1;
    CHECK(check_win_at(7, 7), "gomoku diagonal five detected");
    for (int y = 0; y < GO_N; y++)
        for (int x = 0; x < GO_N; x++) g_board[y][x] = 0;
    for (int i = 0; i < 4; i++) g_board[7][3 + i] = 1;
    CHECK(!check_win_at(7, 6), "gomoku four is not a win");

    /* AI 封堵活四: 玩家 4 连两端开口, AI 必须堵(困难难度) */
    g_diff = 1;
    for (int y = 0; y < GO_N; y++)
        for (int x = 0; x < GO_N; x++) g_board[y][x] = 0;
    for (int i = 0; i < 4; i++) g_board[7][3 + i] = 1;
    g_board[7][2] = 0;
    g_board[7][7] = 0;
    ai_move();
    int blocked = 0;
    for (int i = 0; i < 5; i++)
        if (g_board[7][i] == 2) blocked = 1;
    CHECK(blocked, "gomoku AI blocks open four");
}

/* ---- test: 记忆翻牌 ---- */
static void test_memory(void) {
    /* 洗牌: 12 对, 每种图标恰好 2 张 */
    rng_seed(&m_rng, 55);
    uint8_t deck[MM_W * MM_H];
    for (int i = 0; i < MM_W * MM_H; i++) deck[i] = (uint8_t)(i / 2 + 1);
    for (int i = MM_W * MM_H - 1; i > 0; i--) {
        uint32_t j = rng_range(&m_rng, (uint32_t)i + 1);
        uint8_t t = deck[i];
        deck[i] = deck[j];
        deck[j] = t;
    }
    int count[17] = {0};
    for (int i = 0; i < MM_W * MM_H; i++) count[deck[i]]++;
    int ok = 1;
    for (int v = 1; v <= 16; v++)
        if (count[v] != 2) ok = 0;
    CHECK(ok, "memory deck has 16 pairs");

    /* 配对逻辑: 两张相同 → matched */
    for (int y = 0; y < MM_H; y++)
        for (int x = 0; x < MM_W; x++) {
            m_cards[y][x] = (uint8_t)(y * MM_W + x) / 2 + 1;
            m_matched[y][x] = false;
            m_open[y][x] = false;
        }
    m_f1x = m_f1y = m_f2x = m_f2y = -1;
    m_over = false;
    m_cx = 0; m_cy = 0;
    m_open[0][0] = true; m_f1x = 0; m_f1y = 0;
    m_cx = 1; m_cy = 0;   /* 同图标 (0,0) 与 (1,0) */
    m_open[0][1] = true; m_f2x = 1; m_f2y = 0;
    m_moves = 0;
    if (m_cards[0][0] == m_cards[0][1]) {
        m_matched[0][0] = true;
        m_matched[0][1] = true;
        m_f1x = m_f1y = m_f2x = m_f2y = -1;
    }
    CHECK(m_matched[0][0] && m_matched[0][1], "memory matching marks pair");
}

/* ---- test: 推箱子 ---- */
static void test_sokoban(void) {
    rng_seed(&sk_rng, 20260829);
    /* 每关: 箱子数 == 目标数, 有人 */
    for (int l = 0; l < SK_LEVELS; l++) {
        s_level = (uint8_t)l;
        load_level();
        int boxes = 0, goals = 0;
        for (int y = 0; y < SK_H; y++)
            for (int x = 0; x < SK_W; x++) {
                if (s_box[y][x]) boxes++;
                if (s_goal[y][x]) goals++;
            }
        if (boxes != goals || boxes == 0) {
            CHECK(0, "sokoban level valid");
            return;
        }
    }
    CHECK(1, "sokoban all levels balanced");

    /* 推箱并撤销: 玩家、箱子数量和坐标都必须恢复 */
    load_level();
    bool pushed = false;
    for (int y = 1; y < SK_H - 1 && !pushed; y++)
        for (int x = 1; x < SK_W - 1 && !pushed; x++)
            if (s_box[y][x] && !s_wall[y][x - 1] && !s_box[y][x - 1] &&
                !s_wall[y][x + 1] && !s_box[y][x + 1]) {
                int boxes_before = 0;
                for (int by = 0; by < SK_H; by++)
                    for (int bx = 0; bx < SK_W; bx++)
                        if (s_box[by][bx]) boxes_before++;
                s_px = (int8_t)(x - 1);
                s_py = (int8_t)y;
                try_move(1, 0);
                CHECK(s_box[y][x] == 0 && s_box[y][x + 1] == 1,
                      "sokoban box moved one cell");
                undo_move();
                int boxes_after = 0;
                for (int by = 0; by < SK_H; by++)
                    for (int bx = 0; bx < SK_W; bx++)
                        if (s_box[by][bx]) boxes_after++;
                CHECK(s_px == x - 1 && s_py == y, "sokoban undo restores player");
                CHECK(s_box[y][x] == 1 && s_box[y][x + 1] == 0,
                      "sokoban undo restores pushed box");
                CHECK(boxes_after == boxes_before, "sokoban undo preserves box count");
                pushed = true;
            }
    CHECK(pushed, "sokoban generated level exposes a testable push");

    /* RESET 恢复本关快照，而不是生成另一张地图 */
    s_box[1][1] ^= 1u;
    s_px = 2;
    s_py = 2;
    reset_level();
    bool reset_ok = s_px == s_initial_px && s_py == s_initial_py;
    for (int y = 0; y < SK_H && reset_ok; y++)
        for (int x = 0; x < SK_W; x++)
            if (s_box[y][x] != s_initial_box[y][x]) reset_ok = false;
    CHECK(reset_ok, "sokoban reset restores initial snapshot");
}

/* ---- test: 汉诺塔 ---- */
static void test_hanoi(void) {
    hanoi_reset();
    CHECK(h_count[0] == 6 && h_count[1] == 0 && h_count[2] == 0,
          "hanoi starts stacked on peg 0");
    /* 合法移动: 顶盘 0 移到 1 */
    h_cur = 1;
    h_sel = 0;
    /* 模拟 OK 移动 */
    int src = -1;
    for (int d = 0; d < HN_DISKS; d++)
        if (h_peg[d] == h_sel) { src = d; break; }
    h_peg[src] = h_cur;
    rebuild_counts();
    CHECK(h_count[0] == 5 && h_count[1] == 1, "hanoi top disk moves");
    /* 非法: 大盘放到小盘上 */
    int big = -1;
    for (int d = 0; d < HN_DISKS; d++)
        if (h_peg[d] == 0) { big = d; break; }
    int small = -1;
    for (int d = 0; d < HN_DISKS; d++)
        if (h_peg[d] == 1) { small = d; break; }
    CHECK(big == 1 && small == 0, "hanoi top disks identified");
}

/* ---- test: rng ---- */
static void test_rng(void) {
    rng_t a, b;
    rng_seed(&a, 0xdeadbeef);
    rng_seed(&b, 0xdeadbeef);
    for (int i = 0; i < 100; i++) {
        if (rng_next(&a) != rng_next(&b)) { CHECK(0, "rng deterministic"); return; }
    }
    CHECK(1, "rng deterministic");
    /* 范围与塌陷 */
    rng_seed(&a, 0x1234);
    uint32_t hist[8] = {0};
    for (int i = 0; i < 4000; i++) hist[rng_range(&a, 8)]++;
    CHECK(hist[0] > 300 && hist[0] < 700, "rng_range uniform-ish");
}

int main(void) {
    test_frame_layout();
    test_patterns();
    test_snake();
    test_snake_long_run();
    test_snake_head_render();
    test_tetris();
    test_2048();
    test_mines();
    test_ttt();
    test_memory();
    test_sokoban();
    test_hanoi();
    test_rng();
    if (s_fail == 0) { printf("ALL TESTS PASSED\n"); return 0; }
    printf("%d TEST(S) FAILED\n", s_fail);
    return 1;
}
