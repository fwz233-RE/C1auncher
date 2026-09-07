/* 贪吃蛇 — 24x13 格, 12x10px, tick 120ms→60ms 加速 */
#include "game.h"
#include "../config.h"
#include "../gfx/canvas.h"
#include "../gfx/font.h"
#include "../platform/display.h"
#include "../platform/time.h"
#include "../platform/audio.h"
#include "../platform/led.h"
#include "../rng.h"

#define SN_W 24
#define SN_H 11
#define SN_CAP (SN_W * SN_H)
#define SN_CELL_X 12
#define SN_CELL_Y 12   /* 正方形单元格 */
#define SN_OX 4
#define SN_OY (CCG_HUD_H + 2)     /* 18: 避开 HUD 分隔线 */
#define SN_AREA_W (SN_W * SN_CELL_X)   /* 288 */
#define SN_AREA_H (SN_H * SN_CELL_Y)   /* 132 */
#define SN_BASE_TICK 700   /* 700ms 实测无跳帧 */
#define SN_MIN_TICK 450

static uint8_t s_bx[SN_CAP], s_by[SN_CAP];
static uint16_t s_len;
static uint16_t s_head;   /* 必须 16 位: SN_CAP=312>255, uint8 会在 256 步时回绕 */
static int8_t s_dir, s_next_dir;  /* 0=上 1=右 2=下 3=左 */
static uint8_t s_fx, s_fy;
static uint16_t s_score;
static uint32_t s_tick_ms;
static bool s_alive, s_paused, s_over, s_over_full, s_started;
static rng_t s_rng;

static const int8_t s_dx[4] = { 0, 1, 0, -1 };
static const int8_t s_dy[4] = { -1, 0, 1, 0 };

static bool snake_occupied(uint8_t x, uint8_t y);
static void spawn_food(void);
void snake_render(void);

void snake_enter(void) {
    rng_seed(&s_rng, now_ms() ^ 0x5EED);
    s_len = 4;
    s_head = 0;
    /* 初始: 头在 (10,6), 朝右, 身体向左排 */
    /* 环形容器: 段 k 在索引 (s_head - k + CAP) % CAP; s_head=0 时
     * 身体在 311/310/309, 否则 seg(1..3) 读到未初始化区域 */
    s_bx[0] = 10; s_by[0] = 6;             /* 头 */
    s_bx[SN_CAP - 1] = 9;  s_by[SN_CAP - 1] = 6;
    s_bx[SN_CAP - 2] = 8;  s_by[SN_CAP - 2] = 6;
    s_bx[SN_CAP - 3] = 7;  s_by[SN_CAP - 3] = 6;
    s_dir = 1; s_next_dir = 1;
    s_score = 0;
    s_tick_ms = SN_BASE_TICK;
    s_alive = true; s_paused = false; s_over = false; s_over_full = false; s_started = false;
    game_set_tick_interval(s_tick_ms);
    spawn_food();
    snake_render();
    disp_fast();   /* 前一个页面刚全刷过, 快刷无闪烁 */
}

/* 环形容器: 段 k (0=头) 在 (s_head - k + CAP) % CAP */
static uint8_t seg_x(uint16_t k) { return s_bx[(s_head + SN_CAP - k) % SN_CAP]; }
static uint8_t seg_y(uint16_t k) { return s_by[(s_head + SN_CAP - k) % SN_CAP]; }

static bool snake_occupied(uint8_t x, uint8_t y) {
    for (uint16_t k = 0; k < s_len; k++)
        if (seg_x(k) == x && seg_y(k) == y) return true;
    return false;
}

static void spawn_food(void) {
    if (s_len >= SN_CAP) return;
    uint32_t tries = 0;
    do {
        s_fx = (uint8_t)rng_range(&s_rng, SN_W);
        s_fy = (uint8_t)rng_range(&s_rng, SN_H);
        tries++;
    } while (snake_occupied(s_fx, s_fy) && tries < 200);
    if (snake_occupied(s_fx, s_fy)) {
        for (uint8_t y = 0; y < SN_H; y++) {
            for (uint8_t x = 0; x < SN_W; x++) {
                if (!snake_occupied(x, y)) {
                    s_fx = x;
                    s_fy = y;
                    return;
                }
            }
        }
    }
}

void snake_tick(uint64_t now) {
    (void)now;
    if (!s_alive || s_paused || !s_started) return;

    if (s_next_dir != s_dir) {
        /* 不允许反向 */
        if (s_next_dir != (int8_t)((s_dir + 2) % 4)) s_dir = s_next_dir;
    }
    int nx = (int)s_bx[s_head] + s_dx[s_dir];
    int ny = (int)s_by[s_head] + s_dy[s_dir];
    if (nx < 0 || ny < 0 || nx >= SN_W || ny >= SN_H) {
        s_alive = false;               /* 撞墙 */
        audio_lose();
        led_fx_set(LED_FX_LOSE);
        return;
    }
    bool eating = (nx == s_fx && ny == s_fy);
    /* 自撞检测: 排除尾部(不进食时尾部会移开) */
    uint16_t limit = eating ? s_len : (uint16_t)(s_len - 1);
    for (uint16_t k = 1; k < limit; k++) {
        if (seg_x(k) == (uint8_t)nx && seg_y(k) == (uint8_t)ny) {
            s_alive = false;           /* 自撞 */
            audio_lose();
            led_fx_set(LED_FX_LOSE);
            return;
        }
    }
    uint16_t nhead = (s_head + 1u) % SN_CAP;
    s_bx[nhead] = (uint8_t)nx;
    s_by[nhead] = (uint8_t)ny;
    s_head = nhead;
    if (eating) {
        s_len++;
        s_score += 10;
        if (s_len >= SN_CAP) {
            s_alive = false; s_over = true;   /* 吃满全盘: 胜 */
            audio_win();
            led_fx_set(LED_FX_WIN);
            return;
        }
        audio_clear();                        /* 吃到食物 */
        if (s_score % 50 == 0 && s_tick_ms > SN_MIN_TICK) {
            s_tick_ms -= 5;
            game_set_tick_interval(s_tick_ms);
        }
        spawn_food();
    }
}

void snake_render(void) {
    fb_clear(false);   /* 全帧重绘, 不清除会累积残影 */
    /* 外框 */
    fb_stroke_rect(SN_OX - 2, SN_OY - 2, SN_AREA_W + 4, SN_AREA_H + 4, true);
    /* 食物: 6x6 实心方块 */
    fb_fill_rect(SN_OX + s_fx * SN_CELL_X + 3, SN_OY + s_fy * SN_CELL_Y + 2, 6, 6, true);
    /* 蛇身(段 k>=1) + 蛇头(k=0, 带方向眼睛) */
    for (uint16_t k = 1; k < s_len; k++) {
        uint8_t x = seg_x(k), y = seg_y(k);
        fb_fill_rect(SN_OX + x * SN_CELL_X + 1, SN_OY + y * SN_CELL_Y + 1,
                     SN_CELL_X - 2, SN_CELL_Y - 2, true);
    }
    {
        int hx = SN_OX + seg_x(0) * SN_CELL_X + 1;   /* int! uint8 在列21+会溢出(281->25) */
        int hy = SN_OY + seg_y(0) * SN_CELL_Y + 1;
        fb_fill_rect(hx, hy, SN_CELL_X - 2, SN_CELL_Y - 2, true);
        /* 方向感知眼睛: 2x2 白点, 位于前进方向一侧 */
        int8_t ex[2], ey[2];
        switch (s_dir) {
        case 0: ex[0]=2; ey[0]=1; ex[1]=6; ey[1]=1; break;   /* 上: 双眼居中 */
        case 1: ex[0]=8; ey[0]=2; ex[1]=8; ey[1]=6; break;   /* 右 */
        case 2: ex[0]=2; ey[0]=8; ex[1]=6; ey[1]=8; break;   /* 下: 双眼居中 */
        default: ex[0]=1; ey[0]=2; ex[1]=1; ey[1]=6; break;  /* 左 */
        }
        fb_fill_rect(hx + ex[0], hy + ey[0], 2, 2, false);
        fb_fill_rect(hx + ex[1], hy + ey[1], 2, 2, false);
    }
    hud_draw("SNAKE", s_score);
    if (s_paused) {
        fb_text_center(9, "PAUSED [P]", true);
    } else if (!s_started) {
        fb_text_center(9, "READY - PRESS OK", true);
    }
    if (!s_alive) {
        /* 结束信息放 HUD 区两行, 墙内不放任何提示 */
        fb_fill_rect(0, 0, CCG_W, CCG_HUD_H - 1, false);
        char buf[28];
        const char *tag = s_over ? "YOU WIN " : "GAME OVER ";
        unsigned n = 0;
        while (tag[n]) { buf[n] = tag[n]; n++; }
        uint32_t sc = s_score;
        char rev[12];
        unsigned i = 0;
        if (sc == 0) { rev[i++] = '0'; }
        while (sc && i < 11) { rev[i++] = (char)('0' + sc % 10); sc /= 10; }
        buf[n++] = 'S'; buf[n++] = 'C'; buf[n++] = ':';
        while (i > 0) buf[n++] = rev[--i];
        buf[n] = 0;
        fb_text_center(2, buf, true);
        fb_text_center(10, "OK:N:RETRY  BACK:QUIT", true);
        if (!s_over_full) { s_over_full = true; disp_force_full(); }  /* 结束必全刷清残影 */
    }
}

void snake_on_key(const key_event_t *ev) {
    if (ev->is_repeat) return;
    if (!s_alive) {
        if (ev->key == K_OK || (ev->key == K_CHAR && ev->ch == 'n')) {
            snake_enter();
        } else if (ev->key == K_BACK || ev->key == K_QUIT) {
            s_exit_request = true;
        }
        return;
    }
    if (!s_started) {
        if (ev->key == K_OK || ev->key == K_SPACE) s_started = true;
        return;
    }
    switch (ev->key) {
    case K_UP: s_next_dir = 0; break;
    case K_RIGHT: s_next_dir = 1; break;
    case K_DOWN: s_next_dir = 2; break;
    case K_LEFT: s_next_dir = 3; break;
    case K_PAUSE:
        s_paused = !s_paused;
        if (s_paused) { snake_render(); disp_full(); }
        break;
    case K_CHAR:
        if (ev->ch == 'n') snake_enter();
        else if (ev->ch == 'w') s_next_dir = 0;
        else if (ev->ch == 'a') s_next_dir = 3;
        else if (ev->ch == 's') s_next_dir = 2;
        else if (ev->ch == 'd') s_next_dir = 1;
        break;
    case K_BACK: {
        pause_sel_t sel;
        if (ui_pause_run(&sel)) {
            if (sel == PAUSE_RESTART) snake_enter();
            /* PAUSE_RESUME: 继续 */
        } else {
            s_exit_request = true;
        }
        break;
    }
    case K_QUIT: s_exit_request = true; break;
    default: break;
    }
}

void snake_exit(void) {}
