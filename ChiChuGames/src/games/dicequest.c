/* DICE QUEST — 掷骰冒险: 网格地牢探索(输入驱动, 无 tick)
 * 8x5 格 24px 方形棋盘 192x120, 屏幕居中(OX=52, OY=24); 左右侧栏
 * 每关: 1 宝石(菱形) 1 门 3 随机事件格(?), 均不与玩家起点(0,0)重叠
 * 方向键/WASD 移动小人; 踩上 ? 掷 d6:
 *   1-2 遇敌(再掷战斗: 1-3 损失 1 HP; 4-6 胜 +3 金)
 *   3-4 得 2 金;  5-6 得 1 钥匙
 * 事件格可反复触发(每次踩入掷骰, 风险自选)
 * 宝石 +10 金并开门; 踩已开门 → 下一关(钥匙 x5 折算奖金, HP 回满)
 * 通过第 10 关 → YOU WIN; HP 0 → FAILED
 * HUD: 左 DICE QUEST, 右 LV n HP n GOLD n [KEY n]; 第 2 行状态消息
 * 侧栏: 左 HP 大字反白 + 图例(YOU/ROLL/GEM/DOOR); 右 GOLD 大字反白 +
 *       骰子(LAST 点数) + KEYS 大字
 * 静态前缀 dq_; 零 malloc; 随机循环带 guard(64 次 + 线性兜底); 像素 int
 */
#include "game.h"
#include "../config.h"
#include "../gfx/canvas.h"
#include "../gfx/font.h"
#include "../platform/display.h"
#include "../platform/input.h"
#include "../platform/time.h"
#include "../platform/audio.h"
#include "../platform/led.h"
#include "../rng.h"

#define DQ_COLS 8
#define DQ_ROWS 5
#define DQ_CELL 24
#define DQ_OX ((int)(CCG_W - (unsigned)DQ_COLS * DQ_CELL) / 2)   /* 52 */
#define DQ_OY (CCG_HUD_H + ((int)CCG_H - CCG_HUD_H - (unsigned)DQ_ROWS * DQ_CELL) / 2)  /* 24 */
#define DQ_MAX_HP 5
#define DQ_MAX_LV 10
#define DQ_MSG_CAP 32

#define DQ_DIE_X 252          /* 骰子(右侧栏) */
#define DQ_DIE_Y 64
#define DQ_DIE 20

static rng_t dq_rng;
static int dq_px, dq_py;            /* 玩家格坐标 */
static int dq_gx, dq_gy;            /* 宝石格 */
static int dq_dx, dq_dy;            /* 门格 */
static int dq_evx[3], dq_evy[3];    /* 事件格 */
static int dq_lv;                   /* 1..10 */
static int dq_hp;                   /* 0..5 */
static int dq_gold;
static int dq_keys;
static bool dq_have_gem;            /* 宝石已收集 → 门开 */
static bool dq_over;                /* 结束(胜或败) */
static bool dq_win;
static bool dq_over_full;           /* 结束全刷防重复 */
static int dq_die;                  /* 最近骰面 1..6, 0=未掷 */
static char dq_msg[DQ_MSG_CAP];     /* HUD 状态消息 */

void dicequest_render(void);

/* ---- 手写字符串拼接(零 snprintf) ---- */
static void dq_append(char *buf, int cap, int *n, const char *s) {
    while (*s && *n < cap - 1) buf[(*n)++] = *s++;
}

static void dq_append_int(char *buf, int cap, int *n, int v) {
    char tmp[12];
    int len = 0;
    if (v == 0) tmp[len++] = '0';
    while (v > 0 && len < 11) { tmp[len++] = (char)('0' + v % 10); v /= 10; }
    while (len > 0 && *n < cap - 1) buf[(*n)++] = tmp[--len];
}

static void dq_msg_set(const char *s) {
    int n = 0;
    dq_append(dq_msg, DQ_MSG_CAP, &n, s);
    dq_msg[n] = 0;
}

static void dq_msg_set_int(const char *s, int v) {
    int n = 0;
    dq_append(dq_msg, DQ_MSG_CAP, &n, s);
    dq_append_int(dq_msg, DQ_MSG_CAP, &n, v);
    dq_msg[n] = 0;
}

/* HUD 右侧统计串: "LV n HP n GOLD n [KEY n]" */
static void dq_stats(char *buf, int cap) {
    int n = 0;
    dq_append(buf, cap, &n, "LV ");
    dq_append_int(buf, cap, &n, dq_lv);
    dq_append(buf, cap, &n, " HP ");
    dq_append_int(buf, cap, &n, dq_hp);
    dq_append(buf, cap, &n, " GOLD ");
    dq_append_int(buf, cap, &n, dq_gold);
    if (dq_keys > 0) {
        dq_append(buf, cap, &n, " KEY ");
        dq_append_int(buf, cap, &n, dq_keys);
    }
    buf[n] = 0;
}

static int dq_roll(void) {
    return (int)rng_range(&dq_rng, 6) + 1;
}

/* ---- 关卡生成: 门/宝石/3 事件随机落点, 互不重叠且避开起点 ---- */
static bool dq_occ(int x, int y) {
    if (x == 0 && y == 0) return true;              /* 玩家起点 */
    if (x == dq_dx && y == dq_dy) return true;
    if (x == dq_gx && y == dq_gy) return true;
    for (int i = 0; i < 3; i++)
        if (x == dq_evx[i] && y == dq_evy[i]) return true;
    return false;
}

static void dq_place(int *ox, int *oy) {
    int i;
    for (i = 0; i < 64; i++) {                      /* 随机尝试(有界) */
        int x = (int)rng_range(&dq_rng, DQ_COLS);
        int y = (int)rng_range(&dq_rng, DQ_ROWS);
        if (!dq_occ(x, y)) { *ox = x; *oy = y; return; }
    }
    for (int y = 0; y < DQ_ROWS; y++)               /* 兜底: 线性扫描(必终止) */
        for (int x = 0; x < DQ_COLS; x++)
            if (!dq_occ(x, y)) { *ox = x; *oy = y; return; }
    *ox = 0; *oy = 0;                               /* 理论不可达 */
}

static void dq_new_level(void) {
    dq_dx = dq_dy = -1;
    dq_gx = dq_gy = -1;
    dq_evx[0] = dq_evx[1] = dq_evx[2] = -1;
    dq_evy[0] = dq_evy[1] = dq_evy[2] = -1;
    dq_place(&dq_dx, &dq_dy);
    dq_place(&dq_gx, &dq_gy);
    for (int i = 0; i < 3; i++) dq_place(&dq_evx[i], &dq_evy[i]);
    dq_px = 0;
    dq_py = 0;
    dq_hp = DQ_MAX_HP;
    dq_have_gem = false;
    dq_die = 0;
}

/* 踩门过关: 钥匙 x5 折算金币, 进入下一关; 第 10 关后胜利 */
static void dq_advance(void) {
    int bonus = dq_keys * 5;
    dq_gold += bonus;
    dq_keys = 0;
    dq_lv++;
    if (dq_lv > DQ_MAX_LV) {
        dq_over = true;
        dq_win = true;
        dq_over_full = false;
        audio_win();                   /* 通关 */
        led_fx_set(LED_FX_WIN);
        return;
    }
    dq_new_level();
    audio_clear();                     /* 过关 */
    if (bonus > 0) dq_msg_set_int("LEVEL UP! KEYS +", bonus);
    else dq_msg_set("LEVEL UP!");
}

/* 事件格掷骰(踩入时调用) */
static void dq_do_roll(void) {
    int r = dq_roll();
    if (r <= 2) {                                   /* 遇敌: 战斗再掷 */
        dq_die = dq_roll();
        if (dq_die <= 3) {                          /* 战败: -1 HP */
            dq_hp--;
            dq_msg_set("ENEMY! -1 HP");
            if (dq_hp <= 0) {
                dq_over = true;
                dq_win = false;
                dq_over_full = false;
                audio_lose();          /* HP 归零 */
                led_fx_set(LED_FX_LOSE);
            } else {
                audio_error();         /* 受伤未死 */
            }
        } else {                                    /* 战胜: +3 金 */
            dq_gold += 3;
            dq_msg_set("ENEMY! WIN +3 GOLD");
        }
    } else if (r <= 4) {                            /* 宝藏: +2 金 */
        dq_die = r;
        dq_gold += 2;
        dq_msg_set("GOLD +2");
    } else {                                        /* 5-6: +1 钥匙 */
        dq_die = r;
        dq_keys++;
        dq_msg_set("KEY +1");
    }
}

/* 尝试移动; 出界不动; 目标格事件即时结算(门/宝石/掷骰) */
static void dq_try_move(int dx, int dy) {
    int nx = dq_px + dx;
    int ny = dq_py + dy;
    if (nx < 0 || ny < 0 || nx >= DQ_COLS || ny >= DQ_ROWS) return;
    dq_px = nx;
    dq_py = ny;
    if (nx == dq_gx && ny == dq_gy && !dq_have_gem) {
        dq_have_gem = true;
        dq_gold += 10;
        dq_msg_set("GEM! +10 GOLD DOOR OPEN");
    } else if (nx == dq_dx && ny == dq_dy) {
        if (dq_have_gem) dq_advance();
        else dq_msg_set("DOOR NEEDS GEM");
    } else {
        for (int i = 0; i < 3; i++) {
            if (nx == dq_evx[i] && ny == dq_evy[i]) { dq_do_roll(); break; }
        }
    }
}

static void dq_new_game(void) {
    rng_seed(&dq_rng, now_ms() ^ 0x9E3779B97F4A7C15ULL);
    dq_gold = 0;
    dq_keys = 0;
    dq_lv = 1;
    dq_over = false;
    dq_win = false;
    dq_over_full = false;
    dq_new_level();
    dq_msg_set("LEVEL 1");
}

void dicequest_enter(void) {
    dq_new_game();
    dicequest_render();
    disp_full();
}

/* ---- 绘制 ---- */

/* 小人: 头 + 身体 + 双腿(24px 格内) */
static void dq_draw_person(int cx, int cy) {
    fb_fill_rect(cx + 5, cy + 3, 8, 6, true);   /* 头 */
    fb_fill_rect(cx + 4, cy + 9, 10, 7, true);  /* 身体 */
    fb_fill_rect(cx + 4, cy + 16, 4, 4, true);  /* 左腿 */
    fb_fill_rect(cx + 10, cy + 16, 4, 4, true); /* 右腿 */
}

/* 宝石: 实心菱形 19 宽 20 高 + 高光 */
static void dq_draw_gem(int cx, int cy) {
    for (int i = 0; i < 20; i++) {
        int half = i < 10 ? i : 19 - i;
        fb_fill_rect(cx + 12 - half, cy + 2 + i, half * 2 + 1, 1, true);
    }
    fb_fill_rect(cx + 10, cy + 5, 2, 2, false);
}

/* 门: 锁=实心黑+白锁孔; 开=描边+黑锁孔 */
static void dq_draw_door(int cx, int cy, bool open) {
    if (open) {
        fb_stroke_rect(cx + 4, cy + 2, 16, 20, true);
        fb_fill_rect(cx + 10, cy + 8, 4, 4, true);
        fb_fill_rect(cx + 11, cy + 12, 2, 6, true);
    } else {
        fb_fill_rect(cx + 4, cy + 2, 16, 20, true);
        fb_fill_rect(cx + 10, cy + 8, 4, 4, false);
        fb_fill_rect(cx + 11, cy + 12, 2, 6, false);
    }
}

/* 2x 反白大字(黑底白字), 居中于 w 宽 */
static void dq_draw_big(int x, int y, int w, int v) {
    char b[12];
    int n = 0;
    dq_append_int(b, (int)sizeof b, &n, v);
    b[n] = 0;
    fb_fill_rect(x, y, w, 14, true);
    int tw = n * (FONT_ADV * 2);
    fb_text_scale2(x + (w - tw) / 2, y, b, false);
}

/* d6 点阵(3x3 槽位 6px 间隔), 每面 6 点, {-1,-1} 结束 */
static const int8_t dq_pips[7][6][2] = {
    { { -1, -1 }, { -1, -1 }, { -1, -1 }, { -1, -1 }, { -1, -1 }, { -1, -1 } },
    { { 1, 1 }, { -1, -1 }, { -1, -1 }, { -1, -1 }, { -1, -1 }, { -1, -1 } },
    { { 0, 0 }, { 2, 2 }, { -1, -1 }, { -1, -1 }, { -1, -1 }, { -1, -1 } },
    { { 0, 0 }, { 1, 1 }, { 2, 2 }, { -1, -1 }, { -1, -1 }, { -1, -1 } },
    { { 0, 0 }, { 0, 2 }, { 2, 0 }, { 2, 2 }, { -1, -1 }, { -1, -1 } },
    { { 0, 0 }, { 0, 2 }, { 1, 1 }, { 2, 0 }, { 2, 2 }, { -1, -1 } },
    { { 0, 0 }, { 0, 1 }, { 0, 2 }, { 2, 0 }, { 2, 1 }, { 2, 2 } }
};

static void dq_draw_die(void) {
    fb_stroke_rect(DQ_DIE_X, DQ_DIE_Y, DQ_DIE, DQ_DIE, true);
    if (dq_die < 1 || dq_die > 6) return;
    for (int k = 0; k < 6; k++) {
        int px = dq_pips[dq_die][k][0];
        int py = dq_pips[dq_die][k][1];
        if (px < 0) break;
        fb_fill_rect(DQ_DIE_X + 4 + px * 6, DQ_DIE_Y + 4 + py * 6, 3, 3, true);
    }
}

/* 左侧栏: HP 大字 + 图例 */
static void dq_draw_legend(void) {
    dq_draw_person(2, 56);
    fb_text(24, 62, "YOU", true);
    fb_text_scale2(7, 76, "?", true);
    fb_text(24, 80, "ROLL", true);
    dq_draw_gem(2, 94);
    fb_text(24, 100, "GEM", true);
    dq_draw_door(2, 116, false);
    fb_text(24, 121, "DOOR", true);
}

static void dq_draw_sidebars(void) {
    fb_text(6, 26, "HP", true);
    dq_draw_big(6, 36, 24, dq_hp);
    dq_draw_legend();

    fb_text(250, 26, "GOLD", true);
    dq_draw_big(250, 36, 36, dq_gold > 999 ? 999 : dq_gold);
    dq_draw_die();
    fb_text(250, 88, "LAST", true);
    fb_text(250, 100, "KEYS", true);
    dq_draw_big(250, 110, 24, dq_keys > 99 ? 99 : dq_keys);
}

static void dq_draw_board(void) {
    for (int y = 0; y < DQ_ROWS; y++)
        for (int x = 0; x < DQ_COLS; x++)
            fb_stroke_rect(DQ_OX + x * DQ_CELL, DQ_OY + y * DQ_CELL, DQ_CELL, DQ_CELL, true);
    dq_draw_door(DQ_OX + dq_dx * DQ_CELL, DQ_OY + dq_dy * DQ_CELL, dq_have_gem);
    if (!dq_have_gem)
        dq_draw_gem(DQ_OX + dq_gx * DQ_CELL, DQ_OY + dq_gy * DQ_CELL);
    for (int i = 0; i < 3; i++)
        fb_text_scale2(DQ_OX + dq_evx[i] * DQ_CELL + 7, DQ_OY + dq_evy[i] * DQ_CELL + 5, "?", true);
    dq_draw_person(DQ_OX + dq_px * DQ_CELL, DQ_OY + dq_py * DQ_CELL);  /* 最上层 */
}

void dicequest_render(void) {
    fb_clear(false);
    if (dq_over) {
        fb_fill_rect(0, 0, CCG_W, CCG_HUD_H - 1, false);
        fb_text(2, 2, dq_win ? "YOU WIN!" : "FAILED", true);
        char r1[24];
        int n = 0;
        if (dq_win) {
            dq_append(r1, (int)sizeof r1, &n, "GOLD ");
            dq_append_int(r1, (int)sizeof r1, &n, dq_gold);
        } else {
            dq_append(r1, (int)sizeof r1, &n, "REACHED LV ");
            dq_append_int(r1, (int)sizeof r1, &n, dq_lv);
        }
        r1[n] = 0;
        fb_text(2, 9, r1, true);
        fb_text(CCG_W - 2 - text_width("OK/N:RETRY BACK:QUIT"), 2,
                "OK/N:RETRY BACK:QUIT", true);
        if (!dq_over_full) { dq_over_full = true; disp_force_full(); }
    } else {
        fb_text(0, 0, "DICE QUEST", true);
        char st[32];
        dq_stats(st, (int)sizeof st);
        fb_text(CCG_W - 2 - text_width(st), 0, st, true);
        if (dq_msg[0]) fb_text(2, 8, dq_msg, true);
        fb_hline(0, CCG_HUD_H - 1, CCG_W, true);
    }
    dq_draw_sidebars();
    dq_draw_board();
}

/* ---- 输入(输入驱动, 无 tick) ---- */

void dicequest_on_key(const key_event_t *ev) {
    bool dir = ev->key == K_UP || ev->key == K_DOWN ||
               ev->key == K_LEFT || ev->key == K_RIGHT;
    if (ev->is_repeat && !dir) return;      /* 确认键/字母忽略重复, 方向可重复 */
    if (dq_over) {
        if (ev->key == K_OK || (ev->key == K_CHAR && ev->ch == 'n'))
            dicequest_enter();
        else if (ev->key == K_BACK || ev->key == K_QUIT)
            s_exit_request = true;
        return;
    }
    switch (ev->key) {
    case K_UP:    dq_try_move(0, -1); break;
    case K_DOWN:  dq_try_move(0, 1); break;
    case K_LEFT:  dq_try_move(-1, 0); break;
    case K_RIGHT: dq_try_move(1, 0); break;
    case K_CHAR:
        switch (ev->ch) {
        case 'w': dq_try_move(0, -1); break;
        case 's': dq_try_move(0, 1); break;
        case 'a': dq_try_move(-1, 0); break;
        case 'd': dq_try_move(1, 0); break;
        case 'n': dicequest_enter(); break;
        default: break;
        }
        break;
    case K_OK: break;                       /* 游戏内确认无操作 */
    case K_BACK:
    case K_PAUSE: {
        pause_sel_t sel = PAUSE_RESUME;
        if (ui_pause_run(&sel)) {
            if (sel == PAUSE_RESTART) dicequest_enter();
        } else {
            s_exit_request = true;
        }
        break;
    }
    case K_QUIT: s_exit_request = true; break;
    default: break;
    }
}

void dicequest_tick(uint64_t now) { (void)now; }
void dicequest_exit(void) {}
