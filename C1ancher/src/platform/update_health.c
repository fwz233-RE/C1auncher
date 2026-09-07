#define _DEFAULT_SOURCE 1

#include "platform/update_health.h"

#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static long long now_ms(void)
{
    struct timespec value;

    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) {
        return -1;
    }
    return (long long)value.tv_sec * 1000LL + value.tv_nsec / 1000000L;
}

static void stop_probe(pid_t child)
{
    (void)kill(-child, SIGKILL);
    (void)kill(child, SIGKILL);
    while (waitpid(child, NULL, 0) < 0 && errno == EINTR) {}
}

static int run_short(const char *path, const char *arg1, const char *arg2,
                     const char *arg3, unsigned int timeout_ms)
{
    pid_t child;
    int status = 0;
    int null_fd;
    long long deadline;

    if (path == NULL || path[0] == '\0') {
        return -1;
    }
    child = fork();
    if (child < 0) {
        return -1;
    }
    if (child == 0) {
        if (setpgid(0, 0) != 0) _exit(126);
        null_fd = open("/dev/null", O_WRONLY);
        if (null_fd < 0 || dup2(null_fd, STDOUT_FILENO) < 0 ||
            dup2(null_fd, STDERR_FILENO) < 0) {
            _exit(126);
        }
        if (null_fd > STDERR_FILENO) {
            close(null_fd);
        }
        if (arg3 != NULL) {
            execl(path, path, arg1, arg2, arg3, (char *)NULL);
        } else if (arg2 != NULL) {
            execl(path, path, arg1, arg2, (char *)NULL);
        } else if (arg1 != NULL) {
            execl(path, path, arg1, (char *)NULL);
        } else {
            execl(path, path, (char *)NULL);
        }
        _exit(127);
    }
    if (setpgid(child, child) != 0 && errno != EACCES && errno != ESRCH) {
        stop_probe(child);
        return -1;
    }
    deadline = now_ms();
    if (deadline < 0) {
        stop_probe(child);
        return -1;
    }
    deadline += timeout_ms == 0U ? 1000U : timeout_ms;
    for (;;) {
        pid_t result = waitpid(child, &status, WNOHANG);
        long long current = now_ms();

        if (result == child) {
            /* Even a successful probe must not leave background helpers. */
            (void)kill(-child, SIGKILL);
            return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
        }
        if (result < 0 && errno != EINTR) {
            stop_probe(child);
            return -1;
        }
        if (current < 0 || current >= deadline) {
            stop_probe(child);
            return -1;
        }
        {
            struct timespec delay = {0, 1000000L};
            (void)nanosleep(&delay, NULL);
        }
    }
}

int c1_update_health_should_probe(const struct c1_update_state *state)
{
    return state != NULL && state->phase == C1_UPDATE_PENDING_BOOT;
}

static int running_companion(char *path, size_t path_size, const char *name)
{
    char executable[C1_UPDATE_PATH_MAX];
    char *slash;
    ssize_t length;
    int count;

    if (path == NULL || path_size == 0U || name == NULL || strchr(name, '/') != NULL) {
        return -1;
    }
    length = readlink("/proc/self/exe", executable, sizeof(executable) - 1U);
    if (length <= 0 || (size_t)length >= sizeof(executable)) {
        return -1;
    }
    executable[(size_t)length] = '\0';
    slash = strrchr(executable, '/');
    if (slash == NULL) {
        return -1;
    }
    *slash = '\0';
    count = snprintf(path, path_size, "%s/%s", executable, name);
    return count >= 0 && (size_t)count < path_size ? 0 : -1;
}

int c1_update_health_probe_running(const char *state_root, const char *ready_file,
                                   unsigned int timeout_ms)
{
    char pkg_path[C1_UPDATE_PATH_MAX];
    char updater_path[C1_UPDATE_PATH_MAX];
    struct c1_update_health_config config;

    if (running_companion(pkg_path, sizeof(pkg_path), "c1pkg") != 0 ||
        running_companion(updater_path, sizeof(updater_path), "c1updater") != 0) {
        return -1;
    }
    config.pkg_path = pkg_path;
    config.updater_path = updater_path;
    config.state_root = state_root;
    config.ready_file = ready_file;
    config.timeout_ms = timeout_ms;
    return c1_update_health_probe(&config);
}

int c1_update_health_probe(const struct c1_update_health_config *config)
{
    unsigned int timeout;

    if (config == NULL || config->pkg_path == NULL || config->updater_path == NULL ||
        config->state_root == NULL || config->ready_file == NULL) {
        return -1;
    }
    timeout = config->timeout_ms == 0U ? 1000U : config->timeout_ms;
    if (run_short(config->pkg_path, "--help", NULL, NULL, timeout) != 0 ||
        run_short(config->updater_path, "--self-test", NULL, NULL, timeout) != 0 ||
        run_short(config->updater_path, "mark-ready", config->state_root,
                  config->ready_file, timeout) != 0) {
        return -1;
    }
    return 0;
}