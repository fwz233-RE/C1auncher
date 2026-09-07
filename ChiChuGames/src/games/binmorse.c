/* BINMORSE — 极客训练: 十进制转二进制 + 摩斯电码
 * 两个模式('m' 切换, 避免与摩斯输入左右键冲突):
 *   BIN:   显示十进制数 → 输入二进制(O=0 Z=1, 左/右=0/1), OK 提交
 *   MORSE: 显示字母 → 输入 .-(左=点 右=划), OK 提交
 * 60 秒限时计分, 答对 +5; 答错显示正确答案 1.4s 后自动换题(不减分)
 * 难度递增: 二进制位数 4→8(每答对 4 题扩 1 位, 值域 1-15 → 1-255);
 *           摩斯字母 A-Z(避免连续同字母)
 * HUD 顶栏: 左 BINMORSE, 右 SCORE n LV k TIME s; 结束左上结果右上重试提示
 * 静态前缀 bm_; 零 malloc; 像素坐标一律 int
 *
 * 集成提示(help[] 最多 5 行):
 *   "BIN & MORSE TRAINER", "BIN: O=0 Z=1 (LT/RT SAME)",
 *   "MORSE: LT=DOT RT=DASH", "M: MODE  OK: SUBMIT  60 SEC",
 *   "OK/N: RETRY  BACK: QUIT"
 */
#include "game.h"
#include "../config.h"
#include "../gfx/canvas.h"
#include "../gfx/font.h"
#include "../platform/audio.h"
#include "../platform/display.h"
#include "../platform/input.h"
#include "../platform/led.h"
#include "../platform/time.h"
#include "../rng.h"

/* ---- 节奏 ---- */
#define BM_TICK_MS     100u
#define BM_GAME_MS     60000u        /* 60 秒限时 */
#define BM_FB_OK_MS    900u          /* 答对反馈时长 */
#define BM_FB_BAD_MS   1400u         /* 答错反馈时长 */

/* ---- 布局: 标签/大题/答案格/反馈/提示 ---- */
#define BM_QUEST_Y     30            /* 3x 大题顶 y */
#define BM_CELL_Y      62            /* 答案格顶 y */
#define BM_CELL_W      18
#define BM_CELL_H      22
#define BM_CELL_GAP    6
#define BM_FB_Y        96            /* 反馈行 */
#define BM_HINT_Y      112           /* 输入提示行 */

/* ---- 数据上限 ---- */
#define BM_MAX_BITS    8             /* 二进制最多 8 位(255) */
#define BM_MAX_MORSE   4             /* 摩斯最长 4 符号 */

/* ---- 模式 ---- */
enum { BM_BIN = 0, BM_MORSE };

/* 摩斯表 A-Z(长度表避免 strlen) */
static const char *const bm_code[26] = {
    ".-", "-...", "-.-.", "-..", ".", "..-.", "--.", "....",
    "..", ".---", "-.-", ".-..", "--", "-.", "---", ".--.",
    "--.-", ".-.", "...", "-", "..-", "...-", ".--", "-..-",
    "-.--", "--.."
};
static const uint8_t bm_clen[26] = {
    2, 4, 4, 3, 1, 4, 3, 4, 2, 4, 3, 4, 2, 2, 3, 4,
    4, 3, 3, 1, 3, 4, 3, 4, 4, 4
};

static uint8_t  bm_mode;            /* BM_BIN / BM_MORSE */
static uint32_t bm_score;
static uint32_t bm_correct;         /* 答对次数(难度进度) */
static uint32_t bm_elapsed;         /* 已进行 ms */
static bool     bm_over;
static bool     bm_over_full;
static uint32_t bm_target;          /* 二进制目标值 */
static uint8_t  bm_letter;          /* 摩斯目标字母 0-25 */
static uint8_t  bm_bits[BM_MAX_BITS]; /* 玩家答案: 0/1 或 '.'/'-' */
static uint8_t  bm_n;               /* 答案长度 */
static uint8_t  bm_fb;              /* 反馈: 0=无 1=对 2=错 */
static uint64_t bm_fb_until;
static uint32_t bm_gen;             /* 换局计数(种子混合) */
static rng_t    bm_rng;

void binmorse_render(void);

/* 手写数字追加(无 snprintf 依赖) */
static void bm_append_u32(char *buf, unsigned *n, uint32_t v, unsigned cap) {
    char tmp[8];
    unsigned len = 0;
    if (v == 0) tmp[len++] = '0';
    while (v && len < 7) { tmp[len++] = (char)('0' + (v % 10u)); v /= 10u; }
    while (len && *n < cap) buf[(*n)++] = tmp[--len];
}

/* 追加字符串 */
static void bm_append_str(char *buf, unsigned *n, const char *s, unsigned cap) {
    while (*s && *n < cap) buf[(*n)++] = *s++;
}

/* 十进制 → 二进制字符串(MSB first, 无前导零); 返回位数 */
static int bm_to_binary(uint32_t v, char out[9]) {
    uint32_t bit = 0x80000000u;
    int len = 0;
    if (v == 0) { out[0] = '0'; out[1] = 0; return 1; }
    while (!(v & bit)) bit >>= 1;    /* 定位最高位 */
    for (; bit; bit >>= 1)
        out[len++] = (v & bit) ? '1' : '0';
    out[len] = 0;
    return len;
}

/* 当前难度: 二进制位数 4..8(每 4 题扩 1 位) */
static int bm_bitlen(void) {
    unsigned lv = bm_correct / 4u;
    if (lv > 4u) lv = 4u;
    return 4 + (int)lv;
}

/* 换一题(当前模式), 避免与上一题重复(do-while 带 guard) */
static void bm_next_q(void) {
    if (bm_mode == BM_BIN) {
        int bits = bm_bitlen();
        uint32_t maxv = (1u << bits) - 1u;
        uint32_t v;
        int guard = 0;
        do {
            v = 1u + rng_range(&bm_rng, maxv - 1u);
        } while (v == bm_target && ++guard < 16);
        bm_target = v;
    } else {
        uint8_t i;
        int guard = 0;
        do {
            i = (uint8_t)rng_range(&bm_rng, 26u);
        } while (i == bm_letter && ++guard < 16);
        bm_letter = i;
    }
    bm_n = 0;
    bm_fb = 0;
}

static void bm_new_game(void) {
    bm_gen++;
    rng_seed(&bm_rng, (uint64_t)now_ms() ^ ((uint64_t)bm_gen * 0x9E3779B1u));
    bm_mode = BM_BIN;
    bm_score = 0;
    bm_correct = 0;
    bm_elapsed = 0;
    bm_over = false;
    bm_over_full = false;
    bm_target = 0;
    bm_letter = 0xFFu;               /* 不存在的"上一题" */
    bm_fb = 0;
    bm_n = 0;
    bm_next_q();
    binmorse_render();
    disp_full();
}

void binmorse_enter(void) { bm_new_game(); }

/* ---- 输入编辑 ---- */
static void bm_add_bit(uint8_t b) {
    if (bm_n >= BM_MAX_BITS) return;
    bm_bits[bm_n++] = b;
}

static void bm_add_sym(uint8_t s) {
    if (bm_n >= BM_MAX_MORSE) return;
    bm_bits[bm_n++] = s;
}

/* 左右方向: 二进制=0/1, 摩斯=点/划 */
static void bm_add_dir(int right) {
    if (bm_mode == BM_BIN) {
        bm_add_bit((uint8_t)right);
    } else {
        bm_add_sym(right ? (uint8_t)'-' : (uint8_t)'.');
    }
}

/* OK 提交: 完全匹配(长度+内容) 则 +5, 否则标记错误 */
static void bm_submit(void) {
    if (bm_n == 0) return;
    bool ok = false;
    if (bm_mode == BM_BIN) {
        char want[9];
        int wl = bm_to_binary(bm_target, want);
        if ((int)bm_n == wl) {
            ok = true;
            for (int i = 0; i < wl; i++)
                if (bm_bits[i] != (uint8_t)(want[i] - '0')) { ok = false; break; }
        }
    } else {
        int cl = (int)bm_clen[bm_letter];
        if ((int)bm_n == cl) {
            ok = true;
            for (int i = 0; i < cl; i++)
                if (bm_bits[i] != (uint8_t)bm_code[bm_letter][i]) { ok = false; break; }
        }
    }
    if (ok) {
        bm_score += 5u;
        bm_correct++;
        bm_fb = 1;
        audio_clear();            /* 答对 +5 */
    } else {
        bm_fb = 2;
        audio_error();            /* 答错 */
    }
    bm_fb_until = now_ms() + (ok ? BM_FB_OK_MS : BM_FB_BAD_MS);
}

/* ---- 绘制 ---- */

/* 字形 n 倍放大(font 位序: bit i = 像素 x+i) */
static void bm_draw_glyph(int x, int y, char ch, int scale, bool black) {
    const uint8_t *g = font_glyph5x7[(uint8_t)ch];
    for (int j = 0; j < 7; j++)
        for (int i = 0; i < 5; i++)
            if (g[j] & (1u << i))
                fb_fill_rect(x + i * scale, y + j * scale, scale, scale, black);
}

/* 答案格数(当前模式) */
static int bm_cells(void) {
    return (bm_mode == BM_BIN) ? BM_MAX_BITS : BM_MAX_MORSE;
}

/* 第 i 格左上 x(整行居中) */
static int bm_cell_x(int i) {
    int n = bm_cells();
    int total = n * BM_CELL_W + (n - 1) * BM_CELL_GAP;
    return (CCG_W - total) / 2 + i * (BM_CELL_W + BM_CELL_GAP);
}

/* 格内符号(普通色) */
static void bm_cell_glyph(int x, int y, uint8_t v, bool black) {
    if (bm_mode == BM_BIN) {
        bm_draw_glyph(x + 4, y + 4, (char)('0' + (int)v), 2, black);
    } else {
        if (v == '.') fb_fill_rect(x + 7, y + 9, 4, 4, black);
        else          fb_fill_rect(x + 4, y + 9, 10, 3, black);
    }
}

void binmorse_render(void) {
    fb_clear(false);

    if (!bm_over) {
        /* 模式标签行 */
        fb_text_center(BM_QUEST_Y - 8,
                       (bm_mode == BM_BIN) ? "[BIN] DEC->BIN" : "[MORSE] LETTER->CODE",
                       true);
        /* 大题: 3x 目标值 */
        if (bm_mode == BM_BIN) {
            char s[4];
            unsigned n = 0;
            bm_append_u32(s, &n, bm_target, 3);
            s[n] = 0;
            int w = (int)n * 15;
            int x = (CCG_W - w) / 2;
            for (unsigned i = 0; i < n; i++)
                bm_draw_glyph(x + (int)i * 15, BM_QUEST_Y, s[i], 3, true);
        } else {
            bm_draw_glyph((CCG_W - 15) / 2, BM_QUEST_Y,
                          (char)('A' + (int)bm_letter), 3, true);
        }
        /* 答案格(先全部画完) */
        int n = bm_cells();
        for (int i = 0; i < n; i++) {
            int x = bm_cell_x(i);
            if (i < (int)bm_n)
                bm_cell_glyph(x, BM_CELL_Y, bm_bits[i], true);
            fb_stroke_rect(x, BM_CELL_Y, BM_CELL_W, BM_CELL_H, true);
        }
        /* 光标: 下一输入位反白格(最后画) */
        if (bm_fb == 0 && (int)bm_n < n) {
            int x = bm_cell_x((int)bm_n);
            fb_fill_rect(x, BM_CELL_Y, BM_CELL_W, BM_CELL_H, true);
            bm_cell_glyph(x, BM_CELL_Y,
                          (bm_mode == BM_BIN) ? (uint8_t)0 : (uint8_t)'.',
                          false);
            fb_stroke_rect(x, BM_CELL_Y, BM_CELL_W, BM_CELL_H, true);
        }
        /* 反馈行 */
        if (bm_fb == 1) {
            fb_text_center(BM_FB_Y, "CORRECT +5", true);
        } else if (bm_fb == 2) {
            char buf[24];
            unsigned n = 0;
            bm_append_str(buf, &n, "WRONG! ", sizeof(buf));
            if (bm_mode == BM_BIN) {
                bm_append_u32(buf, &n, bm_target, sizeof(buf));
                bm_append_str(buf, &n, " = ", sizeof(buf));
                char bin[9];
                bm_to_binary(bm_target, bin);
                bm_append_str(buf, &n, bin, sizeof(buf));
            } else {
                buf[n++] = (char)('A' + (int)bm_letter);
                bm_append_str(buf, &n, " = ", sizeof(buf));
                bm_append_str(buf, &n, bm_code[bm_letter], sizeof(buf));
            }
            buf[n] = 0;
            fb_text_center(BM_FB_Y, buf, true);
        }
        /* 输入提示行 */
        fb_text_center(BM_HINT_Y,
                       (bm_mode == BM_BIN)
                           ? "O=0 Z=1 LT/RT=0/1 DEL:UNDO OK:GO"
                           : "LT:DOT RT:DASH DEL:UNDO OK:GO",
                       true);
    } else {
        /* 结算: 墙中央大字 TIME UP, 结果在 HUD */
        const char *t = "TIME UP";
        for (int i = 0; t[i]; i++)
            bm_draw_glyph((CCG_W - 105) / 2 + i * 15, 60, t[i], 3, true);
        fb_text_center(95, "SCORE", true);
        char sv[8];
        unsigned sn = 0;
        bm_append_u32(sv, &sn, bm_score, 7);
        sv[sn] = 0;
        int sw = (int)sn * 15;
        for (unsigned i = 0; i < sn; i++)
            bm_draw_glyph((CCG_W - sw) / 2 + (int)i * 15, 108, sv[i], 3, true);
    }

    /* HUD 顶栏 */
    if (bm_over) {
        fb_fill_rect(0, 0, CCG_W, CCG_HUD_H - 1, false);
        char buf[24];
        unsigned n = 0;
        bm_append_str(buf, &n, "SCORE ", sizeof(buf));
        bm_append_u32(buf, &n, bm_score, sizeof(buf));
        buf[n] = 0;
        fb_text(2, 2, buf, true);
        fb_text(CCG_W - 2 - text_width("OK/N:RETRY BACK:QUIT"), 2,
                "OK/N:RETRY BACK:QUIT", true);
        if (!bm_over_full) { bm_over_full = true; disp_force_full(); }
    } else {
        fb_text(0, 0, "BINMORSE", true);
        char buf[32];
        unsigned n = 0;
        bm_append_str(buf, &n, "SCORE ", sizeof(buf));
        bm_append_u32(buf, &n, bm_score, sizeof(buf));
        bm_append_str(buf, &n, " LV ", sizeof(buf));
        bm_append_u32(buf, &n, (uint32_t)(bm_bitlen() - 3), sizeof(buf));
        bm_append_str(buf, &n, " TIME ", sizeof(buf));
        bm_append_u32(buf, &n, (BM_GAME_MS - bm_elapsed) / 1000u, sizeof(buf));
        buf[n] = 0;
        fb_text(CCG_W - 4 - text_width(buf), 0, buf, true);
        fb_hline(0, CCG_HUD_H - 1, CCG_W, true);
    }
}

void binmorse_tick(uint64_t now) {
    if (bm_over) return;
    bm_elapsed += BM_TICK_MS;
    if (bm_elapsed >= BM_GAME_MS) {
        bm_over = true;              /* 时间到 → 结算 */
        audio_lose();
        led_fx_set(LED_FX_LOSE);
        bm_over_full = false;
        return;
    }
    if (bm_fb && now >= bm_fb_until) bm_next_q();
}

void binmorse_on_key(const key_event_t *ev) {
    bool dir = ev->key == K_UP || ev->key == K_DOWN ||
               ev->key == K_LEFT || ev->key == K_RIGHT || ev->key == K_DEL;
    if (ev->is_repeat && !dir) return;   /* 确认键/字母忽略重复 */
    if (bm_over) {
        if (ev->key == K_OK || (ev->key == K_CHAR && ev->ch == 'n'))
            binmorse_enter();
        else if (ev->key == K_BACK || ev->key == K_QUIT)
            s_exit_request = true;
        return;
    }
    if (bm_fb) return;               /* 反馈期间忽略游戏输入 */
    switch (ev->key) {
    case K_LEFT:  bm_add_dir(0); audio_tick(); break;
    case K_RIGHT: bm_add_dir(1); audio_tick(); break;
    case K_DEL:   if (bm_n > 0) { bm_n--; audio_move(); } break;
    case K_OK:    bm_submit(); break;
    case K_CHAR:
        switch (ev->ch) {
        case 'o': case '0':
        case 'z': case '1':
            if (bm_mode == BM_BIN) { bm_add_bit((ev->ch == 'z' || ev->ch == '1') ? 1u : 0u); audio_tick(); }
            break;
        case 'm': audio_select(); bm_mode ^= 1u; bm_next_q(); break;
        case 'n': audio_select(); binmorse_enter(); break;
        default: break;
        }
        break;
    case K_BACK:
    case K_PAUSE: {
        pause_sel_t sel;
        if (ui_pause_run(&sel)) {
            if (sel == PAUSE_RESTART) binmorse_enter();
        } else {
            s_exit_request = true;
        }
        break;
    }
    case K_QUIT: s_exit_request = true; break;
    default: break;
    }
}

void binmorse_exit(void) {}
