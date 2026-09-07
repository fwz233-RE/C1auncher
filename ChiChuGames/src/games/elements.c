/* ELEMENT QUIZ — 元素周期表问答: 符号 + 原子序数 → 元素名
 * 30 元素题库(原子序 1-30, static const, 确定性答案), 两种模式:
 *   A. 4-CHOICE: 4 选 1, 方向键移动光标 + OK 确认
 *   B. TYPE-IN : 字母拼写元素名(首字母 + 空格数提示), DEL 退格, OK 提交
 * 30 秒限时; 答对 +10 立即下一题; 答错显示正确答案 1.0s 后自动下一题(计时不停)
 * 静态前缀 el_; 像素坐标一律 int; 零 malloc; ASCII 文本
 *
 * 集成提示(help[] 最多 5 行):
 *   "ELEMENT QUIZ",
 *   "NAME THE ELEMENT BY SYMBOL",
 *   "4-CHOICE: ARROWS+OK  TYPE-IN: LETTERS+OK",
 *   "DEL=BACKSPACE  30 SEC  +10 PER RIGHT",
 *   "P:PAUSE  N:NEW  Q:QUIT"
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

#define EL_N            30      /* 题库元素数(原子序 1-30) */
#define EL_TICK_MS      100u    /* 离散 tick(面板平滑上限 700ms 之内) */
#define EL_GAME_MS      30000u  /* 30 秒限时 */
#define EL_FBK_TICKS    10      /* 答错展示正确答案 1.0s */
#define EL_MAX_LEN      11      /* 最长元素名 PHOSPHORUS=10 + '\0' */

/* 布局 */
#define EL_QB_X 88              /* 题目框(符号 + 原子序数) */
#define EL_QB_Y 16
#define EL_QB_W 120
#define EL_QB_H 54
#define EL_OPT_Y1 84            /* 4 选 1 网格两行 */
#define EL_OPT_Y2 108
#define EL_HINT_Y 136

typedef struct {
    uint8_t num;                /* 原子序数 */
    const char *sym;            /* 符号 */
    const char *name;           /* 元素名 */
} el_elem_t;

/* 题库: 原子序 1-30 常用元素 */
static const el_elem_t el_elems[EL_N] = {
    {  1, "H",  "HYDROGEN"   }, {  2, "He", "HELIUM"     },
    {  3, "Li", "LITHIUM"    }, {  4, "Be", "BERYLLIUM"  },
    {  5, "B",  "BORON"      }, {  6, "C",  "CARBON"     },
    {  7, "N",  "NITROGEN"   }, {  8, "O",  "OXYGEN"     },
    {  9, "F",  "FLUORINE"   }, { 10, "Ne", "NEON"       },
    { 11, "Na", "SODIUM"     }, { 12, "Mg", "MAGNESIUM"  },
    { 13, "Al", "ALUMINIUM"  }, { 14, "Si", "SILICON"    },
    { 15, "P",  "PHOSPHORUS" }, { 16, "S",  "SULFUR"     },
    { 17, "Cl", "CHLORINE"   }, { 18, "Ar", "ARGON"      },
    { 19, "K",  "POTASSIUM"  }, { 20, "Ca", "CALCIUM"    },
    { 21, "Sc", "SCANDIUM"   }, { 22, "Ti", "TITANIUM"   },
    { 23, "V",  "VANADIUM"   }, { 24, "Cr", "CHROMIUM"   },
    { 25, "Mn", "MANGANESE"  }, { 26, "Fe", "IRON"       },
    { 27, "Co", "COBALT"     }, { 28, "Ni", "NICKEL"     },
    { 29, "Cu", "COPPER"     }, { 30, "Zn", "ZINC"       },
};

typedef enum {
    EL_ST_MODE = 0,     /* 模式选择 */
    EL_ST_PLAY,         /* 答题中 */
    EL_ST_FBK,          /* 答错反馈(展示正确答案) */
    EL_ST_OVER          /* 时间到结算 */
} el_state_t;

static el_state_t el_state;
static int el_sel;              /* MODE 光标 0=4-choice 1=type-in */
static int el_mode;             /* 本局模式 */
static int el_cur;              /* 4 选 1 光标 0..3 */
static int el_subject;          /* 当前题目(题库下标) */
static int el_last;             /* 上一题下标, -1=无 */
static int el_opts[4];          /* 4 个选项下标 */
static uint32_t el_score;
static uint32_t el_best;
static uint32_t el_correct;
static uint32_t el_wrong;
static uint32_t el_time_ms;     /* 剩余毫秒 */
static int el_fbk_ticks;        /* 反馈剩余 tick */
static bool el_over_full;       /* 结束全刷只做一次 */
static char el_typed[EL_MAX_LEN];   /* TYPE-IN 已输入(大写) */
static int el_typed_n;
static rng_t el_rng;
static uint64_t el_seed_cnt;

void elements_enter(void);
void elements_render(void);
static void el_build_opts(void);

/* 无符号数追加(无 snprintf 依赖) */
static void el_append_u32(char *buf, unsigned *n, uint32_t v, unsigned cap) {
    char tmp[12];
    unsigned len = 0;
    if (v == 0) tmp[len++] = '0';
    while (v && len < 11) { tmp[len++] = (char)('0' + v % 10u); v /= 10u; }
    while (len && *n < cap) buf[(*n)++] = tmp[--len];
}

/* 抽取下一题(不与上一题重复, do-while 带 guard) */
static void el_next_q(void) {
    int guard = 0;
    do {
        el_subject = (int)rng_range(&el_rng, EL_N);
    } while (el_subject == el_last && ++guard < 16);
    el_last = el_subject;
    el_cur = 0;
    el_typed_n = 0;
    if (el_mode == 0) el_build_opts();
    el_state = EL_ST_PLAY;
}

/* 4 个选项 = 题目 + 3 个不同干扰项, 再 Fisher-Yates 洗牌 */
static void el_build_opts(void) {
    int i, j;
    el_opts[0] = el_subject;
    for (i = 1; i < 4; i++) {
        int guard = 0;
        do {
            el_opts[i] = (int)rng_range(&el_rng, EL_N);
            for (j = 0; j < i; j++)
                if (el_opts[i] == el_opts[j]) break;
        } while (j < i && ++guard < 64);
    }
    for (i = 3; i > 0; i--) {
        int k = (int)rng_range(&el_rng, (uint32_t)(i + 1));
        int t = el_opts[i];
        el_opts[i] = el_opts[k];
        el_opts[k] = t;
    }
}

/* 4 选 1 作答 */
static void el_submit_idx(int opt) {
    if (el_state != EL_ST_PLAY) return;
    if (opt == el_subject) {
        el_score += 10u;
        el_correct++;
        audio_clear();                 /* 答对 */
        el_next_q();
    } else {
        el_wrong++;
        el_state = EL_ST_FBK;
        el_fbk_ticks = EL_FBK_TICKS;
        audio_error();                 /* 答错 */
    }
}

/* 输入作答: 与目标名(大写)逐字比较 */
static void el_submit_text(void) {
    const char *name = el_elems[el_subject].name;
    int len = 0, i;
    if (el_state != EL_ST_PLAY) return;
    if (el_typed_n == 0) return;
    while (name[len]) len++;
    if (el_typed_n != len) {
        el_wrong++;
        el_state = EL_ST_FBK;
        el_fbk_ticks = EL_FBK_TICKS;
        return;
    }
    for (i = 0; i < len; i++)
        if (el_typed[i] != name[i]) {
            el_wrong++;
            el_state = EL_ST_FBK;
            el_fbk_ticks = EL_FBK_TICKS;
            return;
        }
    el_score += 10u;
    el_correct++;
    audio_clear();                     /* 拼对 */
    el_next_q();
}

/* 2x2 网格光标(行/列各自翻转循环) */
static void el_move(ccg_key k) {
    int c = el_cur % 2, r = el_cur / 2;
    switch (k) {
    case K_UP:    r = 1 - r; break;
    case K_DOWN:  r = 1 - r; break;
    case K_LEFT:  c = 1 - c; break;
    case K_RIGHT: c = 1 - c; break;
    default:      return;
    }
    el_cur = r * 2 + c;
}

/* TYPE-IN 输入字母(忽略数字/满员) */
static void el_type(uint8_t ch) {
    if (ch >= 'A' && ch <= 'Z') ch = (uint8_t)(ch - 'A' + 'a');
    if (ch < 'a' || ch > 'z') return;
    if (el_typed_n >= EL_MAX_LEN - 1) return;
    el_typed[el_typed_n++] = (char)(ch - 'a' + 'A');
}

/* 开局(按指定模式) */
static void el_start(int mode) {
    el_seed_cnt++;
    rng_seed(&el_rng, (uint64_t)now_ms() ^ ((uint64_t)el_seed_cnt << 32) ^ 0x3E5Cu);
    el_mode = mode;
    el_score = 0;
    el_correct = 0;
    el_wrong = 0;
    el_time_ms = EL_GAME_MS;
    el_last = -1;
    el_over_full = false;
    el_next_q();
}

/* 暂停菜单 */
static void el_pause(void) {
    pause_sel_t sel;
    if (ui_pause_run(&sel)) {
        if (sel == PAUSE_RESTART) elements_enter();
    } else {
        s_exit_request = true;
    }
}

/* ---- 绘制 ---- */

/* 2x 居中文本 */
static void el_text_center2(int y, const char *s) {
    fb_text_scale2((CCG_W - text_width(s) * 2) / 2, y, s, true);
}

/* 4x 放大文本(仅 A-Z/数字), x 为左缘 */
static void el_draw_big(int x, int y, const char *s) {
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        unsigned char c = *p;
        int j, i;
        if (c >= 'a' && c <= 'z') c = (unsigned char)(c - 'a' + 'A');
        for (j = 0; j < FONT_H; j++)
            for (i = 0; i < FONT_W; i++)
                if (font_glyph5x7[c][j] & (1u << i))
                    fb_fill_rect(x + i * 4, y + j * 4, 4, 4, true);
        x += 24;   /* 4 * FONT_ADV */
        p++;
    }
}

/* 居中反白选项行(选中画黑底白字, 未选中黑字) */
static void el_row_inv(int y, const char *s, bool inv) {
    int w = text_width(s);
    int x = (CCG_W - w) / 2;
    if (inv) {
        fb_fill_rect(x - 2, y - 1, w + 4, FONT_H + 2, true);
        fb_text(x, y, s, false);
    } else {
        fb_text(x, y, s, true);
    }
}

/* HUD 右侧: SCORE n TIME s */
static void el_hud_right(char *buf, int cap) {
    unsigned n = 0;
    const char *s = "SCORE ";
    while (*s && n < (unsigned)cap - 1) buf[n++] = *s++;
    el_append_u32(buf, &n, el_score, (unsigned)cap);
    s = " TIME ";
    while (*s && n < (unsigned)cap - 1) buf[n++] = *s++;
    el_append_u32(buf, &n, (el_time_ms + 999u) / 1000u, (unsigned)cap);
    buf[n] = 0;
}

/* 题目区: 框 + 原子序数(2x) + 符号(4x) */
static void el_draw_question(void) {
    const el_elem_t *e = &el_elems[el_subject];
    char buf[8];
    unsigned n = 0;
    const char *s = "No.";
    int l = 0;
    while (*s && n < 6) buf[n++] = *s++;
    el_append_u32(buf, &n, e->num, 6);
    buf[n] = 0;
    fb_stroke_rect_thick(EL_QB_X, EL_QB_Y, EL_QB_W, EL_QB_H, 2, true);
    el_text_center2(EL_QB_Y + 4, buf);
    while (e->sym[l]) l++;
    el_draw_big((CCG_W - (l * 24 - 4)) / 2, EL_QB_Y + 22, e->sym);
}

/* 4 选 1 网格 */
static void el_draw_opts(void) {
    static const int cxs[2] = { 74, 222 };
    static const int ys[2] = { EL_OPT_Y1, EL_OPT_Y2 };
    int i;
    for (i = 0; i < 4; i++) {
        const char *nm = el_elems[el_opts[i]].name;
        int tw = text_width(nm);
        int x = cxs[i % 2] - tw / 2;
        int y = ys[i / 2];
        if (i == el_cur) {
            fb_fill_rect(x - 2, y - 1, tw + 4, FONT_H + 2, true);
            fb_text(x, y, nm, false);
        } else {
            fb_text(x, y, nm, true);
        }
    }
}

/* TYPE-IN 提示: 首字母 + 空格数 */
static void el_draw_blanks(void) {
    const char *nm = el_elems[el_subject].name;
    char buf[24];
    int n = 1, i;
    buf[0] = nm[0];
    for (i = 1; nm[i] && n < (int)sizeof(buf) - 2; i++) {
        buf[n++] = ' ';
        buf[n++] = '_';
    }
    buf[n] = 0;
    fb_text_center(80, buf, true);
}

/* TYPE-IN 已输入 + 块光标 */
static void el_draw_typed(void) {
    char buf[EL_MAX_LEN];
    int n = 0, i;
    int tw, x;
    for (i = 0; i < el_typed_n; i++) buf[n++] = el_typed[i];
    buf[n] = 0;
    tw = text_width(buf) * 2;
    x = (CCG_W - tw) / 2;
    fb_text_scale2(x, 96, buf, true);
    fb_fill_rect(x + tw + 2, 96, 4, 14, true);
}

/* 答错反馈: WRONG! 正确答案 */
static void el_draw_fbk(void) {
    char buf[24];
    int n = 0, i;
    const char *s = "WRONG!  ";
    const char *nm = el_elems[el_subject].name;
    for (i = 0; s[i] && n < (int)sizeof(buf) - 2; i++) buf[n++] = s[i];
    for (i = 0; nm[i] && n < (int)sizeof(buf) - 1; i++) buf[n++] = nm[i];
    buf[n] = 0;
    el_text_center2(92, buf);
}

void elements_render(void) {
    fb_clear(false);

    /* ---- 结束画面: HUD 两行 + 右上操作提示, 墙内只有成绩 ---- */
    if (el_state == EL_ST_OVER) {
        char buf[24];
        unsigned n = 0;
        const char *s;
        fb_fill_rect(0, 0, CCG_W, CCG_HUD_H - 1, false);
        s = "SCORE ";
        while (*s && n < 22) buf[n++] = *s++;
        el_append_u32(buf, &n, el_score, 22);
        s = " BEST ";
        while (*s && n < 22) buf[n++] = *s++;
        el_append_u32(buf, &n, el_best, 22);
        buf[n] = 0;
        fb_text(2, 1, buf, true);
        n = 0;
        s = "RIGHT ";
        while (*s && n < 22) buf[n++] = *s++;
        el_append_u32(buf, &n, el_correct, 22);
        s = "  WRONG ";
        while (*s && n < 22) buf[n++] = *s++;
        el_append_u32(buf, &n, el_wrong, 22);
        buf[n] = 0;
        fb_text(2, 9, buf, true);
        fb_text(CCG_W - 2 - text_width("OK/N:RETRY BACK:QUIT"), 1,
                "OK/N:RETRY BACK:QUIT", true);
        fb_hline(0, CCG_HUD_H - 1, CCG_W, true);
        el_text_center2(26, "TIME UP!");
        {
            char num[4];
            unsigned m = 0;
            el_append_u32(num, &m, el_score, 3);
            num[m] = 0;
            el_draw_big((CCG_W - ((int)m * 24 - 4)) / 2, 46, num);
        }
        n = 0;
        s = "RIGHT ";
        while (*s && n < 22) buf[n++] = *s++;
        el_append_u32(buf, &n, el_correct, 22);
        s = "   WRONG ";
        while (*s && n < 22) buf[n++] = *s++;
        el_append_u32(buf, &n, el_wrong, 22);
        buf[n] = 0;
        fb_text_center(84, buf, true);
        n = 0;
        s = "BEST ";
        while (*s && n < 22) buf[n++] = *s++;
        el_append_u32(buf, &n, el_best, 22);
        buf[n] = 0;
        fb_text_center(98, buf, true);
        if (!el_over_full) {
            el_over_full = true;
            disp_force_full();
        }
        return;
    }

    /* ---- HUD 顶栏: 黑字白底 ---- */
    fb_text(0, 0, "ELEMENT QUIZ", true);
    if (el_state == EL_ST_MODE) {
        fb_text(CCG_W - 4 - text_width("PICK MODE"), 0, "PICK MODE", true);
    } else {
        char buf[24];
        el_hud_right(buf, (int)sizeof(buf));
        fb_text(CCG_W - 4 - text_width(buf), 0, buf, true);
    }
    fb_hline(0, CCG_HUD_H - 1, CCG_W, true);

    /* ---- 模式选择 ---- */
    if (el_state == EL_ST_MODE) {
        char buf[20];
        unsigned n = 0;
        const char *s = "BEST ";
        fb_text_center(30, "NAME THE ELEMENT BY SYMBOL", true);
        el_row_inv(54, "1. 4-CHOICE  PICK FROM FOUR", el_sel == 0);
        el_row_inv(76, "2. TYPE-IN  SPELL THE NAME", el_sel == 1);
        fb_text_center(102, "RULES: 30 SEC, +10 PER RIGHT", true);
        while (*s && n < 18) buf[n++] = *s++;
        el_append_u32(buf, &n, el_best, 18);
        buf[n] = 0;
        fb_text_center(118, buf, true);
        fb_text_center(EL_HINT_Y, "UP/DOWN:CHOOSE  OK:START", true);
        return;
    }

    /* ---- 题目区: 符号 + 原子序数 ---- */
    el_draw_question();

    if (el_state == EL_ST_FBK) {
        el_draw_fbk();
    } else if (el_mode == 0) {
        el_draw_opts();
    } else {
        el_draw_blanks();
        el_draw_typed();
    }

    if (el_mode == 0)
        fb_text_center(EL_HINT_Y, "OK:PICK  P:PAUSE  N:NEW  Q:QUIT", true);
    else
        fb_text_center(EL_HINT_Y, "LETTERS+OK:GUESS  DEL:BKS  P:PAUSE  Q:QUIT", true);
}

void elements_enter(void) {
    el_state = EL_ST_MODE;
    el_sel = 0;
    el_over_full = false;
    el_seed_cnt++;
    rng_seed(&el_rng, (uint64_t)now_ms() ^ ((uint64_t)el_seed_cnt << 32) ^ 0xE1E2u);
    elements_render();
    disp_full();
}

void elements_exit(void) {}

void elements_tick(uint64_t now) {
    (void)now;
    if (el_state != EL_ST_PLAY && el_state != EL_ST_FBK) return;
    if (el_time_ms >= EL_TICK_MS) el_time_ms -= EL_TICK_MS; else el_time_ms = 0;
    if (el_time_ms == 0) {              /* 时间到 → 结算 */
        el_state = EL_ST_OVER;
        el_over_full = false;
        if (el_score > el_best) el_best = el_score;
        audio_lose();                   /* 超时 */
        led_fx_set(LED_FX_LOSE);
        return;
    }
    if (el_state == EL_ST_FBK && el_fbk_ticks > 0) {
        el_fbk_ticks--;
        if (el_fbk_ticks == 0) el_next_q();
    }
}

void elements_on_key(const key_event_t *ev) {
    switch (el_state) {
    case EL_ST_MODE:
        if (ev->is_repeat) {
            if (ev->key == K_UP || ev->key == K_DOWN) el_sel = 1 - el_sel;
            return;
        }
        switch (ev->key) {
        case K_UP:
        case K_DOWN:
            el_sel = 1 - el_sel;
            break;
        case K_OK:
            el_start(el_sel);
            break;
        case K_PAUSE:
        case K_BACK:
            el_pause();
            break;
        case K_QUIT:
            s_exit_request = true;
            break;
        default:
            break;
        }
        break;

    case EL_ST_PLAY:
        if (el_mode == 0) {
            if (ev->is_repeat) {        /* 4 选 1 方向键重复可响应 */
                if (ev->key == K_UP || ev->key == K_DOWN ||
                    ev->key == K_LEFT || ev->key == K_RIGHT)
                    el_move(ev->key);
                return;
            }
            switch (ev->key) {
            case K_UP:
            case K_DOWN:
            case K_LEFT:
            case K_RIGHT:
                el_move(ev->key);
                break;
            case K_OK:
                el_submit_idx(el_opts[el_cur]);
                break;
            case K_CHAR:
                if (ev->ch == 'n') el_start(el_mode);   /* N 新局 */
                break;
            case K_PAUSE:
            case K_BACK:
                el_pause();
                break;
            case K_QUIT:
                s_exit_request = true;
                break;
            default:
                break;
            }
        } else {
            if (ev->is_repeat) return;  /* 打字不接受重复 */
            switch (ev->key) {
            case K_CHAR:
                el_type(ev->ch);
                break;
            case K_DEL:
                if (el_typed_n > 0) el_typed_n--;
                break;
            case K_OK:
                el_submit_text();
                break;
            case K_PAUSE:
            case K_BACK:
                el_pause();
                break;
            case K_QUIT:
                s_exit_request = true;
                break;
            default:
                break;
            }
        }
        break;

    case EL_ST_FBK:
        if (ev->is_repeat) return;
        switch (ev->key) {
        case K_OK:                      /* 跳过反馈立即下一题 */
            el_next_q();
            break;
        case K_PAUSE:
        case K_BACK:
            el_pause();
            break;
        case K_QUIT:
            s_exit_request = true;
            break;
        default:
            break;
        }
        break;

    case EL_ST_OVER:
        if (ev->is_repeat) return;
        if (ev->key == K_OK || (ev->key == K_CHAR && ev->ch == 'n'))
            elements_enter();
        else if (ev->key == K_BACK || ev->key == K_QUIT)
            s_exit_request = true;
        else if (ev->key == K_PAUSE) {
            pause_sel_t sel;
            if (ui_pause_run(&sel)) {
                if (sel == PAUSE_RESTART) elements_enter();
            } else {
                s_exit_request = true;
            }
        }
        break;
    }
}
