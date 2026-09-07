/* KLOTSKI — 华容道 5x4 滑块突围 (经典开局: 横刀立马)
 *
 * 棋子: 曹操 2x2(实心黑块+白字 CAO), 关羽 2x1 横(斜纹),
 * 四竖将 1x2(斜纹), 四小卒 1x1(点阵)。5x4 共 20 格, 18 子 + 2 空位。
 * 胜利: 曹操滑到底部出口行(y=3, x=1-2) → SOLVED!
 *
 * 操作(与 15 谜一致): 光标停在空位上, 方向键把该方向上的邻接棋子
 * 滑入空位(按 UP = 空位正下方的棋子 上滑入空位)。两个空位时先试
 * 光标所在空位, 不可动则试另一个。长按重复由框架合成(仅方向键)。
 * 步数计入顶栏 MOVES。
 *
 * 集成时 help 行(<=5 行 + NULL):
 *   "KLOTSKI: SLIDE PIECES TO FREE CAO",
 *   "DIRECTION: ADJACENT PIECE SLIDES",
 *   "INTO THE BLANK AT THE CURSOR.",
 *   "GET CAO (2X2) DOWN TO ROW 4 MIDDLE.",
 *   "N:NEW  Q:QUIT  P:PAUSE",  */
#include "game.h"
#include "../config.h"
#include "../gfx/canvas.h"
#include "../gfx/font.h"
#include "../gfx/pattern.h"
#include "../platform/display.h"
#include "../platform/input.h"
#include "../platform/audio.h"
#include "../platform/led.h"

#define KK_COLS 5
#define KK_ROWS 4
#define KK_CELL 28                          /* 28px 方格 */
#define KK_BW (KK_COLS * KK_CELL)           /* 140 */
#define KK_BH (KK_ROWS * KK_CELL)           /* 112 */
#define KK_OX ((CCG_W - KK_BW) / 2)         /* 78: 水平居中 */
#define KK_OY 22                            /* HUD(16) 下留 6px */
#define KK_PIECES 10
#define KK_P_CAO 0

/* 开局: 横刀立马(公认可解的经典开局, 曹操 (1,0)-(2,1))
 *   张 曹 曹 赵 卒
 *   张 曹 曹 赵 卒
 *   马 关 关 黄 空
 *   马 卒 卒 黄 空
 * 棋子: 0=曹操 1=关羽 2=张飞 3=赵云 4=马超 5=黄忠 6-9=小卒 */
static const int kk_init_x[KK_PIECES] = { 1, 1,  0, 3, 0, 4,  4, 4, 1, 2 };
static const int kk_init_y[KK_PIECES] = { 0, 2,  0, 0, 2, 2,  0, 1, 3, 3 };
static const int kk_pw[KK_PIECES]     = { 2, 2,  1, 1, 1, 1,  1, 1, 1, 1 };
static const int kk_ph[KK_PIECES]     = { 2, 1,  2, 2, 2, 2,  1, 1, 1, 1 };

static int kk_px[KK_PIECES];               /* 棋子左上角(动态状态) */
static int kk_py[KK_PIECES];
static int kk_grid[KK_COLS * KK_ROWS];     /* 0=空位, 值=棋子号+1 */
static int kk_ax, kk_ay;                   /* 光标锚定的空位 */
static uint32_t kk_moves;
static bool kk_over;
static bool kk_over_full;

/* dir: 0=上 1=下 2=左 3=右 —— 棋子滑入空位的方向(15 谜语义) */
static const int kk_dx[4] = { 0, 0, -1, 1 };
static const int kk_dy[4] = { -1, 1, 0, 0 };

static void kk_rebuild(void) {
    for (int i = 0; i < KK_COLS * KK_ROWS; i++) kk_grid[i] = 0;
    for (int p = 0; p < KK_PIECES; p++)
        for (int y = 0; y < kk_ph[p]; y++)
            for (int x = 0; x < kk_pw[p]; x++)
                kk_grid[(kk_py[p] + y) * KK_COLS + kk_px[p] + x] = p + 1;
}

/* 光标移到行优先第一个空位(移动后空位位置变了) */
static void kk_anchor_first(void) {
    for (int i = 0; i < KK_COLS * KK_ROWS; i++)
        if (kk_grid[i] == 0) { kk_ay = i / KK_COLS; kk_ax = i % KK_COLS; return; }
    kk_ax = 0;                              /* 无空位不应发生 */
    kk_ay = 0;
}

static bool kk_solved(void) {
    /* 曹操底部出口行 y=3, x=1-2 */
    return kk_grid[3 * KK_COLS + 1] == KK_P_CAO + 1 &&
           kk_grid[3 * KK_COLS + 2] == KK_P_CAO + 1;
}

/* 把空位 (ex,ey) 朝 dir 方向的邻接棋子滑入空位。
 * 新占格要么是棋子旧格(滑走部分)要么是空位, 否则不可动。
 * 返回 1=成功(棋盘已更新), 0=不可动。 */
static int kk_slide_at(int ex, int ey, int dir) {
    int ux = kk_dx[dir], uy = kk_dy[dir];
    int tx = ex - ux, ty = ey - uy;         /* 空位反方向邻格 = 棋子 */
    if (tx < 0 || tx >= KK_COLS || ty < 0 || ty >= KK_ROWS) return 0;
    int p = kk_grid[ty * KK_COLS + tx] - 1;
    if (p < 0) return 0;                    /* 邻格也是空位 */
    int nxp = kk_px[p] + ux, nyp = kk_py[p] + uy;
    if (nxp < 0 || nyp < 0 ||
        nxp + kk_pw[p] > KK_COLS || nyp + kk_ph[p] > KK_ROWS) return 0;
    for (int y = 0; y < kk_ph[p]; y++)
        for (int x = 0; x < kk_pw[p]; x++) {
            int cx = nxp + x, cy = nyp + y;
            int in_old = cx >= kk_px[p] && cx < kk_px[p] + kk_pw[p] &&
                         cy >= kk_py[p] && cy < kk_py[p] + kk_ph[p];
            if (!in_old && kk_grid[cy * KK_COLS + cx] != 0) return 0;
        }
    kk_px[p] = nxp;
    kk_py[p] = nyp;
    kk_rebuild();
    return 1;
}

/* 方向键响应: 先试光标空位, 不可动则试另一空位(两个空位都覆盖)。
 * 返回 1=发生滑动(计步)。 */
static int kk_press(int dir) {
    int ox = -1, oy = -1;                   /* 另一空位 */
    for (int i = 0; i < KK_COLS * KK_ROWS; i++) {
        if (kk_grid[i] == 0 &&
            (i % KK_COLS != kk_ax || i / KK_COLS != kk_ay)) {
            ox = i % KK_COLS;
            oy = i / KK_COLS;
            break;
        }
    }
    int moved = kk_slide_at(kk_ax, kk_ay, dir);
    if (!moved && ox >= 0) moved = kk_slide_at(ox, oy, dir);
    if (moved) {
        kk_moves++;
        kk_anchor_first();
        kk_over = kk_solved();
    }
    return moved;
}

void klotski_render(void);

void klotski_enter(void) {
    for (int p = 0; p < KK_PIECES; p++) {
        kk_px[p] = kk_init_x[p];
        kk_py[p] = kk_init_y[p];
    }
    kk_rebuild();
    kk_anchor_first();
    kk_moves = 0;
    kk_over = false;
    kk_over_full = false;
    klotski_render();
    disp_full();
}

void klotski_render(void) {
    fb_clear(false);
    /* 棋子: 曹操=实心白字, 武将(横/竖)=斜纹, 小卒=点阵, 全部加边框 */
    for (int p = 0; p < KK_PIECES; p++) {
        int px = KK_OX + kk_px[p] * KK_CELL;
        int py = KK_OY + kk_py[p] * KK_CELL;
        int w = kk_pw[p] * KK_CELL, h = kk_ph[p] * KK_CELL;
        if (p == KK_P_CAO) {
            fb_fill_rect(px, py, w, h, true);           /* 实心 */
            int tw = text_width("CAO") * 2 - 2;         /* 2x 宽: 32 */
            fb_text_scale2(px + (w - tw) / 2, py + (h - 2 * FONT_H) / 2,
                           "CAO", false);               /* 白字 */
        } else if (kk_pw[p] == 1 && kk_ph[p] == 1) {
            fb_fill_tile(px, py, w, h, pat_get(PAT_DOT_SPARSE));
        } else {
            fb_fill_tile(px, py, w, h, pat_get(PAT_SLASH_D));
        }
        fb_stroke_rect(px, py, w, h, true);
    }
    /* 光标: 空位上的反色边框(在所有格画完后画, 四周对称) */
    fb_stroke_rect_thick(KK_OX + kk_ax * KK_CELL, KK_OY + kk_ay * KK_CELL,
                         KK_CELL, KK_CELL, 2, true);
    /* 底部操作提示 */
    fb_text_center(KK_OY + KK_BH + 6, "ARROWS/WASD: SLIDE", true);
    /* HUD 顶栏: 黑字白底, 左标题右步数 */
    fb_fill_rect(0, 0, CCG_W, CCG_HUD_H - 1, false);
    if (kk_over) {
        fb_text(0, 0, "SOLVED!", true);
        fb_text(CCG_W - 2 - text_width("OK/N:RETRY BACK:QUIT"), 2,
                "OK/N:RETRY BACK:QUIT", true);
        if (!kk_over_full) { kk_over_full = true; disp_force_full(); }
    } else {
        fb_text(0, 0, "KLOTSKI", true);
        char buf[24];
        const char *lab = "MOVES ";
        unsigned n = 0;
        while (lab[n]) n++;
        uint32_t v = kk_moves;
        char rev[12];
        unsigned r = 0;
        if (v == 0) rev[r++] = '0';
        while (v && r < 10) { rev[r++] = (char)('0' + v % 10); v /= 10; }
        char *o = buf;
        for (unsigned i = 0; i < n; i++) *o++ = lab[i];
        while (r > 0) *o++ = rev[--r];
        *o = 0;
        fb_text(CCG_W - 2 - text_width(buf), 0, buf, true);
    }
    fb_hline(0, CCG_HUD_H - 1, CCG_W, true);
}

void klotski_on_key(const key_event_t *ev) {
    if (kk_over) {
        if (ev->is_repeat) return;
        if (ev->key == K_OK || (ev->key == K_CHAR && ev->ch == 'n')) {
            audio_select();
            klotski_enter();
        }
        else if (ev->key == K_BACK || ev->key == K_QUIT)
            s_exit_request = true;
        return;
    }
    int dir = -1;
    switch (ev->key) {
    case K_UP: dir = 0; break;
    case K_DOWN: dir = 1; break;
    case K_LEFT: dir = 2; break;
    case K_RIGHT: dir = 3; break;
    case K_CHAR:
        if (ev->ch == 'w') dir = 0;
        else if (ev->ch == 's') dir = 1;
        else if (ev->ch == 'a') dir = 2;
        else if (ev->ch == 'd') dir = 3;
        else if (ev->ch == 'n' && !ev->is_repeat) { audio_select(); klotski_enter(); }
        break;
    case K_BACK:
        if (!ev->is_repeat) {
            pause_sel_t sel;
            if (ui_pause_run(&sel)) {
                if (sel == PAUSE_RESTART) klotski_enter();
            } else {
                s_exit_request = true;
            }
        }
        break;
    case K_QUIT:
        if (!ev->is_repeat) s_exit_request = true;
        break;
    default: break;
    }
    if (dir >= 0) {
        int moved = kk_press(dir);          /* 方向键重复可响应(长按连滑) */
        if (moved && !ev->is_repeat) {
            audio_move();                   /* 滑动成功(长按连滑静音) */
            if (kk_over) {                  /* 通关瞬间 */
                audio_win();
                led_fx_set(LED_FX_WIN);
            }
        } else if (!moved && !ev->is_repeat) {
            audio_error();                  /* 无子可滑 */
        }
    }
}

void klotski_tick(uint64_t now) { (void)now; }
void klotski_exit(void) {}
