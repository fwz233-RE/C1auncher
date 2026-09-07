/* MUSHROOM GARDEN — 种植节奏: 6x5 蘑菇园, 25 回合制种/收/等待
 * 每个蘑菇 4 阶段(种子/苗/成菇/可收获)各 2 回合; 第 7 回合起可收获(+2),
 * 第 12 回合末未收则腐烂(-1)。每 5 回合掷骰随机事件:
 *   雨水(30%) 所有蘑菇 +1 阶段(age+2, 封顶不触发腐烂)
 *   虫害(20%) 随机 1 个蘑菇 -1 阶段(age-2, 不低于种子)
 * 25 回合总分; 回合制输入驱动(tick_interval_ms=0), 零 malloc, 静态前缀 mg_
 *
 * 几何: 27px 方格子。设计稿 28px(168x140) 在 296x152 屏 + 16px HUD 下
 *       放不下(140 > 136), 按 dev-spec "正方形格子优先 + 最大化利用屏幕"
 *       取 6x27=162 x 5x27=135, x0=67 y0=16(顶栏下方, 最大化方形)。
 * HUD: 左 MUSH, 右 SCORE n TURN t/25; 事件回合显示 RAIN+1 / PESTS-1 /
 *       ROT -1 / +2(替换 TURN)。
 * 格子: 黑框 + 阶段图案 + 2x 倒计时数字(成熟剩 6..1 / 腐烂剩 6..1);
 *       可收获整格反白; 光标 = 反白格 + 内侧白框, 所有格子画完后最后画。
 * 键位: 方向移动光标(可重复), OK = 种/收/等待(每回合一次行动), N 新局,
 *       Q 退出, BACK/PAUSE 暂停菜单。
 * 集成提示(help[], 由 games_table 提供):
 *   "MUSHROOM GARDEN", "GROW AND HARVEST MUSHROOMS: 25 TURNS",
 *   "ARROWS: MOVE  OK: PLANT / HARVEST / WAIT",
 *   "READY +2, ROT -1 (4 STAGES x 2 TURNS)",
 *   "RAIN OR PESTS EVERY 5 TURNS", NULL
 */
#include "game.h"
#include "../config.h"
#include "../gfx/canvas.h"
#include "../gfx/font.h"
#include "../platform/display.h"
#include "../platform/input.h"
#include "../platform/audio.h"
#include "../platform/time.h"
#include "../rng.h"
#include <stdio.h>
#include <string.h>

#define MG_COLS     6
#define MG_ROWS     5
#define MG_N        30          /* 格子总数 */
#define MG_CELL     27          /* 方格边长(play 区最大方形) */
#define MG_X0       67          /* (296-162)/2 */
#define MG_Y0       CCG_HUD_H   /* 顶栏下方 */
#define MG_TURNS    25          /* 总回合 */
#define MG_READY    7           /* 阶段 4(可收获)起始 age */
#define MG_ROT      13          /* age 达到即腐烂 */

/* 生长模型: 种下 age=1(阶段1第1回合)。
 * 阶段 = (age+1)/2: age 1-2 种子, 3-4 苗, 5-6 成菇, 7-12 可收获;
 * 每回合末 age+1, 13 腐烂。成熟倒计时 = 7-age, 腐烂倒计时 = 13-age。 */

typedef enum {
    MG_MSG_NONE = 0,
    MG_MSG_RAIN,        /* 雨水: 所有蘑菇 +1 阶段 */
    MG_MSG_PEST,        /* 虫害: 随机 1 个蘑菇 -1 阶段 */
    MG_MSG_ROT,         /* 有蘑菇腐烂 -1 */
    MG_MSG_HARVEST      /* 收获 +2 */
} mg_msg_t;

typedef enum { MG_ST_PLAY, MG_ST_OVER } mg_state_t;

static int mg_age[MG_N];        /* 0 = 空; 1..12 = 生长回合数 */
static int mg_cx, mg_cy;        /* 光标(像素坐标用 int, 格子坐标同) */
static int mg_turn;             /* 当前回合 1..25 */
static int mg_score;
static mg_msg_t mg_msg;
static mg_state_t mg_state;
static bool mg_over_full;       /* 结束全刷只做一次 */
static uint32_t mg_gens;        /* 换局计数(种子混合) */
static rng_t mg_rng;

void mushgarden_render(void);

static int mg_stage(int age) {
    if (age <= 0) return 0;
    int s = (age + 1) / 2;
    return (s > 4) ? 4 : s;
}

/* 阶段图案(格子内, 内部区 x+1..x+25): 种子=点, 苗=横线, 成菇/可收获=蘑菇 */
static void mg_cell_art(int x, int y, int age, bool black) {
    int st = mg_stage(age);
    if (st == 1) {
        fb_fill_rect(x + 11, y + 9, 5, 5, black);       /* 种子: 点 */
    } else if (st == 2) {
        fb_fill_rect(x + 4, y + 12, 11, 3, black);      /* 苗: 横线 */
    } else {
        /* 蘑菇: 伞盖 4 行(圆顶收窄) + 3x5 菌柄 */
        fb_fill_rect(x + 9, y + 6, 9, 1, black);
        fb_fill_rect(x + 5, y + 7, 17, 1, black);
        fb_fill_rect(x + 3, y + 8, 21, 3, black);       /* 伞盖底 3 行 */
        fb_fill_rect(x + 12, y + 11, 3, 5, black);      /* 菌柄 */
    }
}

/* 2x 放大单字符(5x7 -> 10x14), 用于格子内倒计时 */
static void mg_digit2(int x, int y, char c, bool black) {
    const uint8_t *g = font_glyph5x7[(unsigned char)c];
    for (int j = 0; j < FONT_H; j++)
        for (int i = 0; i < FONT_W; i++)
            if (g[j] & (1u << i))
                fb_fill_rect(x + i * 2, y + j * 2, 2, 2, black);
}

/* 格子内倒计时数值: 成熟剩 7-age, 可收获后剩 13-age(均为 6..1) */
static int mg_countdown_val(int age) {
    return (age < MG_READY) ? (MG_READY - age) : (MG_ROT - age);
}

static void mg_countdown(int x, int y, int age, bool black) {
    mg_digit2(x + 16, y + 11, (char)('0' + mg_countdown_val(age)), black);
}

static int mg_cx_x(void) { return MG_X0 + mg_cx * MG_CELL; }
static int mg_cy_y(void) { return MG_Y0 + mg_cy * MG_CELL; }

/* 单个格子: 黑框 + 阶段图案 + 倒计时; 可收获整格反白 */
static void mg_draw_cell(int idx) {
    int x = MG_X0 + (idx % MG_COLS) * MG_CELL;
    int y = MG_Y0 + (idx / MG_COLS) * MG_CELL;
    fb_stroke_rect(x, y, MG_CELL, MG_CELL, true);
    int age = mg_age[idx];
    if (age == 0) return;
    if (mg_stage(age) == 4) {
        fb_fill_rect(x + 1, y + 1, MG_CELL - 2, MG_CELL - 2, true);
        mg_cell_art(x, y, age, false);
        mg_countdown(x, y, age, false);
    } else {
        mg_cell_art(x, y, age, true);
        mg_countdown(x, y, age, true);
    }
}

/* 光标: 反白格 + 内侧 1px 白框; 在所有格子之后画 */
static void mg_draw_cursor(void) {
    int x = mg_cx_x(), y = mg_cy_y();
    int idx = mg_cy * MG_COLS + mg_cx;
    if (mg_age[idx] && mg_stage(mg_age[idx]) == 4) {
        /* 可收获格已反白: 只加白框 */
        fb_stroke_rect(x + 1, y + 1, MG_CELL - 2, MG_CELL - 2, false);
    } else {
        fb_fill_rect(x + 1, y + 1, MG_CELL - 2, MG_CELL - 2, true);
        if (mg_age[idx]) {
            mg_cell_art(x, y, mg_age[idx], false);
            mg_countdown(x, y, mg_age[idx], false);
        }
        fb_stroke_rect(x + 1, y + 1, MG_CELL - 2, MG_CELL - 2, false);
    }
}

/* 3x 放大文本(5x7 -> 15x21), 用于结束结算大字 */
static void mg_text3(int x, int y, const char *s, bool black) {
    for (const char *p = s; *p; p++) {
        const uint8_t *g = font_glyph5x7[(unsigned char)*p];
        for (int j = 0; j < FONT_H; j++)
            for (int i = 0; i < FONT_W; i++)
                if (g[j] & (1u << i))
                    fb_fill_rect(x + i * 3, y + j * 3, 3, 3, black);
        x += FONT_ADV * 3;
    }
}

static void mg_text3_center(int y, const char *s, bool black) {
    int w = ((int)strlen(s) * FONT_ADV - 1) * 3;
    int x = (CCG_W - w) / 2;
    if (x < 0) x = 0;
    mg_text3(x, y, s, black);
}

/* ---- 随机事件(每 5 回合末掷骰) ---- */
static void mg_event(void) {
    uint32_t roll = rng_range(&mg_rng, 10);
    if (roll <= 2) {
        /* 雨水: 所有蘑菇 +1 阶段(age+2, 封顶 12 不直接腐烂) */
        for (int i = 0; i < MG_N; i++) {
            if (mg_age[i] == 0) continue;
            mg_age[i] += 2;
            if (mg_age[i] > MG_ROT - 1) mg_age[i] = MG_ROT - 1;
        }
        mg_msg = MG_MSG_RAIN;
    } else if (roll == 3 || roll == 4) {
        /* 虫害: 随机 1 个蘑菇 -1 阶段(age-2, 不低于种子) */
        int n = 0;
        for (int i = 0; i < MG_N; i++)
            if (mg_age[i] != 0) n++;
        if (n > 0) {
            uint32_t k = rng_range(&mg_rng, (uint32_t)n);
            for (int i = 0; i < MG_N; i++) {
                if (mg_age[i] == 0) continue;
                if (k == 0) {
                    mg_age[i] -= 2;
                    if (mg_age[i] < 1) mg_age[i] = 1;
                    break;
                }
                k--;
            }
            mg_msg = MG_MSG_PEST;
        }
    }
}

/* ---- 回合末: 生长/腐烂, 事件, 回合推进 ---- */
static void mg_end_turn(void) {
    bool rot_any = false;
    for (int i = 0; i < MG_N; i++) {
        if (mg_age[i] == 0) continue;
        mg_age[i]++;
        if (mg_age[i] >= MG_ROT) {
            mg_age[i] = 0;
            mg_score--;
            rot_any = true;
        }
    }
    if (mg_turn % 5 == 0) {
        mg_event();                     /* 事件消息优先 */
    } else if (rot_any && mg_msg == MG_MSG_NONE) {
        mg_msg = MG_MSG_ROT;
    }
    mg_turn++;
    if (mg_turn > MG_TURNS) {
        mg_state = MG_ST_OVER;
        mg_over_full = false;
    }
}

/* ---- 回合行动: 空格种, 可收获收, 生长中等待; 每次行动推进 1 回合 ---- */
static void mg_act(void) {
    int idx = mg_cy * MG_COLS + mg_cx;
    if (mg_age[idx] == 0) {
        mg_age[idx] = 1;                /* 种植 */
        audio_select();
    } else if (mg_stage(mg_age[idx]) == 4) {
        mg_score += 2;                  /* 收获 */
        mg_age[idx] = 0;
        mg_msg = MG_MSG_HARVEST;
        audio_clear();
    }
    /* 生长中: 等待(无额外效果), 回合照常推进 */
    mg_end_turn();
}

static void mg_new_game(void) {
    mg_gens++;
    rng_seed(&mg_rng, now_ms() ^ ((uint64_t)mg_gens * 0x9E3779B1u));
    memset(mg_age, 0, sizeof(mg_age));
    mg_cx = 0;
    mg_cy = 0;
    mg_turn = 1;
    mg_score = 0;
    mg_msg = MG_MSG_NONE;
    mg_state = MG_ST_PLAY;
    mg_over_full = false;
    mushgarden_render();
    disp_full();
}

void mushgarden_enter(void) { mg_new_game(); }
void mushgarden_exit(void) {}
void mushgarden_tick(uint64_t now) { (void)now; }

void mushgarden_render(void) {
    fb_clear(false);
    char buf[32];

    if (mg_state == MG_ST_OVER) {
        /* HUD 两行: 左上结果 + 右上按键提示; 墙内只放最终成绩 */
        fb_fill_rect(0, 0, CCG_W, CCG_HUD_H - 1, false);
        snprintf(buf, sizeof(buf), "SCORE %d", mg_score);
        fb_text(2, 2, buf, true);
        fb_text(2, 9, "GARDEN DONE", true);
        fb_text(CCG_W - 2 - text_width("OK/N:RETRY BACK:QUIT"), 2,
                "OK/N:RETRY BACK:QUIT", true);
        mg_text3_center(48, "FINAL SCORE", true);
        snprintf(buf, sizeof(buf), "%d", mg_score);
        mg_text3_center(82, buf, true);
        if (!mg_over_full) { mg_over_full = true; disp_force_full(); }
        return;
    }

    /* HUD 顶栏: 左 MUSH, 右 SCORE + 回合/事件 */
    fb_text(2, 2, "MUSH", true);
    switch (mg_msg) {
    case MG_MSG_RAIN:
        snprintf(buf, sizeof(buf), "SCORE %d RAIN +1", mg_score);
        break;
    case MG_MSG_PEST:
        snprintf(buf, sizeof(buf), "SCORE %d PESTS -1", mg_score);
        break;
    case MG_MSG_ROT:
        snprintf(buf, sizeof(buf), "SCORE %d ROT -1", mg_score);
        break;
    case MG_MSG_HARVEST:
        snprintf(buf, sizeof(buf), "SCORE %d +2", mg_score);
        break;
    default:
        snprintf(buf, sizeof(buf), "SCORE %d TURN %d/%d",
                 mg_score, mg_turn, MG_TURNS);
        break;
    }
    fb_text(CCG_W - 2 - text_width(buf), 2, buf, true);
    fb_hline(0, CCG_HUD_H - 1, CCG_W, true);

    /* 花园: 先全部格子, 最后光标 */
    for (int i = 0; i < MG_N; i++) mg_draw_cell(i);
    mg_draw_cursor();
}

void mushgarden_on_key(const key_event_t *ev) {
    if (mg_state == MG_ST_OVER) {
        if (!ev->is_repeat) {
            if (ev->key == K_OK || (ev->key == K_CHAR && ev->ch == 'n')) {
                mg_new_game();
            } else if (ev->key == K_BACK || ev->key == K_QUIT) {
                s_exit_request = true;
            }
        }
        return;
    }

    if (ev->key == K_CHAR) {
        if (ev->is_repeat) return;
        if (ev->ch == 'n') {
            mg_new_game();
        } else if (ev->ch == 'q') {
            s_exit_request = true;
        }
        return;
    }

    switch (ev->key) {
    case K_UP:
        mg_cy = (mg_cy + MG_ROWS - 1) % MG_ROWS;
        if (!ev->is_repeat) audio_move();
        break;
    case K_DOWN:
        mg_cy = (mg_cy + 1) % MG_ROWS;
        if (!ev->is_repeat) audio_move();
        break;
    case K_LEFT:
        mg_cx = (mg_cx + MG_COLS - 1) % MG_COLS;
        if (!ev->is_repeat) audio_move();
        break;
    case K_RIGHT:
        mg_cx = (mg_cx + 1) % MG_COLS;
        if (!ev->is_repeat) audio_move();
        break;
    case K_OK:
        if (!ev->is_repeat) mg_act();
        break;
    case K_BACK:
    case K_PAUSE:
        if (!ev->is_repeat) {
            pause_sel_t sel;
            if (ui_pause_run(&sel)) {
                if (sel == PAUSE_RESTART) mg_new_game();
            } else {
                s_exit_request = true;
            }
        }
        break;
    case K_QUIT:
        if (!ev->is_repeat) s_exit_request = true;
        break;
    default:
        break;
    }
}
