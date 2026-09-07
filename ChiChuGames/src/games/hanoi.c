/* 汉诺塔 — 3 柱 6 盘; 左右选柱 OK 拾取/放下; 步数 vs 2^n-1 */
#include "game.h"
#include "../config.h"
#include "../gfx/canvas.h"
#include "../gfx/font.h"
#include "../platform/display.h"
#include "../platform/input.h"
#include "../platform/audio.h"
#include "../platform/led.h"
#include "../rng.h"
#include "../platform/time.h"

#define HN_DISKS 6
#define HN_PEG_X 106
#define HN_OX ((CCG_W - 2 * HN_PEG_X) / 2)   /* 柱心 x: 42, 148, 254 */
#define HN_BASE_Y 140
#define HN_DISK_H 8
#define HN_DISK_W0 20        /* 最小盘宽 */
#define HN_DISK_STEP 10      /* 每盘 +10 -> 最大 70(半宽 35, 不超边) */

void hanoi_render(void);

static uint8_t h_peg[HN_DISKS];      /* 每盘所在柱 0-2 */
static uint8_t h_count[3];
static int8_t h_sel;                 /* 选中的柱 -1=无 */
static uint8_t h_cur;                /* 光标柱 */
static uint32_t h_steps;
static bool h_over, h_over_full;
static char h_msg[24];
static uint32_t h_msg_until;

static void rebuild_counts(void) {
    for (int p = 0; p < 3; p++) h_count[p] = 0;
    for (int d = 0; d < HN_DISKS; d++) h_count[h_peg[d]]++;
}

#define HN_TARGET 1            /* 目标柱 = 中间 */

static rng_t h_rng;

/* 数学保证: 3 柱汉诺塔状态图 H3^n 连通(归纳证明),
 * 任何配置可达"全部在目标柱"→ 随机关卡必然有解, 无需搜索校验 */

/* 随机关卡: 每盘随机分配柱(各柱堆叠天然合法), 目标柱不全满 */
static void hanoi_reset(void) {
    for (int attempt = 0; attempt < 20; attempt++) {
        for (int d = 0; d < HN_DISKS; d++)
            h_peg[d] = (uint8_t)rng_range(&h_rng, 3);
        int on_target = 0;
        for (int d = 0; d < HN_DISKS; d++)
            if (h_peg[d] == HN_TARGET) on_target++;
        if (on_target < HN_DISKS) break;   /* 连通性定理保证有解 */
    }
    rebuild_counts();
    h_sel = -1;
    h_cur = 0;
    h_steps = 0;
    h_over = false;
    h_over_full = false;
    h_msg[0] = 0;
    h_msg_until = 0;
}

void hanoi_enter(void) {
    rng_seed(&h_rng, now_ms() ^ 0x10AD);
    hanoi_reset();
    hanoi_render();
    disp_full();
}

static void hanoi_disk(int cx, int d, int y, bool held) {
    int w = HN_DISK_W0 + d * HN_DISK_STEP;
    if (held) {
        /* 手持盘: 反白 + 外框 */
        fb_fill_rect(cx - w / 2, y, w, HN_DISK_H, true);
        fb_stroke_rect(cx - w / 2 - 1, y - 1, w + 2, HN_DISK_H + 2, false);
    } else {
        fb_fill_rect(cx - w / 2, y, w, HN_DISK_H, true);
    }
}

void hanoi_render(void) {
    fb_clear(false);
    /* 基座 + 三柱(3px 粗, 高度固定) */
    int stack_h = HN_DISKS * (HN_DISK_H + 1);
    int peg_top = HN_BASE_Y - 3 - stack_h;
    /* 目标柱(中间)加高 */
    int t_top = peg_top - 18;
    fb_fill_rect(HN_OX - 12, HN_BASE_Y, 2 * HN_PEG_X + 24, 3, true);
    int held_d = -1;
    if (h_sel >= 0) {
        for (int d = 0; d < HN_DISKS; d++)
            if (h_peg[d] == h_sel) { held_d = d; break; }
    }
    for (int p = 0; p < 3; p++) {
        int px = HN_OX + p * HN_PEG_X;
        int pt = (p == HN_TARGET) ? t_top : peg_top;
        if (p == (int)h_cur) {
            /* 光标柱: 柱体反白醒目 */
            fb_fill_rect(px - 1, pt - 2, 3, HN_BASE_Y - pt + 2, false);
        }
        fb_vline(px, pt - 2, HN_BASE_Y - pt + 2, true);
        fb_vline(px + 1, pt - 2, HN_BASE_Y - pt + 2, true);
        /* 盘片: 底大顶小(最小盘号=顶部) */
        int idx = 0;
        for (int d = 0; d < HN_DISKS; d++) {
            if (h_peg[d] != p) continue;
            int y = HN_BASE_Y - 1 - (h_count[p] - idx) * (HN_DISK_H + 1);
            hanoi_disk(px, d, y, false);
            idx++;
        }
        /* 目标柱下方标星(去掉编号) */
        if (p == HN_TARGET) {
            fb_symbol(px - 2, HN_BASE_Y + 6, CG_STAR, true);
        }
        /* 光标柱: 空心圆标记(数学圆环, 3px 粗, 常驻; 拾取后跟随光标=落点) */
        if (p == (int)h_cur) {
            int cy = peg_top - 16 + 5;   /* 圆心 */
            for (int dy = -5; dy <= 5; dy++)
                for (int dx = -5; dx <= 5; dx++) {
                    int r2 = dx * dx + dy * dy;
                    if (r2 <= 25 && r2 >= 4) fb_pixel(px + dx, cy + dy, true);
                }
        }
    }
    /* 手持盘画在最上层, 跟随光标柱(显示落点) */
    if (held_d >= 0 && h_sel >= 0) {
        int px = HN_OX + h_cur * HN_PEG_X;
        int stack_h2 = HN_DISKS * (HN_DISK_H + 1);
        hanoi_disk(px, held_d, HN_BASE_Y - 12 - stack_h2 - 22, true);  /* 圆环上方 */
    }
    /* HUD */
    fb_text(0, 0, "HANOI", true);
    char steps_buf[20];
    const char *pre = "STEP ";
    unsigned si = 0;
    while (pre[si]) { steps_buf[si] = pre[si]; si++; }
    uint32_t sv = h_steps;
    char srev[12];
    unsigned svi = 0;
    if (sv == 0) { srev[svi++] = '0'; }
    while (sv && svi < 10) { srev[svi++] = (char)('0' + sv % 10); sv /= 10; }
    while (svi > 0) steps_buf[si++] = srev[--svi];
    steps_buf[si] = 0;
    fb_text(CCG_W - 2 - text_width(steps_buf), 0, steps_buf, true);
    fb_hline(0, CCG_HUD_H - 1, CCG_W, true);
    if (h_msg[0]) {
        fb_text_center(CCG_H - 12, h_msg, true);
    }
    if (h_over) {
        fb_fill_rect(0, 0, CCG_W, CCG_HUD_H - 1, false);
        fb_text(2, 2, "SOLVED!", true);
        fb_text(CCG_W - 2 - text_width("OK/N:RETRY BACK:QUIT"), 2,
                "OK/N:RETRY BACK:QUIT", true);
        if (!h_over_full) { h_over_full = true; disp_force_full(); }
    }
}

void hanoi_on_key(const key_event_t *ev) {
    if (ev->is_repeat) return;
    if (h_over) {
        if (ev->key == K_OK || (ev->key == K_CHAR && ev->ch == 'n'))
            hanoi_enter();
        else if (ev->key == K_BACK || ev->key == K_QUIT)
            s_exit_request = true;
        return;
    }
    switch (ev->key) {
    case K_LEFT: if (h_cur > 0) h_cur--; break;
    case K_RIGHT: if (h_cur < 2) h_cur++; break;
    case K_CHAR:
        if (ev->ch == 'a' && h_cur > 0) h_cur--;
        else if (ev->ch == 'd' && h_cur < 2) h_cur++;
        else if (ev->ch == 'n') hanoi_enter();
        else if (ev->ch == 'r') hanoi_enter();
        break;
    case K_OK: {
        if (h_sel < 0) {
            /* 拾取: 该柱最上面的盘(最小盘号=顶部) */
            int top = -1;
            for (int d = 0; d < HN_DISKS; d++)
                if (h_peg[d] == h_cur) { top = d; break; }
            if (top >= 0) {
                h_sel = (int8_t)h_cur;
                audio_select();
            } else { h_msg_until = 0; }
        } else {
            if (h_cur == h_sel) {
                h_sel = -1;   /* 取消 */
            } else {
                /* 目标柱顶盘 / 源柱顶盘(最小盘号=顶部) */
                int dst_top = -1;
                for (int d = 0; d < HN_DISKS; d++)
                    if (h_peg[d] == h_cur) { dst_top = d; break; }
                int src_top = -1;
                for (int d = 0; d < HN_DISKS; d++)
                    if (h_peg[d] == h_sel) { src_top = d; break; }
                if (src_top >= 0 && (dst_top < 0 || dst_top > src_top)) {
                    h_peg[src_top] = h_cur;
                    h_steps++;
                    h_sel = -1;
                    rebuild_counts();
                    int all = 1;
                    for (int d = 0; d < HN_DISKS; d++)
                        if (h_peg[d] != HN_TARGET) all = 0;
                    if (all) {
                        h_over = true;
                        audio_win();
                        led_fx_set(LED_FX_WIN);
                    } else {
                        audio_move();
                    }
                } else {
                    h_sel = -1;
                    audio_error();
                }
            }
        }
        break;
    }
    case K_BACK: {
        pause_sel_t sel;
        if (ui_pause_run(&sel)) {
            if (sel == PAUSE_RESTART) hanoi_enter();
        } else {
            s_exit_request = true;
        }
        break;
    }
    case K_QUIT: s_exit_request = true; break;
    default: break;
    }
}

void hanoi_tick(uint64_t now) { (void)now; }
void hanoi_exit(void) {}
