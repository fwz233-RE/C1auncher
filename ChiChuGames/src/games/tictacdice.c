/* TIC-TAC-DICE — 井字骰子: 掷骰决定可落子的行(1-3)
 * 玩家 X(先手) vs AI O; 玩家必须在该行选空列落子, 该行无空位则回合跳过(双方同规则)
 * 横/竖/斜三连 → 胜; 满盘 → 平; W/L/D 战绩跨回合累计, 再进游戏清零
 * 操作: OK 掷骰/落子, LEFT/RIGHT(A/D) 选列, N 新局, BACK 暂停, Q 退出 */
#include "game.h"
#include "../config.h"
#include "../gfx/canvas.h"
#include "../gfx/font.h"
#include "../platform/display.h"
#include "../platform/input.h"
#include "../platform/time.h"
#include "../rng.h"
#include "../platform/audio.h"
#include "../platform/led.h"
#include <stdio.h>

#define TD_CELL 44                          /* 棋盘格边长(正方形优先) */
#define TD_OX   ((CCG_W - 3 * TD_CELL) / 2) /* 82: 棋盘居中 */
#define TD_OY   (CCG_HUD_H + 2)             /* 18: 顶栏下 */
#define TD_DIE_X 16                         /* 骰子(左栏) */
#define TD_DIE_Y 22
#define TD_DIE   44
#define TD_LEG_X 224                        /* 阵营图例(右栏) */
#define TD_LEG_Y 26

typedef enum {
    TD_PH_ROLL = 0,     /* 玩家掷骰 */
    TD_PH_SELECT,       /* 玩家选列落子 */
    TD_PH_AI_ROLL,      /* AI 掷骰(自动, tick 驱动) */
    TD_PH_AI_PLACE,     /* AI 落子(自动) */
    TD_PH_OVER
} td_phase_t;

static rng_t td_rng;
static uint8_t td_board[3][3];  /* 0 空 1=X(玩家) 2=O(AI) */
static td_phase_t td_phase;
static int td_row;              /* 骰子指定行 0..2 */
static int td_col;              /* 玩家光标列 0..2 */
static int td_die;              /* 骰面 1..3, 0=本局未掷 */
static int td_winner;           /* 0 无 1 玩家 2 AI 3 平 */
static bool td_over_full;       /* 结算全刷防重复 */
static int td_pwins, td_awins, td_draws;    /* 战绩 */

void tictacdice_render(void);

/* ---- 胜负判定 ---- */
static int td_check_win(uint8_t b[3][3]) {
    for (int i = 0; i < 3; i++) {
        if (b[i][0] && b[i][0] == b[i][1] && b[i][1] == b[i][2]) return b[i][0];
        if (b[0][i] && b[0][i] == b[1][i] && b[1][i] == b[2][i]) return b[0][i];
    }
    if (b[0][0] && b[0][0] == b[1][1] && b[1][1] == b[2][2]) return b[0][0];
    if (b[0][2] && b[0][2] == b[1][1] && b[1][1] == b[2][0]) return b[0][2];
    return 0;
}

static int td_row_full(int r) {
    for (int c = 0; c < 3; c++)
        if (!td_board[r][c]) return 0;
    return 1;
}

/* 终局判定: 胜 → 平; 记战绩(仅首次进入 OVER) */
static void td_check_end(void) {
    if (td_phase == TD_PH_OVER) return;
    int w = td_check_win(td_board);
    if (w) {
        td_winner = w;
        td_phase = TD_PH_OVER;
        if (w == 1) td_pwins++; else td_awins++;
        return;
    }
    int full = 1;
    for (int y = 0; y < 3; y++)
        for (int x = 0; x < 3; x++)
            if (!td_board[y][x]) full = 0;
    if (full) {
        td_winner = 3;
        td_phase = TD_PH_OVER;
        td_draws++;
    }
}

/* AI 选列(限 td_row 行内空位): 胜 > 堵玩家 > 中心 > 最左; 行满返回 -1 */
static int td_ai_pick(void) {
    int cand[3], n = 0;
    for (int c = 0; c < 3; c++)
        if (!td_board[td_row][c]) cand[n++] = c;
    if (!n) return -1;
    for (int i = 0; i < n; i++) {
        td_board[td_row][cand[i]] = 2;
        int w = td_check_win(td_board);
        td_board[td_row][cand[i]] = 0;
        if (w == 2) return cand[i];
    }
    for (int i = 0; i < n; i++) {
        td_board[td_row][cand[i]] = 1;
        int w = td_check_win(td_board);
        td_board[td_row][cand[i]] = 0;
        if (w == 1) return cand[i];
    }
    for (int i = 0; i < n; i++)
        if (cand[i] == 1) return 1;
    return cand[0];
}

static void td_new_round(void) {
    for (int y = 0; y < 3; y++)
        for (int x = 0; x < 3; x++) td_board[y][x] = 0;
    td_phase = TD_PH_ROLL;
    td_row = 0;
    td_col = 1;
    td_die = 0;
    td_winner = 0;
    td_over_full = false;
}

void tictacdice_enter(void) {
    rng_seed(&td_rng, now_ms() ^ 0x9E3779B97F4A7C15ULL);
    td_pwins = td_awins = td_draws = 0;
    td_new_round();
    tictacdice_render();
    disp_full();
}

/* ---- 绘制 ---- */

/* pip 位置 3x3 格(0..2,0..2), x<0 结束 */
static const int8_t td_pips[4][3][2] = {
    { {0, 0}, {-1, -1}, {-1, -1} },  /* 0: 未掷 */
    { {1, 1}, {-1, -1}, {-1, -1} },
    { {0, 0}, {2, 2}, {-1, -1} },
    { {0, 0}, {1, 1}, {2, 2} }
};

static void td_draw_die(void) {
    fb_stroke_rect(TD_DIE_X, TD_DIE_Y, TD_DIE, TD_DIE, true);
    if (td_die < 1 || td_die > 3) return;
    for (int k = 0; k < 3; k++) {
        int px = td_pips[td_die][k][0];
        int py = td_pips[td_die][k][1];
        if (px < 0) break;
        fb_fill_rect(TD_DIE_X + 8 + px * 12, TD_DIE_Y + 8 + py * 12, 8, 8, true);
    }
}

/* X/O 符号; inv=反白(黑底白符) */
static void td_draw_mark(int x, int y, int v, bool inv) {
    int w = TD_CELL - 10, h = TD_CELL - 10;
    x += 5;
    y += 5;
    if (v == 1) {
        for (int i = 0; i < w; i++) {
            fb_fill_rect(x + i, y + i * h / w, 2, 2, !inv);
            fb_fill_rect(x + i, y + h - 1 - i * h / w, 2, 2, !inv);
        }
    } else if (v == 2) {
        fb_fill_rect(x, y, w, h, !inv);
        fb_fill_rect(x + 4, y + 4, w - 8, h - 8, inv);
    }
}

static void td_draw_legend(void) {
    int x = TD_LEG_X, y = TD_LEG_Y;
    for (int i = 0; i < 12; i++) {          /* X 图例 */
        fb_fill_rect(x + i, y + i, 2, 2, true);
        fb_fill_rect(x + i, y + 10 - i, 2, 2, true);
    }
    fb_text(x + 16, y + 2, "YOU", true);
    y += 20;
    fb_fill_rect(x, y, 12, 12, true);       /* O 图例 */
    fb_fill_rect(x + 3, y + 3, 6, 6, false);
    fb_text(x + 16, y + 2, "AI", true);
}

void tictacdice_render(void) {
    fb_clear(false);
    fb_text(2, 0, "TIC-TAC-DICE", true);
    if (td_phase == TD_PH_OVER) {
        fb_fill_rect(0, 0, CCG_W, CCG_HUD_H - 1, false);
        const char *res = td_winner == 1 ? "YOU WIN!" :
                          td_winner == 2 ? "AI WINS" : "DRAW";
        fb_text(2, 2, res, true);
        fb_text(CCG_W - 2 - text_width("OK/N:RETRY BACK:QUIT"), 2,
                "OK/N:RETRY BACK:QUIT", true);
        if (!td_over_full) { td_over_full = true; disp_force_full(); }
    } else {
        const char *st = td_phase == TD_PH_ROLL ? "YOU:ROLL" :
                         td_phase == TD_PH_SELECT ? "YOU:PLACE" :
                         td_phase == TD_PH_AI_ROLL ? "AI ROLLS" : "AI PLACES";
        fb_text(CCG_W - 2 - text_width(st), 0, st, true);
    }
    fb_hline(0, CCG_HUD_H - 1, CCG_W, true);

    td_draw_die();
    char rec[16];
    snprintf(rec, sizeof rec, "W%d L%d D%d", td_pwins, td_awins, td_draws);
    fb_text(TD_DIE_X, TD_DIE_Y + TD_DIE + 8, rec, true);

    /* 需求行反白(黑底白边白符); 光标环在所有格子之后画 */
    bool row_req = (td_phase == TD_PH_SELECT || td_phase == TD_PH_AI_ROLL ||
                    td_phase == TD_PH_AI_PLACE);
    for (int y = 0; y < 3; y++)
        for (int x = 0; x < 3; x++) {
            int cx = TD_OX + x * TD_CELL;
            int cy = TD_OY + y * TD_CELL;
            bool req = row_req && y == td_row;
            if (req) fb_fill_rect(cx, cy, TD_CELL, TD_CELL, true);
            fb_stroke_rect(cx, cy, TD_CELL, TD_CELL, !req);
            if (td_board[y][x]) td_draw_mark(cx, cy, td_board[y][x], req);
        }
    if (td_phase == TD_PH_SELECT) {
        int cx = TD_OX + td_col * TD_CELL;
        int cy = TD_OY + td_row * TD_CELL;
        fb_stroke_rect_thick(cx - 2, cy - 2, TD_CELL + 4, TD_CELL + 4, 3, true);
    }

    td_draw_legend();
}

/* ---- 逻辑 ---- */

void tictacdice_tick(uint64_t now) {
    (void)now;
    if (td_phase == TD_PH_AI_ROLL) {
        td_row = (int)rng_range(&td_rng, 3);
        td_die = td_row + 1;
        if (td_row_full(td_row)) {          /* AI 掷到满行 → 跳过 */
            td_check_end();
            if (td_phase != TD_PH_OVER) td_phase = TD_PH_ROLL;
        } else {
            td_phase = TD_PH_AI_PLACE;
        }
    } else if (td_phase == TD_PH_AI_PLACE) {
        int c = td_ai_pick();
        if (c >= 0) td_board[td_row][c] = 2;
        td_check_end();
        if (td_phase == TD_PH_OVER) {
            if (td_winner == 2) {          /* AI 三连: 负 */
                audio_lose();
                led_fx_set(LED_FX_LOSE);
            } else {
                audio_clear();             /* AI 落子致平 */
            }
        } else {
            td_phase = TD_PH_ROLL;
        }
    }
}

void tictacdice_on_key(const key_event_t *ev) {
    bool dir = (ev->key == K_LEFT || ev->key == K_RIGHT);
    if (ev->is_repeat && !dir) return;      /* 确认/字母忽略重复, 方向可重复 */
    if (td_phase == TD_PH_OVER) {
        if (ev->key == K_OK || (ev->key == K_CHAR && ev->ch == 'n'))
            td_new_round();
        else if (ev->key == K_BACK || ev->key == K_QUIT)
            s_exit_request = true;
        return;
    }
    if (td_phase == TD_PH_AI_ROLL || td_phase == TD_PH_AI_PLACE) return;
    switch (ev->key) {
    case K_LEFT:
        if (td_phase == TD_PH_SELECT) td_col = (td_col + 2) % 3;
        break;
    case K_RIGHT:
        if (td_phase == TD_PH_SELECT) td_col = (td_col + 1) % 3;
        break;
    case K_CHAR:
        if (ev->ch == 'a' && td_phase == TD_PH_SELECT) td_col = (td_col + 2) % 3;
        else if (ev->ch == 'd' && td_phase == TD_PH_SELECT) td_col = (td_col + 1) % 3;
        else if (ev->ch == 'n') td_new_round();
        break;
    case K_OK:
        if (td_phase == TD_PH_ROLL) {
            td_row = (int)rng_range(&td_rng, 3);
            td_die = td_row + 1;
            audio_move();                                  /* 掷骰 */
            if (td_row_full(td_row)) td_phase = TD_PH_AI_ROLL;  /* 满行 → 跳过 */
            else { td_col = 1; td_phase = TD_PH_SELECT; }
        } else if (td_phase == TD_PH_SELECT) {
            if (!td_board[td_row][td_col]) {
                td_board[td_row][td_col] = 1;
                td_check_end();
                if (td_phase == TD_PH_OVER) {
                    if (td_winner == 1) {  /* 玩家三连: 胜 */
                        audio_win();
                        led_fx_set(LED_FX_WIN);
                    } else {
                        audio_clear();     /* 落子致平 */
                    }
                } else {
                    td_phase = TD_PH_AI_ROLL;
                    audio_select();        /* 落子确认 */
                }
            } else {
                audio_error();             /* 该列已占 */
            }
        }
        break;
    case K_BACK: {
        pause_sel_t sel;
        if (ui_pause_run(&sel)) {
            if (sel == PAUSE_RESTART) td_new_round();
        } else {
            s_exit_request = true;
        }
        break;
    }
    case K_QUIT: s_exit_request = true; break;
    default: break;
    }
}

void tictacdice_exit(void) {}
