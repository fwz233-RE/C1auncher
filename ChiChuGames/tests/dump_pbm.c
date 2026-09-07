/* 画面审查 — host 渲染各游戏开局帧到 PBM(P4), 可转 PNG 查看 */
#include "../src/config.h"
#include "../src/gfx/canvas.h"
#include "../src/gfx/font.h"
#include "../src/gfx/pattern.h"
#include "../src/games/game.h"
#include "../src/platform/display.h"   /* g_fb */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* host 框架 stub(g_fb 由 display.c 提供) */
bool s_exit_request = false;
void game_set_tick_interval(uint32_t ms) { (void)ms; }

static void write_pbm(const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(1); }
    fprintf(f, "P4\n%u %u\n", CCG_W, CCG_H);
    fwrite(g_fb, 1, CCG_FRAME_BYTES, f);
    fclose(f);
    printf("wrote %s\n", path);
}

extern void menu_draw_for_dump(void);
extern void menu_goto_games(void);

int main(int argc, char **argv) {
    if (argc > 1 && argv[1][0] == 'g') menu_goto_games();
    /* 菜单 */
    fb_clear(false);
    menu_draw_for_dump();
    write_pbm("build/golden/menu.pbm");

    /* 各游戏开局帧(enter 完成初始化+渲染) */
    for (int i = 0; i < GAME_COUNT; i++) {
        fb_clear(false);
        g_games[i].enter();
        char path[128];
        snprintf(path, sizeof(path), "build/golden/game%02d.pbm", i);
        write_pbm(path);
    }
    return 0;
}
