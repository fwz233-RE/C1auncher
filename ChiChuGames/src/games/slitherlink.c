/* SLITHERLINK (数回) — 5x5 数字格 / 6x6 格点, 26px 格(130x130 居中)
 * 输入驱动; 移动/切边=快刷; 开局/重开/胜负=全刷
 * 操作: 方向/WASD 移光标(格点); OK 切换光标右方边(最右列时切左方);
 *       DEL/SPACE 切换光标下方边(最下行时切上方); 边循环: 空→黑线→灰点→空
 *       N 下一题; R 重开; BACK 暂停; Q 退出
 * 胜利: 每格边数=数字 且 黑线构成一条不交叉不分支的闭合回路 */
#include "game.h"
#include "../config.h"
#include "../gfx/canvas.h"
#include "../gfx/font.h"
#include "../gfx/pattern.h"
#include "../platform/display.h"
#include "../platform/input.h"
#include "../platform/audio.h"
#include "../platform/led.h"
#include "../rng.h"
#include "../platform/time.h"
#include <stddef.h>

#define SL_N 5           /* 每边格数 */
#define SL_P 6           /* 每边格点数 */
#define SL_HE 30         /* 横边数 = P*N */
#define SL_E 60          /* 总边数 = 2*HE */
#define SL_CELL 26       /* 格宽 px: 5*26=130 居中 */
#define SL_OX ((int)((CCG_W - SL_N * SL_CELL) / 2))
#define SL_OY ((int)(CCG_HUD_H + (CCG_H - CCG_HUD_H - SL_N * SL_CELL) / 2))
#define SL_ROM_N 3

void slitherlink_render(void);

/* 谜题库: 25 位行主序, '0'-'4'; 每题唯一解(测试内独立回调解验证) */
static const char sl_rom[SL_ROM_N][SL_N * SL_N + 1] = {
    "2121010121210121210101212",   /* 阶梯回路 */
    "1110021210101102121011100",   /* 矩形回路 */
    "2221110112100121012121210",   /* S 形回路 */
};

static uint8_t sl_edge[SL_E];       /* 0=空 1=黑线 2=灰点(确定无) */
static uint8_t sl_digit[SL_N * SL_N]; /* 题面数字 0-4 */
static int sl_cx, sl_cy;            /* 光标格点 0..5 */
static int sl_pz;                   /* 当前题 0..2 */
static bool sl_over;                /* 胜利 */
static bool sl_over_full;           /* 胜利全刷只做一次 */
static rng_t sl_rng;

/* 边 id(稠密 0..59): 横边 hid(y,x) = y*5+x (y 0..5, x 0..4);
 * 纵边 vid(y,x) = 30 + x*5+y (x 0..5, y 0..4) */
static int sl_hid(int y, int x) { return y * SL_N + x; }
static int sl_vid(int y, int x) { return SL_HE + x * SL_N + y; }

/* 边 e 的两个格点端点 */
static void sl_endpoints(int e, int *ax, int *ay, int *bx, int *by) {
    if (e < SL_HE) {
        *ax = e % SL_N;
        *ay = e / SL_N;
        *bx = *ax + 1;
        *by = *ay;
    } else {
        int t = e - SL_HE;
        *ax = t / SL_N;
        *ay = t % SL_N;
        *bx = *ax;
        *by = *ay + 1;
    }
}

/* 胜利判定: 数字满足 + 每格点度数 0/2 + 有边且单连通(一条闭合回路) */
static bool sl_check_win(void) {
    /* 1. 数字 */
    for (int j = 0; j < SL_N; j++) {
        for (int i = 0; i < SL_N; i++) {
            int c = 0;
            if (sl_edge[sl_hid(j, i)] == 1) c++;
            if (sl_edge[sl_hid(j + 1, i)] == 1) c++;
            if (sl_edge[sl_vid(j, i)] == 1) c++;
            if (sl_edge[sl_vid(j, i + 1)] == 1) c++;
            if (c != (int)sl_digit[j * SL_N + i]) return false;
        }
    }
    /* 2. 格点度数 0 或 2(无分叉无交叉无开口) */
    for (int y = 0; y < SL_P; y++) {
        for (int x = 0; x < SL_P; x++) {
            int d = 0;
            if (y < SL_N && sl_edge[sl_vid(y, x)] == 1) d++;
            if (y > 0 && sl_edge[sl_vid(y - 1, x)] == 1) d++;
            if (x < SL_N && sl_edge[sl_hid(y, x)] == 1) d++;
            if (x > 0 && sl_edge[sl_hid(y, x - 1)] == 1) d++;
            if (d != 0 && d != 2) return false;
        }
    }
    /* 3. 非空 + 连通 */
    int n = 0;
    int first = -1;
    for (int e = 0; e < SL_E; e++) {
        if (sl_edge[e] == 1) {
            n++;
            if (first < 0) first = e;
        }
    }
    if (n == 0) return false;
    bool vis[SL_E];
    for (int e = 0; e < SL_E; e++) vis[e] = false;
    int q[SL_E];
    int head = 0, tail = 0;
    q[tail++] = first;
    vis[first] = true;
    int cnt = 0;
    while (head < tail) {
        int e = q[head++];
        cnt++;
        int ax, ay, bx, by;
        sl_endpoints(e, &ax, &ay, &bx, &by);
        for (int f = 0; f < SL_E; f++) {
            if (vis[f] || sl_edge[f] != 1) continue;
            int fx, fy, gx, gy;
            sl_endpoints(f, &fx, &fy, &gx, &gy);
            if ((fx == ax && fy == ay) || (fx == bx && fy == by) ||
                (gx == ax && gy == ay) || (gx == bx && gy == by)) {
                vis[f] = true;
                q[tail++] = f;
            }
        }
    }
    return cnt == n;
}

/* 切换一条边: 空→线→点→空; 切完判胜 */
static void sl_toggle_edge(int e) {
    uint8_t s = sl_edge[e];
    sl_edge[e] = (s == 2) ? 0 : (uint8_t)(s + 1);
    if (sl_edge[e] == 1 && sl_check_win()) {
        sl_over = true;
        audio_win();
        led_fx_set(LED_FX_WIN);
    } else {
        audio_select();
    }
}

static void sl_new_game(int pz) {
    int r = pz % SL_ROM_N;
    if (r < 0) r += SL_ROM_N;
    sl_pz = r;
    for (int i = 0; i < SL_N * SL_N; i++)
        sl_digit[i] = (uint8_t)(sl_rom[sl_pz][i] - '0');
    for (int e = 0; e < SL_E; e++) sl_edge[e] = 0;
    sl_cx = 3;
    sl_cy = 3;
    sl_over = false;
    sl_over_full = false;
}

static void sl_start_game(int pz) {
    sl_new_game(pz);
    slitherlink_render();
    disp_full();
}

void slitherlink_enter(void) {
    rng_seed(&sl_rng, now_ms());
    sl_new_game((int)rng_range(&sl_rng, (uint32_t)SL_ROM_N));
    slitherlink_render();
    disp_full();
}

void slitherlink_exit(void) {}

void slitherlink_tick(uint64_t now) { (void)now; }

void slitherlink_render(void) {
    fb_clear(false);

    /* 格子数字(含 0) */
    for (int j = 0; j < SL_N; j++) {
        for (int i = 0; i < SL_N; i++) {
            char d[2];
            d[0] = (char)('0' + sl_digit[j * SL_N + i]);
            d[1] = 0;
            fb_text_scale2(SL_OX + i * SL_CELL + (SL_CELL - 10) / 2,
                           SL_OY + j * SL_CELL + (SL_CELL - 14) / 2, d, true);
        }
    }

    /* 横边: 黑线 3px / 灰点 4x4 / 未定稀疏点 */
    for (int y = 0; y < SL_P; y++) {
        for (int x = 0; x < SL_N; x++) {
            int sx = SL_OX + x * SL_CELL;
            int sy = SL_OY + y * SL_CELL;
            uint8_t st = sl_edge[sl_hid(y, x)];
            if (st == 1) {
                fb_fill_rect(sx, sy - 1, SL_CELL, 3, true);
            } else if (st == 2) {
                fb_fill_rect(sx + SL_CELL / 2 - 2, sy - 2, 4, 4, true);
            } else {
                fb_fill_tile(sx, sy - 1, SL_CELL, 2, pat_get(PAT_DOT_SPARSE));
            }
        }
    }
    /* 纵边 */
    for (int y = 0; y < SL_N; y++) {
        for (int x = 0; x < SL_P; x++) {
            int sx = SL_OX + x * SL_CELL;
            int sy = SL_OY + y * SL_CELL;
            uint8_t st = sl_edge[sl_vid(y, x)];
            if (st == 1) {
                fb_fill_rect(sx - 1, sy, 3, SL_CELL, true);
            } else if (st == 2) {
                fb_fill_rect(sx - 2, sy + SL_CELL / 2 - 2, 4, 4, true);
            } else {
                fb_fill_tile(sx - 1, sy, 2, SL_CELL, pat_get(PAT_DOT_SPARSE));
            }
        }
    }

    /* 光标(所有边画完后): 格点白环 + 反色粗框 */
    {
        int px = SL_OX + sl_cx * SL_CELL;
        int py = SL_OY + sl_cy * SL_CELL;
        fb_stroke_rect(px - 6, py - 6, 13, 13, false);       /* 清出白环 */
        fb_stroke_rect_thick(px - 5, py - 5, 11, 11, 2, true);
    }

    /* HUD: 左标题, 右题号 */
    fb_text(0, 0, "SLITHERLINK", true);
    {
        char pbuf[8];
        pbuf[0] = 'P';
        pbuf[1] = 'Z';
        pbuf[2] = ' ';
        pbuf[3] = (char)('1' + sl_pz);
        pbuf[4] = '/';
        pbuf[5] = (char)('0' + SL_ROM_N);
        pbuf[6] = 0;
        fb_text(CCG_W - text_width(pbuf) - 2, 0, pbuf, true);
    }
    fb_hline(0, CCG_HUD_H - 1, CCG_W, true);

    /* 胜利: HUD 区显示 + 全刷一次 */
    if (sl_over) {
        fb_fill_rect(0, 0, CCG_W, CCG_HUD_H - 1, false);
        fb_text(2, 2, "YOU WIN!", true);
        fb_text(CCG_W - text_width("OK/N:RETRY BACK:QUIT") - 2, 2,
                "OK/N:RETRY BACK:QUIT", true);
        if (!sl_over_full) {
            sl_over_full = true;
            disp_force_full();
        }
    }
}

void slitherlink_on_key(const key_event_t *ev) {
    if (sl_over) {
        if (ev->is_repeat) return;
        if (ev->key == K_OK ||
            (ev->key == K_CHAR && (ev->ch == 'n' || ev->ch == 'r')))
            sl_start_game(sl_pz);          /* RETRY: 同一题 */
        else if (ev->key == K_BACK || ev->key == K_QUIT)
            s_exit_request = true;
        return;
    }
    switch (ev->key) {
    case K_UP:
        if (sl_cy > 0) sl_cy--;
        audio_tick();
        break;
    case K_DOWN:
        if (sl_cy < SL_N) sl_cy++;
        audio_tick();
        break;
    case K_LEFT:
        if (sl_cx > 0) sl_cx--;
        audio_tick();
        break;
    case K_RIGHT:
        if (sl_cx < SL_N) sl_cx++;
        audio_tick();
        break;
    case K_OK:                             /* 右方边(最右列切左方) */
        if (ev->is_repeat) break;
        sl_toggle_edge(sl_hid(sl_cy, sl_cx < SL_N ? sl_cx : sl_cx - 1));
        break;
    case K_DEL:
    case K_SPACE:                          /* 下方边(最下行切上方) */
        if (ev->is_repeat) break;
        sl_toggle_edge(sl_vid(sl_cy < SL_N ? sl_cy : sl_cy - 1, sl_cx));
        break;
    case K_CHAR:
        if (ev->ch == 'w') {
            if (sl_cy > 0) sl_cy--;
        } else if (ev->ch == 'a') {
            if (sl_cx > 0) sl_cx--;
        } else if (ev->ch == 's') {
            if (sl_cy < SL_N) sl_cy++;
        } else if (ev->ch == 'd') {
            if (sl_cx < SL_N) sl_cx++;
        } else if (ev->ch == 'n') {
            if (!ev->is_repeat) { audio_select(); sl_start_game((sl_pz + 1) % SL_ROM_N); }
        } else if (ev->ch == 'r') {
            if (!ev->is_repeat) { audio_select(); sl_start_game(sl_pz); }
        }
        break;
    case K_BACK: {
        if (ev->is_repeat) break;
        pause_sel_t sel;
        if (ui_pause_run(&sel)) {
            if (sel == PAUSE_RESTART) sl_start_game(sl_pz);
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
