/* 二进制谜题 (Binary Puzzle / Takuzu) — 10x10, 13px 格 (130x130 居中)
 * 规则: 每行每列 0/1 各 5 个; 任意行/列无三连; 行/列两两不同
 * 操作: 方向/WASD 移光标; OK 循环 空->0->1->空; DEL 清空; N 换题; R 重开;
 *       P/BACK 暂停菜单; Q 退出
 * 完成: 填满且规则全满足 -> WIN (HUD 区显示, 全刷一次)
 * 输入驱动; 移动/填数=快刷; 开局/重开/胜利=全刷 */
#include "game.h"
#include "../config.h"
#include "../gfx/canvas.h"
#include "../gfx/font.h"
#include "../platform/audio.h"
#include "../platform/display.h"
#include "../platform/input.h"
#include "../platform/led.h"
#include "../rng.h"
#include "../platform/time.h"
#include <stddef.h>

#define BP_N 10
#define BP_CELL 13                       /* 13px: 130x130 是 136px 游戏区的最大方阵 */
#define BP_OX ((int)((CCG_W - BP_N * BP_CELL) / 2))
#define BP_OY ((int)(CCG_HUD_H + (CCG_H - CCG_HUD_H - BP_N * BP_CELL) / 2))
#define BP_NCELL (BP_N * BP_N)
#define BP_ROM_N 3

#define BP_EMPTY 0
#define BP_ZERO 1
#define BP_ONE 2

void binarypuzzle_render(void);

/* 3 个 ROM 谜题: 100 格, '0'/'1'=题面, ' '=空位
 * 离线脚本验证: 每道题唯一解且完整解满足全部规则(平衡/无三连/行列互异) */
static const char bp_rom[BP_ROM_N][BP_NCELL + 1] = {
    " 01 1 1  10        0 11  1 01  0  0010     1    10 11          1    1   0     01    11  100   011  0",
    "      01   1  1     010   11 10   0   0  1     1  11 1 10  00 1   1     1 0 00 0010      00    0  1 ",
    "  1    1101 0  1 11  0  1     0  0   10    1100   1  01 1  0   1 1           1  10 0 1  1 1  11  0  ",
};

static uint8_t bp_cell[BP_NCELL];        /* 0=空 1='0' 2='1' */
static uint8_t bp_given[BP_NCELL];       /* 1=题面固定(不可改) */
static uint8_t bp_cx, bp_cy;             /* 光标 0-9 */
static int bp_pz;                        /* 当前谜题 0..2 */
static bool bp_over;                     /* 胜利 */
static bool bp_over_full;                /* 胜利全刷只做一次 */
static rng_t bp_rng;

/* ---- 胜利判定: 填满 且 平衡 且 无三连 且 行列互异 ---- */
static bool bp_check_win(void) {
    int i, r, c;
    for (i = 0; i < BP_NCELL; i++)
        if (bp_cell[i] == BP_EMPTY) return false;
    for (r = 0; r < BP_N; r++) {         /* 行平衡 */
        int ones = 0;
        for (c = 0; c < BP_N; c++)
            if (bp_cell[r * BP_N + c] == BP_ONE) ones++;
        if (ones != BP_N / 2) return false;
    }
    for (c = 0; c < BP_N; c++) {         /* 列平衡 */
        int ones = 0;
        for (r = 0; r < BP_N; r++)
            if (bp_cell[r * BP_N + c] == BP_ONE) ones++;
        if (ones != BP_N / 2) return false;
    }
    for (r = 0; r < BP_N; r++)           /* 行无三连 */
        for (c = 0; c < BP_N - 2; c++)
            if (bp_cell[r * BP_N + c] == bp_cell[r * BP_N + c + 1] &&
                bp_cell[r * BP_N + c + 1] == bp_cell[r * BP_N + c + 2])
                return false;
    for (c = 0; c < BP_N; c++)           /* 列无三连 */
        for (r = 0; r < BP_N - 2; r++)
            if (bp_cell[r * BP_N + c] == bp_cell[(r + 1) * BP_N + c] &&
                bp_cell[(r + 1) * BP_N + c] == bp_cell[(r + 2) * BP_N + c])
                return false;
    for (r = 0; r < BP_N; r++)           /* 行两两不同 */
        for (int r2 = r + 1; r2 < BP_N; r2++) {
            int m1 = 0, m2 = 0;
            for (c = 0; c < BP_N; c++) {
                if (bp_cell[r * BP_N + c] == BP_ONE) m1 |= 1 << c;
                if (bp_cell[r2 * BP_N + c] == BP_ONE) m2 |= 1 << c;
            }
            if (m1 == m2) return false;
        }
    for (c = 0; c < BP_N; c++)           /* 列两两不同 */
        for (int c2 = c + 1; c2 < BP_N; c2++) {
            int m1 = 0, m2 = 0;
            for (r = 0; r < BP_N; r++) {
                if (bp_cell[r * BP_N + c] == BP_ONE) m1 |= 1 << r;
                if (bp_cell[r * BP_N + c2] == BP_ONE) m2 |= 1 << r;
            }
            if (m1 == m2) return false;
        }
    return true;
}

static uint32_t bp_fill_count(void) {
    uint32_t n = 0;
    for (int i = 0; i < BP_NCELL; i++)
        if (bp_cell[i] != BP_EMPTY) n++;
    return n;
}

static void bp_itoa(uint32_t v, char *buf) {
    char rev[8];
    int n = 0;
    do {
        rev[n++] = (char)('0' + (int)(v % 10));
        v /= 10;
    } while (v != 0 && n < 7);
    while (n > 0) *buf++ = rev[--n];
    *buf = 0;
}

/* 载入谜题 pz 并复位 */
static void bp_new_game(int pz) {
    int r = pz % BP_ROM_N;
    if (r < 0) r += BP_ROM_N;
    bp_pz = r;
    const char *s = bp_rom[bp_pz];
    for (int i = 0; i < BP_NCELL; i++) {
        char ch = s[i];
        uint8_t v = (ch == ' ') ? BP_EMPTY : (uint8_t)((ch == '1') ? BP_ONE : BP_ZERO);
        bp_cell[i] = v;
        bp_given[i] = (v != BP_EMPTY) ? 1 : 0;
    }
    bp_cx = BP_N / 2 - 1;
    bp_cy = BP_N / 2 - 1;
    bp_over = false;
    bp_over_full = false;
}

/* 填数: 题面格不可改; 空->0->1->空 循环; 填满且规则全满足即胜 */
static void bp_cycle(int idx) {
    if (bp_given[idx] != 0) { audio_error(); return; }   /* 题面格不可改 */
    bp_cell[idx] = (uint8_t)((bp_cell[idx] + 1) % 3);
    if (bp_check_win()) {
        bp_over = true;
        audio_win();
        led_fx_set(LED_FX_WIN);
    } else {
        audio_select();           /* 填数 */
    }
}

static void bp_clear_cell(int idx) {
    if (bp_given[idx] != 0) return;
    bp_cell[idx] = BP_EMPTY;
}

/* 新局: 复位 + 渲染 + 全刷(开局/重开/换题) */
static void bp_start_game(int pz) {
    bp_new_game(pz);
    binarypuzzle_render();
    disp_full();
}

void binarypuzzle_enter(void) {
    rng_seed(&bp_rng, now_ms());
    bp_start_game((int)rng_range(&bp_rng, (uint32_t)BP_ROM_N));
}

void binarypuzzle_exit(void) {}

void binarypuzzle_tick(uint64_t now) { (void)now; }

void binarypuzzle_render(void) {
    fb_clear(false);

    /* 格子内容 */
    for (int y = 0; y < BP_N; y++) {
        for (int x = 0; x < BP_N; x++) {
            int idx = y * BP_N + x;
            int bx = BP_OX + x * BP_CELL;
            int by = BP_OY + y * BP_CELL;
            uint8_t v = bp_cell[idx];
            if (v == BP_EMPTY) continue;
            char d[2];
            d[0] = (char)('0' + v - 1);
            d[1] = 0;
            if (v == BP_ONE) {
                /* 1: 黑格 + 白字 */
                fb_fill_rect(bx, by, BP_CELL, BP_CELL, true);
                fb_text_scale2(bx + 1, by, d, false);
            } else {
                /* 0: 白格 + 黑字 */
                fb_text_scale2(bx + 1, by, d, true);
            }
            if (bp_given[idx] != 0) {
                /* 题面格: 四角 2x2 标记(0 黑点 / 1 白点) */
                bool ink = (v == BP_ONE) ? false : true;
                fb_fill_rect(bx + 1, by + 1, 2, 2, ink);
                fb_fill_rect(bx + BP_CELL - 3, by + 1, 2, 2, ink);
                fb_fill_rect(bx + 1, by + BP_CELL - 3, 2, 2, ink);
                fb_fill_rect(bx + BP_CELL - 3, by + BP_CELL - 3, 2, 2, ink);
            }
        }
    }

    /* 网格: 内部 1px 线 + 外框 2px(盖掉 2x 数字的 1px 溢出行) */
    for (int i = 0; i <= BP_N; i++) {
        fb_fill_rect(BP_OX + i * BP_CELL, BP_OY, 1, BP_N * BP_CELL, true);
        fb_fill_rect(BP_OX, BP_OY + i * BP_CELL, BP_N * BP_CELL, 1, true);
    }
    fb_stroke_rect_thick(BP_OX, BP_OY, BP_N * BP_CELL, BP_N * BP_CELL, 2, true);

    /* 光标: 所有格画完后, 反色 2px 外框(黑格白框/其余黑框) */
    {
        int bx = BP_OX + (int)bp_cx * BP_CELL;
        int by = BP_OY + (int)bp_cy * BP_CELL;
        bool white = (bp_cell[(int)bp_cy * BP_N + (int)bp_cx] == BP_ONE);
        fb_stroke_rect_thick(bx - 2, by - 2, BP_CELL + 4, BP_CELL + 4, 2,
                             white ? false : true);
    }

    /* HUD: 左标题+题号, 右已填计数 */
    fb_text(0, 0, "BINARY", true);
    {
        char pbuf[8];
        pbuf[0] = 'P';
        pbuf[1] = 'Z';
        pbuf[2] = ' ';
        pbuf[3] = (char)('1' + bp_pz);
        pbuf[4] = '/';
        pbuf[5] = (char)('0' + BP_ROM_N);
        pbuf[6] = 0;
        fb_text(text_width("BINARY") + 4, 0, pbuf, true);
    }
    {
        char nbuf[8];
        bp_itoa(bp_fill_count(), nbuf);
        char fbuf[16];
        const char *label = "FILL ";
        int n = 0;
        while (label[n]) { fbuf[n] = label[n]; n++; }
        for (int j = 0; nbuf[j]; j++) fbuf[n++] = nbuf[j];
        fbuf[n++] = '/';
        fbuf[n++] = '1';
        fbuf[n++] = '0';
        fbuf[n++] = '0';
        fbuf[n] = 0;
        fb_text(CCG_W - text_width(fbuf) - 2, 0, fbuf, true);
    }
    fb_hline(0, CCG_HUD_H - 1, CCG_W, true);

    /* 胜利: HUD 区显示 + 全刷一次 */
    if (bp_over) {
        fb_fill_rect(0, 0, CCG_W, CCG_HUD_H - 1, false);
        fb_text(2, 2, "YOU WIN!", true);
        fb_text(CCG_W - text_width("OK/N:RETRY BACK:QUIT") - 2, 2,
                "OK/N:RETRY BACK:QUIT", true);
        if (!bp_over_full) {
            bp_over_full = true;
            disp_force_full();
        }
    }
}

void binarypuzzle_on_key(const key_event_t *ev) {
    if (bp_over) {
        if (ev->is_repeat) return;
        if (ev->key == K_OK ||
            (ev->key == K_CHAR && (ev->ch == 'n' || ev->ch == 'r')))
            bp_start_game(bp_pz);          /* RETRY: 同一题 */
        else if (ev->key == K_BACK || ev->key == K_QUIT)
            s_exit_request = true;
        return;
    }
    switch (ev->key) {
    case K_UP:
        if (bp_cy > 0) bp_cy--;
        audio_tick();
        break;
    case K_DOWN:
        if (bp_cy < BP_N - 1) bp_cy++;
        audio_tick();
        break;
    case K_LEFT:
        if (bp_cx > 0) bp_cx--;
        audio_tick();
        break;
    case K_RIGHT:
        if (bp_cx < BP_N - 1) bp_cx++;
        audio_tick();
        break;
    case K_OK:
        if (ev->is_repeat) break;
        bp_cycle((int)bp_cy * BP_N + (int)bp_cx);
        break;
    case K_DEL:
        if (ev->is_repeat) break;
        bp_clear_cell((int)bp_cy * BP_N + (int)bp_cx);
        audio_move();
        break;
    case K_CHAR:
        if (ev->ch == 'w') {
            if (bp_cy > 0) bp_cy--;
            audio_tick();
        } else if (ev->ch == 'a') {
            if (bp_cx > 0) bp_cx--;
            audio_tick();
        } else if (ev->ch == 's') {
            if (bp_cy < BP_N - 1) bp_cy++;
            audio_tick();
        } else if (ev->ch == 'd') {
            if (bp_cx < BP_N - 1) bp_cx++;
            audio_tick();
        } else if (ev->ch == 'n') {
            if (!ev->is_repeat) { audio_select(); bp_start_game((bp_pz + 1) % BP_ROM_N); }
        } else if (ev->ch == 'r') {
            if (!ev->is_repeat) { audio_select(); bp_start_game(bp_pz); }
        }
        break;
    case K_PAUSE:
    case K_BACK: {
        if (ev->is_repeat) break;
        pause_sel_t sel;
        if (ui_pause_run(&sel)) {
            if (sel == PAUSE_RESTART) bp_start_game(bp_pz);
        } else {
            s_exit_request = true;
        }
        break;
    }
    case K_QUIT:
        if (!ev->is_repeat) s_exit_request = true;
        break;
    default:
        break;
    }
}
