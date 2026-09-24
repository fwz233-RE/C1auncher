#ifndef C1PKG_METRICS_H
#define C1PKG_METRICS_H

#include "pkg.h"

#define C1PKG_METRICS_QUEUE_MAX 64U
#define C1PKG_METRICS_BODY_MAX 8192U
#define C1PKG_METRICS_INTERVAL 900U

/* Untrusted, eventually consistent display metadata, NEVER install authority.
 * available == 0 or a missing ID means "statistics unavailable", not zero.
 * A known zero is represented by an explicit item with installations == 0. */
struct c1pkg_metrics_item {
    char id[C1PKG_ID_MAX + 1U];
    uint64_t installations;
};
struct c1pkg_metrics_snapshot {
    int available;
    size_t count;
    struct c1pkg_metrics_item items[C1PKG_MAX_PACKAGES];
};
struct c1pkg_metrics_job;

int c1pkg_metrics_parse(const unsigned char *data, size_t size,
                        struct c1pkg_metrics_snapshot *snapshot);
/* Returns 1 only for an explicitly known count, otherwise 0; leaves value
 * untouched on 0. Counts may be UINT64_MAX; use a 64-bit display formatter. */
int c1pkg_metrics_lookup(const struct c1pkg_metrics_snapshot *snapshot,
                         const char *id, uint64_t *value);

/* Legacy v1 event-count compatibility API. Best-effort private local queue
 * only: no network, no waiting for locks. Production uses the v2 hook below;
 * choose ONE hook after a verified online install returns C1PKG_INSTALL_OK.
 * GUI/TUI callers MUST NOT record again after the store's hook.
 * 0 queued, -1 unavailable/full/invalid. Never change the install result.
 * Offline imports have no authenticated online origin and are not reported. */
int c1pkg_metrics_record_install(const struct c1pkg_config *config,
                                 const struct c1pkg_package *package);

/* Start a bounded background worker, or return NULL on local failure. Copies
 * config before returning. Call from the single-threaded GUI/main process.
 * No network, disk locks or child completion are awaited by this call.
 * Only one worker across processes can sync; persistent cooldown is 15 minutes.
 * Suggested use: start on GUI entry and after installs, then every 15 minutes.
 * A busy worker or cooldown returns the matching cached snapshot if available.
 * The cache and queue are separate from signed index/rollback state. */
struct c1pkg_metrics_job *c1pkg_metrics_start(const struct c1pkg_config *config);
/* Nonblocking: 0 running; 1 completed; -1 failed/cancelled. On completion/failure
 * frees the job, sets *job=NULL and reaps its child. snapshot becomes unavailable
 * on failure. Poll from the existing GUI timer, including after cancellation.
 * Keep the snapshot in the GUI; lookup never accesses files or the network. */
int c1pkg_metrics_poll(struct c1pkg_metrics_job **job,
                       struct c1pkg_metrics_snapshot *snapshot);
/* Nonblocking cancellation; KEEP POLLING until *job == NULL to reap/free it.
 * The worker kills/reaps its curl child before exiting. */
void c1pkg_metrics_cancel(struct c1pkg_metrics_job *job);

/* Device-unique v2 statistics. Use these instead of (never alongside) the v1
 * record/start functions. The shared snapshot stores SERVER-RETURNED device
 * counts only. Unsupported v2 servers leave statistics unavailable.
 * Identity/queue live below package state, outside removable application data.
 * HMAC scope combines the application ID with the normalized repository URL.
 * ONLY the four official HTTP bases www.fwz233.com/c1/v1, /c1/v2 and
 * 123.56.214.77/c1/v1, /c1/v2 share canonical http://123.56.214.77/c1 identity.
 * Other bases (including HTTPS, explicit ports and additional path suffixes)
 * retain their exact normalized scope. This identity mapping NEVER changes
 * request URLs or exact-origin queue matching and NEVER bypasses retry gates. */
#define C1PKG_DEVICE_METRICS_RETRY_INTERVAL 30U
#define C1PKG_DEVICE_METRICS_BACKOFF_MAX 900U
int c1pkg_device_metrics_parse(const unsigned char *data, size_t size,
                               struct c1pkg_metrics_snapshot *snapshot);
int c1pkg_device_metrics_record_install(const struct c1pkg_config *config,
                                        const struct c1pkg_package *package);
/* Start on entry/after successful installation, then every RETRY_INTERVAL
 * seconds while the GUI is active; poll existing jobs on the GUI timer. Starting
 * does not bypass the durable global request gate. Successful GETs alone are
 * cached 900s; pending reports can retry after 30s (failure backoff up to 900s),
 * subject to longer server/legacy Retry-After including permanent pauses.
 * At most four events are sent per worker. The worker is nonblocking to callers.
 * Invalid/missing previously initialized identity/cache fails closed. */
struct c1pkg_metrics_job *c1pkg_device_metrics_start(const struct c1pkg_config *config);
int c1pkg_device_metrics_poll(struct c1pkg_metrics_job **job,
                              struct c1pkg_metrics_snapshot *snapshot);
/* Same cancellation/reaping contract as the v1 job functions. */
void c1pkg_device_metrics_cancel(struct c1pkg_metrics_job *job);

#endif
