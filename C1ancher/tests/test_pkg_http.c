/* Isolated HTTP driver: compiled by test_pkg_http.py, never uses device state. */
#include "pkg_transport_clock.h"
#ifndef C1PKG_STATE_ROOT
#error "HTTP tests require an isolated compile-time state root"
#endif
#define clock_gettime test_transport_clock_gettime
#define time test_transport_time
#define poll test_transport_poll
#include "../src/pkg/repo.c"
#undef clock_gettime
#undef time
#undef poll
#include <sys/wait.h>

static const char *download_path;
static uint64_t started, largest;
static unsigned long cancel_ms;
static int cancel_resume;

int c1pkg_storage_state_init(char *error, size_t error_size)
{
    return c1pkg_mkdir_p(C1PKG_STATE_ROOT, 0700, error, error_size);
}

static uint64_t milliseconds(void)
{
    struct timespec now;
    (void)clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
}

static int progress(const char *message, void *context)
{
    struct stat info;
    (void)context;
    if (stat(download_path, &info) == 0 && info.st_size >= 0 && (uint64_t)info.st_size > largest)
        largest = (uint64_t)info.st_size;
    if (message != NULL) printf("STAGE %s\n", message);
    return (cancel_ms != 0U && milliseconds() - started >= cancel_ms) ||
           (cancel_resume && message != NULL && strstr(message, "正在继续下载") != NULL);
}

int main(int argc, char **argv)
{
    char error[256] = "";
    int result, error_number;
    uint64_t limit;
    if (argc != 6) return 2;
    download_path = argv[2];
    limit = (uint64_t)strtoull(argv[3], NULL, 10);
    cancel_ms = strtoul(argv[5], NULL, 10);
    test_transport_real_wait = cancel_ms != 0U;
    cancel_resume = strcmp(argv[4], "cancel-resume") == 0;
    started = milliseconds();
    c1pkg_set_progress(progress, NULL);
    if (strcmp(argv[4], "io-error") == 0) {
        int fd = open("/dev/full", O_WRONLY | O_CLOEXEC);
        char *arguments[] = {"/bin/sh", "-c", "printf x", NULL};
        uint64_t received;
        int exit_code;
        if (fd < 0) return 2;
        result = c1pkg_run_bounded(arguments, fd, limit, &received, &exit_code, error, sizeof(error));
        error_number = errno;
        (void)close(fd);
    } else {
        result = strcmp(argv[4], "metadata") == 0 ?
            fetch_endpoint(argv[1], argv[2], limit, 1U, error, sizeof(error)) :
            c1pkg_fetch(argv[1], argv[2], limit, error, sizeof(error));
        if (result == 0 && strcmp(argv[4], "consecutive") == 0)
            result = c1pkg_fetch(argv[1], argv[2], limit, error, sizeof(error));
        error_number = errno;
    }
    {
        int status;
        if (waitpid(-1, &status, WNOHANG) != -1 || errno != ECHILD) {
            fputs("download helper was not reaped\n", stderr);
            return 3;
        }
    }
    (void)progress(NULL, NULL);
    printf("WAITED %llu\n", (unsigned long long)(test_transport_ms - 100000U));
    printf("RESULT %d ERRNO %d MAXSIZE %llu ELAPSED %llu\nERROR %s\n", result, error_number,
           (unsigned long long)largest, (unsigned long long)(milliseconds() - started), error);
    (void)c1pkg_remove_tree(C1PKG_STATE_ROOT, NULL, 0U);
    return 0;
}
