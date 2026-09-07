#include "platform/app_lease.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <time.h>
#include <unistd.h>

static int open_lease(const char *path, bool guard)
{
    struct stat information;
    int descriptor, saved;
    if (path == NULL || path[0] == '\0') {
        errno = EINVAL;
        return -1;
    }
    /* The exclusive lease is intentionally inherited by the external app.
     * A short-lived hardware guard must never survive exec. */
    descriptor = open(path, O_RDWR | O_CREAT | O_NOFOLLOW | O_NONBLOCK |
                            (guard ? O_CLOEXEC : 0), 0600);
    if (descriptor < 0) return -1;
    if (fstat(descriptor, &information) != 0) {
        saved = errno;
    } else if (!S_ISREG(information.st_mode) || information.st_nlink != 1 ||
               information.st_uid != geteuid() ||
               (information.st_mode & 0777U) != 0600U) {
        saved = EACCES;
    } else {
        return descriptor;
    }
    (void)close(descriptor);
    errno = saved;
    return -1;
}

int c1_app_lease_acquire_at(const char *path)
{
    int descriptor = open_lease(path, false);

    if (descriptor < 0) {
        return -1;
    }
    if (flock(descriptor, LOCK_EX | LOCK_NB) != 0) {
        int error_number = errno;

        close(descriptor);
        errno = error_number;
        return -1;
    }
    return descriptor;
}

int c1_app_lease_guard_acquire_at(const char *path)
{
    int descriptor = open_lease(path, true);

    if (descriptor < 0) {
        return -1;
    }
    if (flock(descriptor, LOCK_SH | LOCK_NB) != 0) {
        int error_number = errno;

        close(descriptor);
        errno = error_number;
        return -1;
    }
    return descriptor;
}

bool c1_app_lease_active_at(const char *path)
{
    int descriptor = c1_app_lease_guard_acquire_at(path);

    if (descriptor >= 0) {
        c1_app_lease_release(descriptor);
        return false;
    }
    /* Unknown lease state must inhibit display/power access as well. */
    return true;
}

int c1_app_run_acquire_at(const char *path)
{
    return c1_app_lease_acquire_at(path);
}

bool c1_app_run_active_at(const char *path)
{
    return c1_app_lease_active_at(path);
}

static int mode_write_guard(void)
{
    unsigned int attempt;
    int descriptor = open_lease(C1_APP_MODE_GUARD_PATH, true);
    if (descriptor < 0) return -1;
    for (attempt = 0; attempt < 100U; ++attempt) {
        struct timespec delay = {0, 1000000L};
        if (flock(descriptor, LOCK_EX | LOCK_NB) == 0) return descriptor;
        if (errno != EWOULDBLOCK && errno != EAGAIN && errno != EINTR) break;
        (void)nanosleep(&delay, NULL);
    }
    {
        int saved = errno;
        close(descriptor);
        errno = saved;
    }
    return -1;
}

int c1_app_run_acquire(void)
{
    int guard = mode_write_guard();
    int descriptor, saved;
    if (guard < 0) return -1;
    (void)fcntl(guard, F_SETFD, FD_CLOEXEC);
    descriptor = c1_app_run_acquire_at(C1_APP_RUN_PATH);
    saved = errno;
    /* Mode readers hold the same guard, so the new owner can never be
     * observed with the previous owner's terminal permission. */
    if (descriptor >= 0 && unlink(C1_APP_MODE_PATH) != 0 && errno != ENOENT) {
        saved = errno;
        close(descriptor);
        descriptor = -1;
    }
    close(guard);
    errno = saved;
    return descriptor;
}

bool c1_app_run_active(void)
{
    return c1_app_run_active_at(C1_APP_RUN_PATH);
}

int c1_app_lease_acquire(void)
{
    return c1_app_lease_acquire_at(C1_APP_LEASE_PATH);
}

int c1_app_lease_guard_acquire(void)
{
    return c1_app_lease_guard_acquire_at(C1_APP_LEASE_PATH);
}

bool c1_app_lease_active(void)
{
    return c1_app_lease_active_at(C1_APP_LEASE_PATH);
}

void c1_app_lease_release(int descriptor)
{
    if (descriptor >= 0) {
        close(descriptor);
    }
}

bool c1_app_lease_write_mode(const char *mode)
{
    int descriptor, guard;
    bool success = false;
    size_t length;
    if (mode == NULL || (strcmp(mode, "terminal") != 0 && strcmp(mode, "direct") != 0)) {
        errno = EINVAL;
        return false;
    }
    guard = mode_write_guard();
    if (guard < 0) return false;
    descriptor = open_lease(C1_APP_MODE_PATH, true);
    length = strlen(mode);
    if (descriptor >= 0) {
        success = ftruncate(descriptor, 0) == 0 &&
                  write(descriptor, mode, length) == (ssize_t)length;
        if (close(descriptor) != 0) success = false;
    }
    close(guard);
    return success;
}

void c1_app_lease_clear_mode(void)
{
    int guard = mode_write_guard();
    if (guard < 0) return;
    (void)unlink(C1_APP_MODE_PATH);
    close(guard);
}

bool c1_app_lease_terminal_mode(void)
{
    char mode[16];
    int descriptor;
    int guard = c1_app_lease_guard_acquire_at(C1_APP_MODE_GUARD_PATH);
    ssize_t count = -1;
    struct stat information;
    if (guard < 0) return false;
    descriptor = open(C1_APP_MODE_PATH, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (descriptor >= 0) {
        if (fstat(descriptor, &information) == 0 && S_ISREG(information.st_mode) &&
            information.st_nlink == 1 && information.st_uid == geteuid() &&
            (information.st_mode & 0777U) == 0600U)
            count = read(descriptor, mode, sizeof(mode));
        close(descriptor);
    }
    close(guard);
    /* Unknown, partial, oversized or stale publication never grants UI access.
     * The display itself still takes the independent shared hardware lease. */
    return count == 8 && memcmp(mode, "terminal", 8U) == 0;
}
