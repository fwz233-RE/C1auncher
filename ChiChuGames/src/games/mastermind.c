/* MASTERMIND — 猜色球破译 4 位密码
 * 6 种 CG 符号代替颜色(1bit 屏): ■ ○ ★ ♥ ◆ ♠
 * 光标选位(左右) + 换符号(上下/W/S) + OK 提交 → 反馈点(实心=位置对,
 * 空心=符号对位置错, 先实心后空心); 10 次未中判负
 * 布局: 中央 4 位输入区(40x28) + 下方 10 行历史(4 符号 + 4 反馈点)
 *       + 右侧纵向候选符号条(当前格符号反白高亮)
 * 键位: 左右/A/D 选位, 上下/W/S 循环符号, OK 提交, N 新局,
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

#define MM_ATTEMPTS 10          /* 最多尝试次数 */
#define MM_LEN 4                /* 密码长度 */
#define MM_PALETTE 6            /* 符号种数 */

/* 输入区: 4 格 40x28, 间隙 4 */
#define MM_CELL_W 40
#define MM_CELL_H 28
#define MM_CELL_GAP 4
#define MM_GRID_W (MM_LEN * MM_CELL_W + (MM_LEN - 1) * MM_CELL_GAP)  /* 172 */
#define MM_IN_X 44
#define MM_IN_Y CCG_HUD_H       /* 16 */

/* 历史区: 10 行, 每行 4 个小符号格 + 4 反馈点(3x3) */
#define MM_HIST_X 93
#define MM_HIST_Y 50
#define MM_HIST_ROW_H 9
#define MM_HIST_CELL 12
#define MM_HIST_GAP 2
#define MM_HIST_W (MM_LEN * MM_HIST_CELL + (MM_LEN - 1) * MM_HIST_GAP) /* 54 */
#define MM_DOT 3

/* 右侧候选符号条 */
#define MM_SIDE_X 256
#define MM_SIDE_W 37            /* 256..293 */
#define MM_SIDE_SYM_X (MM_SIDE_X + 11)
#define MM_SIDE_SYM_Y 24
#define MM_SIDE_STEP 17

/* 候选符号表: CG 符号(●▲ 无对应字形, 以 ○/♠ 近似) */
static const int mm_cg[MM_PALETTE] = {
    CG_SQUARE_FILL, CG_RING, CG_STAR, CG_HEART, CG_DIAMOND, CG_SPADE
};

static uint8_t mm_history[MM_ATTEMPTS][MM_LEN];  /* 已提交行(符号索引) */
static uint8_t mm_fb[MM_ATTEMPTS][MM_LEN];       /* 反馈: 0=无 1=实心 2=空心 */
static int    mm_cur[MM_LEN];                    /* 当前行符号索引 0..5 */
static int    mm_secret[MM_LEN];                 /* 谜底 */
static int    mm_sel;                            /* 光标位 0..3 */
static int    mm_rows;                           /* 已提交行数 */
static bool   mm_over;                           /* 游戏结束 */
static bool   mm_won;
static bool   mm_over_full;                      /* 结束全刷防重复 */
static rng_t  mm_rng;
static uint64_t mm_seed_cnt;                     /* 换局换种子 */

void mastermind_render(void);

/* 2x 放大符号: 10x14, 落在 40x28 格内居中 */
static void mm_sym2(int x, int y, int pi, bool black) {
    const uint8_t *g = font_symbols[mm_cg[pi]];
    int i, j;
    for (j = 0; j < FONT_H; j++)
        for (i = 0; i < FONT_W; i++)
            if (g[j] & (1u << i))
                fb_fill_rect(x + i * 2, y + j * 2, 2, 2, black);
}

/* 反馈: 先数位置对(实心 1), 再按剩余名额数符号对(空心 2), 其余 0 */
static void mm_score_row(void) {
    int avail[MM_PALETTE];
    int i, n = 0;
    for (i = 0; i < MM_PALETTE; i++) avail[i] = 0;
    for (i = 0; i < MM_LEN; i++) avail[mm_secret[i]]++;
    for (i = 0; i < MM_LEN; i++) {
        if (mm_cur[i] == mm_secret[i]) {
            mm_fb[mm_rows][n++] = 1;
            avail[mm_cur[i]]--;
        }
    }
    for (i = 0; i < MM_LEN; i++) {
        if (mm_cur[i] == mm_secret[i]) continue;
        if (avail[mm_cur[i]] > 0) {
            mm_fb[mm_rows][n++] = 2;
            avail[mm_cur[i]]--;
        }
    }
    while (n < MM_LEN) mm_fb[mm_rows][n++] = 0;
}

/* 提交当前行 */
static void mm_submit(void) {
    int i;
    bool win = true;
    for (i = 0; i < MM_LEN; i++) {
        mm_history[mm_rows][i] = (uint8_t)mm_cur[i];
        if (mm_cur[i] != mm_secret[i]) win = false;
    }
    mm_score_row();
    mm_rows++;
    if (win) {
        mm_over = true;
        mm_won = true;
        audio_win();                        /* 破译成功 */
        led_fx_set(LED_FX_WIN);
    } else if (mm_rows >= MM_ATTEMPTS) {
        mm_over = true;
        mm_won = false;
        audio_lose();                       /* 次数用尽 */
        led_fx_set(LED_FX_LOSE);
    } else {
        audio_error();                      /* 猜错, 反馈点提示 */
    }
}

/* 随机谜底: rng_range [0,6) 无拒绝死循环风险, 种子恒非 0 */
static void mm_gen_secret(void) {
    int i;
    for (i = 0; i < MM_LEN; i++)
        mm_secret[i] = (int)rng_range(&mm_rng, MM_PALETTE);
}

void mastermind_enter(void) {
    int r, c;
    for (r = 0; r < MM_ATTEMPTS; r++)
        for (c = 0; c < MM_LEN; c++) {
            mm_history[r][c] = 0;
            mm_fb[r][c] = 0;
        }
    for (c = 0; c < MM_LEN; c++) mm_cur[c] = 0;
    mm_sel = 0;
    mm_rows = 0;
    mm_over = false;
    mm_won = false;
    mm_over_full = false;
    mm_seed_cnt++;
    rng_seed(&mm_rng, (uint64_t)now_ms() ^ ((uint64_t)mm_seed_cnt << 32)
                       ^ 0x4D4DULL);
    mm_gen_secret();
    mastermind_render();
    disp_full();
}

void mastermind_exit(void) {}

void mastermind_tick(uint64_t now) { (void)now; }

/* HUD: 左标题, 右 TRY n/10 */
static void mm_draw_hud(void) {
    char buf[10];
    int n = mm_rows + 1;
    if (n > MM_ATTEMPTS) n = MM_ATTEMPTS;
    buf[0] = 'T'; buf[1] = 'R'; buf[2] = 'Y'; buf[3] = ' ';
    buf[4] = (char)('0' + n / 10);
    buf[5] = (char)('0' + n % 10);
    buf[6] = '/'; buf[7] = '1'; buf[8] = '0'; buf[9] = 0;
    fb_text_scale2(CCG_W - 2 - text_width(buf) * 2, 1, buf, true);
}

void mastermind_render(void) {
    int r, i;
    fb_clear(false);

    /* HUD 顶栏 */
    fb_text_scale2(2, 1, "MASTERMIND", true);
    if (!mm_over) mm_draw_hud();
    fb_hline(0, CCG_HUD_H - 1, CCG_W, true);

    /* 中央输入区: 光标格黑底白符, 其余白底黑符 */
    for (i = 0; i < MM_LEN; i++) {
        int x = MM_IN_X + i * (MM_CELL_W + MM_CELL_GAP);
        if (i == mm_sel && !mm_over) {
            fb_fill_rect(x, MM_IN_Y, MM_CELL_W, MM_CELL_H, true);
            mm_sym2(x + 15, MM_IN_Y + 7, mm_cur[i], false);
        } else {
            fb_stroke_rect(x, MM_IN_Y, MM_CELL_W, MM_CELL_H, true);
            mm_sym2(x + 15, MM_IN_Y + 7, mm_cur[i], true);
        }
    }

    /* 历史行: 4 符号格 + 4 反馈点(先实心后空心) */
    for (r = 0; r < mm_rows; r++) {
        int y = MM_HIST_Y + r * MM_HIST_ROW_H;
        for (i = 0; i < MM_LEN; i++) {
            int x = MM_HIST_X + i * (MM_HIST_CELL + MM_HIST_GAP);
            fb_stroke_rect(x, y, MM_HIST_CELL, MM_HIST_ROW_H, true);
            fb_symbol(x + 3, y + 1, mm_cg[mm_history[r][i]], true);
        }
        for (i = 0; i < MM_LEN; i++) {
            int x = MM_HIST_X + MM_HIST_W + 4 + i * (MM_DOT + 1);
            if (mm_fb[r][i] == 1)
                fb_fill_rect(x, y + 3, MM_DOT, MM_DOT, true);
            else if (mm_fb[r][i] == 2)
                fb_stroke_rect(x, y + 3, MM_DOT, MM_DOT, true);
        }
    }

    /* 右侧候选符号条: 小字标签 + 6 符号, 当前格符号反白高亮 */
    fb_text(MM_SIDE_X + (MM_SIDE_W - text_width("COLOR")) / 2,
            MM_IN_Y + 1, "COLOR", true);
    for (i = 0; i < MM_PALETTE; i++) {
        int y = MM_SIDE_SYM_Y + i * MM_SIDE_STEP;
        if (!mm_over && i == mm_cur[mm_sel]) {
            fb_fill_rect(MM_SIDE_X + 9, y - 2, 14, 18, true);
            mm_sym2(MM_SIDE_SYM_X, y, i, false);
        } else {
            mm_sym2(MM_SIDE_SYM_X, y, i, true);
        }
    }

    /* 光标边框: 最后画(黑格白边, 四周对称) */
    if (!mm_over) {
        int x = MM_IN_X + mm_sel * (MM_CELL_W + MM_CELL_GAP);
        fb_stroke_rect_thick(x + 2, MM_IN_Y + 2, MM_CELL_W - 4,
                             MM_CELL_H - 4, 2, false);
    }

    /* 结束: HUD 两行 结果+答案 / 操作提示; 墙内不放文字 */
    if (mm_over) {
        fb_fill_rect(0, 0, CCG_W, CCG_HUD_H - 1, false);
        if (mm_won) fb_text(2, 1, "YOU WIN!", true);
        else        fb_text(2, 1, "YOU FAIL", true);
        fb_text(2, 9, "ANS:", true);
        for (i = 0; i < MM_LEN; i++)
            fb_symbol(2 + text_width("ANS:") + i * 6, 9,
                      mm_cg[mm_secret[i]], true);
        fb_text(CCG_W - 2 - text_width("OK/N:RETRY BACK:QUIT"), 1,
                "OK/N:RETRY BACK:QUIT", true);
        if (!mm_over_full) {
            mm_over_full = true;
            disp_force_full();
        }
    }
}

void mastermind_on_key(const key_event_t *ev) {
    if (mm_over) {
        if (ev->is_repeat) return;
        if (ev->key == K_OK || (ev->key == K_CHAR && ev->ch == 'n'))
            mastermind_enter();
        else if (ev->key == K_BACK || ev->key == K_QUIT)
            s_exit_request = true;
        return;
    }
    switch (ev->key) {
    case K_LEFT:
    case K_RIGHT:
    case K_UP:
    case K_DOWN:
        if (ev->key == K_LEFT)       mm_sel = (mm_sel + MM_LEN - 1) % MM_LEN;
        else if (ev->key == K_RIGHT) mm_sel = (mm_sel + 1) % MM_LEN;
        else if (ev->key == K_UP)    mm_cur[mm_sel] =
            (mm_cur[mm_sel] + 1) % MM_PALETTE;
        else                         mm_cur[mm_sel] =
            (mm_cur[mm_sel] + MM_PALETTE - 1) % MM_PALETTE;
        if (!ev->is_repeat) audio_move();   /* 光标/符号轮换(跳过重复) */
        break;
    case K_CHAR:
        if (ev->is_repeat) break;   /* 字母重复一律忽略 */
        if (ev->ch == 'a') { mm_sel = (mm_sel + MM_LEN - 1) % MM_LEN; audio_move(); }
        else if (ev->ch == 'd') { mm_sel = (mm_sel + 1) % MM_LEN; audio_move(); }
        else if (ev->ch == 'w') {
            mm_cur[mm_sel] = (mm_cur[mm_sel] + 1) % MM_PALETTE;
            audio_move();
        }
        else if (ev->ch == 's') {
            mm_cur[mm_sel] = (mm_cur[mm_sel] + MM_PALETTE - 1) % MM_PALETTE;
            audio_move();
        }
        else if (ev->ch == 'n')  mastermind_enter();
        break;
    case K_OK:
        if (ev->is_repeat) break;
        mm_submit();
        break;
    case K_BACK:
    case K_PAUSE: {
        pause_sel_t sel;
        if (ev->is_repeat) break;
        if (ui_pause_run(&sel)) {
            if (sel == PAUSE_RESTART) mastermind_enter();
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
 * "Break the 4-color code"
 * "L/R select, U/D cycle color"
 * "OK submit, N new game"
 * "P pause, Q quit menu"
 * NULL */
