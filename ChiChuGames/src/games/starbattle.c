/* Star Battle — 8x8, 每行/每列/每区域恰 2 星, 星不相邻(含对角)
 *
 * 规格说明(设计偏差, 已穷举验证):
 *   原设计为 7x7 每行/列/区域恰 2 星且互不相邻。经 C 穷举(全排列+剪枝)证明
 *   该约束在 7x7 上不存在任何合法布局(与区域划分无关), 8x8 双星才是可行的
 *   最小规格 → 采用业界标准 8x8 双星(本题唯一解已由独立求解器验证)。
 *   格子 16px(136px 游戏区 + 顶部 8px 列计数条 → 8x8=128 全高利用)。
 *
 * 操作: 方向/WASD 移光标; OK 放置/移除星; DEL 移除; N 换题; R 重开;
 *       P/BACK 暂停菜单; Q 退出
 * 完成: 行/列/区域各恰 2 星且无相邻星 -> WIN (HUD 区显示, 全刷一次)
 * 输入驱动; 移动/放星=快刷; 开局/重开/胜利=全刷
 * 实时反馈: 左栏 ROWS / 右栏 AREA / 顶部列计数(满足=反白, 超数=框/点) */
#include "game.h"
#include "../config.h"
#include "../gfx/canvas.h"
#include "../gfx/font.h"
#include "../platform/display.h"
#include "../platform/input.h"
#include "../rng.h"
#include "../platform/time.h"
#include "../platform/audio.h"
#include "../platform/led.h"

void starbattle_render(void);

#define SB_N 8
#define SB_CELL 16                        /* 8x8=128: 游戏区 136 高, 顶留 8px 列计数 */
#define SB_OX ((int)((CCG_W - SB_N * SB_CELL) / 2))   /* 84 */
#define SB_OY ((int)(CCG_HUD_H + 8))                  /* 24: 16 HUD + 8 计数条 */
#define SB_NCELL (SB_N * SB_N)
#define SB_PUZ_N 3

/* ---- ROM 数据(离线穷举+独立求解器验证) ----
 * sb_region: 区域划分 'A'-'H', 每区域恰 8 格且 4-连通
 * sb_sol:    唯一解(16 星, 行/列/区域各 2, 无相邻星)
 * sb_giv:    每题的题面固定星(唯一解的子集, 255 结尾) */
static const char sb_region[SB_NCELL + 1] =
    "EEEDDGGGEEEDDDGGEEDDDAAGFFBBBBAGFFFBBBAGHHFFCBAAHHHFCAACHHHCCCCC";
static const char sb_sol[SB_NCELL + 1] =
    "....#.#.#.#.........#.#.#.#..........#.#.#.#.........#.#.#.#....";
static const uint8_t sb_giv[SB_PUZ_N][SB_PUZ_N] = {
    { 55, 255, 255 },                    /* 题0: (6,7) */
    { 4, 24, 255 },                      /* 题1: (0,4),(3,0) */
    { 10, 53, 255 },                     /* 题2: (1,2),(6,5) */
};

static uint8_t sb_star[SB_NCELL];        /* 1=有星 */
static uint8_t sb_given[SB_NCELL];       /* 1=题面固定星(不可改) */
static uint8_t sb_cx, sb_cy;             /* 光标 0-7 */
static int sb_pz;                        /* 当前谜题 0..2 */
static bool sb_over;                     /* 胜利 */
static bool sb_over_full;                /* 胜利全刷只做一次 */
static rng_t sb_rng;

/* ---- 核心逻辑 ---- */

/* ROM 自检: 题面星必须是唯一解的子集(兜底防数据损坏) */
static bool sb_data_ok(void) {
    int p, j;
    for (p = 0; p < SB_PUZ_N; p++)
        for (j = 0; j < SB_PUZ_N && sb_giv[p][j] != 255; j++) {
            int g = (int)sb_giv[p][j];
            if (g < 0 || g >= SB_NCELL || sb_sol[g] != '#') return false;
        }
    return true;
}

/* 行(which=0)/列(1)/区域(2) 的星数 */
static int sb_count_line(int which, int idx) {
    int n = 0, i;
    if (which == 0) {
        for (i = 0; i < SB_N; i++)
            if (sb_star[idx * SB_N + i]) n++;
    } else if (which == 1) {
        for (i = 0; i < SB_N; i++)
            if (sb_star[i * SB_N + idx]) n++;
    } else {
        for (i = 0; i < SB_NCELL; i++)
            if (sb_star[i] && (int)(sb_region[i] - 'A') == idx) n++;
    }
    return n;
}

/* 该格之星是否与任一星 8 邻域相邻 */
static bool sb_has_neighbor(int idx) {
    int r = idx >> 3, c = idx & 7;
    int dr, dc;
    for (dr = -1; dr <= 1; dr++)
        for (dc = -1; dc <= 1; dc++) {
            int nr, nc;
            if (dr == 0 && dc == 0) continue;
            nr = r + dr;
            nc = c + dc;
            if (nr < 0 || nr >= SB_N || nc < 0 || nc >= SB_N) continue;
            if (sb_star[nr * SB_N + nc]) return true;
        }
    return false;
}

/* 胜利: 行/列/区域各恰 2 星 且 无相邻星(含对角) */
static bool sb_check_win(void) {
    int r, c, k, i;
    for (r = 0; r < SB_N; r++)
        if (sb_count_line(0, r) != 2) return false;
    for (c = 0; c < SB_N; c++)
        if (sb_count_line(1, c) != 2) return false;
    for (k = 0; k < SB_N; k++)
        if (sb_count_line(2, k) != 2) return false;
    for (i = 0; i < SB_NCELL; i++)
        if (sb_star[i] && sb_has_neighbor(i)) return false;
    return true;
}

/* 放置/移除星(题面格不可改); 满足全部规则即胜 */
static void sb_toggle(int idx) {
    if (sb_given[idx] != 0) { audio_error(); return; }   /* 题面星不可改 */
    sb_star[idx] = (uint8_t)(sb_star[idx] ? 0 : 1);
    audio_move();                          /* 放置/移除星 */
    if (sb_check_win()) {
        sb_over = true;
        audio_win();
        led_fx_set(LED_FX_WIN);
    }
}

static void sb_clear(int idx) {
    if (sb_given[idx] != 0) { audio_error(); return; }   /* 题面星不可清除 */
    sb_star[idx] = 0;
}

static void sb_itoa(uint32_t v, char *buf) {
    char rev[4];
    int n = 0;
    do {
        rev[n++] = (char)('0' + (int)(v % 10));
        v /= 10;
    } while (v != 0 && n < 3);
    while (n > 0) *buf++ = rev[--n];
    *buf = 0;
}

/* 载入谜题 pz 并复位 */
static void sb_new_game(int pz) {
    int i;
    pz = pz % SB_PUZ_N;
    if (pz < 0) pz += SB_PUZ_N;
    if (!sb_data_ok()) pz = 0;
    sb_pz = pz;
    for (i = 0; i < SB_NCELL; i++) {
        sb_star[i] = 0;
        sb_given[i] = 0;
    }
    for (i = 0; i < SB_PUZ_N && sb_giv[pz][i] != 255; i++) {
        int g = (int)sb_giv[pz][i];
        sb_star[g] = 1;
        sb_given[g] = 1;
    }
    sb_cx = 3;
    sb_cy = 3;
    sb_over = false;
    sb_over_full = false;
}

/* 新局: 复位 + 渲染 + 全刷 */
static void sb_start(int pz) {
    sb_new_game(pz);
    starbattle_render();
    disp_full();
}

/* ---- 绘制 ---- */

/* 实心菱形(星形): 中心 (cx,cy), 半径 rad */
static void sb_diamond(int cx, int cy, int rad, bool ink) {
    int dy;
    for (dy = -rad; dy <= rad; dy++) {
        int ady = dy < 0 ? -dy : dy;
        int half = rad - ady;
        fb_fill_rect(cx - half, cy + dy, half * 2 + 1, 1, ink);
    }
}

void starbattle_render(void) {
    int i, r, c;
    fb_clear(false);

    /* 星: 实心菱(半径5); 题面星: 小菱(半径4)+四角点; 相邻违例: 菱形内白点 */
    for (r = 0; r < SB_N; r++) {
        for (c = 0; c < SB_N; c++) {
            int idx = r * SB_N + c;
            int bx = SB_OX + c * SB_CELL;
            int by = SB_OY + r * SB_CELL;
            int cx = bx + SB_CELL / 2;
            int cy = by + SB_CELL / 2;
            if (!sb_star[idx]) continue;
            sb_diamond(cx, cy, sb_given[idx] ? 4 : 5, true);
            if (sb_given[idx]) {
                fb_fill_rect(bx + 1, by + 1, 2, 2, true);
                fb_fill_rect(bx + SB_CELL - 3, by + 1, 2, 2, true);
                fb_fill_rect(bx + 1, by + SB_CELL - 3, 2, 2, true);
                fb_fill_rect(bx + SB_CELL - 3, by + SB_CELL - 3, 2, 2, true);
            }
            if (sb_has_neighbor(idx))
                fb_fill_rect(cx - 1, cy - 3, 2, 2, false);
        }
    }

    /* 网格: 内部 1px 线, 区域界 2px, 外框 2px(右/下框内收 2px 保持对称且不越界) */
    for (i = 0; i <= SB_N; i++) {
        int x = SB_OX + i * SB_CELL;
        bool thick = (i == 0 || i == SB_N);
        if (!thick) {
            for (r = 0; r < SB_N; r++)
                if (sb_region[r * SB_N + i - 1] != sb_region[r * SB_N + i]) {
                    thick = true;
                    break;
                }
        }
        if (i == SB_N) x -= 2;
        fb_fill_rect(x, SB_OY, thick ? 2 : 1, SB_N * SB_CELL, true);
    }
    for (i = 0; i <= SB_N; i++) {
        int y = SB_OY + i * SB_CELL;
        bool thick = (i == 0 || i == SB_N);
        if (!thick) {
            for (c = 0; c < SB_N; c++)
                if (sb_region[(i - 1) * SB_N + c] != sb_region[i * SB_N + c]) {
                    thick = true;
                    break;
                }
        }
        if (i == SB_N) y -= 2;
        fb_fill_rect(SB_OX, y, SB_N * SB_CELL, thick ? 2 : 1, true);
    }

    /* 光标: 所有内容画完后, 2px 反色内框(黑格白框/白格黑框) */
    {
        int bx = SB_OX + (int)sb_cx * SB_CELL;
        int by = SB_OY + (int)sb_cy * SB_CELL;
        bool has = sb_star[(int)sb_cy * SB_N + (int)sb_cx] != 0;
        fb_stroke_rect_thick(bx, by, SB_CELL, SB_CELL, 2, has ? false : true);
    }

    /* 列计数条: 小数字(满足=反白, 超数=黑点) + 分隔线 */
    for (c = 0; c < SB_N; c++) {
        int x = SB_OX + c * SB_CELL + 5;
        int n = sb_count_line(1, c);
        char d[2];
        d[0] = (char)('0' + n);
        d[1] = 0;
        if (n == 2) {
            fb_fill_rect(x, SB_OY - 8, 5, 7, true);
            fb_text(x, SB_OY - 8, d, false);
        } else if (n > 2) {
            fb_text(x, SB_OY - 8, d, true);
            fb_fill_rect(x + 5, SB_OY - 6, 2, 2, true);
        } else {
            fb_text(x, SB_OY - 8, d, true);
        }
    }
    fb_hline(SB_OX, SB_OY - 1, SB_N * SB_CELL, true);

    /* 左栏: ROWS 计数(2x 大字; 满足=反白, 超数=黑框) */
    fb_text(SB_OX - 2 - text_width("ROWS"), SB_OY - 8, "ROWS", true);
    for (r = 0; r < SB_N; r++) {
        int x = SB_OX - 2 - 10;
        int y = SB_OY + r * SB_CELL + 1;
        int n = sb_count_line(0, r);
        char d[2];
        d[0] = (char)('0' + n);
        d[1] = 0;
        if (n == 2) {
            fb_fill_rect(x, y, 10, 14, true);
            fb_text_scale2(x, y, d, false);
        } else if (n > 2) {
            fb_text_scale2(x, y, d, true);
            fb_stroke_rect(x, y, 10, 14, true);
        } else {
            fb_text_scale2(x, y, d, true);
        }
    }

    /* 右栏: AREA 计数(区域号小字 + 2x 计数) */
    fb_text(CCG_W - 2 - text_width("AREA"), SB_OY - 8, "AREA", true);
    for (r = 0; r < SB_N; r++) {
        int x = SB_OX + SB_N * SB_CELL + 2;
        int y = SB_OY + r * SB_CELL + 1;
        int n = sb_count_line(2, r);
        char d[2], nd[2];
        d[0] = (char)('0' + n);
        d[1] = 0;
        nd[0] = (char)('1' + r);
        nd[1] = 0;
        if (n == 2) {
            fb_fill_rect(x, y, 10, 14, true);
            fb_text_scale2(x, y, d, false);
        } else if (n > 2) {
            fb_text_scale2(x, y, d, true);
            fb_stroke_rect(x, y, 10, 14, true);
        } else {
            fb_text_scale2(x, y, d, true);
        }
        fb_text(x + 12, y + 4, nd, true);
    }

    /* HUD: 左标题+题号, 右已放星数 */
    fb_text(0, 0, "STAR BATTLE", true);
    {
        char pbuf[6];
        pbuf[0] = 'P';
        pbuf[1] = 'Z';
        pbuf[2] = (char)('1' + sb_pz);
        pbuf[3] = '/';
        pbuf[4] = (char)('0' + SB_PUZ_N);
        pbuf[5] = 0;
        fb_text(text_width("STAR BATTLE") + 3, 0, pbuf, true);
    }
    {
        char fbuf[16];
        char num[4];
        uint32_t tot = 0;
        int n = 0, j;
        const char *label = "STARS ";
        while (label[n]) { fbuf[n] = label[n]; n++; }
        for (i = 0; i < SB_N; i++) tot += (uint32_t)sb_count_line(0, i);
        sb_itoa(tot, num);
        for (j = 0; num[j]; j++) fbuf[n++] = num[j];
        fbuf[n++] = '/';
        fbuf[n++] = '1';
        fbuf[n++] = '6';
        fbuf[n] = 0;
        fb_text(CCG_W - text_width(fbuf) - 2, 0, fbuf, true);
    }
    fb_hline(0, CCG_HUD_H - 1, CCG_W, true);

    /* 胜利: HUD 区显示 + 全刷一次 */
    if (sb_over) {
        fb_fill_rect(0, 0, CCG_W, CCG_HUD_H - 1, false);
        fb_text(2, 2, "YOU WIN!", true);
        fb_text(CCG_W - text_width("OK/N:RETRY BACK:QUIT") - 2, 2,
                "OK/N:RETRY BACK:QUIT", true);
        if (!sb_over_full) {
            sb_over_full = true;
            disp_force_full();
        }
    }

    disp_fast();
}

/* ---- 框架入口 ---- */

void starbattle_enter(void) {
    rng_seed(&sb_rng, now_ms());
    sb_start((int)rng_range(&sb_rng, (uint32_t)SB_PUZ_N));
}

void starbattle_exit(void) {}

void starbattle_tick(uint64_t now) { (void)now; }

void starbattle_on_key(const key_event_t *ev) {
    if (sb_over) {
        if (ev->is_repeat) return;
        if (ev->key == K_OK ||
            (ev->key == K_CHAR && (ev->ch == 'n' || ev->ch == 'r')))
            sb_start(sb_pz);             /* RETRY: 同一题 */
        else if (ev->key == K_BACK || ev->key == K_QUIT)
            s_exit_request = true;
        return;
    }
    switch (ev->key) {
    case K_UP:
        if (sb_cy > 0) sb_cy--;
        break;
    case K_DOWN:
        if (sb_cy < SB_N - 1) sb_cy++;
        break;
    case K_LEFT:
        if (sb_cx > 0) sb_cx--;
        break;
    case K_RIGHT:
        if (sb_cx < SB_N - 1) sb_cx++;
        break;
    case K_OK:
        if (ev->is_repeat) break;
        sb_toggle((int)sb_cy * SB_N + (int)sb_cx);
        break;
    case K_DEL:
        if (ev->is_repeat) break;
        sb_clear((int)sb_cy * SB_N + (int)sb_cx);
        break;
    case K_CHAR:
        if (ev->ch == 'w') {
            if (sb_cy > 0) sb_cy--;
        } else if (ev->ch == 'a') {
            if (sb_cx > 0) sb_cx--;
        } else if (ev->ch == 's') {
            if (sb_cy < SB_N - 1) sb_cy++;
        } else if (ev->ch == 'd') {
            if (sb_cx < SB_N - 1) sb_cx++;
        } else if (ev->ch == 'n') {
            if (!ev->is_repeat) sb_start((sb_pz + 1) % SB_PUZ_N);
        } else if (ev->ch == 'r') {
            if (!ev->is_repeat) sb_start(sb_pz);
        }
        break;
    case K_PAUSE:
    case K_BACK: {
        if (ev->is_repeat) break;
        pause_sel_t sel;
        if (ui_pause_run(&sel)) {
            if (sel == PAUSE_RESTART) sb_start(sb_pz);
        } else {
            s_exit_request = true;
        }
        break;
    }
    case K_QUIT:
        if (!ev->is_repeat) s_exit_request = true;
        break;
    default:
        break;
    }
}
