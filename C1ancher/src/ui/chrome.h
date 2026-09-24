#ifndef C1_UI_CHROME_H
#define C1_UI_CHROME_H

#include "pkg/text.h"
#include <stdint.h>
#include <stdio.h>

/* Shared desktop/application chrome. Content owns every pixel below y=18.
 * There is no second title bar, window border or network glyph in this bar. */
#define C1_CHROME_WIDTH 296U
#define C1_CHROME_SCREEN_HEIGHT 152U
#define C1_CHROME_HEIGHT 18U
#define C1_CHROME_INPUT_Y 119U
#define C1_CHROME_TERMINAL_COLUMNS (C1_CHROME_WIDTH / 8U)
#define C1_CHROME_TERMINAL_ROWS ((C1_CHROME_SCREEN_HEIGHT - C1_CHROME_HEIGHT) / 16U)
#define C1_CHROME_TERMINAL_INPUT_ROWS ((C1_CHROME_INPUT_Y - C1_CHROME_HEIGHT) / 16U)

static inline void c1_chrome_header(uint8_t *frame, const char *name,
                                    const char *clock, const char *battery)
{
    char right[32];
    snprintf(right, sizeof(right), "%s  %s", clock, battery);
    int width = c1pkg_text_width(right);
    if (width > 152) width = 152;
    int name_width = (int)C1_CHROME_WIDTH - width - 18;
    if (c1pkg_text_width(name) > name_width) {
        c1pkg_text(frame, 6, 0, name, name_width - 16, 1);
        c1pkg_text(frame, 6 + name_width - 16, 0, "..", 16, 1);
    } else c1pkg_text(frame, 6, 0, name, name_width, 1);
    c1pkg_text(frame, (int)C1_CHROME_WIDTH - 6 - width, 0, right, width, 1);
    for (unsigned x = 0; x < C1_CHROME_WIDTH; ++x)
        frame[(17U / 8U) * C1_CHROME_WIDTH + x] |= (uint8_t)(0x80U >> (17U % 8U));
}

#endif
