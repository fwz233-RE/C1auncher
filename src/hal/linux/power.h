#ifndef C1_HAL_LINUX_POWER_H
#define C1_HAL_LINUX_POWER_H

#include "core/record.h"
#include "core/status.h"
#include "services/terminal.h"

#include <stdbool.h>

typedef struct {
    bool terminal_running;
    bool ssh_enabled;
    bool wifi_enabled;
    bool wifi_connected;
    bool wifi_managed;
    bool adb_enabled;
    bool terminal_paused;
    bool ssh_paused;
    bool wifi_paused;
    bool adb_paused;
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