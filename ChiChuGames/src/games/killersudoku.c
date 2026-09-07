/* KILLER SUDOKU — 杀手数独 9x9: 数独 + 虚线笼 + 笼和
 *
 * 布局(296x152): HUD 顶栏 16px; 游戏区 9x9 格 14px 方形(126x126), 水平居中
 *   x=85, y=16; 右侧状态栏(笼目标和/当前和/已填数); 笼=细虚线边框分组,
 *   笼角(左上角格)显示 1x 小字和值
 * 操作: 方向/WASD 移光标; OK 选中格(输入态, 再按取消);
 *       1-9 填数(冲突允许, 完成时校验); DEL 清除; P 候选点阵;
 *       N 下一题; R 重开; BACK 暂停菜单; Q 退出
 * 校验: 行/列/宫 1-9 不重复 + 每笼不重复且和等于笼和 → WIN;
 *       HUD 右侧实时显示冲突笼数 "ERR n"; 侧栏 NOW 超和/重复时反白
 * 内置 3 个 ROM 谜题(完整解 + 笼划分 + 笼和), 离线求解器验证:
 *   - 解满足行/列/宫 + 笼约束(笼内无重复、和相等)
 *   - 谜题(题面格 + 笼和)唯一解
 *   - 结构合法: 笼覆盖全部 81 格、连通、和值 3..45、总和 405、
 *     题面格不在笼角(笼角显示和值)
 * 单测 tests/batch1 用独立实现解码+求解器交叉验证。
 *
 * 静态前缀 ks_; 像素坐标一律 int; 零 malloc; ASCII 文本。
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
#include <string.h>

#define KS_CELL 14
#define KS_OX ((int)((CCG_W - 9 * KS_CELL) / 2))   /* 85: 水平居中 */
#define KS_OY ((int)CCG_HUD_H)                     /* 16 */
#define KS_N 81
#define KS_ROM_N 3
#define KS_CAGES_MAX 32

void killersudoku_render(void);

/* ROM 谜题: sol=81 位完整解; giv=81 位题面格('.'=空, 数字=锁定);
 * cmap=81 位笼编号('a'-'z' 0-25, 'A'-'Z' 26-51, 阅读顺序连续);
 * sums=每笼和值('1'-'9'=1-9, 'a'-'z'=10-35, 'A'-'J'=36-45) */
typedef struct {
    const char *sol;
    const char *giv;
    const char *cmap;
    const char *sums;
} ks_puz_t;

static const ks_puz_t ks_rom[KS_ROM_N] = {
    {   /* 28 笼 14 个两格笼, 3 个题面格 */
        "495782613716349258823651497147925836369178542582463179631894725958217364274536981",
        ".......................1......................8.............7....................",
        "aabcdddeeafbcdghiiffjcdghklmfjcnghklmmocnnpplmmoqrnsptuvwqrxsptuvwwyxsztAABByyyzt",
        "kbqp4ffedacfnkbicfbjf8bbje99",
    },
    {   /* 24 笼 7 个两格笼, 1 个题面格 */
        "718546392632179854594823716863412975945367128127985643256731489479658231381294567",
        "........................................6........................................",
        "abcdefffgabcdeefhgibjdddhhkibjlllhkkmnjlloppkmnnloopkkqnnrrsptuqqvvrswtuqqvxxswtu",
        "djajkq6mdcxpaokdofdhhg7b",
    },
    {   /* 19 笼(较大笼居多, 最难), 1 个题面格 */
        "573629814681473259492815637247531986859267143316948725765184392124396578938752461",
        "....................................8............................................",
        "aaabbccccdaeeeccffdeeghhfffiijgggkfliijjmkklliinmmkoolppnmmqoorpnnmqqqqrpppssssrr",
        "n8ranCh6nipivhlyvhi",
    },
};

static uint8_t ks_cell[KS_N];       /* 当前盘面 0=空 */
static uint8_t ks_given[KS_N];      /* 1=题面格(锁定) */
static uint8_t ks_cage[KS_N];       /* 每格笼编号 */
static uint8_t ks_csum[KS_CAGES_MAX];    /* 笼目标值 */
static uint8_t ks_clen[KS_CAGES_MAX];    /* 笼格数 */
static uint8_t ks_ccell[KS_CAGES_MAX][9]; /* 笼内格下标 */
static uint8_t ks_corner[KS_CAGES_MAX];  /* 笼角格(左上, 显示和值) */
static uint8_t ks_ncage;
static uint8_t ks_cx, ks_cy;        /* 光标格 0-8 */
static bool ks_input;               /* 输入态(OK 选中格) */
static uint8_t ks_sel;              /* 输入态目标格 0-80 */
static int ks_pz;                   /* 当前谜题 0..2 */
static bool ks_over;                /* 胜利 */
static bool ks_over_full;           /* 胜利全刷只做一次 */
static bool ks_cand;                /* P: 候选点阵开关 */
static rng_t ks_rng;

/* 笼编号/和值字符解码 */
static uint8_t ks_dec_cage(char c) {
    if (c >= 'a' && c <= 'z') return (uint8_t)(c - 'a');
    return (uint8_t)(26u + (uint8_t)(c - 'A'));
}

static uint8_t ks_dec_sum(char c) {
    if (c >= '1' && c <= '9') return (uint8_t)(c - '0');
    if (c >= 'a' && c <= 'z') return (uint8_t)(10u + (uint8_t)(c - 'a'));
    return (uint8_t)(36u + (uint8_t)(c - 'A'));
}

/* 1-3 位数字转小字 */
static void ks_num(uint32_t v, char *buf) {
    char rev[8];
    int n = 0;
    do {
        rev[n++] = (char)('0' + (int)(v % 10));
        v /= 10;
    } while (v != 0 && n < 6);
    while (n > 0) *buf++ = rev[--n];
    *buf = 0;
}

/* 笼是否不可完成/已错: 笼内重复, 或 已满而和不等,
 * 或 未满但最小/最大可能和已越界 */
static bool ks_cage_bad(uint8_t ci) {
    int sum = 0;
    int empty = 0;
    uint32_t mask = 0;
    for (int k = 0; k < ks_clen[ci]; k++) {
        uint8_t v = ks_cell[ks_ccell[ci][k]];
        if (v != 0) {
            sum += v;
            if ((mask & (1u << v)) != 0) return true;   /* 笼内重复 */
            mask |= 1u << v;
        } else {
            empty++;
        }
    }
    if (empty == 0) return sum != (int)ks_csum[ci];
    return (sum + empty > (int)ks_csum[ci]) ||
           (sum + 9 * empty < (int)ks_csum[ci]);
}

/* 冲突笼总数(HUD ERR) */
static int ks_err_count(void) {
    int e = 0;
    for (uint8_t c = 0; c < ks_ncage; c++)
        if (ks_cage_bad(c)) e++;
    return e;
}

static int ks_fill_count(void) {
    int n = 0;
    for (int i = 0; i < KS_N; i++)
        if (ks_cell[i] != 0) n++;
    return n;
}

/* 该格可行候选位掩码(bit0=数字1 .. bit8=数字9):
 * 行/列/宫不重复 + 笼内不重复 + 笼和剩余预算 */
static uint32_t ks_cand_mask(int idx) {
    int y = idx / 9;
    int x = idx % 9;
    uint32_t used = 0;
    for (int i = 0; i < 9; i++) {
        uint8_t a = ks_cell[y * 9 + i];
        uint8_t b = ks_cell[i * 9 + x];
        if (a != 0) used |= 1u << (a - 1);
        if (b != 0) used |= 1u << (b - 1);
    }
    int by = (y / 3) * 3;
    int bx = (x / 3) * 3;
    for (int i = by; i < by + 3; i++)
        for (int j = bx; j < bx + 3; j++) {
            uint8_t v = ks_cell[i * 9 + j];
            if (v != 0) used |= 1u << (v - 1);
        }
    uint8_t ci = ks_cage[idx];
    int s = 0;
    int empty = 0;
    for (int k = 0; k < ks_clen[ci]; k++) {
        uint8_t v = ks_cell[ks_ccell[ci][k]];
        if (v != 0) {
            if (ks_ccell[ci][k] != idx) used |= 1u << (v - 1);
            s += v;
        } else {
            empty++;
        }
    }
    int lo = (int)ks_csum[ci] - s - 9 * (empty - 1);
    if (lo < 1) lo = 1;
    int hi = (int)ks_csum[ci] - s - (empty - 1);
    if (hi > 9) hi = 9;
    if (lo > hi) return 0;
    uint32_t range = ((1u << hi) - 1u) ^ ((1u << (lo - 1)) - 1u);
    return ~used & range & 0x1FFu;
}

/* 胜负判定: 全部填满 + 行/列/宫无重复 + 每笼和相等且无重复 */
static bool ks_check_win(void) {
    for (int i = 0; i < KS_N; i++)
        if (ks_cell[i] == 0) return false;
    for (int y = 0; y < 9; y++) {
        uint32_t m = 0;
        for (int x = 0; x < 9; x++) {
            uint32_t b = 1u << (ks_cell[y * 9 + x] - 1);
            if ((m & b) != 0) return false;
            m |= b;
        }
    }
    for (int x = 0; x < 9; x++) {
        uint32_t m = 0;
        for (int y = 0; y < 9; y++) {
            uint32_t b = 1u << (ks_cell[y * 9 + x] - 1);
            if ((m & b) != 0) return false;
            m |= b;
        }
    }
    for (int by = 0; by < 9; by += 3)
        for (int bx = 0; bx < 9; bx += 3) {
            uint32_t m = 0;
            for (int i = 0; i < 3; i++)
                for (int j = 0; j < 3; j++) {
                    uint32_t b = 1u << (ks_cell[(by + i) * 9 + bx + j] - 1);
                    if ((m & b) != 0) return false;
                    m |= b;
                }
        }
    for (uint8_t ci = 0; ci < ks_ncage; ci++) {
        int sum = 0;
        uint32_t m = 0;
        for (int k = 0; k < ks_clen[ci]; k++) {
            uint8_t v = ks_cell[ks_ccell[ci][k]];
            sum += v;
            if ((m & (1u << v)) != 0) return false;
            m |= 1u << v;
        }
        if (sum != (int)ks_csum[ci]) return false;
    }
    return true;
}

/* 载入谜题 pz(0..KS_ROM_N-1)并复位 */
static void ks_load(int pz) {
    ks_pz = pz;
    const ks_puz_t *p = &ks_rom[ks_pz];
    ks_ncage = (uint8_t)strlen(p->sums);
    for (int i = 0; i < KS_N; i++) {
        ks_cell[i] = 0;
        char g = p->giv[i];
        ks_given[i] = (g != '.') ? 1 : 0;
        if (ks_given[i]) ks_cell[i] = (uint8_t)(g - '0');
        ks_cage[i] = ks_dec_cage(p->cmap[i]);
    }
    for (uint8_t c = 0; c < ks_ncage; c++) {
        ks_csum[c] = ks_dec_sum(p->sums[c]);
        ks_clen[c] = 0;
        ks_corner[c] = 0;
    }
    {
        uint8_t first[64];
        memset(first, 0xFF, sizeof(first));
        for (int i = 0; i < KS_N; i++) {
            uint8_t ci = ks_cage[i];
            if (first[ci] == 0xFF) first[ci] = (uint8_t)i;   /* 阅读顺序首个=笼角 */
            ks_ccell[ci][ks_clen[ci]++] = (uint8_t)i;
        }
        for (uint8_t c = 0; c < ks_ncage; c++)
            ks_corner[c] = first[c];
    }
    ks_cx = 4;
    ks_cy = 4;
    ks_input = false;
    ks_sel = 0;
    ks_over = false;
    ks_over_full = false;
    ks_cand = false;
}

/* 填数: 题面格不可改; 冲突允许(完成时校验); 填满无冲突即胜 */
static void ks_place(int idx, uint8_t d) {
    if (ks_given[idx] != 0) {
        audio_error();
        return;
    }
    if (ks_cell[idx] == d) return;
    ks_cell[idx] = d;
    if (ks_check_win()) {
        ks_over = true;
        audio_win();
        led_fx_set(LED_FX_WIN);
    } else {
        audio_move();
    }
}

/* 新局/重开: 载入 + 渲染 + 全刷 */
static void ks_start_game(int pz) {
    ks_load(pz);
    killersudoku_render();
    disp_full();
}

/* 光标移动 */
static void ks_move(int dx, int dy) {
    int nx = (int)ks_cx + dx;
    int ny = (int)ks_cy + dy;
    if (nx < 0 || nx > 8 || ny < 0 || ny > 8) return;
    ks_cx = (uint8_t)nx;
    ks_cy = (uint8_t)ny;
}

void killersudoku_enter(void) {
    rng_seed(&ks_rng, now_ms());
    ks_start_game((int)rng_range(&ks_rng, (uint32_t)KS_ROM_N));
}

void killersudoku_exit(void) {}

void killersudoku_tick(uint64_t now) { (void)now; }

void killersudoku_render(void) {
    fb_clear(false);

    /* 网格线: 内线 1px, 宫界/外框 2px(外框收进 1px 对称) */
    for (int i = 0; i <= 9; i++) {
        int t = (i == 0 || i == 9 || i % 3 == 0) ? 2 : 1;
        fb_fill_rect(KS_OX + i * KS_CELL - (i == 9 ? 1 : 0), KS_OY, t,
                     9 * KS_CELL, true);
        fb_fill_rect(KS_OX, KS_OY + i * KS_CELL - (i == 9 ? 1 : 0),
                     9 * KS_CELL, t, true);
    }

    /* 笼边框: 相邻异笼共享边画 1px 虚线(3 黑 3 白, 每段从段首起),
     * 空白段画白以盖掉底层网格线 */
    for (int y = 0; y < 9; y++) {
        for (int x = 0; x < 8; x++) {
            int a = y * 9 + x;
            if (ks_cage[a] == ks_cage[a + 1]) continue;
            int vx = KS_OX + (x + 1) * KS_CELL;
            int y0 = KS_OY + y * KS_CELL;
            for (int py = 0; py < KS_CELL; py++)
                fb_pixel(vx, y0 + py, (py % 6) < 3);
        }
    }
    for (int y = 0; y < 8; y++) {
        for (int x = 0; x < 9; x++) {
            int a = y * 9 + x;
            if (ks_cage[a] == ks_cage[a + 9]) continue;
            int hy = KS_OY + (y + 1) * KS_CELL;
            int x0 = KS_OX + x * KS_CELL;
            for (int px = 0; px < KS_CELL; px++)
                fb_pixel(x0 + px, hy, (px % 6) < 3);
        }
    }

    /* 格子内容: 题面=黑底白字 1x; 玩家数字 2x;
     * 笼角格 1x 数字右下角(左上角留给和值) */
    for (int y = 0; y < 9; y++) {
        for (int x = 0; x < 9; x++) {
            int idx = y * 9 + x;
            int bx = KS_OX + x * KS_CELL;
            int by = KS_OY + y * KS_CELL;
            uint8_t v = ks_cell[idx];
            if (ks_given[idx] != 0) {
                char d[2];
                d[0] = (char)('0' + v);
                d[1] = 0;
                fb_fill_rect(bx, by, KS_CELL, KS_CELL, true);
                fb_text(bx + 4, by + 3, d, false);
            } else if (v != 0) {
                char d[2];
                d[0] = (char)('0' + v);
                d[1] = 0;
                if (ks_corner[ks_cage[idx]] == idx)
                    fb_text(bx + 8, by + 7, d, true);
                else
                    fb_text_scale2(bx + 2, by, d, true);
            } else if (ks_cand && ks_corner[ks_cage[idx]] != idx) {
                /* 候选点阵 3x3(笼角格被和值占用, 不显示) */
                uint32_t m = ks_cand_mask(idx);
                for (int k = 0; k < 9; k++) {
                    if ((m & (1u << k)) != 0)
                        fb_fill_rect(bx + 3 + (k % 3) * 4,
                                     by + 3 + (k / 3) * 4, 2, 2, true);
                }
            }
        }
    }

    /* 笼角和值(最后画, 保证可读) */
    for (uint8_t c = 0; c < ks_ncage; c++) {
        int idx = ks_corner[c];
        char buf[4];
        ks_num(ks_csum[c], buf);
        fb_text(KS_OX + (idx % 9) * KS_CELL + 1,
                KS_OY + (idx / 9) * KS_CELL + 1, buf, true);
    }

    /* 右侧状态栏: 光标所在笼 目标/当前和 + 已填数 */
    {
        int sx = KS_OX + 9 * KS_CELL + 3;
        uint8_t ci = ks_cage[(int)ks_cy * 9 + (int)ks_cx];
        char buf[4];
        fb_text(sx, 21, "CAGE", true);
        ks_num(ks_csum[ci], buf);
        fb_text_scale2(sx, 30, buf, true);
        fb_text(sx, 50, "NOW", true);
        int s = 0;
        for (int k = 0; k < ks_clen[ci]; k++)
            s += ks_cell[ks_ccell[ci][k]];
        ks_num((uint32_t)s, buf);
        if (ks_cage_bad(ci)) {
            fb_fill_rect(sx, 59, text_width(buf) * 2, FONT_H * 2, true);
            fb_text_scale2(sx, 59, buf, false);   /* 超和/重复: 反白 */
        } else {
            fb_text_scale2(sx, 59, buf, true);
        }
        fb_text(sx, 79, "FILL", true);
        ks_num((uint32_t)ks_fill_count(), buf);
        fb_text_scale2(sx, 88, buf, true);
    }

    /* HUD 顶栏: 左标题; 右 "PZ 1/3 ERR n" */
    if (ks_over) {
        fb_fill_rect(0, 0, CCG_W, CCG_HUD_H - 1, false);
        fb_text(2, 2, "YOU WIN!", true);
        fb_text(CCG_W - text_width("OK/N:RETRY BACK:QUIT") - 2, 2,
                "OK/N:RETRY BACK:QUIT", true);
        if (!ks_over_full) {
            ks_over_full = true;
            disp_force_full();
        }
    } else {
        fb_text(0, 0, "KILLER", true);
        char rbuf[16];
        char *p = rbuf;
        *p++ = 'P'; *p++ = 'Z'; *p++ = ' ';
        *p++ = (char)('1' + ks_pz);
        *p++ = '/';
        *p++ = (char)('0' + KS_ROM_N);
        *p++ = ' '; *p++ = 'E'; *p++ = 'R'; *p++ = 'R'; *p++ = ' ';
        int e = ks_err_count();
        if (e >= 10) *p++ = (char)('0' + e / 10);
        *p++ = (char)('0' + e % 10);
        *p = 0;
        fb_text(CCG_W - text_width(rbuf) - 2, 0, rbuf, true);
    }
    fb_hline(0, CCG_HUD_H - 1, CCG_W, true);

    /* 光标/选中格(所有内容画完后): 反色边框 */
    {
        int bx = KS_OX + (int)ks_cx * KS_CELL;
        int by = KS_OY + (int)ks_cy * KS_CELL;
        int cidx = (int)ks_cy * 9 + (int)ks_cx;
        if (ks_input && (int)ks_sel == cidx) {
            if (ks_given[cidx] != 0) {
                fb_stroke_rect(bx, by, KS_CELL, KS_CELL, false);
                fb_stroke_rect(bx + 1, by + 1, KS_CELL - 2, KS_CELL - 2,
                               false);
            } else {
                fb_stroke_rect(bx - 1, by - 1, KS_CELL + 2, KS_CELL + 2, true);
                fb_stroke_rect(bx - 2, by - 2, KS_CELL + 4, KS_CELL + 4, true);
            }
        } else {
            fb_stroke_rect_thick(bx - 1, by - 1, KS_CELL + 2, KS_CELL + 2, 2,
                                 (ks_given[cidx] != 0) ? false : true);
            if (ks_input) {
                int sx = KS_OX + (int)(ks_sel % 9) * KS_CELL;
                int sy = KS_OY + (int)(ks_sel / 9) * KS_CELL;
                if (ks_given[ks_sel] != 0) {
                    fb_stroke_rect(sx, sy, KS_CELL, KS_CELL, false);
                    fb_stroke_rect(sx + 1, sy + 1, KS_CELL - 2, KS_CELL - 2,
                                   false);
                } else {
                    fb_stroke_rect(sx - 1, sy - 1, KS_CELL + 2, KS_CELL + 2,
                                   true);
                    fb_stroke_rect(sx - 2, sy - 2, KS_CELL + 4, KS_CELL + 4,
                                   true);
                }
            }
        }
    }
}

void killersudoku_on_key(const key_event_t *ev) {
    if (ks_over) {
        if (ev->is_repeat) return;
        if (ev->key == K_OK ||
            (ev->key == K_CHAR && (ev->ch == 'n' || ev->ch == 'r')))
            ks_start_game(ks_pz);          /* RETRY: 同一题 */
        else if (ev->key == K_BACK || ev->key == K_QUIT)
            s_exit_request = true;
        return;
    }
    switch (ev->key) {
    case K_UP:
        ks_move(0, -1);
        audio_tick();
        break;
    case K_DOWN:
        ks_move(0, 1);
        audio_tick();
        break;
    case K_LEFT:
        ks_move(-1, 0);
        audio_tick();
        break;
    case K_RIGHT:
        ks_move(1, 0);
        audio_tick();
        break;
    case K_OK:
        if (ev->is_repeat) break;
        ks_input = !ks_input;              /* 选中当前格, 再按取消 */
        ks_sel = (uint8_t)(ks_cy * 9 + ks_cx);
        audio_select();
        break;
    case K_DEL:
        if (ev->is_repeat) break;
        {
            int idx = ks_input ? (int)ks_sel : (int)ks_cy * 9 + (int)ks_cx;
            if (ks_given[idx] == 0) { ks_cell[idx] = 0; audio_move(); }
        }
        break;
    case K_PAUSE:                          /* P: 候选点阵开关 */
        if (ev->is_repeat) break;
        ks_cand = !ks_cand;
        audio_select();
        break;
    case K_CHAR:
        if (ev->ch == 'w') {
            ks_move(0, -1);
            audio_tick();
        } else if (ev->ch == 'a') {
            ks_move(-1, 0);
            audio_tick();
        } else if (ev->ch == 's') {
            ks_move(0, 1);
            audio_tick();
        } else if (ev->ch == 'd') {
            ks_move(1, 0);
            audio_tick();
        } else if (ev->ch == 'n') {
            if (!ev->is_repeat) { audio_select(); ks_start_game((ks_pz + 1) % KS_ROM_N); }
        } else if (ev->ch == 'r') {
            if (!ev->is_repeat) { audio_select(); ks_start_game(ks_pz); }
        } else if (ev->ch >= '1' && ev->ch <= '9') {
            if (!ev->is_repeat) {
                int idx = ks_input ? (int)ks_sel
                                   : (int)ks_cy * 9 + (int)ks_cx;
                ks_place(idx, (uint8_t)(ev->ch - '0'));
                audio_tick();
            }
        }
        break;
    case K_BACK: {
        if (ev->is_repeat) break;
        pause_sel_t sel;
        if (ui_pause_run(&sel)) {
            if (sel == PAUSE_RESTART) ks_start_game(ks_pz);
        } else {
            s_exit_request = true;
        }
        break;
    }
    case K_QUIT:
        if (!ev->is_repeat) s_exit_request = true;
        break;
    default:
        break;
    }
}
