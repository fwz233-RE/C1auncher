/* BOGGLE — 4x4 字母阵找词(60 秒限时)
 * 方向/WASD 移动光标, OK 选字母(须与上一个相邻, 含对角; 同一格不可重复),
 * 再按 OK(在首字母上)或 SPACE 提交: 词长>=3 且命中内置词库即计分
 *   (3 字母=1 分, 4=2, 5=3, 6+=5), 重复词不重复计分
 * BACK/DEL 撤销当前词最后一位; N 新局, P 暂停, Q 退出
 * HUD 顶栏: 左 BOGGLE(2x), 右 SCORE n / TIME s 两行(黑字白底)
 * 棋盘 4x4 格 30px 居中(120x120 最大化), 格上方 2x 行实时显示当前词/消息
 * 时间到: HUD 两行结果 + 棋盘区显示全部找到的词
 * 静态前缀 bo_; 像素坐标一律 int; 零 malloc
 *
 * 集成提示(help[] 最多 5 行):
 *   "BOGGLE - FIND WORDS IN 4X4 GRID",
 *   "ARROWS/WASD: MOVE  OK: PICK",
 *   "NEXT LETTER MUST BE ADJACENT",
 *   "SPACE: SUBMIT WORD (>=3 LETTERS)",
 *   "3=1P 4=2P 5=3P 6+=5P  N:NEW"
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
#include <string.h>

#define BO_ROWS 4
#define BO_COLS 4
#define BO_CELL 30                  /* 30px 正方格 */
#define BO_GRID_W (BO_CELL * BO_COLS)   /* 120 */
#define BO_GRID_X ((CCG_W - BO_GRID_W) / 2)  /* 水平居中 88 */
#define BO_GRID_Y 32                /* HUD + 词行之后, 底到 152 */
#define BO_WORD_Y 17                /* 2x 当前词/消息行 */
#define BO_MAX_LEN 16               /* 路径最长 16 格 */
#define BO_TICK_MS 100u
#define BO_GAME_MS 60000u           /* 60 秒限时 */
#define BO_MSG_TTL 12               /* 消息停留 1.2s */
#define BO_FOUND_CAP 32             /* 已找到词表上限 */

/* 内置词库 (100 词, 3-7 字母, 全小写, 无重复, 仅用字母池内字母) */
static const char bo_dict[100][8] = {
    /* 3 字母 x40 */
    "ace", "act", "age", "aid", "air", "all", "and", "are", "arm", "art",
    "ask", "ate", "bad", "bag", "ban", "bar", "bat", "bed", "bee", "big",
    "bin", "bit", "bow", "bog", "bud", "bun", "bus", "but", "cab", "can",
    "cap", "car", "cat", "cup", "dam", "den", "did", "dig", "dog", "dot",
    /* 4 字母 x28 */
    "able", "acid", "also", "area", "band", "bark", "barn", "bath",
    "bead", "beam", "bean", "bear", "beat", "belt", "bent", "bird",
    "bite", "blow", "blue", "boat", "bold", "bone", "book", "boot",
    "bore", "born", "both", "bowl",
    /* 5 字母 x18 */
    "about", "after", "again", "apple", "baker", "beach", "beard",
    "begin", "black", "blank", "blind", "block", "board", "brain",
    "brave", "bread", "break", "bring",
    /* 6 字母 x10 */
    "absent", "accept", "across", "action", "admire",
    "basket", "better", "bother", "branch", "bridge",
    /* 7 字母 x4 */
    "balance", "banner", "beacon", "beaver",
};
#define BO_DICT_N (int)(sizeof(bo_dict) / sizeof(bo_dict[0]))

/* ---- 对局状态 ---- */
static char     bo_board[16];            /* 4x4 字母(row*4+col) */
static int      bo_cur_r, bo_cur_c;      /* 光标 0-3 */
static char     bo_word[BO_MAX_LEN + 1]; /* 当前选中字母串 */
static uint8_t  bo_path_r[BO_MAX_LEN], bo_path_c[BO_MAX_LEN]; /* 选中路径 */
static int      bo_len;                  /* 已选字母数 */
static uint32_t bo_elapsed;              /* 已进行 ms */
static uint32_t bo_score;
static char     bo_found[BO_FOUND_CAP][8]; /* 已找到的词(去重) */
static int      bo_found_n;
static char     bo_msg[24];              /* 提交结果消息 */
static int      bo_msg_ttl;              /* 消息剩余 tick */
static bool     bo_over;
static bool     bo_over_full;            /* 结束全刷只做一次 */
static rng_t    bo_rng;
static uint32_t bo_gens;                 /* 换局计数(种子混合) */
static int      bo_find_steps;           /* 可摆放性检查步数预算 */

void boggle_render(void);

/* 十进制整数写缓冲, 返回长度(v<=0 视为 0) */
static int bo_itoa(char *b, int v) {
    char t[12];
    int i = 0, j;
    if (v <= 0) { b[0] = '0'; b[1] = 0; return 1; }
    while (v > 0 && i < 10) { t[i++] = (char)('0' + v % 10); v /= 10; }
    for (j = 0; j < i; j++) b[j] = t[i - 1 - j];
    b[i] = 0;
    return i;
}

/* 随机字母: 元音加权(15 元音 vs 24 辅音, 约 38% 元音); 全小写与词库一致 */
static char bo_pick_letter(void) {
    static const char bo_vowels[] = "aaaeeeeiiooouu";      /* 15 */
    static const char bo_cons[] = "nnnrrrssstttllddbbcmfghkpvwj"; /* 24 */
    const uint32_t nv = (uint32_t)sizeof(bo_vowels) - 1;
    const uint32_t nc = (uint32_t)sizeof(bo_cons) - 1;
    if (rng_range(&bo_rng, nv + nc) < nv)
        return bo_vowels[rng_range(&bo_rng, nv)];
    return bo_cons[rng_range(&bo_rng, nc)];
}

/* DFS 检查词 w 是否可由棋盘拼出(相邻含对角, 同格一次) */
static bool bo_dfs_word(const char *w, int r, int c, int idx, uint32_t used) {
    int dr, dc;
    if (--bo_find_steps <= 0) return false;   /* 步数预算防极端棋盘超时 */
    if (bo_board[r * BO_COLS + c] != w[idx]) return false;
    if (w[idx + 1] == '\0') return true;
    used |= 1u << (uint32_t)(r * BO_COLS + c);
    for (dr = -1; dr <= 1; dr++) {
        for (dc = -1; dc <= 1; dc++) {
            int nr = r + dr, nc = c + dc;
            if (dr == 0 && dc == 0) continue;
            if (nr < 0 || nr >= BO_ROWS || nc < 0 || nc >= BO_COLS) continue;
            if (used & (1u << (uint32_t)(nr * BO_COLS + nc))) continue;
            if (bo_dfs_word(w, nr, nc, idx + 1, used)) return true;
        }
    }
    return false;
}

/* 棋盘上是否至少存在一个词库词(保证对局可玩) */
static bool bo_board_findable(void) {
    int wi, r, c;
    bo_find_steps = 20000;
    for (wi = 0; wi < BO_DICT_N; wi++)
        for (r = 0; r < BO_ROWS; r++)
            for (c = 0; c < BO_COLS; c++)
                if (bo_dfs_word(bo_dict[wi], r, c, 0, 0u)) return true;
    return false;
}

/* 生成棋盘; 无词可拼则重洗(guard 限次, 不会死循环) */
static void bo_new_board(void) {
    int guard;
    for (guard = 0; guard < 8; guard++) {
        int i;
        for (i = 0; i < BO_ROWS * BO_COLS; i++) bo_board[i] = bo_pick_letter();
        if (bo_board_findable()) return;   /* 至少一个词库词可拼出 */
    }
    /* 兜底: 8 次后仍不可拼则接受最后一版(概率极低) */
}

/* 剩余秒数(未结束时恒 >0) */
static int bo_time_left(void) {
    return (int)((BO_GAME_MS - bo_elapsed) / 1000u);
}

static void bo_set_msg(const char *s) {
    size_t n = strlen(s);
    if (n >= sizeof(bo_msg)) n = sizeof(bo_msg) - 1;
    memcpy(bo_msg, s, n);
    bo_msg[n] = 0;
    bo_msg_ttl = BO_MSG_TTL;
}

static void bo_clear_word(void) {
    bo_len = 0;
    bo_word[0] = 0;
}

static int bo_score_of(int len) {
    if (len >= 6) return 5;
    if (len == 5) return 3;
    if (len == 4) return 2;
    return 1;   /* 3 */
}

/* 提交当前词: 词库查重 + 重复词拦截, 命中计分 */
static void bo_submit(void) {
    int i, off, x;
    char b[24], m[4];
    if (bo_over) return;
    if (bo_len < 3) { bo_set_msg("TOO SHORT"); bo_clear_word(); return; }
    bo_word[bo_len] = 0;
    for (i = 0; i < BO_DICT_N; i++)
        if (strcmp(bo_dict[i], bo_word) == 0) break;
    if (i >= BO_DICT_N) {
        bo_set_msg("NOT A WORD");
        bo_clear_word();
        audio_error();
        return;
    }
    for (i = 0; i < bo_found_n; i++)
        if (strcmp(bo_found[i], bo_word) == 0) {
            bo_set_msg("ALREADY");
            bo_clear_word();
            return;
        }
    if (bo_found_n < BO_FOUND_CAP) {
        strcpy(bo_found[bo_found_n], bo_word);
        bo_found_n++;
    }
    bo_score += (uint32_t)bo_score_of(bo_len);
    audio_clear();                   /* 命中词库计分 */
    bo_itoa(m, bo_score_of(bo_len));
    off = 0;
    for (x = 0; bo_word[x] && off < 20; x++) b[off++] = bo_word[x];
    b[off++] = '+';
    for (x = 0; m[x]; x++) b[off++] = m[x];
    b[off] = 0;
    bo_set_msg(b);
    bo_clear_word();
}

/* 移动光标(环绕) */
static void bo_move_cursor(int dr, int dc) {
    if (bo_over) return;
    bo_cur_r = (bo_cur_r + dr + BO_ROWS) % BO_ROWS;
    bo_cur_c = (bo_cur_c + dc + BO_COLS) % BO_COLS;
}

/* 选中 (r,c): 首字母可直接选; 之后必须与上一个相邻且未用过;
 * 词长>=3 时在首字母上按 OK 等同提交 */
static void bo_select_cell(int r, int c) {
    int i;
    if (bo_over) return;
    if (bo_len >= 3 && r == bo_path_r[0] && c == bo_path_c[0]) {
        bo_submit();
        return;
    }
    for (i = 0; i < bo_len; i++)
        if (bo_path_r[i] == r && bo_path_c[i] == c) return;   /* 同格不重复 */
    if (bo_len > 0) {
        int dr = r - bo_path_r[bo_len - 1];
        int dc = c - bo_path_c[bo_len - 1];
        if (dr < -1 || dr > 1 || dc < -1 || dc > 1) return;   /* 必须相邻 */
        if (dr == 0 && dc == 0) return;
    }
    if (bo_len >= BO_MAX_LEN) return;
    bo_path_r[bo_len] = (uint8_t)r;
    bo_path_c[bo_len] = (uint8_t)c;
    bo_word[bo_len] = bo_board[r * BO_COLS + c];
    bo_len++;
    bo_word[bo_len] = 0;
    audio_select();                  /* 选中字母 */
}

/* 撤销最后一位, 光标回到该格 */
static void bo_undo(void) {
    if (bo_over || bo_len == 0) return;
    bo_len--;
    bo_cur_r = bo_path_r[bo_len];
    bo_cur_c = bo_path_c[bo_len];
    bo_word[bo_len] = 0;
}

static void bo_new_game(void) {
    bo_gens++;
    rng_seed(&bo_rng, (uint64_t)now_ms() ^ ((uint64_t)bo_gens * 0x9E3779B1u));
    bo_new_board();
    bo_cur_r = 0;
    bo_cur_c = 0;
    bo_clear_word();
    bo_elapsed = 0;
    bo_score = 0;
    bo_found_n = 0;
    bo_msg[0] = 0;
    bo_msg_ttl = 0;
    bo_over = false;
    bo_over_full = false;
    boggle_render();
    disp_full();
}

void boggle_enter(void) { bo_new_game(); }

void boggle_exit(void) {}

void boggle_tick(uint64_t now) {
    (void)now;
    if (bo_over) return;
    bo_elapsed += BO_TICK_MS;
    if (bo_msg_ttl > 0) bo_msg_ttl--;
    if (bo_elapsed >= BO_GAME_MS) {
        bo_over = true;             /* 时间到 → 结算 */
        bo_clear_word();
        bo_msg_ttl = 0;
        audio_lose();               /* 超时结束 */
        led_fx_set(LED_FX_LOSE);
    }
}

void boggle_render(void) {
    int r, c, i;
    fb_clear(false);

    if (bo_over) {
        /* HUD 两行结果 + 右上重试/退出提示 */
        char b[24];
        int n = 0;
        fb_fill_rect(0, 0, CCG_W, CCG_HUD_H - 1, false);
        fb_text(2, 0, "GAME OVER", true);
        memcpy(b, "SCORE ", 6);
        n = 6;
        n += bo_itoa(b + n, (int)bo_score);
        memcpy(b + n, "  WORDS ", 8);
        n += 8;
        n += bo_itoa(b + n, bo_found_n);
        b[n] = 0;
        fb_text(2, 8, b, true);
        fb_text(CCG_W - 2 - text_width("OK/N:RETRY BACK:QUIT"), 0,
                "OK/N:RETRY BACK:QUIT", true);
        fb_hline(0, CCG_HUD_H - 1, CCG_W, true);
        /* 找到的词单(5x7 贪心换行铺满棋盘区) */
        fb_fill_rect(0, CCG_HUD_H, CCG_W, CCG_H - CCG_HUD_H, false);
        {
            int x = 4, y = 20;
            for (i = 0; i < bo_found_n; i++) {
                int w = text_width(bo_found[i]);
                if (x + w > (int)CCG_W - 6) { x = 4; y += 8; }
                fb_text(x, y, bo_found[i], true);
                x += w + 8;
            }
        }
        if (!bo_over_full) {
            bo_over_full = true;
            disp_force_full();
        }
        return;
    }

    /* HUD: 左标题, 右 SCORE/TIME 两行(黑字白底) */
    fb_text_scale2(2, 1, "BOGGLE", true);
    {
        char b[16];
        int n = 0;
        memcpy(b, "SCORE ", 6);
        n = 6;
        n += bo_itoa(b + n, (int)bo_score);
        b[n] = 0;
        fb_text(CCG_W - 2 - text_width(b), 0, b, true);
        n = 0;
        memcpy(b, "TIME ", 5);
        n = 5;
        n += bo_itoa(b + n, bo_time_left());
        b[n] = 0;
        fb_text(CCG_W - 2 - text_width(b), 8, b, true);
    }
    fb_hline(0, CCG_HUD_H - 1, CCG_W, true);

    /* 当前词/消息(2x 居中), 空词时给操作提示 */
    if (bo_msg_ttl > 0) {
        fb_text_scale2((CCG_W - text_width(bo_msg) * 2) / 2, BO_WORD_Y,
                       bo_msg, true);
    } else if (bo_len > 0) {
        fb_text_scale2((CCG_W - text_width(bo_word) * 2) / 2, BO_WORD_Y,
                       bo_word, true);
    } else {
        fb_text_center(24, "OK:PICK  SPACE:GO", true);
    }

    /* 棋盘: 已选格黑底白字高亮 */
    for (r = 0; r < BO_ROWS; r++) {
        for (c = 0; c < BO_COLS; c++) {
            int x = BO_GRID_X + c * BO_CELL;
            int y = BO_GRID_Y + r * BO_CELL;
            bool sel = false;
            char ch[2];
            ch[0] = bo_board[r * BO_COLS + c];
            ch[1] = 0;
            for (i = 0; i < bo_len; i++)
                if (bo_path_r[i] == r && bo_path_c[i] == c) { sel = true; break; }
            if (sel) fb_fill_rect(x, y, BO_CELL, BO_CELL, true);
            fb_stroke_rect(x, y, BO_CELL, BO_CELL, true);
            fb_text_scale2(x + (BO_CELL - 10) / 2, y + (BO_CELL - 14) / 2,
                           ch, !sel);
        }
    }
    /* 光标白边最后画(四周对称) */
    {
        int x = BO_GRID_X + bo_cur_c * BO_CELL;
        int y = BO_GRID_Y + bo_cur_r * BO_CELL;
        fb_stroke_rect_thick(x + 1, y + 1, BO_CELL - 2, BO_CELL - 2, 2, false);
    }
}

void boggle_on_key(const key_event_t *ev) {
    if (bo_over) {
        if (ev->is_repeat) return;
        if (ev->key == K_OK || (ev->key == K_CHAR && ev->ch == 'n'))
            bo_new_game();
        else if (ev->key == K_BACK || ev->key == K_QUIT)
            s_exit_request = true;
        return;
    }
    /* 长按重复只响应方向移动 */
    if (ev->is_repeat) {
        switch (ev->key) {
        case K_UP: bo_move_cursor(-1, 0); break;
        case K_DOWN: bo_move_cursor(1, 0); break;
        case K_LEFT: bo_move_cursor(0, -1); break;
        case K_RIGHT: bo_move_cursor(0, 1); break;
        case K_CHAR:
            if (ev->ch == 'w') bo_move_cursor(-1, 0);
            else if (ev->ch == 's') bo_move_cursor(1, 0);
            else if (ev->ch == 'a') bo_move_cursor(0, -1);
            else if (ev->ch == 'd') bo_move_cursor(0, 1);
            break;
        default:
            break;
        }
        return;
    }
    switch (ev->key) {
    case K_UP: bo_move_cursor(-1, 0); break;
    case K_DOWN: bo_move_cursor(1, 0); break;
    case K_LEFT: bo_move_cursor(0, -1); break;
    case K_RIGHT: bo_move_cursor(0, 1); break;
    case K_OK:
        bo_select_cell(bo_cur_r, bo_cur_c);
        break;
    case K_SPACE:
        bo_submit();
        break;
    case K_DEL:
    case K_BACK:
        bo_undo();
        break;
    case K_CHAR:
        switch (ev->ch) {
        case 'w': bo_move_cursor(-1, 0); break;
        case 's': bo_move_cursor(1, 0); break;
        case 'a': bo_move_cursor(0, -1); break;
        case 'd': bo_move_cursor(0, 1); break;
        case 'n': bo_new_game(); break;
        default: break;
        }
        break;
    case K_PAUSE: {
        pause_sel_t sel;
        if (ui_pause_run(&sel)) {
            if (sel == PAUSE_RESTART) bo_new_game();
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
