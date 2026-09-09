#include "ui.h"
#include "runtime.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static UI u;
static const char *out;
static void snap(const char *name){char path[512];snprintf(path,sizeof path,"%s/%s.pbm",out,name);ui_draw(&u);FILE *f=fopen(path,"wb");assert(f);fprintf(f,"P4\n296 152\n");for(int y=0;y<152;y++)for(int x=0;x<296;x+=8){unsigned char v=0;for(int b=0;b<8;b++)if(g_fb[(y>>3)*296+x+b]&(0x80u>>(y&7)))v|=0x80u>>b;assert(fputc(v,f)!=EOF);}assert(fclose(f)==0);}
static void key(UiKey k){ui_key(&u,k);}
static void go(int x,int y){assert(u.screen==MAP||u.screen==MOVE);while(u.cx<x)key(UI_RIGHT);while(u.cx>x)key(UI_LEFT);while(u.cy<y)key(UI_DOWN);while(u.cy>y)key(UI_UP);}
static void select_unit(int x,int y){go(x,y);key(UI_OK);assert(u.screen==MOVE||u.screen==ACTION);}
static void move_to(int x,int y){assert(u.screen==MOVE);go(x,y);key(UI_OK);if(u.screen!=ACTION)fprintf(stderr,"move failed id=%d to=%d,%d turn=%u side=%u screen=%d message=%s\n",u.unit,x,y,u.game.turn,u.game.side,u.screen,u.message);assert(u.screen==ACTION);}
static void act(int n){assert(u.screen==ACTION);for(int i=0;i<n;i++)key(UI_DOWN);key(UI_OK);}
static void end(void){assert(u.screen==MAP);key(UI_BACK);assert(u.screen==TURN_MENU);key(UI_OK);assert(u.screen==HANDOFF);key(UI_OK);assert(u.screen==MAP);}
int main(int argc,char **argv){out=argc>1?argv[1]:"build/previews";assert(mkdir(out,0700)==0||access(out,W_OK)==0);char save[512];snprintf(save,sizeof save,"%s/closed-loop.sav",out);unlink(save);
 ui_init(&u,save);snap("01-main");key(UI_OK);assert(u.screen==SETUP);for(int i=0;i<3;i++)key(UI_DOWN);key(UI_OK);assert(u.hotseat);key(UI_DOWN);key(UI_OK);assert(!u.fog);key(UI_DOWN);snap("02-setup");key(UI_OK);assert(u.screen==MAP);snap("03-map");
 go(4,8);key(UI_OK);assert(u.screen==PRODUCTION);snap("04-production");uint32_t money=u.game.money[0];key(UI_OK);assert(u.screen==MAP);assert(u.game.money[0]==money-1000);assert(game_unit_at(&u.game,4,8)>=0);
 select_unit(3,9);go(6,9);snap("05-movement");key(UI_OK);assert(u.screen==ACTION);snap("06-actions");act(2);assert(u.screen==MAP);end();end();
 select_unit(6,9);move_to(6,12);act(1);assert(u.game.tiles[12][6].capture==10);snap("07-capture");end();end();select_unit(6,12);move_to(6,12);act(1);assert(u.game.tiles[12][6].owner==0);
 select_unit(4,9);move_to(10,9);act(2);end();select_unit(25,9);move_to(19,9);act(2);end();select_unit(10,9);move_to(12,9);act(2);end();select_unit(19,9);move_to(13,9);act(2);end();
 select_unit(12,9);move_to(12,9);act(0);assert(u.screen==TARGET);snap("08-prediction");Prediction p=game_predict(&u.game,u.unit,u.target);assert(p.damage>0&&p.counter>0);int enemy=u.target;int old_hp=u.game.units[enemy].hp;key(UI_OK);assert(u.screen==RESULT);assert(u.game.units[enemy].hp==old_hp-p.damage);snap("09-result");key(UI_OK);assert(u.screen==MAP);
 key(UI_BACK);snap("10-turn-menu");for(int i=0;i<4;i++)key(UI_DOWN);key(UI_OK);assert(u.screen==INFO);assert(access(save,R_OK)==0);snap("11-saved");key(UI_OK);key(UI_BACK);for(int i=0;i<5;i++)key(UI_DOWN);key(UI_OK);assert(u.quit);uint32_t turn=u.game.turn;int hp=u.game.units[enemy].hp;
 ui_init(&u,save);key(UI_DOWN);key(UI_OK);assert(u.screen==HANDOFF);snap("12-handoff");key(UI_OK);assert(u.screen==MAP);assert(u.game.turn==turn&&u.game.units[enemy].hp==hp);assert(u.game.tiles[12][6].owner==0);snap("13-reloaded");key(UI_BACK);for(int i=0;i<3;i++)key(UI_DOWN);key(UI_OK);assert(u.screen==MINIMAP);snap("14-minimap");key(UI_BACK);
 /* Separate declared render fixture: a valid newly-created fog match. */
 game_new(&u.game,1,1,2,false,true);u.cx=2;u.cy=9;u.vx=0;u.vy=1;u.screen=MAP;snap("15-fog-map");
 /* Regression: single-player AI turns must retain player A's fog perspective. */
 u.game.side=1;ui_draw(&u);uint8_t hidden_frame[CCG_FRAME_BYTES];memcpy(hidden_frame,g_fb,sizeof hidden_frame);
 u.game.units[3].y=10;game_vision(&u.game);ui_draw(&u);assert(memcmp(hidden_frame,g_fb,sizeof hidden_frame)==0);
 /* Explicit boundary fixture, separate from the unmodified-map closed loop:
  * exercise the real power button and two-turn HQ victory screen. */
 game_new(&u.game,0,0,1,true,false);u.screen=MAP;u.game.charge[0]=600;
 key(UI_BACK);key(UI_DOWN);key(UI_DOWN);key(UI_OK);assert(u.screen==INFO&&u.game.power[0]==2);key(UI_OK);
 u.game.units[0].x=27;u.game.units[0].y=9;game_vision(&u.game);
 select_unit(27,9);move_to(27,9);act(1);assert(u.game.tiles[9][27].capture==10);end();end();
 select_unit(27,9);move_to(27,9);act(1);assert(u.screen==VICTORY&&u.game.winner==0);key(UI_OK);assert(u.screen==MAIN);
 printf("PASS UI closed loop: new/setup -> production -> movement -> two-turn capture -> attack/prediction/counter -> save -> exit -> reload; %u turns; 15 PBM frames; power/victory/fog regressions\n",turn);return 0;
}
