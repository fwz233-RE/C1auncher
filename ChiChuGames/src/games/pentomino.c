/* 五连方 (Pentomino) — 12 块拼 6x10 矩形
 * 规则: 12 种五连方(F I L N P T U V W X Y Z)全放入 6x10 区域即胜
 * 操作: UP/DOWN 切换方块; L/R 或 A/D 移光标; W/S 上下移光标;
 *       OK 放置/移除; R 旋转; S 求解提示; N 新局; P/BACK 暂停; Q 退出
 * 完成: 12 块全放入 -> WIN (HUD 区显示, 全刷一次)
 * 输入驱动; 移动/放置=快刷; 开局/重开/求解=全刷
 * 求解: 回溯 + 最少旋转优先, 节点上限兜底(纯静态, 零 malloc) */
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
#include <string.h>

#define PN_COLS 6                      /* 列(宽) */
#define PN_ROWS 10                     /* 行(高) */
#define PN_CELL 12                     /* 12px 方格 */
#define PN_OX 116                      /* 72x120 棋盘: x 116..188 */
#define PN_OY 24                       /*           y 24..144  */
#define PN_PX 196                      /* 侧栏起点 x (196..292) */
#define PN_NP 12                       /* 块数 */

#define PN_SOLVE_NODE_MAX 3000000u     /* 求解器节点上限 */

void pentomino_render(void);

typedef struct { uint8_t c[5]; uint8_t w, h; } pn_ori_t;

/* 12 块基准朝向(规范化: 最小 x/y=0, 5 格打包 (y<<3)|x) */
static const pn_ori_t pn_base[PN_NP] = {
    { { 0x01, 0x02, 0x08, 0x09, 0x11 }, 3, 3 },           /* F */
    { { 0x00, 0x01, 0x02, 0x03, 0x04 }, 5, 1 },           /* I */
    { { 0x00, 0x01, 0x02, 0x03, 0x08|3 }, 4, 2 },         /* L */
    { { 0x01, 0x02, 0x08, 0x09, 0x10 }, 3, 3 },           /* N */
    { { 0x00, 0x01, 0x08|0, 0x08|1, 0x10|0 }, 2, 3 },     /* P */
    { { 0x00, 0x01, 0x02, 0x08|1, 0x10|1 }, 3, 3 },       /* T */
    { { 0x00, 0x02, 0x08|0, 0x08|1, 0x08|2 }, 3, 2 },     /* U */
    { { 0x00, 0x08|0, 0x10|0, 0x10|1, 0x10|2 }, 3, 3 },   /* V */
    { { 0x00, 0x01, 0x09, 0x0A, 0x12 }, 3, 3 },           /* W */
    { { 0x01, 0x08, 0x09, 0x0A, 0x11 }, 3, 3 },           /* X */
    { { 0x00, 0x01, 0x02, 0x03, 0x08|1 }, 4, 2 },         /* Y */
    { { 0x00, 0x01, 0x02, 0x08|2, 0x08|3 }, 4, 2 },       /* Z */
};
static const char pn_letter[PN_NP] = { 'F','I','L','N','P','T','U','V','W','X','Y','Z' };

/* 全部朝向(含镜像, 自由五连方): 最多 8 种 */
static pn_ori_t pn_ori[PN_NP][8];
static uint8_t pn_orin[PN_NP];
static bool pn_oris_done;

/* 游戏状态 */
static uint8_t pn_cell[PN_COLS * PN_ROWS];   /* 0=空, p+1=块 p */
static uint8_t pn_used[PN_NP];
static uint8_t pn_curori[PN_NP];             /* 每块当前旋转 */
static int8_t pn_px[PN_NP], pn_py[PN_NP];    /* 每块放置原点 */
static uint8_t pn_cur;                       /* 当前块 0..11 */
static int pn_cx, pn_cy;                     /* 光标(像素坐标 int) */
static bool pn_over;
static bool pn_over_full;
static rng_t pn_rng;

/* 求解器 scratch */
static uint8_t pn_sb[PN_COLS * PN_ROWS];
static int pn_sb_ord[PN_NP];
static uint32_t pn_sb_nodes;
static int8_t pn_sol_px[PN_NP], pn_sol_py[PN_NP];  /* 解的每块真实原点 */
static uint8_t pn_sol_oi[PN_NP];

/* ---- 朝向生成 ---- */
static void pn_sort5(uint8_t c[5]) {
    for (int i = 1; i < 5; i++) {
        uint8_t v = c[i];
        int j = i - 1;
        while (j >= 0 && c[j] > v) { c[j + 1] = c[j]; j--; }
        c[j + 1] = v;
    }
}

/* 镜像(水平), 保持规范化 */
static void pn_mirror(const uint8_t in[5], int w, uint8_t out[5]) {
    for (int i = 0; i < 5; i++)
        out[i] = (uint8_t)(((in[i] >> 3) << 3) | (uint8_t)(w - 1 - (in[i] & 7)));
    pn_sort5(out);
}

/* 旋转 90°CW 并重新规范化+排序; w/h 交换 */
static void pn_rot(const uint8_t in[5], uint8_t out[5], int *w, int *h) {
    int iw = *w, ih = *h;
    int mx = 99, my = 99;
    int tx[5], ty[5];
    for (int i = 0; i < 5; i++) {
        int x = in[i] & 7, y = in[i] >> 3;
        int nx = ih - 1 - y, ny = x;
        tx[i] = nx; ty[i] = ny;
        if (nx < mx) mx = nx;
        if (ny < my) my = ny;
    }
    for (int i = 0; i < 5; i++)
        out[i] = (uint8_t)(((ty[i] - my) << 3) | (tx[i] - mx));
    pn_sort5(out);
    *w = ih; *h = iw;
}

/* 由基准朝向生成全部旋转+镜像(去重); 朝向 0 恒为基准形状 */
static void pn_init_oris(void) {
    for (int p = 0; p < PN_NP; p++) {
        int n = 0;
        for (int fl = 0; fl < 2 && n < 8; fl++) {
            uint8_t cur[5];
            int w = pn_base[p].w, h = pn_base[p].h;
            if (fl == 0) {
                for (int i = 0; i < 5; i++) cur[i] = pn_base[p].c[i];
            } else {
                pn_mirror(pn_base[p].c, pn_base[p].w, cur);
            }
            for (int r = 0; r < 4; r++) {
                bool dup = false;
                for (int k = 0; k < n && !dup; k++)
                    if (pn_ori[p][k].w == w && pn_ori[p][k].h == h &&
                        memcmp(pn_ori[p][k].c, cur, 5) == 0)
                        dup = true;
                if (!dup && n < 8) {
                    for (int i = 0; i < 5; i++) pn_ori[p][n].c[i] = cur[i];
                    pn_ori[p][n].w = (uint8_t)w;
                    pn_ori[p][n].h = (uint8_t)h;
                    n++;
                }
                int nw = w, nh = h;
                uint8_t cand[5];
                pn_rot(cur, cand, &nw, &nh);
                for (int i = 0; i < 5; i++) cur[i] = cand[i];
                w = nw; h = nh;
            }
        }
        pn_orin[p] = (uint8_t)n;
    }
}

/* ---- 放置 ---- */
static bool pn_fits(const uint8_t *board, int p, int oi, int ox, int oy) {
    const pn_ori_t *o = &pn_ori[p][oi];
    for (int i = 0; i < 5; i++) {
        int x = ox + (o->c[i] & 7), y = oy + (o->c[i] >> 3);
        if (x < 0 || x >= PN_COLS || y < 0 || y >= PN_ROWS) return false;
        if (board[y * PN_COLS + x] != 0) return false;
    }
    return true;
}

static void pn_set_piece(uint8_t *board, int p, int oi, int ox, int oy, uint8_t v) {
    const pn_ori_t *o = &pn_ori[p][oi];
    for (int i = 0; i < 5; i++)
        board[(oy + (o->c[i] >> 3)) * PN_COLS + (ox + (o->c[i] & 7))] = v;
}

static int pn_placed_count(void) {
    int n = 0;
    for (int i = 0; i < PN_NP; i++) if (pn_used[i]) n++;
    return n;
}

/* 移除光标所在格子的块 */
static void pn_remove_at_cursor(void) {
    uint8_t p = pn_cell[pn_cy * PN_COLS + pn_cx];
    if (p == 0) return;
    p--;
    pn_set_piece(pn_cell, (int)p, pn_curori[p], pn_px[p], pn_py[p], 0);
    pn_used[p] = 0;
    pn_cur = p;
}

/* OK: 能放则放(成功后自动切下一块), 否则移除光标所在块 */
static void pn_ok_action(void) {
    int oi = pn_curori[pn_cur];
    if (pn_fits(pn_cell, pn_cur, oi, pn_cx, pn_cy)) {
        pn_set_piece(pn_cell, pn_cur, oi, pn_cx, pn_cy, (uint8_t)(pn_cur + 1));
        pn_used[pn_cur] = 1;
        pn_px[pn_cur] = (int8_t)pn_cx;
        pn_py[pn_cur] = (int8_t)pn_cy;
        if (pn_placed_count() == PN_NP) {
            pn_over = true;
            audio_win();               /* 12 块全放完 */
            led_fx_set(LED_FX_WIN);
        } else {
            pn_cur = (uint8_t)((pn_cur + 1) % PN_NP);
            audio_select();            /* 落块成功 */
        }
    } else {
        pn_remove_at_cursor();
    }
}

/* ---- 回溯求解(6x10 经典拼图, 有解) ---- */
static bool pn_solve_rec(int depth) {
    if (pn_sb_nodes++ > PN_SOLVE_NODE_MAX) return false;
    if (depth == PN_NP) return true;
    int cell = -1;
    for (int i = 0; i < PN_COLS * PN_ROWS; i++) {
        if (pn_sb[i] == 0) { cell = i; break; }
    }
    int cx = cell % PN_COLS, cy = cell / PN_COLS;
    for (int k = depth; k < PN_NP; k++) {
        int p = pn_sb_ord[k];
        for (int oi = 0; oi < pn_orin[p]; oi++) {
            const pn_ori_t *o = &pn_ori[p][oi];
            for (int a = 0; a < 5; a++) {   /* 让格 (cx,cy) 被该块第 a 格覆盖 */
                int ox = cx - (o->c[a] & 7);
                int oy = cy - (o->c[a] >> 3);
                if (!pn_fits(pn_sb, p, oi, ox, oy)) continue;
                pn_set_piece(pn_sb, p, oi, ox, oy, (uint8_t)(p + 1));
                pn_sol_px[p] = (int8_t)ox;
                pn_sol_py[p] = (int8_t)oy;
                pn_sol_oi[p] = (uint8_t)oi;
                int t = pn_sb_ord[k];
                pn_sb_ord[k] = pn_sb_ord[depth];
                pn_sb_ord[depth] = t;
                if (pn_solve_rec(depth + 1)) return true;
                t = pn_sb_ord[k];
                pn_sb_ord[k] = pn_sb_ord[depth];
                pn_sb_ord[depth] = t;
                pn_set_piece(pn_sb, p, oi, ox, oy, 0);
            }
        }
    }
    return false;
}

static bool pn_solve(void) {
    memset(pn_sb, 0, sizeof(pn_sb));
    /* 最少朝向优先(强约束先行), 已按 orin 冒泡排序一次 */
    int n = PN_NP;
    for (int i = 0; i < PN_NP; i++) pn_sb_ord[i] = i;
    for (int i = 0; i < n; i++) {
        for (int j = i + 1; j < n; j++) {
            if (pn_orin[pn_sb_ord[j]] < pn_orin[pn_sb_ord[i]]) {
                int t = pn_sb_ord[i];
                pn_sb_ord[i] = pn_sb_ord[j];
                pn_sb_ord[j] = t;
            }
        }
    }
    pn_sb_nodes = 0;
    return pn_solve_rec(0);
}

/* S 键: 求解并上屏(直接胜局) */
static void pn_solve_action(void) {
    if (!pn_solve()) return;
    memcpy(pn_cell, pn_sb, sizeof(pn_cell));
    for (int i = 0; i < PN_NP; i++) {
        pn_used[i] = 1;
        pn_px[i] = pn_sol_px[i];     /* 求解时记录的真实原点 */
        pn_py[i] = pn_sol_py[i];
        pn_curori[i] = pn_sol_oi[i];
    }
    pn_over = true;
    pn_over_full = false;
    audio_win();                       /* 求解直接胜局 */
    led_fx_set(LED_FX_WIN);
    pentomino_render();
    disp_full();
}

/* ---- 开局 ---- */
static void pn_start(void) {
    memset(pn_cell, 0, sizeof(pn_cell));
    memset(pn_used, 0, sizeof(pn_used));
    memset(pn_curori, 0, sizeof(pn_curori));
    pn_cur = 0;
    pn_cx = PN_COLS / 2 - 1;
    pn_cy = PN_ROWS / 2 - 1;
    pn_over = false;
    pn_over_full = false;
    pentomino_render();
    disp_full();
}

void pentomino_enter(void) {
    if (!pn_oris_done) {
        pn_init_oris();
        pn_oris_done = true;
    }
    rng_seed(&pn_rng, now_ms());
    pn_start();
}

void pentomino_exit(void) {}

void pentomino_tick(uint64_t now) { (void)now; }

/* ---- 渲染 ---- */
void pentomino_render(void) {
    fb_clear(false);

    /* 已放置块: 黑格 + 白字母 */
    for (int y = 0; y < PN_ROWS; y++) {
        for (int x = 0; x < PN_COLS; x++) {
            uint8_t p = pn_cell[y * PN_COLS + x];
            if (p == 0) continue;
            char l[2];
            l[0] = pn_letter[p - 1];
            l[1] = 0;
            fb_fill_rect(PN_OX + x * PN_CELL, PN_OY + y * PN_CELL, PN_CELL, PN_CELL, true);
            fb_text(PN_OX + x * PN_CELL + 4, PN_OY + y * PN_CELL + 3, l, false);
        }
    }

    /* 当前块预览: 能放=图案底+黑字母; 放不下=细边框(越界/占用格不画) */
    if (!pn_over) {
        int oi = pn_curori[pn_cur];
        const pn_ori_t *o = &pn_ori[pn_cur][oi];
        bool fits = pn_fits(pn_cell, pn_cur, oi, pn_cx, pn_cy);
        char l[2];
        l[0] = pn_letter[pn_cur];
        l[1] = 0;
        for (int i = 0; i < 5; i++) {
            int x = pn_cx + (o->c[i] & 7), y = pn_cy + (o->c[i] >> 3);
            if (x < 0 || x >= PN_COLS || y < 0 || y >= PN_ROWS) continue;
            if (pn_cell[y * PN_COLS + x] != 0) continue;
            int bx = PN_OX + x * PN_CELL, by = PN_OY + y * PN_CELL;
            if (fits) {
                fb_fill_tile(bx, by, PN_CELL, PN_CELL, pat_get(PAT_GRID));
                fb_text(bx + 4, by + 3, l, true);
            } else {
                fb_stroke_rect(bx + 1, by + 1, PN_CELL - 2, PN_CELL - 2, true);
            }
        }
    }

    /* 网格: 1px 内线 + 2px 外框 */
    for (int i = 0; i <= PN_COLS; i++)
        fb_fill_rect(PN_OX + i * PN_CELL, PN_OY, 1, PN_ROWS * PN_CELL, true);
    for (int j = 0; j <= PN_ROWS; j++)
        fb_fill_rect(PN_OX, PN_OY + j * PN_CELL, PN_COLS * PN_CELL, 1, true);
    fb_stroke_rect_thick(PN_OX, PN_OY, PN_COLS * PN_CELL, PN_ROWS * PN_CELL, 2, true);

    /* 光标: 全部画完后反色 2px 外框 */
    {
        int bx = PN_OX + pn_cx * PN_CELL, by = PN_OY + pn_cy * PN_CELL;
        bool white = (pn_cell[pn_cy * PN_COLS + pn_cx] != 0);
        fb_stroke_rect_thick(bx - 2, by - 2, PN_CELL + 4, PN_CELL + 4, 2,
                             white ? false : true);
    }

    /* 侧栏: 标签 + 反白大字当前块字母 + 形状预览 + 剩余字母表 */
    fb_text(PN_PX, 24, "CUR", true);
    {
        char l[2];
        l[0] = pn_letter[pn_cur];
        l[1] = 0;
        fb_fill_rect(PN_PX + 2, 32, 10, 14, true);
        fb_text_scale2(PN_PX + 2, 32, l, false);
    }
    {
        const pn_ori_t *o = &pn_ori[pn_cur][pn_curori[pn_cur]];
        int w = (int)o->w * 8, h = (int)o->h * 8;
        int bx = PN_PX + 2 + (94 - w) / 2, by = 50 + (40 - h) / 2;
        char l[2];
        l[0] = pn_letter[pn_cur];
        l[1] = 0;
        for (int i = 0; i < 5; i++) {
            int x = bx + (o->c[i] & 7) * 8, y = by + (o->c[i] >> 3) * 8;
            fb_fill_rect(x, y, 8, 8, true);
            fb_text(x + 1, y + 1, l, false);
        }
    }
    fb_text(PN_PX, 94, "R:ROTATE", true);
    fb_text(PN_PX, 104, "REMAIN", true);
    for (int i = 0; i < PN_NP; i++) {
        int cx = PN_PX + 27 + (i % 4) * 11, cy = 114 + (i / 4) * 11;
        char l[2];
        l[0] = pn_letter[i];
        l[1] = 0;
        if (pn_used[i]) {
            fb_fill_rect(cx, cy, 9, 9, true);
            fb_text(cx + 2, cy + 1, l, false);
        } else {
            fb_text(cx + 2, cy + 1, l, true);
        }
        if (i == pn_cur)
            fb_stroke_rect_thick(cx - 1, cy - 1, 11, 11, 1, true);
    }

    /* HUD: 左标题, 右 PLACED n/12 */
    fb_text(0, 0, "PENTOMINO", true);
    {
        int pc = pn_placed_count();
        char buf[16];
        int n = 0;
        const char *lab = "PLACED ";
        while (lab[n]) { buf[n] = lab[n]; n++; }
        if (pc >= 10) buf[n++] = (char)('0' + pc / 10);
        buf[n++] = (char)('0' + pc % 10);
        buf[n++] = '/';
        buf[n++] = '1';
        buf[n++] = '2';
        buf[n] = 0;
        fb_text(CCG_W - text_width(buf) - 2, 0, buf, true);
    }
    fb_hline(0, CCG_HUD_H - 1, CCG_W, true);

    /* 胜利: HUD 区两行 + 全刷一次 */
    if (pn_over) {
        fb_fill_rect(0, 0, CCG_W, CCG_HUD_H - 1, false);
        fb_text(2, 2, "YOU WIN!", true);
        fb_text(CCG_W - text_width("OK/N:RETRY BACK:QUIT") - 2, 2,
                "OK/N:RETRY BACK:QUIT", true);
        if (!pn_over_full) {
            pn_over_full = true;
            disp_force_full();
        }
    }
}

/* ---- 输入 ---- */
void pentomino_on_key(const key_event_t *ev) {
    if (pn_over) {
        if (ev->is_repeat) return;
        if (ev->key == K_OK ||
            (ev->key == K_CHAR && (ev->ch == 'n' || ev->ch == 'r')))
            pn_start();                        /* RETRY: 同一局 */
        else if (ev->key == K_BACK || ev->key == K_QUIT)
            s_exit_request = true;
        return;
    }
    switch (ev->key) {
    case K_UP:
        pn_cur = (uint8_t)((pn_cur + PN_NP - 1) % PN_NP);
        if (!ev->is_repeat) audio_move();   /* 切块 */
        break;
    case K_DOWN:
        pn_cur = (uint8_t)((pn_cur + 1) % PN_NP);
        if (!ev->is_repeat) audio_move();   /* 切块 */
        break;
    case K_LEFT:
        if (pn_cx > 0) { pn_cx--; if (!ev->is_repeat) audio_move(); }
        break;
    case K_RIGHT:
        if (pn_cx < PN_COLS - 1) { pn_cx++; if (!ev->is_repeat) audio_move(); }
        break;
    case K_OK:
        if (ev->is_repeat) break;
        pn_ok_action();
        break;
    case K_DEL:
        if (ev->is_repeat) break;
        pn_remove_at_cursor();
        break;
    case K_CHAR:
        if (ev->ch == 'w' && pn_cy > 0) { pn_cy--; if (!ev->is_repeat) audio_move(); }
        else if (ev->ch == 's' && pn_cy < PN_ROWS - 1) { pn_cy++; if (!ev->is_repeat) audio_move(); }
        else if (ev->ch == 'a' && pn_cx > 0) { pn_cx--; if (!ev->is_repeat) audio_move(); }
        else if (ev->ch == 'd' && pn_cx < PN_COLS - 1) { pn_cx++; if (!ev->is_repeat) audio_move(); }
        else if (ev->ch == 'r' && !ev->is_repeat) {
            pn_curori[pn_cur] = (uint8_t)((pn_curori[pn_cur] + 1) % pn_orin[pn_cur]);
            audio_move();               /* 旋转 */
        }
        else if (ev->ch == 'n' && !ev->is_repeat)
            pn_start();
        else if (ev->ch == 's' && !ev->is_repeat)
            pn_solve_action();
        break;
    case K_PAUSE:
    case K_BACK: {
        if (ev->is_repeat) break;
        pause_sel_t sel;
        if (ui_pause_run(&sel)) {
            if (sel == PAUSE_RESTART) pn_start();
        } else {
            s_exit_request = true;
        }
        break;
    }
    case K_QUIT:
        if (!ev->is_repeat) s_exit_request = true;
        break;
    default:
        break;
    }
}
