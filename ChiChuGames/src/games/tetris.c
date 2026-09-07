/* 俄罗斯方块 — 场地 10x13, 格 10x10, 重力 700ms 起步
 * e-ink 适配: 移动/旋转/软降=快刷; 开局/结束/暂停=全刷(flag 切换) */
#include "game.h"
#include "../config.h"
#include "../gfx/canvas.h"
#include "../gfx/font.h"
#include "../platform/display.h"
#include "../platform/input.h"
#include "../rng.h"
#include "../platform/time.h"
#include "../platform/audio.h"
#include "../platform/led.h"

#define TT_W 14
#define TT_H 11
#define TT_CELL 12
#define TT_OX 4
#define TT_OY CCG_HUD_H            /* 16 */
#define TT_BASE_GRAVITY 700
#define TT_MIN_GRAVITY 300

/* 七种方块 × 4 旋转, 4x4 位图(行式 nibble 打包, 行0=顶) */
static const uint16_t tt_pieces[7][4] = {
    { 0x0F00, 0x2222, 0x00F0, 0x4444 },  /* I */
    { 0x6600, 0x6600, 0x6600, 0x6600 },  /* O */
    { 0x0E40, 0x4C40, 0x4E00, 0x4640 },  /* T */
    { 0x06C0, 0x4620, 0x06C0, 0x4620 },  /* S */
    { 0x0C60, 0x2640, 0x0C60, 0x2640 },  /* Z */
    { 0x0E80, 0x44C0, 0x02E0, 0x6440 },  /* J */
    { 0x0E20, 0x4460, 0x6220, 0xC440 },  /* L */
};

static uint8_t tt_field[TT_H][TT_W];   /* 0=空 1..7=方块 */
static uint8_t tt_piece, tt_rot;
static int8_t tt_px, tt_py;             /* 当前方块左上角(格坐标) */
static uint8_t tt_next;
static uint8_t tt_hold;            /* 0=空, 1..7=暂存方块 */
static bool tt_hold_used;
static uint8_t tt_bag[7], tt_bag_idx;
static uint32_t tt_score, tt_lines;
static uint32_t tt_gravity_ms;
static bool tt_alive, tt_paused, tt_started, tt_over_full;
static rng_t tt_rng;

void tetris_render(void);
static void do_hold(void);

static uint16_t piece_mask(uint8_t p, uint8_t r) { return tt_pieces[p][r & 3u]; }

/* 方块占据的格位集合 */
static void piece_cells(uint8_t p, uint8_t r, int px, int py,
                        int out[8][2], int *count) {
    uint16_t m = piece_mask(p, r);
    int n = 0;
    for (int j = 0; j < 4; j++)
        for (int i = 0; i < 4; i++)
            if (m & (1u << (15 - (j * 4 + i)))) {
                out[n][0] = px + i;
                out[n][1] = py + j;
                n++;
            }
    *count = n;
}

static bool collides(int px, int py, uint8_t p, uint8_t r) {
    int cells[8][2], n;
    piece_cells(p, r, px, py, cells, &n);
    for (int k = 0; k < n; k++) {
        int x = cells[k][0], y = cells[k][1];
        if (x < 0 || x >= TT_W || y >= TT_H) return true;
        if (y >= 0 && tt_field[y][x]) return true;
    }
    return false;
}

static void bag_fill(void) {
    if (tt_bag_idx >= 7) {
        /* 洗牌(费雪-耶茨) */
        for (int i = 6; i > 0; i--) {
            uint32_t j = rng_range(&tt_rng, (uint32_t)i + 1);
            uint8_t t = tt_bag[i];
            tt_bag[i] = tt_bag[j];
            tt_bag[j] = t;
        }
        tt_bag_idx = 0;
    }
}

static uint8_t bag_take(void) {
    bag_fill();
    return tt_bag[tt_bag_idx++];
}

/* 把 tt_piece 放入场地顶部, 放不下则死亡 */
static void spawn(void) {
    tt_rot = 0;
    tt_px = (TT_W - 4) / 2;   /* 居中出生 */
    tt_py = -1;   /* 部分在场上边界之上 */
    if (collides(tt_px, tt_py, tt_piece, tt_rot)) {
        tt_alive = false;     /* 出生即撞: 游戏结束 */
        audio_lose();
        led_fx_set(LED_FX_LOSE);
    }
}

static void do_hold(void) {
    if (tt_hold_used) return;    /* 每块只可交换一次 */
    if (tt_hold == 0) {
        tt_hold = tt_piece + 1;
        tt_piece = tt_next;
        tt_next = bag_take();
    } else {
        uint8_t t = tt_piece;
        tt_piece = tt_hold - 1;
        tt_hold = t + 1;
    }
    tt_hold_used = true;
    spawn();
}

void tetris_enter(void) {
    rng_seed(&tt_rng, now_ms() ^ 0x7E7);
    for (int y = 0; y < TT_H; y++)
        for (int x = 0; x < TT_W; x++) tt_field[y][x] = 0;
    for (int i = 0; i < 7; i++) tt_bag[i] = (uint8_t)i;
    tt_bag_idx = 7;               /* 触发首次洗牌 */
    tt_score = 0;
    tt_lines = 0;
    tt_gravity_ms = TT_BASE_GRAVITY;
    tt_alive = true;
    tt_paused = false;
    tt_started = false;
    tt_over_full = false;
    tt_hold = 0;
    tt_hold_used = false;
    tt_piece = bag_take();
    tt_next = bag_take();
    spawn();                     /* 出生 */
    game_set_tick_interval(tt_gravity_ms);
    tetris_render();
    disp_full();                 /* 开局场景切换 */
}

static void lock_piece(void) {
    int cells[8][2], n;
    piece_cells(tt_piece, tt_rot, tt_px, tt_py, cells, &n);
    for (int k = 0; k < n; k++) {
        int x = cells[k][0], y = cells[k][1];
        if (x < 0 || x >= TT_W || y < 0 || y >= TT_H) {
            tt_alive = false;
            audio_lose();
            led_fx_set(LED_FX_LOSE);
            return;
        }
        tt_field[y][x] = tt_piece + 1;
    }
    /* 消行 */
    int cleared = 0;
    for (int y = TT_H - 1; y >= 0; y--) {
        bool full = true;
        for (int x = 0; x < TT_W; x++)
            if (!tt_field[y][x]) { full = false; break; }
        if (full) {
            for (int yy = y; yy > 0; yy--)
                for (int x = 0; x < TT_W; x++)
                    tt_field[yy][x] = tt_field[yy - 1][x];
            for (int x = 0; x < TT_W; x++) tt_field[0][x] = 0;
            cleared++;
            y++;   /* 重新检查本行(下移了一行) */
        }
    }
    if (cleared) {
        static const uint32_t pts[5] = { 0, 100, 300, 500, 800 };
        tt_lines += (uint32_t)cleared;
        tt_score += pts[cleared] * (tt_lines / 5 + 1);
        audio_clear();                      /* 消行得分 */
        if (tt_gravity_ms > TT_MIN_GRAVITY) {
            tt_gravity_ms -= 50;
            game_set_tick_interval(tt_gravity_ms);
        }
    }
    /* 下一块(新块到来, 重置 HOLD 机会) */
    tt_piece = tt_next;
    tt_next = bag_take();
    tt_hold_used = false;
    spawn();
}

static void hard_drop(void) {
    while (!collides(tt_px, tt_py + 1, tt_piece, tt_rot)) tt_py++;
    tt_score += 10;
    audio_select();    /* 硬降确认 */
    lock_piece();
}

void tetris_tick(uint64_t now) {
    (void)now;
    if (!tt_alive || tt_paused || !tt_started) return;
    if (!collides(tt_px, tt_py + 1, tt_piece, tt_rot)) {
        tt_py++;
    } else {
        lock_piece();
    }
}

void tetris_render(void) {
    fb_clear(false);
    /* 场地边框 */
    fb_stroke_rect(TT_OX - 1, TT_OY - 1, TT_W * TT_CELL + 2, TT_H * TT_CELL + 2, true);
    /* 已锁方块 */
    for (int y = 0; y < TT_H; y++)
        for (int x = 0; x < TT_W; x++)
            if (tt_field[y][x])
                fb_fill_rect(TT_OX + x * TT_CELL + 1, TT_OY + y * TT_CELL + 1,
                             TT_CELL - 2, TT_CELL - 2, true);
    /* 当前方块(场上部分) */
    {
        int cells[8][2], n;
        piece_cells(tt_piece, tt_rot, tt_px, tt_py, cells, &n);
        for (int k = 0; k < n; k++) {
            int x = cells[k][0], y = cells[k][1];
            if (y < 0 || y >= TT_H) continue;
            fb_fill_rect(TT_OX + x * TT_CELL + 1, TT_OY + y * TT_CELL + 1,
                         TT_CELL - 2, TT_CELL - 2, true);
        }
    }
    /* 侧栏: 干净白底 + NEXT/HOLD 预览 + 大分数 + LEVEL/LINES + 控制 */
    int px = TT_OX + TT_W * TT_CELL + 10;   /* 场地 14 列后余 ~110px */
    fb_text(px, 20, "NEXT", true);
    fb_stroke_rect(px, 30, 50, 50, true);
    {
        int cells[8][2], n;
        piece_cells(tt_next, 0, 0, 0, cells, &n);
        for (int k = 0; k < n; k++) {
            fb_fill_rect(px + 4 + cells[k][0] * 11, 34 + cells[k][1] * 11, 9, 9, true);
        }
    }
    fb_text(px + 58, 20, "HOLD", true);
    fb_stroke_rect(px + 58, 30, 50, 50, true);
    if (tt_hold) {
        int cells[8][2], n;
        piece_cells(tt_hold - 1, 0, 0, 0, cells, &n);
        for (int k = 0; k < n; k++) {
            fb_fill_rect(px + 62 + cells[k][0] * 11, 34 + cells[k][1] * 11, 9, 9, true);
        }
    }
    fb_text(px, 94, "SCORE", true);
    {
        char buf[12];
        uint32_t v = tt_score;
        unsigned i = 0;
        if (v == 0) { buf[i++] = '0'; }
        while (v && i < 10) { buf[i++] = (char)('0' + v % 10); v /= 10; }
        char rev[12];
        unsigned len = i;
        for (unsigned j = 0; j < len; j++) rev[j] = buf[len - 1 - j];
        rev[len] = 0;
        fb_text_scale2(px, 104, rev, true);
    }
    fb_text(px, 130, "LINES", true);
    {
        char buf[12];
        uint32_t v = tt_lines;
        unsigned i = 0;
        if (v == 0) { buf[i++] = '0'; }
        while (v && i < 10) { buf[i++] = (char)('0' + v % 10); v /= 10; }
        char rev[12];
        unsigned len = i;
        for (unsigned j = 0; j < len; j++) rev[j] = buf[len - 1 - j];
        rev[len] = 0;
        fb_text(px, 140, rev, true);
    }
    fb_text(px + 58, 130, "LEVEL", true);
    {
        char buf[12];
        uint32_t v = tt_lines / 5 + 1;
        unsigned i = 0;
        if (v == 0) { buf[i++] = '0'; }
        while (v && i < 10) { buf[i++] = (char)('0' + v % 10); v /= 10; }
        char rev[12];
        unsigned len = i;
        for (unsigned j = 0; j < len; j++) rev[j] = buf[len - 1 - j];
        rev[len] = 0;
        fb_text(px + 58, 140, rev, true);
    }
    fb_text(px, 164, "A/D:MOVE", true);
    fb_text(px, 174, "W:ROT S:SOFT", true);
    fb_text(px, 184, "H:HOLD", true);
    fb_text(px, 194, "OK:DROP P:PAUSE", true);
    fb_text(0, 0, "TETRIS", true);
    fb_hline(0, CCG_HUD_H - 1, CCG_W, true);
    if (tt_paused) {
        fb_text_center(9, "PAUSED [P]", true);
    } else if (!tt_started) {
        fb_text_center(9, "READY - PRESS OK", true);
    }
    if (!tt_alive) {
        fb_fill_rect(0, 0, CCG_W, CCG_HUD_H - 1, false);
        fb_text(2, 2, "GAME OVER", true);
        fb_text(CCG_W - 2 - text_width("OK/N:RETRY BACK:QUIT"), 2,
                "OK/N:RETRY BACK:QUIT", true);
        if (!tt_over_full) { tt_over_full = true; disp_force_full(); }
    }
}

void tetris_on_key(const key_event_t *ev) {
    if (ev->is_repeat) {
        /* 软降长按 */
        if (ev->key == K_DOWN && tt_started && tt_alive && !tt_paused)
            if (!collides(tt_px, tt_py + 1, tt_piece, tt_rot)) tt_py++;
        return;
    }
    if (!tt_alive) {
        if (ev->key == K_OK || (ev->key == K_CHAR && ev->ch == 'n'))
            tetris_enter();
        else if (ev->key == K_BACK || ev->key == K_QUIT)
            s_exit_request = true;
        return;
    }
    if (!tt_started) {
        if (ev->key == K_OK || ev->key == K_SPACE ||
            (ev->key == K_CHAR && ev->ch == 'w')) tt_started = true;
        return;
    }
    if (tt_paused) return;
    switch (ev->key) {
    case K_LEFT:
        if (!collides(tt_px - 1, tt_py, tt_piece, tt_rot)) tt_px--;
        break;
    case K_RIGHT:
        if (!collides(tt_px + 1, tt_py, tt_piece, tt_rot)) tt_px++;
        break;
    case K_CHAR:
        /* WASD 字母控制 */
        if (ev->ch == 'a' && !collides(tt_px - 1, tt_py, tt_piece, tt_rot)) tt_px--;
        else if (ev->ch == 'd' && !collides(tt_px + 1, tt_py, tt_piece, tt_rot)) tt_px++;
        else if (ev->ch == 'w') {
            uint8_t nr = (tt_rot + 1) & 3u;
            if (!collides(tt_px, tt_py, tt_piece, nr)) tt_rot = nr;
            else if (!collides(tt_px - 1, tt_py, tt_piece, nr)) { tt_px--; tt_rot = nr; }
            else if (!collides(tt_px + 1, tt_py, tt_piece, nr)) { tt_px++; tt_rot = nr; }
        } else if (ev->ch == 's') {
            if (!collides(tt_px, tt_py + 1, tt_piece, tt_rot)) tt_py++;
        } else if (ev->ch == 'h') {
            do_hold();
        }
        break;
    case K_UP: {
        uint8_t nr = (tt_rot + 1) & 3u;
        /* 简单踢墙: 原位/左右各一 */
        if (!collides(tt_px, tt_py, tt_piece, nr)) tt_rot = nr;
        else if (!collides(tt_px - 1, tt_py, tt_piece, nr)) { tt_px--; tt_rot = nr; }
        else if (!collides(tt_px + 1, tt_py, tt_piece, nr)) { tt_px++; tt_rot = nr; }
        break;
    }
    case K_DOWN:
        if (!collides(tt_px, tt_py + 1, tt_piece, tt_rot)) tt_py++;
        break;
    case K_OK:
    case K_SPACE:
        hard_drop();
        break;
    case K_PAUSE:
        tt_paused = !tt_paused;
        if (tt_paused) { tetris_render(); disp_full(); }
        break;
    case K_BACK: {
        pause_sel_t sel;
        if (ui_pause_run(&sel)) {
            if (sel == PAUSE_RESTART) tetris_enter();
        } else {
            s_exit_request = true;
        }
        break;
    }
    case K_QUIT: s_exit_request = true; break;
    default: break;
    }
}

void tetris_exit(void) {}
