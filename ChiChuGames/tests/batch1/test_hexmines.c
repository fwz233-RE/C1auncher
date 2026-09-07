/* HEXMINES 逻辑单测 — host 编译, 包含游戏 .c 直接访问静态状态 */
#include "../../src/games/hexmines.c"
#include <stdio.h>
#include <string.h>

static int s_fail = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); s_fail++; } \
    else { printf("ok: %s\n", msg); } \
} while (0)

/* ---- host 框架 stub ---- */
bool s_exit_request = false;
void game_set_tick_interval(uint32_t ms) { (void)ms; }

static void board_reset(void) {
    for (int y = 0; y < HM_ROWS; y++)
        for (int x = 0; x < HM_COLS; x++) {
            hm_b[y][x] = 0;
            hm_rep[y][x] = false;
            hm_flag[y][x] = false;
        }
    hm_over = false;
    hm_won = false;
    hm_revealed = 0;
    hm_flags = 0;
}

/* 六邻几何: 对称性 + 已知邻接对 + 度数界 */
static void test_geom(void) {
    int total_deg = 0;
    for (int y = 0; y < HM_ROWS; y++) {
        for (int x = 0; x < HM_COLS; x++) {
            int deg = 0;
            const int8_t (*d)[2] = hm_d[y & 1];
            for (int i = 0; i < 6; i++) {
                int nx = x + d[i][0], ny = y + d[i][1];
                if (nx < 0 || nx >= HM_COLS || ny < 0 || ny >= HM_ROWS) continue;
                deg++;
                /* 对称: (nx,ny) 的邻居必须含 (x,y) */
                int back = 0;
                const int8_t (*d2)[2] = hm_d[ny & 1];
                for (int j = 0; j < 6; j++)
                    if (nx + d2[j][0] == x && ny + d2[j][1] == y) back = 1;
                CHECK(back, "adjacency symmetric");
            }
            CHECK(deg >= 2 && deg <= 6, "cell degree 2..6");
            total_deg += deg;
        }
    }
    CHECK(total_deg % 2 == 0, "total degree even");
    /* 已知邻接对(手工推演) */
    {
        int nx, ny;
        /* (0,0) 偶数行: 仅右 + 下斜 */
        nx = 0 + hm_d[0][0][0]; ny = 0 + hm_d[0][0][1];
        CHECK(nx == -1 && ny == 0, "(0,0) W out");
        nx = 0 + hm_d[0][1][0]; ny = 0 + hm_d[0][1][1];
        CHECK(nx == 1 && ny == 0, "(0,0) E = (1,0)");
        nx = 0 + hm_d[0][3][0]; ny = 0 + hm_d[0][3][1];
        CHECK(nx == 0 && ny == 1, "(0,0) SE = (0,1)");
        nx = 0 + hm_d[0][5][0]; ny = 0 + hm_d[0][5][1];
        CHECK(nx == -1 && ny == 1, "(0,0) SW out");
        /* (1,1) 奇数行: 斜邻在右 */
        nx = 1 + hm_d[1][4][0]; ny = 1 + hm_d[1][4][1];
        CHECK(nx == 2 && ny == 0, "(1,1) NE = (2,0)");
        nx = 1 + hm_d[1][5][0]; ny = 1 + hm_d[1][5][1];
        CHECK(nx == 2 && ny == 2, "(1,1) SE = (2,2)");
        nx = 1 + hm_d[1][2][0]; ny = 1 + hm_d[1][2][1];
        CHECK(nx == 1 && ny == 0, "(1,1) N = (1,0)");
        /* (7,5) 角落: 仅左 + 上斜 */
        nx = 7 + hm_d[1][0][0]; ny = 5 + hm_d[1][0][1];
        CHECK(nx == 6 && ny == 5, "(7,5) W = (6,5)");
        nx = 7 + hm_d[1][4][0]; ny = 5 + hm_d[1][4][1];
        CHECK(nx == 8 && ny == 4, "(7,5) NE out");
        nx = 7 + hm_d[1][2][0]; ny = 5 + hm_d[1][2][1];
        CHECK(nx == 7 && ny == 4, "(7,5) N = (7,4)");
    }
}

/* 布雷: 数量 / 首击区避雷 / 数字一致性 */
static void test_plant(void) {
    rng_seed(&hm_rng, 42);
    hm_plant(3, 2);
    int mines = 0;
    for (int y = 0; y < HM_ROWS; y++)
        for (int x = 0; x < HM_COLS; x++)
            if (hm_b[y][x] == HM_MINE) mines++;
    CHECK(mines == HM_MINES, "exactly 15 mines planted");
    for (int y = 1; y <= 3; y++)
        for (int x = 2; x <= 4; x++)
            CHECK(hm_b[y][x] != HM_MINE, "first-click zone mine-free");
    int bad = 0;
    for (int y = 0; y < HM_ROWS; y++)
        for (int x = 0; x < HM_COLS; x++) {
            if (hm_b[y][x] == HM_MINE) continue;
            if (hm_b[y][x] != hm_adj_mines(x, y)) bad++;
        }
    CHECK(bad == 0, "all neighbor counts match board");
}

/* 首击流程: 不布雷直接翻 */
static void test_first_click(void) {
    board_reset();
    rng_seed(&hm_rng, 7);
    hm_cx = 4; hm_cy = 3;
    key_event_t ev = { K_OK, 0, false };
    hexmines_on_key(&ev);
    int mines = 0;
    for (int y = 0; y < HM_ROWS; y++)
        for (int x = 0; x < HM_COLS; x++)
            if (hm_b[y][x] == HM_MINE) mines++;
    CHECK(mines == HM_MINES, "first click plants 15 mines");
    CHECK(!hm_over && !hm_won, "first click never loses");
    CHECK(hm_revealed >= 1, "first click reveals at least 1");
    for (int y = 0; y < HM_ROWS; y++)
        for (int x = 0; x < HM_COLS; x++)
            if (hm_rep[y][x]) CHECK(hm_b[y][x] != HM_MINE, "revealed cells mine-free");
    CHECK(hm_b[4][3] != HM_MINE, "clicked cell not a mine");
    /* 洪水后已翻开的数字应一致 */
    int bad = 0;
    for (int y = 0; y < HM_ROWS; y++)
        for (int x = 0; x < HM_COLS; x++)
            if (hm_rep[y][x] && hm_b[y][x] != hm_adj_mines(x, y)) bad++;
    CHECK(bad == 0, "revealed numbers consistent");
}

/* 胜利: 15 雷, 翻完 33 个非雷格 */
static void test_win(void) {
    board_reset();
    for (int y = 0; y < HM_ROWS; y++)
        for (int x = 0; x < HM_COLS; x++) {
            if (y * HM_COLS + x < HM_MINES) hm_b[y][x] = HM_MINE;
            else hm_rep[y][x] = true;
        }
    int last_x = HM_COLS - 1, last_y = HM_ROWS - 1;
    CHECK(hm_b[last_y][last_x] != HM_MINE, "last cell is safe");
    hm_rep[last_y][last_x] = false;
    hm_revealed = HM_TOTAL - HM_MINES - 1;   /* 32: 差最后一格 */
    CHECK(hm_revealed == 32, "setup: 32 revealed");
    hm_cx = (uint8_t)last_x; hm_cy = (uint8_t)last_y;
    key_event_t ev = { K_OK, 0, false };
    hexmines_on_key(&ev);
    CHECK(hm_won && !hm_over, "last safe cell -> WIN");
    CHECK(hm_revealed == HM_TOTAL - HM_MINES, "revealed == 33");
}

/* 失败: 踩雷 -> 全部雷亮出 */
static void test_lose(void) {
    board_reset();
    hm_rep[0][0] = true;                /* 让 revealed 非 0, 跳过布雷 */
    hm_revealed = 1;
    hm_b[2][2] = HM_MINE;
    hm_b[5][6] = HM_MINE;
    hm_cx = 2; hm_cy = 2;
    key_event_t ev = { K_OK, 0, false };
    hexmines_on_key(&ev);
    CHECK(hm_over && !hm_won, "mine click -> FAIL");
    CHECK(hm_rep[2][2] && hm_rep[5][6], "all mines revealed");
    /* 旗格挡雷: 插旗的雷再翻不应爆 */
    board_reset();
    hm_rep[0][0] = true;
    hm_revealed = 1;
    hm_b[3][3] = HM_MINE;
    hm_flag[3][3] = true;
    hm_flags = 1;
    hm_cx = 3; hm_cy = 3;
    hexmines_on_key(&ev);
    CHECK(!hm_over && hm_revealed == 1, "flag protects from reveal");
}

/* 旗: F/DEL 切换, 计数, 剩余雷数 */
static void test_flag(void) {
    board_reset();
    hm_cx = 1; hm_cy = 1;
    key_event_t ev;
    ev.ch = 0; ev.is_repeat = false;
    ev.key = K_CHAR; ev.ch = 'f';
    hexmines_on_key(&ev);
    CHECK(hm_flag[1][1] && hm_flags == 1, "F plants flag");
    hexmines_on_key(&ev);
    CHECK(!hm_flag[1][1] && hm_flags == 0, "F removes flag");
    ev.key = K_DEL; ev.ch = 0;
    hexmines_on_key(&ev);
    CHECK(hm_flag[1][1] && hm_flags == 1, "DEL toggles flag");
    ev.key = K_DEL;
    hexmines_on_key(&ev);
    CHECK(hm_flags == 0, "DEL toggles flag off");
    /* 未翻开格才能插旗 */
    hm_rep[2][2] = true;
    hm_cx = 2; hm_cy = 2;
    ev.key = K_CHAR; ev.ch = 'f';
    hexmines_on_key(&ev);
    CHECK(!hm_flag[2][2], "revealed cell cannot be flagged");
}

/* 光标移动: 边界钳制 + WASD + 重复 */
static void test_move(void) {
    board_reset();
    hm_cx = 0; hm_cy = 0;
    key_event_t ev = { K_LEFT, 0, false };
    hexmines_on_key(&ev);
    CHECK(hm_cx == 0 && hm_cy == 0, "LEFT clamped at col 0");
    ev.key = K_UP;
    hexmines_on_key(&ev);
    CHECK(hm_cx == 0 && hm_cy == 0, "UP clamped at row 0");
    ev.key = K_RIGHT;
    hexmines_on_key(&ev);
    ev.key = K_DOWN;
    hexmines_on_key(&ev);
    CHECK(hm_cx == 1 && hm_cy == 1, "RIGHT/DOWN move to (1,1)");
    ev.key = K_CHAR; ev.ch = 'd';
    hexmines_on_key(&ev);
    CHECK(hm_cx == 2, "D moves right");
    ev.key = K_CHAR; ev.ch = 'w';
    hexmines_on_key(&ev);
    CHECK(hm_cy == 0, "W moves up");
    /* 右下角钳制 */
    hm_cx = HM_COLS - 1; hm_cy = HM_ROWS - 1;
    ev.key = K_RIGHT;
    hexmines_on_key(&ev);
    ev.key = K_DOWN;
    hexmines_on_key(&ev);
    CHECK(hm_cx == 7 && hm_cy == 5, "corner clamps");
    /* 方向重复可响应 */
    ev.key = K_LEFT; ev.is_repeat = true;
    hexmines_on_key(&ev);
    CHECK(hm_cx == 6, "repeat LEFT moves");
}

/* 按键过滤: repeat 确认键忽略 / Q 退出 / N 新局 */
static void test_keys(void) {
    board_reset();
    hm_cx = 4; hm_cy = 3;
    key_event_t ev = { K_OK, 0, true };     /* repeat OK */
    hexmines_on_key(&ev);
    CHECK(hm_revealed == 0, "repeat OK ignored (no plant, no reveal)");
    ev.is_repeat = false;
    ev.key = K_CHAR; ev.ch = 'f'; ev.is_repeat = true;
    hexmines_on_key(&ev);
    CHECK(hm_flags == 0, "repeat F ignored");
    s_exit_request = false;
    ev.is_repeat = false;
    ev.key = K_QUIT;
    hexmines_on_key(&ev);
    CHECK(s_exit_request, "Q requests exit");
    /* 终局: OK 重开, BACK 退出 */
    hm_over = true; hm_won = false;
    s_exit_request = false;
    ev.key = K_OK;
    hexmines_on_key(&ev);
    CHECK(!hm_over && !s_exit_request, "over: OK restarts");
    hm_over = true;
    ev.key = K_BACK;
    hexmines_on_key(&ev);
    CHECK(s_exit_request, "over: BACK quits");
    /* 字母键在终局忽略(除 n) */
    hm_over = true;
    ev.key = K_CHAR; ev.ch = 'a';
    hexmines_on_key(&ev);
    CHECK(hm_over, "over: 'a' ignored");
}

int main(void) {
    test_geom();
    test_plant();
    test_first_click();
    test_win();
    test_lose();
    test_flag();
    test_move();
    test_keys();
    if (s_fail) { printf("TOTAL FAIL: %d\n", s_fail); return 1; }
    printf("ALL PASS\n");
    return 0;
}
