#include "canvas.h"
#include "../config.h"

extern uint8_t g_fb[CCG_FRAME_BYTES]; /* display.c 提供 */

void fb_clear(bool black) {
    uint8_t v = black ? 0xff : 0x00;
    uint32_t i;
    for (i = 0; i < CCG_FRAME_BYTES; i++) g_fb[i] = v;
}

void fb_pixel(int x, int y, bool black) {
    if (x < 0 || y < 0 || x >= (int)CCG_W || y >= (int)CCG_H) return;
    uint32_t off = ((uint32_t)y >> 3) * CCG_W + (uint32_t)x;
    uint8_t mask = 0x80u >> ((uint32_t)y & 7u);
    if (black) g_fb[off] |= mask;
    else g_fb[off] &= (uint8_t)~mask;
}

void fb_hline(int x, int y, int w, bool black) {
    int i;
    for (i = 0; i < w; i++) fb_pixel(x + i, y, black);
}

void fb_vline(int x, int y, int h, bool black) {
    int i;
    for (i = 0; i < h; i++) fb_pixel(x, y + i, black);
}

void fb_fill_rect(int x, int y, int w, int h, bool black) {
    int i;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (w <= 0 || h <= 0) return;
    if (x + w > (int)CCG_W) w = (int)CCG_W - x;
    if (y + h > (int)CCG_H) h = (int)CCG_H - y;
    for (i = 0; i < h; i++) fb_hline(x, y + i, w, black);
}

void fb_stroke_rect(int x, int y, int w, int h, bool black) {
    fb_hline(x, y, w, black);
    fb_hline(x, y + h - 1, w, black);
    fb_vline(x, y, h, black);
    fb_vline(x + w - 1, y, h, black);
}

void fb_stroke_rect_thick(int x, int y, int w, int h, int t, bool black) {
    int i;
    for (i = 0; i < t; i++) {
        fb_hline(x + i, y + i, w - 2 * i, black);
        fb_hline(x + i, y + h - 1 - i, w - 2 * i, black);
        fb_vline(x + i, y + i, h - 2 * i, black);
        fb_vline(x + w - 1 - i, y + i, h - 2 * i, black);
    }
}

void fb_fill_tile(int x, int y, int w, int h, const uint8_t tile[8]) {
    int i, j;
    for (j = 0; j < h; j++) {
        int ty = (y + j) & 7;
        for (i = 0; i < w; i++) {
            if (tile[ty] & (1u << ((x + i) & 7))) fb_pixel(x + i, y + j, true);
        }
    }
}

void fb_blit_sprite(int x, int y, int w, int h, const uint8_t *bits, bool invert) {
    int i, j;
    for (j = 0; j < h; j++) {
        for (i = 0; i < w; i++) {
            bool set = (bits[j] >> i) & 1u;
            fb_pixel(x + i, y + j, set != invert);
        }
    }
}
