/* ISOLA — 7x7 棋盘博弈 vs AI
 * 规则: 轮流 1) 移己棋到相邻格(8向, 含斜角) 2) 拆除一块非棋所在的地板格
 *       先无路可走者输; 地板=空白格, 拆除=实心黑格
 * 玩家=实心圆, AI=空心圆; 19px 格 133x133 居中
 * 操作: 方向移光标; OK 移动(自动切拆除模式) / OK 拆除(切 AI 回合); HUD+侧栏指示
 * AI: 1 层搜索 (move,remove) 组合: 5x 己方机动 - 6x 对手机动, 平手随机(rng_seed 防零)
 * 输入驱动 + tick 延迟展示 AI 思考; 移动/拆除=快刷, 开局/结束=全刷 */
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

#define IS_N 7
#define IS_CELL 19
#define IS_BOARD (IS_N * IS_CELL)           /* 133 */
#define IS_OX ((CCG_W - IS_BOARD) / 2)      /* 81, 水平居中 */
#define IS_OY 18                            /* 顶栏 16px 下留 2px */
#define IS_PLAYER 1
#define IS_AI 2
#define IS_AI_MS 800                        /* AI 思考展示延迟 */
#define IS_PH_MOVE 0
#define IS_PH_REMOVE 1
#define IS_PH_THINK 2

static uint8_t is_board[IS_N][IS_N];        /* 0=地板 1=拆除 */
static int is_px, is_py;                    /* 玩家棋 */
static int is_ax, is_ay;                    /* AI 棋 */
static int is_cx, is_cy;                    /* 光标 */
static int is_phase;                        /* MOVE/REMOVE/THINK */
static int is_turn;                         /* 1=玩家 2=AI */
static int is_winner;                       /* 0=无 1=玩家 2=AI */
static bool is_over, is_over_full;
static uint64_t is_think_at;                /* AI 思考开始时刻 */
static int is_lax, is_lay;                  /* AI 上一步落子格(-1=无) */
static rng_t is_rng;

void isola_render(void);

/* ---- 规则查询 ---- */
static bool is_occupied(int x, int y) {
    return (x == is_px && y == is_py) || (x == is_ax && y == is_ay);
}

/* 棋(x,y)可行走的地板格数(不能踩对方棋) */
static int is_mobility(int x, int y, int oppx, int oppy) {
    int n = 0;
    for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++) {
            if (dx == 0 && dy == 0) continue;
            int nx = x + dx, ny = y + dy;
            if (nx < 0 || nx >= IS_N || ny < 0 || ny >= IS_N) continue;
            if (is_board[ny][nx]) continue;
            if (nx == oppx && ny == oppy) continue;
            n++;
        }
    return n;
}

static int is_player_mobility(void) { return is_mobility(is_px, is_py, is_ax, is_ay); }
static int is_ai_mobility(void) { return is_mobility(is_ax, is_ay, is_px, is_py); }

/* 玩家 MOVE 模式: 光标所在格是否合法移动目标 */
static bool is_valid_move_target(int x, int y) {
    int dx = x - is_px, dy = y - is_py;
    if (dx < -1 || dx > 1 || dy < -1 || dy > 1 || (dx == 0 && dy == 0))
        return false;
    if (is_board[y][x]) return false;
    if (x == is_ax && y == is_ay) return false;
    return true;
}

/* REMOVE 模式: 可拆除格(地板且无棋) */
static bool is_removable(int x, int y) {
    if (is_board[y][x]) return false;
    return !is_occupied(x, y);
}

static bool is_has_removable(void) {
    for (int y = 0; y < IS_N; y++)
        for (int x = 0; x < IS_N; x++)
            if (is_removable(x, y)) return true;
    return false;
}

/* 光标吸附到首个合法移动目标(玩家回合开始) */
static void is_snap_target(void) {
    if (is_valid_move_target(is_cx, is_cy)) return;
    for (int y = 0; y < IS_N; y++)
        for (int x = 0; x < IS_N; x++)
            if (is_valid_move_target(x, y)) { is_cx = x; is_cy = y; return; }
}

/* ---- AI: 1 层启发式 (move,remove) 全组合 ----
 * 分数 = 5*己方机动 - 6*对手机动: 优先保持自己活动空间 + 拆对手周围格 */
static void is_ai_play(void) {
    int best = -1000000;
    int mv[16][2], rm[16][2];               /* 等优组合(平手随机) */
    int n = 0;
    /* 第一遍: 求最优分 */
    for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++) {
            if (dx == 0 && dy == 0) continue;
            int mx = is_ax + dx, my = is_ay + dy;
            if (mx < 0 || mx >= IS_N || my < 0 || my >= IS_N) continue;
            if (is_board[my][mx]) continue;
            if (mx == is_px && my == is_py) continue;
            is_ax = mx;
            is_ay = my;
            bool any_rm = false;
            for (int ry = 0; ry < IS_N; ry++)
                for (int rx = 0; rx < IS_N; rx++) {
                    if (!is_removable(rx, ry)) continue;
                    any_rm = true;
                    is_board[ry][rx] = 1;
                    int sc = 5 * is_ai_mobility() - 6 * is_player_mobility();
                    is_board[ry][rx] = 0;
                    if (sc > best) best = sc;
                }
            if (!any_rm) {                  /* 无格可拆: 只移动 */
                int sc = 5 * is_ai_mobility() - 6 * is_player_mobility();
                if (sc > best) best = sc;
            }
            is_ax -= dx;
            is_ay -= dy;
        }
    /* 第二遍: 收集等优组合 */
    for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++) {
            if (dx == 0 && dy == 0) continue;
            int mx = is_ax + dx, my = is_ay + dy;
            if (mx < 0 || mx >= IS_N || my < 0 || my >= IS_N) continue;
            if (is_board[my][mx]) continue;
            if (mx == is_px && my == is_py) continue;
            is_ax = mx;
            is_ay = my;
            bool any_rm = false;
            for (int ry = 0; ry < IS_N; ry++)
                for (int rx = 0; rx < IS_N; rx++) {
                    if (!is_removable(rx, ry)) continue;
                    any_rm = true;
                    is_board[ry][rx] = 1;
                    int sc = 5 * is_ai_mobility() - 6 * is_player_mobility();
                    is_board[ry][rx] = 0;
                    if (sc == best && n < 16) {
                        mv[n][0] = mx; mv[n][1] = my;
                        rm[n][0] = rx; rm[n][1] = ry;
                        n++;
                    }
                }
            if (!any_rm && n < 16) {
                int sc = 5 * is_ai_mobility() - 6 * is_player_mobility();
                if (sc == best) {
                    mv[n][0] = mx; mv[n][1] = my;
                    rm[n][0] = -1; rm[n][1] = -1;
                    n++;
                }
            }
            is_ax -= dx;
            is_ay -= dy;
        }
    if (n == 0) {                           /* 防御: 调用方保证有路, 兜底不动 */
        mv[0][0] = is_ax; mv[0][1] = is_ay;
        rm[0][0] = -1; rm[0][1] = -1;
        n = 1;
    }
    int pick = (int)rng_range(&is_rng, (uint32_t)n);
    is_ax = mv[pick][0];
    is_ay = mv[pick][1];
    if (rm[pick][0] >= 0) is_board[rm[pick][1]][rm[pick][0]] = 1;
    is_lax = mv[pick][0];
    is_lay = mv[pick][1];
}

/* ---- 开局 ---- */
static void is_new(void) {
    for (int y = 0; y < IS_N; y++)
        for (int x = 0; x < IS_N; x++) is_board[y][x] = 0;
    is_px = 0;
    is_py = IS_N / 2;
    is_ax = IS_N - 1;
    is_ay = IS_N / 2;
    is_phase = IS_PH_MOVE;
    is_turn = IS_PLAYER;
    is_winner = 0;
    is_over = false;
    is_over_full = false;
    is_lax = -1;
    is_lay = -1;
    is_cx = is_px;
    is_cy = is_py;
    rng_seed(&is_rng, now_ms() ^ 0x1501Au);
    is_snap_target();
}

void isola_enter(void) {
    is_new();
    isola_render();
    disp_full();
}

void isola_exit(void) {}

/* ---- 绘制 ---- */
/* 棋子: 玩家实心圆, AI 空心圆, 半径 6 */
static void is_draw_piece(int x, int y, int kind) {
    int cx = IS_OX + x * IS_CELL + IS_CELL / 2;
    int cy = IS_OY + y * IS_CELL + IS_CELL / 2;
    for (int dy = -6; dy <= 6; dy++)
        for (int dx = -6; dx <= 6; dx++) {
            int r2 = dx * dx + dy * dy;
            if (kind == IS_PLAYER) {
                if (r2 <= 36) fb_pixel(cx + dx, cy + dy, true);
            } else {
                if (r2 <= 36 && r2 >= 16) fb_pixel(cx + dx, cy + dy, true);
            }
        }
}

static void is_xor_px(int x, int y) {
    if (x < 0 || x >= (int)CCG_W || y < 0 || y >= (int)CCG_H) return;
    int off = (y >> 3) * (int)CCG_W + x;
    g_fb[off] ^= (uint8_t)(0x80 >> (y & 7));
}

/* 光标: 选中格 2px 反白边框(所有元素之后画, 黑/白上都可见) */
static void is_draw_cursor(void) {
    int x0 = IS_OX + is_cx * IS_CELL;
    int y0 = IS_OY + is_cy * IS_CELL;
    for (int t = 0; t < 2; t++) {
        for (int i = 0; i < IS_CELL; i++) {
            is_xor_px(x0 + i, y0 + t);
            is_xor_px(x0 + i, y0 + IS_CELL - 1 - t);
            is_xor_px(x0 + t, y0 + i);
            is_xor_px(x0 + IS_CELL - 1 - t, y0 + i);
        }
    }
}

/* MOVE 模式: 合法移动目标 4 角 3px 角标 */
static void is_draw_targets(void) {
    for (int y = 0; y < IS_N; y++)
        for (int x = 0; x < IS_N; x++) {
            if (!is_valid_move_target(x, y)) continue;
            int x0 = IS_OX + x * IS_CELL;
            int y0 = IS_OY + y * IS_CELL;
            for (int a = 0; a < 3; a++)
                for (int b = 0; b < 3; b++) {
                    fb_pixel(x0 + a, y0 + b, true);
                    fb_pixel(x0 + IS_CELL - 1 - a, y0 + b, true);
                    fb_pixel(x0 + a, y0 + IS_CELL - 1 - b, true);
                    fb_pixel(x0 + IS_CELL - 1 - a, y0 + IS_CELL - 1 - b, true);
                }
        }
}

/* AI 上一步落子格: 1px 反白边框标记 */
static void is_draw_last_ai_move(void) {
    int x0 = IS_OX + is_lax * IS_CELL;
    int y0 = IS_OY + is_lay * IS_CELL;
    for (int i = 0; i < IS_CELL; i++) {
        is_xor_px(x0 + i, y0);
        is_xor_px(x0 + i, y0 + IS_CELL - 1);
        is_xor_px(x0, y0 + i);
        is_xor_px(x0 + IS_CELL - 1, y0 + i);
    }
}

/* 侧栏: 小字标签 + 2x 反白大字值 */
static void is_side_block(const char *label, const char *val, int y) {
    fb_text(2, y, label, true);
    int w = text_width(val) * 2;
    fb_fill_rect(2, y + 8, w + 8, 16, true);
    fb_text_scale2(6, y + 10, val, false);
}

void isola_render(void) {
    fb_clear(false);
    /* HUD 顶栏: 左标题黑字白底, 右模式标签 */
    if (is_over) {
        fb_fill_rect(0, 0, CCG_W, CCG_HUD_H - 1, false);
        const char *res = (is_winner == IS_PLAYER) ? "YOU WIN!" : "AI WINS";
        fb_text(2, 2, res, true);
        fb_text(CCG_W - 2 - text_width("OK/N:RETRY BACK:QUIT"), 2,
                "OK/N:RETRY BACK:QUIT", true);
    } else {
        fb_text(2, 0, "ISOLA", true);
        const char *mode = is_phase == IS_PH_MOVE ? "MOVE" :
                           is_phase == IS_PH_REMOVE ? "REMOVE" : "AI THINK";
        fb_text(CCG_W - 2 - text_width(mode), 0, mode, true);
    }
    fb_hline(0, CCG_HUD_H - 1, CCG_W, true);
    /* 网格 */
    for (int i = 0; i <= IS_N; i++) {
        fb_vline(IS_OX + i * IS_CELL, IS_OY, IS_BOARD, true);
        fb_hline(IS_OX, IS_OY + i * IS_CELL, IS_BOARD, true);
    }
    /* 拆除格(实心黑格, 留 1px 网格边) */
    for (int y = 0; y < IS_N; y++)
        for (int x = 0; x < IS_N; x++)
            if (is_board[y][x])
                fb_fill_rect(IS_OX + x * IS_CELL + 1, IS_OY + y * IS_CELL + 1,
                             IS_CELL - 1, IS_CELL - 1, true);
    /* 合法目标角标(所有元素之前画) */
    if (!is_over && is_phase == IS_PH_MOVE) is_draw_targets();
    /* 棋子 */
    is_draw_piece(is_px, is_py, IS_PLAYER);
    is_draw_piece(is_ax, is_ay, IS_AI);
    /* AI 上一步落点标记 */
    if (!is_over && is_lax >= 0) is_draw_last_ai_move();
    /* 光标(最后画) */
    if (!is_over) is_draw_cursor();
    /* 侧栏 */
    if (!is_over) {
        is_side_block("TURN", is_turn == IS_PLAYER ? "YOU" : "AI", 18);
        const char *mv = is_phase == IS_PH_MOVE ? "MOVE" :
                         is_phase == IS_PH_REMOVE ? "REMOVE" : "THINK";
        is_side_block("MODE", mv, 46);
    }
    /* 结束: 全刷一次 */
    if (is_over && !is_over_full) { is_over_full = true; disp_force_full(); }
}

/* ---- 输入 ---- */
void isola_on_key(const key_event_t *ev) {
    /* 重复事件只响应方向键 */
    if (ev->is_repeat && ev->key != K_LEFT && ev->key != K_RIGHT &&
        ev->key != K_UP && ev->key != K_DOWN)
        return;
    if (is_over) {
        if (ev->key == K_OK || (ev->key == K_CHAR && ev->ch == 'n'))
            isola_enter();
        else if (ev->key == K_BACK || ev->key == K_QUIT)
            s_exit_request = true;
        return;
    }
    switch (ev->key) {
    case K_LEFT:
    case K_RIGHT:
    case K_UP:
    case K_DOWN:
        if (is_phase != IS_PH_THINK) {
            int dx = 0, dy = 0;
            if (ev->key == K_LEFT) dx = -1;
            else if (ev->key == K_RIGHT) dx = 1;
            else if (ev->key == K_UP) dy = -1;
            else dy = 1;
            int nx = is_cx + dx, ny = is_cy + dy;
            if (nx >= 0 && nx < IS_N && ny >= 0 && ny < IS_N) {
                is_cx = nx;
                is_cy = ny;
            }
        }
        break;
    case K_OK:
        if (is_phase == IS_PH_MOVE) {
            if (is_valid_move_target(is_cx, is_cy)) {
                is_px = is_cx;
                is_py = is_cy;
                audio_move();
                if (is_has_removable()) {
                    is_phase = IS_PH_REMOVE;
                } else if (is_ai_mobility() == 0) {
                    is_winner = IS_PLAYER;
                    is_over = true;
                } else {
                    is_phase = IS_PH_THINK;
                    is_think_at = now_ms();
                }
            }
        } else if (is_phase == IS_PH_REMOVE) {
            if (is_removable(is_cx, is_cy)) {
                is_board[is_cy][is_cx] = 1;
                audio_move();
                if (is_ai_mobility() == 0) {
                    is_winner = IS_PLAYER;
                    is_over = true;
                } else {
                    is_phase = IS_PH_THINK;
                    is_think_at = now_ms();
                }
            }
        }
        break;
    case K_CHAR:
        if (ev->ch == 'a' && is_cx > 0) is_cx--;
        else if (ev->ch == 'd' && is_cx < IS_N - 1) is_cx++;
        else if (ev->ch == 'w' && is_cy > 0) is_cy--;
        else if (ev->ch == 's' && is_cy < IS_N - 1) is_cy++;
        else if (ev->ch == 'n') isola_enter();
        break;
    case K_BACK:
    case K_PAUSE: {
        pause_sel_t sel;
        if (ui_pause_run(&sel)) {
            if (sel == PAUSE_RESTART) isola_enter();
        } else {
            s_exit_request = true;
        }
        break;
    }
    case K_QUIT: s_exit_request = true; break;
    default: break;
    }
    /* 玩家胜利: 状态切换那一刻只响一次 */
    if (is_over && is_winner == IS_PLAYER) {
        audio_win();
        led_fx_set(LED_FX_WIN);
    }
}

/* ---- tick: AI 思考延迟展示(视觉指示 AI 回合) ---- */
void isola_tick(uint64_t now) {
    if (is_over || is_phase != IS_PH_THINK) return;
    if (now - is_think_at < IS_AI_MS) return;
    is_ai_play();
    is_turn = IS_PLAYER;
    if (is_player_mobility() == 0) {
        is_winner = IS_AI;
        is_over = true;
        audio_lose();
        led_fx_set(LED_FX_LOSE);
    } else {
        is_phase = IS_PH_MOVE;
        is_cx = is_px;
        is_cy = is_py;
        is_snap_target();
    }
}
