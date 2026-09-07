/* TOWER DEFENSE — 回合制路径塔防 (C1-Slim 墨水屏)
 *
 * 12x7 格 19px 方形 = 228x133, 水平居中 OX=34, HUD 下 OY=16, 底缘 149 < 152。
 *   (设计稿 20px x 7 行 = 140 高, 加 HUD 16 后 156 超出屏高 152 —— 取 19px)
 * 敌人沿固定 28 格蛇形路径从左上 (0,0) 走到右下 (11,6) 基地, 每 tick 走 1 格。
 * 玩家在路径旁空格用 OK 打开炮塔菜单放置 3 种塔(金币不足的选项禁用/斜纹):
 *   1: 射程1 伤害1 $5    2: 射程2 伤害2 $10    3: 射程3 伤害4 $20
 * 射程 = 切比雪夫距离; 每 tick 每塔攻击射程内路径最靠前的敌人(聚焦火力)。
 * 杀敌 +2 金币; 5 波递增(4/6/8/9/10 个, HP 1/3/8/25/40, 生成间隔 4/4/3/3/2);
 * 波间 6 tick 喘息; 敌人进入基地 → DEFEATED; 清完 5 波 → VICTORY。
 * 回合顺序: 生成 → 敌人前进 → 塔攻击 → 进基地判定(攻击在判定前, 守基地的
 *   塔能在敌人踏入基地那 tick 击杀, 奖励基地布防)。
 *
 * tick 700ms —— 设计稿 500ms, 但平台规格实测: 本面板 ~700ms/帧, 500ms 连续
 *   写帧会丢帧跳格("游戏 tick 必须 >=700ms"), 故调整为 700ms。
 *
 * 操作: 方向/WASD 移光标; OK 炮塔菜单(BACK 取消); P/BACK 暂停; N 新局; Q 退出
 * HUD 顶栏: 左 TOWER DEFENSE, 右 GOLD n WAVE n; 结束时 HUD 两行:
 *   左上 VICTORY!/DEFEATED!, 右上 OK/N:RETRY BACK:QUIT, 全刷一次。
 * 静态前缀 td_; 零 malloc; 静态数据 < 300B。
 *
 * 集成时 help 行(<=5 行 + NULL):
 *   "TOWER DEFENSE: BUILD TOWERS ALONG",
 *   "THE PATH. ENEMIES WALK TOP-LEFT TO",
 *   "BASE (B) BOTTOM-RIGHT.",
 *   "OK: BUILD  ARROWS/WASD: MOVE",
 *   "N:NEW Q:QUIT P:PAUSE", NULL
 */
#include "game.h"
#include "../app_runtime.h"
#include "../config.h"
#include "../gfx/canvas.h"
#include "../gfx/font.h"
#include "../gfx/pattern.h"
#include "../platform/display.h"
#include "../platform/input.h"
#include "../platform/audio.h"
#include "../platform/led.h"

#define TD_COLS 12
#define TD_ROWS 7
#define TD_CELL 19                          /* 12x7x19 = 228x133 */
#define TD_BW (TD_COLS * TD_CELL)
#define TD_BH (TD_ROWS * TD_CELL)
#define TD_OX (((int)CCG_W - TD_BW) / 2)    /* 34: 水平居中 */
#define TD_OY CCG_HUD_H                      /* 16: HUD 之下 */
#define TD_PATH_LEN 28
#define TD_MAX_EN 14
#define TD_MAX_TOWERS 16
#define TD_WAVES 5
#define TD_START_GOLD 20
#define TD_KILL_GOLD 2
#define TD_BREAK_TICKS 6

/* 固定路径(格号 = y*12+x): 左上 (0,0) 蛇形到右下 (11,6) 基地, 共 28 格 */
static const uint8_t td_path[TD_PATH_LEN] = {
    0, 1, 2, 3, 4, 5,                        /* (0,0)-(5,0) */
    17, 29,                                  /* (5,1),(5,2) */
    28, 27, 26, 25, 24,                      /* (4,2)..(0,2) */
    36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, /* (0,3)-(11,3) */
    59,                                      /* (11,4) */
    71, 83                                   /* (11,5),(11,6) */
};

/* 塔属性: 下标 = 塔类型(1..3), 0 号不用 */
static const int td_cost[4] = { 0, 5, 10, 20 };
static const int td_range[4] = { 0, 1, 2, 3 };
static const int td_dmg[4] = { 0, 1, 2, 4 };

/* 波次表: 数量 / HP / 生成间隔(tick); 5 波递增 */
static const int td_wcnt[TD_WAVES] = { 4, 6, 8, 9, 10 };
static const int td_whp[TD_WAVES] = { 1, 3, 8, 25, 40 };
static const int td_wint[TD_WAVES] = { 4, 4, 3, 3, 2 };

/* 炮塔菜单条目(与 td_cost/td_range/td_dmg 一致) */
static const char *td_item[3] = { "1 R1 D1 $5", "2 R2 D2 $10", "3 R3 D4 $20" };

static uint8_t td_tx[TD_MAX_TOWERS];   /* 塔位置与类型 */
static uint8_t td_ty[TD_MAX_TOWERS];
static uint8_t td_tt[TD_MAX_TOWERS];
static int td_tn;                       /* 塔数量 */
static int16_t td_epos[TD_MAX_EN];      /* 敌人路径步号; -1 = 无 */
static int16_t td_ehp[TD_MAX_EN];
static int td_gold;
static int td_wave;                     /* 1..5 */
static int td_spawned;                  /* 本波已生成数 */
static int td_stick;                    /* 生成计时 */
static int td_break;                    /* 波间喘息剩余 tick */
static int td_cx, td_cy;                /* 光标格坐标 */
static bool td_over;
static bool td_win;
static bool td_over_full;               /* 结束全刷只做一次 */
static bool td_menu_open;               /* 菜单期间不画光标 */
static int td_menu_sel;                 /* 0..2 塔, 3=CANCEL */

void towerdefense_enter(void);          /* 前向声明(暂停/重开使用) */

static int td_px(int s) { return td_path[s] % TD_COLS; }
static int td_py(int s) { return td_path[s] / TD_COLS; }

static bool td_on_path(int x, int y) {
    for (int s = 0; s < TD_PATH_LEN; s++)
        if (td_path[s] == y * TD_COLS + x) return true;
    return false;
}

static bool td_cell_buildable(int x, int y) {
    if (x < 0 || x >= TD_COLS || y < 0 || y >= TD_ROWS) return false;
    if (td_on_path(x, y)) return false;
    for (int t = 0; t < td_tn; t++)
        if (td_tx[t] == x && td_ty[t] == y) return false;
    return true;
}

/* 放置塔(菜单与测试共用); 失败返回 false */
static bool td_place_tower(int x, int y, int type) {
    if (type < 1 || type > 3) return false;
    if (!td_cell_buildable(x, y)) return false;
    if (td_tn >= TD_MAX_TOWERS) return false;
    if (td_gold < td_cost[type]) return false;
    td_tx[td_tn] = (uint8_t)x;
    td_ty[td_tn] = (uint8_t)y;
    td_tt[td_tn] = (uint8_t)type;
    td_tn++;
    td_gold -= td_cost[type];
    audio_select();
    return true;
}

static void td_kill_enemy(int i) {
    td_epos[i] = -1;
    td_gold += TD_KILL_GOLD;
    audio_clear();
}

static bool td_in_range(int t, int s) {
    int dx = (int)td_tx[t] - td_px(s);
    int dy = (int)td_ty[t] - td_py(s);
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    return dx <= td_range[td_tt[t]] && dy <= td_range[td_tt[t]];
}

/* 一个回合: 生成 → 前进 → 塔攻击 → 进基地判定 → 波清判定 */
static void td_tick_turn(void) {
    if (td_over) return;
    if (td_break > 0) { td_break--; return; }
    if (td_spawned < td_wcnt[td_wave - 1]) {
        td_stick++;
        if (td_stick >= td_wint[td_wave - 1]) {
            td_stick = 0;
            for (int i = 0; i < TD_MAX_EN; i++) {
                if (td_epos[i] < 0) {
                    td_epos[i] = 0;
                    td_ehp[i] = (int16_t)td_whp[td_wave - 1];
                    break;
                }
            }
            td_spawned++;
        }
    }
    for (int i = 0; i < TD_MAX_EN; i++)
        if (td_epos[i] >= 0) td_epos[i]++;
    for (int t = 0; t < td_tn; t++) {
        int target = -1, lead = -1;
        for (int i = 0; i < TD_MAX_EN; i++) {
            if (td_epos[i] < 0) continue;
            if (td_epos[i] > lead && td_in_range(t, td_epos[i])) {
                lead = td_epos[i];
                target = i;
            }
        }
        if (target >= 0) {
            td_ehp[target] -= (int16_t)td_dmg[td_tt[t]];
            if (td_ehp[target] <= 0) td_kill_enemy(target);
        }
    }
    for (int i = 0; i < TD_MAX_EN; i++) {
        if (td_epos[i] >= TD_PATH_LEN - 1) {   /* 进基地 → 败 */
            td_epos[i] = -1;
            td_over = true;
            td_win = false;
            audio_lose();
            led_fx_set(LED_FX_LOSE);
            return;
        }
    }
    {
        int alive = 0;
        for (int i = 0; i < TD_MAX_EN; i++)
            if (td_epos[i] >= 0) alive++;
        if (alive == 0 && td_spawned >= td_wcnt[td_wave - 1]) {
            if (td_wave >= TD_WAVES) {
                td_over = true;
                td_win = true;
                audio_win();
                led_fx_set(LED_FX_WIN);
                return;
            }
            td_wave++;
            td_spawned = 0;
            td_stick = 0;
            td_break = TD_BREAK_TICKS;
        }
    }
}

/* 光标移动(方向键/WASD; 越界钳制) */
static void td_move(int dx, int dy) {
    td_cx += dx;
    td_cy += dy;
    if (td_cx < 0) td_cx = 0;
    if (td_cx >= TD_COLS) td_cx = TD_COLS - 1;
    if (td_cy < 0) td_cy = 0;
    if (td_cy >= TD_ROWS) td_cy = TD_ROWS - 1;
}

/* ---- 炮塔菜单(模态, 3 选 1 + CANCEL; 金币不足禁用=跳过+斜纹) ---- */
static int td_menu_step(int dir) {
    for (int i = 0; i < 4; i++) {
        td_menu_sel += dir;
        if (td_menu_sel < 0) td_menu_sel = 3;
        if (td_menu_sel > 3) td_menu_sel = 0;
        if (td_menu_sel == 3) return 3;
        if (td_gold >= td_cost[td_menu_sel + 1]) return td_menu_sel;
    }
    return td_menu_sel;
}

static void td_menu_draw(void) {
    int w = 124, h = 55;
    int x = ((int)CCG_W - w) / 2;
    int y = ((int)CCG_H - h) / 2;
    fb_fill_rect(x, y, w, h, false);
    fb_stroke_rect_thick(x, y, w, h, 2, true);
    fb_text_center(y + 2, "BUILD TOWER", true);
    for (int i = 0; i < 4; i++) {
        int iy = y + 13 + i * 10;
        if (i == 3) {
            if (td_menu_sel == 3) fb_text_inv(x + 10, iy, "CANCEL");
            else fb_text(x + 10, iy, "CANCEL", true);
        } else {
            if (td_gold < td_cost[i + 1]) /* 买不起: 斜纹禁用 */
                fb_fill_tile(x + 8, iy - 1, w - 16, 9, pat_get(PAT_DOT_DENSE));
            if (td_menu_sel == i) fb_text_inv(x + 10, iy, td_item[i]);
            else fb_text(x + 10, iy, td_item[i], true);
        }
    }
}

/* 返回 true=选了塔(写入 *type), false=取消 */
static bool td_menu_run(int *type) {
    td_menu_open = true;
    td_menu_sel = -1;
    td_menu_step(1);
    td_menu_draw();
    disp_fast();
    for (;;) {
        if (app_runtime_checkpoint()) {
            td_menu_open = false;
            s_exit_request = true;
            return false;
        }
        input_poll(200);
        if (app_runtime_checkpoint()) {
            td_menu_open = false;
            s_exit_request = true;
            return false;
        }
        key_event_t ev;
        while (input_get(&ev)) {
            if (ev.is_repeat) continue;
            switch (ev.key) {
            case K_UP:
                td_menu_step(-1);
                td_menu_draw();
                disp_fast();
                break;
            case K_DOWN:
                td_menu_step(1);
                td_menu_draw();
                disp_fast();
                break;
            case K_OK:
                td_menu_open = false;
                if (td_menu_sel == 3) return false;
                *type = td_menu_sel + 1;
                return true;
            case K_BACK:
                td_menu_open = false;
                return false;
            case K_QUIT:
                td_menu_open = false;
                s_exit_request = true;
                return false;
            default:
                break;
            }
        }
    }
}

/* ---- 暂停 ---- */
static void td_pause(void) {
    pause_sel_t sel = PAUSE_RESUME;
    if (ui_pause_run(&sel)) {
        if (sel == PAUSE_RESTART) towerdefense_enter();
    } else {
        s_exit_request = true;
    }
}

/* ---- 绘制 ---- */
static char *td_num(char *p, int v) {
    char rev[8];
    int r = 0;
    if (v == 0) rev[r++] = '0';
    while (v > 0 && r < 7) {
        rev[r++] = (char)('0' + v % 10);
        v /= 10;
    }
    while (r > 0) *p++ = rev[--r];
    return p;
}

static void td_hud_right(void) {
    char buf[24];
    char *p = buf;
    const char *lab = "GOLD ";
    while (*lab) *p++ = *lab++;
    p = td_num(p, td_gold);
    lab = " WAVE ";
    while (*lab) *p++ = *lab++;
    p = td_num(p, td_wave);
    *p = 0;
    fb_text((int)CCG_W - 2 - text_width(buf), 0, buf, true);
}

static void td_draw_tower_glyph(int bx, int by, int type) {
    int cx = bx + TD_CELL / 2;
    int cy = by + TD_CELL / 2;
    if (type == 1) {                        /* 1: 十字 */
        fb_hline(cx - 1, cy, 3, true);
        fb_vline(cx, cy - 1, 3, true);
    } else if (type == 2) {                 /* 2: 对角 X */
        for (int k = -1; k <= 1; k++) {
            fb_pixel(cx + k, cy + k, true);
            fb_pixel(cx + k, cy - k, true);
        }
    } else {                                /* 3: 实心菱形 */
        for (int dy = -4; dy <= 4; dy++)
            for (int dx = -4; dx <= 4; dx++) {
                int ax = dx < 0 ? -dx : dx;
                int ay = dy < 0 ? -dy : dy;
                if (ax + ay <= 4) fb_pixel(cx + dx, cy + dy, true);
            }
    }
}

void towerdefense_render(void) {
    fb_clear(false);                        /* 全帧重绘 */
    fb_fill_rect(0, 0, CCG_W, CCG_HUD_H - 1, false);   /* HUD 白底 */
    if (td_over) {
        fb_text(0, 0, td_win ? "VICTORY!" : "DEFEATED!", true);
        fb_text((int)CCG_W - 2 - text_width("OK/N:RETRY BACK:QUIT"), 2,
                "OK/N:RETRY BACK:QUIT", true);
        if (!td_over_full) {
            td_over_full = true;
            disp_force_full();
        }
    } else {
        fb_text(0, 0, "TOWER DEFENSE", true);
        td_hud_right();
    }
    fb_hline(0, CCG_HUD_H - 1, CCG_W, true);
    /* 棋盘网格 */
    for (int cy = 0; cy < TD_ROWS; cy++)
        for (int cx = 0; cx < TD_COLS; cx++)
            fb_stroke_rect(TD_OX + cx * TD_CELL, TD_OY + cy * TD_CELL,
                           TD_CELL, TD_CELL, true);
    /* 路径黑格 */
    for (int s = 0; s < TD_PATH_LEN; s++) {
        int cx = td_px(s);
        int cy = td_py(s);
        fb_fill_rect(TD_OX + cx * TD_CELL, TD_OY + cy * TD_CELL,
                     TD_CELL, TD_CELL, true);
    }
    /* 基地(路径终点): 白环 + B */
    {
        int bx = TD_OX + 11 * TD_CELL;
        int by = TD_OY + 6 * TD_CELL;
        fb_stroke_rect(bx, by, TD_CELL, TD_CELL, false);
        fb_text(bx + 7, by + 6, "B", false);
    }
    /* 炮塔 */
    for (int t = 0; t < td_tn; t++) {
        int bx = TD_OX + (int)td_tx[t] * TD_CELL;
        int by = TD_OY + (int)td_ty[t] * TD_CELL;
        fb_fill_rect(bx, by, TD_CELL, TD_CELL, false);
        fb_stroke_rect(bx, by, TD_CELL, TD_CELL, true);
        td_draw_tower_glyph(bx, by, td_tt[t]);
    }
    /* 敌人: 白色圆盘(9x9 手绘圆) */
    for (int i = 0; i < TD_MAX_EN; i++) {
        int bx, by;
        if (td_epos[i] < 0) continue;
        bx = TD_OX + td_px(td_epos[i]) * TD_CELL + TD_CELL / 2;
        by = TD_OY + td_py(td_epos[i]) * TD_CELL + TD_CELL / 2;
        for (int dy = -4; dy <= 4; dy++)
            for (int dx = -4; dx <= 4; dx++)
                if (dx * dx + dy * dy <= 16)
                    fb_pixel(bx + dx, by + dy, false);
    }
    /* 光标: 最后画, 反色边框(黑格白边 / 白格黑边), 菜单期隐藏 */
    if (!td_menu_open) {
        bool black = !td_on_path(td_cx, td_cy);
        fb_stroke_rect_thick(TD_OX + td_cx * TD_CELL, TD_OY + td_cy * TD_CELL,
                             TD_CELL, TD_CELL, 2, black);
    }
}

void towerdefense_enter(void) {
    td_gold = TD_START_GOLD;
    td_wave = 1;
    td_spawned = 0;
    td_stick = 0;
    td_break = 0;
    td_tn = 0;
    td_over = false;
    td_win = false;
    td_over_full = false;
    td_menu_open = false;
    for (int i = 0; i < TD_MAX_EN; i++) {
        td_epos[i] = -1;
        td_ehp[i] = 0;
    }
    td_cx = 5;
    td_cy = 4;
    towerdefense_render();
    disp_full();
}

void towerdefense_exit(void) {}

void towerdefense_tick(uint64_t now) {
    (void)now;
    td_tick_turn();
}

void towerdefense_on_key(const key_event_t *ev) {
    if (td_over) {
        if (ev->is_repeat) return;
        if (ev->key == K_OK || (ev->key == K_CHAR && ev->ch == 'n'))
            towerdefense_enter();
        else if (ev->key == K_BACK || ev->key == K_QUIT)
            s_exit_request = true;
        return;
    }
    switch (ev->key) {
    case K_UP: td_move(0, -1); break;
    case K_DOWN: td_move(0, 1); break;
    case K_LEFT: td_move(-1, 0); break;
    case K_RIGHT: td_move(1, 0); break;
    case K_OK:
        if (!ev->is_repeat && td_cell_buildable(td_cx, td_cy)) {
            int type = 0;
            if (td_menu_run(&type)) td_place_tower(td_cx, td_cy, type);
        }
        break;
    case K_BACK:
    case K_PAUSE:
        if (!ev->is_repeat) td_pause();
        break;
    case K_CHAR:
        if (ev->ch == 'w') td_move(0, -1);
        else if (ev->ch == 'a') td_move(-1, 0);
        else if (ev->ch == 's') td_move(0, 1);
        else if (ev->ch == 'd') td_move(1, 0);
        else if (ev->ch == 'n' && !ev->is_repeat) towerdefense_enter();
        else if (ev->ch == 'p' && !ev->is_repeat) td_pause();
        break;
    case K_QUIT:
        if (!ev->is_repeat) s_exit_request = true;
        break;
    default:
        break;
    }
}
