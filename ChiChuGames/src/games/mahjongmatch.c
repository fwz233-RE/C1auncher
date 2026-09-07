/* MAHJONG MATCH — 麻将消消乐: 6x6 找对消牌
 * 36 张 = 18 对, 图案: '1'-'9'(万/筒/条) + 'E S W N C F B'(东南西北中发白)
 *              + 花牌 ★/♥(CG_STAR/CG_HEART 符号), 全部 2x 大字。
 * 配对规则(平铺简化): 两张相同图案且各自"可消"才可消。
 *   "可消" = 左或右至少一侧开放(无邻牌)。上方无牌遮挡在平铺布局中
 *   恒成立, 故只判左右侧。
 * 操作: 方向/WASD 移光标, OK 选牌, 再 OK 另一张相同 → 消除;
 *   不同图案 → 提示 NOT A PAIR; 同图案但被夹 → 提示 LOCKED。
 * 全部消除 → WIN(计步); 死局(无任何可消对) → 提示, N 重洗。
 * 输入驱动(回合制, 同 fifteen): 开局/结束全刷, 游戏内快刷
 * (render 由主循环绘制后 disp_fast)。
 *
 * games_table.c 建议条目(help <=5 行 + NULL):
 *   "MAHJONG MATCH",
 *   "ARROWS/WASD: MOVE  OK: SELECT",
 *   "OK AGAIN ON SAME TILE = MATCH",
 *   "FREE = NO TILE ON ITS SIDE",
 *   "N:SHUFFLE  Q:QUIT",
 *   NULL */
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

#define MJ_CELL 20
#define MJ_OX 8
#define MJ_OY 20                  /* HUD 16 下留 4px */
#define MJ_DIVX 132               /* 棋盘右缘 128, 分隔线 132 */
#define MJ_SX 136                 /* 侧栏 x */
#define MJ_N 6

enum { MJ_MSG_INIT = 0, MJ_MSG_SEL, MJ_MSG_MATCH, MJ_MSG_WRONG,
       MJ_MSG_LOCK, MJ_MSG_DEAD, MJ_MSG_EMPTY, MJ_MSG_COUNT };

/* ---- 静态状态(前缀 mj_) ---- */
static uint8_t mj_board[MJ_N * MJ_N];   /* 0=空, 1..18=图案 */
static int mj_cx, mj_cy;                /* 光标(0..5), int 像素坐标纪律 */
static int mj_sel;                      /* 已选格索引, -1=无 */
static int mj_moves;                    /* 已消对数 */
static int mj_pairs_left;               /* 剩余对数 */
static int mj_status;                   /* 侧栏提示(枚举) */
static bool mj_over, mj_over_full;
static rng_t mj_rng;
static uint32_t mj_seed_cnt;

static const char *mj_hint1[MJ_MSG_COUNT] = {
    "OK PICK, OK MATCH", "SELECTED", "PAIR CLEARED!",
    "NOT A PAIR", "LOCKED", "NO MOVES", "EMPTY CELL",
};
static const char *mj_hint2[MJ_MSG_COUNT] = {
    "FREE = SIDE OPEN", "OK ITS MATE", "KEEP GOING",
    "PICK AGAIN", "FREE L/R SIDE", "N:SHUFFLE BOARD", "MOVE CURSOR",
};

/* ---- 核心逻辑 ---- */

/* 可消: 有牌 && (左或右至少一侧开放); 平铺布局无上方遮挡 */
static bool mj_free(int idx) {
    if (mj_board[idx] == 0) return false;
    int x = idx % MJ_N;
    if (x == 0 || x == 5) return true;
    if (mj_board[idx - 1] == 0) return true;
    if (mj_board[idx + 1] == 0) return true;
    return false;
}

/* 死局检测: 是否存在两张相同且都可消的牌 */
static bool mj_has_move(void) {
    for (int i = 0; i < MJ_N * MJ_N; i++) {
        if (mj_board[i] == 0 || !mj_free(i)) continue;
        for (int j = i + 1; j < MJ_N * MJ_N; j++) {
            if (mj_board[j] == mj_board[i] && mj_free(j)) return true;
        }
    }
    return false;
}

/* 随机重排 18 对牌; 洗到有可消对为止(do-while + guard 防极端死循环) */
static void mj_shuffle(void) {
    uint8_t deck[MJ_N * MJ_N];
    for (int i = 0; i < MJ_N * MJ_N; i++) deck[i] = (uint8_t)(i / 2 + 1);
    int guard = 0;
    do {
        for (int i = MJ_N * MJ_N - 1; i > 0; i--) {   /* Fisher-Yates */
            int j = (int)rng_range(&mj_rng, (uint32_t)(i + 1));
            uint8_t t = deck[i];
            deck[i] = deck[j];
            deck[j] = t;
        }
        for (int i = 0; i < MJ_N * MJ_N; i++) mj_board[i] = deck[i];
        guard++;
    } while (!mj_has_move() && guard < 200);
    mj_sel = -1;
    mj_cx = 0;
    mj_cy = 0;
}

static void mj_reshuffle(void) {
    mj_shuffle();
    mj_status = mj_has_move() ? MJ_MSG_INIT : MJ_MSG_DEAD;
}

static void mj_new_game(void) {
    mj_moves = 0;
    mj_pairs_left = 18;
    mj_over = false;
    mj_over_full = false;
    mj_reshuffle();
}

/* OK: 选牌 / 配对消除 / 取消(同格) */
static void mj_ok(void) {
    if (mj_over) return;
    int i = mj_cy * MJ_N + mj_cx;
    if (mj_board[i] == 0) { mj_status = MJ_MSG_EMPTY; return; }
    if (mj_sel < 0) {
        mj_sel = i;
        mj_status = MJ_MSG_SEL;
        audio_select();                     /* 选中第一张 */
        return;
    }
    if (mj_sel == i) { mj_sel = -1; mj_status = MJ_MSG_INIT; return; }
    if (mj_board[i] == mj_board[mj_sel]) {
        if (mj_free(i) && mj_free(mj_sel)) {
            mj_board[i] = 0;
            mj_board[mj_sel] = 0;
            mj_sel = -1;
            mj_moves++;
            mj_pairs_left--;
            if (mj_pairs_left == 0) {
                mj_over = true;             /* 全部消除 → 通关 */
                mj_over_full = false;
                audio_win();
                led_fx_set(LED_FX_WIN);
            } else if (!mj_has_move()) {
                mj_status = MJ_MSG_DEAD;
            } else {
                mj_status = MJ_MSG_MATCH;
                audio_clear();              /* 配对成功 */
            }
            return;
        }
        mj_sel = -1;
        mj_status = MJ_MSG_LOCK;
        return;
    }
    mj_sel = -1;
    mj_status = MJ_MSG_WRONG;
    audio_error();                          /* 图案不同 */
}

void mahjongmatch_render(void);

static void mj_pause(void) {
    pause_sel_t sel;
    if (ui_pause_run(&sel)) {
        if (sel == PAUSE_RESTART) {
            mj_new_game();
            mahjongmatch_render();
            disp_full();
        }
    } else {
        s_exit_request = true;
    }
}

/* ---- 渲染 ---- */

/* 牌面字符: 数字/字母走 2x 文本, 花牌符号 2x 放大 */
static void mj_draw_tile(int bx, int by, uint8_t v, bool black) {
    int px = bx + (MJ_CELL - 2 * FONT_W) / 2;   /* 2x 宽 10 */
    int py = by + (MJ_CELL - 2 * FONT_H) / 2;   /* 2x 高 14 */
    if (v <= 16) {
        char s[2];
        s[0] = (v <= 9) ? (char)('0' + v) : "ESWNCFB"[(int)v - 10];
        s[1] = 0;
        fb_text_scale2(px, py, s, black);
    } else {
        const uint8_t *g = font_symbols[(v == 17) ? CG_STAR : CG_HEART];
        for (int j = 0; j < FONT_H; j++)
            for (int i = 0; i < FONT_W; i++)
                if (g[j] & (1u << i))
                    fb_fill_rect(px + i * 2, py + j * 2, 2, 2, black);
    }
}

/* 光标: 反白格(黑底白字, 四周对称)——网格线在格边界 bx-1/bx+19, 反白格
 * 覆盖整格必然清晰可见; 选中格画双框。必须最后画盖住牌面。 */
static void mj_draw_cursor(void) {
    int cidx = mj_cy * MJ_N + mj_cx;
    int bx = MJ_OX + mj_cx * MJ_CELL;
    int by = MJ_OY + mj_cy * MJ_CELL;
    fb_fill_rect(bx, by, MJ_CELL, MJ_CELL, true);
    if (mj_board[cidx] != 0) mj_draw_tile(bx, by, mj_board[cidx], false);
    if (mj_sel >= 0 && mj_sel == cidx) {
        /* 选中格恰在光标上: 黑格内白双框 */
        fb_stroke_rect(bx + 1, by + 1, MJ_CELL - 2, MJ_CELL - 2, false);
        fb_stroke_rect(bx + 2, by + 2, MJ_CELL - 4, MJ_CELL - 4, false);
    } else if (mj_sel >= 0) {
        int sx = MJ_OX + (mj_sel % MJ_N) * MJ_CELL;
        int sy = MJ_OY + (mj_sel / MJ_N) * MJ_CELL;
        fb_stroke_rect(sx - 1, sy - 1, MJ_CELL + 2, MJ_CELL + 2, true);
        fb_stroke_rect(sx - 2, sy - 2, MJ_CELL + 4, MJ_CELL + 4, true);
    }
}

static void mj_itoa(int v, char *out) {
    char rev[8];
    int n = 0;
    if (v == 0) rev[n++] = '0';
    while (v > 0 && n < 7) {
        rev[n++] = (char)('0' + v % 10);
        v /= 10;
    }
    while (n > 0) *out++ = rev[--n];
    *out = 0;
}

/* 侧栏 2x 反白大字块(小标签 + 反白大字数值) */
static void mj_block(int x, int y, const char *s) {
    int w = text_width(s) * 2;
    fb_fill_rect(x, y, w, FONT_H * 2, true);
    fb_text_scale2(x, y, s, false);
}

static void mj_draw_sidebar(void) {
    fb_vline(MJ_DIVX, MJ_OY, MJ_N * MJ_CELL, true);
    fb_text(MJ_SX, 22, "PAIRS", true);
    {
        char b[4];
        mj_itoa(mj_pairs_left, b);
        mj_block(MJ_SX, 31, b);
    }
    fb_text(MJ_SX, 58, "MOVES", true);
    {
        char b[4];
        mj_itoa(mj_moves, b);
        mj_block(MJ_SX, 67, b);
    }
    fb_hline(MJ_SX, 92, CCG_W - MJ_SX - 4, true);
    fb_text(MJ_SX, 100, mj_hint1[mj_status], true);
    fb_text(MJ_SX, 110, mj_hint2[mj_status], true);
    fb_text(MJ_SX, 143, "N:SHUFFLE Q:QUIT", true);
}

/* HUD 顶栏: 黑字白底, 左标题右标签+数值; 胜利换两行提示 */
static void mj_draw_hud(void) {
    fb_fill_rect(0, 0, CCG_W, CCG_HUD_H - 1, false);
    if (mj_over) {
        fb_text(2, 2, "CLEARED!", true);
        fb_text(CCG_W - 2 - text_width("OK/N:RETRY BACK:QUIT"), 2,
                "OK/N:RETRY BACK:QUIT", true);
        char line[16];
        int m = 0;
        while ("MOVES "[m]) { line[m] = "MOVES "[m]; m++; }
        mj_itoa(mj_moves, line + m);
        fb_text(2, 10, line, true);
        fb_text(CCG_W - 2 - text_width("N:NEW GAME"), 10, "N:NEW GAME", true);
    } else {
        fb_text(0, 0, "MAHJONG", true);
        char line[16];
        int m = 0;
        while ("MOVES "[m]) { line[m] = "MOVES "[m]; m++; }
        mj_itoa(mj_moves, line + m);
        fb_text(CCG_W - 2 - text_width(line), 0, line, true);
    }
    fb_hline(0, CCG_HUD_H - 1, CCG_W, true);
}

void mahjongmatch_render(void) {
    fb_clear(false);
    for (int y = 0; y < MJ_N; y++) {
        for (int x = 0; x < MJ_N; x++) {
            int bx = MJ_OX + x * MJ_CELL;
            int by = MJ_OY + y * MJ_CELL;
            fb_stroke_rect(bx, by, MJ_CELL, MJ_CELL, true);
            uint8_t v = mj_board[y * MJ_N + x];
            if (v != 0) mj_draw_tile(bx, by, v, true);
        }
    }
    fb_stroke_rect_thick(MJ_OX - 2, MJ_OY - 2, MJ_N * MJ_CELL + 4,
                         MJ_N * MJ_CELL + 4, 2, true);
    mj_draw_sidebar();
    if (!mj_over) mj_draw_cursor();
    mj_draw_hud();
    if (mj_over && !mj_over_full) {
        mj_over_full = true;
        disp_force_full();
    }
}

/* ---- 输入 ---- */
void mahjongmatch_on_key(const key_event_t *ev) {
    /* 确认键/字母忽略重复; 方向/wasd 长按可连移(框架合成) */
    if (ev->is_repeat && ev->key != K_UP && ev->key != K_DOWN &&
        ev->key != K_LEFT && ev->key != K_RIGHT && ev->key != K_CHAR)
        return;
    if (mj_over) {
        if (ev->is_repeat) return;
        if (ev->key == K_OK || (ev->key == K_CHAR && ev->ch == 'n')) {
            mj_new_game();
            mahjongmatch_render();
            disp_full();
        } else if (ev->key == K_BACK || ev->key == K_QUIT) {
            s_exit_request = true;
        }
        return;
    }
    switch (ev->key) {
    case K_UP:
        mj_cy = (mj_cy + 5) % MJ_N;
        break;
    case K_DOWN:
        mj_cy = (mj_cy + 1) % MJ_N;
        break;
    case K_LEFT:
        mj_cx = (mj_cx + 5) % MJ_N;
        break;
    case K_RIGHT:
        mj_cx = (mj_cx + 1) % MJ_N;
        break;
    case K_OK:
        mj_ok();
        break;
    case K_BACK:
    case K_PAUSE:
        mj_pause();
        break;
    case K_QUIT:
        s_exit_request = true;
        break;
    case K_CHAR:
        if (ev->ch == 'w') {
            mj_cy = (mj_cy + 5) % MJ_N;
        } else if (ev->ch == 's') {
            mj_cy = (mj_cy + 1) % MJ_N;
        } else if (ev->ch == 'a') {
            mj_cx = (mj_cx + 5) % MJ_N;
        } else if (ev->ch == 'd') {
            mj_cx = (mj_cx + 1) % MJ_N;
        } else if (ev->ch == 'n') {
            if (!ev->is_repeat) mj_reshuffle();
        } else if (ev->ch == 'q') {
            if (!ev->is_repeat) s_exit_request = true;
        }
        break;
    default:
        break;
    }
}

/* ---- 框架 ---- */
void mahjongmatch_enter(void) {
    rng_seed(&mj_rng, now_ms() ^ 0x4D4Au ^ ((uint64_t)mj_seed_cnt++ << 32));
    mj_new_game();
    mahjongmatch_render();
    disp_full();
}

void mahjongmatch_exit(void) {}

void mahjongmatch_tick(uint64_t now) { (void)now; }
