/* RAIL PLANNER — 铺轨连通站点 (8x6 棋盘)
 * S=出发站, 1..n=目标站; 在空格铺轨道, 让 S 与全部目标站 8-连通即过关
 * OK 循环轨段: 空→直→弯→十字→空; DEL 擦除; N 下一关
 * 轨道连通只看格本身(死端简化允许); 轨段画法自适应邻居方向(纯装饰)
 * 3 关 ROM(目标站 1/2/3) + 随机关(目标站 4..6): 随机走迷宫预置轨道,
 *   留缺口, 保证可解(测试验证); 预置轨 = 点纹底+细轨, 玩家轨 = 实心粗轨
 * 关数推进: 过关 OK/N → 下一关, 随机关同关 OK 重开一局新盘
 * 输入驱动回合制; 静态前缀 rl_; 零 malloc; 像素坐标 int
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
#include "../platform/time.h"
#include "../rng.h"

#define RL_COLS 8
#define RL_ROWS 6
#define RL_CELLS (RL_COLS * RL_ROWS)
#define RL_CELL_SZ 22                                /* 22px 格 */
#define RL_OX ((CCG_W - RL_COLS * RL_CELL_SZ) / 2)   /* 60 */
#define RL_OY (CCG_HUD_H + (CCG_H - CCG_HUD_H - RL_ROWS * RL_CELL_SZ) / 2) /* 18 */
#define RL_ROM_LEVELS 3
#define RL_T_MAX 6                                   /* 目标站上限 */

/* 格值: 0 空, 1 直, 2 竖(仅预置), 3 弯, 4 十字, 5 出发站, 6..11 目标站 */
enum { RL_EMPTY = 0, RL_H, RL_V, RL_CUR, RL_X, RL_S, RL_T = 6 };

typedef struct { int t[RL_T_MAX]; uint8_t tcnt; } rl_gen_t;
static rl_gen_t rl_sta;     /* 当前关的目标站格(测试/进度用) */

/* ---- 3 关 ROM: 站 + 预置轨(格值), 缺口留给玩家 ---- */
typedef struct { int cell; uint8_t v; } rl_rom_tile_t;
typedef struct {
    int s;                      /* S 格 */
    int t[RL_T_MAX];            /* 目标站格 */
    uint8_t tcnt;
    const rl_rom_tile_t *fixed; /* 预置轨列表 */
    int nfix;
} rl_rom_t;

/* 格 = 行*8+列; 目标站的 8 邻域内不留预置轨/站(否则开局即连, 白给)
 * 走廊 8-邻域相连, 缺口由玩家补齐(站 8 邻域内缺口可填, 玩家铺轨为装饰)
 * L1: S(1,1)=9 T(3,7)=31  缺口 (3,6)=30 — 铺 1 格
 *     预置 (2,1)=17 (3,1)=25 (3,2)=26 弯 (3,3)=27 (3,4)=28 (3,5)=29
 * L2: S(3,3)=27 T1(5,1)=41 T2(0,7)=7  缺口 (5,2)=42 (1,6)=14 (1,7)=15
 *     预置 (4,3)=35 (5,3)=43 (3,4)=28 (3,5)=29 (2,5)=21 (1,5)=13
 * L3: S(4,1)=33 T1(1,1)=9 T2(5,3)=43 T3(3,6)=30
 *     缺口 (2,1)=17 (4,2)=34 (4,3)=35 (3,5)=29
 *     预置 (3,1)=25 (3,2)=26 弯 (3,3)=27 (3,4)=28 */
static const rl_rom_tile_t rl_rom1_fix[] = {
    { 17, RL_H }, { 25, RL_H }, { 26, RL_CUR }, { 27, RL_V },
    { 28, RL_V }, { 29, RL_V },
};
static const rl_rom_tile_t rl_rom2_fix[] = {
    { 35, RL_H }, { 43, RL_H }, { 28, RL_H }, { 29, RL_H }, { 21, RL_V },
    { 13, RL_V },
};
static const rl_rom_tile_t rl_rom3_fix[] = {
    { 25, RL_H }, { 26, RL_CUR }, { 27, RL_V }, { 28, RL_V },
};
static const rl_rom_t rl_rom[RL_ROM_LEVELS] = {
    { 9, { 31 }, 1, rl_rom1_fix, 6 },
    { 27, { 41, 7 }, 2, rl_rom2_fix, 6 },
    { 33, { 9, 43, 30 }, 3, rl_rom3_fix, 4 },
};

static uint8_t rl_grid[RL_CELLS];   /* 格值 */
static bool rl_fixed[RL_CELLS];     /* 预置(站/固定轨不可改) */
static uint8_t rl_cur;              /* 光标格 */
static int rl_level;                /* 0..2 ROM, >=3 随机 */
static int rl_tcnt;                 /* 目标站数 */
static int rl_done;                 /* 已连通目标站数 */
static bool rl_over, rl_over_full;
static rng_t rl_rng;
static uint32_t rl_gen_cnt;         /* 随机生成次数(播种混合) */

void rails_render(void);

static bool rl_is_station(int c) { return rl_grid[c] >= RL_S; }
static bool rl_is_rail(int c) { return rl_grid[c] >= RL_H && rl_grid[c] <= RL_X; }
static bool rl_solid(int c) { return rl_grid[c] != RL_EMPTY; } /* 连通视为实心 */

/* 两格是否 8-邻(含对角) */
static bool rl_adj8(int a, int b) {
    int dx = a % RL_COLS - b % RL_COLS;
    int dy = a / RL_COLS - b / RL_COLS;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    return dx <= 1 && dy <= 1;
}

/* 8-连通 BFS: S 出发, 统计可达目标站数 */
static void rl_conn(void) {
    uint8_t seen[RL_CELLS];
    uint8_t q[RL_CELLS];
    int s = -1;
    for (int i = 0; i < RL_CELLS; i++) { seen[i] = 0; if (rl_grid[i] == RL_S) s = i; }
    if (s < 0) { rl_done = 0; return; }
    int qh = 0, qt = 0;
    seen[s] = 1;
    q[qt++] = (uint8_t)s;
    while (qh < qt) {
        int c = q[qh++];
        int x = c % RL_COLS, y = c / RL_COLS;
        for (int dy = -1; dy <= 1; dy++) {
            for (int dx = -1; dx <= 1; dx++) {
                if (!dx && !dy) continue;
                int nx = x + dx, ny = y + dy;
                if (nx < 0 || nx >= RL_COLS || ny < 0 || ny >= RL_ROWS) continue;
                int nc = ny * RL_COLS + nx;
                if (!seen[nc] && rl_solid(nc)) { seen[nc] = 1; q[qt++] = (uint8_t)nc; }
            }
        }
    }
    rl_done = 0;
    for (int i = 0; i < rl_tcnt; i++)
        if (rl_sta.t[i] >= 0 && seen[rl_sta.t[i]]) rl_done++;
}

/* ---- 光标移动 ---- */
static void rl_move(int dx, int dy) {
    int x = rl_cur % RL_COLS, y = rl_cur / RL_COLS;
    int nx = x + dx, ny = y + dy;
    if (nx < 0 || nx >= RL_COLS || ny < 0 || ny >= RL_ROWS) return;
    rl_cur = (uint8_t)(ny * RL_COLS + nx);
}

/* ---- 铺设: OK 循环 空→直→弯→十字→空 ---- */
static void rl_ok(void) {
    int c = rl_cur;
    if (rl_fixed[c]) {                  /* 站/预置轨不可改 */
        audio_error();
        return;
    }
    uint8_t v = rl_grid[c];
    if (v == RL_EMPTY) v = RL_H;
    else if (v == RL_H) v = RL_CUR;
    else if (v == RL_CUR) v = RL_X;
    else v = RL_EMPTY;
    rl_grid[c] = v;
    rl_conn();
    if (!rl_over && rl_done == rl_tcnt) {
        rl_over = true;
        audio_win();
        led_fx_set(LED_FX_WIN);
    } else {
        audio_select();
    }
}

static void rl_del(void) {
    int c = rl_cur;
    if (rl_fixed[c]) return;
    if (rl_grid[c] != RL_EMPTY) {
        rl_grid[c] = RL_EMPTY;
        rl_conn();
    }
}

/* ---- 关卡装载 ---- */
static void rl_apply_fixed(const rl_rom_t *rom) {
    for (int i = 0; i < rom->nfix; i++) {
        int c = rom->fixed[i].cell;
        rl_grid[c] = rom->fixed[i].v;
        rl_fixed[c] = true;
    }
}

static void rl_start_rom(int lv) {
    for (int i = 0; i < RL_CELLS; i++) { rl_grid[i] = RL_EMPTY; rl_fixed[i] = false; }
    const rl_rom_t *rom = &rl_rom[lv];
    rl_tcnt = (int)rom->tcnt;
    rl_grid[rom->s] = RL_S;
    rl_fixed[rom->s] = true;
    for (int i = 0; i < rl_tcnt; i++) {
        rl_grid[rom->t[i]] = (uint8_t)(RL_T + i);
        rl_fixed[rom->t[i]] = true;
    }
    rl_apply_fixed(rom);
}

/* ---- 随机关: 随机走迷宫铺预置轨, 留缺口保证可解 ---- */
static void rl_gen_level(int lv) {
    int n = lv + 1;
    if (n > RL_T_MAX) n = RL_T_MAX;
    int attempt = 0;
    for (; attempt < 16; attempt++) {
        for (int i = 0; i < RL_CELLS; i++) { rl_grid[i] = RL_EMPTY; rl_fixed[i] = false; }
        int s = (int)rng_range(&rl_rng, RL_CELLS);
        rl_grid[s] = RL_S;
        rl_fixed[s] = true;
        /* 随机 DFS 走迷宫, 目标长 12+3n */
        int want = 12 + 3 * n;
        if (want > RL_CELLS - 2) want = RL_CELLS - 2;
        uint8_t walk[RL_CELLS];
        int wlen = 0;
        uint8_t vis[RL_CELLS];
        for (int i = 0; i < RL_CELLS; i++) vis[i] = 0;
        walk[wlen++] = (uint8_t)s;
        vis[s] = 1;
        int guard = 0;
        while (wlen < want && guard++ < 400) {
            int c = walk[wlen - 1];
            int x = c % RL_COLS, y = c / RL_COLS;
            int cand[8], cn = 0;
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++) {
                    if (!dx && !dy) continue;
                    int nx = x + dx, ny = y + dy;
                    if (nx < 0 || nx >= RL_COLS || ny < 0 || ny >= RL_ROWS) continue;
                    if (!vis[ny * RL_COLS + nx]) cand[cn++] = ny * RL_COLS + nx;
                }
            if (cn > 0) {
                int c2 = cand[rng_range(&rl_rng, (uint32_t)cn)];
                walk[wlen++] = (uint8_t)c2;
                vis[c2] = 1;
            } else if (wlen > 1) {
                wlen--;              /* 回溯 */
            } else break;
        }
        if (wlen < n + 2) continue;  /* 走廊太短, 重试 */
        /* 从走廊(不含 S, 不 8 邻 S)随机挑 n 个互异目标站 */
        for (int i = 0; i < n; i++) {
            int p;
            int guard2 = 0;
            do {
                p = 1 + (int)rng_range(&rl_rng, (uint32_t)(wlen - 1));
            } while (++guard2 < 50 &&
                     (rl_fixed[walk[p]] || rl_adj8(walk[p], s)));
            rl_grid[walk[p]] = (uint8_t)(RL_T + i);
            rl_fixed[walk[p]] = true;
        }
        {
            int sta_cnt = 0;
            for (int i = 0; i < RL_CELLS; i++)
                if (rl_grid[i] >= RL_T) sta_cnt++;
            if (sta_cnt != n) continue;  /* 站有重复, 整局重试 */
        }
        /* 走廊上约一半铺预置轨(站格跳过), 类型看邻居走向 */
        for (int i = 1; i < wlen; i++) {
            int c = walk[i];
            if (rl_is_station(c)) continue;
            if (rng_range(&rl_rng, 100) >= 55) continue;
            int x = c % RL_COLS, y = c / RL_COLS;
            int nh = 0, nv = 0;
            for (int j = 0; j < wlen; j++) {
                if (j == i) continue;
                int dx = walk[j] % RL_COLS - x, dy = walk[j] / RL_COLS - y;
                if (dy == 0 && (dx == 1 || dx == -1)) nh++;
                if (dx == 0 && (dy == 1 || dy == -1)) nv++;
            }
            rl_grid[c] = (nh && nv) ? RL_X : (nh ? RL_H : (nv ? RL_V : RL_CUR));
            rl_fixed[c] = true;
        }
        rl_tcnt = n;
        rl_sta.tcnt = (uint8_t)n;
        for (int i = 0; i < n; i++) rl_sta.t[i] = -1;
        for (int i = 0; i < RL_CELLS; i++)
            if (rl_grid[i] >= RL_T) rl_sta.t[rl_grid[i] - RL_T] = i;
        /* 目标站 8 邻域内的预置轨全部清空 → 开局各站必孤立(不白给) */
        for (int i = 0; i < n; i++) {
            int t = rl_sta.t[i];
            int tx = t % RL_COLS, ty = t / RL_COLS;
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++) {
                    int nx = tx + dx, ny = ty + dy;
                    if (nx < 0 || nx >= RL_COLS || ny < 0 || ny >= RL_ROWS) continue;
                    int nc = ny * RL_COLS + nx;
                    if (rl_fixed[nc] && rl_is_rail(nc)) {
                        rl_grid[nc] = RL_EMPTY;
                        rl_fixed[nc] = false;
                    }
                }
        }
        rl_conn();
        if (rl_done == rl_tcnt) continue;  /* 已连通=白给, 重试 */
        return;
    }
    /* 兜底: 清掉所有与目标站 8 邻的预置轨, 保证各站孤立未连通 */
    for (int i = 0; i < rl_tcnt; i++) {
        int t = rl_sta.t[i];
        int x = t % RL_COLS, y = t / RL_COLS;
        for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++) {
                int nx = x + dx, ny = y + dy;
                if (nx < 0 || nx >= RL_COLS || ny < 0 || ny >= RL_ROWS) continue;
                int nc = ny * RL_COLS + nx;
                if (rl_fixed[nc] && rl_is_rail(nc)) { rl_grid[nc] = RL_EMPTY; rl_fixed[nc] = false; }
            }
    }
    rl_conn();
}

static void rl_start(int lv) {
    rl_level = lv;
    rl_cur = 0;
    rl_over = false;
    rl_over_full = false;
    if (lv < RL_ROM_LEVELS) {
        rl_start_rom(lv);
        rl_sta.tcnt = (uint8_t)rl_rom[lv].tcnt;
        for (int i = 0; i < (int)rl_sta.tcnt; i++) rl_sta.t[i] = rl_rom[lv].t[i];
    } else {
        rl_gen_level(lv);
    }
    rl_conn();
}

/* ---- 入口/出口 ---- */
void rails_enter(void) {
    rl_gen_cnt++;
    rng_seed(&rl_rng, (uint64_t)now_ms() ^ ((uint64_t)rl_gen_cnt * 0x9E3779B1u) ^ 0x14A15u);
    rl_start(0);
    rails_render();
    disp_full();
}

void rails_exit(void) {}

void rails_tick(uint64_t now) { (void)now; }

/* ---- 渲染 ---- */
/* 卡方位自适应: 直/弯/十字按四邻走向选画法(纯装饰) */
static void rl_orients(int c, int *ho, int *vo) {
    int x = c % RL_COLS, y = c / RL_COLS;
    *ho = 0; *vo = 0;
    if (x > 0 && rl_solid(c - 1)) *ho |= 1;         /* 左 */
    if (x < RL_COLS - 1 && rl_solid(c + 1)) *ho |= 2; /* 右 */
    if (y > 0 && rl_solid(c - RL_COLS)) *vo |= 1;   /* 上 */
    if (y < RL_ROWS - 1 && rl_solid(c + RL_COLS)) *vo |= 2; /* 下 */
}

static void rl_draw_h(int x, int y, int m, int t, bool black) {
    fb_fill_rect(x + m, y + (RL_CELL_SZ - t) / 2, RL_CELL_SZ - 2 * m, t, black);
}
static void rl_draw_v(int x, int y, int m, int t, bool black) {
    fb_fill_rect(x + (RL_CELL_SZ - t) / 2, y + m, t, RL_CELL_SZ - 2 * m, black);
}
/* 四分之一圆弧(整数 Bresenham 点 + 3x3 印章), 连接两邻边 */
static void rl_draw_arc(int x, int y, int m, int r, int cu, int cv, bool black) {
    /* cu/cv: 相邻两条边, 1=上/左, 2=下/右; 弧心在对应角 */
    int cx = x + (cu == 1 ? m + r : RL_CELL_SZ - m - r);
    int cy = y + (cv == 1 ? m + r : RL_CELL_SZ - m - r);
    int bx = cu == 1 ? -1 : 1;      /* 弧相对弧心的象限方向 */
    int by = cv == 1 ? -1 : 1;
    int qx = 0, qy = r, d = 1 - r;  /* 第一八分圆 */
    while (qx <= qy) {
        /* 连接 cu(横)边与 cv(纵)边: 两八分区段点 */
        int px[2] = { bx * qx, bx * qy };
        int py[2] = { by * qy, by * qx };
        for (int k = 0; k < 2; k++)
            fb_fill_rect(cx + px[k] - 1, cy + py[k] - 1, 3, 3, black);
        if (d < 0) d += 2 * qx + 3;
        else { d += 2 * (qx - qy) + 5; qy--; }
        qx++;
    }
}

/* 轨段绘制: pre=true 预置(细轨+点纹底) */
static void rl_draw_track(int c, int x, int y, bool pre) {
    int m = pre ? 5 : 4;
    int t = pre ? 2 : 3;
    int ho, vo;
    rl_orients(c, &ho, &vo);
    int v = rl_grid[c];
    if (v == RL_H) {                      /* 直: 有横邻画横, 否则竖 */
        if (ho) rl_draw_h(x, y, m, t, true);
        else rl_draw_v(x, y, m, t, true);
    } else if (v == RL_V) {
        if (vo) rl_draw_v(x, y, m, t, true);
        else rl_draw_h(x, y, m, t, true);
    } else if (v == RL_X) {
        rl_draw_h(x, y, m, t, true);
        rl_draw_v(x, y, m, t, true);
    } else if (v == RL_CUR) {
        /* 弯: 选两条互相垂直的实心邻边画弧; 无则默认左上 */
        int cu, cv;
        if (ho && vo) { cu = (ho == 1 ? 1 : 2); cv = (vo == 1 ? 1 : 2); }
        else if (ho) { cu = (ho == 1 ? 1 : 2); cv = 1; }
        else { cu = 1; cv = 1; }
        rl_draw_arc(x, y, m, (RL_CELL_SZ - 2 * m) / 2 - 1, cu, cv, true);
    }
}

void rails_render(void) {
    fb_clear(false);
    /* 网格线 */
    for (int i = 0; i <= RL_COLS; i++)
        fb_vline(RL_OX + i * RL_CELL_SZ, RL_OY, RL_ROWS * RL_CELL_SZ, true);
    for (int j = 0; j <= RL_ROWS; j++)
        fb_hline(RL_OX, RL_OY + j * RL_CELL_SZ, RL_COLS * RL_CELL_SZ, true);
    /* 格内容 */
    for (int j = 0; j < RL_ROWS; j++) {
        for (int i = 0; i < RL_COLS; i++) {
            int c = j * RL_COLS + i;
            if (rl_grid[c] == RL_EMPTY) continue;
            int x = RL_OX + i * RL_CELL_SZ;
            int y = RL_OY + j * RL_CELL_SZ;
            if (rl_is_station(c)) {
                fb_fill_rect(x + 1, y + 1, RL_CELL_SZ - 2, RL_CELL_SZ - 2, true);
                char d[2] = { rl_grid[c] == RL_S ? 'S' : (char)('0' + rl_grid[c] - RL_T + 1), 0 };
                fb_text_scale2(x + 5, y + 4, d, false);
            } else {
                if (rl_fixed[c])
                    fb_fill_tile(x + 1, y + 1, RL_CELL_SZ - 2, RL_CELL_SZ - 2,
                                 pat_get(PAT_DOT_SPARSE));
                rl_draw_track(c, x, y, rl_fixed[c]);
            }
        }
    }
    /* 光标(最后画) */
    {
        int ccx = RL_OX + (rl_cur % RL_COLS) * RL_CELL_SZ;
        int ccy = RL_OY + (rl_cur / RL_COLS) * RL_CELL_SZ;
        fb_stroke_rect_thick(ccx - 1, ccy - 1, RL_CELL_SZ + 2, RL_CELL_SZ + 2, 2,
                             !rl_solid(rl_cur));
    }
    /* HUD */
    if (rl_over) {
        fb_fill_rect(0, 0, CCG_W, CCG_HUD_H - 1, false);
        fb_text(2, 2, "ALL LINKED!", true);
        fb_text(CCG_W - 2 - text_width("OK/N:RETRY BACK:QUIT"), 2,
                "OK/N:RETRY BACK:QUIT", true);
        if (!rl_over_full) { rl_over_full = true; disp_force_full(); }
    } else {
        fb_text(0, 0, "RAIL PLANNER", true);
        /* 右: LV<n> LINK <c>/<t> */
        char buf[24];
        unsigned n = 0;
        buf[n++] = 'L'; buf[n++] = 'V';
        buf[n++] = (char)('0' + (rl_level >= 9 ? 9 : rl_level + 1));
        buf[n++] = ' '; buf[n++] = 'L'; buf[n++] = 'I'; buf[n++] = 'N'; buf[n++] = 'K';
        buf[n++] = ' ';
        if (rl_done >= 10) buf[n++] = (char)('0' + rl_done / 10);
        buf[n++] = (char)('0' + rl_done % 10);
        buf[n++] = '/';
        buf[n++] = (char)('0' + rl_tcnt % 10);
        buf[n++] = 0;
        fb_text(CCG_W - 4 - text_width(buf), 0, buf, true);
        fb_hline(0, CCG_HUD_H - 1, CCG_W, true);
    }
}

/* ---- 按键 ---- */
void rails_on_key(const key_event_t *ev) {
    bool dir = ev->key == K_UP || ev->key == K_DOWN ||
               ev->key == K_LEFT || ev->key == K_RIGHT;
    if (ev->is_repeat && !dir) return;   /* 确认键/字母/DEL 忽略重复 */
    if (rl_over) {
        if (ev->key == K_OK || (ev->key == K_CHAR && ev->ch == 'n')) {
            rl_start(rl_level + 1);
            rails_render();
            disp_full();
        } else if (ev->key == K_BACK || ev->key == K_QUIT) {
            s_exit_request = true;
        }
        return;
    }
    switch (ev->key) {
    case K_UP: rl_move(0, -1); audio_tick(); break;
    case K_DOWN: rl_move(0, 1); audio_tick(); break;
    case K_LEFT: rl_move(-1, 0); audio_tick(); break;
    case K_RIGHT: rl_move(1, 0); audio_tick(); break;
    case K_CHAR:
        switch (ev->ch) {
        case 'w': rl_move(0, -1); audio_tick(); break;
        case 's': rl_move(0, 1); audio_tick(); break;
        case 'a': rl_move(-1, 0); audio_tick(); break;
        case 'd': rl_move(1, 0); audio_tick(); break;
        case 'n': audio_select(); rl_start(rl_level + 1); break;
        default: break;
        }
        break;
    case K_OK: rl_ok(); break;
    case K_DEL: rl_del(); audio_move(); break;
    case K_BACK:
    case K_PAUSE: {
        pause_sel_t sel;
        if (ui_pause_run(&sel)) {
            if (sel == PAUSE_RESTART) rl_start(rl_level);
        } else {
            s_exit_request = true;
        }
        break;
    }
    case K_QUIT: s_exit_request = true; break;
    default: break;
    }
}
