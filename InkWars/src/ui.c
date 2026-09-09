#include "ui.h"
#include "version.h"
#include "text.h"
#include "terrain.h"
#include "gfx/canvas.h"
#include "gfx/font.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
static const char *main_items[]={"新建对局","继续存档","操作说明","退出游戏"};
static const char *turn_items[]={"结束回合","指挥能力 300","指挥超能 600","全图查看","保存对局","保存退出","返回地图"};
static const char *actions[]={"攻击","占领","待机","单位资料"};
static void scene(UI *u,Screen s){if(u->screen!=s)u->full=true;u->screen=s;u->selection=0;u->dirty=true;}
static void note(UI *u,const char *s,Screen back){snprintf(u->message,sizeof u->message,"%s",s);u->return_to=back;scene(u,INFO);}
static void follow(UI *u){
 int maxx=u->game.w>MAP_VIEW_W?u->game.w-MAP_VIEW_W:0;
 int maxy=u->game.h>MAP_VIEW_H?u->game.h-MAP_VIEW_H:0;
 if(u->cx<u->vx)u->vx=u->cx;
 if(u->cx>=u->vx+MAP_VIEW_W)u->vx=u->cx-(MAP_VIEW_W-1);
 if(u->cy<u->vy)u->vy=u->cy;
 if(u->cy>=u->vy+MAP_VIEW_H)u->vy=u->cy-(MAP_VIEW_H-1);
 if(u->vx>maxx)u->vx=maxx;
 if(u->vy>maxy)u->vy=maxy;
 if(u->vx<0)u->vx=0;
 if(u->vy<0)u->vy=0;
}
static void focus_side(UI *u){for(int y=0;y<u->game.h;y++)for(int x=0;x<u->game.w;x++)if(u->game.tiles[y][x].terrain==HQ && u->game.tiles[y][x].owner==u->game.side){u->cx=x;u->cy=y;}follow(u);}
void ui_init(UI *u,const char *path){memset(u,0,sizeof *u);u->save_path=path;u->screen=MAIN;u->dirty=u->full=true;u->co1=1;u->fog=1;u->unit=-1;}
static void check_win(UI *u){game_check_win(&u->game);if(u->game.winner>=0)scene(u,VICTORY);}
static void end_turn(UI *u){game_end_turn(&u->game);focus_side(u);scene(u,u->game.hotseat?HANDOFF:MAP);check_win(u);}
static bool has_target(UI *u){for(int i=0;i<MAX_UNITS;i++)if(game_can_attack(&u->game,u->unit,i)){u->target=i;u->cx=u->game.units[i].x;u->cy=u->game.units[i].y;follow(u);return true;}return false;}
static void cycle_target(UI *u,int d){for(int n=1;n<=MAX_UNITS;n++){int id=(u->target+d*n+MAX_UNITS*2)%MAX_UNITS;if(game_can_attack(&u->game,u->unit,id)){u->target=id;u->cx=u->game.units[id].x;u->cy=u->game.units[id].y;follow(u);return;}}}
static void production(UI *u){u->prod_count=0;for(int t=0;t<UNIT_TYPES;t++){
 int terrain=u->game.tiles[u->cy][u->cx].terrain;
 if((terrain==FACTORY && t<=ANTIAIR)||(terrain==AIRPORT && (t==FIGHTER||t==BOMBER))||(terrain==PORT && (t==BATTLESHIP||t==LANDER)))u->prod[u->prod_count++]=t;
 }if(u->prod_count)scene(u,PRODUCTION);}
bool ui_ai_pending(const UI *u){return u->active && !u->game.hotseat && u->game.side==1 && u->game.winner<0 && u->screen==MAP;}
void ui_ai(UI *u){
 if(!ui_ai_pending(u))return;
 Unit before[MAX_UNITS];memcpy(before,u->game.units,sizeof before);
 int side=u->game.side;int old_power=u->game.power[side];game_ai_step(&u->game);u->dirty=true;
 if(side!=u->game.side){u->full=true;focus_side(u);}
 int a=-1,b=-1;
 for(int i=0;i<MAX_UNITS;i++){
  if(before[i].alive && before[i].side==side && !before[i].acted && (!u->game.units[i].alive || u->game.units[i].acted) && u->game.units[i].ammo<before[i].ammo)a=i;
  if(before[i].alive && before[i].side!=side && (!u->game.units[i].alive || u->game.units[i].hp<before[i].hp))b=i;
 }
 if(a>=0 && b>=0){u->battle_a=before[a].type;u->battle_b=before[b].type;u->prediction.damage=before[b].hp-u->game.units[b].hp;int hp=before[a].hp;if(!old_power && u->game.power[side]==2 && u->game.co[side]==2){hp+=20;if(hp>100)hp=100;}u->prediction.counter=hp-u->game.units[a].hp;if(u->prediction.counter<0)u->prediction.counter=0;scene(u,RESULT);}else check_win(u);
}
void ui_key(UI *u,UiKey key){
 if(key==UI_NONE)return;
 u->dirty=true;
 int d=(key==UI_DOWN||key==UI_RIGHT)?1:(key==UI_UP||key==UI_LEFT)?-1:0;
 if(u->screen==MAIN){if(d)u->selection=(u->selection+d+4)%4;if(key==UI_OK){switch(u->selection){case 0:scene(u,SETUP);break;case 1:if(game_load(&u->game,u->save_path,u->message,sizeof u->message)){u->active=true;focus_side(u);scene(u,u->game.hotseat?HANDOFF:MAP);check_win(u);}else note(u,"读取失败 原档保留",MAIN);break;case 2:scene(u,HELP);break;case 3:u->quit=true;break;}}return;}
 if(u->screen==HELP){if(key==UI_BACK||key==UI_OK)scene(u,MAIN);return;}
 if(u->screen==SETUP){if(key==UI_UP||key==UI_DOWN)u->setup_row=(u->setup_row+d+7)%7;else if(key==UI_LEFT||key==UI_RIGHT){switch(u->setup_row){case 0:u->map_choice=(u->map_choice+d+MAP_COUNT)%MAP_COUNT;break;case 1:u->co0=(u->co0+d+CO_COUNT)%CO_COUNT;break;case 2:u->co1=(u->co1+d+CO_COUNT)%CO_COUNT;break;case 3:u->hotseat=!u->hotseat;break;case 4:u->fog=!u->fog;break;}}
 if(key==UI_BACK)scene(u,MAIN);
 if(key==UI_OK){if(u->setup_row==6){scene(u,MAIN);}else if(u->setup_row==5){game_new(&u->game,u->map_choice,u->co0,u->co1,u->hotseat,u->fog);u->active=true;u->vx=u->vy=0;focus_side(u);scene(u,MAP);}else ui_key(u,UI_RIGHT);}return;}
 if(u->screen==INFO){if(key==UI_OK||key==UI_BACK)scene(u,u->return_to);return;}
 if(u->screen==VICTORY){if(key==UI_OK||key==UI_BACK)scene(u,MAIN);return;}
 if(u->screen==HANDOFF){if(key==UI_OK)scene(u,MAP);return;}
 if(u->screen==RESULT){if(key==UI_OK||key==UI_BACK){scene(u,MAP);check_win(u);}return;}
 if(u->screen==MINIMAP){if(key==UI_OK||key==UI_BACK)scene(u,MAP);return;}
 if(u->screen==TURN_MENU){if(d)u->selection=(u->selection+d+7)%7;if(key==UI_BACK){scene(u,MAP);return;}if(key==UI_OK){switch(u->selection){case 0:end_turn(u);break;case 1:case 2:if(game_power(&u->game,u->selection==2))note(u,"能力发动 本回合有效",MAP);else note(u,"能量不足或本回合已用",TURN_MENU);break;case 3:scene(u,MINIMAP);break;case 4:case 5:{bool leave=u->selection==5;if(game_save(&u->game,u->save_path,u->message,sizeof u->message)){if(leave)u->quit=true;else note(u,"保存成功",MAP);}else {fprintf(stderr,"Save: %s\n",u->message);note(u,"保存未确认 请检查存档",TURN_MENU);}break;}case 6:scene(u,MAP);break;}}return;}
 if(u->screen==PRODUCTION){if(d)u->selection=(u->selection+d+u->prod_count)%u->prod_count;if(key==UI_BACK)scene(u,MAP);if(key==UI_OK){if(game_produce(&u->game,u->cx,u->cy,u->prod[u->selection]))scene(u,MAP);else note(u,"资金不足或格子被占",PRODUCTION);}return;}
 if(u->screen==ACTION){if(d)u->selection=(u->selection+d+4)%4;if(key==UI_BACK){scene(u,MAP);return;}if(key==UI_OK){switch(u->selection){case 0:if(has_target(u))scene(u,TARGET);else note(u,"射程内没有可攻击敌军",ACTION);break;case 1:if(game_capture(&u->game,u->unit)){scene(u,MAP);check_win(u);}else note(u,"仅步兵机步可占敌建筑",ACTION);break;case 2:game_wait(&u->game,u->unit);scene(u,MAP);break;case 3:{Unit *a=&u->game.units[u->unit];snprintf(u->message,sizeof u->message,"%s HP%d 油%d 弹%d",unit_defs[a->type].name,a->hp,a->fuel,a->ammo);u->return_to=ACTION;scene(u,INFO);break;}}}return;}
 if(u->screen==TARGET){if(d)cycle_target(u,d);if(key==UI_BACK)scene(u,ACTION);if(key==UI_OK){u->battle_a=u->game.units[u->unit].type;u->battle_b=u->game.units[u->target].type;if(game_attack(&u->game,u->unit,u->target,&u->prediction))scene(u,RESULT);else note(u,"目标已不可攻击",ACTION);}return;}
 if(ui_ai_pending(u))return;
 if(u->screen==MAP||u->screen==MOVE){
 if(key==UI_LEFT && u->cx>0)u->cx--;
 if(key==UI_RIGHT && u->cx+1<u->game.w)u->cx++;
 if(key==UI_UP && u->cy>0)u->cy--;
 if(key==UI_DOWN && u->cy+1<u->game.h)u->cy++;
 follow(u);
 if(key==UI_BACK){scene(u,u->screen==MOVE?MAP:TURN_MENU);return;}
 if(key==UI_OK){if(u->screen==MOVE){Unit *a=&u->game.units[u->unit];if(a->x==u->cx && a->y==u->cy){scene(u,ACTION);}else if(game_move(&u->game,u->unit,u->cx,u->cy)){u->cx=a->x;u->cy=a->y;follow(u);if(a->acted)note(u,"遭遇伏兵 行动结束",MAP);else scene(u,ACTION);}else note(u,"超出范围或格子被占",MOVE);return;}
 int id=game_unit_at(&u->game,u->cx,u->cy);if(id>=0 && game_visible_unit(&u->game,u->game.side,id)){
 Unit *a=&u->game.units[id];if(a->side==u->game.side && !a->acted){u->unit=id;if(a->moved)scene(u,ACTION);else {game_range(&u->game,id,u->cost,u->prev);scene(u,MOVE);}}
 else {snprintf(u->message,sizeof u->message,"%s HP%d 油%d 弹%d",unit_defs[a->type].name,a->hp,a->fuel,a->ammo);u->return_to=MAP;scene(u,INFO);}return;}
 Tile *t=&u->game.tiles[u->cy][u->cx];if(t->owner==u->game.side && (t->terrain==FACTORY||t->terrain==AIRPORT||t->terrain==PORT))production(u);else scene(u,TURN_MENU);
 }return;}
}
static void label(int x,int y,const char *s){iw_text(x,y,s,true);}
static void line_item(int y,const char *s,bool selected){if(selected)fb_fill_rect(8,y,280,18,true);iw_text(14,y,s,!selected);}
static void title(const char *s){fb_fill_rect(0,0,296,19,true);iw_center(1,s,false);}
static void foot(const char *s){fb_hline(0,134,296,true);label(3,136,s);}
/* Original one-bit symbols; unit type letter, friendly outline/enemy reverse,
 * acted diagonal corner. Terrain pixels remain visible around each unit. */
static void map_render(UI *u){
 Game *g=&u->game;char b[96];snprintf(b,sizeof b,"%c 日%u $%u +%d",g->side?'B':'A',g->turn,g->money[g->side],game_income(g,g->side));title(b);
 for(int r=0;r<MAP_VIEW_H;r++)for(int c=0;c<MAP_VIEW_W;c++){int mx=u->vx+c,my=u->vy+r,x=c*MAP_TILE_PX,y=MAP_ORIGIN_Y+r*MAP_TILE_PX;if(mx>=g->w||my>=g->h)continue;
 terrain_draw(g,mx,my,x,y);
 if(g->fog&&!g->visible[g->hotseat?g->side:0][my][mx]){for(int yy=0;yy<MAP_TILE_PX;yy+=4)for(int xx=(yy%8)?0:4;xx<MAP_TILE_PX;xx+=8)fb_pixel(x+xx,y+yy,true);}
 if(g->tiles[my][mx].owner>=0 && (!g->fog || g->visible[g->hotseat?g->side:0][my][mx])){if(g->tiles[my][mx].owner==(g->hotseat?g->side:0))fb_hline(x+2,y+MAP_TILE_PX-2,MAP_TILE_PX-5,true);else for(int j=2;j<MAP_TILE_PX-3;j+=4)fb_pixel(x+j,y+MAP_TILE_PX-2,true);}
 int id=game_unit_at(g,mx,my);if(id>=0&&game_visible_unit(g,g->hotseat?g->side:0,id)){Unit *a=&g->units[id];bool enemy=a->side!=(g->hotseat?g->side:0);fb_fill_rect(x+2,y+2,20,20,enemy);fb_stroke_rect(x+2,y+2,20,20,true);char glyph[2]={"IMRTAXFBCL"[a->type],0};fb_text(x+8,y+6,glyph,!enemy);if(a->acted){fb_pixel(x+18,y+2,!enemy);fb_pixel(x+20,y+4,!enemy);}if(a->hp<100)fb_hline(x+4,y+20,(a->hp+5)/6,!enemy);}
 if(u->screen==MOVE && u->cost[my][mx]!=INF_COST){fb_pixel(x,y,true);fb_pixel(x+11,y+11,true);}
 }
 if(u->screen==MOVE && u->cost[u->cy][u->cx]!=INF_COST){int at=u->cy*MAP_W+u->cx;for(int n=0;n<MAP_W*MAP_H;n++){int xx=at%MAP_W,yy=at/MAP_W;if(xx>=u->vx&&xx<u->vx+MAP_VIEW_W&&yy>=u->vy&&yy<u->vy+MAP_VIEW_H)fb_fill_rect((xx-u->vx)*MAP_TILE_PX+10,(yy-u->vy)*MAP_TILE_PX+MAP_ORIGIN_Y+10,4,4,true);int p=u->prev[yy][xx];if(p<0||p==at)break;at=p;}}
 int x=(u->cx-u->vx)*MAP_TILE_PX,y=MAP_ORIGIN_Y+(u->cy-u->vy)*MAP_TILE_PX;fb_stroke_rect(x,y,MAP_TILE_PX,MAP_TILE_PX,true);fb_stroke_rect(x+2,y+2,MAP_TILE_PX-4,MAP_TILE_PX-4,false);
 foot(u->screen==MOVE?"方向选格 OK移动 BACK取消":ui_ai_pending(u)?"敌军思考 按格行动":"方向移动 OK选择 BACK菜单");
}
static void overlay(const char *name){fb_fill_rect(29,23,239,108,false);fb_stroke_rect_thick(29,23,239,108,2,true);fb_fill_rect(31,25,235,18,true);iw_text(38,26,name,false);}
void ui_draw(UI *u){
 fb_clear(false);char b[128];Game *g=&u->game;
 if(u->screen==MAIN){title("墨纸战争 " IW_VERSION);label(65,25,"离线回合制战棋");for(int i=0;i<4;i++)line_item(47+i*20,main_items[i],u->selection==i);foot("方向选择 OK确认");return;}
 if(u->screen==SETUP){title("地图与指挥官");for(int i=0;i<7;i++){switch(i){case 0:snprintf(b,sizeof b,"地图  %s",map_names[u->map_choice]);break;case 1:snprintf(b,sizeof b,"甲方  %s",co_names[u->co0]);break;case 2:snprintf(b,sizeof b,"乙方  %s",co_names[u->co1]);break;case 3:snprintf(b,sizeof b,"模式  %s",u->hotseat?"双人热座":"单人对战");break;case 4:snprintf(b,sizeof b,"迷雾  %s",u->fog?"开启":"关闭");break;case 5:snprintf(b,sizeof b,"开始对局");break;default:snprintf(b,sizeof b,"返回");break;}line_item(22+i*18,b,u->setup_row==i);}return;}
 if(u->screen==HELP){title("操作说明");label(6,23,"方向移动光标 OK选择确认");label(6,43,"BACK取消或打开回合菜单");label(6,63,"自军线框 敌军反白 角缺待机");label(6,83,"步兵占城 工厂生产 总部决胜");label(6,103,"移动后选择攻击 占领或待机");foot("OK返回 详细规则见说明书");return;}
 if(u->screen==HANDOFF){title("交换设备");snprintf(b,sizeof b,"请交给 %c 方指挥官",g->side?'B':'A');iw_center(49,b,true);iw_center(78,"确认后显示本方视野",true);foot("OK开始回合");return;}
 if(u->screen==VICTORY){title("战役结束");snprintf(b,sizeof b,"%c 方获胜",g->winner?'B':'A');iw_center(48,b,true);snprintf(b,sizeof b,"历时 %u 回合",g->turn);iw_center(78,b,true);foot("OK返回主菜单");return;}
 if(u->screen==TURN_MENU){snprintf(b,sizeof b,"回合指令 能量%u",g->charge[g->side]);title(b);for(int i=0;i<7;i++)line_item(22+i*18,turn_items[i],u->selection==i);return;}
 if(u->screen==MINIMAP){title("全图态势");for(int y=0;y<g->h;y++)for(int x=0;x<g->w;x++){Tile *t=&g->tiles[y][x];int xx=8+x*6,yy=22+y*6;if(t->terrain==SEA||t->terrain==RIVER)for(int j=0;j<6;j+=2)fb_hline(xx,yy+j,5,true);if(t->terrain>=CITY)fb_stroke_rect(xx,yy,5,5,true);int id=game_unit_at(g,x,y);if(id>=0&&game_visible_unit(g,g->hotseat?g->side:0,id))fb_fill_rect(xx+1,yy+1,3,3,true);}fb_stroke_rect(8+u->vx*6,22+u->vy*6,108,54,true);label(195,34,"线框视口");label(195,58,"方块单位");label(195,82,"条纹水域");foot("OK或BACK返回地图");return;}
 if(u->screen==INFO){title("提示");iw_center(53,u->message,true);foot("OK或BACK返回");return;}
 if(u->screen==PRODUCTION){title("生产部署");int start=(u->selection/5)*5;for(int i=start;i<u->prod_count&&i<start+5;i++){int t=u->prod[i];snprintf(b,sizeof b,"%s  $%u%s",unit_defs[t].name,unit_defs[t].price*(g->co[g->side]==2?90:100)/100,game_can_produce(g,u->cx,u->cy,t)?"":" 不可");line_item(24+(i-start)*20,b,i==u->selection);}snprintf(b,sizeof b,"资金 %u  OK生产 BACK返回",g->money[g->side]);foot(b);return;}
 map_render(u);
 if(u->screen==ACTION){overlay("单位指令");for(int i=0;i<4;i++){if(i==u->selection)fb_fill_rect(35,46+i*20,227,19,true);iw_text(44,46+i*20,actions[i],i!=u->selection);}return;}
 if(u->screen==TARGET){Prediction p=game_predict(g,u->unit,u->target);overlay("伤害预测");snprintf(b,sizeof b,"%s  >  %s",unit_defs[g->units[u->unit].type].name,unit_defs[g->units[u->target].type].name);label(39,48,b);snprintf(b,sizeof b,"造成 %d  反击 %d",p.damage,p.counter);label(39,71,b);label(39,96,"方向换目标 OK交战");return;}
 if(u->screen==RESULT){overlay("战斗结算");snprintf(b,sizeof b,"%s  >  %s",unit_defs[u->battle_a].name,unit_defs[u->battle_b].name);label(39,48,b);snprintf(b,sizeof b,"目标 -%d  攻方 -%d",u->prediction.damage,u->prediction.counter);label(39,72,b);label(39,97,"OK返回战场");return;}
}
