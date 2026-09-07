/* CONNECT 4 — 7 列 x 6 行, 20px 格(140x120 居中, y 从 18 起)
 * 玩家执黑先手, AI 执白; 光标在列顶左右移, OK 落子, 棋子落到该列最低空位
 * AI: 极小极大深度 4(negamax + alpha-beta 剪枝), 中心优先移动序,
 *     根层全窗口精确求值 → 平手列随机(种子 rng_seed 防 xorshift 恒 0)
 * 输入驱动; 落子/移动=快刷; 开局/结束=全刷 */
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

#define C4_COLS 7
#define C4_ROWS 6
#define C4_CELL 20
#define C4_BOARD_W (C4_COLS * C4_CELL)          /* 140 */
#define C4_BOARD_H (C4_ROWS * C4_CELL)          /* 120 */
#define C4_OX ((CCG_W - C4_BOARD_W) / 2)        /* 78, 水平居中 */
#define C4_OY 18                                /* 顶栏 16px 下留 2px */
#define C4_DEPTH 4
#define C4_WIN 100000

void connect4_render(void);

static uint8_t c4_board[C4_ROWS][C4_COLS];  /* 0=空 1=黑(玩家) 2=白(AI); [0]=顶 [5]=底 */
static int c4_col_h[C4_COLS];               /* 每列已落子数 0..6 */
static int c4_col;                          /* 光标列 0..6 */
static int c4_turn;                         /* 1=玩家 2=AI */
static int c4_steps;                        /* 已落子数(0..42) */
static int c4_last_col, c4_last_row;        /* 最后落子格(-1=无) */
static int c4_winner;                       /* 0=无/平 1=玩家 2=AI */
static bool c4_over, c4_over_full;
static rng_t c4_rng;

/* 极小极大移动序: 中心列优先(更强剪枝) */
static const int c4_order[C4_COLS] = { 3, 2, 4, 1, 5, 0, 6 };

/* (col,row) 落子后是否成四连(横/竖/两斜) */
static bool c4_win_at(int col, int row) {
    static const int dx4[4] = { 1, 0, 1, 1 };
    static const int dy4[4] = { 0, 1, 1, -1 };
    if (col < 0 || col >= C4_COLS || row < 0 || row >= C4_ROWS) return false;
    uint8_t c = c4_board[row][col];
    if (c == 0) return false;
    for (int d = 0; d < 4; d++) {
        int cnt = 1;
        for (int x = col + dx4[d], y = row + dy4[d];
             x >= 0 && x < C4_COLS && y >= 0 && y < C4_ROWS &&
             c4_board[y][x] == c;
             x += dx4[d], y += dy4[d]) cnt++;
        for (int x = col - dx4[d], y = row - dy4[d];
             x >= 0 && x < C4_COLS && y >= 0 && y < C4_ROWS &&
             c4_board[y][x] == c;
             x -= dx4[d], y -= dy4[d]) cnt++;
        if (cnt >= 4) return true;
    }
    return false;
}

/* 4 格窗口 (r,c) 起点沿 (dx,dy): 白(AI) 正分, 黑(玩家) 负分 */
static int c4_window(int c, int r, int dx, int dy) {
    int a = 0, p = 0;
    for (int i = 0; i < 4; i++) {
        uint8_t v = c4_board[r + i * dy][c + i * dx];
        if (v == 2) a++;
        else if (v == 1) p++;
    }
    if (a == 0 && p == 0) return 0;         /* 全空 */
    if (a == 0) {                           /* 玩家独享窗口 */
        if (p == 4) return -C4_WIN;
        if (p == 3) return -300;            /* 三连(可成四威胁) */
        if (p == 2) return -30;
        return -1;
    }
    if (p == 0) {                           /* AI 独享窗口 */
        if (a == 4) return C4_WIN;
        if (a == 3) return 300;
        if (a == 2) return 30;
        return 1;
    }
    return 0;                               /* 混合窗口无用 */
}

/* 叶子启发式: 全部 88 个 4 格窗口求和 */
static int c4_eval(void) {
    int total = 0;
    for (int r = 0; r < C4_ROWS; r++)
        for (int c = 0; c <= C4_COLS - 4; c++)
            total += c4_window(c, r, 1, 0);
    for (int c = 0; c < C4_COLS; c++)
        for (int r = 0; r <= C4_ROWS - 4; r++)
            total += c4_window(c, r, 0, 1);
    for (int c = 0; c <= C4_COLS - 4; c++)
        for (int r = 0; r <= C4_ROWS - 4; r++)
            total += c4_window(c, r, 1, 1);
    for (int c = 0; c <= C4_COLS - 4; c++)
        for (int r = 3; r < C4_ROWS; r++)
            total += c4_window(c, r, 1, -1);
    return total;
}

/* negamax + alpha-beta; 返回当前玩家(color)视角最佳分 */
static int c4_negamax(int depth, int alpha, int beta, int color) {
    if (depth == 0) {
        int e = c4_eval();
        return (color == 2) ? e : -e;
    }
    int best = -(C4_WIN + C4_DEPTH) - 1;
    bool any = false;
    for (int ci = 0; ci < C4_COLS; ci++) {
        int col = c4_order[ci];
        if (c4_col_h[col] >= C4_ROWS) continue;
        any = true;
        int row = C4_ROWS - 1 - c4_col_h[col];
        c4_board[row][col] = (uint8_t)color;
        c4_col_h[col]++;
        int sc;
        if (c4_win_at(col, row)) {
            sc = C4_WIN + depth;            /* 越快赢分越高 */
        } else {
            sc = -c4_negamax(depth - 1, -beta, -alpha, 3 - color);
        }
        c4_col_h[col]--;
        c4_board[row][col] = 0;
        if (sc > best) best = sc;
        if (sc > alpha) alpha = sc;
        if (alpha >= beta) break;           /* 剪枝 */
    }
    if (!any) return 0;                     /* 满盘: 平局 */
    return best;
}

/* AI 选列: 根层全窗口精确求值(每步都精确, 平手可安全随机), 子层照常剪枝 */
static int c4_ai_best(void) {
    int best = -(C4_WIN + C4_DEPTH) - 1;
    int cand[C4_COLS];
    int nc = 0;
    for (int ci = 0; ci < C4_COLS; ci++) {
        int col = c4_order[ci];
        if (c4_col_h[col] >= C4_ROWS) continue;
        int row = C4_ROWS - 1 - c4_col_h[col];
        c4_board[row][col] = 2;
        c4_col_h[col]++;
        int sc;
        if (c4_win_at(col, row)) {
            sc = C4_WIN + C4_DEPTH;
        } else {
            sc = -c4_negamax(C4_DEPTH - 1, -(C4_WIN + C4_DEPTH) - 1,
                             (C4_WIN + C4_DEPTH) + 1, 1);
        }
        c4_col_h[col]--;
        c4_board[row][col] = 0;
        if (sc > best) { best = sc; nc = 0; cand[nc++] = col; }
        else if (sc == best) { if (nc < C4_COLS) cand[nc++] = col; }
    }
    if (nc == 0) return C4_COLS / 2;        /* 防御: 调用方保证至少一列可下 */
    return cand[(int)rng_range(&c4_rng, (uint32_t)nc)];
}

/* 落子(调用方保证该列未满); 落点=该列最低空位 */
static void c4_drop(int col, int color) {
    int row = C4_ROWS - 1 - c4_col_h[col];
    c4_board[row][col] = (uint8_t)color;
    c4_col_h[col]++;
    c4_steps++;
    c4_last_col = col;
    c4_last_row = row;
    c4_turn = 3 - color;
    audio_move();                    /* 落子(玩家与 AI) */
}

/* 落子后判定: 四连→胜; 满盘→平。返回是否结束 */
static bool c4_check_end(void) {
    if (c4_win_at(c4_last_col, c4_last_row)) {
        c4_winner = c4_board[c4_last_row][c4_last_col];
        c4_over = true;
        if (c4_winner == 1) {
            audio_win();             /* 玩家四连 */
            led_fx_set(LED_FX_WIN);
        } else {
            audio_lose();            /* AI 四连 */
            led_fx_set(LED_FX_LOSE);
        }
        return true;
    }
    if (c4_steps >= C4_COLS * C4_ROWS) {
        c4_winner = 0;
        c4_over = true;
        return true;
    }
    return false;
}

static int c4_count(int color) {
    int n = 0;
    for (int r = 0; r < C4_ROWS; r++)
        for (int c = 0; c < C4_COLS; c++)
            if (c4_board[r][c] == color) n++;
    return n;
}

static void c4_new(void) {
    for (int r = 0; r < C4_ROWS; r++)
        for (int c = 0; c < C4_COLS; c++) c4_board[r][c] = 0;
    for (int c = 0; c < C4_COLS; c++) c4_col_h[c] = 0;
    c4_col = C4_COLS / 2;
    c4_turn = 1;
    c4_steps = 0;
    c4_last_col = -1;
    c4_last_row = -1;
    c4_winner = 0;
    c4_over = false;
    c4_over_full = false;
    rng_seed(&c4_rng, now_ms() ^ 0xC4A1u);
}

void connect4_enter(void) {
    c4_new();
    connect4_render();
    disp_full();
}

/* 棋子: 黑实心圆 / 白空心圆, 半径 7 */
static void c4_draw_stone(int col, int row, int color) {
    int cx = C4_OX + col * C4_CELL + C4_CELL / 2;
    int cy = C4_OY + row * C4_CELL + C4_CELL / 2;
    for (int dy = -7; dy <= 7; dy++)
        for (int dx = -7; dx <= 7; dx++) {
            int r2 = dx * dx + dy * dy;
            if (color == 1) {
                if (r2 <= 49) fb_pixel(cx + dx, cy + dy, true);
            } else {
                if (r2 <= 49 && r2 >= 25) fb_pixel(cx + dx, cy + dy, true);
            }
        }
}

/* 光标: 选中列反白边框(XOR, 黑/白/网格上都可见), 在所有棋子之后画 */
static void c4_xor_px(int x, int y) {
    int off = (y >> 3) * (int)CCG_W + x;
    g_fb[off] ^= (uint8_t)(0x80 >> (y & 7));
}

static void c4_draw_cursor(void) {
    int x0 = C4_OX + c4_col * C4_CELL;
    int y0 = C4_OY;
    int y1 = C4_OY + C4_BOARD_H;
    for (int t = 0; t < 2; t++) {
        for (int y = y0; y < y1; y++) {
            c4_xor_px(x0 + t, y);
            c4_xor_px(x0 + C4_CELL - 1 - t, y);
        }
        for (int x = x0; x < x0 + C4_CELL; x++) {
            c4_xor_px(x, y0 + t);
            c4_xor_px(x, y1 - 1 - t);
        }
    }
}

/* 右栏: 标签 + 2x 反白大字数值 */
static void c4_side_label(const char *name, int val, int y) {
    fb_text(CCG_W - 2 - text_width(name), y, name, true);
    char num[4];
    unsigned i = 0;
    if (val == 0) { num[i++] = '0'; }
    int v = val;
    while (v && i < 3) { num[i++] = (char)('0' + v % 10); v /= 10; }
    char rev[4];
    unsigned len = i;
    for (unsigned j = 0; j < len; j++) rev[j] = num[len - 1 - j];
    rev[len] = 0;
    int w = text_width(rev) * 2;
    fb_fill_rect(CCG_W - 8 - w, y + 8, w + 8, 16, true);
    fb_text_scale2(CCG_W - 4 - w, y + 10, rev, false);
}

void connect4_render(void) {
    fb_clear(false);
    /* 网格 + 外框 */
    for (int i = 0; i <= C4_COLS; i++)
        fb_vline(C4_OX + i * C4_CELL, C4_OY, C4_BOARD_H, true);
    for (int i = 0; i <= C4_ROWS; i++)
        fb_hline(C4_OX, C4_OY + i * C4_CELL, C4_BOARD_W, true);
    /* 棋子 */
    for (int r = 0; r < C4_ROWS; r++)
        for (int c = 0; c < C4_COLS; c++)
            if (c4_board[r][c]) c4_draw_stone(c, r, c4_board[r][c]);
    /* 光标(最后) */
    c4_draw_cursor();
    /* 底下列号 1-7 */
    for (int c = 0; c < C4_COLS; c++) {
        char d[2] = { (char)('1' + c), 0 };
        fb_text(C4_OX + c * C4_CELL + 7, C4_BOARD_H + C4_OY + 2, d, true);
    }
    /* HUD 顶栏: 左标题, 右 MOVE n */
    fb_text(0, 0, "CONNECT4", true);
    {
        char buf[12];
        unsigned i = 0;
        const char *lab = "MOVE ";
        while (lab[i]) { buf[i] = lab[i]; i++; }
        uint32_t v = (uint32_t)c4_steps;
        char num[4];
        unsigned j = 0;
        if (v == 0) { num[j++] = '0'; }
        while (v && j < 3) { num[j++] = (char)('0' + v % 10); v /= 10; }
        while (j > 0) buf[i++] = num[--j];
        buf[i] = 0;
        fb_text(CCG_W - 4 - text_width(buf), 0, buf, true);
    }
    fb_hline(0, CCG_HUD_H - 1, CCG_W, true);
    /* 左栏: 回合指示(2x 反白大字) */
    fb_text(2, 18, "TURN", true);
    {
        const char *who = (c4_turn == 1) ? "YOU" : "AI";
        int w = text_width(who) * 2;
        fb_fill_rect(2, 26, w + 8, 16, true);
        fb_text_scale2(6, 28, who, false);
    }
    /* 右栏: 双方子数 */
    c4_side_label("YOU", c4_count(1), 18);
    c4_side_label("AI", c4_count(2), 58);
    /* 底部提示 */
    fb_text(2, 139, "N:NEW", true);
    fb_text(CCG_W - 2 - text_width("BACK:MENU"), 139, "BACK:MENU", true);
    /* 结束: HUD 区结果 + 操作提示 + 全刷一次 */
    if (c4_over) {
        fb_fill_rect(0, 0, CCG_W, CCG_HUD_H - 1, false);
        const char *res = c4_winner == 1 ? "YOU WIN!" :
                          c4_winner == 2 ? "AI WINS" : "DRAW";
        fb_text(2, 2, res, true);
        fb_text(CCG_W - 2 - text_width("OK/N:RETRY BACK:QUIT"), 2,
                "OK/N:RETRY BACK:QUIT", true);
        if (!c4_over_full) { c4_over_full = true; disp_force_full(); }
    }
}

void connect4_on_key(const key_event_t *ev) {
    /* 重复事件只响应方向键(确认/字母必须忽略) */
    if (ev->is_repeat && ev->key != K_LEFT && ev->key != K_RIGHT)
        return;
    if (c4_over) {
        if (ev->key == K_OK || (ev->key == K_CHAR && ev->ch == 'n'))
            connect4_enter();
        else if (ev->key == K_BACK || ev->key == K_QUIT)
            s_exit_request = true;
        return;
    }
    switch (ev->key) {
    case K_LEFT: if (c4_col > 0) c4_col--; break;
    case K_RIGHT: if (c4_col < C4_COLS - 1) c4_col++; break;
    case K_UP:
    case K_DOWN:
        break;                              /* 选列即可, 无纵向光标 */
    case K_OK:
        if (c4_turn == 1 && c4_col_h[c4_col] < C4_ROWS) {
            c4_drop(c4_col, 1);
            if (!c4_check_end()) {
                int ac = c4_ai_best();
                c4_drop(ac, 2);
                c4_check_end();
            }
        } else {
            audio_error();           /* 列已满 */
        }
        break;
    case K_CHAR:
        if (ev->ch == 'a' && c4_col > 0) c4_col--;
        else if (ev->ch == 'd' && c4_col < C4_COLS - 1) c4_col++;
        else if (ev->ch == 'n') connect4_enter();
        break;
    case K_BACK:
    case K_PAUSE: {
        pause_sel_t sel;
        if (ui_pause_run(&sel)) {
            if (sel == PAUSE_RESTART) connect4_enter();
        } else {
            s_exit_request = true;
        }
        break;
    }
    case K_QUIT: s_exit_request = true; break;
    default: break;
    }
}

void connect4_tick(uint64_t now) { (void)now; }
void connect4_exit(void) {}
