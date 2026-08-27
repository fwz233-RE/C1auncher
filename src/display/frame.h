#ifndef C1_DISPLAY_FRAME_H
#define C1_DISPLAY_FRAME_H

#include <stdbool.h>
#include <stdint.h>

#define C1_DISPLAY_WIDTH 296U
#define C1_DISPLAY_HEIGHT 152U
#define C1_DISPLAY_STRIP_HEIGHT 8U
#define C1_DISPLAY_STRIP_COUNT (C1_DISPLAY_HEIGHT / C1_DISPLAY_STRIP_HEIGHT)
#define C1_DISPLAY_FRAME_BYTES (C1_DISPLAY_WIDTH * C1_DISPLAY_STRIP_COUNT)

void c1_display_frame_clear(uint8_t *frame, bool black);
bool c1_display_frame_set_pixel(uint8_t *frame, uint32_t x, uint32_t y, bool black);

#endif