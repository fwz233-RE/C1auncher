/* 五子棋 — 15x15, 玩家执黑先手, 启发式 AI 执白
 * 输入驱动; 落子=快刷; 开局/胜负=全刷 */
#include "game.h"
#include "../config.h"
#include "../gfx/canvas.h"
#include "../gfx/font.h"
#include "../platform/display.h"
#include "../platform/input.h"
#include "../platform/audio.h"
#include "../platform/led.h"
#include "../rng.h"
#include "../platform/time.h"
#include <stddef.h>

#define GO_N 15
#define GO_CELL_X 9
#define GO_CELL_Y 9    /* 正方形格子, 15x15=135x135 最大化 */
#define GO_OX ((CCG_W - GO_N * GO_CELL_X) / 2)
#define GO_OY (CCG_HUD_H + 1)

void gomoku_render(void);

static uint8_t g_board[GO_N][GO_N];   /* 0=空 1=黑(玩家) 2=白(AI) */
static uint8_t g_cx, g_cy;
static bool g_over, g_over_full;
static uint8_t g_winner;              /* 0=无 1=玩家 2=AI */
static uint32_t g_steps;
static int8_t g_lastx, g_lasty;       /* 最后落子(-1=无) */
static bool g_ai_thinking;
static uint8_t g_diff;                /* 0=简单 1=困难 */
static rng_t g_rng;

static int g_dx[4] = { 1, 1, 0, 1 };
static int g_dy[4] = { 0, 1, 1, -1 };

/* 方向 dir 上, 从 (x,y) 出发黑白连子数 */
static int line_len(int x, int y, int dir, uint8_t color, int *open_end) {
    int n = 1;
    int e1 = 0, e2 = 0;
    int nx = x + g_dx[dir], ny = y + g_dy[dir];
    while (nx >= 0 && nx < GO_N && ny >= 0 && ny < GO_N &&
           g_board[ny][nx] == color) {
        n++;
        nx += g_dx[dir];
        ny += g_dy[dir];
    }
    if (nx >= 0 && nx < GO_N && ny >= 0 && ny < GO_N && g_board[ny][nx] == 0) e1 = 1;
    nx = x - g_dx[dir];
    ny = y - g_dy[dir];
    while (nx >= 0 && nx < GO_N && ny >= 0 && ny < GO_N &&
           g_board[ny][nx] == color) {
        n++;
        nx -= g_dx[dir];
        ny -= g_dy[dir];
    }
    if (nx >= 0 && nx < GO_N && ny >= 0 && ny < GO_N && g_board[ny][nx] == 0) e2 = 1;
    *open_end = e1 + e2;
    return n;
}

static bool check_win_at(int x, int y) {
    uint8_t c = g_board[y][x];
    if (!c) return false;
    for (int d = 0; d < 4; d++) {
        int oe;
        if (line_len(x, y, d, c, &oe) >= 5) return true;
    }
    return false;
}

/* 启发式打分: 该格若放 color 的得分(攻+守) */
static int score_cell(int x, int y, uint8_t color) {
    uint8_t other = (color == 1) ? 2 : 1;
    int total = 0;
    for (int d = 0; d < 4; d++) {
        int oe;
        int n = line_len(x, y, d, color, &oe);
        /* 连子价值: 5=必胜, 4=活四/冲四, 3=活三/眠三, 2=活二 */
        if (n >= 5) total += 100000;
        else if (n == 4) total += (oe == 2) ? 80000 : 15000;
        else if (n == 3) total += (oe == 2) ? 8000 : 800;
        else if (n == 2) total += (oe == 2) ? 300 : 30;
        else if (n == 1) total += (oe == 2) ? 15 : 3;
        /* 防守: 对方连子威胁 */
        int pe;
        int pn = line_len(x, y, d, other, &pe);
        if (pn >= 5) total += 90000;      /* 必须堵 */
        else if (pn == 4) total += (pe == 2) ? 60000 : 12000;
        else if (pn == 3) total += (pe == 2) ? 6000 : 600;
        else if (pn == 2) total += (pe == 2) ? 200 : 20;
    }
    return total;
}

static void ai_move(void) {
    int best = -1, bx = GO_N / 2, by = GO_N / 2;
    int top5[5][3];              /* (分数, x, y) 前五优 */
    int top_n = 0;
    for (int y = 0; y < GO_N; y++)
        for (int x = 0; x < GO_N; x++) {
            if (g_board[y][x]) continue;
            int s = score_cell(x, y, 2);
            if (s > best) { best = s; bx = x; by = y; }
            /* 维护 top5 */
            if (top_n < 5) {
                top5[top_n][0] = s; top5[top_n][1] = x; top5[top_n][2] = y;
                top_n++;
            } else {
                int wi = 0;
                for (int i = 1; i < 5; i++)
                    if (top5[i][0] < top5[wi][0]) wi = i;
                if (s > top5[wi][0]) {
                    top5[wi][0] = s; top5[wi][1] = x; top5[wi][2] = y;
                }
            }
        }
    if (g_diff == 0 && best < 90000 && top_n > 1) {
        /* 简单难度: 前 5 优中随机(有逻辑, 关键棋不随机) */
        int k = (int)rng_range(&g_rng, (uint32_t)top_n);
        bx = top5[k][1];
        by = top5[k][2];
    }
    g_board[by][bx] = 2;
    g_lastx = (int8_t)bx;
    g_lasty = (int8_t)by;
    g_steps++;
    if (check_win_at(bx, by)) {
        g_winner = 2;
        g_over = true;
        audio_lose();
        led_fx_set(LED_FX_LOSE);
    }
}

void gomoku_enter(void) {
    for (int y = 0; y < GO_N; y++)
        for (int x = 0; x < GO_N; x++) g_board[y][x] = 0;
    g_cx = GO_N / 2;
    g_cy = GO_N / 2;
    g_over = false;
    g_over_full = false;
    g_winner = 0;
    g_steps = 0;
    g_lastx = -1;
    g_lasty = -1;
    g_ai_thinking = false;
    g_diff = 0;
    rng_seed(&g_rng, now_ms() ^ 0x60D);
    gomoku_render();
    disp_full();
}

static void draw_stone(int x, int y, uint8_t c) {
    int cx = GO_OX + x * GO_CELL_X + GO_CELL_X / 2;
    int cy = GO_OY + y * GO_CELL_Y + GO_CELL_Y / 2;
    if (c == 1) {
        /* 黑子: 实心圆 r3 */
        for (int dy = -3; dy <= 3; dy++)
            for (int dx = -3; dx <= 3; dx++)
                if (dx * dx + dy * dy <= 9) fb_pixel(cx + dx, cy + dy, true);
    } else {
        /* 白子: 空心圆 r3 */
        for (int dy = -3; dy <= 3; dy++)
            for (int dx = -3; dx <= 3; dx++) {
                int r2 = dx * dx + dy * dy;
                if (r2 <= 9 && r2 >= 5) fb_pixel(cx + dx, cy + dy, true);
            }
    }
}

void gomoku_render(void) {
    fb_clear(false);
    /* 棋盘线 + 星位 */
    for (int i = 0; i < GO_N; i++) {
        fb_vline(GO_OX + i * GO_CELL_X + GO_CELL_X / 2, GO_OY + GO_CELL_Y / 2,
                 (GO_N - 1) * GO_CELL_Y, true);
        fb_hline(GO_OX + GO_CELL_X / 2, GO_OY + i * GO_CELL_Y + GO_CELL_Y / 2,
                 (GO_N - 1) * GO_CELL_X, true);
    }
    static const int stars[5][2] = { {3,3}, {11,3}, {7,7}, {3,11}, {11,11} };
    for (int i = 0; i < 5; i++) {
        int sx = GO_OX + stars[i][0] * GO_CELL_X + GO_CELL_X / 2;
        int sy = GO_OY + stars[i][1] * GO_CELL_Y + GO_CELL_Y / 2;
        fb_fill_rect(sx - 1, sy - 1, 3, 3, true);
    }
    /* 棋子 */
    for (int y = 0; y < GO_N; y++)
        for (int x = 0; x < GO_N; x++)
            if (g_board[y][x]) draw_stone(x, y, g_board[y][x]);
    /* 光标: 反色边框 */
    {
        int cx = GO_OX + g_cx * GO_CELL_X;
        int cy = GO_OY + g_cy * GO_CELL_Y;
        fb_stroke_rect_thick(cx + 1, cy + 1, GO_CELL_X - 2, GO_CELL_Y - 2, 1, false);
    }
    fb_text(0, 0, "GOMOKU", true);
    fb_hline(0, CCG_HUD_H - 1, CCG_W, true);
    /* 左栏: 步数(大号) + 回合指示 */
    {
        char buf[16];
        uint32_t v = g_steps;
        unsigned i = 0;
        if (v == 0) { buf[i++] = '0'; }
        while (v && i < 14) { buf[i++] = (char)('0' + v % 10); v /= 10; }
        char rev[16];
        unsigned len = i;
        for (unsigned j = 0; j < len; j++) rev[j] = buf[len - 1 - j];
        rev[len] = 0;
        fb_text(2, 18, "MOVE", true);
        {
            int w = text_width(rev) * 2;
            fb_fill_rect(0, 26, w + 8, 16, true);
            fb_text_scale2(4, 28, rev, false);
        }
        fb_hline(2, 52, 72, true);
        fb_text(2, 58, "TURN", true);
        /* 玩家(黑) 指示: 当前回合反白 */
        bool you = !g_over && !g_ai_thinking;
        if (you) fb_fill_rect(2, 82, 76, 15, true);
        for (int dy = -3; dy <= 3; dy++)
            for (int dx = -3; dx <= 3; dx++)
                if (dx * dx + dy * dy <= 9) fb_pixel(10 + dx, 90 + dy, !you);
        fb_text(20, 84, "YOU", you ? false : true);
        /* AI(白) 指示 */
        bool ai = !g_over && g_ai_thinking;
        if (ai) fb_fill_rect(2, 100, 76, 15, true);
        for (int dy = -3; dy <= 3; dy++)
            for (int dx = -3; dx <= 3; dx++) {
                int r2 = dx * dx + dy * dy;
                if (r2 <= 9 && r2 >= 5) fb_pixel(10 + dx, 108 + dy, !ai);
            }
        fb_text(20, 102, "AI", ai ? false : true);
    }
    /* 右栏: 最后落子 + 难度(大号) */
    {
        fb_text(CCG_W - 2 - text_width("LAST"), 18, "LAST", true);
        if (g_lastx >= 0) {
            char buf[12];
            buf[0] = (char)('A' + g_lastx);
            buf[1] = (char)('0' + (g_lasty + 1) / 10);
            buf[2] = (char)('0' + (g_lasty + 1) % 10);
            buf[3] = 0;
            int w = text_width(buf) * 2;
            fb_fill_rect(CCG_W - 8 - w, 26, w + 8, 16, true);
            fb_text_scale2(CCG_W - 4 - w, 28, buf, false);
        } else {
            fb_text_scale2(CCG_W - 2 - text_width("--") * 2, 28, "--", false);
        }
        fb_hline(CCG_W - 74, 52, 72, true);
        fb_text(CCG_W - 2 - text_width("DIFF"), 58, "DIFF", true);
        const char *d = g_diff ? "HARD" : "EASY";
        int dw = text_width(d) * 2;
        fb_fill_rect(CCG_W - 8 - dw, 68, dw + 8, 16, true);
        fb_text_scale2(CCG_W - 4 - dw, 70, d, false);
    }
    if (g_over) {
        fb_fill_rect(0, 0, CCG_W, CCG_HUD_H - 1, false);
        fb_text(2, 2, g_winner == 1 ? "YOU WIN!" : "AI WINS", true);
        fb_text(CCG_W - 2 - text_width("OK/N:RETRY BACK:QUIT"), 2,
                "OK/N:RETRY BACK:QUIT", true);
        if (!g_over_full) { g_over_full = true; disp_force_full(); }
    }
}

void gomoku_on_key(const key_event_t *ev) {
    if (ev->is_repeat) return;
    if (g_over) {
        if (ev->key == K_OK || (ev->key == K_CHAR && ev->ch == 'n'))
            gomoku_enter();
        else if (ev->key == K_BACK || ev->key == K_QUIT)
            s_exit_request = true;
        return;
    }
    switch (ev->key) {
    case K_LEFT: if (g_cx > 0) g_cx--; break;
    case K_RIGHT: if (g_cx < GO_N - 1) g_cx++; break;
    case K_UP: if (g_cy > 0) g_cy--; break;
    case K_DOWN: if (g_cy < GO_N - 1) g_cy++; break;
    case K_CHAR:
        if (ev->ch == 'a' && g_cx > 0) g_cx--;
        else if (ev->ch == 'd' && g_cx < GO_N - 1) g_cx++;
        else if (ev->ch == 'w' && g_cy > 0) g_cy--;
        else if (ev->ch == 's' && g_cy < GO_N - 1) g_cy++;
        else if (ev->ch == 'n') gomoku_enter();
        else if (ev->ch == 'f') g_diff = g_diff ? 0 : 1;
        break;
    case K_OK:
        if (!g_board[g_cy][g_cx]) {
            g_board[g_cy][g_cx] = 1;
            g_lastx = (int8_t)g_cx;
            g_lasty = (int8_t)g_cy;
            g_steps++;
            if (check_win_at(g_cx, g_cy)) {
                g_winner = 1;
                g_over = true;
                audio_win();
                led_fx_set(LED_FX_WIN);
            } else {
                audio_select();
                g_ai_thinking = true;
                ai_move();
                g_ai_thinking = false;
            }
        } else {
            audio_error();
        }
        break;
    case K_BACK: {
        pause_sel_t sel;
        if (ui_pause_run(&sel)) {
            if (sel == PAUSE_RESTART) gomoku_enter();
        } else {
            s_exit_request = true;
        }
        break;
    }
    case K_QUIT: s_exit_request = true; break;
    default: break;
    }
}

void gomoku_tick(uint64_t now) { (void)now; }
void gomoku_exit(void) {}
