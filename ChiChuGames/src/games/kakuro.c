/* KAKURO — 算术数独: 每横/竖段填 1-9 不重复, 段内数字和等于提示
 *
 * 布局(296x152): HUD 顶栏 16px; 游戏区 9x7 格 19px 方形(171x133)
 *   水平居中 x=62, y=16(设计稿 20px 格 140 高会溢出 156>152, 取 19px)
 *   黑格=提示格: 右上小字=向下段和(列和), 左下小字=向右段和(行和)
 *   白格=输入格; 数字 2x 放大显示
 * 操作: 方向/WASD 移光标(自动跳过黑格); OK 选中格(输入态, 再按取消);
 *       1-9 填数(冲突允许, 完成时校验); DEL 清除; N 换题; R 重开;
 *       BACK/P 暂停菜单; Q 退出
 * 胜利: 全部空格填满且每段无重复、和等于提示 → HUD 显示 YOU WIN!
 * 实时 HUD 右侧显示冲突段数 "ERR n"(与数织一致)
 *
 * 内置 3 个 ROM 谜题(提示格+空格+答案), 由离线生成器验证:
 *   - 每段数字互不重复且和等于提示(与答案逐位核对)
 *   - 谜题唯一解(回溯求解器计数==1)
 *   - 题面结构合法(每段左侧/上方必有提示格, 首行首列无空格)
 * 单测 tests 会用独立实现的解码+求解器再次交叉验证。
 *
 * 静态前缀 ka_; 像素坐标一律 int; 零 malloc; ASCII 文本。
 * 集成 help[](<=5 行, games_table.c):
 *   "ARROWS/WASD: MOVE  OK: SELECT",
 *   "1-9: FILL  DEL: CLEAR",
 *   "N: NEXT PUZZLE",
 *   "EACH LINE: UNIQUE 1-9, SUM MATCH",
 *   "HINT: TOP=DOWN SUM  BOTTOM=RIGHT SUM", NULL
 */
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

#define KA_COLS 9
#define KA_ROWS 7
#define KA_CELLS 63
#define KA_CELL 19                     /* 20px 格 140 高放不下, 取 19px */
#define KA_OX ((int)((CCG_W - KA_COLS * KA_CELL) / 2))   /* 62 */
#define KA_OY ((int)CCG_HUD_H)         /* 16; 底边 149, 留 3px */
#define KA_ROM_N 3
#define KA_RUN_MAX 28                  /* 3 题均 22 段 */
#define KA_RUN_CELLS 8                 /* 最长段 4 格 */
#define KA_NONE 255

void kakuro_render(void);

/* 谜题数据: grid=63 字符('.', '#', '0'), hints=每提示格 2 字符
 * (阅读顺序: 上=向下段和, 下=向右段和; '-'=无; '0'-'9'=1-9,
 *  'a'-'z'=10-35, 'A'-'Z'=36-45), ans=63 字符答案(测试用) */
typedef struct {
    const char *grid;
    const char *hints;
    const char *ans;
} ka_puz_t;

static const ka_puz_t ka_rom[KA_ROM_N] = {
    {   /* 38 提示格 22 段(h12+v10) 段长 1-3 和 1..21 */
        "#############0.##0###00##00##00#0##0#00#00..##0#000####..#.###0",
        "--------8-------a-------b2--41----99l--7--8dg9---6-a9h---2-k----7----------7",
        ".............2...1...36..43..58.9..6.64.98....2.974...........7",
    },
    {   /* 41 提示格 22 段(h12+v10) 段长 1-4 和 1..23 */
        "##########.####0#0#####00######00##0###000#00##00#0#0.#.##0#00#",
        "------------a---1---------i9-1--------b3--f-------g6--c8----9n-b---g2756-----2-7--",
        "...............9.1.....21......51..8...968.47..97.7.6.....2.52.",
    },
    {   /* 40 提示格 22 段(h12+v10) 段长 1-3 和 1..21 */
        "#############0.##0#.#00##0.#.#.##0#######00#0###0#0#00#.##00.#0",
        "--------4-------7-------83--67---9--96------l8--g-------2--a99-----237-f-----8-1",
        ".............3...7...81..6.......8.......91.9...2.7.96....35..1",
    },
};
static uint8_t ka_cell[KA_CELLS];      /* 当前盘面 0=空 */
static bool ka_input[KA_CELLS];        /* true=空格(可填) */
static uint8_t ka_down[KA_CELLS];      /* 提示格: 向下段和, 0=无 */
static uint8_t ka_right[KA_CELLS];     /* 提示格: 向右段和, 0=无 */
static uint8_t ka_rid[KA_CELLS];       /* 所在横段, KA_NONE=无 */
static uint8_t ka_cid[KA_CELLS];       /* 所在竖段, KA_NONE=无 */
static uint8_t ka_rsum[KA_RUN_MAX];    /* 段目标和 */
static uint8_t ka_rlen[KA_RUN_MAX];    /* 段格数 */
static uint8_t ka_rcell[KA_RUN_MAX][KA_RUN_CELLS];  /* 段内格下标 */
static uint8_t ka_nrun;                /* 段数(横段在前) */
static uint8_t ka_ninput;              /* 空格总数 */
static uint8_t ka_cx, ka_cy;           /* 光标 0-8 / 0-6 */
static bool ka_input_mode;             /* OK 选中格输入态 */
static uint8_t ka_sel;                 /* 输入态目标格 */
static int ka_pz;                      /* 当前谜题 0..2 */
static bool ka_over;                   /* 胜利 */
static bool ka_over_full;              /* 胜利全刷只做一次 */
static rng_t ka_rng;

/* 提示字符解码: '-'=0, '0'-'9'=1-9, 'a'-'z'=10-35, 'A'-'Z'=36-61 */
static uint8_t ka_hint_val(char c) {
    if (c >= '0' && c <= '9') return (uint8_t)(c - '0');
    if (c >= 'a' && c <= 'z') return (uint8_t)(10u + (uint8_t)(c - 'a'));
    if (c >= 'A' && c <= 'Z') return (uint8_t)(36u + (uint8_t)(c - 'A'));
    return 0;
}

/* 载入谜题 pz 并复位(网格+提示解码+段表构建) */
static void ka_load(int pz) {
    int r = pz % KA_ROM_N;
    if (r < 0) r += KA_ROM_N;
    ka_pz = r;
    const char *g = ka_rom[ka_pz].grid;
    const char *h = ka_rom[ka_pz].hints;
    int hi = 0;
    ka_ninput = 0;
    for (int i = 0; i < KA_CELLS; i++) {
        ka_cell[i] = 0;
        ka_down[i] = 0;
        ka_right[i] = 0;
        ka_rid[i] = KA_NONE;
        ka_cid[i] = KA_NONE;
        char c = g[i];
        if (c == '0') {
            ka_input[i] = true;
            ka_ninput++;
        } else {
            ka_input[i] = false;
            if (c == '#') {
                ka_down[i] = ka_hint_val(h[2 * hi]);
                ka_right[i] = ka_hint_val(h[2 * hi + 1]);
                hi++;
            }
        }
    }
    /* 横段(阅读顺序) */
    ka_nrun = 0;
    for (int y = 0; y < KA_ROWS; y++) {
        int x = 0;
        while (x < KA_COLS) {
            if (ka_input[y * KA_COLS + x]) {
                int x0 = x;
                while (x < KA_COLS && ka_input[y * KA_COLS + x]) x++;
                ka_rsum[ka_nrun] = ka_right[y * KA_COLS + x0 - 1];
                ka_rlen[ka_nrun] = 0;
                for (int cx = x0; cx < x; cx++) {
                    uint8_t idx = (uint8_t)(y * KA_COLS + cx);
                    ka_rcell[ka_nrun][ka_rlen[ka_nrun]++] = idx;
                    ka_rid[idx] = ka_nrun;
                }
                ka_nrun++;
            } else {
                x++;
            }
        }
    }
    /* 竖段 */
    for (int x = 0; x < KA_COLS; x++) {
        int y = 0;
        while (y < KA_ROWS) {
            if (ka_input[y * KA_COLS + x]) {
                int y0 = y;
                while (y < KA_ROWS && ka_input[y * KA_COLS + x]) y++;
                ka_rsum[ka_nrun] = ka_down[(y0 - 1) * KA_COLS + x];
                ka_rlen[ka_nrun] = 0;
                for (int cy = y0; cy < y; cy++) {
                    uint8_t idx = (uint8_t)(cy * KA_COLS + x);
                    ka_rcell[ka_nrun][ka_rlen[ka_nrun]++] = idx;
                    ka_cid[idx] = ka_nrun;
                }
                ka_nrun++;
            } else {
                y++;
            }
        }
    }
    /* 光标定位到第一个空格 */
    ka_cx = 0;
    ka_cy = 0;
    while (ka_cy < KA_ROWS && !ka_input[(int)ka_cy * KA_COLS + (int)ka_cx]) {
        if (++ka_cx >= KA_COLS) {
            ka_cx = 0;
            ka_cy++;
        }
    }
    ka_input_mode = false;
    ka_sel = 0;
    ka_over = false;
    ka_over_full = false;
}

/* 段是否冲突(已填满且重复或和不等); 未填满的段不标记 */
static bool ka_run_bad(uint8_t r) {
    uint32_t mask = 0;
    int sum = 0;
    for (int k = 0; k < ka_rlen[r]; k++) {
        uint8_t v = ka_cell[ka_rcell[r][k]];
        if (v == 0) return false;      /* 段未填满, 不判 */
        mask |= 1u << v;
        sum += v;
    }
    int n = 0;                          /* 已填满: 统计不同数字个数 */
    for (int b = 1; b <= 9; b++)
        if ((mask & (1u << b)) != 0) n++;
    if (n != ka_rlen[r]) return true;   /* 段内重复 */
    return sum != ka_rsum[r];           /* 和不等于提示 */
}

/* 冲突段总数(HUD ERR) */
static int ka_err_count(void) {
    int e = 0;
    for (uint8_t r = 0; r < ka_nrun; r++)
        if (ka_run_bad(r)) e++;
    return e;
}

static int ka_fill_count(void) {
    int n = 0;
    for (int i = 0; i < KA_CELLS; i++)
        if (ka_cell[i] != 0) n++;
    return n;
}

/* 胜利: 全部空格填满且无冲突段 */
static bool ka_check_win(void) {
    if (ka_fill_count() != ka_ninput) return false;
    return ka_err_count() == 0;
}

/* 填数: 冲突允许(完成时校验); 填满无冲突即胜 */
static void ka_place(int idx, uint8_t d) {
    if (!ka_input[idx]) return;
    if (ka_cell[idx] == d) return;
    ka_cell[idx] = d;
    if (ka_check_win()) {
        ka_over = true;
        audio_win();
        led_fx_set(LED_FX_WIN);
    } else {
        audio_move();
    }
}

/* 新局/重开: 载入 + 渲染 + 全刷 */
static void ka_start(int pz) {
    ka_load(pz);
    kakuro_render();
    disp_full();
}

/* 光标移动: 自动跳过黑格(沿方向跳到第一个空格, 无则不动) */
static void ka_move(int dx, int dy) {
    int nx = (int)ka_cx + dx;
    int ny = (int)ka_cy + dy;
    while (nx >= 0 && nx < KA_COLS && ny >= 0 && ny < KA_ROWS &&
           !ka_input[ny * KA_COLS + nx]) {
        nx += dx;
        ny += dy;
    }
    if (nx >= 0 && nx < KA_COLS && ny >= 0 && ny < KA_ROWS) {
        ka_cx = (uint8_t)nx;
        ka_cy = (uint8_t)ny;
    }
}

/* 1-2 位数字转小字 */
static void ka_num(uint8_t v, char *buf) {
    if (v >= 10) *buf++ = (char)('0' + v / 10);
    *buf++ = (char)('0' + v % 10);
    *buf = 0;
}

void kakuro_enter(void) {
    rng_seed(&ka_rng, now_ms());
    ka_start((int)rng_range(&ka_rng, (uint32_t)KA_ROM_N));
}

void kakuro_exit(void) {}

void kakuro_tick(uint64_t now) { (void)now; }

void kakuro_render(void) {
    fb_clear(false);

    /* HUD 顶栏: 左标题; 右 "PZ x/y ERR n"; 胜利时左结果右按键提示 */
    if (ka_over) {
        fb_fill_rect(0, 0, CCG_W, CCG_HUD_H - 1, false);
        fb_text(2, 2, "YOU WIN!", true);
        fb_text(CCG_W - text_width("OK/N:RETRY BACK:QUIT") - 2, 2,
                "OK/N:RETRY BACK:QUIT", true);
        if (!ka_over_full) {
            ka_over_full = true;
            disp_force_full();
        }
    } else {
        fb_text(0, 0, "KAKURO", true);
        char rbuf[16];
        char *p = rbuf;
        *p++ = 'P'; *p++ = 'Z'; *p++ = ' ';
        *p++ = (char)('1' + ka_pz);
        *p++ = '/';
        *p++ = (char)('0' + KA_ROM_N);
        *p++ = ' '; *p++ = 'E'; *p++ = 'R'; *p++ = 'R'; *p++ = ' ';
        int e = ka_err_count();
        if (e >= 10) *p++ = (char)('0' + e / 10);
        *p++ = (char)('0' + e % 10);
        *p = 0;
        fb_text(CCG_W - text_width(rbuf) - 2, 0, rbuf, true);
    }
    fb_hline(0, CCG_HUD_H - 1, CCG_W, true);

    /* 格子内容: 黑提示格(小字两行) + 白输入格(2x 数字) */
    for (int y = 0; y < KA_ROWS; y++) {
        for (int x = 0; x < KA_COLS; x++) {
            int idx = y * KA_COLS + x;
            int bx = KA_OX + x * KA_CELL;
            int by = KA_OY + y * KA_CELL;
            if (!ka_input[idx]) {
                fb_fill_rect(bx, by, KA_CELL, KA_CELL, true);
                if (ka_down[idx] != 0) {
                    char d[3];
                    ka_num(ka_down[idx], d);
                    fb_text(bx + KA_CELL - text_width(d) - 1, by + 1, d,
                            false);   /* 右上: 向下段和 */
                }
                if (ka_right[idx] != 0) {
                    char d[3];
                    ka_num(ka_right[idx], d);
                    fb_text(bx + 1, by + KA_CELL - FONT_H - 1, d,
                            false);   /* 左下: 向右段和 */
                }
            } else if (ka_cell[idx] != 0) {
                char d[2];
                d[0] = (char)('0' + ka_cell[idx]);
                d[1] = 0;
                fb_text_scale2(bx + 4, by + 2, d, true);
            }
        }
    }

    /* 网格线: 内线 1px, 外框 2px(外框向外扩 1px 补全四角) */
    for (int i = 0; i <= KA_COLS; i++) {
        int t = (i == 0 || i == KA_COLS) ? 2 : 1;
        fb_fill_rect(KA_OX + i * KA_CELL - (i == KA_COLS ? 1 : 0), KA_OY, t,
                     KA_ROWS * KA_CELL + (i == KA_COLS ? 1 : 0), true);
    }
    for (int i = 0; i <= KA_ROWS; i++) {
        int t = (i == 0 || i == KA_ROWS) ? 2 : 1;
        fb_fill_rect(KA_OX, KA_OY + i * KA_CELL - (i == KA_ROWS ? 1 : 0),
                     KA_COLS * KA_CELL + (i == KA_ROWS ? 1 : 0), t, true);
    }

    /* 光标/选中格(所有格画完后): 反色边框
     * 输入态: 选中格白双框; 光标格粗黑框; 未输入态: 光标格粗黑框 */
    {
        int bx = KA_OX + (int)ka_cx * KA_CELL;
        int by = KA_OY + (int)ka_cy * KA_CELL;
        int cidx = (int)ka_cy * KA_COLS + (int)ka_cx;
        if (ka_input_mode && (int)ka_sel == cidx) {
            fb_stroke_rect(bx, by, KA_CELL, KA_CELL, false);
            fb_stroke_rect(bx + 1, by + 1, KA_CELL - 2, KA_CELL - 2, false);
        } else {
            fb_stroke_rect_thick(bx - 1, by - 1, KA_CELL + 2, KA_CELL + 2, 2,
                                 true);
            if (ka_input_mode) {
                int sx = KA_OX + (int)(ka_sel % KA_COLS) * KA_CELL;
                int sy = KA_OY + (int)(ka_sel / KA_COLS) * KA_CELL;
                fb_stroke_rect(sx, sy, KA_CELL, KA_CELL, false);
                fb_stroke_rect(sx + 1, sy + 1, KA_CELL - 2, KA_CELL - 2,
                               false);
            }
        }
    }
}

void kakuro_on_key(const key_event_t *ev) {
    if (ka_over) {
        if (ev->is_repeat) return;
        if (ev->key == K_OK ||
            (ev->key == K_CHAR && (ev->ch == 'n' || ev->ch == 'r')))
            ka_start(ka_pz);           /* RETRY: 同一题 */
        else if (ev->key == K_BACK || ev->key == K_QUIT)
            s_exit_request = true;
        return;
    }
    switch (ev->key) {
    case K_UP:
        ka_move(0, -1);
        audio_tick();
        break;
    case K_DOWN:
        ka_move(0, 1);
        audio_tick();
        break;
    case K_LEFT:
        ka_move(-1, 0);
        audio_tick();
        break;
    case K_RIGHT:
        ka_move(1, 0);
        audio_tick();
        break;
    case K_OK:
        if (ev->is_repeat) break;
        ka_input_mode = !ka_input_mode;   /* 选中当前格, 再按取消 */
        ka_sel = (uint8_t)(ka_cy * KA_COLS + ka_cx);
        audio_select();
        break;
    case K_DEL:
        if (ev->is_repeat) break;
        {
            int idx = ka_input_mode ? (int)ka_sel
                                    : (int)ka_cy * KA_COLS + (int)ka_cx;
            if (ka_input[idx]) { ka_cell[idx] = 0; audio_move(); }
        }
        break;
    case K_PAUSE:
    case K_BACK: {
        if (ev->is_repeat) break;
        pause_sel_t sel;
        if (ui_pause_run(&sel)) {
            if (sel == PAUSE_RESTART) ka_start(ka_pz);
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
            ka_move(0, -1);
            audio_tick();
        } else if (ev->ch == 'a') {
            ka_move(-1, 0);
            audio_tick();
        } else if (ev->ch == 's') {
            ka_move(0, 1);
            audio_tick();
        } else if (ev->ch == 'd') {
            ka_move(1, 0);
            audio_tick();
        } else if (ev->ch >= '1' && ev->ch <= '9') {
            if (!ev->is_repeat) {
                int idx = ka_input_mode ? (int)ka_sel
                                        : (int)ka_cy * KA_COLS + (int)ka_cx;
                ka_place(idx, (uint8_t)(ev->ch - '0'));
                audio_tick();
            }
        } else if (ev->ch == 'n') {
            if (!ev->is_repeat) { audio_select(); ka_start((ka_pz + 1) % KA_ROM_N); }
        } else if (ev->ch == 'r') {
            if (!ev->is_repeat) { audio_select(); ka_start(ka_pz); }
        }
        break;
    default:
        break;
    }
}
