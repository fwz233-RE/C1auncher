/* 六角扫雷 HEX MINES — 8x6 交错六邻网格, 15 雷, 首击安全
 * 规则同扫雷: OK 翻格 / F|DEL 插旗; 数字=周围 6 邻雷数
 * 全部非雷格翻开=WIN; 踩雷=显示全部雷 FAIL
 * 显示: 交错矩形格(奇数行右移半格)+ 六邻判定, 2x 数字/符号
 * 输入驱动: 翻格/插旗/移动=快刷; 开局/胜负=全刷 */
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
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>

#define HM_COLS   8
#define HM_ROWS   6
#define HM_CW     30
#define HM_CH     22
#define HM_OX     ((CCG_W - (HM_COLS * HM_CW + HM_CW / 2)) / 2)  /* 偶+奇行总宽 270 */
#define HM_OY     (CCG_HUD_H + 2)
#define HM_MINES  15          /* 48 格约 31% */
#define HM_TOTAL  (HM_COLS * HM_ROWS)
#define HM_MINE   7           /* 雷标记(0-6 为邻雷数) */

/* 六邻偏移(交错行): 偶数行斜邻在左, 奇数行斜邻在右
 * 邻接关系对称: 若 A 邻 B 则 B 邻 A(数字一致性依赖此) */
static const int8_t hm_d[2][6][2] = {
    { {-1, 0}, {1, 0}, {0, -1}, {0, 1}, {-1, -1}, {-1, 1} },
    { {-1, 0}, {1, 0}, {0, -1}, {0, 1}, { 1, -1}, { 1, 1} }
};

static uint8_t hm_b[HM_ROWS][HM_COLS];      /* 0-6 邻雷数, 7=雷 */
static bool hm_rep[HM_ROWS][HM_COLS];       /* 已翻开 */
static bool hm_flag[HM_ROWS][HM_COLS];      /* 插旗 */
static uint8_t hm_cx, hm_cy;
static bool hm_over, hm_won, hm_over_full;
static uint8_t hm_revealed, hm_flags;       /* 已翻开非雷格数 / 旗数 */
static rng_t hm_rng;

static int hm_adj_mines(int x, int y);
static void hm_self_check(void);
void hexmines_render(void);

/* 格 -> 屏幕坐标(奇数行右移半格) */
static void hm_cell_xy(int x, int y, int *cx, int *cy) {
    *cx = HM_OX + x * HM_CW + (y & 1) * (HM_CW / 2);
    *cy = HM_OY + y * HM_CH;
}

/* 8x8 符号 2x 放大(黑=1 白=0) */
static void hm_sym2(int x, int y, int idx, bool black) {
    const uint8_t *g = font_symbols[idx];
    for (int r = 0; r < 8; r++)
        for (int c = 0; c < 8; c++)
            if (g[r] & (1u << c))
                fb_fill_rect(x + c * 2, y + r * 2, 2, 2, black);
}

static int hm_adj_mines(int x, int y) {
    int n = 0;
    const int8_t (*d)[2] = hm_d[y & 1];
    for (int i = 0; i < 6; i++) {
        int nx = x + d[i][0], ny = y + d[i][1];
        if (nx >= 0 && nx < HM_COLS && ny >= 0 && ny < HM_ROWS &&
            hm_b[ny][nx] == HM_MINE)
            n++;
    }
    return n;
}

/* 首击后布雷: 避开 (ex,ey) 及周围 3x3(含全部六邻) */
static void hm_plant(int ex, int ey) {
    for (int y = 0; y < HM_ROWS; y++)
        for (int x = 0; x < HM_COLS; x++) hm_b[y][x] = 0;
    int placed = 0;
    int guard = 0;
    while (placed < HM_MINES && guard++ < 200) {
        int x = (int)rng_range(&hm_rng, HM_COLS);
        int y = (int)rng_range(&hm_rng, HM_ROWS);
        if (hm_b[y][x] == HM_MINE) continue;
        if (x >= ex - 1 && x <= ex + 1 && y >= ey - 1 && y <= ey + 1) continue;
        hm_b[y][x] = HM_MINE;
        placed++;
    }
    for (int y = 0; y < HM_ROWS; y++)
        for (int x = 0; x < HM_COLS; x++)
            if (hm_b[y][x] != HM_MINE) hm_b[y][x] = (uint8_t)hm_adj_mines(x, y);
}

/* 0 格洪泛翻开 */
static void hm_flood(int x, int y) {
    if (x < 0 || x >= HM_COLS || y < 0 || y >= HM_ROWS) return;
    if (hm_rep[y][x] || hm_flag[y][x]) return;
    hm_rep[y][x] = true;
    hm_revealed++;
    if (hm_b[y][x] == 0) {
        const int8_t (*d)[2] = hm_d[y & 1];
        for (int i = 0; i < 6; i++)
            hm_flood(x + d[i][0], y + d[i][1]);
    }
}

static void hm_reveal(void) {
    if (hm_rep[hm_cy][hm_cx] || hm_flag[hm_cy][hm_cx]) {
        audio_error();
        return;
    }
    if (hm_revealed == 0) hm_plant((int)hm_cx, (int)hm_cy);    /* 首击布雷 */
    if (hm_b[hm_cy][hm_cx] == HM_MINE) {
        hm_over = true;
        for (int y = 0; y < HM_ROWS; y++)
            for (int x = 0; x < HM_COLS; x++)
                if (hm_b[y][x] == HM_MINE) hm_rep[y][x] = true; /* 亮出全部雷 */
        audio_lose();
        led_fx_set(LED_FX_LOSE);
        return;
    }
    hm_flood((int)hm_cx, (int)hm_cy);
    if (hm_revealed == HM_TOTAL - HM_MINES) {
        hm_won = true;
        audio_win();
        led_fx_set(LED_FX_WIN);
    } else {
        audio_select();
    }
}

static void hm_toggle_flag(void) {
    if (hm_rep[hm_cy][hm_cx]) return;
    hm_flag[hm_cy][hm_cx] = !hm_flag[hm_cy][hm_cx];
    hm_flags += hm_flag[hm_cy][hm_cx] ? 1 : -1;
}

static void hm_move_dir(int dx, int dy) {
    int nx = (int)hm_cx + dx, ny = (int)hm_cy + dy;
    if (nx >= 0 && nx < HM_COLS && ny >= 0 && ny < HM_ROWS) {
        hm_cx = (uint8_t)nx;
        hm_cy = (uint8_t)ny;
    }
}

/* 十进制整数写 buf, 返回长度(0 特判, 同 mines.c 风格) */
static int hm_num_str(int v, char *buf, int cap) {
    char tmp[8];
    int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v > 0 && n < cap) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
    for (int i = 0; i < n; i++) buf[i] = tmp[n - 1 - i];
    buf[n] = 0;
    return n;
}

void hexmines_enter(void) {
    rng_seed(&hm_rng, now_ms() ^ 0x48C0FFEEu);
    hm_cx = HM_COLS / 2;
    hm_cy = HM_ROWS / 2;
    hm_over = false;
    hm_won = false;
    hm_over_full = false;
    hm_revealed = 0;
    hm_flags = 0;
    for (int y = 0; y < HM_ROWS; y++)
        for (int x = 0; x < HM_COLS; x++) {
            hm_b[y][x] = 0;
            hm_rep[y][x] = false;
            hm_flag[y][x] = false;
        }
    hexmines_render();
    disp_full();
}

void hexmines_render(void) {
    fb_clear(false);
    /* 网格: 已翻开白底细框+数字/雷, 旗黑底白旗, 未翻开黑底 */
    for (int y = 0; y < HM_ROWS; y++) {
        for (int x = 0; x < HM_COLS; x++) {
            int cx, cy;
            hm_cell_xy(x, y, &cx, &cy);
            if (hm_rep[y][x]) {
                fb_stroke_rect(cx, cy, HM_CW, HM_CH, true);
                if (hm_b[y][x] == HM_MINE) {
                    hm_sym2(cx + (HM_CW - 16) / 2, cy + (HM_CH - 16) / 2,
                            CG_MINE, true);
                } else if (hm_b[y][x] > 0) {
                    char d[2] = { (char)('0' + hm_b[y][x]), 0 };
                    fb_text_scale2(cx + (HM_CW - 10) / 2, cy + (HM_CH - 14) / 2,
                                   d, true);
                }
            } else if (hm_flag[y][x]) {
                fb_fill_rect(cx, cy, HM_CW, HM_CH, true);
                hm_sym2(cx + (HM_CW - 16) / 2, cy + (HM_CH - 16) / 2,
                        CG_FLAG, false);
            } else {
                fb_fill_rect(cx, cy, HM_CW, HM_CH, true);
            }
        }
    }
    /* 光标最后画(四周对称, 双重对比: 外圈反色 + 内圈同色) */
    {
        int cx, cy;
        hm_cell_xy((int)hm_cx, (int)hm_cy, &cx, &cy);
        bool cell_white = hm_rep[hm_cy][hm_cx];
        fb_stroke_rect(cx - 2, cy - 2, HM_CW + 4, HM_CH + 4, !cell_white);
        fb_stroke_rect_thick(cx - 1, cy - 1, HM_CW + 2, HM_CH + 2, 2, cell_white);
    }
    /* HUD: 左标题黑字, 右 M:剩余雷数 */
    fb_text(2, 2, "HEXMINES", true);
    {
        int left = HM_MINES - (int)hm_flags;
        if (left < 0) left = 0;
        char v[8];
        hm_num_str(left, v, 7);
        char buf[16];
        buf[0] = 'M';
        buf[1] = ':';
        int i = 2;
        for (const char *p = v; *p && i < 14; p++) buf[i++] = *p;
        buf[i] = 0;
        fb_text(CCG_W - 2 - text_width(buf), 2, buf, true);
    }
    fb_hline(0, CCG_HUD_H - 1, CCG_W, true);
    /* 终局: HUD 区两行提示(墙内不放文字) */
    if (hm_over || hm_won) {
        fb_fill_rect(0, 0, CCG_W, CCG_HUD_H - 1, false);
        fb_text(2, 2, hm_won ? "YOU WIN!" : "BOOM!", true);
        fb_text(CCG_W - 2 - text_width("OK/N:RETRY BACK:QUIT"), 2,
                "OK/N:RETRY BACK:QUIT", true);
        if (!hm_over_full) { hm_over_full = true; disp_force_full(); }
    }
}

void hexmines_on_key(const key_event_t *ev) {
    if (ev->is_repeat) {
        /* 方向键重复可响应, 其余忽略 */
        switch (ev->key) {
        case K_LEFT:  hm_move_dir(-1, 0); return;
        case K_RIGHT: hm_move_dir(1, 0); return;
        case K_UP:    hm_move_dir(0, -1); return;
        case K_DOWN:  hm_move_dir(0, 1); return;
        case K_CHAR:
            if (ev->ch == 'a') hm_move_dir(-1, 0);
            else if (ev->ch == 'd') hm_move_dir(1, 0);
            else if (ev->ch == 'w') hm_move_dir(0, -1);
            else if (ev->ch == 's') hm_move_dir(0, 1);
            return;
        default: return;
        }
    }
    if (hm_over || hm_won) {
        if (ev->key == K_OK || (ev->key == K_CHAR && ev->ch == 'n'))
            hexmines_enter();
        else if (ev->key == K_BACK || ev->key == K_QUIT)
            s_exit_request = true;
        return;
    }
    switch (ev->key) {
    case K_LEFT:  hm_move_dir(-1, 0); break;
    case K_RIGHT: hm_move_dir(1, 0); break;
    case K_UP:    hm_move_dir(0, -1); break;
    case K_DOWN:  hm_move_dir(0, 1); break;
    case K_OK:    hm_reveal(); break;
    case K_DEL:   hm_toggle_flag(); break;
    case K_QUIT:  s_exit_request = true; break;
    case K_PAUSE: {
        pause_sel_t sel;
        if (ui_pause_run(&sel)) {
            if (sel == PAUSE_RESTART) hexmines_enter();
        } else {
            s_exit_request = true;
        }
        break;
    }
    case K_BACK: {
        pause_sel_t sel;
        if (ui_pause_run(&sel)) {
            if (sel == PAUSE_RESTART) hexmines_enter();
        } else {
            s_exit_request = true;
        }
        break;
    }
    case K_CHAR:
        switch (ev->ch) {
        case 'a': hm_move_dir(-1, 0); break;
        case 'd': hm_move_dir(1, 0); break;
        case 'w': hm_move_dir(0, -1); break;
        case 's': hm_move_dir(0, 1); break;
        case 'n': hexmines_enter(); break;
        case 'f': hm_toggle_flag(); break;
        case 'v': hm_self_check(); break;
        default: break;
        }
        break;
    default: break;
    }
}

/* 自检: 校验所有已翻开数字 vs 实际邻雷数, 结果写 /dev/shm/hexmines.check */
void hm_self_check(void) {
    int fd = open("/dev/shm/hexmines.check", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    char b[64];
    int n = snprintf(b, sizeof(b), "CHECK revealed=%u flags=%u over=%d won=%d\n",
                     (unsigned)hm_revealed, (unsigned)hm_flags,
                     (int)hm_over, (int)hm_won);
    ssize_t w = write(fd, b, (size_t)n); (void)w;
    int bad = 0;
    for (int y = 0; y < HM_ROWS; y++) {
        for (int x = 0; x < HM_COLS; x++) {
            if (!hm_rep[y][x]) continue;
            if (hm_b[y][x] == HM_MINE) continue;
            int real = hm_adj_mines(x, y);
            if (hm_b[y][x] != real) {
                n = snprintf(b, sizeof(b), "BAD (%d,%d) shown=%u real=%d\n",
                             x, y, hm_b[y][x], real);
                w = write(fd, b, (size_t)n); (void)w;
                bad++;
            }
        }
    }
    n = snprintf(b, sizeof(b), bad ? "RESULT: %d BAD\n" : "RESULT: OK\n", bad);
    w = write(fd, b, (size_t)n); (void)w;
    close(fd);
}

void hexmines_tick(uint64_t now) { (void)now; }

void hexmines_exit(void) {}
