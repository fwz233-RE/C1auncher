/* KENKEN — 4x4 算术数独: 行/列 1-4 不重复, 每笼数字按运算符得到目标值
 *
 * 版面(296x152): HUD 顶栏 16px; 游戏区 4x4 格 32px(128x128) + 1px 外框
 *   = 130x130 居中: x=83..213, y=18..148; 笼=粗边框(3px)分组,
 *   笼角(阅读顺序首格)显示算术线索, 如 "3+","12x","4-","2/"
 * 操作: 方向/WASD 移光标; OK 选中(输入态, 再按取消); 1-4 填数;
 *       DEL 清除(单格笼=题面格不可改); N 下一题; R 重开;
 *       P/BACK 暂停; Q 退出
 * 校验: 行/列 1-4 不重复 + 每笼数字经运算符(2 格笼 + - x /,
 *       3/4 格笼 + - x)得目标 → WIN; HUD 右侧实时显示错误数
 *       ERR(行/列重复 + 笼超和/乘积越界/已满而错)
 * 内置 3 个 ROM 谜题(笼划分 + 运算符 + 目标), 离线枚举 576 个
 *   Latin 方验证: 每题恰好 1 个解, 笼连通且覆盖全部 16 格
 * 静态前缀 kn_; 像素坐标一律 int; 零 malloc; ASCII 文本
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

#define KN_CELL 32
#define KN_BOARD (4 * KN_CELL + 2)                 /* 130: 格 + 两侧外框 */
#define KN_OX ((int)((CCG_W - KN_BOARD) / 2))      /* 83 */
#define KN_OY ((int)(CCG_HUD_H + (CCG_H - CCG_HUD_H - KN_BOARD) / 2)) /* 19 */
#define KN_N 16
#define KN_ROM_N 3
#define KN_CAGES_MAX 9

void kenken_render(void);

/* ROM 谜题: cmap=16 格笼编号('a' 起, 阅读顺序, 连通笼);
 * ops=每笼运算符(+ - x /, 单格笼为 '+');
 * tgts=每笼目标: '1'-'9'=1-9, 'a'-'p'=10-25 */
typedef struct {
    const char *cmap;
    const char *ops;
    const char *tgts;
} kn_puz_t;

static const kn_puz_t kn_rom[KN_ROM_N] = {
    {   /* 易: 行对 + 列条, 3 个单格给定 */
        "aabbcdefcgghciih",
        "++++++++x",
        "379412546",
    },
    {   /* 中: 整行笼 10+ + 列条, 3 个单格给定 */
        "aaaabcdebfgebffe",
        "+++++++",
        "a934671",
    },
    {   /* 难: L 形 4 格笼 + 除/减, 1 个单格给定 */
        "aabbccbdefbdeggg",
        "-+/-++x",
        "2933728",
    },
};

static uint8_t kn_cell[KN_N];                /* 当前盘面 0=空 */
static uint8_t kn_given[KN_N];               /* 1=题面格(单格笼, 锁定) */
static uint8_t kn_cage[KN_N];                /* 每格笼编号 */
static uint8_t kn_clen[KN_CAGES_MAX];        /* 笼格数 */
static uint8_t kn_ccell[KN_CAGES_MAX][4];    /* 笼内格下标 */
static uint8_t kn_corner[KN_CAGES_MAX];      /* 笼角格(左上, 显示线索) */
static char kn_op[KN_CAGES_MAX];             /* 笼运算符 */
static uint8_t kn_tgt[KN_CAGES_MAX];         /* 笼目标值 */
static uint8_t kn_ncage;
static uint8_t kn_cx, kn_cy;                 /* 光标格 0-3 */
static bool kn_input;                        /* 输入态(OK 选中格) */
static uint8_t kn_sel;                       /* 输入态目标格 0-15 */
static int kn_pz;                            /* 当前谜题 0..2 */
static bool kn_over;                         /* 胜利 */
static bool kn_over_full;                    /* 胜利全刷只做一次 */
static rng_t kn_rng;

/* 目标字符解码: '1'-'9'=1-9, 'a'-'p'=10-25 */
static uint8_t kn_dec_tgt(char c) {
    if (c >= '1' && c <= '9') return (uint8_t)(c - '0');
    return (uint8_t)(10u + (uint8_t)(c - 'a'));
}

/* 载入谜题 pz 并复位 */
static void kn_load(int pz) {
    kn_pz = pz;
    const kn_puz_t *p = &kn_rom[kn_pz];
    kn_ncage = (uint8_t)strlen(p->ops);
    for (int i = 0; i < KN_N; i++) {
        kn_cell[i] = 0;
        kn_given[i] = 0;
        kn_cage[i] = (uint8_t)(p->cmap[i] - 'a');
    }
    for (uint8_t c = 0; c < kn_ncage; c++) {
        kn_op[c] = p->ops[c];
        kn_tgt[c] = kn_dec_tgt(p->tgts[c]);
        kn_clen[c] = 0;
        kn_corner[c] = 0;
    }
    {
        uint8_t first[KN_CAGES_MAX];
        memset(first, 0xFF, sizeof(first));
        for (int i = 0; i < KN_N; i++) {
            uint8_t ci = kn_cage[i];
            if (first[ci] == 0xFF) first[ci] = (uint8_t)i;
            kn_ccell[ci][kn_clen[ci]++] = (uint8_t)i;
        }
        for (uint8_t c = 0; c < kn_ncage; c++) {
            kn_corner[c] = first[c];
            if (kn_clen[c] == 1) {
                /* 单格笼 = 题面格: 预填其值并锁定 */
                uint8_t cell = kn_ccell[c][0];
                kn_given[cell] = 1;
                kn_cell[cell] = kn_tgt[c];
            }
        }
    }
    kn_cx = 1;
    kn_cy = 1;
    kn_input = false;
    kn_sel = 0;
    kn_over = false;
    kn_over_full = false;
}

/* 笼内数字(全部填满)按运算符求值是否等于目标 */
static bool kn_cage_ok(uint8_t ci) {
    int t = (int)kn_tgt[ci];
    uint8_t n = kn_clen[ci];
    int v[4];
    for (int k = 0; k < (int)n; k++)
        v[k] = (int)kn_cell[kn_ccell[ci][k]];
    switch (kn_op[ci]) {
    case '+': {
        int s = 0;
        for (int k = 0; k < (int)n; k++) s += v[k];
        return s == t;
    }
    case 'x': {
        int p = 1;
        for (int k = 0; k < (int)n; k++) p *= v[k];
        return p == t;
    }
    case '-': {                     /* 最大 - 其余之和 */
        int mx = 0, s = 0;
        for (int k = 0; k < (int)n; k++) {
            if (v[k] > mx) mx = v[k];
            s += v[k];
        }
        return mx - s + mx == t;
    }
    case '/': {                     /* 仅 2 格: 大/小 */
        int a = v[0], b = v[1];
        int big = (a > b) ? a : b;
        int sm = (a > b) ? b : a;
        return sm != 0 && big % sm == 0 && big / sm == t;
    }
    }
    return false;
}

/* 笼已错/已不可能(实时 ERR 用): 填满而错, 或 + / x 部分填已越界 */
static bool kn_cage_bad(uint8_t ci) {
    int t = (int)kn_tgt[ci];
    uint8_t n = kn_clen[ci];
    int v[4];
    int empty = 0;
    for (int k = 0; k < (int)n; k++) {
        v[k] = (int)kn_cell[kn_ccell[ci][k]];
        if (v[k] == 0) empty++;
    }
    if (empty == 0) return !kn_cage_ok(ci);
    switch (kn_op[ci]) {
    case '+': {
        int s = 0;
        for (int k = 0; k < (int)n; k++) s += v[k];
        return s + empty > t || s + 4 * empty < t;
    }
    case 'x': {
        int p = 1;
        for (int k = 0; k < (int)n; k++)
            if (v[k] != 0) p *= v[k];
        if (p > t || t % p != 0) return true;
        for (int k = 0; k < empty; k++) p *= 4;
        return p < t;
    }
    default:
        return false;               /* - / 部分填不做可行性判断 */
    }
}

/* 行/列重复错误数 */
static int kn_dup_err(void) {
    int e = 0;
    for (int y = 0; y < 4; y++) {
        uint32_t m = 0;
        for (int x = 0; x < 4; x++) {
            uint8_t v = kn_cell[y * 4 + x];
            if (v != 0) {
                if ((m & (1u << (v - 1))) != 0) { e++; break; }
                m |= 1u << (v - 1);
            }
        }
    }
    for (int x = 0; x < 4; x++) {
        uint32_t m = 0;
        for (int y = 0; y < 4; y++) {
            uint8_t v = kn_cell[y * 4 + x];
            if (v != 0) {
                if ((m & (1u << (v - 1))) != 0) { e++; break; }
                m |= 1u << (v - 1);
            }
        }
    }
    return e;
}

static int kn_cage_err(void) {
    int e = 0;
    for (uint8_t c = 0; c < kn_ncage; c++)
        if (kn_cage_bad(c)) e++;
    return e;
}

/* 胜负判定: 填满 + 行/列不重复 + 每笼算术满足 */
static bool kn_check_win(void) {
    for (int i = 0; i < KN_N; i++)
        if (kn_cell[i] == 0) return false;
    for (int y = 0; y < 4; y++) {
        uint32_t m = 0;
        for (int x = 0; x < 4; x++) {
            uint32_t b = 1u << (kn_cell[y * 4 + x] - 1);
            if ((m & b) != 0) return false;
            m |= b;
        }
    }
    for (int x = 0; x < 4; x++) {
        uint32_t m = 0;
        for (int y = 0; y < 4; y++) {
            uint32_t b = 1u << (kn_cell[y * 4 + x] - 1);
            if ((m & b) != 0) return false;
            m |= b;
        }
    }
    for (uint8_t ci = 0; ci < kn_ncage; ci++)
        if (!kn_cage_ok(ci)) return false;
    return true;
}

/* 填数: 题面格不可改; 冲突允许(完成时校验); 填满无冲突即胜 */
static void kn_place(int idx, uint8_t d) {
    if (idx < 0 || idx >= KN_N) return;
    if (kn_given[idx] != 0) {
        audio_error();
        return;
    }
    if (kn_cell[idx] == d) return;
    kn_cell[idx] = d;
    if (kn_check_win()) {
        kn_over = true;
        audio_win();
        led_fx_set(LED_FX_WIN);
    } else {
        audio_move();
    }
}

/* 新局/重开: 载入 + 渲染 + 全刷 */
static void kn_start_game(int pz) {
    kn_load(pz);
    kenken_render();
    disp_full();
}

/* 光标移动 */
static void kn_move(int dx, int dy) {
    int nx = (int)kn_cx + dx;
    int ny = (int)kn_cy + dy;
    if (nx < 0 || nx > 3 || ny < 0 || ny > 3) return;
    kn_cx = (uint8_t)nx;
    kn_cy = (uint8_t)ny;
}

/* 笼线索文本: 目标数字 + 运算符(单格笼无运算符) */
static void kn_clue_str(uint8_t ci, char *buf) {
    int t = (int)kn_tgt[ci];
    int n = 0;
    char rev[4];
    do {
        rev[n++] = (char)('0' + t % 10);
        t /= 10;
    } while (t != 0 && n < 3);
    while (n > 0) *buf++ = rev[--n];
    if (kn_clen[ci] > 1) *buf++ = kn_op[ci];
    *buf = 0;
}

void kenken_enter(void) {
    rng_seed(&kn_rng, now_ms());
    kn_start_game((int)rng_range(&kn_rng, (uint32_t)KN_ROM_N));
}

void kenken_exit(void) {}

void kenken_tick(uint64_t now) { (void)now; }

void kenken_render(void) {
    fb_clear(false);

    /* 内网格线 1px */
    for (int i = 0; i <= 4; i++) {
        fb_vline(KN_OX + i * KN_CELL, KN_OY, 4 * KN_CELL, true);
        fb_hline(KN_OX, KN_OY + i * KN_CELL, 4 * KN_CELL, true);
    }
    /* 外框 3px */
    fb_fill_rect(KN_OX - 1, KN_OY - 1, 4 * KN_CELL + 2, 3, true);
    fb_fill_rect(KN_OX - 1, KN_OY + 4 * KN_CELL - 2, 4 * KN_CELL + 2, 3, true);
    fb_fill_rect(KN_OX - 1, KN_OY - 1, 3, 4 * KN_CELL + 2, true);
    fb_fill_rect(KN_OX + 4 * KN_CELL - 2, KN_OY - 1, 3, 4 * KN_CELL + 2, true);
    /* 笼边界 3px(逐段: 相邻异笼的共享边) */
    for (int y = 0; y < 4; y++)
        for (int x = 1; x < 4; x++)
            if (kn_cage[y * 4 + x - 1] != kn_cage[y * 4 + x])
                fb_fill_rect(KN_OX + x * KN_CELL - 1, KN_OY + y * KN_CELL, 3,
                             KN_CELL, true);
    for (int x = 0; x < 4; x++)
        for (int y = 1; y < 4; y++)
            if (kn_cage[(y - 1) * 4 + x] != kn_cage[y * 4 + x])
                fb_fill_rect(KN_OX + x * KN_CELL, KN_OY + y * KN_CELL - 1,
                             KN_CELL, 3, true);

    /* 格子内容: 题面=黑底白字 2x; 玩家数字 2x(笼角格下移避开线索) */
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            int idx = y * 4 + x;
            int bx = KN_OX + x * KN_CELL;
            int by = KN_OY + y * KN_CELL;
            uint8_t v = kn_cell[idx];
            if (kn_given[idx] != 0) {
                char d[2];
                d[0] = (char)('0' + v);
                d[1] = 0;
                fb_fill_rect(bx + 1, by + 1, KN_CELL - 2, KN_CELL - 2, true);
                fb_text_scale2(bx + 11, by + 9, d, false);
            } else if (v != 0) {
                char d[2];
                d[0] = (char)('0' + v);
                d[1] = 0;
                if ((int)kn_corner[kn_cage[idx]] == idx)
                    fb_text_scale2(bx + 11, by + 16, d, true);
                else
                    fb_text_scale2(bx + 11, by + 9, d, true);
            }
        }
    }

    /* 笼角线索(最后画, 保证可读; 单格笼不画, 其值为题面) */
    for (uint8_t ci = 0; ci < kn_ncage; ci++) {
        if (kn_clen[ci] == 1) continue;
        int idx = (int)kn_corner[ci];
        char buf[4];
        kn_clue_str(ci, buf);
        fb_text(KN_OX + (idx % 4) * KN_CELL + 2,
                KN_OY + (idx / 4) * KN_CELL + 1, buf, true);
    }

    /* HUD 顶栏: 左标题; 右 "PZ 1/3 ERR n" */
    if (kn_over) {
        fb_fill_rect(0, 0, CCG_W, CCG_HUD_H - 1, false);
        fb_text(2, 2, "YOU WIN!", true);
        fb_text(CCG_W - text_width("OK/N:RETRY BACK:QUIT") - 2, 2,
                "OK/N:RETRY BACK:QUIT", true);
        if (!kn_over_full) {
            kn_over_full = true;
            disp_force_full();
        }
    } else {
        fb_text(2, 2, "KENKEN", true);
        char rbuf[20];
        int r = 0;
        rbuf[r++] = 'P';
        rbuf[r++] = 'Z';
        rbuf[r++] = ' ';
        rbuf[r++] = (char)('1' + kn_pz);
        rbuf[r++] = '/';
        rbuf[r++] = (char)('0' + KN_ROM_N);
        rbuf[r++] = ' ';
        rbuf[r++] = 'E';
        rbuf[r++] = 'R';
        rbuf[r++] = 'R';
        rbuf[r++] = ' ';
        int e = kn_dup_err() + kn_cage_err();
        if (e >= 10) rbuf[r++] = (char)('0' + e / 10);
        rbuf[r++] = (char)('0' + e % 10);
        rbuf[r] = 0;
        fb_text(CCG_W - text_width(rbuf) - 2, 2, rbuf, true);
    }
    fb_hline(0, CCG_HUD_H - 1, CCG_W, true);

    /* 光标/选中格(所有内容画完后): 反色边框 */
    {
        int bx = KN_OX + (int)kn_cx * KN_CELL;
        int by = KN_OY + (int)kn_cy * KN_CELL;
        int cidx = (int)kn_cy * 4 + (int)kn_cx;
        if (kn_given[cidx] != 0) {
            fb_stroke_rect(bx, by, KN_CELL, KN_CELL, false);
            fb_stroke_rect(bx + 1, by + 1, KN_CELL - 2, KN_CELL - 2, false);
            fb_stroke_rect_thick(bx - 1, by - 1, KN_CELL + 2, KN_CELL + 2, 2,
                                 true);
        } else {
            fb_stroke_rect_thick(bx - 2, by - 2, KN_CELL + 4, KN_CELL + 4, 2,
                                 true);
        }
        if (kn_input) {
            int sx = KN_OX + (int)(kn_sel % 4) * KN_CELL;
            int sy = KN_OY + (int)(kn_sel / 4) * KN_CELL;
            if (kn_given[kn_sel] != 0) {
                fb_stroke_rect(sx, sy, KN_CELL, KN_CELL, false);
                fb_stroke_rect(sx + 1, sy + 1, KN_CELL - 2, KN_CELL - 2,
                               false);
            } else {
                fb_stroke_rect(sx - 1, sy - 1, KN_CELL + 2, KN_CELL + 2, true);
                fb_stroke_rect(sx - 2, sy - 2, KN_CELL + 4, KN_CELL + 4, true);
            }
        }
    }
}

void kenken_on_key(const key_event_t *ev) {
    if (kn_over) {
        if (ev->is_repeat) return;
        if (ev->key == K_OK ||
            (ev->key == K_CHAR && (ev->ch == 'n' || ev->ch == 'r')))
            kn_start_game(kn_pz);          /* RETRY: 同一题 */
        else if (ev->key == K_BACK || ev->key == K_QUIT)
            s_exit_request = true;
        return;
    }
    switch (ev->key) {
    case K_UP:
        kn_move(0, -1);
        break;
    case K_DOWN:
        kn_move(0, 1);
        break;
    case K_LEFT:
        kn_move(-1, 0);
        break;
    case K_RIGHT:
        kn_move(1, 0);
        break;
    case K_OK:
        if (ev->is_repeat) break;
        kn_input = !kn_input;              /* 选中当前格, 再按取消 */
        kn_sel = (uint8_t)(kn_cy * 4 + kn_cx);
        audio_select();
        break;
    case K_DEL:
        if (ev->is_repeat) break;
        {
            int idx = kn_input ? (int)kn_sel : (int)kn_cy * 4 + (int)kn_cx;
            if (kn_given[idx] == 0) { kn_cell[idx] = 0; audio_move(); }
        }
        break;
    case K_PAUSE:
        if (ev->is_repeat) break;
        break;
    case K_CHAR:
        if (ev->ch == 'w') {
            kn_move(0, -1);
        } else if (ev->ch == 'a') {
            kn_move(-1, 0);
        } else if (ev->ch == 's') {
            kn_move(0, 1);
        } else if (ev->ch == 'd') {
            kn_move(1, 0);
        } else if (ev->ch == 'n') {
            if (!ev->is_repeat) { audio_select(); kn_start_game((kn_pz + 1) % KN_ROM_N); }
        } else if (ev->ch == 'r') {
            if (!ev->is_repeat) { audio_select(); kn_start_game(kn_pz); }
        } else if (ev->ch >= '1' && ev->ch <= '4') {
            if (!ev->is_repeat) {
                int idx = kn_input ? (int)kn_sel
                                   : (int)kn_cy * 4 + (int)kn_cx;
                kn_place(idx, (uint8_t)(ev->ch - '0'));
                audio_tick();
            }
        }
        break;
    case K_BACK: {
        if (ev->is_repeat) break;
        pause_sel_t sel;
        if (ui_pause_run(&sel)) {
            if (sel == PAUSE_RESTART) kn_start_game(kn_pz);
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
