#define _DEFAULT_SOURCE 1
/* Standalone host test. Compile this file alone (it injects the implementation):
 * cc -std=c11 -Wall -Wextra -Wpedantic -Werror -Isrc tests/test_time_sync.c -o /tmp/c1-time-tests
 * Only private proc fixtures and our own harmless helper children are used.
 * No real ntpd, network, clock/RTC changes, hardware or external process signals. */
#include "services/time_sync.h"
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/timex.h>
#include <sys/wait.h>
#include <unistd.h>

static char proc_root[256];
static bool installed;
static int installation_error, kernel_result, kernel_status, forced_wait_error;
static bool fork_failure, install_in_child;
static unsigned spawn_count, signal_count, observation_count;
static pid_t last_spawned;

static int test_access(const char *path, int mode)
{
    if (!strcmp(path, "/usr/sbin/ntpd") || !strcmp(path, "/sbin/ntpd")) {
        assert(mode == F_OK);
        if (installed) return 0;
        errno = installation_error ? installation_error : ENOENT;
        return -1;
    }
    return access(path, mode);
}

static DIR *test_opendir(const char *path)
{
    return opendir(!strcmp(path, "/proc") ? proc_root : path);
}

static int test_adjtimex(struct timex *value)
{
    assert(value->modes == 0); /* Production must never request a write. */
    ++observation_count;
    value->status = kernel_status;
    return kernel_result;
}

static pid_t test_fork(void)
{
    if (fork_failure) { errno = EAGAIN; return -1; }
    pid_t result = fork();
    if (result == 0 && install_in_child) installed = true;
    if (result > 0) { last_spawned = result; ++spawn_count; }
    return result;
}

static pid_t test_waitpid(pid_t pid, int *result, int options)
{
    assert(options == WNOHANG); /* Stop must not introduce a blocking waitpid either. */
    if (forced_wait_error) { errno = forced_wait_error; return -1; }
    return waitpid(pid, result, options);
}

static int test_kill(pid_t pid, int signal_number)
{
    assert(pid > 0 && pid == last_spawned && signal_number == SIGKILL);
    ++signal_count;
    return kill(pid, signal_number);
}

#define access test_access
#define opendir test_opendir
#define adjtimex test_adjtimex
#define fork test_fork
#define waitpid test_waitpid
#define kill test_kill
#include "../src/services/time_sync.c"
#undef access
#undef opendir
#undef adjtimex
#undef fork
#undef waitpid
#undef kill

static void write_fixture(const char *path, const char *bytes, size_t size)
{
    FILE *file = fopen(path, "wb");
    assert(file);
    assert(fwrite(bytes, 1, size, file) == size);
    assert(fclose(file) == 0);
}

static void task_fixture(unsigned pid, const char *name, const char *command, size_t size)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%u", proc_root, pid);
    assert(mkdir(path, 0700) == 0);
    snprintf(path, sizeof(path), "%s/%u/comm", proc_root, pid);
    write_fixture(path, name, strlen(name));
    snprintf(path, sizeof(path), "%s/%u/cmdline", proc_root, pid);
    write_fixture(path, command, size);
}

static void remove_task(unsigned pid)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%u/comm", proc_root, pid);
    assert(unlink(path) == 0);
    snprintf(path, sizeof(path), "%s/%u/cmdline", proc_root, pid);
    assert(unlink(path) == 0);
    snprintf(path, sizeof(path), "%s/%u", proc_root, pid);
    assert(rmdir(path) == 0);
}

static c1_time_sync fresh(void)
{
    installed = false;
    installation_error = 0;
    kernel_result = TIME_ERROR;
    kernel_status = STA_UNSYNC;
    forced_wait_error = 0;
    fork_failure = install_in_child = false;
    spawn_count = signal_count = observation_count = 0;
    last_spawned = -1;
    assert(setenv("C1_TEST_TIME_LIST", "cat\nntpd\nsh\n", 1) == 0);
    assert(setenv("C1_TEST_TIME_LIST_EXIT", "0", 1) == 0);
    assert(setenv("C1_TEST_TIME_NTP_EXIT", "0", 1) == 0);
    unsetenv("C1_TEST_TIME_HANG");
    unsetenv("C1_TEST_TIME_FD");
    c1_time_sync sync;
    c1_time_sync_init(&sync);
    sync.busybox_path = "/proc/self/exe";
    return sync;
}

static void finish(c1_time_sync *sync, int64_t now)
{
    for (unsigned i = 0; i < 2000 && sync->pid > 0; ++i) {
        usleep(1000);
        c1_time_sync_tick(sync, true, true, now);
    }
    assert(sync->pid < 0 && sync->probe_fd < 0);
}

static void stop_and_reap(c1_time_sync *sync)
{
    for (unsigned i = 0; i < 2000; ++i) {
        c1_time_sync_stop(sync);
        if (sync->pid < 0) break;
        usleep(1000);
    }
    assert(sync->pid < 0 && sync->probe_fd < 0 && sync->state == C1_TIME_UNCHECKED);
}

static void test_system_observation(void)
{
    c1_time_sync sync = fresh();
    installed = true;
    sync.busybox_path = "/no/busybox";
    c1_time_sync_tick(&sync, true, true, 0);
    assert(sync.state == C1_TIME_WAITING_SYSTEM && sync.pid < 0);
    assert(spawn_count == 0 && signal_count == 0);
    /* Plausible host date and an installed/running daemon are never sufficient. */
    c1_time_sync_tick(&sync, true, true, 4999);
    assert(observation_count == 1 && sync.state == C1_TIME_WAITING_SYSTEM);
    kernel_result = TIME_OK;
    kernel_status = 0;
    c1_time_sync_tick(&sync, true, true, 5000);
    assert(sync.state == C1_TIME_SYNCED);
    kernel_status = STA_UNSYNC;
    c1_time_sync_tick(&sync, true, true, 10000);
    assert(sync.state == C1_TIME_WAITING_SYSTEM);
    kernel_status = STA_CLOCKERR;
    c1_time_sync_tick(&sync, true, true, 15000);
    assert(sync.state == C1_TIME_WAITING_SYSTEM);
    kernel_status = 0;
    kernel_result = -1;
    c1_time_sync_tick(&sync, true, true, 20000);
    assert(sync.state == C1_TIME_WAITING_SYSTEM);
    c1_time_sync_tick(&sync, false, true, 20001);
    assert(sync.state == C1_TIME_UNCHECKED && signal_count == 0);
    kernel_result = TIME_OK;
    c1_time_sync_tick(&sync, true, true, 20002);
    assert(sync.state == C1_TIME_SYNCED && spawn_count == 0);
    stop_and_reap(&sync);

    for (int state = TIME_OK; state <= TIME_ERROR; ++state) {
        kernel_result = state;
        assert(kernel_synchronized() == (state != TIME_ERROR));
    }
}

static void test_process_ownership(void)
{
    c1_time_sync sync = fresh();
    const char daemon[] = "/usr/sbin/ntpd\0-g\0-p\0/var/run/ntpd.pid\0";
    task_fixture(900001, "ntpd\n", daemon, sizeof(daemon) - 1);
    c1_time_sync_tick(&sync, true, true, 0);
    assert(sync.state == C1_TIME_WAITING_SYSTEM && spawn_count == 0);
    remove_task(900001);
    const char busybox[] = "/bin/busybox\0ntpd\0-n\0";
    task_fixture(900002, "busybox\n", busybox, sizeof(busybox) - 1);
    assert(system_service(-1) == SERVICE_SYSTEM);
    /* Do not mistake our own fallback child for an external clock service. */
    assert(system_service(900002) == SERVICE_NONE);
    remove_task(900002);
    const char names[][24] = {"chronyd\n", "systemd-timesyn\n"};
    for (size_t i = 0; i < 2; ++i) {
        task_fixture(900003, names[i], "", 0);
        assert(system_service(-1) == SERVICE_SYSTEM);
        remove_task(900003);
    }
    const char other[] = "/bin/not-ntpd\0ntpd\0";
    task_fixture(900004, "not-ntpd\n", other, sizeof(other) - 1);
    assert(system_service(-1) == SERVICE_NONE);
    remove_task(900004);
    assert(signal_count == 0);
}

static void test_unknown_ownership(void)
{
    c1_time_sync sync = fresh();
    installation_error = EACCES;
    c1_time_sync_tick(&sync, true, true, 0);
    assert(sync.state == C1_TIME_BLOCKED && !spawn_count);
    installation_error = 0;
    char old_root[256];
    strcpy(old_root, proc_root);
    strcpy(proc_root, "/no/c1-proc-fixture");
    assert(system_service(-1) == SERVICE_UNKNOWN);
    strcpy(proc_root, old_root);
    task_fixture(900005, "busybox\n", "/bin/busybox", 12); /* Truncated argv. */
    assert(system_service(-1) == SERVICE_UNKNOWN);
    remove_task(900005);
    task_fixture(900006, "worker\n", "", 0);
    char path[512];
    snprintf(path, sizeof(path), "%s/900006/comm", proc_root);
    assert(unlink(path) == 0 && mkfifo(path, 0600) == 0);
    assert(system_service(-1) == SERVICE_UNKNOWN); /* Must never block on the FIFO. */
    remove_task(900006);
    for (unsigned i = 1; i <= 2049; ++i) {
        snprintf(path, sizeof(path), "%s/%u", proc_root, i + 100000);
        assert(mkdir(path, 0700) == 0);
    }
    assert(system_service(-1) == SERVICE_UNKNOWN); /* Incomplete scan is fail-closed. */
    for (unsigned i = 1; i <= 2049; ++i) {
        snprintf(path, sizeof(path), "%s/%u", proc_root, i + 100000);
        assert(rmdir(path) == 0);
    }
}

static void test_exact_applet_probe(void)
{
    static const char *lists[] = {"", "cat\nsh\n", "ntpd-helper\n", " ntpd\n", "ntpd \n", "xntpd\n"};
    for (size_t i = 0; i < sizeof(lists) / sizeof(lists[0]); ++i) {
        c1_time_sync sync = fresh();
        assert(setenv("C1_TEST_TIME_LIST", lists[i], 1) == 0);
        c1_time_sync_tick(&sync, true, true, 0);
        finish(&sync, 100);
        assert(sync.state == C1_TIME_UNAVAILABLE && spawn_count == 1);
        c1_time_sync_tick(&sync, true, true, 1000);
        assert(spawn_count == 1); /* Backoff, not a fork on every heartbeat. */
    }
    c1_time_sync sync = fresh();
    assert(setenv("C1_TEST_TIME_LIST", "ntpd", 1) == 0);
    c1_time_sync_tick(&sync, true, true, 0);
    finish(&sync, 100);
    assert(sync.state == C1_TIME_FAILED && spawn_count == 1); /* Truncated output. */
    sync = fresh();
    char oversized[C1_TIME_PROBE_BYTES + 2];
    memset(oversized, 'x', sizeof(oversized) - 1);
    oversized[sizeof(oversized) - 1] = '\0';
    assert(setenv("C1_TEST_TIME_LIST", oversized, 1) == 0);
    c1_time_sync_tick(&sync, true, true, 0);
    finish(&sync, 100);
    assert(sync.state == C1_TIME_FAILED && spawn_count == 1);
    sync = fresh();
    assert(setenv("C1_TEST_TIME_LIST_EXIT", "1", 1) == 0);
    c1_time_sync_tick(&sync, true, true, 0);
    finish(&sync, 100);
    assert(sync.state == C1_TIME_FAILED && spawn_count == 1);
}

static void test_fallback_evidence_and_exits(void)
{
    c1_time_sync sync = fresh();
    int extra = open("/dev/null", O_RDONLY);
    assert(extra > 2);
    char number[32];
    snprintf(number, sizeof(number), "%d", extra);
    assert(setenv("C1_TEST_TIME_FD", number, 1) == 0);
    c1_time_sync_tick(&sync, true, true, 0);
    finish(&sync, 100);
    assert(close(extra) == 0);
    assert(spawn_count == 2 && sync.state == C1_TIME_UNVERIFIED);
    assert(sync.applet_available);
    kernel_result = TIME_OK; kernel_status = 0;
    c1_time_sync_tick(&sync, true, true, 5100);
    assert(sync.state == C1_TIME_SYNCED && spawn_count == 2);
    kernel_status = STA_UNSYNC;
    c1_time_sync_tick(&sync, true, true, 10100);
    assert(sync.state == C1_TIME_UNVERIFIED); /* Never retain a stale success forever. */
    for (unsigned i = 0; i < 3; ++i) {
        sync = fresh();
        assert(setenv("C1_TEST_TIME_NTP_EXIT", i == 0 ? "7" : i == 1 ? "127" : "signal", 1) == 0);
        c1_time_sync_tick(&sync, true, true, 0);
        finish(&sync, 100);
        assert(sync.state == (i == 1 ? C1_TIME_UNAVAILABLE : C1_TIME_FAILED));
        assert(spawn_count == 2 && !sync.applet_available);
    }
    sync = fresh();
    sync.applet_available = true;
    install_in_child = true; /* Ownership changes after fork, before the final exec guard. */
    c1_time_sync_tick(&sync, true, true, 0);
    finish(&sync, 100);
    assert(sync.state == C1_TIME_BLOCKED && spawn_count == 1 && !signal_count);
    sync = fresh();
    sync.busybox_path = "/no/c1-busybox";
    c1_time_sync_tick(&sync, false, true, 0);
    assert(sync.state == C1_TIME_UNCHECKED && sync.pid < 0 && spawn_count == 0);
    c1_time_sync_tick(&sync, true, true, 1);
    assert(sync.state == C1_TIME_UNAVAILABLE && spawn_count == 0);
    assert(sync.next_attempt_ms == 300001);
    sync = fresh();
    char path[512];
    snprintf(path, sizeof(path), "%s/bad-executable", proc_root);
    write_fixture(path, "invalid executable", 18);
    assert(chmod(path, 0700) == 0);
    sync.busybox_path = path;
    c1_time_sync_tick(&sync, true, true, 0);
    finish(&sync, 100);
    assert(sync.state == C1_TIME_UNAVAILABLE && spawn_count == 1);
    assert(unlink(path) == 0);
    sync = fresh();
    fork_failure = true;
    c1_time_sync_tick(&sync, true, true, 0);
    assert(sync.state == C1_TIME_FAILED && sync.pid < 0 && sync.probe_fd < 0);
}

static void start_hanging(c1_time_sync *sync, bool probe)
{
    *sync = fresh();
    sync->applet_available = !probe;
    assert(setenv("C1_TEST_TIME_HANG", probe ? "--list" : "ntpd", 1) == 0);
    c1_time_sync_tick(sync, true, true, 100);
    assert(sync->pid > 0 && sync->state == C1_TIME_RUNNING);
}

static void test_timeout_and_cancellation(void)
{
    for (unsigned probe = 0; probe < 2; ++probe) {
        c1_time_sync sync;
        start_hanging(&sync, probe != 0);
        assert(sync.deadline_ms == (probe ? 3100 : 45100));
        int64_t deadline = sync.deadline_ms;
        c1_time_sync_tick(&sync, true, true, deadline);
        finish(&sync, deadline + 1);
        assert(sync.timed_out && sync.state == C1_TIME_FAILED && signal_count <= 1);
        c1_time_sync_tick(&sync, true, true, deadline + 1000);
        assert(spawn_count == 1);
        start_hanging(&sync, probe != 0);
        c1_time_sync_tick(&sync, false, true, 101);
        stop_and_reap(&sync);
        assert(signal_count <= 1);
        start_hanging(&sync, probe != 0);
        c1_time_sync_tick(&sync, true, false, 101);
        stop_and_reap(&sync);
        assert(signal_count <= 1);
    }
    c1_time_sync sync;
    start_hanging(&sync, false);
    installed = true; /* A system installation appears: cancel only our own child. */
    c1_time_sync_tick(&sync, true, true, 5100);
    finish(&sync, 5101);
    assert(sync.state == C1_TIME_WAITING_SYSTEM && signal_count <= 1);
    c1_time_sync_tick(&sync, true, true, 400000);
    assert(spawn_count == 1); /* No fallback even after the retry deadline. */
    start_hanging(&sync, true);
    installed = true;
    c1_time_sync_tick(&sync, true, true, 5100);
    finish(&sync, 5101);
    assert(sync.state == C1_TIME_WAITING_SYSTEM && spawn_count == 1);
}

static void test_reap_ownership_and_interruptions(void)
{
    c1_time_sync sync;
    start_hanging(&sync, false);
    forced_wait_error = EINTR;
    c1_time_sync_stop(&sync);
    assert(sync.pid > 0 && signal_count == 0); /* Interrupted wait proves no ownership. */
    forced_wait_error = 0;
    stop_and_reap(&sync);
    assert(signal_count <= 1);
    sync = fresh();
    pid_t child = fork();
    assert(child >= 0);
    if (!child) _exit(0);
    assert(waitpid(child, NULL, 0) == child);
    sync.pid = child; /* Already reaped: must not signal a stale/reused PID. */
    c1_time_sync_stop(&sync);
    assert(sync.pid < 0 && signal_count == 0);
    sync = fresh();
    sync.pid = child;
    sync.deadline_ms = 1000;
    c1_time_sync_tick(&sync, true, true, 100);
    assert(sync.state == C1_TIME_FAILED && sync.pid < 0 && signal_count == 0);
    assert(after_ms(INT64_MAX - 1, 300000) == INT64_MAX);
    c1_time_sync_tick(NULL, true, true, 0);
    c1_time_sync_stop(NULL);
    c1_time_sync_init(NULL);
}

static int helper(int argc, char **argv)
{
    const char *fd = getenv("C1_TEST_TIME_FD");
    if (fd) assert(fcntl(atoi(fd), F_GETFD) < 0 && errno == EBADF);
    sigset_t mask;
    assert(sigprocmask(SIG_SETMASK, NULL, &mask) == 0);
    assert(!sigismember(&mask, SIGTERM) && !sigismember(&mask, SIGINT));
    const char *hang = getenv("C1_TEST_TIME_HANG");
    if (hang && !strcmp(hang, argv[1])) for (;;) pause();
    if (!strcmp(argv[1], "--list")) {
        assert(argc == 2);
        const char *list = getenv("C1_TEST_TIME_LIST");
        assert(list);
        size_t length = strlen(list), used = 0;
        while (used < length) {
            ssize_t n = write(STDOUT_FILENO, list + used, length - used);
            if (n < 0 && errno == EINTR) continue;
            assert(n > 0);
            used += (size_t)n;
        }
        return atoi(getenv("C1_TEST_TIME_LIST_EXIT"));
    }
    assert(argc == 6 && !strcmp(argv[1], "ntpd") && !strcmp(argv[2], "-n") &&
           !strcmp(argv[3], "-q") && !strcmp(argv[4], "-p") &&
           !strcmp(argv[5], "time.cloudflare.com"));
    if (!strcmp(getenv("C1_TEST_TIME_NTP_EXIT"), "signal")) {
        raise(SIGTERM);
        _exit(126);
    }
    return atoi(getenv("C1_TEST_TIME_NTP_EXIT"));
}

int main(int argc, char **argv)
{
    if (argc > 1) return helper(argc, argv);
    strcpy(proc_root, "/tmp/c1-time-test-XXXXXX");
    assert(mkdtemp(proc_root));
    /* A separately owned child must survive every service stop and timeout. */
    pid_t parent = getpid();
    pid_t unrelated = fork();
    assert(unrelated >= 0);
    if (!unrelated) {
        assert(prctl(PR_SET_PDEATHSIG, SIGKILL) == 0);
        if (getppid() != parent) _exit(1);
        for (;;) pause();
    }
    test_system_observation();
    test_process_ownership();
    test_unknown_ownership();
    test_exact_applet_probe();
    test_fallback_evidence_and_exits();
    test_timeout_and_cancellation();
    test_reap_ownership_and_interruptions();
    assert(kill(unrelated, 0) == 0);
    assert(kill(unrelated, SIGKILL) == 0);
    assert(waitpid(unrelated, NULL, 0) == unrelated);
    assert(rmdir(proc_root) == 0);
    assert(waitpid(-1, NULL, WNOHANG) < 0 && errno == ECHILD);
    puts("time sync tests passed: read-only system evidence, exact applets, exits, timeouts, owned-child cleanup");
    return 0;
}
