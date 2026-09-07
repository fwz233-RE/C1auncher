#include "app_runtime.h"

#include "platform/display.h"

#include <signal.h>
#include <string.h>

static volatile sig_atomic_t s_stop_requested;
static volatile sig_atomic_t s_resume_requested;

static void handle_signal(int signal_number)
{
    if (signal_number == SIGCONT) {
        s_resume_requested = 1;
    } else {
        s_stop_requested = 1;
    }
}

int app_runtime_install_signals(void)
{
    static const int handled_signals[] = {SIGHUP, SIGTERM, SIGINT, SIGCONT};
    struct sigaction action;

    s_stop_requested = 0;
    s_resume_requested = 0;
    memset(&action, 0, sizeof(action));
    action.sa_handler = handle_signal;
    if (sigemptyset(&action.sa_mask) != 0) {
        return -1;
    }
    for (unsigned int i = 0; i < sizeof(handled_signals) / sizeof(handled_signals[0]); i++) {
        if (sigaction(handled_signals[i], &action, NULL) != 0) {
            return -1;
        }
    }
    action.sa_handler = SIG_IGN;
    if (sigaction(SIGPIPE, &action, NULL) != 0) {
        return -1;
    }
    return 0;
}

bool app_runtime_checkpoint(void)
{
    if (s_resume_requested) {
        s_resume_requested = 0;
        disp_resume();
    }
    return s_stop_requested != 0;
}
