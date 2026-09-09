/* SPDX-License-Identifier: GPL-3.0-only */
#include "ui.h"
#include "runtime.h"
#include "version.h"
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <unistd.h>

static UI ui;
static UiKey scripted(char c) {
    switch (c) {
    case 'w': return UI_UP;
    case 's': return UI_DOWN;
    case 'a': return UI_LEFT;
    case 'd': return UI_RIGHT;
    case 'o': return UI_OK;
    case 'b': return UI_BACK;
    default: return UI_NONE;
    }
}
static void wait_panel(uint64_t deadline) {
    uint64_t now;
    while ((now = rt_now()) < deadline) {
        int delay = (int)(deadline - now);
        if (poll(NULL, 0, delay) < 0 && errno != EINTR) break;
    }
}
static int dump_frame(const char *dir, unsigned n) {
    char path[512];
    if (snprintf(path, sizeof path, "%s/%04u.pbm", dir, n) >= (int)sizeof path) return -1;
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    fprintf(f, "P4\n296 152\n");
    for (int y = 0; y < 152; y++) {
        for (int x = 0; x < 296; x += 8) {
            unsigned char row = 0;
            for (int b = 0; b < 8; b++)
                if (g_fb[(y >> 3) * 296 + x + b] & (0x80u >> (y & 7))) row |= 0x80u >> b;
            if (fputc(row, f) == EOF) { fclose(f); return -1; }
        }
    }
    return fclose(f);
}
static void usage(const char *name) {
    fprintf(stderr, "Usage: %s [--save FILE] [--preview FILE.pbm] [--replay wsadob...] [--frames DIR] [--headless]\n"
                    "Replay supplies logical UI keys, NOT physical evdev input. --headless is a fast non-display test.\n", name);
}
int main(int argc, char **argv) {
    #ifdef RT_HOST
    const char *save = "inkwars.sav";
#else
    /* The publisher requires an ELF entry, so writable state must not depend
     * on launch.sh changing the working directory. rt_init creates this dir. */
    const char *save = "/usr/data/inkwars/inkwars.sav";
#endif
    const char *preview = "inkwars.pbm", *replay = NULL, *frames = NULL;
    bool headless = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--version")) { puts("Ink Wars " IW_VERSION); return 0; }
        else if (!strcmp(argv[i], "--headless")) headless = true;
        else if (i + 1 < argc && !strcmp(argv[i], "--save")) save = argv[++i];
        else if (i + 1 < argc && !strcmp(argv[i], "--preview")) preview = argv[++i];
        else if (i + 1 < argc && !strcmp(argv[i], "--replay")) replay = argv[++i];
        else if (i + 1 < argc && !strcmp(argv[i], "--frames")) frames = argv[++i];
        else { usage(argv[0]); return 2; }
    }
    if (headless && !replay) { usage(argv[0]); return 2; }
#ifndef RT_HOST
    if (!headless) {
        /* Never print runtime diagnostics into the launcher's PTY. */
        if (mkdir("/usr/data/inkwars", 0700) != 0 && errno != EEXIST) return 1;
        if (!freopen("/usr/data/inkwars/last-run.log", "w", stderr)) return 1;
    }
#endif
    if (frames && mkdir(frames, 0700) < 0 && errno != EEXIST) { perror(frames); return 1; }
    if (!headless && rt_init(preview) < 0) { perror("Ink Wars runtime"); return 1; }
    ui_init(&ui, save);
    uint64_t started = rt_now(), next = 0, full_retry = 0, settle = 0;
    unsigned frame = 0;
    size_t step = 0;
    bool signal_exit = false;
    int status = 0;
    for (;;) {
        uint64_t now = rt_now();
        if (ui.dirty && (headless || now >= next)) {
            ui_draw(&ui);
            int presented = headless ? 1 : rt_present();
            if (presented < 0) { perror("frame write"); status = 1; break; }
            /* Zero means either deduplication or a deferred write. Preserve
             * dirty state until the display deadline has passed in the latter case. */
            bool deferred = !headless && presented == 0 && rt_now() < rt_frame_deadline();
            next = rt_now() + RT_TICK_MS;
            if (!deferred) {
                if (frames && dump_frame(frames, frame) < 0) { perror("frame dump"); status = 1; break; }
                frame++;
                ui.dirty = false;
                if (ui.full) {
                    if (!headless) rt_request_full();
                    full_retry = rt_now() + RT_FULL_GAP_MS + RT_TICK_MS;
                    settle = next;
                    ui.full = false;
                }
            }
        }
        if (ui.quit) break;
        if (replay && replay[step] == '\0' && !ui.dirty) break;
        if (replay && !ui.dirty) {
            if (!headless) {
                wait_panel(next);
                if (rt_key(0) == RT_QUIT) { signal_exit = true; break; }
                rt_idle(rt_now());
                wait_panel(rt_frame_deadline());
            }
            if (replay[step] == '.') ui_ai(&ui);
            else ui_key(&ui, scripted(replay[step]));
            step++;
            continue;
        }
        now = rt_now();
        if (!ui.dirty && now >= next && ui_ai_pending(&ui)) { ui_ai(&ui); continue; }
        if (!ui.dirty && settle && now >= settle) {
            rt_idle(now);
            if (next < rt_frame_deadline()) next = rt_frame_deadline();
            settle = 0;
        }
        if (!ui.dirty && full_retry && now >= full_retry) {
            rt_idle(rt_now());
            if (next < rt_frame_deadline()) next = rt_frame_deadline();
            full_retry = 0;
        }
        uint64_t deadline = UINT64_MAX;
        if (ui.dirty || ui_ai_pending(&ui)) deadline = next;
        if (!ui.dirty && settle && settle < deadline) deadline = settle;
        if (!ui.dirty && full_retry && full_retry < deadline) deadline = full_retry;
        rt_event ev = rt_wait(deadline);
        if (ev == RT_QUIT) { signal_exit = true; break; }
        if (ev == RT_REDRAW) { ui.dirty = true; continue; }
        if (ev >= RT_UP && ev <= RT_BACK) {
            /* At most one logical input per visible tick. Subsequent events
             * stay in evdev instead of discarding confirmation presses. */
            wait_panel(next);
            ui_key(&ui, (UiKey)ev);
        }
    }
    if (signal_exit && ui.active) {
        char error[128];
        if (!game_save(&ui.game, save, error, sizeof error)) {
            fprintf(stderr, "Emergency save failed: %s\n", error);
            status = 1;
        }
    }
    if (!headless) {
        wait_panel(rt_frame_deadline());
        rt_shutdown();
    }
    fprintf(stderr, "Ink Wars: frames=%u replay_keys=%zu turn=%u side=%u screen=%d\n", frame, step, ui.game.turn, ui.game.side, ui.screen);
    struct rusage resource;
    if (getrusage(RUSAGE_SELF, &resource) == 0)
        fprintf(stderr, "Ink Wars: elapsed_ms=%llu maxrss_kib=%ld cpu_user_us=%lld cpu_system_us=%lld state_bytes=%zu\n",
                (unsigned long long)(rt_now() - started), resource.ru_maxrss,
                (long long)resource.ru_utime.tv_sec * 1000000 + resource.ru_utime.tv_usec,
                (long long)resource.ru_stime.tv_sec * 1000000 + resource.ru_stime.tv_usec, sizeof ui);
    return status;
}
