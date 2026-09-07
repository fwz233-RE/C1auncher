/* 黑白棋(Reversi/Othello) — 8x8 格 16px 方形(128x128 居中, y 从 18 起)
 * 玩家执黑先手 vs AI 执白; 标准翻转规则; 无合法落子自动跳过
 * AI: 启发式逐格打分(角+50 / 边+10 / 邻角-20 / 翻转数+1), 无搜索
 * 输入驱动; 落子/移动=快刷; 开局/结束=全刷 */
#include "game.h"
#include "../config.h"
#include "../gfx/canvas.h"
#include "../gfx/font.h"
#include "../platform/display.h"
#include "../platform/input.h"
#include "../platform/audio.h"
#include "../platform/led.h"

#define RV_N 8
#define RV_CELL 16
#define RV_BOARD (RV_N * RV_CELL)              /* 128 */
#define RV_OX ((CCG_W - RV_BOARD) / 2)         /* 84, 水平居中 */
#define RV_OY 18                               /* 顶栏 16px 下留 2px */

void reversi_render(void);

static uint8_t rv_board[RV_N][RV_N];  /* 0=空 1=黑(玩家) 2=白(AI) */
static int rv_turn;                   /* 当前回合 1=玩家 2=AI */
static int rv_cx, rv_cy;              /* 光标(格坐标) */
static bool rv_over, rv_over_full;

/* 计算 color 落在 (x,y) 的翻转数; apply=true 时执行翻转(不含落子,
 * 落子由调用方在翻转后单独放置, 避免占用判定)
 * 返回 0 表示非法(占用/无翻转) */
static int rv_flips_at(int x, int y, int color, bool apply) {
    static const int dx8[8] = { 1, 1, 0, -1, -1, -1, 0, 1 };
    static const int dy8[8] = { 0, 1, 1, 1, 0, -1, -1, -1 };
    int opp = 3 - color;
    int total = 0;
    if (x < 0 || x >= RV_N || y < 0 || y >= RV_N) return 0;
    if (rv_board[y][x] != 0) return 0;
    for (int d = 0; d < 8; d++) {
        int nx = x + dx8[d], ny = y + dy8[d];
        if (nx < 0 || nx >= RV_N || ny < 0 || ny >= RV_N) continue;
        if (rv_board[ny][nx] != opp) continue;
        int cnt = 0;
        while (nx >= 0 && nx < RV_N && ny >= 0 && ny < RV_N &&
               rv_board[ny][nx] == opp) {
            nx += dx8[d];
            ny += dy8[d];
            cnt++;
        }
        if (nx >= 0 && nx < RV_N && ny >= 0 && ny < RV_N &&
            rv_board[ny][nx] == color && cnt > 0) {
            total += cnt;
            if (apply) {
                int fx = x + dx8[d], fy = y + dy8[d];
                for (int i = 0; i < cnt; i++) {
                    rv_board[fy][fx] = (uint8_t)color;
                    fx += dx8[d];
                    fy += dy8[d];
                }
            }
        }
    }
    return total;
}

static bool rv_has_any(int color) {
    for (int y = 0; y < RV_N; y++)
        for (int x = 0; x < RV_N; x++)
            if (rv_board[y][x] == 0 && rv_flips_at(x, y, color, false) > 0)
                return true;
    return false;
}

/* 落子后推进回合: 对方有合法步→对方走; 对方无→己方再走(对方被跳过);
 * 双方均无合法步→对局结束。返回 false = 结束 */
static bool rv_advance(void) {
    int cur = rv_turn, opp = 3 - cur;
    if (rv_has_any(opp)) { rv_turn = opp; return true; }
    if (rv_has_any(cur)) { rv_turn = cur; return true; }
    rv_over = true;
    return false;
}

static int rv_count(int color) {
    int n = 0;
    for (int y = 0; y < RV_N; y++)
        for (int x = 0; x < RV_N; x++)
            if (rv_board[y][x] == color) n++;
    return n;
}

/* AI 启发式打分: 翻转数 + 角/边/邻角惩罚 */
static int rv_ai_score(int x, int y, int flips) {
    static const int corners[4][2] = { {0,0}, {0,7}, {7,0}, {7,7} };
    int s = flips;
    bool corner = (x == 0 || x == 7) && (y == 0 || y == 7);
    bool edge = (x == 0 || x == 7 || y == 0 || y == 7);
    if (corner) s += 50;
    else if (edge) s += 10;
    bool near = false;
    for (int i = 0; i < 4; i++) {
        int dx = x - corners[i][0];
        int dy = y - corners[i][1];
        if (dx < 0) dx = -dx;
        if (dy < 0) dy = -dy;
        if (dx <= 1 && dy <= 1 && !(dx == 0 && dy == 0)) near = true;
    }
    if (near) s -= 20;
    return s;
}

/* AI 走一步; 返回 false 表示无合法步(调用方保证不会发生)
 * 注意: 打分可为负(邻角 -20), 首候选用 bx<0 判定而非 best 初值 */
static bool rv_ai_move(void) {
    int best = 0, bx = -1, by = -1;
    for (int y = 0; y < RV_N; y++)
        for (int x = 0; x < RV_N; x++) {
            if (rv_board[y][x] != 0) continue;
            int f = rv_flips_at(x, y, 2, false);
            if (f <= 0) continue;
            int s = rv_ai_score(x, y, f);
            if (bx < 0 || s > best) { best = s; bx = x; by = y; }
        }
    if (bx < 0) return false;
    rv_flips_at(bx, by, 2, true);   /* 先翻转(格子仍空), 后落子 */
    rv_board[by][bx] = 2;
    return true;
}

/* 玩家落子 + AI 应答(可能连走, 玩家被跳过时) */
static void rv_player_move(int x, int y) {
    if (rv_flips_at(x, y, 1, false) <= 0) {         /* 非法落子: 无效果 */
        audio_error();
        return;
    }
    rv_flips_at(x, y, 1, true);                     /* 先翻转 */
    rv_board[y][x] = 1;                             /* 后落子 */
    audio_select();
    rv_advance();
    while (!rv_over && rv_turn == 2) {
        if (!rv_ai_move()) break;   /* 防御: 逻辑上不可达 */
        rv_advance();
    }
    if (rv_over) {                                  /* 终局: 按子数定胜负 */
        int b = rv_count(1), w = rv_count(2);
        if (b > w) {
            audio_win();
            led_fx_set(LED_FX_WIN);
        } else if (w > b) {
            audio_lose();
            led_fx_set(LED_FX_LOSE);
        }
    }
}

static void rv_new(void) {
    for (int y = 0; y < RV_N; y++)
        for (int x = 0; x < RV_N; x++) rv_board[y][x] = 0;
    /* 标准开局: 黑 e4/d5, 白 d4/e5 (0 起下标) */
    rv_board[3][3] = 2;
    rv_board[4][4] = 2;
    rv_board[4][3] = 1;
    rv_board[3][4] = 1;
    rv_cx = 3;
    rv_cy = 3;
    rv_turn = 1;
    rv_over = false;
    rv_over_full = false;
    while (!rv_over && !rv_has_any(rv_turn)) rv_advance();  /* 起始跳过 */
}

void reversi_enter(void) {
    rv_new();
    reversi_render();
    disp_full();
}

/* 棋子: 黑实心圆 / 白空心圆, 半径 5 */
static void rv_draw_stone(int x, int y, int color) {
    int cx = RV_OX + x * RV_CELL + RV_CELL / 2;
    int cy = RV_OY + y * RV_CELL + RV_CELL / 2;
    for (int dy = -5; dy <= 5; dy++)
        for (int dx = -5; dx <= 5; dx++) {
            int r2 = dx * dx + dy * dy;
            if (color == 1) {
                if (r2 <= 25) fb_pixel(cx + dx, cy + dy, true);
            } else {
                if (r2 <= 25 && r2 >= 9) fb_pixel(cx + dx, cy + dy, true);
            }
        }
}

/* 光标: 反色边框(黑格白边/白格黑边), 在所有棋子之后绘制 */
static void rv_draw_cursor(void) {
    int x = RV_OX + rv_cx * RV_CELL;
    int y = RV_OY + rv_cy * RV_CELL;
    bool inv = (rv_board[rv_cy][rv_cx] == 1) ? false : true;
    fb_stroke_rect_thick(x, y, RV_CELL, RV_CELL, 2, inv);
}

/* "NAME n" 计数文本(0..64, 至多 2 位) */
static void rv_label(char *buf, const char *name, int v) {
    int n = 0;
    while (name[n]) { buf[n] = name[n]; n++; }
    buf[n++] = ' ';
    if (v >= 10) buf[n++] = (char)('0' + v / 10);
    buf[n++] = (char)('0' + v % 10);
    buf[n] = 0;
}

void reversi_render(void) {
    fb_clear(false);
    /* 棋盘网格 9x9 线 */
    for (int i = 0; i <= RV_N; i++) {
        fb_vline(RV_OX + i * RV_CELL, RV_OY, RV_BOARD, true);
        fb_hline(RV_OX, RV_OY + i * RV_CELL, RV_BOARD, true);
    }
    /* 棋子 */
    for (int y = 0; y < RV_N; y++)
        for (int x = 0; x < RV_N; x++)
            if (rv_board[y][x]) rv_draw_stone(x, y, rv_board[y][x]);
    /* 玩家回合: 合法落子提示(小点) */
    if (!rv_over && rv_turn == 1) {
        for (int y = 0; y < RV_N; y++)
            for (int x = 0; x < RV_N; x++)
                if (rv_board[y][x] == 0 && rv_flips_at(x, y, 1, false) > 0) {
                    int cx = RV_OX + x * RV_CELL + RV_CELL / 2;
                    int cy = RV_OY + y * RV_CELL + RV_CELL / 2;
                    fb_fill_rect(cx - 1, cy - 1, 2, 2, true);
                }
    }
    rv_draw_cursor();
    /* HUD 顶栏: 左标题, 右 BLACK/WHITE 计数两行 */
    fb_text(0, 0, "REVERSI", true);
    {
        char buf[16];
        rv_label(buf, "BLACK", rv_count(1));
        fb_text(CCG_W - 4 - text_width(buf), 0, buf, true);
        rv_label(buf, "WHITE", rv_count(2));
        fb_text(CCG_W - 4 - text_width(buf), 8, buf, true);
    }
    fb_hline(0, CCG_HUD_H - 1, CCG_W, true);
    /* 左栏: 回合指示 */
    fb_text(2, 30, "TURN", true);
    {
        int icx = 10, icy = 44;
        if (rv_turn == 1) {
            for (int dy = -4; dy <= 4; dy++)
                for (int dx = -4; dx <= 4; dx++)
                    if (dx * dx + dy * dy <= 16)
                        fb_pixel(icx + dx, icy + dy, true);
        } else {
            for (int dy = -4; dy <= 4; dy++)
                for (int dx = -4; dx <= 4; dx++) {
                    int r2 = dx * dx + dy * dy;
                    if (r2 <= 16 && r2 >= 7)
                        fb_pixel(icx + dx, icy + dy, true);
                }
        }
        fb_text(22, 42, rv_turn == 1 ? "YOU" : "AI", true);
    }
    fb_text(2, 128, "N:NEW", true);
    fb_text(2, 138, "BACK:MENU", true);
    /* 结束: HUD 区结果 + 操作提示 */
    if (rv_over) {
        fb_fill_rect(0, 0, CCG_W, CCG_HUD_H - 1, false);
        int b = rv_count(1), w = rv_count(2);
        const char *res = (b > w) ? "YOU WIN!" : (w > b) ? "AI WINS" : "DRAW";
        fb_text(2, 2, res, true);
        fb_text(CCG_W - 2 - text_width("OK/N:RETRY BACK:QUIT"), 2,
                "OK/N:RETRY BACK:QUIT", true);
        if (!rv_over_full) { rv_over_full = true; disp_force_full(); }
    }
}

void reversi_on_key(const key_event_t *ev) {
    /* 重复事件只响应方向键(确认/字母必须忽略) */
    if (ev->is_repeat && ev->key != K_LEFT && ev->key != K_RIGHT &&
        ev->key != K_UP && ev->key != K_DOWN)
        return;
    if (rv_over) {
        if (ev->key == K_OK || (ev->key == K_CHAR && ev->ch == 'n'))
            reversi_enter();
        else if (ev->key == K_BACK || ev->key == K_QUIT)
            s_exit_request = true;
        return;
    }
    switch (ev->key) {
    case K_LEFT: if (rv_cx > 0) rv_cx--; break;
    case K_RIGHT: if (rv_cx < RV_N - 1) rv_cx++; break;
    case K_UP: if (rv_cy > 0) rv_cy--; break;
    case K_DOWN: if (rv_cy < RV_N - 1) rv_cy++; break;
    case K_OK:
        if (rv_turn == 1) rv_player_move(rv_cx, rv_cy);
        break;
    case K_CHAR:
        if (ev->ch == 'a' && rv_cx > 0) rv_cx--;
        else if (ev->ch == 'd' && rv_cx < RV_N - 1) rv_cx++;
        else if (ev->ch == 'w' && rv_cy > 0) rv_cy--;
        else if (ev->ch == 's' && rv_cy < RV_N - 1) rv_cy++;
        else if (ev->ch == 'n') reversi_enter();
        break;
    case K_BACK:
    case K_PAUSE: {
        pause_sel_t sel;
        if (ui_pause_run(&sel)) {
            if (sel == PAUSE_RESTART) reversi_enter();
        } else {
            s_exit_request = true;
        }
        break;
    }
    case K_QUIT: s_exit_request = true; break;
    default: break;
    }
}

void reversi_tick(uint64_t now) { (void)now; }
void reversi_exit(void) {}
