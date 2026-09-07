/* ChiChuGames 主入口 — 单线程 poll 事件循环
 * 生命周期: 信号置 flag → 菜单 → 说明页 → 游戏 → 退出白屏 */
#include "config.h"
#include "app_runtime.h"
#include "games/game.h"
#include "platform/display.h"
#include "platform/input.h"
#include "platform/time.h"
#include "platform/led.h"
#include "platform/audio.h"
#include "ui/help.h"
#include "ui/menu.h"
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>

bool s_exit_request = false;

static uint32_t s_active_interval = 0;   /* 当前 tick 间隔(游戏可调) */

static void dbg_log(const char *msg) {
    int f = open("/dev/shm/ccg.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (f >= 0) { ssize_t w = write(f, msg, strlen(msg)); (void)w; close(f); }
}

#ifdef CCG_TIMELINE
/* 时间线探针: KEY 事件 → AUD 音效 → FULL 全刷, 串行日志, 量化端到端延迟 */
void tm_mark(const char *tag) {
    static int f = -1;
    if (f < 0) f = open("/dev/shm/timeline.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (f < 0) return;
    char b[64];
    int n = snprintf(b, sizeof(b), "%s %llu\n", tag, (unsigned long long)now_ms());
    (void)write(f, b, (size_t)n);
}
#endif

void game_set_tick_interval(uint32_t ms) { s_active_interval = ms; }

/* ---- tick 调度(漏桶): SIGSTOP 醒来跳过积压 ---- */
static uint64_t tick_advance(uint64_t next, uint64_t now, uint32_t interval) {
    if (now - next > 500) return now + interval;   /* 跳过大积压 */
    while (next <= now) next += interval;
    return next;
}

static void run_game(game_id_t id) {
    const game_desc_t *g = &g_games[id];
    s_active_interval = g->tick_interval_ms ? g->tick_interval_ms : CCG_INPUT_POLL_MS;
    uint64_t now = now_ms();
    uint64_t next_tick = now + s_active_interval;

    s_exit_request = false;
    input_set_repeat(g->repeat_init_ms ? g->repeat_init_ms : CCG_REPEAT_INIT_MS,
                     g->repeat_ms ? g->repeat_ms : CCG_REPEAT_MS);
    g->enter();

    while (!app_runtime_checkpoint() && !s_exit_request) {
        now = now_ms();
        led_fx_tick(now);           /* 驱动灯光效果 */
        int timeout = (int)((next_tick > now) ? (next_tick - now) : 0);
        if (timeout > (int)CCG_IDLE_POLL_MS) timeout = CCG_IDLE_POLL_MS;
        input_poll(timeout);

        bool changed = false;
        key_event_t ev;
        while (input_get(&ev)) {
            g->on_key(&ev);
            changed = true;
        }

        now = now_ms();
        if (now >= next_tick && g->tick_interval_ms) {
            g->tick(now);
            next_tick = tick_advance(next_tick, now, s_active_interval);
            changed = true;
        }

        if (changed) {
            g->render();
            disp_fast();
        }
        disp_maybe_auto_full();
        if (disp_full_pending()) disp_drain_pending();

    }
    g->exit();
}

int main(int argc, char **argv) {
    /* Metadata queries must exit before signals, display, audio or input setup. */
    if (argc == 2 && strcmp(argv[1], "--version") == 0) {
        printf("ChiChuGames %s\n", CCG_VERSION);
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "--help") == 0) {
        puts("Usage: chichugames [--version|--help|--selftest-audio]");
        return 0;
    }
    if (argc > 1 && (argc != 2 || strcmp(argv[1], "--selftest-audio") != 0)) {
        fputs("Unsupported arguments; use --help\n", stderr);
        return 2;
    }
    if (app_runtime_install_signals() != 0) return 1;

    /* --selftest-audio: 顺序播放 7 音效(真机盲测回放链路); 之后照常进入 */
    if (argc > 1 && strcmp(argv[1], "--selftest-audio") == 0) {
        audio_tick(); audio_move(); audio_select(); audio_clear();
        audio_error(); audio_win(); audio_lose();
        audio_cleanup();           /* 播完即退出, 避免管道残留 */
        return 0;
    }

    dbg_log("STEP disp_init\n");
    led_set(0, true); led_set(1, true); led_set(2, true); led_set(3, true);
    sleep_until(now_ms() + 300);
    led_set(0, false); led_set(1, false); led_set(2, false); led_set(3, false);
    if (disp_init() != 0) return 1;
    dbg_log("STEP input_init\n");
    if (input_init() != 0) { disp_cleanup(); return 1; }
    dbg_log("STEP menu\n");

    for (;;) {
        game_id_t id = menu_run();
        if (id == (game_id_t)GAME_COUNT || app_runtime_checkpoint()) {
            dbg_log("EXIT menu\n");
            break;
        }
        if (help_run(&g_games[id])) {
            run_game(id);
        }
    }

    dbg_log("EXIT disp_blank\n");
    disp_blank();          /* 退出白屏 */
    input_cleanup();
    audio_cleanup();
    disp_cleanup();
    dbg_log("EXIT done\n");
    return 0;
}
