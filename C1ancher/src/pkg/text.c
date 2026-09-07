#include "text.h"

#include <limits.h>
#include <stddef.h>

#include "font_generated.h"
#include "font_license_generated.h"

const char *c1pkg_font_license(void)
{
    return (const char *)c1pkg_font_notice;
}

/* Reads continuations one at a time, stopping at NUL before touching the next
 * byte. Rejects overlong forms, surrogates, stray continuations and > U+10FFFF.
 * On error the cursor is unchanged: callers stop, never scan into a suffix. */
static int decode(const unsigned char **cursor, uint32_t *codepoint)
{
    const unsigned char *p = *cursor;
    uint32_t cp, minimum;
    unsigned int count, i;
    if (*p == 0U) return 0;
    if (*p < 0x80U) { *codepoint = *p; *cursor = p + 1; return 1; }
    if (*p >= 0xc2U && *p <= 0xdfU) { cp = *p & 0x1fU; minimum = 0x80U; count = 2U; }
    else if (*p >= 0xe0U && *p <= 0xefU) { cp = *p & 0x0fU; minimum = 0x800U; count = 3U; }
    else if (*p >= 0xf0U && *p <= 0xf4U) { cp = *p & 0x07U; minimum = 0x10000U; count = 4U; }
    else return -1;
    for (i = 1U; i < count; ++i) {
        if ((p[i] & 0xc0U) != 0x80U) return -1;
        cp = (cp << 6) | (p[i] & 0x3fU);
    }
    if (cp < minimum || cp > 0x10ffffU || (cp >= 0xd800U && cp <= 0xdfffU)) return -1;
    *cursor = p + count;
    *codepoint = cp;
    return 1;
}

/* Explicit, version-stable policy shared with server model.go. In addition to
 * C0/C1 and bidi controls, disallow format/invisible modifiers rather than
 * silently rendering deceptive labels. Ordinary internal spaces are allowed. */
static int forbidden(uint32_t cp)
{
    static const uint32_t ranges[][2] = {
        {0x0000U, 0x001fU}, {0x007fU, 0x009fU}, {0x00adU, 0x00adU},
        {0x034fU, 0x034fU}, {0x0600U, 0x0605U}, {0x061cU, 0x061cU},
        {0x06ddU, 0x06ddU}, {0x070fU, 0x070fU}, {0x0890U, 0x0891U},
        {0x08e2U, 0x08e2U}, {0x115fU, 0x1160U}, {0x17b4U, 0x17b5U},
        {0x180bU, 0x180fU}, {0x200bU, 0x200fU}, {0x2028U, 0x202eU},
        {0x2060U, 0x206fU}, {0x3164U, 0x3164U}, {0xfe00U, 0xfe0fU},
        {0xfeffU, 0xfeffU}, {0xffa0U, 0xffa0U}, {0xfff9U, 0xfffdU},
        {0x110bdU, 0x110bdU}, {0x110cdU, 0x110cdU}, {0x13430U, 0x1345fU},
        {0x1bca0U, 0x1bca3U}, {0x1d173U, 0x1d17aU}, {0xe0000U, 0xe0fffU}
    };
    size_t i;
    if ((cp >= 0xfdd0U && cp <= 0xfdefU) || (cp & 0xffffU) >= 0xfffeU) return 1;
    for (i = 0U; i < sizeof(ranges) / sizeof(ranges[0]); ++i) {
        if (cp >= ranges[i][0] && cp <= ranges[i][1]) return 1;
    }
    return 0;
}

static int whitespace(uint32_t cp)
{
    return cp == 0x20U || cp == 0x85U || cp == 0xa0U || cp == 0x1680U ||
           (cp >= 0x2000U && cp <= 0x200aU) || cp == 0x2028U || cp == 0x2029U ||
           cp == 0x202fU || cp == 0x205fU || cp == 0x3000U;
}

int c1pkg_valid_label(const char *text)
{
    const unsigned char *p = (const unsigned char *)text;
    uint32_t cp = 0U;
    size_t length = 0U;
    int first = 1;
    if (text == NULL) return 0;
    while (length <= C1PKG_LABEL_MAX_BYTES && text[length] != '\0') ++length;
    if (length == 0U || length > C1PKG_LABEL_MAX_BYTES) return 0;
    while (*p != 0U) {
        if (decode(&p, &cp) != 1 || forbidden(cp) || (first && whitespace(cp))) return 0;
        first = 0;
    }
    return !whitespace(cp);
}

static size_t glyph(uint32_t cp)
{
    size_t i;
    for (i = 0U; i < sizeof(c1pkg_font_ranges) / sizeof(c1pkg_font_ranges[0]); ++i) {
        if (cp >= c1pkg_font_ranges[i].first && cp <= c1pkg_font_ranges[i].last)
            return c1pkg_font_ranges[i].offset + cp - c1pkg_font_ranges[i].first;
    }
    /* The last range is U+FFFD; genuine input U+FFFD is rejected separately. */
    return sizeof(c1pkg_font_widths) / sizeof(c1pkg_font_widths[0]) - 1U;
}

int c1pkg_text_width(const char *text)
{
    const unsigned char *p = (const unsigned char *)text;
    uint32_t cp;
    int width = 0;
    if (text == NULL) return 0;
    while (decode(&p, &cp) == 1 && !forbidden(cp)) {
        int advance = c1pkg_font_widths[glyph(cp)];
        if (width > INT_MAX - advance) return INT_MAX;
        width += advance;
    }
    return width;
}

void c1pkg_text(uint8_t *frame, int x, int y, const char *text,
                int max_width, int black)
{
    const unsigned char *p = (const unsigned char *)text;
    uint32_t cp;
    int used = 0;
    if (frame == NULL || text == NULL || max_width <= 0 ||
        x >= C1PKG_TEXT_FRAME_WIDTH || y >= C1PKG_TEXT_FRAME_HEIGHT ||
        y <= -C1PKG_TEXT_HEIGHT) return;
    while (decode(&p, &cp) == 1 && !forbidden(cp)) {
        size_t index = glyph(cp);
        int advance = c1pkg_font_widths[index];
        int row, col;
        int64_t left = (int64_t)x + used;
        if (advance > max_width - used || left >= C1PKG_TEXT_FRAME_WIDTH) break;
        for (row = 0; row < C1PKG_TEXT_HEIGHT; ++row) {
            int64_t py = (int64_t)y + row;
            if (py < 0 || py >= C1PKG_TEXT_FRAME_HEIGHT) continue;
            for (col = 0; col < advance; ++col) {
                int64_t px = left + col;
                if (px >= 0 && px < C1PKG_TEXT_FRAME_WIDTH &&
                    (c1pkg_font_rows[index][row] & (0x8000U >> col)) != 0U) {
                    size_t offset = (size_t)(py / 8) * C1PKG_TEXT_FRAME_WIDTH + (size_t)px;
                    uint8_t mask = (uint8_t)(0x80U >> (py % 8));
                    if (black) frame[offset] |= mask;
                    else frame[offset] &= (uint8_t)~mask;
                }
            }
        }
        used += advance;
    }
}
