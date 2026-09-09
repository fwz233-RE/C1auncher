#include "runtime.h"
#ifndef RT_HOST
#include "session.h"
#endif
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/input.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#ifndef EPAPER
#define EPAPER "/sys/devices/platform/e0266a128/epaper/"
#endif
#ifndef STATE_DIR
#define STATE_DIR "/usr/data/inkwars"
#endif
#ifndef BOOT_ID_PATH
#define BOOT_ID_PATH "/proc/sys/kernel/random/boot_id"
#endif
#ifndef SCREEN_DEVICE
#define SCREEN_DEVICE "/dev/epaper_lcd"
#endif
#ifndef INPUT_MATRIX
#define INPUT_MATRIX "/dev/input/event0"
#endif
#ifndef INPUT_GPIO
#define INPUT_GPIO "/dev/input/event1"
#endif
#define BUDGET_LIMIT 30u
/* Real driver count is a boot counter, NOT a per-minute rate. Fast writes
 * consume about 1, full refresh about 8. Reserve BEFORE I/O; never refund. */
#define FULL_COST 8u

uint8_t g_fb[CCG_FRAME_BYTES];
static uint8_t last_frame[CCG_FRAME_BYTES];
static bool has_last, pending_full, initialized;
static uint64_t last_write, last_full;
static volatile sig_atomic_t quitting, resumed;
static int wake_pipe[2] = {-1, -1};
static const int handled_signals[] = {SIGINT, SIGTERM, SIGHUP, SIGQUIT, SIGCONT};
static struct sigaction old_actions[5];
static unsigned installed_signals;
static int inputs[2] = {-1, -1};
static bool held[2][8], dropped[2];
static uint64_t repeat_at[2][8];
static unsigned next_input;
#ifdef RT_HOST
static const char *preview_path;
static struct termios original_term;
static bool term_changed;
#else
static int screen_fd = -1, budget_lock = -1;
static bool fast_safe;
#endif
#if !defined(RT_HOST) || defined(RT_TEST)
static bool budget_safe;
static unsigned budget_used, observed_count;
static char boot_id[37];
#endif

uint64_t rt_now(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static void signal_handler(int sig) {
    int saved = errno;
    unsigned char b = 1;
    if (sig == SIGCONT) resumed = 1;
    else quitting = 1;
    if (wake_pipe[1] >= 0) { ssize_t n = write(wake_pipe[1], &b, 1); (void)n; }
    errno = saved;
}

static rt_event map_device(unsigned code) {
    switch (code) {
    case KEY_UP: return RT_UP;
    case KEY_DOWN: return RT_DOWN;
    case KEY_LEFT: return RT_LEFT;
    case KEY_RIGHT: return RT_RIGHT;
    case 352: case 28: return RT_OK;
    case 158: return RT_BACK;
    default: return RT_NONE;
    }
}

/* Device aliases such as WASD, P, Q and DELETE intentionally never map. */
static rt_event device_event(unsigned dev, const struct input_event *ev, uint64_t now) {
    rt_event key;
    if (ev->type == EV_SYN && ev->code == SYN_DROPPED) {
        memset(held[dev], 0, sizeof held[dev]);
        dropped[dev] = true;
        fprintf(stderr, "Ink Wars: evdev queue overflow; waiting for SYN_REPORT\n");
        return RT_NONE;
    }
    if (dropped[dev]) {
        if (ev->type == EV_SYN && ev->code == SYN_REPORT) dropped[dev] = false;
        return RT_NONE;
    }
    if (ev->type != EV_KEY || (key = map_device(ev->code)) == RT_NONE) return RT_NONE;
    if (ev->value == 0) { held[dev][key] = false; return RT_NONE; }
    if (ev->value != 1 || held[dev][key]) return RT_NONE;
    held[dev][key] = true;
    repeat_at[dev][key] = now + RT_TICK_MS;
    return key;
}

static uint64_t repeat_deadline(uint64_t deadline) {
    unsigned d, k;
    for (d = 0; d < 2; ++d) for (k = RT_UP; k <= RT_RIGHT; ++k)
        if (held[d][k] && repeat_at[d][k] < deadline) deadline = repeat_at[d][k];
    return deadline;
}

static rt_event emit_repeat(uint64_t now) {
    unsigned d, k;
    for (d = 0; d < 2; ++d) for (k = RT_UP; k <= RT_RIGHT; ++k) {
        if (held[d][k] && now >= repeat_at[d][k]) {
            /* No catch-up burst after a blocking refresh or suspend. */
            repeat_at[d][k] = now + RT_TICK_MS;
            return (rt_event)k;
        }
    }
    return RT_NONE;
}

#ifdef RT_HOST
static rt_event host_event(unsigned char c) {
    switch (c) {
    case 'w': return RT_UP;
    case 's': return RT_DOWN;
    case 'a': return RT_LEFT;
    case 'd': return RT_RIGHT;
    case '\r': case '\n': return RT_OK;
    case 'b': return RT_BACK;
    default: return RT_NONE;
    }
}
#endif

static void resume_if_needed(void) {
    if (resumed) {
        resumed = 0;
        has_last = false;
        /* Never reset boot budget or request an unsolicited full refresh. */
        memset(held, 0, sizeof held);
    }
}

rt_event rt_wait(uint64_t deadline) {
    for (;;) {
        struct pollfd fds[3];
        uint64_t now, due;
        int timeout, r;
        unsigned i;
        rt_event key;
        if (quitting) return RT_QUIT;
        if (resumed) { resume_if_needed(); return RT_REDRAW; }
        now = rt_now();
        due = repeat_deadline(deadline);
        timeout = due <= now ? 0 : due - now > INT_MAX ? INT_MAX : (int)(due - now);
        for (i = 0; i < 2; ++i) {
            fds[i].fd = inputs[i]; fds[i].events = POLLIN; fds[i].revents = 0;
        }
        fds[2].fd = wake_pipe[0]; fds[2].events = POLLIN; fds[2].revents = 0;
        r = poll(fds, 3, timeout);
        if (r < 0) { if (errno == EINTR) continue; return RT_QUIT; }
        if (fds[2].revents & POLLIN) {
            unsigned char buf[64];
            while (read(wake_pipe[0], buf, sizeof buf) > 0) {}
        }
        if (quitting) return RT_QUIT;
        if (resumed) { resume_if_needed(); return RT_REDRAW; }
        for (i = 0; i < 2; ++i) {
            unsigned d = (next_input + i) % 2;
            if (fds[d].revents & POLLIN) {
#ifdef RT_HOST
                unsigned char c;
                ssize_t n = read(inputs[d], &c, 1);
                if (n == 0) return RT_QUIT;
                key = n == 1 ? host_event(c) : RT_NONE;
#else
                struct input_event ev;
                ssize_t n = read(inputs[d], &ev, sizeof ev);
                key = n == (ssize_t)sizeof ev ? device_event(d, &ev, rt_now()) : RT_NONE;
#endif
                if (key != RT_NONE) { next_input = 1u - d; return key; }
                /* Read queued releases/non-key events BEFORE synthesizing repeats.
                 * No queue draining at full refresh or scene transitions. */
                goto again;
            }
            if (fds[d].revents & (POLLERR | POLLHUP | POLLNVAL)) return RT_QUIT;
        }
        now = rt_now();
        if ((key = emit_repeat(now)) != RT_NONE) return key;
        if (now >= deadline) return RT_NONE;
again: ;
    }
}

rt_event rt_key(int timeout_ms) {
    return rt_wait(timeout_ms < 0 ? UINT64_MAX : rt_now() + (unsigned)timeout_ms);
}

#if !defined(RT_HOST) || defined(RT_TEST)
static int write_node(const char *path, const char *value) {
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    ssize_t n;
    if (fd < 0) return -1;
    do { n = write(fd, value, 1); } while (n < 0 && errno == EINTR && !quitting);
    close(fd);
    return n == 1 ? 0 : -1;
}

static bool read_count(unsigned *out) {
    char buf[32], *end;
    unsigned long v;
    int fd = open(EPAPER "refresh_cnt", O_RDONLY | O_CLOEXEC);
    ssize_t n;
    if (fd < 0) return false;
    n = read(fd, buf, sizeof buf - 1); close(fd);
    if (n <= 0 || n == (ssize_t)sizeof buf - 1) return false;
    buf[n] = 0;
    if (buf[0] < '0' || buf[0] > '9') return false;
    errno = 0; v = strtoul(buf, &end, 10);
    while (*end == '\n' || *end == '\r' || *end == ' ' || *end == '\t') ++end;
    if (errno || *end || v > BUDGET_LIMIT) return false;
    *out = (unsigned)v;
    return true;
}

static bool persist_budget(void) {
    /* Lifetime flock covers read-modify-write and rename. The ledger is
     * committed BEFORE a potentially consuming syscall (crash conservative). */
    char text[128];
    int len = snprintf(text, sizeof text, "IW1 %s %u %llu\n", boot_id,
                       budget_used, (unsigned long long)last_full);
    int fd = open(STATE_DIR "/refresh-budget.tmp", O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    bool ok;
    if (fd < 0) return false;
    ok = write(fd, text, (size_t)len) == len && fsync(fd) == 0;
    if (close(fd) != 0) ok = false;
    if (!ok || rename(STATE_DIR "/refresh-budget.tmp", STATE_DIR "/refresh-budget") != 0) return false;
    fd = open(STATE_DIR, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return false;
    ok = fsync(fd) == 0; close(fd);
    return ok;
}

static bool synchronize_budget(void) {
    unsigned count;
    if (!budget_safe || !read_count(&count)) { budget_safe = false; return false; }
    /* A falling counter within one boot cannot safely create new credit. */
    if (count < observed_count) budget_used = BUDGET_LIMIT;
    if (count > budget_used) budget_used = count;
    observed_count = count;
    return true;
}

static void init_budget(void) {
    char id[64], saved_id[64], extra;
    unsigned used;
    unsigned long long timestamp;
    ssize_t n;
    FILE *fp;
    int fd = open(BOOT_ID_PATH, O_RDONLY | O_CLOEXEC);
    budget_safe = false;
    if (fd < 0) return;
    n = read(fd, id, sizeof id - 1); close(fd);
    if (n != 37 || id[36] != '\n') return;
    id[36] = 0;
    for (unsigned i = 0; i < 36; ++i)
        if (!((id[i] >= '0' && id[i] <= '9') || (id[i] >= 'a' && id[i] <= 'f') || id[i] == '-')) return;
    memcpy(boot_id, id, 37);
    if (!read_count(&observed_count)) return;
    budget_used = observed_count;
    fp = fopen(STATE_DIR "/refresh-budget", "r");
    if (fp) {
        int parsed = fscanf(fp, "IW1 %63s %u %llu %c", saved_id, &used, &timestamp, &extra);
        fclose(fp);
        if (parsed != 3 || used > BUDGET_LIMIT) return;
        if (!strcmp(saved_id, boot_id)) {
            if (used > budget_used) budget_used = used;
            last_full = timestamp;
            if (last_full > rt_now()) return;
        }
    } else if (errno != ENOENT) return;
    budget_safe = persist_budget();
}

static bool reserve_full(uint64_t now) {
    if (!synchronize_budget() || budget_used > BUDGET_LIMIT - FULL_COST) return false;
    budget_used += FULL_COST;
    last_full = now;
    if (!persist_budget()) { budget_safe = false; return false; }
    return true;
}

static void reserve_fast(void) {
    unsigned before = budget_used;
    if (!synchronize_budget()) return;
    if (budget_used < BUDGET_LIMIT) ++budget_used;
    /* Once exhausted, the durable 30 never changes again this boot. Avoid
     * needless flash writes/fsync on every subsequent 700ms frame. */
    if (budget_used != before && !persist_budget()) budget_safe = false;
}
#endif

#ifdef RT_HOST
static int write_preview(void) {
    char temp[PATH_MAX];
    uint8_t row[(CCG_W + 7) / 8];
    FILE *fp;
    bool ok = true;
    unsigned x, y;
    if (!preview_path || !*preview_path) return 0;
    if (snprintf(temp, sizeof temp, "%s.tmp", preview_path) >= (int)sizeof temp) return -1;
    fp = fopen(temp, "wb");
    if (!fp) return -1;
    if (fprintf(fp, "P4\n%u %u\n", CCG_W, CCG_H) < 0) ok = false;
    for (y = 0; ok && y < CCG_H; ++y) {
        memset(row, 0, sizeof row);
        for (x = 0; x < CCG_W; ++x)
            if (g_fb[(y >> 3) * CCG_W + x] & (0x80u >> (y & 7))) row[x >> 3] |= (uint8_t)(0x80u >> (x & 7));
        ok = fwrite(row, 1, sizeof row, fp) == sizeof row;
    }
    if (fclose(fp) != 0) ok = false;
    if (!ok || rename(temp, preview_path) != 0) { unlink(temp); return -1; }
    return 0;
}
#endif

uint64_t rt_frame_deadline(void) { return last_write ? last_write + RT_TICK_MS : 0; }

int rt_present(void) {
    uint64_t now;
    if (!initialized || quitting) return -1;
    resume_if_needed();
    if (has_last && memcmp(last_frame, g_fb, sizeof last_frame) == 0) return 0;
    now = rt_now();
    if (last_write && now - last_write < RT_TICK_MS) return 0;
#ifdef RT_HOST
    if (write_preview() != 0) return -1;
#else
    if (!fast_safe) return -1;
    for (unsigned attempt = 0; attempt < 2; ++attempt) {
        ssize_t n;
        reserve_fast();
        n = write(screen_fd, g_fb, sizeof g_fb);
        if (n == (ssize_t)sizeof g_fb) goto written;
        if (quitting) return -1;
    }
    return -1;
written:
#endif
    /* Commit only after the entire frame / atomic preview succeeds. */
    memcpy(last_frame, g_fb, sizeof last_frame);
    has_last = true;
    last_write = rt_now();
    return 1;
}

void rt_request_full(void) { pending_full = true; }

void rt_idle(uint64_t now) {
    if (!initialized || quitting || !pending_full || !has_last) return;
    if (memcmp(last_frame, g_fb, sizeof last_frame) != 0) return;
    if (now < last_full || now - last_full < RT_FULL_GAP_MS) return;
    /* Let the asynchronous fast frame complete before issuing full refresh. */
    if (now < last_write || now - last_write < RT_TICK_MS) return;
#ifdef RT_HOST
    last_full = now;
    pending_full = false;
#else
    pending_full = false;
    if (!fast_safe || !reserve_full(now)) return;
    /* This sequence can block ~1.5s or more. Inputs stay in the evdev queues;
     * software never drains/discards them. No 700ms latency claim here. */
    if (write_node(EPAPER "fast_refresh_only", "0") == 0) {
        if (write_node(EPAPER "refresh", "1") != 0)
            fprintf(stderr, "Ink Wars: full refresh failed; budget retained\n");
    }
    fast_safe = write_node(EPAPER "fast_refresh_only", "1") == 0;
    if (!fast_safe) {
        fprintf(stderr, "Ink Wars: cannot restore fast-only mode; stopping display writes\n");
        quitting = 1;
    }
    last_write = rt_now();
    last_full = last_write;
    if (!persist_budget()) budget_safe = false;
#endif
}

int rt_init(const char *host_output_path) {
    struct sigaction sa;
    if (initialized) return -1;
    quitting = resumed = 0;
    has_last = pending_full = false;
    last_write = last_full = 0;
    installed_signals = 0;
    next_input = 0;
    memset(held, 0, sizeof held);
    memset(dropped, 0, sizeof dropped);
    if (pipe2(wake_pipe, O_NONBLOCK | O_CLOEXEC) != 0) return -1;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    for (unsigned i = 0; i < sizeof handled_signals / sizeof handled_signals[0]; ++i) {
        if (sigaction(handled_signals[i], &sa, &old_actions[i]) != 0) goto fail;
        ++installed_signals;
    }
#ifdef RT_HOST
    /* Reference shared device mapping for host-side deterministic tests too. */
    (void)device_event;
    preview_path = host_output_path;
    inputs[0] = STDIN_FILENO;
    inputs[1] = -1;
    if (isatty(STDIN_FILENO)) {
        struct termios raw;
        if (tcgetattr(STDIN_FILENO, &original_term) != 0) goto fail;
        raw = original_term;
        raw.c_lflag &= (tcflag_t)~(ICANON | ECHO);
        raw.c_cc[VMIN] = 1; raw.c_cc[VTIME] = 0;
        if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) goto fail;
        term_changed = true;
    }
#else
    (void)host_output_path;
    if (iw_session_open() != 0) goto fail;
    /* A prior terminal fast frame may still be settling after its shared
     * lease is released. Defer the first game frame by one panel tick. */
    last_write = rt_now();
    /* Shared state and its lock live outside the sealed package directory. */
    if (mkdir(STATE_DIR, 0700) != 0 && errno != EEXIST) goto fail;
    budget_lock = open(STATE_DIR "/runtime.lock", O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (budget_lock < 0 || flock(budget_lock, LOCK_EX | LOCK_NB) != 0) goto fail;
    fast_safe = write_node(EPAPER "fast_refresh_only", "1") == 0;
    if (!fast_safe) goto fail;
    screen_fd = open(SCREEN_DEVICE, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (screen_fd < 0) goto fail;
    inputs[0] = open(INPUT_MATRIX, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    inputs[1] = open(INPUT_GPIO, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (inputs[0] < 0 && inputs[1] < 0) goto fail;
    init_budget();
    if (!budget_safe) fprintf(stderr, "Ink Wars: unsafe refresh counter/state; full refresh disabled\n");
#endif
    initialized = true;
    return 0;
fail:
    rt_shutdown();
    return -1;
}

void rt_shutdown(void) {
#ifdef RT_HOST
    if (term_changed) { tcsetattr(STDIN_FILENO, TCSANOW, &original_term); term_changed = false; }
#else
    if (screen_fd >= 0) {
        /* Keep automatic full refresh suppressed even on ordinary exit. */
        (void)write_node(EPAPER "fast_refresh_only", "1");
        close(screen_fd); screen_fd = -1;
    }
    for (unsigned i = 0; i < 2; ++i) if (inputs[i] >= 0) close(inputs[i]);
    if (budget_lock >= 0) { close(budget_lock); budget_lock = -1; }
    iw_session_close();
#endif
    inputs[0] = inputs[1] = -1;
    for (unsigned i = 0; i < installed_signals; ++i) sigaction(handled_signals[i], &old_actions[i], NULL);
    installed_signals = 0;
    for (unsigned i = 0; i < 2; ++i) if (wake_pipe[i] >= 0) { close(wake_pipe[i]); wake_pipe[i] = -1; }
    initialized = false;
}
