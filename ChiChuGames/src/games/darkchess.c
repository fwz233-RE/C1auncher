/* 暗棋 / DARK CHESS — 4x8 翻棋: 32 枚棋子随机打乱、背面朝上
 * 翻开: OK 翻开己方子(归翻出方); 吃子: 翻开子可吃上下左右
 *   己方大于对方的子(将J>士S>象X>马M>车C>炮P>兵B, 兵B>将J 循环)
 * 未翻开的子不能吃也不能被吃; 一方无子且无子可翻 = 败
 * AI: 一步贪心 — 吃子得分(子值-被吃风险) vs 翻子期望值(剩余均值+邻敌对决 EV)
 *   "优先吃能赢的子, 避开必被吃的送子"; AI 回合延时 750ms 并高亮行动格
 * 输入驱动 + tick 定时; 内存: 全静态(棋盘 32B + 少量状态), 零 malloc */
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

#define DC_ROWS 4
#define DC_COLS 8
#define DC_CELL 34                     /* 34px: 8x34=272 宽, 4x34=136 高 */
#define DC_BOARD_W (DC_COLS * DC_CELL)
#define DC_BOARD_H (DC_ROWS * DC_CELL)
#define DC_OX ((CCG_W - DC_BOARD_W) / 2)   /* 12, 水平居中 */
#define DC_OY CCG_HUD_H                     /* 16, 顶栏下 */
#define DC_AI_MS 750                   /* AI 回合视觉延时(真机快刷建议>=700ms) */

/* 棋子类型(格子编码位 0-2) */
#define DC_J 0                         /* 将 */
#define DC_S 1                         /* 士 */
#define DC_X 2                         /* 象 */
#define DC_M 3                         /* 马 */
#define DC_C 4                         /* 车 */
#define DC_P 5                         /* 炮 */
#define DC_B 6                         /* 兵 */
#define DC_TYPE_MASK 7u
#define DC_FACE 8u                     /* 背面朝上(未翻开) */
#define DC_OWN_P 16u                   /* 归玩家(翻出方) */
#define DC_OWN_A 32u                   /* 归 AI */

/* 格子编码: 0=空; 否则 type | (FACE?) | (OWN_*) */
static uint8_t dc_cell[DC_ROWS][DC_COLS];

static rng_t dc_rng;
static int dc_cx, dc_cy;               /* 光标(格坐标) */
static bool dc_ai_pending;             /* AI 回合延时中 */
static uint64_t dc_ai_at;              /* AI 行动时刻(ms) */
static int dc_hlx, dc_hly;             /* AI 上次行动格(高亮) */
static bool dc_hl_ok;
static bool dc_over;                   /* 终局 */
static int dc_winner;                  /* 1=玩家 2=AI */
static bool dc_over_full;              /* 终局全刷防重复 */

/* 子值(吃子收益 / 风险评估 / 翻子期望) */
static const int dc_val[7] = { 600, 100, 60, 60, 300, 120, 10 };
/* 显示字母: 将J 士S 象X 马M 车C 炮P 兵B */
static const char dc_letters[7] = { 'J', 'S', 'X', 'M', 'C', 'P', 'B' };
/* 32 枚: 将x2 士x4 象x4 马x4 车x4 炮x4 兵x10 */
static const uint8_t dc_set[32] = {
    0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 6, 6, 6,
    0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 6, 6, 6
};

static int dc_owner(uint8_t c) { return (int)((c >> 4) & 3u); }

/* 克制判定: a 能否吃 d(将>士>象>马>车>炮>兵, 兵>将 循环) */
static bool dc_beats(int a, int d) {
    if (d == DC_B) return a != DC_B && a != DC_J;  /* 兵只被士象马车炮吃 */
    if (a == DC_B) return d == DC_J;               /* 兵只吃将 */
    return a < d;
}

/* e 能否被 me(owner) 的 my(type) 吃掉: 必须是翻开敌子 */
static bool dc_can_eat(uint8_t e, int my, int me) {
    if (!e || (e & DC_FACE)) return false;
    if (dc_owner(e) == me) return false;
    return dc_beats(my, (int)(e & DC_TYPE_MASK));
}

static int dc_count(int owner) {
    int n = 0;
    for (int r = 0; r < DC_ROWS; r++)
        for (int c = 0; c < DC_COLS; c++) {
            uint8_t v = dc_cell[r][c];
            if (v && dc_owner(v) == owner) n++;
        }
    return n;
}

/* owner 是否有合法行动: 可翻(有面下子)或可吃(有己方子吃相邻敌子) */
static bool dc_has_action(int owner) {
    for (int r = 0; r < DC_ROWS; r++)
        for (int c = 0; c < DC_COLS; c++) {
            uint8_t v = dc_cell[r][c];
            if (v & DC_FACE) return true;          /* 可翻 */
            if (!v || dc_owner(v) != owner) continue;
            int my = (int)(v & DC_TYPE_MASK);
            if (r > 0 && dc_can_eat(dc_cell[r - 1][c], my, owner)) return true;
            if (r + 1 < DC_ROWS && dc_can_eat(dc_cell[r + 1][c], my, owner)) return true;
            if (c > 0 && dc_can_eat(dc_cell[r][c - 1], my, owner)) return true;
            if (c + 1 < DC_COLS && dc_can_eat(dc_cell[r][c + 1], my, owner)) return true;
        }
    return false;
}

/* ---- 新局: 洗牌(32 枚全背面), 需先 rng_seed ---- */
static void dc_new(void) {
    uint8_t a[32];
    for (int i = 0; i < 32; i++) a[i] = dc_set[i];
    for (int i = 31; i > 0; i--) {                 /* Fisher-Yates */
        uint32_t j = rng_range(&dc_rng, (uint32_t)i + 1);
        uint8_t t = a[i];
        a[i] = a[j];
        a[j] = t;
    }
    for (int r = 0; r < DC_ROWS; r++)
        for (int c = 0; c < DC_COLS; c++)
            dc_cell[r][c] = (uint8_t)(a[r * DC_COLS + c] | DC_FACE);
    dc_cx = 0;
    dc_cy = 0;
    dc_ai_pending = false;
    dc_hl_ok = false;
    dc_over = false;
    dc_winner = 0;
    dc_over_full = false;
}

/* ---- AI 一步贪心 ---- */
static void dc_ai_move(void);

/* ---- AI 回合定时: 延时结束才行动(视觉指示) ---- */
static void dc_ai_exec(uint64_t now) {
    if (!dc_ai_pending || now < dc_ai_at) return;
    dc_ai_pending = false;
    dc_ai_move();
}

/* 新翻子的期望收益 vs 相邻敌子 etype:
 * EV = p_beat*敌值 - (1-p_beat)*新子均值; 无克制则必亏均值 */
static int dc_flip_ev(int etype, int rem_total, const int rem_cnt[7], int avg) {
    int pb = 0;
    for (int t = 0; t < 7; t++)
        if (dc_beats(t, etype)) pb += rem_cnt[t];
    if (pb <= 0) return -avg;
    return (pb * dc_val[etype] - (rem_total - pb) * avg) / rem_total;
}

static void dc_ai_move(void) {
    static const int dx4[4] = { 1, -1, 0, 0 };
    static const int dy4[4] = { 0, 0, 1, -1 };
    /* 剩余面下统计(翻子期望) */
    int rem_cnt[7] = { 0, 0, 0, 0, 0, 0, 0 };
    int rem_total = 0, rem_sum = 0;
    for (int r = 0; r < DC_ROWS; r++)
        for (int c = 0; c < DC_COLS; c++) {
            uint8_t v = dc_cell[r][c];
            if (v & DC_FACE) {
                rem_cnt[v & DC_TYPE_MASK]++;
                rem_total++;
                rem_sum += dc_val[v & DC_TYPE_MASK];
            }
        }
    int avg = rem_total > 0 ? rem_sum / rem_total : 0;

    /* 最优吃子: 得分 = 子值 - (落点被吃 ? 己子值 : 0) */
    int sr = -1, sc = -1, tr = -1, tc = -1, bcscore = 0;
    for (int r = 0; r < DC_ROWS; r++)
        for (int c = 0; c < DC_COLS; c++) {
            uint8_t v = dc_cell[r][c];
            if (!v || (v & DC_FACE) || dc_owner(v) != 2) continue;
            int my = (int)(v & DC_TYPE_MASK);
            for (int i = 0; i < 4; i++) {
                int nx = c + dx4[i], ny = r + dy4[i];
                if (nx < 0 || nx >= DC_COLS || ny < 0 || ny >= DC_ROWS) continue;
                uint8_t e = dc_cell[ny][nx];
                if (!dc_can_eat(e, my, 2)) continue;
                int gain = dc_val[e & DC_TYPE_MASK];
                int risk = 0;
                for (int j = 0; j < 4; j++) {      /* 落点威胁(己方原位已空) */
                    int mx = nx + dx4[j], myy = ny + dy4[j];
                    if (mx < 0 || mx >= DC_COLS || myy < 0 || myy >= DC_ROWS) continue;
                    uint8_t t = dc_cell[myy][mx];
                    if (t && !(t & DC_FACE) && dc_owner(t) == 1 &&
                        dc_beats((int)(t & DC_TYPE_MASK), my))
                        risk = 1;
                }
                int score = gain - (risk ? dc_val[my] : 0);
                if (tr < 0 || score > bcscore) {
                    tr = nx; tc = ny; sr = r; sc = c; bcscore = score;
                }
            }
        }

    /* 最优翻子: 剩余均值 + 相邻敌子对决期望 */
    int bfx = -1, bfy = -1, bfscore = 0;
    if (rem_total > 0) {
        for (int r = 0; r < DC_ROWS; r++)
            for (int c = 0; c < DC_COLS; c++) {
                uint8_t v = dc_cell[r][c];
                if (!(v & DC_FACE)) continue;
                int s = avg;
                for (int i = 0; i < 4; i++) {
                    int nx = c + dx4[i], ny = r + dy4[i];
                    if (nx < 0 || nx >= DC_COLS || ny < 0 || ny >= DC_ROWS) continue;
                    uint8_t e = dc_cell[ny][nx];
                    if (e && !(e & DC_FACE) && dc_owner(e) == 1)
                        s += dc_flip_ev((int)(e & DC_TYPE_MASK), rem_total,
                                        rem_cnt, avg);
                }
                if (bfx < 0 || s > bfscore) { bfx = c; bfy = r; bfscore = s; }
            }
    }

    if (tr < 0 && bfx < 0) {           /* 无任何行动 → 玩家胜 */
        dc_over = true;
        dc_winner = 1;
        audio_win();                   /* AI 无行动获胜 */
        led_fx_set(LED_FX_WIN);
        return;
    }
    if (tr >= 0 && (bfx < 0 || bcscore >= bfscore)) {
        uint8_t mv = dc_cell[sr][sc];  /* 吃子: 己子移入敌格 */
        dc_cell[sr][sc] = 0;
        dc_cell[tc][tr] = mv;
        dc_hlx = tr;
        dc_hly = tc;
    } else {
        dc_cell[bfy][bfx] =
            (uint8_t)((dc_cell[bfy][bfx] & DC_TYPE_MASK) | DC_OWN_A);
        dc_hlx = bfx;
        dc_hly = bfy;
    }
    dc_hl_ok = true;
    if (!dc_has_action(1)) {           /* 玩家无行动 → AI 胜 */
        dc_over = true;
        dc_winner = 2;
        audio_lose();                  /* AI 胜 */
        led_fx_set(LED_FX_LOSE);
    }
}

/* ---- 玩家行动: 翻开或吃子; 返回是否发生行动 ---- */
static bool dc_player_act(void) {
    uint8_t v = dc_cell[dc_cy][dc_cx];
    if (!v) return false;
    if (v & DC_FACE) {                 /* 翻开: 归翻出方(玩家) */
        dc_cell[dc_cy][dc_cx] = (uint8_t)((v & DC_TYPE_MASK) | DC_OWN_P);
        dc_hl_ok = false;
        if (!dc_has_action(2)) {       /* AI 无行动 → 玩家胜 */
            dc_over = true;
            dc_winner = 1;
            audio_win();               /* 翻子获胜 */
            led_fx_set(LED_FX_WIN);
        } else {
            dc_ai_pending = true;
            dc_ai_at = now_ms() + DC_AI_MS;
        }
        return true;
    }
    if (dc_owner(v) != 1) return false;
    /* 吃子: 挑价值最高的可吃敌子 */
    int bx = -1, by = -1, best = 0;
    int my = (int)(v & DC_TYPE_MASK);
    if (dc_cy > 0 && dc_can_eat(dc_cell[dc_cy - 1][dc_cx], my, 1)) {
        int val = dc_val[dc_cell[dc_cy - 1][dc_cx] & DC_TYPE_MASK];
        if (val > best) { best = val; bx = dc_cx; by = dc_cy - 1; }
    }
    if (dc_cy + 1 < DC_ROWS && dc_can_eat(dc_cell[dc_cy + 1][dc_cx], my, 1)) {
        int val = dc_val[dc_cell[dc_cy + 1][dc_cx] & DC_TYPE_MASK];
        if (val > best) { best = val; bx = dc_cx; by = dc_cy + 1; }
    }
    if (dc_cx > 0 && dc_can_eat(dc_cell[dc_cy][dc_cx - 1], my, 1)) {
        int val = dc_val[dc_cell[dc_cy][dc_cx - 1] & DC_TYPE_MASK];
        if (val > best) { best = val; bx = dc_cx - 1; by = dc_cy; }
    }
    if (dc_cx + 1 < DC_COLS && dc_can_eat(dc_cell[dc_cy][dc_cx + 1], my, 1)) {
        int val = dc_val[dc_cell[dc_cy][dc_cx + 1] & DC_TYPE_MASK];
        if (val > best) { best = val; bx = dc_cx + 1; by = dc_cy; }
    }
    if (bx < 0) return false;
    dc_cell[dc_cy][dc_cx] = 0;         /* 己子移入敌格 */
    dc_cell[by][bx] = v;
    dc_hl_ok = false;
    if (!dc_has_action(2)) {
        dc_over = true;
        dc_winner = 1;
        audio_win();                   /* 吃子获胜 */
        led_fx_set(LED_FX_WIN);
    } else {
        dc_ai_pending = true;
        dc_ai_at = now_ms() + DC_AI_MS;
    }
    return true;
}

/* ---- 渲染 ---- */
/* 3x 放大字形(15x21), g 为 7 字节字形 */
static void dc_blit3(int x, int y, const uint8_t g[7], bool black) {
    for (int j = 0; j < FONT_H; j++)
        for (int i = 0; i < FONT_W; i++)
            if (g[j] & (1u << i))
                fb_fill_rect(x + i * 3, y + j * 3, 3, 3, black);
}
#define DC_GX ((DC_CELL - FONT_W * 3) / 2)   /* 9 */
#define DC_GY ((DC_CELL - FONT_H * 3) / 2)   /* 6 */

/* "YOU 12  AI 10" 计数文本 */
static void dc_counts(char *buf) {
    int p = dc_count(1), a = dc_count(2);
    int n = 0;
    const char *s = "YOU ";
    while (s[n]) { buf[n] = s[n]; n++; }
    if (p >= 10) buf[n++] = (char)('0' + p / 10);
    buf[n++] = (char)('0' + p % 10);
    buf[n++] = ' ';
    buf[n++] = ' ';
    s = "AI ";
    int k = 0;
    while (s[k]) { buf[n++] = s[k++]; }
    if (a >= 10) buf[n++] = (char)('0' + a / 10);
    buf[n++] = (char)('0' + a % 10);
    buf[n] = 0;
}

static void dc_draw_cursor(void) {
    if (dc_ai_pending || dc_over) return;      /* AI 回合/终局不显示光标 */
    int x = DC_OX + dc_cx * DC_CELL;
    int y = DC_OY + dc_cy * DC_CELL;
    bool white = (dc_cell[dc_cy][dc_cx] & DC_OWN_A) != 0;  /* 黑底格用白边 */
    fb_stroke_rect_thick(x, y, DC_CELL, DC_CELL, 2, !white);
}

static void dc_draw_hud(void) {
    char buf[16];
    if (dc_over) {
        fb_fill_rect(0, 0, CCG_W, CCG_HUD_H - 1, false);
        fb_text(2, 0, dc_winner == 1 ? "YOU WIN!" : "AI WINS", true);
        fb_text(CCG_W - 2 - text_width("OK/N:RETRY BACK:QUIT"), 0,
                "OK/N:RETRY BACK:QUIT", true);
        dc_counts(buf);
        fb_text(2, 8, buf, true);
        if (!dc_over_full) { dc_over_full = true; disp_force_full(); }
        return;
    }
    fb_text(2, 0, "DARK CHESS", true);
    dc_counts(buf);
    fb_text(CCG_W - 4 - text_width(buf), 0, buf, true);
    fb_text(2, 8, dc_ai_pending ? "AI ..." : "YOUR TURN", true);
    fb_text(CCG_W - 4 - text_width("BACK:MENU"), 8, "BACK:MENU", true);
    fb_hline(0, CCG_HUD_H - 1, CCG_W, true);
}

void darkchess_render(void) {
    fb_clear(false);
    dc_ai_exec(now_ms());              /* tick 未注册时兜底: 到期即行动 */
    /* 网格 */
    for (int c = 0; c <= DC_COLS; c++)
        fb_vline(DC_OX + c * DC_CELL, DC_OY, DC_BOARD_H, true);
    for (int r = 0; r <= DC_ROWS; r++)
        fb_hline(DC_OX, DC_OY + r * DC_CELL, DC_BOARD_W, true);
    /* 棋子: 背面=图案+方框标记; 翻开=字母(玩家黑字/AI 反白) */
    for (int r = 0; r < DC_ROWS; r++)
        for (int c = 0; c < DC_COLS; c++) {
            uint8_t v = dc_cell[r][c];
            if (!v) continue;
            int x = DC_OX + c * DC_CELL;
            int y = DC_OY + r * DC_CELL;
            if (v & DC_FACE) {
                fb_fill_rect(x + 1, y + 1, DC_CELL - 2, DC_CELL - 2, false);
                fb_fill_tile(x + 2, y + 2, DC_CELL - 4, DC_CELL - 4,
                             pat_get(PAT_SLASH_S));
                dc_blit3(x + DC_GX, y + DC_GY, font_symbols[CG_SQUARE_OPEN],
                         true);
            } else if (dc_owner(v) == 1) {
                dc_blit3(x + DC_GX, y + DC_GY,
                         font_glyph5x7[(unsigned char)
                                       dc_letters[v & DC_TYPE_MASK]], true);
            } else {
                fb_fill_rect(x + 1, y + 1, DC_CELL - 2, DC_CELL - 2, true);
                dc_blit3(x + DC_GX, y + DC_GY,
                         font_glyph5x7[(unsigned char)
                                       dc_letters[v & DC_TYPE_MASK]], false);
            }
        }
    /* AI 上次行动格高亮(所有格子之后画) */
    if (dc_hl_ok) {
        int x = DC_OX + dc_hlx * DC_CELL;
        int y = DC_OY + dc_hly * DC_CELL;
        bool white = (dc_cell[dc_hly][dc_hlx] & DC_OWN_A) != 0;
        fb_stroke_rect(x + 1, y + 1, DC_CELL - 2, DC_CELL - 2, !white);
    }
    dc_draw_cursor();
    dc_draw_hud();
}

/* ---- 暂停 ---- */
void darkchess_enter(void);
static void dc_pause(void) {
    pause_sel_t sel;
    if (ui_pause_run(&sel)) {
        if (sel == PAUSE_RESTART) darkchess_enter();
    } else {
        s_exit_request = true;
    }
}
void darkchess_enter(void) {
    rng_seed(&dc_rng, now_ms() ^ 0xDC7u);
    dc_new();
    darkchess_render();
    disp_full();
}

void darkchess_exit(void) {}

void darkchess_tick(uint64_t now) {
    dc_ai_exec(now);
}

void darkchess_on_key(const key_event_t *ev) {
    /* 重复事件只响应方向键 */
    if (ev->is_repeat && ev->key != K_LEFT && ev->key != K_RIGHT &&
        ev->key != K_UP && ev->key != K_DOWN)
        return;
    if (dc_ai_pending) {               /* AI 回合: 只响应暂停/退出 */
        if (ev->key == K_BACK || ev->key == K_PAUSE) dc_pause();
        else if (ev->key == K_QUIT) s_exit_request = true;
        return;
    }
    if (dc_over) {
        if (ev->key == K_OK || (ev->key == K_CHAR && ev->ch == 'n'))
            darkchess_enter();
        else if (ev->key == K_BACK || ev->key == K_QUIT)
            s_exit_request = true;
        return;
    }
    switch (ev->key) {
    case K_LEFT: if (dc_cx > 0) dc_cx--; break;
    case K_RIGHT: if (dc_cx < DC_COLS - 1) dc_cx++; break;
    case K_UP: if (dc_cy > 0) dc_cy--; break;
    case K_DOWN: if (dc_cy < DC_ROWS - 1) dc_cy++; break;
    case K_OK:
        dc_player_act();
        break;
    case K_CHAR:
        if (ev->ch == 'a' && dc_cx > 0) dc_cx--;
        else if (ev->ch == 'd' && dc_cx < DC_COLS - 1) dc_cx++;
        else if (ev->ch == 'w' && dc_cy > 0) dc_cy--;
        else if (ev->ch == 's' && dc_cy < DC_ROWS - 1) dc_cy++;
        else if (ev->ch == 'n') darkchess_enter();
        break;
    case K_BACK:
    case K_PAUSE: dc_pause(); break;
    case K_QUIT: s_exit_request = true; break;
    default: break;
    }
}
