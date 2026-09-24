#define _POSIX_C_SOURCE 200809L
#include "services/input_service.h"

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    char root[PATH_MAX];
    char socket_path[PATH_MAX];
    char shared[PATH_MAX];
    char user[PATH_MAX];
    c1_input_service service;
} fixture;

static char self_exe[PATH_MAX];
static volatile sig_atomic_t terminated;

static int64_t now_ms(void)
{
    struct timespec now;
    assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static void delay_ms(long milliseconds)
{
    struct timespec delay = {milliseconds / 1000, (milliseconds % 1000) * 1000000};
    while (nanosleep(&delay, &delay) != 0) assert(errno == EINTR);
}

static void path_join(char *out, size_t size, const char *directory, const char *name)
{
    int length = snprintf(out, size, "%s/%s", directory, name);
    assert(length > 0 && (size_t)length < size);
}

static void write_text(const char *directory, const char *name, const char *text)
{
    char path[PATH_MAX];
    int fd;
    path_join(path, sizeof(path), directory, name);
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    assert(fd >= 0);
    assert(write(fd, text, strlen(text)) == (ssize_t)strlen(text));
    assert(close(fd) == 0);
}

static void assert_text(const char *directory, const char *name, const char *text)
{
    char path[PATH_MAX], buffer[128] = {0};
    int fd;
    path_join(path, sizeof(path), directory, name);
    fd = open(path, O_RDONLY);
    assert(fd >= 0);
    assert(read(fd, buffer, sizeof(buffer) - 1) == (ssize_t)strlen(text));
    assert(strcmp(buffer, text) == 0);
    assert(close(fd) == 0);
}

static void term_handler(int signal_number)
{
    (void)signal_number;
    terminated = 1;
}

static int bind_socket(const char *path)
{
    struct sockaddr_un address = {0};
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    assert(fd >= 0);
    address.sun_family = AF_UNIX;
    assert(strlen(path) < sizeof(address.sun_path));
    strcpy(address.sun_path, path);
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
        close(fd);
        return -1;
    }
    assert(listen(fd, 2) == 0);
    return fd;
}

/* The executable re-execs itself as a worker. All arguments must point inside
 * this test's mkdtemp directory; no real service, global socket, or data is used. */
static int fake_worker(int argc, char **argv)
{
    char root[PATH_MAX], expected[PATH_MAX], mode[40] = {0};
    struct stat null_info, fd_info;
    struct sigaction action = {0}, inherited;
    sigset_t mask;
    int fd, listener, death_signal = 0;
    DIR *fds;
    struct dirent *entry;

    assert(argc == 7 && strcmp(argv[1], "--socket") == 0);
    assert(strcmp(argv[3], "--shared-data") == 0);
    assert(strcmp(argv[5], "--user-data") == 0);
    assert(strncmp(argv[2], "/tmp/c1-input-service-", 22) == 0);
    assert(strlen(argv[2]) < sizeof(root));
    strcpy(root, argv[2]);
    assert(strrchr(root, '/') != NULL);
    *strrchr(root, '/') = '\0';
    path_join(expected, sizeof(expected), root, "socket");
    assert(strcmp(argv[2], expected) == 0);
    path_join(expected, sizeof(expected), root, "shared");
    assert(strcmp(argv[4], expected) == 0);
    path_join(expected, sizeof(expected), root, "user");
    assert(strcmp(argv[6], expected) == 0);

    assert(prctl(PR_GET_PDEATHSIG, &death_signal) == 0 && death_signal == SIGTERM);
    assert(sigaction(SIGTERM, NULL, &inherited) == 0 && inherited.sa_handler == SIG_DFL);
    assert(sigprocmask(SIG_SETMASK, NULL, &mask) == 0 && !sigismember(&mask, SIGTERM));
    assert(stat("/dev/null", &null_info) == 0);
    for (int i = 0; i <= 2; ++i) {
        assert(fstat(i, &fd_info) == 0 && S_ISCHR(fd_info.st_mode));
        assert(fd_info.st_rdev == null_info.st_rdev);
        assert((fcntl(i, F_GETFL) & O_ACCMODE) == O_RDWR);
    }
    fds = opendir("/proc/self/fd");
    assert(fds != NULL);
    while ((entry = readdir(fds)) != NULL) {
        char *end;
        long number = strtol(entry->d_name, &end, 10);
        if (end != entry->d_name && *end == '\0' && number > 2) {
            assert(number == dirfd(fds));
        }
    }
    assert(closedir(fds) == 0);

    path_join(expected, sizeof(expected), root, "mode");
    fd = open(expected, O_RDONLY);
    assert(fd >= 0 && read(fd, mode, sizeof(mode) - 1) > 0);
    close(fd);
    if (strcmp(mode, "exit") == 0) return 34;
    if (strcmp(mode, "exit-zero") == 0) return 0;
    if (strcmp(mode, "slow") == 0) delay_ms(700);

    action.sa_handler = strcmp(mode, "stubborn") == 0 ? SIG_IGN : term_handler;
    sigemptyset(&action.sa_mask);
    assert(sigaction(SIGTERM, &action, NULL) == 0);
    if (mkdir(argv[6], 0700) != 0) assert(errno == EEXIST);
    assert(stat(argv[6], &fd_info) == 0 && (fd_info.st_mode & 0777) == 0700);
    path_join(expected, sizeof(expected), argv[6], "lock");
    fd = open(expected, O_CREAT | O_EXCL | O_WRONLY, 0600);
    if (fd < 0) return 42;
    assert(close(fd) == 0);
    listener = bind_socket(argv[2]);
    if (listener < 0) return 41;
    write_text(argv[6], "ready", "ready");
    while (!terminated) delay_ms(5);
    delay_ms(75); /* Simulate bounded Rime flush after TERM. */
    write_text(argv[6], "flushed", "flushed");
    close(listener);
    return 0;
}

static void fixture_init(fixture *test, const char *mode)
{
    memset(test, 0, sizeof(*test));
    strcpy(test->root, "/tmp/c1-input-service-XXXXXX");
    assert(mkdtemp(test->root) != NULL);
    path_join(test->socket_path, sizeof(test->socket_path), test->root, "socket");
    path_join(test->shared, sizeof(test->shared), test->root, "shared");
    path_join(test->user, sizeof(test->user), test->root, "user");
    assert(mkdir(test->shared, 0700) == 0);
    write_text(test->root, "mode", mode);
    c1_input_service_init(&test->service);
    test->service.package_root = NULL;
    test->service.prebuilt_only = false;
    test->service.exe_path = self_exe;
    test->service.socket_path = test->socket_path;
    test->service.shared_data_path = test->shared;
    test->service.user_data_path = test->user;
}

static void fixture_destroy(fixture *test)
{
    static const char *files[] = {"user/ready", "user/flushed", "user/lock",
                                  "user/marker", "mode", "socket", "bad-exe"};
    char path[PATH_MAX];
    assert(test->service.pid == -1);
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); ++i) {
        path_join(path, sizeof(path), test->root, files[i]);
        assert(unlink(path) == 0 || errno == ENOENT);
    }
    assert(rmdir(test->user) == 0 || errno == ENOENT);
    assert(rmdir(test->shared) == 0);
    assert(rmdir(test->root) == 0);
}

static void wait_ready(fixture *test)
{
    char path[PATH_MAX];
    int64_t deadline = now_ms() + 4000;
    path_join(path, sizeof(path), test->user, "ready");
    while (access(path, F_OK) != 0) {
        assert(now_ms() < deadline);
        if (test->service.pid > 0) {
            assert(c1_input_service_poll(&test->service) == C1_INPUT_SERVICE_RUNNING);
        }
        delay_ms(5);
    }
}

static void wait_service(c1_input_service *service)
{
    int64_t deadline = now_ms() + 4000;
    while (service->pid > 0) {
        assert(now_ms() < deadline);
        c1_input_service_poll(service);
        delay_ms(5); /* The real caller may service its heartbeat here. */
    }
}

static int wait_child(pid_t child)
{
    int status;
    int64_t deadline = now_ms() + 4000;
    pid_t result;
    while ((result = waitpid(child, &status, WNOHANG)) == 0) {
        assert(now_ms() < deadline);
        delay_ms(5);
    }
    assert(result == child);
    return status;
}

static void test_defaults_and_unavailable(void)
{
    fixture test;
    c1_input_service defaults;
    c1_input_service_init(NULL);
    assert(c1_input_service_start(NULL) == C1_INPUT_SERVICE_FAILED);
    assert(c1_input_service_poll(NULL) == C1_INPUT_SERVICE_FAILED);
    assert(c1_input_service_stop(NULL) == C1_INPUT_SERVICE_FAILED);
    c1_input_service_init(&defaults);
    assert(strcmp(defaults.exe_path, "/storage/c1/apps/c1-ime/current/bin/c1-ime-service") == 0);
    assert(strcmp(defaults.socket_path, "/run/c1-ime/socket") == 0);
    assert(strcmp(defaults.shared_data_path, "/storage/c1/apps/c1-ime/current/share/rime-data") == 0);
    assert(strcmp(defaults.user_data_path, "/storage/c1/ime") == 0);
    assert(strcmp(defaults.package_root, "/storage/c1/apps/c1-ime") == 0);
    assert(defaults.prebuilt_only);
    assert(defaults.pid == -1 && defaults.state == C1_INPUT_SERVICE_IDLE);
    assert(c1_input_service_stop(&defaults) == C1_INPUT_SERVICE_STOPPED);
    assert(c1_input_service_start(&defaults) == C1_INPUT_SERVICE_STOPPED);

    fixture_init(&test, "exit");
    test.service.exe_path = test.socket_path; /* Deliberately absent, never global. */
    assert(c1_input_service_start(&test.service) == C1_INPUT_SERVICE_UNAVAILABLE);
    assert(test.service.last_error == ENOENT && test.service.pid == -1);
    assert(waitpid(-1, NULL, WNOHANG) == -1 && errno == ECHILD);
    test.service.exe_path = self_exe;
    assert(c1_input_service_start(&test.service) == C1_INPUT_SERVICE_UNAVAILABLE);
    assert(c1_input_service_poll(&test.service) == C1_INPUT_SERVICE_UNAVAILABLE);
    assert(c1_input_service_stop(&test.service) == C1_INPUT_SERVICE_UNAVAILABLE);
    fixture_destroy(&test);

    fixture_init(&test, "exit");
    test.service.exe_path = test.shared; /* An executable directory is not a binary. */
    assert(c1_input_service_start(&test.service) == C1_INPUT_SERVICE_UNAVAILABLE);
    assert(waitpid(-1, NULL, WNOHANG) == -1 && errno == ECHILD);
    fixture_destroy(&test);

    fixture_init(&test, "exit");
    test.service.socket_path = NULL;
    assert(c1_input_service_start(&test.service) == C1_INPUT_SERVICE_FAILED);
    assert(test.service.last_error == EINVAL && test.service.pid == -1);
    fixture_destroy(&test);
}

static void test_exec_failures(void)
{
    fixture test;
    char bad[PATH_MAX];
    fixture_init(&test, "exit");
    path_join(bad, sizeof(bad), test.root, "bad-exe");
    write_text(test.root, "bad-exe", "not an executable format\n");
    test.service.exe_path = bad;
    assert(c1_input_service_start(&test.service) == C1_INPUT_SERVICE_UNAVAILABLE);
    assert(test.service.pid == -1 && !test.service.wait_status_valid);
    assert(chmod(bad, 0700) == 0);
    c1_input_service_init(&test.service);
    test.service.package_root = NULL;
    test.service.prebuilt_only = false;
    test.service.exe_path = bad;
    test.service.socket_path = test.socket_path;
    test.service.shared_data_path = test.shared;
    test.service.user_data_path = test.user;
    assert(c1_input_service_start(&test.service) == C1_INPUT_SERVICE_RUNNING);
    wait_service(&test.service);
    assert(test.service.state == C1_INPUT_SERVICE_FAILED && test.service.wait_status_valid);
    assert(WIFEXITED(test.service.wait_status) && WEXITSTATUS(test.service.wait_status) == 127);
    assert(c1_input_service_start(&test.service) == C1_INPUT_SERVICE_FAILED);
    fixture_destroy(&test);

    for (int zero = 0; zero < 2; ++zero) {
        fixture_init(&test, zero ? "exit-zero" : "exit");
        assert(c1_input_service_start(&test.service) == C1_INPUT_SERVICE_RUNNING);
        wait_service(&test.service);
        assert(test.service.state == C1_INPUT_SERVICE_FAILED);
        assert(WIFEXITED(test.service.wait_status));
        assert(WEXITSTATUS(test.service.wait_status) == (zero ? 0 : 34));
        assert(c1_input_service_start(&test.service) == C1_INPUT_SERVICE_FAILED);
        fixture_destroy(&test);
    }
}

static void test_nonblocking_and_graceful_stop(void)
{
    fixture test;
    struct sigaction ignored = {0}, original;
    sigset_t blocked, original_mask;
    int pipe_fds[2], high_fd;
    int64_t before, deadline;
    pid_t child;
    fixture_init(&test, "slow");
    assert(pipe(pipe_fds) == 0);
    high_fd = fcntl(pipe_fds[1], F_DUPFD, 512);
    assert(high_fd >= 512);
    ignored.sa_handler = SIG_IGN;
    sigemptyset(&ignored.sa_mask);
    assert(sigaction(SIGTERM, &ignored, &original) == 0);
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGTERM);
    assert(sigprocmask(SIG_BLOCK, &blocked, &original_mask) == 0);
    before = now_ms();
    assert(c1_input_service_start(&test.service) == C1_INPUT_SERVICE_RUNNING);
    assert(now_ms() - before < 500);
    child = test.service.pid;
    assert(c1_input_service_start(&test.service) == C1_INPUT_SERVICE_RUNNING);
    assert(test.service.pid == child);
    assert(sigaction(SIGTERM, &original, NULL) == 0);
    assert(sigprocmask(SIG_SETMASK, &original_mask, NULL) == 0);
    close(pipe_fds[0]);
    close(pipe_fds[1]);
    close(high_fd);
    wait_ready(&test); /* Worker checks exact argv, clean FDs and reset TERM. */
    before = now_ms();
    assert(c1_input_service_stop(&test.service) == C1_INPUT_SERVICE_STOPPING);
    assert(now_ms() - before < 100);
    deadline = test.service.stop_deadline_ms;
    assert(c1_input_service_stop(&test.service) == C1_INPUT_SERVICE_STOPPING);
    assert(test.service.stop_deadline_ms == deadline);
    wait_service(&test.service);
    assert(test.service.state == C1_INPUT_SERVICE_STOPPED && !test.service.kill_sent);
    assert(WIFEXITED(test.service.wait_status) && WEXITSTATUS(test.service.wait_status) == 0);
    assert_text(test.user, "flushed", "flushed");
    assert(access(test.socket_path, F_OK) == 0); /* Supervisor must not unlink it. */
    assert(waitpid(child, NULL, WNOHANG) == -1 && errno == ECHILD);
    assert(c1_input_service_stop(&test.service) == C1_INPUT_SERVICE_STOPPED);
    assert(c1_input_service_start(&test.service) == C1_INPUT_SERVICE_STOPPED);
    fixture_destroy(&test);
}

static void test_forced_stop(void)
{
    fixture test;
    int64_t before;
    fixture_init(&test, "stubborn");
    test.service.stop_grace_ms = 100;
    assert(c1_input_service_start(&test.service) == C1_INPUT_SERVICE_RUNNING);
    wait_ready(&test);
    before = now_ms();
    assert(c1_input_service_stop(&test.service) == C1_INPUT_SERVICE_STOPPING);
    assert(c1_input_service_poll(&test.service) == C1_INPUT_SERVICE_STOPPING);
    assert(!test.service.kill_sent);
    wait_service(&test.service);
    assert(now_ms() - before >= 100 && now_ms() - before < 1500);
    assert(test.service.state == C1_INPUT_SERVICE_STOPPED && test.service.kill_sent);
    assert(WIFSIGNALED(test.service.wait_status) && WTERMSIG(test.service.wait_status) == SIGKILL);
    fixture_destroy(&test);
}

static void test_stop_grace_bounds(void)
{
    fixture test;
    int64_t before;
    fixture_init(&test, "normal");
    test.service.stop_grace_ms = UINT_MAX;
    assert(c1_input_service_start(&test.service) == C1_INPUT_SERVICE_RUNNING);
    wait_ready(&test);
    before = now_ms();
    assert(c1_input_service_stop(&test.service) == C1_INPUT_SERVICE_STOPPING);
    assert(test.service.stop_deadline_ms >= before + C1_INPUT_SERVICE_MAX_STOP_GRACE_MS);
    assert(test.service.stop_deadline_ms <= now_ms() + C1_INPUT_SERVICE_MAX_STOP_GRACE_MS);
    wait_service(&test.service);
    assert(test.service.state == C1_INPUT_SERVICE_STOPPED && !test.service.kill_sent);
    fixture_destroy(&test);

    fixture_init(&test, "stubborn");
    test.service.stop_grace_ms = 0;
    assert(c1_input_service_start(&test.service) == C1_INPUT_SERVICE_RUNNING);
    wait_ready(&test);
    assert(c1_input_service_stop(&test.service) == C1_INPUT_SERVICE_STOPPING);
    assert(!test.service.kill_sent);
    assert(c1_input_service_poll(&test.service) == C1_INPUT_SERVICE_STOPPING);
    assert(test.service.kill_sent);
    wait_service(&test.service);
    assert(test.service.state == C1_INPUT_SERVICE_STOPPED);
    assert(WIFSIGNALED(test.service.wait_status) && WTERMSIG(test.service.wait_status) == SIGKILL);
    fixture_destroy(&test);
}

static void test_external_socket_and_lock(void)
{
    fixture test;
    struct sockaddr_un address = {0};
    int listener, client, accepted;
    fixture_init(&test, "normal");
    listener = bind_socket(test.socket_path);
    assert(listener >= 0);
    assert(c1_input_service_start(&test.service) == C1_INPUT_SERVICE_RUNNING);
    wait_service(&test.service);
    assert(test.service.state == C1_INPUT_SERVICE_FAILED);
    assert(WIFEXITED(test.service.wait_status) && WEXITSTATUS(test.service.wait_status) == 41);
    assert(c1_input_service_stop(&test.service) == C1_INPUT_SERVICE_FAILED);
    address.sun_family = AF_UNIX;
    assert(strlen(test.socket_path) < sizeof(address.sun_path));
    strcpy(address.sun_path, test.socket_path);
    client = socket(AF_UNIX, SOCK_STREAM, 0);
    assert(client >= 0 && connect(client, (struct sockaddr *)&address, sizeof(address)) == 0);
    accepted = accept(listener, NULL, NULL);
    assert(accepted >= 0);
    assert(write(accepted, "ok", 2) == 2);
    {
        char reply[2];
        assert(read(client, reply, sizeof(reply)) == 2 && memcmp(reply, "ok", 2) == 0);
    }
    close(accepted);
    close(client);
    close(listener);
    fixture_destroy(&test);

    fixture_init(&test, "normal");
    assert(mkdir(test.user, 0700) == 0);
    write_text(test.user, "lock", "external-lock");
    write_text(test.user, "marker", "preserved-data");
    assert(c1_input_service_start(&test.service) == C1_INPUT_SERVICE_RUNNING);
    wait_service(&test.service);
    assert(test.service.state == C1_INPUT_SERVICE_FAILED);
    assert(WIFEXITED(test.service.wait_status) && WEXITSTATUS(test.service.wait_status) == 42);
    assert(c1_input_service_stop(&test.service) == C1_INPUT_SERVICE_FAILED);
    assert_text(test.user, "lock", "external-lock");
    assert_text(test.user, "marker", "preserved-data");
    fixture_destroy(&test);
}

static void test_lost_ownership(void)
{
    fixture test;
    fixture_init(&test, "exit");
    assert(c1_input_service_start(&test.service) == C1_INPUT_SERVICE_RUNNING);
    assert(WIFEXITED(wait_child(test.service.pid))); /* Simulate an errant other reaper. */
    assert(c1_input_service_stop(&test.service) == C1_INPUT_SERVICE_FAILED);
    assert(test.service.pid == -1 && test.service.last_error == ECHILD);
    assert(!test.service.wait_status_valid && !test.service.kill_sent);
    fixture_destroy(&test);

    /* Inject a non-child PID as a worst-case stale/reused numeric PID. A blind
     * kill would terminate this test process instead of safely dropping it. */
    c1_input_service_init(&test.service);
    test.service.pid = getpid();
    test.service.state = C1_INPUT_SERVICE_RUNNING;
    assert(c1_input_service_stop(&test.service) == C1_INPUT_SERVICE_FAILED);
    assert(test.service.pid == -1 && test.service.last_error == ECHILD);
}

static void test_parent_death(void)
{
    int previous_subreaper;
    assert(prctl(PR_GET_CHILD_SUBREAPER, &previous_subreaper) == 0);
    assert(prctl(PR_SET_CHILD_SUBREAPER, 1) == 0);
    for (int attempt = 0; attempt < 6; ++attempt) {
        fixture test;
        int report[2], release[2], status;
        pid_t owner, worker;
        fixture_init(&test, "normal");
        assert(pipe(report) == 0 && pipe(release) == 0);
        owner = fork();
        assert(owner >= 0);
        if (owner == 0) {
            char byte;
            close(report[0]);
            close(release[1]);
            assert(c1_input_service_start(&test.service) == C1_INPUT_SERVICE_RUNNING);
            assert(write(report[1], &test.service.pid, sizeof(pid_t)) == (ssize_t)sizeof(pid_t));
            if (attempt == 0) assert(read(release[0], &byte, 1) == 1);
            _exit(0);
        }
        close(report[1]);
        close(release[0]);
        assert(read(report[0], &worker, sizeof(worker)) == (ssize_t)sizeof(worker));
        close(report[0]);
        if (attempt == 0) {
            wait_ready(&test);
            assert(write(release[1], "x", 1) == 1);
        }
        close(release[1]);
        status = wait_child(owner);
        assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
        status = wait_child(worker); /* Adopted by this isolated test subreaper. */
        if (attempt == 0) {
            assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
            assert_text(test.user, "flushed", "flushed");
        } else {
            assert((WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM) ||
                   (WIFEXITED(status) && (WEXITSTATUS(status) == 126 || WEXITSTATUS(status) == 0)));
        }
        fixture_destroy(&test);
    }
    assert(prctl(PR_SET_CHILD_SUBREAPER, previous_subreaper) == 0);
}

static void test_package_paths(void)
{
    char root[] = "/tmp/c1-input-package-XXXXXX";
    char path[PATH_MAX], current[PATH_MAX], exe[PATH_MAX], shared[PATH_MAX], pinned[PATH_MAX];
    const char *dirs[] = {"versions", "versions/0.1.0", "versions/0.1.0/bin",
                          "versions/0.1.0/share", "versions/0.1.0/share/rime-data"};
    assert(mkdtemp(root));
    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); ++i) {
        path_join(path, sizeof(path), root, dirs[i]);
        assert(mkdir(path, 0755) == 0);
    }
    write_text(root, "versions/0.1.0/bin/c1-ime-service", "test executable");
    path_join(path, sizeof(path), root, "versions/0.1.0/bin/c1-ime-service");
    assert(chmod(path, 0555) == 0);
    path_join(current, sizeof(current), root, "current");
    assert(symlink("versions/0.1.0", current) == 0);
    assert(c1_input_service_package_paths(root, exe, sizeof(exe), shared, sizeof(shared)));
    path_join(pinned, sizeof(pinned), root, "versions/0.1.0/bin/c1-ime-service");
    assert(strcmp(exe, pinned) == 0 && strstr(shared, "/versions/0.1.0/share/rime-data"));
    assert(!c1_input_service_package_paths(root, exe, 1, shared, sizeof(shared)));
    assert(chmod(path, 0777) == 0);
    assert(!c1_input_service_package_paths(root, exe, sizeof(exe), shared, sizeof(shared)));
    assert(chmod(path, 0555) == 0);
    assert(unlink(current) == 0);
    assert(symlink("versions/../0.1.0", current) == 0);
    assert(!c1_input_service_package_paths(root, exe, sizeof(exe), shared, sizeof(shared)));
    assert(unlink(current) == 0);
    assert(symlink("/tmp", current) == 0);
    assert(!c1_input_service_package_paths(root, exe, sizeof(exe), shared, sizeof(shared)));
    assert(unlink(current) == 0);
    assert(symlink("versions/0.1.0", current) == 0);
    assert(chmod(root, 0777) == 0);
    assert(!c1_input_service_package_paths(root, exe, sizeof(exe), shared, sizeof(shared)));
    assert(chmod(root, 0700) == 0);
    assert(unlink(path) == 0);
    assert(symlink(self_exe, path) == 0);
    assert(!c1_input_service_package_paths(root, exe, sizeof(exe), shared, sizeof(shared)));
    assert(unlink(path) == 0 && unlink(current) == 0);
    for (size_t i = sizeof(dirs) / sizeof(dirs[0]); i > 0; --i) {
        path_join(path, sizeof(path), root, dirs[i - 1]);
        assert(rmdir(path) == 0);
    }
    assert(rmdir(root) == 0);
}

int main(int argc, char **argv)
{
    ssize_t length;
    if (argc > 1) return fake_worker(argc, argv);
    alarm(30);
    length = readlink("/proc/self/exe", self_exe, sizeof(self_exe) - 1);
    assert(length > 0 && (size_t)length < sizeof(self_exe) - 1);
    self_exe[length] = '\0';
    test_defaults_and_unavailable();
    test_package_paths();
    test_exec_failures();
    test_nonblocking_and_graceful_stop();
    test_forced_stop();
    test_stop_grace_bounds();
    test_external_socket_and_lock();
    test_lost_ownership();
    test_parent_death();
    assert(waitpid(-1, NULL, WNOHANG) == -1 && errno == ECHILD);
    puts("input service passed: defaults, one-shot start, missing/invalid exe, isolated FDs/argv/signals, "
         "nonblocking TERM/KILL, external socket/lock preserved, PID ownership, parent death/race");
    return 0;
}
