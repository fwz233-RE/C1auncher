#ifndef C1_UI_FOCUS_H
#define C1_UI_FOCUS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Shared four-corner brackets, independent of fonts and directional arrows.
 * Same vertical MSB-first bitmap packing as desktop and package GUI. Keeping
 * this helper inline mirrors chrome.h and avoids coupling the two renderers.
 * White ink marks the corners of an inverted row without touching its text. */
static inline void c1_ui_focus_frame(uint8_t *frame, size_t stride, size_t height,
                                      unsigned x, unsigned y, unsigned width,
                                      unsigned box_height, bool ink)
{
    if (!frame || width < 3U || box_height < 3U || x >= stride || y >= height ||
        width > stride - x || box_height > height - y) return;
    unsigned arm = width / 3U;
    if (arm > box_height / 3U) arm = box_height / 3U;
    if (arm > 4U) arm = 4U;
    for (unsigned corner = 0; corner < 4U; ++corner) {
        unsigned cx = x + (corner & 1U ? width - 1U : 0U);
        unsigned cy = y + (corner & 2U ? box_height - 1U : 0U);
        for (unsigned i = 0; i < arm; ++i) {
            unsigned px = corner & 1U ? cx - i : cx + i;
            unsigned py = corner & 2U ? cy - i : cy + i;
            size_t horizontal = (size_t)(cy / 8U) * stride + px;
            size_t vertical = (size_t)(py / 8U) * stride + cx;
            uint8_t hm = (uint8_t)(0x80U >> (cy % 8U));
            uint8_t vm = (uint8_t)(0x80U >> (py % 8U));
            if (ink) { frame[horizontal] |= hm; frame[vertical] |= vm; }
            else { frame[horizontal] &= (uint8_t)~hm; frame[vertical] &= (uint8_t)~vm; }
        }
    }
}
#endif
