/* BULLS & COWS — 猜 4 位不重复数字
 * 电脑随机生成 4 位不重复数字(0-9, 首位可 0); 玩家 10 次机会
 * 输入: 4 个输入位, 左右/A/D 移动光标, 上下/W/S 循环 0-9, OK 提交整行
 * 反馈: 位置对且数字对 = BULL, 数字对位置错 = COW; 行尾显示 Bn Cn
 * 布局: HUD 顶栏(标题 + TRY n/10) + 9 行历史 + 底部中央 4 格输入区(36x30)
 * 键位: 左右/A/D 选位, 上下/W/S 换数字, OK 提交, N 新局,
 *       BACK/P 暂停, Q 退出 */
#include "game.h"
#include "../config.h"
#include "../gfx/canvas.h"
#include "../gfx/font.h"
#include "../platform/display.h"
#include "../platform/input.h"
#include "../platform/time.h"
#include "../platform/audio.h"
#include "../platform/led.h"
#include "../rng.h"

#define BC_LEN 4                /* 密码长度 */
#define BC_ATTEMPTS 10          /* 最多尝试次数 */
#define BC_HIST_ROWS 9          /* 历史可见行数 */

/* 输入区: 4 格 36x30, 间隙 4, 水平居中, 贴底部 */
#define BC_CELL_W 36
#define BC_CELL_H 30
#define BC_CELL_GAP 4
#define BC_GRID_W (BC_LEN * BC_CELL_W + (BC_LEN - 1) * BC_CELL_GAP)  /* 156 */
#define BC_IN_X ((CCG_W - BC_GRID_W) / 2)                            /* 70 */
#define BC_IN_Y (CCG_H - BC_CELL_H - 6)                              /* 116 */

/* 历史区: 9 行 x10px, 4 个小数字格 + 行尾反馈 "Bn Cn" */
#define BC_HIST_Y 18
#define BC_HIST_ROW_H 10
#define BC_HIST_CELL_W 14
#define BC_HIST_GAP 3
#define BC_HIST_W (BC_LEN * BC_HIST_CELL_W + (BC_LEN - 1) * BC_HIST_GAP) /* 65 */
#define BC_HIST_X ((CCG_W - (BC_HIST_W + 8 + text_width("B0 C0"))) / 2)
#define BC_HIST_FB_X (BC_HIST_X + BC_HIST_W + 8)

static uint8_t bc_hist[BC_ATTEMPTS][BC_LEN];  /* 已提交行 */
static uint8_t bc_bulls[BC_ATTEMPTS];         /* 各行动反馈 BULL 数 */
static uint8_t bc_cows[BC_ATTEMPTS];          /* 各行动反馈 COW 数 */
static int    bc_cur[BC_LEN];                 /* 当前行数字 0-9 */
static int    bc_secret[BC_LEN];              /* 谜底 */
static int    bc_sel;                         /* 光标位 0..3 */
static int    bc_rows;                        /* 已提交行数 */
static bool   bc_over;                        /* 游戏结束 */
static bool   bc_won;
static bool   bc_over_full;                   /* 结束全刷防重复 */
static rng_t  bc_rng;
static uint64_t bc_seed_cnt;                  /* 换局换种子 */

void bullscows_render(void);

/* 反馈计分: 先数位置对(BULL), 再按剩余名额数数字对(COW) */
static void bc_score(const int cur[BC_LEN], const int sec[BC_LEN],
                     int *bulls, int *cows) {
    int avail[10];
    int i;
    *bulls = 0;
    *cows = 0;
    for (i = 0; i < 10; i++) avail[i] = 0;
    for (i = 0; i < BC_LEN; i++) avail[sec[i]]++;
    for (i = 0; i < BC_LEN; i++) {
        if (cur[i] == sec[i]) {
            (*bulls)++;
            avail[cur[i]]--;
        }
    }
    for (i = 0; i < BC_LEN; i++) {
        if (cur[i] != sec[i] && avail[cur[i]] > 0) {
            (*cows)++;
            avail[cur[i]]--;
        }
    }
}

/* 提交当前行 */
static void bc_submit(void) {
    int i, bulls, cows;
    for (i = 0; i < BC_LEN; i++) bc_hist[bc_rows][i] = (uint8_t)bc_cur[i];
    bc_score(bc_cur, bc_secret, &bulls, &cows);
    bc_bulls[bc_rows] = (uint8_t)bulls;
    bc_cows[bc_rows] = (uint8_t)cows;
    bc_rows++;
    if (bulls == BC_LEN) {
        bc_over = true;
        bc_won = true;
        audio_win();                 /* 猜中谜底 */
        led_fx_set(LED_FX_WIN);
    } else if (bc_rows >= BC_ATTEMPTS) {
        bc_over = true;
        bc_won = false;
        audio_lose();                /* 次数耗尽 */
        led_fx_set(LED_FX_LOSE);
    } else {
        audio_select();              /* 提交猜测(未分胜负) */
    }
}

/* 随机谜底: 4 位互不重复(首位可 0); 无拒绝循环, 种子恒非 0 */
static void bc_gen_secret(void) {
    int used[10];
    int i, k;
    for (i = 0; i < 10; i++) used[i] = 0;
    for (i = 0; i < BC_LEN; i++) {
        int pick = (int)rng_range(&bc_rng, (uint32_t)(10 - i)); /* 0..9-i */
        for (k = 0; k < 10; k++) {
            if (!used[k]) {
                if (pick == 0) break;
                pick--;
            }
        }
        used[k] = 1;
        bc_secret[i] = k;
    }
}

void bullscows_enter(void) {
    int r, c;
    for (r = 0; r < BC_ATTEMPTS; r++)
        for (c = 0; c < BC_LEN; c++) bc_hist[r][c] = 0;
    for (r = 0; r < BC_ATTEMPTS; r++) {
        bc_bulls[r] = 0;
        bc_cows[r] = 0;
    }
    for (c = 0; c < BC_LEN; c++) bc_cur[c] = 0;
    bc_sel = 0;
    bc_rows = 0;
    bc_over = false;
    bc_won = false;
    bc_over_full = false;
    bc_seed_cnt++;
    rng_seed(&bc_rng, (uint64_t)now_ms() ^ ((uint64_t)bc_seed_cnt << 32)
                       ^ 0x4243ULL);
    bc_gen_secret();
    bullscows_render();
    disp_full();
}

void bullscows_exit(void) {}

void bullscows_tick(uint64_t now) { (void)now; }

/* HUD: 左标题, 右 TRY n/10 */
static void bc_draw_hud(void) {
    char buf[10];
    int n = bc_rows + 1;
    if (n > BC_ATTEMPTS) n = BC_ATTEMPTS;
    buf[0] = 'T'; buf[1] = 'R'; buf[2] = 'Y'; buf[3] = ' ';
    buf[4] = (char)('0' + n / 10);
    buf[5] = (char)('0' + n % 10);
    buf[6] = '/'; buf[7] = '1'; buf[8] = '0'; buf[9] = 0;
    fb_text_scale2(CCG_W - 2 - text_width(buf) * 2, 1, buf, true);
}

/* 3x 放大数字: 15x21, 落在 36x30 格内居中 */
static void bc_digit3(int x, int y, int d, bool black) {
    const uint8_t *g = font_glyph5x7[(unsigned)('0' + d)];
    int i, j;
    for (j = 0; j < FONT_H; j++)
        for (i = 0; i < FONT_W; i++)
            if (g[j] & (1u << i))
                fb_fill_rect(x + i * 3, y + j * 3, 3, 3, black);
}

void bullscows_render(void) {
    int r, i;
    fb_clear(false);

    /* HUD 顶栏 */
    fb_text_scale2(2, 1, "BULLS&COWS", true);
    if (!bc_over) bc_draw_hud();
    fb_hline(0, CCG_HUD_H - 1, CCG_W, true);

    /* 历史行: 4 数字格 + 行尾反馈 Bn Cn; 只显示最近 9 行 */
    {
        int start = bc_rows > BC_HIST_ROWS ? bc_rows - BC_HIST_ROWS : 0;
        for (r = start; r < bc_rows; r++) {
            int y = BC_HIST_Y + (r - start) * BC_HIST_ROW_H;
            char fb[6];
        for (i = 0; i < BC_LEN; i++) {
            int x = BC_HIST_X + i * (BC_HIST_CELL_W + BC_HIST_GAP);
            char d[2];
            d[0] = (char)('0' + bc_hist[r][i]);
            d[1] = 0;
            fb_stroke_rect(x, y, BC_HIST_CELL_W, BC_HIST_ROW_H, true);
            fb_text(x + (BC_HIST_CELL_W - FONT_W) / 2, y + 2, d, true);
        }
        fb[0] = 'B';
        fb[1] = (char)('0' + bc_bulls[r]);
        fb[2] = ' ';
        fb[3] = 'C';
        fb[4] = (char)('0' + bc_cows[r]);
        fb[5] = 0;
        fb_text(BC_HIST_FB_X, y + 2, fb, true);
        }
    }

    /* 输入区: 光标格黑底白字, 其余白底黑字 */
    for (i = 0; i < BC_LEN; i++) {
        int x = BC_IN_X + i * (BC_CELL_W + BC_CELL_GAP);
        if (!bc_over && i == bc_sel) {
            fb_fill_rect(x, BC_IN_Y, BC_CELL_W, BC_CELL_H, true);
            bc_digit3(x + (BC_CELL_W - FONT_W * 3) / 2,
                      BC_IN_Y + (BC_CELL_H - FONT_H * 3) / 2,
                      bc_cur[i], false);
        } else {
            fb_stroke_rect(x, BC_IN_Y, BC_CELL_W, BC_CELL_H, true);
            bc_digit3(x + (BC_CELL_W - FONT_W * 3) / 2,
                      BC_IN_Y + (BC_CELL_H - FONT_H * 3) / 2,
                      bc_cur[i], true);
        }
    }

    /* 光标白粗边框: 最后画(黑格白边, 四周对称) */
    if (!bc_over) {
        int x = BC_IN_X + bc_sel * (BC_CELL_W + BC_CELL_GAP);
        fb_stroke_rect_thick(x + 2, BC_IN_Y + 2, BC_CELL_W - 4,
                             BC_CELL_H - 4, 2, false);
    }

    /* 结束: HUD 两行 结果+答案 / 操作提示; 墙内不放文字 */
    if (bc_over) {
        char ab[BC_LEN + 1];
        fb_fill_rect(0, 0, CCG_W, CCG_HUD_H - 1, false);
        if (bc_won) fb_text(2, 1, "YOU WIN!", true);
        else        fb_text(2, 1, "YOU FAIL", true);
        fb_text(2, 9, "ANS:", true);
        for (i = 0; i < BC_LEN; i++) ab[i] = (char)('0' + bc_secret[i]);
        ab[BC_LEN] = 0;
        fb_text(2 + text_width("ANS:"), 9, ab, true);
        fb_text(CCG_W - 2 - text_width("OK/N:RETRY BACK:QUIT"), 1,
                "OK/N:RETRY BACK:QUIT", true);
        if (!bc_over_full) {
            bc_over_full = true;
            disp_force_full();
        }
    }
}

void bullscows_on_key(const key_event_t *ev) {
    if (bc_over) {
        if (ev->is_repeat) return;
        if (ev->key == K_OK || (ev->key == K_CHAR && ev->ch == 'n'))
            bullscows_enter();
        else if (ev->key == K_BACK || ev->key == K_QUIT)
            s_exit_request = true;
        return;
    }
    switch (ev->key) {
    case K_LEFT:
        bc_sel = (bc_sel + BC_LEN - 1) % BC_LEN;
        audio_tick();
        break;
    case K_RIGHT:
        bc_sel = (bc_sel + 1) % BC_LEN;
        audio_tick();
        break;
    case K_UP:
        bc_cur[bc_sel] = (bc_cur[bc_sel] + 1) % 10;
        audio_tick();
        break;
    case K_DOWN:
        bc_cur[bc_sel] = (bc_cur[bc_sel] + 9) % 10;
        audio_tick();
        break;
    case K_CHAR:
        if (ev->ch == 'a')      { bc_sel = (bc_sel + BC_LEN - 1) % BC_LEN; audio_tick(); }
        else if (ev->ch == 'd') { bc_sel = (bc_sel + 1) % BC_LEN; audio_tick(); }
        else if (ev->ch == 'w') { bc_cur[bc_sel] = (bc_cur[bc_sel] + 1) % 10; audio_tick(); }
        else if (ev->ch == 's') { bc_cur[bc_sel] = (bc_cur[bc_sel] + 9) % 10; audio_tick(); }
        else if (ev->ch == 'n' && !ev->is_repeat) { audio_select(); bullscows_enter(); }
        break;
    case K_OK:
        if (ev->is_repeat) break;
        bc_submit();
        break;
    case K_BACK:
    case K_PAUSE: {
        pause_sel_t sel;
        if (ev->is_repeat) break;
        if (ui_pause_run(&sel)) {
            if (sel == PAUSE_RESTART) bullscows_enter();
        } else {
            s_exit_request = true;
        }
        break;
    }
    case K_QUIT:
        if (ev->is_repeat) break;
        s_exit_request = true;
        break;
    default:
        break;
    }
}

/* 说明页(供 games_table.c 注册, 不超过 5 行 + NULL):
 * "BULLS & COWS"
 * "GUESS THE 4-DIGIT SECRET"
 * "LT/RT: POSITION  UP/DN: DIGIT"
 * "OK: SUBMIT  N: NEW"
 * "P: PAUSE  Q: QUIT"
 * NULL */
