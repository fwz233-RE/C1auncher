#ifndef INKWARS_RUNTIME_H
#define INKWARS_RUNTIME_H
#include <stdint.h>
#include "config.h"

/* Shared ChiChuGames canvas layout: black=1, (y/8)*296+x, MSB top. */
extern uint8_t g_fb[CCG_FRAME_BYTES];
typedef enum {
    RT_NONE = 0, RT_UP, RT_DOWN, RT_LEFT, RT_RIGHT, RT_OK, RT_BACK, RT_QUIT, RT_REDRAW
} rt_event;
#define RT_TICK_MS 700u
#define RT_FULL_GAP_MS 5000u

/* RT_HOST builds read stdin and optionally publish an atomic PBM preview.
 * Device builds ignore host_output_path. Return 0 on success, -1 on error. */
int rt_init(const char *host_output_path);
void rt_shutdown(void);
uint64_t rt_now(void); /* CLOCK_MONOTONIC milliseconds */
rt_event rt_key(int timeout_ms); /* -1=infinite, 0=nonblocking */
rt_event rt_wait(uint64_t deadline_ms); /* poll; RT_NONE at deadline or SIGCONT (redraw then) */
/* 1=written, 0=unchanged/deferred (<700ms since write), -1=failed.
 * A deferred/failed frame is NOT cached: call again on a later tick. */
int rt_present(void);
uint64_t rt_frame_deadline(void); /* next safe write, including after a full refresh */
/* Call ONLY on explicit scene/turn/result transitions. Requests coalesce. */
void rt_request_full(void);
/* Call at idle points after present. Never sleeps for throttle eligibility.
 * A permitted full refresh itself BLOCKS about 1.5s (possibly longer).
 * 700ms input response is impossible during that syscall; evdev is not drained
 * or discarded. Kernel queue overflow remains a hardware/OS limitation. */
void rt_idle(uint64_t now_ms);
#endif
