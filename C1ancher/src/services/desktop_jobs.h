#ifndef C1_DESKTOP_JOBS_H
#define C1_DESKTOP_JOBS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define C1_DESKTOP_JOB_OUTPUT_CAPACITY 256U
#define C1_DESKTOP_PACKAGE_JOB_TIMEOUT_MS 35000
#define C1_DESKTOP_CORE_JOB_TIMEOUT_MS 100000
#define C1_DESKTOP_PACKAGE_CANCEL_GRACE_MS 2000
/* check-configured needs time to stop its isolated fetch worker and remove its
 * own .check directory. Callers must continue polling throughout this grace. */
#define C1_DESKTOP_CORE_CANCEL_GRACE_MS 10000

typedef struct {
    pid_t pid;
    int fd, kind;
    bool cancelled, killed, failed;
    int64_t deadline;
    size_t used;
    char output[C1_DESKTOP_JOB_OUTPUT_CAPACITY];
} c1_desktop_job;

/* Single-owner lifecycle: init only idle storage; do not reap the job elsewhere.
 * All timestamps are nonnegative CLOCK_MONOTONIC milliseconds. */
void c1_desktop_job_init(c1_desktop_job *job);
/* kind 1: c1pkg desktop-summary; kind 2: c1updater check-configured. */
bool c1_desktop_job_start(c1_desktop_job *job, int kind, int64_t now);
/* Nonblocking: 0 running/idle, 1 successful complete text output, -1 failure.
 * A terminal result closes fd, resets pid, and retains kind. Failed/cancelled
 * output is cleared. After cancel, keep polling until a terminal result. */
int c1_desktop_job_poll(c1_desktop_job *job, int64_t now);
void c1_desktop_job_cancel(c1_desktop_job *job, int64_t now);
/* Exact v1 wire format, positive canonical uint64 sequence and 1..64 byte
 * manifest-safe version token. Outputs remain untouched on parse failure. */
bool c1_desktop_update_parse(const char *text, size_t size, uint64_t *sequence, bool *available);

#endif
