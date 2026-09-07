/* HASHI — 数桥 (Hashiwokakero)
 * 岛间搭桥: 桥只水平/垂直连接同排同列且之间无岛的岛; 每对最多 2 座;
 * 桥不交叉(实时检测); 每岛桥数 = 岛上数字; 全部岛连通 = WIN
 * 操作: 方向/WASD 移光标; OK 选中岛, 移到另一岛 OK 建桥(1->2->0 循环);
 *       DEL 清除(选中时清该对, 否则清光标岛全部桥); N 下一关; R 重开
 * 内置 3 个 ROM 谜题(解在 host 测试中独立验证: 度数和 + 连通)
 * 静态前缀 ha_; 像素坐标一律 int; 零 malloc; 无随机(纯 ROM 谜题)
 * 注册建议(games_table.c): G_HASHI
 *   .title = "HASHI", .tagline = "BRIDGE THE ISLANDS",
 *   .help = { "OK: SELECT ISLAND",
 *             "OK ON ANOTHER: 1-2-0 BRIDGES",
 *             "NO CROSSING, MAX 2 PER PAIR",
 *             "SAT = DEGREES MET  NET = LINKED",
 *             "N: NEXT  R: RESET", NULL },
 *   .tick_interval_ms = 0
 */
#include "game.h"
#include "../config.h"
#include "../gfx/canvas.h"
#include "../gfx/font.h"
#include "../platform/display.h"
#include "../platform/audio.h"
#include "../platform/led.h"

#define HA_COLS 6
#define HA_ROWS 5
#define HA_CELLS 30
#define HA_MAX_ISL 9
#define HA_LEVELS 3
#define HA_CS 27        /* 格 27px: 28px 时 5 行 140px 超出 136px 游戏区 */

#define HA_OX ((CCG_W - HA_COLS * HA_CS) / 2)                   /* 67 */
#define HA_OY (CCG_HUD_H + (CCG_H - CCG_HUD_H - HA_ROWS * HA_CS) / 2)  /* 16 */

typedef struct { uint8_t x, y, deg; } ha_isl_t;

/* 3 个 ROM 谜题: (列, 行, 所需桥数); 最小间距 2 格(保证桥可见) */
static const ha_isl_t ha_puzzles[HA_LEVELS][HA_MAX_ISL] = {
    /* L1 易: 3x3 网格, 全单桥 */
    { {0,0,2},{2,0,3},{4,0,2},{0,2,3},{2,2,4},{4,2,3},{0,4,2},{2,4,3},{4,4,2} },
    /* L2 中: 含双桥 */
    { {0,0,4},{2,0,3},{4,0,2},{0,2,3},{2,2,4},{4,2,5},{0,4,1},{2,4,2},{4,4,2} },
    /* L3 中上: 双桥 + 跨列 */
    { {0,0,1},{3,0,4},{5,0,1},{0,2,2},{3,2,5},{5,2,2},{0,4,2},{3,4,3},{5,4,2} },
};

static uint8_t ha_n;                  /* 岛数 */
static ha_isl_t ha_islands[HA_MAX_ISL];
static int8_t ha_island_of[HA_CELLS]; /* 格 -> 岛索引, -1 无 */
static uint8_t ha_pc[HA_MAX_ISL][HA_MAX_ISL];  /* 桥数: 岛对(i<j) 0-2 */
static uint8_t ha_chh[HA_CELLS];      /* 横桥穿过此格的座数(交叉检测) */
static uint8_t ha_cvh[HA_CELLS];      /* 竖桥穿过此格的座数(交叉检测) */
static int ha_cur;                    /* 光标格 */
static int ha_sel;                    /* 选中岛索引, -1 无 */
static uint8_t ha_level;
static bool ha_over;
static bool ha_over_full;

void hashi_render(void);

/* 格 -> 岛索引 */
static int ha_island_cell(int c) {
    return (c >= 0 && c < HA_CELLS) ? (int)ha_island_of[c] : -1;
}

/* 两岛格同排/同列且之间无岛(且至少留 1 格空隙给桥) */
static bool ha_clear_line(int a, int b) {
    int ax = a % HA_COLS, ay = a / HA_COLS;
    int bx = b % HA_COLS, by = b / HA_COLS;
    if (ax == bx) {
        int y0 = ay < by ? ay : by, y1 = ay < by ? by : ay;
        if (y1 - y0 < 2) return false;
        for (int y = y0 + 1; y < y1; y++)
            if (ha_island_of[y * HA_COLS + ax] >= 0) return false;
        return true;
    }
    if (ay == by) {
        int x0 = ax < bx ? ax : bx, x1 = ax < bx ? bx : ax;
        if (x1 - x0 < 2) return false;
        for (int x = x0 + 1; x < x1; x++)
            if (ha_island_of[ay * HA_COLS + x] >= 0) return false;
        return true;
    }
    return false;
}

/* 岛 i-j 当前桥数(不同线/被挡返回 0) */
static int ha_pair_count(int i, int j) {
    if (i < 0 || j < 0 || i >= (int)ha_n || j >= (int)ha_n || i == j) return 0;
    int a = ha_islands[i].y * HA_COLS + ha_islands[i].x;
    int b = ha_islands[j].y * HA_COLS + ha_islands[j].x;
    if (!ha_clear_line(a, b)) return 0;
    int lo = i < j ? i : j, hi = i < j ? j : i;
    return (int)ha_pc[lo][hi];
}

/* 能否在岛 i-j 间再加一座桥(同线、无岛挡、不穿越已有桥、每对 <=2) */
static bool ha_can_build(int i, int j) {
    if (i < 0 || j < 0 || i >= (int)ha_n || j >= (int)ha_n || i == j) return false;
    if (ha_pair_count(i, j) >= 2) return false;
    int ax = ha_islands[i].x, ay = ha_islands[i].y;
    int bx = ha_islands[j].x, by = ha_islands[j].y;
    if (ax == bx) {
        int y0 = ay < by ? ay : by, y1 = ay < by ? by : ay;
        if (y1 - y0 < 2) return false;
        for (int y = y0 + 1; y < y1; y++)
            if (ha_island_of[y * HA_COLS + ax] >= 0) return false;  /* 有岛挡 */
        for (int y = y0 + 1; y < y1; y++)
            if (ha_chh[y * HA_COLS + ax]) return false;             /* 横桥竖穿 */
        return true;
    }
    if (ay == by) {
        int x0 = ax < bx ? ax : bx, x1 = ax < bx ? bx : ax;
        if (x1 - x0 < 2) return false;
        for (int x = x0 + 1; x < x1; x++)
            if (ha_island_of[ay * HA_COLS + x] >= 0) return false;
        for (int x = x0 + 1; x < x1; x++)
            if (ha_cvh[ay * HA_COLS + x]) return false;             /* 竖桥横穿 */
        return true;
    }
    return false;
}

/* 加一座桥(含覆盖标记); 失败返回 false */
static bool ha_build_pair(int i, int j) {
    if (!ha_can_build(i, j)) return false;
    int lo = i < j ? i : j, hi = i < j ? j : i;
    ha_pc[lo][hi]++;
    int ax = ha_islands[i].x, ay = ha_islands[i].y;
    int bx = ha_islands[j].x, by = ha_islands[j].y;
    if (ay == by) {
        int x0 = ax < bx ? ax : bx, x1 = ax < bx ? bx : ax;
        for (int x = x0 + 1; x < x1; x++) ha_chh[ay * HA_COLS + x]++;
    } else {
        int y0 = ay < by ? ay : by, y1 = ay < by ? by : ay;
        for (int y = y0 + 1; y < y1; y++) ha_cvh[y * HA_COLS + ax]++;
    }
    return true;
}

/* 拆一座桥 */
static void ha_remove_pair(int i, int j) {
    if (ha_pair_count(i, j) == 0) return;
    int lo = i < j ? i : j, hi = i < j ? j : i;
    ha_pc[lo][hi]--;
    int ax = ha_islands[i].x, ay = ha_islands[i].y;
    int bx = ha_islands[j].x, by = ha_islands[j].y;
    if (ay == by) {
        int x0 = ax < bx ? ax : bx, x1 = ax < bx ? bx : ax;
        for (int x = x0 + 1; x < x1; x++) ha_chh[ay * HA_COLS + x]--;
    } else {
        int y0 = ay < by ? ay : by, y1 = ay < by ? by : ay;
        for (int y = y0 + 1; y < y1; y++) ha_cvh[y * HA_COLS + ax]--;
    }
}

/* 岛 i 当前桥数 */
static int ha_incident(int i) {
    int n = 0;
    for (int j = 0; j < (int)ha_n; j++) n += ha_pair_count(i, j);
    return n;
}

/* 从 0 号岛出发可达的岛数(BFS) */
static int ha_net_count(void) {
    uint8_t seen[HA_MAX_ISL];
    uint8_t q[HA_MAX_ISL];
    for (int i = 0; i < HA_MAX_ISL; i++) seen[i] = 0;
    int qh = 0, qt = 0, cnt = 1;
    seen[0] = 1;
    q[qt++] = 0;
    while (qh < qt) {
        int i = q[qh++];
        for (int j = 0; j < (int)ha_n; j++)
            if (!seen[j] && ha_pair_count(i, j) > 0) {
                seen[j] = 1;
                q[qt++] = (uint8_t)j;
                cnt++;
            }
    }
    return cnt;
}

/* 度数已满足的岛数 */
static int ha_satisfied(void) {
    int n = 0;
    for (int i = 0; i < (int)ha_n; i++)
        if (ha_incident(i) == (int)ha_islands[i].deg) n++;
    return n;
}

/* 全岛度数满足 且 一个网络 -> WIN */
static void ha_check_win(void) {
    if (ha_over) return;
    if (ha_net_count() != (int)ha_n) return;
    for (int i = 0; i < (int)ha_n; i++)
        if (ha_incident(i) != (int)ha_islands[i].deg) return;
    ha_over = true;
    audio_win();
    led_fx_set(LED_FX_WIN);
}

/* 开局: 装载谜题, 复位状态 */
static void ha_start(uint8_t level) {
    ha_level = level;
    ha_n = HA_MAX_ISL;
    for (int i = 0; i < HA_MAX_ISL; i++) ha_islands[i] = ha_puzzles[level][i];
    for (int c = 0; c < HA_CELLS; c++) {
        ha_chh[c] = 0; ha_cvh[c] = 0;
        ha_island_of[c] = -1;
    }
    for (int i = 0; i < HA_MAX_ISL; i++)
        for (int j = 0; j < HA_MAX_ISL; j++)
            ha_pc[i][j] = 0;
    for (int i = 0; i < (int)ha_n; i++)
        ha_island_of[ha_islands[i].y * HA_COLS + ha_islands[i].x] = (int8_t)i;
    ha_cur = ha_islands[0].y * HA_COLS + ha_islands[0].x;
    ha_sel = -1;
    ha_over = false;
    ha_over_full = false;
}

/* OK: 选岛 / 建桥(1->2->0 循环) / 取消选中 */
static void ha_ok(void) {
    int ci = ha_island_cell(ha_cur);
    if (ha_sel < 0) {
        if (ci >= 0) {
            ha_sel = ci;
            audio_select();
        }
        return;
    }
    if (ci < 0) { ha_sel = -1; return; }     /* 空格 OK -> 取消选中 */
    if (ci == ha_sel) { ha_sel = -1; return; }
    if (ha_can_build(ha_sel, ci)) {
        ha_build_pair(ha_sel, ci);
        audio_clear();
    } else if (ha_pair_count(ha_sel, ci) > 0) {
        /* 已满 2 座: 2 -> 0 循环 */
        while (ha_pair_count(ha_sel, ci) > 0) ha_remove_pair(ha_sel, ci);
    } else {
        ha_sel = ci;                          /* 视线被挡/交叉: 改选此岛 */
    }
    ha_check_win();
}

/* DEL: 选中且光标在另一岛 -> 清该对; 否则清光标岛全部桥 */
static void ha_del(void) {
    if (ha_over) return;
    int ci = ha_island_cell(ha_cur);
    if (ci < 0) return;
    if (ha_sel >= 0 && ha_sel != ci && ha_pair_count(ha_sel, ci) > 0) {
        while (ha_pair_count(ha_sel, ci) > 0) ha_remove_pair(ha_sel, ci);
        ha_check_win();
        return;
    }
    for (int i = 0; i < (int)ha_n; i++)
        while (ha_pair_count(ci, i) > 0) ha_remove_pair(ci, i);
    ha_check_win();
}

/* 光标移动(方向键/WASD) */
static void ha_move(int dx, int dy) {
    int x = ha_cur % HA_COLS, y = ha_cur / HA_COLS;
    int nx = x + dx, ny = y + dy;
    if (nx < 0 || nx >= HA_COLS || ny < 0 || ny >= HA_ROWS) return;
    ha_cur = ny * HA_COLS + nx;
}

void hashi_enter(void) {
    ha_start(0);
    hashi_render();
    disp_full();
}

void hashi_exit(void) {}

void hashi_tick(uint64_t now) { (void)now; }

/* 手写数字追加(无 snprintf 依赖) */
static void ha_append_u32(char *buf, unsigned *n, uint32_t v, unsigned cap) {
    char tmp[12];
    unsigned len = 0;
    if (v == 0) tmp[len++] = '0';
    while (v && len < 11) { tmp[len++] = (char)('0' + v % 10u); v /= 10u; }
    while (len && *n < cap) buf[(*n)++] = tmp[--len];
}

/* 画横桥(x0,x1 为岛列, 共行 y; n 座) */
static void ha_draw_h(int x0, int y, int x1, int n) {
    int cy = HA_OY + y * HA_CS + HA_CS / 2 - 1;
    int px0 = HA_OX + x0 * HA_CS + HA_CS;
    int len = (x1 - x0 - 1) * HA_CS;
    if (n == 1) {
        fb_fill_rect(px0, cy - 1, len, 2, true);
    } else {
        fb_fill_rect(px0, cy - 4, len, 2, true);
        fb_fill_rect(px0, cy + 2, len, 2, true);
    }
}

/* 画竖桥(y0,y1 为岛行, 共列 x; n 座) */
static void ha_draw_v(int x, int y0, int y1, int n) {
    int cx = HA_OX + x * HA_CS + HA_CS / 2 - 1;
    int py0 = HA_OY + y0 * HA_CS + HA_CS;
    int len = (y1 - y0 - 1) * HA_CS;
    if (n == 1) {
        fb_fill_rect(cx - 1, py0, 2, len, true);
    } else {
        fb_fill_rect(cx - 4, py0, 2, len, true);
        fb_fill_rect(cx + 2, py0, 2, len, true);
    }
}

void hashi_render(void) {
    fb_clear(false);
    /* 桥: 先画, 岛方块压住端头 */
    for (int i = 0; i < (int)ha_n; i++) {
        for (int j = i + 1; j < (int)ha_n; j++) {
            int n = (int)ha_pc[i][j];
            if (!n) continue;
            int ax = ha_islands[i].x, ay = ha_islands[i].y;
            int bx = ha_islands[j].x, by = ha_islands[j].y;
            if (ay == by) {
                if (ax < bx) ha_draw_h(ax, ay, bx, n);
                else ha_draw_h(bx, ay, ax, n);
            } else {
                if (ay < by) ha_draw_v(ax, ay, by, n);
                else ha_draw_v(ax, by, ay, n);
            }
        }
    }
    /* 岛: 黑方块 + 2x 白数字 */
    for (int i = 0; i < (int)ha_n; i++) {
        int cx = HA_OX + (int)ha_islands[i].x * HA_CS;
        int cy = HA_OY + (int)ha_islands[i].y * HA_CS;
        fb_fill_rect(cx, cy, HA_CS, HA_CS, true);
        char d[2] = { (char)('0' + ha_islands[i].deg), 0 };
        fb_text_scale2(cx + 8, cy + 6, d, false);
    }
    /* 选中岛: 内白圈(画在岛之后) */
    if (ha_sel >= 0) {
        int cx = HA_OX + (int)ha_islands[ha_sel].x * HA_CS;
        int cy = HA_OY + (int)ha_islands[ha_sel].y * HA_CS;
        fb_stroke_rect(cx + 2, cy + 2, HA_CS - 4, HA_CS - 4, false);
    }
    /* 光标(最后画, 四周对称: 黑格白边/白格黑边; 画在格内不侵入桥缝) */
    {
        int cx = HA_OX + (ha_cur % HA_COLS) * HA_CS;
        int cy = HA_OY + (ha_cur / HA_COLS) * HA_CS;
        bool dark = ha_island_cell(ha_cur) >= 0;
        fb_stroke_rect_thick(cx, cy, HA_CS, HA_CS, 2, !dark);
    }
    /* HUD 顶栏 */
    if (ha_over) {
        fb_fill_rect(0, 0, CCG_W, CCG_HUD_H - 1, false);
        fb_text(2, 2, "SOLVED!", true);
        fb_text(CCG_W - 2 - text_width("OK/N:RETRY BACK:QUIT"), 2,
                "OK/N:RETRY BACK:QUIT", true);
        if (!ha_over_full) { ha_over_full = true; disp_force_full(); }
    } else {
        fb_text(0, 0, "HASHI", true);
        char buf[28];
        unsigned n = 0;
        buf[n++] = 'L';
        buf[n++] = (char)('1' + ha_level);
        const char *p = " NET ";
        while (*p && n < 27) buf[n++] = *p++;
        ha_append_u32(buf, &n, (uint32_t)ha_net_count(), 27);
        buf[n++] = '/';
        ha_append_u32(buf, &n, (uint32_t)ha_n, 27);
        p = " SAT ";
        while (*p && n < 27) buf[n++] = *p++;
        ha_append_u32(buf, &n, (uint32_t)ha_satisfied(), 27);
        buf[n++] = '/';
        ha_append_u32(buf, &n, (uint32_t)ha_n, 27);
        buf[n] = 0;
        fb_text(CCG_W - 4 - text_width(buf), 0, buf, true);
        fb_hline(0, CCG_HUD_H - 1, CCG_W, true);
    }
}

void hashi_on_key(const key_event_t *ev) {
    bool dir = ev->key == K_UP || ev->key == K_DOWN ||
               ev->key == K_LEFT || ev->key == K_RIGHT;
    if (ev->is_repeat && !dir) return;   /* 确认键/字母/DEL 忽略重复 */
    if (ha_over) {
        if (ev->key == K_OK || (ev->key == K_CHAR && ev->ch == 'n')) {
            ha_start(ha_level);
            hashi_render();
            disp_full();
        } else if (ev->key == K_BACK || ev->key == K_QUIT) {
            s_exit_request = true;
        }
        return;
    }
    switch (ev->key) {
    case K_UP: ha_move(0, -1); audio_tick(); break;
    case K_DOWN: ha_move(0, 1); audio_tick(); break;
    case K_LEFT: ha_move(-1, 0); audio_tick(); break;
    case K_RIGHT: ha_move(1, 0); audio_tick(); break;
    case K_CHAR:
        switch (ev->ch) {
        case 'w': ha_move(0, -1); audio_tick(); break;
        case 's': ha_move(0, 1); audio_tick(); break;
        case 'a': ha_move(-1, 0); audio_tick(); break;
        case 'd': ha_move(1, 0); audio_tick(); break;
        case 'n': audio_select(); ha_start((uint8_t)((ha_level + 1) % HA_LEVELS)); break;
        case 'r': audio_select(); ha_start(ha_level); break;
        default: break;
        }
        break;
    case K_OK: ha_ok(); break;
    case K_DEL: ha_del(); break;
    case K_BACK:
    case K_PAUSE: {
        pause_sel_t sel = PAUSE_RESUME;
        if (ui_pause_run(&sel)) {
            if (sel == PAUSE_RESTART) ha_start(ha_level);
        } else {
            s_exit_request = true;
        }
        break;
    }
    case K_QUIT: s_exit_request = true; break;
    default: break;
    }
}
