#include "font.h"
#include "canvas.h"
#include "../config.h"
#include <stddef.h>

/* 字形数据在 font_data.c(由 tools/genfont.py 生成) */
extern const uint8_t font_glyph5x7[256][7];
extern const uint8_t font_symbols[CG_COUNT][7];

static void blit_glyph(int x, int y, const uint8_t g[7], int w, bool black) {
    int i, j;
    for (j = 0; j < FONT_H; j++) {
        for (i = 0; i < w; i++) {
            if (g[j] & (1u << i)) fb_pixel(x + i, y + j, black);
        }
    }
}

void fb_text(int x, int y, const char *s, bool black) {
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        if (*p < 0x20 || *p > 0x7e) { p++; continue; }
        blit_glyph(x, y, font_glyph5x7[*p], FONT_W, black); /* 表按 ASCII 直接索引 */
        x += FONT_ADV;
        p++;
    }
}

void fb_text_inv(int x, int y, const char *s) {
    int w = text_width(s);
    fb_fill_rect(x, y, w, FONT_H, true);
    fb_text(x, y, s, false);
}

void fb_text_center(int y, const char *s, bool black) {
    int w = text_width(s);
    fb_text((CCG_W - w) / 2, y, s, black);
}

void fb_text_center_inv(int y, const char *s) {
    int w = text_width(s);
    int x = (CCG_W - w) / 2;
    fb_fill_rect(x, y, w, FONT_H, true);
    fb_text(x, y, s, false);
}

int text_width(const char *s) {
    int n = 0;
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        if (*p >= 0x20 && *p <= 0x7e) n++;
        p++;
    }
    return n * FONT_ADV;
}

void fb_text_scale2(int x, int y, const char *s, bool black) {
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        if (*p < 0x20 || *p > 0x7e) { p++; continue; }
        int i, j;
        for (j = 0; j < FONT_H; j++)
            for (i = 0; i < FONT_W; i++)
                if (font_glyph5x7[*p][j] & (1u << i)) {
                    fb_fill_rect(x + i * 2, y + j * 2, 2, 2, black);
                }
        x += FONT_ADV * 2;
        p++;
    }
}

void fb_symbol(int x, int y, int cg, bool black) {
    if (cg < 0 || cg >= CG_COUNT) return;
    blit_glyph(x, y, font_symbols[cg], FONT_W, black);
}

void fb_symbol_center(int y, int cg, bool black) {
    int w = FONT_W;
    fb_symbol((CCG_W - w) / 2, y, cg, black);
}

/* ---- CJK(12x12 子集, M3 生成) ---- */
/* 在 font_data.c 中: font_cjk_count=0 时空表, 此处直接按缺失处理 */
extern const uint8_t font_cjk_count;
extern const uint16_t font_cjk_codes[];
extern const uint8_t font_cjk_data[];

static const uint16_t *cjk_find(uint16_t code) {
    uint32_t lo = 0, hi = font_cjk_count;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (font_cjk_codes[mid] < code) lo = mid + 1;
        else hi = mid;
    }
    if (lo < font_cjk_count && font_cjk_codes[lo] == code)
        return &font_cjk_codes[lo];
    return NULL;
}

int fb_text_utf8(int x, int y, const char *s, bool black) {
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        if (*p < 0x80) {
            if (*p >= 0x20 && *p <= 0x7e)
                blit_glyph(x, y, font_glyph5x7[*p], FONT_W, black);
            x += FONT_ADV;
            p++;
            continue;
        }
        /* UTF-8 3 字节 CJK */
        if ((p[0] & 0xe0) == 0xe0 && p[1] && p[2]) {
            uint16_t code = (uint16_t)(((p[0] & 0x0f) << 12) |
                                       ((p[1] & 0x3f) << 6) | (p[2] & 0x3f));
            const uint16_t *idx = cjk_find(code);
            if (idx) {
                uint32_t n = (uint32_t)(idx - font_cjk_codes);
                const uint8_t *g = &font_cjk_data[n * FONT_CJK_H];
                int i, j;
                for (j = 0; j < FONT_CJK_H; j++)
                    for (i = 0; i < FONT_CJK_W; i++)
                        if (g[j] & (1u << i)) fb_pixel(x + i, y + j, black);
                x += FONT_CJK_ADV;
            } else {
                x += FONT_CJK_ADV; /* 未收录: 留空占位 */
            }
            p += 3;
            continue;
        }
        x += FONT_ADV;
        p++;
    }
    return x;
}

int text_width_utf8(const char *s) {
    const unsigned char *p = (const unsigned char *)s;
    int w = 0;
    while (*p) {
        if (*p < 0x80) { w += FONT_ADV; p++; }
        else if ((p[0] & 0xe0) == 0xe0 && p[1] && p[2]) { w += FONT_CJK_ADV; p += 3; }
        else p++;
    }
    return w;
}
