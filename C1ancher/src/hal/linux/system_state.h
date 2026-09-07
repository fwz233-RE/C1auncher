#ifndef C1_HAL_LINUX_SYSTEM_STATE_H
#define C1_HAL_LINUX_SYSTEM_STATE_H

#include "ui/model.h"

#include <stdbool.h>

bool c1_linux_system_status_read(c1_ui_status *status);
bool c1_linux_external_power_read(bool *online);

#endif