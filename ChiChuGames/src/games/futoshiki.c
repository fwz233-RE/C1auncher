/* 不等号数独 FUTOSHIKI — 4x4 填 1-4, 行/列不重复且满足 < > 不等式
 * 输入驱动; 移动/填数=快刷; 开局/重开/胜利=全刷
 * 操作: 方向/WASD 移光标; OK 选中(输入态, 再按取消); 1-4 填数;
 *       DEL 清除(题面格不可改); N 下一题; R 重开; Q 退出; P/BACK 暂停
 * 胜利: 填满且行/列无重复且全部不等式成立 → HUD 显示 YOU WIN!
 * 版面: 格 28px(112) + 格间 8px 放不等式符号(3 处) = 136x136 居中
 * 菜单元数据建议(games_table.c):
 *   title "FUTOSHIKI"  tagline "INEQUALITY SUDOKU 4X4"
 *   help: "Fill each row & column with", "1-4 (no repeats) so every",
 *         "< > hint between cells holds", "true. OK select, N next,",
 *         "R reset", NULL
 *   tick_interval_ms 0, repeat 默认 */
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

#define FT_CELL 28                       /* 格子边长 */
#define FT_GAP 8                         /* 格间不等式区宽 */
#define FT_PITCH (FT_CELL + FT_GAP)      /* 36: 同行相邻格中心距 */
#define FT_BOARD (4 * FT_CELL + 3 * FT_GAP)   /* 136 */
#define FT_OX ((int)((CCG_W - FT_BOARD) / 2)) /* 80 */
#define FT_OY ((int)CCG_HUD_H)                /* 16 */
#define FT_N 16
#define FT_ROM_N 5

void futoshiki_render(void);

/* 谜题库: 40 位 = 16 数字题面(0=空) + 12 横向不等式 + 12 纵向不等式
 * 横向索引 y*3+x: 第 x 与 x+1 列之间(y 行); '<'=左<右, '>'=左>右, '.'=无
 * 纵向索引 y*4+x: 第 y 与 y+1 行之间(x 列); '<'=上<下, '>'=上>下
 * 每题唯一解(host 测试回溯计数 == 1 验证) */
static const char ft_rom[FT_ROM_N][FT_N + 24 + 1] = {
    "0000002002003010<<>>...<.<><.>><...<<<..",
    "4020000420010000.<...<<..<><><.<<.<>.<.<",
    "0003301003001030.<.>..>.><.><<><.<<>.<<.",
    "4000004332001002>>>.<>>><...>>..<<><.<.>",
    "0020031010434100<>>...<.>><>>>><.>..<.>.",
};

static uint8_t ft_cell[FT_N];    /* 当前盘面 0=空 */
static uint8_t ft_given[FT_N];   /* 1=题面(不可修改) */
static char ft_h[12];            /* 横向不等式 12 个 */
static char ft_v[12];            /* 纵向不等式 12 个 */
static uint8_t ft_cx, ft_cy;     /* 光标格 0-3 */
static bool ft_input;            /* OK 选中输入态 */
static uint8_t ft_sel;           /* 输入态目标格 0-15 */
static int ft_pz;                /* 当前谜题 0..4 */
static bool ft_over;             /* 胜利 */
static bool ft_over_full;        /* 胜利全刷只做一次 */
static rng_t ft_rng;

/* 胜负判定: 全填满 + 行/列无重复 + 全部不等式成立 */
static bool ft_check_win(void) {
    for (int i = 0; i < FT_N; i++)
        if (ft_cell[i] == 0) return false;
    for (int y = 0; y < 4; y++) {
        uint32_t m = 0;
        for (int x = 0; x < 4; x++) {
            uint32_t b = 1u << (ft_cell[y * 4 + x] - 1);
            if ((m & b) != 0) return false;
            m |= b;
        }
    }
    for (int x = 0; x < 4; x++) {
        uint32_t m = 0;
        for (int y = 0; y < 4; y++) {
            uint32_t b = 1u << (ft_cell[y * 4 + x] - 1);
            if ((m & b) != 0) return false;
            m |= b;
        }
    }
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 3; x++) {
            char s = ft_h[y * 3 + x];
            if (s == '<' && !(ft_cell[y * 4 + x] < ft_cell[y * 4 + x + 1]))
                return false;
            if (s == '>' && !(ft_cell[y * 4 + x] > ft_cell[y * 4 + x + 1]))
                return false;
        }
    for (int y = 0; y < 3; y++)
        for (int x = 0; x < 4; x++) {
            char s = ft_v[y * 4 + x];
            if (s == '<' && !(ft_cell[y * 4 + x] < ft_cell[(y + 1) * 4 + x]))
                return false;
            if (s == '>' && !(ft_cell[y * 4 + x] > ft_cell[(y + 1) * 4 + x]))
                return false;
        }
    return true;
}

/* 载入谜题 pz 并复位 */
static void ft_new_game(int pz) {
    int r = pz % FT_ROM_N;
    if (r < 0) r += FT_ROM_N;
    ft_pz = r;
    const char *s = ft_rom[ft_pz];
    for (int i = 0; i < FT_N; i++) {
        uint8_t v = (uint8_t)(s[i] - '0');
        ft_cell[i] = v;
        ft_given[i] = (v != 0) ? 1 : 0;
    }
    for (int i = 0; i < 12; i++) {
        ft_h[i] = s[16 + i];
        ft_v[i] = s[28 + i];
    }
    ft_cx = 1;
    ft_cy = 1;
    ft_input = false;
    ft_sel = 0;
    ft_over = false;
    ft_over_full = false;
}

/* 填数: 题面格不可改; 冲突允许(完成时校验); 填满无冲突即胜 */
static void ft_place_digit(int idx, uint8_t d) {
    if (ft_given[idx] != 0) {
        audio_error();                 /* 题面格不可改 */
        return;
    }
    if (ft_cell[idx] == d) return;
    ft_cell[idx] = d;
    if (ft_check_win()) {
        ft_over = true;
        audio_win();                   /* 全部成立 */
        led_fx_set(LED_FX_WIN);
    } else {
        audio_move();                  /* 填数 */
    }
}

/* 新局: 复位 + 渲染 + 全刷(开局/重开/胜利重试) */
static void ft_start_game(int pz) {
    ft_new_game(pz);
    futoshiki_render();
    disp_full();
}

void futoshiki_enter(void) {
    rng_seed(&ft_rng, now_ms());
    ft_new_game((int)rng_range(&ft_rng, (uint32_t)FT_ROM_N));
    futoshiki_render();
    disp_full();
}

void futoshiki_exit(void) {}

void futoshiki_tick(uint64_t now) { (void)now; }

void futoshiki_render(void) {
    fb_clear(false);

    /* 格子内容 */
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            int idx = y * 4 + x;
            int bx = FT_OX + x * FT_PITCH;
            int by = FT_OY + y * FT_PITCH;
            uint8_t v = ft_cell[idx];
            if (v != 0) {
                char d[2];
                d[0] = (char)('0' + v);
                d[1] = 0;
                if (ft_given[idx] != 0) {
                    fb_fill_rect(bx, by, FT_CELL, FT_CELL, true);
                    fb_text_scale2(bx + 9, by + 7, d, false);  /* 题面: 黑底白字 */
                } else {
                    fb_text_scale2(bx + 9, by + 7, d, true);   /* 玩家: 黑字 */
                }
            }
        }
    }

    /* 网格: 外框 2px, 内线 1px(格间留 8px 空隙放符号) */
    for (int i = 0; i <= 4; i++) {
        int t = (i == 0 || i == 4) ? 2 : 1;
        int x = FT_OX + i * FT_CELL + (i > 0 ? (i - 1) * FT_GAP : 0);
        int y = FT_OY + i * FT_CELL + (i > 0 ? (i - 1) * FT_GAP : 0);
        fb_fill_rect(x, FT_OY, t, FT_BOARD, true);
        fb_fill_rect(FT_OX, y, FT_BOARD, t, true);
    }

    /* 不等式符号: 1x 字 5x7 居中于 8px 格间 */
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 3; x++) {
            char s = ft_h[y * 3 + x];
            if (s == '<' || s == '>') {
                char t[2];
                t[0] = s;
                t[1] = 0;
                fb_text(FT_OX + x * FT_PITCH + FT_CELL + 2,
                        FT_OY + y * FT_PITCH + 11, t, true);
            }
        }
    for (int y = 0; y < 3; y++)
        for (int x = 0; x < 4; x++) {
            char s = ft_v[y * 4 + x];
            if (s == '<' || s == '>') {
                char t[2];
                t[0] = s;
                t[1] = 0;
                fb_text(FT_OX + x * FT_PITCH + 12,
                        FT_OY + y * FT_PITCH + FT_CELL + 2, t, true);
            }
        }

    /* 光标/选中格(所有格子画完后, 反色边框: 题面黑底白框, 其余黑框) */
    {
        int bx = FT_OX + (int)ft_cx * FT_PITCH;
        int by = FT_OY + (int)ft_cy * FT_PITCH;
        int cidx = (int)ft_cy * 4 + (int)ft_cx;
        bool white = (ft_given[cidx] != 0);
        if (ft_input && (int)ft_sel == cidx) {
            if (white) {
                fb_stroke_rect(bx, by, FT_CELL, FT_CELL, false);
                fb_stroke_rect(bx + 1, by + 1, FT_CELL - 2, FT_CELL - 2, false);
            } else {
                fb_stroke_rect(bx - 1, by - 1, FT_CELL + 2, FT_CELL + 2, true);
                fb_stroke_rect(bx - 2, by - 2, FT_CELL + 4, FT_CELL + 4, true);
            }
        } else {
            fb_stroke_rect_thick(bx - 1, by - 1, FT_CELL + 2, FT_CELL + 2, 2,
                                 white ? false : true);
            if (ft_input) {
                int sx = FT_OX + (int)(ft_sel % 4) * FT_PITCH;
                int sy = FT_OY + (int)(ft_sel / 4) * FT_PITCH;
                bool sw = (ft_given[ft_sel] != 0);
                if (sw) {
                    fb_stroke_rect(sx, sy, FT_CELL, FT_CELL, false);
                    fb_stroke_rect(sx + 1, sy + 1, FT_CELL - 2, FT_CELL - 2, false);
                } else {
                    fb_stroke_rect(sx - 1, sy - 1, FT_CELL + 2, FT_CELL + 2, true);
                    fb_stroke_rect(sx - 2, sy - 2, FT_CELL + 4, FT_CELL + 4, true);
                }
            }
        }
    }

    /* HUD: 左标题, 右谜题号 */
    fb_text(0, 0, "FUTOSHIKI", true);
    {
        char pbuf[8];
        pbuf[0] = 'P';
        pbuf[1] = 'Z';
        pbuf[2] = ' ';
        pbuf[3] = (char)('1' + ft_pz);
        pbuf[4] = '/';
        pbuf[5] = (char)('0' + FT_ROM_N);
        pbuf[6] = 0;
        fb_text(CCG_W - text_width(pbuf) - 2, 0, pbuf, true);
    }
    fb_hline(0, CCG_HUD_H - 1, CCG_W, true);

    /* 胜利: HUD 区显示 + 全刷一次 */
    if (ft_over) {
        fb_fill_rect(0, 0, CCG_W, CCG_HUD_H - 1, false);
        fb_text(2, 2, "YOU WIN!", true);
        fb_text(CCG_W - text_width("OK/N:RETRY BACK:QUIT") - 2, 2,
                "OK/N:RETRY BACK:QUIT", true);
        if (!ft_over_full) {
            ft_over_full = true;
            disp_force_full();
        }
    }
}

void futoshiki_on_key(const key_event_t *ev) {
    if (ft_over) {
        if (ev->is_repeat) return;
        if (ev->key == K_OK ||
            (ev->key == K_CHAR && (ev->ch == 'n' || ev->ch == 'r')))
            ft_start_game(ft_pz);          /* RETRY: 同一题 */
        else if (ev->key == K_BACK || ev->key == K_QUIT || ev->key == K_PAUSE)
            s_exit_request = true;
        return;
    }
    switch (ev->key) {
    case K_UP:
        if (ft_cy > 0) ft_cy--;
        break;
    case K_DOWN:
        if (ft_cy < 3) ft_cy++;
        break;
    case K_LEFT:
        if (ft_cx > 0) ft_cx--;
        break;
    case K_RIGHT:
        if (ft_cx < 3) ft_cx++;
        break;
    case K_OK:
        if (ev->is_repeat) break;
        if (ft_input) {
            ft_input = false;              /* 再按 OK 取消 */
        } else {
            ft_input = true;               /* 选中当前格 */
            ft_sel = (uint8_t)(ft_cy * 4 + ft_cx);
        }
        audio_select();                    /* 选中/取消 */
        break;
    case K_DEL:
        if (ev->is_repeat) break;
        {
            int idx = ft_input ? (int)ft_sel : (int)ft_cy * 4 + (int)ft_cx;
            if (ft_given[idx] == 0) ft_cell[idx] = 0;
        }
        break;
    case K_PAUSE:                          /* P: 暂停 */
    case K_BACK: {
        if (ev->is_repeat) break;
        pause_sel_t sel;
        if (ui_pause_run(&sel)) {
            if (sel == PAUSE_RESTART) ft_start_game(ft_pz);
        } else {
            s_exit_request = true;
        }
        break;
    }
    case K_QUIT:
        if (!ev->is_repeat) s_exit_request = true;
        break;
    case K_CHAR:
        if (ev->ch == 'w') {
            if (ft_cy > 0) ft_cy--;
        } else if (ev->ch == 'a') {
            if (ft_cx > 0) ft_cx--;
        } else if (ev->ch == 's') {
            if (ft_cy < 3) ft_cy++;
        } else if (ev->ch == 'd') {
            if (ft_cx < 3) ft_cx++;
        } else if (ev->ch == 'n') {
            if (!ev->is_repeat) ft_start_game((ft_pz + 1) % FT_ROM_N);
        } else if (ev->ch == 'r') {
            if (!ev->is_repeat) ft_start_game(ft_pz);
        } else if (ev->ch >= '1' && ev->ch <= '4') {
            if (!ev->is_repeat) {
                int idx = ft_input ? (int)ft_sel : (int)ft_cy * 4 + (int)ft_cx;
                ft_place_digit(idx, (uint8_t)(ev->ch - '0'));
            }
        }
        break;
    default:
        break;
    }
}
