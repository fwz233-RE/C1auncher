/* Cross-process transport driver. Clock injection is test-only; no production
 * override, production state, or real repository URL is used by the runner. */
#ifndef C1PKG_STATE_ROOT
#error "requires isolated compile-time C1PKG_STATE_ROOT"
#endif
#include "pkg_transport_clock.h"
#define clock_gettime test_transport_clock_gettime
#define time test_transport_time
#define poll test_transport_poll
#include "../src/pkg/repo.c"
#undef clock_gettime
#undef time
#undef poll
#include <sys/wait.h>

int c1pkg_storage_state_init(char *error, size_t size)
{
    return c1pkg_mkdir_p(C1PKG_STATE_ROOT, 0700, error, size);
}
static int cancel_countdown;
static int progress(const char *message, void *context)
{
    (void)context;
    return cancel_countdown && message != NULL && strncmp(message, "等待 ", strlen("等待 ")) == 0;
}

int main(int argc, char **argv)
{
    char error[256] = "", url[COOLDOWN_URL_MAX], output[C1PKG_PATH_MAX];
    uint64_t start;
    int result = -1, saved;
    if (argc < 5) return 2;
    test_transport_ms = strtoull(argv[4], NULL, 10);
    test_transport_real_wait = strcmp(argv[4], "real") == 0;
    start = monotonic_milliseconds();
    if (strcmp(argv[2], "-") != 0) {
        struct c1pkg_config config = {argv[2], NULL};
        if (c1pkg_repo_bind_transport(&config) != 0) return 2;
    }
    strcpy(url, argv[3]);
    c1pkg_set_progress(progress, NULL);
    if (strcmp(argv[1], "bind") == 0) {
        result = 0; saved = errno;
    } else if (strcmp(argv[1], "fetch") == 0) {
        (void)snprintf(output, sizeof(output), "%s/download.%ld", C1PKG_STATE_ROOT, (long)getpid());
        result = c1pkg_fetch(url, output, 8192U, error, sizeof(error));
        saved = errno;
        (void)unlink(output);
    } else if (strcmp(argv[1], "fork") == 0) {
        pid_t child = fork();
        int status;
        if (child == 0) {
            int ok = cooldown_open(url, error, sizeof(error));
            if (ok == 0) ok = remember_cooldown(75U);
            cooldown_close();
            _exit(ok == 0 ? 0 : 1);
        }
        if (child <= 0 || waitpid(child, &status, 0) != child || !WIFEXITED(status) || WEXITSTATUS(status) != 0) return 3;
        /* Neither the child's bound scope nor memory deadline is needed. */
        transport_repo_scope[0] = '\0'; transport_not_before = 0U;
        result = cooldown_open(url, error, sizeof(error));
        if (result == 0) result = wait_for_cooldown(error, sizeof(error));
        saved = errno;
        cooldown_close();
    } else {
        result = cooldown_open(url, error, sizeof(error));
        if (result == 0) {
            if (strcmp(argv[1], "set") == 0 && argc == 6)
                result = remember_cooldown(strtoull(argv[5], NULL, 10));
            else if (strcmp(argv[1], "pending") == 0 || strcmp(argv[1], "pending-reboot") == 0) {
                if (cooldown_begin_request() != 0) return 4;
                if (strcmp(argv[1], "pending-reboot") == 0) {
                    strcpy(cooldown_active.record.boot, "12345678-1234-1234-1234-123456789abc");
                    if (cooldown_store() != 0) return 4;
                }
                _exit(77); /* Crash while HTTP is in flight: retain its marker. */
            } else if (strcmp(argv[1], "reboot") == 0) {
                strcpy(cooldown_active.record.boot, "12345678-1234-1234-1234-123456789abc");
                result = cooldown_store();
            } else if (strcmp(argv[1], "hold") == 0) {
                puts("LOCKED"); fflush(stdout);
                (void)getchar();
                result = remember_cooldown(75U);
            } else {
                cancel_countdown = strcmp(argv[1], "cancel") == 0;
                result = wait_for_cooldown(error, sizeof(error));
            }
        }
        saved = errno;
        cooldown_close();
    }
    printf("RESULT %d ERRNO %d WAITED %llu UNTIL %llu\n%s\n", result, saved,
           (unsigned long long)(monotonic_milliseconds() - start), (unsigned long long)transport_not_before, error);
    return 0;
}
