/* SIMON — 4 图案记忆复刻(墨水屏 4 区域版)
 * 4 个大按钮区(2x2, 各 55x55px, 图案区分: 实心/斜纹/网格/点阵);
 * 序列随机生成, 播放时每步高亮 600ms → 玩家光标+OK 依序复刻;
 * 正确: SCORE+1 并加长序列重新播放; 错误: 结束(FAIL + 最终得分)
 * tick 100ms 驱动播放节奏与输入窗口; HUD 左 SIMON 右 SCORE n
 * 静态前缀 sm_; 零 malloc; 像素坐标一律 int
 *
 * 集成提示(help[] 最多 5 行):
 *   "SIMON", "WATCH THE SEQUENCE", "REPEAT IT IN ORDER",
 *   "EACH ROUND GROWS LONGER", "OK/N: RETRY  BACK: QUIT"
 */
#include "game.h"
#include "../config.h"
#include "../gfx/canvas.h"
#include "../gfx/font.h"
#include "../gfx/pattern.h"
#include "../platform/display.h"
#include "../platform/input.h"
#include "../platform/audio.h"
#include "../platform/led.h"
#include "../platform/time.h"
#include "../rng.h"

/* ---- 布局: 2x2 按钮区 55x55, 顶条状态行 ---- */
#define SM_BTN      55
#define SM_GAPX     16
#define SM_GAPY     10
#define SM_MX       ((CCG_W - 2 * SM_BTN - SM_GAPX) / 2)   /* 85: 水平居中 */
#define SM_MY       16          /* 状态行高度(按钮顶 y = HUD+MY) */
#define SM_ROW0     (CCG_HUD_H + SM_MY)                    /* 32 */
#define SM_ROW1     (CCG_HUD_H + SM_MY + SM_BTN + SM_GAPY) /* 97 */

/* ---- 节奏(tick 100ms) ---- */
#define SM_MAX      60          /* 序列长度上限(60 字节) */
#define SM_HL_MS    600u        /* 每步高亮时长 */
#define SM_GAP_MS   400u        /* 播放结束 → 输入窗口间隔 */
#define SM_FLASH_MS 700u        /* 错误区闪烁时长 */

/* 4 区图案: 实心 / 斜纹 / 网格 / 点阵 */
static const pat_id_t sm_pat[4] = {
    PAT_SOLID, PAT_SLASH_D, PAT_GRID, PAT_DOT_DENSE
};

enum { SM_SHOW = 0, SM_INPUT };  /* 播放序列 / 玩家输入 */

static uint8_t  sm_seq[SM_MAX];  /* 序列(区号 0-3) */
static uint8_t  sm_len;          /* 当前序列长度 */
static uint8_t  sm_state;        /* SM_SHOW / SM_INPUT */
static bool     sm_waiting;      /* 播放结束后停顿(gap) */
static uint8_t  sm_show_pos;     /* 正在高亮的序列下标 */
static uint64_t sm_show_t0;      /* 当前高亮/停顿起始时刻 */
static uint8_t  sm_in_pos;       /* 玩家已正确复刻数 */
static uint8_t  sm_cursor;       /* 输入光标区 0-3 */
static uint16_t sm_score;
static uint8_t  sm_flash;        /* 错误闪烁区, 0xFF=无 */
static uint64_t sm_flash_until;
static bool     sm_over;
static bool     sm_over_full;
static rng_t    sm_rng;

void simon_render(void);

/* 手写数字追加(无 snprintf 依赖) */
static void sm_append_u32(char *buf, unsigned *n, uint32_t v, unsigned cap) {
    char tmp[8];
    unsigned len = 0;
    if (v == 0) tmp[len++] = '0';
    while (v && len < 7) { tmp[len++] = (char)('0' + (v % 10u)); v /= 10u; }
    while (len && *n < cap) buf[(*n)++] = tmp[--len];
}

/* 区 → 左上角像素坐标 */
static void sm_zone_xy(int idx, int *x, int *y) {
    *x = SM_MX + (idx & 1) * (SM_BTN + SM_GAPX);
    *y = (idx & 2) ? SM_ROW1 : SM_ROW0;
}

/* 反色图案填充: 黑底白纹(高亮/光标/错误闪烁用) */
static void sm_tile_inv(int x, int y, int w, int h, const uint8_t *tile) {
    for (int j = 0; j < h; j++) {
        uint8_t row = tile[j & 7];
        for (int i = 0; i < w; i++)
            if (row & (1u << (i & 7)))
                fb_pixel(x + i, y + j, false);
    }
}

static void sm_draw_zone(int idx, bool invert) {
    int x, y;
    sm_zone_xy(idx, &x, &y);
    if (invert) {
        fb_fill_rect(x, y, SM_BTN, SM_BTN, true);
        sm_tile_inv(x + 3, y + 3, SM_BTN - 6, SM_BTN - 6, pat_get(sm_pat[idx]));
    } else {
        fb_fill_rect(x, y, SM_BTN, SM_BTN, false);
        fb_fill_tile(x + 3, y + 3, SM_BTN - 6, SM_BTN - 6, pat_get(sm_pat[idx]));
        fb_stroke_rect(x, y, SM_BTN, SM_BTN, true);
    }
}

/* 追加一个随机区: 避免同区连续出现 3 次(do-while 带 guard) */
static void sm_append(void) {
    uint8_t n;
    int guard = 0;
    do {
        n = (uint8_t)rng_range(&sm_rng, 4u);
    } while (sm_len >= 2 && n == sm_seq[sm_len - 1] &&
             n == sm_seq[sm_len - 2] && ++guard < 16);
    sm_seq[sm_len++] = n;
}

static void sm_new_game(void) {
    rng_seed(&sm_rng, (uint64_t)now_ms() ^ 0x51D0Du);
    sm_len = 0;
    sm_append();                 /* 序列从 1 个区开始 */
    sm_state = SM_SHOW;
    sm_show_pos = 0;
    sm_waiting = false;
    sm_show_t0 = now_ms();
    sm_in_pos = 0;
    sm_cursor = sm_seq[0];
    sm_score = 0;
    sm_flash = 0xFFu;
    sm_flash_until = 0;
    sm_over = false;
    sm_over_full = false;
    simon_render();
    disp_full();
}

void simon_enter(void) { sm_new_game(); }

void simon_tick(uint64_t now) {
    if (sm_over) {
        /* 错误闪烁到时熄灭 */
        if (sm_flash != 0xFFu && now >= sm_flash_until) sm_flash = 0xFFu;
        return;
    }
    if (sm_state == SM_SHOW) {
        uint64_t el = now - sm_show_t0;
        uint32_t th = sm_waiting ? SM_GAP_MS : SM_HL_MS;
        if (el >= th) {
            if (sm_waiting) {
                /* 播放完毕, 进入输入窗口 */
                sm_state = SM_INPUT;
                sm_in_pos = 0;
                sm_cursor = sm_seq[0];
            } else if (sm_show_pos + 1u < sm_len) {
                sm_show_pos++;
                sm_show_t0 = now;
            } else {
                sm_waiting = true;   /* 末步亮完 → 停顿 */
                sm_show_t0 = now;
            }
        }
    }
}

/* OK 确认当前光标区 */
static void sm_press(void) {
    if (sm_cursor == sm_seq[sm_in_pos]) {
        sm_score++;
        sm_in_pos++;
        audio_clear();
        if (sm_in_pos == sm_len) {
            /* 本轮复刻完成: 加长序列, 重新播放 */
            if (sm_len < SM_MAX) sm_append();
            sm_state = SM_SHOW;
            sm_show_pos = 0;
            sm_waiting = false;
            sm_show_t0 = now_ms();
        }
    } else {
        sm_over = true;
        sm_over_full = false;
        sm_flash = sm_cursor;    /* 亮出按错的区 */
        sm_flash_until = now_ms() + SM_FLASH_MS;
        audio_lose();
        led_fx_set(LED_FX_LOSE);
    }
}

void simon_render(void) {
    fb_clear(false);
    /* 四个按钮区(高亮/光标/错误闪烁 = 反色) */
    for (int i = 0; i < 4; i++) {
        bool invert = false;
        if (!sm_over) {
            if (sm_state == SM_SHOW && !sm_waiting &&
                i == (int)sm_seq[sm_show_pos])
                invert = true;
            else if (sm_state == SM_INPUT && i == (int)sm_cursor)
                invert = true;
        } else if (sm_flash != 0xFFu && i == (int)sm_flash) {
            invert = true;
        }
        sm_draw_zone(i, invert);
    }
    /* 光标: 全部格画完后加双层对比边框(输入态) */
    if (!sm_over && sm_state == SM_INPUT) {
        int x, y;
        sm_zone_xy(sm_cursor, &x, &y);
        fb_stroke_rect_thick(x - 2, y - 2, SM_BTN + 4, SM_BTN + 4, 2, true);
        fb_stroke_rect(x - 1, y - 1, SM_BTN + 2, SM_BTN + 2, false);
    }
    /* 状态行: 播放中 ROUND n, 输入中 REPEAT i/n */
    if (!sm_over) {
        char buf[24];
        unsigned n = 0;
        const char *p = (sm_state == SM_SHOW) ? "ROUND " : "REPEAT ";
        while (*p && n + 1 < sizeof(buf)) buf[n++] = *p++;
        if (sm_state == SM_SHOW) {
            sm_append_u32(buf, &n, sm_len, sizeof(buf));
        } else {
            sm_append_u32(buf, &n, (uint32_t)sm_in_pos + 1u, sizeof(buf));
            if (n + 1 < sizeof(buf)) buf[n++] = '/';
            sm_append_u32(buf, &n, sm_len, sizeof(buf));
        }
        buf[n] = 0;
        fb_text_center(SM_MY + 1, buf, true);
    }
    /* HUD 顶栏 */
    if (sm_over) {
        fb_fill_rect(0, 0, CCG_W, CCG_HUD_H - 1, false);
        char buf[24];
        unsigned n = 0;
        const char *p = "FAIL SCORE ";
        while (*p && n + 1 < sizeof(buf)) buf[n++] = *p++;
        sm_append_u32(buf, &n, sm_score, sizeof(buf));
        buf[n] = 0;
        fb_text(2, 2, buf, true);
        fb_text(CCG_W - 2 - text_width("OK/N:RETRY BACK:QUIT"), 2,
                "OK/N:RETRY BACK:QUIT", true);
        if (!sm_over_full) { sm_over_full = true; disp_force_full(); }
    } else {
        hud_draw("SIMON", sm_score);
    }
}

void simon_on_key(const key_event_t *ev) {
    bool dir = ev->key == K_UP || ev->key == K_DOWN ||
               ev->key == K_LEFT || ev->key == K_RIGHT;
    if (ev->is_repeat && !dir) return;   /* 确认键/字母忽略重复, 方向可重复 */
    if (sm_over) {
        if (ev->key == K_OK || (ev->key == K_CHAR && ev->ch == 'n'))
            simon_enter();
        else if (ev->key == K_BACK || ev->key == K_QUIT)
            s_exit_request = true;
        return;
    }
    switch (ev->key) {
    case K_UP:    if (sm_cursor >= 2u) sm_cursor -= 2; audio_tick(); break;
    case K_DOWN:  if (sm_cursor < 2u)  sm_cursor += 2; audio_tick(); break;
    case K_LEFT:  if ((sm_cursor & 1u) != 0u) sm_cursor--; audio_tick(); break;
    case K_RIGHT: if ((sm_cursor & 1u) == 0u) sm_cursor++; audio_tick(); break;
    case K_CHAR:
        switch (ev->ch) {
        case 'w': if (sm_cursor >= 2u) sm_cursor -= 2; audio_tick(); break;
        case 's': if (sm_cursor < 2u)  sm_cursor += 2; audio_tick(); break;
        case 'a': if ((sm_cursor & 1u) != 0u) sm_cursor--; audio_tick(); break;
        case 'd': if ((sm_cursor & 1u) == 0u) sm_cursor++; audio_tick(); break;
        case 'n': audio_select(); simon_enter(); break;
        default: break;
        }
        break;
    case K_OK:
        if (sm_state == SM_INPUT) { audio_select(); sm_press(); }
        break;
    case K_BACK:
    case K_PAUSE: {
        pause_sel_t sel;
        if (ui_pause_run(&sel)) {
            if (sel == PAUSE_RESTART) simon_enter();
        } else {
            s_exit_request = true;
        }
        break;
    }
    case K_QUIT: s_exit_request = true; break;
    default: break;
    }
}

void simon_exit(void) {}
