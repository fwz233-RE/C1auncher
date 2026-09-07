/* ANT COLONY (蚂蚁帝国) — 回合制蚂蚁寻路
 * 8x6 格 22px(176x132 居中于 HUD 下方); 蚁穴(▲)固定左上角, 食物堆(●+数量)
 * 随机刷新, 岩石 4 块随机阻挡(蚁穴两出口格永不落岩, 保证可达)
 * 方向/WASD = 当前蚂蚁走 1 格(消耗 1 回合, 寿命 12 步); 到达食物堆整个带回
 * (+AC_PILE 食物, 自动 BFS 最短路径回巢并以点线显示); 食物每集 10/20 解锁
 * 新蚂蚁(最多 3 只轮流出击); AC_TURN_MAX 回合内集 AC_WIN_FOOD 食物 → WIN
 * 输入驱动(无 tick); 移动=快刷, 开局/结束=全刷 */
#include "game.h"
#include "../config.h"
#include "../gfx/canvas.h"
#include "../gfx/font.h"
#include "../platform/audio.h"
#include "../platform/display.h"
#include "../platform/input.h"
#include "../platform/led.h"
#include "../platform/time.h"
#include "../rng.h"

#define AC_COLS 8
#define AC_ROWS 6
#define AC_CELL 22
#define AC_BW (AC_COLS * AC_CELL)                        /* 176 */
#define AC_BH (AC_ROWS * AC_CELL)                        /* 132 */
#define AC_OX ((CCG_W - AC_BW) / 2)                      /* 60 水平居中 */
#define AC_OY (CCG_HUD_H + (CCG_H - CCG_HUD_H - AC_BH) / 2) /* 18 垂直居中 */
#define AC_NX 0
#define AC_NY 0
#define AC_CELLS (AC_COLS * AC_ROWS)                     /* 48 */
#define AC_ROCKS 4
#define AC_PILE 5            /* 食物堆单位数(一次整个带回) */
#define AC_ANT_LIFE 12       /* 蚂蚁寿命(步) */
#define AC_TURN_MAX 25       /* 回合上限 */
#define AC_WIN_FOOD 15       /* 胜利所需食物 */
#define AC_MAX_DIST 12       /* 食物刷新点 BFS 距离上限(=寿命, 必可达) */
#define AC_ANTS2 10          /* 集满 10 食物解锁第 2 只 */
#define AC_ANTS3 20          /* 集满 20 食物解锁第 3 只 */
#define AC_MAX_ANTS 3

enum { AC_EMPTY = 0, AC_ROCK = 1 };

void antcolony_enter(void);
void antcolony_render(void);

static rng_t ac_rng;
static uint8_t ac_cell[AC_ROWS][AC_COLS];   /* AC_EMPTY / AC_ROCK */
static bool ac_grave[AC_ROWS][AC_COLS];     /* 蚂蚁阵亡标记 X */
static int ac_fx, ac_fy;                    /* 食物堆格 */
static int ac_ax, ac_ay;                    /* 当前蚂蚁格 */
static int ac_age;                          /* 本次出击已走步数 */
static int ac_ants;                         /* 可用蚂蚁数 1..AC_MAX_ANTS */
static int ac_active;                       /* 当前出击蚂蚁 0..ants-1 */
static int ac_food;                         /* 已收集食物 */
static int ac_turn;                         /* 已用回合(步数) */
static int ac_path[AC_CELLS];               /* 回巢路径格序列(不含蚁穴格) */
static int ac_path_len;
static bool ac_over, ac_win, ac_over_full;

/* BFS 最短路: (sx,sy) → 蚁穴, 岩石不可通过。path 输出自 (sx,sy) 起、止于蚁穴
 * 前一个格子的格序列(不含蚁穴格); 返回长度。不可达返回 -1; 起点即蚁穴返回 0。
 * 栈上小数组(BFS 临时), 零 malloc。 */
static int ac_bfs_path(int sx, int sy, int *path) {
    static const int dx[4] = { 1, -1, 0, 0 };
    static const int dy[4] = { 0, 0, 1, -1 };
    int qx[AC_CELLS], qy[AC_CELLS], prev[AC_CELLS];
    int qh = 0, qt = 0;
    int start = sy * AC_COLS + sx;
    for (int i = 0; i < AC_CELLS; i++) prev[i] = -1;
    prev[start] = start;
    qx[qt] = sx; qy[qt] = sy; qt++;
    while (qh < qt) {
        int x = qx[qh], y = qy[qh];
        int i = y * AC_COLS + x;
        qh++;
        if (x == AC_NX && y == AC_NY) break;
        for (int d = 0; d < 4; d++) {
            int nx = x + dx[d], ny = y + dy[d];
            if (nx < 0 || nx >= AC_COLS || ny < 0 || ny >= AC_ROWS) continue;
            if (ac_cell[ny][nx] == AC_ROCK) continue;
            int ni = ny * AC_COLS + nx;
            if (prev[ni] != -1) continue;
            prev[ni] = i;
            qx[qt] = nx; qy[qt] = ny; qt++;
        }
    }
    if (sx == AC_NX && sy == AC_NY) return 0;            /* 起点即蚁穴 */
    if (prev[AC_NY * AC_COLS + AC_NX] == -1) return -1;  /* 不可达 */
    /* 从蚁穴沿 prev 链回走收集(prev[start] 是 BFS 自标记, 只能从蚁穴侧走):
     * 得 [蚁穴, ..., 食物旁格]; 反转 → 去掉蚁穴格 → 前插食物格 */
    int len = 0, cur = AC_NY * AC_COLS + AC_NX;
    while (cur != start) {
        path[len++] = cur;
        cur = prev[cur];
    }
    for (int i = 0, j = len - 1; i < j; i++, j--) {
        int t = path[i]; path[i] = path[j]; path[j] = t;
    }
    if (len > 0) len--;                    /* 去掉蚁穴格 */
    for (int i = len; i > 0; i--) path[i] = path[i - 1];
    path[0] = start;                       /* 补上食物格 */
    len++;
    return len;
}

static int ac_tmp[AC_CELLS];   /* 摆放/校验用 BFS 缓冲 */

/* 岩石: 4 块随机, 避开蚁穴与蚁穴两出口格(保证食物始终可达) */
static void ac_place_rocks(void) {
    int placed = 0, tries = 0;
    do {
        tries++;
        int x = (int)rng_range(&ac_rng, AC_COLS);
        int y = (int)rng_range(&ac_rng, AC_ROWS);
        if (x == AC_NX && y == AC_NY) continue;
        if ((x == 1 && y == 0) || (x == 0 && y == 1)) continue;
        if (ac_cell[y][x] != AC_ROCK) {
            ac_cell[y][x] = AC_ROCK;
            placed++;
        }
    } while (placed < AC_ROCKS && tries < 200);
    if (placed < AC_ROCKS) {   /* 兜底: 顺序填满(候选必然充足) */
        for (int y = 0; y < AC_ROWS && placed < AC_ROCKS; y++)
            for (int x = 0; x < AC_COLS && placed < AC_ROCKS; x++) {
                if (x == AC_NX && y == AC_NY) continue;
                if ((x == 1 && y == 0) || (x == 0 && y == 1)) continue;
                if (ac_cell[y][x] != AC_ROCK) {
                    ac_cell[y][x] = AC_ROCK;
                    placed++;
                }
            }
    }
}

/* 食物堆刷新: 随机可达格(BFS 距离 1..AC_MAX_DIST), 带 guard 的 do-while +
 * 顺序扫描兜底(蚁穴出口恒通, 必有候选) */
static void ac_place_food(void) {
    int px = -1, py = -1, tries = 0;
    do {
        tries++;
        int x = (int)rng_range(&ac_rng, AC_COLS);
        int y = (int)rng_range(&ac_rng, AC_ROWS);
        if (x == AC_NX && y == AC_NY) continue;
        if (ac_cell[y][x] == AC_ROCK) continue;
        int d = ac_bfs_path(x, y, ac_tmp);
        if (d < 1 || d > AC_MAX_DIST) continue;
        px = x; py = y;
    } while (px < 0 && tries < 200);
    if (px < 0) {
        for (int d = 1; d <= AC_MAX_DIST && px < 0; d++)
            for (int y = 0; y < AC_ROWS && px < 0; y++)
                for (int x = 0; x < AC_COLS && px < 0; x++)
                    if (ac_cell[y][x] != AC_ROCK &&
                        ac_bfs_path(x, y, ac_tmp) == d) { px = x; py = y; }
    }
    ac_fx = px; ac_fy = py;
}

static void ac_new(void) {
    rng_seed(&ac_rng, now_ms() ^ 0xAC0FFEEu);
    for (int y = 0; y < AC_ROWS; y++)
        for (int x = 0; x < AC_COLS; x++) {
            ac_cell[y][x] = AC_EMPTY;
            ac_grave[y][x] = false;
        }
    ac_food = 0;
    ac_turn = 0;
    ac_ants = 1;
    ac_active = 0;
    ac_ax = AC_NX; ac_ay = AC_NY;
    ac_age = 0;
    ac_path_len = 0;
    ac_over = false; ac_win = false; ac_over_full = false;
    ac_place_rocks();
    ac_place_food();
}

/* 出击结束: 回巢并切换下一只蚂蚁 */
static void ac_ant_back(void) {
    ac_ax = AC_NX; ac_ay = AC_NY;
    ac_age = 0;
    ac_active = (ac_active + 1) % ac_ants;
}

/* 蚂蚁走一步: 返回 true=发生移动(消耗 1 回合); 撞墙/撞岩不耗回合。
 * 到达食物: 整个带回(食物 +AC_PILE), 自动 BFS 回巢(路径点线显示, 不耗回合) */
static bool ac_step(int dx, int dy) {
    int nx = ac_ax + dx, ny = ac_ay + dy;
    if (nx < 0 || nx >= AC_COLS || ny < 0 || ny >= AC_ROWS) return false;
    if (ac_cell[ny][nx] == AC_ROCK) return false;
    ac_path_len = 0;                 /* 新一段行走, 旧回巢路径消失 */
    ac_ax = nx; ac_ay = ny;
    ac_turn++;
    ac_age++;
    if (nx == ac_fx && ny == ac_fy) {                /* 到达食物堆 */
        ac_food += AC_PILE;
        ac_path_len = ac_bfs_path(nx, ny, ac_path);
        ac_ant_back();
        ac_place_food();
        if (ac_food == AC_ANTS2 && ac_ants < AC_MAX_ANTS) ac_ants++;
        if (ac_food == AC_ANTS3 && ac_ants < AC_MAX_ANTS) ac_ants++;
        if (ac_food >= AC_WIN_FOOD) {
            ac_over = true; ac_win = true;
            audio_win(); led_fx_set(LED_FX_WIN);
        }
        return true;
    }
    if (ac_age >= AC_ANT_LIFE) {                     /* 寿命耗尽阵亡 */
        ac_grave[ny][nx] = true;
        ac_ant_back();
        return true;
    }
    if (!ac_over && ac_turn >= AC_TURN_MAX) {
        ac_over = true; ac_win = false;
        audio_lose(); led_fx_set(LED_FX_LOSE);
    }
    return true;
}

/* 方向移动(带音效): 成功=移动音/带回食物=得分音, 撞墙/撞岩=错误音; 长按重复静音 */
static void ac_key_move(int dx, int dy, bool repeat) {
    if (repeat) { ac_step(dx, dy); return; }
    bool to_food = (ac_ax + dx == ac_fx && ac_ay + dy == ac_fy);
    if (!ac_step(dx, dy)) { audio_error(); return; }
    if (to_food) audio_clear();
    else audio_move();
}

/* ---- 绘制 ---- */

static void ac_render_hud(void) {
    /* 左: ANTS n */
    fb_text(2, 2, "ANTS", true);
    {
        char b[4];
        int k = 0;
        if (ac_ants >= 10) b[k++] = (char)('0' + ac_ants / 10);
        b[k++] = (char)('0' + ac_ants % 10);
        b[k] = 0;
        fb_text(2 + text_width("ANTS") + 1, 2, b, true);
    }
    /* 右: FOOD n TURN n(剩余回合) */
    {
        char buf[20];
        int n = 0;
        static const char f[] = "FOOD ";
        int i = 0;
        while (f[i]) buf[n++] = f[i++];
        if (ac_food >= 100) buf[n++] = (char)('0' + ac_food / 100);
        if (ac_food >= 10) buf[n++] = (char)('0' + (ac_food / 10) % 10);
        buf[n++] = (char)('0' + ac_food % 10);
        static const char t[] = " TURN ";
        i = 0;
        while (t[i]) buf[n++] = t[i++];
        int rem = AC_TURN_MAX - ac_turn;
        if (rem < 0) rem = 0;
        if (rem >= 100) buf[n++] = (char)('0' + rem / 100);
        if (rem >= 10) buf[n++] = (char)('0' + (rem / 10) % 10);
        buf[n++] = (char)('0' + rem % 10);
        buf[n] = 0;
        fb_text(CCG_W - 4 - text_width(buf), 2, buf, true);
    }
}

void antcolony_render(void) {
    fb_clear(false);
    ac_render_hud();
    fb_hline(0, CCG_HUD_H - 1, CCG_W, true);
    /* 网格 */
    for (int x = 0; x <= AC_COLS; x++)
        fb_vline(AC_OX + x * AC_CELL, AC_OY, AC_BH, true);
    for (int y = 0; y <= AC_ROWS; y++)
        fb_hline(AC_OX, AC_OY + y * AC_CELL, AC_BW, true);
    /* 岩石(实心方+白心)与阵亡标记 X */
    for (int y = 0; y < AC_ROWS; y++)
        for (int x = 0; x < AC_COLS; x++) {
            int cx = AC_OX + x * AC_CELL + AC_CELL / 2;
            int cy = AC_OY + y * AC_CELL + AC_CELL / 2;
            if (ac_cell[y][x] == AC_ROCK) {
                fb_fill_rect(AC_OX + x * AC_CELL, AC_OY + y * AC_CELL,
                             AC_CELL, AC_CELL, true);
                fb_fill_rect(cx - 2, cy - 2, 5, 5, false);
            }
            if (ac_grave[y][x]) {
                for (int i = -3; i <= 3; i++) {
                    fb_pixel(cx + i, cy + i, true);
                    fb_pixel(cx + i, cy - i, true);
                }
            }
        }
    /* 回巢路径: 点线(每格 3x3 点) */
    for (int i = 0; i < ac_path_len; i++) {
        int cell = ac_path[i];
        int cx = AC_OX + (cell % AC_COLS) * AC_CELL + AC_CELL / 2;
        int cy = AC_OY + (cell / AC_COLS) * AC_CELL + AC_CELL / 2;
        fb_fill_rect(cx - 1, cy - 1, 3, 3, true);
    }
    /* 食物堆: 圆 + 数量 */
    {
        int cx = AC_OX + ac_fx * AC_CELL + AC_CELL / 2;
        int cy = AC_OY + ac_fy * AC_CELL + AC_CELL / 2;
        for (int dy = -4; dy <= 4; dy++)
            for (int dx = -4; dx <= 4; dx++)
                if (dx * dx + dy * dy <= 16) fb_pixel(cx - 5 + dx, cy + dy, true);
        {
            char b[2];
            b[0] = (char)('0' + AC_PILE);
            b[1] = 0;
            fb_text(cx + 2, cy - 3, b, true);
        }
    }
    /* 蚁穴 ▲(左上角格左半) */
    {
        int cx = AC_OX + AC_NX * AC_CELL + AC_CELL / 2;
        int cy = AC_OY + AC_NY * AC_CELL + AC_CELL / 2;
        fb_hline(cx - 8, cy + 4, 9, true);
        fb_pixel(cx - 4, cy - 4, true);
        for (int i = 1; i <= 4; i++) {               /* 两斜边 */
            fb_pixel(cx - 4 - i, cy - 4 + i, true);
            fb_pixel(cx - 4 + i, cy - 4 + i, true);
        }
    }
    /* 蚂蚁 ●(随格移动, 偏右半便于与路径点区分) */
    {
        int cx = AC_OX + ac_ax * AC_CELL + AC_CELL / 2 + 5;
        int cy = AC_OY + ac_ay * AC_CELL + AC_CELL / 2;
        for (int dy = -3; dy <= 3; dy++)
            for (int dx = -3; dx <= 3; dx++)
                if (dx * dx + dy * dy <= 9) fb_pixel(cx + dx, cy + dy, true);
    }
    /* 结束: HUD 区左上结果 + 右上操作提示, 强制全刷一次 */
    if (ac_over) {
        fb_fill_rect(0, 0, CCG_W, CCG_HUD_H - 1, false);
        fb_text(2, 2, ac_win ? "WIN! 15 FOOD" : "TURNS UP", true);
        fb_text(CCG_W - 2 - text_width("OK/N:RETRY BACK:QUIT"), 2,
                "OK/N:RETRY BACK:QUIT", true);
        if (!ac_over_full) { ac_over_full = true; disp_force_full(); }
    }
    disp_fast();
}

/* ---- 输入 ---- */

static void ac_pause(void) {
    pause_sel_t sel;
    if (ui_pause_run(&sel)) {
        if (sel == PAUSE_RESTART) antcolony_enter();
    } else {
        s_exit_request = true;
    }
}

void antcolony_on_key(const key_event_t *ev) {
    /* 重复事件只响应方向键(确认/字母必须忽略) */
    if (ev->is_repeat && ev->key != K_LEFT && ev->key != K_RIGHT &&
        ev->key != K_UP && ev->key != K_DOWN)
        return;
    if (ac_over) {
        if (ev->key == K_OK || (ev->key == K_CHAR && ev->ch == 'n'))
            antcolony_enter();
        else if (ev->key == K_BACK || ev->key == K_QUIT)
            s_exit_request = true;
        return;
    }
    switch (ev->key) {
    case K_LEFT: ac_key_move(-1, 0, ev->is_repeat); break;
    case K_RIGHT: ac_key_move(1, 0, ev->is_repeat); break;
    case K_UP: ac_key_move(0, -1, ev->is_repeat); break;
    case K_DOWN: ac_key_move(0, 1, ev->is_repeat); break;
    case K_BACK: ac_pause(); break;
    case K_PAUSE: ac_pause(); break;
    case K_CHAR:
        if (ev->ch == 'a') ac_key_move(-1, 0, ev->is_repeat);
        else if (ev->ch == 'd') ac_key_move(1, 0, ev->is_repeat);
        else if (ev->ch == 'w') ac_key_move(0, -1, ev->is_repeat);
        else if (ev->ch == 's') ac_key_move(0, 1, ev->is_repeat);
        else if (ev->ch == 'n') antcolony_enter();
        break;
    case K_QUIT: s_exit_request = true; break;
    default: break;
    }
}

void antcolony_tick(uint64_t now) { (void)now; }
void antcolony_exit(void) {}

void antcolony_enter(void) {
    ac_new();
    antcolony_render();
    disp_full();
}
