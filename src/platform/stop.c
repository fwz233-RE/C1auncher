#include "platform/stop.h"

#include <signal.h>
#include <string.h>

static volatile sig_atomic_t stop_requested_flag = 0;

static void handle_stop_signal(int signal_number)
{
    (void)signal_number;
    stop_requested_flag = 1;
}

void c1_stop_install(void)
{
    struct sigaction action;

    memset(&action, 0, sizeof(action));
    action.sa_handler = handle_stop_signal;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);
    sigaction(SIGHUP, &action, NULL);
}

bool c1_stop_requested(void)
{
    return stop_requested_flag != 0;
}

void c1_stop_reset(void)
{
    stop_requested_flag = 0;
}