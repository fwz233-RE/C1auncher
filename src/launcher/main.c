#define _DEFAULT_SOURCE 1

#include "launcher/policy.h"

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define C1ANCHER_PATH "/usr/data/c1/bin/C1ancher"
#define C1_CHILD_STOP_GRACE_MS 3000
#define C1_RESTART_DELAY_SECONDS 1U

static volatile sig_atomic_t stop_requested = 0;
static volatile sig_atomic_t child_pid = -1;

static void handle_stop(int signal_number)
{
    (void)signal_number;
    stop_requested = 1;
    if (child_pid > 0) {
        kill((pid_t)child_pid, SIGTERM);
    }
}

static int64_t monotonic_milliseconds(void)
{
    struct timespec value;

    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) {
        return -1;
    }
    return (int64_t)value.tv_sec * 1000 + value.tv_nsec / 1000000;
}

static void stop_child(pid_t process)
{
    int64_t deadline = monotonic_milliseconds() + C1_CHILD_STOP_GRACE_MS;
    int status;

    kill(process, SIGTERM);
    while (monotonic_milliseconds() < deadline) {
        pid_t result = waitpid(process, &status, WNOHANG);

        if (result == process || (result < 0 && errno == ECHILD)) {
            return;
        }
        if (result < 0 && errno != EINTR) {
            break;
        }
        {
            struct timespec interval = {0, 100000000L};
            nanosleep(&interval, NULL);
        }
    }
    kill(process, SIGKILL);
    while (waitpid(process, &status, 0) < 0 && errno == EINTR) {
    }
}

static void run_child(void)
{
    int status = 0;
    pid_t process = fork();

    if (process < 0) {
        return;
    }
    if (process == 0) {
        pid_t parent = getppid();

        prctl(PR_SET_PDEATHSIG, SIGTERM);
        if (getppid() != parent) {
            _exit(125);
        }
        execl(C1ANCHER_PATH, C1ANCHER_PATH, "app", (char *)NULL);
        _exit(127);
    }

    child_pid = process;
    for (;;) {
        pid_t result = waitpid(process, &status, 0);

        if (result == process) {
            break;
        }
        if (result < 0 && errno == EINTR) {
            if (stop_requested) {
                stop_child(process);
                break;
            }
            continue;
        }
        break;
    }
    child_pid = -1;
}

static void install_signal_handlers(void)
{
    struct sigaction action;

    memset(&action, 0, sizeof(action));
    action.sa_handler = handle_stop;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);
    sigaction(SIGHUP, &action, NULL);
}

int main(void)
{
    prctl(PR_SET_NAME, "app_daemon");
    install_signal_handlers();

    do {
        run_child();
        if (c1_launcher_should_restart(stop_requested != 0)) {
            sleep(C1_RESTART_DELAY_SECONDS);
        }
    } while (c1_launcher_should_restart(stop_requested != 0));

    return EXIT_SUCCESS;
}