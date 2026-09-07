/* 帧缓冲绘制 — 操作全局 g_fb(display.h 声明) */
#ifndef CCG_CANVAS_H
#define CCG_CANVAS_H

#include <stdbool.h>
#include <stdint.h>

/* 像素寻址公式(与 C1ancher frame.c 验证一致):
 * offset = (y>>3)*CCG_W + x;  mask = 0x80 >> (y&7);  黑=1 白=0 */
void fb_clear(bool black);
void fb_pixel(int x, int y, bool black);
void fb_hline(int x, int y, int w, bool black);
void fb_vline(int x, int y, int h, bool black);
void fb_fill_rect(int x, int y, int w, int h, bool black);
void fb_stroke_rect(int x, int y, int w, int h, bool black);
void fb_stroke_rect_thick(int x, int y, int w, int h, int t, bool black);
/* 8x8 图案平铺(见 pattern.h) */
void fb_fill_tile(int x, int y, int w, int h, const uint8_t tile[8]);
/* 位图精灵: bits 为每行 1 位(x 递增), w*h; invert=true 时 1 变白 0 变黑 */
void fb_blit_sprite(int x, int y, int w, int h, const uint8_t *bits, bool invert);

#endif
