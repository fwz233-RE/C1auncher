/* FLAG QUIZ — 国旗猜谜: 看 12x8 简化国旗图案, 4 选 1 国家名
 * 题库 24 组(静态 const, 1bit 图案用 #/. 字符串定义, '#'=黑):
 *   三色条(横/纵)、十字(Nordic/正/斜)、星、新月、菱形、徽章块等, 两两图案互异
 * 玩法: 方向键 2x2 网格选选项, OK 确认; 对 +10, 错显示正确答案;
 *       N 跳过本题; 24 题答完进入结算
 * HUD 顶栏: 左 FLAGS, 右 Q n/24 SCORE s; 结束: 左上结果 + 右上
 *       OK/N:RETRY BACK:QUIT, 墙内 FINAL SCORE 大字
 * 静态前缀 fq_; 像素坐标一律 int; 零 malloc; 全部按键忽略重复
 *
 * 集成提示(help[] 最多 5 行, 由 games_table 提供):
 *   "FLAG QUIZ", "NAME THE COUNTRY FROM THE FLAG",
 *   "ARROWS: CHOOSE  OK: CONFIRM", "RIGHT +10, WRONG SHOWS ANSWER",
 *   "N: SKIP", NULL
 */
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
#include <stdio.h>
#include <string.h>

#define FQ_N        24          /* 题库国家数 */
#define FQ_OPTS     4           /* 每题目选项数 */

/* ---- 布局(游戏区 y>=16) ---- */
#define FQ_SCALE    10          /* 图案放大倍率 */
#define FQ_FLAG_W   12
#define FQ_FLAG_H   8
#define FQ_FLAG_X   ((int)((CCG_W - FQ_FLAG_W * FQ_SCALE) / 2))   /* 88 */
#define FQ_FLAG_Y   18
#define FQ_FB_Y     101         /* 反馈/提示行 */
#define FQ_OPT_W    130
#define FQ_OPT_H    14
#define FQ_OPT_X0   12
#define FQ_OPT_Y0   110
#define FQ_OPT_DX   (FQ_OPT_W + 12)   /* 两列间距 */
#define FQ_OPT_DY   (FQ_OPT_H + 12)   /* 两行间距 */

/* ---- 题库: 每面旗 = 8 行 x 12 列, '#' 黑 '.' 白 ---- */
static const char *const fq_flag_pix[FQ_N] = {
    /* 0 JAPAN: 白底中央日轮 */
    "............\n"
    "............\n"
    "....####....\n"
    "....####....\n"
    "....####....\n"
    "....####....\n"
    "............\n"
    "............",
    /* 1 SWITZERLAND: 白底中央短十字 */
    "............\n"
    ".....##.....\n"
    ".....##.....\n"
    "...######...\n"
    "...######...\n"
    ".....##.....\n"
    ".....##.....\n"
    "............",
    /* 2 UK: 正十字 + 双斜十字 */
    "##...##...##\n"
    ".##..##..##.\n"
    "..##.##.##..\n"
    "############\n"
    "############\n"
    ".....##.....\n"
    "....####....\n"
    "...######...",
    /* 3 NORWAY: 左侧北欧十字(通高) */
    "....##......\n"
    "....##......\n"
    "....##......\n"
    "############\n"
    "############\n"
    "....##......\n"
    "....##......\n"
    "....##......",
    /* 4 FRANCE: 竖三色 */
    "####....####\n"
    "####....####\n"
    "####....####\n"
    "####....####\n"
    "####....####\n"
    "####....####\n"
    "####....####\n"
    "####....####",
    /* 5 GERMANY: 横三色 */
    "############\n"
    "############\n"
    "############\n"
    "............\n"
    "............\n"
    "############\n"
    "############\n"
    "############",
    /* 6 THAILAND: 五横条 */
    "############\n"
    "############\n"
    "............\n"
    "############\n"
    "############\n"
    "............\n"
    "############\n"
    "############",
    /* 7 POLAND: 上白下红(两横条) */
    "............\n"
    "............\n"
    "............\n"
    "............\n"
    "############\n"
    "############\n"
    "############\n"
    "############",
    /* 8 IRELAND: 竖绿白橙(左右白中黑) */
    "....####....\n"
    "....####....\n"
    "....####....\n"
    "....####....\n"
    "....####....\n"
    "....####....\n"
    "....####....\n"
    "....####....",
    /* 9 ESTONIA: 横白黑蓝(上下白中黑) */
    "............\n"
    "............\n"
    "............\n"
    "############\n"
    "############\n"
    "............\n"
    "............\n"
    "............",
    /* 10 USA: 左上蓝底块 + 红白横条 */
    "#####.......\n"
    "#####.......\n"
    "#####.......\n"
    "#####.......\n"
    "############\n"
    "............\n"
    "############\n"
    "............",
    /* 11 GREECE: 蓝白横条 + 左上十字块 */
    "############\n"
    "#####.......\n"
    "############\n"
    "#####.......\n"
    "############\n"
    "............\n"
    "############\n"
    "............",
    /* 12 CANADA: 左右红条 + 中央枫叶 */
    "##........##\n"
    "####..##..##\n"
    "######..####\n"
    "############\n"
    "############\n"
    "##.######.##\n"
    "##..####..##\n"
    "##...##...##",
    /* 13 ISRAEL: 上下蓝条 + 中央六芒星 */
    "############\n"
    "############\n"
    "....####....\n"
    "...##..##...\n"
    "...##..##...\n"
    "....####....\n"
    "############\n"
    "############",
    /* 14 TURKEY: 白底左弯月(开口朝右) */
    "............\n"
    "............\n"
    "....####....\n"
    "...#....#...\n"
    "...#........\n"
    "...#....#...\n"
    "....####....\n"
    "............",
    /* 15 VIETNAM: 红底中央黄星 */
    "############\n"
    "############\n"
    "######.#####\n"
    "#####...####\n"
    "####.....###\n"
    "#####...####\n"
    "######.#####\n"
    "############",
    /* 16 CHINA: 红底左上大星 + 四点小星 */
    "###.########\n"
    "##...#######\n"
    "#.....######\n"
    "##...##..###\n"
    "###.#####..#\n"
    "######.##..#\n"
    "######....##\n"
    "########..##",
    /* 17 BRAZIL: 绿底中央黄菱形 */
    "############\n"
    "####..######\n"
    "####....####\n"
    "###......###\n"
    "###......###\n"
    "####....####\n"
    "####..######\n"
    "############",
    /* 18 SAUDI ARABIA: 绿底白字横带 */
    "############\n"
    "############\n"
    "############\n"
    "############\n"
    "############\n"
    "............\n"
    "............\n"
    "############",
    /* 19 AUSTRALIA: 蓝底左上米字块 + 右下星 */
    "##..##......\n"
    "##..##......\n"
    "######......\n"
    "##..##......\n"
    "##..##......\n"
    "##########.#\n"
    "#########...\n"
    "##########.#",
    /* 20 BANGLADESH: 绿底左中红日 */
    "############\n"
    "############\n"
    "##....######\n"
    "##....######\n"
    "##....######\n"
    "##....######\n"
    "############\n"
    "############",
    /* 21 MADAGASCAR: 左白竖带 + 右红 */
    "....########\n"
    "....########\n"
    "....########\n"
    "....########\n"
    "....########\n"
    "....########\n"
    "....########\n"
    "....########",
    /* 22 SUDAN: 红白黑横条 + 左绿三角 */
    "############\n"
    "############\n"
    "############\n"
    "####........\n"
    "####........\n"
    "############\n"
    "############\n"
    "############",
    /* 23 BOTSWANA: 天蓝底黑条白线(中带) */
    "............\n"
    "............\n"
    "............\n"
    "############\n"
    "............\n"
    "############\n"
    "............\n"
    "............",
};

static const char *const fq_names[FQ_N] = {
    "JAPAN", "SWITZERLAND", "UK", "NORWAY", "FRANCE", "GERMANY",
    "THAILAND", "POLAND", "IRELAND", "ESTONIA", "USA", "GREECE",
    "CANADA", "ISRAEL", "TURKEY", "VIETNAM", "CHINA", "BRAZIL",
    "SAUDI ARABIA", "AUSTRALIA", "BANGLADESH", "MADAGASCAR", "SUDAN",
    "BOTSWANA",
};

/* ---- 对局状态 ---- */
typedef enum {
    FQ_ST_PLAY,     /* 选择答案 */
    FQ_ST_FEED,     /* 判定反馈(OK/N 下一题) */
    FQ_ST_OVER      /* 24 题结束 */
} fq_state_t;

static fq_state_t fq_state;
static int fq_order[FQ_N];      /* 出题顺序(洗牌) */
static int fq_qi;               /* 当前题下标 0..FQ_N-1 */
static int fq_opts[FQ_OPTS];    /* 4 个选项(国家下标) */
static int fq_sel;              /* 光标 0..3 */
static int fq_score;
static int fq_right;
static bool fq_feed_ok;         /* 上一题是否答对 */
static int fq_correct;          /* 上一题正确答案下标 */
static bool fq_over_full;       /* 结束全刷只做一次 */
static uint32_t fq_gens;        /* 换局计数(种子混合) */
static rng_t fq_rng;

void flagquiz_render(void);

/* 图案字符串绘制: '#' 画 scale 见方黑块, '\n' 换行 */
static void fq_draw_flag(int x, int y, int idx, int scale) {
    const char *p = fq_flag_pix[idx];
    int cx = 0, cy = 0;
    for (; *p && cy < FQ_FLAG_H; p++) {
        char c = *p;
        if (c == '\n') {
            cx = 0;
            cy++;
        } else if (cx < FQ_FLAG_W) {
            if (c == '#')
                fb_fill_rect(x + cx * scale, y + cy * scale, scale, scale, true);
            cx++;
        }
    }
}

/* 5x7 字形 3x 放大(每字符 15x21) */
static void fq_text3(int x, int y, const char *s, bool black) {
    for (const char *p = s; *p; p++) {
        const uint8_t *g = font_glyph5x7[(unsigned char)*p];
        for (int j = 0; j < FONT_H; j++)
            for (int i = 0; i < FONT_W; i++)
                if (g[j] & (1u << i))
                    fb_fill_rect(x + i * 3, y + j * 3, 3, 3, black);
        x += FONT_ADV * 3;
    }
}

static int fq_text3_center(int y, const char *s, bool black) {
    int w = ((int)strlen(s) * FONT_ADV - 1) * 3;
    int x = (CCG_W - w) / 2;
    if (x < 0) x = 0;
    fq_text3(x, y, s, black);
    return x;
}

/* 选项 2x2 网格坐标 */
static int fq_opt_x(int i) { return FQ_OPT_X0 + (i & 1) * FQ_OPT_DX; }
static int fq_opt_y(int i) { return FQ_OPT_Y0 + (i >> 1) * FQ_OPT_DY; }

/* 生成 4 选项: 正确答案 + 3 个不重复干扰项, 再洗牌落位 */
static void fq_pick_opts(void) {
    fq_opts[0] = fq_order[fq_qi];
    {
        int n = 1, guard = 0, c;
        while (n < FQ_OPTS && guard < 64) {
            guard++;
            c = (int)rng_range(&fq_rng, (uint32_t)FQ_N);
            bool dup = false;
            int i;
            for (i = 0; i < n; i++)
                if (fq_opts[i] == c) { dup = true; break; }
            if (!dup) fq_opts[n++] = c;
        }
        /* 兜底顺序补全(随机抽取耗尽后, 几乎不会触发) */
        for (c = 0; c < FQ_N && n < FQ_OPTS; c++) {
            bool dup = false;
            int i;
            for (i = 0; i < n; i++)
                if (fq_opts[i] == c) { dup = true; break; }
            if (!dup) fq_opts[n++] = c;
        }
    }
    /* 选项洗牌 */
    for (int i = FQ_OPTS - 1; i > 0; i--) {
        int j = (int)rng_range(&fq_rng, (uint32_t)(i + 1));
        int t = fq_opts[i];
        fq_opts[i] = fq_opts[j];
        fq_opts[j] = t;
    }
    fq_sel = 0;
}

/* 洗牌出题顺序(Fisher-Yates) */
static void fq_shuffle(void) {
    for (int i = FQ_N - 1; i > 0; i--) {
        int j = (int)rng_range(&fq_rng, (uint32_t)(i + 1));
        int t = fq_order[i];
        fq_order[i] = fq_order[j];
        fq_order[j] = t;
    }
}

static void fq_next_q(void) {
    fq_qi++;
    if (fq_qi >= FQ_N) {
        fq_state = FQ_ST_OVER;
        audio_win();                   /* 24 题完成 */
        led_fx_set(LED_FX_WIN);
        return;
    }
    fq_pick_opts();
    fq_state = FQ_ST_PLAY;
}

static void fq_submit(void) {
    fq_correct = fq_order[fq_qi];
    fq_feed_ok = (fq_opts[fq_sel] == fq_correct);
    if (fq_feed_ok) {
        fq_score += 10;
        fq_right++;
        audio_clear();                 /* 答对 */
    } else {
        audio_error();                 /* 答错 */
    }
    fq_state = FQ_ST_FEED;
}

static void fq_new_game(void) {
    fq_gens++;
    rng_seed(&fq_rng, now_ms() ^ ((uint64_t)fq_gens * 0x9E3779B1u));
    for (int i = 0; i < FQ_N; i++) fq_order[i] = i;
    fq_shuffle();
    fq_qi = 0;
    fq_score = 0;
    fq_right = 0;
    fq_over_full = false;
    fq_pick_opts();
    fq_state = FQ_ST_PLAY;
    flagquiz_render();
    disp_full();
}

void flagquiz_enter(void) { fq_new_game(); }
void flagquiz_exit(void) {}
void flagquiz_tick(uint64_t now) { (void)now; }

/* ---- 渲染 ---- */
void flagquiz_render(void) {
    fb_clear(false);
    char buf[40];

    if (fq_state == FQ_ST_OVER) {
        /* HUD 两行: 左上结果 + 右上按键提示; 墙内只放最终成绩 */
        fb_fill_rect(0, 0, CCG_W, CCG_HUD_H - 1, false);
        snprintf(buf, sizeof(buf), "SCORE %d", fq_score);
        fb_text(2, 2, buf, true);
        snprintf(buf, sizeof(buf), "RIGHT %d/%d", fq_right, FQ_N);
        fb_text(2, 9, buf, true);
        fb_text(CCG_W - 2 - text_width("OK/N:RETRY BACK:QUIT"), 2,
                "OK/N:RETRY BACK:QUIT", true);
        fq_text3_center(40, "FINAL SCORE", true);
        snprintf(buf, sizeof(buf), "%d", fq_score);
        fq_text3_center(74, buf, true);
        if (!fq_over_full) { fq_over_full = true; disp_force_full(); }
        return;
    }

    /* HUD 顶栏: 左 FLAGS, 右 Q n/24 SCORE s */
    fb_text(0, 0, "FLAGS", true);
    snprintf(buf, sizeof(buf), "Q %d/%d SCORE %d", fq_qi + 1, FQ_N, fq_score);
    fb_text(CCG_W - 4 - text_width(buf), 0, buf, true);
    fb_hline(0, CCG_HUD_H - 1, CCG_W, true);

    /* 国旗图案 + 外框 */
    fq_draw_flag(FQ_FLAG_X, FQ_FLAG_Y, fq_order[fq_qi], FQ_SCALE);
    fb_stroke_rect(FQ_FLAG_X - 2, FQ_FLAG_Y - 2,
                   FQ_FLAG_W * FQ_SCALE + 4, FQ_FLAG_H * FQ_SCALE + 4, true);

    /* 反馈/提示行 */
    if (fq_state == FQ_ST_FEED) {
        if (fq_feed_ok)
            snprintf(buf, sizeof(buf), "RIGHT +10  OK/N:NEXT");
        else
            snprintf(buf, sizeof(buf), "WRONG: %s  OK/N:NEXT",
                     fq_names[fq_correct]);
    } else {
        snprintf(buf, sizeof(buf), "ARROWS: CHOOSE  OK:GO  N:SKIP");
    }
    fb_text_center(FQ_FB_Y, buf, true);

    /* 选项 2x2(先全部绘制, 最后画光标反白格) */
    for (int i = 0; i < FQ_OPTS; i++) {
        int x = fq_opt_x(i), y = fq_opt_y(i);
        /* 判定中: 答错时反白正确项, 答对保持选中反白 */
        bool inv = (fq_state == FQ_ST_FEED && !fq_feed_ok)
                       ? (fq_opts[i] == fq_correct)
                       : (i == fq_sel);
        fb_stroke_rect(x, y, FQ_OPT_W, FQ_OPT_H, true);
        if (inv) fb_fill_rect(x, y, FQ_OPT_W, FQ_OPT_H, true);
        int tx = x + (FQ_OPT_W - text_width(fq_names[fq_opts[i]])) / 2;
        fb_text(tx, y + 4, fq_names[fq_opts[i]], !inv);
    }
}

/* ---- 输入 ---- */
void flagquiz_on_key(const key_event_t *ev) {
    if (ev->is_repeat) return;             /* 全部按键忽略重复 */

    if (fq_state == FQ_ST_OVER) {
        if (ev->key == K_OK || (ev->key == K_CHAR && ev->ch == 'n'))
            fq_new_game();
        else if (ev->key == K_BACK || ev->key == K_QUIT)
            s_exit_request = true;
        return;
    }

    if (fq_state == FQ_ST_FEED) {
        if (ev->key == K_OK || (ev->key == K_CHAR && ev->ch == 'n'))
            fq_next_q();
        else if (ev->key == K_BACK || ev->key == K_QUIT)
            s_exit_request = true;
        return;
    }

    /* PLAY: 答题中 */
    if (ev->key == K_CHAR) {
        if (ev->ch == 'n') {               /* N: 跳过本题 */
            audio_move();
            fq_next_q();
        } else if (ev->ch == 'q') {
            s_exit_request = true;
        }
        return;
    }
    switch (ev->key) {
    case K_UP:    fq_sel ^= 2; audio_tick(); break;      /* 2x2 网格: 上下换行 */
    case K_DOWN:  fq_sel ^= 2; audio_tick(); break;
    case K_LEFT:  fq_sel ^= 1; audio_tick(); break;      /* 左右换列 */
    case K_RIGHT: fq_sel ^= 1; audio_tick(); break;
    case K_OK:
        audio_select();
        fq_submit();
        break;
    case K_BACK:
    case K_PAUSE: {
        pause_sel_t sel;
        if (ui_pause_run(&sel)) {
            if (sel == PAUSE_RESTART) fq_new_game();
        } else {
            s_exit_request = true;
        }
        break;
    }
    case K_QUIT:
        s_exit_request = true;
        break;
    default:
        break;
    }
}
