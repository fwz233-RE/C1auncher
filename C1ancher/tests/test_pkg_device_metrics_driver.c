/* Isolated v2 API/clock driver, including the existing v1 reference driver. */
#define main legacy_metrics_driver_main
#include "test_pkg_metrics_driver.c"
#undef main

int main(int argc, char **argv)
{
    struct c1pkg_config config = {argc > 2 ? argv[2] : NULL, NULL};
    struct c1pkg_metrics_snapshot snapshot = {0};
    size_t i;
    if (argc > 2 && strcmp(argv[1], "legacy") == 0)
        return legacy_metrics_driver_main(argc - 1, argv + 1);
    if (argc == 2 && strcmp(argv[1], "parse") == 0) {
        unsigned char input[C1PKG_METRICS_BODY_MAX + 1U];
        size_t size = fread(input, 1U, sizeof(input), stdin);
        if (c1pkg_device_metrics_parse(input, size, &snapshot) != 0) return 3;
    } else if (argc == 5 && strcmp(argv[1], "record") == 0) {
        struct c1pkg_package package = {0};
        if (strlen(argv[3]) >= sizeof(package.id) || strlen(argv[4]) >= sizeof(package.version)) return 2;
        strcpy(package.id, argv[3]); strcpy(package.version, argv[4]);
        return c1pkg_device_metrics_record_install(&config, &package) == 0 ? 0 : 2;
    } else if (argc == 3 && strcmp(argv[1], "next") == 0) {
        /* Pure local queue selection: NEVER contacts this repository URL. */
        struct device_event event;
        char repo[REPO_MAX + 1U];
        int root, result;
        if (normalize_repo(config.repo_base, repo) != 0) return 2;
        root = open_root();
        if (root < 0) return 2;
        result = next_device_event(root, repo, &event);
        (void)close(root);
        if (result != 0) return 2;
        fputs(event.wire.body, stdout);
        return 0;
    } else if (argc == 4 && strcmp(argv[1], "legacy-gate") == 0) {
        struct metrics_cache cache = {0};
        int root = open_root(), result;
        if (root < 0 || normalize_repo(config.repo_base, cache.repo) != 0 || decimal(argv[3], &cache.retry_at) != 0) return 2;
        strcpy(cache.magic, CACHE_MAGIC);
        cache.snapshot.available = 1; cache.snapshot.count = 1U;
        strcpy(cache.snapshot.items[0].id, "app"); cache.snapshot.items[0].installations = 99999U;
        result = atomic_file(root, "cache.tmp", "cache", &cache, sizeof(cache));
        (void)close(root);
        return result == 0 ? 0 : 2;
    } else if (argc == 3 && (strcmp(argv[1], "sync") == 0 || strcmp(argv[1], "cancel") == 0)) {
        struct c1pkg_metrics_job *job;
        uint64_t start = monotonic_ms();
        int result;
        job = c1pkg_device_metrics_start(&config);
        if (job == NULL || monotonic_ms() - start > 250U) return 4;
        if (strcmp(argv[1], "cancel") == 0) {
            (void)poll(NULL, 0U, 100);
            c1pkg_device_metrics_cancel(job);
        }
        do {
            uint64_t tick = monotonic_ms();
            result = c1pkg_device_metrics_poll(&job, &snapshot);
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
    for (i = 0U; i < snapshot.count; ++i)
        printf("%s\t%llu\n", snapshot.items[i].id, (unsigned long long)snapshot.items[i].installations);
    return 0;
}
