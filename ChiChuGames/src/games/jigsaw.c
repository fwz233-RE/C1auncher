/* JIGSAW — 电子拼图: 4x5 滑片拼图(1-19 数字方块 + 1 空格)
 *
 * 目标画 = 数字 1..19 按序排列(空格在右下角, 即第 20 格位置)。
 * 方向/WASD 移动光标, OK 将光标处方块滑入正交相邻的空格(计步)。
 * 洗牌从完成态随机做 240 次合法滑动, 数学保证可解。
 * 长按方向重复由框架合成(仅方向键); 游戏内移动只走 disp_fast。
 * 完成态: jg_board[i] = i+1 (i<19), jg_board[19] = 0。 */
#include "game.h"
#include "../config.h"
#include "../gfx/canvas.h"
#include "../gfx/font.h"
#include "../platform/display.h"
#include "../platform/input.h"
#include "../platform/time.h"
#include "../rng.h"
#include "../platform/audio.h"
#include "../platform/led.h"

#define JG_COLS 4
#define JG_ROWS 5
#define JG_CELLS (JG_COLS * JG_ROWS)        /* 20 */
#define JG_CELL 27                          /* 5 行 27px = 135, 游戏区 136 */
#define JG_BOARD_W (JG_COLS * JG_CELL)      /* 108 */
#define JG_OX ((CCG_W - JG_BOARD_W) / 2)    /* 94: 水平居中 */
#define JG_OY (CCG_HUD_H + 1)               /* 17: 顶栏下 1px, 底边 152 */
#define JG_SHUFFLE_MOVES 240                /* 随机滑动次数, 保证可解 */

static uint8_t jg_board[JG_CELLS];          /* 0=空格, 1-19=数字; 位置 = y*4+x */
static uint32_t jg_moves;
static int jg_cx, jg_cy;                    /* 光标(像素坐标 int) */
static bool jg_over, jg_over_full;
static rng_t jg_rng;

void jigsaw_render(void);

static int jg_blank(void) {
    for (int i = 0; i < JG_CELLS; i++)
        if (jg_board[i] == 0) return i;
    return JG_CELLS - 1;
}

/* 将 index 格方块滑入空格; 仅限正交相邻(光标选中 + OK)。
 * 返回 1=滑动成功, 0=不相邻/越界(不计步)。 */
static int jg_slide_to(int idx) {
    int b = jg_blank();
    int bx = b % JG_COLS, by = b / JG_COLS;
    int ix = idx % JG_COLS, iy = idx / JG_COLS;
    if (ix < 0 || ix >= JG_COLS || iy < 0 || iy >= JG_ROWS) return 0;
    if (!((ix == bx && (iy == by - 1 || iy == by + 1)) ||
          (iy == by && (ix == bx - 1 || ix == bx + 1))))
        return 0;
    uint8_t v = jg_board[idx];
    jg_board[idx] = 0;
    jg_board[b] = v;
    return 1;
}

static bool jg_solved(void) {
    for (int i = 0; i < JG_CELLS - 1; i++)
        if (jg_board[i] != (uint8_t)(i + 1)) return false;
    return jg_board[JG_CELLS - 1] == 0;
}

/* 新局: 完成态(1-19 顺序 + 右下空格)随机合法滑动 JG_SHUFFLE_MOVES 次。
 * 每次滑动都保持可解性; do-while 防呆: 随机走回完成态则重洗。 */
static void jg_shuffle(void) {
    do {
        for (int i = 0; i < JG_CELLS; i++)
            jg_board[i] = (uint8_t)((i + 1) % JG_CELLS);
        for (int i = 0; i < JG_SHUFFLE_MOVES; i++) {
            int b = jg_blank();
            int bx = b % JG_COLS, by = b / JG_COLS;
            int cand[4], n = 0;
            if (by > 0)           cand[n++] = b - JG_COLS;
            if (by < JG_ROWS - 1) cand[n++] = b + JG_COLS;
            if (bx > 0)           cand[n++] = b - 1;
            if (bx < JG_COLS - 1) cand[n++] = b + 1;
            jg_slide_to(cand[rng_range(&jg_rng, (uint32_t)n)]);
        }
    } while (jg_solved());
}

void jigsaw_enter(void) {
    rng_seed(&jg_rng, now_ms() ^ 0x1A75u);
    jg_shuffle();
    jg_moves = 0;
    jg_cx = JG_COLS - 1;          /* 光标停在空格上, 方向键即可选相邻方块 */
    jg_cy = JG_ROWS - 1;
    jg_over = false;
    jg_over_full = false;
    jigsaw_render();
    disp_full();
}

/* 数字 2x 放大居中(2x 后字形 10x14, 2 位宽 20, 均 < 27px 格) */
static void jg_draw_num(int px, int py, int v) {
    char s[3];
    if (v >= 10) {
        s[0] = (char)('0' + v / 10);
        s[1] = (char)('0' + v % 10);
        s[2] = 0;
    } else {
        s[0] = (char)('0' + v);
        s[1] = 0;
    }
    int w2 = text_width(s) * 2 - 2;        /* 2x 实际宽度: 12*(n-1)+10 */
    fb_text_scale2(px + (JG_CELL - w2) / 2,
                   py + (JG_CELL - 2 * FONT_H) / 2, s, true);
}

void jigsaw_render(void) {
    fb_clear(false);
    /* 棋盘: 每格细边框 + 数字 */
    for (int y = 0; y < JG_ROWS; y++)
        for (int x = 0; x < JG_COLS; x++) {
            int v = jg_board[y * JG_COLS + x];
            int px = JG_OX + x * JG_CELL, py = JG_OY + y * JG_CELL;
            fb_stroke_rect(px, py, JG_CELL, JG_CELL, true);
            if (v) jg_draw_num(px, py, v);
        }
    /* 光标: 必须所有格子画完后画, 2px 粗边框(白格黑边, 四周对称) */
    fb_stroke_rect_thick(JG_OX + jg_cx * JG_CELL, JG_OY + jg_cy * JG_CELL,
                         JG_CELL, JG_CELL, 2, true);
    /* HUD 顶栏: 黑字白底, 左标题右步数 */
    fb_fill_rect(0, 0, CCG_W, CCG_HUD_H - 1, false);
    if (jg_over) {
        fb_text(2, 2, "SOLVED!", true);
        fb_text(CCG_W - 2 - text_width("OK/N:RETRY BACK:QUIT"), 2,
                "OK/N:RETRY BACK:QUIT", true);
        if (!jg_over_full) { jg_over_full = true; disp_force_full(); }
    } else {
        fb_text(0, 0, "JIGSAW", true);
        char buf[16];
        unsigned i = 0, n = 0;
        const char *lab = "MOVES ";
        while (lab[n]) buf[i++] = lab[n++];
        uint32_t v = jg_moves;
        char rev[12];
        unsigned ri = 0;
        if (v == 0) rev[ri++] = '0';
        while (v && ri < 10) { rev[ri++] = (char)('0' + v % 10); v /= 10; }
        while (ri > 0) buf[i++] = rev[--ri];
        buf[i] = 0;
        fb_text(CCG_W - 2 - text_width(buf), 0, buf, true);
    }
    fb_hline(0, CCG_HUD_H - 1, CCG_W, true);
}

void jigsaw_on_key(const key_event_t *ev) {
    if (jg_over) {
        if (ev->is_repeat) return;
        if (ev->key == K_OK || (ev->key == K_CHAR && ev->ch == 'n'))
            jigsaw_enter();
        else if (ev->key == K_BACK || ev->key == K_QUIT)
            s_exit_request = true;
        return;
    }
    switch (ev->key) {
    case K_UP:    if (jg_cy > 0) jg_cy--; audio_tick(); break;
    case K_DOWN:  if (jg_cy < JG_ROWS - 1) jg_cy++; audio_tick(); break;
    case K_LEFT:  if (jg_cx > 0) jg_cx--; audio_tick(); break;
    case K_RIGHT: if (jg_cx < JG_COLS - 1) jg_cx++; audio_tick(); break;
    case K_CHAR:
        if (ev->ch == 'w')      { if (jg_cy > 0) jg_cy--; audio_tick(); }
        else if (ev->ch == 's') { if (jg_cy < JG_ROWS - 1) jg_cy++; audio_tick(); }
        else if (ev->ch == 'a') { if (jg_cx > 0) jg_cx--; audio_tick(); }
        else if (ev->ch == 'd') { if (jg_cx < JG_COLS - 1) jg_cx++; audio_tick(); }
        else if (ev->ch == 'n' && !ev->is_repeat) { audio_select(); jigsaw_enter(); }
        break;
    case K_OK:
        /* 确认键忽略重复; 光标格方块滑入相邻空格 */
        if (!ev->is_repeat && jg_slide_to(jg_cy * JG_COLS + jg_cx)) {
            jg_moves++;
            if (jg_solved()) {
                jg_over = true;
                audio_win();
                led_fx_set(LED_FX_WIN);
            } else {
                audio_move();
            }
        } else if (!ev->is_repeat) {
            audio_error();
        }
        break;
    case K_BACK:
        if (!ev->is_repeat) {
            pause_sel_t sel;
            if (ui_pause_run(&sel)) {
                if (sel == PAUSE_RESTART) jigsaw_enter();
            } else {
                s_exit_request = true;
            }
        }
        break;
    case K_QUIT:
        if (!ev->is_repeat) s_exit_request = true;
        break;
    default: break;
    }
    /* 方向键重复可响应(长按连移由框架合成) */
}

void jigsaw_tick(uint64_t now) { (void)now; }
void jigsaw_exit(void) {}
