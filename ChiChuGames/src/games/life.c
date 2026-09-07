/* GAME OF LIFE — Conway 生命游戏
 * 24x12 网格 10px 格(240x120 水平居中, y 从 18 起), 标准 B3/S23, 边缘视为死
 * 活细胞=实心黑格; 光标=反色 2px 边框(所有格绘制完成后最后画)
 * HUD 顶栏: 左标题黑字, 右 GEN n, 暂停时中央 PAUSED
 * 操作: 方向/WASD 移光标, OK 翻转当前格, P 暂停/继续, S 单步(先停后走),
 *       C 清空, R 随机填充(30%), N 随机新局, BACK 暂停菜单, Q 退出
 * 静态前缀 lf_; 零 malloc; 像素坐标一律 int; tick 100ms 一代
 *
 * 集成提示(help[] 最多 5 行):
 *   "GAME OF LIFE", "ARROWS: MOVE  OK: FLIP", "P: PAUSE  S: STEP  C: CLEAR",
 *   "R: RANDOM  N: NEW", "BACK: QUIT"
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

#define LF_COLS 24
#define LF_ROWS 12
#define LF_CELL 10                       /* 格边长 10px */
#define LF_OX ((CCG_W - LF_COLS * LF_CELL) / 2)   /* 28, 水平居中 */
#define LF_OY 18                         /* 游戏区 y 起点 */
#define LF_CELLS (LF_COLS * LF_ROWS)     /* 288 */

static uint8_t lf_cells[LF_CELLS];       /* 1=活 0=死 */
static uint8_t lf_next[LF_CELLS];        /* 下一代暂存 */
static int lf_cx, lf_cy;                 /* 光标 0..LF_COLS-1 / 0..LF_ROWS-1 */
static bool lf_running;                  /* false=暂停 */
static uint32_t lf_gen;                  /* 代数 */
static uint32_t lf_gens;                 /* 新局计数器(种子混合) */
static rng_t lf_rng;

void life_render(void);

/* B3/S23 单步演化; 边缘外一律视为死 */
static void lf_step(void) {
    for (int y = 0; y < LF_ROWS; y++) {
        for (int x = 0; x < LF_COLS; x++) {
            int n = 0;
            for (int dy = -1; dy <= 1; dy++) {
                int yy = y + dy;
                if (yy < 0 || yy >= LF_ROWS) continue;
                for (int dx = -1; dx <= 1; dx++) {
                    if (dx == 0 && dy == 0) continue;
                    int xx = x + dx;
                    if (xx < 0 || xx >= LF_COLS) continue;
                    if (lf_cells[yy * LF_COLS + xx]) n++;
                }
            }
            int i = y * LF_COLS + x;
            lf_next[i] = (uint8_t)(lf_cells[i] ? (n == 2 || n == 3) : (n == 3));
        }
    }
    for (int i = 0; i < LF_CELLS; i++) lf_cells[i] = lf_next[i];
    lf_gen++;
}

/* 30% 随机填充; 内部播种保证 rng 状态非 0(种子 0 会让 xorshift 恒 0 死循环) */
static void lf_random_fill(void) {
    rng_seed(&lf_rng, now_ms() ^ ((uint64_t)lf_gens * 0x9E3779B1u));
    for (int i = 0; i < LF_CELLS; i++)
        lf_cells[i] = (uint8_t)(rng_range(&lf_rng, 100u) < 30u);
}

/* 随机新局: 完成后直接开始演化 */
static void lf_new_game(void) {
    lf_gens++;
    lf_random_fill();
    lf_gen = 0;
    lf_cx = LF_COLS / 2;
    lf_cy = LF_ROWS / 2;
    lf_running = true;
}

void life_enter(void) {
    lf_new_game();
    life_render();
    disp_full();
}

void life_exit(void) {}

/* 手写数字追加(无 snprintf 依赖) */
static void lf_append_u32(char *buf, unsigned *n, uint32_t v, unsigned cap) {
    char tmp[12];
    unsigned len = 0;
    if (v == 0) tmp[len++] = '0';
    while (v && len < 11) { tmp[len++] = (char)('0' + v % 10u); v /= 10u; }
    while (len && *n < cap) buf[(*n)++] = tmp[--len];
}

void life_render(void) {
    fb_clear(false);
    /* 网格线(在格填充之下) */
    for (int i = 0; i <= LF_COLS; i++)
        fb_vline(LF_OX + i * LF_CELL, LF_OY, LF_ROWS * LF_CELL, true);
    for (int j = 0; j <= LF_ROWS; j++)
        fb_hline(LF_OX, LF_OY + j * LF_CELL, LF_COLS * LF_CELL, true);
    /* 活细胞 = 实心黑格 */
    for (int y = 0; y < LF_ROWS; y++) {
        for (int x = 0; x < LF_COLS; x++) {
            if (lf_cells[y * LF_COLS + x])
                fb_fill_rect(LF_OX + x * LF_CELL, LF_OY + y * LF_CELL,
                             LF_CELL, LF_CELL, true);
        }
    }
    /* 光标: 反色 2px 边框(黑格白边/白格黑边, 四周对称, 最后画) */
    {
        int ccx = LF_OX + lf_cx * LF_CELL;
        int ccy = LF_OY + lf_cy * LF_CELL;
        bool on = lf_cells[lf_cy * LF_COLS + lf_cx] != 0;
        fb_stroke_rect_thick(ccx - 2, ccy - 2, LF_CELL + 4, LF_CELL + 4, 2, !on);
    }
    /* HUD 顶栏: 左标题, 右 GEN n, 暂停中央 PAUSED */
    fb_text(2, 2, "GAME OF LIFE", true);
    char buf[24];
    unsigned n = 0;
    const char *p = "GEN ";
    while (*p && n < 23) buf[n++] = *p++;
    lf_append_u32(buf, &n, lf_gen, 23);
    buf[n] = 0;
    fb_text(CCG_W - 4 - text_width(buf), 2, buf, true);
    if (!lf_running) fb_text_center(2, "PAUSED", true);
    fb_hline(0, CCG_HUD_H - 1, CCG_W, true);
}

void life_tick(uint64_t now) {
    (void)now;
    if (lf_running) lf_step();
}

void life_on_key(const key_event_t *ev) {
    bool dir = ev->key == K_UP || ev->key == K_DOWN ||
               ev->key == K_LEFT || ev->key == K_RIGHT;
    if (ev->is_repeat && !dir) return;   /* 确认键/字母忽略重复, 方向可重复 */
    switch (ev->key) {
    case K_UP:
        if (lf_cy > 0) { lf_cy--; if (!ev->is_repeat) audio_move(); }
        break;
    case K_DOWN:
        if (lf_cy < LF_ROWS - 1) { lf_cy++; if (!ev->is_repeat) audio_move(); }
        break;
    case K_LEFT:
        if (lf_cx > 0) { lf_cx--; if (!ev->is_repeat) audio_move(); }
        break;
    case K_RIGHT:
        if (lf_cx < LF_COLS - 1) { lf_cx++; if (!ev->is_repeat) audio_move(); }
        break;
    case K_OK:
        lf_cells[lf_cy * LF_COLS + lf_cx] ^= 1;   /* 翻转当前格 */
        audio_select();                            /* 翻转音 */
        break;
    case K_PAUSE:
        lf_running = !lf_running;                  /* P 暂停/继续 */
        break;
    case K_CHAR:
        switch (ev->ch) {
        case 'w': if (lf_cy > 0) { lf_cy--; audio_move(); } break;
        case 'a': if (lf_cx > 0) { lf_cx--; audio_move(); } break;
        case 'd': if (lf_cx < LF_COLS - 1) { lf_cx++; audio_move(); } break;
        case 'p': lf_running = !lf_running; break;
        case 's': lf_running = false; lf_step(); break;  /* 单步: 先停后走 */
        case 'c':
            for (int i = 0; i < LF_CELLS; i++) lf_cells[i] = 0;
            lf_gen = 0;
            lf_running = false;
            break;
        case 'r':
            lf_random_fill();
            lf_gen = 0;
            break;
        case 'n': lf_new_game(); break;
        case 'q': s_exit_request = true; break;
        default: break;
        }
        break;
    case K_BACK: {
        pause_sel_t sel;
        if (ui_pause_run(&sel)) {
            if (sel == PAUSE_RESTART) lf_new_game();
            else if (sel == PAUSE_RESUME) lf_running = true;
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
