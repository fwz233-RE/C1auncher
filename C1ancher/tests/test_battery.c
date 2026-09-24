#define _DEFAULT_SOURCE 1
#include "services/battery.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const int64_t base = C1_BATTERY_MIN_TIME;

static bool observe(c1_battery_history *h, int64_t seconds, bool available)
{
    return c1_battery_history_observe(h, base + seconds, seconds * 1000,
                                      available, 80, C1_BATTERY_DISCHARGING);
}

static void test_sampling(void)
{
    c1_battery_history h = {0};
    assert(!c1_battery_history_observe(&h, 0, 0, true, 80, C1_BATTERY_DISCHARGING));
    assert(!c1_battery_history_observe(&h, base, -1, true, 80, C1_BATTERY_DISCHARGING));
    assert(observe(&h, 7, true));
    assert(h.count == 1 && h.samples[0].timestamp == base + 7 && !h.samples[0].connected);
    for (int i = 1; i < 6; ++i) assert(!observe(&h, 7 + i * 10, true));
    assert(c1_battery_history_observe(&h, base + 67, 67000, true, 78, C1_BATTERY_PLUGGED));
    assert(h.count == 2 && h.samples[1].connected && h.samples[1].power == C1_BATTERY_PLUGGED);
    assert(h.samples[0].timestamp == base + 7 && h.samples[0].percent == 80);
    /* No invented points and no line across suspend, restart or unavailable sensors. */
    assert(observe(&h, 367, true));
    assert(h.count == 3 && !h.samples[2].connected);
    for (int i = 1; i < 6; ++i) assert(!observe(&h, 367 + i * 10, true));
    assert(observe(&h, 427, true));
    assert(h.count == 4 && h.samples[3].connected);
    assert(!observe(&h, 437, false));
    assert(!h.continuous);
    assert(!observe(&h, 447, true));
    assert(!observe(&h, 457, true));
    assert(!observe(&h, 467, true));
    assert(!observe(&h, 477, true));
    assert(observe(&h, 487, true));
    assert(!h.samples[4].connected); /* Good same-minute reads cannot erase a gap. */
    assert(!c1_battery_history_observe(&h, base + 497, 497000, true, 101, C1_BATTERY_DISCHARGING));
    assert(!c1_battery_history_observe(&h, base + 507, 507000, true, 75, (c1_battery_power)99));
    /* A wall/monotonic mismatch breaks continuity even with adjacent buckets. */
    assert(c1_battery_history_observe(&h, base + 547, 517000, true, 75, C1_BATTERY_DISCHARGING));
    assert(!h.samples[5].connected);

    c1_battery_history_init(&h);
    for (int seconds = 7; seconds <= (int)(C1_BATTERY_HISTORY_POINTS + 10U) * 60 + 7; seconds += 10)
        assert(observe(&h, seconds, true) == (seconds % 60 == 7));
    assert(h.count == C1_BATTERY_HISTORY_POINTS);
    assert(h.samples[0].timestamp == base + 11 * 60 + 7 && !h.samples[0].connected);
    assert(h.samples[h.count - 1U].connected);
    assert(observe(&h, 3 * 86400, false) && h.count == 0);
    assert(observe(&h, 3 * 86400 + 60, true));
    assert(c1_battery_history_observe(&h, base, 300000000, true, 20, C1_BATTERY_DISCHARGING));
    assert(h.count == 1 && h.samples[0].timestamp == base && !h.samples[0].connected);
}

static void put(const char *path, const char *data)
{
    FILE *f = fopen(path, "w"); assert(f);
    assert(fputs(data, f) >= 0 && fclose(f) == 0);
    assert(chmod(path, 0600) == 0);
}

static void test_cache(void)
{
    char directory[] = "/tmp/c1-battery-XXXXXX", path[256], link[256];
    assert(mkdtemp(directory));
    snprintf(path, sizeof(path), "%s/history", directory);
    snprintf(link, sizeof(link), "%s/link", directory);
    c1_battery_history h = {0}, loaded;
    assert(!c1_battery_history_load(path, &loaded) && !loaded.count);
    assert(c1_battery_history_save(path, &h));
    assert(c1_battery_history_load(path, &loaded) && !loaded.count);
    for (unsigned i = 0; i < C1_BATTERY_HISTORY_POINTS; ++i) assert(observe(&h, i * 60 + 7, true));
    assert(h.count == C1_BATTERY_HISTORY_POINTS);
    assert(c1_battery_history_save(path, &h));
    assert(c1_battery_history_load(path, &loaded));
    assert(loaded.count == h.count);
    for (size_t i = 0; i < h.count; ++i) {
        assert(loaded.samples[i].timestamp == h.samples[i].timestamp);
        assert(loaded.samples[i].percent == h.samples[i].percent);
        assert(loaded.samples[i].power == h.samples[i].power);
        assert(loaded.samples[i].connected == h.samples[i].connected);
    }
    assert(!loaded.continuous && !loaded.last_wall);
    assert(observe(&loaded, C1_BATTERY_HISTORY_POINTS * 60 + 7, true));
    assert(loaded.count == C1_BATTERY_HISTORY_POINTS && !loaded.samples[loaded.count - 1].connected);
    assert(c1_battery_history_save(path, &loaded));
    assert(symlink(path, link) == 0);
    assert(!c1_battery_history_load(link, &h));
    assert(!c1_battery_history_save(link, &loaded));
    assert(chmod(path, 0644) == 0);
    assert(!c1_battery_history_load(path, &h));
    assert(!c1_battery_history_save(path, &loaded));
    assert(unlink(link) == 0 && mkfifo(link, 0600) == 0);
    assert(!c1_battery_history_load(link, &h));
    assert(!c1_battery_history_save(link, &loaded));
    const char *bad[] = {"", "C1BATTERY 3\n",
        "C1BATTERY 1\n1704067200 80 1 1\n",
        "C1BATTERY 1\n1704067200 80 1 0\n1704067260 79 1 0\n",
        "C1BATTERY 1\n1704067200 80 1 0\n1704074400 79 1 1\n",
        "C1BATTERY 2\n1704067200 101 1 0\n", "C1BATTERY 2\n1704067200 80 1 1\n",
        "C1BATTERY 2\n1704067200 80 4 0\n", "C1BATTERY 2\n1704067200 80 1 0",
        "C1BATTERY 2\n1704067200 80 1 2\n",
        "C1BATTERY 2\n1704067200 80 1 0\n1704067210 70 1 0\n",
        "C1BATTERY 2\n1704067200 80 1 0\n1704067320 70 1 1\n",
        "C1BATTERY 2\n1704067260 80 1 0\n1704067200 70 1 0\n",
        "C1BATTERY 2\n1704067200 80 1 0\n1704153600 70 1 0\n",
        "C1BATTERY 2\n99999999999999999999999999 80 1 0\n"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(*bad); ++i) {
        put(path, bad[i]);
        assert(!c1_battery_history_load(path, &h) && !h.count);
    }
    /* Truncation, embedded NUL and a cache beyond the bounded file limit. */
    put(path, "C1BATTERY 2\n");
    FILE *f = fopen(path, "ab"); assert(f);
    assert(fputc(0, f) == 0 && fclose(f) == 0);
    assert(!c1_battery_history_load(path, &h));
    f = fopen(path, "w"); assert(f);
    for (unsigned i = 0; i < 40000U; ++i) assert(fputc('0', f) == '0');
    assert(fclose(f) == 0);
    assert(!c1_battery_history_load(path, &h));
    assert(unlink(path) == 0 && unlink(link) == 0 && rmdir(directory) == 0);
}

static void test_legacy_cache(void)
{
    char directory[] = "/tmp/c1-battery-legacy-XXXXXX", path[256], header[32];
    assert(mkdtemp(directory));
    snprintf(path, sizeof(path), "%s/history", directory);
    /* Keep actual old timestamps/values, but not hourly connecting lines. */
    put(path, "C1BATTERY 1\n1704067207 80 1 0\n1704070809 79 1 1\n1704074403 82 2 1\n");
    c1_battery_history h, loaded;
    assert(c1_battery_history_load(path, &h) && h.count == 3);
    assert(h.samples[0].timestamp == base + 7 && h.samples[0].percent == 80);
    assert(h.samples[1].timestamp == base + 3609 && h.samples[1].percent == 79);
    assert(h.samples[2].timestamp == base + 7203 && h.samples[2].power == C1_BATTERY_PLUGGED);
    assert(!h.samples[0].connected && !h.samples[1].connected && !h.samples[2].connected);
    assert(!h.continuous && !h.last_wall);
    FILE *f = fopen(path, "r"); assert(f);
    assert(fgets(header, sizeof(header), f) && !strcmp(header, "C1BATTERY 1\n"));
    assert(fclose(f) == 0); /* Loading alone never rewrites the old history. */
    assert(observe(&h, 7267, true) && h.count == 4 && !h.samples[3].connected);
    assert(c1_battery_history_save(path, &h));
    assert(c1_battery_history_load(path, &loaded) && loaded.count == 4);
    for (size_t i = 0; i < h.count; ++i) {
        assert(loaded.samples[i].timestamp == h.samples[i].timestamp);
        assert(loaded.samples[i].percent == h.samples[i].percent);
        assert(loaded.samples[i].power == h.samples[i].power);
    }
    f = fopen(path, "r"); assert(f);
    assert(fgets(header, sizeof(header), f) && !strcmp(header, "C1BATTERY 2\n"));
    assert(fclose(f) == 0);
    assert(unlink(path) == 0 && rmdir(directory) == 0);
}

int main(void)
{
    test_sampling();
    test_cache();
    test_legacy_cache();
    puts("battery history: minute bounds, continuity, clock changes, full cache and unsafe files passed");
    return 0;
}
