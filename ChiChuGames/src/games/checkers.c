/* CHECKERS — 跳棋(国际跳棋简化版): 8x8 vs AI
 * 标准: 24 子/方, 斜向移动, 跳吃; 简化: 无强制跳吃, 无连跳
 * 升级: 兵到底线成王, 王可斜向任意步移动/跳吃(恰过一个敌子)
 * 胜负: 吃光对方或对方无路可走 → 胜; 连续 CK_DRAW_LIMIT 步无吃 → 和
 * 显示: 8x8 格 17px 方形(136x136 占满 HUD 下全部空间); 黑格浅斜纹
 * AI: 贪心(跳吃绝对优先) + 升级加分 + 王价值 + 中心化 + 随机破平局
 * 玩家黑子实心圆, AI 红子空心圆; 王中心加圆点标记
 * AI 回合由 tick(延迟 CK_AI_DELAY_MS)驱动, 左栏 TURN 指示变化 */
#include "game.h"
#include "../config.h"
#include "../gfx/canvas.h"
#include "../gfx/font.h"
#include "../gfx/pattern.h"
#include "../platform/display.h"
#include "../platform/input.h"
#include "../platform/time.h"
#include "../platform/audio.h"
#include "../platform/led.h"
#include "../rng.h"
#include <string.h>

#define CK_N 8
#define CK_CELL 17
#define CK_BOARD (CK_N * CK_CELL)             /* 136 */
#define CK_OX ((CCG_W - CK_BOARD) / 2)        /* 80, 水平居中 */
#define CK_OY CCG_HUD_H                       /* 16, 顶栏下紧贴 */
#define CK_DRAW_LIMIT 80                      /* 连续无吃步数 → 和棋 */
#define CK_AI_DELAY_MS 700                    /* AI 回合延迟(可见指示) */

/* 棋子: 0=空 1=玩家兵(黑,向下) 2=AI兵(红,向上) 3=玩家王 4=AI王 */
#define CK_P1 1
#define CK_P2 2
#define CK_K1 3
#define CK_K2 4

void checkers_render(void);

static uint8_t ck_board[CK_N][CK_N];
static int ck_turn;                 /* 1=玩家 2=AI */
static int ck_winner;               /* 0=进行中 1=玩家胜 2=AI胜 3=和 */
static int ck_cx, ck_cy;            /* 光标(格坐标) */
static bool ck_sel;                 /* 已选中棋子 */
static int ck_sx, ck_sy;
static bool ck_over, ck_over_full;
static int ck_moves_since_cap;      /* 连续无吃步数 */
static uint64_t ck_ai_at;           /* AI 最早行动时刻 */
static rng_t ck_rng;

static int ck_color(int p) { return (p <= 2) ? p : p - 2; }
static int ck_man(int p) { return p <= 2; }
static int ck_opp(int c) { return 3 - c; }
static int ck_dir(int c) { return (c == 1) ? 1 : -1; }   /* 兵行进方向 */

/* 判定 (fx,fy)->(tx,ty) 的合法性(针对该格棋子):
 * 返回 0=非法 1=普通移动 2=跳吃(被吃位置经 *mx,*my 返回) */
static int ck_move_kind(int fx, int fy, int tx, int ty, int *mx, int *my) {
    int p = ck_board[fy][fx];
    if (p == 0) return 0;
    int color = ck_color(p);
    if (tx < 0 || tx >= CK_N || ty < 0 || ty >= CK_N) return 0;
    if (ck_board[ty][tx] != 0) return 0;
    int dx = tx - fx, dy = ty - fy;
    int ax = dx < 0 ? -dx : dx;
    int ay = dy < 0 ? -dy : dy;
    if (ax != ay || ax == 0) return 0;
    if (ck_man(p)) {
        if (dy * ck_dir(color) <= 0) return 0;   /* 兵只能前进(含两步跳吃) */
        if (ax == 1) return 1;
        if (ax == 2) {
            int jx = fx + dx / 2, jy = fy + dy / 2;
            int q = ck_board[jy][jx];
            if (q != 0 && ck_color(q) == ck_opp(color)) {
                *mx = jx;
                *my = jy;
                return 2;
            }
        }
        return 0;
    }
    /* 王: 沿对角线滑行; 中间全空=普通移动, 恰过一个敌子(其后全空)=跳吃 */
    int sx = dx < 0 ? -1 : 1, sy = dy < 0 ? -1 : 1;
    int x = fx + sx, y = fy + sy;
    bool seen = false;
    while (x != tx || y != ty) {
        int q = ck_board[y][x];
        if (q != 0) {
            if (seen || ck_color(q) == color) return 0;
            seen = true;
            *mx = x;
            *my = y;
        }
        x += sx;
        y += sy;
        if (x < 0 || x >= CK_N || y < 0 || y >= CK_N) return 0;
    }
    return seen ? 2 : 1;
}

/* 统计 color 方棋子数 */
static int ck_piece_count(int color) {
    int n = 0;
    for (int y = 0; y < CK_N; y++)
        for (int x = 0; x < CK_N; x++)
            if (ck_board[y][x] != 0 && ck_color(ck_board[y][x]) == color) n++;
    return n;
}

/* color 方是否还有合法走法 */
static bool ck_has_move(int color) {
    for (int y = 0; y < CK_N; y++)
        for (int x = 0; x < CK_N; x++) {
            int p = ck_board[y][x];
            if (p == 0 || ck_color(p) != color) continue;
            int mx, my;
            if (ck_man(p)) {
                int d = ck_dir(color);
                for (int sx = -1; sx <= 1; sx += 2) {
                    if (ck_move_kind(x, y, x + sx, y + d, &mx, &my)) return true;
                    if (ck_move_kind(x, y, x + 2 * sx, y + 2 * d, &mx, &my))
                        return true;
                }
            } else {
                static const int sxd[4] = { 1, 1, -1, -1 };
                static const int syd[4] = { 1, -1, 1, -1 };
                for (int i = 0; i < 4; i++)
                    for (int d = 1; d < CK_N; d++)
                        if (ck_move_kind(x, y, x + sxd[i] * d, y + syd[i] * d,
                                         &mx, &my))
                            return true;
            }
        }
    return false;
}

/* 执行移动(含吃子/升王); 非法移动无效果 */
static void ck_apply(int fx, int fy, int tx, int ty) {
    int mx, my;
    int kind = ck_move_kind(fx, fy, tx, ty, &mx, &my);
    if (kind == 0) return;
    int p = ck_board[fy][fx];
    ck_board[fy][fx] = 0;
    if (kind == 2) {
        ck_board[my][mx] = 0;
        ck_moves_since_cap = 0;
        audio_clear();               /* 跳吃 */
    } else {
        ck_moves_since_cap++;
    }
    if (ck_man(p)) {
        if ((p == CK_P1 && ty == CK_N - 1) || (p == CK_P2 && ty == 0))
            p += 2;                          /* 升王 */
    }
    ck_board[ty][tx] = (uint8_t)p;
}

/* AI 候选打分: 跳吃优先, 升王/王价值, 中心化 */
static int ck_ai_score_move(int tx, int ty, int kind, int p, int capt_val) {
    int s = 0;
    if (kind == 2) s += 100 + capt_val;
    if (ck_man(p)) {
        if (p == CK_P2 && ty == 0) s += 40;  /* 升王 */
    } else {
        s += 25;                             /* 王价值 */
    }
    int c = (tx == 3 || tx == 4) ? 3 : (tx == 2 || tx == 5) ? 2 :
            (tx == 1 || tx == 6) ? 1 : 0;
    return s + c;
}

static void ck_eval_cand(int fx, int fy, int tx, int ty, int p,
                         int *best, int *bx, int *by, int *btx, int *bty) {
    int mx, my;
    int kind = ck_move_kind(fx, fy, tx, ty, &mx, &my);
    if (kind == 0) return;
    int cv = (kind == 2) ? (ck_man(ck_board[my][mx]) ? 6 : 15) : 0;
    int s = ck_ai_score_move(tx, ty, kind, p, cv);
    s += (int)rng_range(&ck_rng, 4);         /* 随机破平局 */
    if (s > *best) {
        *best = s;
        *bx = fx;
        *by = fy;
        *btx = tx;
        *bty = ty;
    }
}

/* AI 走一步; 返回 false = 无路可走 */
static bool ck_ai_move(void) {
    int best = -1000000, bx = -1, by = -1, btx = 0, bty = 0;
    for (int y = 0; y < CK_N; y++)
        for (int x = 0; x < CK_N; x++) {
            int p = ck_board[y][x];
            if (p == 0 || ck_color(p) != CK_P2) continue;
            if (ck_man(p)) {
                int d = ck_dir(CK_P2);
                for (int sx = -1; sx <= 1; sx += 2) {
                    ck_eval_cand(x, y, x + sx, y + d, p,
                                 &best, &bx, &by, &btx, &bty);
                    ck_eval_cand(x, y, x + 2 * sx, y + 2 * d, p,
                                 &best, &bx, &by, &btx, &bty);
                }
            } else {
                static const int sxd[4] = { 1, 1, -1, -1 };
                static const int syd[4] = { 1, -1, 1, -1 };
                for (int i = 0; i < 4; i++)
                    for (int d = 1; d < CK_N; d++)
                        ck_eval_cand(x, y, x + sxd[i] * d, y + syd[i] * d, p,
                                     &best, &bx, &by, &btx, &bty);
            }
        }
    if (bx < 0) return false;
    ck_apply(bx, by, btx, bty);
    ck_cx = btx;                              /* 光标移到 AI 落点 */
    ck_cy = bty;
    if (ck_sel && (ck_board[ck_sy][ck_sx] == 0 ||
                   ck_color(ck_board[ck_sy][ck_sx]) != CK_P1))
        ck_sel = false;                       /* 选中子被吃/挪走 → 取消 */
    return true;
}

/* 判定刚落子后的结局; 返回 0=继续, 否则为胜方(3=和棋) */
static int ck_check_end(int mover) {
    int opp = ck_opp(mover);
    if (ck_piece_count(opp) == 0 || !ck_has_move(opp)) return mover;
    if (ck_moves_since_cap >= CK_DRAW_LIMIT) return 3;
    return 0;
}

/* 玩家落子后: 判负或转 AI 回合 */
static void ck_after_player_move(void) {
    int e = ck_check_end(CK_P1);
    if (e) {
        ck_winner = e;
        ck_over = true;
        if (e == CK_P1) {
            audio_win();             /* 吃光对方/对方无路 */
            led_fx_set(LED_FX_WIN);
        }
        return;
    }
    ck_turn = 2;
    ck_ai_at = now_ms() + CK_AI_DELAY_MS;
}

/* 执行 AI 回合(到点由 tick/on_key 调用) */
static void ck_ai_turn(void) {
    if (ck_over || ck_turn != 2) return;
    if (!ck_ai_move()) {
        ck_winner = CK_P1;
        ck_over = true;
        audio_win();                 /* AI 无路可走 → 玩家胜 */
        led_fx_set(LED_FX_WIN);
        return;
    }
    int e = ck_check_end(CK_P2);
    if (e) {
        ck_winner = e;
        ck_over = true;
        if (e == CK_P2) {
            audio_lose();            /* AI 吃光玩家/玩家无路 */
            led_fx_set(LED_FX_LOSE);
        }
        return;
    }
    ck_turn = 1;
}

/* ---- 玩家操作: 选中 / 落子 ---- */
static void ck_handle_ok(void) {
    if (ck_turn != CK_P1) return;
    int p = ck_board[ck_cy][ck_cx];
    if (ck_sel) {
        if (ck_cx == ck_sx && ck_cy == ck_sy) { ck_sel = false; return; }
        if (p != 0 && ck_color(p) == CK_P1) { ck_sx = ck_cx; ck_sy = ck_cy; return; }
        int mx, my;
        if (ck_move_kind(ck_sx, ck_sy, ck_cx, ck_cy, &mx, &my) == 0)
            return;                           /* 非法目标: 保持选择 */
        ck_apply(ck_sx, ck_sy, ck_cx, ck_cy);
        ck_sel = false;
        ck_sx = ck_sy = 0;
        ck_after_player_move();
    } else {
        if (p != 0 && ck_color(p) == CK_P1) { ck_sel = true; ck_sx = ck_cx; ck_sy = ck_cy; }
    }
}

static void ck_new(void) {
    memset(ck_board, 0, sizeof(ck_board));
    for (int y = 0; y < 3; y++)
        for (int x = 0; x < CK_N; x++)
            if (((x + y) & 1) != 0) ck_board[y][x] = CK_P1;
    for (int y = 5; y < CK_N; y++)
        for (int x = 0; x < CK_N; x++)
            if (((x + y) & 1) != 0) ck_board[y][x] = CK_P2;
    ck_turn = CK_P1;
    ck_winner = 0;
    ck_cx = 1;
    ck_cy = 0;
    ck_sel = false;
    ck_sx = ck_sy = 0;
    ck_over = false;
    ck_over_full = false;
    ck_moves_since_cap = 0;
    ck_ai_at = 0;
}

void checkers_enter(void) {
    rng_seed(&ck_rng, now_ms() ^ 0x51ab3cu);
    ck_new();
    checkers_render();
    disp_full();
}

void checkers_exit(void) {}

void checkers_tick(uint64_t now) {
    (void)now;
    if (ck_over || ck_turn != 2) return;
    if (now_ms() >= ck_ai_at) ck_ai_turn();
}

void checkers_on_key(const key_event_t *ev) {
    /* 重复事件只响应方向键(确认/字母必须忽略) */
    if (ev->is_repeat && ev->key != K_LEFT && ev->key != K_RIGHT &&
        ev->key != K_UP && ev->key != K_DOWN)
        return;
    if (ck_over) {
        if (ev->key == K_OK || (ev->key == K_CHAR && ev->ch == 'n'))
            checkers_enter();
        else if (ev->key == K_BACK || ev->key == K_QUIT)
            s_exit_request = true;
        return;
    }
    if (ev->key == K_BACK || ev->key == K_PAUSE) {
        pause_sel_t sel;
        if (ui_pause_run(&sel)) {
            if (sel == PAUSE_RESTART) checkers_enter();
        } else {
            s_exit_request = true;
        }
        return;
    }
    if (ev->key == K_QUIT) { s_exit_request = true; return; }
    /* AI 回合: 到点先推进 AI(左栏指示保持可见), 本键随后处理 */
    if (ck_turn == 2) {
        if (now_ms() >= ck_ai_at) {
            ck_ai_turn();
            if (ck_over) return;
        } else if (ev->key == K_OK || ev->key == K_CHAR) {
            return;                           /* 未到点: 忽略确认/字母 */
        }
    }
    switch (ev->key) {
    case K_LEFT: if (ck_cx > 0) ck_cx--; break;
    case K_RIGHT: if (ck_cx < CK_N - 1) ck_cx++; break;
    case K_UP: if (ck_cy > 0) ck_cy--; break;
    case K_DOWN: if (ck_cy < CK_N - 1) ck_cy++; break;
    case K_OK: ck_handle_ok(); break;
    case K_CHAR:
        if (ev->ch == 'a' && ck_cx > 0) ck_cx--;
        else if (ev->ch == 'd' && ck_cx < CK_N - 1) ck_cx++;
        else if (ev->ch == 'w' && ck_cy > 0) ck_cy--;
        else if (ev->ch == 's' && ck_cy < CK_N - 1) ck_cy++;
        else if (ev->ch == 'n') checkers_enter();
        break;
    default: break;
    }
}

/* ---- 渲染 ---- */

/* 棋子: 实心/空心圆 r6; 王中心 3px 圆点(黑白反色) */
static void ck_draw_piece(int x, int y, int p) {
    int cx = CK_OX + x * CK_CELL + CK_CELL / 2;
    int cy = CK_OY + y * CK_CELL + CK_CELL / 2;
    bool solid = (p == CK_P1 || p == CK_K1);
    bool king = (p >= CK_K1);
    for (int dy = -6; dy <= 6; dy++)
        for (int dx = -6; dx <= 6; dx++) {
            int r2 = dx * dx + dy * dy;
            if (solid) {
                if (r2 <= 36) fb_pixel(cx + dx, cy + dy, true);
            } else if (r2 <= 36 && r2 >= 16) {
                fb_pixel(cx + dx, cy + dy, true);
            }
        }
    if (king)
        for (int dy = -3; dy <= 3; dy++)
            for (int dx = -3; dx <= 3; dx++)
                if (dx * dx + dy * dy <= 9)
                    fb_pixel(cx + dx, cy + dy, solid ? false : true);
}

/* 格框(光标 2px / 选中 3px): 反色边框, 黑格白边白格黑边 */
static void ck_cell_frame(int x, int y, int t) {
    bool inv = ((x + y) & 1) ? false : true;
    fb_stroke_rect_thick(CK_OX + x * CK_CELL, CK_OY + y * CK_CELL,
                         CK_CELL, CK_CELL, t, inv);
}

/* 右侧 2x 反白大字(白字黑底) */
static void ck_big_inv(int xr, int y, int v) {
    char buf[4];
    int n = 0;
    if (v >= 10) buf[n++] = (char)('0' + v / 10);
    buf[n++] = (char)('0' + v % 10);
    buf[n] = 0;
    int w = n * (FONT_ADV * 2);
    fb_fill_rect(xr - w, y, w, 16, true);
    fb_text_scale2(xr - w, y + 1, buf, false);
}

static void ck_label(char *buf, const char *name, int v) {
    int n = 0;
    while (name[n]) { buf[n] = name[n]; n++; }
    buf[n++] = ' ';
    if (v >= 10) buf[n++] = (char)('0' + v / 10);
    buf[n++] = (char)('0' + v % 10);
    buf[n] = 0;
}

void checkers_render(void) {
    fb_clear(false);
    /* 顶栏: 左标题 + 右 YOU/AI 剩余数 */
    fb_text(0, 0, "CHECKERS", true);
    {
        char buf[16];
        ck_label(buf, "YOU", ck_piece_count(CK_P1));
        fb_text(CCG_W - 4 - text_width(buf), 0, buf, true);
        ck_label(buf, "AI", ck_piece_count(CK_P2));
        fb_text(CCG_W - 4 - text_width(buf), 8, buf, true);
    }
    fb_hline(0, CCG_HUD_H - 1, CCG_W, true);
    /* 棋盘: 黑格浅斜纹 + 网格 + 外框 */
    for (int y = 0; y < CK_N; y++)
        for (int x = 0; x < CK_N; x++)
            if (((x + y) & 1) != 0)
                fb_fill_tile(CK_OX + x * CK_CELL, CK_OY + y * CK_CELL,
                             CK_CELL, CK_CELL, pat_get(PAT_SLASH_S));
    for (int i = 0; i <= CK_N; i++) {
        fb_vline(CK_OX + i * CK_CELL, CK_OY, CK_BOARD, true);
        if (i < CK_N) fb_hline(CK_OX, CK_OY + i * CK_CELL, CK_BOARD, true);
    }
    fb_hline(CK_OX, CK_OY + CK_BOARD - 1, CK_BOARD, true);
    fb_stroke_rect(CK_OX - 1, CK_OY - 1, CK_BOARD + 2, CK_BOARD + 1, true);
    /* 棋子 */
    for (int y = 0; y < CK_N; y++)
        for (int x = 0; x < CK_N; x++)
            if (ck_board[y][x] != 0) ck_draw_piece(x, y, ck_board[y][x]);
    /* 选中高亮 + 合法目标提示 */
    if (ck_sel && ck_board[ck_sy][ck_sx] != 0 &&
        ck_color(ck_board[ck_sy][ck_sx]) == CK_P1) {
        ck_cell_frame(ck_sx, ck_sy, 3);
        for (int ty = 0; ty < CK_N; ty++)
            for (int tx = 0; tx < CK_N; tx++) {
                int mx, my;
                int kind = ck_move_kind(ck_sx, ck_sy, tx, ty, &mx, &my);
                if (kind == 0) continue;
                int px = CK_OX + tx * CK_CELL + CK_CELL / 2;
                int py = CK_OY + ty * CK_CELL + CK_CELL / 2;
                if (kind == 1) fb_fill_rect(px - 1, py - 1, 2, 2, true);
                else fb_stroke_rect(px - 2, py - 2, 5, 5, true);   /* 跳吃: 方框 */
            }
    }
    /* 左栏: 回合指示(实心=玩家, 空心+AI..=AI) */
    fb_text(2, 20, "TURN", true);
    {
        int icx = 12, icy = 40;
        bool you = (ck_turn == CK_P1);
        for (int dy = -5; dy <= 5; dy++)
            for (int dx = -5; dx <= 5; dx++) {
                int r2 = dx * dx + dy * dy;
                if (you) {
                    if (r2 <= 25) fb_pixel(icx + dx, icy + dy, true);
                } else if (r2 <= 25 && r2 >= 9) {
                    fb_pixel(icx + dx, icy + dy, true);
                }
            }
        fb_text(26, 36, you ? "YOU" : "AI..", true);
    }
    fb_text(2, 128, "N:NEW", true);
    fb_text(2, 140, "BACK:MENU", true);
    /* 右栏: 吃子数 */
    fb_text(218, 20, "CAPT", true);
    fb_text(218, 30, "YOU", true);
    ck_big_inv(294, 42, 12 - ck_piece_count(CK_P2));
    fb_text(218, 70, "AI", true);
    ck_big_inv(294, 82, 12 - ck_piece_count(CK_P1));
    /* 光标最后画 */
    ck_cell_frame(ck_cx, ck_cy, 2);
    /* 结束: HUD 区两行提示 */
    if (ck_over) {
        fb_fill_rect(0, 0, CCG_W, CCG_HUD_H - 1, false);
        const char *res = (ck_winner == CK_P1) ? "YOU WIN!" :
                          (ck_winner == CK_P2) ? "AI WINS" : "DRAW";
        fb_text(2, 2, res, true);
        fb_text(CCG_W - 2 - text_width("OK/N:RETRY BACK:QUIT"), 2,
                "OK/N:RETRY BACK:QUIT", true);
        if (!ck_over_full) {
            ck_over_full = true;
            disp_force_full();
        }
    }
}
