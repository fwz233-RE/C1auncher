/* 菜单分类导航静态验证 */
#include "../../src/games/game.h"
#include <stdio.h>
#include <string.h>
static int fail = 0;
#define CHECK(c,m) do{ if(!(c)){printf("FAIL: %s\n",m);fail++;} else printf("ok: %s\n",m);}while(0)
bool s_exit_request = false;
void game_set_tick_interval(uint32_t ms) { (void)ms; }
int main(void){
    /* 每个分类至少 1 款, 总数 = GAME_COUNT */
    int sum = 0, seen[CAT_COUNT] = {0};
    for (int i = 0; i < GAME_COUNT; i++) {
        int c = (int)g_games[i].cat;
        CHECK(c >= 0 && c < CAT_COUNT, "cat in range");
        seen[c]++;
        sum++;
    }
    CHECK(sum == GAME_COUNT, "all games in a category");
    CHECK(sum == 86, "86 games total");
    const int expect[CAT_COUNT] = {23,13,7,7,9,6,3,8,10};
    for (int c = 0; c < CAT_COUNT; c++) {
        char msg[64];
        snprintf(msg, sizeof msg, "%s count %d", g_cat_names[c], seen[c]);
        CHECK(seen[c] == expect[c], msg);
    }
    /* cat_game 映射: 每分类第 0 项 id 合法且 cat 匹配 */
    for (int c = 0; c < CAT_COUNT; c++) {
        game_id_t id0 = 0;
        for (int i = 0; i < GAME_COUNT; i++)
            if ((int)g_games[i].cat == c) { id0 = g_games[i].id; break; }
        CHECK((int)g_games[id0].cat == c, "first game cat matches");
    }
    /* 名称唯一性 */
    for (int i = 0; i < GAME_COUNT; i++)
        for (int j = i+1; j < GAME_COUNT; j++)
            CHECK(strcmp(g_games[i].title, g_games[j].title) != 0, "titles unique");
    printf(fail ? "FAILED\n" : "ALL PASS\n");
    return fail ? 1 : 0;
}
/* 框架 stub 定义(链接补充) */
