/* CHESS — 国际象棋(简化版): 8x8 vs AI
 * 完整棋子集(王/后/车/象/马/兵), 简化规则:
 *   无王车易位/吃过路兵; 兵到底线自动升后(白 Q / 黑 q)
 *   不判将军: 能吃掉对方王即胜(king capture 规则), 无子可动判负;
 *   步数封顶按子力定胜负(平和则和)
 * 显示: 8x8 格 17px 方形(136x136)占满 HUD 下全部空间; 黑格浅斜纹
 *   白方棋子黑色字符, AI(黑方)棋子深底反白字符; 字符 2x 放大
 * 操作: 方向/WASD 移光标, OK 选中(高亮可走格), 再 OK 移动, BACK 取消/暂停
 * AI: 负极大搜索(深度 2, 残局 ≤12 子深度 3) + alpha-beta 剪枝
 *    + 吃子优先排序 + 子力估价(P1 N3 B3 R5 Q9) + 兵推进/中心微调
 *    + 平分随机破平局; AI 回合由 tick(延迟 700ms)驱动, 左栏 TURN 指示 */
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

#define CH_CELL 17                 /* 17px 格: 136x136 = HUD 下全高 */
#define CH_BOARD (8 * CH_CELL)     /* 136 */
#define CH_OX ((CCG_W - CH_BOARD) / 2)   /* 80, 水平居中 */
#define CH_OY CCG_HUD_H            /* 16, 顶栏下紧贴 */
#define CH_AI_DELAY_MS 700         /* AI 回合延迟(可见指示) */
#define CH_PLIES_MAX 150           /* 半回合封顶 → 按子力定胜负 */
#define CH_MAX_MV 256              /* 走法数组上限 */
#define CH_MATE 1000000            /* 将杀分值 */
#define CH_BUDGET 300000           /* 搜索节点预算(防极端耗时) */
#define CH_ROOT_DEPTH 2
#define CH_ENDGAME_PIECES 12       /* 棋子数 ≤ 此值 → 搜索加深 1 层 */

/* 棋子编码: 1-6 白方 9-14 黑方 */
#define CH_EMPTY 0
#define CH_WP 1
#define CH_WN 2
#define CH_WB 3
#define CH_WR 4
#define CH_WQ 5
#define CH_WK 6
#define CH_BP 9
#define CH_BN 10
#define CH_BB 11
#define CH_BR 12
#define CH_BQ 13
#define CH_BK 14

typedef struct {
    int fx, fy, tx, ty;
} ch_mv_t;

void chess_render(void);

static uint8_t ch_board[8][8];
static int ch_turn;                /* 1=玩家(白,下) 2=AI(黑,上) */
static int ch_winner;              /* 0=进行中 1=玩家胜 2=AI胜 3=和 */
static int ch_cx, ch_cy;           /* 光标(格坐标) */
static bool ch_sel;                /* 已选中棋子 */
static int ch_sx, ch_sy;
static bool ch_over, ch_over_full;
static int ch_plies;               /* 半回合数(显示/和棋判定) */
static uint64_t ch_ai_at;          /* AI 最早行动时刻 */
static int ch_budget;              /* 搜索节点预算 */
static rng_t ch_rng;

static const int ch_dirs8[8][2] = {
    { 1, 0 }, { 1, 1 }, { 0, 1 }, { -1, 1 },
    { -1, 0 }, { -1, -1 }, { 0, -1 }, { 1, -1 }
};
static const int ch_diag4[4][2] = { { 1, 1 }, { 1, -1 }, { -1, 1 }, { -1, -1 } };
static const int ch_orth4[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
static const int ch_knight[8][2] = {
    { 1, 2 }, { 2, 1 }, { -1, 2 }, { -2, 1 },
    { 1, -2 }, { 2, -1 }, { -1, -2 }, { -2, -1 }
};

static int ch_color(int p) { return (p <= 6) ? 1 : 2; }   /* 0 不调用 */
static int ch_king(int side) { return (side == 1) ? CH_WK : CH_BK; }

/* 加入走法: 越界/吃己方 → 丢弃 */
static void ch_add(int fx, int fy, int tx, int ty, ch_mv_t *mvs, int *n) {
    if (*n >= CH_MAX_MV) return;
    if (tx < 0 || tx >= 8 || ty < 0 || ty >= 8) return;
    int p = ch_board[ty][tx];
    if (p != 0 && ch_color(p) == ch_color(ch_board[fy][fx])) return;
    mvs[*n].fx = fx;
    mvs[*n].fy = fy;
    mvs[*n].tx = tx;
    mvs[*n].ty = ty;
    (*n)++;
}

/* 滑子(d 组方向)走法生成 */
static void ch_slide(int x, int y, int side, const int d[][2], int nd,
                     ch_mv_t *mvs, int *n) {
    for (int i = 0; i < nd; i++) {
        int tx = x + d[i][0], ty = y + d[i][1];
        while (tx >= 0 && tx < 8 && ty >= 0 && ty < 8) {
            int q = ch_board[ty][tx];
            if (q != 0) {
                if (ch_color(q) != side) ch_add(x, y, tx, ty, mvs, n);
                break;
            }
            ch_add(x, y, tx, ty, mvs, n);
            tx += d[i][0];
            ty += d[i][1];
        }
    }
}

/* side 方全部伪合法走法(吃王规则下即为合法走法) */
static int ch_gen_moves(int side, ch_mv_t *mvs) {
    int n = 0;
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++) {
            int p = ch_board[y][x];
            if (p == 0 || ch_color(p) != side) continue;
            switch (p) {
            case CH_WP:
            case CH_BP: {
                int dir = (p == CH_WP) ? -1 : 1;
                int sy = (p == CH_WP) ? 6 : 1;
                int ny = y + dir;
                if (ny >= 0 && ny < 8 && ch_board[ny][x] == 0) {
                    ch_add(x, y, x, ny, mvs, &n);
                    if (y == sy && ch_board[y + 2 * dir][x] == 0)
                        ch_add(x, y, x, y + 2 * dir, mvs, &n);
                }
                if (ny >= 0 && ny < 8) {
                    if (x > 0 && ch_board[ny][x - 1] != 0 &&
                        ch_color(ch_board[ny][x - 1]) != side)
                        ch_add(x, y, x - 1, ny, mvs, &n);
                    if (x < 7 && ch_board[ny][x + 1] != 0 &&
                        ch_color(ch_board[ny][x + 1]) != side)
                        ch_add(x, y, x + 1, ny, mvs, &n);
                }
                break;
            }
            case CH_WN:
            case CH_BN:
                for (int i = 0; i < 8; i++)
                    ch_add(x, y, x + ch_knight[i][0], y + ch_knight[i][1],
                           mvs, &n);
                break;
            case CH_WB:
            case CH_BB:
                ch_slide(x, y, side, ch_diag4, 4, mvs, &n);
                break;
            case CH_WR:
            case CH_BR:
                ch_slide(x, y, side, ch_orth4, 4, mvs, &n);
                break;
            case CH_WQ:
            case CH_BQ:
                ch_slide(x, y, side, ch_dirs8, 8, mvs, &n);
                break;
            case CH_WK:
            case CH_BK:
                for (int i = 0; i < 8; i++)
                    ch_add(x, y, x + ch_dirs8[i][0], y + ch_dirs8[i][1],
                           mvs, &n);
                break;
            default:
                break;
            }
        }
    return n;
}

/* 落子(含吃子/自动升后); 返回被吃棋子(0=无), *promo=1 表示升变 */
static int ch_make(int fx, int fy, int tx, int ty, int *captured, int *promo) {
    int p = ch_board[fy][fx];
    *captured = ch_board[ty][tx];
    *promo = 0;
    ch_board[ty][tx] = (uint8_t)p;
    ch_board[fy][fx] = 0;
    if ((p == CH_WP && ty == 0) || (p == CH_BP && ty == 7)) {
        ch_board[ty][tx] = (uint8_t)((p == CH_WP) ? CH_WQ : CH_BQ);
        *promo = 1;
    }
    return *captured;
}

static void ch_unmake(int fx, int fy, int tx, int ty, int captured, int promo) {
    int p = ch_board[ty][tx];
    if (promo) p = (p == CH_WQ) ? CH_WP : CH_BP;
    ch_board[fy][fx] = (uint8_t)p;
    ch_board[ty][tx] = (uint8_t)captured;
}

static bool ch_has_king(int side) {
    int k = ch_king(side);
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++)
            if (ch_board[y][x] == k) return true;
    return false;
}

static bool ch_has_moves(int side) {
    ch_mv_t tmp[CH_MAX_MV];
    return ch_gen_moves(side, tmp) > 0;
}

static bool ch_legal(int fx, int fy, int tx, int ty) {
    if (ch_board[fy][fx] == 0) return false;
    ch_mv_t mv[CH_MAX_MV];
    int n = ch_gen_moves(ch_color(ch_board[fy][fx]), mv);
    for (int i = 0; i < n; i++)
        if (mv[i].tx == tx && mv[i].ty == ty) return true;
    return false;
}

/* 中心化微调(马/象/后) */
static int ch_center(int x, int y) {
    int dx = (x == 3 || x == 4) ? 2 : ((x == 2 || x == 5) ? 1 : 0);
    int dy = (y == 3 || y == 4) ? 2 : ((y == 2 || y == 5) ? 1 : 0);
    return dx + dy;
}

static int ch_val(int p) {
    switch (p) {
    case CH_WP: case CH_BP: return 100;
    case CH_WN: case CH_BN: return 320;
    case CH_WB: case CH_BB: return 330;
    case CH_WR: case CH_BR: return 500;
    case CH_WQ: case CH_BQ: return 900;
    default: return 0;
    }
}

/* 静态估价, 从 side 方视角
 * 子力为主 + 中心控制 + 出子激励(后排放-12, 离位+8) + 兵推进微调 */
static int ch_eval_from(int side) {
    int w = 0, b = 0;
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++) {
            int p = ch_board[y][x];
            switch (p) {
            case CH_WP: w += 100 + (7 - y) * 2; break;
            case CH_WN: w += 320 + ch_center(x, y) * 3 + (y == 7 ? -12 : 8); break;
            case CH_WB: w += 330 + ch_center(x, y) * 3 + (y == 7 ? -12 : 8); break;
            case CH_WR: w += 500 + (y == 7 ? -8 : 4); break;
            case CH_WQ: w += 900 + ch_center(x, y) * 2 + (y == 7 ? -4 : 2); break;
            case CH_BP: b += 100 + y * 2; break;
            case CH_BN: b += 320 + ch_center(x, y) * 3 + (y == 0 ? -12 : 8); break;
            case CH_BB: b += 330 + ch_center(x, y) * 3 + (y == 0 ? -12 : 8); break;
            case CH_BR: b += 500 + (y == 0 ? -8 : 4); break;
            case CH_BQ: b += 900 + ch_center(x, y) * 2 + (y == 0 ? -4 : 2); break;
            default: break;
            }
        }
    return (side == 2) ? b - w : w - b;
}

/* 子力差(白-黑, 分) */
static int ch_material(void) {
    int w = 0, b = 0;
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++) {
            int p = ch_board[y][x];
            if (p == 0) continue;
            if (ch_color(p) == 1) w += ch_val(p);
            else b += ch_val(p);
        }
    return w - b;
}

/* side 方被吃子数(王被吃即终局, 场上无王 → 最多 15) */
static int ch_captured(int side) {
    int n = 0;
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++)
            if (ch_board[y][x] != 0 && ch_color(ch_board[y][x]) == side) n++;
    return 16 - n;
}

static int ch_piece_count(void) {
    int n = 0;
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++)
            if (ch_board[y][x] != 0) n++;
    return n;
}

/* 负极大搜索; 返回 side 方视角分值 */
static int ch_ai_search(int depth, int side, int alpha, int beta) {
    if (--ch_budget <= 0) return ch_eval_from(side);
    ch_mv_t mv[CH_MAX_MV];
    int n = ch_gen_moves(side, mv);
    if (n == 0) return -(CH_MATE - depth);   /* 无子可动: 该方负 */
    /* 吃子优先排序(浅序, 加速剪枝) */
    int cur = 0;
    for (int i = 0; i < n; i++)
        if (ch_board[mv[i].ty][mv[i].tx] != 0) {
            ch_mv_t t = mv[cur];
            mv[cur] = mv[i];
            mv[i] = t;
            cur++;
        }
    int best = -CH_MATE - 1;
    for (int i = 0; i < n; i++) {
        int cap, promo;
        ch_make(mv[i].fx, mv[i].fy, mv[i].tx, mv[i].ty, &cap, &promo);
        int s;
        if (cap == ch_king(side == 1 ? 2 : 1)) s = CH_MATE - depth;
        else if (depth <= 1) s = ch_eval_from(side);   /* 叶子: 己方视角(调用方取反) */
        else s = -ch_ai_search(depth - 1, side == 1 ? 2 : 1, -beta, -alpha);
        ch_unmake(mv[i].fx, mv[i].fy, mv[i].tx, mv[i].ty, cap, promo);
        if (s > best) best = s;
        if (best > alpha) alpha = best;
        if (alpha >= beta) break;
    }
    return best;
}

/* AI 选步并落子; 返回 false = 无路可走 */
static bool ch_ai_move(void) {
    ch_mv_t mv[CH_MAX_MV];
    int n = ch_gen_moves(2, mv);
    if (n == 0) return false;
    int depth = (ch_piece_count() <= CH_ENDGAME_PIECES) ? CH_ROOT_DEPTH + 1
                                                        : CH_ROOT_DEPTH;
    ch_budget = CH_BUDGET;
    int best = -CH_MATE - 1;
    int picks[CH_MAX_MV];
    int np = 0;
    for (int i = 0; i < n; i++) {
        int cap, promo;
        ch_make(mv[i].fx, mv[i].fy, mv[i].tx, mv[i].ty, &cap, &promo);
        int s;
        if (cap == CH_WK) s = CH_MATE;
        else s = -ch_ai_search(depth - 1, 1, -CH_MATE - 1, -best);
        ch_unmake(mv[i].fx, mv[i].fy, mv[i].tx, mv[i].ty, cap, promo);
        if (s > best) {
            best = s;
            np = 0;
        }
        if (s == best && np < CH_MAX_MV) picks[np++] = i;
    }
    /* 同分偏好吃子(进攻性), 仍有多者随机 */
    if (np > 1) {
        int ncap = 0;
        for (int j = 0; j < np; j++)
            if (ch_board[mv[picks[j]].ty][mv[picks[j]].tx] != 0)
                picks[ncap++] = picks[j];
        if (ncap > 0) np = ncap;
    }
    int k = (int)rng_range(&ch_rng, (uint32_t)np);   /* 平分随机破平局 */
    int fx = mv[picks[k]].fx, fy = mv[picks[k]].fy;
    int tx = mv[picks[k]].tx, ty = mv[picks[k]].ty;
    {
        int cap, promo;
        ch_make(fx, fy, tx, ty, &cap, &promo);
    }
    ch_plies++;
    ch_cx = tx;                                      /* 光标移到 AI 落点 */
    ch_cy = ty;
    return true;
}

/* 落子后判定: 吃王/无子可动/步数封顶 → 终局, 否则轮到对方 */
static void ch_finish(int mover) {
    int opp = (mover == 1) ? 2 : 1;
    if (!ch_has_king(opp) || !ch_has_moves(opp)) {
        ch_winner = mover;
        ch_over = true;
        if (ch_winner == 1) {
            audio_win();             /* 吃王/对方无子可动 */
            led_fx_set(LED_FX_WIN);
        } else {
            audio_lose();
            led_fx_set(LED_FX_LOSE);
        }
        return;
    }
    if (ch_plies >= CH_PLIES_MAX) {
        /* 步数封顶: 按子力定胜负(平和则和棋) */
        int m = ch_material();
        ch_winner = (m > 0) ? 1 : (m < 0) ? 2 : 3;
        ch_over = true;
        if (ch_winner == 1) {
            audio_win();             /* 步数封顶按子力胜 */
            led_fx_set(LED_FX_WIN);
        } else if (ch_winner == 2) {
            audio_lose();
            led_fx_set(LED_FX_LOSE);
        }
        return;
    }
    ch_turn = opp;
}

/* 玩家落子: 移动 + 记步 */
static void ch_play(int fx, int fy, int tx, int ty) {
    int cap, promo;
    ch_make(fx, fy, tx, ty, &cap, &promo);
    if (cap != 0) audio_clear();     /* 吃子 */
    ch_plies++;
}

static void ch_handle_ok(void) {
    if (ch_turn != 1) return;
    int p = ch_board[ch_cy][ch_cx];
    if (ch_sel) {
        if (ch_cx == ch_sx && ch_cy == ch_sy) { ch_sel = false; return; }
        if (p != 0 && ch_color(p) == 1) {
            ch_sx = ch_cx;                         /* 改选己方棋子 */
            ch_sy = ch_cy;
            return;
        }
        if (!ch_legal(ch_sx, ch_sy, ch_cx, ch_cy)) return;   /* 非法目标 */
        ch_play(ch_sx, ch_sy, ch_cx, ch_cy);
        ch_sel = false;
        ch_sx = ch_sy = 0;
        ch_finish(1);
    } else {
        if (p != 0 && ch_color(p) == 1) {
            ch_sel = true;
            ch_sx = ch_cx;
            ch_sy = ch_cy;
        }
    }
}

/* 执行 AI 回合(到点由 tick/on_key 调用) */
static void ch_ai_turn(void) {
    if (ch_over || ch_turn != 2) return;
    if (!ch_ai_move()) {                           /* AI 无路 → 玩家胜 */
        ch_winner = 1;
        ch_over = true;
        audio_win();
        led_fx_set(LED_FX_WIN);
        return;
    }
    if (ch_sel && (ch_board[ch_sy][ch_sx] == 0 ||
                   ch_color(ch_board[ch_sy][ch_sx]) != 1))
        ch_sel = false;                            /* 选中子被吃/挪走 */
    ch_finish(2);
}

static void ch_new(void) {
    static const uint8_t back[8] = {
        CH_WR, CH_WN, CH_WB, CH_WQ, CH_WK, CH_WB, CH_WN, CH_WR
    };
    memset(ch_board, 0, sizeof(ch_board));
    for (int x = 0; x < 8; x++) {
        ch_board[1][x] = CH_BP;
        ch_board[6][x] = CH_WP;
        ch_board[0][x] = (uint8_t)(back[x] + 8);
        ch_board[7][x] = back[x];
    }
    ch_turn = 1;
    ch_winner = 0;
    ch_cx = 4;
    ch_cy = 6;
    ch_sel = false;
    ch_sx = ch_sy = 0;
    ch_over = false;
    ch_over_full = false;
    ch_plies = 0;
    ch_ai_at = 0;
}

void chess_enter(void) {
    rng_seed(&ch_rng, now_ms() ^ 0xc1a7b3u);
    ch_new();
    chess_render();
    disp_full();
}

void chess_exit(void) {}

void chess_tick(uint64_t now) {
    (void)now;
    if (ch_over || ch_turn != 2) return;
    if (now_ms() >= ch_ai_at) ch_ai_turn();
}

void chess_on_key(const key_event_t *ev) {
    /* 重复事件只响应方向键(确认/字母必须忽略) */
    if (ev->is_repeat && ev->key != K_LEFT && ev->key != K_RIGHT &&
        ev->key != K_UP && ev->key != K_DOWN)
        return;
    if (ch_over) {
        if (ev->key == K_OK || (ev->key == K_CHAR && ev->ch == 'n'))
            chess_enter();
        else if (ev->key == K_BACK || ev->key == K_QUIT)
            s_exit_request = true;
        return;
    }
    if (ev->key == K_BACK || ev->key == K_PAUSE) {
        pause_sel_t sel;
        if (ui_pause_run(&sel)) {
            if (sel == PAUSE_RESTART) chess_enter();
        } else {
            s_exit_request = true;
        }
        return;
    }
    if (ev->key == K_QUIT) { s_exit_request = true; return; }
    /* AI 回合: 到点先推进 AI(左栏指示保持可见), 本键随后处理 */
    if (ch_turn == 2) {
        if (now_ms() >= ch_ai_at) {
            ch_ai_turn();
            if (ch_over) return;
        } else if (ev->key == K_OK || ev->key == K_CHAR) {
            return;                                /* 未到点: 忽略确认/字母 */
        }
    }
    switch (ev->key) {
    case K_LEFT: if (ch_cx > 0) ch_cx--; break;
    case K_RIGHT: if (ch_cx < 7) ch_cx++; break;
    case K_UP: if (ch_cy > 0) ch_cy--; break;
    case K_DOWN: if (ch_cy < 7) ch_cy++; break;
    case K_OK: ch_handle_ok(); break;
    case K_CHAR:
        if (ev->ch == 'a' && ch_cx > 0) ch_cx--;
        else if (ev->ch == 'd' && ch_cx < 7) ch_cx++;
        else if (ev->ch == 'w' && ch_cy > 0) ch_cy--;
        else if (ev->ch == 's' && ch_cy < 7) ch_cy++;
        else if (ev->ch == 'n') chess_enter();
        break;
    default: break;
    }
}

/* ---- 渲染 ---- */

/* 棋子: 字符 2x; AI(黑方)深底反白字符 */
static void ch_draw_piece(int x, int y, int p) {
    static const char table[6] = { 'P', 'N', 'B', 'R', 'Q', 'K' };
    char c = table[(p - 1) & 7];
    char buf[2];
    buf[0] = (p > 6) ? (char)(c + 32) : c;
    buf[1] = 0;
    int px = CH_OX + x * CH_CELL + (CH_CELL - 10) / 2;
    int py = CH_OY + y * CH_CELL + (CH_CELL - 14) / 2;
    if (p > 6) {
        fb_fill_rect(px - 2, py - 1, 14, 16, true);
        fb_text_scale2(px, py, buf, false);
    } else {
        fb_text_scale2(px, py, buf, true);
    }
}

/* 格框(光标 2px / 选中 3px): 黑格白边白格黑边, 四周对称 */
static void ch_cell_frame(int x, int y, int t) {
    bool inv = ((x + y) & 1) ? false : true;
    fb_stroke_rect_thick(CH_OX + x * CH_CELL, CH_OY + y * CH_CELL,
                         CH_CELL, CH_CELL, t, inv);
}

/* 右侧 2x 反白大字(白字黑底); sign=true 带 +/- 号 */
static void ch_big_num(int xr, int y, int v, bool sign) {
    char buf[5];
    int n = 0;
    if (sign) {
        buf[n++] = (v < 0) ? '-' : '+';
        if (v < 0) v = -v;
    }
    if (v >= 100) { buf[n++] = (char)('0' + v / 100); v %= 100; }
    if (v >= 10) buf[n++] = (char)('0' + v / 10);
    buf[n++] = (char)('0' + v % 10);
    buf[n] = 0;
    int w = n * (FONT_ADV * 2);
    fb_fill_rect(xr - w, y, w, 16, true);
    fb_text_scale2(xr - w, y + 1, buf, false);
}

/* HUD 右侧 "MOVE n" */
static void ch_hud_right(char *buf) {
    int m = ch_plies / 2 + 1;
    buf[0] = 'M';
    buf[1] = 'O';
    buf[2] = 'V';
    buf[3] = 'E';
    buf[4] = ' ';
    int n = 5;
    if (m >= 100) buf[n++] = (char)('0' + m / 100);
    if (m >= 10) buf[n++] = (char)('0' + (m / 10) % 10);
    buf[n++] = (char)('0' + m % 10);
    buf[n] = 0;
}

void chess_render(void) {
    fb_clear(false);
    /* 顶栏: 左标题 + 右 MOVE 计数 */
    fb_text(0, 0, "CHESS", true);
    {
        char buf[12];
        ch_hud_right(buf);
        fb_text(CCG_W - 4 - text_width(buf), 0, buf, true);
    }
    fb_hline(0, CCG_HUD_H - 1, CCG_W, true);
    /* 棋盘: 黑格浅斜纹 + 网格 + 外框 */
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++)
            if (((x + y) & 1) != 0)
                fb_fill_tile(CH_OX + x * CH_CELL, CH_OY + y * CH_CELL,
                             CH_CELL, CH_CELL, pat_get(PAT_SLASH_S));
    for (int i = 0; i <= 8; i++) {
        fb_vline(CH_OX + i * CH_CELL, CH_OY, CH_BOARD, true);
        if (i < 8) fb_hline(CH_OX, CH_OY + i * CH_CELL, CH_BOARD, true);
    }
    fb_hline(CH_OX, CH_OY + CH_BOARD - 1, CH_BOARD, true);
    fb_stroke_rect(CH_OX - 1, CH_OY - 1, CH_BOARD + 2, CH_BOARD + 1, true);
    /* 棋子 */
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++)
            if (ch_board[y][x] != 0) ch_draw_piece(x, y, ch_board[y][x]);
    /* 选中高亮 + 合法目标提示 */
    if (ch_sel && ch_board[ch_sy][ch_sx] != 0 &&
        ch_color(ch_board[ch_sy][ch_sx]) == 1) {
        ch_cell_frame(ch_sx, ch_sy, 3);
        for (int ty = 0; ty < 8; ty++)
            for (int tx = 0; tx < 8; tx++) {
                if (!ch_legal(ch_sx, ch_sy, tx, ty)) continue;
                int px = CH_OX + tx * CH_CELL + CH_CELL / 2;
                int py = CH_OY + ty * CH_CELL + CH_CELL / 2;
                if (ch_board[ty][tx] != 0)
                    fb_stroke_rect(px - 2, py - 2, 5, 5, true);   /* 吃子 */
                else
                    fb_fill_rect(px - 1, py - 1, 2, 2, true);     /* 空位 */
            }
    }
    /* 左栏: TURN 指示(实心=玩家, 空心+AI..=AI) + 双方被吃数 */
    fb_text(2, 20, "TURN", true);
    {
        int icx = 12, icy = 40;
        bool you = (ch_turn == 1);
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
    fb_text(2, 76, "CAP", true);
    fb_text(2, 86, "YOU", true);
    ch_big_num(78, 98, ch_captured(2), false);
    fb_text(2, 116, "AI", true);
    ch_big_num(78, 128, ch_captured(1), false);
    /* 右栏: 子力差(玩家视角, 单位兵) */
    fb_text(222, 20, "MAT", true);
    ch_big_num(294, 32, ch_material() / 100, true);
    /* 光标最后画 */
    ch_cell_frame(ch_cx, ch_cy, 2);
    /* 结束: HUD 区两行提示 */
    if (ch_over) {
        fb_fill_rect(0, 0, CCG_W, CCG_HUD_H - 1, false);
        const char *res = (ch_winner == 1) ? "YOU WIN!" :
                          (ch_winner == 2) ? "AI WINS" : "DRAW";
        fb_text(2, 2, res, true);
        fb_text(CCG_W - 2 - text_width("OK/N:RETRY BACK:QUIT"), 2,
                "OK/N:RETRY BACK:QUIT", true);
        if (!ch_over_full) {
            ch_over_full = true;
            disp_force_full();
        }
    }
}
