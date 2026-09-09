#include "terrain.h"
#include "gfx/canvas.h"
#include "config.h"

/* Original hand-pixelled silhouettes, not scaled/antialiased artwork. Dots are
 * deliberate white holes. The bottom row stays free for ownership markers.
 * Buildings have open windows, rather than the solid/reversed unit frames. */
static const char art[TERRAIN_COUNT][12][13] = {
 [PLAIN]={
 "............","............","....#.......","..#.#.......",
 "...##.......","............","........#...",".......#.#..",
 "........#...","..#.........","............","............"},
 [FOREST]={
 "............","...##.......","..#..#..#...",".#..###..#..",
 "..#..#.#..#.",".#..#...###.","#..##..#..#.",".##..##.##..",
 "...#..#.#...","..##..###...","............","............"},
 [MOUNTAIN]={
 "............",".....#......","....#.#.....","...#...#....",
 "...#.###....","..#.#.###...","..#....##...",".#....#.##..",
 ".#...#..###.",".##########.","............","............"},
 [CITY]={
 "............","....#.......","...###......","..#####.##..",
 ".#######..#.","..#...#.###.","..#.#.#.#.#.","..#...#...#.",
 "..#.#.#.#.#.","..#########.","............","............"},
 [FACTORY]={
 "............","........##..","........##..","...#..#.##..",
 "..##.##.##..",".##########.",".#........#.",".#.##.##..#.",
 ".#......#.#.",".##########.","............","............"},
 [AIRPORT]={
 "............","..#.....#...","..#.###.#...","..#.....#...",
 "..#..#..#...","..#..#..#...","..#.....#...","..#..#..#...",
 "..#.....#...","..#.###.#...","............","............"},
 [PORT]={
 "............","..##........",".#..#.......",".####.##.##.",
 ".#..#..#..#.",".##########.",".......#..#.","..##...#..#.",
 ".#..#..#..#.","......##.##.","............","............"},
 [HQ]={
 "............",".....#####..",".....#..#...",".....###....",
 ".....#......","...#####....","..#######...","..#..#..#...",
 "..#.#.#.#...","..#######...","............","............"}
};
enum { N=1,E=2,S=4,W=8 };
static int tile_at(const Game *g,int x,int y){
 if(x<0||y<0||x>=g->w||y>=g->h||x>=MAP_W||y>=MAP_H)return -1;
 return g->tiles[y][x].terrain;
}
static bool road_target(int t){return t==ROAD||(t>=CITY&&t<=HQ);}
static bool water_target(int t){return t==RIVER||t==SEA||t==PORT;}
static unsigned neighbors(const Game *g,int x,int y,bool road){
 const int dx[4]={0,1,0,-1},dy[4]={-1,0,1,0};unsigned mask=0;
 for(int d=0;d<4;d++){
  int t=tile_at(g,x+dx[d],y+dy[d]);
  if(road?road_target(t):water_target(t))mask|=1u<<d;
 }
 return mask;
}
/* This predicate deliberately extends past the tile at connected edges, so
 * boundary tracing never draws an end-cap over a real neighboring connection. */
static bool channel(int x,int y,unsigned m,int lo,int hi){
 bool cx=x>=lo&&x<=hi,cy=y>=lo&&y<=hi;
 return (cx&&cy)||(cx&&y<lo&&(m&N))||(cy&&x>hi&&(m&E))||
        (cx&&y>hi&&(m&S))||(cy&&x<lo&&(m&W));
}
static bool channel_ink(int x,int y,unsigned m,bool road){
 int lo=3,hi=road?7:8;
 if(!channel(x,y,m,lo,hi))return false;
 if(!channel(x-1,y,m,lo,hi)||!channel(x+1,y,m,lo,hi)||
    !channel(x,y-1,m,lo,hi)||!channel(x,y+1,m,lo,hi))return true;
 if(road){
  /* Long dashes, not the isolated regular dots of the fog overlay. */
  return ((m&(N|S))&&x==5&&(y%6==1||y%6==2))||
         ((m&(E|W))&&y==5&&(x%6==1||x%6==2));
 }
 return (y==5&&(x==5||x==6))||(y==7&&x==6);
}
static bool sea_ink(const Game *g,int mx,int my,int x,int y){
 /* Sparse paired wave crests. No checkerboard/dither, no solid water fill. */
 bool ink=(y==4&&(x==3||x==4))||(y==3&&x==5)||
          (y==8&&(x==7||x==8))||(y==7&&x==9);
 const int dx[4]={0,1,0,-1},dy[4]={-1,0,1,0};
 for(int d=0;d<4;d++){
  int t=tile_at(g,mx+dx[d],my+dy[d]);
  int along=(d==0||d==2)?x:y;
  int depth=d==0?y:d==1?11-x:d==2?11-y:x;
  /* Outside the map is unknown, not invented land. A river mouth opens only
   * the river's actual six-pixel channel; a port has its own dock artwork. */
  bool coast=t>=0&&t!=SEA&&t!=PORT&&!(t==RIVER&&along>=3&&along<=8);
  if(coast&&depth==(along>=4&&along<=7?1:0))ink=true;
 }
 return ink;
}
void terrain_draw(const Game *g,int mapx,int mapy,int screenx,int screeny){
 if(!g)return;
 int t=tile_at(g,mapx,mapy);
 if(t<0||t>=TERRAIN_COUNT)return;
 /* Reject wholly offscreen tiles before adding offsets (also avoids int
  * overflow for hostile screen coordinates). fb_pixel clips partial tiles. */
 if(screenx<=-24||screeny<=-24||screenx>=(int)CCG_W||screeny>=(int)CCG_H)return;
 unsigned m=neighbors(g,mapx,mapy,t==ROAD);
 bool mirror=(t==PLAIN||t==FOREST)&&((mapx*3+mapy)&1);
 for(int y=0;y<24;y++)for(int x=0;x<24;x++){
  int sx=x/2,sy=y/2; bool ink;
  if(t==ROAD||t==RIVER)ink=channel_ink(sx,sy,m,t==ROAD);
  else if(t==SEA)ink=sea_ink(g,mapx,mapy,sx,sy);
  else ink=art[t][sy][mirror?11-sx:sx]=='#';
  fb_pixel(screenx+x,screeny+y,ink);
 }
}
