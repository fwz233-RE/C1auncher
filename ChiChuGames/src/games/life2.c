/* LIFE VARIANTS — 康威生命游戏 3 规则变体
 * 24x8 网格 12px 格(288x96 水平居中, y 从 CCG_HUD_H 起), 边缘外视为死
 * 规则: 0 STANDARD(B3/S23) 1 HIGH LIFE(B36/S23) 2 SEEDS(B2/S), R 循环切换
 * 活细胞=实心黑格; 光标=反色 2px 边框(最后画)
 * 操作: 方向/WASD 移光标, SPACE 编辑活/死, OK/P 运行/暂停, N 随机新局,
 *       C 清空, R 切换规则, BACK 暂停菜单, Q 退出
 * 统计: HUD 右 GEN n, 底部 ALIVE n + 规则选择器 + 记号/PAUSED
 * 静态前缀 l2_; 零 malloc; 像素坐标一律 int; tick 400ms 一代
 *
 * 集成提示(help[] 最多 5 行):
 *   "LIFE VARIANTS", "3 RULES: STANDARD/HIGH/SEEDS", "ARROWS/WASD: MOVE SPACE: EDIT",
 *   "OK/P: RUN/STOP R: RULE N: NEW", "C: CLEAR BACK: QUIT"
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

#define L2_COLS 24
#define L2_ROWS 8
#define L2_CELL 12                        /* 格边长 12px */
#define L2_OX ((CCG_W - L2_COLS * L2_CELL) / 2)   /* 4, 水平居中 */
#define L2_OY CCG_HUD_H                   /* 16, 顶栏之下 */
#define L2_CELLS (L2_COLS * L2_ROWS)      /* 192 */
#define L2_RULES 3
#define L2_BY (L2_OY + L2_ROWS * L2_CELL) /* 112: 底部信息区起点 */

/* 规则: birth/survive 位图, 位 n = 邻居数 n 时出生/存活 */
typedef struct {
    const char *name;
    const char *code;
    uint16_t birth;
    uint16_t survive;
} l2_rule_t;

static const l2_rule_t l2_rules[L2_RULES] = {
    { "STANDARD",  "B3/S23",  0x0008u, 0x000Cu },  /* bit3 / bits2,3 */
    { "HIGH LIFE", "B36/S23", 0x0048u, 0x000Cu },  /* bits3,6 / bits2,3 */
    { "SEEDS",     "B2/S",    0x0004u, 0x0000u },  /* bit2 / 无存活 */
};

static uint8_t l2_cells[L2_CELLS];       /* 1=活 0=死 */
static uint8_t l2_next[L2_CELLS];        /* 下一代暂存 */
static int l2_cx, l2_cy;                 /* 光标 0..L2_COLS-1 / 0..L2_ROWS-1 */
static bool l2_running;                  /* false=暂停 */
static uint32_t l2_gen;                  /* 世代数 */
static uint32_t l2_gens;                 /* 新局计数(种子混合) */
static uint32_t l2_alive;                /* 活细胞计数 */
static int l2_rule;                      /* 0..2 */
static rng_t l2_rng;

void life2_render(void);

static uint32_t l2_count_alive(void) {
    uint32_t n = 0;
    for (int i = 0; i < L2_CELLS; i++)
        if (l2_cells[i]) n++;
    return n;
}

/* 当前规则单步演化; 边缘外一律视为死; 同步维护 l2_alive */
static void l2_step(void) {
    const l2_rule_t *r = &l2_rules[l2_rule];
    uint32_t alive = 0;
    for (int y = 0; y < L2_ROWS; y++) {
        for (int x = 0; x < L2_COLS; x++) {
            int n = 0;
            for (int dy = -1; dy <= 1; dy++) {
                int yy = y + dy;
                if (yy < 0 || yy >= L2_ROWS) continue;
                for (int dx = -1; dx <= 1; dx++) {
                    if (dx == 0 && dy == 0) continue;
                    int xx = x + dx;
                    if (xx < 0 || xx >= L2_COLS) continue;
                    if (l2_cells[yy * L2_COLS + xx]) n++;
                }
            }
            int i = y * L2_COLS + x;
            uint16_t m = (uint16_t)(1u << n);
            uint8_t v = (uint8_t)(l2_cells[i]
                ? ((r->survive & m) ? 1u : 0u)
                : ((r->birth & m) ? 1u : 0u));
            l2_next[i] = v;
            alive += v;
        }
    }
    for (int i = 0; i < L2_CELLS; i++) l2_cells[i] = l2_next[i];
    l2_alive = alive;
    l2_gen++;
}

/* 30% 随机填充; 内部播种(种子 0 会让 xorshift 恒 0, rng_seed 已防);
 * do-while 带 guard: 最多 8 次保证非空, 兜底强制 1 活格 */
static void l2_random_fill(void) {
    rng_seed(&l2_rng, now_ms() ^ ((uint64_t)l2_gens * 0x9E3779B1u));
    unsigned tries = 0;
    do {
        for (int i = 0; i < L2_CELLS; i++)
            l2_cells[i] = (uint8_t)(rng_range(&l2_rng, 100u) < 30u);
        l2_alive = l2_count_alive();
        tries++;
    } while (l2_alive == 0 && tries < 8u);   /* guard: 避免死循环 */
    if (l2_alive == 0) {
        l2_cells[(int)rng_range(&l2_rng, (uint32_t)L2_CELLS)] = 1;
        l2_alive = 1;
    }
}

/* 随机新局: 完成后直接运行 */
static void l2_new_game(void) {
    l2_gens++;
    l2_random_fill();
    l2_gen = 0;
    l2_cx = L2_COLS / 2;
    l2_cy = L2_ROWS / 2;
    l2_running = true;
}

void life2_enter(void) {
    l2_new_game();
    life2_render();
    disp_full();
}

void life2_exit(void) {}

/* 手写数字追加(无 snprintf 依赖) */
static void l2_append_u32(char *buf, unsigned *n, uint32_t v, unsigned cap) {
    char tmp[12];
    unsigned len = 0;
    if (v == 0) tmp[len++] = '0';
    while (v && len < 11) { tmp[len++] = (char)('0' + v % 10u); v /= 10u; }
    while (len && *n < cap) buf[(*n)++] = tmp[--len];
}

void life2_render(void) {
    fb_clear(false);
    /* 网格线(在格填充之下) */
    for (int i = 0; i <= L2_COLS; i++)
        fb_vline(L2_OX + i * L2_CELL, L2_OY, L2_ROWS * L2_CELL, true);
    for (int j = 0; j <= L2_ROWS; j++)
        fb_hline(L2_OX, L2_OY + j * L2_CELL, L2_COLS * L2_CELL, true);
    /* 活细胞 = 实心黑格 */
    for (int y = 0; y < L2_ROWS; y++) {
        for (int x = 0; x < L2_COLS; x++) {
            if (l2_cells[y * L2_COLS + x])
                fb_fill_rect(L2_OX + x * L2_CELL, L2_OY + y * L2_CELL,
                             L2_CELL, L2_CELL, true);
        }
    }
    /* 光标: 反色 2px 边框(黑格白边/白格黑边, 四周对称, 最后画) */
    {
        int ccx = L2_OX + l2_cx * L2_CELL;
        int ccy = L2_OY + l2_cy * L2_CELL;
        bool on = l2_cells[l2_cy * L2_COLS + l2_cx] != 0;
        fb_stroke_rect_thick(ccx - 2, ccy - 2, L2_CELL + 4, L2_CELL + 4, 2, !on);
    }
    /* HUD 顶栏: 左标题黑字, 右 GEN n */
    fb_text(2, 2, "LIFE VARIANTS", true);
    {
        char buf[24];
        unsigned n = 0;
        const char *p = "GEN ";
        while (*p && n < 23) buf[n++] = *p++;
        l2_append_u32(buf, &n, l2_gen, 23);
        buf[n] = 0;
        fb_text(CCG_W - 4 - text_width(buf), 2, buf, true);
    }
    fb_hline(0, CCG_HUD_H - 1, CCG_W, true);
    /* 底部信息区(网格下方): 规则选择器 / ALIVE / 记号或 PAUSED */
    {
        char rbuf[48];
        unsigned n = 0;
        for (int i = 0; i < L2_RULES && n < 46; i++) {
            const char *nm = l2_rules[i].name;
            if (i > 0) { rbuf[n++] = ' '; rbuf[n++] = ' '; }
            if (i == l2_rule) { rbuf[n++] = '['; }
            while (*nm && n < 45) rbuf[n++] = *nm++;
            if (i == l2_rule) { rbuf[n++] = ']'; }
        }
        rbuf[n] = 0;
        fb_text_center(L2_BY + 4, rbuf, true);
    }
    {
        char buf[24];
        unsigned n = 0;
        const char *p = "ALIVE ";
        while (*p && n < 23) buf[n++] = *p++;
        l2_append_u32(buf, &n, l2_alive, 23);
        buf[n] = 0;
        fb_text_center(L2_BY + 12, buf, true);
    }
    fb_text_center(L2_BY + 20, l2_running ? l2_rules[l2_rule].code : "PAUSED", true);
}

void life2_tick(uint64_t now) {
    (void)now;
    if (l2_running) l2_step();
}

void life2_on_key(const key_event_t *ev) {
    bool dir = ev->key == K_UP || ev->key == K_DOWN ||
               ev->key == K_LEFT || ev->key == K_RIGHT;
    if (ev->is_repeat && !dir) return;   /* 确认键/字母/空格忽略重复, 方向可重复 */
    switch (ev->key) {
    case K_UP:
        if (l2_cy > 0) { l2_cy--; if (!ev->is_repeat) audio_move(); }
        break;
    case K_DOWN:
        if (l2_cy < L2_ROWS - 1) { l2_cy++; if (!ev->is_repeat) audio_move(); }
        break;
    case K_LEFT:
        if (l2_cx > 0) { l2_cx--; if (!ev->is_repeat) audio_move(); }
        break;
    case K_RIGHT:
        if (l2_cx < L2_COLS - 1) { l2_cx++; if (!ev->is_repeat) audio_move(); }
        break;
    case K_OK:
    case K_PAUSE:
        l2_running = !l2_running;        /* OK/P 运行/暂停 */
        break;
    case K_SPACE: {                      /* 编辑当前格 */
        int i = l2_cy * L2_COLS + l2_cx;
        l2_cells[i] ^= 1u;
        if (l2_cells[i]) l2_alive++; else l2_alive--;
        audio_select();                  /* 编辑音 */
        break;
    }
    case K_CHAR:
        switch (ev->ch) {
        case 'w': if (l2_cy > 0) { l2_cy--; audio_move(); } break;
        case 'a': if (l2_cx > 0) { l2_cx--; audio_move(); } break;
        case 's': if (l2_cy < L2_ROWS - 1) { l2_cy++; audio_move(); } break;
        case 'd': if (l2_cx < L2_COLS - 1) { l2_cx++; audio_move(); } break;
        case 'p': l2_running = !l2_running; break;
        case 'n': l2_new_game(); break;
        case 'c':
            for (int i = 0; i < L2_CELLS; i++) l2_cells[i] = 0;
            l2_alive = 0;
            l2_gen = 0;
            l2_running = false;
            break;
        case 'r':                        /* 切换规则, 从本代重新计数 */
            l2_rule = (l2_rule + 1) % L2_RULES;
            l2_gen = 0;
            break;
        case 'q': s_exit_request = true; break;
        default: break;
        }
        break;
    case K_BACK: {
        pause_sel_t sel;
        if (ui_pause_run(&sel)) {
            if (sel == PAUSE_RESTART) l2_new_game();
            else if (sel == PAUSE_RESUME) l2_running = true;
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
