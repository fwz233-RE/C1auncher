#include "ui/canvas.h"

#include "display/frame.h"

#include <stddef.h>
#include <string.h>

#define C1_GLYPH(a, b, c, d, e) \
    ((uint16_t)(((a) << 12) | ((b) << 9) | ((c) << 6) | ((d) << 3) | (e)))

static uint16_t glyph(char character)
{
    switch (character) {
    case 'a': return C1_GLYPH(0, 3, 1, 3, 3);
    case 'b': return C1_GLYPH(4, 4, 6, 5, 6);
    case 'c': return C1_GLYPH(0, 3, 4, 4, 3);
    case 'd': return C1_GLYPH(1, 1, 3, 5, 3);
    case 'e': return C1_GLYPH(0, 2, 5, 6, 3);
    case 'f': return C1_GLYPH(1, 2, 7, 2, 2);
    case 'g': return C1_GLYPH(0, 3, 5, 3, 1);
    case 'h': return C1_GLYPH(4, 4, 6, 5, 5);
    case 'i': return C1_GLYPH(2, 0, 6, 2, 7);
    case 'j': return C1_GLYPH(1, 0, 1, 5, 2);
    case 'k': return C1_GLYPH(4, 5, 6, 5, 5);
    case 'l': return C1_GLYPH(6, 2, 2, 2, 7);
    case 'm': return C1_GLYPH(0, 7, 7, 5, 5);
    case 'n': return C1_GLYPH(0, 6, 5, 5, 5);
    case 'o': return C1_GLYPH(0, 2, 5, 5, 2);
    case 'p': return C1_GLYPH(0, 6, 5, 6, 4);
    case 'q': return C1_GLYPH(0, 3, 5, 3, 1);
    case 'r': return C1_GLYPH(0, 5, 6, 4, 4);
    case 's': return C1_GLYPH(0, 3, 6, 1, 6);
    case 't': return C1_GLYPH(2, 7, 2, 2, 1);
    case 'u': return C1_GLYPH(0, 5, 5, 5, 3);
    case 'v': return C1_GLYPH(0, 5, 5, 5, 2);
    case 'w': return C1_GLYPH(0, 5, 5, 7, 5);
    case 'x': return C1_GLYPH(0, 5, 2, 5, 5);
    case 'y': return C1_GLYPH(0, 5, 5, 3, 1);
    case 'z': return C1_GLYPH(0, 7, 1, 2, 7);
    case 'A': return C1_GLYPH(2, 5, 7, 5, 5);
    case 'B': return C1_GLYPH(6, 5, 6, 5, 6);
    case 'C': return C1_GLYPH(3, 4, 4, 4, 3);
    case 'D': return C1_GLYPH(6, 5, 5, 5, 6);
    case 'E': return C1_GLYPH(7, 4, 6, 4, 7);
    case 'F': return C1_GLYPH(7, 4, 6, 4, 4);
    case 'G': return C1_GLYPH(3, 4, 5, 5, 3);
    case 'H': return C1_GLYPH(5, 5, 7, 5, 5);
    case 'I': return C1_GLYPH(7, 2, 2, 2, 7);
    case 'J': return C1_GLYPH(1, 1, 1, 5, 2);
    case 'K': return C1_GLYPH(5, 5, 6, 5, 5);
    case 'L': return C1_GLYPH(4, 4, 4, 4, 7);
    case 'M': return C1_GLYPH(5, 7, 7, 5, 5);
    case 'N': return C1_GLYPH(5, 7, 7, 7, 5);
    case 'O': return C1_GLYPH(2, 5, 5, 5, 2);
    case 'P': return C1_GLYPH(6, 5, 6, 4, 4);
    case 'Q': return C1_GLYPH(2, 5, 5, 3, 1);
    case 'R': return C1_GLYPH(6, 5, 6, 5, 5);
    case 'S': return C1_GLYPH(3, 4, 2, 1, 6);
    case 'T': return C1_GLYPH(7, 2, 2, 2, 2);
    case 'U': return C1_GLYPH(5, 5, 5, 5, 7);
    case 'V': return C1_GLYPH(5, 5, 5, 5, 2);
    case 'W': return C1_GLYPH(5, 5, 7, 7, 5);
    case 'X': return C1_GLYPH(5, 5, 2, 5, 5);
    case 'Y': return C1_GLYPH(5, 5, 2, 2, 2);
    case 'Z': return C1_GLYPH(7, 1, 2, 4, 7);
    case '0': return C1_GLYPH(7, 5, 5, 5, 7);
    case '1': return C1_GLYPH(2, 6, 2, 2, 7);
    case '2': return C1_GLYPH(6, 1, 7, 4, 7);
    case '3': return C1_GLYPH(6, 1, 3, 1, 6);
    case '4': return C1_GLYPH(5, 5, 7, 1, 1);
    case '5': return C1_GLYPH(7, 4, 6, 1, 6);
    case '6': return C1_GLYPH(3, 4, 7, 5, 7);
    case '7': return C1_GLYPH(7, 1, 2, 2, 2);
    case '8': return C1_GLYPH(7, 5, 7, 5, 7);
    case '9': return C1_GLYPH(7, 5, 7, 1, 6);
    case '-': return C1_GLYPH(0, 0, 7, 0, 0);
    case ':': return C1_GLYPH(0, 2, 0, 2, 0);
    case '.': return C1_GLYPH(0, 0, 0, 0, 2);
    case '/': return C1_GLYPH(1, 1, 2, 4, 4);
    case '%': return C1_GLYPH(5, 1, 2, 4, 5);
    case '>': return C1_GLYPH(4, 2, 1, 2, 4);
    case '<': return C1_GLYPH(1, 2, 4, 2, 1);
    case '_': return C1_GLYPH(0, 0, 0, 0, 7);
    case '@': return C1_GLYPH(2, 5, 7, 4, 3);
    case '!': return C1_GLYPH(2, 2, 2, 0, 2);
    case '#': return C1_GLYPH(5, 7, 5, 7, 5);
    case '$': return C1_GLYPH(3, 6, 2, 3, 6);
    case '&': return C1_GLYPH(2, 5, 2, 5, 3);
    case '*': return C1_GLYPH(0, 5, 2, 5, 0);
    case '+': return C1_GLYPH(0, 2, 7, 2, 0);
    case '=': return C1_GLYPH(0, 7, 0, 7, 0);
    case '?': return C1_GLYPH(6, 1, 2, 0, 2);
    case '(': return C1_GLYPH(1, 2, 2, 2, 1);
    case ')': return C1_GLYPH(4, 2, 2, 2, 4);
    case '[': return C1_GLYPH(3, 2, 2, 2, 3);
    case ']': return C1_GLYPH(6, 2, 2, 2, 6);
    case '{': return C1_GLYPH(1, 2, 6, 2, 1);
    case '}': return C1_GLYPH(4, 2, 3, 2, 4);
    case '\\': return C1_GLYPH(4, 4, 2, 1, 1);
    case '|': return C1_GLYPH(2, 2, 2, 2, 2);
    case ';': return C1_GLYPH(0, 2, 0, 2, 4);
    case ',': return C1_GLYPH(0, 0, 0, 2, 4);
    case '"': return C1_GLYPH(5, 5, 0, 0, 0);
    case '\'': return C1_GLYPH(2, 2, 0, 0, 0);
    default: return 0U;
    }
}

void c1_canvas_fill_rect(uint8_t *frame,
                         uint32_t x,
                         uint32_t y,
                         uint32_t width,
                         uint32_t height,
                         bool black)
{
    uint32_t row;

    for (row = y; row < y + height && row < C1_DISPLAY_HEIGHT; ++row) {
        uint32_t column;

        for (column = x; column < x + width && column < C1_DISPLAY_WIDTH; ++column) {
            c1_display_frame_set_pixel(frame, column, row, black);
        }
    }
}

void c1_canvas_stroke_rect(uint8_t *frame,
                           uint32_t x,
                           uint32_t y,
                           uint32_t width,
                           uint32_t height,
                           uint32_t thickness,
                           bool black)
{
    if (frame == NULL || width == 0U || height == 0U || thickness == 0U) {
        return;
    }
    c1_canvas_fill_rect(frame, x, y, width, thickness, black);
    c1_canvas_fill_rect(frame, x, y + height - thickness, width, thickness, black);
    c1_canvas_fill_rect(frame, x, y, thickness, height, black);
    c1_canvas_fill_rect(frame, x + width - thickness, y, thickness, height, black);
}

void c1_canvas_text(uint8_t *frame,
                    uint32_t x,
                    uint32_t y,
                    const char *text,
                    uint32_t scale,
                    bool black)
{
    size_t index;

    if (frame == NULL || text == NULL || scale == 0U) {
        return;
    }
    for (index = 0U; text[index] != '\0'; ++index) {
        uint16_t bits = glyph(text[index]);
        uint32_t row;

        for (row = 0U; row < 5U; ++row) {
            uint32_t column;

            for (column = 0U; column < 3U; ++column) {
                uint16_t mask = (uint16_t)(1U << (14U - (row * 3U + column)));

                if ((bits & mask) != 0U) {
                    c1_canvas_fill_rect(frame,
                                        x + (uint32_t)index * 4U * scale + column * scale,
                                        y + row * scale,
                                        scale,
                                        scale,
                                        black);
                }
            }
        }
    }
}

static const uint8_t terminal_font[95][5] = {
    {0x00,0x00,0x00,0x00,0x00},{0x00,0x00,0x5f,0x00,0x00},
    {0x00,0x07,0x00,0x07,0x00},{0x14,0x7f,0x14,0x7f,0x14},
    {0x24,0x2a,0x7f,0x2a,0x12},{0x23,0x13,0x08,0x64,0x62},
    {0x36,0x49,0x55,0x22,0x50},{0x00,0x05,0x03,0x00,0x00},
    {0x00,0x1c,0x22,0x41,0x00},{0x00,0x41,0x22,0x1c,0x00},
    {0x14,0x08,0x3e,0x08,0x14},{0x08,0x08,0x3e,0x08,0x08},
    {0x00,0x50,0x30,0x00,0x00},{0x08,0x08,0x08,0x08,0x08},
    {0x00,0x60,0x60,0x00,0x00},{0x20,0x10,0x08,0x04,0x02},
    {0x3e,0x51,0x49,0x45,0x3e},{0x00,0x42,0x7f,0x40,0x00},
    {0x42,0x61,0x51,0x49,0x46},{0x21,0x41,0x45,0x4b,0x31},
    {0x18,0x14,0x12,0x7f,0x10},{0x27,0x45,0x45,0x45,0x39},
    {0x3c,0x4a,0x49,0x49,0x30},{0x01,0x71,0x09,0x05,0x03},
    {0x36,0x49,0x49,0x49,0x36},{0x06,0x49,0x49,0x29,0x1e},
    {0x00,0x36,0x36,0x00,0x00},{0x00,0x56,0x36,0x00,0x00},
    {0x08,0x14,0x22,0x41,0x00},{0x14,0x14,0x14,0x14,0x14},
    {0x00,0x41,0x22,0x14,0x08},{0x02,0x01,0x51,0x09,0x06},
    {0x32,0x49,0x79,0x41,0x3e},{0x7e,0x11,0x11,0x11,0x7e},
    {0x7f,0x49,0x49,0x49,0x36},{0x3e,0x41,0x41,0x41,0x22},
    {0x7f,0x41,0x41,0x22,0x1c},{0x7f,0x49,0x49,0x49,0x41},
    {0x7f,0x09,0x09,0x09,0x01},{0x3e,0x41,0x49,0x49,0x7a},
    {0x7f,0x08,0x08,0x08,0x7f},{0x00,0x41,0x7f,0x41,0x00},
    {0x20,0x40,0x41,0x3f,0x01},{0x7f,0x08,0x14,0x22,0x41},
    {0x7f,0x40,0x40,0x40,0x40},{0x7f,0x02,0x0c,0x02,0x7f},
    {0x7f,0x04,0x08,0x10,0x7f},{0x3e,0x41,0x41,0x41,0x3e},
    {0x7f,0x09,0x09,0x09,0x06},{0x3e,0x41,0x51,0x21,0x5e},
    {0x7f,0x09,0x19,0x29,0x46},{0x46,0x49,0x49,0x49,0x31},
    {0x01,0x01,0x7f,0x01,0x01},{0x3f,0x40,0x40,0x40,0x3f},
    {0x1f,0x20,0x40,0x20,0x1f},{0x3f,0x40,0x38,0x40,0x3f},
    {0x63,0x14,0x08,0x14,0x63},{0x07,0x08,0x70,0x08,0x07},
    {0x61,0x51,0x49,0x45,0x43},{0x00,0x7f,0x41,0x41,0x00},
    {0x02,0x04,0x08,0x10,0x20},{0x00,0x41,0x41,0x7f,0x00},
    {0x04,0x02,0x01,0x02,0x04},{0x40,0x40,0x40,0x40,0x40},
    {0x00,0x01,0x02,0x04,0x00},{0x20,0x54,0x54,0x54,0x78},
    {0x7f,0x48,0x44,0x44,0x38},{0x38,0x44,0x44,0x44,0x20},
    {0x38,0x44,0x44,0x48,0x7f},{0x38,0x54,0x54,0x54,0x18},
    {0x08,0x7e,0x09,0x01,0x02},{0x0c,0x52,0x52,0x52,0x3e},
    {0x7f,0x08,0x04,0x04,0x78},{0x00,0x44,0x7d,0x40,0x00},
    {0x20,0x40,0x44,0x3d,0x00},{0x7f,0x10,0x28,0x44,0x00},
    {0x00,0x41,0x7f,0x40,0x00},{0x7c,0x04,0x18,0x04,0x78},
    {0x7c,0x08,0x04,0x04,0x78},{0x38,0x44,0x44,0x44,0x38},
    {0x7c,0x14,0x14,0x14,0x08},{0x08,0x14,0x14,0x18,0x7c},
    {0x7c,0x08,0x04,0x04,0x08},{0x48,0x54,0x54,0x54,0x20},
    {0x04,0x3f,0x44,0x40,0x20},{0x3c,0x40,0x40,0x20,0x7c},
    {0x1c,0x20,0x40,0x20,0x1c},{0x3c,0x40,0x30,0x40,0x3c},
    {0x44,0x28,0x10,0x28,0x44},{0x0c,0x50,0x50,0x50,0x3c},
    {0x44,0x64,0x54,0x4c,0x44},{0x00,0x08,0x36,0x41,0x00},
    {0x00,0x00,0x7f,0x00,0x00},{0x00,0x41,0x36,0x08,0x00},
    {0x08,0x04,0x08,0x10,0x08}
};

static const uint8_t *terminal_glyph(uint32_t codepoint)
{
    static const uint8_t replacement[5] = {0x7f, 0x41, 0x5d, 0x41, 0x7f};

    switch (codepoint) {
    case 0x2500U:
    case 0x2501U:
        codepoint = '-';
        break;
    case 0x2502U:
    case 0x2503U:
        codepoint = '|';
        break;
    case 0x250cU:
    case 0x2510U:
    case 0x2514U:
    case 0x2518U:
    case 0x251cU:
    case 0x2524U:
    case 0x252cU:
    case 0x2534U:
    case 0x253cU:
        codepoint = '+';
        break;
    default:
        break;
    }
    if (codepoint >= 0x20U && codepoint <= 0x7eU) {
        return terminal_font[codepoint - 0x20U];
    }
    return replacement;
}

void c1_canvas_terminal_cell(uint8_t *frame,
                             uint32_t x,
                             uint32_t y,
                             uint32_t codepoint,
                             bool inverse,
                             bool underline,
                             bool bold)
{
    const uint8_t *bits = terminal_glyph(codepoint);
    uint32_t column;

    if (frame == NULL) {
        return;
    }
    c1_canvas_fill_rect(frame, x, y, 6U, 8U, inverse);
    for (column = 0U; column < 5U; ++column) {
        uint32_t row;

        for (row = 0U; row < 7U; ++row) {
            bool set = (bits[column] & (uint8_t)(1U << row)) != 0U;

            if (underline && row == 6U) {
                set = true;
            }
            if (set) {
                c1_display_frame_set_pixel(frame, x + column, y + row, !inverse);
                if (bold && column + 1U < 5U) {
                    c1_display_frame_set_pixel(frame, x + column + 1U, y + row, !inverse);
                }
            }
        }
    }
}

uint32_t c1_canvas_text_width(const char *text, uint32_t scale)
{
    if (text == NULL || scale == 0U) {
        return 0U;
    }
    return (uint32_t)strlen(text) * 4U * scale;
}