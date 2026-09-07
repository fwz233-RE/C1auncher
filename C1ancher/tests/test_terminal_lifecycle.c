#define _GNU_SOURCE 1
#include "services/terminal.h"
#include "platform/app_lease.h"
#include "platform/liveness.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

static int failures;
static void expect(bool okay, const char *message)
{
    if (!okay) { fprintf(stderr, "FAIL: %s\n", message); ++failures; }
}
static int64_t now_ms(void)
{
    struct timespec value;
    (void)clock_gettime(CLOCK_MONOTONIC, &value);
    return (int64_t)value.tv_sec * 1000 + value.tv_nsec / 1000000;
}
static void pause_tick(void)
{
    struct timespec delay = {0, 10000000L};
    (void)nanosleep(&delay, NULL);
}
static pid_t read_pid(const char *path)
{
    FILE *file = fopen(path, "r");
    long pid = -1;
    if (file != NULL) { if (fscanf(file, "%ld", &pid) != 1) pid = -1; fclose(file); }
    return (pid_t)pid;
}

/* Session root creates a foreground group that ignores HUP, or a detached
 * descendant. The session root may exit before its child; neither is a reason
 * to skip cleanup. Its lease descriptor intentionally survives exec. */
static int fixture(int argc, char **argv)
{
    int ready[2];
    pid_t child;
    if (argc < 5) return 126;
    if (strcmp(argv[1], "--hold") == 0) {
        FILE *file = fopen(argv[3], "w");
        if (file == NULL) return 126;
        fprintf(file, "%ld\n", (long)getpid()); fclose(file);
        for (;;) pause();
    }
    if (pipe(ready) != 0) return 126;
    child = fork();
    if (child < 0) return 126;
    if (child == 0) {
        int lease;
        close(ready[0]);
        (void)signal(SIGHUP, SIG_IGN);
        (void)signal(SIGTERM, SIG_IGN);
        (void)signal(SIGTTOU, SIG_IGN);
        if (strcmp(argv[1], "--detached") == 0) {
            if (setsid() < 0) _exit(126);
        } else {
            if (setpgid(0, 0) != 0 || tcsetpgrp(STDIN_FILENO, getpid()) != 0) _exit(126);
        }
        lease = c1_app_run_acquire_at(argv[2]);
        if (lease < 0 || (fcntl(lease, F_GETFD) & FD_CLOEXEC)) _exit(126);
        if (write(ready[1], "R", 1U) != 1) _exit(126);
        close(ready[1]);
        execl(argv[4], argv[4], "--hold", argv[2], argv[3], argv[4], (char *)NULL);
        _exit(127);
    }
    close(ready[1]);
    {
        char byte;
        if (read(ready[0], &byte, 1U) != 1) return 126;
    }
    close(ready[0]);
    /* Wait for exec and the identity marker, not merely the fork. */
    while (read_pid(argv[3]) <= 0) pause_tick();
    if (strcmp(argv[1], "--shell-first") == 0 || strcmp(argv[1], "--detached") == 0)
        return 0;
    for (;;) pause();
}

static void test_cleanup(const char *self, const char *mode)
{
    char root[] = "/tmp/c1-terminal-lifecycle-XXXXXX";
    char lock[256], pidfile[256];
    char *arguments[] = {(char *)self, (char *)mode, lock, pidfile, (char *)self, NULL};
    c1_terminal_session session;
    int64_t deadline;
    pid_t descendant = -1;
    int lease;
    expect(mkdtemp(root) != NULL, "create isolated cleanup fixture");
    snprintf(lock, sizeof(lock), "%s/lock", root);
    snprintf(pidfile, sizeof(pidfile), "%s/pid", root);
    c1_terminal_init(&session);
    expect(c1_terminal_start_exec(&session, 49, 19, self, arguments) == C1_STATUS_OK,
           "start dedicated executable session");
    deadline = now_ms() + 3000;
    while ((descendant = read_pid(pidfile)) <= 0 && now_ms() < deadline) pause_tick();
    expect(descendant > 0, "foreground or detached descendant reached exec");
    if (strcmp(mode, "--foreground") != 0) {
        while (c1_terminal_is_running(&session) && now_ms() < deadline) pause_tick();
        expect(!c1_terminal_is_running(&session), "shell exit completes bounded descendant cleanup");
    }
    c1_terminal_stop(&session);
    expect(descendant > 0 && kill(descendant, 0) != 0 && errno == ESRCH,
           "session cleanup reaps HUP-ignoring descendants even after shell exits");
    lease = c1_app_run_acquire_at(lock);
    expect(lease >= 0, "inherited run lock released after descendant exec and cleanup");
    c1_app_lease_release(lease);
    unlink(lock); unlink(pidfile); rmdir(root);
}

static void test_parent_crash(const char *self)
{
    char root[] = "/tmp/c1-terminal-owner-XXXXXX";
    char lock[256], pidfile[256];
    char *arguments[] = {(char *)self, "--foreground", lock, pidfile, (char *)self, NULL};
    int report[2];
    pid_t owner, supervisor = -1, descendant;
    int64_t deadline;
    bool reaped = false;
    expect(mkdtemp(root) != NULL, "create crashed terminal owner fixture");
    snprintf(lock, sizeof(lock), "%s/lock", root);
    snprintf(pidfile, sizeof(pidfile), "%s/pid", root);
    expect(pipe(report) == 0, "create supervisor identity pipe");
    owner = fork();
    if (owner == 0) {
        c1_terminal_session session;
        close(report[0]);
        c1_terminal_init(&session);
        if (c1_terminal_start_exec(&session, 49, 19, self, arguments) != C1_STATUS_OK) _exit(126);
        if (write(report[1], &session.child_pid, sizeof(session.child_pid)) !=
            (ssize_t)sizeof(session.child_pid)) _exit(126);
        deadline = now_ms() + 3000;
        while (read_pid(pidfile) <= 0 && now_ms() < deadline) pause_tick();
        _exit(1); /* Deliberately bypass c1_terminal_stop, as a crashed UI does. */
    }
    close(report[1]);
    expect(read(report[0], &supervisor, sizeof(supervisor)) == (ssize_t)sizeof(supervisor),
           "record owned session supervisor before UI crash");
    close(report[0]);
    waitpid(owner, NULL, 0);
    descendant = read_pid(pidfile);
    deadline = now_ms() + 3000;
    while (supervisor > 0 && now_ms() < deadline) {
        if (waitpid(supervisor, NULL, WNOHANG) == supervisor) { reaped = true; break; }
        pause_tick();
    }
    expect(reaped, "anonymous control pipe detects UI death without a service");
    expect(descendant > 0 && kill(descendant, 0) < 0 && errno == ESRCH,
           "crashed UI cannot leave HUP-ignoring terminal descendants");
    expect(!c1_app_run_active_at(lock), "UI crash releases descendants' inherited application lock");
    unlink(lock); unlink(pidfile); rmdir(root);
}

static void test_busy_terminal_app(void)
{
    c1_terminal_session user, app;
    char *arguments[] = {"/bin/sh", "-c", "printf APP_ONLY; sleep .15", NULL};
    const char command[] = "printf OLD_NEOFETCH; sleep 30 & sleep 30\r";
    char output[512] = "";
    size_t used = 0;
    int64_t deadline = now_ms() + 3000;
    pid_t user_supervisor;
    c1_terminal_init(&user); c1_terminal_init(&app);
    expect(c1_terminal_start(&user, 49, 19) == C1_STATUS_OK, "start normal user shell");
    while (!c1_terminal_shell_is_foreground(&user) && now_ms() < deadline) pause_tick();
    expect(c1_terminal_write(&user, command, sizeof(command)-1U) == C1_STATUS_OK,
           "normal user terminal has existing foreground and background work");
    while (c1_terminal_shell_is_foreground(&user) && now_ms() < deadline) pause_tick();
    expect(!c1_terminal_shell_is_foreground(&user), "normal terminal is busy");
    user_supervisor = user.child_pid;
    expect(c1_terminal_start_exec(&app, 49, 19, arguments[0], arguments) == C1_STATUS_OK,
           "busy shell cannot drop a dedicated APP request");
    while (c1_terminal_is_running(&app) && now_ms() < deadline) {
        ssize_t count = c1_terminal_read(&app, output + used, sizeof(output)-used-1U);
        if (count > 0) { used += (size_t)count; output[used] = '\0'; }
        pause_tick();
    }
    expect(strstr(output, "APP_ONLY") != NULL && strstr(output, "OLD_NEOFETCH") == NULL,
           "APP output is isolated from the old normal terminal screen");
    expect(!c1_terminal_is_running(&app), "dedicated app exits instead of returning to a shell");
    c1_terminal_stop(&app);
    expect(c1_terminal_is_running(&user) && user.child_pid == user_supervisor &&
           kill(user_supervisor, 0) == 0, "closing APP preserves the busy user terminal");
    c1_terminal_stop(&user);
}

static void test_heartbeat(void)
{
    int pipefd[2];
    char value[32], byte;
    expect(!c1_liveness_expired(19999, 0, -1, 20000, 12000), "startup grace is bounded but tolerant");
    expect(c1_liveness_expired(20000, 0, -1, 20000, 12000), "startup hang detected before thirty-second confirmation");
    expect(!c1_liveness_expired(13000, 0, 2000, 20000, 12000), "normal slow operation has heartbeat grace");
    expect(c1_liveness_expired(14000, 0, 2000, 20000, 12000), "stopped UI pulses detect a hang");
    expect(pipe2(pipefd, O_NONBLOCK | O_CLOEXEC) == 0, "create anonymous heartbeat pipe");
    snprintf(value, sizeof(value), "%d", pipefd[1]);
    setenv(C1_HEARTBEAT_FD_ENV, value, 1);
    c1_liveness_init();
    c1_liveness_beat(1000);
    expect(read(pipefd[0], &byte, 1U) == 1 && byte == 'H', "loop sends an anonymous pipe heartbeat");
    c1_liveness_beat(1001);
    expect(read(pipefd[0], &byte, 1U) < 0 && errno == EAGAIN, "heartbeat rate is low even in busy loops");
    {
        pid_t child = fork();
        if (child == 0) { c1_liveness_beat(4000); _exit(0); }
        waitpid(child, NULL, 0);
        expect(read(pipefd[0], &byte, 1U) < 0 && errno == EAGAIN,
               "forked workers cannot fake UI loop progress");
    }
    expect((fcntl(pipefd[1], F_GETFD) & FD_CLOEXEC) != 0, "heartbeat cannot leak to exec applications");
    close(pipefd[0]);
    c1_liveness_beat(5000); /* Closed reader must not kill the UI with SIGPIPE. */
    c1_liveness_close();
}

int main(int argc, char **argv)
{
    char self[4096];
    ssize_t count;
    if (argc > 1) return fixture(argc, argv);
    count = readlink("/proc/self/exe", self, sizeof(self)-1U);
    if (count <= 0) return 1;
    self[count] = '\0';
    expect(c1_descendants_adopt() == 0, "test can reap the deliberately orphaned supervisor");
    test_cleanup(self, "--foreground");
    test_cleanup(self, "--shell-first");
    test_cleanup(self, "--detached");
    test_parent_crash(self);
    test_busy_terminal_app();
    test_heartbeat();
    if (failures) return 1;
    puts("all terminal lifecycle tests passed");
    return 0;
}
