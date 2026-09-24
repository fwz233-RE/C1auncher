#define _DEFAULT_SOURCE 1
#include "services/desktop_jobs.h"
#include "update/update.h"

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* Linked wrappers are test-only. Exec routes the two fixed production commands
 * back into this executable, never to a device, repository or production key. */
#define FAKE_PID 123456
#define FAKE_FD 123457
static const char valid[] = "C1UPDATE-CHECK 1\nS\t42\nV\t1.2.3\nA\t1\n";
static bool mock;
static int mock_wait, mock_status, mock_signals, mock_closes, mock_reads;
static bool mock_reaped, signalled_after_reap, mock_group_missing, mock_eagain, mock_eintr;
static int last_signal;
static pid_t last_target;
static unsigned char mock_output[512];
static size_t mock_size, mock_offset;
static const char *fixture_mode = "ok";
static const char *fixture_directory = "";
static int fixture_sentinel = -1, fixture_kind = 1;

ssize_t __real_read(int fd, void *buffer, size_t size);
ssize_t __wrap_read(int fd, void *buffer, size_t size)
{
    if (!mock || fd != FAKE_FD) return __real_read(fd, buffer, size);
    ++mock_reads;
    if (mock_eintr) { errno = EINTR; return -1; }
    if (mock_offset < mock_size) {
        size_t remaining = mock_size - mock_offset;
        if (size > remaining) size = remaining;
        memcpy(buffer, mock_output + mock_offset, size);
        mock_offset += size;
        return (ssize_t)size;
    }
    if (mock_eagain) { errno = EAGAIN; return -1; }
    return 0;
}

pid_t __real_waitpid(pid_t pid, int *status, int options);
pid_t __wrap_waitpid(pid_t pid, int *status, int options)
{
    if (!mock || pid != FAKE_PID) return __real_waitpid(pid, status, options);
    assert(options == WNOHANG);
    if (mock_wait < 0) { errno = ECHILD; mock_reaped = true; return -1; }
    if (mock_wait == 0) return 0;
    *status = mock_status;
    mock_reaped = true;
    return pid;
}

int __real_kill(pid_t pid, int number);
int __wrap_kill(pid_t pid, int number)
{
    if (!mock) return __real_kill(pid, number);
    ++mock_signals;
    last_signal = number;
    last_target = pid;
    if (mock_reaped) signalled_after_reap = true;
    if (mock_group_missing && pid < 0) { errno = ESRCH; return -1; }
    return 0;
}

int __real_close(int fd);
int __wrap_close(int fd)
{
    if (mock && fd == FAKE_FD) { ++mock_closes; return 0; }
    return __real_close(fd);
}

DIR *__real_opendir(const char *path);
DIR *__wrap_opendir(const char *path)
{
    if (strcmp(fixture_mode, "proc-unavailable") == 0 && strcmp(path, "/proc/self/fd") == 0) {
        errno = EACCES;
        return NULL;
    }
    return __real_opendir(path);
}

int __real_execv(const char *path, char *const arguments[]);
int __wrap_execv(const char *path, char *const arguments[])
{
    char descriptor[32];
    char *test_arguments[] = {"test-desktop-jobs", "--fixture", (char *)fixture_mode,
                             descriptor, (char *)fixture_directory, NULL};
    const char *expected_path = fixture_kind == 1 ? "/usr/data/c1/bin/c1pkg" : "/usr/data/c1/bin/c1updater";
    const char *expected_name = fixture_kind == 1 ? "c1pkg" : "c1updater";
    const char *expected_command = fixture_kind == 1 ? "desktop-summary" : "check-configured";
    if (strcmp(path, expected_path) != 0 || strcmp(arguments[0], expected_name) != 0 ||
        arguments[1] == NULL || strcmp(arguments[1], expected_command) != 0 || arguments[2] != NULL) _exit(110);
    if (strcmp(fixture_mode, "exec-failure") == 0) { errno = ENOENT; return -1; }
    (void)snprintf(descriptor, sizeof(descriptor), "%d", fixture_sentinel);
    return __real_execv("/proc/self/exe", test_arguments);
}

static int64_t milliseconds(void)
{
    struct timespec now;
    assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static void write_all(int descriptor, const char *data, size_t size)
{
    while (size > 0U) {
        ssize_t amount = write(descriptor, data, size);
        if (amount < 0 && errno == EINTR) continue;
        if (amount <= 0) _exit(111);
        data += amount;
        size -= (size_t)amount;
    }
}

static volatile sig_atomic_t stopping;
static void stop_fixture(int number)
{
    (void)number;
    stopping = 1;
}

static int fixture(const char *mode, const char *sentinel, const char *directory)
{
    if (strcmp(mode, "inspect") == 0) {
        sigset_t mask;
        struct sigaction action;
        struct stat input, errors, null_info;
        char byte;
        DIR *fds;
        struct dirent *entry;
        int null;
        if (getpgrp() != getpid() || sigprocmask(SIG_SETMASK, NULL, &mask) != 0 ||
            sigismember(&mask, SIGTERM) || sigismember(&mask, SIGINT)) return 112;
        for (size_t i = 0; i < 4; ++i) {
            const int signals[] = {SIGTERM, SIGPIPE, SIGCHLD, SIGUSR1};
            if (sigaction(signals[i], NULL, &action) != 0 || action.sa_handler != SIG_DFL) return 113;
        }
        if (fcntl(atoi(sentinel), F_GETFD) >= 0 || errno != EBADF) return 114;
        if (read(STDIN_FILENO, &byte, 1) != 0 || fstat(0, &input) != 0 || fstat(2, &errors) != 0) return 115;
        null = open("/dev/null", O_RDWR);
        if (null < 0 || fstat(null, &null_info) != 0 || input.st_rdev != null_info.st_rdev ||
            errors.st_rdev != null_info.st_rdev || !S_ISCHR(input.st_mode) || !S_ISCHR(errors.st_mode)) return 116;
        close(null);
        fds = opendir("/proc/self/fd");
        if (fds == NULL) return 117;
        while ((entry = readdir(fds)) != NULL) {
            char *end;
            long fd = strtol(entry->d_name, &end, 10);
            if (*end == '\0' && fd > 2 && fd != dirfd(fds)) return 118;
        }
        closedir(fds);
    } else if (strcmp(mode, "cleanup") == 0) {
        struct sigaction action = {0};
        char metadata[512];
        struct timespec delay = {3, 0};
        int descriptor;
        action.sa_handler = stop_fixture;
        sigemptyset(&action.sa_mask);
        if (sigaction(SIGTERM, &action, NULL) != 0 || mkdir(directory, 0700) != 0) return 119;
        if (snprintf(metadata, sizeof(metadata), "%s/manifest.v1", directory) >= (int)sizeof(metadata)) return 120;
        descriptor = open(metadata, O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (descriptor < 0) return 121;
        write_all(descriptor, "metadata", 8);
        close(descriptor);
        write_all(1, "ready\n", 6);
        while (!stopping) {
            struct timespec tick = {0, 10000000};
            (void)nanosleep(&tick, NULL);
        }
        while (nanosleep(&delay, &delay) != 0 && errno == EINTR) {}
        if (unlink(metadata) != 0 || rmdir(directory) != 0) return 122;
        return 0;
    } else if (strcmp(mode, "hold") == 0) {
        signal(SIGTERM, SIG_IGN);
        write_all(1, "ready\n", 6);
        for (;;) pause();
    } else if (strcmp(mode, "overflow") == 0) {
        char buffer[512];
        memset(buffer, 'x', sizeof(buffer));
        write_all(1, buffer, sizeof(buffer));
        return 0;
    } else if (strcmp(mode, "failed") == 0) {
        write_all(1, valid, sizeof(valid) - 1U);
        return 7;
    }
    write_all(1, valid, sizeof(valid) - 1U);
    return 0;
}

static void reset_mock(c1_desktop_job *job, size_t output_size)
{
    c1_desktop_job_init(job);
    job->pid = FAKE_PID;
    job->fd = FAKE_FD;
    job->kind = 2;
    job->deadline = 100000;
    mock = true;
    mock_wait = mock_status = mock_signals = mock_closes = mock_reads = last_signal = 0;
    mock_reaped = signalled_after_reap = mock_group_missing = mock_eagain = mock_eintr = false;
    last_target = 0;
    mock_offset = 0;
    mock_size = output_size;
    memset(mock_output, 'x', sizeof(mock_output));
}

static void test_poll_ownership_and_limits(void)
{
    c1_desktop_job job;
    /* Reaped child plus overflowing final drain must NEVER kill its old PGID. */
    reset_mock(&job, 300);
    mock_wait = 1;
    assert(c1_desktop_job_poll(&job, 200000) == -1);
    assert(mock_signals == 0 && !signalled_after_reap && mock_closes == 1);
    assert(job.pid == -1 && job.fd == -1 && job.used == 0 && job.output[0] == '\0');
    assert(c1_desktop_job_poll(&job, 999999) == 0);
    c1_desktop_job_cancel(&job, 999999);
    assert(mock_signals == 0);

    reset_mock(&job, 300);
    mock_wait = -1;
    assert(c1_desktop_job_poll(&job, 200000) == -1);
    assert(mock_signals == 0 && !signalled_after_reap);

    reset_mock(&job, 255);
    mock_wait = 1;
    assert(c1_desktop_job_poll(&job, 0) == 1);
    assert(job.used == 255 && job.output[255] == 0 && mock_signals == 0);
    reset_mock(&job, 256);
    mock_wait = 1;
    assert(c1_desktop_job_poll(&job, 0) == -1 && mock_signals == 0);
    reset_mock(&job, 5);
    mock_output[3] = 0;
    mock_wait = 1;
    assert(c1_desktop_job_poll(&job, 0) == -1 && job.used == 0);

    reset_mock(&job, sizeof(valid) - 1U);
    memcpy(mock_output, valid, mock_size);
    mock_wait = 1;
    mock_eagain = true; /* A descendant still holds the pipe: not a full result. */
    assert(c1_desktop_job_poll(&job, 0) == -1 && job.used == 0 && mock_signals == 0);

    reset_mock(&job, sizeof(valid) - 1U);
    memcpy(mock_output, valid, mock_size);
    mock_wait = 1;
    mock_status = 7 << 8;
    assert(c1_desktop_job_poll(&job, 0) == -1 && job.used == 0);

    reset_mock(&job, 300);
    assert(c1_desktop_job_poll(&job, 12) == 0);
    assert(job.failed && job.cancelled && !job.killed && mock_signals == 1 && last_signal == SIGTERM);
    assert(job.deadline == 12 + C1_DESKTOP_CORE_CANCEL_GRACE_MS);
    assert(c1_desktop_job_poll(&job, job.deadline) == 0);
    assert(job.killed && mock_signals == 2 && last_signal == SIGKILL);
    assert(c1_desktop_job_poll(&job, job.deadline + 1) == 0 && mock_signals == 2);
    mock_wait = 1;
    mock_status = SIGKILL;
    assert(c1_desktop_job_poll(&job, job.deadline + 2) == -1);
    assert(!signalled_after_reap && mock_signals == 2);

    reset_mock(&job, 0);
    mock_eintr = true;
    assert(c1_desktop_job_poll(&job, 0) == 0 && mock_reads == 4);
    mock = false;
}

static void test_deadline_and_cancel(void)
{
    c1_desktop_job job;
    reset_mock(&job, 0);
    mock_eagain = true;
    assert(c1_desktop_job_poll(&job, 99999) == 0 && mock_signals == 0);
    assert(c1_desktop_job_poll(&job, 100000) == 0);
    assert(mock_signals == 1 && last_signal == SIGTERM && job.deadline == 110000);
    c1_desktop_job_cancel(&job, 105000);
    assert(mock_signals == 1 && job.deadline == 110000); /* No grace extension. */
    assert(c1_desktop_job_poll(&job, 102001) == 0 && !job.killed);
    assert(c1_desktop_job_poll(&job, 109999) == 0 && !job.killed);
    assert(c1_desktop_job_poll(&job, 110000) == 0 && job.killed && last_signal == SIGKILL);

    reset_mock(&job, 0);
    job.kind = 1;
    mock_group_missing = true;
    c1_desktop_job_cancel(&job, 10);
    assert(mock_signals == 2 && last_target == FAKE_PID && last_signal == SIGTERM && job.deadline == 2010);
    reset_mock(&job, 0);
    c1_desktop_job_cancel(&job, INT64_MAX - 5);
    assert(job.deadline == INT64_MAX);
    mock = false;
}

static void rejected(const char *text, size_t size)
{
    uint64_t sequence = 987;
    bool available = true;
    assert(!c1_desktop_update_parse(text, size, &sequence, &available));
    assert(sequence == 987 && available);
}

static void test_parser(void)
{
    uint64_t sequence = 0;
    bool available = false;
    char text[300], version[67];
    const char *bad[] = {
        "C1UPDATE-CHECK 1\nS\t0\nV\t1\nA\t1\n", "C1UPDATE-CHECK 1\nS\t01\nV\t1\nA\t1\n",
        "C1UPDATE-CHECK 1\nS\t+1\nV\t1\nA\t1\n", "C1UPDATE-CHECK 1\nS\t-1\nV\t1\nA\t1\n",
        "C1UPDATE-CHECK 1\nS\t18446744073709551616\nV\t1\nA\t1\n",
        "C1UPDATE-CHECK 1\nS\t999999999999999999999999999999999999\nV\t1\nA\t1\n",
        "C1UPDATE-CHECK 1\nS\t 1\nV\t1\nA\t1\n", "C1UPDATE-CHECK 1\nS 1\nV\t1\nA\t1\n",
        "C1UPDATE-CHECK 1\nS\t1 \nV\t1\nA\t1\n", "C1UPDATE-CHECK 1\nS\t1\nV\t\nA\t1\n",
        "C1UPDATE-CHECK 1\nS\t1\nV\t-1\nA\t1\n", "C1UPDATE-CHECK 1\nS\t1\nV\t1-\nA\t1\n",
        "C1UPDATE-CHECK 1\nS\t1\nV\t1/2\nA\t1\n", "C1UPDATE-CHECK 1\nS\t1\nV\t1\nA\t2\n",
        "C1UPDATE-CHECK 1\nS\t1\nV\t1\nA\t01\n", "C1UPDATE-CHECK 1\nS\t1\nV\t1\nA\t+1\n",
        "C1UPDATE-CHECK 1\nS\t1\nV\t1\nA\t-0\n", "C1UPDATE-CHECK 1\nS\t1\nV\t1\nA\t 1\n",
        "C1UPDATE-CHECK 1\nS\t1\nV\t1\nA\t1 \n", "C1UPDATE-CHECK 1\nS\t1\nV\t1\nA\t1",
        "C1UPDATE-CHECK 1\nS\t1\nV\t1\nA\t1\n\n", "C1UPDATE-CHECK 1\nS\t1\nV\t1\nA\t1\nextra",
        " C1UPDATE-CHECK 1\nS\t1\nV\t1\nA\t1\n", "C1UPDATE-CHECK 2\nS\t1\nV\t1\nA\t1\n",
        "C1UPDATE-CHECK 1\r\nS\t1\r\nV\t1\r\nA\t1\r\n",
        "C1UPDATE-CHECK 1\n\nS\t1\nV\t1\nA\t1\n", "C1UPDATE-CHECK 1\nS\t1\nV\t\xff\nA\t1\n"
    };
    assert(c1_desktop_update_parse(valid, sizeof(valid) - 1U, &sequence, &available));
    assert(sequence == 42 && available);
    for (size_t length = 0; length < sizeof(valid) - 1U; ++length) rejected(valid, length);
    rejected(valid, sizeof(valid)); /* Embedded/trailing NUL is not part of the wire protocol. */
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) rejected(bad[i], strlen(bad[i]));
    memset(version, 'a', 64); version[64] = 0;
    snprintf(text, sizeof(text), "C1UPDATE-CHECK 1\nS\t18446744073709551615\nV\t%s\nA\t0\n", version);
    assert(c1_desktop_update_parse(text, strlen(text), &sequence, &available));
    assert(sequence == UINT64_MAX && !available);
    version[64] = 'a'; version[65] = 0;
    snprintf(text, sizeof(text), "C1UPDATE-CHECK 1\nS\t1\nV\t%s\nA\t0\n", version);
    rejected(text, strlen(text));
    snprintf(text, sizeof(text), "C1UPDATE-CHECK 1\nS\t1\nV\t1.2+dev_build-1\nA\t1\n");
    assert(c1_desktop_update_parse(text, strlen(text), &sequence, &available));
    memcpy(text, valid, sizeof(valid)); text[5] = 0; rejected(text, sizeof(valid) - 1U);
    memset(text, 'x', sizeof(text)); rejected(text, 256);
    rejected(NULL, 1);
    assert(!c1_desktop_update_parse(valid, sizeof(valid) - 1U, NULL, &available));
    assert(!c1_desktop_update_parse(valid, sizeof(valid) - 1U, &sequence, NULL));
    /* The API accepts a sized non-NUL-terminated buffer, even at a guard page. */
    {
        size_t page = (size_t)sysconf(_SC_PAGESIZE);
        char *region = mmap(NULL, page * 2U, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        char *wire;
        assert(region != MAP_FAILED && mprotect(region + page, page, PROT_NONE) == 0);
        wire = region + page - (sizeof(valid) - 1U);
        memcpy(wire, valid, sizeof(valid) - 1U);
        assert(c1_desktop_update_parse(wire, sizeof(valid) - 1U, &sequence, &available));
        assert(munmap(region, page * 2U) == 0);
    }
}

static int finish(c1_desktop_job *job, int64_t timeout)
{
    int64_t until = milliseconds() + timeout;
    while (milliseconds() < until) {
        struct timespec delay = {0, 2000000};
        int result = c1_desktop_job_poll(job, milliseconds());
        if (result != 0) return result;
        nanosleep(&delay, NULL);
    }
    if (job->pid > 0) { __real_kill(-job->pid, SIGKILL); __real_kill(job->pid, SIGKILL); __real_waitpid(job->pid, NULL, 0); }
    assert(!"test fixture exceeded its deadline");
    return -1;
}

static void ready(c1_desktop_job *job)
{
    int64_t until = milliseconds() + 3000;
    while (job->used < 6 && milliseconds() < until) {
        struct timespec delay = {0, 2000000};
        assert(c1_desktop_job_poll(job, milliseconds()) == 0);
        nanosleep(&delay, NULL);
    }
    assert(job->used == 6 && strcmp(job->output, "ready\n") == 0);
}

static void test_real_children(void)
{
    c1_desktop_job job;
    int original = open("/dev/null", O_RDWR);
    int sentinel = fcntl(original, F_DUPFD, 200);
    assert(original >= 0 && sentinel >= 200);
    close(original);
    fixture_sentinel = sentinel;
    c1_desktop_job_init(&job);
    assert(!c1_desktop_job_start(&job, 0, 0) && !c1_desktop_job_start(&job, 3, 0));
    assert(!c1_desktop_job_start(&job, 1, -1));
    assert(c1_desktop_job_poll(&job, 0) == 0 && !c1_desktop_job_start(NULL, 1, 0));
    for (int kind = 1; kind <= 2; ++kind) {
        struct sigaction ignored = {0}, old_term, old_pipe, old_user;
        sigset_t blocked, old_mask;
        pid_t pid;
        int fd;
        fixture_kind = kind;
        fixture_mode = "inspect";
        ignored.sa_handler = SIG_IGN;
        sigemptyset(&ignored.sa_mask);
        assert(sigaction(SIGTERM, &ignored, &old_term) == 0);
        assert(sigaction(SIGPIPE, &ignored, &old_pipe) == 0);
        assert(sigaction(SIGUSR1, &ignored, &old_user) == 0);
        sigemptyset(&blocked); sigaddset(&blocked, SIGTERM); sigaddset(&blocked, SIGINT);
        assert(sigprocmask(SIG_BLOCK, &blocked, &old_mask) == 0);
        assert(c1_desktop_job_start(&job, kind, milliseconds()));
        assert(sigprocmask(SIG_SETMASK, &old_mask, NULL) == 0);
        sigaction(SIGTERM, &old_term, NULL); sigaction(SIGPIPE, &old_pipe, NULL); sigaction(SIGUSR1, &old_user, NULL);
        assert(fcntl(job.fd, F_GETFL) & O_NONBLOCK);
        assert(fcntl(job.fd, F_GETFD) & FD_CLOEXEC);
        pid = job.pid; fd = job.fd;
        assert(!c1_desktop_job_start(&job, kind, 0) && job.pid == pid && job.fd == fd);
        assert(finish(&job, 5000) == 1 && job.kind == kind && job.pid == -1 && job.fd == -1);
        assert(strcmp(job.output, valid) == 0 && job.used == sizeof(valid) - 1U);
        assert(fcntl(fd, F_GETFD) == -1 && errno == EBADF);
        assert(__real_waitpid(pid, NULL, WNOHANG) == -1 && errno == ECHILD);
        assert(fcntl(sentinel, F_GETFD) >= 0);
    }
    fixture_kind = 1;
    for (size_t i = 0; i < 4; ++i) {
        const char *modes[] = {"overflow", "failed", "exec-failure", "proc-unavailable"};
        fixture_mode = modes[i];
        assert(c1_desktop_job_start(&job, 1, milliseconds()));
        assert(finish(&job, 5000) == -1 && job.used == 0 && job.output[0] == '\0');
    }
    fixture_mode = "ok";
    for (int i = 0; i < 5; ++i) {
        assert(c1_desktop_job_start(&job, 1, milliseconds()));
        assert(finish(&job, 5000) == 1);
    }
    /* The pipe must survive exec when any/all parent standard fds are closed. */
    {
        pid_t child = fork();
        int status;
        assert(child >= 0);
        if (child == 0) {
            close(0); close(1); close(2);
            fixture_mode = "inspect";
            c1_desktop_job_init(&job);
            if (!c1_desktop_job_start(&job, 1, milliseconds()) || finish(&job, 5000) != 1 ||
                strcmp(job.output, valid) != 0) _exit(124);
            _exit(0);
        }
        assert(__real_waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }
    close(sentinel);
    fixture_sentinel = -1;
}

static void test_real_cancellation_cleanup(void)
{
    c1_desktop_job job;
    char root[] = "/tmp/c1-desktop-jobs-XXXXXX";
    char directory[256];
    int64_t cancelled_at;
    assert(mkdtemp(root) != NULL);
    snprintf(directory, sizeof(directory), "%s/.check.fixture", root);
    fixture_directory = directory;
    fixture_kind = 2;
    fixture_mode = "cleanup";
    c1_desktop_job_init(&job);
    assert(c1_desktop_job_start(&job, 2, milliseconds()));
    ready(&job);
    cancelled_at = milliseconds();
    c1_desktop_job_cancel(&job, cancelled_at);
    assert(job.deadline == cancelled_at + C1_DESKTOP_CORE_CANCEL_GRACE_MS);
    assert(finish(&job, 6000) == -1);
    assert(milliseconds() - cancelled_at >= 2900 && !job.killed);
    assert(access(directory, F_OK) != 0 && errno == ENOENT);
    assert(rmdir(root) == 0);
    fixture_directory = "";

    fixture_kind = 1;
    fixture_mode = "hold";
    assert(c1_desktop_job_start(&job, 1, milliseconds()));
    ready(&job);
    /* Exercise the 35s soft deadline without waiting 35 real seconds. */
    assert(c1_desktop_job_poll(&job, job.deadline) == 0 && job.cancelled && !job.killed);
    assert(c1_desktop_job_poll(&job, job.deadline) == 0 && job.killed);
    assert(finish(&job, 3000) == -1 && job.pid == -1 && job.fd == -1);
    fixture_mode = "ok";
}

int main(int argc, char **argv)
{
    if (argc == 5 && strcmp(argv[1], "--fixture") == 0) return fixture(argv[2], argv[3], argv[4]);
    test_parser();
    test_poll_ownership_and_limits();
    test_deadline_and_cancel();
    test_real_children();
    test_real_cancellation_cleanup();
    puts("all desktop jobs tests passed");
    return 0;
}
