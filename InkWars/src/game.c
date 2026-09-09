#include "game.h"
#include <limits.h>
#include <string.h>

/* Original, deliberately simplified rules, not a reproduction of another game.
 * HP is 0..100; capture strength is ceil(HP/10), against 20 capture points.
 * Damage is integer/floor at each stage:
 * base * attacker_HP / 100 * attack_percent / 100 * defense_percent / 100.
 * Defense percent = max(20, 100 - terrain_stars*10*defender_HP/100 - CO_defense).
 * Air ignores terrain. A legal nonzero attack deals at least 1 HP. No randomness.
 * CO 0: ground attack +5%; powers +20/+35%, super movement +1.
 * CO 1: vision +1; forest/mountain defense +5%; powers defense +15/+25%, vision +1/+2.
 * CO 2: production costs 90%; powers refill and heal 10/20 HP, movement +1/+2.
 * Powers cost 300/600 charge, expire at the start of the user's next turn. Both sides
 * gain actual HP damage as charge (maximum 1000). Income: 1000 per property.
 * Repair: up to 20 HP (30 for CO 2), ceil(price * repaired_HP / 100), affordable
 * HP only, then free fuel/ammo on compatible owned facilities. Infantry can
 * traverse rivers/mountains; ships use sea/ports; aircraft use all terrain.
 * LANDER is an unarmed, durable naval scout in this reduced ruleset; transport
 * loading is intentionally outside the public interface.
 */
const UnitDef unit_defs[UNIT_TYPES] = {
 {"步兵",1000,3,2,60,9,1,1}, {"机步兵",3000,2,2,50,6,1,1},
 {"侦察车",4000,8,5,80,9,1,1}, {"坦克",7000,6,3,70,9,1,1},
 {"自行火炮",6000,5,2,50,6,2,3}, {"防空车",8000,6,3,60,9,1,1},
 {"战斗机",20000,9,5,99,9,1,1}, {"轰炸机",22000,7,3,99,9,1,1},
 {"战列舰",24000,5,3,99,6,2,6}, {"侦察艇",12000,6,4,99,0,0,0}
};
const char *terrain_names[TERRAIN_COUNT] = {
 "平原","森林","山地","河流","海洋","道路","城市","工厂","机场","港口","司令部"
};
const char *co_names[CO_COUNT] = {"顾岳·突击","林岚·守望","苏澜·后勤"};
const char *map_names[MAP_COUNT] = {"青岚河谷","潮汐双湾","赤岩高地"};
static const uint8_t base_damage[UNIT_TYPES][UNIT_TYPES] = {
 {55,45,12,5,15,5,0,0,0,0}, {65,55,65,55,70,65,0,0,0,0},
 {70,65,35,8,45,10,0,0,0,0}, {75,70,85,55,70,65,0,0,10,20},
 {90,85,80,70,75,75,0,0,40,55}, {105,105,60,25,50,45,65,75,0,0},
 {0,0,0,0,0,0,55,100,0,0}, {110,110,105,95,105,95,0,0,75,95},
 {95,90,90,80,85,85,0,0,50,85}, {0,0,0,0,0,0,0,0,0,0}
};
static const int dx[4] = {0,-1,1,0}, dy[4] = {-1,0,0,1};
static bool inside(int x,int y) { return x>=0 && x<MAP_W && y>=0 && y<MAP_H; }
static bool property(int t) { return t>=CITY && t<=HQ; }
static bool air(int t) { return t==FIGHTER || t==BOMBER; }
static bool naval(int t) { return t==BATTLESHIP || t==LANDER; }
static int dist(int x,int y,int a,int b) { int p=x-a,q=y-b; return (p<0?-p:p)+(q<0?-q:q); }
static bool valid_unit(const Game *g,int id) { return g && id>=0 && id<MAX_UNITS && g->units[id].alive; }
static bool active(const Game *g,int id) {
 return valid_unit(g,id) && g->winner<0 && g->units[id].side==g->side && !g->units[id].acted;
}
static int movement(const Game *g,const Unit *u) {
 int n=unit_defs[u->type].move;
 if(g->co[u->side]==0 && g->power[u->side]==2) ++n;
 if(g->co[u->side]==2) n+=g->power[u->side];
 return n;
}
static int terrain_cost(int type,int t) {
 if(air(type)) return 1;
 if(naval(type)) return t==SEA || t==PORT ? 1 : INF_COST;
 if(t==SEA) return INF_COST;
 if(type==INFANTRY || type==MECH) {
  if(t==MOUNTAIN || t==RIVER) return type==MECH ? 1 : 2;
  return 1;
 }
 if(t==MOUNTAIN || t==RIVER) return INF_COST;
 if(t==FOREST) return type==RECON ? 3 : 2;
 return 1;
}
int game_defense(int terrain) {
 static const int stars[TERRAIN_COUNT]={1,2,4,0,0,0,3,3,3,3,4};
 return terrain>=0 && terrain<TERRAIN_COUNT ? stars[terrain] : 0;
}
int game_unit_at(const Game *g,int x,int y) {
 if(!g || !inside(x,y)) return -1;
 for(int i=0;i<MAX_UNITS;++i)
  if(g->units[i].alive && g->units[i].x==x && g->units[i].y==y) return i;
 return -1;
}
void game_vision(Game *g) {
 if(!g) return;
 memset(g->visible,g->fog?0:1,sizeof(g->visible));
 if(!g->fog) return;
 for(int s=0;s<2;++s) {
  for(int y=0;y<MAP_H;++y) for(int x=0;x<MAP_W;++x)
   if(property(g->tiles[y][x].terrain) && g->tiles[y][x].owner==s)
    for(int k=0;k<5;++k) {
     int a=x+(k<4?dx[k]:0),b=y+(k<4?dy[k]:0);
     if(inside(a,b)) g->visible[s][b][a]=1;
    }
  for(int i=0;i<MAX_UNITS;++i) {
   const Unit *u=&g->units[i];
   if(!u->alive || u->side!=s) continue;
   int radius=unit_defs[u->type].vision;
   if(g->co[s]==1) radius+=1+g->power[s];
   if((u->type==INFANTRY || u->type==MECH) && g->tiles[u->y][u->x].terrain==MOUNTAIN) radius+=2;
   for(int y=0;y<MAP_H;++y) for(int x=0;x<MAP_W;++x)
    if(dist(x,y,u->x,u->y)<=radius) g->visible[s][y][x]=1;
  }
 }
}
bool game_visible_unit(const Game *g,int side,int id) {
 if(!valid_unit(g,id) || side<0 || side>1) return false;
 const Unit *u=&g->units[id];
 if(u->side==side || !g->fog) return true;
 if(!g->visible[side][u->y][u->x]) return false;
 if(g->tiles[u->y][u->x].terrain!=FOREST || air(u->type)) return true;
 /* Seeing forest terrain does not reveal a ground unit until a friendly unit
  * is adjacent. Buildings illuminate terrain, but do not defeat concealment. */
 for(int i=0;i<MAX_UNITS;++i)
  if(g->units[i].alive && g->units[i].side==side && dist(u->x,u->y,g->units[i].x,g->units[i].y)<=1) return true;
 return false;
}
void game_range(const Game *g,int id,int16_t cost[MAP_H][MAP_W],int16_t prev[MAP_H][MAP_W]) {
 uint8_t done[MAP_H][MAP_W]={{0}};
 for(int y=0;y<MAP_H;++y) for(int x=0;x<MAP_W;++x) { cost[y][x]=INF_COST; prev[y][x]=-1; }
 if(!valid_unit(g,id)) return;
 const Unit *u=&g->units[id];
 cost[u->y][u->x]=0;
 if(u->acted || u->moved || u->side!=g->side || g->winner>=0) return;
 int limit=movement(g,u); if(limit>u->fuel) limit=u->fuel;
 for(;;) {
  int bx=-1,by=-1,best=INF_COST;
  for(int y=0;y<MAP_H;++y) for(int x=0;x<MAP_W;++x)
   if(!done[y][x] && cost[y][x]<best) { best=cost[y][x]; bx=x; by=y; }
  if(bx<0 || best>limit) break;
  done[by][bx]=1;
  for(int k=0;k<4;++k) {
   int x=bx+dx[k],y=by+dy[k]; if(!inside(x,y)) continue;
   int occ=game_unit_at(g,x,y);
   if(occ>=0 && g->units[occ].side!=u->side && game_visible_unit(g,u->side,occ)) continue;
   int n=best+terrain_cost(u->type,g->tiles[y][x].terrain);
   if(n<=limit && n<cost[y][x]) { cost[y][x]=(int16_t)n; prev[y][x]=(int16_t)(by*MAP_W+bx); }
  }
 }
}
static void reset_capture(Game *g,const Unit *u) {
 if(property(g->tiles[u->y][u->x].terrain)) g->tiles[u->y][u->x].capture=20;
}
bool game_move(Game *g,int id,int x,int y) {
 if(!active(g,id) || !inside(x,y)) return false;
 Unit *u=&g->units[id];
 if(u->moved || (u->x==x && u->y==y)) return false;
 int occupant=game_unit_at(g,x,y);
 if(occupant>=0 && (g->units[occupant].side==u->side || game_visible_unit(g,u->side,occupant))) return false;
 int16_t cost[MAP_H][MAP_W],prev[MAP_H][MAP_W],path[MAP_W*MAP_H];
 game_range(g,id,cost,prev);
 if(cost[y][x]==INF_COST) return false;
 int len=0,p=y*MAP_W+x,start=u->y*MAP_W+u->x;
 while(p!=start && len<MAP_W*MAP_H) { path[len++]=(int16_t)p; p=prev[p/MAP_W][p%MAP_W]; if(p<0) return false; }
 if(p!=start) return false;
 int nx=u->x,ny=u->y,used=0; bool ambush=false;
 for(int j=len-1;j>=0;--j) {
  int a=path[j]%MAP_W,b=path[j]/MAP_W,occ=game_unit_at(g,a,b);
  if(occ>=0 && g->units[occ].side!=u->side) { ambush=true; break; }
  /* Friendly units may be crossed, but never become the final stop. */
  if(occ<0 || occ==id) { nx=a; ny=b; used=cost[b][a]; }
 }
 if(nx!=u->x || ny!=u->y) reset_capture(g,u);
 u->x=(uint8_t)nx; u->y=(uint8_t)ny; u->fuel=(uint8_t)(u->fuel-used);
 u->moved=1; if(ambush) u->acted=1;
 game_vision(g);
 return true;
}
bool game_can_attack(const Game *g,int a,int b) {
 if(!active(g,a) || !valid_unit(g,b) || a==b) return false;
 const Unit *u=&g->units[a],*v=&g->units[b];
 if(u->side==v->side || !u->ammo || !base_damage[u->type][v->type] || !game_visible_unit(g,u->side,b)) return false;
 const UnitDef *d=&unit_defs[u->type];
 int r=dist(u->x,u->y,v->x,v->y);
 return r>=d->min_range && r<=d->max_range && !(u->moved && d->min_range>1);
}
static int damage(const Game *g,const Unit *u,const Unit *v,int hp) {
 int base=base_damage[u->type][v->type]; if(!base || hp<=0) return 0;
 int attack=100,def=0;
 if(g->co[u->side]==0 && !air(u->type) && !naval(u->type)) {
  attack+=5; if(g->power[u->side]) attack+=g->power[u->side]==1 ? 20 : 35;
 }
 int t=g->tiles[v->y][v->x].terrain;
 if(!air(v->type)) def=game_defense(t)*10*v->hp/100;
 if(g->co[v->side]==1) {
  if(!air(v->type) && (t==FOREST || t==MOUNTAIN)) def+=5;
  if(g->power[v->side]) def+=g->power[v->side]==1 ? 15 : 25;
 }
 int factor=100-def; if(factor<20) factor=20;
 int n=base*hp/100; n=n*attack/100; n=n*factor/100;
 return n<1 ? 1 : n;
}
Prediction game_predict(const Game *g,int a,int b) {
 Prediction p={0,0}; if(!game_can_attack(g,a,b)) return p;
 const Unit *u=&g->units[a],*v=&g->units[b];
 p.damage=damage(g,u,v,u->hp); if(p.damage>v->hp) p.damage=v->hp;
 if(v->hp>p.damage && v->ammo && unit_defs[v->type].min_range==1 &&
    unit_defs[v->type].max_range>=1 && dist(u->x,u->y,v->x,v->y)==1 && base_damage[v->type][u->type]) {
  p.counter=damage(g,v,u,v->hp-p.damage); if(p.counter>u->hp) p.counter=u->hp;
 }
 return p;
}
static void add_charge(Game *g,int side,int amount) {
 int n=g->charge[side]+amount; g->charge[side]=(uint16_t)(n>1000?1000:n);
}
bool game_attack(Game *g,int a,int b,Prediction *result) {
 if(result) *result=(Prediction){0,0};
 if(!game_can_attack(g,a,b)) return false;
 Prediction p=game_predict(g,a,b);
 Unit *u=&g->units[a],*v=&g->units[b];
 --u->ammo; u->acted=1; v->hp=(uint8_t)(v->hp-p.damage);
 if(p.counter) { --v->ammo; u->hp=(uint8_t)(u->hp-p.counter); }
 add_charge(g,u->side,p.damage+p.counter); add_charge(g,v->side,p.damage+p.counter);
 if(!v->hp) { reset_capture(g,v); memset(v,0,sizeof(*v)); }
 if(!u->hp) { reset_capture(g,u); memset(u,0,sizeof(*u)); }
 if(result) *result=p;
 game_vision(g); game_check_win(g); return true;
}
bool game_capture(Game *g,int id) {
 if(!active(g,id)) return false;
 Unit *u=&g->units[id]; Tile *t=&g->tiles[u->y][u->x];
 if((u->type!=INFANTRY && u->type!=MECH) || !property(t->terrain) || t->owner==u->side) return false;
 int n=(u->hp+9)/10; u->acted=1;
 if(t->capture<=n) { t->owner=(int8_t)u->side; t->capture=20; }
 else t->capture=(uint8_t)(t->capture-n);
 game_vision(g); game_check_win(g); return true;
}
bool game_wait(Game *g,int id) {
 if(!active(g,id)) return false;
 g->units[id].acted=1; return true;
}
static int price(const Game *g,int side,int type) {
 return unit_defs[type].price*(g->co[side]==2?90:100)/100;
}
bool game_can_produce(const Game *g,int x,int y,int type) {
 if(!g || g->winner>=0 || !inside(x,y) || type<0 || type>=UNIT_TYPES || game_unit_at(g,x,y)>=0) return false;
 const Tile *t=&g->tiles[y][x];
 if(t->owner!=g->side || g->money[g->side]<(uint32_t)price(g,g->side,type)) return false;
 if(!((t->terrain==FACTORY && type<=ANTIAIR) || (t->terrain==AIRPORT && air(type)) || (t->terrain==PORT && naval(type)))) return false;
 for(int i=0;i<MAX_UNITS;++i) if(!g->units[i].alive) return true;
 return false;
}
bool game_produce(Game *g,int x,int y,int type) {
 if(!game_can_produce(g,x,y,type)) return false;
 for(int i=0;i<MAX_UNITS;++i) if(!g->units[i].alive) {
  g->money[g->side]-=(uint32_t)price(g,g->side,type);
  g->units[i]=(Unit){1,(uint8_t)type,g->side,(uint8_t)x,(uint8_t)y,100,unit_defs[type].fuel,unit_defs[type].ammo,1,0};
  game_vision(g); return true;
 }
 return false;
}
bool game_power(Game *g,bool super) {
 if(!g || g->winner>=0 || g->power[g->side]) return false;
 int s=g->side,cost=super?600:300;
 if(g->charge[s]<cost) return false;
 g->charge[s]=(uint16_t)(g->charge[s]-cost); g->power[s]=super?2:1;
 if(g->co[s]==2) for(int i=0;i<MAX_UNITS;++i) {
  Unit *u=&g->units[i]; if(!u->alive || u->side!=s) continue;
  u->fuel=unit_defs[u->type].fuel; u->ammo=unit_defs[u->type].ammo;
  int hp=u->hp+(super?20:10); u->hp=(uint8_t)(hp>100?100:hp);
 }
 game_vision(g); return true;
}
int game_income(const Game *g,int side) {
 if(!g || side<0 || side>1) return 0;
 int n=0;
 for(int y=0;y<MAP_H;++y) for(int x=0;x<MAP_W;++x)
  if(property(g->tiles[y][x].terrain) && g->tiles[y][x].owner==side) n+=1000;
 return n;
}
void game_check_win(Game *g) {
 if(!g || g->winner>=0) return;
 int count[2]={0,0},hq[2]={0,0};
 for(int i=0;i<MAX_UNITS;++i) if(g->units[i].alive) ++count[g->units[i].side];
 for(int y=0;y<MAP_H;++y) for(int x=0;x<MAP_W;++x)
  if(g->tiles[y][x].terrain==HQ && g->tiles[y][x].owner>=0) ++hq[(int)g->tiles[y][x].owner];
 if(!count[0] || !hq[0]) g->winner=1;
 else if(!count[1] || !hq[1]) g->winner=0;
}
static bool repair_site(int type,int terrain) {
 if(air(type)) return terrain==AIRPORT;
 if(naval(type)) return terrain==PORT;
 return terrain==CITY || terrain==FACTORY || terrain==HQ;
}
static void begin_turn(Game *g) {
 int s=g->side;
 g->power[s]=0;
 uint32_t income=(uint32_t)game_income(g,s);
 g->money[s]=g->money[s]>1000000000U-income ? 1000000000U : g->money[s]+income;
 for(int i=0;i<MAX_UNITS;++i) {
  Unit *u=&g->units[i]; if(!u->alive || u->side!=s) continue;
  u->acted=0; u->moved=0;
  Tile *t=&g->tiles[u->y][u->x];
  if(t->owner==s && repair_site(u->type,t->terrain)) {
   int hp=100-u->hp,limit=g->co[s]==2?30:20;
   if(hp>limit) hp=limit;
   int per=price(g,s,u->type);
   while(hp>0 && (uint32_t)((per*hp+99)/100)>g->money[s]) --hp;
   g->money[s]-=(uint32_t)((per*hp+99)/100); u->hp=(uint8_t)(u->hp+hp);
   u->fuel=unit_defs[u->type].fuel; u->ammo=unit_defs[u->type].ammo;
  }
 }
 game_vision(g);
}
void game_end_turn(Game *g) {
 if(!g || g->winner>=0) return;
 game_check_win(g); if(g->winner>=0) return;
 g->side=(uint8_t)(1-g->side);
 if(g->turn<1000000000U) ++g->turn;
 begin_turn(g); game_check_win(g);
}
static void put(Game *g,int x,int y,int terrain,int owner) {
 g->tiles[y][x]=(Tile){(uint8_t)terrain,(int8_t)owner,20};
}
static void spawn(Game *g,int id,int side,int type,int x,int y) {
 g->units[id]=(Unit){1,(uint8_t)type,(uint8_t)side,(uint8_t)x,(uint8_t)y,100,unit_defs[type].fuel,unit_defs[type].ammo,0,0};
}
void game_new(Game *g,int map,int co0,int co1,bool hotseat,bool fog) {
 if(!g) return;
 memset(g,0,sizeof(*g));
 g->w=MAP_W; g->h=MAP_H; g->winner=-1; g->turn=1;
 g->map_id=(uint8_t)(map>=0 && map<MAP_COUNT?map:0);
 g->co[0]=(uint8_t)(co0>=0 && co0<CO_COUNT?co0:0);
 g->co[1]=(uint8_t)(co1>=0 && co1<CO_COUNT?co1:0);
 g->hotseat=(uint8_t)hotseat; g->fog=(uint8_t)fog;
 g->money[0]=g->money[1]=10000;
 for(int y=0;y<MAP_H;++y) for(int x=0;x<MAP_W;++x) {
  int t=PLAIN;
  if(g->map_id==0) {
   if((x*7+y*11)%19<4) t=FOREST;
   if((x>=7 && x<=10 && y>=2 && y<=5) || (x>=19 && x<=22 && y>=12 && y<=15)) t=MOUNTAIN;
   if(x==14 || x==15) t=RIVER;
   if((y==0 || y==17) && x>=12 && x<=17) t=SEA;
   if((y==4 || y==9 || y==14) || (x==5 && y>3 && y<15) || (x==24 && y>3 && y<15)) t=ROAD;
  } else if(g->map_id==1) {
   if((x*3+y*7)%17<4) t=FOREST;
   if(y<4 || y>13 || x==14 || x==15) t=SEA;
   if((x>=5 && x<=7 && y==6) || (x>=22 && x<=24 && y==11)) t=MOUNTAIN;
   if(y==8 || y==9) t=ROAD;
   if(y==6 && x>=10 && x<=12) t=RIVER;
  } else {
   if((x*11+y*3)%13<4) t=FOREST;
   if((y==4 || y==13) && x>4 && x<25 && x%7!=0) t=MOUNTAIN;
   if(y==8) t=RIVER;
   if(x==6 || x==15 || x==23 || y==9) t=ROAD;
   if((x<3 && y<3) || (x>26 && y>14)) t=SEA;
  }
  /* A continuous outer coast connects both ports on every map. */
  if(x==0 || x==MAP_W-1 || y==0 || y==MAP_H-1) t=SEA;
  put(g,x,y,t,-1);
 }
 for(int s=0;s<2;++s) {
  int hx=s?27:2,fx=s?25:4;
  put(g,hx,9,HQ,s); put(g,fx,s?10:8,FACTORY,s);
  put(g,fx,s?6:11,AIRPORT,s); put(g,s?26:3,s?12:6,CITY,s);
  put(g,s?22:7,9,CITY,s);
  put(g,s?20:9,s?10:7,FACTORY,-1);
  put(g,s?18:11,s?7:10,CITY,-1);
  put(g,s?23:6,s?5:12,CITY,-1);
  /* Guaranteed land deployment, even where the highland river crosses. */
  put(g,s?26:3,9,ROAD,-1); put(g,s?25:4,9,ROAD,-1);
  put(g,s?26:3,s?8:10,PLAIN,-1);
  spawn(g,s*3,s,INFANTRY,s?26:3,9);
  spawn(g,s*3+1,s,TANK,s?25:4,9);
  spawn(g,s*3+2,s,INFANTRY,s?26:3,s?8:10);
 }
 if(g->map_id==0) { put(g,12,1,PORT,0); put(g,17,16,PORT,1); put(g,12,0,SEA,-1); put(g,17,17,SEA,-1); }
 else if(g->map_id==1) { put(g,8,4,PORT,0); put(g,21,13,PORT,1); }
 else { put(g,2,3,PORT,0); put(g,27,14,PORT,1); }
 put(g,14,9,CITY,-1); put(g,15,9,CITY,-1);
 begin_turn(g);
}
/* AI never reads concealed enemy units for evaluation. Terrain and HQ locations
 * are public; ordinary enemy/neutral ownership is consulted only in vision.
 * Each call consumes an unacted unit, occupies a production site, or ends turn.
 * Thus even a blocked map has at most MAX_UNITS+MAP_W*MAP_H+1 calls per turn. */
static int best_attack(const Game *g,int id) {
 int target=-1,best=INT_MIN;
 for(int b=0;b<MAX_UNITS;++b) if(game_can_attack(g,id,b)) {
  Prediction p=game_predict(g,id,b);
  int score=p.damage*unit_defs[g->units[b].type].price/100-p.counter*unit_defs[g->units[id].type].price/100;
  if(p.damage>=g->units[b].hp) score+=2000;
  if(score>best) { best=score; target=b; }
 }
 return target;
}
static int goal_score(const Game *g,const Unit *u,int x,int y) {
 int best=100000;
 bool capturer=u->type==INFANTRY || u->type==MECH;
 for(int b=0;b<MAX_UNITS;++b) {
  const Unit *v=&g->units[b];
  if(!v->alive || v->side==u->side || !game_visible_unit(g,u->side,b) || !base_damage[u->type][v->type]) continue;
  int d=dist(x,y,v->x,v->y),range=unit_defs[u->type].max_range;
  int n=(d>range?d-range:range-d)*12+12;
  if(d>=unit_defs[u->type].min_range && d<=range) n=0;
  if(n<best) best=n;
 }
 for(int b=0;b<MAP_H;++b) for(int a=0;a<MAP_W;++a) {
  const Tile *t=&g->tiles[b][a];
  if(t->terrain==HQ && t->owner!=u->side) {
   int n=dist(x,y,a,b)*10+(capturer?5:30); if(n<best) best=n;
  } else if(capturer && property(t->terrain) && g->visible[u->side][b][a] && t->owner!=u->side) {
   int n=dist(x,y,a,b)*10; if(n<best) best=n;
  }
 }
 return best;
}
bool game_ai_step(Game *g) {
 if(!g || g->winner>=0) return true;
 if(g->charge[g->side]>=600 && !g->power[g->side]) (void)game_power(g,true);
 for(int i=0;i<MAX_UNITS;++i) if(active(g,i)) {
  Unit *u=&g->units[i];
  if(game_capture(g,i)) return g->winner>=0;
  int target=best_attack(g,i);
  if(target>=0) { (void)game_attack(g,i,target,0); return g->winner>=0; }
  int16_t cost[MAP_H][MAP_W],prev[MAP_H][MAP_W]; game_range(g,i,cost,prev);
  int bx=u->x,by=u->y,best=goal_score(g,u,bx,by)*100;
  for(int y=0;y<MAP_H;++y) for(int x=0;x<MAP_W;++x) {
   if(cost[y][x]==INF_COST || (x==u->x && y==u->y)) continue;
   int occ=game_unit_at(g,x,y);
   if(occ>=0 && (g->units[occ].side==u->side || game_visible_unit(g,u->side,occ))) continue;
   int score=goal_score(g,u,x,y)*100+cost[y][x];
   if(score<best) { best=score; bx=x; by=y; }
  }
  if(bx!=u->x || by!=u->y) (void)game_move(g,i,bx,by);
  if(active(g,i)) {
   target=best_attack(g,i);
   if(target>=0) (void)game_attack(g,i,target,0);
   else if(!game_capture(g,i)) (void)game_wait(g,i);
  }
  return g->winner>=0;
 }
 /* Maintain an infantry presence, then choose a useful affordable combat unit. */
 int infantry=0,visible_air=0;
 for(int i=0;i<MAX_UNITS;++i) if(g->units[i].alive) {
  if(g->units[i].side==g->side && (g->units[i].type==INFANTRY || g->units[i].type==MECH)) ++infantry;
  if(g->units[i].side!=g->side && game_visible_unit(g,g->side,i) && air(g->units[i].type)) ++visible_air;
 }
 for(int y=0;y<MAP_H;++y) for(int x=0;x<MAP_W;++x) {
  int choices[5]={infantry<3?INFANTRY:(visible_air?ANTIAIR:TANK),ARTILLERY,MECH,RECON,INFANTRY};
  if(g->tiles[y][x].terrain==FACTORY)
   for(int k=0;k<5;++k) if(game_produce(g,x,y,choices[k])) return false;
  if(g->tiles[y][x].terrain==AIRPORT && g->money[g->side]>30000 && game_produce(g,x,y,visible_air?FIGHTER:BOMBER)) return false;
  if(g->tiles[y][x].terrain==PORT && g->money[g->side]>40000 && game_produce(g,x,y,BATTLESHIP)) return false;
 }
 game_end_turn(g); return true;
}
