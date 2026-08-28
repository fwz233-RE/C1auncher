#define _DEFAULT_SOURCE 1

#include "hal/linux/power.h"

#include "services/ssh.h"
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
#define C1_POWER_DISABLE_PATH "/usr/data/c1/disable-auto-suspend"
#define C1_POWER_USB_ADB "/etc/init.d/usb/adb"
#define C1_POWER_USB_MAIN "/etc/init.d/S90usb"
#define C1_POWER_COMMAND_TIMEOUT_MS 10000

static int64_t monotonic_milliseconds(void)
{
    struct timespec value;

    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) {
        return -1;
    }
    return (int64_t)value.tv_sec * 1000 + value.tv_nsec / 1000000;
}

static bool wait_child(pid_t child)
{
    int64_t deadline = monotonic_milliseconds() + C1_POWER_COMMAND_TIMEOUT_MS;

    while (monotonic_milliseconds() < deadline) {
        int status;
        pid_t result = waitpid(child, &status, WNOHANG);

        if (result == child) {
            return WIFEXITED(status) && WEXITSTATUS(status) == 0;
        }
        if (result < 0 && errno != EINTR) {
            return false;
        }
        {
            struct timespec pause = {0, 50000000L};
            (void)nanosleep(&pause, NULL);
        }
    }
    (void)kill(child, SIGKILL);
    while (waitpid(child, NULL, 0) < 0 && errno == EINTR) {
    }
    return false;
}

static bool run_usb(const char *script, const char *action)
{
    pid_t child = fork();

    if (child < 0) {
        return false;
    }
    if (child == 0) {
        int null_fd = open("/dev/null", O_RDWR | O_CLOEXEC);

        if (null_fd >= 0) {
            (void)dup2(null_fd, STDIN_FILENO);
            (void)dup2(null_fd, STDOUT_FILENO);
            (void)dup2(null_fd, STDERR_FILENO);
            if (null_fd > STDERR_FILENO) {
                close(null_fd);
            }
        }
        (void)setenv("CONFIGFS_HOME", "/sys/kernel/config", 1);
        execl(script, script, action, (char *)NULL);
        _exit(127);
    }
    return wait_child(child);
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

static bool adb_is_enabled(void)
{
    return access("/sys/kernel/config/usb_gadget/demo/configs/c.1/ffs.adb", F_OK) == 0;
}

bool c1_linux_power_available(void)
{
    return access(C1_POWER_DISABLE_PATH, F_OK) != 0 &&
           access(C1_POWER_STATE_PATH, W_OK) == 0 &&
           file_contains_word(C1_POWER_STATE_PATH, "mem");
}

static c1_status resume_services(c1_linux_power_context *context,
                                 c1_terminal_session *terminal)
{
    c1_status result = C1_STATUS_OK;

    if (context->adb_paused) {
        if (!run_usb(C1_POWER_USB_ADB, "start")) {
            result = C1_STATUS_IO_ERROR;
        } else {
            context->adb_paused = false;
        }
    }
    if (context->wifi_paused) {
        if (c1_wifi_resume(context->wifi_enabled,
                           context->wifi_connected,
                           context->wifi_managed) != C1_STATUS_OK) {
            result = C1_STATUS_IO_ERROR;
        } else {
            context->wifi_paused = false;
        }
    }
    if (context->ssh_paused) {
        if (c1_ssh_resume(context->ssh_enabled) != C1_STATUS_OK) {
            result = C1_STATUS_IO_ERROR;
        } else {
            context->ssh_paused = false;
        }
    }
    if (context->terminal_paused) {
        if (c1_terminal_resume(terminal, context->terminal_running) != C1_STATUS_OK) {
            result = C1_STATUS_IO_ERROR;
        }
        context->terminal_paused = false;
    }
    if (access(C1_POWER_USB_MAIN, X_OK) == 0) {
        (void)run_usb(C1_POWER_USB_MAIN, "suspendoff");
    }
    return result;
}

c1_status c1_linux_power_prepare(c1_linux_power_context *context,
                                 c1_terminal_session *terminal)
{
    c1_status status;

    if (context == NULL || terminal == NULL || !c1_linux_power_available()) {
        return C1_STATUS_UNAVAILABLE;
    }
    memset(context, 0, sizeof(*context));
    status = c1_terminal_suspend(terminal, &context->terminal_running);
    if (status != C1_STATUS_OK) {
        return status;
    }
    context->terminal_paused = context->terminal_running;

    status = c1_ssh_pause(&context->ssh_enabled);
    context->ssh_paused = context->ssh_enabled;
    if (status != C1_STATUS_OK) {
        c1_linux_power_rollback(context, terminal);
        return status;
    }

    status = c1_wifi_pause(&context->wifi_enabled,
                           &context->wifi_connected,
                           &context->wifi_managed);
    context->wifi_paused = context->wifi_enabled;
    if (status != C1_STATUS_OK) {
        c1_linux_power_rollback(context, terminal);
        return status;
    }

    context->adb_enabled = adb_is_enabled();
    if (context->adb_enabled) {
        context->adb_paused = true;
        if (!run_usb(C1_POWER_USB_ADB, "stop")) {
            c1_linux_power_rollback(context, terminal);
            return C1_STATUS_IO_ERROR;
        }
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
    if (context == NULL || terminal == NULL) {
        return C1_STATUS_INVALID_ARGUMENT;
    }
    return resume_services(context, terminal);
}

void c1_linux_power_rollback(c1_linux_power_context *context,
                             c1_terminal_session *terminal)
{
    if (context != NULL && terminal != NULL) {
        (void)resume_services(context, terminal);
    }
}