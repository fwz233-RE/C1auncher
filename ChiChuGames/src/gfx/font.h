/* 文本绘制 — ASCII 5x7 + 符号表 + CJK 12x12 子集(M3) */
#ifndef CCG_FONT_H
#define CCG_FONT_H

#include <stdbool.h>
#include <stdint.h>
#include "font_gen.h"

/* ASCII 绘制 */
void fb_text(int x, int y, const char *s, bool black);
void fb_text_inv(int x, int y, const char *s);          /* 反白(深底白字) */
void fb_text_center(int y, const char *s, bool black);  /* 水平居中 */
void fb_text_center_inv(int y, const char *s);
int  text_width(const char *s);
void fb_text_scale2(int x, int y, const char *s, bool black); /* 2x 大标题 */

/* 符号绘制 */
void fb_symbol(int x, int y, int cg_index, bool black);
void fb_symbol_center(int y, int cg_index, bool black);

/* CJK: UTF-8 字符串绘制(无 CJK 字库时原样跳过 CJK 字符)。
 * 返回绘制后的 x。 */
int fb_text_utf8(int x, int y, const char *s, bool black);
int text_width_utf8(const char *s);

#endif
