/* Test-only bridge for the real C client <-> local Go v2 handler.
 * No production clock override and no access to a connected device. */
#define _POSIX_C_SOURCE 200809L
#include <stdlib.h>
#include <time.h>
static time_t bridge_time(time_t *out)
{
    const char *text = getenv("C1PKG_METRICS_TEST_TIME");
    time_t value = text ? (time_t)strtoll(text, NULL, 10) : time(NULL);
    if (out) *out = value;
    return value;
}
#define time bridge_time
#include "../src/pkg/metrics.c"
#undef time

int main(int argc, char **argv)
{
    struct c1pkg_config config = {argc > 2 ? argv[2] : NULL, NULL};
    struct c1pkg_metrics_snapshot snapshot = {0};
    struct c1pkg_metrics_job *job;
    if (argc == 5 && (!strcmp(argv[1], "record") || !strcmp(argv[1], "record-legacy"))) {
        struct c1pkg_package package = {0};
        if (strlen(argv[3]) >= sizeof(package.id) || strlen(argv[4]) >= sizeof(package.version)) return 2;
        strcpy(package.id, argv[3]); strcpy(package.version, argv[4]);
        int result = !strcmp(argv[1], "record") ?
            c1pkg_device_metrics_record_install(&config, &package) :
            c1pkg_metrics_record_install(&config, &package);
        return result == 0 ? 0 : 3;
    }
    if (argc != 3 || strcmp(argv[1], "sync")) return 2;
    job = c1pkg_device_metrics_start(&config);
    if (job == NULL) return 4;
    uint64_t started = monotonic_ms();
    for (;;) {
        int result = c1pkg_device_metrics_poll(&job, &snapshot);
        if (result != 0) {
            if (result != 1) return 5;
            break;
        }
        if (monotonic_ms() - started > 30000U) c1pkg_device_metrics_cancel(job);
        (void)poll(NULL, 0U, 10);
    }
    printf("available=%d count=%lu\n", snapshot.available, (unsigned long)snapshot.count);
    for (size_t i = 0U; i < snapshot.count; ++i)
        printf("%s\t%llu\n", snapshot.items[i].id, (unsigned long long)snapshot.items[i].installations);
    return 0;
}
