#ifndef IW_GAME_H
#define IW_GAME_H
#include <stdint.h>
#include <stdbool.h>
#define MAP_W 30
#define MAP_H 18
#define MAX_UNITS 96
#define INF_COST 32767
#define MAP_COUNT 3
#define CO_COUNT 3
typedef enum { PLAIN, FOREST, MOUNTAIN, RIVER, SEA, ROAD, CITY, FACTORY, AIRPORT, PORT, HQ, TERRAIN_COUNT } Terrain;
typedef enum { INFANTRY, MECH, RECON, TANK, ARTILLERY, ANTIAIR, FIGHTER, BOMBER, BATTLESHIP, LANDER, UNIT_TYPES } UnitType;
typedef struct { uint8_t terrain; int8_t owner; uint8_t capture; } Tile;
typedef struct { uint8_t alive,type,side,x,y,hp,fuel,ammo,acted,moved; } Unit;
typedef struct { const char *name; uint16_t price; uint8_t move,vision,fuel,ammo,min_range,max_range; } UnitDef;
typedef struct {
 uint32_t turn,money[2]; uint16_t charge[2]; uint8_t co[2],power[2];
 uint8_t side,hotseat,fog,map_id,w,h; int8_t winner;
 Tile tiles[MAP_H][MAP_W]; Unit units[MAX_UNITS]; uint8_t visible[2][MAP_H][MAP_W];
} Game;
typedef struct { int damage,counter; } Prediction;
extern const UnitDef unit_defs[UNIT_TYPES];
extern const char *terrain_names[TERRAIN_COUNT], *co_names[CO_COUNT], *map_names[MAP_COUNT];
void game_new(Game *g,int map,int co0,int co1,bool hotseat,bool fog);
int game_unit_at(const Game *g,int x,int y);
bool game_visible_unit(const Game *g,int side,int id);
void game_vision(Game *g);
void game_range(const Game *g,int id,int16_t cost[MAP_H][MAP_W],int16_t prev[MAP_H][MAP_W]);
bool game_move(Game *g,int id,int x,int y);
bool game_can_attack(const Game *g,int a,int b);
Prediction game_predict(const Game *g,int a,int b);
bool game_attack(Game *g,int a,int b,Prediction *result);
bool game_capture(Game *g,int id);
bool game_wait(Game *g,int id);
bool game_produce(Game *g,int x,int y,int type);
bool game_can_produce(const Game *g,int x,int y,int type);
bool game_power(Game *g,bool super);
void game_end_turn(Game *g);
void game_check_win(Game *g);
/* Performs one AI unit action or production/end-turn; returns true when turn ended. */
bool game_ai_step(Game *g);
int game_income(const Game *g,int side);
int game_defense(int terrain);
/* Canonical endian-independent versioned CRC save. Load preserves destination on error.
 * Save refuses to overwrite an existing corrupt file. err receives concise diagnostic. */
bool game_save(const Game *g,const char *path,char *err,unsigned cap);
bool game_load(Game *g,const char *path,char *err,unsigned cap);
#endif
