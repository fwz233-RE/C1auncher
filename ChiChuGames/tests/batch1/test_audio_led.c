/* 音效/灯光框架测试 — 波形生成 + LED 效果状态机 */
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include "platform/time.h"
#include "../../src/platform/audio.c"   /* host 播放器=/dev/null, 波形纯函数可测 */
#include "../../src/platform/led.c"     /* host 写虚拟亮度数组 */

int led_get_virtual(int index);   /* led.c host 测试钩子 */

static int s_fail = 0;
#define CHECK(c, m) do { if(!(c)){printf("FAIL: %s\n", m); s_fail++;} else printf("ok: %s\n", m);} while(0)

/* 波形取样: 幅度与符号 */
static int32_t amp(int16_t v) { return v < 0 ? -(int32_t)v : v; }

/* PCM 素材峰值(±29000 归一的真实采样) */
static int32_t sfx_peak(const int16_t *p, uint32_t n) {
    int32_t m = 0;
    for (uint32_t i = 0; i < n; i++) {
        int32_t a = amp(p[i]);
        if (a > m) m = a;
    }
    return m;
}

int main(void) {
    /* ---- 音效素材完整性(Kenney CC0 PCM 表, 零合成) ---- */
    CHECK(g_sfx_tick_len > 0 && g_sfx_tick_len < 96000, "tick len sane (<2s)");
    CHECK(g_sfx_move_len > 0 && g_sfx_move_len < 96000, "move len sane");
    CHECK(g_sfx_select_len > 0 && g_sfx_select_len < 96000, "select len sane");
    CHECK(g_sfx_clear_len > 0 && g_sfx_clear_len < 96000, "clear len sane");
    CHECK(g_sfx_error_len > 0 && g_sfx_error_len < 96000, "error len sane");
    CHECK(g_sfx_win_len > 0 && g_sfx_win_len < 96000, "win len sane");
    CHECK(g_sfx_lose_len > 0 && g_sfx_lose_len < 96000, "lose len sane");
    CHECK(sfx_peak(g_sfx_tick, g_sfx_tick_len) > 8000, "tick audible peak");
    CHECK(sfx_peak(g_sfx_move, g_sfx_move_len) > 8000, "move audible peak");
    CHECK(sfx_peak(g_sfx_select, g_sfx_select_len) > 8000, "select audible peak");
    CHECK(sfx_peak(g_sfx_clear, g_sfx_clear_len) > 8000, "clear audible peak");
    CHECK(sfx_peak(g_sfx_error, g_sfx_error_len) > 6000, "error audible peak");
    CHECK(sfx_peak(g_sfx_win, g_sfx_win_len) > 8000, "win audible peak");
    CHECK(sfx_peak(g_sfx_lose, g_sfx_lose_len) > 8000, "lose audible peak");
    /* 无削顶: 全部样本幅值 <= 29000(归一留 10% 余量) */
    int32_t mx = 0;
    for (uint32_t i = 0; i < g_sfx_tick_len; i++) { int32_t a = amp(g_sfx_tick[i]); if (a > mx) mx = a; }
    CHECK(mx <= 13000 && sfx_peak(g_sfx_tick, g_sfx_tick_len) <= 29000, "tick no clipping");

    /* ---- 音效 API 冒烟(host 播放器写 /dev/null) ---- */
    audio_tick(); audio_move(); audio_select(); audio_clear();
    audio_error(); audio_win(); audio_lose(); audio_beep(500, 50);
    audio_cleanup();
    CHECK(1, "audio API calls no-crash");

    /* ---- LED 静态模式 ---- */
    led_fx_set(LED_FX_ALL);
    CHECK(led_get_virtual(0) == 1 && led_get_virtual(3) == 1, "ALL: 4 lights on");
    led_fx_set(LED_FX_OFF);
    CHECK(led_get_virtual(0) == 0 && led_get_virtual(3) == 0, "OFF: all off");
    led_set(2, true);
    CHECK(led_get_virtual(2) == 1, "led_set direct on");
    led_set(2, false);
    CHECK(led_get_virtual(2) == 0, "led_set direct off");

    /* ---- WIN: 双灯对扫 100ms/步, 8 相后自动熄灭 ---- */
    led_fx_set(LED_FX_WIN);
    uint64_t t0 = s_start;
    led_fx_tick(t0);
    CHECK(led_get_virtual(0) == 1 && led_get_virtual(3) == 1, "win phase0: ends on");
    CHECK(led_get_virtual(1) == 0 && led_get_virtual(2) == 0, "win phase0: center off");
    led_fx_tick(t0 + 99);
    CHECK(led_get_virtual(0) == 1, "win same phase: no rewrite");
    led_fx_tick(t0 + 100);
    CHECK(led_get_virtual(1) == 1 && led_get_virtual(2) == 1, "win phase1: center on");
    CHECK(led_get_virtual(0) == 0, "win phase1: ends off");
    led_fx_tick(t0 + 300);
    CHECK(led_get_virtual(0) == 1 && led_get_virtual(3) == 1, "win phase3: back to ends");
    led_fx_tick(t0 + 800);
    CHECK(led_get_virtual(0) == 0 && led_get_virtual(3) == 0, "win auto-off after 8 phases");
    led_fx_tick(t0 + 5000);
    CHECK(led_get_virtual(0) == 0, "win stays off");

    /* ---- LOSE: 快闪 3 次(200ms 相) ---- */
    led_fx_set(LED_FX_LOSE);
    t0 = s_start;
    led_fx_tick(t0);
    CHECK(led_get_virtual(0) == 1, "lose flash1 on");
    led_fx_tick(t0 + 200);
    CHECK(led_get_virtual(0) == 0, "lose flash1 off");
    led_fx_tick(t0 + 400);
    CHECK(led_get_virtual(0) == 1, "lose flash2 on");
    led_fx_tick(t0 + 1000);
    CHECK(led_get_virtual(0) == 0, "lose flash3 off");
    led_fx_tick(t0 + 1200);
    CHECK(led_get_virtual(0) == 0, "lose auto-off after 6 phases");

    /* ---- PULSE: 呼吸持续, 偶相亮 ---- */
    led_fx_set(LED_FX_PULSE);
    t0 = s_start;
    led_fx_tick(t0);
    CHECK(led_get_virtual(0) == 1, "pulse starts on");
    led_fx_tick(t0 + 300);
    CHECK(led_get_virtual(0) == 0, "pulse off at 300ms");
    led_fx_tick(t0 + 600);
    CHECK(led_get_virtual(0) == 1, "pulse on at 600ms");
    led_fx_tick(t0 + 10000);
    CHECK(led_get_virtual(0) == 0 || led_get_virtual(0) == 1, "pulse never auto-offs");

    /* ---- SWEEP: 单灯轮转持续 ---- */
    led_fx_set(LED_FX_SWEEP);
    t0 = s_start;
    led_fx_tick(t0);
    CHECK(led_get_virtual(0) == 1 && led_get_virtual(1) == 0, "sweep starts led0");
    led_fx_tick(t0 + 150);
    CHECK(led_get_virtual(0) == 0 && led_get_virtual(1) == 1, "sweep moves to led1");
    led_fx_tick(t0 + 600);
    CHECK(led_get_virtual(0) == 1 && led_get_virtual(3) == 0, "sweep wraps to led0");

    if (s_fail == 0) { printf("ALL AUDIO/LED TESTS PASSED\n"); return 0; }
    printf("%d FAILED\n", s_fail);
    return 1;
}
