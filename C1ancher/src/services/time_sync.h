#ifndef C1_TIME_SYNC_H
#define C1_TIME_SYNC_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

typedef enum {
    C1_TIME_UNCHECKED, C1_TIME_RUNNING, C1_TIME_SYNCED, C1_TIME_FAILED, C1_TIME_UNAVAILABLE,
    C1_TIME_WAITING_SYSTEM, /* System service owns the clock; no kernel sync evidence yet. */
    C1_TIME_UNVERIFIED,     /* One-shot exited successfully, but synchronization is unproven. */
    C1_TIME_BLOCKED         /* Cannot safely establish whether another service owns the clock. */
} c1_time_state;
typedef struct {
    pid_t pid;
    int64_t deadline_ms, next_attempt_ms, next_observation_ms;
    bool previous_online, previous_enabled, timed_out, cancelled, kill_sent;
    c1_time_state state;
    const char *busybox_path;
    /* Private state for the bounded --list probe and read-only system observation. */
    int probe_fd, system_service;
    bool probing, applet_available, probe_found, probe_eof, probe_invalid, kernel_synced;
    size_t probe_bytes, probe_line_length;
    char probe_line[16];
} c1_time_sync;
void c1_time_sync_init(c1_time_sync *sync);
/* Nonblocking; now_ms is monotonic. System ntpd is observed, never started,
 * stopped or reconfigured. SYNCED means adjtimex(modes=0) confirms kernel sync,
 * not merely that a daemon exists, a command succeeded or the date is plausible.
 * An installed /usr/sbin/ntpd reserves clock ownership even while restarting.
 * BusyBox is a fallback only after an exact --list applet probe and a complete
 * bounded process scan. The fallback never invokes another ntpd's CLI or RTC. */
void c1_time_sync_tick(c1_time_sync *sync, bool enabled, bool online, int64_t now_ms);
/* Shutdown cancellation of the exact, still-owned direct child. Uses WNOHANG
 * only, with at most 100 ms of polling grace. If pid > 0 remains (e.g. an
 * uninterruptible child), keep ticking/calling stop; its PID is never forgotten. */
void c1_time_sync_stop(c1_time_sync *sync);
#endif
