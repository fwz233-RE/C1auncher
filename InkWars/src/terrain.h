#ifndef IW_TERRAIN_H
#define IW_TERRAIN_H
#include "game.h"
enum { TERRAIN_TILE_SIZE=24 };
/* Paint one 24x24, one-bit tile by nearest-neighbor enlargement of the
 * native motif. Invalid coordinates are ignored; screen coordinates clip.
 * Draw before overlays. No allocation or game mutation. */
void terrain_draw(const Game *g,int mapx,int mapy,int screenx,int screeny);
#endif
