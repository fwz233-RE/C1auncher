#include "hal/linux/poweroff.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* UI-thread only: retain at most one timed-out helper until it is reapable. */
static pid_t pending_child = -1;

void c1_linux_poweroff_reap(void)
{
    if (pending_child > 0) {
        pid_t waited = waitpid(pending_child, NULL, WNOHANG);
        if (waited == pending_child || (waited < 0 && errno == ECHILD))
            pending_child = -1;
    }
}

static int64_t clock_ms(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return -1;
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

int c1_linux_poweroff_run(const c1_poweroff_command *commands, size_t count,
                          unsigned int timeout_ms, void (*heartbeat)(void))
{
    int64_t started = clock_ms();
    pid_t child;
    int result = EIO;
    if (commands == NULL || count == 0 || timeout_ms == 0 || started < 0) return EINVAL;
    for (size_t i = 0; i < count; ++i)
        if (commands[i].path == NULL || commands[i].path[0] != '/') return EINVAL;
    c1_linux_poweroff_reap();
    if (pending_child > 0) return EBUSY;
    child = fork();
    if (child < 0) return errno;
    if (child == 0) {
        size_t i;
        int nullfd;
        /* Isolate only this helper, never the launcher or other apps. */
        (void)setpgid(0, 0);
        (void)signal(SIGTERM, SIG_DFL);
        (void)signal(SIGINT, SIG_DFL);
        (void)signal(SIGHUP, SIG_DFL);
        nullfd = open("/dev/null", O_RDONLY);
        if (nullfd >= 0) {
            (void)dup2(nullfd, STDIN_FILENO);
            if (nullfd != STDIN_FILENO) close(nullfd);
        }
        for (i = 0; i < count; ++i) {
            if (commands[i].shutdown_arguments)
                execl(commands[i].path, "shutdown", "-h", "now", (char *)NULL);
            else
                execl(commands[i].path, "poweroff", (char *)NULL);
        }
        _exit(127); // All exec attempts failed. Parent must not report success.
    }
    (void)setpgid(child, child);
    for (;;) {
        int status;
        int64_t now;
        pid_t waited = waitpid(child, &status, WNOHANG);
        if (waited == child)
            return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : EIO;
        if (waited < 0 && errno != EINTR) return errno;
        if (heartbeat != NULL) heartbeat();
        now = clock_ms();
        if (now < 0 || now - started >= timeout_ms) {
            result = now < 0 ? EIO : ETIMEDOUT;
            break;
        }
        {
            struct timespec pause = {0, 20000000L};
            (void)nanosleep(&pause, NULL);
        }
    }
    /* Bounded helper timeout. No forced reboot/power-cut fallback. */
    (void)kill(-child, SIGKILL);
    (void)kill(child, SIGKILL);
    /* Even SIGKILL cannot immediately end an uninterruptible kernel wait.
     * Never block the UI/supervisor heartbeat in a final waitpid here. */
    pending_child = child;
    c1_linux_poweroff_reap();
    return result;
}

int c1_linux_poweroff_request(void (*heartbeat)(void))
{
    static const c1_poweroff_command commands[] = {
        {"/sbin/poweroff", 0},
        {"/bin/poweroff", 0},
        {"/sbin/shutdown", 1}
    };
    return c1_linux_poweroff_run(commands, sizeof(commands) / sizeof(commands[0]),
                                10000U, heartbeat);
}
