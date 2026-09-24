#define _DEFAULT_SOURCE 1
#include "services/time_sync.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/timex.h>
#include <sys/wait.h>
#include <unistd.h>

#define C1_TIME_RETRY_MS 300000
#define C1_TIME_OBSERVE_MS 5000
#define C1_TIME_PROBE_BYTES 32768U

enum { SERVICE_UNKNOWN = -1, SERVICE_NONE, SERVICE_SYSTEM };

static int64_t after_ms(int64_t now, int64_t delay)
{
    return now > INT64_MAX - delay ? INT64_MAX : now + delay;
}

void c1_time_sync_init(c1_time_sync *sync)
{
    if (sync) *sync = (c1_time_sync){.pid = -1, .probe_fd = -1,
        .system_service = SERVICE_UNKNOWN, .busybox_path = "/bin/busybox"};
}

static bool kernel_synchronized(void)
{
    struct timex value = {0};
    /* modes=0 is strictly read-only: no clock discipline, status or RTC writes. */
    int result = adjtimex(&value);
    return result >= TIME_OK && result < TIME_ERROR &&
           !(value.status & (STA_UNSYNC | STA_CLOCKERR));
}

static bool clock_name(const char *name)
{
    const char *base = strrchr(name, '/');
    if (base) name = base + 1;
    return strcmp(name, "ntpd") == 0 || strcmp(name, "chronyd") == 0 ||
           strcmp(name, "systemd-timesyncd") == 0 || strcmp(name, "systemd-timesyn") == 0;
}

static ssize_t proc_read(int directory, const char *name, char *buffer, size_t size)
{
    int fd = openat(directory, name, O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return -1;
    struct stat metadata;
    ssize_t length = -1;
    if (fstat(fd, &metadata) == 0 && S_ISREG(metadata.st_mode)) {
        length = read(fd, buffer, size - 1);
        if (length >= 0) buffer[length] = '\0';
    } else {
        errno = EINVAL;
    }
    int error = errno;
    close(fd);
    errno = error;
    return length;
}

static int system_service(pid_t owned_child)
{
    /* Reserve the known system installation even if its process is between
     * restarts. Never execute this binary, guess its options, or read a PID file
     * as authority to signal it. /sbin covers the other conventional ntpd path. */
    static const char *paths[] = {"/usr/sbin/ntpd", "/sbin/ntpd"};
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); ++i) {
        if (access(paths[i], F_OK) == 0) return SERVICE_SYSTEM;
        if (errno != ENOENT && errno != ENOTDIR) return SERVICE_UNKNOWN;
    }
    DIR *directory = opendir("/proc");
    if (!directory) return SERVICE_UNKNOWN;
    unsigned scanned = 0, inspected = 0;
    int result = SERVICE_NONE;
    struct dirent *entry;
    for (;;) {
        errno = 0;
        entry = readdir(directory);
        if (!entry) {
            if (errno) result = SERVICE_UNKNOWN;
            break;
        }
        if (++scanned > 8192) { result = SERVICE_UNKNOWN; break; }
        size_t digits = strspn(entry->d_name, "0123456789");
        if (!digits || entry->d_name[digits] || entry->d_name[0] == '0') continue;
        if (++inspected > 2048) { result = SERVICE_UNKNOWN; break; }
        char *end;
        long pid = strtol(entry->d_name, &end, 10);
        if (*end || pid <= 0 || pid == (long)getpid() || pid == (long)owned_child) continue;
        int task = openat(dirfd(directory), entry->d_name,
                          O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (task < 0) {
            if (errno == ENOENT || errno == ESRCH) continue;
            result = SERVICE_UNKNOWN; break;
        }
        char name[64], command[512];
        ssize_t n = proc_read(task, "comm", name, sizeof(name));
        if (n > 0) {
            if (name[n - 1] == '\n') name[n - 1] = '\0';
            if (clock_name(name)) { close(task); result = SERVICE_SYSTEM; break; }
        } else if (n < 0 && errno != ENOENT && errno != ESRCH) {
            close(task); result = SERVICE_UNKNOWN; break;
        }
        n = proc_read(task, "cmdline", command, sizeof(command));
        int error = errno;
        close(task);
        if (n < 0) {
            if (error == ENOENT || error == ESRCH) continue;
            result = SERVICE_UNKNOWN; break;
        }
        if (!n) continue; /* Kernel thread or a task which is exiting. */
        char *first_end = memchr(command, '\0', (size_t)n);
        if (!first_end) { result = SERVICE_UNKNOWN; break; }
        if (clock_name(command)) { result = SERVICE_SYSTEM; break; }
        const char *base = strrchr(command, '/');
        base = base ? base + 1 : command;
        if (strcmp(base, "busybox") == 0) {
            char *applet = first_end + 1;
            size_t remaining = (size_t)n - (size_t)(applet - command);
            if (!remaining || !memchr(applet, '\0', remaining)) {
                result = SERVICE_UNKNOWN; break;
            }
            if (strcmp(applet, "ntpd") == 0) { result = SERVICE_SYSTEM; break; }
        }
    }
    closedir(directory);
    return result;
}

static void close_probe(c1_time_sync *sync)
{
    if (sync->probe_fd >= 0) close(sync->probe_fd);
    sync->probe_fd = -1;
}

static void read_probe(c1_time_sync *sync)
{
    if (sync->probe_fd < 0 || sync->probe_invalid) return;
    char buffer[512];
    /* A malicious/broken executable cannot keep a UI tick in an endless read. */
    for (size_t budget = 0; budget < C1_TIME_PROBE_BYTES; budget += sizeof(buffer)) {
        ssize_t size = read(sync->probe_fd, buffer, sizeof(buffer));
        if (!size) { sync->probe_eof = true; return; }
        if (size < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) sync->probe_invalid = true;
            return;
        }
        sync->probe_bytes += (size_t)size;
        if (sync->probe_bytes > C1_TIME_PROBE_BYTES) { sync->probe_invalid = true; return; }
        for (ssize_t i = 0; i < size; ++i) {
            if (buffer[i] == '\n') {
                if (sync->probe_line_length == 4 && memcmp(sync->probe_line, "ntpd", 4) == 0)
                    sync->probe_found = true;
                sync->probe_line_length = 0;
            } else {
                if (buffer[i] == '\0') sync->probe_invalid = true;
                if (sync->probe_line_length < sizeof(sync->probe_line))
                    sync->probe_line[sync->probe_line_length++] = buffer[i];
            }
        }
    }
}

static void cancel_child(c1_time_sync *sync)
{
    if (sync->pid <= 0) return;
    /* WNOHANG==0 is the only evidence that this is still our direct child.
     * Never signal on ECHILD/EINTR, a stale PID, a PID file, or a process group. */
    pid_t owned = waitpid(sync->pid, NULL, WNOHANG);
    if (owned == 0 && !sync->kill_sent) {
        if (kill(sync->pid, SIGKILL) == 0) sync->kill_sent = true;
    } else if (owned == sync->pid || (owned < 0 && errno == ECHILD)) {
        sync->pid = -1;
        close_probe(sync);
    }
}

static void request_stop(c1_time_sync *sync)
{
    sync->cancelled = true;
    cancel_child(sync);
    sync->state = C1_TIME_UNCHECKED;
    sync->previous_enabled = false;
}

void c1_time_sync_stop(c1_time_sync *sync)
{
    if (!sync) return;
    request_stop(sync);
    /* The existing shutdown caller calls stop once. Give a killed child a short,
     * bounded chance to be reaped without a blocking waitpid (e.g. D-state I/O).
     * Heartbeat cancellation below does not use this shutdown-only grace period. */
    for (unsigned attempt = 0; attempt < 20 && sync->pid > 0; ++attempt) {
        (void)poll(NULL, 0, 5);
        cancel_child(sync);
    }
}

static void child_fds(int output)
{
    /* Duplicate output before opening /dev/null, also when stdio was closed. */
    if (output >= 0 && (dup2(output, STDOUT_FILENO) < 0 ||
                        fcntl(STDOUT_FILENO, F_SETFD, 0) < 0)) _exit(126);
    int null = open("/dev/null", O_RDWR);
    if (null < 0) _exit(126);
    if (dup2(null, STDIN_FILENO) < 0 || dup2(null, STDERR_FILENO) < 0 ||
        (output < 0 && dup2(null, STDOUT_FILENO) < 0)) _exit(126);
    DIR *directory = opendir("/proc/self/fd");
    if (!directory) _exit(126);
    struct dirent *entry;
    while ((entry = readdir(directory))) {
        char *end;
        long fd = strtol(entry->d_name, &end, 10);
        if (!*end && fd > 2 && fd != dirfd(directory)) close((int)fd);
    }
    closedir(directory);
}

static bool start_child(c1_time_sync *sync, int64_t now)
{
    int channel[2] = {-1, -1};
    bool probe = !sync->applet_available;
    if (probe) {
        if (pipe(channel) != 0) return false;
        if (fcntl(channel[0], F_SETFL, O_NONBLOCK) < 0 ||
            fcntl(channel[0], F_SETFD, FD_CLOEXEC) < 0 ||
            fcntl(channel[1], F_SETFD, FD_CLOEXEC) < 0) {
            close(channel[0]); close(channel[1]); return false;
        }
    }
    pid_t parent = getpid();
    pid_t pid = fork();
    if (pid == 0) {
        struct sigaction action = {0};
        sigset_t empty;
        action.sa_handler = SIG_DFL;
        sigemptyset(&action.sa_mask);
        sigemptyset(&empty);
        if (sigaction(SIGTERM, &action, NULL) != 0 || sigaction(SIGINT, &action, NULL) != 0 ||
            sigprocmask(SIG_SETMASK, &empty, NULL) != 0 ||
            prctl(PR_SET_PDEATHSIG, SIGKILL) != 0 || getppid() != parent) _exit(126);
        if (!probe && system_service(-1) != SERVICE_NONE) _exit(125);
        child_fds(channel[1]);
        if (probe) execl(sync->busybox_path, "busybox", "--list", (char *)NULL);
        else execl(sync->busybox_path, "busybox", "ntpd", "-n", "-q", "-p",
                   "time.cloudflare.com", (char *)NULL);
        _exit(127);
    }
    if (channel[1] >= 0) close(channel[1]);
    if (pid < 0) { if (channel[0] >= 0) close(channel[0]); return false; }
    sync->pid = pid;
    sync->probe_fd = channel[0];
    sync->probing = probe;
    sync->probe_found = sync->probe_eof = sync->probe_invalid = false;
    sync->probe_bytes = sync->probe_line_length = 0;
    sync->timed_out = sync->cancelled = sync->kill_sent = false;
    sync->state = C1_TIME_RUNNING;
    sync->deadline_ms = after_ms(now, probe ? 3000 : 45000);
    return true;
}

void c1_time_sync_tick(c1_time_sync *sync, bool enabled, bool online, int64_t now)
{
    if (!sync || now < 0) return;
    bool resumed = online && enabled && (!sync->previous_online || !sync->previous_enabled);
    sync->previous_online = online;
    sync->previous_enabled = enabled;
    int result = 0;
    bool finished = false, exited = false;
    if (sync->pid > 0) {
        read_probe(sync);
        pid_t done = waitpid(sync->pid, &result, WNOHANG);
        if (done == sync->pid || (done < 0 && errno == ECHILD)) {
            /* Drain bytes written between the initial read and child exit. */
            read_probe(sync);
            finished = true;
            exited = done > 0 && WIFEXITED(result);
            sync->pid = -1;
            if (now >= sync->deadline_ms) sync->timed_out = true;
            close_probe(sync);
        }
    }
    if (!enabled || !online) {
        request_stop(sync);
        return;
    }
    bool observed = resumed || finished || now >= sync->next_observation_ms;
    if (observed) {
        sync->system_service = system_service(sync->pid);
        sync->kernel_synced = kernel_synchronized();
        sync->next_observation_ms = after_ms(now, C1_TIME_OBSERVE_MS);
    }
    if (sync->system_service != SERVICE_NONE || sync->kernel_synced) {
        sync->cancelled = true;
        cancel_child(sync);
        sync->state = sync->kernel_synced ? C1_TIME_SYNCED :
            sync->system_service == SERVICE_SYSTEM ? C1_TIME_WAITING_SYSTEM : C1_TIME_BLOCKED;
        /* Continue read-only observation, never take over a known system daemon. */
        return;
    }
    if (observed && (sync->state == C1_TIME_SYNCED || sync->state == C1_TIME_WAITING_SYSTEM ||
                     sync->state == C1_TIME_BLOCKED)) sync->state = C1_TIME_UNVERIFIED;
    if (resumed && sync->pid <= 0) sync->next_attempt_ms = now;
    if (finished && !sync->cancelled) {
        sync->next_attempt_ms = after_ms(now, C1_TIME_RETRY_MS);
        if (sync->timed_out || !exited || WEXITSTATUS(result) != 0) {
            sync->state = exited && !sync->timed_out && WEXITSTATUS(result) == 127
                ? C1_TIME_UNAVAILABLE : C1_TIME_FAILED;
            if (exited && WEXITSTATUS(result) == 125) sync->state = C1_TIME_BLOCKED;
            if (!sync->probing) sync->applet_available = false;
        } else if (sync->probing) {
            if (!sync->probe_eof || sync->probe_invalid || sync->probe_line_length) {
                sync->state = C1_TIME_FAILED;
            } else if (!sync->probe_found) {
                sync->state = C1_TIME_UNAVAILABLE;
            } else {
                sync->applet_available = true;
                sync->next_attempt_ms = now;
            }
        } else {
            sync->state = C1_TIME_UNVERIFIED;
        }
    }
    if (sync->pid > 0) {
        if (now >= sync->deadline_ms || sync->probe_invalid) {
            sync->timed_out = true;
            cancel_child(sync);
            sync->state = C1_TIME_FAILED;
            sync->next_attempt_ms = after_ms(now, C1_TIME_RETRY_MS);
        }
        return;
    }
    if (now < sync->next_attempt_ms) return;
    if (!sync->busybox_path || access(sync->busybox_path, X_OK) != 0) {
        sync->state = C1_TIME_UNAVAILABLE;
        sync->next_attempt_ms = after_ms(now, C1_TIME_RETRY_MS);
    } else if (!start_child(sync, now)) {
        sync->state = C1_TIME_FAILED;
        sync->next_attempt_ms = after_ms(now, C1_TIME_RETRY_MS);
    }
}
