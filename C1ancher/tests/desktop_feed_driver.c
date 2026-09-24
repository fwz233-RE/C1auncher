/* Loopback-only integration driver. test_desktop_feed.py supplies private
 * relative CACHE/SEEN/STATE macros and runs inside its own TemporaryDirectory.
 * Only transport waiting is accelerated; desktop.c and repo.c are production.
 */
#include "pkg_transport_clock.h"
#ifndef C1_DESKTOP_CACHE
#error "desktop feed tests require an isolated C1_DESKTOP_CACHE"
#endif
#ifndef C1_DESKTOP_SEEN
#error "desktop feed tests require an isolated C1_DESKTOP_SEEN"
#endif
#ifndef C1PKG_STATE_ROOT
#error "desktop feed tests require an isolated C1PKG_STATE_ROOT"
#endif
#define clock_gettime test_transport_clock_gettime
#define time test_transport_time
#define poll test_transport_poll
#include "../src/pkg/repo.c"
#undef clock_gettime
#undef time
#undef poll
#include "pkg/desktop.h"
#include "services/desktop_data.h"
#include <sys/wait.h>

/* GNU ld wrapping is local to this executable: route only curl to a fixture
 * that tightens --max-time. Other helper resolution is entirely unchanged. */
const char *__real_c1pkg_helper(const char *absolute, const char *name);
const char *__wrap_c1pkg_helper(const char *absolute, const char *name)
{
    if (!strcmp(name, "curl")) {
        const char *helper = getenv("C1_TEST_DESKTOP_CURL");
        if (!helper || helper[0] != '/') abort();
        return helper;
    }
    return __real_c1pkg_helper(absolute, name);
}

int c1pkg_storage_state_init(char *error, size_t size)
{
    return c1pkg_mkdir_p(C1PKG_STATE_ROOT, 0700, error, size);
}

static void report(int result)
{
    c1_desktop_data cache = {0}, seen = {0};
    bool have_cache = c1_desktop_load(C1_DESKTOP_CACHE, &cache);
    bool have_seen = c1_desktop_load(C1_DESKTOP_SEEN, &seen);
    printf("RESULT %d\nCACHE %d\nSEEN %d\nNEW %u\n", result, have_cache, have_seen,
           c1_desktop_new_count(have_cache ? &cache : NULL, have_seen ? &seen : NULL));
    if (have_cache) {
        printf("SEQUENCE %llu\nCOUNT %u\nSOURCE %s\nDATE %s\nZH %s\nEN %s\n",
               (unsigned long long)cache.sequence, cache.count, cache.source,
               cache.date, cache.quote_zh, cache.quote_en);
        for (unsigned i = 0; i < cache.count; ++i) printf("P %s\n", cache.ids[i]);
    }
    if (have_seen) {
        printf("SEEN_SEQUENCE %llu\nSEEN_COUNT %u\nSEEN_SOURCE %s\n",
               (unsigned long long)seen.sequence, seen.count, seen.source);
        for (unsigned i = 0; i < seen.count; ++i) printf("SEEN_P %s\n", seen.ids[i]);
    }
}

int main(int argc, char **argv)
{
    /* Refuse to operate if an integration mistake substituted device paths. */
    if (strcmp(C1_DESKTOP_CACHE, "desktop-summary.cache") || strcmp(C1_DESKTOP_SEEN, "desktop-seen.cache") ||
        strcmp(C1PKG_STATE_ROOT, "pkg-state")) return 2;
    if (argc < 2) return 2;
    int result = 0;
    if (!strcmp(argv[1], "summary") && argc == 3) {
        struct c1pkg_config config = {argv[2], "/unused-test-public-key"};
        result = c1pkg_desktop_summary(&config);
    } else if (!strcmp(argv[1], "seen") && argc >= 4) {
        struct c1pkg_config config = {argv[2], "/unused-test-public-key"};
        struct c1pkg_index index = {0};
        index.sequence = strtoull(argv[3], NULL, 10);
        index.count = (size_t)(argc-4);
        if (index.count > C1PKG_MAX_PACKAGES) return 2;
        for (size_t i = 0; i < index.count; ++i) {
            if (strlen(argv[i+4]) > C1PKG_ID_MAX) return 2;
            strcpy(index.packages[i].id, argv[i+4]);
            strcpy(index.packages[i].version, "1.0");
        }
        c1pkg_desktop_seen(&config, &index);
    } else if (strcmp(argv[1], "inspect") || argc != 2) return 2;
    report(result);
    int status;
    if (waitpid(-1, &status, WNOHANG) != -1 || errno != ECHILD) {
        fputs("download helper was not reaped\n", stderr);
        return 3;
    }
    /* Transport cooldown persistence has its own suite. Reset only this
     * fixture's transport state so independent child clocks cannot interfere;
     * leave the desktop summary and viewed baseline intact across calls. */
    if (c1pkg_remove_tree(C1PKG_STATE_ROOT, NULL, 0U)) return 4;
    return 0;
}
