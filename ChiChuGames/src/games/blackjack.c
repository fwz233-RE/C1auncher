/* BLACKJACK — 21 点: 玩家 vs 庄家
 * 一副 52 张 Fisher-Yates 洗牌(rng); A=1/11, 10/J/Q/K=10, 2-9 面值
 * OK=HIT  BACK/SPACE=STAND  N=新局  P=暂停; 庄家 17 停; W/L/T 记分 */
#include "game.h"
#include "../config.h"
#include "../gfx/canvas.h"
#include "../gfx/font.h"
#include "../gfx/pattern.h"
#include "../platform/audio.h"
#include "../platform/display.h"
#include "../platform/input.h"
#include "../platform/led.h"
#include "../platform/time.h"
#include "../rng.h"

#define BJ_MAX_HAND 12          /* 手牌上限(两方合计最多 24 张, 不会中途重洗) */
#define BJ_SHOWN 8              /* 单行最多画 8 张, 超出画 +N */
#define BJ_CARD_W 24
#define BJ_CARD_H 32
#define BJ_STEP 26
#define BJ_X0 6
#define BJ_ROW_D 25             /* 庄家牌行 y */
#define BJ_ROW_P 67             /* 玩家牌行 y */
#define BJ_SEP_Y 101            /* 状态区分隔线 */

typedef enum {
    BJ_PH_PLAY = 0,             /* 玩家回合 */
    BJ_PH_DEALER,               /* 庄家逐张抽(tick 驱动, 800ms/张) */
    BJ_PH_OVER                  /* 本局结束 */
} bj_phase_t;

typedef enum {
    BJ_RES_WIN, BJ_RES_LOSE, BJ_RES_PUSH,
    BJ_RES_PBUST, BJ_RES_DBUST, BJ_RES_BJ, BJ_RES_DBJ
} bj_res_t;

static int8_t bj_deck[52];              /* 牌 id 0..51: 花色=id/13, 点数=id%13 */
static int bj_pos;                      /* 下一张位置 */
static rng_t bj_rng;

static int8_t bj_hand[2][BJ_MAX_HAND];  /* [0]=玩家 [1]=庄家 */
static int bj_n[2];
static bj_phase_t bj_phase;
static bj_res_t bj_res;
static bool bj_hole;                    /* 庄家第 2 张暗牌未揭 */
static uint32_t bj_win, bj_loss, bj_ties;

void blackjack_render(void);

/* ---- 牌值: A=11(软 A 打折), J/Q/K=10, 2-9 面值 ---- */
static int bj_rank_value(int rank) {
    if (rank == 0) return 11;           /* A */
    if (rank >= 10) return 10;          /* J Q K */
    return rank + 1;
}

static int bj_total(const int8_t *h, int n) {
    int sum = 0, aces = 0;
    for (int i = 0; i < n; i++) {
        int r = h[i] % 13;
        if (r == 0) aces++;
        sum += bj_rank_value(r);
    }
    while (sum > 21 && aces > 0) { sum -= 10; aces--; }
    return sum;
}

/* ---- 牌组: Fisher-Yates + 顺序发牌 ---- */
static void bj_shuffle(void) {
    for (int i = 0; i < 52; i++) bj_deck[i] = (int8_t)i;
    for (int i = 51; i > 0; i--) {
        uint32_t j = rng_range(&bj_rng, (uint32_t)i + 1);
        int8_t t = bj_deck[i];
        bj_deck[i] = bj_deck[j];
        bj_deck[j] = t;
    }
    bj_pos = 0;
}

static int bj_draw(void) {
    if (bj_pos >= 52) bj_shuffle();     /* 用尽重洗 */
    return bj_deck[bj_pos++];
}

/* ---- 结算(爆牌/比点/21), 更新 W/L/T ---- */
static void bj_finish(void) {
    bj_phase = BJ_PH_OVER;
    int pt = bj_total(bj_hand[0], bj_n[0]);
    int dt = bj_total(bj_hand[1], bj_n[1]);
    if (pt > 21) { bj_loss++; bj_res = BJ_RES_PBUST; audio_lose(); led_fx_set(LED_FX_LOSE); }
    else if (dt > 21) { bj_win++; bj_res = BJ_RES_DBUST; audio_win(); led_fx_set(LED_FX_WIN); }
    else if (pt > dt) { bj_win++; bj_res = BJ_RES_WIN; audio_win(); led_fx_set(LED_FX_WIN); }
    else if (pt < dt) { bj_loss++; bj_res = BJ_RES_LOSE; audio_lose(); led_fx_set(LED_FX_LOSE); }
    else { bj_ties++; bj_res = BJ_RES_PUSH; audio_move(); }
}

/* ---- 发新一局: 各 2 张, 处理自然 21 ---- */
static void bj_deal(void) {
    bj_n[0] = 0;
    bj_n[1] = 0;
    bj_hand[0][bj_n[0]++] = (int8_t)bj_draw();
    bj_hand[1][bj_n[1]++] = (int8_t)bj_draw();
    bj_hand[0][bj_n[0]++] = (int8_t)bj_draw();
    bj_hand[1][bj_n[1]++] = (int8_t)bj_draw();
    bj_phase = BJ_PH_PLAY;
    bj_hole = true;
    int pt = bj_total(bj_hand[0], bj_n[0]);
    int dt = bj_total(bj_hand[1], bj_n[1]);
    if (pt == 21 || dt == 21) {
        bj_hole = false;                /* 揭暗牌判黑杰克 */
        if (pt == 21 && dt == 21) { bj_ties++; bj_res = BJ_RES_PUSH; audio_move(); }
        else if (pt == 21) { bj_win++; bj_res = BJ_RES_BJ; audio_win(); led_fx_set(LED_FX_WIN); }
        else { bj_loss++; bj_res = BJ_RES_DBJ; audio_lose(); led_fx_set(LED_FX_LOSE); }
        bj_phase = BJ_PH_OVER;
    }
}

static void bj_hit(void) {
    if (bj_phase != BJ_PH_PLAY || bj_n[0] >= BJ_MAX_HAND) return;
    bj_hand[0][bj_n[0]++] = (int8_t)bj_draw();
    if (bj_total(bj_hand[0], bj_n[0]) > 21) bj_finish();
    else audio_move();                  /* 抽牌 */
}

static void bj_stand(void) {
    if (bj_phase != BJ_PH_PLAY) return;
    bj_hole = false;                    /* 揭暗牌 */
    bj_phase = BJ_PH_DEALER;            /* 庄家回合(逐张见 tick) */
    audio_select();                     /* 停牌 */
}

/* ---- 渲染工具 ---- */
static void bj_num(char *buf, uint32_t v) {
    char tmp[12];
    int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v > 0 && n < 11) { tmp[n++] = (char)('0' + (int)(v % 10)); v /= 10; }
    for (int i = 0; i < n; i++) buf[i] = tmp[n - 1 - i];
    buf[n] = 0;
}

/* 2x 放大 ASCII(5x7 -> 10x14, 字距 2px) */
static void bj_text2x(int x, int y, const char *s) {
    extern const uint8_t font_glyph5x7[256][7];
    for (int i = 0; s[i]; i++) {
        const uint8_t *g = font_glyph5x7[(unsigned char)s[i]];
        for (int j = 0; j < FONT_H; j++)
            for (int k = 0; k < FONT_W; k++)
                if (g[j] & (1u << k))
                    fb_fill_rect(x + k * 2, y + j * 2, 2, 2, true);
        x += FONT_ADV * 2;
    }
}

/* 牌面: 左上角 2x 点数 + 下方 2x 花色 */
static void bj_draw_rank(int x, int y, int card) {
    char s[3];
    int rank = card % 13;
    if (rank == 0) { s[0] = 'A'; s[1] = 0; }
    else if (rank == 9) { s[0] = '1'; s[1] = '0'; s[2] = 0; }
    else if (rank >= 10) { s[0] = "JQK"[rank - 10]; s[1] = 0; }
    else { s[0] = (char)('1' + rank); s[1] = 0; }
    int w = 0;
    for (int i = 0; s[i]; i++) w += FONT_W;
    int ox = x + (BJ_CARD_W - 4 - w * 2) / 2;   /* 内边距 2 每侧, 水平居中 */
    bj_text2x(ox, y, s);
}

static void bj_draw_suit(int x, int y, int card) {
    static const int suit_sym[4] = { CG_SPADE, CG_HEART, CG_DIAMOND, CG_CLUB };
    extern const uint8_t font_symbols[CG_COUNT][7];
    const uint8_t *g = font_symbols[suit_sym[card / 13]];
    for (int j = 0; j < FONT_H; j++)
        for (int k = 0; k < FONT_W; k++)
            if (g[j] & (1u << k))
                fb_fill_rect(x + k * 2, y + j * 2, 2, 2, true);
}

static void bj_draw_card(int x, int y, int card) {
    fb_fill_rect(x, y, BJ_CARD_W, BJ_CARD_H, false);   /* 白底 */
    bj_draw_rank(x + 2, y + 2, card);
    bj_draw_suit(x + 7, y + 17, card);
    fb_stroke_rect(x, y, BJ_CARD_W, BJ_CARD_H, true);
}

static void bj_draw_back(int x, int y) {
    fb_fill_rect(x, y, BJ_CARD_W, BJ_CARD_H, false);
    fb_fill_tile(x + 3, y + 3, BJ_CARD_W - 6, BJ_CARD_H - 6, pat_get(PAT_CROSS));
    fb_stroke_rect(x, y, BJ_CARD_W, BJ_CARD_H, true);
}

/* 一行牌 + 行尾合计(hide_hole 时庄家第 2 张画牌背, 合计显示 ?) */
static void bj_draw_row(int y, int who, bool hide_hole) {
    int shown = bj_n[who];
    if (shown > BJ_SHOWN) shown = BJ_SHOWN;
    int x = BJ_X0;
    for (int i = 0; i < shown; i++) {
        if (hide_hole && i == 1) bj_draw_back(x, y);
        else bj_draw_card(x, y, bj_hand[who][i]);
        x += BJ_STEP;
    }
    if (bj_n[who] > BJ_SHOWN) {         /* 超出显示 +N */
        char s[16];
        int len = 0;
        s[len++] = '+';
        char nn[8];
        bj_num(nn, (uint32_t)(bj_n[who] - BJ_SHOWN));
        for (int i = 0; nn[i]; i++) s[len++] = nn[i];
        s[len] = 0;
        fb_text(x + 2, y + (BJ_CARD_H - FONT_H) / 2, s, true);
        x += 2 + text_width(s) + 6;
    }
    char tot[16];
    if (hide_hole) { tot[0] = '?'; tot[1] = 0; }
    else {
        tot[0] = '=';
        bj_num(&tot[1], (uint32_t)bj_total(bj_hand[who], bj_n[who]));
    }
    bj_text2x(x + 6, y + (BJ_CARD_H - FONT_H * 2) / 2, tot);
}

static void bj_draw_status(void) {
    const char *msg;
    if (bj_phase == BJ_PH_PLAY) msg = "HIT OR STAND";
    else if (bj_phase == BJ_PH_DEALER) msg = "DEALER DRAWING";
    else {
        switch (bj_res) {
        case BJ_RES_WIN:   msg = "YOU WIN"; break;
        case BJ_RES_LOSE:  msg = "DEALER WINS"; break;
        case BJ_RES_PUSH:  msg = "PUSH"; break;
        case BJ_RES_PBUST: msg = "BUST! DEALER WINS"; break;
        case BJ_RES_DBUST: msg = "DEALER BUST - YOU WIN"; break;
        case BJ_RES_BJ:    msg = "BLACKJACK! YOU WIN"; break;
        default:           msg = "DEALER BLACKJACK"; break;
        }
    }
    fb_text(BJ_X0, BJ_SEP_Y + 3, msg, true);
    if (bj_phase == BJ_PH_OVER)
        fb_text(BJ_X0, BJ_SEP_Y + 21, "OK/N:NEW  BACK:QUIT", true);
    else
        fb_text(BJ_X0, BJ_SEP_Y + 21, "OK:HIT  BACK/SPC:STAND", true);
}

/* ---- 框架接口 ---- */
void blackjack_enter(void) {
    rng_seed(&bj_rng, now_ms() ^ 0xB17B);
    bj_win = 0;
    bj_loss = 0;
    bj_ties = 0;
    bj_shuffle();
    bj_deal();
    blackjack_render();
    disp_full();
}

void blackjack_exit(void) {}

void blackjack_tick(uint64_t now) {
    (void)now;
    if (bj_phase != BJ_PH_DEALER) return;
    int dt = bj_total(bj_hand[1], bj_n[1]);
    if (dt >= 17 || bj_n[1] >= BJ_MAX_HAND) { bj_finish(); return; }
    bj_hand[1][bj_n[1]++] = (int8_t)bj_draw();
    if (bj_total(bj_hand[1], bj_n[1]) >= 17) bj_finish();
}

void blackjack_render(void) {
    fb_clear(false);
    /* HUD: 左标题, 右 W/L/T */
    fb_text(0, 0, "BLACKJACK", true);
    char w[12], l[12], t[12];
    bj_num(w, bj_win);
    bj_num(l, bj_loss);
    bj_num(t, bj_ties);
    char line[48];
    int n = 0;
    line[n++] = 'W'; line[n++] = ':';
    for (int i = 0; w[i]; i++) line[n++] = w[i];
    line[n++] = ' '; line[n++] = 'L'; line[n++] = ':';
    for (int i = 0; l[i]; i++) line[n++] = l[i];
    line[n++] = ' '; line[n++] = 'T'; line[n++] = ':';
    for (int i = 0; t[i]; i++) line[n++] = t[i];
    line[n] = 0;
    fb_text(CCG_W - 2 - text_width(line), 0, line, true);
    fb_hline(0, CCG_HUD_H - 1, CCG_W, true);
    /* 两行牌 */
    fb_text(BJ_X0, 17, "DEALER", true);
    bj_draw_row(BJ_ROW_D, 1, bj_phase == BJ_PH_PLAY);
    fb_text(BJ_X0, 59, "PLAYER", true);
    bj_draw_row(BJ_ROW_P, 0, false);
    fb_hline(0, BJ_SEP_Y, CCG_W, true);
    bj_draw_status();
}

void blackjack_on_key(const key_event_t *ev) {
    if (ev->is_repeat) return;          /* 确认键/字母忽略长按重复 */
    if (ev->key == K_PAUSE || (ev->key == K_CHAR && ev->ch == 'p')) {
        pause_sel_t sel;
        if (ui_pause_run(&sel)) {
            if (sel == PAUSE_RESTART) blackjack_enter();
        } else {
            s_exit_request = true;
        }
        return;
    }
    if (bj_phase == BJ_PH_OVER) {
        if (ev->key == K_OK || (ev->key == K_CHAR && ev->ch == 'n'))
            bj_deal();                  /* 新一局 */
        else if (ev->key == K_BACK || ev->key == K_QUIT)
            s_exit_request = true;
        return;
    }
    switch (ev->key) {
    case K_OK:
        bj_hit();
        break;
    case K_BACK:
    case K_SPACE:
        bj_stand();
        break;
    case K_CHAR:
        if (ev->ch == 'h') bj_hit();
        else if (ev->ch == 's') bj_stand();
        else if (ev->ch == 'n') bj_deal();
        break;
    case K_QUIT:
        s_exit_request = true;
        break;
    default:
        break;
    }
}
