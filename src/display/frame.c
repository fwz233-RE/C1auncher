#include "display/frame.h"

#include <string.h>

void c1_display_frame_clear(uint8_t *frame, bool black)
{
    if (frame != NULL) {
        memset(frame, black ? 0xff : 0x00, C1_DISPLAY_FRAME_BYTES);
    }
}

bool c1_display_frame_set_pixel(uint8_t *frame, uint32_t x, uint32_t y, bool black)
{
    size_t offset;
    uint8_t mask;

    if (frame == NULL || x >= C1_DISPLAY_WIDTH || y >= C1_DISPLAY_HEIGHT) {
        return false;
    }

    offset = (size_t)(y / C1_DISPLAY_STRIP_HEIGHT) * C1_DISPLAY_WIDTH + x;
    mask = (uint8_t)(0x80U >> (y % C1_DISPLAY_STRIP_HEIGHT));
    if (black) {
        frame[offset] |= mask;
    } else {
        frame[offset] &= (uint8_t)~mask;
    }
    return true;
}
