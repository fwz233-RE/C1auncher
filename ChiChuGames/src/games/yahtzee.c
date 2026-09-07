/* YAHTZEE — 快艇骰子: 5 骰 3 掷, 13 类别计分(单人)
 * DICE 阶段: LEFT/RIGHT 选骰, OK 锁定/解锁(反白), UP 掷(最多 3 次), DOWN 进入计分
 * SCORE 阶段: 方向键选类别, OK 确认计分, BACK 回 DICE
 * 上区 1-6 合计 >= 63 奖励 35 分; 13 类别填满后结算总分
 * N=新局 P=暂停 Q=退出; W/S/A/D 同方向 */
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

#define YZ_DIE 40              /* 骰子边长 */
#define YZ_DICE_X 30           /* 首骰 x */
#define YZ_DICE_Y 18           /* 骰子行 y(0 与 40 的 pip 网格使用 8px 格) */
#define YZ_STEP 49             /* 骰间距 */
#define YZ_TAB_Y 68            /* 计分表顶 */
#define YZ_ROW_H 12            /* 表行高 */
#define YZ_COL_B 152           /* 右列标签 x */

typedef enum {
    YZ_PH_DICE = 0,            /* 选骰/掷骰 */
    YZ_PH_SCORE,               /* 选类别计分 */
    YZ_PH_OVER                 /* 结算 */
} yz_phase_t;

static rng_t yz_rng;
static int yz_dice[5];         /* 1..6 */
static bool yz_hold[5];        /* 本回合锁定(保留) */
static int yz_score[13];       /* -1 = 未填 */
static yz_phase_t yz_phase;
static int yz_rolls;           /* 本回合已掷 1..3 */
static int yz_cur;             /* 骰子光标 0..4 */
static int yz_cat;             /* 类别光标 0..12 */
static int yz_turn;            /* 已填类别数 0..13 */
static bool yz_over_full;      /* 结算全刷防重复 */

void yahtzee_render(void);     /* enter 在前, 先声明 */

/* ---- 计分 ---- */
static int yz_face_sum(int v) {
    int s = 0;
    for (int i = 0; i < 5; i++)
        if (yz_dice[i] == v) s += v;
    return s;
}

static int yz_total_sum(void) {
    int s = 0;
    for (int i = 0; i < 5; i++) s += yz_dice[i];
    return s;
}

static int yz_max_count(void) {
    int best = 0;
    for (int v = 1; v <= 6; v++) {
        int c = 0;
        for (int i = 0; i < 5; i++)
            if (yz_dice[i] == v) c++;
        if (c > best) best = c;
    }
    return best;
}

static int yz_full_house(void) {   /* 严格 3+2(五同不算) */
    int cnt[7] = { 0, 0, 0, 0, 0, 0, 0 };
    int c3 = 0, c2 = 0;
    for (int i = 0; i < 5; i++) cnt[yz_dice[i]]++;
    for (int v = 1; v <= 6; v++) {
        if (cnt[v] == 3) c3++;
        else if (cnt[v] == 2) c2++;
    }
    return (c3 == 1 && c2 == 1) ? 25 : 0;
}

static int yz_face_mask(void) {
    int m = 0;
    for (int i = 0; i < 5; i++) m |= 1 << (yz_dice[i] - 1);
    return m;
}

static int yz_small_straight(void) {
    int m = yz_face_mask();
    if ((m & 0x0F) == 0x0F || (m & 0x1E) == 0x1E || (m & 0x3C) == 0x3C)
        return 30;               /* 1234 / 2345 / 3456 */
    return 0;
}

static int yz_large_straight(void) {
    int m = yz_face_mask();
    return (m == 0x1F || m == 0x3E) ? 40 : 0;   /* 12345 / 23456 */
}

/* 类别 0..12 在当前骰面下的得分 */
static int yz_calc(int cat) {
    if (cat < 6) return yz_face_sum(cat + 1);
    switch (cat) {
    case 6:  return yz_max_count() >= 3 ? yz_total_sum() : 0;
    case 7:  return yz_max_count() >= 4 ? yz_total_sum() : 0;
    case 8:  return yz_full_house();
    case 9:  return yz_small_straight();
    case 10: return yz_large_straight();
    case 11: return yz_max_count() >= 5 ? 50 : 0;
    default: return yz_total_sum();             /* 12: CHANCE */
    }
}

static int yz_upper_sum(void) {
    int s = 0;
    for (int i = 0; i < 6; i++)
        if (yz_score[i] >= 0) s += yz_score[i];
    return s;
}

static int yz_total(void) {
    int s = 0;
    for (int i = 0; i < 13; i++)
        if (yz_score[i] >= 0) s += yz_score[i];
    return s + (yz_upper_sum() >= 63 ? 35 : 0);
}

/* ---- 回合流程 ---- */
static void yz_roll(void) {
    if (yz_phase != YZ_PH_DICE || yz_rolls >= 3) return;
    yz_rolls++;
    for (int i = 0; i < 5; i++)
        if (!yz_hold[i])
            yz_dice[i] = (int)rng_range(&yz_rng, 6) + 1;
    if (yz_rolls >= 3) yz_phase = YZ_PH_SCORE;  /* 掷满 3 次强制计分 */
    audio_move();
}

static void yz_confirm(void) {
    if (yz_phase != YZ_PH_SCORE || yz_score[yz_cat] >= 0) {
        audio_error();
        return;
    }
    yz_score[yz_cat] = yz_calc(yz_cat);
    yz_turn++;
    if (yz_turn >= 13) {
        yz_phase = YZ_PH_OVER;
        audio_win();
        led_fx_set(LED_FX_WIN);
        return;
    }
    audio_clear();
    for (int i = 0; i < 5; i++) yz_hold[i] = false;
    yz_phase = YZ_PH_DICE;
    yz_rolls = 0;
    yz_cur = 0;
    yz_roll();                                  /* 新回合第 1 掷 */
}

static void yz_new_game(void) {
    for (int i = 0; i < 13; i++) yz_score[i] = -1;
    yz_turn = 0;
    yz_cur = 0;
    yz_cat = 0;
    yz_phase = YZ_PH_DICE;
    yz_rolls = 0;
    yz_over_full = false;
    yz_roll();
}

/* ---- 渲染工具 ---- */
static void yz_num(char *buf, int v) {
    char tmp[12];
    int n = 0;
    if (v <= 0) tmp[n++] = '0';
    while (v > 0 && n < 11) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; }
    for (int i = 0; i < n; i++) buf[i] = tmp[n - 1 - i];
    buf[n] = 0;
}

static void yz_append(char *s, int *n, const char *t) {
    while (*t) s[(*n)++] = *t++;
}

/* pip 位置: 3x3 网格 (0,0)..(2,2), {-1,-1} 结尾 */
static const int8_t yz_pips[7][7][2] = {
    { { -1, -1 } },
    { { 1, 1 }, { -1, -1 } },
    { { 0, 0 }, { 2, 2 }, { -1, -1 } },
    { { 0, 0 }, { 1, 1 }, { 2, 2 }, { -1, -1 } },
    { { 0, 0 }, { 2, 0 }, { 0, 2 }, { 2, 2 }, { -1, -1 } },
    { { 0, 0 }, { 2, 0 }, { 1, 1 }, { 0, 2 }, { 2, 2 }, { -1, -1 } },
    { { 0, 0 }, { 2, 0 }, { 0, 1 }, { 2, 1 }, { 0, 2 }, { 2, 2 }, { -1, -1 } }
};

/* 骰面: 白底黑点; 锁定=反白(黑底白点白框); 光标=外围 2px 反色框(最后画) */
static void yz_draw_die(int x, int y, int face, bool locked, bool sel) {
    bool black = locked;
    fb_fill_rect(x, y, YZ_DIE, YZ_DIE, black);
    for (int k = 0; k < 7; k++) {
        int px = yz_pips[face][k][0];
        if (px < 0) break;
        int py = yz_pips[face][k][1];
        fb_fill_rect(x + 8 + px * 8, y + 8 + py * 8, 8, 8, !black);
    }
    fb_stroke_rect(x, y, YZ_DIE, YZ_DIE, !black);
    if (sel) fb_stroke_rect_thick(x - 2, y - 2, YZ_DIE + 4, YZ_DIE + 4, 2, !black);
}

static void yz_draw_status(void) {
    if (yz_phase == YZ_PH_OVER) return;      /* 结算时墙内不放提示文字 */
    char s[40];
    int n = 0;
    yz_append(s, &n, "R ");
    char rs[4];
    yz_num(rs, yz_rolls);
    yz_append(s, &n, rs);
    yz_append(s, &n, "/3  ");
    if (yz_phase == YZ_PH_DICE)
        yz_append(s, &n, "OK:HOLD  UP:ROLL  DN:SCORE");
    else
        yz_append(s, &n, "OK:SCORE  BACK:DICE");
    s[n] = 0;
    fb_text(YZ_DICE_X, 59, s, true);
}

static const char *const yz_names[13] = {
    "ONES", "TWOS", "THREES", "FOURS", "FIVES", "SIXES",
    "3KIND", "4KIND", "FULL", "SMALL", "LARGE", "YAHTZ", "CHANCE"
};

/* 计分表: 左列上区 6 类 + BONUS 行, 右列下区 7 类; 未填显示当前可得分 */
static void yz_draw_table(void) {
    fb_vline(146, YZ_TAB_Y, CCG_H - YZ_TAB_Y, true);
    for (int c = 0; c < 13; c++) {
        int col = (c < 6) ? 0 : 1;
        int r = (c < 6) ? c : c - 6;
        int x = (col == 0) ? 6 : YZ_COL_B;
        int y = YZ_TAB_Y + r * YZ_ROW_H;
        bool cur = (yz_phase == YZ_PH_SCORE) && (c == yz_cat);
        if (cur) {
            fb_fill_rect(col == 0 ? 2 : 150, y, 140, YZ_ROW_H, true);
            fb_text(x, y + 2, yz_names[c], false);
        } else {
            fb_text(x, y + 2, yz_names[c], true);
        }
        char val[8];
        if (yz_score[c] >= 0) { val[0] = '='; yz_num(&val[1], yz_score[c]); }
        else yz_num(val, yz_calc(c));
        fb_text((col == 0 ? 142 : 292) - text_width(val), y + 2, val, !cur);
    }
    /* 左列第 7 行: 上区小计(>=63 得 35 奖励) */
    int y = YZ_TAB_Y + 6 * YZ_ROW_H;
    fb_text(6, y + 2, "BONUS", true);
    char b[8];
    int nb = 0;
    if (yz_upper_sum() >= 63) {
        b[nb++] = '=';
        yz_num(&b[nb], 35);
    } else {
        char us[4];
        yz_num(us, yz_upper_sum());
        for (int i = 0; us[i]; i++) b[nb++] = us[i];
        b[nb++] = '/';
        b[nb++] = '6';
        b[nb++] = '3';
    }
    b[nb] = 0;
    fb_text(142 - text_width(b), y + 2, b, true);
}

/* ---- 框架接口 ---- */
void yahtzee_enter(void) {
    rng_seed(&yz_rng, now_ms() ^ 0x59A7u);
    yz_new_game();
    yahtzee_render();
    disp_full();
}

void yahtzee_exit(void) {}

void yahtzee_tick(uint64_t now) {
    (void)now;                   /* 纯按键驱动, 无需周期逻辑 */
}

void yahtzee_render(void) {
    fb_clear(false);
    hud_draw("YAHTZEE", (uint32_t)yz_total());
    for (int i = 0; i < 5; i++)
        yz_draw_die(YZ_DICE_X + i * YZ_STEP, YZ_DICE_Y, yz_dice[i],
                    yz_hold[i], yz_phase == YZ_PH_DICE && i == yz_cur);
    yz_draw_status();
    yz_draw_table();
    if (yz_phase == YZ_PH_OVER) {
        /* 结算: HUD 区左结果 + 右提示, 全刷一次 */
        fb_fill_rect(0, 0, CCG_W, CCG_HUD_H - 1, false);
        char fin[16];
        int n = 0;
        yz_append(fin, &n, "FINAL ");
        char ts[4];
        yz_num(ts, yz_total());
        yz_append(fin, &n, ts);
        fin[n] = 0;
        fb_text(2, 2, fin, true);
        fb_text(CCG_W - 2 - text_width("OK/N:RETRY BACK:QUIT"), 2,
                "OK/N:RETRY BACK:QUIT", true);
        if (!yz_over_full) { yz_over_full = true; disp_force_full(); }
    }
}

void yahtzee_on_key(const key_event_t *ev) {
    if (ev->key == K_PAUSE || (ev->key == K_CHAR && ev->ch == 'p')) {
        pause_sel_t sel;
        if (ui_pause_run(&sel)) {
            if (sel == PAUSE_RESTART) yahtzee_enter();
        } else {
            s_exit_request = true;
        }
        return;
    }
    if (yz_phase == YZ_PH_OVER) {
        if (ev->is_repeat) return;
        if (ev->key == K_OK || (ev->key == K_CHAR && ev->ch == 'n'))
            yahtzee_enter();
        else if (ev->key == K_BACK || ev->key == K_QUIT)
            s_exit_request = true;
        return;
    }
    /* 方向键可响应长按重复, 确认键/字母忽略 */
    bool dir = ev->key == K_UP || ev->key == K_DOWN ||
               ev->key == K_LEFT || ev->key == K_RIGHT;
    if (ev->is_repeat && !dir) return;
    switch (ev->key) {
    case K_UP:
        if (yz_phase == YZ_PH_DICE) yz_roll();
        else yz_cat = (yz_cat + 12) % 13;
        break;
    case K_DOWN:
        if (yz_phase == YZ_PH_DICE) yz_phase = YZ_PH_SCORE;
        else yz_cat = (yz_cat + 1) % 13;
        break;
    case K_LEFT:
        if (yz_phase == YZ_PH_DICE) yz_cur = (yz_cur + 4) % 5;
        else yz_cat = (yz_cat + 12) % 13;
        break;
    case K_RIGHT:
        if (yz_phase == YZ_PH_DICE) yz_cur = (yz_cur + 1) % 5;
        else yz_cat = (yz_cat + 1) % 13;
        break;
    case K_OK:
        if (yz_phase == YZ_PH_DICE) yz_hold[yz_cur] = !yz_hold[yz_cur];
        else yz_confirm();
        break;
    case K_BACK:
        if (yz_phase == YZ_PH_SCORE) yz_phase = YZ_PH_DICE;  /* 子模式返回 */
        else {
            pause_sel_t sel;
            if (ui_pause_run(&sel)) {
                if (sel == PAUSE_RESTART) yahtzee_enter();
            } else {
                s_exit_request = true;
            }
        }
        break;
    case K_CHAR:
        if (ev->ch == 'n') yahtzee_enter();
        else if (ev->ch == 'w') {
            if (yz_phase == YZ_PH_DICE) yz_roll();
            else yz_cat = (yz_cat + 12) % 13;
        } else if (ev->ch == 's') {
            if (yz_phase == YZ_PH_DICE) yz_phase = YZ_PH_SCORE;
            else yz_cat = (yz_cat + 1) % 13;
        } else if (ev->ch == 'a') {
            if (yz_phase == YZ_PH_DICE) yz_cur = (yz_cur + 4) % 5;
            else yz_cat = (yz_cat + 12) % 13;
        } else if (ev->ch == 'd') {
            if (yz_phase == YZ_PH_DICE) yz_cur = (yz_cur + 1) % 5;
            else yz_cat = (yz_cat + 1) % 13;
        }
        break;
    case K_QUIT:
        s_exit_request = true;
        break;
    default:
        break;
    }
}
