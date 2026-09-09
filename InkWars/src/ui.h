#ifndef IW_UI_H
#define IW_UI_H
#include "game.h"
#include "terrain.h"
enum { MAP_TILE_PX=TERRAIN_TILE_SIZE, MAP_VIEW_W=12, MAP_VIEW_H=5,
       MAP_ORIGIN_X=4, MAP_ORIGIN_Y=16, MAP_FOOTER_Y=136 };
typedef enum { UI_NONE,UI_UP,UI_DOWN,UI_LEFT,UI_RIGHT,UI_OK,UI_BACK } UiKey;
typedef enum { MAIN,SETUP,MAP,MOVE,ACTION,TARGET,PRODUCTION,TURN_MENU,RESULT,INFO,MINIMAP,HANDOFF,VICTORY,HELP } Screen;
typedef struct {
 Game game; Screen screen,return_to;
 int selection,setup_row,map_choice,co0,co1,hotseat,fog;
 int cx,cy,vx,vy,unit,target,prod[UNIT_TYPES],prod_count;
 int16_t cost[MAP_H][MAP_W],prev[MAP_H][MAP_W];
 Prediction prediction; int battle_a,battle_b;
 bool active,quit,full,dirty;
 char message[96]; const char *save_path;
} UI;
void ui_init(UI *u,const char *save_path);
void ui_key(UI *u,UiKey key);
void ui_draw(UI *u);
void ui_ai(UI *u);
bool ui_ai_pending(const UI *u);
#endif
