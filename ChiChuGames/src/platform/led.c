/* 指示灯 — 4 个 GPIO LED + 非阻塞效果引擎
 * 游戏调用 led_fx_set 设置模式, main 循环每轮 led_fx_tick 推进;
 * WIN/LOSE 等一次性模式到点自动熄灭; 相位变化才写 sysfs
 * host 下写虚拟亮度数组供测试断言 */
#include "led.h"
#include "time.h"
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>

#define LED_COUNT 4

#ifdef CHICHU_HOST
static int s_vled[LED_COUNT];
/* 测试钩子: 虚拟亮度 0/1 */
int led_get_virtual(int index) {
    if (index < 0 || index > 3) return 0;
    return s_vled[index];
}
#endif

static void led_write(int index, int on) {
    if (index < 0 || index > 3) return;
#ifdef CHICHU_HOST
    s_vled[index] = on;
#else
    char path[40];
    int n = snprintf(path, sizeof(path),
                     "/sys/class/leds/led%d/brightness", index + 2);
    if (n <= 0) return;
    int fd = open(path, O_WRONLY);
    if (fd < 0) return;
    ssize_t w = write(fd, on ? "255" : "0", 3);   /* 写完整 "255"/"0" */
    (void)w;
    close(fd);
#endif
}

static void led_all(int on) {
    for (int i = 0; i < LED_COUNT; i++) led_write(i, on);
}

void led_set(int index, bool on) { led_write(index, on ? 1 : 0); }

/* ---- 效果引擎 ---- */
typedef struct {
    uint16_t period_ms;   /* 每相时长 (0=静态) */
    uint8_t phases;       /* 总相数 (0=无限循环) */
    bool auto_off;        /* 相数走完自动熄灭 */
} fx_spec_t;

static const fx_spec_t FX_SPEC[LED_FX_COUNT] = {
    [LED_FX_OFF]   = { 0,   0, false },
    [LED_FX_ALL]   = { 0,   0, false },
    [LED_FX_PULSE] = { 300, 0, false },   /* 呼吸: 全亮/全灭交替 */
    [LED_FX_SWEEP] = { 150, 0, false },   /* 单灯轮转 */
    [LED_FX_WIN]   = { 100, 8, true  },   /* 双灯对扫 4 步 ×2 轮 */
    [LED_FX_LOSE]  = { 200, 6, true  },   /* 全闪 3 次 */
};

static led_fx_t s_fx = LED_FX_OFF;
static uint64_t s_start = 0;   /* 效果起始时间 */
static int s_phase = -1;       /* 当前相(-1 强制首写) */

static void fx_apply(int phase) {
    switch (s_fx) {
    case LED_FX_PULSE:
        led_all(!(phase & 1));   /* 偶相亮(起始点亮) */
        break;
    case LED_FX_SWEEP: {
        int on = phase % LED_COUNT;
        for (int i = 0; i < LED_COUNT; i++) led_write(i, i == on);
        break;
    }
    case LED_FX_WIN: {         /* 灯 i 与 3-i 对扫 */
        int p = phase % 4;
        led_all(0);
        led_write(p, 1);
        led_write(3 - p, 1);
        break;
    }
    case LED_FX_LOSE:
        led_all(!(phase & 1));
        break;
    default:
        break;
    }
}

void led_fx_set(led_fx_t fx) {
    if (fx >= LED_FX_COUNT) fx = LED_FX_OFF;
    s_fx = fx;
    s_start = now_ms();
    s_phase = -1;
    if (fx == LED_FX_OFF || fx == LED_FX_ALL) {   /* 静态模式立即生效 */
        led_all(fx == LED_FX_ALL);
        s_phase = 0;
    }
}

void led_fx_tick(uint64_t now) {
    const fx_spec_t *sp = &FX_SPEC[s_fx];
    if (sp->period_ms == 0) return;              /* OFF/ALL 静态 */
    uint64_t el = now - s_start;
    int phase = (int)(el / sp->period_ms);
    if (sp->auto_off && phase >= sp->phases) {   /* 到点自动熄灭 */
        if (s_phase != 0x7fffffff) { led_all(0); s_phase = 0x7fffffff; }
        return;
    }
    if (phase != s_phase) { s_phase = phase; fx_apply(phase); }
}
