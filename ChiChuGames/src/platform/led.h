/* 指示灯 — /sys/class/leds/led2-5 (4 个 GPIO LED)
 * 游戏侧用 led_fx_set 设置效果模式(main 循环驱动), 或 led_set 直接点亮 */
#ifndef CCG_LED_H
#define CCG_LED_H

#include <stdbool.h>
#include <stdint.h>

void led_set(int index, bool on);          /* index 0-3 = led2-led5 */

/* ---- 非阻塞效果引擎 ---- */
typedef enum {
    LED_FX_OFF = 0,    /* 全灭 */
    LED_FX_ALL,        /* 全亮持续 */
    LED_FX_PULSE,      /* 呼吸: 全亮/全灭 300ms 交替(持续) */
    LED_FX_SWEEP,      /* 单灯轮转 150ms/灯(持续) */
    LED_FX_WIN,        /* 双灯对扫 ×2 轮后自动熄灭 */
    LED_FX_LOSE,       /* 全闪 3 次后自动熄灭 */
    LED_FX_COUNT
} led_fx_t;

void led_fx_set(led_fx_t fx);
void led_fx_tick(uint64_t now_ms);   /* main 循环每轮调用 */

#endif
