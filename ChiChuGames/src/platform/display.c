#include "display.h"
#include "time.h"
#include "input.h"
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>

#define EPAPER_DEV "/dev/epaper_lcd"
#define EPAPER_REFRESH "/sys/devices/platform/e0266a128/epaper/refresh"
#define EPAPER_REFRESH_MAX "/sys/devices/platform/e0266a128/epaper/refresh_max"
#define EPAPER_FAST_ONLY "/sys/devices/platform/e0266a128/epaper/fast_refresh_only"

uint8_t g_fb[CCG_FRAME_BYTES];

static uint8_t s_last[CCG_FRAME_BYTES];
static bool s_has_last = false;

#ifndef CHICHU_HOST
static int s_fd = -1;
static int s_refresh_fd = -1;
#endif

static uint64_t s_last_full_ms = 0;
static uint32_t s_full_gap_ms = CCG_FULL_GAP_MIN_MS;
static bool s_pending_full = false;

static bool s_auto_full_enabled = false;   /* 实测快刷干净, 默认关闭防残影插刷 */

static uint64_t s_last_write_us = 0;   /* 最近一次写帧耗时(面板忙等待) */
static uint32_t s_max_write_us = 0;
static uint64_t s_sum_write_us = 0;
static uint32_t s_write_count = 0;

static uint32_t s_fast_writes = 0;
static uint32_t s_full_refreshes = 0;
static uint32_t s_skips = 0;
static uint32_t s_refresh_errors = 0;
static uint32_t s_fast_since_full = 0;

#ifndef CHICHU_HOST
static int read_refresh_max(void) {
    char buf[16];
    int fd = open(EPAPER_REFRESH_MAX, O_RDONLY);
    if (fd < 0) return CCG_REFRESH_MAX_DEFAULT;
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return CCG_REFRESH_MAX_DEFAULT;
    buf[n] = 0;
    int v = 0;
    for (ssize_t i = 0; i < n && buf[i] >= '0' && buf[i] <= '9'; i++)
        v = v * 10 + (buf[i] - '0');
    if (v < 10) return CCG_REFRESH_MAX_DEFAULT;  /* 异常值兜底 */
    return v;
}
#endif

int disp_init(void) {
#ifdef CHICHU_HOST
    memset(s_last, 0, sizeof(s_last));
    return 0;
#else
    /* 关闭驱动自动全刷: 快刷达到 refresh_max(30) 次后驱动自行全刷,
     * 游戏过程中表现为随机强刷闪烁+跳帧; 场景切换由我们显式全刷防残影 */
    int ffr = open(EPAPER_FAST_ONLY, O_WRONLY);
    if (ffr >= 0) { ssize_t w4 = write(ffr, "1", 1); (void)w4; close(ffr); }

    s_fd = open(EPAPER_DEV, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (s_fd < 0) return -1;
    s_refresh_fd = open(EPAPER_REFRESH, O_WRONLY | O_CLOEXEC);
    if (s_refresh_fd < 0) { close(s_fd); s_fd = -1; return -1; }
    memset(s_last, 0, sizeof(s_last));
    int max = read_refresh_max();
    uint32_t gap = 60000u / (uint32_t)max;  /* 上限换算硬间隔 */
    if (gap < CCG_FULL_GAP_MIN_MS) gap = CCG_FULL_GAP_MIN_MS;
    if (gap > 10000u) gap = 10000u;
    s_full_gap_ms = gap;
    return 0;
#endif
}

void disp_cleanup(void) {
#ifndef CHICHU_HOST
    if (s_fd >= 0) { close(s_fd); s_fd = -1; }
    if (s_refresh_fd >= 0) { close(s_refresh_fd); s_refresh_fd = -1; }
#endif
}

/* 写帧 + 可选全刷; 返回是否成功写屏 */
static bool write_frame(bool full) {
#ifdef CHICHU_HOST
    (void)full;
    return false;  /* host 模式不写屏 */
#else
    if (s_fd < 0) return false;
    struct timespec tw0, tw1;
    clock_gettime(CLOCK_MONOTONIC, &tw0);
    ssize_t n = write(s_fd, g_fb, CCG_FRAME_BYTES);
    if (n != (ssize_t)CCG_FRAME_BYTES) {
        n = write(s_fd, g_fb, CCG_FRAME_BYTES);  /* 重试一次 */
        if (n != (ssize_t)CCG_FRAME_BYTES) return false;
    }
    clock_gettime(CLOCK_MONOTONIC, &tw1);
    s_last_write_us = (uint64_t)(tw1.tv_sec - tw0.tv_sec) * 1000000u +
                      (uint64_t)(tw1.tv_nsec - tw0.tv_nsec) / 1000u;
    if (s_last_write_us > s_max_write_us) s_max_write_us = s_last_write_us;
    s_sum_write_us += s_last_write_us;
    s_write_count++;
    if (full && s_refresh_fd >= 0) {
        if (write(s_refresh_fd, "1", 1) != 1) {
            s_refresh_errors++;
            return true;  /* 帧已写, 降级快刷 */
        }
    }
    return true;
#endif
}

static void drain_input_twice(void) {
    input_drain();
    input_drain();
}

bool disp_fast(void) {
    if (!s_has_last || memcmp(g_fb, s_last, CCG_FRAME_BYTES) != 0) {
        memcpy(s_last, g_fb, CCG_FRAME_BYTES);
        s_has_last = true;
        if (write_frame(false)) s_fast_writes++;
        s_fast_since_full++;
        return true;
    }
    s_skips++;
    return false;
}

/* fast_refresh_only=1 会屏蔽全刷(含手动 refresh 写), 全刷前临时关闭 */
static void set_fast_only(bool on) {
#ifdef CHICHU_HOST
    (void)on;
#else
    int f = open(EPAPER_FAST_ONLY, O_WRONLY);
    if (f >= 0) {
        ssize_t w = write(f, on ? "1" : "0", 1);
        (void)w;
        close(f);
    }
#endif
}

static void do_full(void) {
    drain_input_twice();
    set_fast_only(false);            /* 允许全刷波形 */
#ifdef CCG_TIMELINE
    extern void tm_mark(const char *);
    tm_mark("FULLstart");
#endif
    bool ok = write_frame(true);
#ifdef CCG_TIMELINE
    tm_mark("FULLend");
#endif
    set_fast_only(true);             /* 恢复: 抑制驱动自动全刷 */
    if (ok) {
        s_full_refreshes++;
        s_fast_since_full = 0;
    }
    s_last_full_ms = now_ms();
    s_pending_full = false;
    drain_input_twice();
}

void disp_full(void) {
    uint64_t now = now_ms();
    if (now - s_last_full_ms < s_full_gap_ms) {
        s_pending_full = true;   /* 预算内延后 */
        return;
    }
    do_full();
}

void disp_force_full(void) {
    drain_input_twice();
    do_full();
}

void disp_blank(void) {
    memset(g_fb, 0xff, CCG_FRAME_BYTES);
    disp_force_full();
}

void disp_drain_pending(void) {
    if (!s_pending_full) return;
    uint64_t now = now_ms();
    uint64_t due = s_last_full_ms + s_full_gap_ms;
    if (now < due) {
        sleep_until(due);       /* 等到预算允许 */
    }
    do_full();
}

void disp_maybe_auto_full(void) {
    if (!s_auto_full_enabled) return;
    if (s_fast_since_full >= CCG_AUTO_FULL_FAST_MIN) {
        uint64_t now = now_ms();
        if (now - s_last_full_ms >= CCG_AUTO_FULL_GAP_MS) {
            do_full();
        }
    }
}

void disp_set_auto_full(bool on) { s_auto_full_enabled = on; }

void disp_suspend(void) { s_pending_full = false; }

void disp_resume(void) {
    s_has_last = false;         /* 强制下次全刷 */
    s_last_full_ms = 0;         /* 重置预算(休眠时间不算) */
    disp_force_full();
}

bool disp_refresh_available(void) {
    return (now_ms() - s_last_full_ms) >= s_full_gap_ms;
}

bool disp_full_pending(void) { return s_pending_full; }

void disp_get_stats(disp_stats_t *out) {
    out->fast_writes = s_fast_writes;
    out->full_refreshes = s_full_refreshes;
    out->skips = s_skips;
    out->refresh_errors = s_refresh_errors;
    uint64_t now = now_ms();
    out->last_full_age_ms = (uint32_t)(now - s_last_full_ms);
    out->fast_since_full = s_fast_since_full;
}

uint32_t disp_last_write_us(void) { return (uint32_t)s_last_write_us; }
uint32_t disp_max_write_us(void) { return s_max_write_us; }
uint32_t disp_avg_write_us(void) {
    return s_write_count ? (uint32_t)(s_sum_write_us / s_write_count) : 0;
}
