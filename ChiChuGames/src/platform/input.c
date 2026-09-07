#include "input.h"
#include "time.h"
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <string.h>
#include <linux/input.h>

#define DEV_MATRIX "/dev/input/event0"
#define DEV_GPIO "/dev/input/event1"

#define QUEUE_CAP 32

#ifndef CHICHU_HOST
static int s_fd0 = -1, s_fd1 = -1;
#endif

static key_event_t s_q[QUEUE_CAP];
static uint8_t s_qr = 0, s_qw = 0;

static bool s_held[K_COUNT];
static uint64_t s_held_since[K_COUNT];
static uint64_t s_last_emit[K_COUNT];

static uint32_t s_repeat_init = 500, s_repeat_ms = 150;
static bool s_repeat_on = true;

static void q_push(key_event_t e) {
    s_q[s_qw] = e;
    s_qw = (s_qw + 1) & (QUEUE_CAP - 1);
    if (s_qw == s_qr) s_qr = (s_qr + 1) & (QUEUE_CAP - 1); /* 覆盖最旧 */
}

#ifndef CHICHU_HOST
/* ---- 键码映射 ---- */
static const char s_row1[] = "qwertyuiop";  /* KEY_Q=16 .. KEY_P=25 */
static const char s_row2[] = "asdfghjkl";   /* KEY_A=30 .. KEY_L=38 */
static const char s_row3[] = "zxcvbnm";     /* KEY_Z=44 .. KEY_M=50 */
static const char s_digits[] = "1234567890";/* KEY_1=2  .. KEY_0=11 */
#endif

#ifndef CHICHU_HOST
static ccg_key map_key(int code, uint8_t *ch) {
    *ch = 0;
    switch (code) {
    case KEY_UP: return K_UP;
    case KEY_DOWN: return K_DOWN;
    case KEY_LEFT: return K_LEFT;
    case KEY_RIGHT: return K_RIGHT;
    case KEY_ENTER: return K_OK;
    case 352 /* KEY_OK */: return K_OK;
    case KEY_BACK: return K_BACK;
    case KEY_DELETE: return K_DEL;
    case KEY_WAKEUP: return K_WAKEUP;
    case KEY_SPACE: return K_SPACE;
    case KEY_LEFTSHIFT: return K_SHIFT;
    case KEY_VOLUMEDOWN: return K_VOLDOWN;
    case KEY_VOLUMEUP: return K_VOLUP;
    case KEY_Q: return K_QUIT;         /* Q 专用退出键 */
    case 25 /* KEY_P */: return K_PAUSE;
    case KEY_HOME: return K_NONE;      /* 系统层处理, 忽略 */
    }
    if (code >= 16 && code <= 25) { *ch = (uint8_t)s_row1[code - 16]; return K_CHAR; }
    if (code >= 30 && code <= 38) { *ch = (uint8_t)s_row2[code - 30]; return K_CHAR; }
    if (code >= 44 && code <= 50) { *ch = (uint8_t)s_row3[code - 44]; return K_CHAR; }
    if (code >= 2 && code <= 11) { *ch = (uint8_t)s_digits[code - 2]; return K_CHAR; }
    return K_NONE;
}
#endif /* !CHICHU_HOST */

static bool is_repeatable(ccg_key k) {
    switch (k) {
    case K_UP: case K_DOWN: case K_LEFT: case K_RIGHT: case K_DEL:
        return true;
    default:
        return false;
    }
}

#ifndef CHICHU_HOST
/* ---- 读取事件 ---- */
static void read_events(int fd) {
    struct input_event ev;
    for (;;) {
        ssize_t n = read(fd, &ev, sizeof(ev));
        if (n != (ssize_t)sizeof(ev)) break;
        if (ev.type != EV_KEY) continue;
        uint8_t ch;
        ccg_key k = map_key((int)ev.code, &ch);
        if (k == K_NONE) continue;
        key_event_t e;
        e.key = k;
        e.ch = ch;
        if (ev.value == 0) {           /* release */
            s_held[k] = false;
            s_last_emit[k] = 0;
            continue;                  /* 释放不入队(避免菜单误触) */
        }
        if (ev.value != 1) continue;    /* 内核重复由统一的软件时序生成 */
        s_held[k] = true;
        s_held_since[k] = now_ms();
        e.is_repeat = false;
        q_push(e);
    }
}
#endif /* !CHICHU_HOST */

int input_init(void) {
#ifndef CHICHU_HOST
    s_fd0 = open(DEV_MATRIX, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    s_fd1 = open(DEV_GPIO, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (s_fd0 < 0 && s_fd1 < 0) return -1;
#endif
    input_drain();
    memset(s_held_since, 0, sizeof(s_held_since));
    return 0;
}

void input_cleanup(void) {
#ifndef CHICHU_HOST
    if (s_fd0 >= 0) { close(s_fd0); s_fd0 = -1; }
    if (s_fd1 >= 0) { close(s_fd1); s_fd1 = -1; }
#endif
}

void input_poll(int timeout_ms) {
#ifndef CHICHU_HOST
    struct pollfd fds[2];
    int nfds = 0;
    if (s_fd0 >= 0) { fds[nfds].fd = s_fd0; fds[nfds].events = POLLIN; nfds++; }
    if (s_fd1 >= 0) { fds[nfds].fd = s_fd1; fds[nfds].events = POLLIN; nfds++; }
    if (nfds == 0) return;
    int r = poll(fds, nfds, timeout_ms);
    if (r > 0) {
        for (int i = 0; i < nfds; i++)
            if (fds[i].revents & POLLIN) read_events(fds[i].fd);
    }
#endif
    (void)timeout_ms; /* host 模式无 poll */
    /* 长按重复合成 */
    if (!s_repeat_on) return;
    uint64_t now = now_ms();
    for (int k = 1; k < K_COUNT; k++) {
        if (!is_repeatable((ccg_key)k) || !s_held[k]) continue;
        if (now - s_held_since[k] < s_repeat_init) continue;
        uint64_t next = s_last_emit[k] ? s_last_emit[k] + s_repeat_ms
                                       : s_held_since[k] + s_repeat_init;
        int emitted = 0;
        while (next <= now && emitted < 2) {  /* 上限防积压突爆 */
            key_event_t e = { (ccg_key)k, 0, true };
            q_push(e);
            s_last_emit[k] = next;
            next += s_repeat_ms;
            emitted++;
        }
        /* Drop missed repeat intervals after a stall instead of replaying
         * another pair on every subsequent poll. */
        if (next <= now) s_last_emit[k] = now;
    }
}

bool input_get(key_event_t *ev) {
    if (s_qr == s_qw) return false;
    *ev = s_q[s_qr];
    s_qr = (s_qr + 1) & (QUEUE_CAP - 1);
#ifdef CCG_TIMELINE
    extern void tm_mark(const char *);
    tm_mark("KEY");   /* 消费给逻辑的输入时刻 */
#endif
    return true;
}

void input_drain(void) {
    s_qr = s_qw;
    memset(s_held, 0, sizeof(s_held));
    memset(s_last_emit, 0, sizeof(s_last_emit));
}

void input_set_repeat(uint32_t init_ms, uint32_t repeat_ms) {
    s_repeat_init = init_ms;
    s_repeat_ms = repeat_ms;
}
