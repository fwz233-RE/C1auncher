/* LIGHTS OUT — 5x5 点灯游戏
 * 按一格翻转自身 + 上/下/左/右十字邻居; 目标: 全灭
 * 盘面生成: 从全灭态随机按压 30-60 次(保证有解)
 * HUD 顶栏: 左标题, 右 MOVES n MIN m(m=当前盘面理论最少步, GF(2) 精确求解)
 * 胜利(全灭) → HUD 显示 SOLVED!
 * 静态前缀 lo_; 像素坐标一律 int; 零 malloc
 *
 * 集成提示(help[] 最多 5 行):
 *   "LIGHTS OUT", "ARROWS: MOVE  OK: FLIP",
 *   "TURN OFF ALL 25 LIGHTS", "P: PAUSE  N: NEW", "BACK: QUIT"
 */
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

#define LO_CELL 26                        /* 5x5 格 26px → 130x130 */
#define LO_OX ((CCG_W - 5 * LO_CELL) / 2) /* 水平居中 */
#define LO_OY 18                          /* y 从 18 起 */

static uint8_t lo_g[25];      /* 1=亮 0=灭, 索引 y*5+x */
static uint8_t lo_cx, lo_cy;  /* 光标格索引 0-4 */
static uint32_t lo_moves;     /* 玩家步数 */
static uint32_t lo_min;       /* 当前盘面最少步数参考 */
static bool lo_over;          /* 全灭 → 胜利 */
static bool lo_over_full;     /* 结束全刷只做一次 */
static uint32_t lo_gens;      /* 换局计数器(种子混合) */
static rng_t lo_rng;

void lightsout_render(void);

/* 按压一格: 翻转自身 + 十字邻居 */
static void lo_press_at(int i, uint8_t *b) {
    int x = i % 5, y = i / 5;
    b[i] ^= 1;
    if (x > 0) b[i - 1] ^= 1;
    if (x < 4) b[i + 1] ^= 1;
    if (y > 0) b[i - 5] ^= 1;
    if (y < 4) b[i + 5] ^= 1;
}

/* 最少步数: 解 GF(2) 线性系统 A x = g
 * A 的第 i 行 = 按压 i 格的效果(与 lo_press_at 同源, 不会跑偏)
 * 高斯-若尔当 → 行最简形; 枚举自由变量全部赋值(5x5 盘面 nfree 恒为 2)
 * 取满足全部方程的最小重量解; 无解返回 25(游戏中不会出现) */
static uint32_t lo_solve_min(const uint8_t *g) {
    uint32_t a[25];
    int piv_cols[25];
    int free_cols[25];
    int i, j, col;

    for (i = 0; i < 25; i++) {
        uint8_t b[25] = {0};
        lo_press_at(i, b);
        uint32_t r = 0;
        for (j = 0; j < 25; j++)
            if (b[j]) r |= 1u << j;
        a[i] = r | ((uint32_t)g[i] << 25);   /* bit25 = RHS */
    }
    int piv = 0;
    for (col = 0; col < 25 && piv < 25; col++) {
        int r = -1;
        for (i = piv; i < 25; i++)
            if (a[i] & (1u << col)) { r = i; break; }
        if (r < 0) continue;
        uint32_t t = a[piv];
        a[piv] = a[r];
        a[r] = t;
        for (i = 0; i < 25; i++)
            if (i != piv && (a[i] & (1u << col))) a[i] ^= a[piv];
        piv_cols[piv++] = col;
    }
    /* 自由列 */
    uint32_t used = 0;
    for (i = 0; i < piv; i++) used |= 1u << piv_cols[i];
    int nfree = 0;
    for (col = 0; col < 25; col++)
        if (!(used & (1u << col))) free_cols[nfree++] = col;
    if (nfree > 10) nfree = 10;   /* 防御: 本盘面恒为 2 */
    /* 枚举自由变量赋值: 主元行唯一确定完整解, 非主元行做一致性过滤 */
    uint32_t best = 25;
    uint32_t limit = 1u << nfree;
    for (uint32_t fv = 0; fv < limit; fv++) {
        int ok = 1;
        for (i = piv; i < 25 && ok; i++) {
            uint32_t s = a[i] >> 25;
            for (j = 0; j < nfree; j++)
                if ((a[i] & (1u << free_cols[j])) && (fv & (1u << j))) s ^= 1u;
            if (s) ok = 0;
        }
        if (!ok) continue;
        uint32_t x = 0;
        for (j = 0; j < nfree; j++)
            if (fv & (1u << j)) x |= 1u << free_cols[j];
        for (i = 0; i < piv; i++) {
            uint32_t s = a[i] >> 25;
            for (j = 0; j < nfree; j++)
                if ((a[i] & (1u << free_cols[j])) && (fv & (1u << j))) s ^= 1u;
            if (s) x |= 1u << piv_cols[i];
        }
        uint32_t w = 0, v = x;
        while (v) { v &= v - 1; w++; }
        if (w < best) best = w;
    }
    return best;
}

/* 新局: 全灭态随机按压 30-60 次(反向生成, 保证有解) */
static void lo_new_game(void) {
    lo_gens++;
    rng_seed(&lo_rng, now_ms() ^ ((uint64_t)lo_gens * 0x9E3779B1u));
    for (;;) {
        for (int i = 0; i < 25; i++) lo_g[i] = 0;
        uint32_t presses = 30u + rng_range(&lo_rng, 31u);   /* 30..60 */
        for (uint32_t i = 0; i < presses; i++)
            lo_press_at((int)rng_range(&lo_rng, 25u), lo_g);
        lo_min = lo_solve_min(lo_g);
        if (lo_min > 0) break;   /* 避免极罕见的全灭盘 */
    }
    lo_moves = 0;
    lo_cx = 2;
    lo_cy = 2;
    lo_over = false;
    lo_over_full = false;
}

void lightsout_enter(void) {
    lo_new_game();
    lightsout_render();
    disp_full();
}

/* 手写数字追加(无 snprintf 依赖) */
static void lo_append_u32(char *buf, unsigned *n, uint32_t v, unsigned cap) {
    char tmp[12];
    unsigned len = 0;
    if (v == 0) tmp[len++] = '0';
    while (v && len < 11) { tmp[len++] = (char)('0' + v % 10u); v /= 10u; }
    while (len && *n < cap) buf[(*n)++] = tmp[--len];
}

void lightsout_render(void) {
    fb_clear(false);
    /* 5x5 格: 亮=白底+中央黑圆点, 灭=黑底 */
    for (int y = 0; y < 5; y++) {
        for (int x = 0; x < 5; x++) {
            int cx = LO_OX + x * LO_CELL;
            int cy = LO_OY + y * LO_CELL;
            if (lo_g[y * 5 + x]) {
                fb_fill_rect(cx + 8, cy + 8, 10, 10, true);
            } else {
                fb_fill_rect(cx, cy, LO_CELL, LO_CELL, true);
            }
        }
    }
    /* 格线分隔 */
    for (int i = 1; i < 5; i++) {
        fb_vline(LO_OX + i * LO_CELL, LO_OY, 5 * LO_CELL, true);
        fb_hline(LO_OX, LO_OY + i * LO_CELL, 5 * LO_CELL, true);
    }
    /* 光标: 反色 2px 边框(所有格画完后最后画) */
    {
        int ccx = LO_OX + (int)lo_cx * LO_CELL;
        int ccy = LO_OY + (int)lo_cy * LO_CELL;
        bool on = lo_g[(int)lo_cy * 5 + (int)lo_cx] != 0;
        fb_stroke_rect_thick(ccx - 2, ccy - 2, LO_CELL + 4, LO_CELL + 4, 2, on);
    }
    /* HUD 顶栏 */
    if (lo_over) {
        fb_fill_rect(0, 0, CCG_W, CCG_HUD_H - 1, false);
        fb_text(2, 2, "SOLVED!", true);
        fb_text(CCG_W - 2 - text_width("OK/N:RETRY BACK:QUIT"), 2,
                "OK/N:RETRY BACK:QUIT", true);
        if (!lo_over_full) { lo_over_full = true; disp_force_full(); }
    } else {
        fb_text(0, 0, "LIGHTS OUT", true);
        char buf[24];
        unsigned n = 0;
        const char *p = "MOVES ";
        while (*p && n < 23) buf[n++] = *p++;
        lo_append_u32(buf, &n, lo_moves, 23);
        p = " MIN ";
        while (*p && n < 23) buf[n++] = *p++;
        lo_append_u32(buf, &n, lo_min, 23);
        buf[n] = 0;
        fb_text(CCG_W - 4 - text_width(buf), 0, buf, true);
        fb_hline(0, CCG_HUD_H - 1, CCG_W, true);
    }
}

void lightsout_on_key(const key_event_t *ev) {
    bool dir = ev->key == K_UP || ev->key == K_DOWN ||
               ev->key == K_LEFT || ev->key == K_RIGHT;
    if (ev->is_repeat && !dir) return;   /* 确认键/字母忽略重复, 方向可重复 */
    if (lo_over) {
        if (ev->key == K_OK || (ev->key == K_CHAR && ev->ch == 'n'))
            lightsout_enter();
        else if (ev->key == K_BACK || ev->key == K_QUIT)
            s_exit_request = true;
        return;
    }
    switch (ev->key) {
    case K_UP: if (lo_cy > 0) { lo_cy--; if (!ev->is_repeat) audio_move(); } break;
    case K_DOWN: if (lo_cy < 4) { lo_cy++; if (!ev->is_repeat) audio_move(); } break;
    case K_LEFT: if (lo_cx > 0) { lo_cx--; if (!ev->is_repeat) audio_move(); } break;
    case K_RIGHT: if (lo_cx < 4) { lo_cx++; if (!ev->is_repeat) audio_move(); } break;
    case K_CHAR:
        switch (ev->ch) {
        case 'w': if (lo_cy > 0) { lo_cy--; audio_move(); } break;
        case 's': if (lo_cy < 4) { lo_cy++; audio_move(); } break;
        case 'a': if (lo_cx > 0) { lo_cx--; audio_move(); } break;
        case 'd': if (lo_cx < 4) { lo_cx++; audio_move(); } break;
        case 'n': lightsout_enter(); break;
        default: break;
        }
        break;
    case K_OK:
        lo_press_at((int)lo_cy * 5 + (int)lo_cx, lo_g);
        lo_moves++;
        lo_min = lo_solve_min(lo_g);
        if (lo_min == 0) {                 /* 全灭 → 胜利 */
            lo_over = true;
            audio_win();
            led_fx_set(LED_FX_WIN);
        } else {
            audio_select();                /* 翻转音 */
        }
        break;
    case K_BACK:
    case K_PAUSE: {
        pause_sel_t sel;
        if (ui_pause_run(&sel)) {
            if (sel == PAUSE_RESTART) lightsout_enter();
        } else {
            s_exit_request = true;
        }
        break;
    }
    case K_QUIT: s_exit_request = true; break;
    default: break;
    }
}

void lightsout_tick(uint64_t now) { (void)now; }
void lightsout_exit(void) {}
