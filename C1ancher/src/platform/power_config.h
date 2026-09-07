#ifndef C1_PLATFORM_POWER_CONFIG_H
#define C1_PLATFORM_POWER_CONFIG_H

#include <stdbool.h>
#include <stddef.h>

#define C1_POWER_CONFIG_DIR "/usr/data/c1"
#define C1_POWER_DISABLE_PATH C1_POWER_CONFIG_DIR "/disable-auto-suspend"

bool c1_power_config_auto_suspend_enabled(void);
int c1_power_config_set_auto_suspend(bool enabled,
                                      char *error,
                                      size_t error_size);

#endif