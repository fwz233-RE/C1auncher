#define _POSIX_C_SOURCE 200809L
#include "game.h"
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static unsigned checks;
#define CHECK(x) do { ++checks; if(!(x)) { fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x); exit(1); } } while(0)
static void tile(Game *g,int x,int y,int terrain,int owner) { g->tiles[y][x]=(Tile){(uint8_t)terrain,(int8_t)owner,20}; }
static void unit(Game *g,int id,int side,int type,int x,int y) {
 g->units[id]=(Unit){1,(uint8_t)type,(uint8_t)side,(uint8_t)x,(uint8_t)y,100,unit_defs[type].fuel,unit_defs[type].ammo,0,0};
}
static void arena(Game *g) {
 game_new(g,0,0,0,true,false);
 memset(g->units,0,sizeof(g->units));
 for(int y=0;y<MAP_H;++y) for(int x=0;x<MAP_W;++x) tile(g,x,y,PLAIN,-1);
 tile(g,1,1,HQ,0); tile(g,28,16,HQ,1);
 unit(g,0,0,INFANTRY,2,2); unit(g,1,1,INFANTRY,27,15);
 g->money[0]=g->money[1]=0; g->side=0; g->turn=1; g->winner=-1;
 game_vision(g);
}
static void next_own_turn(Game *g) { game_end_turn(g); game_end_turn(g); }
static bool ports_connected(const Game *g) {
 int start=-1,end=-1,queue[MAP_W*MAP_H],head=0,tail=0; unsigned char seen[MAP_H][MAP_W]={{0}};
 for(int y=0;y<MAP_H;++y) for(int x=0;x<MAP_W;++x) if(g->tiles[y][x].terrain==PORT) {
  if(g->tiles[y][x].owner==0) start=y*MAP_W+x;
  if(g->tiles[y][x].owner==1) end=y*MAP_W+x;
 }
 if(start<0 || end<0) return false;
 queue[tail++]=start; seen[start/MAP_W][start%MAP_W]=1;
 while(head<tail) {
  int p=queue[head++],x=p%MAP_W,y=p/MAP_W;
  if(p==end) return true;
  const int nx[4]={x-1,x+1,x,x},ny[4]={y,y,y-1,y+1};
  for(int k=0;k<4;++k) if(nx[k]>=0 && nx[k]<MAP_W && ny[k]>=0 && ny[k]<MAP_H && !seen[ny[k]][nx[k]]) {
   int t=g->tiles[ny[k]][nx[k]].terrain;
   if(t==SEA || t==PORT) { seen[ny[k]][nx[k]]=1; queue[tail++]=ny[k]*MAP_W+nx[k]; }
  }
 }
 return false;
}
static void test_maps(void) {
 Game g,other;
 for(int m=0;m<MAP_COUNT;++m) {
  game_new(&g,m,0,2,true,true); game_new(&other,m,0,2,true,true);
  CHECK(memcmp(&g,&other,sizeof(g))==0);
  CHECK(g.w==30 && g.h==18 && g.map_id==m && g.winner==-1 && g.side==0 && g.turn==1);
  int terrain[TERRAIN_COUNT]={0},count[2]={0,0},hq[2]={0,0};
  for(int y=0;y<MAP_H;++y) for(int x=0;x<MAP_W;++x) {
   CHECK(g.tiles[y][x].terrain<TERRAIN_COUNT); ++terrain[g.tiles[y][x].terrain];
   if(g.tiles[y][x].terrain==HQ) ++hq[(int)g.tiles[y][x].owner];
  }
  for(int i=0;i<MAX_UNITS;++i) if(g.units[i].alive) { ++count[g.units[i].side]; CHECK(game_unit_at(&g,g.units[i].x,g.units[i].y)==i); }
  CHECK(count[0]>=2 && count[1]>=2 && hq[0]==1 && hq[1]==1);
  for(int t=0;t<TERRAIN_COUNT;++t) CHECK(terrain[t]>0);
  CHECK(ports_connected(&g));
  CHECK(game_income(&g,0)==game_income(&g,1));
  CHECK(g.money[0]==10000U+(unsigned)game_income(&g,0));
  CHECK(g.money[1]==10000U);
  game_check_win(&g); CHECK(g.winner==-1);
 }
 game_new(&g,-1,-2,50,false,false); CHECK(g.map_id==0 && g.co[0]==0 && g.co[1]==0);
 CHECK(strcmp(map_names[0],map_names[1])!=0 && strcmp(map_names[1],map_names[2])!=0);
}
static void test_production(void) {
 Game g; arena(&g); g.money[0]=100000;
 tile(&g,4,4,FACTORY,0); tile(&g,5,4,AIRPORT,0); tile(&g,6,4,PORT,0);
 for(int type=0;type<UNIT_TYPES;++type) {
  int x=type<=ANTIAIR?4:(type<=BOMBER?5:6);
  CHECK(game_can_produce(&g,x,4,type));
  uint32_t money=g.money[0]; CHECK(game_produce(&g,x,4,type));
  int id=game_unit_at(&g,x,4); CHECK(id>=0 && g.units[id].type==type && g.units[id].acted);
  CHECK(g.units[id].hp==100 && g.units[id].ammo==unit_defs[type].ammo && g.units[id].fuel==unit_defs[type].fuel);
  CHECK(g.money[0]==money-unit_defs[type].price);
  CHECK(!game_produce(&g,x,4,type)); memset(&g.units[id],0,sizeof(Unit)); g.money[0]=100000;
 }
 CHECK(!game_produce(&g,4,4,FIGHTER)); CHECK(!game_produce(&g,5,4,TANK)); CHECK(!game_produce(&g,6,4,INFANTRY));
 CHECK(!game_produce(&g,-1,4,INFANTRY)); CHECK(!game_produce(&g,4,4,UNIT_TYPES));
 tile(&g,4,4,FACTORY,1); CHECK(!game_produce(&g,4,4,INFANTRY)); tile(&g,4,4,FACTORY,0);
 g.money[0]=999; CHECK(!game_produce(&g,4,4,INFANTRY));
 g.co[0]=2; g.money[0]=900; CHECK(game_produce(&g,4,4,INFANTRY)); CHECK(g.money[0]==0);
}
static void test_movement(void) {
 Game g; arena(&g); int16_t cost[MAP_H][MAP_W],prev[MAP_H][MAP_W];
 unit(&g,0,0,RECON,2,2); tile(&g,3,2,FOREST,-1);
 game_range(&g,0,cost,prev); CHECK(cost[2][3]==3); CHECK(cost[2][4]==4);
 /* The direct two-forest route costs 7; Dijkstra finds the 5-cost detour. */
 tile(&g,4,2,FOREST,-1); game_range(&g,0,cost,prev); CHECK(cost[2][5]==5);
 CHECK(game_move(&g,0,5,2)); CHECK(g.units[0].fuel==75 && g.units[0].moved && !g.units[0].acted);
 CHECK(!game_move(&g,0,6,2)); CHECK(game_wait(&g,0)); CHECK(!game_wait(&g,0));
 arena(&g); unit(&g,0,0,TANK,2,2); tile(&g,3,2,MOUNTAIN,-1); tile(&g,2,3,RIVER,-1); tile(&g,1,2,SEA,-1);
 game_range(&g,0,cost,prev); CHECK(cost[2][3]==INF_COST && cost[3][2]==INF_COST && cost[2][1]==INF_COST);
 unit(&g,0,0,INFANTRY,2,2); game_range(&g,0,cost,prev); CHECK(cost[2][3]==2 && cost[3][2]==2);
 unit(&g,0,0,MECH,2,2); game_range(&g,0,cost,prev); CHECK(cost[2][3]==1 && cost[3][2]==1);
 unit(&g,0,0,FIGHTER,2,2); game_range(&g,0,cost,prev); CHECK(cost[2][3]==1 && cost[2][1]==1);
 g.units[0].fuel=1; CHECK(!game_move(&g,0,4,2)); CHECK(game_move(&g,0,3,2)); CHECK(g.units[0].fuel==0);
 arena(&g); tile(&g,2,2,PORT,0); tile(&g,3,2,SEA,-1); tile(&g,4,2,SEA,-1); unit(&g,0,0,BATTLESHIP,2,2);
 game_range(&g,0,cost,prev); CHECK(cost[2][4]==2 && cost[1][2]==INF_COST); CHECK(game_move(&g,0,4,2));
 arena(&g); unit(&g,0,0,TANK,2,2); unit(&g,2,0,INFANTRY,3,2);
 CHECK(!game_move(&g,0,3,2)); CHECK(game_move(&g,0,4,2)); CHECK(g.units[0].fuel==68);
 arena(&g); unit(&g,0,0,TANK,2,2); unit(&g,1,1,INFANTRY,3,2); game_vision(&g);
 game_range(&g,0,cost,prev); CHECK(cost[2][3]==INF_COST); CHECK(!game_move(&g,0,3,2));
 CHECK(!game_move(&g,1,4,2)); CHECK(!game_move(&g,MAX_UNITS,4,2));
}
static void test_fog_and_ambush(void) {
 Game g; arena(&g); g.fog=1; unit(&g,0,0,RECON,2,2); unit(&g,1,1,TANK,4,2); tile(&g,4,2,FOREST,-1); game_vision(&g);
 CHECK(g.visible[0][2][4]); CHECK(!game_visible_unit(&g,0,1)); CHECK(game_visible_unit(&g,1,1));
 int16_t cost[MAP_H][MAP_W],prev[MAP_H][MAP_W]; game_range(&g,0,cost,prev); CHECK(cost[2][4]!=INF_COST);
 CHECK(game_move(&g,0,4,2)); CHECK(g.units[0].x==3 && g.units[0].y==2 && g.units[0].acted);
 CHECK(g.units[0].fuel==79 && game_visible_unit(&g,0,1)); CHECK(game_unit_at(&g,4,2)==1);
 arena(&g); g.fog=1; unit(&g,1,1,FIGHTER,4,2); tile(&g,4,2,FOREST,-1); game_vision(&g); CHECK(game_visible_unit(&g,0,1));
 unit(&g,1,1,TANK,20,12); game_vision(&g); CHECK(!game_visible_unit(&g,0,1));
 g.fog=0; game_vision(&g); CHECK(game_visible_unit(&g,0,1));
 CHECK(!game_visible_unit(&g,2,1)); CHECK(!game_visible_unit(&g,0,-1));
 arena(&g); g.fog=1; tile(&g,2,2,MOUNTAIN,-1); game_vision(&g); CHECK(g.visible[0][2][6]);
 tile(&g,2,2,PLAIN,-1); game_vision(&g); CHECK(!g.visible[0][2][6]);
}
static void test_capture(void) {
 Game g; arena(&g); tile(&g,2,2,CITY,-1);
 CHECK(game_capture(&g,0)); CHECK(g.tiles[2][2].capture==10 && g.tiles[2][2].owner==-1);
 CHECK(!game_capture(&g,0)); next_own_turn(&g); CHECK(game_capture(&g,0));
 CHECK(g.tiles[2][2].owner==0 && g.tiles[2][2].capture==20 && game_income(&g,0)==2000);
 next_own_turn(&g); CHECK(!game_capture(&g,0));
 arena(&g); tile(&g,2,2,CITY,1); g.units[0].hp=41;
 CHECK(game_capture(&g,0)); CHECK(g.tiles[2][2].capture==15);
 next_own_turn(&g); CHECK(game_move(&g,0,3,2)); CHECK(g.tiles[2][2].capture==20);
 arena(&g); tile(&g,2,2,CITY,-1); unit(&g,0,0,TANK,2,2); CHECK(!game_capture(&g,0));
 arena(&g); unit(&g,0,0,MECH,28,16); CHECK(game_capture(&g,0)); next_own_turn(&g); CHECK(game_capture(&g,0)); CHECK(g.winner==0);
 CHECK(!game_wait(&g,0)); uint32_t turn=g.turn; game_end_turn(&g); CHECK(g.turn==turn);
}
static void test_combat(void) {
 Game g; arena(&g); unit(&g,0,0,TANK,2,2); unit(&g,1,1,TANK,3,2); game_vision(&g);
 Prediction p=game_predict(&g,0,1),q;
 /* 55*100/100=55; floor(55*105/100)=57; plain gives floor(57*.90)=51.
  * Return fire: floor(55*.49)=26; floor(26*1.05)=27; floor(27*.90)=24. */
 CHECK(p.damage==51 && p.counter==24); CHECK(game_can_attack(&g,0,1));
 Game unchanged=g; CHECK(memcmp(&g,&unchanged,sizeof(g))==0); CHECK(game_predict(&g,0,1).damage==p.damage);
 CHECK(game_attack(&g,0,1,&q)); CHECK(q.damage==p.damage && q.counter==p.counter);
 CHECK(g.units[0].hp==76 && g.units[1].hp==49 && g.units[0].ammo==8 && g.units[1].ammo==8 && g.units[0].acted);
 CHECK(g.charge[0]==75 && g.charge[1]==75); CHECK(!game_attack(&g,0,1,&q)); CHECK(q.damage==0 && q.counter==0);
 arena(&g); unit(&g,0,0,ARTILLERY,2,2); unit(&g,1,1,TANK,4,2); game_vision(&g);
 p=game_predict(&g,0,1); CHECK(p.damage>0 && p.counter==0);
 CHECK(game_move(&g,0,2,3)); CHECK(!game_can_attack(&g,0,1));
 arena(&g); unit(&g,0,0,ARTILLERY,2,2); unit(&g,1,1,TANK,3,2); game_vision(&g); CHECK(!game_can_attack(&g,0,1));
 arena(&g); unit(&g,0,0,FIGHTER,2,2); unit(&g,1,1,TANK,3,2); game_vision(&g); CHECK(!game_can_attack(&g,0,1));
 unit(&g,1,1,BOMBER,3,2); CHECK(game_can_attack(&g,0,1)); p=game_predict(&g,0,1); CHECK(p.damage==100 && p.counter==0);
 tile(&g,3,2,MOUNTAIN,-1); CHECK(game_predict(&g,0,1).damage==100);
 CHECK(game_attack(&g,0,1,&q)); CHECK(!g.units[1].alive && g.winner==0);
 arena(&g); unit(&g,0,0,TANK,2,2); unit(&g,1,1,TANK,3,2); g.units[0].ammo=0; CHECK(!game_can_attack(&g,0,1));
 g.units[0].ammo=9; g.units[1].ammo=0; CHECK(game_predict(&g,0,1).counter==0);
 g.units[1].ammo=9; int plain=game_predict(&g,0,1).damage; tile(&g,3,2,CITY,1); CHECK(game_predict(&g,0,1).damage<plain);
 arena(&g); unit(&g,0,0,INFANTRY,2,2); unit(&g,1,1,TANK,3,2); g.units[0].hp=1;
 CHECK(game_attack(&g,0,1,&q)); CHECK(!g.units[0].alive && g.winner==1);
 arena(&g); tile(&g,3,2,CITY,0); unit(&g,0,0,TANK,2,2); unit(&g,1,1,INFANTRY,3,2); g.units[1].hp=1; g.tiles[2][3].capture=10;
 CHECK(game_attack(&g,0,1,&q)); CHECK(g.tiles[2][3].capture==20);
}
static void test_income_repair_and_power(void) {
 Game g; arena(&g); tile(&g,2,2,CITY,0); unit(&g,0,0,TANK,2,2); g.units[0].hp=50; g.units[0].fuel=1; g.units[0].ammo=0;
 next_own_turn(&g); CHECK(g.units[0].hp==70 && g.money[0]==600 && g.units[0].fuel==70 && g.units[0].ammo==9);
 CHECK(!g.units[0].acted && !g.units[0].moved && g.side==0 && g.turn==3);
 arena(&g); tile(&g,2,2,AIRPORT,0); unit(&g,0,0,BOMBER,2,2); g.units[0].hp=50; g.units[0].fuel=1;
 next_own_turn(&g); CHECK(g.units[0].hp==59 && g.money[0]==20 && g.units[0].fuel==99);
 arena(&g); tile(&g,2,2,CITY,0); unit(&g,0,0,FIGHTER,2,2); g.units[0].hp=50; g.units[0].ammo=0;
 next_own_turn(&g); CHECK(g.units[0].hp==50 && g.units[0].ammo==0);
 arena(&g); g.co[0]=2; tile(&g,2,2,CITY,0); g.units[0].hp=50; next_own_turn(&g);
 CHECK(g.units[0].hp==80 && g.money[0]==1730);
 arena(&g); CHECK(!game_power(&g,false)); g.charge[0]=600; CHECK(game_power(&g,true)); CHECK(g.power[0]==2 && g.charge[0]==0);
 CHECK(!game_power(&g,false)); int16_t cost[MAP_H][MAP_W],prev[MAP_H][MAP_W]; game_range(&g,0,cost,prev); CHECK(cost[2][6]==4);
 game_end_turn(&g); CHECK(g.power[0]==2); game_end_turn(&g); CHECK(g.power[0]==0);
 arena(&g); unit(&g,0,0,TANK,2,2); unit(&g,1,1,TANK,3,2); int before=game_predict(&g,0,1).damage;
 g.charge[0]=300; CHECK(game_power(&g,false)); CHECK(game_predict(&g,0,1).damage>before);
 arena(&g); g.co[0]=1; g.fog=1; game_vision(&g); CHECK(g.visible[0][2][5]); CHECK(!g.visible[0][2][6]);
 g.charge[0]=600; CHECK(game_power(&g,true)); CHECK(g.visible[0][2][7]);
 arena(&g); g.co[0]=2; g.units[0].hp=50; g.units[0].fuel=0; g.units[0].ammo=0; g.charge[0]=300;
 CHECK(game_power(&g,false)); CHECK(g.units[0].hp==60 && g.units[0].fuel==60 && g.units[0].ammo==9);
 arena(&g); g.co[0]=2; g.units[0].hp=95; g.charge[0]=600; CHECK(game_power(&g,true)); CHECK(g.units[0].hp==100);
}
static void test_ai(void) {
 Game a,b; arena(&a); a.fog=1; unit(&a,0,0,TANK,2,2); unit(&a,1,1,TANK,25,14); tile(&a,25,14,FOREST,-1);
 b=a; unit(&b,1,1,BOMBER,23,14); game_vision(&a); game_vision(&b);
 CHECK(!game_visible_unit(&a,0,1) && !game_visible_unit(&b,0,1));
 CHECK(game_ai_step(&a)==game_ai_step(&b)); CHECK(memcmp(&a.units[0],&b.units[0],sizeof(Unit))==0);
 CHECK(a.money[0]==b.money[0] && a.units[0].acted);
 arena(&a); tile(&a,2,2,CITY,-1); CHECK(!game_ai_step(&a)); CHECK(a.tiles[2][2].capture==10); CHECK(game_ai_step(&a)); CHECK(a.side==1);
 arena(&a); unit(&a,1,1,INFANTRY,3,2); CHECK(!game_ai_step(&a)); CHECK(a.units[1].hp<100);
 for(int m=0;m<MAP_COUNT;++m) {
  game_new(&a,m,m,(m+1)%3,false,true);
  for(int turns=0;turns<30 && a.winner<0;++turns) {
   uint32_t turn=a.turn; int calls=0; bool ended=false;
   while(!ended && calls<MAX_UNITS+MAP_W*MAP_H+2) { ended=game_ai_step(&a); ++calls; }
   CHECK(ended); CHECK(a.turn>turn || a.winner>=0); CHECK(calls<=MAX_UNITS+MAP_W*MAP_H+1);
   for(int i=0;i<MAX_UNITS;++i) if(a.units[i].alive) {
    CHECK(a.units[i].hp>0 && a.units[i].hp<=100); CHECK(game_unit_at(&a,a.units[i].x,a.units[i].y)==i);
   }
  }
 }
 /* Fully blocked units still consume exactly one action and then end turn. */
 arena(&a); unit(&a,0,0,TANK,2,2);
 tile(&a,1,2,MOUNTAIN,-1); tile(&a,3,2,MOUNTAIN,-1); tile(&a,2,1,MOUNTAIN,-1); tile(&a,2,3,MOUNTAIN,-1);
 CHECK(!game_ai_step(&a)); CHECK(a.units[0].acted); CHECK(game_ai_step(&a)); CHECK(a.side==1);
}
#define SAVE_BYTES (20+27+MAP_W*MAP_H*3+MAX_UNITS*10)
static uint32_t test_crc(const unsigned char *data,size_t n) {
 uint32_t c=0xffffffffU;
 for(size_t i=0;i<n;++i) { c^=data[i]; for(int b=0;b<8;++b) c=(c>>1)^(0xedb88320U&(0U-(c&1U))); }
 return c^0xffffffffU;
}
static void checksum(unsigned char *data) { uint32_t c=test_crc(data+20,SAVE_BYTES-20); for(int i=0;i<4;++i) data[16+i]=(unsigned char)(c>>(8*i)); }
static void write_bytes(const char *path,const unsigned char *data,size_t n) {
 FILE *f=fopen(path,"wb"); CHECK(f!=NULL); CHECK(fwrite(data,1,n,f)==n); CHECK(fclose(f)==0);
}
static void read_bytes(const char *path,unsigned char *data,size_t n) {
 FILE *f=fopen(path,"rb"); CHECK(f!=NULL); CHECK(fread(data,1,n,f)==n); CHECK(fgetc(f)==EOF); CHECK(fclose(f)==0);
}
static void test_save(void) {
 char directory[]="/tmp/inkwars-test-XXXXXX",path[256],path2[256],err[160];
 CHECK(mkdtemp(directory)!=NULL); (void)snprintf(path,sizeof(path),"%s/game.iws",directory); (void)snprintf(path2,sizeof(path2),"%s/other.iws",directory);
 Game g,loaded,original; unsigned char bytes[SAVE_BYTES],mutated[SAVE_BYTES],after[SAVE_BYTES];
 CHECK(test_crc((const unsigned char *)"123456789",9)==0xcbf43926U);
 for(int m=0;m<MAP_COUNT;++m) {
  game_new(&g,m,m,(m+1)%3,true,true); CHECK(game_save(&g,path,err,sizeof(err))); CHECK(err[0]=='\0');
  memset(&loaded,0x5a,sizeof(loaded)); CHECK(game_load(&loaded,path,err,sizeof(err))); CHECK(memcmp(&g,&loaded,sizeof(g))==0);
 }
 Game played;
 arena(&played); tile(&played,2,2,CITY,-1); CHECK(game_capture(&played,0));
 played.charge[0]=600; CHECK(game_power(&played,true));
 CHECK(game_save(&played,path,err,sizeof(err))); CHECK(game_load(&loaded,path,err,sizeof(err))); CHECK(memcmp(&played,&loaded,sizeof(played))==0);
 next_own_turn(&played); CHECK(game_capture(&played,0));
 CHECK(game_save(&played,path,err,sizeof(err))); CHECK(game_load(&loaded,path,err,sizeof(err))); CHECK(memcmp(&played,&loaded,sizeof(played))==0);
 arena(&played); unit(&played,0,0,FIGHTER,2,2); unit(&played,1,1,BOMBER,3,2); CHECK(game_attack(&played,0,1,NULL)); CHECK(played.winner==0);
 CHECK(game_save(&played,path,err,sizeof(err))); CHECK(game_load(&loaded,path,err,sizeof(err))); CHECK(memcmp(&played,&loaded,sizeof(played))==0);
 CHECK(game_save(&g,path,err,sizeof(err))); CHECK(game_load(&loaded,path,err,sizeof(err)));
 read_bytes(path,bytes,sizeof(bytes)); CHECK(memcmp(bytes,"IWARS\r\n\032",8)==0); CHECK(bytes[8]==1 && bytes[9]==0 && bytes[10]==0 && bytes[11]==0);
 CHECK(bytes[20]==1 && bytes[21]==0 && bytes[22]==0 && bytes[23]==0);
 CHECK(game_save(&g,path2,err,sizeof(err))); read_bytes(path2,after,sizeof(after)); CHECK(memcmp(bytes,after,sizeof(bytes))==0);
 original=loaded;
 /* Every byte is protected by exact header validation or CRC. Each failed load
  * must preserve every byte of the caller's destination. */
 for(size_t i=0;i<sizeof(bytes);++i) {
  memcpy(mutated,bytes,sizeof(bytes)); mutated[i]^=1; write_bytes(path,mutated,sizeof(mutated));
  CHECK(!game_load(&loaded,path,err,sizeof(err))); CHECK(memcmp(&loaded,&original,sizeof(loaded))==0);
 }
 CHECK(!game_save(&g,path,err,sizeof(err))); read_bytes(path,after,sizeof(after)); CHECK(memcmp(mutated,after,sizeof(mutated))==0);
 /* Semantically invalid but correctly checksummed files must also be refused. */
 const size_t offsets[]={20,33,36,38,40,41,42,43,44,45,46,47,48,49,47+1620,47+1620+1,47+1620+2,47+1620+3,47+1620+5,47+1620+6,47+1620+7,47+1620+8,47+1620+9};
 const unsigned char values[]={0,255,3,3,2,2,2,3,29,17,2,11,2,0,2,10,2,30,0,255,255,2,2};
 for(size_t i=0;i<sizeof(offsets)/sizeof(offsets[0]);++i) {
  memcpy(mutated,bytes,sizeof(bytes)); mutated[offsets[i]]=values[i]; checksum(mutated); write_bytes(path,mutated,sizeof(mutated));
  CHECK(!game_load(&loaded,path,err,sizeof(err))); CHECK(memcmp(&loaded,&original,sizeof(loaded))==0);
  CHECK(!game_save(&g,path,err,sizeof(err))); read_bytes(path,after,sizeof(after)); CHECK(memcmp(mutated,after,sizeof(mutated))==0);
 }
 /* Duplicate live unit positions, neutral HQ, impossible capture progress,
  * impossible terrain placement and noncanonical dead records. */
 memcpy(mutated,bytes,sizeof(bytes)); size_t units=47+1620;
 mutated[units+10+3]=mutated[units+3]; mutated[units+10+4]=mutated[units+4]; checksum(mutated); write_bytes(path,mutated,sizeof(mutated)); CHECK(!game_load(&loaded,path,err,sizeof(err)));
 memcpy(mutated,bytes,sizeof(bytes)); mutated[47+(9*MAP_W+2)*3+1]=255; checksum(mutated); write_bytes(path,mutated,sizeof(mutated)); CHECK(!game_load(&loaded,path,err,sizeof(err)));
 memcpy(mutated,bytes,sizeof(bytes)); mutated[47+(9*MAP_W+2)*3+2]=10; checksum(mutated); write_bytes(path,mutated,sizeof(mutated)); CHECK(!game_load(&loaded,path,err,sizeof(err)));
 memcpy(mutated,bytes,sizeof(bytes)); mutated[47+(9*MAP_W+4)*3]=SEA; checksum(mutated); write_bytes(path,mutated,sizeof(mutated)); CHECK(!game_load(&loaded,path,err,sizeof(err)));
 memcpy(mutated,bytes,sizeof(bytes)); mutated[units+6*10+5]=100; checksum(mutated); write_bytes(path,mutated,sizeof(mutated)); CHECK(!game_load(&loaded,path,err,sizeof(err)));
 for(size_t n=0;n<sizeof(bytes);n+=137) { write_bytes(path,bytes,n); CHECK(!game_load(&loaded,path,err,sizeof(err))); CHECK(!game_save(&g,path,err,sizeof(err))); }
 write_bytes(path,bytes,sizeof(bytes)); FILE *f=fopen(path,"ab"); CHECK(f!=NULL); CHECK(fputc(0,f)!=EOF); CHECK(fclose(f)==0); CHECK(!game_load(&loaded,path,err,sizeof(err))); CHECK(!game_save(&g,path,err,sizeof(err)));
 CHECK(unlink(path)==0); CHECK(symlink(path2,path)==0); CHECK(!game_save(&g,path,err,sizeof(err))); CHECK(!game_load(&loaded,path,err,sizeof(err))); CHECK(unlink(path)==0);
 CHECK(mkfifo(path,0600)==0); CHECK(!game_load(&loaded,path,err,sizeof(err))); CHECK(!game_save(&g,path,err,sizeof(err))); CHECK(unlink(path)==0);
 CHECK(!game_load(&loaded,directory,err,sizeof(err)));
 CHECK(!game_load(&loaded,path,err,sizeof(err))); CHECK(memcmp(&loaded,&original,sizeof(loaded))==0);
 CHECK(!game_save(&g,"/no-such-inkwars-directory/game.iws",err,sizeof(err)));
 CHECK(!game_save(&g,"",NULL,0)); CHECK(!game_load(NULL,path,NULL,0));
 g.units[0].hp=0; CHECK(!game_save(&g,path,err,sizeof(err))); g.units[0].hp=100;
 CHECK(game_save(&g,path,err,sizeof(err))); CHECK(game_load(&loaded,path,err,sizeof(err))); CHECK(memcmp(&g,&loaded,sizeof(g))==0);
 CHECK(unlink(path)==0); CHECK(unlink(path2)==0); CHECK(rmdir(directory)==0);
}
/* Optional cross-filesystem regression: MIPS32 stat must handle large host
 * inode numbers (notably WSL /mnt/d). Keep FIFO/symlink tests on /tmp. */
static void test_requested_save_filesystem(void) {
 const char *base=getenv("INKWARS_TEST_SAVE_DIR");
 if(!base || !*base) return;
 char directory[512],path[1024],err[160];
 int n=snprintf(directory,sizeof(directory),"%s/inkwars-largefile-XXXXXX",base);
 CHECK(n>0 && (size_t)n<sizeof(directory)); CHECK(mkdtemp(directory)!=NULL);
 (void)snprintf(path,sizeof(path),"%s/repeated.iws",directory);
 Game g,loaded; game_new(&g,0,0,1,true,true);
 for(int i=0;i<3;++i) {
  if(!game_save(&g,path,err,sizeof(err))) { fprintf(stderr,"filesystem save %d failed: %s\n",i,err); CHECK(false); }
  CHECK(game_load(&loaded,path,err,sizeof(err))); CHECK(memcmp(&g,&loaded,sizeof(g))==0);
  game_end_turn(&g);
 }
 CHECK(unlink(path)==0); CHECK(rmdir(directory)==0);
 printf("PASS repeated save/load on %s (three atomic saves)\n",base);
}
int main(void) {
 test_maps(); test_production(); test_movement(); test_fog_and_ambush(); test_capture(); test_combat();
 test_income_repair_and_power(); test_ai(); test_save(); test_requested_save_filesystem();
 printf("InkWars rules/save: %u checks passed\n",checks); return 0;
}
