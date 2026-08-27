#ifndef C1_UI_CANVAS_H
#define C1_UI_CANVAS_H

#include <stdbool.h>
#include <stdint.h>

void c1_canvas_fill_rect(uint8_t *frame,
                         uint32_t x,
                         uint32_t y,
                         uint32_t width,
                         uint32_t height,
                         bool black);
void c1_canvas_stroke_rect(uint8_t *frame,
                           uint32_t x,
                           uint32_t y,
                           uint32_t width,
                           uint32_t height,
                           uint32_t thickness,
                           bool black);
void c1_canvas_text(uint8_t *frame,
                    uint32_t x,
                    uint32_t y,
                    const char *text,
                    uint32_t scale,
                    bool black);
void c1_canvas_terminal_cell(uint8_t *frame,
                             uint32_t x,
                             uint32_t y,
                             uint32_t codepoint,
                             bool inverse,
                             bool underline,
                             bool bold);
uint32_t c1_canvas_text_width(const char *text, uint32_t scale);

#endif