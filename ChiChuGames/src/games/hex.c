/* HEX — 11x11 六角格连通博弈(六子棋)
 * 玩家红(左-右连通, 实心菱形) vs AI 蓝(上-下连通, 空心菱形), 玩家先手
 * 棋盘: 11x11 菱形格(行错位半格: 奇数行右移 7px), 格 14px
 * 六邻接: 偶数行上邻 (r-1,c-1)(r-1,c) 下邻 (r+1,c-1)(r+1,c);
 *         奇数行上邻 (r-1,c)(r-1,c+1) 下邻 (r+1,c)(r+1,c+1)
 * 胜负: 洪水填充判定本子连通域是否同时触到两条边
 * AI: 1) 一步取胜 2) 堵对手一步胜 3) 启发式(邻子数+纵向中心进度), 平手随机
 * AI 回合经 tick 延迟 700ms(面板节奏), 右栏指示条反白作为视觉提示
 * 输入驱动 + 周期 AI; 落子/移动=快刷; 开局/结束=全刷 */
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

#define HX_N 11
#define HX_CELLS (HX_N * HX_N)
#define HX_CW 14            /* 格宽(列距), 菱形全宽/全高 */
#define HX_HALF 7           /* 半格: 菱形半宽/半高, 也是行距与错位量 */
#define HX_OX 70            /* 棋盘左缘(左栏 2..61) */
#define HX_OY 44            /* 棋盘顶缘(垂直居中于 HUD 下区域) */

void hex_render(void);

static uint8_t hx_board[HX_N][HX_N];    /* 0=空 1=红(玩家 L-R) 2=蓝(AI T-B) */
static int hx_cx, hx_cy;                /* 光标 0..10 */
static int hx_turn;                     /* 1=玩家 2=AI */
static bool hx_ai_pending;              /* AI 回合等待 tick */
static int hx_moves;                    /* 已落子数 0..121 */
static int hx_lastr, hx_lastc, hx_lastp;/* 最后落子(-1=无) */
static int hx_winner;                   /* 0=无/平 1=玩家 2=AI */
static bool hx_over, hx_over_full;
static bool hx_vis[HX_CELLS];           /* 洪水填充访问标记 */
static bool hx_winpath[HX_CELLS];       /* 取胜连通域(终局高亮) */
static int16_t hx_stk_r[HX_CELLS], hx_stk_c[HX_CELLS];  /* 填充栈 */
static rng_t hx_rng;

/* 六邻接偏移表 [行奇偶][6][2], 与"奇数行右移半格"的菱形铺排一致 */
static const int8_t hx_nb[2][6][2] = {
    { { -1, -1 }, { -1, 0 }, { 0, -1 }, { 0, 1 }, { 1, -1 }, { 1, 0 } },
    { { -1, 0 }, { -1, 1 }, { 0, -1 }, { 0, 1 }, { 1, 0 }, { 1, 1 } }
};

/* ---- 规则核心 ---- */

/* (r,c) 格中心像素坐标 */
static void hx_cell_center(int r, int c, int *x, int *y) {
    *x = HX_OX + HX_HALF + c * HX_CW + (r & 1) * HX_HALF;
    *y = HX_OY + HX_HALF + r * HX_HALF;
}

/* 洪水填充: (r,c) 所在同色连通域是否连通玩家/AI 的两条目标边 */
static bool hx_check_win(int r, int c) {
    uint8_t col = hx_board[r][c];
    if (col == 0) return false;
    for (int i = 0; i < HX_CELLS; i++) hx_vis[i] = false;
    bool e1 = false, e2 = false;    /* 触到边 A / 边 B */
    int sp = 0;
    hx_stk_r[sp] = (int16_t)r;
    hx_stk_c[sp] = (int16_t)c;
    sp++;
    hx_vis[r * HX_N + c] = true;
    while (sp > 0) {
        sp--;
        int cr = hx_stk_r[sp], cc = hx_stk_c[sp];
        if (col == 1) {             /* 红: 左-右 */
            if (cc == 0) e1 = true;
            if (cc == HX_N - 1) e2 = true;
        } else {                    /* 蓝: 上-下 */
            if (cr == 0) e1 = true;
            if (cr == HX_N - 1) e2 = true;
        }
        if (e1 && e2) return true;
        const int8_t (*nb)[2] = hx_nb[cr & 1];
        for (int k = 0; k < 6; k++) {
            int nr = cr + nb[k][0], nc = cc + nb[k][1];
            if (nr < 0 || nr >= HX_N || nc < 0 || nc >= HX_N) continue;
            if (hx_vis[nr * HX_N + nc] || hx_board[nr][nc] != col) continue;
            hx_vis[nr * HX_N + nc] = true;
            hx_stk_r[sp] = (int16_t)nr;
            hx_stk_c[sp] = (int16_t)nc;
            sp++;
        }
    }
    return false;
}

/* 记录取胜连通域(终局高亮), 仅在已判胜后调用 */
static void hx_mark_win(int r, int c) {
    uint8_t col = hx_board[r][c];
    for (int i = 0; i < HX_CELLS; i++) hx_winpath[i] = false;
    int sp = 0;
    hx_stk_r[sp] = (int16_t)r;
    hx_stk_c[sp] = (int16_t)c;
    sp++;
    hx_winpath[r * HX_N + c] = true;
    while (sp > 0) {
        sp--;
        int cr = hx_stk_r[sp], cc = hx_stk_c[sp];
        const int8_t (*nb)[2] = hx_nb[cr & 1];
        for (int k = 0; k < 6; k++) {
            int nr = cr + nb[k][0], nc = cc + nb[k][1];
            if (nr < 0 || nr >= HX_N || nc < 0 || nc >= HX_N) continue;
            if (hx_winpath[nr * HX_N + nc] || hx_board[nr][nc] != col) continue;
            hx_winpath[nr * HX_N + nc] = true;
            hx_stk_r[sp] = (int16_t)nr;
            hx_stk_c[sp] = (int16_t)nc;
            sp++;
        }
    }
}

/* 落子收尾: 记步/记最近/判胜判平 */
static void hx_commit(int r, int c, int p) {
    hx_board[r][c] = (uint8_t)p;
    hx_moves++;
    hx_lastr = r;
    hx_lastc = c;
    hx_lastp = p;
    if (hx_check_win(r, c)) {
        hx_mark_win(r, c);
        hx_winner = p;
        hx_over = true;
        if (p == 1) {
            audio_win();
            led_fx_set(LED_FX_WIN);
        } else {
            audio_lose();
            led_fx_set(LED_FX_LOSE);
        }
    } else if (hx_moves >= HX_CELLS) {
        hx_over = true;             /* 防御分支: 满盘判平 */
    }
}

/* ---- AI(蓝, 上-下连通) ---- */
static void hx_ai_move(void) {
    int r, c, k;
    /* 1) 一步取胜 */
    for (r = 0; r < HX_N; r++)
        for (c = 0; c < HX_N; c++) {
            if (hx_board[r][c]) continue;
            hx_board[r][c] = 2;
            if (hx_check_win(r, c)) {
                hx_commit(r, c, 2);
                return;
            }
            hx_board[r][c] = 0;
        }
    /* 2) 堵对手一步胜: 对手一步胜格可能有多个, 选最优堵点
     *    (己方邻子多 > 位置居中), 避免堵了边角留下直连口 */
    {
        int br = -1, bc = -1, bs = -1;
        for (r = 0; r < HX_N; r++)
            for (c = 0; c < HX_N; c++) {
                if (hx_board[r][c]) continue;
                hx_board[r][c] = 1;
                if (hx_check_win(r, c)) {
                    int b_nb = 0;
                    const int8_t (*nb2)[2] = hx_nb[r & 1];
                    for (k = 0; k < 6; k++) {
                        int nr = r + nb2[k][0], nc = c + nb2[k][1];
                        if (nr < 0 || nr >= HX_N || nc < 0 || nc >= HX_N) continue;
                        if (hx_board[nr][nc] == 2) b_nb++;
                    }
                    int vr = r < 5 ? r : HX_N - 1 - r;
                    int hc = c < 5 ? c : HX_N - 1 - c;
                    int s = b_nb * 8 + vr + hc;
                    if (s > bs) { bs = s; br = r; bc = c; }
                }
                hx_board[r][c] = 0;
            }
        if (br >= 0) {
            hx_commit(br, bc, 2);
            return;
        }
    }
    /* 3) 启发式: 己方邻子(+12) > 对方邻子(+6), 加居中进度; 平手随机 */
    {
        int best = -1, pool_n = 0;
        int pool[8][2];
        for (r = 0; r < HX_N; r++)
            for (c = 0; c < HX_N; c++) {
                if (hx_board[r][c]) continue;
                int s = 0;
                const int8_t (*nb)[2] = hx_nb[r & 1];
                for (k = 0; k < 6; k++) {
                    int nr = r + nb[k][0], nc = c + nb[k][1];
                    if (nr < 0 || nr >= HX_N || nc < 0 || nc >= HX_N) continue;
                    if (hx_board[nr][nc] == 2) s += 12;
                    else if (hx_board[nr][nc] == 1) s += 6;
                }
                int vr = r < 5 ? r : HX_N - 1 - r;  /* 纵向居中进度 0..5 */
                int hc = c < 5 ? c : HX_N - 1 - c;  /* 横向居中(兼堵对方) */
                s += vr + hc;
                if (s > best) {
                    best = s;
                    pool_n = 0;
                    pool[pool_n][0] = r;
                    pool[pool_n][1] = c;
                    pool_n++;
                } else if (s == best && pool_n < 8) {
                    pool[pool_n][0] = r;
                    pool[pool_n][1] = c;
                    pool_n++;
                }
            }
        if (pool_n == 0) return;    /* 防御: 棋盘满时不应走到 */
        uint32_t kk = rng_range(&hx_rng, (uint32_t)pool_n);
        hx_commit(pool[kk][0], pool[kk][1], 2);
    }
}

/* ---- 绘制 ---- */

/* 实心菱形 |dx|+|dy| <= half */
static void hx_diamond(int cx, int cy, int half, bool black) {
    for (int dy = -half; dy <= half; dy++) {
        int ay = dy < 0 ? -dy : dy;
        for (int dx = -half; dx <= half; dx++) {
            int ax = dx < 0 ? -dx : dx;
            if (ax + ay <= half) fb_pixel(cx + dx, cy + dy, black);
        }
    }
}

/* 实心圆 */
static void hx_dot(int cx, int cy, int rad, bool black) {
    for (int dy = -rad; dy <= rad; dy++)
        for (int dx = -rad; dx <= rad; dx++)
            if (dx * dx + dy * dy <= rad * rad)
                fb_pixel(cx + dx, cy + dy, black);
}

/* 小图例菱形: who=1 实心(红), 2 空心(蓝); inv 反色 */
static void hx_glyph(int cx, int cy, int who, bool inv) {
    if (who == 1) {
        hx_diamond(cx, cy, 5, inv);
        hx_dot(cx, cy, 1, !inv);
    } else {
        hx_diamond(cx, cy, 5, inv);
        hx_diamond(cx, cy, 4, !inv);
        hx_dot(cx, cy, 1, inv);
    }
}

/* 画一格: inv=true 为光标反白(整格黑底+内容反色); 终局高亮获胜连通域 */
static void hx_draw_cell(int r, int c, bool inv) {
    int cx, cy;
    hx_cell_center(r, c, &cx, &cy);
    uint8_t v = hx_board[r][c];
    bool wp = hx_winpath[r * HX_N + c];
    if (inv) {
        hx_diamond(cx, cy, HX_HALF, true);
        if (v == 0) {
            hx_diamond(cx, cy, HX_HALF - 1, false);
            hx_diamond(cx, cy, HX_HALF - 2, true);
        } else if (v == 1) {
            hx_diamond(cx, cy, HX_HALF, false);
            hx_dot(cx, cy, 2, true);
        } else {
            hx_diamond(cx, cy, HX_HALF - 1, false);
            hx_dot(cx, cy, 2, true);
        }
        return;
    }
    if (v == 0) {
        hx_diamond(cx, cy, HX_HALF, true);
        hx_diamond(cx, cy, HX_HALF - 1, false);
    } else if (v == 1) {
        bool last = (hx_lastp == 1 && r == hx_lastr && c == hx_lastc);
        hx_diamond(cx, cy, HX_HALF, true);
        if (wp) {
            hx_diamond(cx, cy, HX_HALF - 1, false);
            hx_diamond(cx, cy, HX_HALF - 2, true);
        }
        hx_dot(cx, cy, last ? 3 : 2, false);
    } else {
        bool last = (hx_lastp == 2 && r == hx_lastr && c == hx_lastc);
        hx_diamond(cx, cy, HX_HALF, true);
        hx_diamond(cx, cy, (wp ? HX_HALF - 2 : HX_HALF - 1), false);
        hx_dot(cx, cy, last ? 3 : 2, true);
    }
}

/* 十进制字符串 */
static void hx_num(char *buf, unsigned cap, uint32_t v) {
    unsigned i = 0;
    if (v == 0) buf[i++] = '0';
    while (v && i < cap - 1) { buf[i++] = (char)('0' + v % 10); v /= 10; }
    unsigned len = i;
    char rev[16];
    for (unsigned j = 0; j < len && j < 15; j++) rev[j] = buf[len - 1 - j];
    rev[len] = 0;
    for (unsigned j = 0; j <= len; j++) buf[j] = rev[j];
}

void hex_render(void) {
    fb_clear(false);
    /* HUD 顶栏: 左标题, 右 MOVE 计数 */
    fb_text(0, 0, "HEX", true);
    {
        char buf[16];
        hx_num(buf, sizeof(buf), (uint32_t)hx_moves);
        char full[24];
        const char *label = "MOVE ";
        unsigned n = 0;
        while (label[n]) { full[n] = label[n]; n++; }
        for (unsigned j = 0; buf[j]; j++) full[n++] = buf[j];
        full[n] = 0;
        fb_text(CCG_W - text_width(full) - 4, 0, full, true);
    }
    fb_hline(0, CCG_HUD_H - 1, CCG_W, true);
    /* 左栏: MOVE 大号反白 + LAST + 目标说明 */
    {
        char buf[12];
        hx_num(buf, sizeof(buf), (uint32_t)hx_moves);
        fb_text(2, 20, "MOVE", true);
        int w = text_width(buf) * 2;
        fb_fill_rect(0, 28, w + 8, 16, true);
        fb_text_scale2(4, 30, buf, false);
        fb_hline(2, 52, 58, true);
        fb_text(2, 60, "LAST", true);
        if (hx_lastr >= 0) {
            char cbuf[4];
            cbuf[0] = (char)('A' + hx_lastc);
            cbuf[1] = (char)('0' + (hx_lastr + 1) / 10);
            cbuf[2] = (char)('0' + (hx_lastr + 1) % 10);
            cbuf[3] = 0;
            fb_text(2, 68, cbuf, true);
        } else {
            fb_text(2, 68, "--", true);
        }
        fb_hline(2, 88, 58, true);
        fb_text(2, 96, "RED: L-R", true);
        fb_text(2, 106, "BLUE: T-B", true);
    }
    /* 右栏: 回合指示条(当前回合反白) */
    {
        bool you = !hx_over && hx_turn == 1;
        bool ai = !hx_over && hx_turn == 2;
        if (you) fb_fill_rect(233, 24, 62, 28, true);
        fb_text(236, 26, "YOU", you ? false : true);
        hx_glyph(276, 38, 1, !you);
        if (ai) fb_fill_rect(233, 60, 62, 28, true);
        fb_text(236, 62, "AI", ai ? false : true);
        hx_glyph(276, 74, 2, !ai);
    }
    /* 棋盘: 菱形格 */
    for (int r = 0; r < HX_N; r++)
        for (int c = 0; c < HX_N; c++)
            hx_draw_cell(r, c, false);
    /* 光标: 反白格(所有格画完后最后画) */
    if (!hx_over) hx_draw_cell(hx_cy, hx_cx, true);
    /* 终局: HUD 区两行提示 + 强制全刷一次 */
    if (hx_over) {
        fb_fill_rect(0, 0, CCG_W, CCG_HUD_H - 1, false);
        fb_text(2, 2, hx_winner == 1 ? "YOU WIN!" :
                     hx_winner == 2 ? "AI WINS" : "DRAW", true);
        fb_text(CCG_W - 2 - text_width("OK/N:RETRY BACK:QUIT"), 2,
                "OK/N:RETRY BACK:QUIT", true);
        if (!hx_over_full) { hx_over_full = true; disp_force_full(); }
    }
}

/* ---- 生命周期 ---- */

void hex_enter(void) {
    for (int i = 0; i < HX_CELLS; i++) {
        hx_board[i / HX_N][i % HX_N] = 0;
        hx_winpath[i] = false;
    }
    hx_cx = 5;
    hx_cy = 5;
    hx_turn = 1;
    hx_ai_pending = false;
    hx_moves = 0;
    hx_lastr = -1;
    hx_lastc = -1;
    hx_lastp = 0;
    hx_winner = 0;
    hx_over = false;
    hx_over_full = false;
    rng_seed(&hx_rng, now_ms() ^ 0x1A5C);
    hex_render();
    disp_full();
}

void hex_exit(void) {}

/* AI 回合: 延迟一拍执行, 期间右栏 AI 指示条反白 */
void hex_tick(uint64_t now) {
    (void)now;
    if (hx_ai_pending && !hx_over) {
        hx_ai_pending = false;
        hx_ai_move();
        hx_turn = 1;
    }
}

void hex_on_key(const key_event_t *ev) {
    if (hx_over) {
        if (ev->is_repeat) return;
        if (ev->key == K_OK || (ev->key == K_CHAR && ev->ch == 'n')) hex_enter();
        else if (ev->key == K_BACK || ev->key == K_QUIT) s_exit_request = true;
        return;
    }
    switch (ev->key) {
    case K_LEFT:
    case K_RIGHT:
    case K_UP:
    case K_DOWN:            /* 方向键响应长按重复 */
        if (ev->key == K_LEFT && hx_cx > 0) hx_cx--;
        else if (ev->key == K_RIGHT && hx_cx < HX_N - 1) hx_cx++;
        else if (ev->key == K_UP && hx_cy > 0) hx_cy--;
        else if (ev->key == K_DOWN && hx_cy < HX_N - 1) hx_cy++;
        break;
    case K_CHAR:
        if (ev->is_repeat) break;
        if (ev->ch == 'a' && hx_cx > 0) hx_cx--;
        else if (ev->ch == 'd' && hx_cx < HX_N - 1) hx_cx++;
        else if (ev->ch == 'w' && hx_cy > 0) hx_cy--;
        else if (ev->ch == 's' && hx_cy < HX_N - 1) hx_cy++;
        else if (ev->ch == 'n') hex_enter();
        break;
    case K_OK:
        if (ev->is_repeat) break;
        if (hx_turn != 1 || hx_board[hx_cy][hx_cx]) {
            audio_error();
            break;
        }
        hx_board[hx_cy][hx_cx] = 1;
        hx_commit(hx_cy, hx_cx, 1);
        if (!hx_over) {
            audio_move();
            hx_turn = 2;
            hx_ai_pending = true;
        }
        break;
    case K_BACK: {
        if (ev->is_repeat) break;
        pause_sel_t sel;
        if (ui_pause_run(&sel)) {
            if (sel == PAUSE_RESTART) hex_enter();
        } else {
            s_exit_request = true;
        }
        break;
    }
    case K_QUIT:
        if (ev->is_repeat) break;
        s_exit_request = true;
        break;
    default:
        break;
    }
}
