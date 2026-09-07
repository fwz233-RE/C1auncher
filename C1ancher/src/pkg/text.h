#ifndef C1PKG_TEXT_H
#define C1PKG_TEXT_H

#include <stdint.h>

#define C1PKG_TEXT_HEIGHT 16
#define C1PKG_TEXT_FRAME_WIDTH 296
#define C1PKG_TEXT_FRAME_HEIGHT 152
#define C1PKG_TEXT_FRAME_BYTES (296 * 152 / 8)
#define C1PKG_LABEL_MAX_BYTES 40U

/* Draw NUL-terminated UTF-8, top-left origin. Font cells are 8x16 or 16x16.
 * max_width is a pixel budget, not a byte/character limit: only complete
 * glyphs fitting it are drawn. Screen edges are pixel-clipped. Glyph background
 * is transparent. black != 0 sets ink bits; black == 0 clears ink bits.
 * Frame layout: frame[(y / 8) * 296 + x], bit 0x80 >> (y % 8).
 * NULL inputs/nonpositive budget are no-ops. Malformed UTF-8 or forbidden
 * controls stop drawing at that code point; unsupported valid characters use
 * the 8-pixel replacement glyph. No allocation, locale, shaping or font I/O.
 */
void c1pkg_text(uint8_t *frame, int x, int y, const char *text,
                int max_width, int black);

/* Same decoding and glyph advances as drawing, saturated at INT_MAX.
 * NULL/empty input has width zero. Stops before invalid/control input.
 */
int c1pkg_text_width(const char *text);

/* Metadata labels: 1..40 UTF-8 BYTES, valid Unicode scalar values, no leading
 * or trailing Unicode whitespace. Reject controls, bidi/format/invisible
 * modifiers, line separators, noncharacters and U+FFFD. This matches server
 * label() policy. Printable characters outside the bundled font are allowed.
 * Input must be NUL-terminated; NULL is invalid. Never truncates metadata.
 */
/* Complete embedded font copyright/license notices, including OTA binaries. */
const char *c1pkg_font_license(void);

int c1pkg_valid_label(const char *text);

#endif
