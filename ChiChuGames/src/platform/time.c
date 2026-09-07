#define _POSIX_C_SOURCE 200809L
#include "time.h"
#include <time.h>

uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

void sleep_until(uint64_t t_ms) {
    struct timespec ts;
    uint64_t now = now_ms();
    if (t_ms <= now) return;
    ts.tv_sec = (time_t)((t_ms - now) / 1000u);
    ts.tv_nsec = (long)(((t_ms - now) % 1000u) * 1000000u);
    clock_nanosleep(CLOCK_MONOTONIC, 0, &ts, NULL);
}
