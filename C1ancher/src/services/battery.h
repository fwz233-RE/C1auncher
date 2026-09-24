#ifndef C1_SERVICES_BATTERY_H
#define C1_SERVICES_BATTERY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define C1_BATTERY_HISTORY_POINTS 1440U /* One real point per minute for 24 hours. */
#define C1_BATTERY_MIN_TIME INT64_C(1704067200) /* 2024: reject an unset RTC. */
#define C1_BATTERY_MAX_TIME INT64_C(4102444800)

typedef enum {
    C1_BATTERY_POWER_UNKNOWN = 0,
    C1_BATTERY_DISCHARGING = 1, /* External power absent, not measured watts. */
    C1_BATTERY_PLUGGED = 2     /* Plugged in does not necessarily mean charging. */
} c1_battery_power;

typedef struct {
    int64_t timestamp; /* Actual UTC observation, never rounded to an invented time. */
    uint8_t percent;
    uint8_t power;
    bool connected; /* Continuous ten-second observations since the previous minute point. */
} c1_battery_sample;

typedef struct {
    c1_battery_sample samples[C1_BATTERY_HISTORY_POINTS];
    size_t count;
    /* Volatile continuity evidence; deliberately never restored from disk. */
    int64_t last_wall, last_monotonic;
    bool continuous;
} c1_battery_history;

void c1_battery_history_init(c1_battery_history *history);
bool c1_battery_history_load(const char *path, c1_battery_history *history);
bool c1_battery_history_save(const char *path, const c1_battery_history *history);

/* Pure, bounded update. Read hardware about every ten seconds, including while
 * locked or another application owns the display. Keep the first real point
 * per UTC minute in the latest 1440 buckets. No writes for same-minute reads.
 * Missing sensor data, clock jumps, restart and long observation gaps break
 * connections; never infer shutdown, charge rate or zero consumption from a gap.
 * Return true iff persistent samples changed (caller may save once). */
bool c1_battery_history_observe(c1_battery_history *history, int64_t wall,
                                int64_t monotonic_ms, bool available,
                                uint32_t percent, c1_battery_power power);
#endif
