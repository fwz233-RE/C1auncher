/* CHINESE CHECKERS — 中国跳棋: 六角星棋盘(半径 4, 61 格点) 2 人对战 AI
 * 棋盘: 9x9 栅格内嵌正六边形(行宽 5..9..5, 行错位), 立方坐标
 *       q = x-4, r = y-4, s = -q-r;  |q|,|r|,|s| <= 4 即合法格点
 * 开局: 玩家 10 子占上三角(r<=-1 且 -4-r<=q<=0), AI 10 子占对角下三角
 * 规则: 一步移到相邻空位, 或跳过相邻子(任意颜色)落其后空位, 跳后可连续跳
 * 胜利: 己方 10 子全部进入对方目标三角; 连续 CC_DRAW_LIMIT 步无进展判和
 * AI: 贪心——逐子 BFS 全部可达位, 主分=到对角角(4,0)立方距离(只进不退),
 *     同分时跳数少者优, 疏开拥挤(落点邻己子*3), 加随机破平局; 全局取最低分落点
 * 显示: 格点=小方块, 玩家实心 / AI 空心, 光标反白 9x9 方块(最后画),
 *       选中后可达位画 5x5 实心点; 两侧栏 TURN 指示 / HOME 进度
 * 输入驱动 + tick 延迟 AI(700ms); 移动=快刷, 开局/结束=全刷 */
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

#define CC_GW 9
#define CC_CELLW 26
#define CC_ROWH 13
#define CC_OX 44                       /* 棋盘左缘(侧栏 2..40 / 254..294) */
#define CC_OY 18                       /* 棋盘顶缘(垂直居中) */
#define CC_AI_DELAY_MS 700
#define CC_DRAW_LIMIT 120
#define CC_INV 0x7f                    /* 无效格标记 */

void ccheckers_render(void);

static uint8_t cc_board[CC_GW][CC_GW]; /* 0=空 1=玩家 2=AI CC_INV=无效 */
static int cc_turn;                    /* 1=玩家 2=AI */
static int cc_winner;                  /* 0=进行中 1=玩家 2=AI 3=和 */
static int cc_cx, cc_cy;               /* 光标(格坐标) */
static bool cc_sel;
static int cc_sx, cc_sy;               /* 选中的子 */
static bool cc_over, cc_over_full;
static int cc_moves;                   /* 步数 */
static int cc_no_prog;                 /* 连续无净进展步数 */
static int cc_home_max[2];             /* 双方历史最大入目标三角子数 */
static int cc_home_total_max;          /* 两者之和(净进展度量) */
static uint64_t cc_ai_at;              /* AI 最早行动时刻 */
static rng_t cc_rng;

/* BFS 工作区(81 元素, 全静态) */
static int8_t cc_vis[CC_GW * CC_GW];   /* 0=未达; >0=路径长 */
static int8_t cc_par[CC_GW * CC_GW];   /* 父格索引, -1=源 */
static uint8_t cc_que[CC_GW * CC_GW];  /* BFS 队列 */

/* 立方坐标六方向 (dq,dr) */
static const int8_t cc_dir[6][2] = {
    { 1, -1 }, { 1, 0 }, { 0, 1 }, { -1, 1 }, { -1, 0 }, { 0, -1 }
};

/* (x,y) 是否合法格点(六边形内) */
static int cc_valid(int x, int y) {
    if (x < 0 || x > 8 || y < 0 || y > 8) return 0;
    int q = x - 4, r = y - 4;
    int s = -q - r;
    int a = q < 0 ? -q : q, b = r < 0 ? -r : r, c = s < 0 ? -s : s;
    return a <= 4 && b <= 4 && c <= 4;
}

/* 区域: 0=中盘 1=玩家起始三角(上) 2=AI 起始三角(下) */
static int cc_region(int x, int y) {
    int q = x - 4, r = y - 4;
    if (r <= -1 && q >= -4 - r && q <= 0) return 1;
    if (r >= 1 && q >= 0 && q <= 4 - r) return 2;
    return 0;
}

/* 立方距离 */
static int cc_cdist(int x1, int y1, int x2, int y2) {
    int q1 = x1 - 4, r1 = y1 - 4;
    int q2 = x2 - 4, r2 = y2 - 4;
    int dq = q1 - q2, dr = r1 - r2, ds = -dq - dr;
    int a = dq < 0 ? -dq : dq, b = dr < 0 ? -dr : dr, c = ds < 0 ? -ds : ds;
    return (a + b + c) / 2;
}

/* 本行最左列号 */
static int cc_row_start(int y) { return (y < 4) ? 4 - y : y - 4; }

/* 格点中心像素坐标 */
static void cc_cell_center(int x, int y, int *px, int *py) {
    *px = CC_OX + CC_CELLW / 2 + (x - cc_row_start(y)) * CC_CELLW;
    *py = CC_OY + CC_ROWH / 2 + y * CC_ROWH;
}

/* 从 (sx,sy) BFS: 一步邻接空位 或 跳过相邻子(任意色)落其后空位, 跳后可续跳。
 * 结果写入 cc_vis(路径长)/cc_par(父格); 源格不算可达。 */
static void cc_bfs(int sx, int sy) {
    int si = sy * CC_GW + sx;
    for (int i = 0; i < CC_GW * CC_GW; i++) cc_vis[i] = 0;
    int head = 0, tail = 0;
    cc_que[tail++] = (uint8_t)si;
    cc_vis[si] = 1;              /* 占位: 源占用 */
    cc_par[si] = -1;
    while (head < tail) {
        int idx = cc_que[head++];
        int cx = idx % CC_GW, cy = idx / CC_GW;
        int q = cx - 4, r = cy - 4;
        bool from_src = (idx == si);
        for (int d = 0; d < 6; d++) {
            int nq = q + cc_dir[d][0], nr = r + cc_dir[d][1];
            int nx = nq + 4, ny = nr + 4;
            if (!cc_valid(nx, ny)) continue;
            int nidx = ny * CC_GW + nx;
            if (cc_board[ny][nx] == 0) {
                if (!from_src) continue;   /* 跳后只能续跳, 不能走一步 */
                if (cc_vis[nidx] == 0) {
                    cc_vis[nidx] = 1;
                    cc_par[nidx] = (int8_t)idx;
                    cc_que[tail++] = (uint8_t)nidx;
                }
            } else {
                /* 跳: 越过此子落其后 */
                int jq = nq + cc_dir[d][0], jr = nr + cc_dir[d][1];
                int jx = jq + 4, jy = jr + 4;
                if (!cc_valid(jx, jy)) continue;
                int jidx = jy * CC_GW + jx;
                if (cc_board[jy][jx] != 0) continue;
                if (cc_vis[jidx] == 0) {
                    cc_vis[jidx] = (int8_t)(cc_vis[idx] + 1);
                    cc_par[jidx] = (int8_t)idx;
                    cc_que[tail++] = (uint8_t)jidx;
                }
            }
        }
    }
    cc_vis[si] = 0;              /* 源不可作为目标 */
}

/* 执行移动(合法性已由 BFS 保证) */
static void cc_apply_move(int sx, int sy, int tx, int ty) {
    cc_board[ty][tx] = cc_board[sy][sx];
    cc_board[sy][sx] = 0;
}

/* who 方已进入对方三角的子数 */
static int cc_home_count(int who) {
    int target = (who == 1) ? 2 : 1;
    int n = 0;
    for (int y = 0; y < CC_GW; y++)
        for (int x = 0; x < CC_GW; x++)
            if (cc_board[y][x] == (uint8_t)who && cc_region(x, y) == target) n++;
    return n;
}

/* 落子收尾: 计步/净进展/胜负/和棋
 * 净进展 = 双方"历史最大入三角子数之和"严格增加(防双方互堵时的涓流死循环) */
static void cc_after_move(int who) {
    (void)who;
    cc_moves++;
    cc_sel = false;
    int h1 = cc_home_count(1), h2 = cc_home_count(2);
    if (h1 > cc_home_max[0]) cc_home_max[0] = h1;
    if (h2 > cc_home_max[1]) cc_home_max[1] = h2;
    if (cc_home_max[0] + cc_home_max[1] > cc_home_total_max) {
        cc_home_total_max = cc_home_max[0] + cc_home_max[1];
        cc_no_prog = 0;
    } else {
        cc_no_prog++;
    }
    if (cc_no_prog >= CC_DRAW_LIMIT) { cc_winner = 3; cc_over = true; return; }
    if (h1 >= 10) {
        cc_winner = 1;
        cc_over = true;
        audio_win();                 /* 玩家 10 子全入对方三角 */
        led_fx_set(LED_FX_WIN);
        return;
    }
    if (h2 >= 10) {
        cc_winner = 2;
        cc_over = true;
        audio_lose();                /* AI 10 子全入玩家三角 */
        led_fx_set(LED_FX_LOSE);
        return;
    }
}

/* 目标格相邻己子数(疏开用) */
static int cc_own_adj(int x, int y, int who) {
    int q = x - 4, r = y - 4, n = 0;
    for (int d = 0; d < 6; d++) {
        int nx = q + cc_dir[d][0] + 4, ny = r + cc_dir[d][1] + 4;
        if (cc_valid(nx, ny) && cc_board[ny][nx] == (uint8_t)who) n++;
    }
    return n;
}

/* AI 贪心选一步(全局最低分); 第一遍只进不退, 第二遍允许回退防死锁。
 * 分 = 目标角(4,0)距离*100 + 跳数*2 - 跳奖励(跳>=1 且同距时跳优) + 随机破平 */
static bool cc_ai_pick(bool allow_back) {
    int best = 1 << 30;
    int bsx = -1, bsy = -1, btx = 0, bty = 0;
    for (int y = 0; y < CC_GW; y++)
        for (int x = 0; x < CC_GW; x++) {
            if (cc_board[y][x] != 2) continue;
            int cur = cc_cdist(x, y, 4, 0);
            cc_bfs(x, y);
            for (int i = 0; i < CC_GW * CC_GW; i++) {
                if (cc_vis[i] <= 0) continue;
                int vx = i % CC_GW, vy = i / CC_GW;
                int nd = cc_cdist(vx, vy, 4, 0);
                if (!allow_back && nd > cur) continue;
                int plen = cc_vis[i];
                int crowd = (cc_region(vx, vy) == 1)
                                ? 0                     /* 目标三角内不忌拥挤 */
                                : cc_own_adj(vx, vy, 2) * 3;
                int score = nd * 100 + plen * 2 - (plen > 1 ? 4 : 0) +
                            crowd + (int)rng_range(&cc_rng, 3);
                if (score < best) {
                    best = score;
                    bsx = x; bsy = y; btx = vx; bty = vy;
                }
            }
        }
    if (bsx < 0) return false;
    cc_apply_move(bsx, bsy, btx, bty);
    cc_cx = btx;
    cc_cy = bty;
    /* 玩家选中子被挪走/吃掉 → 取消选中 */
    if (cc_sel && (cc_board[cc_sy][cc_sx] != 1 ||
                   (cc_sx == btx && cc_sy == bty)))
        cc_sel = false;
    return true;
}

static void cc_ai_move(void) {
    if (cc_ai_pick(false)) return;
    if (cc_ai_pick(true)) return;
    cc_winner = 1;               /* AI 完全无路可走 → 玩家胜 */
    cc_over = true;
}

/* 执行 AI 回合(到点由 tick/on_key 调用) */
static void cc_ai_turn(void) {
    if (cc_over || cc_turn != 2) return;
    cc_ai_move();
    if (cc_over) return;
    cc_after_move(2);
    if (cc_over) return;
    cc_turn = 1;
}

/* 光标移动: 跳过无效格(沿方向找最近的合法格, 9 步守卫) */
static void cc_cursor_move(int dx, int dy) {
    int nx = cc_cx + dx, ny = cc_cy + dy;
    for (int i = 0; i < 9; i++) {
        if (nx < 0 || nx > 8 || ny < 0 || ny > 8) break;
        if (cc_valid(nx, ny)) { cc_cx = nx; cc_cy = ny; return; }
        nx += dx;
        ny += dy;
    }
}

/* 玩家操作: 选中 / 落子 */
static void cc_handle_ok(void) {
    if (cc_turn != 1 || cc_over) return;
    if (!cc_sel) {
        if (cc_board[cc_cy][cc_cx] == 1) {
            cc_sel = true;
            cc_sx = cc_cx;
            cc_sy = cc_cy;
        }
        return;
    }
    if (cc_cx == cc_sx && cc_cy == cc_sy) { cc_sel = false; return; }
    if (cc_board[cc_cy][cc_cx] == 1) { cc_sx = cc_cx; cc_sy = cc_cy; return; }
    cc_bfs(cc_sx, cc_sy);
    if (cc_vis[cc_cy * CC_GW + cc_cx] <= 0) {         /* 非法目标: 保持选择 */
        audio_error();
        return;
    }
    cc_apply_move(cc_sx, cc_sy, cc_cx, cc_cy);
    audio_move();                    /* 落子 */
    cc_sel = false;
    cc_sx = cc_sy = 0;
    cc_after_move(1);
    if (cc_over) return;
    cc_turn = 2;
    cc_ai_at = now_ms() + CC_AI_DELAY_MS;
}

static void cc_new(void) {
    for (int y = 0; y < CC_GW; y++)
        for (int x = 0; x < CC_GW; x++)
            cc_board[y][x] = cc_valid(x, y) ? 0 : CC_INV;
    for (int y = 0; y < CC_GW; y++)
        for (int x = 0; x < CC_GW; x++) {
            int reg = cc_region(x, y);
            if (reg == 1) cc_board[y][x] = 1;
            else if (reg == 2) cc_board[y][x] = 2;
        }
    cc_turn = 1;
    cc_winner = 0;
    cc_cx = 4;
    cc_cy = 3;                   /* 光标起点: 玩家三角内一枚子 */
    cc_sel = false;
    cc_sx = cc_sy = 0;
    cc_over = false;
    cc_over_full = false;
    cc_moves = 0;
    cc_no_prog = 0;
    cc_home_max[0] = 0;
    cc_home_max[1] = 0;
    cc_home_total_max = 0;
    cc_ai_at = 0;
}

void ccheckers_enter(void) {
    rng_seed(&cc_rng, now_ms() ^ 0x2c0ffeeu);
    cc_new();
    ccheckers_render();
    disp_full();
}

void ccheckers_exit(void) {}

void ccheckers_tick(uint64_t now) {
    (void)now;
    if (cc_over || cc_turn != 2) return;
    if (now_ms() >= cc_ai_at) cc_ai_turn();
}

void ccheckers_on_key(const key_event_t *ev) {
    /* 重复事件只响应方向键 */
    if (ev->is_repeat && ev->key != K_LEFT && ev->key != K_RIGHT &&
        ev->key != K_UP && ev->key != K_DOWN)
        return;
    if (cc_over) {
        if (ev->key == K_OK || (ev->key == K_CHAR && ev->ch == 'n'))
            ccheckers_enter();
        else if (ev->key == K_BACK || ev->key == K_QUIT)
            s_exit_request = true;
        return;
    }
    if (ev->key == K_BACK || ev->key == K_PAUSE) {
        pause_sel_t sel;
        if (ui_pause_run(&sel)) {
            if (sel == PAUSE_RESTART) ccheckers_enter();
        } else {
            s_exit_request = true;
        }
        return;
    }
    if (ev->key == K_QUIT) { s_exit_request = true; return; }
    /* AI 回合: 到点先推进 AI(指示保持可见), 本键随后处理 */
    if (cc_turn == 2) {
        if (now_ms() >= cc_ai_at) {
            cc_ai_turn();
            if (cc_over) return;
        } else if (ev->key == K_OK || ev->key == K_CHAR) {
            return;              /* 未到点: 忽略确认/字母 */
        }
    }
    switch (ev->key) {
    case K_LEFT: cc_cursor_move(-1, 0); break;
    case K_RIGHT: cc_cursor_move(1, 0); break;
    case K_UP: cc_cursor_move(0, -1); break;
    case K_DOWN: cc_cursor_move(0, 1); break;
    case K_OK: cc_handle_ok(); break;
    case K_CHAR:
        if (ev->ch == 'a') cc_cursor_move(-1, 0);
        else if (ev->ch == 'd') cc_cursor_move(1, 0);
        else if (ev->ch == 'w') cc_cursor_move(0, -1);
        else if (ev->ch == 's') cc_cursor_move(0, 1);
        else if (ev->ch == 'n') ccheckers_enter();
        break;
    default: break;
    }
}

/* ---- 渲染 ---- */

static void cc_itoa(char *buf, int v) {
    int i = 0;
    if (v == 0) buf[i++] = '0';
    while (v > 0 && i < 8) { buf[i++] = (char)('0' + v % 10); v /= 10; }
    buf[i] = 0;
    for (int a = 0, b = i - 1; a < b; a++, b--) {
        char t = buf[a];
        buf[a] = buf[b];
        buf[b] = t;
    }
}

/* 格点标记: 0=空(小方块) 1=玩家(实心) 2=AI(空心) */
static void cc_draw_cell(int x, int y) {
    int px, py;
    cc_cell_center(x, y, &px, &py);
    int reg = cc_region(x, y);
    if (reg != 0)                 /* 起始三角: 浅斜纹底 */
        fb_fill_tile(px - 6, py - 6, 13, 13, pat_get(PAT_SLASH_S));
    uint8_t p = cc_board[y][x];
    if (p == 1) fb_fill_rect(px - 3, py - 3, 7, 7, true);
    else if (p == 2) fb_stroke_rect_thick(px - 3, py - 3, 7, 7, 2, true);
    else fb_stroke_rect(px - 2, py - 2, 5, 5, true);
}

/* 光标: 反白 9x9(所有格画完后最后画) */
static void cc_draw_cursor(void) {
    int px, py;
    cc_cell_center(cc_cx, cc_cy, &px, &py);
    fb_fill_rect(px - 4, py - 4, 9, 9, true);
    uint8_t p = cc_board[cc_cy][cc_cx];
    if (p == 1) fb_stroke_rect(px - 3, py - 3, 7, 7, false);
    else if (p == 2) fb_fill_rect(px - 3, py - 3, 7, 7, false);
    else fb_fill_rect(px - 1, py - 1, 3, 3, false);
}

/* 选中框 + 全部可达位提示 */
static void cc_draw_sel(void) {
    if (!cc_sel || cc_board[cc_sy][cc_sx] != 1) return;
    int px, py;
    cc_cell_center(cc_sx, cc_sy, &px, &py);
    fb_stroke_rect_thick(px - 5, py - 5, 11, 11, 2, true);
    cc_bfs(cc_sx, cc_sy);
    for (int y = 0; y < CC_GW; y++)
        for (int x = 0; x < CC_GW; x++) {
            if (cc_vis[y * CC_GW + x] <= 0) continue;
            cc_cell_center(x, y, &px, &py);
            fb_fill_rect(px - 2, py - 2, 5, 5, true);
        }
}

void ccheckers_render(void) {
    fb_clear(false);
    /* HUD 顶栏: 左标题 + 右步数 */
    fb_text(0, 0, "C-CHECKERS", true);
    {
        char full[16];
        unsigned n = 0;
        const char *lab = "MOVE ";
        while (lab[n]) { full[n] = lab[n]; n++; }
        char num[8];
        cc_itoa(num, cc_moves);
        for (int j = 0; num[j]; j++) full[n++] = num[j];
        full[n] = 0;
        fb_text(CCG_W - text_width(full) - 4, 0, full, true);
    }
    fb_hline(0, CCG_HUD_H - 1, CCG_W, true);
    /* 棋盘 */
    for (int y = 0; y < CC_GW; y++)
        for (int x = 0; x < CC_GW; x++)
            if (cc_valid(x, y)) cc_draw_cell(x, y);
    /* 左栏: TURN 指示(当前回合行反白) */
    fb_text(2, 20, "TURN", true);
    {
        bool you = (cc_turn == 1) && !cc_over;
        bool ai = (cc_turn == 2) && !cc_over;
        if (you) fb_fill_rect(2, 30, 38, 15, true);
        fb_text(6, 32, "YOU", !you);
        fb_fill_rect(30, 34, 7, 7, !you);
        if (ai) fb_fill_rect(2, 46, 38, 15, true);
        fb_text(6, 48, "AI", !ai);
        fb_stroke_rect_thick(30, 50, 7, 7, 2, !ai);
    }
    /* 右栏: HOME 进度(已入对方三角子数) */
    fb_text(254, 20, "HOME", true);
    fb_text(254, 30, "YOU", true);
    {
        char num[4];
        cc_itoa(num, cc_home_count(1));
        fb_fill_rect(268, 28, 26, 16, true);
        fb_text_scale2(270, 29, num, false);
    }
    fb_text(254, 46, "AI", true);
    {
        char num[4];
        cc_itoa(num, cc_home_count(2));
        fb_fill_rect(268, 44, 26, 16, true);
        fb_text_scale2(270, 45, num, false);
    }
    /* 选中提示(棋子之后) */
    cc_draw_sel();
    /* 光标最后画 */
    if (!cc_over) cc_draw_cursor();
    /* 终局: HUD 区两行提示 + 强制全刷一次 */
    if (cc_over) {
        fb_fill_rect(0, 0, CCG_W, CCG_HUD_H - 1, false);
        const char *res = (cc_winner == 1) ? "YOU WIN!" :
                          (cc_winner == 2) ? "AI WINS" : "DRAW";
        fb_text(2, 2, res, true);
        fb_text(CCG_W - 2 - text_width("OK/N:RETRY BACK:QUIT"), 2,
                "OK/N:RETRY BACK:QUIT", true);
        if (!cc_over_full) {
            cc_over_full = true;
            disp_force_full();
        }
    }
}
