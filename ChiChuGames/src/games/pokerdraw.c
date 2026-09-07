/* POKER DRAW — 5 张换牌比大小(玩家 vs AI)
 * 流程: 玩家标记要换的牌(OK 选中/取消, 最多 3 张) → OK 确认换牌
 *       → AI 换牌(简单策略: 保留对子/三条/四条, 换非对子, 单牌过多保留高牌)
 *       → 比牌: 同花顺>四条>葫芦>同花>顺子>三条>两对>一对>高牌
 * 显示: 两行牌; AI 第 2/4 张初始隐藏直到比牌; 比牌时显示牌型
 * 键位: L/R(+A/D) 移动  OK 标记/确认换牌  N 新局  P 暂停  Q 退出 */
#include "game.h"
#include "../config.h"
#include "../gfx/canvas.h"
#include "../gfx/font.h"
#include "../gfx/font_gen.h"
#include "../gfx/pattern.h"
#include "../platform/display.h"
#include "../platform/input.h"
#include "../platform/audio.h"
#include "../platform/led.h"
#include "../platform/time.h"
#include "../rng.h"

/* ---- 布局 ---- */
#define PD_CARD_W 30            /* 牌宽 */
#define PD_CARD_H 36            /* 牌高 */
#define PD_STEP 35              /* 牌间距 */
#define PD_X0 35                /* 首牌 x(5 张 + DRAW 按钮居中) */
#define PD_BTN_X 215            /* DRAW 按钮 x */
#define PD_BTN_W 48
#define PD_ROW_AI 22            /* AI 牌行 y */
#define PD_ROW_PL 66            /* 玩家牌行 y */
#define PD_SEP_Y 106            /* 状态区分隔线 */
#define PD_ST_Y 110             /* 状态行 1 */
#define PD_ST_Y2 126            /* 状态行 2 */
#define PD_AI_DELAY_MS 800u     /* AI 换牌提示时长(tick 间隔一致) */
#define PD_MAX_DISCARD 3        /* 最多换 3 张 */

typedef enum {
    PD_HIGH = 0, PD_PAIR, PD_TWOPAIR, PD_THREE,
    PD_STRAIGHT, PD_FLUSH, PD_FULL, PD_FOUR, PD_SF
} pd_type_t;

typedef enum {
    PD_PH_SELECT = 0,           /* 玩家选牌换牌 */
    PD_PH_AIDRAW,               /* AI 换牌(tick 驱动) */
    PD_PH_SHOW                  /* 比牌显示结果 */
} pd_phase_t;

static rng_t pd_rng;
static int8_t pd_deck[52];      /* 牌 id 0..51: 花色=id/13, 点数=id%13 */
static int pd_pos;              /* 下一张位置 */
static int8_t pd_hand[2][5];    /* [0]=玩家 [1]=AI */
static pd_phase_t pd_phase;
static int pd_cur;              /* 光标 0..5, 5 = DRAW 按钮 */
static bool pd_disc[5];         /* 玩家标记换牌 */
static int pd_ndisc;            /* 已标记张数 */
static uint64_t pd_ai_time;     /* AI 换牌阶段开始时刻 */
static int pd_result;           /* 1=玩家胜 -1=AI 胜 0=平 */
static bool pd_over_full;       /* 比牌全刷防重复 */
static int pd_win, pd_loss;     /* 连续对局计分 */

void pokerdraw_render(void);    /* enter 在前, 先声明 */

/* ---- 牌组: Fisher-Yates + 顺序发牌 ---- */
static void pd_shuffle(void) {
    for (int i = 0; i < 52; i++) pd_deck[i] = (int8_t)i;
    for (int i = 51; i > 0; i--) {
        uint32_t j = rng_range(&pd_rng, (uint32_t)i + 1);
        int8_t t = pd_deck[i];
        pd_deck[i] = pd_deck[j];
        pd_deck[j] = t;
    }
    pd_pos = 0;
}

static int pd_draw(void) {
    if (pd_pos >= 52) pd_shuffle();     /* 用尽重洗(单局最多 16 张, 防患) */
    return pd_deck[pd_pos++];
}

static void pd_deal(void) {
    for (int i = 0; i < 5; i++) {
        pd_hand[0][i] = (int8_t)pd_draw();
        pd_hand[1][i] = (int8_t)pd_draw();
    }
}

/* ---- 牌型判定 ---- */
static void pd_tally(const int8_t *h, int cnt[13], int uniq[13], int *nu) {
    for (int r = 0; r < 13; r++) cnt[r] = 0;
    for (int i = 0; i < 5; i++) cnt[h[i] % 13]++;
    *nu = 0;
    for (int r = 12; r >= 0; r--)
        if (cnt[r]) uniq[(*nu)++] = r;  /* 点数降序 */
}

static pd_type_t pd_hand_type(const int8_t *h) {
    int cnt[13], uniq[13], nu;
    pd_tally(h, cnt, uniq, &nu);
    int s0 = h[0] / 13;
    bool flush = h[1] / 13 == s0 && h[2] / 13 == s0 &&
                 h[3] / 13 == s0 && h[4] / 13 == s0;
    bool straight = (nu == 5 && uniq[0] - uniq[4] == 4);  /* A2345 即 0..4 顺 */
    int c4 = 0, c3 = 0, c2 = 0;
    for (int r = 0; r < 13; r++) {
        if (cnt[r] == 4) c4++;
        else if (cnt[r] == 3) c3++;
        else if (cnt[r] == 2) c2++;
    }
    if (flush && straight) return PD_SF;
    if (c4) return PD_FOUR;
    if (c3 && c2) return PD_FULL;
    if (flush) return PD_FLUSH;
    if (straight) return PD_STRAIGHT;
    if (c3) return PD_THREE;
    if (c2 == 2) return PD_TWOPAIR;
    if (c2) return PD_PAIR;
    return PD_HIGH;
}

/* 比较键: out[0]=牌型, out[1..5]=逐位比较点数(高位优先, 未用 -1) */
static void pd_tiebreak(const int8_t *h, int out[6]) {
    int cnt[13], uniq[13], nu;
    pd_tally(h, cnt, uniq, &nu);
    int t = (int)pd_hand_type(h);
    out[0] = t;
    for (int i = 1; i < 6; i++) out[i] = -1;
    switch (t) {
    case PD_SF:
    case PD_STRAIGHT:
        out[1] = uniq[0];                       /* 顺子高张 */
        break;
    case PD_FOUR:
        for (int r = 0; r < 13; r++) {
            if (cnt[r] == 4) out[1] = r;
            else if (cnt[r] == 1) out[2] = r;
        }
        break;
    case PD_FULL:
        for (int r = 0; r < 13; r++) {
            if (cnt[r] == 3) out[1] = r;
            else if (cnt[r] == 2) out[2] = r;
        }
        break;
    case PD_FLUSH:
    case PD_HIGH:
        for (int i = 0; i < nu; i++) out[1 + i] = uniq[i];
        break;
    case PD_THREE: {
        int t3 = 0;
        for (int r = 0; r < 13; r++)
            if (cnt[r] == 3) t3 = r;
        out[1] = t3;
        for (int i = 0, nk = 0; i < nu && nk < 2; i++)
            if (uniq[i] != t3) out[2 + nk++] = uniq[i];
        break;
    }
    case PD_TWOPAIR: {
        int pr[2] = { -1, -1 }, k = -1, np = 0;
        for (int r = 0; r < 13; r++) {
            if (cnt[r] == 2) pr[np++] = r;
            else if (cnt[r] == 1) k = r;
        }
        out[1] = pr[0] > pr[1] ? pr[0] : pr[1]; /* 高对 */
        out[2] = pr[0] < pr[1] ? pr[0] : pr[1]; /* 低对 */
        out[3] = k;
        break;
    }
    case PD_PAIR: {
        int pr = 0;
        for (int r = 0; r < 13; r++)
            if (cnt[r] == 2) pr = r;
        out[1] = pr;
        for (int i = 0, nk = 0; i < nu && nk < 3; i++)
            if (uniq[i] != pr) out[2 + nk++] = uniq[i];
        break;
    }
    default:
        break;
    }
}

static int pd_compare(const int8_t *a, const int8_t *b) {
    int ka[6], kb[6];
    pd_tiebreak(a, ka);
    pd_tiebreak(b, kb);
    for (int i = 0; i < 6; i++) {
        if (ka[i] != kb[i]) return ka[i] < kb[i] ? -1 : 1;
    }
    return 0;
}

static const char *pd_type_name(int t) {
    static const char *const names[9] = {
        "HIGH CARD", "ONE PAIR", "TWO PAIR", "THREE KIND", "STRAIGHT",
        "FLUSH", "FULL HOUSE", "FOUR KIND", "STRAIGHT FLUSH"
    };
    return names[t];
}

/* ---- AI 换牌: 保留成对/三条/四条, 换非对子; 单牌超过 3 张保留点数最高的 ---- */
static void pd_ai_discard(void) {
    int cnt[13];
    for (int r = 0; r < 13; r++) cnt[r] = 0;
    for (int i = 0; i < 5; i++) cnt[pd_hand[1][i] % 13]++;
    bool drop[5];
    int n = 0;
    for (int i = 0; i < 5; i++) {
        drop[i] = (cnt[pd_hand[1][i] % 13] == 1);
        if (drop[i]) n++;
    }
    while (n > PD_MAX_DISCARD) {            /* 单牌过多: 保留点数最高的 n-3 张 */
        int keep = -1, keepr = -1;
        for (int i = 0; i < 5; i++)
            if (drop[i] && pd_hand[1][i] % 13 > keepr) {
                keep = i;
                keepr = pd_hand[1][i] % 13;
            }
        drop[keep] = false;
        n--;
    }
    for (int i = 0; i < 5; i++)
        if (drop[i]) pd_hand[1][i] = (int8_t)pd_draw();
}

/* ---- 比牌结算 ---- */
static void pd_finish(void) {
    int c = pd_compare(pd_hand[0], pd_hand[1]);
    if (c > 0) { pd_win++; pd_result = 1; audio_win(); led_fx_set(LED_FX_WIN); }
    else if (c < 0) { pd_loss++; pd_result = -1; audio_lose(); led_fx_set(LED_FX_LOSE); }
    else { pd_result = 0; audio_move(); }
    pd_phase = PD_PH_SHOW;
    pd_over_full = false;
}

/* ---- 流程 ---- */
static void pd_new_game(void) {
    pd_shuffle();
    pd_deal();
    pd_phase = PD_PH_SELECT;
    pd_cur = 0;
    pd_ndisc = 0;
    for (int i = 0; i < 5; i++) pd_disc[i] = false;
    pd_over_full = false;
}

static void pd_toggle_discard(void) {
    if (pd_cur >= 5) return;
    if (pd_disc[pd_cur]) {
        pd_disc[pd_cur] = false;
        pd_ndisc--;
        audio_select();              /* 取消标记 */
    } else if (pd_ndisc < PD_MAX_DISCARD) {
        pd_disc[pd_cur] = true;
        pd_ndisc++;
        audio_select();              /* 标记换牌 */
    }
}

static void pd_confirm_draw(void) {
    if (pd_phase != PD_PH_SELECT) return;
    for (int i = 0; i < 5; i++)
        if (pd_disc[i]) pd_hand[0][i] = (int8_t)pd_draw();
    pd_phase = PD_PH_AIDRAW;
    pd_ai_time = now_ms();
    audio_select();                  /* 确认换牌 */
}

/* ---- 渲染工具 ---- */
static void pd_append(char *s, int *n, const char *t) {
    while (*t) s[(*n)++] = *t++;
}

static void pd_num(char *buf, int v) {
    char tmp[12];
    int n = 0;
    if (v <= 0) tmp[n++] = '0';
    while (v > 0 && n < 11) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
    for (int i = 0; i < n; i++) buf[i] = tmp[n - 1 - i];
    buf[n] = 0;
}

/* 2x 放大 ASCII(5x7 -> 10x14, 字距 2px) */
static void pd_text2x(int x, int y, const char *s) {
    for (int i = 0; s[i]; i++) {
        const uint8_t *g = font_glyph5x7[(unsigned char)s[i]];
        for (int j = 0; j < FONT_H; j++)
            for (int k = 0; k < FONT_W; k++)
                if (g[j] & (1u << k))
                    fb_fill_rect(x + k * 2, y + j * 2, 2, 2, true);
        x += FONT_ADV * 2;
    }
}

/* 点数文本(1-2 字符) */
static void pd_rank_str(int rank, char s[3]) {
    if (rank == 0) { s[0] = 'A'; s[1] = 0; }
    else if (rank == 9) { s[0] = '1'; s[1] = '0'; s[2] = 0; }
    else if (rank >= 10) { s[0] = "JQK"[rank - 10]; s[1] = 0; }
    else { s[0] = (char)('1' + rank); s[1] = 0; }
}

/* 牌面: selected=黑底白字(标记丢弃), sel=光标(最后画, 反色粗框) */
static void pd_draw_card(int x, int y, int card, bool selected, bool sel) {
    static const int suit_sym[4] = { CG_SPADE, CG_HEART, CG_DIAMOND, CG_CLUB };
    char s[3];
    int rank = card % 13;
    bool black = selected;
    fb_fill_rect(x, y, PD_CARD_W, PD_CARD_H, black);
    pd_rank_str(rank, s);
    pd_text2x(x + 2, y + 2, s);                          /* 左上 2x 点数 */
    const uint8_t *g = font_symbols[suit_sym[card / 13]];
    int sy = y + PD_CARD_H - 2 - FONT_H * 2;
    int sx = x + PD_CARD_W - 2 - FONT_W * 2;
    for (int j = 0; j < FONT_H; j++)
        for (int k = 0; k < FONT_W; k++)
            if (g[j] & (1u << k))
                fb_fill_rect(sx + k * 2, sy + j * 2, 2, 2, !black);
    fb_stroke_rect(x, y, PD_CARD_W, PD_CARD_H, !black);
    if (sel) fb_stroke_rect_thick(x - 2, y - 2, PD_CARD_W + 4, PD_CARD_H + 4,
                                  2, !black);
}

static void pd_draw_back(int x, int y) {
    fb_fill_rect(x, y, PD_CARD_W, PD_CARD_H, false);
    fb_fill_tile(x + 3, y + 3, PD_CARD_W - 6, PD_CARD_H - 6, pat_get(PAT_CROSS));
    fb_stroke_rect(x, y, PD_CARD_W, PD_CARD_H, true);
}

static void pd_draw_btn(int x, int y, bool sel) {
    fb_fill_rect(x, y, PD_BTN_W, PD_CARD_H, false);
    pd_text2x(x + (PD_BTN_W - 4 * FONT_ADV * 2) / 2,
              y + (PD_CARD_H - FONT_H * 2) / 2, "DRAW");
    fb_stroke_rect(x, y, PD_BTN_W, PD_CARD_H, true);
    if (sel) fb_stroke_rect_thick(x - 2, y - 2, PD_BTN_W + 4, PD_CARD_H + 4,
                                  2, true);
}

static void pd_draw_hud(void) {
    fb_text(0, 0, "POKER DRAW", true);
    char line[24], w[8], l[8];
    pd_num(w, pd_win);
    pd_num(l, pd_loss);
    int n = 0;
    pd_append(line, &n, "W:");
    pd_append(line, &n, w);
    pd_append(line, &n, " L:");
    pd_append(line, &n, l);
    line[n] = 0;
    fb_text(CCG_W - 2 - text_width(line), 0, line, true);
    fb_hline(0, CCG_HUD_H - 1, CCG_W, true);
}

static void pd_draw_status(void) {
    if (pd_phase == PD_PH_SELECT) {
        fb_text(PD_X0, PD_ST_Y, "OK: MARK  L/R: MOVE  N: NEW", true);
        if (pd_cur == 5) {
            fb_text(PD_X0, PD_ST_Y2, "OK: DRAW CARDS NOW", true);
        } else {
            char s[32];
            int n = 0;
            pd_append(s, &n, "DISCARD ");
            char m[4];
            pd_num(m, pd_ndisc);
            pd_append(s, &n, m);
            pd_append(s, &n, "/3  RIGHT END: DRAW");
            s[n] = 0;
            fb_text(PD_X0, PD_ST_Y2, s, true);
        }
    } else if (pd_phase == PD_PH_AIDRAW) {
        fb_text(PD_X0, PD_ST_Y, "AI DRAWING...", true);
    } else {
        /* 比牌: 两行牌型 + 比分 */
        char s1[48];
        int n = 0;
        pd_append(s1, &n, "YOU: ");
        pd_append(s1, &n, pd_type_name((int)pd_hand_type(pd_hand[0])));
        pd_append(s1, &n, "   AI: ");
        pd_append(s1, &n, pd_type_name((int)pd_hand_type(pd_hand[1])));
        s1[n] = 0;
        fb_text(PD_X0, PD_ST_Y, s1, true);
        char s2[24], w[8], l[8];
        pd_num(w, pd_win);
        pd_num(l, pd_loss);
        n = 0;
        pd_append(s2, &n, "SCORE  W:");
        pd_append(s2, &n, w);
        pd_append(s2, &n, " L:");
        pd_append(s2, &n, l);
        s2[n] = 0;
        fb_text(PD_X0, PD_ST_Y2, s2, true);
    }
}

/* ---- 框架接口 ---- */
void pokerdraw_enter(void) {
    rng_seed(&pd_rng, now_ms() ^ 0x9D2Fu);
    pd_win = 0;
    pd_loss = 0;
    pd_new_game();
    pokerdraw_render();
    disp_full();
}

void pokerdraw_exit(void) {}

void pokerdraw_tick(uint64_t now) {
    if (pd_phase != PD_PH_AIDRAW) return;
    if (now - pd_ai_time >= PD_AI_DELAY_MS) {
        pd_ai_discard();
        pd_finish();
    }
}

void pokerdraw_render(void) {
    fb_clear(false);
    pd_draw_hud();
    /* AI 行: 比牌前隐藏第 2/4 张 */
    fb_text(8, 18, "AI", true);
    bool hide = pd_phase != PD_PH_SHOW;
    for (int i = 0; i < 5; i++) {
        int x = PD_X0 + i * PD_STEP;
        if (hide && (i == 1 || i == 3)) pd_draw_back(x, PD_ROW_AI);
        else pd_draw_card(x, PD_ROW_AI, pd_hand[1][i], false, false);
    }
    /* 玩家行 + DRAW 按钮 */
    fb_text(8, 62, "PLAYER", true);
    for (int i = 0; i < 5; i++) {
        bool sel = (pd_phase == PD_PH_SELECT) && (pd_cur == i);
        pd_draw_card(PD_X0 + i * PD_STEP, PD_ROW_PL, pd_hand[0][i],
                     pd_disc[i], sel);
    }
    pd_draw_btn(PD_BTN_X, PD_ROW_PL, (pd_phase == PD_PH_SELECT) && pd_cur == 5);
    fb_hline(0, PD_SEP_Y, CCG_W, true);
    pd_draw_status();
    if (pd_phase == PD_PH_SHOW) {
        /* 游戏结束: HUD 区左结果 + 右提示, 全刷一次 */
        fb_fill_rect(0, 0, CCG_W, CCG_HUD_H - 1, false);
        if (pd_result > 0) fb_text(2, 2, "YOU WIN", true);
        else if (pd_result < 0) fb_text(2, 2, "AI WINS", true);
        else fb_text(2, 2, "TIE", true);
        fb_text(CCG_W - 2 - text_width("OK/N:RETRY BACK:QUIT"), 2,
                "OK/N:RETRY BACK:QUIT", true);
        if (!pd_over_full) { pd_over_full = true; disp_force_full(); }
    }
}

void pokerdraw_on_key(const key_event_t *ev) {
    if (ev->key == K_PAUSE || (ev->key == K_CHAR && ev->ch == 'p')) {
        pause_sel_t sel;
        if (ui_pause_run(&sel)) {
            if (sel == PAUSE_RESTART) pokerdraw_enter();
        } else {
            s_exit_request = true;
        }
        return;
    }
    if (pd_phase == PD_PH_SHOW) {
        if (ev->is_repeat) return;
        if (ev->key == K_OK || (ev->key == K_CHAR && ev->ch == 'n'))
            pd_new_game();
        else if (ev->key == K_BACK || ev->key == K_QUIT)
            s_exit_request = true;
        return;
    }
    /* 方向键可响应长按重复, 确认键/字母忽略 */
    bool dir = ev->key == K_LEFT || ev->key == K_RIGHT;
    if (ev->is_repeat && !dir) return;
    switch (ev->key) {
    case K_LEFT:
        pd_cur = (pd_cur + 5) % 6;
        if (!ev->is_repeat) audio_move();
        break;
    case K_RIGHT:
        pd_cur = (pd_cur + 1) % 6;
        if (!ev->is_repeat) audio_move();
        break;
    case K_OK:
        if (pd_phase == PD_PH_SELECT) {
            if (pd_cur == 5) pd_confirm_draw();
            else pd_toggle_discard();
        }
        break;
    case K_CHAR:
        if (ev->ch == 'a') pd_cur = (pd_cur + 5) % 6;
        else if (ev->ch == 'd') pd_cur = (pd_cur + 1) % 6;
        else if (ev->ch == 'n') pd_new_game();
        break;
    case K_QUIT:
        s_exit_request = true;
        break;
    default:
        break;
    }
}
