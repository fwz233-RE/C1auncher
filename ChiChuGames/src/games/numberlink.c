/* NUMBERLINK — 一笔画连线谜题 (6x8 棋盘)
 * 6 对同数字端点, 用不交叉不重叠的路径连接每对数字(可选全填)
 * 操作: 方向/WASD 移光标; OK 在端点上开始画线, 移动延伸路径,
 *       走到另一端自动连接; OK 中途结束(路径作废); DELETE 撤销最后一段
 *       (画线中退格 / 空闲时撤销上一条完成的路径)
 * 内置 3 个 ROM 谜题(设计解 + 回溯求解器独立验证有解)
 * 胜利: 6 对全部连通 → HUD SOLVED!, 满格显示 FULL
 * 静态前缀 nl_; 像素坐标一律 int; 零 malloc; 无随机(rng 不需要播种)
 */
#include "game.h"
#include "../config.h"
#include "../gfx/canvas.h"
#include "../gfx/font.h"
#include "../gfx/pattern.h"
#include "../platform/display.h"
#include "../platform/input.h"
#include "../platform/audio.h"
#include "../platform/led.h"

#define NL_COLS 8
#define NL_ROWS 6
#define NL_CELLS 48
#define NL_PAIRS 6
#define NL_LEVELS 3
#define NL_CELL_SZ 18                                   /* 格 18px */
#define NL_OX ((CCG_W - NL_COLS * NL_CELL_SZ) / 2)      /* 76: 水平居中 */
#define NL_OY (CCG_HUD_H + (CCG_H - CCG_HUD_H - NL_ROWS * NL_CELL_SZ) / 2)  /* 30 */

/* 每关 6 对端点, 每对 2 格(行*8+列), 顺序: 起端, 终端 */
static const uint8_t nl_puzzles[NL_LEVELS][NL_PAIRS * 2] = {
    /* L1 易: 条状分隔区 */
    { 0, 42, 7, 45, 8, 41, 3, 36, 14, 47, 11, 44 },
    /* L2 中: 螺旋 */
    { 0, 7, 15, 44, 43, 8, 9, 30, 38, 17, 18, 21 },
    /* L3 中上: 中区交错 */
    { 0, 40, 6, 46, 2, 35, 4, 37, 10, 43, 12, 45 },
};

/* 每对路径的 8x8 图案(端点为实心黑格+白字, 路径用图案区分) */
static const pat_id_t nl_pair_pat[NL_PAIRS + 1] = {
    PAT_EMPTY, PAT_CROSS, PAT_SLASH_D, PAT_GRID,
    PAT_DOT_DENSE, PAT_HLINE, PAT_DOT_SPARSE
};

static uint8_t nl_grid[NL_CELLS];   /* 0 空, 1-6 归属配对(端点+路径) */
static uint8_t nl_fixed[NL_CELLS];  /* 1 = 端点格 */
static uint8_t nl_cur;              /* 光标格 */
static uint8_t nl_draw_pair;        /* 画线中的配对 */
static uint8_t nl_path[NL_CELLS];   /* 当前路径格序列(0 为起点) */
static uint8_t nl_plen;             /* 路径长度 */
static uint8_t nl_last_pair;        /* 最近完成的配对(DEL 撤销用) */
static uint8_t nl_level;            /* 0..2 */
static bool nl_drawing;
static bool nl_over;
static bool nl_over_full;           /* 结束全刷只做一次 */

void numberlink_render(void);

/* 清掉配对 p 的所有非端点格 */
static void nl_clear_pair(int p) {
    for (int i = 0; i < NL_CELLS; i++)
        if (nl_grid[i] == p && !nl_fixed[i]) nl_grid[i] = 0;
}

/* 开局: 装载谜题, 复位状态 */
static void nl_start(uint8_t level) {
    nl_level = level;
    for (int i = 0; i < NL_CELLS; i++) { nl_grid[i] = 0; nl_fixed[i] = 0; }
    for (int i = 0; i < NL_PAIRS * 2; i++) {
        uint8_t c = nl_puzzles[level][i];
        nl_fixed[c] = 1;
        nl_grid[c] = (uint8_t)(i / 2 + 1);
    }
    nl_cur = 0;                 /* 光标落在 1 号起点 */
    nl_drawing = false;
    nl_plen = 0;
    nl_last_pair = 0;
    nl_over = false;
    nl_over_full = false;
}

/* 配对 p 两端点是否被路径连通(BFS 沿 nl_grid==p 的格) */
static bool nl_pair_connected(int p) {
    int a = -1, b = -1;
    for (int i = 0; i < NL_CELLS; i++)
        if (nl_fixed[i] && nl_grid[i] == p) {
            if (a < 0) a = i; else b = i;
        }
    if (a < 0 || b < 0) return false;
    uint8_t seen[NL_CELLS];
    uint8_t q[NL_CELLS];
    for (int i = 0; i < NL_CELLS; i++) seen[i] = 0;
    int qh = 0, qt = 0;
    seen[a] = 1;
    q[qt++] = (uint8_t)a;
    while (qh < qt) {
        int c = q[qh++];
        if (c == b) return true;
        int x = c % NL_COLS, y = c / NL_COLS;
        if (y > 0 && !seen[c - NL_COLS] && nl_grid[c - NL_COLS] == p) {
            seen[c - NL_COLS] = 1;
            q[qt++] = (uint8_t)(c - NL_COLS);
        }
        if (y < NL_ROWS - 1 && !seen[c + NL_COLS] && nl_grid[c + NL_COLS] == p) {
            seen[c + NL_COLS] = 1;
            q[qt++] = (uint8_t)(c + NL_COLS);
        }
        if (x > 0 && !seen[c - 1] && nl_grid[c - 1] == p) {
            seen[c - 1] = 1;
            q[qt++] = (uint8_t)(c - 1);
        }
        if (x < NL_COLS - 1 && !seen[c + 1] && nl_grid[c + 1] == p) {
            seen[c + 1] = 1;
            q[qt++] = (uint8_t)(c + 1);
        }
    }
    return false;
}

static int nl_pairs_done(void) {
    int n = 0;
    for (int p = 1; p <= NL_PAIRS; p++)
        if (nl_pair_connected(p)) n++;
    return n;
}

static bool nl_full_cover(void) {
    for (int i = 0; i < NL_CELLS; i++)
        if (!nl_grid[i]) return false;
    return true;
}

/* 光标移动 / 画线延伸 */
static void nl_move(int dx, int dy) {
    int x = nl_cur % NL_COLS, y = nl_cur / NL_COLS;
    int nx = x + dx, ny = y + dy;
    if (nx < 0 || nx >= NL_COLS || ny < 0 || ny >= NL_ROWS) return;
    int c = ny * NL_COLS + nx;
    if (!nl_drawing) {
        nl_cur = (uint8_t)c;
        return;
    }
    /* 画线中: 向候选格延伸 */
    if (c == (int)nl_path[0]) { audio_error(); return; }  /* 不回走起点 */
    if (nl_grid[c] != 0) {
        if (nl_fixed[c] && nl_grid[c] == nl_draw_pair) {
            /* 走到另一端 → 自动连接 */
            nl_drawing = false;
            nl_last_pair = nl_draw_pair;
            if (nl_pairs_done() == NL_PAIRS) {
                nl_over = true;
                audio_win();           /* 全连通关 */
                led_fx_set(LED_FX_WIN);
            } else {
                audio_clear();         /* 单对连通 */
            }
        } else {
            audio_error();             /* 占用格阻挡 */
        }
        return;
    }
    nl_path[nl_plen++] = (uint8_t)c;
    nl_grid[c] = nl_draw_pair;
    nl_cur = (uint8_t)c;
}

static void nl_ok(void) {
    if (nl_drawing) {
        int p = nl_draw_pair;
        if (nl_fixed[nl_cur] && nl_grid[nl_cur] == p && nl_cur != nl_path[0]) {
            /* 在另一端按 OK → 连接 */
            nl_drawing = false;
            nl_last_pair = (uint8_t)p;
            if (nl_pairs_done() == NL_PAIRS) {
                nl_over = true;
                audio_win();           /* 全连通关 */
                led_fx_set(LED_FX_WIN);
            } else {
                audio_clear();         /* 单对连通 */
            }
        } else {
            /* 中途(或原地再按)OK → 整条路径作废 */
            for (int i = 1; i < nl_plen; i++) nl_grid[nl_path[i]] = 0;
            nl_plen = 0;
            nl_drawing = false;
        }
        return;
    }
    /* 空闲: 端点上 OK → 开始画线(已连接则先清掉重画) */
    if (nl_fixed[nl_cur]) {
        int p = nl_grid[nl_cur];
        if (nl_pair_connected(p)) {
            nl_clear_pair(p);
            nl_last_pair = 0;
        }
        nl_drawing = true;
        nl_draw_pair = (uint8_t)p;
        nl_plen = 1;
        nl_path[0] = nl_cur;
    }
}

static void nl_del(void) {
    if (nl_drawing) {
        if (nl_plen > 1) {
            nl_plen--;
            nl_grid[nl_path[nl_plen]] = 0;
            nl_cur = nl_path[nl_plen - 1];
        } else {
            nl_drawing = false;      /* 只剩起点 → 取消画线 */
            nl_plen = 0;
        }
        return;
    }
    if (nl_last_pair) {              /* 撤销上一条完成的路径 */
        nl_clear_pair(nl_last_pair);
        nl_last_pair = 0;
    }
}

void numberlink_enter(void) {
    nl_start(0);
    numberlink_render();
    disp_full();
}

void numberlink_exit(void) {}

void numberlink_tick(uint64_t now) { (void)now; }

/* 手写数字追加(无 snprintf 依赖) */
static void nl_append_u32(char *buf, unsigned *n, uint32_t v, unsigned cap) {
    char tmp[12];
    unsigned len = 0;
    if (v == 0) tmp[len++] = '0';
    while (v && len < 11) { tmp[len++] = (char)('0' + v % 10u); v /= 10u; }
    while (len && *n < cap) buf[(*n)++] = tmp[--len];
}

void numberlink_render(void) {
    fb_clear(false);
    /* 棋盘格线 */
    for (int i = 0; i <= NL_COLS; i++)
        fb_vline(NL_OX + i * NL_CELL_SZ, NL_OY, NL_ROWS * NL_CELL_SZ, true);
    for (int j = 0; j <= NL_ROWS; j++)
        fb_hline(NL_OX, NL_OY + j * NL_CELL_SZ, NL_COLS * NL_CELL_SZ, true);
    /* 格内容: 端点 = 实心黑格 + 2x 白数字; 路径 = 配对图案 */
    for (int j = 0; j < NL_ROWS; j++) {
        for (int i = 0; i < NL_COLS; i++) {
            int c = j * NL_COLS + i;
            if (!nl_grid[c]) continue;
            int cx = NL_OX + i * NL_CELL_SZ;
            int cy = NL_OY + j * NL_CELL_SZ;
            if (nl_fixed[c]) {
                fb_fill_rect(cx, cy, NL_CELL_SZ, NL_CELL_SZ, true);
                char d[2] = { (char)('0' + nl_grid[c]), 0 };
                fb_text_scale2(cx + 4, cy + 2, d, false);
            } else {
                fb_fill_tile(cx + 1, cy + 1, NL_CELL_SZ - 2, NL_CELL_SZ - 2,
                             pat_get(nl_pair_pat[nl_grid[c]]));
            }
        }
    }
    /* 光标(最后画, 黑格白边/白格黑边) */
    {
        int ccx = NL_OX + (nl_cur % NL_COLS) * NL_CELL_SZ;
        int ccy = NL_OY + (nl_cur / NL_COLS) * NL_CELL_SZ;
        bool dark = nl_grid[nl_cur] != 0;
        fb_stroke_rect_thick(ccx - 1, ccy - 1, NL_CELL_SZ + 2, NL_CELL_SZ + 2, 2,
                             !dark);
    }
    /* HUD 顶栏 */
    if (nl_over) {
        fb_fill_rect(0, 0, CCG_W, CCG_HUD_H - 1, false);
        fb_text(2, 2, "SOLVED!", true);
        fb_text(CCG_W - 2 - text_width("OK/N:RETRY BACK:QUIT"), 2,
                "OK/N:RETRY BACK:QUIT", true);
        if (!nl_over_full) { nl_over_full = true; disp_force_full(); }
    } else {
        fb_text(0, 0, "NUMBERLINK", true);
        char buf[24];
        unsigned n = 0;
        const char *p = "LV ";
        while (*p && n < 23) buf[n++] = *p++;
        buf[n++] = (char)('1' + nl_level);
        p = " PAIRS ";
        while (*p && n < 23) buf[n++] = *p++;
        nl_append_u32(buf, &n, (uint32_t)nl_pairs_done(), 23);
        if (n < 22) { buf[n++] = '/'; buf[n++] = '6'; }
        if (nl_full_cover() && n < 22) {
            p = " FULL";
            while (*p && n < 23) buf[n++] = *p++;
        }
        buf[n] = 0;
        fb_text(CCG_W - 4 - text_width(buf), 0, buf, true);
        fb_hline(0, CCG_HUD_H - 1, CCG_W, true);
    }
}

void numberlink_on_key(const key_event_t *ev) {
    bool dir = ev->key == K_UP || ev->key == K_DOWN ||
               ev->key == K_LEFT || ev->key == K_RIGHT;
    if (ev->is_repeat && !dir) return;   /* 确认键/字母/DEL 忽略重复 */
    if (nl_over) {
        if (ev->key == K_OK || (ev->key == K_CHAR && ev->ch == 'n')) {
            nl_start(nl_level);
            numberlink_render();
            disp_full();
        } else if (ev->key == K_BACK || ev->key == K_QUIT) {
            s_exit_request = true;
        }
        return;
    }
    switch (ev->key) {
    case K_UP: {
        uint8_t p = nl_cur;
        nl_move(0, -1);
        if (!ev->is_repeat && nl_cur != p) audio_move();
        break;
    }
    case K_DOWN: {
        uint8_t p = nl_cur;
        nl_move(0, 1);
        if (!ev->is_repeat && nl_cur != p) audio_move();
        break;
    }
    case K_LEFT: {
        uint8_t p = nl_cur;
        nl_move(-1, 0);
        if (!ev->is_repeat && nl_cur != p) audio_move();
        break;
    }
    case K_RIGHT: {
        uint8_t p = nl_cur;
        nl_move(1, 0);
        if (!ev->is_repeat && nl_cur != p) audio_move();
        break;
    }
    case K_CHAR:
        switch (ev->ch) {
        case 'w': {
            uint8_t p = nl_cur;
            nl_move(0, -1);
            if (!ev->is_repeat && nl_cur != p) audio_move();
            break;
        }
        case 's': {
            uint8_t p = nl_cur;
            nl_move(0, 1);
            if (!ev->is_repeat && nl_cur != p) audio_move();
            break;
        }
        case 'a': {
            uint8_t p = nl_cur;
            nl_move(-1, 0);
            if (!ev->is_repeat && nl_cur != p) audio_move();
            break;
        }
        case 'd': {
            uint8_t p = nl_cur;
            nl_move(1, 0);
            if (!ev->is_repeat && nl_cur != p) audio_move();
            break;
        }
        case 'n': nl_start((uint8_t)((nl_level + 1) % NL_LEVELS)); break;
        default: break;
        }
        break;
    case K_OK: nl_ok(); break;
    case K_DEL: nl_del(); break;
    case K_BACK:
    case K_PAUSE: {
        pause_sel_t sel;
        if (ui_pause_run(&sel)) {
            if (sel == PAUSE_RESTART) nl_start(nl_level);
        } else {
            s_exit_request = true;
        }
        break;
    }
    case K_QUIT: s_exit_request = true; break;
    default: break;
    }
}
