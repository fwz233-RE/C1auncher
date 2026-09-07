#define _DEFAULT_SOURCE 1
#include "platform/liveness.h"
#include "platform/child_processes.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int heartbeat_fd = -1;
static pid_t heartbeat_owner;
static int64_t next_beat;

unsigned int c1_liveness_setting(const char *name, unsigned int fallback)
{
    const char *value = getenv(name);
    char *end;
    unsigned long parsed;
    if (value == NULL || *value == '\0') return fallback;
    errno = 0;
    parsed = strtoul(value, &end, 10);
    return errno == 0 && *end == '\0' && parsed >= 50U && parsed <= 60000U
               ? (unsigned int)parsed : fallback;
}

void c1_liveness_init(void)
{
    const char *value = getenv(C1_HEARTBEAT_FD_ENV);
    char *end;
    long descriptor;
    struct stat st;
    heartbeat_owner = getpid();
    if (value == NULL) return;
    errno = 0;
    descriptor = strtol(value, &end, 10);
    if (errno || *end || descriptor <= STDERR_FILENO || descriptor > INT_MAX) return;
    if (fstat((int)descriptor, &st) != 0 || !S_ISFIFO(st.st_mode)) return;
    heartbeat_fd = (int)descriptor;
    (void)fcntl(heartbeat_fd, F_SETFD, FD_CLOEXEC);
    (void)fcntl(heartbeat_fd, F_SETFL, O_NONBLOCK);
    (void)unsetenv(C1_HEARTBEAT_FD_ENV);
    next_beat = 0;
}

void c1_liveness_beat(int64_t now)
{
    sigset_t block, old, pending;
    bool was_pending;
    int saved;
    if (heartbeat_fd < 0 || getpid() != heartbeat_owner || now < next_beat) return;
    /* A dead supervisor must not turn one pipe write into SIGPIPE termination. */
    (void)sigemptyset(&block);
    (void)sigaddset(&block, SIGPIPE);
    if (sigprocmask(SIG_BLOCK, &block, &old) != 0) return;
    (void)sigpending(&pending);
    was_pending = sigismember(&pending, SIGPIPE) == 1;
    saved = write(heartbeat_fd, "H", 1U) == 1 ? 0 : errno;
    if (saved == EPIPE && !was_pending) {
        struct timespec zero = {0, 0};
        (void)sigtimedwait(&block, NULL, &zero);
    }
    (void)sigprocmask(SIG_SETMASK, &old, NULL);
    if (saved != 0 && saved != EAGAIN && saved != EINTR) c1_liveness_close();
    next_beat = now + C1_HEARTBEAT_INTERVAL_MS;
}

void c1_liveness_close(void)
{
    if (heartbeat_fd >= 0) (void)close(heartbeat_fd);
    heartbeat_fd = -1;
}

bool c1_liveness_expired(int64_t now, int64_t started, int64_t last,
                         unsigned int startup_ms, unsigned int timeout_ms)
{
    if (now < 0 || started < 0) return true;
    return last < 0 ? now - started >= startup_ms : now - last >= timeout_ms;
}

int c1_descendants_adopt(void)
{
    return prctl(PR_SET_CHILD_SUBREAPER, 1);
}

static int64_t now_ms(void)
{
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) return -1;
    return (int64_t)value.tv_sec * 1000 + value.tv_nsec / 1000000;
}

bool c1_descendants_cleanup(unsigned int timeout_ms)
{
    int64_t start = now_ms();
    for (;;) {
        int remaining = c1_child_processes_signal(-1, SIGKILL);
        if (remaining < 0) return false;
        if (remaining == 0) return true;
        if (start < 0 || now_ms() < 0 || now_ms() - start >= timeout_ms) return false;
        {
            struct timespec delay = {0, 10000000L};
            (void)nanosleep(&delay, NULL);
        }
    }
}
