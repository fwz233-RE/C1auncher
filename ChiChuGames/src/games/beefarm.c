/* 蜜蜂农场 BEE FARM — 7x5 格子养殖规划
 * 回合制经营: 种花/搬蜂箱各结束一回合, 25 回合内攒 30 蜜
 * - 每 3 回合(第 3/6/9/.../24 回合末)每个蜂箱产蜜 = min(8 邻花数, 5), 存入蜂箱
 * - 花 5 回合后枯萎需重种(第 6 次回合末移除)
 * - 光标 OK/SPACE: 空地种花(耗回合) / 蜂箱有存蜜则自动收蜜(不耗回合)
 * - B: 选中蜂箱后移到空格(耗回合); 每回合 15 秒倒计时, 超时自动过回合
 * 注: 设计草稿写 tick 100ms, 但平台实测面板处理 ~700ms/帧、主循环
 * tick 即触发 render, 100ms 会以 10fps 写帧导致跳格/丢帧(platform-spec
 * 8.5: 游戏 tick 必须 >=700ms), 故按秒倒计时用 1000ms tick。
 * 收蜜不耗回合: 若收蜜也要占一回合, 25 回合内既要保花又要 3-4 次
 * 上蜂箱收蜜, 最优打法也只能收 ~27 蜜(模拟验证), 目标 30 几乎不可达。
 */
#include "game.h"
#include "../config.h"
#include "../gfx/canvas.h"
#include "../gfx/font.h"
#include "../platform/audio.h"
#include "../platform/display.h"
#include "../platform/input.h"
#include "../platform/led.h"
#include "../platform/time.h"
#include "../rng.h"

#define BF_COLS 7
#define BF_ROWS 5
#define BF_CELL 26
#define BF_X0 ((CCG_W - BF_COLS * BF_CELL) / 2)            /* 57 */
#define BF_Y0 (CCG_HUD_H + ((CCG_H - CCG_HUD_H) - BF_ROWS * BF_CELL) / 2)  /* 19 */
#define BF_TURNS 25
#define BF_GOAL 30
#define BF_PRODUCE_EVERY 3
#define BF_WILT_AGE 6          /* 花龄达到 6 时枯萎(存活 5 回合末) */
#define BF_TURN_MS 15000u      /* 每回合倒计时 */
#define BF_TICK_MS 1000u       /* tick 间隔 */
#define BF_BAR_X (BF_X0 + BF_COLS * BF_CELL + 22)          /* 右侧倒计时条 */
#define BF_BAR_W 12

enum { BF_EMPTY = 0, BF_FLOWER = 1, BF_HIVE = 2 };
enum { BF_PLAY = 0, BF_WIN = 1, BF_LOSE = 2 };

static uint8_t bf_grid[BF_ROWS][BF_COLS];   /* 0=空 1=花 2=蜂箱 */
static uint8_t bf_age[BF_ROWS][BF_COLS];    /* 花龄 */
static uint8_t bf_stock[BF_ROWS][BF_COLS];  /* 蜂箱存蜜 */
static int bf_turn;                          /* 已过回合数 0..25 */
static int bf_honey;                         /* 已收进仓库的蜜 */
static int bf_cx, bf_cy;                     /* 光标 */
static int bf_sel;                           /* 选中蜂箱 cell idx, -1=无 */
static uint32_t bf_ms;                       /* 本回合倒计时剩余 ms */
static int bf_over;                          /* BF_PLAY/WIN/LOSE */
static bool bf_over_full;
static rng_t bf_rng;

/* 花: 7x7 圆环 + 中心点, 3 倍放大 */
static const uint8_t bf_flower_bits[7] = {
    0x3E, 0x41, 0x41, 0x49, 0x41, 0x41, 0x3E
};

void beefarm_render(void);

static void bf_end_turn(void);

static void bf_reset(void) {
    int y, x;
    for (y = 0; y < BF_ROWS; y++)
        for (x = 0; x < BF_COLS; x++) {
            bf_grid[y][x] = BF_EMPTY;
            bf_age[y][x] = 0;
            bf_stock[y][x] = 0;
        }
    bf_grid[2][1] = BF_HIVE;     /* 起始双蜂箱, 中央一行 */
    bf_grid[2][5] = BF_HIVE;
    bf_turn = 0;
    bf_honey = 0;
    bf_cx = 3;
    bf_cy = 2;
    bf_sel = -1;
    bf_ms = BF_TURN_MS;
    bf_over = BF_PLAY;
    bf_over_full = false;
}

static int bf_flowers_around(int x, int y) {
    int n = 0;
    for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++) {
            if (dx == 0 && dy == 0) continue;
            int nx = x + dx, ny = y + dy;
            if (nx < 0 || nx >= BF_COLS || ny < 0 || ny >= BF_ROWS) continue;
            if (bf_grid[ny][nx] == BF_FLOWER) n++;
        }
    return n;
}

static int bf_count_flowers(void) {
    int n = 0;
    for (int y = 0; y < BF_ROWS; y++)
        for (int x = 0; x < BF_COLS; x++)
            if (bf_grid[y][x] == BF_FLOWER) n++;
    return n;
}

static void bf_check_goal(void) {
    if (bf_honey >= BF_GOAL) {
        bf_over = BF_WIN;
        audio_win();
        led_fx_set(LED_FX_WIN);
    }
}

/* 回合结束: 花老化 -> 产蜜 -> 回合计数 -> 胜负判定 */
static void bf_end_turn(void) {
    int y, x;
    for (y = 0; y < BF_ROWS; y++)
        for (x = 0; x < BF_COLS; x++)
            if (bf_grid[y][x] == BF_FLOWER) {
                if (bf_age[y][x] + 1 >= BF_WILT_AGE) {
                    bf_grid[y][x] = BF_EMPTY;
                    bf_age[y][x] = 0;
                } else {
                    bf_age[y][x]++;
                }
            }
    bf_turn++;
    if (bf_turn % BF_PRODUCE_EVERY == 0) {
        for (y = 0; y < BF_ROWS; y++)
            for (x = 0; x < BF_COLS; x++)
                if (bf_grid[y][x] == BF_HIVE) {
                    int n = bf_flowers_around(x, y);
                    if (n > 5) n = 5;
                    bf_stock[y][x] += (uint8_t)n;
                }
    }
    bf_ms = BF_TURN_MS;
    bf_check_goal();
    if (bf_over == BF_PLAY && bf_turn >= BF_TURNS) {
        bf_over = BF_LOSE;
        audio_lose();
        led_fx_set(LED_FX_LOSE);
    }
}

void beefarm_enter(void) {
    rng_seed(&bf_rng, now_ms() ^ 0xFA0BEEu);
    bf_reset();
    beefarm_render();
    disp_full();
}

void beefarm_exit(void) {}

void beefarm_tick(uint64_t now) {
    (void)now;
    if (bf_over != BF_PLAY) return;
    if (bf_ms <= BF_TICK_MS) bf_end_turn();
    else bf_ms -= BF_TICK_MS;
}

/* ---- 绘制 ---- */

static void bf_draw_flower(int x, int y) {
    int bx = BF_X0 + x * BF_CELL, by = BF_Y0 + y * BF_CELL;
    for (int r = 0; r < 7; r++)
        for (int c = 0; c < 7; c++)
            if (bf_flower_bits[r] & (1u << c))
                fb_fill_rect(bx + 2 + c * 3, by + 2 + r * 3, 3, 3, true);
    /* 花龄点: 1..4 颗 2x2 点, 满 4 颗即将枯萎 */
    int a = bf_age[y][x];
    for (int i = 0; i < a && i < 4; i++)
        fb_fill_rect(bx + 4 + i * 5, by + 23, 2, 2, true);
}

static void bf_draw_hive(int x, int y, bool sel) {
    int bx = BF_X0 + x * BF_CELL, by = BF_Y0 + y * BF_CELL;
    if (sel) {
        /* 选中: 整格反黑 + 白框 */
        fb_fill_rect(bx, by, BF_CELL, BF_CELL, true);
        fb_stroke_rect_thick(bx + 2, by + 2, 22, 22, 2, false);
    } else {
        /* 黑箱 + 两道白缝 */
        fb_fill_rect(bx + 2, by + 2, 22, 22, true);
    }
    fb_fill_rect(bx + 4, by + 8, 18, 2, false);
    fb_fill_rect(bx + 4, by + 13, 18, 2, false);
    int st = bf_stock[y][x];
    if (st > 0) {
        /* 存蜜数: 白字小数字, 箱内右下 */
        char buf[8];
        int n = 0;
        if (st >= 10) buf[n++] = (char)('0' + st / 10);
        buf[n++] = (char)('0' + st % 10);
        buf[n] = 0;
        fb_text(bx + 13, by + 16, buf, false);
    }
}

void beefarm_render(void) {
    fb_clear(false);
    /* HUD: 黑字白底; 右侧三组右对齐数值 */
    fb_text(2, 0, "BEE FARM", true);
    {
        char buf[24];
        int n = 0;
        const char *l = "TURN ";
        while (l[n]) { buf[n] = l[n]; n++; }
        buf[n++] = (char)('0' + bf_turn / 10);
        buf[n++] = (char)('0' + bf_turn % 10);
        buf[n++] = '/';
        buf[n++] = (char)('0' + BF_TURNS / 10);
        buf[n++] = (char)('0' + BF_TURNS % 10);
        buf[n] = 0;
        fb_text(150 - text_width(buf), 0, buf, true);
    }
    {
        char buf[24];
        int n = 0;
        const char *l = "FLOWER ";
        while (l[n]) { buf[n] = l[n]; n++; }
        int f = bf_count_flowers();
        buf[n++] = (char)('0' + f / 10);
        buf[n++] = (char)('0' + f % 10);
        buf[n] = 0;
        fb_text(232 - text_width(buf), 0, buf, true);
    }
    {
        char buf[24];
        int n = 0;
        const char *l = "HONEY ";
        while (l[n]) { buf[n] = l[n]; n++; }
        buf[n++] = (char)('0' + bf_honey / 10);
        buf[n++] = (char)('0' + bf_honey % 10);
        buf[n++] = '/';
        buf[n++] = (char)('0' + BF_GOAL / 10);
        buf[n++] = (char)('0' + BF_GOAL % 10);
        buf[n] = 0;
        fb_text(294 - text_width(buf), 0, buf, true);
    }
    fb_hline(0, CCG_HUD_H - 1, CCG_W, true);

    /* 棋盘外框 + 网格 */
    fb_stroke_rect(BF_X0, BF_Y0, BF_COLS * BF_CELL, BF_ROWS * BF_CELL, true);
    for (int i = 1; i < BF_COLS; i++)
        fb_vline(BF_X0 + i * BF_CELL, BF_Y0, BF_ROWS * BF_CELL, true);
    for (int j = 1; j < BF_ROWS; j++)
        fb_hline(BF_X0, BF_Y0 + j * BF_CELL, BF_COLS * BF_CELL, true);

    /* 格子内容 */
    for (int y = 0; y < BF_ROWS; y++)
        for (int x = 0; x < BF_COLS; x++) {
            if (bf_grid[y][x] == BF_FLOWER) bf_draw_flower(x, y);
            else if (bf_grid[y][x] == BF_HIVE)
                bf_draw_hive(x, y, bf_sel == y * BF_COLS + x);
        }

    /* 回合倒计时条: 右侧竖条, 从底端排空 */
    int h = (int)((uint64_t)bf_ms * (BF_ROWS * BF_CELL) / BF_TURN_MS);
    if (h > 0) fb_fill_rect(BF_BAR_X, BF_Y0 + BF_ROWS * BF_CELL - h, BF_BAR_W, h, true);

    /* 光标: 最后画, 白框切断网格线 */
    {
        int bx = BF_X0 + bf_cx * BF_CELL, by = BF_Y0 + bf_cy * BF_CELL;
        fb_stroke_rect_thick(bx + 1, by + 1, BF_CELL - 2, BF_CELL - 2, 1, false);
    }

    if (bf_over != BF_PLAY) {
        fb_fill_rect(0, 0, CCG_W, CCG_HUD_H - 1, false);
        {
            char buf[24];
            int n = 0;
            const char *l = (bf_over == BF_WIN) ? "WIN! HONEY " : "LOSE: HONEY ";
            while (l[n]) { buf[n] = l[n]; n++; }
            if (bf_honey >= 10) buf[n++] = (char)('0' + bf_honey / 10);
            buf[n++] = (char)('0' + bf_honey % 10);
            buf[n] = 0;
            fb_text(2, 2, buf, true);
        }
        fb_text(CCG_W - 2 - text_width("OK/N:RETRY BACK:QUIT"), 2,
                "OK/N:RETRY BACK:QUIT", true);
        if (!bf_over_full) { bf_over_full = true; disp_force_full(); }
    }
}

/* ---- 输入 ---- */

static void bf_ok_action(void) {
    int t = bf_grid[bf_cy][bf_cx];
    if (t == BF_EMPTY) {
        bf_grid[bf_cy][bf_cx] = BF_FLOWER;
        bf_age[bf_cy][bf_cx] = 0;
        bf_end_turn();
        if (bf_over == BF_PLAY) audio_select();   /* 种花 */
    } else if (t == BF_HIVE && bf_stock[bf_cy][bf_cx] > 0) {
        /* 自动收蜜: 不耗回合, 立即判胜 */
        bf_honey += bf_stock[bf_cy][bf_cx];
        bf_stock[bf_cy][bf_cx] = 0;
        bf_check_goal();
        if (bf_over == BF_PLAY) audio_clear();    /* 收蜜得分 */
    }
    /* 花上/无存蜜蜂箱 OK: 无动作, 不耗回合 */
}

static void bf_b_action(void) {
    if (bf_sel < 0) {
        if (bf_grid[bf_cy][bf_cx] == BF_HIVE)
            bf_sel = bf_cy * BF_COLS + bf_cx;   /* 选中 */
    } else if (bf_grid[bf_cy][bf_cx] == BF_EMPTY) {
        int sy = bf_sel / BF_COLS, sx = bf_sel % BF_COLS;
        bf_grid[bf_cy][bf_cx] = BF_HIVE;
        bf_stock[bf_cy][bf_cx] = bf_stock[sy][sx];
        bf_stock[sy][sx] = 0;
        bf_grid[sy][sx] = BF_EMPTY;
        bf_sel = -1;
        bf_end_turn();                          /* 搬家耗一回合 */
    } else {
        bf_sel = -1;                            /* 取消选择 */
    }
}

void beefarm_on_key(const key_event_t *ev) {
    if (bf_over != BF_PLAY) {
        if (ev->is_repeat) return;
        if (ev->key == K_OK || ev->key == K_SPACE ||
            (ev->key == K_CHAR && (ev->ch == 'n' || ev->ch == 'r')))
            beefarm_enter();
        else if (ev->key == K_BACK || ev->key == K_QUIT)
            s_exit_request = true;
        return;
    }
    switch (ev->key) {
    case K_UP:
        if (bf_cy > 0) bf_cy--;
        break;
    case K_DOWN:
        if (bf_cy < BF_ROWS - 1) bf_cy++;
        break;
    case K_LEFT:
        if (bf_cx > 0) bf_cx--;
        break;
    case K_RIGHT:
        if (bf_cx < BF_COLS - 1) bf_cx++;
        break;
    case K_OK:
    case K_SPACE:
        if (!ev->is_repeat) bf_ok_action();
        break;
    case K_DEL:
        if (!ev->is_repeat) bf_sel = -1;
        break;
    case K_BACK:
    case K_PAUSE:
        if (!ev->is_repeat) {
            pause_sel_t sel = PAUSE_RESUME;
            if (ui_pause_run(&sel)) {
                if (sel == PAUSE_RESTART) beefarm_enter();
            } else {
                s_exit_request = true;
            }
        }
        break;
    case K_QUIT:
        if (!ev->is_repeat) s_exit_request = true;
        break;
    case K_CHAR:
        if (ev->ch == 'w' && bf_cy > 0) bf_cy--;
        else if (ev->ch == 's' && bf_cy < BF_ROWS - 1) bf_cy++;
        else if (ev->ch == 'a' && bf_cx > 0) bf_cx--;
        else if (ev->ch == 'd' && bf_cx < BF_COLS - 1) bf_cx++;
        else if (ev->ch == 'b' && !ev->is_repeat) bf_b_action();
        else if ((ev->ch == 'n' || ev->ch == 'r') && !ev->is_repeat)
            beefarm_enter();
        break;
    default:
        break;
    }
}
