/* 十字跳棋(Peg Solitaire) — 英式 33 孔十字棋盘, 7x7 格 19px(133x133 居中于
 * HUD 下方): 跳吃规则, 目标剩 1 颗(中心收尾为 PERFECT), 无合法跳则 STUCK
 * 操作: 方向/WASD 移光标; OK 选中(整格反白)/跳到空位; BACK 取消/暂停
 * 输入驱动; 移动=快刷; 开局/结束=全刷 */
#include "game.h"
#include "../config.h"
#include "../gfx/canvas.h"
#include "../gfx/font.h"
#include "../platform/display.h"
#include "../platform/input.h"
#include "../platform/audio.h"
#include "../platform/led.h"

#define PG_N 7
#define PG_CELL 19
#define PG_BOARD (PG_N * PG_CELL)                             /* 133 */
#define PG_OX ((CCG_W - PG_BOARD) / 2)                        /* 81 水平居中 */
#define PG_OY ((CCG_H - CCG_HUD_H - PG_BOARD) / 2 + CCG_HUD_H) /* 17 垂直居中 */

void peg_enter(void);
void peg_render(void);

static uint8_t pg_board[PG_N][PG_N];  /* 1=棋子 0=空 */
static int pg_cx, pg_cy;              /* 光标(格坐标) */
static bool pg_sel;                   /* 已选中棋子(选中格=pg_selx/pg_sely) */
static int pg_selx, pg_sely;
static int pg_remain;                 /* 剩余棋子数 */
static bool pg_over, pg_stuck, pg_perfect, pg_over_full;

/* 英式十字: 33 孔 = 7x7 去四角(上下两行仅中间 3 孔); 越界一律非孔(防越界读) */
static bool pg_is_hole(int x, int y) {
    if (x < 0 || x >= PG_N || y < 0 || y >= PG_N) return false;
    return (x >= 2 && x <= 4) || (y >= 2 && y <= 4);
}

static void pg_new(void) {
    for (int y = 0; y < PG_N; y++)
        for (int x = 0; x < PG_N; x++)
            pg_board[y][x] = (uint8_t)pg_is_hole(x, y);
    pg_board[3][3] = 0;               /* 中心孔空, 其余 32 孔有子 */
    pg_remain = 32;
    pg_cx = 3;
    pg_cy = 3;
    pg_sel = false;
    pg_selx = 3;
    pg_sely = 3;
    pg_over = false;
    pg_stuck = false;
    pg_perfect = false;
    pg_over_full = false;
}

/* (sx,sy)->(tx,ty) 跳吃合法: 正交 2 格、源有子、中间有被跳子、目标空孔 */
static bool pg_valid_jump(int sx, int sy, int tx, int ty) {
    int ax = tx - sx, ay = ty - sy;
    if (ax < 0) ax = -ax;
    if (ay < 0) ay = -ay;
    if (!((ax == 2 && ay == 0) || (ax == 0 && ay == 2))) return false;
    if (!pg_is_hole(tx, ty)) return false;      /* 目标必须为空孔(越界也拒) */
    if (!pg_board[sy][sx] || pg_board[ty][tx]) return false;
    return pg_board[(sy + ty) / 2][(sx + tx) / 2] != 0;
}

static bool pg_has_move(void) {
    static const int dx4[4] = { 2, -2, 0, 0 };
    static const int dy4[4] = { 0, 0, 2, -2 };
    for (int y = 0; y < PG_N; y++)
        for (int x = 0; x < PG_N; x++) {
            if (!pg_board[y][x]) continue;
            for (int d = 0; d < 4; d++)
                if (pg_valid_jump(x, y, x + dx4[d], y + dy4[d]))
                    return true;
        }
    return false;
}

/* 每跳后更新结局: 剩 1 胜(中心=PERFECT); 无合法跳=STUCK */
static void pg_check_end(void) {
    if (pg_remain == 1) {
        pg_over = true;
        pg_perfect = (pg_board[3][3] != 0);
        audio_win();                   /* 收尾剩 1 颗 */
        led_fx_set(LED_FX_WIN);
    } else if (!pg_has_move()) {
        pg_over = true;
        pg_stuck = true;
        audio_lose();                  /* 无合法跳 */
        led_fx_set(LED_FX_LOSE);
    }
}

static void pg_do_jump(int sx, int sy, int tx, int ty) {
    int mx = (sx + tx) / 2, my = (sy + ty) / 2;
    pg_board[sy][sx] = 0;
    pg_board[my][mx] = 0;
    pg_board[ty][tx] = 1;
    pg_remain--;
    pg_cx = tx;                       /* 光标跟随落点 */
    pg_cy = ty;
    pg_sel = false;
    pg_check_end();
    if (!pg_over) audio_clear();      /* 跳吃成功(未终局) */
}

/* 实心圆(棋子, 半径 7) */
static void pg_draw_peg(int cx, int cy) {
    for (int dy = -7; dy <= 7; dy++)
        for (int dx = -7; dx <= 7; dx++)
            if (dx * dx + dy * dy <= 49) fb_pixel(cx + dx, cy + dy, true);
}

/* 小圆点(空孔标记, 半径 3) */
static void pg_draw_dot(int cx, int cy) {
    for (int dy = -3; dy <= 3; dy++)
        for (int dx = -3; dx <= 3; dx++)
            if (dx * dx + dy * dy <= 9) fb_pixel(cx + dx, cy + dy, true);
}

void peg_render(void) {
    fb_clear(false);
    /* 顶栏 HUD: 左标题, 右 REMAIN n, 黑字白底 */
    fb_text(2, 2, "PEG", true);
    {
        char buf[12];
        int n = 0;
        static const char label[] = "REMAIN ";
        while (label[n]) { buf[n] = label[n]; n++; }
        if (pg_remain >= 10) buf[n++] = (char)('0' + pg_remain / 10);
        buf[n++] = (char)('0' + pg_remain % 10);
        buf[n] = 0;
        fb_text(CCG_W - 4 - text_width(buf), 2, buf, true);
    }
    fb_hline(0, CCG_HUD_H - 1, CCG_W, true);
    /* 棋盘孔位: 棋子=实心圆, 空孔=小圆点, 选中=整格反白+白子 */
    for (int y = 0; y < PG_N; y++)
        for (int x = 0; x < PG_N; x++) {
            if (!pg_is_hole(x, y)) continue;
            int cx = PG_OX + x * PG_CELL + PG_CELL / 2;
            int cy = PG_OY + y * PG_CELL + PG_CELL / 2;
            if (pg_board[y][x]) {
                if (pg_sel && x == pg_selx && y == pg_sely) {
                    fb_fill_rect(PG_OX + x * PG_CELL, PG_OY + y * PG_CELL,
                                 PG_CELL, PG_CELL, true);
                    for (int dy = -7; dy <= 7; dy++)
                        for (int dx = -7; dx <= 7; dx++)
                            if (dx * dx + dy * dy <= 49)
                                fb_pixel(cx + dx, cy + dy, false);
                } else {
                    pg_draw_peg(cx, cy);
                }
            } else {
                pg_draw_dot(cx, cy);
            }
        }
    /* 光标: 反色边框(选中格白边/其余黑边), 在所有格之后绘制 */
    {
        int x = PG_OX + pg_cx * PG_CELL;
        int y = PG_OY + pg_cy * PG_CELL;
        bool black = !(pg_sel && pg_cx == pg_selx && pg_cy == pg_sely);
        fb_stroke_rect_thick(x, y, PG_CELL, PG_CELL, 2, black);
    }
    /* 结束: HUD 区左上结果 + 右上操作提示, 强制全刷一次 */
    if (pg_over) {
        fb_fill_rect(0, 0, CCG_W, CCG_HUD_H - 1, false);
        const char *res = pg_stuck ? "STUCK" :
                          pg_perfect ? "PERFECT!" : "SOLVED!";
        fb_text(2, 2, res, true);
        fb_text(CCG_W - 2 - text_width("OK/N:RETRY BACK:QUIT"), 2,
                "OK/N:RETRY BACK:QUIT", true);
        if (!pg_over_full) { pg_over_full = true; disp_force_full(); }
    }
    disp_fast();
}

/* 暂停菜单: 返回 false 时退出回主菜单; RESTART 则重开 */
static void pg_pause(void) {
    pause_sel_t sel;
    if (ui_pause_run(&sel)) {
        if (sel == PAUSE_RESTART) peg_enter();
    } else {
        s_exit_request = true;
    }
}

void peg_on_key(const key_event_t *ev) {
    /* 重复事件只响应方向键(确认/字母必须忽略) */
    if (ev->is_repeat && ev->key != K_LEFT && ev->key != K_RIGHT &&
        ev->key != K_UP && ev->key != K_DOWN)
        return;
    if (pg_over) {
        if (ev->key == K_OK || (ev->key == K_CHAR && ev->ch == 'n'))
            peg_enter();
        else if (ev->key == K_BACK || ev->key == K_QUIT)
            s_exit_request = true;
        return;
    }
    switch (ev->key) {
    case K_LEFT: if (pg_cx > 0) pg_cx--; break;
    case K_RIGHT: if (pg_cx < PG_N - 1) pg_cx++; break;
    case K_UP: if (pg_cy > 0) pg_cy--; break;
    case K_DOWN: if (pg_cy < PG_N - 1) pg_cy++; break;
    case K_OK:
        if (!pg_sel) {
            if (pg_board[pg_cy][pg_cx]) {
                pg_sel = true;
                pg_selx = pg_cx;
                pg_sely = pg_cy;
                audio_select();        /* 选中棋子 */
            }
        } else if (pg_valid_jump(pg_selx, pg_sely, pg_cx, pg_cy)) {
            pg_do_jump(pg_selx, pg_sely, pg_cx, pg_cy);
        } else if (pg_board[pg_cy][pg_cx]) {
            pg_selx = pg_cx;          /* 换选光标处棋子 */
            pg_sely = pg_cy;
            audio_select();            /* 换选 */
        }
        /* 其余: 目标不可达, 无操作 */
        break;
    case K_CHAR:
        if (ev->ch == 'a' && pg_cx > 0) pg_cx--;
        else if (ev->ch == 'd' && pg_cx < PG_N - 1) pg_cx++;
        else if (ev->ch == 'w' && pg_cy > 0) pg_cy--;
        else if (ev->ch == 's' && pg_cy < PG_N - 1) pg_cy++;
        else if (ev->ch == 'n') peg_enter();
        break;
    case K_BACK:
        if (pg_sel) pg_sel = false;   /* 取消选中 */
        else pg_pause();
        break;
    case K_PAUSE: pg_pause(); break;
    case K_QUIT: s_exit_request = true; break;
    default: break;
    }
}

void peg_tick(uint64_t now) { (void)now; }
void peg_exit(void) {}

void peg_enter(void) {
    pg_new();
    peg_render();
    disp_full();
}
