/* 游戏说明页 */
#ifndef CCG_HELP_H
#define CCG_HELP_H

#include "../games/game.h"

/* 显示说明页; 返回 true=进入游戏, false=返回菜单 */
bool help_run(const game_desc_t *g);

#endif
