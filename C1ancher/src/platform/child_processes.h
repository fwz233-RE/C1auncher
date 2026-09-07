#ifndef C1_PLATFORM_CHILD_PROCESSES_H
#define C1_PLATFORM_CHILD_PROCESSES_H

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* For single-threaded subreapers with no competing waitpid caller. The target
 * kernel omits /proc/PID/task/TID/children (CONFIG_CHECKPOINT_RESTORE), so use
 * /proc only to enumerate candidate PIDs. waitpid, not a racy PPid snapshot,
 * proves ownership before any signal. A live direct child cannot have its PID
 * reused until this caller reaps it. Never reap or signal the excluded primary.
 * Return 1 after finding/reaping a child so newly adopted descendants get another
 * scan, 0 after a clean scan, and -1 on an enumeration/wait/signal error. */
static inline int c1_child_processes_signal(pid_t primary, int signal_number)
{
    DIR *directory = opendir("/proc");
    struct dirent *entry;
    int remaining = 0;
    if (directory == NULL) return -1;
    for (;;) {
        char *end;
        long value;
        pid_t child, waited;
        errno = 0;
        entry = readdir(directory);
        if (entry == NULL) {
            if (errno != 0) remaining = -1;
            break;
        }
        if (entry->d_name[0] < '1' || entry->d_name[0] > '9') continue;
        errno = 0;
        value = strtol(entry->d_name, &end, 10);
        if (errno != 0 || *end != '\0' || value <= 0 || value > INT_MAX) continue;
        child = (pid_t)value;
        if (child == primary) continue;
        do { waited = waitpid(child, NULL, WNOHANG); } while (waited < 0 && errno == EINTR);
        if (waited == child) {
            remaining = 1;
        } else if (waited == 0) {
            remaining = 1;
            if (kill(child, signal_number) != 0 && errno != ESRCH) {
                remaining = -1;
                break;
            }
        } else if (errno != ECHILD && errno != ESRCH) {
            remaining = -1;
            break;
        }
    }
    if (closedir(directory) != 0) remaining = -1;
    return remaining;
}

#endif
