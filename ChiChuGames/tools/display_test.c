/* 显示诊断工具 — 用几何图案验证位序/偏移/极性(真机跑, 人眼看) */
void text_test(int mode2);
void ghost_test(void);
void fast_only_test(void);
void full_only_test(void);
void rate_sweep_test(void);
void toggle_order_test(int order);
void delayed_full_test(void);
#include <stdbool.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define W 296
#define H 152
#define BYTES (W * (H / 8))

static uint8_t fb[BYTES];
/* canvas.c 依赖的全局帧(5624B) — 与 fb 相同布局 */
uint8_t g_fb[BYTES];

static void setp(int x, int y, bool on) {
    if (x < 0 || y < 0 || x >= W || y >= H) return;
    size_t off = (size_t)(y / 8) * W + x;
    uint8_t mask = (uint8_t)(0x80u >> (y % 8));
    if (on) fb[off] |= mask; else fb[off] &= (uint8_t)~mask;
}

static void full_refresh(void) {
    int fd = open("/dev/epaper_lcd", O_WRONLY | O_NONBLOCK);
    if (fd < 0) { perror("open epaper"); exit(1); }
    if (write(fd, fb, BYTES) != BYTES) perror("write frame");
    close(fd);
    fd = open("/sys/devices/platform/e0266a128/epaper/refresh", O_WRONLY);
    if (fd < 0) { perror("open refresh"); exit(1); }
    if (write(fd, "1", 1) != 1) perror("write refresh");
    close(fd);
}

static void draw_big_a(int ox, int oy) {
    /* 9x7 放大 A: 每像素 4x4 方块 */
    static const char *a[7] = {
        "....#....", "....#....", "...###...", "..##.##..",
        ".#######.", ".#.....#.", "#.......#",
    };
    for (int r = 0; r < 7; r++)
        for (int c = 0; c < 9; c++)
            if (a[r][c] == '#')
                for (int dy = 0; dy < 4; dy++)
                    for (int dx = 0; dx < 4; dx++)
                        setp(ox + c * 4 + dx, oy + r * 4 + dy, true);
}

int main(int argc, char **argv) {
    int mode = argc > 1 ? atoi(argv[1]) : 0;
    memset(fb, 0, sizeof(fb));
    switch (mode) {
    case 0: /* 全黑 */
        memset(fb, 0xff, sizeof(fb));
        break;
    case 1: /* 上白下黑: 分界在 y=75/76 */
        for (int y = 76; y < H; y++)
            for (int x = 0; x < W; x++) setp(x, y, true);
        break;
    case 2: /* 4 条 1px 横线: y=0(顶) y=7 y=8(条带边界) y=151(底) */
        for (int x = 0; x < W; x++) { setp(x, 0, true); setp(x, 7, true); setp(x, 8, true); setp(x, 151, true); }
        break;
    case 3: /* 3 条 1px 竖线: x=0 x=147 x=295 */
        for (int y = 0; y < H; y++) { setp(0, y, true); setp(147, y, true); setp(295, y, true); }
        break;
    case 4: /* 8px 黑白交替横条 */
        for (int y = 0; y < H; y++) {
            bool on = (y / 8) % 2 == 0;
            for (int x = 0; x < W; x++) setp(x, y, on);
        }
        break;
    case 5: /* 每 8px 顶部一条 1px 横线 */
        for (int y = 0; y < H; y += 8)
            for (int x = 0; x < W; x++) setp(x, y, true);
        break;
    case 6: /* 放大 A 在左上角 (ox=4,oy=4) 和 (ox=150,oy=80) */
        draw_big_a(4, 4);
        draw_big_a(150, 80);
        break;
    case 7: /* 整屏棋盘格 4px */
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++)
                setp(x, y, ((x / 4) + (y / 4)) % 2 == 0);
        break;
    case 8:
    case 9:
        text_test(mode);
        return 0;
    case 10:
        ghost_test();
        return 0;
    case 11:
        fast_only_test();
        return 0;
    case 12:
        full_only_test();
        return 0;
    case 13:
        rate_sweep_test();
        return 0;
    case 14:
        toggle_order_test(14);
        return 0;
    case 15:
        toggle_order_test(15);
        return 0;
    case 16:
        delayed_full_test();
        return 0;
    default:
        draw_big_a(4, 4);
        break;
    }
    full_refresh();
    printf("displayed mode %d\n", mode);
    return 0;
}

/* mode 8+: 使用游戏真实文本渲染路径 */
extern void fb_text(int x, int y, const char *s, bool black);
extern void fb_text_scale2(int x, int y, const char *s, bool black);
extern void fb_text_inv(int x, int y, const char *s);
extern void fb_clear(bool black);
void text_test(int mode2) {
    fb_clear(false);
    if (mode2 == 9) {
        fb_text_scale2(20, 20, "CHICHU GAMES", true);
        fb_text(20, 60, "ABCDEFGHIJKLMNOPQRSTUVWXYZ", true);
        fb_text(20, 70, "abcdefghijklmnopqrstuvwxyz", true);
        fb_text(20, 80, "0123456789 !?@#$%^&*()", true);
    } else {
        fb_text(20, 30, "ABCDEFGHIJKLMNOPQRSTUVWXYZ", true);
        fb_text(20, 42, "0123456789 :;,.+-=/<>", true);
        fb_text_inv(20, 60, "INVERTED TEXT");
        fb_text(20, 90, "THE QUICK BROWN FOX JUMPS OVER", true);
        fb_text(20, 102, "THE LAZY DOG 1234567890", true);
    }
    memcpy(fb, g_fb, BYTES);
    full_refresh();
}

/* mode 12: 纯全刷测试 — 4 次全刷移动 */
void full_only_test(void) {
    for (int step = 0; step < 4; step++) {
        memset(fb, 0, sizeof(fb));
        int x = 8 + step * 30;
        for (int dy = 0; dy < 16; dy++)
            for (int dx = 0; dx < 16; dx++)
                setp(x + dx, 60 + dy, true);
        int fd = open("/dev/epaper_lcd", O_WRONLY | O_NONBLOCK);
        if (fd >= 0) { ssize_t w = write(fd, fb, BYTES); close(fd); (void)w; }
        int rfd = open("/sys/devices/platform/e0266a128/epaper/refresh", O_WRONLY);
        if (rfd >= 0) { ssize_t w2 = write(rfd, "1", 1); close(rfd); (void)w2; }
        usleep(800000);
    }
}

static void set_ffo(const char *v) {
    int f = open("/sys/devices/platform/e0266a128/epaper/fast_refresh_only", O_WRONLY);
    if (f >= 0) { ssize_t w = write(f, v, 1); close(f); (void)w; }
}

/* mode 16: 关 flag + 延时 300ms + 帧后全刷 */
void delayed_full_test(void) {
    for (int step = 0; step < 3; step++) {
        memset(fb, 0, sizeof(fb));
        int x = 8 + step * 40;
        for (int dy = 0; dy < 16; dy++)
            for (int dx = 0; dx < 16; dx++)
                setp(x + dx, 60 + dy, true);
        set_ffo("0");
        usleep(300000);   /* 等驱动采纳 flag */
        int fd = open("/dev/epaper_lcd", O_WRONLY | O_NONBLOCK);
        int rfd = open("/sys/devices/platform/e0266a128/epaper/refresh", O_WRONLY);
        if (fd >= 0) { ssize_t w = write(fd, fb, BYTES); close(fd); (void)w; }
        if (rfd >= 0) { ssize_t w = write(rfd, "1", 1); close(rfd); (void)w; }
        set_ffo("1");
        usleep(800000);
    }
}

/* mode 14: 帧后全刷(我们的当前顺序) 15: 帧前全刷 */
void toggle_order_test(int order) {
    for (int step = 0; step < 3; step++) {
        memset(fb, 0, sizeof(fb));
        int x = 8 + step * 40;
        for (int dy = 0; dy < 16; dy++)
            for (int dx = 0; dx < 16; dx++)
                setp(x + dx, 60 + dy, true);
        set_ffo("0");
        int fd = open("/dev/epaper_lcd", O_WRONLY | O_NONBLOCK);
        int rfd = open("/sys/devices/platform/e0266a128/epaper/refresh", O_WRONLY);
        if (order == 14) {  /* 帧后刷 */
            if (fd >= 0) { ssize_t w = write(fd, fb, BYTES); close(fd); (void)w; }
            if (rfd >= 0) { ssize_t w = write(rfd, "1", 1); close(rfd); (void)w; }
        } else {            /* 帧前刷 */
            if (rfd >= 0) { ssize_t w = write(rfd, "1", 1); close(rfd); (void)w; }
            if (fd >= 0) { ssize_t w = write(fd, fb, BYTES); close(fd); (void)w; }
        }
        set_ffo("1");
        usleep(800000);
    }
}

/* mode 13: 显示速率扫描 — 3 段速度各 5 帧, 找平滑速度 */
void rate_sweep_test(void) {
    static const int speeds[] = { 300, 600, 900 };
    for (int phase = 0; phase < 3; phase++) {
        for (int step = 0; step < 5; step++) {
            memset(fb, 0, sizeof(fb));
            int x = 8 + (step + phase * 5) * 12;
            for (int dy = 0; dy < 16; dy++)
                for (int dx = 0; dx < 16; dx++)
                    setp(x + dx, 60 + dy, true);
            int fd = open("/dev/epaper_lcd", O_WRONLY | O_NONBLOCK);
            if (fd >= 0) { ssize_t w = write(fd, fb, BYTES); close(fd); (void)w; }
            usleep(speeds[phase] * 1000);
        }
        usleep(1500000);  /* 段间停顿 */
    }
}

/* mode 11: 纯快刷测试 — 8 次快刷移动, 无全刷 */
void fast_only_test(void) {
    for (int step = 0; step < 8; step++) {
        memset(fb, 0, sizeof(fb));
        int x = 8 + step * 20;
        for (int dy = 0; dy < 16; dy++)
            for (int dx = 0; dx < 16; dx++)
                setp(x + dx, 60 + dy, true);
        int fd = open("/dev/epaper_lcd", O_WRONLY | O_NONBLOCK);
        if (fd >= 0) { ssize_t w = write(fd, fb, BYTES); close(fd); (void)w; }
        usleep(400000);
    }
}

/* mode 10: 快刷残影测试 — 方块依次移动, 观察旧位置是否留残影 */
void ghost_test(void) {
    for (int step = 0; step < 6; step++) {
        memset(fb, 0, sizeof(fb));
        int x = 8 + step * 24;
        for (int dy = 0; dy < 16; dy++)
            for (int dx = 0; dx < 16; dx++)
                setp(x + dx, 60 + dy, true);
        int fd = open("/dev/epaper_lcd", O_WRONLY | O_NONBLOCK);
        if (fd < 0) continue;
        ssize_t w = write(fd, fb, BYTES);
        close(fd);
        (void)w;
        if (step % 2 == 0) {  /* 偶数步: 全刷 */
            int rfd = open("/sys/devices/platform/e0266a128/epaper/refresh", O_WRONLY);
            if (rfd >= 0) { ssize_t w2 = write(rfd, "1", 1); close(rfd); (void)w2; }
        }
        usleep(500000);
    }
    /* 最后: 全屏白(fast), 看是否清干净 */
    memset(fb, 0, sizeof(fb));
    int fd = open("/dev/epaper_lcd", O_WRONLY | O_NONBLOCK);
    if (fd >= 0) { ssize_t w3 = write(fd, fb, BYTES); close(fd); (void)w3; }
}
