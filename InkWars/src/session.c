/* SPDX-License-Identifier: GPL-3.0-only */
#include "session.h"
#include "platform/app_lease.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdlib.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

static int lease = -1;
static struct termios saved_terminal;
static bool terminal_changed;

/* flock locks belong to open file descriptions and survive exec. Reopening
 * the pathname would conflict with our own inherited lock. Discover only
 * descriptors for the validated lease inode, then retain a private duplicate. */
static int inherited_lease(void) {
    struct stat expected;
    if (lstat(C1_APP_LEASE_PATH, &expected) != 0) return -1;
    if (!S_ISREG(expected.st_mode) || expected.st_nlink != 1 ||
        expected.st_uid != geteuid() || (expected.st_mode & 0777U) != 0600U) {
        errno = EACCES;
        return -1;
    }
    DIR *directory = opendir("/proc/self/fd");
    if (!directory) return -1;
    int found = -1;
    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL) {
        char *end;
        long n = strtol(entry->d_name, &end, 10);
        if (*end || n < 3 || n > INT_MAX || n == dirfd(directory)) continue;
        struct stat actual;
        if (fstat((int)n, &actual) != 0 || actual.st_dev != expected.st_dev ||
            actual.st_ino != expected.st_ino) continue;
        /* Never trust an arbitrary descriptor as proof of exclusive ownership. */
        if (flock((int)n, LOCK_EX | LOCK_NB) != 0) continue;
        found = fcntl((int)n, F_DUPFD_CLOEXEC, 3);
        break;
    }
    int saved = found < 0 ? EWOULDBLOCK : 0;
    closedir(directory);
    errno = saved;
    return found;
}

int iw_session_open(void) {
    if (lease >= 0) { errno = EALREADY; return -1; }
    lease = c1_app_lease_acquire();
    if (lease < 0 && (errno == EWOULDBLOCK || errno == EAGAIN))
        lease = inherited_lease();
    if (lease < 0) return -1;
    /* Unlike c1pkg, Ink Wars does not pass display ownership to child execs. */
    if (fcntl(lease, F_SETFD, FD_CLOEXEC) < 0) goto fail;
    if (isatty(STDIN_FILENO)) {
        struct termios quiet;
        if (tcgetattr(STDIN_FILENO, &saved_terminal) != 0) goto fail;
        quiet = saved_terminal;
        quiet.c_lflag &= (tcflag_t)~(ECHO | ECHONL | ICANON);
        quiet.c_cc[VMIN] = 1;
        quiet.c_cc[VTIME] = 0;
        /* TCSANOW, not TCSAFLUSH: retain queued input; never send terminal
         * escape sequences or application diagnostics to the live display. */
        if (tcsetattr(STDIN_FILENO, TCSANOW, &quiet) != 0) goto fail;
        terminal_changed = true;
    }
    return 0;
fail: {
    int saved = errno;
    iw_session_close();
    errno = saved;
    return -1;
}}

void iw_session_close(void) {
    if (terminal_changed) {
        (void)tcsetattr(STDIN_FILENO, TCSANOW, &saved_terminal);
        terminal_changed = false;
    }
    if (lease >= 0) c1_app_lease_release(lease);
    lease = -1;
    /* Never unlink the lease inode, or another process can bypass flock.
     * Inherited originals remain held until process exit, as c1pkg intends. */
}
