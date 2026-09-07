/* 自动生成: tools/genfont.py — 请勿手改 */
#ifndef FONT_GEN_H
#define FONT_GEN_H
#include <stdint.h>
#define FONT_W 5
#define FONT_H 7
#define FONT_ADV 6            /* 5+1 间距 */
#define FONT_CJK_W 12
#define FONT_CJK_H 12
#define FONT_CJK_ADV 12
#define CG_ARROW_UP 0
#define CG_ARROW_DN 1
#define CG_ARROW_LT 2
#define CG_ARROW_RT 3
#define CG_SQUARE_FILL 4
#define CG_SQUARE_OPEN 5
#define CG_FLAG 6
#define CG_MINE 7
#define CG_STAR 8
#define CG_CHECK 9
#define CG_CROSS 10
#define CG_HEART 11
#define CG_DIAMOND 12
#define CG_SPADE 13
#define CG_CLUB 14
#define CG_RING 15
#define CG_COUNT 16
/* CJK 子集占位: 0 表示未生成(M3 由 genfont.py --cjk 生成) */
extern const uint8_t font_cjk_count;
extern const uint16_t font_cjk_codes[];
extern const uint8_t font_cjk_data[];
extern const uint8_t font_glyph5x7[256][7];
extern const uint8_t font_symbols[CG_COUNT][7];
#endif
