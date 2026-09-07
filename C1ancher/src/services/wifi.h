#ifndef C1_SERVICES_WIFI_H
#define C1_SERVICES_WIFI_H

#include "core/status.h"

#include <stdbool.h>
#include <stddef.h>

#define C1_WIFI_MAX_NETWORKS 8U
#define C1_WIFI_SSID_CAPACITY 33U
#define C1_WIFI_IP_CAPACITY 16U
#define C1_WIFI_ERROR_CAPACITY 48U

typedef enum {
    C1_WIFI_DISABLED = 0,
    C1_WIFI_READY,
    C1_WIFI_SCANNING,
    C1_WIFI_CONNECTING,
    C1_WIFI_CONNECTED,
    C1_WIFI_ERROR
} c1_wifi_state;

/* Only OPEN and WPA_PSK are currently connectable. WPA_PSK includes WPA2
 * and WPA2/WPA3 transition networks advertising PSK; SAE-only is unsupported. */
typedef enum {
    C1_WIFI_SECURITY_OPEN = 0,
    C1_WIFI_SECURITY_WPA_PSK,
    C1_WIFI_SECURITY_WEP,
    C1_WIFI_SECURITY_ENTERPRISE,
    C1_WIFI_SECURITY_SAE,
    C1_WIFI_SECURITY_OWE,
    C1_WIFI_SECURITY_UNKNOWN
} c1_wifi_security;

typedef enum {
    C1_WIFI_PHASE_IDLE = 0,
    C1_WIFI_PHASE_PREPARING,
    C1_WIFI_PHASE_SCANNING,
    C1_WIFI_PHASE_AUTHENTICATING,
    C1_WIFI_PHASE_ACQUIRING_ADDRESS,
    C1_WIFI_PHASE_SAVING,
    C1_WIFI_PHASE_RESTORING,
    C1_WIFI_PHASE_DONE,
    C1_WIFI_PHASE_CANCELLED,
    C1_WIFI_PHASE_FAILED
} c1_wifi_phase;

typedef struct {
    char ssid[C1_WIFI_SSID_CAPACITY];
    int signal_dbm;
    bool secured; /* Compatibility field: true for every non-open type. */
    c1_wifi_security security;
    bool supported;
    bool saved; /* Matching configured credentials; never contains the secret. */
} c1_wifi_network;

typedef struct {
    c1_wifi_state state;
    c1_wifi_network networks[C1_WIFI_MAX_NETWORKS];
    size_t network_count;
    char connected_ssid[C1_WIFI_SSID_CAPACITY];
    char ipv4[C1_WIFI_IP_CAPACITY];
    char error[C1_WIFI_ERROR_CAPACITY];
    c1_wifi_phase phase;
} c1_wifi_snapshot;

/* Called synchronously during bounded waits. Return false to cancel. The
 * callback must not reenter this service or block. With fork-based UI workers,
 * read a nonblocking cancellation pipe here; killing the worker prevents
 * rollback. RESTORING ignores cancellation; SAVING is the commit boundary.
 * APIs are single-operation/non-thread-safe, as before. */
typedef bool (*c1_wifi_progress_fn)(c1_wifi_phase phase, void *context);
typedef struct {
    c1_wifi_progress_fn progress;
    void *context;
} c1_wifi_operation_options;

/* Passive discovery also works before the first service operation, including
 * the factory wlan0 control socket. Requires COMPLETED + decoded SSID + IPv4.
 * Read-only status probes share a 40 ms total wait budget and cache both
 * success/failure for 1 s; UI reads within that interval perform no probe.
 * A lost connection may therefore remain visible for up to 1 s. Busy/error
 * states and explicit disable take precedence, and adopted results invalidate
 * the observation cache. Snapshot reads never initialize hardware or DHCP. */
bool c1_wifi_read_snapshot(c1_wifi_snapshot *snapshot);
bool c1_wifi_decode_scan_ssid(const char *encoded, char *decoded, size_t capacity);
bool c1_wifi_encode_control_ssid(const char *ssid, char *encoded, size_t capacity);
bool c1_wifi_security_supported(c1_wifi_security security);
/* Operations use a monotonic total budget: scan 20 s, connect 45 s plus at
 * most 10 s rollback, resume 30 s. Ordinary OS I/O/scheduling is not hard
 * real-time. Existing callbacks and config bounds can shorten these budgets.
 * Scan requires ATTACH, BSS_FLUSH and BSS FIRST/NEXT in wpa_supplicant;
 * near-limit network lists require LIST_NETWORKS LAST_ID pagination; short
 * complete replies also support older standard 4096-byte-reply daemons.
 * Foreign wlan0 supplicants/DHCP clients are refused rather than terminated;
 * vendor wifi_up/down scripts are deliberately not executed. Cold start may
 * load only /etc/firmware/atbm603x_wifi_sdio.ko, with a 6 s hardware budget
 * inside the operation budget, and unblock only wlan0's physical radio.
 * Disable/pause lower only wlan0 and keep the managed daemon available for
 * resume; they do not promise chipset/module power removal. */
c1_status c1_wifi_scan(c1_wifi_snapshot *snapshot);
c1_status c1_wifi_scan_ex(const c1_wifi_operation_options *options, c1_wifi_snapshot *snapshot);
/* Legacy connect infers security from the scan cache, rejecting ambiguous
 * same-name types. Without a cached match it retains open/PSK inference. */
c1_status c1_wifi_connect(const char *ssid, const char *password, c1_wifi_snapshot *snapshot);
c1_status c1_wifi_connect_ex(const char *ssid, const char *password,
                              c1_wifi_security security,
                              const c1_wifi_operation_options *options,
                              c1_wifi_snapshot *snapshot);
/* Reuse a matching supplicant profile without reading its password into UI,
 * creating another profile, or rewriting persistent credentials. */
c1_status c1_wifi_connect_saved_ex(const char *ssid, c1_wifi_security security,
                                    const c1_wifi_operation_options *options,
                                    c1_wifi_snapshot *snapshot);
c1_status c1_wifi_disable(c1_wifi_snapshot *snapshot);
c1_status c1_wifi_pause(bool *was_enabled, bool *was_connected, bool *was_managed);
c1_status c1_wifi_resume(bool was_enabled, bool was_connected, bool was_managed);
/* The UI-thread suspend coordinator supplies a progress callback to maintain
 * its supervisor heartbeat during bounded reconnect waits after resume. */
c1_status c1_wifi_resume_ex(bool was_enabled, bool was_connected, bool was_managed,
                            const c1_wifi_operation_options *options);
void c1_wifi_adopt_snapshot(const c1_wifi_snapshot *snapshot);

#endif
