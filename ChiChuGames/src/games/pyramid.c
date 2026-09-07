/* PYRAMID — 金字塔纸牌 (Pyramid Solitaire)
 *
 * 28 张牌摆成 7 层金字塔, 余 24 张为发牌堆。J/Q/K=11/12/13, A=1。
 * 规则: 选两张未被压住的牌凑 13 移除; K(13) 单独移除; 发牌堆逐张
 * 翻到废牌堆, 废牌堆顶可与金字塔牌凑 13; 发牌堆耗尽后 BACK 自动
 * 将废牌堆倒序回收重翻。全金字塔清空 -> WIN; 无步可走 -> NO MOVES。
 *
 * 操作: 方向/WASD 移光标(底部行可到发牌堆/废牌堆槽), OK 选中/取消,
 * 第二次 OK 提交两张(K 重复 OK 单除), BACK 翻发牌堆, P 暂停, N 新局,
 * Q 退出。方向键长按重复由框架合成。
 *
 * 集成帮助页(写入 games_table.c):
 *   help = { "PYRAMID", "PAIR CARDS SUMMING TO 13",
 *            "ARROWS/WASD: MOVE  OK: PICK",
 *            "OK AGAIN: COMMIT (K: DROP ALONE)",
 *            "BACK: DEAL STOCK  P: PAUSE  N: NEW", NULL }
 */
#include "game.h"
#include "../config.h"
#include "../gfx/canvas.h"
#include "../gfx/font.h"
#include "../gfx/pattern.h"
#include "../platform/display.h"
#include "../platform/input.h"
#include "../platform/audio.h"
#include "../platform/led.h"
#include "../platform/time.h"
#include "../rng.h"
#include <stdio.h>

#define PY_CARD_W 28              /* 牌宽 */
#define PY_CARD_H 26              /* 牌高 */
#define PY_HX 14                  /* 行内相邻牌水平偏移(半宽) */
#define PY_HY 7                   /* 行间水平偏移(HX/2), 上层压下层 */
#define PY_V 13                   /* 行垂直步进(每张可见 13px) */
#define PY_ROW_N 7
#define PY_PYR_N 28               /* 7 层: 1+2+3+4+5+6+7 */
#define PY_STOCK_N 24             /* 52-28 */
#define PY_S0 134                 /* X(r,c)=S0+c*HX-r*HY, 各行水平居中 */
#define PY_TOP 18                 /* 金字塔顶 y */
#define PY_BOT_Y 124              /* 底部牌行 y(124..150) */
#define PY_STOCK_X 8
#define PY_WASTE_X 48
#define PY_HINT_X (PY_WASTE_X + PY_CARD_W + 8)
#define PY_HINT_Y (PY_BOT_Y + 9)
#define PY_BACK_N 5               /* 发牌堆可视层数(≤5 张) */

static int  py_val[PY_PYR_N];     /* 金字塔牌值 1..13, 0=已移除 */
static int  py_suit[PY_PYR_N];    /* 花色 0=红桃 1=方块 2=黑桃 3=梅花 */
static int  py_stock[PY_STOCK_N]; /* 发牌堆(面朝下), 按下标递增翻出 */
static int  py_stock_s[PY_STOCK_N];
static int  py_stock_i;           /* 下一张待翻下标, ==24 已发完 */
static int  py_waste[PY_STOCK_N]; /* 废牌堆, 顶=最后一张 */
static int  py_waste_s[PY_STOCK_N];
static int  py_waste_n;
static int  py_selp;              /* 选中的金字塔牌, -1=无 */
static bool py_selw;              /* 是否选中废牌堆顶 */
static int  py_cz;                /* 光标区: 0=金字塔 1=底部行 */
static int  py_cr, py_cc;         /* 金字塔光标行/列 */
static int  py_bc;                /* 底部槽: 0=发牌堆 1=废牌堆 */
static bool py_over, py_won;
static bool py_over_full;         /* 结束全刷只做一次 */
static const char *py_msg;        /* 底部提示, NULL=默认指引 */
static rng_t py_rng;
static uint64_t py_seed_cnt;

static const int py_sym[4] = { CG_HEART, CG_DIAMOND, CG_SPADE, CG_CLUB };

void pyramid_render(void);
void pyramid_enter(void);

static int py_idx(int r, int c) { return r * (r + 1) / 2 + c; }

static int py_row_of(int idx) {
    int r = 0;
    while ((r + 1) * (r + 2) / 2 <= idx) r++;
    return r;
}

/* 是否被压住: 正下方 (r+1,c) 与 (r+1,c+1) 都移除才算露出 */
static bool py_exposed(int idx) {
    int r = py_row_of(idx);
    if (r >= PY_ROW_N - 1) return true;
    int b = idx + r + 1;
    return py_val[b] == 0 && py_val[b + 1] == 0;
}

static int py_left(void) {
    int n = 0;
    for (int i = 0; i < PY_PYR_N; i++)
        if (py_val[i]) n++;
    return n;
}

static const char *py_rank(int v) {
    static const char *const names[13] = {
        "A", "2", "3", "4", "5", "6", "7", "8", "9", "10", "J", "Q", "K" };
    return (v >= 1 && v <= 13) ? names[v - 1] : "?";
}

/* 是否存在可执行步: 暴露 K 单除 / 两暴露牌凑 13 / 废牌堆顶配 13 或单除 K */
static bool py_has_move(void) {
    int i, j;
    for (i = 0; i < PY_PYR_N; i++)
        if (py_val[i] && py_exposed(i) && py_val[i] == 13) return true;
    for (i = 0; i < PY_PYR_N; i++)
        if (py_val[i] && py_exposed(i))
            for (j = i + 1; j < PY_PYR_N; j++)
                if (py_val[j] && py_exposed(j) && py_val[i] + py_val[j] == 13)
                    return true;
    if (py_waste_n > 0) {
        int w = py_waste[py_waste_n - 1];
        if (w == 13) return true;
        for (i = 0; i < PY_PYR_N; i++)
            if (py_val[i] && py_exposed(i) && py_val[i] + w == 13) return true;
    }
    return false;
}

/* 每步结算后: 判胜/死局, 清空选择 */
static void py_after_move(void) {
    py_selp = -1;
    py_selw = false;
    if (py_left() == 0) {
        py_over = true;
        py_won = true;
        audio_win();               /* 金字塔全清 */
        led_fx_set(LED_FX_WIN);
        return;
    }
    if (py_stock_i >= PY_STOCK_N && !py_has_move()) {
        py_over = true;
        py_won = false;
        audio_lose();              /* 无步可走 */
        led_fx_set(LED_FX_LOSE);
    }
}

/* 翻发牌堆: 逐张翻到废牌堆; 发完则废牌堆倒序回收回发牌堆 */
static bool py_flip(void) {
    if (py_stock_i < PY_STOCK_N) {
        py_waste[py_waste_n] = py_stock[py_stock_i];
        py_waste_s[py_waste_n] = py_stock_s[py_stock_i];
        py_waste_n++;
        py_stock_i++;
        py_msg = NULL;
        py_after_move();
        return true;
    }
    if (py_waste_n > 0) {
        int n = 0;
        for (int i = py_waste_n - 1; i >= 0; i--) {
            py_stock[n] = py_waste[i];
            py_stock_s[n] = py_waste_s[i];
            n++;
        }
        py_waste_n = 0;
        py_stock_i = 0;
        py_msg = NULL;
        py_after_move();
        return true;
    }
    return false;
}

/* OK 于金字塔牌: 选中/取消/配对结算 */
static void py_ok_py(int idx) {
    if (idx < 0 || idx >= PY_PYR_N) return;
    if (py_val[idx] == 0 || !py_exposed(idx)) return;
    if (py_selw) {                 /* 废牌堆顶 + 金字塔: 凑 13 */
        py_selw = false;
        if (py_waste[py_waste_n - 1] + py_val[idx] == 13) {
            py_waste_n--;
            py_val[idx] = 0;
            py_msg = "MATCH!";
            py_after_move();
            if (!py_over) audio_clear();      /* 配对成功 */
        } else {
            py_msg = "NO MATCH";
            audio_error();                    /* 不配 13 */
        }
        return;
    }
    if (py_selp == idx) {          /* 重复 OK: K 单除 / 取消 */
        if (py_val[idx] == 13) {
            py_val[idx] = 0;
            py_msg = "K DROPPED";
            py_after_move();
            if (!py_over) audio_clear();      /* K 单除 */
        } else {
            py_selp = -1;
        }
        return;
    }
    if (py_selp < 0) {             /* 首次选中 */
        py_selp = idx;
        py_msg = NULL;
        return;
    }
    if (py_val[py_selp] + py_val[idx] == 13) {  /* 两张凑 13 移除 */
        py_val[py_selp] = 0;
        py_val[idx] = 0;
        py_msg = "MATCH!";
        py_after_move();
        if (!py_over) audio_clear();      /* 配对成功 */
    } else {                       /* 不匹配: 双双取消 */
        py_selp = -1;
        py_msg = "NO MATCH";
        audio_error();                    /* 不配 13 */
    }
}

/* OK 于废牌堆顶 */
static void py_ok_waste(void) {
    if (py_waste_n == 0) return;
    if (py_selw) {                 /* 重复 OK: K 单丢 / 取消 */
        if (py_waste[py_waste_n - 1] == 13) {
            py_waste_n--;
            py_msg = "K DROPPED";
            py_after_move();
            if (!py_over) audio_clear();      /* K 单丢 */
        } else {
            py_selw = false;
        }
        return;
    }
    if (py_selp >= 0) {            /* 金字塔 + 废牌堆顶: 凑 13 */
        if (py_val[py_selp] + py_waste[py_waste_n - 1] == 13) {
            py_val[py_selp] = 0;
            py_waste_n--;
            py_selp = -1;
            py_msg = "MATCH!";
            py_after_move();
            if (!py_over) audio_clear();      /* 配对成功 */
        } else {
            py_selp = -1;
            py_msg = "NO MATCH";
            audio_error();                    /* 不配 13 */
        }
        return;
    }
    py_selw = true;                /* 首次选中废牌堆顶 */
    py_msg = NULL;
}

/* 光标移动: 0=上 1=下 2=左 3=右 */
static void py_cursor(int dir) {
    py_msg = NULL;
    if (py_cz == 1) {
        if (dir == 0) {            /* 底部回金字塔底行 */
            py_cz = 0;
            py_cr = PY_ROW_N - 1;
            if (py_cc > py_cr) py_cc = py_cr;
        } else if (dir == 2) {
            if (py_bc > 0) py_bc--;
        } else if (dir == 3) {
            if (py_bc < 1) py_bc++;
        }
        return;
    }
    switch (dir) {
    case 0:
        if (py_cr > 0) { py_cr--; if (py_cc > py_cr) py_cc = py_cr; }
        break;
    case 1:
        if (py_cr < PY_ROW_N - 1) { py_cr++; if (py_cc > py_cr) py_cc = py_cr; }
        else { py_cz = 1; py_bc = 1; }
        break;
    case 2:
        if (py_cc > 0) py_cc--;
        break;
    default:
        if (py_cc < py_cr) py_cc++;
        break;
    }
}

/* 发牌: 洗 52 张, 前 28 张按行序摆金字塔, 余 24 张为发牌堆 */
static void py_deal(void) {
    int deck[52];
    for (int i = 0; i < 52; i++) deck[i] = i;
    for (int i = 51; i > 0; i--) {  /* Fisher-Yates, 有界循环无死锁 */
        uint32_t j = rng_range(&py_rng, (uint32_t)(i + 1));
        int t = deck[i];
        deck[i] = deck[(int)j];
        deck[(int)j] = t;
    }
    for (int i = 0; i < 28; i++) {
        py_val[i] = deck[i] % 13 + 1;
        py_suit[i] = deck[i] / 13;
    }
    for (int i = 0; i < 24; i++) {
        py_stock[i] = deck[28 + i] % 13 + 1;
        py_stock_s[i] = deck[28 + i] / 13;
    }
    py_stock_i = 0;
    py_waste_n = 0;
    py_selp = -1;
    py_selw = false;
    py_cz = 0;
    py_cr = PY_ROW_N - 1;
    py_cc = 0;
    py_bc = 1;
    py_over = false;
    py_won = false;
    py_over_full = false;
    py_msg = NULL;
}

void pyramid_enter(void) {
    py_seed_cnt++;
    rng_seed(&py_rng, now_ms() ^ (py_seed_cnt << 32) ^ 0x1CE5ULL);
    py_deal();
    pyramid_render();
    disp_full();
}

void pyramid_exit(void) {}

void pyramid_tick(uint64_t now) { (void)now; }

/* ---- 渲染 ---- */

static void py_draw_face(int x, int y, int v, int s, bool sel) {
    const char *r = py_rank(v);
    int sx = x + 2 + text_width(r) + 1;
    if (sel) {                     /* 选中: 反白 */
        fb_fill_rect(x, y, PY_CARD_W, PY_CARD_H, true);
        fb_text(x + 2, y + 1, r, false);
        fb_symbol(sx, y + 1, py_sym[s & 3], false);
        fb_stroke_rect(x, y, PY_CARD_W, PY_CARD_H, false);
    } else {
        fb_stroke_rect(x, y, PY_CARD_W, PY_CARD_H, true);
        fb_text(x + 2, y + 1, r, true);
        fb_symbol(sx, y + 1, py_sym[s & 3], true);
    }
}

/* 已移除的空位: 稀疏点阵 */
static void py_draw_slot(int x, int y) {
    fb_fill_tile(x, y, PY_CARD_W, PY_CARD_H, pat_get(PAT_DOT_SPARSE));
}

/* 发牌堆: 最多 5 层叠放, 顶层实心背面 + 白字余量 */
static void py_draw_stock(void) {
    int x = PY_STOCK_X, y = PY_BOT_Y;
    int n = PY_STOCK_N - py_stock_i;
    char buf[12];
    if (n <= 0) {
        fb_stroke_rect(x, y, PY_CARD_W, PY_CARD_H, true);
        snprintf(buf, sizeof(buf), "0");
        fb_text(x + (PY_CARD_W - text_width(buf)) / 2,
                y + (PY_CARD_H - FONT_H) / 2, buf, true);
        return;
    }
    int layers = n > PY_BACK_N ? PY_BACK_N : n;
    for (int i = layers - 1; i >= 0; i--) {
        int bx = x + 2 * i;
        if (i == layers - 1) {
            fb_fill_rect(bx, y, PY_CARD_W, PY_CARD_H, true);
            fb_stroke_rect(bx, y, PY_CARD_W, PY_CARD_H, false);
        } else {
            fb_stroke_rect(bx, y, PY_CARD_W, PY_CARD_H, true);
        }
    }
    snprintf(buf, sizeof(buf), "%d", n);
    int bx = x + 2 * (layers - 1);
    fb_text(bx + (PY_CARD_W - text_width(buf)) / 2,
            y + (PY_CARD_H - FONT_H) / 2, buf, false);
}

static void py_draw_waste(void) {
    int x = PY_WASTE_X, y = PY_BOT_Y;
    if (py_waste_n == 0) {
        fb_stroke_rect(x, y, PY_CARD_W, PY_CARD_H, true);
        return;
    }
    py_draw_face(x, y, py_waste[py_waste_n - 1], py_waste_s[py_waste_n - 1],
                 py_selw);
}

/* 光标: 外圈反色 + 内圈同色(黑/白底均可辨), 在所有牌画完后绘制 */
static void py_draw_cursor(void) {
    int x, y;
    bool sel = false;
    if (py_cz == 0) {
        int idx = py_idx(py_cr, py_cc);
        x = PY_S0 + py_cc * PY_HX - py_cr * PY_HY;
        y = PY_TOP + py_cr * PY_V;
        sel = py_selp == idx;
    } else if (py_bc == 1) {
        x = PY_WASTE_X;
        y = PY_BOT_Y;
        sel = py_selw;
    } else {
        x = PY_STOCK_X;
        y = PY_BOT_Y;
    }
    fb_stroke_rect(x - 2, y - 2, PY_CARD_W + 4, PY_CARD_H + 4, !sel);
    fb_stroke_rect_thick(x - 1, y - 1, PY_CARD_W + 2, PY_CARD_H + 2, 2, sel);
}

void pyramid_render(void) {
    fb_clear(false);
    /* HUD 顶栏: 黑字白底 */
    if (py_over) {
        char b[24];
        fb_fill_rect(0, 0, CCG_W, CCG_HUD_H - 1, false);
        fb_text(2, 1, py_won ? "YOU WIN!" : "NO MOVES", true);
        if (py_won)
            snprintf(b, sizeof(b), "PYRAMID CLEARED");
        else
            snprintf(b, sizeof(b), "LEFT %d CARDS", py_left());
        fb_text(2, 9, b, true);
        fb_text(CCG_W - 2 - text_width("OK/N:RETRY BACK:QUIT"), 1,
                "OK/N:RETRY BACK:QUIT", true);
        fb_hline(0, CCG_HUD_H - 1, CCG_W, true);
        if (!py_over_full) {
            py_over_full = true;
            disp_force_full();
        }
    } else {
        char b[16];
        fb_fill_rect(0, 0, CCG_W, CCG_HUD_H - 1, false);
        fb_text(2, 1, "PYRAMID", true);
        snprintf(b, sizeof(b), "LEFT %d", py_left());
        fb_text(CCG_W - 2 - text_width(b), 1, b, true);
        fb_hline(0, CCG_HUD_H - 1, CCG_W, true);
    }
    /* 金字塔: 自顶向下画, 下层盖住上层下半截 */
    for (int r = 0; r < PY_ROW_N; r++) {
        for (int c = 0; c <= r; c++) {
            int idx = py_idx(r, c);
            int x = PY_S0 + c * PY_HX - r * PY_HY;
            int y = PY_TOP + r * PY_V;
            if (py_val[idx])
                py_draw_face(x, y, py_val[idx], py_suit[idx], py_selp == idx);
            else
                py_draw_slot(x, y);
        }
    }
    /* 底部: 发牌堆 + 废牌堆 + 状态提示 */
    py_draw_stock();
    py_draw_waste();
    const char *s = py_msg;
    if (!s) {
        if (py_selw)
            s = "WASTE PICKED";
        else if (py_selp >= 0)
            s = "PICK 13 PAIR";
        else
            s = "OK:PICK BACK:DEAL";
    }
    fb_text(PY_HINT_X, PY_HINT_Y, s, true);
    /* 光标最后画 */
    if (!py_over) py_draw_cursor();
}

/* ---- 输入 ---- */

static int py_key_dir(const key_event_t *ev) {
    switch (ev->key) {
    case K_UP: return 0;
    case K_DOWN: return 1;
    case K_LEFT: return 2;
    case K_RIGHT: return 3;
    case K_CHAR:
        if (ev->ch == 'w') return 0;
        if (ev->ch == 's') return 1;
        if (ev->ch == 'a') return 2;
        if (ev->ch == 'd') return 3;
        break;
    default: break;
    }
    return -1;
}

static void py_pause(void) {
    pause_sel_t sel;
    if (ui_pause_run(&sel)) {
        if (sel == PAUSE_RESTART) pyramid_enter();
    } else {
        s_exit_request = true;
    }
}

void pyramid_on_key(const key_event_t *ev) {
    if (ev->is_repeat) {           /* 只响应方向键重复 */
        int dir = py_key_dir(ev);
        if (!py_over && dir >= 0) py_cursor(dir);
        return;
    }
    if (py_over) {
        if (ev->key == K_OK || (ev->key == K_CHAR && ev->ch == 'n'))
            pyramid_enter();
        else if (ev->key == K_BACK || ev->key == K_QUIT ||
                 (ev->key == K_CHAR && ev->ch == 'q'))
            s_exit_request = true;
        return;
    }
    int dir = py_key_dir(ev);
    if (dir >= 0) {
        py_cursor(dir);
        return;
    }
    switch (ev->key) {
    case K_OK:
        if (py_cz == 0)
            py_ok_py(py_idx(py_cr, py_cc));
        else if (py_bc == 1)
            py_ok_waste();
        else
            py_flip();             /* OK 于发牌堆 = 翻牌 */
        break;
    case K_BACK:
        py_flip();                 /* 设计: BACK 翻发牌堆, P 暂停 */
        break;
    case K_PAUSE:
        py_pause();
        break;
    case K_QUIT:
        s_exit_request = true;
        break;
    case K_CHAR:
        if (ev->ch == 'n') pyramid_enter();
        else if (ev->ch == 'p') py_pause();
        else if (ev->ch == 'q') s_exit_request = true;
        break;
    default: break;
    }
}
