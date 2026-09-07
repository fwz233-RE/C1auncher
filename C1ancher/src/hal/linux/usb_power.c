#define _DEFAULT_SOURCE 1

#include "hal/linux/usb_power.h"
#include "platform/liveness.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <stdlib.h>

#define USB_POLL_MS 50U
#define USB_READY_POLLS 100U
#define USB_COMMAND_POLLS 200U

const c1_usb_power_paths c1_usb_power_default_paths = {
    "/sys/kernel/config/usb_gadget/demo/UDC",
    "/sys/kernel/config/usb_gadget/demo/configs/c.1/ffs.adb",
    "/sys/kernel/config/usb_gadget/demo/configs/c.1/ffs.mtp",
    {"/dev/usb-ffs/adb/ep1", "/dev/usb-ffs/adb/ep2"},
    {"/dev/ffs-mtp/ep1", "/dev/ffs-mtp/ep2", "/dev/ffs-mtp/ep3"},
    "/etc/init.d/usb/adb", "/etc/init.d/usb/mtp"
};

static int linux_exists(void *data, const char *path)
{
    struct stat value;
    (void)data;
    if (lstat(path, &value) == 0) return 1;
    return errno == ENOENT ? 0 : -1;
}

static bool linux_read_udc(void *data, const char *path, char *value, size_t capacity)
{
    int fd;
    ssize_t count;
    (void)data;
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    do {
        count = read(fd, value, capacity - 1U);
    } while (count < 0 && errno == EINTR);
    (void)close(fd);
    if (count < 0 || (size_t)count == capacity - 1U) return false;
    while (count > 0 && (value[count - 1] == '\n' || value[count - 1] == '\r')) --count;
    value[count] = '\0';
    return true; /* Empty is a legitimate, originally unbound gadget. */
}

static bool linux_write_udc(void *data, const char *path, const char *value)
{
    int fd;
    ssize_t count;
    size_t length;
    bool ok;
    (void)data;
    /* A zero-byte write cannot unbind configfs. */
    if (*value == '\0') value = "\n";
    length = strlen(value);
    fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0) return false;
    do {
        count = write(fd, value, length);
    } while (count < 0 && errno == EINTR);
    ok = count == (ssize_t)length;
    if (close(fd) != 0) ok = false;
    return ok;
}

static void linux_pause(void *data, unsigned int milliseconds)
{
    struct timespec delay = {milliseconds / 1000U,
                             (long)(milliseconds % 1000U) * 1000000L};
    (void)data;
    /* No EINTR retry: signal storms must not make a bounded poll unbounded. */
    (void)nanosleep(&delay, NULL);
}

static void linux_heartbeat(void *data)
{
    struct timespec now;
    (void)data;
    if (clock_gettime(CLOCK_MONOTONIC, &now) == 0)
        c1_liveness_beat((int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000);
}

static bool linux_run(void *data, const char *script, const char *action)
{
    pid_t child = fork();
    unsigned int poll;
    (void)data;
    if (child < 0) return false;
    if (child == 0) {
        int null_fd = open("/dev/null", O_RDWR | O_CLOEXEC);
        if (null_fd >= 0) {
            (void)dup2(null_fd, STDIN_FILENO);
            (void)dup2(null_fd, STDOUT_FILENO);
            (void)dup2(null_fd, STDERR_FILENO);
            if (null_fd > STDERR_FILENO) close(null_fd);
        }
        (void)setenv("CONFIGFS_HOME", "/sys/kernel/config", 1);
        execl(script, script, action, (char *)NULL);
        _exit(127);
    }
    for (poll = 0; poll < USB_COMMAND_POLLS; ++poll) {
        int status;
        pid_t result;
        linux_heartbeat(NULL);
        result = waitpid(child, &status, WNOHANG);
        if (result == child) return WIFEXITED(status) && WEXITSTATUS(status) == 0;
        if (result < 0 && errno != EINTR) return false;
        linux_pause(NULL, USB_POLL_MS);
    }
    (void)kill(child, SIGKILL);
    /* Never block in a final waitpid, even on an uninterruptible helper. */
    for (poll = 0; poll < 10U; ++poll) {
        pid_t result = waitpid(child, NULL, WNOHANG);
        if (result == child || (result < 0 && errno != EINTR)) break;
        linux_heartbeat(NULL);
        linux_pause(NULL, USB_POLL_MS);
    }
    return false;
}

const c1_usb_power_ops c1_usb_power_default_ops = {
    NULL, linux_exists, linux_read_udc, linux_write_udc,
    linux_run, linux_pause, linux_heartbeat
};

static bool valid(const c1_usb_power_context *context,
                  const c1_usb_power_paths *paths, const c1_usb_power_ops *ops)
{
    return context != NULL && paths != NULL && ops != NULL &&
           ops->exists != NULL && ops->read_udc != NULL && ops->write_udc != NULL &&
           ops->run != NULL && ops->pause_ms != NULL && ops->heartbeat != NULL;
}

bool c1_usb_power_prepare(c1_usb_power_context *context,
                          const c1_usb_power_paths *paths, const c1_usb_power_ops *ops)
{
    int adb, mtp, udc;
    if (!valid(context, paths, ops) || context->restore_pending) return false;
    memset(context, 0, sizeof(*context));
    adb = ops->exists(ops->data, paths->adb_link);
    mtp = ops->exists(ops->data, paths->mtp_link);
    udc = ops->exists(ops->data, paths->udc);
    if (adb < 0 || mtp < 0 || udc < 0) return false;
    /* A system with no gadget needs no USB restoration. */
    if (udc == 0) return adb == 0 && mtp == 0;
    if (!ops->read_udc(ops->data, paths->udc, context->udc, sizeof(context->udc)))
        return false;
    context->adb_enabled = adb != 0;
    context->mtp_enabled = mtp != 0;
    context->captured = true;
    context->restore_pending = context->adb_enabled || context->mtp_enabled;
    /* Save UDC and both flags BEFORE stop can implicitly unbind the gadget. */
    ops->heartbeat(ops->data);
    return !context->adb_enabled || ops->run(ops->data, paths->adb_script, "stop");
}

static bool function_ready(const c1_usb_power_ops *ops, const char *link,
                           const char *const *endpoints, size_t count)
{
    size_t index;
    if (ops->exists(ops->data, link) != 1) return false;
    for (index = 0; index < count; ++index)
        if (ops->exists(ops->data, endpoints[index]) != 1) return false;
    return true;
}

static bool functions_ready(const c1_usb_power_context *context,
                            const c1_usb_power_paths *paths, const c1_usb_power_ops *ops)
{
    /* Also reject unexpectedly enabled functions, without changing user config. */
    bool adb = context->adb_enabled
                   ? function_ready(ops, paths->adb_link, paths->adb_endpoints, 2U)
                   : ops->exists(ops->data, paths->adb_link) == 0;
    bool mtp = context->mtp_enabled
                   ? function_ready(ops, paths->mtp_link, paths->mtp_endpoints, 3U)
                   : ops->exists(ops->data, paths->mtp_link) == 0;
    return adb && mtp;
}

bool c1_usb_power_resume(c1_usb_power_context *context,
                         const c1_usb_power_paths *paths, const c1_usb_power_ops *ops)
{
    unsigned int poll;
    bool scripts_ok = true;
    char current[C1_USB_POWER_UDC_CAPACITY];
    if (!valid(context, paths, ops)) return false;
    if (!context->captured || !context->restore_pending) return true;
    ops->heartbeat(ops->data);
    if (context->adb_enabled && !context->adb_started) {
        /* A failed script may have launched adbd before its last command failed.
         * On retry, accept verified descriptors instead of spawning another adbd. */
        if (context->adb_start_attempted &&
            function_ready(ops, paths->adb_link, paths->adb_endpoints, 2U)) {
            context->adb_started = true;
        } else {
            context->adb_start_attempted = true;
            context->adb_started = ops->run(ops->data, paths->adb_script, "start");
            if (!context->adb_started) scripts_ok = false;
        }
    }
    if (context->mtp_enabled && !context->mtp_started) {
        /* Call the function script directly: S90usb backgrounds this action and
         * loses its exit status. Never restart an intentionally disabled MTP. */
        context->mtp_started = ops->run(ops->data, paths->mtp_script, "suspendoff");
        if (!context->mtp_started) scripts_ok = false;
    }
    if (!scripts_ok) return false;
    for (poll = 0; poll <= USB_READY_POLLS; ++poll) {
        ops->heartbeat(ops->data);
        if (functions_ready(context, paths, ops)) break;
        if (poll == USB_READY_POLLS) return false;
        ops->pause_ms(ops->data, USB_POLL_MS);
    }
    if (!ops->read_udc(ops->data, paths->udc, current, sizeof(current))) return false;
    /* Restore only the binding lost while stopping ADB. Do not force a
     * disconnect/reconnect when already bound: the user may replug the cable
     * if the host needs to enumerate again. Local readiness is not evidence
     * that the host has reconnected. Never override another controller. */
    if (current[0] != '\0' && strcmp(current, context->udc) != 0) return false;
    if (current[0] == '\0' && context->udc[0] != '\0') {
        if (!ops->write_udc(ops->data, paths->udc, context->udc)) return false;
        ops->pause_ms(ops->data, 100U);
        ops->heartbeat(ops->data);
    }
    if (!ops->read_udc(ops->data, paths->udc, current, sizeof(current)) ||
        strcmp(current, context->udc) != 0 || !functions_ready(context, paths, ops)) return false;
    context->restore_pending = false;
    return true;
}
