/* 2048 — 4x4 格 64x30, 1bit 图案梯度 11 级
 * 输入驱动(滑动一步=一次快刷); 开局/结束=全刷 */
#include "game.h"
#include "../config.h"
#include "../gfx/canvas.h"
#include "../gfx/font.h"
#include "../gfx/pattern.h"
#include "../platform/display.h"
#include "../platform/input.h"
#include "../platform/audio.h"
#include "../platform/led.h"
#include "../rng.h"
#include "../platform/time.h"

#define G2_N 4
#define G2_CELL_X 64
#define G2_CELL_Y 30
#define G2_GAP 4
#define G2_OX ((CCG_W - (G2_N * G2_CELL_X + (G2_N - 1) * G2_GAP)) / 2)  /* 居中 */
#define G2_OY (CCG_HUD_H + 4)

void g2048_render(void);

static uint8_t g2_cell[G2_N][G2_N];   /* 0=空, 1..11 = 2^(n+1) */
static uint32_t g2_score;
static bool g2_over, g2_over_full;
static rng_t g2_rng;

static void spawn_tile(void) {
    int empties[16][2], n = 0;
    for (int y = 0; y < G2_N; y++)
        for (int x = 0; x < G2_N; x++)
            if (!g2_cell[y][x]) { empties[n][0] = x; empties[n][1] = y; n++; }
    if (n == 0) return;
    uint32_t i = rng_range(&g2_rng, (uint32_t)n);
    g2_cell[empties[i][1]][empties[i][0]] =
        (rng_range(&g2_rng, 10) == 0) ? 2 : 1;   /* 10% 出 4 */
}

static bool can_move(void) {
    for (int y = 0; y < G2_N; y++)
        for (int x = 0; x < G2_N; x++) {
            if (!g2_cell[y][x]) return true;
            if (x + 1 < G2_N && g2_cell[y][x] == g2_cell[y][x + 1]) return true;
            if (y + 1 < G2_N && g2_cell[y][x] == g2_cell[y + 1][x]) return true;
        }
    return false;
}

/* 沿一行/列滑动合并; dir: 0=左 1=右 2=上 3=下 */
static bool slide_line(uint8_t line[G2_N], int dir, uint32_t *gain) {
    uint8_t tmp[G2_N];
    int n = 0;
    if (dir == 1 || dir == 3) {        /* 右/下: 反向压缩 */
        for (int i = G2_N - 1; i >= 0; i--)
            if (line[i]) tmp[n++] = line[i];
        /* 合并(从高索引方向) */
        for (int i = 0; i < n - 1; i++) {
            if (tmp[i] == tmp[i + 1]) {
                tmp[i]++;
                *gain += 1u << tmp[i];
                for (int j = i + 1; j < n - 1; j++) tmp[j] = tmp[j + 1];
                n--;
            }
        }
        /* 反转: tmp[0] 变最右/最下, 再靠右/下放置 */
        for (int i = 0; i < n / 2; i++) {
            uint8_t t = tmp[i];
            tmp[i] = tmp[n - 1 - i];
            tmp[n - 1 - i] = t;
        }
        for (int i = 0; i < G2_N; i++)
            line[i] = (i >= G2_N - n) ? tmp[i - (G2_N - n)] : 0;
    } else {                            /* 左/上: 正向压缩 */
        for (int i = 0; i < G2_N; i++)
            if (line[i]) tmp[n++] = line[i];
        for (int i = 0; i < n - 1; i++) {
            if (tmp[i] == tmp[i + 1]) {
                tmp[i]++;
                *gain += 1u << tmp[i];
                for (int j = i + 1; j < n - 1; j++) tmp[j] = tmp[j + 1];
                n--;
            }
        }
        for (int i = 0; i < G2_N; i++) line[i] = (i < n) ? tmp[i] : 0;
    }
    return true;
}

static bool move_dir(int dir) {
    bool changed = false;
    uint32_t gain = 0;
    for (int i = 0; i < G2_N; i++) {
        uint8_t line[G2_N];
        for (int j = 0; j < G2_N; j++)
            line[j] = (dir <= 1) ? g2_cell[i][j] : g2_cell[j][i];
        uint8_t before[G2_N];
        for (int j = 0; j < G2_N; j++) before[j] = line[j];
        slide_line(line, dir, &gain);
        for (int j = 0; j < G2_N; j++)
            if (line[j] != before[j]) changed = true;
        for (int j = 0; j < G2_N; j++) {
            if (dir <= 1) g2_cell[i][j] = line[j];
            else g2_cell[j][i] = line[j];
        }
    }
    if (changed) {
        g2_score += gain;
        spawn_tile();
        if (!can_move()) {
            g2_over = true;
            audio_lose();
            led_fx_set(LED_FX_LOSE);
        } else if (gain > 0) {
            audio_clear();
        } else {
            audio_move();
        }
    } else {
        audio_error();
    }
    return changed;
}

void g2048_enter(void) {
    rng_seed(&g2_rng, now_ms() ^ 0x2D48);
    for (int y = 0; y < G2_N; y++)
        for (int x = 0; x < G2_N; x++) g2_cell[y][x] = 0;
    g2_score = 0;
    g2_over = false;
    g2_over_full = false;
    spawn_tile();
    spawn_tile();
    g2048_render();
    disp_full();
}

void g2048_render(void) {
    fb_clear(false);
    /* 网格背景与数值梯度 */
    for (int y = 0; y < G2_N; y++) {
        for (int x = 0; x < G2_N; x++) {
            int cx = G2_OX + x * (G2_CELL_X + G2_GAP);
            int cy = G2_OY + y * (G2_CELL_Y + G2_GAP);
            uint8_t v = g2_cell[y][x];
            if (v == 0) {
                fb_stroke_rect(cx, cy, G2_CELL_X, G2_CELL_Y, true);
                continue;
            }
            /* 图案: 1..11 -> PAT 映射; 密度>=50% 反白 */
            static const pat_id_t pats[12] = {
                PAT_EMPTY, PAT_EMPTY, PAT_CORNERS, PAT_DOT_SPARSE,
                PAT_DOT_DENSE, PAT_SLASH_S, PAT_SLASH_S2, PAT_SLASH_D,
                PAT_HLINE, PAT_GRID, PAT_CROSS, PAT_SOLID
            };
            bool heavy = (v >= 7);          /* >=128 反白 */
            bool thick = (v >= 9);          /* >=512 粗框 */
            if (heavy)
                fb_fill_rect(cx, cy, G2_CELL_X, G2_CELL_Y, true);
            fb_fill_tile(cx, cy, G2_CELL_X, G2_CELL_Y, pat_get(pats[v]));
            if (thick) fb_stroke_rect_thick(cx, cy, G2_CELL_X, G2_CELL_Y, 2, true);
            else fb_stroke_rect(cx, cy, G2_CELL_X, G2_CELL_Y, true);
            /* 数字: 轻底黑字 / 重底白字 */
            char num[8];
            uint32_t val = 1u << v;
            unsigned i = 0;
            if (val == 0) { num[i++] = '0'; }
            while (val && i < 7) { num[i++] = (char)('0' + val % 10); val /= 10; }
            char rev[8];
            unsigned len = i;
            for (unsigned j = 0; j < len; j++) rev[j] = num[len - 1 - j];
            rev[len] = 0;
            int tx = cx + (G2_CELL_X - text_width(rev)) / 2;
            int ty = cy + (G2_CELL_Y - FONT_H) / 2;
            /* 数字留白底板: 轻底白板黑字, 重底黑板白字 */
            fb_fill_rect(tx - 2, ty - 1, text_width(rev) + 4, FONT_H + 2, heavy);
            if (heavy) fb_text(tx, ty, rev, false);
            else fb_text(tx, ty, rev, true);
        }
    }
    hud_draw("2048", g2_score);
    if (g2_over) {
        fb_fill_rect(0, 0, CCG_W, CCG_HUD_H - 1, false);
        fb_text(2, 2, "GAME OVER", true);
        fb_text(CCG_W - 2 - text_width("OK/N:RETRY BACK:QUIT"), 2,
                "OK/N:RETRY BACK:QUIT", true);
        if (!g2_over_full) { g2_over_full = true; disp_force_full(); }
    }
}

void g2048_on_key(const key_event_t *ev) {
    if (ev->is_repeat) return;
    if (g2_over) {
        if (ev->key == K_OK || (ev->key == K_CHAR && ev->ch == 'n'))
            g2048_enter();
        else if (ev->key == K_BACK || ev->key == K_QUIT)
            s_exit_request = true;
        return;
    }
    switch (ev->key) {
    case K_LEFT: move_dir(0); break;
    case K_RIGHT: move_dir(1); break;
    case K_UP: move_dir(2); break;
    case K_DOWN: move_dir(3); break;
    case K_CHAR:
        if (ev->ch == 'a') move_dir(0);
        else if (ev->ch == 'd') move_dir(1);
        else if (ev->ch == 'w') move_dir(2);
        else if (ev->ch == 's') move_dir(3);
        else if (ev->ch == 'n') g2048_enter();
        break;
    case K_PAUSE: {
        pause_sel_t sel;
        if (ui_pause_run(&sel)) {
            if (sel == PAUSE_RESTART) g2048_enter();
        } else {
            s_exit_request = true;
        }
        break;
    }
    case K_BACK: {
        pause_sel_t sel;
        if (ui_pause_run(&sel)) {
            if (sel == PAUSE_RESTART) g2048_enter();
        } else {
            s_exit_request = true;
        }
        break;
    }
    case K_QUIT: s_exit_request = true; break;
    default: break;
    }
}

void g2048_tick(uint64_t now) { (void)now; }

void g2048_exit(void) {}
