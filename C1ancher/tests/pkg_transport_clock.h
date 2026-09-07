/* Test-only clock injection for repo.c; util.c still polls real child processes.
 * No production environment switches or public API hooks are added. */
#ifndef C1PKG_TEST_TRANSPORT_CLOCK_H
#define C1PKG_TEST_TRANSPORT_CLOCK_H
#include <stdint.h>
#include <poll.h>
#include <time.h>

static uint64_t test_transport_ms = 100000U;
static int test_transport_real_wait;

static time_t test_transport_time(time_t *output)
{
    time_t now = test_transport_real_wait ? time(NULL) : (time_t)1700000000;
    if (output != NULL) *output = now;
    return now;
}

static int test_transport_clock_gettime(clockid_t clock, struct timespec *now)
{
    if (clock != CLOCK_MONOTONIC || test_transport_real_wait) return clock_gettime(clock, now);
    now->tv_sec = (time_t)(test_transport_ms / 1000U);
    now->tv_nsec = (long)(test_transport_ms % 1000U) * 1000000L;
    return 0;
}

static int test_transport_poll(struct pollfd *fds, nfds_t count, int timeout)
{
    if (fds != NULL || count != 0U || timeout < 0 || test_transport_real_wait)
        return poll(fds, count, timeout);
    test_transport_ms += (unsigned int)timeout;
    return 0;
}
#endif
