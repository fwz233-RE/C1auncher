/* MAZE — 程序化迷宫(迭代回溯生成完美迷宫) + 走出迷宫
 * 网格 20x13 格 10px 方形(200x130, 水平居中, y 从 16 起; 底部 146 < 152 屏高
 *   —— 设计稿 20x14 格从 y=16 起会到 156 超出 152 屏高, 故取 13 行)
 * 墙=实心黑格, 路=空白; 起点左上, 终点右下(星号标记)
 * 随机 Prim 生成(只挖"恰 1 个通路邻居"的墙格) → 路格诱导子图为生成树
 * → 全连通且任意两格唯一路径; 终点未入树重试(guard 24)+BFS 打通兜底
 * N: 新迷宫  H: 切换 BFS 解法路径(实心点标记, 从玩家位置实时到终点)
 * 走到终点星号 → SOLVED!(步数统计)
 * HUD 顶栏: 左标题 MAZE, 右 MOVES n
 * 静态前缀 mz_; 像素坐标一律 int; 零 malloc; 无 do-while 随机循环
 *
 * 集成提示(help[] 最多 5 行):
 *   "MAZE", "ARROWS/WASD: MOVE", "REACH THE STAR",
 *   "N: NEW  H: SHOW PATH", NULL
 *   (设计稿的 S 键解法与 WASD 的 s=下行冲突, 改 H(HINT))
 */
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

#define MZ_W 20                       /* 列数 */
#define MZ_H 13                       /* 行数(10px x 13 = 130, 16+130=146) */
#define MZ_N (MZ_W * MZ_H)            /* 260 格 */
#define MZ_CELL 10
#define MZ_OX ((int)(CCG_W - (unsigned)MZ_W * MZ_CELL) / 2)   /* 48 */
#define MZ_OY 16                      /* HUD 之下 */
#define MZ_END ((MZ_H - 1) * MZ_W + (MZ_W - 1))               /* 终点格 */

/* 实心圆 8x8(玩家小人), bit0=最左(与 pattern.h 同约定) */
static const uint8_t mz_ball[8] = {
    0x18, 0x3C, 0x7E, 0xFF, 0xFF, 0x7E, 0x3C, 0x18
};
/* 解法路径点 8x8: 中央 2x4 竖条 */
static const uint8_t mz_dot[8] = {
    0x00, 0x00, 0x18, 0x18, 0x18, 0x18, 0x00, 0x00
};

static uint8_t mz_g[MZ_N];    /* 1=墙 0=路 */
static uint8_t mz_sol[MZ_N];  /* 解法路径标记(1=在解路径上) */
static int mz_px, mz_py;      /* 玩家格坐标 */
static uint32_t mz_moves;     /* 步数 */
static bool mz_over;          /* 到终点 → 胜利 */
static bool mz_over_full;     /* 结束全刷只做一次 */
static bool mz_show_sol;      /* S 键切换解法显示 */
static uint32_t mz_gens;      /* 换局计数器(种子混合) */
static rng_t mz_rng;

/* 工作区: 求解用 BFS 队列 + 父指针(静态; 生成/求解分时复用) */
static uint16_t mz_q[MZ_N];
static int16_t mz_pr[MZ_N];

static const int mz_dx[4] = { 0, 0, -1, 1 };
static const int mz_dy[4] = { -1, 1, 0, 0 };

void maze_render(void);

/* 统计 i 格 4 邻中已挖通路格数 */
static int mz_path_nbrs(int i) {
    int cx = i % MZ_W;
    int cy = i / MZ_W;
    int n = 0;
    if (cy > 0        && !mz_g[i - MZ_W]) n++;
    if (cy < MZ_H - 1 && !mz_g[i + MZ_W]) n++;
    if (cx > 0        && !mz_g[i - 1])    n++;
    if (cx < MZ_W - 1 && !mz_g[i + 1])    n++;
    return n;
}

/* 兜底(理论上不可达): 终点未入生成树时, BFS 墙间寻路从终点向最近
 * 通路格打通一条走廊(可能带旁路, 但保证终点可达可解); BFS 有界必终止 */
static void mz_force_end(void) {
    int i;
    int head = 0, tail = 0;
    for (i = 0; i < MZ_N; i++) mz_pr[i] = -1;
    mz_q[tail++] = (uint16_t)MZ_END;
    mz_pr[MZ_END] = MZ_END;
    {
        int goal = -1;
        while (head < tail) {
            int c = mz_q[head++];
            int cx = c % MZ_W;
            int cy = c / MZ_W;
            if (c != MZ_END && mz_path_nbrs(c) >= 1) { goal = c; break; }
            for (i = 0; i < 4; i++) {
                int nx = cx + mz_dx[i];
                int ny = cy + mz_dy[i];
                if (nx < 0 || ny < 0 || nx >= MZ_W || ny >= MZ_H) continue;
                {
                    int n = ny * MZ_W + nx;
                    if (mz_pr[n] != -1) continue;
                    mz_pr[n] = (int16_t)c;
                    mz_q[tail++] = (uint16_t)n;
                }
            }
        }
        if (goal < 0) {
            mz_g[MZ_END] = 0;   /* 全墙不可能(起点已挖开), 防御 */
            return;
        }
        {
            int c = goal;
            while (c != MZ_END) {   /* 沿 BFS 父链挖回终点 */
                mz_g[c] = 0;
                c = mz_pr[c];
            }
            mz_g[MZ_END] = 0;
        }
    }
}

/* 随机 Prim 生成完美迷宫(1:1 格模型):
 * 只挖开"恰有 1 个通路邻居"的墙格 → 路格诱导子图恒为生成树
 * → 任意两路格间唯一路径(起点左上, 终点右下必连通)
 * 每轮循环恰好挖开一格, ≤260 次必然终止; 终点未入树则重试(带 guard 24),
 * 兜底 mz_force_end 保证可解。无 do-while 随机循环。 */
static void mz_generate(void) {
    int i;
    int attempt;
    for (i = 0; i < MZ_N; i++) mz_g[i] = 1;
    mz_g[0] = 0;                          /* 起点格挖开 */
    for (attempt = 0; attempt < 24; attempt++) {
        for (i = 1; i < MZ_N; i++) mz_g[i] = 1;   /* 重试: 复位(保留起点) */
        for (;;) {
            uint16_t f[MZ_N];       /* 前沿墙格索引(≤259, uint16 足够) */
            int fn = 0;
            for (i = 0; i < MZ_N; i++) {
                if (!mz_g[i]) continue;           /* 只考虑墙格 */
                if (mz_path_nbrs(i) == 1) f[fn++] = (uint16_t)i;  /* 入前沿 */
            }
            if (fn == 0) break;                   /* 生成树已最大 */
            mz_g[f[(int)rng_range(&mz_rng, (uint32_t)fn)]] = 0;  /* 挖开 */
        }
        if (mz_g[MZ_END] == 0) break;             /* 终点已连通 → 完成 */
    }
    if (mz_g[MZ_END] != 0) mz_force_end();        /* 兜底 */
}

/* BFS 从玩家位置求到终点的最短路径, 标记到 mz_sol[](不含玩家格与终点格,
 * 终点由星号表示)。完美迷宫必有解; 即便无解也不死循环(队列有界)。 */
static void mz_solve(void) {
    int i;
    for (i = 0; i < MZ_N; i++) mz_pr[i] = -1;
    for (i = 0; i < MZ_N; i++) mz_sol[i] = 0;
    {
        int head = 0, tail = 0;
        int st = mz_py * MZ_W + mz_px;
        mz_q[tail++] = (uint16_t)st;
        mz_pr[st] = st;                   /* 起点父指针指向自身 */
        while (head < tail) {
            int cur = mz_q[head++];
            int cx = cur % MZ_W;
            int cy = cur / MZ_W;
            if (cur == MZ_END) break;
            for (i = 0; i < 4; i++) {
                int nx = cx + mz_dx[i];
                int ny = cy + mz_dy[i];
                if (nx < 0 || ny < 0 || nx >= MZ_W || ny >= MZ_H) continue;
                {
                    int n = ny * MZ_W + nx;
                    if (mz_g[n]) continue;        /* 墙 */
                    if (mz_pr[n] != -1) continue; /* 已访问 */
                    mz_pr[n] = (int16_t)cur;
                    mz_q[tail++] = (uint16_t)n;
                }
            }
        }
        {
            int c = mz_pr[MZ_END];        /* 终点父指针开始回溯 */
            while (c != st && c >= 0) {
                mz_sol[c] = 1;
                c = mz_pr[c];
            }
        }
    }
}

/* 新局: 重播种(种子 0 会被 rng_seed 修正) + 生成 + 复位状态 */
static void mz_new_game(void) {
    mz_gens++;
    rng_seed(&mz_rng, now_ms() ^ ((uint64_t)mz_gens * 0x9E3779B1u));
    mz_generate();
    mz_px = 0;
    mz_py = 0;
    mz_moves = 0;
    mz_over = false;
    mz_over_full = false;
    mz_show_sol = false;
}

void maze_enter(void) {
    mz_new_game();
    maze_render();
    disp_full();
}

/* 尝试移动; 撞墙/出界不动; 返回是否移动成功 */
static bool mz_move(int dx, int dy) {
    int nx = mz_px + dx;
    int ny = mz_py + dy;
    if (nx < 0 || ny < 0 || nx >= MZ_W || ny >= MZ_H) { audio_error(); return false; }
    if (mz_g[ny * MZ_W + nx]) { audio_error(); return false; }   /* 墙 */
    mz_px = nx;
    mz_py = ny;
    mz_moves++;
    if (mz_show_sol) mz_solve();                  /* 解法路径随玩家实时刷新 */
    if (nx == MZ_W - 1 && ny == MZ_H - 1) {       /* 到达终点 */
        mz_over = true;
        audio_win();
        led_fx_set(LED_FX_WIN);
    }
    return true;
}

/* 手写数字追加(无 snprintf 依赖) */
static void mz_append_u32(char *buf, unsigned *n, uint32_t v, unsigned cap) {
    char tmp[12];
    unsigned len = 0;
    if (v == 0) tmp[len++] = '0';
    while (v && len < 11) { tmp[len++] = (char)('0' + v % 10u); v /= 10u; }
    while (len && *n < cap) buf[(*n)++] = tmp[--len];
}

void maze_render(void) {
    fb_clear(false);
    /* 迷宫: 墙=实心黑格; 路=空白; 解法路径=中央点; 终点=星号 */
    for (int y = 0; y < MZ_H; y++) {
        for (int x = 0; x < MZ_W; x++) {
            int idx = y * MZ_W + x;
            int cx = MZ_OX + x * MZ_CELL;
            int cy = MZ_OY + y * MZ_CELL;
            if (mz_g[idx]) {
                fb_fill_rect(cx, cy, MZ_CELL, MZ_CELL, true);
            } else {
                if (mz_show_sol && mz_sol[idx] && idx != MZ_END)
                    fb_fill_tile(cx + 1, cy + 1, 8, 8, mz_dot);
                if (idx == MZ_END && !(mz_px == x && mz_py == y))
                    fb_text(cx + 2, cy + 1, "*", true);
            }
        }
    }
    /* 玩家小人(最后画, 永远在最上层) */
    fb_fill_tile(MZ_OX + mz_px * MZ_CELL + 1,
                 MZ_OY + mz_py * MZ_CELL + 1, 8, 8, mz_ball);
    /* HUD 顶栏 */
    if (mz_over) {
        fb_fill_rect(0, 0, CCG_W, CCG_HUD_H - 1, false);
        fb_text(2, 2, "SOLVED!", true);
        fb_text(CCG_W - 2 - text_width("OK/N:RETRY BACK:QUIT"), 2,
                "OK/N:RETRY BACK:QUIT", true);
        if (!mz_over_full) { mz_over_full = true; disp_force_full(); }
    } else {
        fb_text(0, 0, "MAZE", true);
        {
            char buf[24];
            unsigned n = 0;
            const char *p = "MOVES ";
            while (*p && n < 23) buf[n++] = *p++;
            mz_append_u32(buf, &n, mz_moves, 23);
            buf[n] = 0;
            fb_text(CCG_W - 4 - text_width(buf), 0, buf, true);
        }
        fb_hline(0, CCG_HUD_H - 1, CCG_W, true);
    }
}

void maze_on_key(const key_event_t *ev) {
    bool dir = ev->key == K_UP || ev->key == K_DOWN ||
               ev->key == K_LEFT || ev->key == K_RIGHT;
    if (ev->is_repeat && !dir) return;   /* 确认键/字母忽略重复, 方向可重复 */
    if (mz_over) {
        if (ev->key == K_OK || (ev->key == K_CHAR && ev->ch == 'n'))
            maze_enter();
        else if (ev->key == K_BACK || ev->key == K_QUIT)
            s_exit_request = true;
        return;
    }
    switch (ev->key) {
    case K_UP:    if (mz_move(0, -1) && !ev->is_repeat) audio_move(); break;
    case K_DOWN:  if (mz_move(0, 1) && !ev->is_repeat) audio_move(); break;
    case K_LEFT:  if (mz_move(-1, 0) && !ev->is_repeat) audio_move(); break;
    case K_RIGHT: if (mz_move(1, 0) && !ev->is_repeat) audio_move(); break;
    case K_CHAR:
        switch (ev->ch) {
        case 'w': if (mz_move(0, -1) && !ev->is_repeat) audio_move(); break;
        case 's': if (mz_move(0, 1) && !ev->is_repeat) audio_move(); break;
        case 'a': if (mz_move(-1, 0) && !ev->is_repeat) audio_move(); break;
        case 'd': if (mz_move(1, 0) && !ev->is_repeat) audio_move(); break;
        case 'n': maze_enter(); break;
        case 'h': case 'H':   /* 's' 已被 WASD 下行占用, 解法用 H(HINT) */
            mz_show_sol = !mz_show_sol;
            if (mz_show_sol) mz_solve();
            break;
        default: break;
        }
        break;
    case K_OK: break;   /* 游戏内确认键无操作 */
    case K_BACK:
    case K_PAUSE: {
        pause_sel_t sel = PAUSE_RESUME;
        if (ui_pause_run(&sel)) {
            if (sel == PAUSE_RESTART) maze_enter();
        } else {
            s_exit_request = true;
        }
        break;
    }
    case K_QUIT: s_exit_request = true; break;
    default: break;
    }
}

void maze_tick(uint64_t now) { (void)now; }
void maze_exit(void) {}
