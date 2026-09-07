/* SKYSCRAPERS — 4x4 摩天楼: 每行每列楼高 1-4 不重复;
 * 四周数字 = 该方向可见楼数(高者遮矮者)
 *
 * 布局(296x152): HUD 顶栏 16px; 游戏区 4x4 格 26px 方形(104x104)居中
 *   网格 x=96..199, y=32..135; 四周可见数小字:
 *   上排 y=20, 下排 y=140, 左列 x=88, 右列 x=203 (与格行/列对齐)
 * 操作: 方向/WASD 移光标; OK 选中(输入态, 再按取消);
 *       输入态 上下/WS 循环楼高 1-4, 左右 移动选中格;
 *       1-4 直接填数; DEL 清空; N 换题; R 重开; BACK/P 暂停; Q 退出
 * 胜利: 16 格填满且每行每列无重复、四周可见数全对 → HUD YOU WIN!
 * 实时 HUD 右侧 "PZ x/3 ERR n"(n=已满且违例的行列数)
 *
 * 内置 3 个 ROM 谜题(sol=答案 16 字符, clu=四周线索 16 字符
 * '0'=无线索; 顺序 上[4]下[4]左[4]右[4]), 由离线穷举验证唯一解:
 * 对全部 576 个 4 阶拉丁方逐一枚举, 每个谜题恰 1 解, 且答案
 * 满足每条线索; 线索数 8/6/5 递进难度。
 *
 * 静态前缀 sk_; 像素坐标一律 int; 零 malloc; ASCII 文本。
 * help[](<=5 行): 集成见 games_table.c
 */
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

#define SK_N 4
#define SK_CELLS 16
#define SK_CELL 26
#define SK_GRID (SK_N * SK_CELL)         /* 104 */
#define SK_OX ((int)((CCG_W - SK_GRID) / 2))   /* 96 */
#define SK_OY ((int)CCG_HUD_H + 16)      /* 32 */
#define SK_ROM_N 3

void skyscrapers_render(void);

typedef struct {
    const char *sol;   /* 16 字符答案 '1'-'4' */
    const char *clu;   /* 16 字符线索 '0'-'4': 上[4]下[4]左[4]右[4] */
} sk_puz_t;

static const sk_puz_t sk_rom[SK_ROM_N] = {
    { "2134342142131342", "3000201000130322" },   /* 8 线索 */
    { "3124431224311243", "0330021200000300" },   /* 6 线索 */
    { "2143321443211432", "0010010020020043" },   /* 5 线索 */
};

static uint8_t sk_cell[SK_CELLS];        /* 当前盘面 0=空 */
static uint8_t sk_clue[SK_CELLS];        /* 四周线索 0=无 */
static int sk_cx, sk_cy;                 /* 光标 0-3 */
static bool sk_sel;                      /* OK 选中格(输入态) */
static int sk_pz;                        /* 当前谜题 0..2 */
static bool sk_over;                     /* 胜利 */
static bool sk_over_full;                /* 胜利全刷只做一次 */
static rng_t sk_rng;

/* 可见楼数: 从 line 起点方向看(高者遮矮者) */
static int sk_visible(const uint8_t *line) {
    int maxh = 0;
    int n = 0;
    for (int i = 0; i < SK_N; i++) {
        if ((int)line[i] > maxh) {
            maxh = line[i];
            n++;
        }
    }
    return n;
}

/* 线索索引: 上 0-3(列), 下 4-7(列), 左 8-11(行), 右 12-15(行) */
static void sk_load(int pz) {
    int r = pz % SK_ROM_N;
    if (r < 0) r += SK_ROM_N;
    sk_pz = r;
    const char *cl = sk_rom[sk_pz].clu;
    for (int i = 0; i < SK_CELLS; i++)
        sk_clue[i] = (uint8_t)(cl[i] - '0');
    for (int i = 0; i < SK_CELLS; i++)
        sk_cell[i] = 0;
    sk_cx = 0;
    sk_cy = 0;
    sk_sel = false;
    sk_over = false;
    sk_over_full = false;
}

/* 行 r(0-3) 是否已填满且违例(重复或可见数不符) */
static bool sk_row_bad(int r) {
    uint8_t *row = &sk_cell[r * SK_N];
    if (row[0] == 0 || row[1] == 0 || row[2] == 0 || row[3] == 0)
        return false;                        /* 未填满不判 */
    uint32_t mask = 0;
    for (int i = 0; i < SK_N; i++) mask |= 1u << row[i];
    if (mask != 0x1Eu) return true;          /* 1-4 各一次 */
    if (sk_clue[8 + r] != 0 && sk_visible(row) != (int)sk_clue[8 + r])
        return true;
    if (sk_clue[12 + r] != 0) {
        uint8_t rev[SK_N];
        for (int i = 0; i < SK_N; i++) rev[i] = row[SK_N - 1 - i];
        if (sk_visible(rev) != (int)sk_clue[12 + r]) return true;
    }
    return false;
}

/* 列 c(0-3) 是否已填满且违例 */
static bool sk_col_bad(int c) {
    if (sk_cell[c] == 0 || sk_cell[SK_N + c] == 0 ||
        sk_cell[2 * SK_N + c] == 0 || sk_cell[3 * SK_N + c] == 0)
        return false;
    uint32_t mask = 0;
    for (int r = 0; r < SK_N; r++) mask |= 1u << sk_cell[r * SK_N + c];
    if (mask != 0x1Eu) return true;
    uint8_t col[SK_N];
    for (int r = 0; r < SK_N; r++) col[r] = sk_cell[r * SK_N + c];
    if (sk_clue[c] != 0 && sk_visible(col) != (int)sk_clue[c])
        return true;
    if (sk_clue[4 + c] != 0) {
        uint8_t rev[SK_N];
        for (int r = 0; r < SK_N; r++) rev[r] = col[SK_N - 1 - r];
        if (sk_visible(rev) != (int)sk_clue[4 + c]) return true;
    }
    return false;
}

/* 违例行/列总数(HUD ERR) */
static int sk_err_count(void) {
    int e = 0;
    for (int r = 0; r < SK_N; r++)
        if (sk_row_bad(r)) e++;
    for (int c = 0; c < SK_N; c++)
        if (sk_col_bad(c)) e++;
    return e;
}

static bool sk_filled(void) {
    for (int i = 0; i < SK_CELLS; i++)
        if (sk_cell[i] == 0) return false;
    return true;
}

/* 胜利: 全填满且无违例行/列 */
static bool sk_check_win(void) {
    if (!sk_filled()) return false;
    return sk_err_count() == 0;
}

static void sk_place(int idx, uint8_t d) {
    if (idx < 0 || idx >= SK_CELLS) return;
    if (sk_cell[idx] == d) return;
    sk_cell[idx] = d;
    if (sk_check_win()) {
        sk_over = true;
        audio_win();
        led_fx_set(LED_FX_WIN);
    } else {
        audio_move();
    }
}

/* 输入态: 上下循环楼高 1-4(不去 0); 清空用 DEL */
static void sk_cycle(int step) {
    int idx = sk_cy * SK_N + sk_cx;
    uint8_t v = sk_cell[idx];
    int b;                                   /* 映射 1-4 -> 0-3 */
    if (v == 0) b = (step > 0) ? -1 : 4;     /* 空格: 上=1 下=4 */
    else b = (int)v - 1;
    b = (b + step + 4) % 4;                  /* 负模安全 */
    sk_place(idx, (uint8_t)(b + 1));
}

/* 新局/重开: 载入 + 渲染 + 全刷 */
static void sk_start(int pz) {
    sk_load(pz);
    skyscrapers_render();
    disp_full();
}

/* 光标移动(输入态下左右: 移动选中格) */
static void sk_move(int dx, int dy) {
    int nx = sk_cx + dx;
    int ny = sk_cy + dy;
    if (nx < 0) nx = SK_N - 1;
    if (nx >= SK_N) nx = 0;
    if (ny < 0) ny = SK_N - 1;
    if (ny >= SK_N) ny = 0;
    sk_cx = nx;
    sk_cy = ny;
}

void skyscrapers_enter(void) {
    rng_seed(&sk_rng, now_ms());
    sk_start((int)rng_range(&sk_rng, (uint32_t)SK_ROM_N));
}

void skyscrapers_exit(void) {}

void skyscrapers_tick(uint64_t now) { (void)now; }

/* 数字 '0'-'4' 小字(0=空白) */
static void sk_clue_digit(int x, int y, uint8_t v) {
    if (v == 0) return;
    char d[2];
    d[0] = (char)('0' + v);
    d[1] = 0;
    fb_text(x, y, d, true);
}

void skyscrapers_render(void) {
    fb_clear(false);

    /* HUD 顶栏: 左标题; 右 "PZ x/3 ERR n"; 胜利时左结果右按键提示 */
    if (sk_over) {
        fb_fill_rect(0, 0, CCG_W, CCG_HUD_H - 1, false);
        fb_text(2, 2, "YOU WIN!", true);
        fb_text(CCG_W - text_width("OK/N:RETRY BACK:QUIT") - 2, 2,
                "OK/N:RETRY BACK:QUIT", true);
        if (!sk_over_full) {
            sk_over_full = true;
            disp_force_full();
        }
    } else {
        fb_text(2, 2, "SKYSCRP", true);
        char rbuf[16];
        char *p = rbuf;
        *p++ = 'P'; *p++ = 'Z'; *p++ = ' ';
        *p++ = (char)('1' + sk_pz);
        *p++ = '/';
        *p++ = (char)('0' + SK_ROM_N);
        *p++ = ' '; *p++ = 'E'; *p++ = 'R'; *p++ = 'R'; *p++ = ' ';
        int e = sk_err_count();
        if (e >= 10) *p++ = (char)('0' + e / 10);
        *p++ = (char)('0' + e % 10);
        *p = 0;
        fb_text(CCG_W - text_width(rbuf) - 2, 2, rbuf, true);
    }
    fb_hline(0, CCG_HUD_H - 1, CCG_W, true);

    /* 网格线: 内线 1px, 外框 2px(外框向外扩 1px 补全四角) */
    for (int i = 0; i <= SK_N; i++) {
        int t = (i == 0 || i == SK_N) ? 2 : 1;
        fb_fill_rect(SK_OX + i * SK_CELL - (i == SK_N ? 1 : 0), SK_OY, t,
                     SK_GRID + (i == SK_N ? 1 : 0), true);
    }
    for (int i = 0; i <= SK_N; i++) {
        int t = (i == 0 || i == SK_N) ? 2 : 1;
        fb_fill_rect(SK_OX, SK_OY + i * SK_CELL - (i == SK_N ? 1 : 0),
                     SK_GRID + (i == SK_N ? 1 : 0), t, true);
    }

    /* 格子内容: 2x 放大数字 */
    for (int y = 0; y < SK_N; y++) {
        for (int x = 0; x < SK_N; x++) {
            uint8_t v = sk_cell[y * SK_N + x];
            if (v == 0) continue;
            char d[2];
            d[0] = (char)('0' + v);
            d[1] = 0;
            fb_text_scale2(SK_OX + x * SK_CELL + 8, SK_OY + y * SK_CELL + 6,
                           d, true);
        }
    }

    /* 四周线索(小字, 与格行/列中心对齐) */
    for (int i = 0; i < SK_N; i++) {
        int gx = SK_OX + i * SK_CELL + 10;          /* 列线索 x */
        int gy = SK_OY + i * SK_CELL + 10;          /* 行线索 y */
        sk_clue_digit(gx, SK_OY - 12, sk_clue[i]);          /* 上 */
        sk_clue_digit(gx, SK_OY + SK_GRID + 4, sk_clue[4 + i]); /* 下 */
        sk_clue_digit(SK_OX - 8, gy, sk_clue[8 + i]);        /* 左 */
        sk_clue_digit(SK_OX + SK_GRID + 3, gy, sk_clue[12 + i]); /* 右 */
    }

    /* 光标/选中格(所有格子绘制完之后):
     * 输入态: 反白格(黑底白字); 光标: 2px 粗黑框向外扩 1px */
    if (sk_sel) {
        int bx = SK_OX + sk_cx * SK_CELL;
        int by = SK_OY + sk_cy * SK_CELL;
        fb_fill_rect(bx, by, SK_CELL, SK_CELL, true);
        if (sk_cell[sk_cy * SK_N + sk_cx] != 0) {
            char d[2];
            d[0] = (char)('0' + sk_cell[sk_cy * SK_N + sk_cx]);
            d[1] = 0;
            fb_text_scale2(bx + 8, by + 6, d, false);
        }
    }
    {
        int bx = SK_OX + sk_cx * SK_CELL;
        int by = SK_OY + sk_cy * SK_CELL;
        fb_stroke_rect_thick(bx - 1, by - 1, SK_CELL + 2, SK_CELL + 2, 2,
                             true);
    }

    disp_fast();
}

void skyscrapers_on_key(const key_event_t *ev) {
    if (sk_over) {
        if (ev->is_repeat) return;
        if (ev->key == K_OK ||
            (ev->key == K_CHAR && (ev->ch == 'n' || ev->ch == 'r')))
            sk_start(sk_pz);           /* RETRY: 同一题 */
        else if (ev->key == K_BACK || ev->key == K_QUIT)
            s_exit_request = true;
        return;
    }
    switch (ev->key) {
    case K_UP:
        if (sk_sel) {
            if (!ev->is_repeat) { sk_cycle(1); audio_tick(); }
        } else {
            sk_move(0, -1);
            audio_tick();
        }
        break;
    case K_DOWN:
        if (sk_sel) {
            if (!ev->is_repeat) { sk_cycle(-1); audio_tick(); }
        } else {
            sk_move(0, 1);
            audio_tick();
        }
        break;
    case K_LEFT:
        sk_move(-1, 0);
        audio_tick();
        break;
    case K_RIGHT:
        sk_move(1, 0);
        audio_tick();
        break;
    case K_OK:
        if (ev->is_repeat) break;
        sk_sel = !sk_sel;              /* 选中当前格, 再按取消 */
        audio_select();
        break;
    case K_DEL:
        if (ev->is_repeat) break;
        sk_place(sk_cy * SK_N + sk_cx, 0);
        audio_move();
        break;
    case K_PAUSE:
    case K_BACK: {
        if (ev->is_repeat) break;
        pause_sel_t sel;
        if (ui_pause_run(&sel)) {
            if (sel == PAUSE_RESTART) sk_start(sk_pz);
        } else {
            s_exit_request = true;
        }
        break;
    }
    case K_QUIT:
        if (!ev->is_repeat) s_exit_request = true;
        break;
    case K_CHAR:
        if (ev->ch == 'w') {
            if (sk_sel) {
                if (!ev->is_repeat) { sk_cycle(1); audio_tick(); }
            } else {
                sk_move(0, -1);
                audio_tick();
            }
        } else if (ev->ch == 's') {
            if (sk_sel) {
                if (!ev->is_repeat) { sk_cycle(-1); audio_tick(); }
            } else {
                sk_move(0, 1);
                audio_tick();
            }
        } else if (ev->ch == 'a') {
            sk_move(-1, 0);
            audio_tick();
        } else if (ev->ch == 'd') {
            sk_move(1, 0);
            audio_tick();
        } else if (ev->ch >= '1' && ev->ch <= '4') {
            if (!ev->is_repeat) {
                sk_place(sk_cy * SK_N + sk_cx, (uint8_t)(ev->ch - '0'));
                audio_tick();
            }
        } else if (ev->ch == 'n') {
            if (!ev->is_repeat) { audio_select(); sk_start((sk_pz + 1) % SK_ROM_N); }
        } else if (ev->ch == 'r') {
            if (!ev->is_repeat) { audio_select(); sk_start(sk_pz); }
        }
        break;
    default:
        break;
    }
}
