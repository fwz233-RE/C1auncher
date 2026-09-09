/* Uses ChiChuGames ASCII/canvas and repository licensed 16x16 bitmap subset. */
#include "text.h"
#include "gfx/canvas.h"
#include "gfx/font.h"
#include "cjk.h"
#include <stdint.h>
static unsigned decode(const unsigned char **s) {
 unsigned c=*(*s)++;
 if(c<128) return c;
 if((c&0xe0)==0xc0 && (*s)[0]) { unsigned v=(c&31)<<6; v|=*(*s)++&63; return v; }
 if((c&0xf0)==0xe0 && (*s)[0] && (*s)[1]) {unsigned v=(c&15)<<12;v|=(*(*s)++&63)<<6;v|=*(*s)++&63;return v;}
 return '?';
}
int iw_text(int x,int y,const char *s,bool black) {
 const unsigned char *p=(const unsigned char *)s;
 while(*p) {unsigned c=decode(&p);if(c<128){char a[2]={(char)c,0};fb_text(x,y+4,a,black);x+=6;continue;}
 if(c==0xb7){fb_fill_rect(x+2,y+7,2,2,black);x+=6;continue;}unsigned lo=0,hi=IW_GLYPHS;while(lo<hi){unsigned m=(lo+hi)/2;if(iw_codes[m]<c)lo=m+1;else hi=m;}
 if(lo<IW_GLYPHS && iw_codes[lo]==c){for(int r=0;r<16;r++)for(int b=0;b<16;b++)if(iw_rows[lo][r]&(0x8000u>>b))fb_pixel(x+b,y+r,black);}
 else fb_stroke_rect(x+2,y+2,12,12,black);
 x+=16;
 }return x;
}
void iw_center(int y,const char *s,bool black){const unsigned char *p=(const unsigned char *)s;int w=0;while(*p){unsigned c=decode(&p);w+=(c<128||c==0xb7)?6:16;}iw_text((296-w)/2,y,s,black);}
