#define _DEFAULT_SOURCE 1

#include "hal/linux/power.h"

#include "hal/linux/system_state.h"
#include "platform/app_lease.h"
#include "platform/liveness.h"
#include "platform/power_config.h"
#include "services/wifi.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define C1_POWER_STATE_PATH "/sys/power/state"
#define C1_POWER_WAKEUP_COUNT_PATH "/sys/power/wakeup_count"
#define C1_POWER_RESUME_ATTEMPTS 3U
#define C1_POWER_RESUME_RETRY_NS 250000000L

static int64_t monotonic_milliseconds(void)
{
    struct timespec value;

    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) {
        return -1;
    }
    return (int64_t)value.tv_sec * 1000 + value.tv_nsec / 1000000;
}

static bool file_contains_word(const char *path, const char *word)
{
    FILE *stream = fopen(path, "r");
    char line[256];
    bool found = false;

    if (stream == NULL) {
        return false;
    }
    if (fgets(line, sizeof(line), stream) != NULL) {
        char *token;
        char *save = NULL;

        for (token = strtok_r(line, " \t\r\n", &save);
             token != NULL;
             token = strtok_r(NULL, " \t\r\n", &save)) {
            if (strcmp(token, word) == 0) {
                found = true;
                break;
            }
        }
    }
    fclose(stream);
    return found;
}

bool c1_linux_power_available(void)
{
    return c1_power_config_auto_suspend_enabled() &&
           access(C1_POWER_STATE_PATH, W_OK) == 0 &&
           file_contains_word(C1_POWER_STATE_PATH, "mem");
}

static bool external_power_offline(void)
{
    bool online = false;

    return c1_linux_external_power_read(&online) && !online;
}

static bool resume_wifi_progress(c1_wifi_phase phase, void *context)
{
    (void)phase;
    (void)context;
    c1_liveness_beat(monotonic_milliseconds());
    return true;
}

static c1_status resume_services(c1_linux_power_context *context,
                                 c1_terminal_session *terminal)
{
    c1_status result = context->terminal_restore_failed
                           ? C1_STATUS_IO_ERROR
                           : C1_STATUS_OK;

    if (!c1_usb_power_resume(&context->usb, &c1_usb_power_default_paths,
                             &c1_usb_power_default_ops)) {
        result = C1_STATUS_IO_ERROR;
    }
    if (context->wifi_paused) {
        const c1_wifi_operation_options options = {resume_wifi_progress, NULL};
        if (c1_wifi_resume_ex(context->wifi_enabled,
                              context->wifi_connected,
                              context->wifi_managed, &options) != C1_STATUS_OK) {
            result = C1_STATUS_IO_ERROR;
        } else {
            context->wifi_paused = false;
        }
    }
    if (context->terminal_paused) {
        if (c1_terminal_resume(terminal, context->terminal_running) != C1_STATUS_OK) {
            context->terminal_restore_failed = true;
            result = C1_STATUS_IO_ERROR;
        }
        context->terminal_paused = false;
    }
    return result;
}

c1_status c1_linux_power_prepare(c1_linux_power_context *context,
                                 c1_terminal_session *terminal)
{
    c1_status status;

    if (context == NULL || terminal == NULL) {
        return C1_STATUS_INVALID_ARGUMENT;
    }
    if (!c1_linux_power_available()) {
        return C1_STATUS_UNAVAILABLE;
    }
    if (c1_app_lease_active()) {
        return C1_STATUS_INTERRUPTED;
    }
    if (!external_power_offline()) {
        return C1_STATUS_INTERRUPTED;
    }
    memset(context, 0, sizeof(*context));
    status = c1_terminal_suspend(terminal, &context->terminal_running);
    if (status != C1_STATUS_OK) {
        return status;
    }
    context->terminal_paused = context->terminal_running;

    status = c1_wifi_pause(&context->wifi_enabled,
                           &context->wifi_connected,
                           &context->wifi_managed);
    context->wifi_paused = context->wifi_enabled;
    if (status != C1_STATUS_OK) {
        c1_linux_power_rollback(context, terminal);
        return status;
    }

    if (!c1_usb_power_prepare(&context->usb, &c1_usb_power_default_paths,
                              &c1_usb_power_default_ops)) {
        c1_linux_power_rollback(context, terminal);
        return C1_STATUS_IO_ERROR;
    }
    return C1_STATUS_OK;
}

static bool copy_power_value(const char *path, char *buffer, size_t capacity)
{
    int descriptor;
    ssize_t count;

    descriptor = open(path, O_RDONLY | O_CLOEXEC);
    if (descriptor < 0) {
        return false;
    }
    count = read(descriptor, buffer, capacity - 1U);
    close(descriptor);
    if (count <= 0) {
        return false;
    }
    while (count > 0 && (buffer[count - 1] == '\n' || buffer[count - 1] == '\r')) {
        --count;
    }
    buffer[count] = '\0';
    return count > 0;
}

static bool write_power_value(const char *path, const char *value)
{
    int descriptor = open(path, O_WRONLY | O_CLOEXEC);
    size_t length = strlen(value);
    ssize_t count;
    int saved_error;

    if (descriptor < 0) {
        return false;
    }
    do {
        count = write(descriptor, value, length);
    } while (count < 0 && errno == EINTR);
    saved_error = count < 0 ? errno : (count == (ssize_t)length ? 0 : EIO);
    if (close(descriptor) != 0 && saved_error == 0) {
        saved_error = errno;
    }
    if (saved_error != 0) {
        errno = saved_error;
        return false;
    }
    return true;
}

c1_status c1_linux_power_suspend(c1_record_sink sink)
{
    char wakeup_count[64];
    bool used_wakeup_count = false;
    bool suspended;
    int error_number;
    c1_record record;

    if (!c1_linux_power_available()) {
        return C1_STATUS_UNAVAILABLE;
    }
    if (c1_app_lease_active()) {
        return C1_STATUS_INTERRUPTED;
    }
    if (!external_power_offline()) {
        return C1_STATUS_INTERRUPTED;
    }
    if (copy_power_value(C1_POWER_WAKEUP_COUNT_PATH, wakeup_count, sizeof(wakeup_count))) {
        if (!write_power_value(C1_POWER_WAKEUP_COUNT_PATH, wakeup_count)) {
            return C1_STATUS_IO_ERROR;
        }
        used_wakeup_count = true;
    }
    errno = 0;
    suspended = write_power_value(C1_POWER_STATE_PATH, "mem");
    error_number = suspended ? 0 : (errno != 0 ? errno : EIO);

    c1_record_init(&record, "power", "suspend");
    if (c1_record_add_boolean(&record, "wakeup_count", used_wakeup_count) != C1_STATUS_OK ||
        c1_record_add_boolean(&record, "resumed", suspended) != C1_STATUS_OK ||
        c1_record_add_integer(&record, "errno", error_number) != C1_STATUS_OK ||
        c1_record_emit(sink, &record) != C1_STATUS_OK) {
        return C1_STATUS_IO_ERROR;
    }
    return suspended ? C1_STATUS_OK : C1_STATUS_IO_ERROR;
}

c1_status c1_linux_power_resume(c1_linux_power_context *context,
                                c1_terminal_session *terminal)
{
    unsigned int attempt;
    c1_status status = C1_STATUS_IO_ERROR;

    if (context == NULL || terminal == NULL) {
        return C1_STATUS_INVALID_ARGUMENT;
    }
    for (attempt = 0U; attempt < C1_POWER_RESUME_ATTEMPTS; ++attempt) {
        c1_liveness_beat(monotonic_milliseconds());
        status = resume_services(context, terminal);
        if (status == C1_STATUS_OK) {
            return C1_STATUS_OK;
        }
        if (attempt + 1U < C1_POWER_RESUME_ATTEMPTS) {
            struct timespec pause = {0, C1_POWER_RESUME_RETRY_NS};

            (void)nanosleep(&pause, NULL);
        }
    }
    return status;
}

void c1_linux_power_rollback(c1_linux_power_context *context,
                             c1_terminal_session *terminal)
{
    if (context != NULL && terminal != NULL) {
        (void)c1_linux_power_resume(context, terminal);
    }
}