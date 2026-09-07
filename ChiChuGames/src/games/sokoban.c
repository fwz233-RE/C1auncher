/* 推箱子 — 16x10 格 12px 方形, 8 档随机关卡; 方向键移动+推箱, U 撤销 */
#include "game.h"
#include "../config.h"
#include "../gfx/canvas.h"
#include "../gfx/font.h"
#include "../gfx/pattern.h"
#include "../platform/display.h"
#include "../platform/input.h"
#include "../rng.h"
#include "../platform/time.h"
#include "../platform/audio.h"
#include "../platform/led.h"

#define SK_W 16
#define SK_H 10
#define SK_CELL 12
#define SK_OX ((CCG_W - SK_W * SK_CELL) / 2)
#define SK_OY (CCG_HUD_H + 2)
#define SK_LEVELS 8
#define SK_UNDO 256



void sokoban_render(void);
static bool level_done(void);

static uint8_t s_wall[SK_H][SK_W];
static uint8_t s_goal[SK_H][SK_W];
static uint8_t s_box[SK_H][SK_W];
static uint8_t s_initial_box[SK_H][SK_W];
static int8_t s_px, s_py;
static int8_t s_initial_px, s_initial_py;
static uint8_t s_level;
static uint32_t s_moves;
static uint32_t s_undos;

/* 撤销栈 */
static uint8_t s_ux[SK_UNDO], s_uy[SK_UNDO];
static uint8_t s_uboxx[SK_UNDO], s_uboxy[SK_UNDO];
static bool s_uboxm[SK_UNDO];      /* 该步是否推动了箱子 */
static uint16_t s_undo_n;

static bool s_over, s_over_full;
static rng_t sk_rng;

/* 随机可解关卡(反向拉动): 从目标态反向拉箱, 构造上保证可解 */
static void sk_gen_random(void) {
    if (sk_rng.s == 0) rng_seed(&sk_rng, 0x5EED2026u);   /* 防零种子死循环 */
    /* 墙: 只留边框 */
    for (int y = 0; y < SK_H; y++)
        for (int x = 0; x < SK_W; x++)
            s_wall[y][x] = (y == 0 || y == SK_H - 1 || x == 0 || x == SK_W - 1);
    for (int y = 0; y < SK_H; y++)
        for (int x = 0; x < SK_W; x++) { s_goal[y][x] = 0; s_box[y][x] = 0; }
    /* 目标布局: 难度随关卡增长 */
    int nb = 4 + ((int)s_level + 1) / 2;
    if (nb > 8) nb = 8;
    int placed = 0, guard = 0;
    while (placed < nb && guard++ < 500) {
        int x = 1 + (int)rng_range(&sk_rng, SK_W - 2);
        int y = 1 + (int)rng_range(&sk_rng, SK_H - 2);
        if (!s_goal[y][x]) { s_goal[y][x] = 1; placed++; }
    }
    if (placed == 0) s_goal[1][1] = 1;   /* 兜底: 至少一个目标 */
    /* 反向拉动起点: 箱子全在目标上 */
    for (int y = 0; y < SK_H; y++)
        for (int x = 0; x < SK_W; x++)
            s_box[y][x] = s_goal[y][x];
    /* 人随机(带 guard, 防种子异常死循环) */
    int pguard = 0;
    do {
        s_px = (int8_t)(1 + (int)rng_range(&sk_rng, SK_W - 2));
        s_py = (int8_t)(1 + (int)rng_range(&sk_rng, SK_H - 2));
        pguard++;
    } while (s_goal[s_py][s_px] && pguard < 200);
    if (pguard >= 200) {
        /* 兜底: 找任意非目标格 */
        for (int y = 1; y < SK_H - 1 && s_goal[s_py][s_px]; y++)
            for (int x = 1; x < SK_W - 1; x++)
                if (!s_goal[y][x]) { s_px = (int8_t)x; s_py = (int8_t)y; }
    }
    /* 反向拉动 */
    int pulls = 22 + (int)s_level * 2;
    static int8_t qx[160], qy[160];
    static uint8_t vis[160];
    for (int it = 0; it < pulls; it++) {
        /* BFS 找最近可拉位置: 人邻箱且背后空 */
        for (int i = 0; i < 160; i++) vis[i] = 0;
        int qh = 0, qt = 0;
        qx[qt] = s_px; qy[qt] = s_py; qt++;
        vis[s_py * SK_W + s_px] = 1;
        int found = -1, fdx = 0, fdy = 0;
        while (qh < qt) {
            int cx = qx[qh], cy = qy[qh];
            qh++;
            for (int d = 0; d < 4; d++) {
                static const int dx4[4] = { 1, -1, 0, 0 };
                static const int dy4[4] = { 0, 0, 1, -1 };
                int bx = cx + dx4[d], by = cy + dy4[d];
                int ax = cx - dx4[d], ay = cy - dy4[d];
                if (bx <= 0 || bx >= SK_W - 1 || by <= 0 || by >= SK_H - 1) continue;
                if (s_box[by][bx]) {
                    if (ax <= 0 || ax >= SK_W - 1 || ay <= 0 || ay >= SK_H - 1) continue;
                    if (!s_wall[ay][ax] && !s_box[ay][ax]) {
                        found = cy * SK_W + cx;
                        fdx = dx4[d];
                        fdy = dy4[d];
                        break;
                    }
                } else if (!s_wall[by][bx] && !vis[by * SK_W + bx]) {
                    vis[by * SK_W + bx] = 1;
                    qx[qt] = (int8_t)bx;
                    qy[qt] = (int8_t)by;
                    qt++;
                }
            }
            if (found >= 0) break;
        }
        if (found < 0) break;   /* 无法继续拉 */
        int px = found % SK_W, py = found / SK_W;
        int bx = px + fdx, by = py + fdy;
        int ax = px - fdx, ay = py - fdy;
        s_box[by][bx] = 0;
        s_box[py][px] = 1;
        s_px = (int8_t)ax;
        s_py = (int8_t)ay;
    }
    s_moves = 0;
    s_undos = 0;
    s_undo_n = 0;
    s_over = false;
    s_over_full = false;
}

static void save_initial_level(void) {
    for (int y = 0; y < SK_H; y++)
        for (int x = 0; x < SK_W; x++)
            s_initial_box[y][x] = s_box[y][x];
    s_initial_px = s_px;
    s_initial_py = s_py;
}

static void reset_level(void) {
    for (int y = 0; y < SK_H; y++)
        for (int x = 0; x < SK_W; x++)
            s_box[y][x] = s_initial_box[y][x];
    s_px = s_initial_px;
    s_py = s_initial_py;
    s_moves = 0;
    s_undos = 0;
    s_undo_n = 0;
    s_over = false;
    s_over_full = false;
}

static void load_level(void) {
    for (int attempt = 0; attempt < 32; attempt++) {
        sk_gen_random();
        if (!level_done()) break;
    }
    save_initial_level();
}

static bool level_done(void) {
    for (int y = 0; y < SK_H; y++)
        for (int x = 0; x < SK_W; x++)
            if (s_goal[y][x] && !s_box[y][x]) return false;
    return true;
}

void sokoban_enter(void) {
    rng_seed(&sk_rng, now_ms() ^ 0x50B);
    load_level();
    sokoban_render();
    disp_full();
}

static void record_undo(uint8_t player_x, uint8_t player_y,
                        uint8_t box_x, uint8_t box_y, bool pushed) {
    if (s_undo_n == SK_UNDO) {
        for (uint16_t i = 1; i < SK_UNDO; i++) {
            s_ux[i - 1] = s_ux[i];
            s_uy[i - 1] = s_uy[i];
            s_uboxx[i - 1] = s_uboxx[i];
            s_uboxy[i - 1] = s_uboxy[i];
            s_uboxm[i - 1] = s_uboxm[i];
        }
        s_undo_n--;
    }
    s_ux[s_undo_n] = player_x;
    s_uy[s_undo_n] = player_y;
    s_uboxx[s_undo_n] = box_x;
    s_uboxy[s_undo_n] = box_y;
    s_uboxm[s_undo_n] = pushed;
    s_undo_n++;
}

static void try_move(int dx, int dy) {
    int nx = s_px + dx, ny = s_py + dy;
    if (nx < 0 || nx >= SK_W || ny < 0 || ny >= SK_H) return;
    if (s_wall[ny][nx]) { audio_error(); return; }   /* 撞墙 */
    bool pushed = false;
    if (s_box[ny][nx]) {
        int bx = nx + dx, by = ny + dy;
        if (bx < 0 || bx >= SK_W || by < 0 || by >= SK_H) return;
        if (s_wall[by][bx] || s_box[by][bx]) { audio_error(); return; }   /* 箱推不动 */
        s_box[ny][nx] = 0;
        s_box[by][bx] = 1;
        if (s_goal[by][bx]) audio_clear();   /* 箱子推上目标 */
        pushed = true;
    }
    /* 记录撤销 */
    record_undo((uint8_t)s_px, (uint8_t)s_py, (uint8_t)nx, (uint8_t)ny, pushed);
    s_px = (int8_t)nx;
    s_py = (int8_t)ny;
    s_moves++;
    if (level_done()) {
        s_over = true;                     /* 本关完成 */
        audio_win();
        led_fx_set(LED_FX_WIN);
    } else {
        audio_move();                      /* 正常移动 */
    }
}

static void undo_move(void) {
    if (s_undo_n == 0 || s_over) return;
    s_undo_n--;
    if (s_uboxm[s_undo_n]) {
        int box_x = s_uboxx[s_undo_n];
        int box_y = s_uboxy[s_undo_n];
        int dx = box_x - s_ux[s_undo_n];
        int dy = box_y - s_uy[s_undo_n];

        s_box[box_y + dy][box_x + dx] = 0;
        s_box[box_y][box_x] = 1;
    }
    s_px = (int8_t)s_ux[s_undo_n];
    s_py = (int8_t)s_uy[s_undo_n];
    s_undos++;
}

void sokoban_render(void) {
    fb_clear(false);
    /* 场地图案: 地板 */
    for (int y = 0; y < SK_H; y++)
        for (int x = 0; x < SK_W; x++) {
            int cx = SK_OX + x * SK_CELL;
            int cy = SK_OY + y * SK_CELL;
            if (s_wall[y][x]) {
                fb_fill_rect(cx, cy, SK_CELL, SK_CELL, true);
            } else {
                fb_stroke_rect(cx, cy, SK_CELL, SK_CELL, false);
                if (s_goal[y][x])
                    fb_symbol(cx + (SK_CELL - FONT_W) / 2, cy + (SK_CELL - FONT_H) / 2,
                              CG_STAR, true);   /* 目标: 星号 */
            }
            if (s_box[y][x]) {
                if (s_goal[y][x]) {
                    /* 箱子在目标: 反白 */
                    fb_fill_rect(cx + 1, cy + 1, SK_CELL - 2, SK_CELL - 2, true);
                    fb_fill_tile(cx + 3, cy + 3, SK_CELL - 6, SK_CELL - 6, pat_get(PAT_GRID));
                } else {
                    fb_fill_tile(cx + 1, cy + 1, SK_CELL - 2, SK_CELL - 2, pat_get(PAT_SLASH_D));
                    fb_stroke_rect(cx + 1, cy + 1, SK_CELL - 2, SK_CELL - 2, true);
                }
            }
        }
    /* 人 */
    {
        int cx = SK_OX + s_px * SK_CELL + SK_CELL / 2;
        int cy = SK_OY + s_py * SK_CELL + SK_CELL / 2;
        for (int dy = -4; dy <= 4; dy++)
            for (int dx = -4; dx <= 4; dx++)
                if (dx * dx + dy * dy <= 16) fb_pixel(cx + dx, cy + dy, true);
        if (s_goal[s_py][s_px]) {
            fb_pixel(cx - 1, cy, false);
            fb_pixel(cx + 1, cy, false);
            fb_pixel(cx, cy - 1, false);
            fb_pixel(cx, cy + 1, false);
        }
    }
    /* 侧栏: 关卡/步数/撤销 */
    fb_text(2, 20, "LEVEL", true);
    {
        char buf[8];
        buf[0] = (char)('1' + s_level);
        buf[1] = 0;
        fb_text_scale2(2, 30, buf, true);
    }
    fb_text(2, 56, "MOVE", true);
    {
        char buf[12];
        uint32_t v = s_moves;
        unsigned i = 0;
        if (v == 0) { buf[i++] = '0'; }
        while (v && i < 10) { buf[i++] = (char)('0' + v % 10); v /= 10; }
        char rev[12];
        unsigned len = i;
        for (unsigned j = 0; j < len; j++) rev[j] = buf[len - 1 - j];
        rev[len] = 0;
        fb_text(2, 66, rev, true);
    }
    fb_text(2, 92, "UNDO", true);
    {
        char buf[12];
        uint32_t v = s_undos;
        unsigned i = 0;
        if (v == 0) { buf[i++] = '0'; }
        while (v && i < 10) { buf[i++] = (char)('0' + v % 10); v /= 10; }
        char rev[12];
        unsigned len = i;
        for (unsigned j = 0; j < len; j++) rev[j] = buf[len - 1 - j];
        rev[len] = 0;
        fb_text(2, 102, rev, true);
    }
    fb_text(2, 128, "U:UNDO", true);
    fb_text(2, 138, "R:RESET", true);
    /* HUD */
    fb_text(0, 0, "SOKOBAN", true);
    char hud[24];
    const char *lbl = "LV ";
    unsigned n = 0;
    while (lbl[n]) { hud[n] = lbl[n]; n++; }
    hud[n++] = (char)('1' + s_level);
    hud[n] = 0;
    fb_text(CCG_W - 2 - text_width(hud), 0, hud, true);
    fb_hline(0, CCG_HUD_H - 1, CCG_W, true);
    if (s_over) {
        fb_fill_rect(0, 0, CCG_W, CCG_HUD_H - 1, false);
        fb_text(2, 2, "LEVEL CLEAR!", true);
        fb_text(CCG_W - 2 - text_width("OK:NEXT BACK:QUIT"), 2,
                "OK:NEXT BACK:QUIT", true);
        if (!s_over_full) { s_over_full = true; disp_force_full(); }
    }
}

void sokoban_on_key(const key_event_t *ev) {
    if (ev->is_repeat) return;
    if (s_over) {
        if (ev->key == K_OK || (ev->key == K_CHAR && ev->ch == 'n')) {
            s_level = (uint8_t)((s_level + 1u) % SK_LEVELS);
            sokoban_enter();
        } else if (ev->key == K_BACK || ev->key == K_QUIT) {
            s_exit_request = true;
        }
        return;
    }
    switch (ev->key) {
    case K_LEFT: try_move(-1, 0); break;
    case K_RIGHT: try_move(1, 0); break;
    case K_UP: try_move(0, -1); break;
    case K_DOWN: try_move(0, 1); break;
    case K_CHAR:
        if (ev->ch == 'a') try_move(-1, 0);
        else if (ev->ch == 'd') try_move(1, 0);
        else if (ev->ch == 'w') try_move(0, -1);
        else if (ev->ch == 's') try_move(0, 1);
        else if (ev->ch == 'u') undo_move();
        else if (ev->ch == 'r') { reset_level(); }
        else if (ev->ch == 'n') {
            s_level = (uint8_t)((s_level + 1u) % SK_LEVELS);
            sokoban_enter();
        }
        break;
    case K_BACK: {
        pause_sel_t sel;
        if (ui_pause_run(&sel)) {
            if (sel == PAUSE_RESTART) reset_level();
        } else {
            s_exit_request = true;
        }
        break;
    }
    case K_QUIT: s_exit_request = true; break;
    default: break;
    }
}

void sokoban_tick(uint64_t now) { (void)now; }
void sokoban_exit(void) {}
