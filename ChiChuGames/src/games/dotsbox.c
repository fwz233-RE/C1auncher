/* DOTS AND BOXES — 4x4 格点格棋(玩家 vs 简单 AI)
 * 5x5 顶点网格, 32px 格(128x128 居中, y 从 18), 共 40 条边
 * 轮流画边; 画完 1x1 格的第四条边 → 该格归属当前玩家并再走一次
 * 全部边画完 → 格多者胜; 光标沿边移动(方向键), OK 画边
 * AI: 1)优先完成自己的格 2)其次不给对手制造 3 边格 3)最后随机(最小送格)
 * 输入驱动; 画边/移动=快刷; 开局/结束=全刷 */
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
#include <stddef.h>

void dotsbox_render(void);

#define DB_GRID 4               /* 每边格数 */
#define DB_NV (DB_GRID + 1)     /* 每边顶点数(5) */
#define DB_CELL 32              /* 格边长 px */
#define DB_BOARD (DB_GRID * DB_CELL)    /* 128 */
#define DB_OX ((CCG_W - DB_BOARD) / 2)  /* 84, 水平居中 */
#define DB_OY 18                /* 顶栏 16px 下留 2px */
#define DB_EH_COUNT (DB_NV * DB_GRID)   /* 横边 20 */
#define DB_EDGES (DB_EH_COUNT * 2)      /* 总边数 40 */

/* ---- 状态(静态前缀 db_) ---- */
static uint8_t db_edges[DB_EDGES];      /* 0=空 1=玩家 2=AI */
static uint8_t db_cells[DB_GRID * DB_GRID]; /* 格归属 0=空 1=玩家 2=AI */
static int db_seg;                      /* 光标: 0=横边 1=竖边 */
static int db_ex, db_ey;                /* 横边: ex 0..3, ey 0..4; 竖边: ex 0..4, ey 0..3 */
static int db_turn;                     /* 1=玩家 2=AI */
static int db_moves;                    /* 已画边数 0..40 */
static bool db_over, db_over_full;
static rng_t db_rng;

/* ---- 边/格索引 ---- */
static int db_eh(int r, int c) { return r * DB_GRID + c; }        /* 横边: 行 r(0..4) 列 c(0..3) */
static int db_ev(int c, int r) { return DB_EH_COUNT + c * DB_GRID + r; } /* 竖边: 列 c(0..4) 行 r(0..3) */

/* 格 (br,bc) 已画边数 */
static int db_sides(int br, int bc) {
    int n = 0;
    if (db_edges[db_eh(br, bc)]) n++;
    if (db_edges[db_eh(br + 1, bc)]) n++;
    if (db_edges[db_ev(bc, br)]) n++;
    if (db_edges[db_ev(bc + 1, br)]) n++;
    return n;
}

/* 画边并结算捕获; 返回捕获格数(0 则换手, 调用方保证该边未画) */
static int db_place(int owner, int idx) {
    db_edges[idx] = (uint8_t)owner;
    db_moves++;
    int cap = 0;
    if (idx < DB_EH_COUNT) {            /* 横边: 邻格为上格/下格 */
        int r = idx / DB_GRID, c = idx % DB_GRID;
        if (r > 0 && db_sides(r - 1, c) == 4 && !db_cells[(r - 1) * DB_GRID + c]) {
            db_cells[(r - 1) * DB_GRID + c] = (uint8_t)owner;
            cap++;
        }
        if (r < DB_GRID && db_sides(r, c) == 4 && !db_cells[r * DB_GRID + c]) {
            db_cells[r * DB_GRID + c] = (uint8_t)owner;
            cap++;
        }
    } else {                            /* 竖边: 邻格为左格/右格 */
        int c = (idx - DB_EH_COUNT) / DB_GRID;
        int r = (idx - DB_EH_COUNT) % DB_GRID;
        if (c > 0 && db_sides(r, c - 1) == 4 && !db_cells[r * DB_GRID + (c - 1)]) {
            db_cells[r * DB_GRID + (c - 1)] = (uint8_t)owner;
            cap++;
        }
        if (c < DB_GRID && db_sides(r, c) == 4 && !db_cells[r * DB_GRID + c]) {
            db_cells[r * DB_GRID + c] = (uint8_t)owner;
            cap++;
        }
    }
    if (cap == 0) db_turn = 3 - owner;
    if (cap > 0) audio_clear();        /* 完成一格 */
    return cap;
}

/* 边 idx 补上后能立即完成的格数 */
static int db_capture_count(int idx) {
    int n = 0;
    if (idx < DB_EH_COUNT) {
        int r = idx / DB_GRID, c = idx % DB_GRID;
        if (r > 0 && db_sides(r - 1, c) == 3) n++;
        if (r < DB_GRID && db_sides(r, c) == 3) n++;
    } else {
        int c = (idx - DB_EH_COUNT) / DB_GRID;
        int r = (idx - DB_EH_COUNT) % DB_GRID;
        if (c > 0 && db_sides(r, c - 1) == 3) n++;
        if (c < DB_GRID && db_sides(r, c) == 3) n++;
    }
    return n;
}

/* 边 idx 会送给对手的 3 边格数(对手下一步可直接完成) */
static int db_gift_count(int idx) {
    int n = 0;
    if (idx < DB_EH_COUNT) {
        int r = idx / DB_GRID, c = idx % DB_GRID;
        if (r > 0 && db_sides(r - 1, c) == 2) n++;
        if (r < DB_GRID && db_sides(r, c) == 2) n++;
    } else {
        int c = (idx - DB_EH_COUNT) / DB_GRID;
        int r = (idx - DB_EH_COUNT) % DB_GRID;
        if (c > 0 && db_sides(r, c - 1) == 2) n++;
        if (c < DB_GRID && db_sides(r, c) == 2) n++;
    }
    return n;
}

/* AI 选边: 1)优先完成格(随机均分) 2)其次随机安全边(不送格) 3)最后最小送格 */
static int db_ai_move(void) {
    int cap[DB_EDGES]; int nc = 0, best_cap = 0;
    int safe[DB_EDGES]; int ns = 0;
    int give[DB_EDGES]; int ng = 0, best_give = 99;
    for (int idx = 0; idx < DB_EDGES; idx++) {
        if (db_edges[idx] != 0) continue;
        int c = db_capture_count(idx);
        if (c > 0) {
            if (c > best_cap) { best_cap = c; nc = 0; cap[nc++] = idx; }
            else if (c == best_cap && nc < DB_EDGES) cap[nc++] = idx;
            continue;
        }
        int g = db_gift_count(idx);
        if (g == 0) {
            if (ns < DB_EDGES) safe[ns++] = idx;
        } else {
            if (g < best_give) { best_give = g; ng = 0; give[ng++] = idx; }
            else if (g == best_give && ng < DB_EDGES) give[ng++] = idx;
        }
    }
    if (nc > 0) return cap[(int)rng_range(&db_rng, (uint32_t)nc)];
    if (ns > 0) return safe[(int)rng_range(&db_rng, (uint32_t)ns)];
    return give[(int)rng_range(&db_rng, (uint32_t)ng)]; /* 调用方保证还有空边 */
}

static int db_count(int owner) {
    int n = 0;
    for (int i = 0; i < DB_GRID * DB_GRID; i++)
        if (db_cells[i] == owner) n++;
    return n;
}

/* 光标所在边索引 */
static int db_cursor_edge(void) {
    return (db_seg == 0) ? db_eh(db_ey, db_ex) : db_ev(db_ex, db_ey);
}

/* 移动光标到相邻边(沿边网; 边界处转到同顶点的垂直边) */
static void db_cursor_move(int dir) {   /* 0=UP 1=DOWN 2=LEFT 3=RIGHT */
    if (db_seg == 0) {                  /* 横边 H(ex,ey): ex 0..3, ey 0..4 */
        if (dir == 0) {
            if (db_ey > 0) db_ey--;
            else db_seg = 1;            /* 顶行 → V(ex,0) */
        } else if (dir == 1) {
            if (db_ey < DB_NV - 1) db_ey++;
            else { db_seg = 1; db_ey = DB_GRID - 1; }   /* 底行 → V(ex,3) */
        } else if (dir == 2) {
            if (db_ex > 0) db_ex--;
            else { db_seg = 1; if (db_ey > DB_GRID - 1) db_ey = DB_GRID - 1; } /* → V(0,ey) */
        } else {
            if (db_ex < DB_GRID - 1) db_ex++;
            else { db_seg = 1; db_ex = DB_NV - 1; if (db_ey > DB_GRID - 1) db_ey = DB_GRID - 1; } /* → V(4,ey) */
        }
    } else {                            /* 竖边 V(ex,ey): ex 0..4, ey 0..3 */
        if (dir == 0) {
            if (db_ey > 0) db_ey--;
            else { db_seg = 0; db_ey = 0; if (db_ex > DB_GRID - 1) db_ex = DB_GRID - 1; } /* → H(ex,0) */
        } else if (dir == 1) {
            if (db_ey < DB_GRID - 1) db_ey++;
            else { db_seg = 0; db_ey = DB_NV - 1; if (db_ex > DB_GRID - 1) db_ex = DB_GRID - 1; } /* → H(ex,4) */
        } else if (dir == 2) {
            if (db_ex > 0) db_ex--;
            else db_seg = 0;            /* → H(0,ey) */
        } else {
            if (db_ex < DB_NV - 1) db_ex++;
            else { db_seg = 0; db_ex = DB_GRID - 1; }   /* → H(3,ey) */
        }
    }
}

static void db_check_end(void) {
    if (db_moves >= DB_EDGES) {
        db_over = true;
        if (db_count(1) > db_count(2)) {
            audio_win();               /* 玩家格多 */
            led_fx_set(LED_FX_WIN);
        } else if (db_count(2) > db_count(1)) {
            audio_lose();              /* AI 格多 */
            led_fx_set(LED_FX_LOSE);
        }
    }
}

static void db_new(void) {
    for (int i = 0; i < DB_EDGES; i++) db_edges[i] = 0;
    for (int i = 0; i < DB_GRID * DB_GRID; i++) db_cells[i] = 0;
    db_seg = 0;
    db_ex = 0;
    db_ey = 0;
    db_turn = 1;
    db_moves = 0;
    db_over = false;
    db_over_full = false;
    rng_seed(&db_rng, now_ms() ^ 0xDBB0u);
}

void dotsbox_enter(void) {
    db_new();
    dotsbox_render();
    disp_full();
}

void dotsbox_exit(void) {}

void dotsbox_tick(uint64_t now) { (void)now; }

/* ---- 绘制 ---- */
static void db_xor_px(int x, int y) {
    int off = (y >> 3) * (int)CCG_W + x;
    g_fb[off] ^= (uint8_t)(0x80 >> (y & 7));
}

/* 光标: 沿边线的 5px 宽反色带(每像素只翻转一次; 黑白/已画边上都可见, 最后画) */
static void db_draw_cursor(void) {
    int x0, y0, x1, y1;
    if (db_seg == 0) {
        x0 = DB_OX + db_ex * DB_CELL;
        y0 = DB_OY + db_ey * DB_CELL;
        x1 = DB_OX + (db_ex + 1) * DB_CELL;
        y1 = y0;
    } else {
        x0 = DB_OX + db_ex * DB_CELL;
        y0 = DB_OY + db_ey * DB_CELL;
        x1 = x0;
        y1 = DB_OY + (db_ey + 1) * DB_CELL;
    }
    for (int t = -2; t <= 2; t++) {
        if (db_seg == 0) {
            for (int x = x0 - 2; x <= x1 + 2; x++) db_xor_px(x, y0 + t);
        } else {
            for (int y = y0 - 2; y <= y1 + 2; y++) db_xor_px(x0 + t, y);
        }
    }
}

/* 十进制小数字(反序填充) */
static void db_num(char *buf, int v) {
    unsigned i = 0;
    if (v == 0) { buf[i++] = '0'; }
    while (v > 0 && i < 4) { buf[i++] = (char)('0' + v % 10); v /= 10; }
    buf[i] = 0;
    for (unsigned j = 0; j < i / 2; j++) {
        char t = buf[j];
        buf[j] = buf[i - 1 - j];
        buf[i - 1 - j] = t;
    }
}

/* 右栏: 小字标签 + 2x 反白大字数值 */
static void db_panel_num(const char *label, int val, int y) {
    fb_text(CCG_W - 2 - text_width(label), y, label, true);
    char num[4];
    db_num(num, val);
    int w = text_width(num) * 2;
    fb_fill_rect(CCG_W - 8 - w, y + 8, w + 8, 16, true);
    fb_text_scale2(CCG_W - 4 - w, y + 10, num, false);
}

void dotsbox_render(void) {
    fb_clear(false);
    /* HUD 顶栏: 左标题, 右 "P n AI m" */
    fb_text(0, 0, "DOTSBOX", true);
    {
        char pl[4], al[4], buf[16];
        db_num(pl, db_count(1));
        db_num(al, db_count(2));
        unsigned i = 0;
        buf[i++] = 'P'; buf[i++] = ' ';
        for (unsigned j = 0; pl[j] && i < 14; j++) buf[i++] = pl[j];
        buf[i++] = ' '; buf[i++] = 'A'; buf[i++] = 'I'; buf[i++] = ' ';
        for (unsigned j = 0; al[j] && i < 14; j++) buf[i++] = al[j];
        buf[i] = 0;
        fb_text(CCG_W - 4 - text_width(buf), 0, buf, true);
    }
    fb_hline(0, CCG_HUD_H - 1, CCG_W, true);

    /* 已画边(3px 粗黑线) */
    for (int r = 0; r < DB_NV; r++)
        for (int c = 0; c < DB_GRID; c++)
            if (db_edges[db_eh(r, c)])
                fb_fill_rect(DB_OX + c * DB_CELL, DB_OY + r * DB_CELL, DB_CELL + 1, 3, true);
    for (int c = 0; c < DB_NV; c++)
        for (int r = 0; r < DB_GRID; r++)
            if (db_edges[db_ev(c, r)])
                fb_fill_rect(DB_OX + c * DB_CELL, DB_OY + r * DB_CELL, 3, DB_CELL + 1, true);
    /* 顶点: 3x3 实心小方块 */
    for (int vy = 0; vy < DB_NV; vy++)
        for (int vx = 0; vx < DB_NV; vx++)
            fb_fill_rect(DB_OX + vx * DB_CELL - 1, DB_OY + vy * DB_CELL - 1, 3, 3, true);
    /* 格标记: 玩家=实心方块, AI=空心方块 */
    for (int r = 0; r < DB_GRID; r++)
        for (int c = 0; c < DB_GRID; c++) {
            if (db_cells[r * DB_GRID + c] == 1)
                fb_fill_rect(DB_OX + c * DB_CELL + 8, DB_OY + r * DB_CELL + 8, 16, 16, true);
            else if (db_cells[r * DB_GRID + c] == 2)
                fb_stroke_rect(DB_OX + c * DB_CELL + 8, DB_OY + r * DB_CELL + 8, 16, 16, true);
        }
    /* 光标(最后画) */
    db_draw_cursor();

    /* 左栏: 回合指示 + 底部提示 */
    fb_text(2, 18, "TURN", true);
    {
        const char *who = (db_turn == 1) ? "YOU" : "AI";
        int w = text_width(who) * 2;
        fb_fill_rect(2, 26, w + 8, 16, true);
        fb_text_scale2(6, 28, who, false);
    }
    fb_text(2, 125, "N:NEW", true);
    fb_text(2, 134, "BACK:MENU", true);
    /* 右栏: 双方格数 */
    db_panel_num("YOU", db_count(1), 18);
    db_panel_num("AI", db_count(2), 52);

    /* 结束: HUD 区结果 + 操作提示 + 全刷一次 */
    if (db_over) {
        fb_fill_rect(0, 0, CCG_W, CCG_HUD_H - 1, false);
        int p = db_count(1), a = db_count(2);
        const char *res = (p > a) ? "YOU WIN" : (a > p) ? "AI WINS" : "DRAW";
        fb_text(2, 2, res, true);
        fb_text(CCG_W - 2 - text_width("OK/N:RETRY BACK:QUIT"), 2,
                "OK/N:RETRY BACK:QUIT", true);
        if (!db_over_full) { db_over_full = true; disp_force_full(); }
    }
}

void dotsbox_on_key(const key_event_t *ev) {
    /* 重复事件只响应方向键(确认/字母必须忽略) */
    if (ev->is_repeat && ev->key != K_LEFT && ev->key != K_RIGHT &&
        ev->key != K_UP && ev->key != K_DOWN)
        return;
    if (db_over) {
        if (ev->key == K_OK || (ev->key == K_CHAR && ev->ch == 'n'))
            dotsbox_enter();
        else if (ev->key == K_BACK || ev->key == K_QUIT)
            s_exit_request = true;
        return;
    }
    switch (ev->key) {
    case K_UP: db_cursor_move(0); break;
    case K_DOWN: db_cursor_move(1); break;
    case K_LEFT: db_cursor_move(2); break;
    case K_RIGHT: db_cursor_move(3); break;
    case K_OK:
        if (db_turn == 1) {
            int idx = db_cursor_edge();
            if (!db_edges[idx]) {
                db_place(1, idx);
                db_check_end();
                while (db_turn == 2 && !db_over) {
                    db_place(2, db_ai_move());
                    db_check_end();
                }
            } else {
                audio_error();         /* 该边已画 */
            }
        }
        break;
    case K_CHAR:
        if (ev->ch == 'w') db_cursor_move(0);
        else if (ev->ch == 's') db_cursor_move(1);
        else if (ev->ch == 'a') db_cursor_move(2);
        else if (ev->ch == 'd') db_cursor_move(3);
        else if (ev->ch == 'n') dotsbox_enter();
        break;
    case K_BACK:
    case K_PAUSE: {
        pause_sel_t sel;
        if (ui_pause_run(&sel)) {
            if (sel == PAUSE_RESTART) dotsbox_enter();
        } else {
            s_exit_request = true;
        }
        break;
    }
    case K_QUIT: s_exit_request = true; break;
    default: break;
    }
}
