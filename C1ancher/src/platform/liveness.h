#ifndef C1_PLATFORM_LIVENESS_H
#define C1_PLATFORM_LIVENESS_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

#define C1_SUPERVISOR_HEARTBEAT_FD_ENV "C1_SUPERVISOR_HEARTBEAT_FD"
/* Launcher -> core supervisor, one atomic packet per observed UI heartbeat.
 * A candidate must see a packet at/after its full ready observation window,
 * from the same UI PID, before confirmation. Missing packets fail closed. */
struct c1_liveness_message {
    int64_t monotonic_ms;
    int64_t ui_pid;
};

#define C1_HEARTBEAT_FD_ENV "C1_UI_HEARTBEAT_FD"
#define C1_HEARTBEAT_INTERVAL_MS 2000U
#define C1_HEARTBEAT_TIMEOUT_MS 12000U
#define C1_HEARTBEAT_STARTUP_MS 20000U

/* Only the UI loop calls beat; workers must never manufacture progress. */
void c1_liveness_init(void);
void c1_liveness_beat(int64_t now);
void c1_liveness_close(void);
bool c1_liveness_expired(int64_t now, int64_t started, int64_t last,
                         unsigned int startup_ms, unsigned int timeout_ms);
unsigned int c1_liveness_setting(const char *name, unsigned int fallback);

/* Linux: adopt only our own orphan descendants. Cleanup never scans/kills
 * unrelated process groups. Direct children are held unreaped until signalled,
 * preventing PID reuse between identification and kill. */
int c1_descendants_adopt(void);
bool c1_descendants_cleanup(unsigned int timeout_ms);

#endif
