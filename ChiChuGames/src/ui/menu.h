/* 主菜单 — 极客风格游戏选择器 + ABOUT */
#ifndef CCG_MENU_H
#define CCG_MENU_H

#include <stdbool.h>
#include "../games/game.h"

/* 运行主菜单; 返回选中的游戏 id, 由框架进入 */
game_id_t menu_run(void);

#endif
