/* 主菜单 — 分类导航: 分类页 → 游戏页(每页 10 项) → 游戏 */
#include "menu.h"
#include "../app_runtime.h"
#include <stddef.h>
#include "../config.h"
#include "../gfx/canvas.h"
#include "../gfx/font.h"
#include "../gfx/pattern.h"
#include "../platform/display.h"
#include "../platform/input.h"
#include "../platform/audio.h"
#include "../platform/led.h"

#define MENU_TOP 30
#define MENU_ROW 10
#define MENU_VISIBLE 10                 /* 每页 10 项 */
#define CAT_ITEMS (CAT_COUNT + 1)       /* +1 = ABOUT */

static int s_cat = 0;                   /* 当前分类 */
static int s_sel = 0;                   /* 当前层内选中 */
static bool s_in_games = false;         /* false=分类页 true=游戏页 */

/* ---- 数字转字符串(零 malloc) ---- */
static void u32_to_str(uint32_t v, char out[16]) {
    char buf[16];
    unsigned i = 0;
    do { buf[i++] = (char)('0' + v % 10); v /= 10; } while (v && i < 15);
    unsigned len = i;
    for (unsigned j = 0; j < len; j++) out[j] = buf[len - 1 - j];
    out[len] = 0;
}

/* 该分类下游戏数 */
static int cat_count(int c) {
    int n = 0;
    for (int i = 0; i < GAME_COUNT; i++)
        if ((int)g_games[i].cat == c) n++;
    return n;
}

/* 该分类下第 idx 个游戏 id */
static game_id_t cat_game(int c, int idx) {
    int n = 0;
    for (int i = 0; i < GAME_COUNT; i++) {
        if ((int)g_games[i].cat == c) {
            if (n == idx) return g_games[i].id;
            n++;
        }
    }
    return (game_id_t)GAME_COUNT;   /* 不应到达 */
}

static void menu_draw_title(const char *t) {
    fb_text_scale2((CCG_W - text_width(t) * 2) / 2, 6, t, true);
    fb_hline(0, 26, CCG_W, true);
    fb_fill_tile(0, 27, CCG_W, 3, pat_get(PAT_DOT_DENSE));
}

/* 页码 n/m 显示在标题右侧 */
static void draw_pg(int page, int pages) {
    char pg[16];
    unsigned pi = 0;
    unsigned n = (unsigned)page + 1, m = (unsigned)pages;
    if (n >= 10) pg[pi++] = (char)('0' + n / 10);
    pg[pi++] = (char)('0' + n % 10);
    pg[pi++] = '/';
    if (m >= 10) pg[pi++] = (char)('0' + m / 10);
    pg[pi++] = (char)('0' + m % 10);
    pg[pi] = 0;
    fb_text(CCG_W - 4 - text_width(pg), 6, pg, true);
}

/* ---- 分类页: 9 类 + ABOUT ---- */
static void cat_draw(void) {
    fb_clear(false);
    menu_draw_title(CCG_NAME);
    draw_pg(0, 1);
    int y = MENU_TOP;
    for (int c = 0; c < CAT_ITEMS; c++) {
        if (c == s_sel) {
            fb_fill_rect(0, y, 4, 8, true);
            fb_text_inv(8, y, c == CAT_COUNT ? "ABOUT" : g_cat_names[c]);
            if (c != CAT_COUNT) {
                /* 右端: 分类内游戏数 */
                char cnt[16];
                u32_to_str((uint32_t)cat_count(c), cnt);
                fb_text(CCG_W - 8 - text_width(cnt), y, cnt, true);
            }
        } else {
            fb_text(8, y, c == CAT_COUNT ? "ABOUT" : g_cat_names[c], true);
        }
        y += MENU_ROW;
    }
    fb_text(4, CCG_H - 10, "UP/DN SEL  OK:ENTER  BACK:QUIT", true);
}

/* ---- 游戏页: 当前分类内分页列表 ---- */
static void game_draw(void) {
    fb_clear(false);
    char title[24];
    const char *cn = g_cat_names[s_cat];
    unsigned i = 0;
    while (cn[i] && i < 10) { title[i] = cn[i]; i++; }
    title[i] = 0;
    menu_draw_title(title);
    int total = cat_count(s_cat);
    int pages = (total + MENU_VISIBLE - 1) / MENU_VISIBLE;
    if (pages == 0) pages = 1;
    draw_pg(s_sel / MENU_VISIBLE, pages);
    int page = s_sel / MENU_VISIBLE;
    int base = page * MENU_VISIBLE;
    int y = MENU_TOP;
    for (int i2 = base; i2 < total && i2 < base + MENU_VISIBLE; i2++) {
        game_id_t id = cat_game(s_cat, i2);
        const game_desc_t *g = &g_games[id];
        if (i2 == s_sel) {
            fb_fill_rect(0, y, 4, 8, true);   /* 光标 */
            fb_text_inv(8, y, g->title);
            int tx = CCG_W - 8 - text_width(g->tagline);
            fb_text(tx, y, g->tagline, true);
        } else {
            fb_text(8, y, g->title, true);
        }
        y += MENU_ROW;
    }
    if (pages > 1) {
        fb_text(CCG_W - 10, MENU_TOP, "<", true);
        fb_text(CCG_W - 10, CCG_H - 20, ">", true);
    }
    fb_text(4, CCG_H - 10, "UP/DN SEL  OK:PLAY  BACK:CATS", true);
}

static void about_draw(void) {
    fb_clear(false);
    fb_text_center(8, "ABOUT", true);
    fb_text_center(24, CCG_NAME " v" CCG_VERSION, true);
    fb_text_center(36, "C1-SLIM E-INK GAME SUITE", true);
    fb_text_center(46, "296x152 1BIT EPAPER", true);
    fb_text_center(56, "ZERO-MALLOC STATIC BUILD", true);
    disp_stats_t st;
    disp_get_stats(&st);
    char a1[16], a2[16], a3[16];
    u32_to_str(st.full_refreshes, a1);
    u32_to_str(st.skips, a2);
    u32_to_str(st.fast_since_full, a3);
    char line[48];
    const char *p1 = "FULL:", *p2 = "  SKIP:", *p3 = "  FSF:";
    int n = 0;
    while (*p1) line[n++] = *p1++;
    p1 = a1; while (*p1) line[n++] = *p1++;
    p1 = p2; while (*p1) line[n++] = *p1++;
    p1 = a2; while (*p1) line[n++] = *p1++;
    p1 = p3; while (*p1) line[n++] = *p1++;
    p1 = a3; while (*p1) line[n++] = *p1++;
    line[n] = 0;
    fb_text_center(84, line, true);
    /* 音频统计: played=写入成功, dropped=缓冲满丢弃 */
    audio_stats_t as;
    audio_get_stats(&as);
    char a4[16], a5[16];
    u32_to_str(as.played, a4);
    u32_to_str(as.dropped, a5);
    char l2[48];
    const char *p4 = "  AUDIO:", *p5 = " PLAYED ", *p6 = "  DROP ";
    (void)p6;
    n = 0;
    p1 = p4; while (*p1) l2[n++] = *p1++;
    p1 = a4; while (*p1) l2[n++] = *p1++;
    p1 = p5; while (*p1) l2[n++] = *p1++;
    p1 = a5; while (*p1) l2[n++] = *p1++;
    l2[n] = 0;
    fb_text_center(96, l2, true);
    fb_text_center(CCG_H - 10, "OK/BACK:RETURN", true);
}

/* 供 tests/dump_pbm.c 画面审查使用 */
void menu_draw_for_dump(void) {
    if (s_in_games) game_draw(); else cat_draw();
}
void menu_goto_games(void) { s_in_games = true; s_cat = 0; s_sel = 0; }

static void menu_draw(void) {
    if (s_in_games) game_draw(); else cat_draw();
}

game_id_t menu_run(void) {
    led_fx_set(LED_FX_OFF);   /* 菜单期间灯灭 */
    int before = s_sel;
    menu_draw();
    disp_full();
    for (;;) {
        if (app_runtime_checkpoint()) return (game_id_t)GAME_COUNT;
        input_poll(250);
        if (app_runtime_checkpoint()) return (game_id_t)GAME_COUNT;
        key_event_t ev;
        while (input_get(&ev)) {
            if (ev.is_repeat) continue;
            if (!s_in_games) {
                /* ---- 分类页 ---- */
                switch (ev.key) {
                case K_UP:
                case K_LEFT:
                    s_sel = (s_sel + CAT_ITEMS - 1) % CAT_ITEMS;
                    audio_tick();
                    break;
                case K_DOWN:
                case K_RIGHT:
                    s_sel = (s_sel + 1) % CAT_ITEMS;
                    audio_tick();
                    break;
                case K_OK:
                case K_SPACE:
                    audio_select();
                    if (s_sel == CAT_COUNT) {
                        /* ABOUT 页: 任意返回键退出 */
                        about_draw();
                        disp_full();
                        for (;;) {
                            if (app_runtime_checkpoint()) return (game_id_t)GAME_COUNT;
                            input_poll(250);
                            if (app_runtime_checkpoint()) return (game_id_t)GAME_COUNT;
                            key_event_t ev2;
                            while (input_get(&ev2)) {
                                if (ev2.key == K_OK || ev2.key == K_BACK ||
                                    ev2.key == K_QUIT || ev2.key == K_SPACE) {
                                    audio_move();
                                    s_sel = 0;
                                    before = -1;   /* 强制重绘 */
                                    goto redraw;
                                }
                            }
                        }
                    }
                    s_cat = s_sel;
                    s_sel = 0;
                    s_in_games = true;
                    break;
                case K_BACK:
                case K_QUIT:
                    audio_move();
                    return (game_id_t)GAME_COUNT;   /* 哨兵: 退出应用 */
                case K_CHAR:
                    if (ev.ch == 'w') { s_sel = (s_sel + CAT_ITEMS - 1) % CAT_ITEMS; }
                    else if (ev.ch == 's') { s_sel = (s_sel + 1) % CAT_ITEMS; }
                    else if (ev.ch >= '1' && ev.ch <= '9') {
                        s_sel = ev.ch - '1';
                    } else if (ev.ch == '0') {
                        s_sel = CAT_COUNT;   /* ABOUT */
                    }
                    audio_tick();
                    break;
                default: break;
                }
            } else {
                /* ---- 游戏页 ---- */
                int total = cat_count(s_cat);
                int pages = (total + MENU_VISIBLE - 1) / MENU_VISIBLE;
                if (pages == 0) pages = 1;
                switch (ev.key) {
                case K_UP:
                    s_sel = (s_sel + total - 1) % total;
                    audio_tick();
                    break;
                case K_DOWN:
                    s_sel = (s_sel + 1) % total;
                    audio_tick();
                    break;
                case K_LEFT:
                    s_sel = (s_sel / MENU_VISIBLE - 1 + pages) % pages * MENU_VISIBLE
                             + s_sel % MENU_VISIBLE;
                    if (s_sel >= total) s_sel = total - 1;
                    audio_tick();
                    break;
                case K_RIGHT:
                    s_sel = (s_sel / MENU_VISIBLE + 1) % pages * MENU_VISIBLE
                             + s_sel % MENU_VISIBLE;
                    if (s_sel >= total) s_sel = total - 1;
                    audio_tick();
                    break;
                case K_OK:
                case K_SPACE:
                    audio_select();
                    return cat_game(s_cat, s_sel);
                case K_BACK:
                    s_sel = s_cat;
                    s_in_games = false;
                    audio_move();
                    break;
                case K_QUIT:
                    audio_move();
                    return (game_id_t)GAME_COUNT;
                case K_CHAR:
                    if (ev.ch == 'w') { s_sel = (s_sel + total - 1) % total; }
                    else if (ev.ch == 's') { s_sel = (s_sel + 1) % total; }
                    else if (ev.ch == 'a') {
                        s_sel = (s_sel / MENU_VISIBLE - 1 + pages) % pages * MENU_VISIBLE
                                 + s_sel % MENU_VISIBLE;
                        if (s_sel >= total) s_sel = total - 1;
                    }
                    else if (ev.ch == 'd') {
                        s_sel = (s_sel / MENU_VISIBLE + 1) % pages * MENU_VISIBLE
                                 + s_sel % MENU_VISIBLE;
                        if (s_sel >= total) s_sel = total - 1;
                    }
                    else if (ev.ch >= '1' && ev.ch <= '9') {
                        int idx = ev.ch - '1';
                        if (idx < total) s_sel = idx;
                    } else if (ev.ch == '0' && total >= 10) {
                        s_sel = 9;
                    }
                    audio_tick();
                    break;
                default: break;
                }
            }
            if (s_sel != before || (ev.key != K_UP && ev.key != K_DOWN &&
                ev.key != K_LEFT && ev.key != K_RIGHT && ev.key != K_CHAR)) {
                /* 层切换(OK/BACK)时强制重绘; 方向移动按差异重绘 */
                before = s_sel;
            redraw:
                menu_draw();
                disp_fast();
            }
        }
    }
}
