#include "terrain.h"
#include "gfx/canvas.h"
#include "gfx/font.h"
#include "config.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

uint8_t g_fb[CCG_FRAME_BYTES];
static Game g;
static uint8_t before[CCG_FRAME_BYTES];
static unsigned char samples[TERRAIN_COUNT][576];
static bool pixel(int x,int y){return (g_fb[(y>>3)*CCG_W+x]&(0x80u>>(y&7)))!=0;}
static bool old_pixel(int x,int y){return (before[(y>>3)*CCG_W+x]&(0x80u>>(y&7)))!=0;}
static void reset(void){memset(&g,0,sizeof g);g.w=MAP_W;g.h=MAP_H;}
static void check_clipped(int sx,int sy){
 for(int y=0;y<(int)CCG_H;y++)for(int x=0;x<(int)CCG_W;x++)
  if((long long)x<sx||(long long)x>=(long long)sx+24||
     (long long)y<sy||(long long)y>=(long long)sy+24)
   assert(pixel(x,y)==old_pixel(x,y));
}
static void artwork_tests(void){
 const int pos[][2]={{17,19},{-5,-7},{291,148},{-12,0},{296,0},
  {0,152},{INT_MAX,INT_MAX},{INT_MIN,INT_MIN}};
 reset();
 for(int t=0;t<TERRAIN_COUNT;t++){
  g.tiles[2][2].terrain=(uint8_t)t;
  fb_clear(false);terrain_draw(&g,2,2,17,19);unsigned count=0;
  for(int y=0;y<24;y++)for(int x=0;x<24;x++){
   samples[t][y*24+x]=(unsigned char)pixel(17+x,19+y);count+=pixel(17+x,19+y);
  }
  assert(count>=24&&count<=288); /* <=50% black, with genuine white negative space */
  for(int p=0;p<(int)(sizeof pos/sizeof pos[0]);p++){
   memset(g_fb,0xa5,sizeof g_fb);memcpy(before,g_fb,sizeof before);
   terrain_draw(&g,2,2,pos[p][0],pos[p][1]);check_clipped(pos[p][0],pos[p][1]);
  }
  g.w=g.h=1;g.tiles[0][0].terrain=(uint8_t)t;
  fb_clear(false);terrain_draw(&g,0,0,284,140);
  g.w=MAP_W;g.h=MAP_H;
 }
 for(int a=0;a<TERRAIN_COUNT;a++)for(int b=a+1;b<TERRAIN_COUNT;b++){
  unsigned diff=0;for(int i=0;i<576;i++)diff+=samples[a][i]!=samples[b][i];
  assert(diff>=8);
 }
 /* Invalid input is a no-op, including corrupt dimensions beyond storage. */
 memcpy(before,g_fb,sizeof before);
 terrain_draw(NULL,0,0,0,0);terrain_draw(&g,-1,0,0,0);
 terrain_draw(&g,g.w,0,0,0);terrain_draw(&g,0,g.h,0,0);
 g.w=g.h=255;terrain_draw(&g,MAP_W,MAP_H,0,0);
 g.tiles[0][0].terrain=255;terrain_draw(&g,0,0,0,0);
 assert(memcmp(g_fb,before,sizeof g_fb)==0);
}
static void adjacency_tests(void){
 const int dx[4]={0,1,0,-1},dy[4]={-1,0,1,0};
 for(int water=0;water<2;water++)for(unsigned mask=0;mask<16;mask++){
  reset();g.tiles[2][2].terrain=water?RIVER:ROAD;
  for(int d=0;d<4;d++)if(mask&(1u<<d))g.tiles[2+dy[d]][2+dx[d]].terrain=water?RIVER:ROAD;
  fb_clear(false);terrain_draw(&g,2,2,24,24);
  for(int d=0;d<4;d++){
   int count=0;
   for(int i=0;i<24;i++){
    int x=d==1?11*2:d==3?0:i*2,y=d==0?0:d==2?11*2:i*2;
    bool ink=pixel(24+x,24+y);count+=ink;
    if(ink)assert(i==3||i==(water?8:7));
   }
   assert(count==((mask&(1u<<d))?2:0));
  }
 }
 /* Only ROAD or a real building can continue a road. */
 for(int t=0;t<TERRAIN_COUNT;t++){
  reset();g.tiles[2][2].terrain=ROAD;g.tiles[2][3].terrain=(uint8_t)t;
  fb_clear(false);terrain_draw(&g,2,2,0,0);
  assert(pixel(23,6)==(t==ROAD||t>=CITY));
 }
 /* Coastline disappears into sea and has a six-pixel river mouth. */
 reset();g.tiles[2][2].terrain=SEA;
 fb_clear(false);terrain_draw(&g,2,2,0,0);assert(pixel(0,2));
 g.tiles[2][1].terrain=SEA;
 fb_clear(false);terrain_draw(&g,2,2,0,0);assert(!pixel(0,2));
 g.tiles[2][1].terrain=RIVER;
 fb_clear(false);terrain_draw(&g,2,2,0,0);assert(pixel(0,2));assert(!pixel(0,6));
 /* Map corners never acquire fictional road branches. */
 reset();g.w=g.h=1;g.tiles[0][0].terrain=ROAD;
 fb_clear(false);terrain_draw(&g,0,0,0,0);
 for(int i=0;i<24;i++)assert(!pixel(i,0)&&!pixel(i,23)&&!pixel(0,i)&&!pixel(23,i));
 /* Rendering is deterministic and cannot mutate the game. */
 Game copy=g;memcpy(before,g_fb,sizeof before);terrain_draw(&g,0,0,0,0);
 assert(memcmp(before,g_fb,sizeof before)==0&&memcmp(&copy,&g,sizeof g)==0);
}
static void save_pbm(const char *dir,const char *name){
 char path[512];snprintf(path,sizeof path,"%s/%s.pbm",dir,name);
 FILE *f=fopen(path,"wb");assert(f);fprintf(f,"P4\n296 152\n");
 for(int y=0;y<152;y++)for(int x=0;x<296;x+=8){
  unsigned char b=0;for(int k=0;k<8;k++)if(pixel(x+k,y))b|=0x80u>>k;
  assert(fputc(b,f)!=EOF);
 }
 assert(fclose(f)==0);
}
static void previews(const char *dir){
 assert(mkdir(dir,0700)==0||access(dir,W_OK)==0);
 reset();g.w=12;g.h=5;
 const char *rows[]={
  "cccrcccoooo", "cffrffrcccar", "cccrhccooodr",
  "ppprrrpppddd", "fffrffrcccrrr"};
 for(int y=0;y<5;y++)for(int x=0;x<12;x++){
  char c=rows[y][x];int t=PLAIN;
  switch(c){case 'f':t=FOREST;break;case 'm':t=MOUNTAIN;break;case 'w':t=SEA;break;
   case '-':case '+':case 'r':t=ROAD;break;case 'v':t=RIVER;break;
   case 'c':t=CITY;break;case 'h':t=HQ;break;case 'a':t=AIRPORT;break;
   case 'o':t=FACTORY;break;case 'd':t=PORT;break;}
  g.tiles[y][x].terrain=(uint8_t)t;
 }
 fb_clear(false);fb_text(3,2,"INKWARS / CITY BLOCKS / 1 BIT",true);
 for(int y=0;y<5;y++)for(int x=0;x<12;x++)terrain_draw(&g,x,y,x*24,16+y*24);
 fb_hline(0,135,296,true);fb_text(3,140,"24PX TILES / DENSE PIXEL TERRAIN",true);
 save_pbm(dir,"terrain-map");
 const char *names[]={"GRASS","GROVE","PEAK","RIVER","SEA","ROAD","CITY","WORKS","AIR","PORT","HQ"};
 fb_clear(false);reset();
 for(int t=0;t<TERRAIN_COUNT;t++){
  int sx=(t%5)*59,sy=(t/5)*50;
  reset();g.tiles[2][2].terrain=(uint8_t)t;
  if(t==ROAD||t==RIVER){g.tiles[1][2].terrain=(uint8_t)t;g.tiles[3][2].terrain=(uint8_t)t;}
  if(t==SEA){g.tiles[1][2].terrain=SEA;g.tiles[3][2].terrain=SEA;g.tiles[2][1].terrain=SEA;g.tiles[2][3].terrain=SEA;}
  fb_text(sx+2,sy+2,names[t],true);terrain_draw(&g,2,2,sx+2,sy+18);
  /* Exact 2x nearest-neighbor enlargement alongside the native 12x12. */
  for(int y=0;y<12;y++)for(int x=0;x<12;x++)
   if(pixel(sx+2+x,sy+18+y))fb_fill_rect(sx+23+x*2,sy+13+y*2,2,2,true);
 }
 fb_text(66,112,"NATIVE + 2X",true);fb_text(66,125,"ORIGINAL PIXEL ART",true);
 save_pbm(dir,"terrain-atlas");
}
int main(int argc,char **argv){
 artwork_tests();adjacency_tests();previews(argc>1?argv[1]:"build/previews");
 puts("PASS terrain: 11 distinct tiles, <=50% ink, tile/screen clipping, invalid inputs, all 16 road/river masks, building links, coasts, determinism; 2 PBMs");
 return 0;
}
