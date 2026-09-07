/* 8x8 图案 tile 库 — 1bit 屏幕的"灰度"梯度(2048 等)与装饰 */
#ifndef CCG_PATTERN_H
#define CCG_PATTERN_H

#include <stdint.h>

typedef enum {
    PAT_EMPTY = 0,      /* 全 0 */
    PAT_CORNERS,        /* 四角 2x2 点 */
    PAT_DOT_SPARSE,     /* 稀疏点(4px 间隔) */
    PAT_DOT_DENSE,      /* 棋盘格(1x1 交错) */
    PAT_SLASH_S,        /* 斜线 / 4px 间隔 */
    PAT_SLASH_S2,       /* 反斜线 \ 4px 间隔 */
    PAT_SLASH_D,        /* 密斜线 // 2px 间隔 */
    PAT_HLINE,          /* 横线 2px 间隔 */
    PAT_GRID,           /* 网格 4px */
    PAT_CROSS,          /* 交叉双斜线 */
    PAT_SOLID,          /* 全 1 */
    PAT_COUNT
} pat_id_t;

/* 返回 8 字节 tile, 每字节一行, bit0=最左 */
const uint8_t *pat_get(pat_id_t id);

#endif
