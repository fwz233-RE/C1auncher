/* NIM — 取子博弈(每手从一堆取 1-3 颗), misère/normal 双规则
 * 玩家先手: 方向键选堆(光标=反白整行), OK 弹出 1/2/3 取子面板, 再 OK 确认
 * AI: 对限步 1-3 的尼姆, 以每堆 Grundy 值(堆数 mod 4)异或做必胜策略
 *     (s≠0 取到 s=0, s=0 或限步内无解则随机合法步); misère 附加
 *     "全 1 堆留奇数个"终局规则(rng_seed 播种防零)
 * 输入驱动(同 gomoku/reversi): AI 即时响应, HUD 显示 AI 落手
 * 开局/结束全刷(disp_force_full 防重), 游戏内主循环快刷
 *
 * games_table.c 建议条目(help ≤5 行 + NULL):
 *   "TAKE 1-3 STONES FROM A HEAP",
 *   "MISERE: LAST PICK LOSES",
 *   "NORMAL: LAST PICK WINS",
 *   "OK: SELECT  N: NEW GAME",
 *   NULL */
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

#define NM_MAX_HEAPS 4
#define NM_ROW_H 26          /* 行距 */
#define NM_ROW_TOP 17        /* 游戏区顶(顶栏 16 下留 1px) */
#define NM_BAND_H 22         /* 光标反白带高度 */
#define NM_STONE_SZ 12       /* 石子方块边长 */
#define NM_PITCH 16          /* 石间距 */
#define NM_STONE_X 46        /* 石列起点 x */
#define NM_LABEL_X 10        /* H1..H4 标签 x */
#define NM_PANEL_Y 120       /* 取子面板顶 */

enum { NM_MODE = 0, NM_PICK, NM_TAKE, NM_OVER };

static int nm_heaps[NM_MAX_HEAPS];        /* 每堆余量 0..8 */
static int nm_n;                          /* 堆数 3..4 */
static int nm_mode;                       /* 0=misère(取末者输) 1=normal(取末者胜) */
static int nm_state;
static int nm_cur;                        /* 光标堆 0..n-1 */
static int nm_take_sel;                   /* 取子面板选中 1..3 */
static int nm_maxk;                       /* 当前堆可取上限 min(3,余量) */
static int nm_last_ai_h, nm_last_ai_k;    /* AI 最近一手(-1=无), HUD 显示 */
static int nm_winner;                     /* 0=无 1=玩家 2=AI */
static bool nm_over_full;                 /* 结束全刷防重 */
static uint32_t nm_seed_cnt;
static rng_t nm_rng;

void nim_render(void);

/* ---- AI 走子 ----
 * 纯函数: 输入堆形返回一手; 返回 0 成功(必然), total=0 返回 -1
 * 1) 全部堆 ≤1(每步恰取 1 颗): misère 留奇数个 1 堆(偶=对方败形)
 * 2) Grundy 异或 s≠0 → 取 k 使 (h-k)%4 = g^s, 1≤k≤3(限步下正确的 nim-sum)
 *    misère: 若落手后全堆 ≤1 且 1 堆数为偶(败形)则跳过该胜手
 * 3) misère 补救: 留出奇数个 1 堆(对方必取最后一颗而输)
 * 4) 必败/限步内无解: 随机合法步 */
static int nm_plan_move(const int *h, int n, bool misere, int *hi, int *ki) {
    int i, k, ones = 0, total = 0;
    for (i = 0; i < n; i++) { if (h[i] == 1) ones++; total += h[i]; }
    if (total == 0) return -1;
    if (ones == total) {                       /* 只剩 1 堆 */
        for (i = 0; i < n; i++)
            if (h[i] == 1) { *hi = i; *ki = 1; return 0; }
    }
    int s = 0;
    for (i = 0; i < n; i++) s ^= h[i] % 4;     /* Grundy 异或 */
    if (s != 0) {
        for (i = 0; i < n; i++) {
            int need = (h[i] % 4) ^ s;
            for (k = 1; k <= 3 && k <= h[i]; k++) {
                if ((h[i] - k) % 4 != need) continue;
                if (misere && h[i] - k <= 1) {          /* 可能留全 1 堆 */
                    int rest = 0, all1 = 1;
                    for (int j = 0; j < n; j++)
                        if (j != i) { if (h[j] > 1) all1 = 0; else rest++; }
                    if (all1 && (rest + (h[i] - k == 1)) % 2 == 0)
                        continue;                       /* 偶数 1 堆=misère 败形 */
                }
                *hi = i; *ki = k; return 0;
            }
        }
    }
    if (misere) {                              /* 终局补救: 留奇数个 1 堆 */
        for (i = 0; i < n; i++)
            if (h[i] >= 2) {
                int rest = 0, all1 = 1;
                for (int j = 0; j < n; j++)
                    if (j != i) { if (h[j] > 1) all1 = 0; else rest++; }
                if (!all1) continue;
                for (k = 1; k <= 3 && k <= h[i]; k++) {
                    int left = h[i] - k;
                    if (left > 1) continue;             /* 必须留成全 1 堆 */
                    if ((rest + (left == 1)) % 2 == 1) { *hi = i; *ki = k; return 0; }
                }
            }
    }
    {                                          /* 必败: 随机合法步 */
        int cands[NM_MAX_HEAPS], nc = 0;
        for (i = 0; i < n; i++) if (h[i] > 0) cands[nc++] = i;
        i = cands[(int)rng_range(&nm_rng, (uint32_t)nc)];
        int m = h[i] < 3 ? h[i] : 3;
        *hi = i;
        *ki = 1 + (int)rng_range(&nm_rng, (uint32_t)m);
    }
    return 0;
}

/* 新局: 3-4 堆, 每堆 3-8 颗 */
static void nim_start(void) {
    nm_n = 3 + (int)rng_range(&nm_rng, 2);
    for (int i = 0; i < nm_n; i++)
        nm_heaps[i] = 3 + (int)rng_range(&nm_rng, 6);
    nm_cur = 0;
    nm_take_sel = 1;
    nm_maxk = 3;
    nm_last_ai_h = -1;
    nm_last_ai_k = 0;
    nm_winner = 0;
    nm_over_full = false;
    nm_state = NM_PICK;
}

/* mover 刚取完: 若清空棋盘则终局 */
static void nm_check_after(int mover) {
    int total = 0;
    for (int i = 0; i < nm_n; i++) total += nm_heaps[i];
    if (total == 0) {
        nm_state = NM_OVER;
        nm_winner = (nm_mode == 0) ? (mover == 1 ? 2 : 1)   /* misère: 取末者输 */
                                   : mover;                  /* normal: 取末者胜 */
        if (nm_winner == 1) {
            audio_win();
            led_fx_set(LED_FX_WIN);
        } else {
            audio_lose();
            led_fx_set(LED_FX_LOSE);
        }
    }
}

static void nm_pause(void) {
    pause_sel_t sel;
    if (ui_pause_run(&sel)) {
        if (sel == PAUSE_RESTART) nim_start();
    } else {
        s_exit_request = true;
    }
}

/* ---- 渲染 ---- */
static void draw_btn(int x, int y, int w, int h, const char *s, bool sel) {
    int tw = text_width(s);
    if (sel) {
        fb_fill_rect(x, y, w, h, true);
        fb_text(x + (w - tw) / 2, y + (h - 7) / 2, s, false);
    } else {
        fb_stroke_rect(x, y, w, h, true);
        fb_text(x + (w - tw) / 2, y + (h - 7) / 2, s, true);
    }
}

static void draw_mode_screen(void) {
    fb_text(0, 0, "NIM", true);
    fb_text(CCG_W - 2 - text_width("SETUP RULE"), 0, "SETUP RULE", true);
    fb_hline(0, CCG_HUD_H - 1, CCG_W, true);
    int tw = text_width("NIM") * 2;
    fb_text_scale2((CCG_W - tw) / 2, 24, "NIM", true);
    fb_text_center(50, "TAKE 1-3 FROM ONE HEAP", true);
    fb_text_center(62, "LAST STONE RULE:", true);
    draw_btn(80, 78, 64, 22, "MISERE", nm_mode == 0);
    draw_btn(156, 78, 64, 22, "NORMAL", nm_mode == 1);
    fb_text(226, 86, "L/R MOVE", true);
    fb_text_center(108, "MISERE = LAST PICKER LOSES", true);
    fb_text_center(120, "NORMAL = LAST PICKER WINS", true);
    fb_text_center(140, "OK START   BACK QUIT", true);
}

static void draw_heaps_hud(void) {
    int total = 0;
    for (int i = 0; i < nm_n; i++) total += nm_heaps[i];
    char line[12];
    int m = 0;
    const char *lab = "HEAP ";
    while (lab[m]) { line[m] = lab[m]; m++; }
    char d[3];
    int dig = 0;
    int t = total;
    do { d[dig++] = (char)('0' + t % 10); t /= 10; } while (t && dig < 2);
    while (dig > 0) line[m++] = d[--dig];
    line[m] = 0;
    fb_text(CCG_W - 2 - text_width(line), 0, line, true);
}

static void draw_turn_hud(void) {
    char t[16];
    if (nm_last_ai_h >= 0) {
        t[0] = 'A'; t[1] = 'I'; t[2] = ' '; t[3] = '-';
        t[4] = (char)('0' + nm_last_ai_k);
        t[5] = ' '; t[6] = 'H';
        t[7] = (char)('0' + nm_last_ai_h + 1);
        t[8] = 0;
    } else {
        const char *s = nm_state == NM_TAKE ? "PICK AMOUNT" : "YOUR TURN";
        int i = 0;
        while (s[i]) { t[i] = s[i]; i++; }
        t[i] = 0;
    }
    fb_text(CCG_W - 2 - text_width(t), 8, t, true);
}

/* 第 i 堆: 标签 + 石子方块; invert=true 反色(光标带内白字白块) */
static void draw_stones(int i, bool invert) {
    int ry = NM_ROW_TOP + i * NM_ROW_H;
    char lab[3];
    lab[0] = 'H';
    lab[1] = (char)('0' + i + 1);
    lab[2] = 0;
    fb_text(NM_LABEL_X, ry + 8, lab, !invert);
    for (int s = 0; s < nm_heaps[i]; s++)
        fb_fill_rect(NM_STONE_X + s * NM_PITCH, ry + 5, NM_STONE_SZ, NM_STONE_SZ, !invert);
}

static void draw_board(void) {
    for (int i = 0; i < nm_n; i++) draw_stones(i, false);
    if (nm_state == NM_PICK || nm_state == NM_TAKE) {
        /* 光标: 反白整行, 最后画以盖住格子 */
        int ry = NM_ROW_TOP + nm_cur * NM_ROW_H;
        fb_fill_rect(0, ry, CCG_W, NM_BAND_H, true);
        draw_stones(nm_cur, true);
    }
}

static void draw_take_panel(void) {
    fb_fill_rect(8, NM_PANEL_Y, CCG_W - 16, 30, false);   /* 白底盖残留 */
    fb_stroke_rect(8, NM_PANEL_Y, CCG_W - 16, 30, true);
    char lab[16];
    int m = 0;
    const char *pre = "TAKE FROM H";
    while (pre[m]) { lab[m] = pre[m]; m++; }
    lab[m++] = (char)('0' + nm_cur + 1);
    lab[m] = 0;
    fb_text(14, NM_PANEL_Y + 4, lab, true);
    for (int b = 0; b < 3; b++) {
        if (b + 1 > nm_maxk) continue;                 /* 超出余量不显示 */
        int bx = 110 + b * 34;
        bool sel = (b + 1 == nm_take_sel);
        if (sel) fb_fill_rect(bx, NM_PANEL_Y + 12, 28, 16, true);
        else fb_stroke_rect(bx, NM_PANEL_Y + 12, 28, 16, true);
        char d[2];
        d[0] = (char)('0' + b + 1);
        d[1] = 0;
        fb_text(bx + 11, NM_PANEL_Y + 16, d, !sel);
    }
    fb_text(224, NM_PANEL_Y + 15, "OK TAKE", true);
}

void nim_render(void) {
    fb_clear(false);
    if (nm_state == NM_MODE) { draw_mode_screen(); return; }
    if (nm_state == NM_OVER) {
        fb_fill_rect(0, 0, CCG_W, CCG_HUD_H - 1, false);
        const char *res = nm_winner == 1 ? "YOU WIN!" : "AI WINS";
        fb_text(2, 2, res, true);
        fb_text(CCG_W - 2 - text_width("OK/N:RETRY BACK:QUIT"), 2,
                "OK/N:RETRY BACK:QUIT", true);
        fb_text(2, 10, nm_mode == 0 ? "MISERE" : "NORMAL", true);
        fb_text(CCG_W - 2 - text_width("N:NEW GAME"), 10, "N:NEW GAME", true);
        if (!nm_over_full) { nm_over_full = true; disp_force_full(); }
    } else {
        fb_text(0, 0, "NIM", true);
        draw_heaps_hud();
        fb_text(0, 8, nm_mode == 0 ? "MISERE" : "NORMAL", true);
        draw_turn_hud();
    }
    draw_board();
    if (nm_state == NM_TAKE) draw_take_panel();
}

/* ---- 输入 ---- */
void nim_on_key(const key_event_t *ev) {
    if (ev->is_repeat &&
        ev->key != K_UP && ev->key != K_DOWN &&
        ev->key != K_LEFT && ev->key != K_RIGHT)
        return;                          /* 确认键/字母忽略重复 */
    switch (nm_state) {
    case NM_MODE:
        switch (ev->key) {
        case K_LEFT: case K_RIGHT: case K_UP: case K_DOWN:
            nm_mode = !nm_mode;
            break;
        case K_CHAR:
            if (ev->ch == 'a' || ev->ch == 'd' || ev->ch == 'w' || ev->ch == 's')
                nm_mode = !nm_mode;
            else if (ev->ch == 'n') nim_start();
            else if (ev->ch == 'q') s_exit_request = true;
            break;
        case K_OK: nim_start(); break;
        case K_BACK: case K_QUIT: s_exit_request = true; break;
        default: break;
        }
        break;
    case NM_PICK:
        switch (ev->key) {
        case K_UP: if (nm_cur > 0) nm_cur--; break;
        case K_DOWN: if (nm_cur < nm_n - 1) nm_cur++; break;
        case K_CHAR:
            if (ev->ch == 'w' && nm_cur > 0) nm_cur--;
            else if (ev->ch == 's' && nm_cur < nm_n - 1) nm_cur++;
            else if (ev->ch == 'n') nim_start();
            else if (ev->ch == 'q') s_exit_request = true;
            break;
        case K_OK:
            if (nm_heaps[nm_cur] > 0) {
                nm_maxk = nm_heaps[nm_cur] < 3 ? nm_heaps[nm_cur] : 3;
                nm_take_sel = 1;
                nm_state = NM_TAKE;
                audio_select();
            }
            break;
        case K_BACK: case K_PAUSE: nm_pause(); break;
        case K_QUIT: s_exit_request = true; break;
        default: break;
        }
        break;
    case NM_TAKE:
        switch (ev->key) {
        case K_LEFT:
            nm_take_sel = nm_take_sel > 1 ? nm_take_sel - 1 : nm_maxk;
            break;
        case K_RIGHT:
            nm_take_sel = nm_take_sel < nm_maxk ? nm_take_sel + 1 : 1;
            break;
        case K_CHAR:
            if (ev->ch == 'a')
                nm_take_sel = nm_take_sel > 1 ? nm_take_sel - 1 : nm_maxk;
            else if (ev->ch == 'd')
                nm_take_sel = nm_take_sel < nm_maxk ? nm_take_sel + 1 : 1;
            else if (ev->ch == 'q') s_exit_request = true;
            break;
        case K_OK: {
            int take = nm_take_sel;
            nm_heaps[nm_cur] -= take;
            audio_clear();
            nm_last_ai_h = -1;
            nm_check_after(1);
            if (nm_state != NM_OVER) {
                int hi, ki;
                if (nm_plan_move(nm_heaps, nm_n, nm_mode == 0, &hi, &ki) == 0) {
                    nm_heaps[hi] -= ki;
                    nm_last_ai_h = hi;
                    nm_last_ai_k = ki;
                }
                nm_check_after(2);
            }
            if (nm_state != NM_OVER) {
                nm_state = NM_PICK;
                if (nm_heaps[nm_cur] == 0)
                    for (int i = 0; i < nm_n; i++)
                        if (nm_heaps[i] > 0) { nm_cur = i; break; }
            }
            break;
        }
        case K_BACK: nm_state = NM_PICK; break;   /* 取消取子 */
        case K_PAUSE: nm_pause(); break;
        case K_QUIT: s_exit_request = true; break;
        default: break;
        }
        break;
    case NM_OVER:
        if (ev->key == K_OK || (ev->key == K_CHAR && ev->ch == 'n'))
            nim_start();
        else if (ev->key == K_BACK || ev->key == K_QUIT)
            s_exit_request = true;
        break;
    default: break;
    }
}

/* ---- 框架 ---- */
void nim_enter(void) {
    rng_seed(&nm_rng, now_ms() ^ 0x16D3u ^ ((uint64_t)nm_seed_cnt++ << 32));
    nm_mode = 0;
    nm_state = NM_MODE;
    nm_cur = 0;
    nm_take_sel = 1;
    nm_maxk = 3;
    nm_last_ai_h = -1;
    nm_last_ai_k = 0;
    nm_winner = 0;
    nm_over_full = false;
    nim_render();
    disp_full();
}

void nim_exit(void) {}

void nim_tick(uint64_t now) { (void)now; }
