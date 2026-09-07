/* FIFTEEN — 15 谜: 4x4 滑动数字, 方向键朝空格方向滑动, 步数计数
 *
 * 规则: 按下的方向 = 数字滑入空格的方向。例: 按 UP 将空格正下方的
 * 数字上滑入空格(空格下移一行); 空格在棋盘边界则该方向无效不计步。
 * 洗牌从完成态随机做 200 次合法滑动, 数学保证可解。
 * 长按重复由框架合成(仅方向键), 游戏内移动只走 disp_fast。 */
#include "game.h"
#include "../config.h"
#include "../gfx/canvas.h"
#include "../gfx/font.h"
#include "../platform/display.h"
#include "../platform/input.h"
#include "../platform/time.h"
#include "../platform/audio.h"
#include "../platform/led.h"
#include "../rng.h"

#define FP_CELL 32
#define FP_BOARD (4 * FP_CELL)            /* 128 */
#define FP_OX ((CCG_W - FP_BOARD) / 2)    /* 84: 水平居中 */
#define FP_OY 18                          /* HUD(16) 下留 2px */

#define FP_SHUFFLE_MOVES 200              /* 随机滑动次数, 保证可解 */

static uint8_t fp_board[16];              /* 0=空格, 1-15=数字; 位置 = y*4+x */
static uint32_t fp_moves;
static bool fp_over, fp_over_full;
static rng_t fp_rng;

void fifteen_render(void);

static int fp_blank(void) {
    for (int i = 0; i < 16; i++)
        if (fp_board[i] == 0) return i;
    return 15;
}

/* 朝空格方向滑动: dir 0=上 1=下 2=左 3=右
 * 按 UP:   空格正下方的数字上滑入空格(要求空格不在底行);
 * 按 DOWN: 空格正上方的数字下滑入空格(要求空格不在顶行);
 * 按 LEFT: 空格正右侧的数字左滑入空格(要求空格不在最右列);
 * 按 RIGHT:空格正左侧的数字右滑入空格(要求空格不在最左列)。
 * 返回 1=发生滑动(计步), 0=边界无效(不计步)。 */
static int fp_slide_dir(int dir) {
    int b = fp_blank();
    int bx = b & 3, by = b >> 2;
    int t;
    switch (dir) {
    case 0: if (by >= 3) return 0; t = b + 4; break;
    case 1: if (by <= 0) return 0; t = b - 4; break;
    case 2: if (bx >= 3) return 0; t = b + 1; break;
    default: if (bx <= 0) return 0; t = b - 1; break;
    }
    uint8_t v = fp_board[t];
    fp_board[t] = 0;
    fp_board[b] = v;
    return 1;
}

static bool fp_solved(void) {
    for (int i = 0; i < 15; i++)
        if (fp_board[i] != (uint8_t)(i + 1)) return false;
    return fp_board[15] == 0;
}

/* 新局: 完成态(1-15 顺序 + 右下空格)随机合法滑动 FP_SHUFFLE_MOVES 次。
 * 每次滑动都保持可解性, 200 次后必为可解随机盘。 */
static void fp_shuffle(void) {
    do {
        for (int i = 0; i < 16; i++) fp_board[i] = (uint8_t)((i + 1) & 15);
        for (int i = 0; i < FP_SHUFFLE_MOVES; i++)
            fp_slide_dir((int)rng_range(&fp_rng, 4));
    } while (fp_solved());   /* 防呆: 随机走回完成态则重洗 */
}

void fifteen_enter(void) {
    rng_seed(&fp_rng, now_ms() ^ 0xF1F7u);
    fp_shuffle();
    fp_moves = 0;
    fp_over = false;
    fp_over_full = false;
    fifteen_render();
    disp_full();
}

/* 数字 2x 放大居中(2x 后字形 10x14, 2 位宽 22, 均 < 32px 格) */
static void fp_draw_num(int px, int py, int v) {
    char s[3];
    if (v >= 10) {
        s[0] = (char)('0' + v / 10);
        s[1] = (char)('0' + v % 10);
        s[2] = 0;
    } else {
        s[0] = (char)('0' + v);
        s[1] = 0;
    }
    int w2 = text_width(s) * 2 - 2;       /* 2x 实际宽度: 12*(n-1)+10 */
    fb_text_scale2(px + (FP_CELL - w2) / 2, py + (FP_CELL - 2 * FONT_H) / 2, s, true);
}

void fifteen_render(void) {
    fb_clear(false);
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++) {
            int v = fp_board[y * 4 + x];
            int px = FP_OX + x * FP_CELL, py = FP_OY + y * FP_CELL;
            fb_stroke_rect(px, py, FP_CELL, FP_CELL, true);
            if (v) fp_draw_num(px, py, v);
        }
    /* HUD 顶栏: 黑字白底, 左标题右步数 */
    fb_fill_rect(0, 0, CCG_W, CCG_HUD_H - 1, false);
    if (fp_over) {
        fb_text(2, 2, "SOLVED!", true);
        fb_text(CCG_W - 2 - text_width("OK/N:RETRY BACK:QUIT"), 2,
                "OK/N:RETRY BACK:QUIT", true);
        if (!fp_over_full) { fp_over_full = true; disp_force_full(); }
    } else {
        fb_text(0, 0, "FIFTEEN", true);
        char buf[16];
        unsigned i = 0, n = 0;
        const char *lab = "MOVES ";
        while (lab[n]) buf[i++] = lab[n++];
        uint32_t v = fp_moves;
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

void fifteen_on_key(const key_event_t *ev) {
    if (fp_over) {
        if (ev->is_repeat) return;
        if (ev->key == K_OK || (ev->key == K_CHAR && ev->ch == 'n')) {
            audio_select();
            fifteen_enter();
        }
        else if (ev->key == K_BACK || ev->key == K_QUIT)
            s_exit_request = true;
        return;
    }
    int dir = -1;
    switch (ev->key) {
    case K_UP: dir = 0; break;
    case K_DOWN: dir = 1; break;
    case K_LEFT: dir = 2; break;
    case K_RIGHT: dir = 3; break;
    case K_CHAR:
        if (ev->ch == 'w') dir = 0;
        else if (ev->ch == 's') dir = 1;
        else if (ev->ch == 'a') dir = 2;
        else if (ev->ch == 'd') dir = 3;
        else if (ev->ch == 'n' && !ev->is_repeat) { audio_select(); fifteen_enter(); }
        break;
    case K_BACK:
        if (!ev->is_repeat) {
            pause_sel_t sel;
            if (ui_pause_run(&sel)) {
                if (sel == PAUSE_RESTART) fifteen_enter();
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
    /* 方向键重复可响应(长按连滑由框架合成) */
    if (dir >= 0) {
        if (fp_slide_dir(dir)) {
            fp_moves++;
            if (fp_solved()) {
                fp_over = true;
                audio_win();           /* 拼图完成 */
                led_fx_set(LED_FX_WIN);
            } else if (!ev->is_repeat) {
                audio_move();          /* 单步滑动 */
            }
        } else if (!ev->is_repeat) {
            audio_error();             /* 边界无效 */
        }
    }
}

void fifteen_tick(uint64_t now) { (void)now; }
void fifteen_exit(void) {}
