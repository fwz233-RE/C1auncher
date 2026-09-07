/* 扫雷 — 15x9 格 18x15, 15 雷; 首击安全; 输入驱动
 * 揭示/插旗=快刷; 开局/胜负=全刷 */
#include "game.h"
#include "../config.h"
#include "../gfx/canvas.h"
#include "../gfx/font.h"
#include "../platform/display.h"
#include "../platform/input.h"
#include "../platform/audio.h"
#include "../platform/led.h"
#include "../rng.h"
#include "../platform/time.h"
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>

#define MS_W 19
#define MS_H 9
#define MS_CELL_X 15
#define MS_CELL_Y 15
#define MS_OX ((CCG_W - MS_W * MS_CELL_X) / 2)
#define MS_OY (CCG_HUD_H + 1)   /* 正方形格 15x15, 225x135 铺满 */
#define MS_MINES 19   /* 171 格约 11% */
#define MS_CELL 9            /* 雷标记 */
#define MS_EMPTY 0

void mines_render(void);
static void self_check(void);

static uint8_t ms_b[MS_H][MS_W];        /* 0-8 邻雷数, 9=雷 */
static bool ms_rep[MS_H][MS_W];         /* 已翻开 */
static bool ms_flag[MS_H][MS_W];        /* 插旗 */
static uint8_t ms_cx, ms_cy;
static bool ms_over, ms_won, ms_over_full;
static uint32_t ms_revealed, ms_flags;
static rng_t ms_rng;

static uint8_t count_adj(int x, int y) {
    int n = 0;
    for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++) {
            if (!dx && !dy) continue;
            int nx = x + dx, ny = y + dy;
            if (nx >= 0 && nx < MS_W && ny >= 0 && ny < MS_H && ms_b[ny][nx] == MS_CELL)
                n++;
        }
    return (uint8_t)n;
}

/* 首击后布雷: 避开 (ex,ey) 及其邻格 */
static void plant_mines(int ex, int ey) {
    for (int y = 0; y < MS_H; y++)
        for (int x = 0; x < MS_W; x++) ms_b[y][x] = MS_EMPTY;
    int planted = 0;
    while (planted < MS_MINES) {
        int x = (int)rng_range(&ms_rng, MS_W);
        int y = (int)rng_range(&ms_rng, MS_H);
        if (ms_b[y][x] == MS_CELL) continue;
        if (x >= ex - 1 && x <= ex + 1 && y >= ey - 1 && y <= ey + 1) continue;
        ms_b[y][x] = MS_CELL;
        planted++;
    }
    for (int y = 0; y < MS_H; y++)
        for (int x = 0; x < MS_W; x++)
            if (ms_b[y][x] != MS_CELL) ms_b[y][x] = count_adj(x, y);
}

static void flood_reveal(int x, int y) {
    if (x < 0 || x >= MS_W || y < 0 || y >= MS_H) return;
    if (ms_rep[y][x] || ms_flag[y][x]) return;
    ms_rep[y][x] = true;
    ms_revealed++;
    if (ms_b[y][x] == MS_EMPTY) {
        for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++)
                if (dx || dy) flood_reveal(x + dx, y + dy);
    }
}

static void reveal_cell(void) {
    if (ms_rep[ms_cy][ms_cx] || ms_flag[ms_cy][ms_cx]) { audio_error(); return; }
    if (ms_revealed == 0) plant_mines((int)ms_cx, (int)ms_cy);   /* 首击布雷 */
    if (ms_b[ms_cy][ms_cx] == MS_CELL) {
        ms_over = true;
        for (int y = 0; y < MS_H; y++)
            for (int x = 0; x < MS_W; x++)
                if (ms_b[y][x] == MS_CELL) ms_rep[y][x] = true;  /* 亮出全部雷 */
        audio_lose();
        led_fx_set(LED_FX_LOSE);
        return;
    }
    flood_reveal((int)ms_cx, (int)ms_cy);
    audio_clear();
    if (ms_revealed == (uint32_t)(MS_W * MS_H - MS_MINES)) {
        ms_won = true;
        audio_win();
        led_fx_set(LED_FX_WIN);
    }
}

static void toggle_flag(void) {
    if (ms_rep[ms_cy][ms_cx]) return;
    ms_flag[ms_cy][ms_cx] = !ms_flag[ms_cy][ms_cx];
    ms_flags += ms_flag[ms_cy][ms_cx] ? 1 : -1;
}

void mines_enter(void) {
    rng_seed(&ms_rng, now_ms() ^ 0x51E);
    for (int y = 0; y < MS_H; y++)
        for (int x = 0; x < MS_W; x++) {
            ms_b[y][x] = MS_EMPTY;
            ms_rep[y][x] = false;
            ms_flag[y][x] = false;
        }
    ms_cx = MS_W / 2;
    ms_cy = MS_H / 2;
    ms_over = false;
    ms_won = false;
    ms_over_full = false;
    ms_revealed = 0;
    ms_flags = 0;
    mines_render();
    disp_full();
}

void mines_render(void) {
    fb_clear(false);
    for (int y = 0; y < MS_H; y++) {
        for (int x = 0; x < MS_W; x++) {
            int cx = MS_OX + x * MS_CELL_X;
            int cy = MS_OY + y * MS_CELL_Y;
            if (ms_rep[y][x]) {
                /* 已翻开: 白底细框 + 数字/雷 */
                fb_stroke_rect(cx, cy, MS_CELL_X, MS_CELL_Y, true);
                if (ms_b[y][x] == MS_CELL) {
                    fb_symbol(cx + 5, cy + 4, CG_MINE, true);
                } else if (ms_b[y][x] > 0) {
                    char d[2] = { (char)('0' + ms_b[y][x]), 0 };
                    fb_text(cx + (MS_CELL_X - text_width(d)) / 2,
                            cy + (MS_CELL_Y - FONT_H) / 2, d, true);
                }
            } else if (ms_flag[y][x]) {
                /* 插旗: 黑底 + 旗符号 */
                fb_fill_rect(cx, cy, MS_CELL_X, MS_CELL_Y, true);
                fb_symbol(cx + 5, cy + 4, CG_FLAG, false);
            } else {
                /* 未翻开: 黑底 */
                fb_fill_rect(cx, cy, MS_CELL_X, MS_CELL_Y, true);
            }
        }
    }
    /* 光标边框最后画, 避免被相邻格子覆盖(四周对称)
     * 双重对比: 外圈 1px 反色 + 内圈 2px 同色, 任何背景下可见 */
    {
        int cx = MS_OX + ms_cx * MS_CELL_X;
        int cy = MS_OY + ms_cy * MS_CELL_Y;
        bool cell_white = ms_rep[ms_cy][ms_cx];
        fb_stroke_rect(cx - 2, cy - 2, MS_CELL_X + 4, MS_CELL_Y + 4, !cell_white);
        fb_stroke_rect_thick(cx - 1, cy - 1, MS_CELL_X + 2, MS_CELL_Y + 2, 2, cell_white);
    }
    /* HUD: 标题 + 雷数/旗数 */
    char buf[24];
    uint32_t v = MS_MINES - ms_flags;
    unsigned i = 0;
    if (v == 0) { buf[i++] = '0'; }
    while (v && i < 20) { buf[i++] = (char)('0' + v % 10); v /= 10; }
    char rev[22];
    unsigned len = i;
    for (unsigned j = 0; j < len; j++) rev[j] = buf[len - 1 - j];
    rev[len] = 0;
    fb_text(0, 0, "MINES", true);
    char full[28];
    const char *lbl = " LEFT:";
    unsigned n = 4;
    while (lbl[n - 4]) { full[n] = lbl[n - 4]; n++; }
    for (unsigned j = 0; j < len; j++) full[n++] = rev[j];
    full[n] = 0;
    int x = CCG_W - text_width(full) - 4;
    fb_text(x, 0, full, true);
    fb_hline(0, CCG_HUD_H - 1, CCG_W, true);
    if (ms_over || ms_won) {
        fb_fill_rect(0, 0, CCG_W, CCG_HUD_H - 1, false);
        fb_text(2, 2, ms_won ? "YOU WIN!" : "GAME OVER", true);
        fb_text(CCG_W - 2 - text_width("OK/N:RETRY BACK:QUIT"), 2,
                "OK/N:RETRY BACK:QUIT", true);
        if (!ms_over_full) { ms_over_full = true; disp_force_full(); }
    }
}

void mines_on_key(const key_event_t *ev) {
    if (ev->is_repeat) return;
    if (ms_over || ms_won) {
        if (ev->key == K_OK || (ev->key == K_CHAR && ev->ch == 'n'))
            mines_enter();
        else if (ev->key == K_BACK || ev->key == K_QUIT)
            s_exit_request = true;
        return;
    }
    switch (ev->key) {
    case K_LEFT: if (ms_cx > 0) ms_cx--; break;
    case K_RIGHT: if (ms_cx < MS_W - 1) ms_cx++; break;
    case K_UP: if (ms_cy > 0) ms_cy--; break;
    case K_DOWN: if (ms_cy < MS_H - 1) ms_cy++; break;
    case K_CHAR:
        if (ev->ch == 'a' && ms_cx > 0) ms_cx--;
        else if (ev->ch == 'd' && ms_cx < MS_W - 1) ms_cx++;
        else if (ev->ch == 'w' && ms_cy > 0) ms_cy--;
        else if (ev->ch == 's' && ms_cy < MS_H - 1) ms_cy++;
        else if (ev->ch == 'n') mines_enter();
        else if (ev->ch == 'f') toggle_flag();
        else if (ev->ch == 'v') self_check();
        break;
    case K_OK:
        reveal_cell();
        break;
    case K_DEL:
        toggle_flag();
        break;
    case K_PAUSE: {
        pause_sel_t sel;
        if (ui_pause_run(&sel)) {
            if (sel == PAUSE_RESTART) mines_enter();
        } else {
            s_exit_request = true;
        }
        break;
    }
    case K_BACK: {
        pause_sel_t sel;
        if (ui_pause_run(&sel)) {
            if (sel == PAUSE_RESTART) mines_enter();
        } else {
            s_exit_request = true;
        }
        break;
    }
    case K_QUIT: s_exit_request = true; break;
    default: break;
    }
}

/* 自检: 校验所有已翻开数字 vs 实际邻雷数, 结果写 /dev/shm/mines.check */
static void self_check(void) {
    int f = open("/dev/shm/mines.check", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (f < 0) return;
    char b[64];
    int n = snprintf(b, sizeof(b), "CHECK revealed=%u flags=%u over=%d won=%d\n",
                     ms_revealed, ms_flags, ms_over, ms_won);
    ssize_t w = write(f, b, (size_t)n); (void)w;
    int bad = 0;
    for (int y = 0; y < MS_H; y++) {
        for (int x = 0; x < MS_W; x++) {
            if (!ms_rep[y][x]) continue;
            uint8_t real = count_adj(x, y);
            if (ms_b[y][x] != MS_CELL && ms_b[y][x] != real) {
                n = snprintf(b, sizeof(b), "BAD (%d,%d) shown=%u real=%u\n",
                             x, y, ms_b[y][x], real);
                w = write(f, b, (size_t)n); (void)w;
                bad++;
            }
            /* 雷区里显示 0-8 的情况 */
            if (ms_b[y][x] == MS_CELL && ms_rep[y][x]) {
                n = snprintf(b, sizeof(b), "MINE (%d,%d) revealed\n", x, y);
                w = write(f, b, (size_t)n); (void)w;
            }
        }
    }
    n = snprintf(b, sizeof(b), bad ? "RESULT: %d BAD\n" : "RESULT: OK\n", bad);
    w = write(f, b, (size_t)n); (void)w;
    close(f);
}

void mines_tick(uint64_t now) { (void)now; }

void mines_exit(void) {}
