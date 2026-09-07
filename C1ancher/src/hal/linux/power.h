#ifndef C1_HAL_LINUX_POWER_H
#define C1_HAL_LINUX_POWER_H

#include "core/record.h"
#include "core/status.h"
#include "hal/linux/usb_power.h"
#include "services/terminal.h"

#include <stdbool.h>

typedef struct {
    bool terminal_running;
    bool wifi_enabled;
    bool wifi_connected;
    bool wifi_managed;
    c1_usb_power_context usb;
    bool terminal_paused;
    bool wifi_paused;
    bool terminal_restore_failed;
} c1_linux_power_context;

bool c1_linux_power_available(void);
c1_status c1_linux_power_prepare(c1_linux_power_context *context,
                                 c1_terminal_session *terminal);
c1_status c1_linux_power_suspend(c1_record_sink sink);
c1_status c1_linux_power_resume(c1_linux_power_context *context,
                                c1_terminal_session *terminal);
void c1_linux_power_rollback(c1_linux_power_context *context,
                             c1_terminal_session *terminal);

#endif