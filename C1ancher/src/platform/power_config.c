#define _DEFAULT_SOURCE 1

#include "platform/power_config.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void set_error(char *error, size_t error_size, const char *message)
{
    if (error != NULL && error_size > 0U) {
        (void)snprintf(error, error_size, "%s: %s", message, strerror(errno));
    }
}

bool c1_power_config_auto_suspend_enabled(void)
{
    struct stat marker;

    if (access(C1_POWER_CONFIG_DIR, R_OK | X_OK) != 0) {
        return false;
    }
    if (lstat(C1_POWER_DISABLE_PATH, &marker) == 0) {
        return false;
    }
    return errno == ENOENT;
}

int c1_power_config_set_auto_suspend(bool enabled, char *error, size_t error_size)
{
    int descriptor;

    if (enabled) {
        if (unlink(C1_POWER_DISABLE_PATH) != 0 && errno != ENOENT) {
            set_error(error, error_size, "enable automatic suspend");
            return -1;
        }
        return 0;
    }
    descriptor = open(C1_POWER_DISABLE_PATH, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (descriptor >= 0) {
        if (close(descriptor) != 0) {
            set_error(error, error_size, "disable automatic suspend");
            return -1;
        }
        return 0;
    }
    if (errno == EEXIST) {
        return 0;
    }
    set_error(error, error_size, "disable automatic suspend");
    return -1;
}