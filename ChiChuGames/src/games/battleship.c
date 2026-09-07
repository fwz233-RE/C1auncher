/* BATTLESHIP — 海战棋 vs AI (8x8 双海图)
 * 左侧玩家海图: 布置 5 船(3/2/2/1/1), 方向键移光标, SHIFT/R 旋转, OK 放置
 * 右侧敌方海图: 攻击阶段 OK 开火; 命中=实心圆, 未中=点, 沉船=格内 X
 * AI: 命中后邻格优先(寻线方向最优先), 沉船清队列, 无目标随机搜索
 * 玩家先手; AI 回合由 tick(700ms) 驱动, 视觉指示=双图间箭头+HUD 标签
 * 布局: 17px 格 → 136x136 双图并排, 最大化 296x152(16px 顶栏下紧贴)
 * 静态前缀 bs_ */
#include "game.h"
#include "../config.h"
#include "../gfx/canvas.h"
#include "../gfx/font.h"
#include "../gfx/pattern.h"
#include "../platform/audio.h"
#include "../platform/display.h"
#include "../platform/input.h"
#include "../platform/led.h"
#include "../rng.h"
#include "../platform/time.h"
#include <string.h>

#define BS_N 8                     /* 海图 8x8 */
#define BS_CELL 17                 /* 正方形格 17px(136 = 游戏区满高) */
#define BS_BOARD (BS_N * BS_CELL)  /* 136 */
#define BS_OY CCG_HUD_H            /* 顶栏 16px 下紧贴 */
#define BS_OX1 ((CCG_W - 2 * BS_BOARD - 8) / 2)   /* 左图 x=8 */
#define BS_OX2 (BS_OX1 + BS_BOARD + 8)            /* 右图 x=152, 间隙 8px */
#define BS_NSHIP 5
#define BS_PLACE 0
#define BS_ATTACK 1

void battleship_render(void);

static const uint8_t bs_lens[BS_NSHIP] = { 3, 2, 2, 1, 1 };

static uint8_t bs_ship[2][BS_N * BS_N];  /* 1=船格; [0]=玩家海图 [1]=敌方海图 */
static uint8_t bs_shot[2][BS_N * BS_N];  /* 对该海图的射击: 0=未 1=命中 2=未中 */
static int bs_left[2];                    /* 每方剩余船数 */
static int bs_phase;                      /* BS_PLACE / BS_ATTACK */
static int bs_cur;                        /* 布置中的船索引 0..4 */
static int bs_dir;                        /* 0=水平 1=垂直 */
static int bs_cx, bs_cy;                  /* 光标 */
static int bs_turn;                       /* 1=玩家 2=AI */
static bool bs_ai_pending;                /* AI 待行动(tick 驱动) */
static bool bs_over, bs_over_full;
static int bs_winner;                     /* 1=玩家 2=AI */
static rng_t bs_rng;

/* AI 目标队列(环, 元素打包 y*8+x) */
static uint8_t bs_aiq[BS_N * BS_N];
static int bs_aiq_head, bs_aiq_n;
static int8_t bs_ai_lastx, bs_ai_lasty;   /* 上次 AI 命中(-1=无) */

/* 该格本身或 8 邻域有船(越界忽略); 含自身 → 重叠/相邻一次检查 */
static bool bs_blocked(int side, int x, int y) {
    int dy, dx;
    for (dy = -1; dy <= 1; dy++)
        for (dx = -1; dx <= 1; dx++) {
            int nx = x + dx, ny = y + dy;
            if (nx < 0 || ny < 0 || nx >= BS_N || ny >= BS_N) continue;
            if (bs_ship[side][ny * BS_N + nx]) return true;
        }
    return false;
}

/* (x,y) 起 dir 方向 len 格能否放: 边界内且全部空格+无邻船 */
static bool bs_fit(int side, int x, int y, int dir, int len) {
    int i;
    if (dir == 0) {
        if (x + len > BS_N) return false;
        for (i = 0; i < len; i++)
            if (bs_blocked(side, x + i, y)) return false;
    } else {
        if (y + len > BS_N) return false;
        for (i = 0; i < len; i++)
            if (bs_blocked(side, x, y + i)) return false;
    }
    return true;
}

static void bs_stamp(int side, int x, int y, int dir, int len) {
    int i;
    for (i = 0; i < len; i++)
        bs_ship[side][(y + (dir ? i : 0)) * BS_N + (x + (dir ? 0 : i))] = 1;
}

/* 随机放 5 船到敌方海图(不重叠+互不相邻); 随机兜底顺序扫描 */
static void bs_place_fleet_ai(void) {
    int s;
    for (s = 0; s < BS_NSHIP; s++) {
        int len = bs_lens[s];
        bool placed = false;
        int guard = 0;
        while (!placed && guard < 300) {
            int dir = (int)rng_range(&bs_rng, 2);
            int x = (int)rng_range(&bs_rng, (uint32_t)(dir ? BS_N : BS_N - len + 1));
            int y = (int)rng_range(&bs_rng, (uint32_t)(dir ? BS_N - len + 1 : BS_N));
            guard++;
            if (bs_fit(1, x, y, dir, len)) {
                bs_stamp(1, x, y, dir, len);
                placed = true;
            }
        }
        if (!placed) {   /* 兜底: 顺序扫描(网格远未满, 必成功) */
            int dir, x, y;
            for (dir = 0; dir < 2 && !placed; dir++)
                for (x = 0; x < BS_N && !placed; x++)
                    for (y = 0; y < BS_N && !placed; y++)
                        if (bs_fit(1, x, y, dir, len)) {
                            bs_stamp(1, x, y, dir, len);
                            placed = true;
                        }
        }
    }
}

/* 玩家布置当前船(调用方已用 bs_fit 验证) */
static void bs_place_player(void) {
    bs_stamp(0, bs_cx, bs_cy, bs_dir, bs_lens[bs_cur]);
    bs_cur++;
    audio_select();               /* 放置成功 */
    if (bs_cur >= BS_NSHIP) {
        bs_phase = BS_ATTACK;
        bs_turn = 1;
        bs_cx = 3;
        bs_cy = 3;
    }
}

/* (x,y) 所在船是否已全命中(沉没); 船格互不相邻 → 洪水即整船 */
static bool bs_sunk(int side, int x, int y) {
    static const int dx4[4] = { 1, -1, 0, 0 };
    static const int dy4[4] = { 0, 0, 1, -1 };
    int cells[12][2];
    int cnt = 0, k;
    cells[cnt][0] = x;
    cells[cnt][1] = y;
    cnt++;
    for (k = 0; k < cnt; k++) {
        int d;
        for (d = 0; d < 4; d++) {
            int nx = cells[k][0] + dx4[d], ny = cells[k][1] + dy4[d];
            bool dup = false;
            int j;
            if (nx < 0 || ny < 0 || nx >= BS_N || ny >= BS_N) continue;
            if (!bs_ship[side][ny * BS_N + nx]) continue;
            for (j = 0; j < cnt; j++)
                if (cells[j][0] == nx && cells[j][1] == ny) { dup = true; break; }
            if (!dup) { cells[cnt][0] = nx; cells[cnt][1] = ny; cnt++; }
        }
    }
    for (k = 0; k < cnt; k++)
        if (bs_shot[side][cells[k][1] * BS_N + cells[k][0]] != 1) return false;
    return true;
}

/* 对 side 海图 (x,y) 开火; 返回 0=已开火过 1=未中 2=命中 3=命中且沉没 */
static int bs_fire(int side, int x, int y) {
    int idx = y * BS_N + x;
    if (bs_shot[side][idx] != 0) return 0;
    if (bs_ship[side][idx]) {
        bs_shot[side][idx] = 1;
        bool sunk = bs_sunk(side, x, y);
        if (sunk) {
            bs_left[side]--;
            if (bs_left[side] <= 0) {
                bs_over = true;
                bs_winner = (side == 1) ? 1 : 2;   /* 灭敌=玩家胜 */
                if (bs_winner == 1) {
                    audio_win();
                    led_fx_set(LED_FX_WIN);
                } else {
                    audio_lose();
                    led_fx_set(LED_FX_LOSE);
                }
                return 3;
            }
        }
        audio_clear();            /* 命中/沉船(未终局) */
        return sunk ? 3 : 2;
    }
    bs_shot[side][idx] = 2;
    audio_error();                /* 未中 */
    return 1;
}

/* ---- AI 目标队列(环缓冲) ---- */
static void bs_aiq_clear(void) {
    bs_aiq_head = 0;
    bs_aiq_n = 0;
}

static void bs_aiq_put(uint8_t c, bool front) {
    if (bs_aiq_n >= BS_N * BS_N) return;
    if (front) {
        bs_aiq_head = (bs_aiq_head + BS_N * BS_N - 1) % (BS_N * BS_N);
        bs_aiq[bs_aiq_head] = c;
    } else {
        bs_aiq[(bs_aiq_head + bs_aiq_n) % (BS_N * BS_N)] = c;
    }
    bs_aiq_n++;
}

static uint8_t bs_aiq_get(void) {
    uint8_t c = bs_aiq[bs_aiq_head];
    bs_aiq_head = (bs_aiq_head + 1) % (BS_N * BS_N);
    bs_aiq_n--;
    return c;
}

/* AI 命中 (x,y): 更新寻线方向, 邻格入队(线方向格插队最优先) */
static void bs_ai_on_hit(int x, int y) {
    int ldx = 0, ldy = 0;
    if (bs_ai_lastx >= 0) {
        int dx = x - bs_ai_lastx, dy = y - bs_ai_lasty;
        if (dx * dx + dy * dy == 1) { ldx = dx; ldy = dy; }
    }
    bs_ai_lastx = (int8_t)x;
    bs_ai_lasty = (int8_t)y;
    if (ldx || ldy) {
        int nx = x + ldx, ny = y + ldy;
        if (nx >= 0 && ny >= 0 && nx < BS_N && ny < BS_N)
            bs_aiq_put((uint8_t)(ny * BS_N + nx), true);
    }
    if (x + 1 < BS_N) bs_aiq_put((uint8_t)(y * BS_N + x + 1), false);
    if (x - 1 >= 0)   bs_aiq_put((uint8_t)(y * BS_N + x - 1), false);
    if (y + 1 < BS_N) bs_aiq_put((uint8_t)((y + 1) * BS_N + x), false);
    if (y - 1 >= 0)   bs_aiq_put((uint8_t)((y - 1) * BS_N + x), false);
}

/* AI 回合: 队列优先(命中邻格), 无目标随机未开火格(带 guard + 兜底扫描) */
static void bs_ai_turn(void) {
    int x = -1, y = -1;
    while (bs_aiq_n > 0) {
        uint8_t c = bs_aiq_get();
        if (!bs_shot[0][c]) { x = c & 7; y = c >> 3; break; }
    }
    if (x < 0) {
        int guard = 0;
        do {
            x = (int)rng_range(&bs_rng, BS_N);
            y = (int)rng_range(&bs_rng, BS_N);
            guard++;
        } while (bs_shot[0][y * BS_N + x] != 0 && guard < 256);
        if (bs_shot[0][y * BS_N + x] != 0) {   /* 兜底: 顺序找未开火格 */
            int k;
            for (k = 0; k < BS_N * BS_N; k++)
                if (!bs_shot[0][k]) { x = k & 7; y = k >> 3; break; }
        }
    }
    {
        int r = bs_fire(0, x, y);
        if (r == 2) bs_ai_on_hit(x, y);
        else if (r == 3) { bs_aiq_clear(); bs_ai_lastx = -1; bs_ai_lasty = -1; }
    }
    bs_turn = 1;
}

/* 新局: 空图 + 玩家布置 + AI 布船(播种 rng) */
static void bs_new(void) {
    memset(bs_ship, 0, sizeof bs_ship);
    memset(bs_shot, 0, sizeof bs_shot);
    bs_left[0] = bs_left[1] = BS_NSHIP;
    bs_phase = BS_PLACE;
    bs_cur = 0;
    bs_dir = 0;
    bs_cx = 3;
    bs_cy = 3;
    bs_turn = 1;
    bs_ai_pending = false;
    bs_over = false;
    bs_over_full = false;
    bs_winner = 0;
    bs_aiq_clear();
    bs_ai_lastx = -1;
    bs_ai_lasty = -1;
    rng_seed(&bs_rng, now_ms() ^ 0xBA57A57u);   /* 非零种子防 xorshift 恒 0 */
    bs_place_fleet_ai();
}

void battleship_enter(void) {
    bs_new();
    battleship_render();
    disp_full();
}

/* ---- 绘制 ---- */
static void bs_draw_circle(int cx, int cy, int r, bool black) {
    int dy, dx;
    for (dy = -r; dy <= r; dy++)
        for (dx = -r; dx <= r; dx++)
            if (dx * dx + dy * dy <= r * r) fb_pixel(cx + dx, cy + dy, black);
}

static void bs_draw_x(int x0, int y0, bool black) {
    int t, i;
    for (t = -1; t <= 1; t++)
        for (i = 2; i <= 13; i++) {
            fb_pixel(x0 + i + t, y0 + i, black);
            fb_pixel(x0 + i + t, y0 + 15 - i, black);
        }
}

static void bs_draw_dot(int cx, int cy, bool black) {
    fb_fill_rect(cx - 1, cy - 1, 3, 3, black);
}

static void bs_draw_boards(void) {
    int b, y, x;
    for (b = 0; b < 2; b++) {
        int ox = (b == 0) ? BS_OX1 : BS_OX2;
        int i;
        for (i = 0; i <= BS_N; i++) {
            fb_vline(ox + i * BS_CELL, BS_OY, BS_BOARD, true);
            fb_hline(ox, BS_OY + i * BS_CELL, BS_BOARD, true);
        }
    }
    /* 左图: 玩家船 = 黑块 */
    for (y = 0; y < BS_N; y++)
        for (x = 0; x < BS_N; x++)
            if (bs_ship[0][y * BS_N + x])
                fb_fill_rect(BS_OX1 + x * BS_CELL + 1, BS_OY + y * BS_CELL + 1,
                             BS_CELL - 2, BS_CELL - 2, true);
    /* 左图: 敌弹(后画覆盖黑块) 未中=点 命中=白圆 沉=白X */
    for (y = 0; y < BS_N; y++)
        for (x = 0; x < BS_N; x++) {
            int idx = y * BS_N + x;
            int cx = BS_OX1 + x * BS_CELL + BS_CELL / 2;
            int cy = BS_OY + y * BS_CELL + BS_CELL / 2;
            if (bs_shot[0][idx] == 2)
                bs_draw_dot(cx, cy, true);
            else if (bs_shot[0][idx] == 1) {
                if (bs_sunk(0, x, y))
                    bs_draw_x(BS_OX1 + x * BS_CELL + 1, BS_OY + y * BS_CELL + 1, false);
                else
                    bs_draw_circle(cx, cy, 5, false);
            }
        }
    /* 右图: 玩家射敌 未中=点 命中=黑圆 沉=黑X */
    for (y = 0; y < BS_N; y++)
        for (x = 0; x < BS_N; x++) {
            int idx = y * BS_N + x;
            int cx = BS_OX2 + x * BS_CELL + BS_CELL / 2;
            int cy = BS_OY + y * BS_CELL + BS_CELL / 2;
            if (bs_shot[1][idx] == 2)
                bs_draw_dot(cx, cy, true);
            else if (bs_shot[1][idx] == 1) {
                if (bs_sunk(1, x, y))
                    bs_draw_x(BS_OX2 + x * BS_CELL + 1, BS_OY + y * BS_CELL + 1, true);
                else
                    bs_draw_circle(cx, cy, 6, true);
            }
        }
}

/* 布置期幽灵船: 合法=实心, 不合法=稀疏点 */
static void bs_draw_ghost(void) {
    int len = bs_lens[bs_cur];
    bool ok = bs_fit(0, bs_cx, bs_cy, bs_dir, len);
    const uint8_t *tile = pat_get(PAT_DOT_SPARSE);
    int i;
    for (i = 0; i < len; i++) {
        int gx = bs_cx + (bs_dir ? 0 : i);
        int gy = bs_cy + (bs_dir ? i : 0);
        if (gx >= BS_N || gy >= BS_N) continue;   /* 越界部分不画 */
        int x0 = BS_OX1 + gx * BS_CELL + 1;
        int y0 = BS_OY + gy * BS_CELL + 1;
        if (ok)
            fb_fill_rect(x0, y0, BS_CELL - 2, BS_CELL - 2, true);
        else
            fb_fill_tile(x0, y0, BS_CELL - 2, BS_CELL - 2, tile);
    }
}

/* 光标: 反色边框(XOR), 所有内容绘制之后 */
static void bs_xor_px(int x, int y) {
    if (x < 0 || y < 0 || x >= (int)CCG_W || y >= (int)CCG_H) return;
    int off = (y >> 3) * (int)CCG_W + x;
    g_fb[off] ^= (uint8_t)(0x80 >> (y & 7));
}

static void bs_draw_cursor(void) {
    int ox = (bs_phase == BS_PLACE) ? BS_OX1 : BS_OX2;
    int x0 = ox + bs_cx * BS_CELL;
    int y0 = BS_OY + bs_cy * BS_CELL;
    int t, i;
    for (t = 0; t < 2; t++) {
        for (i = 0; i < BS_CELL; i++) {
            bs_xor_px(x0 + t, y0 + i);
            bs_xor_px(x0 + BS_CELL - 1 - t, y0 + i);
            bs_xor_px(x0 + i, y0 + t);
            bs_xor_px(x0 + i, y0 + BS_CELL - 1 - t);
        }
    }
}

/* 双图间箭头: 玩家回合→指向敌图(右), AI 回合→指向己图(左) */
static void bs_draw_arrow(void) {
    int ax = BS_OX1 + BS_BOARD + (8 - 5) / 2;
    int ay = BS_OY + BS_BOARD / 2 - 3;
    if (bs_turn == 1) fb_symbol(ax, ay, CG_ARROW_RT, true);
    else fb_symbol(ax, ay, CG_ARROW_LT, true);
}

/* 布置期右侧面板: 操作提示 + 舰队图例(已放=实心 当前=反白 待放=边框) */
static void bs_draw_panel(void) {
    int cx = BS_OX2 + BS_BOARD / 2;
    int s;
    fb_text(cx - text_width("PLACE YOUR FLEET") / 2, 22, "PLACE YOUR FLEET", true);
    fb_text(cx - text_width("ARROWS/WASD: MOVE") / 2, 34, "ARROWS/WASD: MOVE", true);
    fb_text(cx - text_width("SHIFT/R: ROTATE") / 2, 44, "SHIFT/R: ROTATE", true);
    fb_text(cx - text_width("OK: PLACE SHIP") / 2, 54, "OK: PLACE SHIP", true);
    fb_text(cx - text_width("BACK: MENU") / 2, 64, "BACK: MENU", true);
    fb_text(cx - text_width("FLEET") / 2, 80, "FLEET", true);
    for (s = 0; s < BS_NSHIP; s++) {
        int w = bs_lens[s] * 7;
        int x = BS_OX2 + 8 + s * 26;
        int y = 92;
        if (s < bs_cur) {
            fb_fill_rect(x, y, w, 8, true);
        } else if (s == bs_cur) {
            fb_fill_rect(x - 3, y - 3, w + 6, 14, true);
            fb_fill_rect(x, y, w, 8, false);
        } else {
            fb_stroke_rect(x, y, w, 8, true);
        }
    }
}

static int bs_num(char *buf, int v) {
    char tmp[6];
    int n = 0, i;
    if (v == 0) tmp[n++] = '0';
    while (v && n < 5) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
    for (i = 0; i < n; i++) buf[i] = tmp[n - 1 - i];
    return n;
}

static void bs_hud(void) {
    char buf[20];
    int n = 0;
    fb_text(0, 0, "BATTLESHIP", true);
    if (bs_phase == BS_PLACE) {
        const char *p = "PLACE ";
        while (*p && n < 19) buf[n++] = *p++;
        n += bs_num(buf + n, bs_cur + 1);
        buf[n++] = '/';
        buf[n++] = '5';
    } else {
        const char *p = (bs_turn == 1) ? "YOU L" : "AI L";
        while (*p && n < 19) buf[n++] = *p++;
        n += bs_num(buf + n, bs_left[0]);
        buf[n++] = ' ';
        buf[n++] = 'R';
        n += bs_num(buf + n, bs_left[1]);
    }
    buf[n] = 0;
    fb_text(CCG_W - 4 - text_width(buf), 0, buf, true);
    fb_hline(0, CCG_HUD_H - 1, CCG_W, true);
}

/* 结束: HUD 区结果 + 操作提示, 全刷一次 */
static void bs_draw_over(void) {
    fb_fill_rect(0, 0, CCG_W, CCG_HUD_H - 1, false);
    fb_text(2, 2, bs_winner == 1 ? "YOU WIN!" : "AI WINS", true);
    fb_text(CCG_W - 2 - text_width("OK/N:RETRY BACK:QUIT"), 2,
            "OK/N:RETRY BACK:QUIT", true);
    if (!bs_over_full) {
        bs_over_full = true;
        disp_force_full();
    }
}

void battleship_render(void) {
    fb_clear(false);
    bs_draw_boards();
    if (bs_phase == BS_PLACE) {
        bs_draw_ghost();
        bs_draw_panel();
    }
    bs_draw_cursor();          /* 光标最后画 */
    if (bs_phase == BS_ATTACK && !bs_over) bs_draw_arrow();
    bs_hud();
    if (bs_over) bs_draw_over();
}

void battleship_tick(uint64_t now) {
    (void)now;
    if (bs_over || !bs_ai_pending) return;
    bs_ai_pending = false;
    bs_ai_turn();
}

void battleship_on_key(const key_event_t *ev) {
    /* 重复只响应方向键(确认/字母忽略) */
    if (ev->is_repeat && ev->key != K_UP && ev->key != K_DOWN &&
        ev->key != K_LEFT && ev->key != K_RIGHT)
        return;
    if (bs_over) {
        if (ev->key == K_OK || (ev->key == K_CHAR && ev->ch == 'n')) {
            battleship_enter();
        } else if (ev->key == K_BACK || ev->key == K_QUIT) {
            s_exit_request = true;
        }
        return;
    }
    if (ev->key == K_QUIT) { s_exit_request = true; return; }
    if (ev->key == K_BACK || ev->key == K_PAUSE) {
        pause_sel_t sel;
        if (ui_pause_run(&sel)) {
            if (sel == PAUSE_RESTART) battleship_enter();
        } else {
            s_exit_request = true;
        }
        return;
    }
    if (bs_phase == BS_PLACE) {
        if (ev->key == K_SHIFT || (ev->key == K_CHAR && ev->ch == 'r')) {
            bs_dir = 1 - bs_dir;
            return;
        }
        if (ev->key == K_OK) {
            if (bs_fit(0, bs_cx, bs_cy, bs_dir, bs_lens[bs_cur])) bs_place_player();
            return;
        }
        if (ev->key == K_CHAR && ev->ch == 'n') { battleship_enter(); return; }
        if (ev->key == K_LEFT || (ev->key == K_CHAR && ev->ch == 'a')) bs_cx--;
        else if (ev->key == K_RIGHT || (ev->key == K_CHAR && ev->ch == 'd')) bs_cx++;
        else if (ev->key == K_UP || (ev->key == K_CHAR && ev->ch == 'w')) bs_cy--;
        else if (ev->key == K_DOWN || (ev->key == K_CHAR && ev->ch == 's')) bs_cy++;
        if (bs_cx < 0) bs_cx = 0; else if (bs_cx >= BS_N) bs_cx = BS_N - 1;
        if (bs_cy < 0) bs_cy = 0; else if (bs_cy >= BS_N) bs_cy = BS_N - 1;
        return;
    }
    /* BS_ATTACK: AI 回合不接收操作(暂停已在上方处理) */
    if (bs_ai_pending) return;
    if (ev->key == K_CHAR && ev->ch == 'n') { battleship_enter(); return; }
    if (ev->key == K_LEFT || (ev->key == K_CHAR && ev->ch == 'a')) bs_cx--;
    else if (ev->key == K_RIGHT || (ev->key == K_CHAR && ev->ch == 'd')) bs_cx++;
    else if (ev->key == K_UP || (ev->key == K_CHAR && ev->ch == 'w')) bs_cy--;
    else if (ev->key == K_DOWN || (ev->key == K_CHAR && ev->ch == 's')) bs_cy++;
    if (bs_cx < 0) bs_cx = 0; else if (bs_cx >= BS_N) bs_cx = BS_N - 1;
    if (bs_cy < 0) bs_cy = 0; else if (bs_cy >= BS_N) bs_cy = BS_N - 1;
    if (ev->key == K_OK) {
        int r = bs_fire(1, bs_cx, bs_cy);
        if (r != 0 && !bs_over) {
            bs_turn = 2;
            bs_ai_pending = true;   /* 下一 tick(700ms) AI 行动 */
        }
    }
}

void battleship_exit(void) {}
