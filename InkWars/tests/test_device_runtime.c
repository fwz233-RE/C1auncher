/* SPDX-License-Identifier: GPL-3.0-only */
/* Actual device branch, regular-file panel surrogate, FIFO Linux input events.
 * This reproduces launcher ownership and startup timing, not physical waveforms. */
#undef RT_HOST
#define ROOT "/tmp/inkwars-device-runtime"
#define EPAPER ROOT "/"
#define STATE_DIR ROOT
#define BOOT_ID_PATH ROOT "/boot-id"
#define SCREEN_DEVICE ROOT "/screen"
#define INPUT_MATRIX ROOT "/matrix"
#define INPUT_GPIO ROOT "/gpio"
#include "../src/runtime.c"
#include "platform/app_lease.h"
#include <assert.h>

static void file(const char *name, const char *data) {
    int fd = open(name, O_CREAT | O_TRUNC | O_WRONLY | O_NOFOLLOW, 0600);
    assert(fd >= 0);
    assert(write(fd, data, strlen(data)) == (ssize_t)strlen(data));
    assert(close(fd) == 0);
}
static void guard(bool available) {
    int fd = c1_app_lease_guard_acquire();
    assert((fd >= 0) == available);
    if (fd >= 0) close(fd);
}
int main(void) {
    assert(mkdir(ROOT, 0700) == 0);
    file(EPAPER "refresh_cnt", "30\n");
    file(EPAPER "fast_refresh_only", "0");
    file(EPAPER "refresh", "0");
    file(BOOT_ID_PATH, "11111111-2222-3333-4444-555555555555\n");
    file(SCREEN_DEVICE, "");
    assert(mkfifo(INPUT_MATRIX, 0600) == 0 && mkfifo(INPUT_GPIO, 0600) == 0);
    int matrix = open(INPUT_MATRIX, O_RDWR | O_NONBLOCK);
    int gpio = open(INPUT_GPIO, O_RDWR | O_NONBLOCK);
    assert(matrix >= 0 && gpio >= 0);
    guard(true);
    assert(rt_init(NULL) == 0);
    guard(false);
    memset(g_fb, 0x5a, sizeof g_fb);
    uint64_t deadline = rt_frame_deadline();
    assert(rt_present() == 0); /* first frame must wait for terminal to settle */
    assert(rt_wait(deadline) == RT_NONE);
    assert(rt_now() >= deadline);
    assert(rt_present() == 1);
    assert(rt_present() == 0); /* dedupe */
    guard(false);
    struct input_event ev = {.type=EV_KEY, .code=352, .value=1};
    assert(write(gpio, &ev, sizeof ev) == sizeof ev);
    assert(rt_key(20) == RT_OK);
    guard(false); /* pressing OK never gives terminal permission to draw */
    ev.value = 0; assert(write(gpio, &ev, sizeof ev) == sizeof ev);
    assert(rt_key(0) == RT_NONE);
    raise(SIGCONT); assert(rt_key(-1) == RT_REDRAW);
    guard(false);
    assert(rt_wait(rt_frame_deadline()) == RT_NONE);
    assert(rt_present() == 1); /* restore same frame even without a joystick */
    rt_request_full(); assert(rt_wait(rt_frame_deadline()) == RT_NONE);
    rt_idle(rt_now()); assert(budget_used == 30);
    assert(rt_key(0) == RT_NONE);
    rt_shutdown(); guard(true);
    close(matrix); close(gpio);
    const char *names[]={"refresh_cnt","fast_refresh_only","refresh","boot-id","screen","matrix","gpio","runtime.lock","refresh-budget","hardware.lock"};
    for (unsigned i=0;i<sizeof names/sizeof names[0];i++) {
        char path[256]; snprintf(path,sizeof path,"%s/%s",ROOT,names[i]);
        assert(unlink(path)==0);
    }
    assert(rmdir(ROOT)==0);
    puts("PASS device branch: startup frame without input, OK excludes terminal guard, resume redraw, dedupe, budget30 retained, release on shutdown");
    return 0;
}
