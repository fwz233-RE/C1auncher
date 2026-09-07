/* WORDLE — 猜词游戏: 5 列 x 6 行, 200 词词表, 实体键盘字母直输
 * 格 48x16 (240x96 居中); 提交后着色: 反白=就位, 斜纹=错位, 素格=不在
 * 键位: 字母直输(自动进格), DEL 退格, OK 提交, BACK/P 暂停, Q 退出 */
#include "game.h"
#include "../config.h"
#include "../gfx/canvas.h"
#include "../gfx/font.h"
#include "../gfx/pattern.h"
#include "../platform/display.h"
#include "../platform/input.h"
#include "../platform/time.h"
#include "../platform/audio.h"
#include "../platform/led.h"
#include "../rng.h"
#include <string.h>

#define WD_ROWS 6
#define WD_LEN  5
#define WD_CELL_W 48
#define WD_CELL_H 16
#define WD_GRID_W (WD_LEN * WD_CELL_W)               /* 240 */
#define WD_GRID_H (WD_ROWS * WD_CELL_H)              /* 96  */
#define WD_GRID_X ((CCG_W - WD_GRID_W) / 2)          /* 28  */
#define WD_GRID_Y (CCG_HUD_H + (CCG_H - CCG_HUD_H - WD_GRID_H) / 2)  /* 36 */
#define WD_MSG_TICKS 2                               /* INVALID 提示 ~1.4s */

/* 内置词表 (200 x 5 字母, 全小写, 无重复) */
static const char wd_words[][WD_LEN + 1] = {
    "about", "above", "abuse", "actor", "acute", "admit", "adopt", "adult",
    "after", "again", "agent", "agree", "ahead", "alarm", "album", "alert",
    "alike", "alive", "allow", "alone", "along", "alter", "among", "anger",
    "angle", "angry", "ankle", "apart", "apple", "apply", "arena", "argue",
    "arise", "armor", "aroma", "array", "arrow", "aside", "asset", "audio",
    "audit", "avoid", "award", "aware", "awful", "bacon", "badge", "baker",
    "basic", "beach", "beard", "beast", "begin", "being", "belly", "below",
    "bench", "berry", "birth", "black", "blade", "blame", "blank", "blast",
    "blaze", "bleed", "blend", "bless", "blind", "block", "blood", "bloom",
    "bluff", "board", "boast", "bonus", "boost", "booth", "bound", "brain",
    "brand", "brave", "bread", "break", "breed", "brick", "bride", "brief",
    "bring", "broad", "broke", "brown", "brush", "build", "built", "bunch",
    "burst", "cabin", "cable", "candy", "cargo", "carry", "catch", "cause",
    "cease", "chain", "chair", "chalk", "chant", "chaos", "charm", "chart",
    "chase", "cheap", "check", "cheek", "cheer", "chess", "chest", "chief",
    "chill", "china", "choir", "choke", "chose", "chunk", "cigar", "claim",
    "clash", "class", "clean", "clear", "clerk", "click", "cliff", "climb",
    "cloak", "clock", "clone", "close", "cloth", "cloud", "clown", "coach",
    "coast", "color", "could", "count", "court", "cover", "crack", "craft",
    "crane", "crash", "cream", "creek", "creep", "crime", "crisp", "cross",
    "crowd", "crown", "crude", "cruel", "crush", "crust", "curve", "cycle",
    "daily", "dairy", "dance", "death", "debut", "delay", "dense", "depth",
    "devil", "diary", "digit", "dirty", "disco", "doubt", "dozen", "draft",
    "drain", "drama", "dream", "dress", "drink", "drive", "drunk", "early",
    "earth", "eager", "eagle", "elbow", "elder", "elect", "email", "empty",
};
#define WD_WORDS_N (sizeof(wd_words) / sizeof(wd_words[0]))

/* 已提交行: 字母 wd_g[行][列] + 颜色 0=灰 1=黄 2=绿; 当前行: wd_cur */
static char   wd_g[WD_ROWS][WD_LEN];
static uint8_t wd_col[WD_ROWS][WD_LEN];
static char   wd_cur[WD_LEN];
static int    wd_cur_len;         /* 当前行已输入字母数 0..5 */
static int    wd_row;             /* 已提交行数 0..6 */
static char   wd_ans[WD_LEN + 1]; /* 谜底 */
static bool   wd_over;
static bool   wd_won;
static bool   wd_over_full;       /* 结束全刷只做一次 */
static int    wd_msg_ticks;       /* INVALID 闪烁剩余 tick 数 */
static rng_t  wd_rng;
static uint64_t wd_seed_cnt;      /* 换局换种子 */

void wordle_render(void);

/* 词表成员判定 */
static bool wd_in_words(const char *w, size_t length) {
    unsigned i;
    if (length != WD_LEN) return false;
    for (i = 0; i < WD_WORDS_N; i++)
        if (memcmp(w, wd_words[i], WD_LEN) == 0) return true;
    return false;
}

/* 标准 Wordle 着色: 先绿后黄, 每字母按答案剩余次数限量 */
static void wd_score_row(void) {
    int avail[26] = { 0 };
    int i;
    for (i = 0; i < WD_LEN; i++) avail[wd_ans[i] - 'a']++;
    for (i = 0; i < WD_LEN; i++) {
        if (wd_cur[i] == wd_ans[i]) {
            wd_col[wd_row][i] = 2;
            avail[wd_cur[i] - 'a']--;
        }
    }
    for (i = 0; i < WD_LEN; i++) {
        int k = wd_cur[i] - 'a';
        if (wd_col[wd_row][i] == 0 && avail[k] > 0) {
            wd_col[wd_row][i] = 1;
            avail[k]--;
        }
    }
}

/* 从词表随机抽谜底 */
static void wd_pick_answer(void) {
    uint32_t idx = rng_range(&wd_rng, (uint32_t)WD_WORDS_N);
    memcpy(wd_ans, wd_words[idx], WD_LEN);
    wd_ans[WD_LEN] = 0;
}

/* 提交当前行: 不足 5 字母或不在词表 -> INVALID */
static void wd_submit(void) {
    if (wd_cur_len < WD_LEN) { wd_msg_ticks = WD_MSG_TICKS; audio_error(); return; }
    if (!wd_in_words(wd_cur, (size_t)wd_cur_len)) { wd_msg_ticks = WD_MSG_TICKS; audio_error(); return; }
    memcpy(wd_g[wd_row], wd_cur, WD_LEN);
    wd_score_row();
    if (memcmp(wd_cur, wd_ans, WD_LEN) == 0) {
        wd_over = true;
        wd_won = true;
        audio_win();
        led_fx_set(LED_FX_WIN);
    }
    wd_row++;
    wd_cur_len = 0;
    memset(wd_cur, 0, WD_LEN);
    if (wd_row >= WD_ROWS) {
        wd_over = true;
        wd_won = false;
        audio_lose();
        led_fx_set(LED_FX_LOSE);
    }
}

void wordle_enter(void) {
    int r, c;
    for (r = 0; r < WD_ROWS; r++)
        for (c = 0; c < WD_LEN; c++) {
            wd_g[r][c] = 0;
            wd_col[r][c] = 0;
        }
    memset(wd_cur, 0, WD_LEN);
    wd_cur_len = 0;
    wd_row = 0;
    wd_over = false;
    wd_won = false;
    wd_over_full = false;
    wd_msg_ticks = 0;
    wd_seed_cnt++;
    rng_seed(&wd_rng, (uint64_t)now_ms() ^ ((uint64_t)wd_seed_cnt << 32) ^ 0x57EULL);
    wd_pick_answer();
    wordle_render();
    disp_full();
}

void wordle_exit(void) {}

void wordle_tick(uint64_t now) {
    (void)now;
    if (wd_msg_ticks > 0) wd_msg_ticks--;   /* 归零后主循环重绘清除 INVALID */
}

/* "ANS:XXXXX" */
static void wd_answer_str(char *out) {
    out[0] = 'A'; out[1] = 'N'; out[2] = 'S'; out[3] = ':';
    memcpy(out + 4, wd_ans, WD_LEN);
    out[9] = 0;
}

/* 格内 2x 字母: 10x14 落在 48x16 格内居中 */
static void wd_draw_letter(int x, int y, char ch, bool inv) {
    char b[2];
    b[0] = ch;
    b[1] = 0;
    fb_text_scale2(x + (WD_CELL_W - 10) / 2, y + (WD_CELL_H - 14) / 2, b, !inv);
}

/* 单格: kind 0=灰/未定 1=黄(斜纹) 2=绿(反白); active=光标格 */
static void wd_draw_cell(int col, int row, char ch, int kind, bool active) {
    int x = WD_GRID_X + col * WD_CELL_W;
    int y = WD_GRID_Y + row * WD_CELL_H;
    if (kind == 2) {
        fb_fill_rect(x, y, WD_CELL_W, WD_CELL_H, true);
    } else if (kind == 1) {
        fb_fill_tile(x, y, WD_CELL_W, WD_CELL_H, pat_get(PAT_SLASH_D));
        fb_stroke_rect(x, y, WD_CELL_W, WD_CELL_H, true);
    } else if (active) {
        fb_fill_rect(x, y, WD_CELL_W, WD_CELL_H, true);
    } else {
        fb_stroke_rect(x, y, WD_CELL_W, WD_CELL_H, true);
    }
    if (ch) wd_draw_letter(x, y, ch, kind == 2 || active);
}

void wordle_render(void) {
    int r, c;
    fb_clear(false);

    /* HUD 顶栏: 左标题黑字, 右 TRY n/6 (黑字白底) */
    fb_text_scale2(2, 1, "WORDLE", true);
    if (wd_msg_ticks > 0) {
        fb_text_inv(CCG_W - 2 - text_width("INVALID"), 2, "INVALID");
    } else if (!wd_over) {
        char buf[8];
        int n = wd_row + 1;               /* 下一尝试 */
        if (n < 1) n = 1;
        if (n > WD_ROWS) n = WD_ROWS;
        buf[0] = 'T'; buf[1] = 'R'; buf[2] = 'Y'; buf[3] = ' ';
        buf[4] = (char)('0' + n);
        buf[5] = '/'; buf[6] = '6'; buf[7] = 0;
        fb_text_scale2(CCG_W - 2 - text_width(buf) * 2, 1, buf, true);
    }
    fb_hline(0, CCG_HUD_H - 1, CCG_W, true);

    /* 格子: 已提交行按颜色, 当前行按输入, 光标格黑底 */
    for (r = 0; r < WD_ROWS; r++) {
        for (c = 0; c < WD_LEN; c++) {
            char ch = 0;
            int kind = 0;
            bool active = false;
            if (r < wd_row) {
                ch = wd_g[r][c];
                kind = wd_col[r][c];
            } else if (r == wd_row && !wd_over) {
                int ac = wd_cur_len < WD_LEN ? wd_cur_len : WD_LEN - 1;
                ch = (c < wd_cur_len) ? wd_cur[c] : 0;
                active = (c == ac);
            }
            wd_draw_cell(c, r, ch, kind, active);
        }
    }
    /* 光标(黑格白边)在所有格子画完之后再画 */
    if (!wd_over) {
        int ac = wd_cur_len < WD_LEN ? wd_cur_len : WD_LEN - 1;
        int x = WD_GRID_X + ac * WD_CELL_W;
        int y = WD_GRID_Y + wd_row * WD_CELL_H;
        fb_stroke_rect_thick(x + 1, y + 1, WD_CELL_W - 2, WD_CELL_H - 2, 2, false);
    }

    /* 结束: HUD 两行 结果 + 操作提示; 墙内不放文字 */
    if (wd_over) {
        fb_fill_rect(0, 0, CCG_W, CCG_HUD_H - 1, false);
        if (wd_won) {
            char b[16];
            int n = wd_row;                 /* 猜中时的尝试数 */
            if (n < 1) n = 1;
            b[0] = 'Y'; b[1] = 'O'; b[2] = 'U'; b[3] = ' ';
            b[4] = 'W'; b[5] = 'I'; b[6] = 'N'; b[7] = '!';
            b[8] = ' ';
            b[9] = (char)('0' + n);
            b[10] = '/'; b[11] = '6';
            b[12] = 0;
            fb_text(2, 1, b, true);
        } else {
            char b[16];
            wd_answer_str(b);
            fb_text(2, 1, "YOU LOSE", true);
            fb_text(2, 9, b, true);
        }
        fb_text(CCG_W - 2 - text_width("OK/N:RETRY BACK:QUIT"), 1,
                "OK/N:RETRY BACK:QUIT", true);
        if (!wd_over_full) {
            wd_over_full = true;
            disp_force_full();
        }
    }
}

void wordle_on_key(const key_event_t *ev) {
    if (ev->is_repeat && ev->key != K_DEL) return;   /* 长按 DEL 连续退格 */
    if (wd_over) {
        if (ev->key == K_OK || (ev->key == K_CHAR && ev->ch == 'n'))
            wordle_enter();
        else if (ev->key == K_BACK || ev->key == K_QUIT)
            s_exit_request = true;
        return;
    }
    wd_msg_ticks = 0;                               /* 任何输入清除提示 */
    switch (ev->key) {
    case K_CHAR: {
        char ch = (char)ev->ch;
        if (ch >= 'A' && ch <= 'Z') ch = (char)(ch - 'A' + 'a');   /* 大小写统一 */
        if (ch >= 'a' && ch <= 'z' && wd_cur_len < WD_LEN)
            wd_cur[wd_cur_len++] = ch;
        break;
    }
    case K_DEL:
        if (wd_cur_len > 0) wd_cur[--wd_cur_len] = 0;
        break;
    case K_OK:
        wd_submit();
        break;
    case K_PAUSE:
    case K_BACK: {
        pause_sel_t sel;
        if (ui_pause_run(&sel)) {
            if (sel == PAUSE_RESTART) wordle_enter();
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
