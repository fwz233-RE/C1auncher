/* GOLF SOLITAIRE — 高尔夫纸牌
 * 5 列 x 5 行牌山(第一行翻开, 其余背面) + 发牌堆 + 废牌堆
 * 规则: 废牌堆顶与列顶点数相邻(差 1 或 A-K 环绕)则该列顶牌移入废牌堆;
 *       发牌堆(BACK)翻新废牌; 空列可接收废牌堆顶任意牌;
 *       清空牌山 = WIN; 发牌堆耗尽且无任何可动 = FAIL。
 * 操作: 方向/WASD 选列, OK 移牌, BACK 翻发牌堆, P 暂停, N 新局, Q 退出。
 * 注: 本游戏设计规定 BACK = 翻发牌堆(核心操作), 暂停由 P 键触发。
 */
#include "game.h"
#include "../config.h"
#include "../gfx/canvas.h"
#include "../gfx/font.h"
#include "../gfx/pattern.h"
#include "../platform/display.h"
#include "../platform/input.h"
#include "../platform/audio.h"
#include "../platform/led.h"
#include "../rng.h"
#include "../platform/time.h"

#define GO_COLS 5
#define GO_ROWS 5
#define GO_CW 26                /* 牌宽 */
#define GO_CH 34                /* 牌高 */
#define GO_PITCH_X 46           /* 列距 */
#define GO_PITCH_Y 24           /* 行距(堆叠露出高度) */
#define GO_TAB_X 86             /* 牌山左缘 */
#define GO_TAB_Y 20             /* 牌山顶行 */
#define GO_PILE_X 8             /* 发牌堆 x */
#define GO_PILE_Y 20            /* 堆 y */
#define GO_WASTE_X 44           /* 废牌堆 x */
#define GO_WASTE_Y GO_PILE_Y
#define GO_STOCK_MAX 26         /* 52 - 25 牌山 - 1 开局废牌 */
#define GO_WASTE_MAX 52
#define GO_TAB_CARDS (GO_COLS * GO_ROWS)

/* 牌编码: 低 4 位 = 点数 1..13(A..K), 高 4 位 = 花色 0..3 (0=CLUB...) */
#define GO_RANK(c) ((int)((c) & 0x0F))
#define GO_SUIT(c) ((int)(((c) >> 4) & 3))

static uint8_t go_tab[GO_COLS][GO_ROWS];  /* 0 = 空位; r=0 恒为列顶(翻开) */
static uint8_t go_cols[GO_COLS];          /* 每列张数 */
static uint8_t go_stock[GO_STOCK_MAX];    /* 发牌堆(索引 0 = 顶) */
static int go_stock_cnt;
static uint8_t go_waste[GO_WASTE_MAX];    /* 废牌堆(顶 = 最后元素) */
static int go_waste_cnt;
static int go_sel;                        /* 选中列 0..4 */
static uint32_t go_moves;
static bool go_over;                      /* 终局(WIN 或 FAIL) */
static bool go_win;
static bool go_over_full;                 /* 终局全刷防重复 */
static rng_t go_rng;

static const int go_sym[4] = { CG_CLUB, CG_DIAMOND, CG_HEART, CG_SPADE };

void golf_render(void);

/* 点数相邻: 差 1 或 K-A 环绕 */
static bool go_adj(int a, int b) {
    int d = a > b ? a - b : b - a;
    return d == 1 || d == 12;
}

static uint8_t go_tab_top(int c) { return go_tab[c][0]; }

static uint8_t go_waste_top(void) { return go_waste[go_waste_cnt - 1]; }

/* 洗牌发牌: 废牌堆 1 张(经典开局), 牌山 25 张(第一行翻开), 发牌堆 26 张 */
static void go_deal(void) {
    uint8_t deck[52];
    for (int i = 0; i < 52; i++)
        deck[i] = (uint8_t)(i / 4 + 1) | (uint8_t)((i % 4) << 4);
    for (int i = 51; i > 0; i--) {
        uint32_t j = rng_range(&go_rng, (uint32_t)i + 1);
        uint8_t t = deck[i];
        deck[i] = deck[j];
        deck[j] = t;
    }
    go_waste_cnt = 1;
    go_waste[0] = deck[0];
    for (int i = 0; i < GO_TAB_CARDS; i++) {
        go_tab[i % GO_COLS][i / GO_COLS] = deck[1 + i];
        go_cols[i % GO_COLS] = (uint8_t)(i / GO_COLS + 1);
    }
    go_stock_cnt = GO_STOCK_MAX;
    for (int i = 0; i < GO_STOCK_MAX; i++) go_stock[i] = deck[51 - i];
}

/* 列顶 → 废牌堆: 废牌堆空可入任意, 否则必须点数相邻 */
static bool go_try_col_to_waste(int c) {
    if (go_cols[c] == 0) return false;
    uint8_t card = go_tab_top(c);
    if (go_waste_cnt > 0 && !go_adj(GO_RANK(card), GO_RANK(go_waste_top())))
        return false;
    go_waste[go_waste_cnt++] = card;
    go_cols[c]--;
    if (go_cols[c] > 0) {
        for (int r = 0; r + 1 < GO_ROWS; r++) go_tab[c][r] = go_tab[c][r + 1];
        go_tab[c][GO_ROWS - 1] = 0;
    }
    return true;
}

/* 废牌堆顶 → 空列(列空可放任意) */
static bool go_try_waste_to_col(int c) {
    if (go_cols[c] != 0 || go_waste_cnt == 0) return false;
    go_tab[c][0] = go_waste_top();
    go_cols[c] = 1;
    go_waste_cnt--;
    return true;
}

/* 发牌堆顶 → 废牌堆 */
static bool go_draw(void) {
    if (go_stock_cnt <= 0) return false;
    go_waste[go_waste_cnt++] = go_stock[0];
    go_stock_cnt--;
    for (int i = 0; i < go_stock_cnt; i++) go_stock[i] = go_stock[i + 1];
    return true;
}

/* 扫描是否存在任何合法行动 */
static bool go_any_move(void) {
    for (int c = 0; c < GO_COLS; c++) {
        if (go_cols[c] == 0) {
            if (go_waste_cnt > 0) return true;   /* 空列可接废牌堆顶 */
            continue;
        }
        if (go_waste_cnt == 0) return true;      /* 废牌堆空: 任意列顶可入 */
        if (go_adj(GO_RANK(go_tab_top(c)), GO_RANK(go_waste_top()))) return true;
    }
    return false;
}

static void go_check_end(void) {
    if (go_over) return;
    int left = 0;
    for (int c = 0; c < GO_COLS; c++) left += go_cols[c];
    if (left == 0) {
        go_over = true;
        go_win = true;
    } else if (go_stock_cnt == 0 && !go_any_move()) {
        go_over = true;
        go_win = false;
    }
}

static const char *go_rank_str(int r) {
    static char buf[3];
    if (r == 1) return "A";
    if (r == 11) return "J";
    if (r == 12) return "Q";
    if (r == 13) return "K";
    if (r < 10) {
        buf[0] = (char)('0' + r);
        buf[1] = 0;
    } else {
        buf[0] = '1';
        buf[1] = '0';
        buf[2] = 0;
    }
    return buf;
}

static int go_itoa(uint32_t v, char *buf) {
    char tmp[12];
    int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v && n < 11) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
    for (int i = 0; i < n; i++) buf[i] = tmp[n - 1 - i];
    buf[n] = 0;
    return n;
}

/* 3x 放大符号(26px 牌内清晰) */
static void go_draw_sym3(int x, int y, int idx, bool black) {
    const uint8_t *g = font_symbols[idx];
    for (int j = 0; j < FONT_H; j++)
        for (int i = 0; i < FONT_W; i++)
            if (g[j] & (1u << i))
                fb_fill_rect(x + i * 3, y + j * 3, 3, 3, black);
}

/* 牌正面: 白底 + 左上点数 + 中央大花色 */
static void go_draw_face(int x, int y, uint8_t card) {
    fb_fill_rect(x, y, GO_CW, GO_CH, false);
    fb_stroke_rect(x, y, GO_CW, GO_CH, true);
    fb_text(x + 2, y + 1, go_rank_str(GO_RANK(card)), true);
    go_draw_sym3(x + (GO_CW - 3 * FONT_W) / 2, y + (GO_CH - 3 * FONT_H) / 2,
                 go_sym[GO_SUIT(card)], true);
}

/* 牌背面: 白底 + 斜纹 + 边框 */
static void go_draw_back(int x, int y) {
    fb_fill_rect(x, y, GO_CW, GO_CH, false);
    fb_fill_tile(x + 1, y + 1, GO_CW - 2, GO_CH - 2, pat_get(PAT_SLASH_S));
    fb_stroke_rect(x, y, GO_CW, GO_CH, true);
}

static void go_draw_empty_pile(int x, int y) {
    fb_stroke_rect(x, y, GO_CW, GO_CH, true);
    fb_stroke_rect(x + 3, y + 3, GO_CW - 6, GO_CH - 6, true);
}

void golf_enter(void) {
    rng_seed(&go_rng, now_ms() ^ 0x51A7u);
    go_deal();
    go_sel = GO_COLS / 2;
    go_moves = 0;
    go_over = false;
    go_win = false;
    go_over_full = false;
    golf_render();
    disp_full();
}

void golf_exit(void) {}

void golf_tick(uint64_t now) { (void)now; }

void golf_render(void) {
    fb_clear(false);
    go_check_end();   /* 无输入时也在下一帧呈现 WIN/FAIL(发牌堆耗尽无法动) */

    /* 发牌堆(背面 + 余量) */
    if (go_stock_cnt > 0) go_draw_back(GO_PILE_X, GO_PILE_Y);
    else go_draw_empty_pile(GO_PILE_X, GO_PILE_Y);
    fb_text(GO_PILE_X, GO_PILE_Y + GO_CH + 4, "STOCK", true);
    {
        char cnt[4];
        go_itoa((uint32_t)go_stock_cnt, cnt);
        fb_text(GO_PILE_X, GO_PILE_Y + GO_CH + 14, cnt, true);
    }

    /* 废牌堆(顶牌正面, 空则空框) */
    if (go_waste_cnt > 0) go_draw_face(GO_WASTE_X, GO_WASTE_Y, go_waste_top());
    else go_draw_empty_pile(GO_WASTE_X, GO_WASTE_Y);
    fb_text(GO_WASTE_X, GO_WASTE_Y + GO_CH + 4, "WASTE", true);

    /* 牌山: 第一行翻开, 其余背面, 逐列向下堆叠 */
    for (int c = 0; c < GO_COLS; c++) {
        for (int r = 0; r < go_cols[c]; r++) {
            int x = GO_TAB_X + c * GO_PITCH_X;
            int y = GO_TAB_Y + r * GO_PITCH_Y;
            if (r == 0) go_draw_face(x, y, go_tab[c][r]);
            else go_draw_back(x, y);
        }
    }

    /* 光标: 选中列顶牌(或空列位)四周对称黑框, 最后绘制 */
    fb_stroke_rect_thick(GO_TAB_X + go_sel * GO_PITCH_X - 2, GO_TAB_Y - 2,
                         GO_CW + 4, GO_CH + 4, 2, true);

    /* HUD: 左标题, 右 MOVES */
    fb_text(0, 0, "GOLF", true);
    {
        char num[12];
        char full[20];
        go_itoa(go_moves, num);
        const char *lab = "MOVES ";
        unsigned n = 0;
        while (lab[n]) { full[n] = lab[n]; n++; }
        unsigned len = 0;
        while (num[len]) { full[n++] = num[len]; len++; }
        full[n] = 0;
        fb_text(CCG_W - 2 - text_width(full), 0, full, true);
    }
    fb_hline(0, CCG_HUD_H - 1, CCG_W, true);

    if (go_over) {
        fb_fill_rect(0, 0, CCG_W, CCG_HUD_H - 1, false);
        fb_text(2, 2, go_win ? "WIN!" : "NO MOVES", true);
        fb_text(CCG_W - 2 - text_width("OK/N:RETRY BACK:QUIT"), 2,
                "OK/N:RETRY BACK:QUIT", true);
        if (!go_over_full) { go_over_full = true; disp_force_full(); }
    }
}

void golf_on_key(const key_event_t *ev) {
    if (go_over) {
        if (ev->is_repeat) return;
        if (ev->key == K_OK || (ev->key == K_CHAR && ev->ch == 'n'))
            golf_enter();
        else if (ev->key == K_BACK || ev->key == K_QUIT)
            s_exit_request = true;
        return;
    }
    switch (ev->key) {
    case K_LEFT:  if (go_sel > 0) go_sel--; break;
    case K_RIGHT: if (go_sel < GO_COLS - 1) go_sel++; break;
    case K_CHAR:
        if (ev->is_repeat) break;
        if (ev->ch == 'a' && go_sel > 0) go_sel--;
        else if (ev->ch == 'd' && go_sel < GO_COLS - 1) go_sel++;
        else if (ev->ch == 'n') golf_enter();
        break;
    case K_OK:
        if (ev->is_repeat) break;
        if (go_try_col_to_waste(go_sel) || go_try_waste_to_col(go_sel)) {
            go_moves++;
            go_check_end();
            if (go_over) {
                if (go_win) {
                    audio_win();
                    led_fx_set(LED_FX_WIN);
                } else {
                    audio_lose();
                    led_fx_set(LED_FX_LOSE);
                }
            } else {
                audio_clear();
            }
        } else {
            audio_error();
        }
        break;
    case K_BACK:
        if (ev->is_repeat) break;
        go_draw();      /* 翻发牌堆(核心操作, 见文件头注) */
        go_check_end();
        if (go_over) {
            audio_lose();
            led_fx_set(LED_FX_LOSE);
        }
        break;
    case K_PAUSE: {
        if (ev->is_repeat) break;
        pause_sel_t sel;
        if (ui_pause_run(&sel)) {
            if (sel == PAUSE_RESTART) golf_enter();
        } else {
            s_exit_request = true;
        }
        break;
    }
    case K_QUIT: if (!ev->is_repeat) s_exit_request = true; break;
    default: break;
    }
}
