/* BACKGAMMON — 简化西洋双陆棋(单向竞速版)
 * 24 格单跑道(顶底两行蛇形, 每格 12x20); 双方各 6 子从两端出发:
 * 玩家从左端(格 0)向右, AI 从右端(格 23)向左; 掷 2 骰按点数前进,
 * 全部 6 子越过终点的一边获胜。
 * 落子规则: 目标格内对方子 >= 2 时不能落子(可跳过); 对方单子可同格共处;
 * 若某骰对选中子无合法落点则该骰强制跳跃(保证对局必然推进, 永不僵局)。
 * 操作: OK 掷骰; < >/AD 选子(自动吸附到有子的格); OK 移动(自动用最大可用点数);
 * N 新局; BACK/P 暂停菜单; Q 退出。AI 自动: 每 tick 一步(700ms),
 * 贪心 + 1 层前瞻启发式(优先越线、按进度、避免跳罚)。
 * 建议 games_table 条目: .tick_interval_ms = 700 */
#include "game.h"
#include "../config.h"
#include "../gfx/canvas.h"
#include "../gfx/font.h"
#include "../platform/audio.h"
#include "../platform/display.h"
#include "../platform/input.h"
#include "../platform/led.h"
#include "../platform/time.h"
#include "../rng.h"
#include <stddef.h>

#define BG_N       24            /* 跑道格数 */
#define BG_PER     6             /* 每方棋子数 */
#define BG_LAST    23
#define BG_CELL_W  12            /* 格宽 */
#define BG_CELL_H  20            /* 格高 */
#define BG_OX      4             /* 跑道左边缘 ((296-288)/2) */
#define BG_ROW0_Y  18            /* 顶行(格 0-11, 左→右) */
#define BG_ROW1_Y  42            /* 底行(格 12-23, 右→左 蛇形) */
#define BG_INFO_Y  62            /* 信息区分隔线 */
#define BG_DIE     28            /* 骰子框边长 */
#define BG_DIE_X1  120
#define BG_DIE_X2  150
#define BG_DIE_Y   68
#define BG_STAT_Y  116           /* 状态行 */

typedef enum {
    BG_PH_ROLL = 0,   /* 玩家回合: 等 OK 掷骰 */
    BG_PH_SELECT,     /* 玩家选子移动 */
    BG_PH_AI,         /* AI 自动(每 tick 一步) */
    BG_PH_OVER
} bg_phase_t;

static uint8_t bg_p[BG_N];       /* 每格玩家子数 */
static uint8_t bg_a[BG_N];       /* 每格 AI 子数 */
static int bg_pfin, bg_afin;     /* 各自越过终点的子数 */
static int bg_rem[2];            /* 剩余骰面(0=已用) */
static int bg_dice[2];           /* 本回合骰面(显示用) */
static int bg_cur;               /* 光标格 */
static bg_phase_t bg_phase;
static int bg_ai_stage;          /* 0=待掷骰 1=移动中 */
static bool bg_over, bg_over_full;
static int bg_winner;            /* 0=无 1=玩家 2=AI */
static rng_t bg_rng;

void backgammon_render(void);    /* enter 在前, 先声明 */

/* 格 cell 的屏幕坐标(蛇形: 底行 12-23 自右向左) */
static void bg_cell_xy(int cell, int *x, int *y) {
    if (cell < 12) {
        *y = BG_ROW0_Y;
        *x = BG_OX + cell * BG_CELL_W;
    } else {
        *y = BG_ROW1_Y;
        *x = BG_OX + (BG_LAST - cell) * BG_CELL_W;
    }
}

/* cell 处子前进 d 是否合法: 越过终点恒合法;
 * 否则目标格内对方子数 < 2(对方单子可同格, 2+ 成墙阻挡) */
static bool bg_legal(int cell, int d, bool is_player) {
    int to = is_player ? cell + d : cell - d;
    if (is_player) {
        if (to > BG_LAST) return true;
        return bg_a[to] < 2;
    }
    if (to < 0) return true;
    return bg_p[to] < 2;
}

/* 掷 2 骰(1..6) */
static void bg_roll(void) {
    bg_dice[0] = 1 + (int)rng_range(&bg_rng, 6);
    bg_dice[1] = 1 + (int)rng_range(&bg_rng, 6);
    bg_rem[0] = bg_dice[0];
    bg_rem[1] = bg_dice[1];
}

/* 玩家选中格的用骰: 取最大可用点数; 最大不合法时退次大;
 * 两个都不合法则用最大骰强制跳跃(绝不卡死) */
static int bg_piece_die(int cell) {
    int hi = bg_rem[0] > bg_rem[1] ? bg_rem[0] : bg_rem[1];
    int lo = bg_rem[0] > bg_rem[1] ? bg_rem[1] : bg_rem[0];
    if (bg_legal(cell, hi, true)) return hi;
    if (lo > 0 && bg_legal(cell, lo, true)) return lo;   /* lo==0 时不可取 */
    return hi;                                            /* 强制跳: 恒 >= 1 */
}

/* 光标向 dir(+1/-1) 吸附到下一个有子格(环绕; 循环 24 次保证终止) */
static void bg_cursor_step(int dir) {
    int c = bg_cur;
    for (int k = 0; k < BG_N; k++) {
        c = (c + dir + BG_N) % BG_N;
        if (bg_p[c] > 0) { bg_cur = c; return; }
    }
}

static void bg_check_win(void) {
    if (bg_pfin >= BG_PER) {
        bg_winner = 1;
        bg_over = true;
        bg_phase = BG_PH_OVER;
        audio_win();
        led_fx_set(LED_FX_WIN);
    } else if (bg_afin >= BG_PER) {
        bg_winner = 2;
        bg_over = true;
        bg_phase = BG_PH_OVER;
        audio_lose();
        led_fx_set(LED_FX_LOSE);
    }
}

static void bg_start_ai(void) {
    bg_phase = BG_PH_AI;
    bg_ai_stage = 0;              /* 下一 tick 掷骰 */
}

static void bg_start_player(void) {
    bg_phase = BG_PH_ROLL;
    bg_cur = 0;
}

/* ---- 玩家回合 ---- */
static void bg_player_roll(void) {
    bg_roll();
    bg_phase = BG_PH_SELECT;
    bg_cur = 0;
    bg_cursor_step(1);            /* 吸附到最近的子 */
    audio_move();
}

static void bg_player_move(void) {
    int d = bg_piece_die(bg_cur);
    if (d <= 0) return;           /* 防御: 正常情况下总有骰可用 */
    int i = (bg_rem[0] == d) ? 0 : 1;
    bg_rem[i] = 0;
    int to = bg_cur + d;
    bg_p[bg_cur]--;
    if (to > BG_LAST) bg_pfin++;
    else bg_p[to]++;
    bg_check_win();
    if (bg_over) return;
    audio_select();               /* 落子 */
    if (bg_rem[0] == 0 && bg_rem[1] == 0) { bg_start_ai(); return; }
    bg_cursor_step(1);            /* 吸附到下一个有子格 */
}

/* ---- AI: 简单启发式 ---- */
/* AI 子 cell 用骰 d 的单步得分: 越过终点 1000+进度; 否则按新进度;
 * 非法落点(强跳)罚 12 分, 让 AI 尽量走合法格 */
static int bg_ai_step_score(int cell, int d) {
    int to = cell - d;
    int s = (to < 0) ? (1000 + (BG_LAST - cell)) : (BG_LAST - to);
    if (!bg_legal(cell, d, false)) s -= 12;
    return s;
}

/* 在当前棋盘上对剩余骰 r0/r1 求最佳单步(每子每骰都评估) */
static int bg_ai_best_single(int r0, int r1, int *cell_out, int *die_out) {
    int best = -1, bcell = -1, bdie = 0;
    for (int c = 0; c < BG_N; c++) {
        if (!bg_a[c]) continue;
        if (r0 > 0) {
            int s = bg_ai_step_score(c, r0);
            if (s > best) { best = s; bcell = c; bdie = r0; }
        }
        if (r1 > 0) {
            int s = bg_ai_step_score(c, r1);
            if (s > best) { best = s; bcell = c; bdie = r1; }
        }
    }
    *cell_out = bcell;
    *die_out = bdie;
    return best;
}

/* AI 走一步: 每子 × 每骰 组合 = 本步得分 + 模拟后另一骰最佳续走, 取总分最高 */
static void bg_ai_move_best(void) {
    int best_total = -1, best_c = -1, best_d = 0;
    int r0 = bg_rem[0], r1 = bg_rem[1];
    for (int c = 0; c < BG_N; c++) {
        if (!bg_a[c]) continue;
        for (int k = 0; k < 2; k++) {
            int d = (k == 0) ? r0 : r1;
            if (d <= 0) continue;
            int step = bg_ai_step_score(c, d);
            int to = c - d;
            bg_a[c]--;                                    /* 模拟 */
            if (to < 0) bg_afin++; else bg_a[to]++;
            int or0 = (k == 0) ? 0 : r0;      /* 用了 r0 则续走骰只剩 r1 */
            int or1 = (k == 0) ? r1 : 0;
            int fc, fd;
            int follow = (or0 > 0 || or1 > 0) ?
                         bg_ai_best_single(or0, or1, &fc, &fd) : 0;
            if (to < 0) bg_afin--; else bg_a[to]--;       /* 撤销 */
            bg_a[c]++;
            int total = step + follow;
            if (total > best_total) { best_total = total; best_c = c; best_d = d; }
        }
    }
    if (best_c < 0) return;                               /* 防御: 无子可动 */
    int i = (bg_rem[0] == best_d) ? 0 : 1;
    bg_rem[i] = 0;
    int to = best_c - best_d;
    bg_a[best_c]--;
    if (to < 0) bg_afin++;
    else bg_a[to]++;
}

/* ---- 生命周期 ---- */
void backgammon_enter(void) {
    for (int i = 0; i < BG_N; i++) { bg_p[i] = 0; bg_a[i] = 0; }
    bg_p[0] = BG_PER;             /* 玩家 6 子起点(左端) */
    bg_a[BG_LAST] = BG_PER;       /* AI 6 子起点(右端) */
    bg_pfin = 0;
    bg_afin = 0;
    bg_rem[0] = bg_rem[1] = 0;
    bg_dice[0] = bg_dice[1] = 0;
    bg_cur = 0;
    bg_phase = BG_PH_ROLL;
    bg_ai_stage = 0;
    bg_over = false;
    bg_over_full = false;
    bg_winner = 0;
    rng_seed(&bg_rng, now_ms() ^ 0x6B6u);
    backgammon_render();
    disp_full();
}

void backgammon_exit(void) {}

void backgammon_tick(uint64_t now) {
    (void)now;
    if (bg_over || bg_phase != BG_PH_AI) return;
    if (bg_ai_stage == 0) {
        bg_roll();
        bg_ai_stage = 1;
        return;
    }
    if (bg_rem[0] == 0 && bg_rem[1] == 0) { bg_start_player(); return; }
    bg_ai_move_best();
    bg_check_win();
    if (bg_over) return;
    if (bg_rem[0] == 0 && bg_rem[1] == 0) bg_start_player();
}

/* ---- 绘制 ---- */
/* 棋子: count>=3 画 2x 数字(12px 格内放不下更多圆), 否则画圆
 * (玩家实心 / AI 空心); black 决定墨色(光标反色格用 false) */
static void bg_draw_stone(int cx, int cy, bool player, bool black, int count) {
    if (count >= 3) {
        char b[2] = { (char)('0' + count), 0 };
        fb_text_scale2(cx - 5, cy - 7, b, black);
        return;
    }
    int n = (count == 2) ? 2 : 1;
    int r = 2;
    for (int i = 0; i < n; i++) {
        int px = cx + ((n == 2) ? (i == 0 ? -3 : 3) : 0);
        for (int dy = -r; dy <= r; dy++)
            for (int dx = -r; dx <= r; dx++) {
                int rr = dx * dx + dy * dy;
                if (rr > r * r) continue;
                if (player) fb_pixel(px + dx, cy + dy, black);
                else if (rr >= 1) fb_pixel(px + dx, cy + dy, black);
            }
    }
}

/* 骰子框: 未掷=空框; 已用=框+对角叉; 可用=黑框白字 */
static void bg_draw_die(int x, int y, int val, bool used) {
    if (val <= 0) {
        fb_stroke_rect(x, y, BG_DIE, BG_DIE, true);
        return;
    }
    if (used) {
        fb_stroke_rect(x, y, BG_DIE, BG_DIE, true);
        for (int i = 3; i < BG_DIE - 3; i++) {
            fb_pixel(x + i, y + i, true);
            fb_pixel(x + i, y + BG_DIE - 1 - i, true);
        }
    } else {
        fb_fill_rect(x, y, BG_DIE, BG_DIE, true);
        char b[2] = { (char)('0' + val), 0 };
        fb_text_scale2(x + (BG_DIE - 10) / 2, y + (BG_DIE - 14) / 2, b, false);
    }
}

void backgammon_render(void) {
    fb_clear(false);

    /* HUD 顶栏: 黑字白底 */
    if (bg_over) {
        fb_fill_rect(0, 0, CCG_W, CCG_HUD_H - 1, false);
        fb_text(2, 2, bg_winner == 1 ? "YOU WIN!" : "AI WINS", true);
        fb_text(CCG_W - 2 - text_width("OK/N:RETRY BACK:QUIT"), 2,
                "OK/N:RETRY BACK:QUIT", true);
        if (!bg_over_full) { bg_over_full = true; disp_force_full(); }
    } else {
        fb_text(2, 2, "BACKGAMMON", true);
        {
            char buf[8];          /* "P:0 A:0" */
            buf[0] = 'P'; buf[1] = ':';
            buf[2] = (char)('0' + bg_pfin);
            buf[3] = ' '; buf[4] = 'A'; buf[5] = ':';
            buf[6] = (char)('0' + bg_afin);
            buf[7] = 0;
            fb_text(CCG_W - 2 - text_width(buf), 2, buf, true);
        }
    }
    fb_hline(0, CCG_HUD_H - 1, CCG_W, true);

    /* 棋盘: 24 格 + 棋子 */
    for (int cell = 0; cell < BG_N; cell++) {
        int x, y;
        bg_cell_xy(cell, &x, &y);
        fb_stroke_rect(x, y, BG_CELL_W, BG_CELL_H, true);
        int cx = x + BG_CELL_W / 2;
        int cy = y + BG_CELL_H / 2;
        if (bg_p[cell]) bg_draw_stone(cx, cy, true, true, bg_p[cell]);
        if (bg_a[cell]) bg_draw_stone(cx, cy, false, true, bg_a[cell]);
    }
    /* 光标: 反色格(必须在所有格子画完之后) */
    if (bg_phase == BG_PH_SELECT) {
        int x, y;
        bg_cell_xy(bg_cur, &x, &y);
        fb_fill_rect(x, y, BG_CELL_W, BG_CELL_H, true);
        if (bg_p[bg_cur]) bg_draw_stone(x + 6, y + 10, true, false, bg_p[bg_cur]);
    }

    /* 信息区: 回合指示 + 完成数 + 骰子 + 状态行 */
    fb_hline(0, BG_INFO_Y, CCG_W, true);
    bool pturn = (bg_winner == 1) ||
                 (!bg_over && (bg_phase == BG_PH_ROLL || bg_phase == BG_PH_SELECT));
    bool aturn = (bg_winner == 2) || (!bg_over && bg_phase == BG_PH_AI);
    {
        int w = text_width("YOU");
        if (pturn) fb_fill_rect(4, 68, w + 8, 15, true);
        fb_text(8, 70, "YOU", pturn ? false : true);
        fb_text(8, 86, "FIN", true);
        char b[2] = { (char)('0' + (bg_pfin > 9 ? 9 : bg_pfin)), 0 };
        fb_fill_rect(4, 92, 18, 16, true);
        fb_text_scale2(8, 93, b, false);
    }
    {
        int w = text_width("AI");
        int bx = CCG_W - 4 - w - 8;
        if (aturn) fb_fill_rect(bx, 68, w + 8, 15, true);
        fb_text(bx + 4, 70, "AI", aturn ? false : true);
        fb_text(bx + 4, 86, "FIN", true);
        char b[2] = { (char)('0' + (bg_afin > 9 ? 9 : bg_afin)), 0 };
        fb_fill_rect(CCG_W - 22, 92, 18, 16, true);
        fb_text_scale2(CCG_W - 18, 93, b, false);
    }
    bg_draw_die(BG_DIE_X1, BG_DIE_Y, bg_dice[0], bg_dice[0] > 0 && bg_rem[0] == 0);
    bg_draw_die(BG_DIE_X2, BG_DIE_Y, bg_dice[1], bg_dice[1] > 0 && bg_rem[1] == 0);

    const char *st;
    if (bg_over) st = "GAME OVER";
    else if (bg_phase == BG_PH_ROLL) st = "OK: ROLL DICE";
    else if (bg_phase == BG_PH_SELECT) st = "OK: MOVE PIECE";
    else st = (bg_ai_stage == 0) ? "AI: ROLLING..." : "AI: MOVING...";
    fb_text_center(BG_STAT_Y, st, true);
}

/* ---- 输入 ---- */
void backgammon_on_key(const key_event_t *ev) {
    if (ev->is_repeat) return;
    if (bg_over) {
        if (ev->key == K_OK || (ev->key == K_CHAR && ev->ch == 'n'))
            backgammon_enter();
        else if (ev->key == K_BACK || ev->key == K_QUIT)
            s_exit_request = true;
        return;
    }
    switch (ev->key) {
    case K_LEFT:
        if (bg_phase == BG_PH_SELECT) bg_cursor_step(-1);
        break;
    case K_RIGHT:
        if (bg_phase == BG_PH_SELECT) bg_cursor_step(1);
        break;
    case K_CHAR:
        if (ev->ch == 'a' && bg_phase == BG_PH_SELECT) bg_cursor_step(-1);
        else if (ev->ch == 'd' && bg_phase == BG_PH_SELECT) bg_cursor_step(1);
        else if (ev->ch == 'n') backgammon_enter();
        break;
    case K_OK:
        if (bg_phase == BG_PH_ROLL) bg_player_roll();
        else if (bg_phase == BG_PH_SELECT) bg_player_move();
        break;
    case K_PAUSE:
    case K_BACK: {
        pause_sel_t sel;
        if (ui_pause_run(&sel)) {
            if (sel == PAUSE_RESTART) backgammon_enter();
        } else {
            s_exit_request = true;
        }
        break;
    }
    case K_QUIT:
        s_exit_request = true;
        break;
    default:
        break;
    }
}
