/* Test-only driver; production metrics has no environment clock override. */
#include <time.h>
#include <stdlib.h>
static time_t fixture_time(time_t *out)
{
    const char *text = getenv("C1PKG_METRICS_TEST_TIME");
    time_t value = text != NULL ? (time_t)strtoll(text, NULL, 10) : time(NULL);
    if (out != NULL) *out = value;
    return value;
}
#define time fixture_time
#include "../src/pkg/metrics.c"
#undef time

int main(int argc, char **argv)
{
    struct c1pkg_config config = {argc > 2 ? argv[2] : NULL, NULL};
    struct c1pkg_metrics_snapshot snapshot = {0};
    struct c1pkg_metrics_job *job;
    size_t i;
    if (argc == 2 && strcmp(argv[1], "parse") == 0) {
        unsigned char input[C1PKG_METRICS_BODY_MAX + 1U];
        size_t size = fread(input, 1U, sizeof(input), stdin);
        if (c1pkg_metrics_parse(input, size, &snapshot) != 0) return 3;
    } else if (argc == 5 && strcmp(argv[1], "record") == 0) {
        struct c1pkg_package package = {0};
        if (strlen(argv[3]) >= sizeof(package.id) || strlen(argv[4]) >= sizeof(package.version)) return 2;
        strcpy(package.id, argv[3]); strcpy(package.version, argv[4]);
        return c1pkg_metrics_record_install(&config, &package) == 0 ? 0 : 2;
    } else if (argc == 3 && (strcmp(argv[1], "sync") == 0 || strcmp(argv[1], "cancel") == 0 ||
                            strcmp(argv[1], "inherit") == 0)) {
        uint64_t start = monotonic_ms(), tick;
        int result, inherited = -1;
        if (strcmp(argv[1], "inherit") == 0) {
            inherited = open(C1PKG_STATE_ROOT "/inherited.lock", O_RDWR | O_CREAT, 0600);
            if (inherited < 0 || flock(inherited, LOCK_EX | LOCK_NB) != 0) return 12;
        }
        job = c1pkg_metrics_start(&config);
        if (job == NULL) return 4;
        if (inherited >= 0) {
            (void)close(inherited);
            (void)poll(NULL, 0U, 100);
            inherited = open(C1PKG_STATE_ROOT "/inherited.lock", O_RDWR);
            if (inherited < 0 || flock(inherited, LOCK_EX | LOCK_NB) != 0) return 13;
            (void)close(inherited);
        }
        if (monotonic_ms() - start > 250U) return 5;
        if (strcmp(argv[1], "cancel") == 0) {
            (void)poll(NULL, 0U, 100);
            c1pkg_metrics_cancel(job);
        }
        do {
            tick = monotonic_ms();
            result = c1pkg_metrics_poll(&job, &snapshot);
            if (monotonic_ms() - tick > 250U) return 6;
            if (result == 0) (void)poll(NULL, 0U, 10);
        } while (result == 0 && monotonic_ms() - start < 30000U);
        if (job != NULL) return 7;
        if (strcmp(argv[1], "cancel") == 0) {
            int status;
            if (result != -1 || waitpid(-1, &status, WNOHANG) >= 0 || errno != ECHILD) return 8;
            puts("cancelled and reaped");
            return 0;
        }
        if (result != 1) return 9;
    } else return 2;
    printf("available=%d count=%lu\n", snapshot.available, (unsigned long)snapshot.count);
    for (i = 0U; i < snapshot.count; ++i) {
        uint64_t count = UINT64_MAX;
        if (!c1pkg_metrics_lookup(&snapshot, snapshot.items[i].id, &count)) return 10;
        printf("%s\t%llu\n", snapshot.items[i].id, (unsigned long long)count);
    }
    {
        uint64_t sentinel = 73U;
        if (c1pkg_metrics_lookup(&snapshot, "not-present", &sentinel) || sentinel != 73U) return 11;
    }
    return 0;
}
