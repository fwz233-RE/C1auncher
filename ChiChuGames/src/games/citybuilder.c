/* 城市规划 CITY BUILDER — 10x5 格放置建筑, 20 回合, 邻接得分
 * 简化单人: 20 次放置内最大化分数; DEL 拆除(不消耗回合, 但已花放置额度)
 * 28px(280x140) 与 16px HUD 叠加超屏 → 26px 方格(260x130)居中, 右侧建筑栏
 * 回合制输入驱动, 无 tick 逻辑; 得分 = 所有 4-邻接建筑对得分总和(每对一次)
 * 帮助(供 games_table 使用, <=5 行):
 *   "PLACE 20 BUILDINGS. ADJACENT"
 *   "BONUS: H+P=2 H+F=-2 S+H=1"
 *   "S+F=1 F+P=-1 (4-ADJACENT)"
 *   "ARROWS MOVE  H/S/P/F PICK"
 *   "OK=PLACE  DEL=REMOVE"
 */
#include "game.h"
#include "../config.h"
#include "../gfx/canvas.h"
#include "../gfx/font.h"
#include "../platform/display.h"
#include "../platform/input.h"
#include "../platform/audio.h"
#include "../platform/led.h"
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>

#define CB_COLS 10
#define CB_ROWS 5
#define CB_CELL 26
#define CB_TURNS 20
#define CB_OX ((CCG_W - CB_COLS * CB_CELL) / 2)                 /* 18 */
#define CB_OY (CCG_HUD_H + (CCG_H - CCG_HUD_H - CB_ROWS * CB_CELL) / 2)  /* 19 */
#define CB_SB_X (CB_OX + CB_COLS * CB_CELL + 1)                 /* 侧栏 x 起点 279 */
#define CB_SB_ITEM_H 24
#define CB_SB_GAP 4

/* 建筑类型: 1=住宅H 2=商店S 3=公园P 4=工厂F */
enum { CB_H = 1, CB_S, CB_P, CB_F };

static uint8_t cb_b[CB_ROWS][CB_COLS];   /* 0=空 */
static int cb_cx = 5, cb_cy = 2;         /* 光标(像素坐标一律 int) */
static int cb_sel = CB_H;                /* 待放置类型 */
static int cb_turns;                     /* 已放置数(=回合消耗) */
static int cb_score;
static bool cb_over, cb_over_full;

void citybuilder_render(void);
static void cb_self_check(void);

/* 邻接对得分表(对称; 每对只计一次) */
static const int cb_pair[5][5] = {
    { 0,  0,  0,  0,  0 },
    { 0,  0,  1,  2, -2 },   /* H: 邻 S +1, 邻 P +2, 邻 F -2 */
    { 0,  1,  0,  0,  1 },   /* S: 邻 H +1, 邻 F +1 */
    { 0,  2,  0,  0, -1 },   /* P: 邻 H +2, 邻 F -1 */
    { 0, -2,  1, -1,  0 },   /* F: 邻 H -2, 邻 S +1, 邻 P -1 */
};

static char cb_type_char(int t) { return "HSPF"[(t - 1) & 3]; }

/* 全盘重算得分: 每对相邻建筑计一次(只看右/下邻居) */
static int cb_score_all(void) {
    int s = 0;
    for (int y = 0; y < CB_ROWS; y++) {
        for (int x = 0; x < CB_COLS; x++) {
            int a = cb_b[y][x];
            if (!a) continue;
            if (x + 1 < CB_COLS && cb_b[y][x + 1])
                s += cb_pair[a][cb_b[y][x + 1]];
            if (y + 1 < CB_ROWS && cb_b[y + 1][x])
                s += cb_pair[a][cb_b[y + 1][x]];
        }
    }
    return s;
}

static void cb_place(void) {
    if (cb_over || cb_b[cb_cy][cb_cx]) {
        audio_error();               /* 格已占用 */
        return;
    }
    cb_b[cb_cy][cb_cx] = (uint8_t)cb_sel;
    cb_turns++;
    cb_score = cb_score_all();
    audio_move();                    /* 放置 */
    if (cb_turns >= CB_TURNS) {
        cb_over = true;
        audio_win();                 /* 20 回合完成 */
        led_fx_set(LED_FX_WIN);
    }
}

static void cb_remove(void) {
    if (cb_over || !cb_b[cb_cy][cb_cx]) return;
    cb_b[cb_cy][cb_cx] = 0;
    cb_score = cb_score_all();
    audio_move();                    /* 拆除 */
}

static const char *cb_rating(int s) {
    if (s >= 40) return "URBAN UTOPIA";
    if (s >= 25) return "THRIVING CITY";
    if (s >= 12) return "A GROWING TOWN";
    if (s >= 0)  return "FIRST STEPS";
    return "URBAN RUINS";
}

/* 3x 放大字母(5x7 -> 15x21) */
static void cb_letter3(int x, int y, char ch, bool black) {
    const uint8_t *g = font_glyph5x7[(unsigned char)ch];
    for (int j = 0; j < FONT_H; j++)
        for (int i = 0; i < FONT_W; i++)
            if (g[j] & (1u << i)) fb_fill_rect(x + i * 3, y + j * 3, 3, 3, black);
}

/* 无符号整数转十进制, 返回写入字符数 */
static int cb_itoa(char *p, int v) {
    char t[8];
    int n = 0;
    if (v == 0) t[n++] = '0';
    while (v > 0 && n < 6) { t[n++] = (char)('0' + v % 10); v /= 10; }
    for (int i = n - 1; i >= 0; i--) *p++ = t[i];
    return n;
}

/* HUD 右侧 "T<回合> S<分>" */
static void cb_hud(char *out, int turns, int score) {
    char *p = out;
    *p++ = 'T';
    p += cb_itoa(p, turns);
    *p++ = ' ';
    *p++ = 'S';
    if (score < 0) { *p++ = '-'; score = -score; }
    p += cb_itoa(p, score);
    *p = 0;
}

void citybuilder_enter(void) {
    for (int y = 0; y < CB_ROWS; y++)
        for (int x = 0; x < CB_COLS; x++)
            cb_b[y][x] = 0;
    cb_cx = CB_COLS / 2;
    cb_cy = CB_ROWS / 2;
    cb_sel = CB_H;
    cb_turns = 0;
    cb_score = 0;
    cb_over = false;
    cb_over_full = false;
    citybuilder_render();
    disp_full();
}

void citybuilder_render(void) {
    fb_clear(false);
    /* 棋盘: 空格黑底白框, 建筑白底 + 3x 字母 */
    for (int y = 0; y < CB_ROWS; y++) {
        for (int x = 0; x < CB_COLS; x++) {
            int cx = CB_OX + x * CB_CELL;
            int cy = CB_OY + y * CB_CELL;
            int t = cb_b[y][x];
            if (t) {
                fb_fill_rect(cx, cy, CB_CELL, CB_CELL, true);
                cb_letter3(cx + (CB_CELL - 15) / 2, cy + (CB_CELL - 21) / 2,
                           cb_type_char(t), false);
            } else {
                fb_fill_rect(cx, cy, CB_CELL, CB_CELL, true);
                fb_stroke_rect(cx, cy, CB_CELL, CB_CELL, false);
            }
        }
    }
    /* 待放置预览(1x 小字) + 光标(最后画, 四周对称) */
    if (!cb_over) {
        if (!cb_b[cb_cy][cb_cx]) {
            char ch[2] = { cb_type_char(cb_sel), 0 };
            fb_text(CB_OX + cb_cx * CB_CELL + (CB_CELL - 5) / 2,
                    CB_OY + cb_cy * CB_CELL + (CB_CELL - 7) / 2, ch, true);
        }
        {
            int cx = CB_OX + cb_cx * CB_CELL;
            int cy = CB_OY + cb_cy * CB_CELL;
            bool cell_white = cb_b[cb_cy][cb_cx] != 0;
            fb_stroke_rect(cx - 2, cy - 2, CB_CELL + 4, CB_CELL + 4, !cell_white);
            fb_stroke_rect_thick(cx - 1, cy - 1, CB_CELL + 2, CB_CELL + 2, 2, cell_white);
        }
    }
    /* 侧栏: 分隔线 + H/S/P/F 四选(选中反白) */
    fb_vline(CB_SB_X - 1, CB_OY, CB_ROWS * CB_CELL, true);
    for (int i = 0; i < 4; i++) {
        int t = i + 1;
        int x = CB_SB_X + 1;
        int y = CB_OY + i * (CB_SB_ITEM_H + CB_SB_GAP);
        if (t == cb_sel) {
            fb_fill_rect(x, y, 15, CB_SB_ITEM_H, true);
            cb_letter3(x, y + (CB_SB_ITEM_H - 21) / 2, cb_type_char(t), false);
        } else {
            fb_stroke_rect(x, y, 15, CB_SB_ITEM_H, true);
            cb_letter3(x, y + (CB_SB_ITEM_H - 21) / 2, cb_type_char(t), true);
        }
    }
    /* HUD: 左标题黑字, 右回合/得分 */
    {
        char hud[24];
        cb_hud(hud, cb_turns, cb_score);
        fb_text(0, 0, "CITY", true);
        fb_text(CCG_W - text_width(hud) - 2, 0, hud, true);
    }
    fb_hline(0, CCG_HUD_H - 1, CCG_W, true);
    /* 结束: HUD 区两行(左结果 + 右操作), 全刷一次 */
    if (cb_over) {
        char line[32];
        snprintf(line, sizeof(line), "SCORE %d %s", cb_score, cb_rating(cb_score));
        fb_fill_rect(0, 0, CCG_W, CCG_HUD_H - 1, false);
        fb_text(2, 2, line, true);
        fb_text(CCG_W - 2 - text_width("OK/N:RETRY BACK:QUIT"), 2,
                "OK/N:RETRY BACK:QUIT", true);
        if (!cb_over_full) { cb_over_full = true; disp_force_full(); }
    }
}

void citybuilder_on_key(const key_event_t *ev) {
    /* 方向键重复可响应 */
    switch (ev->key) {
    case K_LEFT:  if (cb_cx > 0) cb_cx--; return;
    case K_RIGHT: if (cb_cx < CB_COLS - 1) cb_cx++; return;
    case K_UP:    if (cb_cy > 0) cb_cy--; return;
    case K_DOWN:  if (cb_cy < CB_ROWS - 1) cb_cy++; return;
    default: break;
    }
    if (ev->is_repeat) return;   /* 确认键/字母忽略重复 */
    if (cb_over) {
        if (ev->key == K_OK || (ev->key == K_CHAR && ev->ch == 'n'))
            citybuilder_enter();
        else if (ev->key == K_BACK || ev->key == K_QUIT)
            s_exit_request = true;
        return;
    }
    switch (ev->key) {
    case K_OK:
        cb_place();
        break;
    case K_DEL:
        cb_remove();
        break;
    case K_SPACE:
        cb_sel = cb_sel % 4 + 1;   /* 循环切换建筑 */
        break;
    case K_CHAR:
        if (ev->ch == 'h') cb_sel = CB_H;
        else if (ev->ch == 's') cb_sel = CB_S;
        else if (ev->ch == 'p') cb_sel = CB_P;
        else if (ev->ch == 'f') cb_sel = CB_F;
        else if (ev->ch >= '1' && ev->ch <= '4') cb_sel = ev->ch - '0';
        else if (ev->ch == 'w' && cb_cy > 0) cb_cy--;
        else if (ev->ch == 'a' && cb_cx > 0) cb_cx--;
        else if (ev->ch == 'd' && cb_cx < CB_COLS - 1) cb_cx++;
        else if (ev->ch == 'n') citybuilder_enter();
        else if (ev->ch == 'v') cb_self_check();
        break;
    case K_PAUSE:
    case K_BACK: {
        pause_sel_t sel;
        if (ui_pause_run(&sel)) {
            if (sel == PAUSE_RESTART) citybuilder_enter();
        } else {
            s_exit_request = true;
        }
        break;
    }
    case K_QUIT:
        s_exit_request = true;
        break;
    default:
        break;
    }
}

/* 自检: 校验回合=占用数、得分=全盘重算, 结果写 /dev/shm/citybuilder.check */
static void cb_self_check(void) {
    int f = open("/dev/shm/citybuilder.check", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (f < 0) return;
    char b[128];
    int n = snprintf(b, sizeof(b), "CHECK turns=%d score=%d over=%d\n",
                     cb_turns, cb_score, cb_over);
    ssize_t w = write(f, b, (size_t)n); (void)w;
    int bad = 0;
    int filled = 0;
    for (int y = 0; y < CB_ROWS; y++)
        for (int x = 0; x < CB_COLS; x++)
            if (cb_b[y][x]) filled++;
    if (filled != cb_turns) {
        n = snprintf(b, sizeof(b), "BAD filled=%d turns=%d\n", filled, cb_turns);
        w = write(f, b, (size_t)n); (void)w;
        bad++;
    }
    if (cb_score_all() != cb_score) {
        n = snprintf(b, sizeof(b), "BAD score=%d recompute=%d\n",
                     cb_score, cb_score_all());
        w = write(f, b, (size_t)n); (void)w;
        bad++;
    }
    n = snprintf(b, sizeof(b), bad ? "RESULT: %d BAD\n" : "RESULT: OK\n", bad);
    w = write(f, b, (size_t)n); (void)w;
    close(f);
}

void citybuilder_tick(uint64_t now) { (void)now; }   /* 输入驱动, 无 tick */

void citybuilder_exit(void) {}
