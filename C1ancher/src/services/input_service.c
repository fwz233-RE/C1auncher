#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L
#include "services/input_service.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int64_t monotonic_ms(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return -1;
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

void c1_input_service_init(c1_input_service *service)
{
    if (service == NULL) return;
    *service = (c1_input_service){
        .exe_path = C1_INPUT_SERVICE_DEFAULT_EXE,
        .socket_path = C1_INPUT_SERVICE_DEFAULT_SOCKET,
        .shared_data_path = C1_INPUT_SERVICE_DEFAULT_SHARED_DATA,
        .user_data_path = C1_INPUT_SERVICE_DEFAULT_USER_DATA,
        .package_root = C1_INPUT_SERVICE_PACKAGE_ROOT,
        .prebuilt_only = true,
        .stop_grace_ms = C1_INPUT_SERVICE_STOP_GRACE_MS,
        .state = C1_INPUT_SERVICE_IDLE,
        .pid = -1
    };
}

static void prepare_child(pid_t parent)
{
    static const int signals[] = {SIGTERM, SIGINT, SIGHUP, SIGQUIT, SIGPIPE, SIGCHLD};
    struct sigaction action = {0};
    sigset_t empty;
    DIR *fds;
    int null_fd;

    /* Ignored dispositions and blocked signals survive exec. In particular,
     * an inherited ignored/blocked SIGTERM would defeat PR_SET_PDEATHSIG. */
    action.sa_handler = SIG_DFL;
    sigemptyset(&action.sa_mask);
    for (size_t i = 0; i < sizeof(signals) / sizeof(signals[0]); ++i) {
        if (sigaction(signals[i], &action, NULL) != 0) _exit(126);
    }
    sigemptyset(&empty);
    if (sigprocmask(SIG_SETMASK, &empty, NULL) != 0 ||
        prctl(PR_SET_PDEATHSIG, SIGTERM) != 0 || getppid() != parent) {
        _exit(126);
    }

    fds = opendir("/proc/self/fd");
    if (fds == NULL) _exit(126);
    for (;;) {
        struct dirent *entry;
        char *end;
        long fd;
        errno = 0;
        entry = readdir(fds);
        if (entry == NULL) {
            if (errno != 0) _exit(126);
            break;
        }
        fd = strtol(entry->d_name, &end, 10);
        if (end != entry->d_name && *end == '\0' && fd > STDERR_FILENO &&
            fd <= INT_MAX && fd != dirfd(fds)) {
            (void)close((int)fd);
        }
    }
    if (closedir(fds) != 0) _exit(126);
    null_fd = open("/dev/null", O_RDWR);
    if (null_fd < 0) _exit(126);
    for (int fd = STDIN_FILENO; fd <= STDERR_FILENO; ++fd) {
        if (dup2(null_fd, fd) < 0) _exit(126);
    }
    if (null_fd > STDERR_FILENO) (void)close(null_fd);
}

static bool trusted_directory_chain(const char *path)
{
    char copy[PATH_MAX];
    struct stat info;
    size_t length = strlen(path);
    if (length < 2 || length >= sizeof(copy) || path[0] != '/') return false;
    memcpy(copy, path, length + 1);
    for (size_t i = 1; i <= length; ++i) {
        if (copy[i] != '/' && copy[i] != '\0') continue;
        char saved = copy[i];
        copy[i] = '\0';
        if (lstat(copy, &info) != 0 || !S_ISDIR(info.st_mode) ||
            (info.st_uid != 0 && info.st_uid != geteuid()) ||
            /* Sticky root-owned /tmp is acceptable for isolated test stores;
             * an ordinary writable ancestor could substitute installed code. */
            ((info.st_mode & 0022) && !(i < length && info.st_uid == 0 &&
                                      (info.st_mode & S_ISVTX)))) return false;
        copy[i] = saved;
    }
    return true;
}

bool c1_input_service_package_paths(const char *root, char *exe, size_t exe_size,
                                    char *shared, size_t shared_size)
{
    char current[PATH_MAX], target[64], base[PATH_MAX], directory[PATH_MAX];
    struct stat info;
    ssize_t length;
    int count;
    if (!root || !exe || !shared || !trusted_directory_chain(root)) return false;
    count = snprintf(current, sizeof(current), "%s/current", root);
    if (count < 0 || (size_t)count >= sizeof(current)) return false;
    if (lstat(current, &info) != 0 || !S_ISLNK(info.st_mode) || info.st_uid != geteuid()) return false;
    length = readlink(current, target, sizeof(target) - 1);
    if (length < 10 || (size_t)length >= sizeof(target) - 1) return false;
    target[length] = '\0';
    if (strncmp(target, "versions/", 9) || strlen(target + 9) > 48 ||
        target[9] == '.' || target[length - 1] == '.' || strstr(target + 9, "..")) return false;
    for (const char *p = target + 9; *p; ++p) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '.' || *p == '_' || *p == '+' || *p == '-')) return false;
    }
    count = snprintf(base, sizeof(base), "%s/%s", root, target);
    if (count < 0 || (size_t)count >= sizeof(base)) return false;
    count = snprintf(directory, sizeof(directory), "%s/bin", base);
    if (count < 0 || (size_t)count >= sizeof(directory) || !trusted_directory_chain(directory)) return false;
    count = snprintf(exe, exe_size, "%s/bin/c1-ime-service", base);
    if (count < 0 || (size_t)count >= exe_size || lstat(exe, &info) != 0 ||
        !S_ISREG(info.st_mode) || info.st_uid != geteuid() ||
        (info.st_mode & 0022) || !(info.st_mode & S_IXUSR)) return false;
    count = snprintf(shared, shared_size, "%s/share/rime-data", base);
    if (count < 0 || (size_t)count >= shared_size || !trusted_directory_chain(shared)) return false;
    return true;
}

static bool valid_path(const char *path)
{
    return path != NULL && path[0] == '/';
}

c1_input_service_state c1_input_service_start(c1_input_service *service)
{
    struct stat info;
    pid_t parent, child;
    if (service == NULL) return C1_INPUT_SERVICE_FAILED;
    if (service->state != C1_INPUT_SERVICE_IDLE) return service->state;
    if (!valid_path(service->exe_path) || !valid_path(service->socket_path) ||
        !valid_path(service->shared_data_path) || !valid_path(service->user_data_path)) {
        service->last_error = EINVAL;
        service->state = C1_INPUT_SERVICE_FAILED;
        return service->state;
    }
    if (service->package_root && !strcmp(service->package_root, C1_INPUT_SERVICE_PACKAGE_ROOT)) {
        struct stat system, storage;
        if (stat("/", &system) != 0 || lstat("/storage", &storage) != 0 ||
            !S_ISDIR(storage.st_mode) || system.st_dev == storage.st_dev) {
            service->last_error = ENODEV;
            service->state = C1_INPUT_SERVICE_UNAVAILABLE;
            return service->state;
        }
    }
    if (stat(service->exe_path, &info) != 0) {
        service->last_error = errno;
        service->state = C1_INPUT_SERVICE_UNAVAILABLE;
        return service->state;
    }
    if (!S_ISREG(info.st_mode)) {
        service->last_error = EACCES;
        service->state = C1_INPUT_SERVICE_UNAVAILABLE;
        return service->state;
    }
    if (access(service->exe_path, X_OK) != 0) {
        service->last_error = errno;
        service->state = C1_INPUT_SERVICE_UNAVAILABLE;
        return service->state;
    }
    parent = getpid();
    child = fork();
    if (child == 0) {
        char exe[PATH_MAX], shared[PATH_MAX];
        const char *program = service->exe_path, *data = service->shared_data_path;
        const struct rlimit no_core_dump = {0, 0};
        prepare_child(parent);
        if (setrlimit(RLIMIT_CORE, &no_core_dump) != 0) _exit(126);
        if (service->package_root) {
            if (!c1_input_service_package_paths(service->package_root, exe, sizeof(exe),
                                                shared, sizeof(shared))) _exit(126);
            program = exe;
            data = shared;
        }
        /* Passing NULL ends argv in ordinary/source-dictionary test mode. */
        execl(program, program,
              "--socket", service->socket_path,
              "--shared-data", data,
              "--user-data", service->user_data_path,
              service->prebuilt_only ? "--prebuilt-only" : (char *)NULL, (char *)NULL);
        _exit(127);
    }
    if (child < 0) {
        service->last_error = errno;
        service->state = C1_INPUT_SERVICE_FAILED;
        return service->state;
    }
    service->pid = child;
    service->state = C1_INPUT_SERVICE_RUNNING;
    return service->state;
}

/* Only a zero waitpid result proves we still own an unreaped direct child.
 * EINTR/other errors defer all signals; ECHILD permanently drops ownership. */
static bool child_owned(c1_input_service *service)
{
    int status;
    pid_t result;
    if (service->pid <= 0) return false;
    result = waitpid(service->pid, &status, WNOHANG);
    if (result == 0) return true;
    if (result == service->pid) {
        service->pid = -1;
        service->wait_status = status;
        service->wait_status_valid = true;
        service->state = service->state == C1_INPUT_SERVICE_STOPPING
                             ? C1_INPUT_SERVICE_STOPPED : C1_INPUT_SERVICE_FAILED;
    } else if (result < 0) {
        service->last_error = errno;
        if (errno == ECHILD) {
            service->pid = -1;
            service->state = C1_INPUT_SERVICE_FAILED;
        }
    }
    return false;
}

c1_input_service_state c1_input_service_poll(c1_input_service *service)
{
    int64_t now;
    if (service == NULL) return C1_INPUT_SERVICE_FAILED;
    if (!child_owned(service)) return service->state;
    if (service->state != C1_INPUT_SERVICE_STOPPING || service->kill_sent) {
        return service->state;
    }
    now = monotonic_ms();
    if (now < 0 || now >= service->stop_deadline_ms) {
        if (kill(service->pid, SIGKILL) != 0) service->last_error = errno;
        else service->kill_sent = true;
    }
    return service->state;
}

c1_input_service_state c1_input_service_stop(c1_input_service *service)
{
    int64_t now;
    unsigned int grace;
    if (service == NULL) return C1_INPUT_SERVICE_FAILED;
    if (service->state == C1_INPUT_SERVICE_IDLE) {
        service->state = C1_INPUT_SERVICE_STOPPED;
        return service->state;
    }
    if (service->state == C1_INPUT_SERVICE_STOPPING) {
        return c1_input_service_poll(service);
    }
    if (!child_owned(service)) return service->state;
    grace = service->stop_grace_ms;
    if (grace > C1_INPUT_SERVICE_MAX_STOP_GRACE_MS) {
        grace = C1_INPUT_SERVICE_MAX_STOP_GRACE_MS;
    }
    now = monotonic_ms();
    service->stop_deadline_ms = now < 0 ? 0 : now + grace;
    service->state = C1_INPUT_SERVICE_STOPPING;
    if (kill(service->pid, SIGTERM) != 0) service->last_error = errno;
    return service->state;
}
