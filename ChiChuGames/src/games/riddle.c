/* RIDDLE QUIZ — 谜语竞猜: 30 则 classic 英文谜语题库
 * 玩法: 显示谜面, 玩家输入答案(字母键, OK 提交, DEL 退格);
 *       答对 +10 分并累计连对; N(空答案时)跳过显示答案;
 *       30 题答完或 Q 主动结束 → 总结面(得分/答对数/最高连对)
 * 键位: A-Z 输入; OK 提交/继续; DEL 退格(长按连退); N 跳过;
 *       对局中 BACK/P 暂停, Q 结束看总结; 总结面 OK/N 重来, BACK/Q 退出 */
#include "game.h"
#include "../config.h"
#include "../gfx/canvas.h"
#include "../gfx/font.h"
#include "../platform/display.h"
#include "../platform/input.h"
#include "../platform/audio.h"
#include "../platform/led.h"
#include "../platform/time.h"
#include "../rng.h"
#include <string.h>

#define RQ_N        30      /* 题库量 */
#define RQ_ANS_MAX  12      /* 答案最长 */
#define RQ_SCORE    10      /* 每题得分 */
#define RQ_FB_MS    1400u   /* 对/错反馈停留(2 拍 @700ms) */
#define RQ_LINE     46      /* 谜面单行最大字符(296/6=49, 留边距) */

typedef struct {
    const char *q;          /* 谜面 <=60 字符 */
    const char *a;          /* 答案 <=12 字符, 全小写字母 */
} rq_riddle_t;

/* 30 则 classic 英文谜语(谜面短 <=60, 答案单词 <=12, 答案两两不同) */
static const rq_riddle_t rq_riddles[RQ_N] = {
    {"What has keys but opens no locks?", "keyboard"},
    {"What has a face but no head?", "clock"},
    {"Cities but no houses, forests but no trees. What am I?", "map"},
    {"What has a neck but no head?", "bottle"},
    {"What gets wetter the more it dries?", "towel"},
    {"What has a thumb and four fingers but is not a hand?", "glove"},
    {"The more you take, the more you leave behind. What am I?", "footsteps"},
    {"What goes up but never comes down?", "age"},
    {"What is always coming but never arrives?", "tomorrow"},
    {"What has one eye but cannot see?", "needle"},
    {"What has a bed but never sleeps?", "river"},
    {"What has an ear but cannot hear?", "corn"},
    {"What is full of holes but still holds water?", "sponge"},
    {"What runs around the yard without moving?", "fence"},
    {"What has teeth but cannot bite?", "comb"},
    {"What has a ring but no finger?", "telephone"},
    {"What has legs but cannot walk?", "table"},
    {"What begins with T, ends with T, and has T in it?", "teapot"},
    {"What is black and white and read all over?", "newspaper"},
    {"What has a head and a tail but no body?", "coin"},
    {"What is easy to lift but hard to throw?", "feather"},
    {"What has a heart that never beats?", "artichoke"},
    {"What can you catch but not throw?", "cold"},
    {"What gets broken without being held?", "promise"},
    {"What has many keys but opens no doors?", "piano"},
    {"What gets bigger the more you take away?", "hole"},
    {"What is always in front of you but cannot be seen?", "future"},
    {"What has four legs but only one foot?", "bed"},
    {"What can travel the world while staying in a corner?", "stamp"},
    {"What has a tongue but cannot talk?", "shoe"},
};

/* ---- 对局状态 ---- */
typedef enum { RQ_PLAY, RQ_FB, RQ_OVER } rq_phase_t;

static uint8_t rq_order[RQ_N];      /* 每局洗牌的出题顺序 */
static int     rq_idx;              /* 当前题在顺序中的位置 0..29 */
static char    rq_ans[RQ_ANS_MAX + 1];
static int     rq_ans_len;
static int     rq_score;
static int     rq_streak;           /* 当前连续答对数 */
static int     rq_max_streak;
static int     rq_correct;          /* 累计答对数 */
static rq_phase_t rq_phase;
static bool    rq_fb_ok;            /* 反馈: 答对 */
static bool    rq_fb_skip;          /* 反馈: 跳过 */
static uint64_t rq_fb_until;        /* 反馈自动进入下一题的绝对时刻 */
static bool    rq_complete;         /* 30 题全答完(否则为主动结束) */
static bool    rq_over_full;        /* 结束全刷只做一次 */
static rng_t   rq_rng;
static uint64_t rq_seed_cnt;

void riddle_render(void);

/* 十进制写入 b 并补 '\0', 返回长度 */
static int rq_putint(char *b, int v) {
    char tmp[6];
    int n = 0, k = 0;
    do { tmp[k++] = (char)('0' + v % 10); v /= 10; } while (v > 0 && k < 6);
    while (k > 0) b[n++] = tmp[--k];
    b[n] = 0;
    return n;
}

static const char *rq_cur_q(void) { return rq_riddles[rq_order[rq_idx]].q; }
static const char *rq_cur_a(void) { return rq_riddles[rq_order[rq_idx]].a; }

/* Fisher-Yates 洗牌(必须 rng_seed 播种后调用; rng_range n<=1 内部早退) */
static void rq_shuffle(void) {
    int i;
    for (i = 0; i < RQ_N; i++) rq_order[i] = (uint8_t)i;
    for (i = RQ_N - 1; i > 0; i--) {
        uint32_t j = rng_range(&rq_rng, (uint32_t)(i + 1));
        uint8_t t = rq_order[i];
        rq_order[i] = rq_order[j];
        rq_order[j] = t;
    }
}

/* 进入下一题; 出界则总结面 */
static void rq_advance(void) {
    rq_ans_len = 0;
    rq_ans[0] = 0;
    rq_idx++;
    if (rq_idx >= RQ_N) {
        rq_phase = RQ_OVER;
        rq_complete = true;
        rq_over_full = false;
        audio_win();
        led_fx_set(LED_FX_WIN);
    } else {
        rq_phase = RQ_PLAY;
    }
}

/* 提交答案: 对 +10/连对, 错清连对; 空答案忽略 */
static void rq_submit(void) {
    if (rq_ans_len == 0) return;
    if (strcmp(rq_ans, rq_cur_a()) == 0) {
        rq_score += RQ_SCORE;
        rq_streak++;
        rq_correct++;
        if (rq_streak > rq_max_streak) rq_max_streak = rq_streak;
        rq_fb_ok = true;
        audio_clear();
    } else {
        rq_streak = 0;
        rq_fb_ok = false;
        audio_error();
    }
    rq_fb_skip = false;
    rq_phase = RQ_FB;
    rq_fb_until = now_ms() + RQ_FB_MS;
}

/* 跳过本谜: 亮答案, 无分, 清连对 */
static void rq_skip(void) {
    rq_streak = 0;
    rq_fb_ok = false;
    rq_fb_skip = true;
    rq_phase = RQ_FB;
    rq_fb_until = now_ms() + RQ_FB_MS;
}

void riddle_enter(void) {
    rq_seed_cnt++;
    rng_seed(&rq_rng, (uint64_t)now_ms() ^ ((uint64_t)rq_seed_cnt << 32) ^ 0x51D1ULL);
    rq_shuffle();
    rq_idx = 0;
    rq_ans_len = 0;
    rq_ans[0] = 0;
    rq_score = 0;
    rq_streak = 0;
    rq_max_streak = 0;
    rq_correct = 0;
    rq_phase = RQ_PLAY;
    rq_complete = false;
    rq_over_full = false;
    riddle_render();
    disp_full();
}

void riddle_exit(void) {}

void riddle_tick(uint64_t now) {
    if (rq_phase == RQ_FB && now >= rq_fb_until) rq_advance();
}

/* 谜面按词折行, 最多 2 行(<=60 字符, 46/行足够) */
static void rq_wrap(char out[2][RQ_LINE + 1]) {
    const char *s = rq_cur_q();
    int li = 0, wi = 0;
    out[0][0] = 0;
    out[1][0] = 0;
    while (*s && li < 2) {
        const char *start = s;
        int wlen;
        while (*s && *s != ' ') s++;
        wlen = (int)(s - start);
        if (wlen > RQ_LINE) wlen = RQ_LINE;   /* 超长单词截断兜底 */
        if (wi > 0 && wi + 1 + wlen > RQ_LINE) {
            li++;
            wi = 0;
            if (li >= 2) break;
        }
        if (wi > 0) out[li][wi++] = ' ';
        memcpy(out[li] + wi, start, (size_t)wlen);
        wi += wlen;
        out[li][wi] = 0;
        if (*s) s++;
    }
}

/* 反馈行: 对/错/跳过(错与跳过亮出答案) */
static void rq_fb_line(char *b, size_t cap) {
    size_t n = 0;
    if (rq_fb_ok) {
        static const char pre[] = "CORRECT! +10";
        while (pre[n] && n + 1 < cap) { b[n] = pre[n]; n++; }
    } else {
        static const char pre[] = "WRONG! ANSWER: ";
        const char *a;
        if (rq_fb_skip) {
            static const char sp[] = "SKIPPED. ANSWER: ";
            while (sp[n] && n + 1 < cap) { b[n] = sp[n]; n++; }
        } else {
            while (pre[n] && n + 1 < cap) { b[n] = pre[n]; n++; }
        }
        a = rq_cur_a();
        while (*a && n + 1 < cap) { b[n++] = *a; a++; }
    }
    b[n] = 0;
}

/* 底部提示行 */
static void rq_hint_line(char *b, size_t cap) {
    size_t n = 0;
    static const char base[] = "OK:CHECK DEL:DEL";
    while (base[n] && n + 1 < cap) { b[n] = base[n]; n++; }
    if (rq_phase == RQ_PLAY) {
        /* 答案以 n 开头时 N 用于输入, 不提供跳过(避免误导) */
        if (rq_cur_a()[0] != 'n') {
            static const char sk[] = " N:SKIP";
            size_t k = 0;
            while (sk[k] && n + 1 < cap) { b[n++] = sk[k]; k++; }
        }
    } else {
        static const char nx[] = " OK:NEXT";
        size_t k = 0;
        while (nx[k] && n + 1 < cap) { b[n++] = nx[k]; k++; }
    }
    b[n] = 0;
}

void riddle_render(void) {
    fb_clear(false);

    /* ---- 总结面: HUD 两行 + 右上操作提示; 墙内大分数 ---- */
    if (rq_phase == RQ_OVER) {
        char b[24];
        int n;
        fb_fill_rect(0, 0, CCG_W, CCG_HUD_H - 1, false);
        fb_text(2, 1, rq_complete ? "ALL DONE!" : "QUIT EARLY", true);
        n = 0;
        {
            static const char lab[] = "SCORE ";
            size_t k = 0;
            while (lab[k]) { b[n++] = lab[k]; k++; }
        }
        n += rq_putint(b + n, rq_score);
        fb_text(2, 9, b, true);
        fb_text(CCG_W - 2 - text_width("OK/N:RETRY BACK:QUIT"), 1,
                "OK/N:RETRY BACK:QUIT", true);
        fb_hline(0, CCG_HUD_H - 1, CCG_W, true);
        if (!rq_over_full) {
            rq_over_full = true;
            disp_force_full();
        }
        {
            char big[16];
            int m = 0;
            {
                static const char lab[] = "SCORE ";
                size_t k = 0;
                while (lab[k]) { big[m++] = lab[k]; k++; }
            }
            m += rq_putint(big + m, rq_score);
            fb_text_scale2((CCG_W - text_width(big) * 2) / 2, 40, big, true);
        }
        {
            char l2[24];
            int m = 0;
            {
                static const char lab[] = "CORRECT ";
                size_t k = 0;
                while (lab[k]) { l2[m++] = lab[k]; k++; }
            }
            m += rq_putint(l2 + m, rq_correct);
            l2[m++] = '/';
            m += rq_putint(l2 + m, RQ_N);
            l2[m++] = ' ';
            l2[m++] = ' ';
            {
                static const char lab[] = "BEST ";
                size_t k = 0;
                while (lab[k]) { l2[m++] = lab[k]; k++; }
            }
            m += rq_putint(l2 + m, rq_max_streak);
            fb_text_center(70, l2, true);
        }
        fb_text_center(84, "OK/N:RETRY  BACK:QUIT", true);
        return;
    }

    /* ---- HUD 顶栏: 黑字白底 ---- */
    fb_text_scale2(2, 1, "RIDDLE QUIZ", true);
    {
        char rb[16];
        int m = 0;
        {
            static const char lab[] = "SCORE ";
            size_t k = 0;
            while (lab[k]) { rb[m++] = lab[k]; k++; }
        }
        m += rq_putint(rb + m, rq_score);
        fb_text_scale2(CCG_W - 2 - text_width(rb) * 2, 1, rb, true);
    }
    fb_hline(0, CCG_HUD_H - 1, CCG_W, true);

    /* ---- 题号 + 连对 ---- */
    {
        char lb[16];
        int m = 0;
        lb[m++] = 'Q';
        lb[m++] = ' ';
        m += rq_putint(lb + m, rq_idx + 1);
        lb[m++] = '/';
        m += rq_putint(lb + m, RQ_N);
        fb_text(2, 18, lb, true);
        {
            char rb[16];
            int k = 0;
            {
                static const char lab[] = "STREAK ";
                size_t j = 0;
                while (lab[j]) { rb[k++] = lab[j]; j++; }
            }
            k += rq_putint(rb + k, rq_streak);
            fb_text(CCG_W - 2 - text_width(rb), 18, rb, true);
        }
    }

    /* ---- 谜面(折行 1-2 行) ---- */
    {
        char lines[2][RQ_LINE + 1];
        rq_wrap(lines);
        fb_text(8, 30, lines[0], true);
        if (lines[1][0]) fb_text(8, 38, lines[1], true);
    }

    /* ---- 答案输入区: 标签 + 2x 字母 + 光标下划线 ---- */
    fb_text(2, 60, "YOUR ANSWER:", true);
    {
        int i;
        for (i = 0; i < rq_ans_len; i++) {
            char c[2];
            c[0] = rq_ans[i];
            c[1] = 0;
            fb_text_scale2(2 + i * 12, 70, c, true);
        }
        if (rq_phase == RQ_PLAY && rq_ans_len < RQ_ANS_MAX) {
            int cx = 2 + rq_ans_len * 12;
            fb_fill_rect(cx, 81, 12, 3, true);   /* 光标: 下一格下划线 */
        }
    }

    /* ---- 反馈行(仅反馈期显示, 防止上一题反馈残留到新题) ---- */
    if (rq_phase == RQ_FB) {
        char fb[40];
        rq_fb_line(fb, sizeof(fb));
        fb_text_center(96, fb, true);
    }

    /* ---- 底部按键提示 ---- */
    {
        char hb[32];
        rq_hint_line(hb, sizeof(hb));
        fb_text_center(140, hb, true);
    }
}

void riddle_on_key(const key_event_t *ev) {
    if (ev->is_repeat) {
        if (ev->key == K_DEL && rq_phase == RQ_PLAY && rq_ans_len > 0)
            rq_ans[--rq_ans_len] = 0;   /* 长按 DEL 连续退格 */
        return;
    }
    if (rq_phase == RQ_OVER) {
        if (ev->key == K_OK || (ev->key == K_CHAR && ev->ch == 'n'))
            riddle_enter();
        else if (ev->key == K_BACK || ev->key == K_QUIT)
            s_exit_request = true;
        return;
    }
    switch (ev->key) {
    case K_CHAR: {
        char ch = (char)ev->ch;
        if (ch >= 'A' && ch <= 'Z') ch = (char)(ch - 'A' + 'a');
        if (ch < 'a' || ch > 'z') break;
        if (rq_phase != RQ_PLAY) break;   /* 反馈中忽略输入 */
        /* 空答案时 N = 跳过(答案以 n 开头则 N 正常输入, 防止打不出首字母) */
        if (ch == 'n' && rq_ans_len == 0 && rq_cur_a()[0] != 'n') {
            audio_select();
            rq_skip();
        } else if (rq_ans_len < RQ_ANS_MAX) {
            rq_ans[rq_ans_len++] = ch;
            rq_ans[rq_ans_len] = 0;
            audio_tick();
        }
        break;
    }
    case K_DEL:
        if (rq_phase == RQ_PLAY && rq_ans_len > 0) { rq_ans[--rq_ans_len] = 0; audio_move(); }
        break;
    case K_OK:
        if (rq_phase == RQ_PLAY) rq_submit();
        else if (rq_phase == RQ_FB) rq_advance();
        break;
    case K_BACK:
    case K_PAUSE: {
        pause_sel_t sel;
        if (rq_phase == RQ_FB) break;   /* 反馈停留期忽略暂停, 便于读答案 */
        if (ui_pause_run(&sel)) {
            if (sel == PAUSE_RESTART) riddle_enter();
        } else {
            s_exit_request = true;
        }
        break;
    }
    case K_QUIT:
        /* 对局中 Q = 主动结束看总结(设计要求); 其余退出 */
        if (rq_phase == RQ_PLAY || rq_phase == RQ_FB) {
            rq_phase = RQ_OVER;
            rq_complete = false;
            rq_over_full = false;
        } else {
            s_exit_request = true;
        }
        break;
    default:
        break;
    }
}
