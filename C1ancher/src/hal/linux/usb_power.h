#ifndef C1_HAL_LINUX_USB_POWER_H
#define C1_HAL_LINUX_USB_POWER_H

#include <stdbool.h>
#include <stddef.h>

#define C1_USB_POWER_UDC_CAPACITY 128U

typedef struct {
    bool captured;
    bool adb_enabled;
    bool mtp_enabled;
    bool restore_pending;
    bool adb_start_attempted;
    bool adb_started;
    bool mtp_started;
    char udc[C1_USB_POWER_UDC_CAPACITY];
} c1_usb_power_context;

/* Inject all I/O and delays so host tests run the production state machine
 * without accessing sysfs or starting device services. exists: 1/0/-1. */
typedef struct {
    void *data;
    int (*exists)(void *data, const char *path);
    bool (*read_udc)(void *data, const char *path, char *value, size_t capacity);
    bool (*write_udc)(void *data, const char *path, const char *value);
    bool (*run)(void *data, const char *script, const char *action);
    void (*pause_ms)(void *data, unsigned int milliseconds);
    void (*heartbeat)(void *data);
} c1_usb_power_ops;

typedef struct {
    const char *udc;
    const char *adb_link;
    const char *mtp_link;
    const char *adb_endpoints[2];
    const char *mtp_endpoints[3];
    const char *adb_script;
    const char *mtp_script;
} c1_usb_power_paths;

extern const c1_usb_power_paths c1_usb_power_default_paths;
extern const c1_usb_power_ops c1_usb_power_default_ops;

/* Restore the pre-suspend services and a missing binding, without forcing
 * host re-enumeration. Success means device-side readiness only; the user may
 * need to unplug/replug USB after waking. One bounded attempt; failures retain
 * progress for the caller's bounded retry loop. An uncaptured context is a no-op. */
bool c1_usb_power_prepare(c1_usb_power_context *context,
                          const c1_usb_power_paths *paths,
                          const c1_usb_power_ops *ops);
bool c1_usb_power_resume(c1_usb_power_context *context,
                         const c1_usb_power_paths *paths,
                         const c1_usb_power_ops *ops);

#endif
