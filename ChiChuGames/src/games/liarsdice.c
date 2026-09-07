/* LIAR'S DICE — 猜骰: 叫点与质疑(玩家 3 骰 vs AI 3 骰, 骰面互不可见)
 * 轮叫: 每次叫点(q,f)必须严格递增(数量或点数); 质疑即开盅:
 *   全场该点数总数不足 q → 叫方失 1 骰; 足 → 质疑方失 1 骰
 * 骰数归 0 者败; 失骰者下轮先叫
 * 操作: 数字键先输数量再输点数, OK 提交; SPACE/BACK 质疑; DEL 清输入
 *       N 新局  P 暂停  Q 退出 */
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

#define LD_DIE 36                       /* 骰子边长(正方形) */
#define LD_AI_Y 18                      /* AI 行(开盅前隐藏) */
#define LD_PL_Y 94                      /* 玩家行(可见) */
#define LD_X0 ((int)((CCG_W - (3 * LD_DIE + 8)) / 2))  /* 三骰居中 */
#define LD_BID_Y 58                     /* 叫点大字 y */
#define LD_ST_Y 78                      /* 状态小字 y */
#define LD_FOOT_Y 136                   /* 底部提示 y */
#define LD_START 3                      /* 初始骰数 */

typedef enum {
    LD_PH_BID = 0,      /* 玩家轮叫/质疑 */
    LD_PH_AI,           /* AI 决策(tick 驱动) */
    LD_PH_REVEAL,       /* 开盅结果展示 */
    LD_PH_OVER          /* 终局 */
} ld_phase_t;

typedef enum {
    LD_HINT_NONE = 0,
    LD_HINT_LOW,        /* 叫点不够高 */
    LD_HINT_HIGH,       /* 超过总骰数 */
    LD_HINT_INC,        /* 输入未完成 */
    LD_HINT_NOBID       /* 无可质疑的叫点 */
} ld_hint_t;

static rng_t ld_rng;
static ld_phase_t ld_phase;
static int ld_pd[3];        /* 玩家骰面 0=无骰 */
static int ld_ad[3];        /* AI 骰面 0=无骰 */
static int ld_pn, ld_an;    /* 双方骰数 */
static int ld_cur_q, ld_cur_f;      /* 当前叫点; 0,0 = 本轮未开叫 */
static int ld_in_q, ld_in_f;        /* 玩家输入数量/点数 0=未设 */
static int ld_bidder;       /* 当前叫点的叫方 0=玩家 1=AI */
static int ld_loser;        /* 最近质疑的失骰方 0=玩家 1=AI */
static int ld_reveal_q, ld_reveal_f, ld_reveal_cnt;   /* 开盅快照 */
static int ld_winner;       /* 0=玩家胜 1=AI 胜 */
static ld_hint_t ld_hint;
static bool ld_over_full;   /* 终局全刷防重复 */
static int ld_wins, ld_losses;      /* 战绩(enter 清零) */

static const char *const ld_words[7] = {
    "", "ONES", "TWOS", "THREES", "FOURS", "FIVES", "SIXES"
};

/* pip 位置 3x3 格(0..2,0..2), { -1, -1 } 结束 */
static const int8_t ld_pips[7][7][2] = {
    { { -1, -1 } },
    { { 1, 1 }, { -1, -1 } },
    { { 0, 0 }, { 2, 2 }, { -1, -1 } },
    { { 0, 0 }, { 1, 1 }, { 2, 2 }, { -1, -1 } },
    { { 0, 0 }, { 2, 0 }, { 0, 2 }, { 2, 2 }, { -1, -1 } },
    { { 0, 0 }, { 2, 0 }, { 1, 1 }, { 0, 2 }, { 2, 2 }, { -1, -1 } },
    { { 0, 0 }, { 2, 0 }, { 0, 1 }, { 2, 1 }, { 0, 2 }, { 2, 2 }, { -1, -1 } }
};

void liarsdice_render(void);

/* ---- 逻辑 ---- */

/* 叫点合法性: 数量或点数必须严格大于当前叫点, 且数量 <= 总骰数 */
static bool ld_bid_ok(int q, int f) {
    int total = ld_pn + ld_an;
    if (q < 1 || q > total || f < 1 || f > 6) return false;
    if (ld_cur_q == 0) return true;
    if (q > ld_cur_q) return true;
    return q == ld_cur_q && f > ld_cur_f;
}

/* 质疑开盅: caller 0=玩家 1=AI; 开盅后失骰方为下轮先叫 */
static void ld_challenge(int caller) {
    int cnt = 0;
    for (int i = 0; i < 3; i++) {
        if (ld_pd[i]) cnt += (ld_pd[i] == ld_cur_f) ? 1 : 0;
        if (ld_ad[i]) cnt += (ld_ad[i] == ld_cur_f) ? 1 : 0;
    }
    bool met = cnt >= ld_cur_q;         /* 足→质疑方失骰; 不足→叫方失骰 */
    int loser = met ? caller : ld_bidder;
    ld_reveal_q = ld_cur_q;
    ld_reveal_f = ld_cur_f;
    ld_reveal_cnt = cnt;
    ld_loser = loser;
    if (loser == 0) ld_pn--; else ld_an--;
    ld_phase = LD_PH_REVEAL;
    if (ld_pn == 0 || ld_an == 0) {
        ld_winner = (ld_pn == 0) ? 1 : 0;
        if (ld_winner == 1) ld_losses++; else ld_wins++;
    }
}

/* 重掷幸存骰 */
static void ld_reroll(void) {
    for (int i = 0; i < 3; i++) {
        if (ld_pd[i]) ld_pd[i] = (int)rng_range(&ld_rng, 6) + 1;
        if (ld_ad[i]) ld_ad[i] = (int)rng_range(&ld_rng, 6) + 1;
    }
}

/* 新回合: starter 0=玩家先叫 1=AI 先叫 */
static void ld_new_round(int starter) {
    ld_cur_q = 0;
    ld_cur_f = 0;
    ld_in_q = 0;
    ld_in_f = 0;
    ld_hint = LD_HINT_NONE;
    ld_reroll();
    ld_phase = (starter == 0) ? LD_PH_BID : LD_PH_AI;
}

/* 整局新开(战绩保留) */
static void ld_new_game(void) {
    ld_pn = LD_START;
    ld_an = LD_START;
    for (int i = 0; i < 3; i++) { ld_pd[i] = 1; ld_ad[i] = 1; }
    ld_winner = 0;
    ld_over_full = false;
    ld_new_round(0);
}

/* 开盅确认: 有人归 0 进终局, 否则失骰方开新回合 */
static void ld_next(void) {
    if (ld_pn == 0 || ld_an == 0) {
        ld_phase = LD_PH_OVER;
        if (ld_winner == 0) {               /* 终局瞬间: 玩家胜 */
            audio_win();
            led_fx_set(LED_FX_WIN);
        } else {                            /* 终局瞬间: 玩家败 */
            audio_lose();
            led_fx_set(LED_FX_LOSE);
        }
        return;
    }
    ld_new_round(ld_loser);
}

/* ---- AI: 简单启发式 ---- */
static void ld_ai_move(void) {
    int total = ld_pn + ld_an;
    int acnt[7] = { 0, 0, 0, 0, 0, 0, 0 };
    for (int i = 0; i < 3; i++)
        if (ld_ad[i]) acnt[ld_ad[i]]++;

    if (ld_cur_q == 0) {
        /* 开局: 叫自己最多的点数, 25% 加 1 施压 */
        int bf = 1, bc = 0;
        for (int f = 1; f <= 6; f++)
            if (acnt[f] > bc) { bc = acnt[f]; bf = f; }
        if (bc == 0) bc = 1;
        if (bc < total && rng_range(&ld_rng, 100) < 25) bc++;
        ld_cur_q = bc;
        ld_cur_f = bf;
        ld_bidder = 1;
        ld_phase = LD_PH_BID;
        return;
    }

    /* 加叫: 每面可接受上限 = 己方计数 + 对手骰数/3, 选最高的合法叫 */
    int opp = ld_pn;
    int best_q = -1, best_f = 0;
    for (int f = 1; f <= 6; f++) {
        int cap = acnt[f] + (opp + 2) / 3;
        if (cap > total) cap = total;
        int q = ld_cur_q;
        if (f <= ld_cur_f) q = ld_cur_q + 1;    /* 同数量须更高点数 */
        if (q > cap) continue;                  /* 此面无法安全加叫 */
        if (q > best_q) { best_q = q; best_f = f; }
    }
    if (best_q > 0) {
        ld_cur_q = best_q;
        ld_cur_f = best_f;
        ld_bidder = 1;
        ld_phase = LD_PH_BID;
        return;
    }

    /* 高叫点无法安全加叫: 50% 质疑, 否则最小加叫冒险 */
    if (rng_range(&ld_rng, 100) < 50) {
        ld_challenge(1);
        return;
    }
    int f = ld_cur_f + 1;
    int q = ld_cur_q;
    if (f > 6) { f = 1; q++; }
    if (q > total) { ld_challenge(1); return; } /* 已顶到极限: 只能质疑 */
    ld_cur_q = q;
    ld_cur_f = f;
    ld_bidder = 1;
    ld_phase = LD_PH_BID;
}

/* ---- 渲染 ---- */

static void ld_draw_die(int x, int y, int face, bool hidden) {
    if (hidden) {
        fb_fill_rect(x, y, LD_DIE, LD_DIE, true);
        fb_text_scale2(x + 13, y + 11, "?", false);
        return;
    }
    fb_stroke_rect(x, y, LD_DIE, LD_DIE, true);
    if (face < 1 || face > 6) return;
    for (int k = 0; k < 7; k++) {
        int px = ld_pips[face][k][0];
        if (px < 0) break;
        int py = ld_pips[face][k][1];
        fb_fill_rect(x + 6 + px * 8, y + 6 + py * 8, 7, 7, true);
    }
}

/* 叫点大字行: 优先显示输入, 否则当前叫点; 开盅时显示全场计数 */
static void ld_draw_bid(void) {
    if (ld_phase == LD_PH_OVER) return;      /* 墙内不放提示文字 */
    char buf[16];
    if (ld_phase == LD_PH_REVEAL) {
        snprintf(buf, sizeof buf, "FOUND %d/%d", ld_reveal_cnt, ld_reveal_q);
    } else if (ld_in_q > 0) {
        if (ld_in_f > 0)
            snprintf(buf, sizeof buf, "%d %s", ld_in_q, ld_words[ld_in_f]);
        else
            snprintf(buf, sizeof buf, "%d _", ld_in_q);
    } else if (ld_cur_q > 0) {
        snprintf(buf, sizeof buf, "%d %s", ld_cur_q, ld_words[ld_cur_f]);
    } else {
        snprintf(buf, sizeof buf, "--");
    }
    fb_text_scale2((CCG_W - text_width(buf) * 2) / 2, LD_BID_Y, buf, true);
}

/* 状态小字行: 提示/对错反馈 */
static void ld_draw_status(void) {
    if (ld_phase == LD_PH_OVER) return;
    const char *s = "";
    switch (ld_phase) {
    case LD_PH_BID:
        if (ld_hint == LD_HINT_LOW) s = "BID TOO LOW - RAISE IT";
        else if (ld_hint == LD_HINT_HIGH) s = "OVER TOTAL DICE - CALL";
        else if (ld_hint == LD_HINT_INC) s = "NEED QTY + FACE DIGITS";
        else if (ld_hint == LD_HINT_NOBID) s = "NO BID TO CALL - OPEN";
        else s = ld_cur_q ? "YOUR TURN - RAISE OR CALL"
                          : "YOUR TURN - OPEN THE BID";
        break;
    case LD_PH_AI:
        s = "AI IS BIDDING...";
        break;
    case LD_PH_REVEAL:
        if (ld_loser == ld_bidder) {
            s = (ld_bidder == 0) ? "YOU BLUFFED - LOSE A DIE"
                                 : "AI BLUFFED - AI LOSES A DIE";
        } else {
            s = (ld_bidder == 0) ? "YOUR CALL WRONG - LOSE A DIE"
                                 : "AI CALLED WRONG - AI LOSES A DIE";
        }
        break;
    case LD_PH_OVER:
        break;
    }
    if (s[0]) fb_text_center(LD_ST_Y, s, true);
}

/* 底部提示行 */
static void ld_draw_foot(void) {
    if (ld_phase == LD_PH_OVER) return;
    if (ld_phase == LD_PH_REVEAL) {
        char b[40];
        snprintf(b, sizeof b, "BID %d %s  OK:NEXT ROUND",
                 ld_reveal_q, ld_words[ld_reveal_f]);
        fb_text_center(LD_FOOT_Y, b, true);
        return;
    }
    const char *s = (ld_phase == LD_PH_AI) ? "P=PAUSE  N=NEW"
                   : ld_cur_q ? "1ST=QTY 2ND=FACE OK:RAISE SP:CALL"
                              : "DIGITS SET QTY THEN FACE  OK:OPEN";
    fb_text_center(LD_FOOT_Y, s, true);
}

void liarsdice_render(void) {
    fb_clear(false);
    bool reveal = (ld_phase == LD_PH_REVEAL || ld_phase == LD_PH_OVER);

    /* HUD 顶栏: 黑字白底; 终局两行提示 */
    if (ld_phase == LD_PH_OVER) {
        fb_fill_rect(0, 0, CCG_W, CCG_HUD_H - 1, false);
        fb_text(2, 2, ld_winner == 1 ? "AI WINS" : "YOU WIN!", true);
        fb_text(CCG_W - 2 - text_width("OK/N:RETRY BACK:QUIT"), 2,
                "OK/N:RETRY BACK:QUIT", true);
        if (!ld_over_full) { ld_over_full = true; disp_force_full(); }
    } else {
        char rec[16];
        snprintf(rec, sizeof rec, "W%d L%d", ld_wins, ld_losses);
        fb_text(CCG_W - 2 - text_width(rec), 0, rec, true);
    }
    fb_text(2, 0, "LIAR'S DICE", true);
    fb_hline(0, CCG_HUD_H - 1, CCG_W, true);

    /* AI 行: 开盅前隐藏(黑盒+?), 开盅后亮面 */
    fb_text(4, LD_AI_Y + 14, "AI", true);
    for (int i = 0; i < ld_an; i++)
        ld_draw_die(LD_X0 + i * (LD_DIE + 4), LD_AI_Y, ld_ad[i], !reveal);

    ld_draw_bid();
    ld_draw_status();

    /* 玩家行: 骰面始终可见 */
    fb_text(4, LD_PL_Y + 14, "YOU", true);
    for (int i = 0; i < ld_pn; i++)
        ld_draw_die(LD_X0 + i * (LD_DIE + 4), LD_PL_Y, ld_pd[i], false);

    ld_draw_foot();
}

/* ---- 框架接口 ---- */

void liarsdice_tick(uint64_t now) {
    (void)now;
    if (ld_phase == LD_PH_AI) ld_ai_move();
}

void liarsdice_on_key(const key_event_t *ev) {
    if (ev->is_repeat) return;              /* 全部忽略重复 */
    switch (ev->key) {
    case K_CHAR:
        if (ev->ch == 'n') { ld_new_game(); return; }
        if (ld_phase == LD_PH_BID && ev->ch >= '1' && ev->ch <= '6') {
            int d = ev->ch - '0';
            ld_hint = LD_HINT_NONE;
            if (!ld_in_q) ld_in_q = d;
            else if (!ld_in_f) ld_in_f = d;
            /* 两项已满再按数字: 忽略, 用 DEL 重输 */
        }
        break;
    case K_DEL:
        if (ld_phase == LD_PH_BID) {
            ld_in_q = 0;
            ld_in_f = 0;
            ld_hint = LD_HINT_NONE;
        }
        break;
    case K_OK:
        if (ld_phase == LD_PH_BID) {
            if (ld_in_q && ld_in_f) {
                if (ld_bid_ok(ld_in_q, ld_in_f)) {
                    ld_cur_q = ld_in_q;
                    ld_cur_f = ld_in_f;
                    ld_bidder = 0;
                    ld_in_q = 0;
                    ld_in_f = 0;
                    ld_hint = LD_HINT_NONE;
                    ld_phase = LD_PH_AI;
                    audio_move();           /* 叫点成立 */
                } else {
                    ld_hint = (ld_in_q > ld_pn + ld_an) ? LD_HINT_HIGH
                                                        : LD_HINT_LOW;
                    audio_error();          /* 叫点不合法 */
                }
            } else {
                ld_hint = LD_HINT_INC;
            }
        } else if (ld_phase == LD_PH_REVEAL) {
            ld_next();
        } else if (ld_phase == LD_PH_OVER) {
            ld_new_game();
        }
        break;
    case K_SPACE:
    case K_BACK:
        if (ld_phase == LD_PH_BID) {
            if (ld_cur_q == 0) {
                ld_hint = LD_HINT_NOBID;
            } else {
                ld_in_q = 0;
                ld_in_f = 0;
                ld_hint = LD_HINT_NONE;
                ld_challenge(0);
                audio_select();             /* 开盅 */
            }
        } else if (ev->key == K_BACK && ld_phase == LD_PH_OVER) {
            s_exit_request = true;
        }
        break;
    case K_PAUSE: {
        pause_sel_t sel;
        if (ui_pause_run(&sel)) {
            if (sel == PAUSE_RESTART) ld_new_game();
        } else {
            s_exit_request = true;
        }
        break;
    }
    case K_QUIT: s_exit_request = true; break;
    default: break;
    }
}

void liarsdice_enter(void) {
    rng_seed(&ld_rng, now_ms() ^ 0xA5C39D12u);
    ld_wins = 0;
    ld_losses = 0;
    ld_new_game();
    liarsdice_render();
    disp_full();
}

void liarsdice_exit(void) {}
