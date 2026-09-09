#define _FILE_OFFSET_BITS 64
#define _POSIX_C_SOURCE 200809L
#include "game.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Format v1: 8-byte magic, LE32 version, LE32 payload size, LE32 CRC32
 * (IEEE polynomial, payload only), then field-by-field payload. No padding,
 * pointers, native enum representation, or derived visibility is serialized.
 * Header fields are exact constants, so corrupt headers are also rejected.
 * Dead unit records are canonical all-zero bytes. Maximum stack use is small;
 * no heap allocation is used by this implementation.
 */
#define SAVE_VERSION 1U
#define PAYLOAD_SIZE (27U + MAP_W * MAP_H * 3U + MAX_UNITS * 10U)
#define HEADER_SIZE 20U
#define FILE_SIZE (HEADER_SIZE + PAYLOAD_SIZE)
#define PATH_CAP 4096
static const uint8_t magic[8]={'I','W','A','R','S','\r','\n',0x1a};
static bool error(char *err,unsigned cap,const char *message) {
 if(err && cap) (void)snprintf(err,cap,"%s",message);
 return false;
}
static void clear_error(char *err,unsigned cap) { if(err && cap) err[0]='\0'; }
static void put16(uint8_t **p,uint16_t n) { *(*p)++=(uint8_t)n; *(*p)++=(uint8_t)(n>>8); }
static void put32(uint8_t **p,uint32_t n) { for(int i=0;i<4;++i) *(*p)++=(uint8_t)(n>>(8*i)); }
static uint16_t get16(const uint8_t **p) { uint16_t n=(*p)[0]|((uint16_t)(*p)[1]<<8); *p+=2; return n; }
static uint32_t get32(const uint8_t **p) {
 uint32_t n=0; for(int i=0;i<4;++i) n|=(uint32_t)*(*p)++<<(8*i); return n;
}
static uint32_t crc32(const uint8_t *p,unsigned n) {
 uint32_t c=0xffffffffU;
 for(unsigned i=0;i<n;++i) { c^=p[i]; for(int b=0;b<8;++b) c=(c>>1)^(0xedb88320U & (0U-(c&1U))); }
 return c^0xffffffffU;
}
static bool is_property(int t) { return t>=CITY && t<=HQ; }
static bool valid_position(int type,int terrain) {
 if(type==FIGHTER || type==BOMBER) return true;
 if(type==BATTLESHIP || type==LANDER) return terrain==SEA || terrain==PORT;
 if(terrain==SEA) return false;
 if(type!=INFANTRY && type!=MECH && (terrain==MOUNTAIN || terrain==RIVER)) return false;
 return true;
}
static bool valid_game(const Game *g,char *err,unsigned cap) {
 if(!g || g->w!=MAP_W || g->h!=MAP_H || g->map_id>=MAP_COUNT || g->side>1 ||
    g->hotseat>1 || g->fog>1 || g->winner < -1 || g->winner>1 ||
    !g->turn || g->turn>1000000000U)
  return error(err,cap,"存档局面参数无效");
 for(int s=0;s<2;++s)
  if(g->co[s]>=CO_COUNT || g->power[s]>2 || g->charge[s]>1000 || g->money[s]>1000000000U)
   return error(err,cap,"存档指挥官或资金无效");
 int hqs=0,hq_owned[2]={0,0},units[2]={0,0};
 int16_t occupied[MAP_H][MAP_W];
 for(int y=0;y<MAP_H;++y) for(int x=0;x<MAP_W;++x) {
  occupied[y][x]=-1;
  const Tile *t=&g->tiles[y][x];
  if(t->terrain>=TERRAIN_COUNT || t->owner < -1 || t->owner>1 || !t->capture || t->capture>20)
   return error(err,cap,"存档地形或占领点无效");
  if(!is_property(t->terrain) && (t->owner!=-1 || t->capture!=20))
   return error(err,cap,"非建筑地形含非法归属");
  if(t->terrain==HQ) {
   if(t->owner<0) return error(err,cap,"司令部必须有归属");
   ++hqs; ++hq_owned[(int)t->owner];
  }
 }
 if(hqs!=2) return error(err,cap,"存档必须包含两座司令部");
 for(int i=0;i<MAX_UNITS;++i) {
  const Unit *u=&g->units[i];
  if(u->alive>1) return error(err,cap,"存档单位存活标记无效");
  if(!u->alive) {
   if(u->type || u->side || u->x || u->y || u->hp || u->fuel || u->ammo || u->acted || u->moved)
    return error(err,cap,"空单位记录必须为零");
   continue;
  }
  if(u->type>=UNIT_TYPES || u->side>1 || u->x>=MAP_W || u->y>=MAP_H ||
     !u->hp || u->hp>100 || u->acted>1 || u->moved>1)
   return error(err,cap,"存档单位字段无效");
  if(u->fuel>unit_defs[u->type].fuel || u->ammo>unit_defs[u->type].ammo ||
     !valid_position(u->type,g->tiles[u->y][u->x].terrain))
   return error(err,cap,"存档单位补给或位置无效");
  if(occupied[u->y][u->x]>=0) return error(err,cap,"存档单位位置重叠");
  occupied[u->y][u->x]=(int16_t)i; ++units[u->side];
 }
 for(int y=0;y<MAP_H;++y) for(int x=0;x<MAP_W;++x) {
  const Tile *t=&g->tiles[y][x];
  if(t->capture<20) {
   int i=occupied[y][x];
   if(i<0 || (g->units[i].type!=INFANTRY && g->units[i].type!=MECH) || g->units[i].side==t->owner)
    return error(err,cap,"占领进度缺少有效占领者");
  }
 }
 int winner=-1;
 if(!units[0] || !hq_owned[0]) winner=1;
 else if(!units[1] || !hq_owned[1]) winner=0;
 if(g->winner!=winner) return error(err,cap,"存档胜负状态不一致");
 return true;
}
static void encode(const Game *g,uint8_t data[FILE_SIZE]) {
 memcpy(data,magic,sizeof(magic));
 uint8_t *p=data+8; put32(&p,SAVE_VERSION); put32(&p,PAYLOAD_SIZE); put32(&p,0);
 put32(&p,g->turn); put32(&p,g->money[0]); put32(&p,g->money[1]);
 put16(&p,g->charge[0]); put16(&p,g->charge[1]);
 *p++=g->co[0]; *p++=g->co[1]; *p++=g->power[0]; *p++=g->power[1];
 *p++=g->side; *p++=g->hotseat; *p++=g->fog; *p++=g->map_id; *p++=g->w; *p++=g->h;
 *p++=g->winner<0?255U:(uint8_t)g->winner;
 for(int y=0;y<MAP_H;++y) for(int x=0;x<MAP_W;++x) {
  const Tile *t=&g->tiles[y][x]; *p++=t->terrain; *p++=t->owner<0?255U:(uint8_t)t->owner; *p++=t->capture;
 }
 for(int i=0;i<MAX_UNITS;++i) {
  const Unit *u=&g->units[i];
  *p++=u->alive; *p++=u->type; *p++=u->side; *p++=u->x; *p++=u->y;
  *p++=u->hp; *p++=u->fuel; *p++=u->ammo; *p++=u->acted; *p++=u->moved;
 }
 p=data+16; put32(&p,crc32(data+HEADER_SIZE,PAYLOAD_SIZE));
}
static bool decode(const uint8_t data[FILE_SIZE],Game *g,char *err,unsigned cap) {
 if(memcmp(data,magic,sizeof(magic))) return error(err,cap,"存档标识损坏");
 const uint8_t *p=data+8;
 if(get32(&p)!=SAVE_VERSION) return error(err,cap,"不支持的存档版本");
 if(get32(&p)!=PAYLOAD_SIZE) return error(err,cap,"存档长度字段无效");
 if(get32(&p)!=crc32(data+HEADER_SIZE,PAYLOAD_SIZE)) return error(err,cap,"存档校验失败，原文件已保留");
 memset(g,0,sizeof(*g));
 g->turn=get32(&p); g->money[0]=get32(&p); g->money[1]=get32(&p);
 g->charge[0]=get16(&p); g->charge[1]=get16(&p);
 g->co[0]=*p++; g->co[1]=*p++; g->power[0]=*p++; g->power[1]=*p++;
 g->side=*p++; g->hotseat=*p++; g->fog=*p++; g->map_id=*p++; g->w=*p++; g->h=*p++;
 unsigned v=*p++; if(v!=255 && v>1) return error(err,cap,"存档胜者编码无效");
 g->winner=v==255?-1:(int8_t)v;
 for(int y=0;y<MAP_H;++y) for(int x=0;x<MAP_W;++x) {
  Tile *t=&g->tiles[y][x]; t->terrain=*p++; v=*p++;
  if(v!=255 && v>1) return error(err,cap,"存档归属编码无效");
  t->owner=v==255?-1:(int8_t)v; t->capture=*p++;
 }
 for(int i=0;i<MAX_UNITS;++i) {
  Unit *u=&g->units[i];
  u->alive=*p++; u->type=*p++; u->side=*p++; u->x=*p++; u->y=*p++;
  u->hp=*p++; u->fuel=*p++; u->ammo=*p++; u->acted=*p++; u->moved=*p++;
 }
 if(!valid_game(g,err,cap)) return false;
 game_vision(g); return true;
}
static bool read_all(int fd,uint8_t *p,size_t n) {
 while(n) { ssize_t got=read(fd,p,n); if(got<0 && errno==EINTR) continue; if(got<=0) return false; p+=got; n-=(size_t)got; }
 return true;
}
static bool write_all(int fd,const uint8_t *p,size_t n) {
 while(n) { ssize_t sent=write(fd,p,n); if(sent<0 && errno==EINTR) continue; if(sent<=0) return false; p+=sent; n-=(size_t)sent; }
 return true;
}
static bool sync_fd(int fd) { int n; do { n=fsync(fd); } while(n<0 && errno==EINTR); return n==0; }
static bool load_file(Game *g,const char *path,char *err,unsigned cap) {
 int flags=O_RDONLY|O_NONBLOCK;
#ifdef O_NOFOLLOW
 flags|=O_NOFOLLOW;
#endif
 int fd=open(path,flags); if(fd<0) return error(err,cap,"无法打开存档");
 struct stat st;
 bool ok=fstat(fd,&st)==0 && S_ISREG(st.st_mode) && st.st_size==(off_t)FILE_SIZE;
 uint8_t data[FILE_SIZE];
 if(ok) ok=read_all(fd,data,sizeof(data));
 if(ok) { uint8_t extra; ssize_t n; do { n=read(fd,&extra,1); } while(n<0 && errno==EINTR); ok=n==0; }
 if(close(fd)!=0) ok=false;
 if(!ok) return error(err,cap,"存档长度损坏或读取失败");
 return decode(data,g,err,cap);
}
bool game_load(Game *g,const char *path,char *err,unsigned cap) {
 clear_error(err,cap);
 if(!g || !path || !*path) return error(err,cap,"存档路径或目标无效");
 Game candidate;
 if(!load_file(&candidate,path,err,cap)) return false;
 *g=candidate; return true;
}
bool game_save(const Game *g,const char *path,char *err,unsigned cap) {
 clear_error(err,cap);
 if(!path || !*path || strlen(path)>PATH_CAP-16) return error(err,cap,"存档路径无效或过长");
 if(!valid_game(g,err,cap)) return false;
 /* Inspect existing files before creating anything. Corrupt files, symlinks,
  * devices and inaccessible paths are never treated as a fresh save slot. */
 struct stat original;
 bool existed=false;
 if(lstat(path,&original)==0) {
  existed=true;
  if(!S_ISREG(original.st_mode)) return error(err,cap,"拒绝覆盖非普通存档文件");
  Game old;
  if(!load_file(&old,path,err,cap)) return false;
 } else if(errno!=ENOENT) return error(err,cap,"无法检查已有存档");
 char temp[PATH_CAP],directory[PATH_CAP];
 (void)snprintf(temp,sizeof(temp),"%s.tmp.XXXXXX",path);
 const char *slash=strrchr(path,'/');
 if(!slash) (void)snprintf(directory,sizeof(directory),".");
 else if(slash==path) (void)snprintf(directory,sizeof(directory),"/");
 else { size_t n=(size_t)(slash-path); memcpy(directory,path,n); directory[n]='\0'; }
 int flags=O_RDONLY;
#ifdef O_DIRECTORY
 flags|=O_DIRECTORY;
#endif
 int dirfd=open(directory,flags);
 if(dirfd<0) return error(err,cap,"无法打开存档目录");
 int fd=mkstemp(temp);
 if(fd<0) { (void)close(dirfd); return error(err,cap,"无法建立临时存档"); }
 uint8_t data[FILE_SIZE]; encode(g,data);
 bool ok=write_all(fd,data,sizeof(data)) && sync_fd(fd);
 if(close(fd)!=0) ok=false;
 if(!ok) { (void)unlink(temp); (void)close(dirfd); return error(err,cap,"写入或同步失败，原存档已保留"); }
 /* Detect a concurrent replacement before commit. Application is single-writer;
  * no advisory lock is imposed on unrelated programs. Revalidate contents too. */
 struct stat now;
 if(existed) {
  Game old;
  if(lstat(path,&now)!=0 || !S_ISREG(now.st_mode) || now.st_dev!=original.st_dev || now.st_ino!=original.st_ino ||
     !load_file(&old,path,err,cap)) {
   (void)unlink(temp); (void)close(dirfd); return error(err,cap,"原存档已改变或损坏，拒绝覆盖");
  }
 } else if(lstat(path,&now)==0 || errno!=ENOENT) {
  (void)unlink(temp); (void)close(dirfd); return error(err,cap,"存档路径已改变，拒绝覆盖");
 }
 if(rename(temp,path)!=0) { (void)unlink(temp); (void)close(dirfd); return error(err,cap,"替换存档失败，原文件已保留"); }
 ok=sync_fd(dirfd);
 if(close(dirfd)!=0) ok=false;
 if(!ok) return error(err,cap,"存档已替换，但目录同步失败");
 return true;
}
