/* 数独 — 9x9, 14px 格(126x126 居中), 右侧状态栏
 * 输入驱动; 移动/填数=快刷; 开局/重开/胜负=全刷
 * 操作: 方向/WASD 移光标; OK 选中当前格(输入态, 再按取消);
 *       1-9 填数(冲突允许); DEL 清除; P 候选点阵; N 下一题; R 重开;
 *       Q/BACK 退出(暂停菜单)
 * 胜利: 填满且无冲突 → HUD 显示 YOU WIN! */
#include "game.h"
#include "../config.h"
#include "../gfx/canvas.h"
#include "../gfx/font.h"
#include "../platform/display.h"
#include "../platform/input.h"
#include "../rng.h"
#include "../platform/time.h"
#include "../platform/audio.h"
#include "../platform/led.h"
#include <stddef.h>

#define SD_CELL 14
#define SD_OX ((int)((CCG_W - 9 * SD_CELL) / 2))   /* 网格左缘 */
#define SD_OY ((int)CCG_HUD_H + 2)                 /* 网格上缘 */
#define SD_N 81
#define SD_ROM_N 5

void sudoku_render(void);

/* 谜题库: 81 位数字, 0=空; 每题唯一解(离线求解器验证) */
static const char sd_rom[SD_ROM_N][SD_N + 1] = {
    "710040000032085600506070401000020305005403080390756200020030800673090002804260903",
    "005060700603490208874205060401076000250901470080004006500008000198700004007109025",
    "100800600060007400200096000040005860610903574000004123031000740807050036420378051",
    "041000009309060000076800003700490012194000367000070504000613048810900706003207951",
    "509000016014006037200817000105300480000640350048125760003090170020760008000081003",
};

static uint8_t sd_cell[SD_N];    /* 当前盘面 0=空 */
static uint8_t sd_given[SD_N];   /* 1=初始题面(不可修改) */
static uint8_t sd_cx, sd_cy;     /* 光标格 0-8 */
static bool sd_input;            /* 输入态(OK 选中格) */
static uint8_t sd_sel;           /* 输入态目标格 0-80 */
static bool sd_cand_mode;        /* P: 候选点阵开关 */
static int sd_pz;                /* 当前谜题 0..4 */
static bool sd_over;             /* 胜利 */
static bool sd_over_full;        /* 胜利全刷只做一次 */
static rng_t sd_rng;

/* 该格可行候选位掩码(bit0=数字1 .. bit8=数字9) */
static uint32_t sd_cand_mask(int idx) {
    int y = idx / 9;
    int x = idx % 9;
    uint32_t used = 0;
    for (int i = 0; i < 9; i++) {
        uint8_t a = sd_cell[y * 9 + i];
        uint8_t b = sd_cell[i * 9 + x];
        if (a != 0) used |= 1u << (a - 1);
        if (b != 0) used |= 1u << (b - 1);
    }
    int by = (y / 3) * 3;
    int bx = (x / 3) * 3;
    for (int i = by; i < by + 3; i++)
        for (int j = bx; j < bx + 3; j++) {
            uint8_t v = sd_cell[i * 9 + j];
            if (v != 0) used |= 1u << (v - 1);
        }
    return ~used & 0x1FFu;
}

/* 胜负判定: 全部填满且行/列/宫无重复 */
static bool sd_check_win(void) {
    for (int i = 0; i < SD_N; i++)
        if (sd_cell[i] == 0) return false;
    for (int y = 0; y < 9; y++) {
        uint32_t m = 0;
        for (int x = 0; x < 9; x++) {
            uint32_t b = 1u << (sd_cell[y * 9 + x] - 1);
            if ((m & b) != 0) return false;
            m |= b;
        }
    }
    for (int x = 0; x < 9; x++) {
        uint32_t m = 0;
        for (int y = 0; y < 9; y++) {
            uint32_t b = 1u << (sd_cell[y * 9 + x] - 1);
            if ((m & b) != 0) return false;
            m |= b;
        }
    }
    for (int by = 0; by < 9; by += 3)
        for (int bx = 0; bx < 9; bx += 3) {
            uint32_t m = 0;
            for (int i = 0; i < 3; i++)
                for (int j = 0; j < 3; j++) {
                    uint32_t b = 1u << (sd_cell[(by + i) * 9 + bx + j] - 1);
                    if ((m & b) != 0) return false;
                    m |= b;
                }
        }
    return true;
}

static uint32_t sd_fill_count(void) {
    uint32_t n = 0;
    for (int i = 0; i < SD_N; i++)
        if (sd_cell[i] != 0) n++;
    return n;
}

static void sd_itoa(uint32_t v, char *buf) {
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
static void sd_new_game(int pz) {
    int r = pz % SD_ROM_N;
    if (r < 0) r += SD_ROM_N;
    sd_pz = r;
    const char *s = sd_rom[sd_pz];
    for (int i = 0; i < SD_N; i++) {
        uint8_t v = (uint8_t)(s[i] - '0');
        sd_cell[i] = v;
        sd_given[i] = (v != 0) ? 1 : 0;
    }
    sd_cx = 4;
    sd_cy = 4;
    sd_input = false;
    sd_sel = 0;
    sd_cand_mode = false;
    sd_over = false;
    sd_over_full = false;
}

/* 填数: 题面格不可改; 冲突允许(完成时校验); 填满无冲突即胜 */
static void sd_place_digit(int idx, uint8_t d) {
    if (sd_given[idx] != 0) { audio_error(); return; }   /* 题面格不可改 */
    if (sd_cell[idx] == d) return;
    sd_cell[idx] = d;
    if (sd_check_win()) {
        sd_over = true;
        audio_win();
        led_fx_set(LED_FX_WIN);
    } else {
        audio_move();                     /* 正常填数 */
    }
}

/* 新局: 复位 + 渲染 + 全刷(开局/重开/胜负重试) */
static void sd_start_game(int pz) {
    sd_new_game(pz);
    sudoku_render();
    disp_full();
}

/* 侧栏状态块: 小标签 + 2x 反白大字数值 */
static void sd_block(int x, int y, const char *s) {
    int w = text_width(s) * 2;
    fb_fill_rect(x, y, w, FONT_H * 2, true);
    fb_text_scale2(x, y, s, false);
}

void sudoku_enter(void) {
    rng_seed(&sd_rng, now_ms());
    sd_new_game((int)rng_range(&sd_rng, (uint32_t)SD_ROM_N));
    sudoku_render();
    disp_full();
}

void sudoku_exit(void) {}

void sudoku_tick(uint64_t now) { (void)now; }

void sudoku_render(void) {
    fb_clear(false);

    /* 格子内容 */
    for (int y = 0; y < 9; y++) {
        for (int x = 0; x < 9; x++) {
            int idx = y * 9 + x;
            int bx = SD_OX + x * SD_CELL;
            int by = SD_OY + y * SD_CELL;
            uint8_t v = sd_cell[idx];
            if (v != 0) {
                char d[2];
                d[0] = (char)('0' + v);
                d[1] = 0;
                if (sd_given[idx] != 0) {
                    fb_fill_rect(bx, by, SD_CELL, SD_CELL, true);
                    fb_text_scale2(bx + 2, by, d, false);   /* 题面: 黑底白字 */
                } else {
                    fb_text_scale2(bx + 2, by, d, true);
                }
            } else if (sd_cand_mode) {
                /* 候选点阵: 3x3, 每点 2x2, 间隔 2px */
                uint32_t m = sd_cand_mask(idx);
                for (int k = 0; k < 9; k++) {
                    if ((m & (1u << k)) != 0)
                        fb_fill_rect(bx + 3 + (k % 3) * 4,
                                     by + 3 + (k / 3) * 4, 2, 2, true);
                }
            }
        }
    }

    /* 网格: 3x3 宫界 2px, 格线 1px, 外框 2px */
    for (int i = 0; i <= 9; i++) {
        int t = (i % 3 == 0) ? 2 : 1;
        fb_fill_rect(SD_OX + i * SD_CELL, SD_OY, t, 9 * SD_CELL, true);
        fb_fill_rect(SD_OX, SD_OY + i * SD_CELL, 9 * SD_CELL, t, true);
    }

    /* 光标/选中格(所有格画完后, 反色边框: 题面黑底白框, 其余黑框) */
    {
        int bx = SD_OX + (int)sd_cx * SD_CELL;
        int by = SD_OY + (int)sd_cy * SD_CELL;
        int cidx = (int)sd_cy * 9 + (int)sd_cx;
        bool white = (sd_given[cidx] != 0);
        if (sd_input && (int)sd_sel == cidx) {
            if (white) {
                /* 题面格黑底: 白框落在格内边缘才可见 */
                fb_stroke_rect(bx, by, SD_CELL, SD_CELL, false);
                fb_stroke_rect(bx + 1, by + 1, SD_CELL - 2, SD_CELL - 2, false);
            } else {
                fb_stroke_rect(bx - 1, by - 1, SD_CELL + 2, SD_CELL + 2, true);
                fb_stroke_rect(bx - 2, by - 2, SD_CELL + 4, SD_CELL + 4, true);
            }
        } else {
            fb_stroke_rect_thick(bx - 1, by - 1, SD_CELL + 2, SD_CELL + 2, 2,
                                 white ? false : true);
            if (sd_input) {
                int sx = SD_OX + (int)(sd_sel % 9) * SD_CELL;
                int sy = SD_OY + (int)(sd_sel / 9) * SD_CELL;
                bool sw = (sd_given[sd_sel] != 0);
                if (sw) {
                    fb_stroke_rect(sx, sy, SD_CELL, SD_CELL, false);
                    fb_stroke_rect(sx + 1, sy + 1, SD_CELL - 2, SD_CELL - 2, false);
                } else {
                    fb_stroke_rect(sx - 1, sy - 1, SD_CELL + 2, SD_CELL + 2, true);
                    fb_stroke_rect(sx - 2, sy - 2, SD_CELL + 4, SD_CELL + 4, true);
                }
            }
        }
    }

    /* 右侧状态栏: 仅状态(候选开关/输入态/已填数), 无操作说明文字 */
    {
        int sx = SD_OX + 9 * SD_CELL + 3;
        fb_text(sx, 21, "CAND", true);
        sd_block(sx, 30, sd_cand_mode ? "ON" : "OFF");
        fb_text(sx, 50, "MODE", true);
        sd_block(sx, 59, sd_input ? "FILL" : "MOVE");
        fb_text(sx, 79, "FILL", true);
        char fbuf[4];
        sd_itoa(sd_fill_count(), fbuf);
        sd_block(sx, 88, fbuf);
    }

    /* HUD: 左标题, 右谜题号 */
    fb_text(0, 0, "SUDOKU", true);
    {
        char pbuf[8];
        pbuf[0] = 'P';
        pbuf[1] = 'Z';
        pbuf[2] = ' ';
        pbuf[3] = (char)('1' + sd_pz);
        pbuf[4] = '/';
        pbuf[5] = (char)('0' + SD_ROM_N);
        pbuf[6] = 0;
        fb_text(CCG_W - text_width(pbuf) - 2, 0, pbuf, true);
    }
    fb_hline(0, CCG_HUD_H - 1, CCG_W, true);

    /* 胜利: HUD 区显示 + 全刷一次 */
    if (sd_over) {
        fb_fill_rect(0, 0, CCG_W, CCG_HUD_H - 1, false);
        fb_text(2, 2, "YOU WIN!", true);
        fb_text(CCG_W - text_width("OK/N:RETRY BACK:QUIT") - 2, 2,
                "OK/N:RETRY BACK:QUIT", true);
        if (!sd_over_full) {
            sd_over_full = true;
            disp_force_full();
        }
    }
}

void sudoku_on_key(const key_event_t *ev) {
    if (sd_over) {
        if (ev->is_repeat) return;
        if (ev->key == K_OK ||
            (ev->key == K_CHAR && (ev->ch == 'n' || ev->ch == 'r')))
            sd_start_game(sd_pz);          /* RETRY: 同一题 */
        else if (ev->key == K_BACK || ev->key == K_QUIT)
            s_exit_request = true;
        return;
    }
    switch (ev->key) {
    case K_UP:
        if (sd_cy > 0) sd_cy--;
        break;
    case K_DOWN:
        if (sd_cy < 8) sd_cy++;
        break;
    case K_LEFT:
        if (sd_cx > 0) sd_cx--;
        break;
    case K_RIGHT:
        if (sd_cx < 8) sd_cx++;
        break;
    case K_OK:
        if (ev->is_repeat) break;
        if (sd_input) {
            sd_input = false;              /* 再按 OK 取消 */
        } else {
            sd_input = true;               /* 选中当前格 */
            sd_sel = (uint8_t)(sd_cy * 9 + sd_cx);
        }
        audio_select();                    /* 选中/取消输入态 */
        break;
    case K_DEL:
        if (ev->is_repeat) break;
        {
            int idx = sd_input ? (int)sd_sel : (int)sd_cy * 9 + (int)sd_cx;
            if (sd_given[idx] == 0) {
                sd_cell[idx] = 0;
                audio_move();              /* 清除已填数字 */
            }
        }
        break;
    case K_PAUSE:                          /* P: 候选点阵开关 */
        if (ev->is_repeat) break;
        sd_cand_mode = !sd_cand_mode;
        break;
    case K_CHAR:
        if (ev->ch == 'w') {
            if (sd_cy > 0) sd_cy--;
        } else if (ev->ch == 'a') {
            if (sd_cx > 0) sd_cx--;
        } else if (ev->ch == 's') {
            if (sd_cy < 8) sd_cy++;
        } else if (ev->ch == 'd') {
            if (sd_cx < 8) sd_cx++;
        } else if (ev->ch == 'n') {
            if (!ev->is_repeat) sd_start_game((sd_pz + 1) % SD_ROM_N);
        } else if (ev->ch == 'r') {
            if (!ev->is_repeat) sd_start_game(sd_pz);
        } else if (ev->ch >= '1' && ev->ch <= '9') {
            if (!ev->is_repeat) {
                int idx = sd_input ? (int)sd_sel : (int)sd_cy * 9 + (int)sd_cx;
                sd_place_digit(idx, (uint8_t)(ev->ch - '0'));
            }
        }
        break;
    case K_BACK: {
        if (ev->is_repeat) break;
        pause_sel_t sel;
        if (ui_pause_run(&sel)) {
            if (sel == PAUSE_RESTART) sd_start_game(sd_pz);
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
