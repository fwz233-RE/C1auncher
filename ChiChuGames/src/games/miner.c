/* MINER — 挖矿升级: 9x5 矿层, 挖煤/铁/金攒金币, 升级铲子/矿灯/炸弹, 闯 5 层
 *
 * 玩法(回合制, 输入驱动, 无 tick):
 *   - 方向键/WASD 移动(可长按重复), OK 挖当前格, B 炸弹清 3x3, SPACE 商店
 *   - 未踏足的格子是"岩壁"(交叉线); 走到/挖到才揭示; 矿灯升级后自动揭示邻格
 *   - 矿石有耐久: 煤 1 击 / 铁 2 击 / 金 3 击; 铲子每级 +1 伤害(0..2 级)
 *   - 金币: 煤 1 / 铁 3 / 金 8; 每 10 步移动 -1 体力; 体力 0 或挖通 5 层结束
 *   - 每层矿石数递增(12/16/20/24/28); 层清完 +1 炸弹(上限 3)进入下一层
 * 布局: 9x5 格 27px 方形(243x135) 紧贴 HUD 下沿水平居中
 * 图案: 煤=稀疏点 铁=斜线 金=实心+白星 岩壁=交叉线 已挖=空白
 * HUD: 左 MINER, 右 LV n GOLD n HP n; 结束: 左上结果 + 右上 OK/N:RETRY BACK:QUIT,
 *       墙内 3x 大字; 开局/换层/商店 disp_full, 游戏内 disp_fast
 * 静态前缀 mn_; 零 malloc; 像素坐标 int; 全部文本 ASCII
 *
 * 集成提示(help[] 最多 5 行, 由 games_table 提供):
 *   "MINER", "DIG ORES, BUY UPGRADES, GO DEEP",
 *   "ARROWS/WASD: MOVE  OK: DIG", "B: BOMB(3X3)  SPACE: SHOP",
 *   "COAL 1 IRON 3 GOLD 8  HP -1/10 STEPS", NULL
 */
#include "game.h"
#include "../config.h"
#include "../gfx/canvas.h"
#include "../gfx/font.h"
#include "../gfx/pattern.h"
#include "../platform/display.h"
#include "../platform/input.h"
#include "../platform/audio.h"
#include "../platform/led.h"
#include "../platform/time.h"
#include "../rng.h"
#include <stdio.h>
#include <string.h>

#define MN_COLS        9
#define MN_ROWS        5
#define MN_LAYERS      5
#define MN_CELL        27              /* 27px 方形格(28 放不下 HUD 后 136px 区) */
#define MN_BOARD_W     (MN_COLS * MN_CELL)   /* 243 */
#define MN_BOARD_H     (MN_ROWS * MN_CELL)   /* 135 */
#define MN_X0          ((CCG_W - MN_BOARD_W) / 2)  /* 26 */
#define MN_Y0          CCG_HUD_H              /* 16 */
#define MN_CELLS       (MN_COLS * MN_ROWS)    /* 45 */

#define MN_MAX_HP      30              /* 体力上限(每 10 步 -1) */
#define MN_MAX_SHOVEL  2               /* 铲子最高级(伤害 1+级数) */
#define MN_MAX_BOMB    3               /* 炸弹持有上限 */
#define MN_COST_LAMP   12              /* 矿灯一次性 */
#define MN_COST_BOMB   10              /* 炸弹单颗 */
#define MN_SHOP_ITEMS  4

typedef enum { MN_O_NONE = 0, MN_O_COAL, MN_O_IRON, MN_O_GOLD } mn_ore_t;

typedef enum { MN_ST_PLAY, MN_ST_SHOP, MN_ST_NEXT, MN_ST_OVER } mn_state_t;

void miner_render(void);

/* ---- 每层矿石数量(煤/铁/金), 密度随深度递增 ---- */
static const uint8_t mn_ore_table[MN_LAYERS][3] = {
    { 8, 3, 1 },    /* L1: 12 */
    { 9, 5, 2 },    /* L2: 16 */
    { 9, 8, 3 },    /* L3: 20 */
    { 9, 11, 4 },   /* L4: 24 */
    { 9, 13, 6 },   /* L5: 28 */
};
static const int mn_value[4]   = { 0, 1, 3, 8 };    /* 煤1 铁3 金8 */
static const int mn_ore_hp[4]  = { 0, 1, 2, 3 };    /* 耐久: 煤1 铁2 金3 */
static const int mn_shovel_cost[2] = { 5, 8 };      /* 铲子 1/2 级 */

/* ---- 对局状态 ---- */
static mn_state_t mn_state;
static uint8_t mn_cell[MN_CELLS];   /* 矿石类型 */
static uint8_t mn_hp[MN_CELLS];     /* 剩余耐久, 0 = 已挖/空地 */
static uint8_t mn_rev[MN_CELLS];    /* 1 = 已揭示 */
static uint8_t mn_crack[MN_CELLS];  /* 1 = 挖过未挖穿(裂痕) */
static int mn_px, mn_py;            /* 玩家位置 0..8 / 0..4 */
static int mn_layer;                /* 0..4 */
static int mn_ores_left;            /* 本层剩余矿石 */
static int mn_gold;                 /* 金币总数 */
static int mn_hp_now;               /* 体力 */
static int mn_moves;                /* 累计移动步数 */
static int mn_shovel;               /* 0..2 */
static bool mn_lamp;                /* 矿灯: 自动揭示邻格 */
static int mn_bombs;                /* 炸弹数量 */
static int mn_sel;                  /* 商店光标 */
static bool mn_win;                 /* 胜利 */
static bool mn_over_full;           /* 结束全刷只做一次 */
static uint32_t mn_gens;            /* 换局计数(种子混合) */
static rng_t mn_rng;

/* ---- 矿工小人精灵: 9x12, '#'=黑(头盔+脸+躯干+腿) ---- */
static const char *const mn_miner_spr[12] = {
    "...###...",
    "..#####..",
    "..#####..",
    ".#######.",
    ".##.#.##.",
    "...###...",
    ".#######.",
    "..#####..",
    "..#..#...",
    "..#..#...",
    "..#..#...",
    ".##..##..",
};

/* ---- 5x7 字形 3x 放大(每字符 15x21) ---- */
static void mn_text3(int x, int y, const char *s, bool black) {
    for (const char *p = s; *p; p++) {
        const uint8_t *g = font_glyph5x7[(unsigned char)*p];
        for (int j = 0; j < FONT_H; j++)
            for (int i = 0; i < FONT_W; i++)
                if (g[j] & (1u << i))
                    fb_fill_rect(x + i * 3, y + j * 3, 3, 3, black);
        x += FONT_ADV * 3;
    }
}

static void mn_text3_center(int y, const char *s, bool black) {
    int w = ((int)strlen(s) * FONT_ADV - 1) * 3;
    int x = (CCG_W - w) / 2;
    if (x < 0) x = 0;
    mn_text3(x, y, s, black);
}

static int mn_idx(int x, int y) { return y * MN_COLS + x; }

static void mn_reveal_cell(int x, int y) {
    if (x < 0 || x >= MN_COLS || y < 0 || y >= MN_ROWS) return;
    mn_rev[mn_idx(x, y)] = 1;
}

/* 生成当前层: 按配比装袋 + 洗牌; 出生格(0,4)保证空地 */
static void mn_gen_layer(void) {
    const uint8_t *t = mn_ore_table[mn_layer];
    for (int i = 0; i < MN_CELLS; i++) {
        int o = MN_O_NONE;
        if (i < t[0]) o = MN_O_COAL;
        else if (i < t[0] + t[1]) o = MN_O_IRON;
        else if (i < t[0] + t[1] + t[2]) o = MN_O_GOLD;
        mn_cell[i] = (uint8_t)o;
        mn_hp[i] = (uint8_t)mn_ore_hp[o];
        mn_rev[i] = 0;
        mn_crack[i] = 0;
    }
    for (int i = MN_CELLS - 1; i > 0; i--) {        /* Fisher-Yates */
        int j = (int)rng_range(&mn_rng, (uint32_t)(i + 1));
        uint8_t c = mn_cell[i]; mn_cell[i] = mn_cell[j]; mn_cell[j] = c;
        uint8_t h = mn_hp[i];   mn_hp[i]   = mn_hp[j];   mn_hp[j]   = h;
    }
    {   /* 出生格(0,4)不放矿石: 与某个空格交换 */
        int s = mn_idx(0, 4);
        int n = MN_CELLS - 1;
        while (mn_cell[n] != MN_O_NONE) n--;        /* 空格恒 >= 17 个 */
        if (mn_cell[s] != MN_O_NONE) {
            uint8_t c = mn_cell[s]; mn_cell[s] = mn_cell[n]; mn_cell[n] = c;
            uint8_t h = mn_hp[s];   mn_hp[s]   = mn_hp[n];   mn_hp[n]   = h;
        }
    }
    mn_ores_left = t[0] + t[1] + t[2];
    mn_px = 0;
    mn_py = 4;
    mn_rev[mn_idx(0, 4)] = 1;
}

/* 层清空 → 进下一层或胜利 */
static void mn_layer_done(void) {
    if (mn_layer >= MN_LAYERS - 1) {
        mn_win = true;
        mn_state = MN_ST_OVER;
        audio_win();
        led_fx_set(LED_FX_WIN);
        return;
    }
    mn_layer++;
    if (mn_bombs < MN_MAX_BOMB) mn_bombs++;
    mn_gen_layer();
    mn_state = MN_ST_NEXT;
    audio_clear();
    miner_render();
    disp_full();
}

static void mn_move(int dx, int dy) {
    int nx = mn_px + dx, ny = mn_py + dy;
    if (nx < 0 || nx >= MN_COLS || ny < 0 || ny >= MN_ROWS) return;
    mn_px = nx;
    mn_py = ny;
    mn_reveal_cell(nx, ny);
    if (mn_lamp) {
        for (int yy = -1; yy <= 1; yy++)
            for (int xx = -1; xx <= 1; xx++)
                mn_reveal_cell(nx + xx, ny + yy);
    }
    mn_moves++;
    if (mn_moves % 10 == 0) {       /* 每 10 步 -1 体力 */
        mn_hp_now--;
        if (mn_hp_now <= 0) {
            mn_win = false;
            mn_state = MN_ST_OVER;
            audio_lose();
            led_fx_set(LED_FX_LOSE);
        }
    }
}

static void mn_dig(void) {
    int i = mn_idx(mn_px, mn_py);
    mn_rev[i] = 1;
    if (mn_cell[i] == MN_O_NONE || mn_hp[i] == 0) return;
    int dmg = 1 + mn_shovel;
    if (mn_hp[i] > dmg) {
        mn_hp[i] = (uint8_t)(mn_hp[i] - dmg);
        mn_crack[i] = 1;
        return;
    }
    mn_hp[i] = 0;
    mn_crack[i] = 0;
    mn_gold += mn_value[mn_cell[i]];
    mn_ores_left--;
    if (mn_ores_left <= 0) mn_layer_done();
}

static void mn_use_bomb(void) {
    if (mn_bombs <= 0) return;
    mn_bombs--;
    for (int y = mn_py - 1; y <= mn_py + 1; y++) {
        if (y < 0 || y >= MN_ROWS) continue;
        for (int x = mn_px - 1; x <= mn_px + 1; x++) {
            if (x < 0 || x >= MN_COLS) continue;
            int i = mn_idx(x, y);
            mn_rev[i] = 1;
            if (mn_cell[i] != MN_O_NONE && mn_hp[i] > 0) {
                mn_hp[i] = 0;
                mn_crack[i] = 0;
                mn_gold += mn_value[mn_cell[i]];
                mn_ores_left--;
            }
        }
    }
    if (mn_ores_left <= 0) mn_layer_done();
}

static bool mn_shop_can_buy(int sel) {
    switch (sel) {
    case 0: return mn_shovel < MN_MAX_SHOVEL &&
                   mn_gold >= mn_shovel_cost[mn_shovel];
    case 1: return !mn_lamp && mn_gold >= MN_COST_LAMP;
    case 2: return mn_bombs < MN_MAX_BOMB && mn_gold >= MN_COST_BOMB;
    default: return true;   /* BACK */
    }
}

static void mn_shop_buy(void) {
    switch (mn_sel) {
    case 0:
        if (mn_shovel < MN_MAX_SHOVEL &&
            mn_gold >= mn_shovel_cost[mn_shovel]) {
            mn_gold -= mn_shovel_cost[mn_shovel];
            mn_shovel++;
        }
        break;
    case 1:
        if (!mn_lamp && mn_gold >= MN_COST_LAMP) {
            mn_gold -= MN_COST_LAMP;
            mn_lamp = true;
        }
        break;
    case 2:
        if (mn_bombs < MN_MAX_BOMB && mn_gold >= MN_COST_BOMB) {
            mn_gold -= MN_COST_BOMB;
            mn_bombs++;
        }
        break;
    default:
        break;
    }
}

static void mn_new_game(void) {
    mn_gens++;
    rng_seed(&mn_rng, now_ms() ^ ((uint64_t)mn_gens * 0x9E3779B1u));
    mn_layer = 0;
    mn_gold = 0;
    mn_hp_now = MN_MAX_HP;
    mn_moves = 0;
    mn_shovel = 0;
    mn_lamp = false;
    mn_bombs = 1;
    mn_win = false;
    mn_over_full = false;
    mn_gen_layer();
    mn_state = MN_ST_PLAY;
    miner_render();
    disp_full();
}

void miner_enter(void) { mn_new_game(); }
void miner_exit(void) {}
void miner_tick(uint64_t now) { (void)now; }

/* ---- 金星(金矿上的白色火花) ---- */
static void mn_gold_star(int cx, int cy) {
    const int s = 4;
    fb_hline(cx - s, cy, s * 2 + 1, false);
    fb_vline(cx, cy - s, s * 2 + 1, false);
    for (int k = 1; k <= s; k++) {
        fb_pixel(cx - k, cy - k, false);
        fb_pixel(cx + k, cy + k, false);
        fb_pixel(cx - k, cy + k, false);
        fb_pixel(cx + k, cy - k, false);
    }
}

/* ---- 矿工(白底黑小人, 压在格子上) ---- */
static void mn_draw_miner(void) {
    int cx = MN_X0 + mn_px * MN_CELL;
    int cy = MN_Y0 + mn_py * MN_CELL;
    fb_fill_rect(cx + 6, cy + 4, 15, 19, false);
    for (int j = 0; j < 12; j++) {
        const char *row = mn_miner_spr[j];
        for (int i = 0; i < 9; i++)
            if (row[i] == '#')
                fb_pixel(cx + 9 + i, cy + 7 + j, true);
    }
}

/* ---- 商店面板 ---- */
static void mn_draw_shop(void) {
    char l[24], r[16];
    int x = 40, y = 26, w = 216, h = 92;
    fb_fill_rect(x, y, w, h, false);
    fb_stroke_rect_thick(x, y, w, h, 2, true);
    snprintf(l, sizeof(l), "SHOP  GOLD %d", mn_gold);
    fb_text_center(y + 3, l, true);
    for (int i = 0; i < MN_SHOP_ITEMS; i++) {
        switch (i) {
        case 0:
            if (mn_shovel < MN_MAX_SHOVEL) {
                snprintf(l, sizeof(l), "SHOVEL LV%d", mn_shovel);
                snprintf(r, sizeof(r), "COST %d", mn_shovel_cost[mn_shovel]);
            } else {
                snprintf(l, sizeof(l), "SHOVEL LV%d", mn_shovel);
                snprintf(r, sizeof(r), "MAX");
            }
            break;
        case 1:
            snprintf(l, sizeof(l), "LAMP");
            if (mn_lamp) snprintf(r, sizeof(r), "ON");
            else snprintf(r, sizeof(r), "COST %d", MN_COST_LAMP);
            break;
        case 2:
            snprintf(l, sizeof(l), "BOMB x%d", mn_bombs);
            if (mn_bombs >= MN_MAX_BOMB) snprintf(r, sizeof(r), "MAX");
            else snprintf(r, sizeof(r), "COST %d", MN_COST_BOMB);
            break;
        default:
            snprintf(l, sizeof(l), "BACK");
            r[0] = 0;
            break;
        }
        bool inv = (i == mn_sel) && mn_shop_can_buy(i);
        int ry = y + 17 + i * 12;
        int lx = x + 10;
        int rx = x + w - 10 - text_width(r);
        if (inv) {
            fb_fill_rect(x + 4, ry - 1, w - 8, 9, true);
            fb_text(lx, ry, l, false);
            if (r[0]) fb_text(rx, ry, r, false);
        } else {
            fb_text(lx, ry, l, true);
            if (r[0]) fb_text(rx, ry, r, true);
        }
    }
    fb_text_center(y + 78, "OK BUY  BACK CLOSE", true);
}

/* ---- 渲染 ---- */
void miner_render(void) {
    fb_clear(false);
    char buf[40];

    /* 结束画面: HUD 两行 + 墙内 3x 大字, 只全刷一次 */
    if (mn_state == MN_ST_OVER) {
        fb_fill_rect(0, 0, CCG_W, CCG_HUD_H - 1, false);
        if (mn_win) fb_text(2, 2, "MINE CLEARED", true);
        else        fb_text(2, 2, "OUT OF STAMINA", true);
        snprintf(buf, sizeof(buf), "GOLD %d LV %d", mn_gold, mn_layer + 1);
        fb_text(2, 9, buf, true);
        fb_text(CCG_W - 2 - text_width("OK/N:RETRY BACK:QUIT"), 2,
                "OK/N:RETRY BACK:QUIT", true);
        if (mn_win) mn_text3_center(40, "MINE CLEARED", true);
        else        mn_text3_center(40, "OUT OF STAMINA", true);
        snprintf(buf, sizeof(buf), "GOLD %d", mn_gold);
        mn_text3_center(76, buf, true);
        if (!mn_over_full) { mn_over_full = true; disp_force_full(); }
        return;
    }

    /* HUD: 左 MINER, 右 LV n GOLD n HP n */
    fb_text(0, 0, "MINER", true);
    snprintf(buf, sizeof(buf), "LV %d GOLD %d HP %d", mn_layer + 1,
             mn_gold, mn_hp_now);
    fb_text(CCG_W - 4 - text_width(buf), 0, buf, true);
    fb_hline(0, CCG_HUD_H - 1, CCG_W, true);

    /* 矿层格子 */
    for (int y = 0; y < MN_ROWS; y++) {
        for (int x = 0; x < MN_COLS; x++) {
            int i = mn_idx(x, y);
            int cx = MN_X0 + x * MN_CELL;
            int cy = MN_Y0 + y * MN_CELL;
            if (!mn_rev[i]) {
                fb_fill_tile(cx, cy, MN_CELL, MN_CELL, pat_get(PAT_CROSS));
            } else if (mn_hp[i] > 0) {
                if (mn_cell[i] == MN_O_COAL)
                    fb_fill_tile(cx, cy, MN_CELL, MN_CELL, pat_get(PAT_DOT_SPARSE));
                else if (mn_cell[i] == MN_O_IRON)
                    fb_fill_tile(cx, cy, MN_CELL, MN_CELL, pat_get(PAT_SLASH_S));
                else if (mn_cell[i] == MN_O_GOLD) {
                    fb_fill_tile(cx, cy, MN_CELL, MN_CELL, pat_get(PAT_SOLID));
                    mn_gold_star(cx + MN_CELL / 2, cy + MN_CELL / 2);
                }
                if (mn_crack[i])
                    fb_hline(cx, cy + MN_CELL / 2, MN_CELL, false);  /* 裂痕 */
            }
        }
    }

    /* 网格线 */
    for (int i = 0; i <= MN_COLS; i++)
        fb_vline(MN_X0 + i * MN_CELL, MN_Y0, MN_BOARD_H, true);
    for (int j = 0; j <= MN_ROWS; j++)
        fb_hline(MN_X0, MN_Y0 + j * MN_CELL, MN_BOARD_W, true);

    /* 矿工(最后画, 覆盖格子) */
    mn_draw_miner();

    /* 换层横幅 */
    if (mn_state == MN_ST_NEXT) {
        snprintf(buf, sizeof(buf), "LAYER %d", mn_layer + 1);
        mn_text3_center(56, buf, true);
        fb_text_center(100, "PRESS ANY KEY", true);
    }

    /* 商店面板(盖在棋盘上) */
    if (mn_state == MN_ST_SHOP) mn_draw_shop();
}

/* ---- 输入 ---- */
static bool mn_is_dir(const key_event_t *ev) {
    if (ev->key == K_UP || ev->key == K_DOWN ||
        ev->key == K_LEFT || ev->key == K_RIGHT)
        return true;
    if (ev->key == K_CHAR && (ev->ch == 'w' || ev->ch == 'a' ||
                              ev->ch == 's' || ev->ch == 'd'))
        return true;
    return false;
}

void miner_on_key(const key_event_t *ev) {
    if (ev->is_repeat && !(mn_state == MN_ST_PLAY && mn_is_dir(ev)))
        return;                          /* 只有移动键响应长按 */

    if (mn_state == MN_ST_OVER) {
        if (ev->key == K_OK || (ev->key == K_CHAR && ev->ch == 'n'))
            mn_new_game();
        else if (ev->key == K_BACK || ev->key == K_QUIT)
            s_exit_request = true;
        return;
    }

    if (mn_state == MN_ST_NEXT) {        /* 换层横幅: 任意键开挖 */
        if (ev->key == K_QUIT || (ev->key == K_CHAR && ev->ch == 'q'))
            s_exit_request = true;
        else if (ev->key == K_BACK || ev->key == K_PAUSE) {
            pause_sel_t sel;
            if (ui_pause_run(&sel)) {
                if (sel == PAUSE_RESTART) mn_new_game();
            } else {
                s_exit_request = true;
            }
        } else if (ev->key == K_CHAR && ev->ch == 'n') {
            mn_new_game();
        } else {
            mn_state = MN_ST_PLAY;
        }
        return;
    }

    if (mn_state == MN_ST_SHOP) {
        switch (ev->key) {
        case K_UP:
        case K_LEFT:
            mn_sel = (mn_sel + 3) % MN_SHOP_ITEMS;
            break;
        case K_DOWN:
        case K_RIGHT:
            mn_sel = (mn_sel + 1) % MN_SHOP_ITEMS;
            break;
        case K_OK:
            if (mn_sel == MN_SHOP_ITEMS - 1) {
                mn_state = MN_ST_PLAY;
                miner_render();
                disp_full();
            } else if (mn_shop_can_buy(mn_sel)) {
                mn_shop_buy();
            }
            break;
        case K_BACK:
        case K_PAUSE:
            mn_state = MN_ST_PLAY;
            miner_render();
            disp_full();
            break;
        case K_QUIT:
            s_exit_request = true;
            break;
        default:
            break;
        }
        return;
    }

    /* ---- PLAY ---- */
    if (ev->key == K_CHAR) {
        switch (ev->ch) {
        case 'w': mn_move(0, -1); if (!ev->is_repeat) audio_move(); return;
        case 's': mn_move(0, 1);  if (!ev->is_repeat) audio_move(); return;
        case 'a': mn_move(-1, 0); if (!ev->is_repeat) audio_move(); return;
        case 'd': mn_move(1, 0);  if (!ev->is_repeat) audio_move(); return;
        case 'b': mn_use_bomb();  return;
        case 'n': mn_new_game();  return;
        case 'q': s_exit_request = true; return;
        default: return;
        }
    }
    switch (ev->key) {
    case K_UP:    mn_move(0, -1); if (!ev->is_repeat) audio_move(); break;
    case K_DOWN:  mn_move(0, 1);  if (!ev->is_repeat) audio_move(); break;
    case K_LEFT:  mn_move(-1, 0); if (!ev->is_repeat) audio_move(); break;
    case K_RIGHT: mn_move(1, 0);  if (!ev->is_repeat) audio_move(); break;
    case K_SPACE: mn_state = MN_ST_SHOP;
                  mn_sel = 0;
                  miner_render();
                  disp_full();
                  break;
    case K_OK:    mn_dig(); break;
    case K_BACK:
    case K_PAUSE: {
        pause_sel_t sel;
        if (ui_pause_run(&sel)) {
            if (sel == PAUSE_RESTART) mn_new_game();
        } else {
            s_exit_request = true;
        }
        break;
    }
    case K_QUIT:  s_exit_request = true; break;
    default:      break;
    }
}
