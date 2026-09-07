/* 记忆翻牌 — 6x4 张, 32px 方形格; 翻回超时 1.2s; 输入驱动 + 100ms 轻 tick */
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

#define MM_W 8
#define MM_H 4
#define MM_CELL 32
#define MM_OX ((CCG_W - MM_W * MM_CELL) / 2)
#define MM_OY (CCG_HUD_H + 4)
#define MM_FLIP_MS 1200

static uint8_t m_cards[MM_H][MM_W];    /* 0=未配对图标 1..12=图标 */
static bool m_open[MM_H][MM_W];        /* 当前翻开(含已配对) */
static bool m_matched[MM_H][MM_W];     /* 已配对 */
static uint8_t m_cx, m_cy;
static int8_t m_f1x, m_f1y, m_f2x, m_f2y;   /* 当前翻开的两张(-1=无) */
static uint64_t m_flip_until;
static uint32_t m_moves;
static bool m_over, m_over_full;
static rng_t m_rng;

void memory_render(void);

static const int m_icons[CG_COUNT] = {
    CG_ARROW_UP, CG_ARROW_DN, CG_ARROW_LT, CG_ARROW_RT,
    CG_SQUARE_FILL, CG_SQUARE_OPEN, CG_FLAG, CG_MINE,
    CG_STAR, CG_CHECK, CG_CROSS, CG_HEART,
    CG_DIAMOND, CG_SPADE, CG_CLUB, CG_RING
};

/* 3x 放大图标(32px 格内清晰可见) */
static void draw_icon(int cx, int cy, int idx, bool white) {
    extern const uint8_t font_symbols[CG_COUNT][7];
    const uint8_t *g = font_symbols[idx];
    int ox = cx + (MM_CELL - FONT_W * 3) / 2;
    int oy = cy + (MM_CELL - FONT_H * 3) / 2;
    for (int j = 0; j < FONT_H; j++)
        for (int i = 0; i < FONT_W; i++)
            if (g[j] & (1u << i))
                fb_fill_rect(ox + i * 3, oy + j * 3, 3, 3, white);
}

void memory_enter(void) {
    rng_seed(&m_rng, now_ms() ^ 0x6D3);
    /* 洗牌: 12 对图标随机放置 */
    uint8_t deck[MM_W * MM_H];
    for (int i = 0; i < MM_W * MM_H; i++) deck[i] = (uint8_t)(i / 2 + 1);
    for (int i = MM_W * MM_H - 1; i > 0; i--) {
        uint32_t j = rng_range(&m_rng, (uint32_t)i + 1);
        uint8_t t = deck[i];
        deck[i] = deck[j];
        deck[j] = t;
    }
    for (int y = 0; y < MM_H; y++)
        for (int x = 0; x < MM_W; x++) {
            m_cards[y][x] = deck[y * MM_W + x];
            m_open[y][x] = false;
            m_matched[y][x] = false;
        }
    m_cx = MM_W / 2;
    m_cy = MM_H / 2;
    m_f1x = m_f1y = m_f2x = m_f2y = -1;
    m_flip_until = 0;
    m_moves = 0;
    m_over = false;
    m_over_full = false;
    memory_render();
    disp_full();
}

void memory_tick(uint64_t now) {
    if (m_f2x >= 0 && now >= m_flip_until) {
        /* 两张不匹配: 翻回 */
        m_open[m_f1y][m_f1x] = false;
        m_open[m_f2y][m_f2x] = false;
        m_f1x = m_f1y = m_f2x = m_f2y = -1;
    }
}

void memory_render(void) {
    fb_clear(false);
    for (int y = 0; y < MM_H; y++) {
        for (int x = 0; x < MM_W; x++) {
            int cx = MM_OX + x * MM_CELL;
            int cy = MM_OY + y * MM_CELL;
            bool cursor = (x == m_cx && y == m_cy);
            if (cursor) fb_fill_rect(cx, cy, MM_CELL, MM_CELL, true);
            if (m_open[y][x] || m_matched[y][x]) {
                /* 正面: 图标 */
                draw_icon(cx, cy, m_icons[m_cards[y][x] - 1], !cursor);
                if (!cursor) fb_stroke_rect(cx, cy, MM_CELL, MM_CELL, true);
            } else {
                /* 背面: 图案 + 边框 */
                fb_fill_tile(cx + 2, cy + 2, MM_CELL - 4, MM_CELL - 4, pat_get(PAT_CROSS));
                if (!cursor) fb_stroke_rect(cx, cy, MM_CELL, MM_CELL, true);
            }
            if (m_matched[y][x] && !cursor) {
                /* 配对成功: 四角点标记 */
                fb_pixel(cx + 1, cy + 1, true);
                fb_pixel(cx + MM_CELL - 2, cy + 1, true);
                fb_pixel(cx + 1, cy + MM_CELL - 2, true);
                fb_pixel(cx + MM_CELL - 2, cy + MM_CELL - 2, true);
            }
        }
    }
    /* HUD */
    fb_text(0, 0, "MEMORY", true);
    char buf[16];
    uint32_t v = m_moves;
    unsigned i = 0;
    if (v == 0) { buf[i++] = '0'; }
    while (v && i < 14) { buf[i++] = (char)('0' + v % 10); v /= 10; }
    char rev[16];
    unsigned len = i;
    for (unsigned j = 0; j < len; j++) rev[j] = buf[len - 1 - j];
    rev[len] = 0;
    int total = text_width("MOVES ") + text_width(rev);
    fb_text(CCG_W - 2 - total, 0, "MOVES", true);
    fb_text(CCG_W - 2 - total + text_width("MOVES "), 0, rev, true);
    fb_hline(0, CCG_HUD_H - 1, CCG_W, true);
    if (m_over) {
        fb_fill_rect(0, 0, CCG_W, CCG_HUD_H - 1, false);
        fb_text(2, 2, "ALL MATCHED!", true);
        fb_text(CCG_W - 2 - text_width("OK/N:RETRY BACK:QUIT"), 2,
                "OK/N:RETRY BACK:QUIT", true);
        if (!m_over_full) { m_over_full = true; disp_force_full(); }
    }
}

void memory_on_key(const key_event_t *ev) {
    if (ev->is_repeat) return;
    if (m_over) {
        if (ev->key == K_OK || (ev->key == K_CHAR && ev->ch == 'n'))
            memory_enter();
        else if (ev->key == K_BACK || ev->key == K_QUIT)
            s_exit_request = true;
        return;
    }
    switch (ev->key) {
    case K_LEFT: if (m_cx > 0) m_cx--; break;
    case K_RIGHT: if (m_cx < MM_W - 1) m_cx++; break;
    case K_UP: if (m_cy > 0) m_cy--; break;
    case K_DOWN: if (m_cy < MM_H - 1) m_cy++; break;
    case K_CHAR:
        if (ev->ch == 'a' && m_cx > 0) m_cx--;
        else if (ev->ch == 'd' && m_cx < MM_W - 1) m_cx++;
        else if (ev->ch == 'w' && m_cy > 0) m_cy--;
        else if (ev->ch == 's' && m_cy < MM_H - 1) m_cy++;
        else if (ev->ch == 'n') memory_enter();
        break;
    case K_OK:
        if (m_f2x >= 0) {
            /* 前两张还在等翻回: 提前翻回 */
            m_open[m_f1y][m_f1x] = false;
            m_open[m_f2y][m_f2x] = false;
            m_f1x = m_f1y = m_f2x = m_f2y = -1;
            break;
        }
        if (m_matched[m_cy][m_cx] || m_open[m_cy][m_cx]) break;
        if (m_f1x < 0) {
            m_f1x = (int8_t)m_cx;
            m_f1y = (int8_t)m_cy;
            m_open[m_cy][m_cx] = true;
            audio_select();
        } else {
            m_f2x = (int8_t)m_cx;
            m_f2y = (int8_t)m_cy;
            m_open[m_cy][m_cx] = true;
            m_moves++;
            if (m_cards[m_f1y][m_f1x] == m_cards[m_f2y][m_f2x]) {
                m_matched[m_f1y][m_f1x] = true;
                m_matched[m_f2y][m_f2x] = true;
                m_f1x = m_f1y = m_f2x = m_f2y = -1;
                /* 全部配对? */
                int all = 1;
                for (int y = 0; y < MM_H && all; y++)
                    for (int x = 0; x < MM_W && all; x++)
                        if (!m_matched[y][x]) all = 0;
                audio_clear();
                if (all) {
                    m_over = true;
                    audio_win();
                    led_fx_set(LED_FX_WIN);
                }
            } else {
                m_flip_until = now_ms() + MM_FLIP_MS;
                audio_error();
            }
        }
        break;
    case K_BACK: {
        pause_sel_t sel;
        if (ui_pause_run(&sel)) {
            if (sel == PAUSE_RESTART) memory_enter();
        } else {
            s_exit_request = true;
        }
        break;
    }
    case K_QUIT: s_exit_request = true; break;
    default: break;
    }
}

void memory_exit(void) {}
