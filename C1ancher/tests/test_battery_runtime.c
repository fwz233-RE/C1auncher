#define _DEFAULT_SOURCE 1
#include "services/battery.h"
#include <assert.h>
#include <time.h>
static time_t wall;
static time_t fake_time(time_t *out) { if (out) *out = wall; return wall; }
static unsigned saves;
static bool fake_save(const char *path, const c1_battery_history *history)
{ (void)path; assert(history->count <= C1_BATTERY_HISTORY_POINTS); ++saves; return false; }
#define time fake_time
#define c1_battery_history_save fake_save
#include "../src/hal/linux/ui_runtime.c"
#undef time
#undef c1_battery_history_save
static bool available = true, known = true, plugged;
bool c1_linux_battery_read(uint32_t *percent) { *percent = 81; return available; }
bool c1_linux_external_power_read(bool *online) { *online = plugged; return known; }

int main(void)
{
    wall = C1_BATTERY_MIN_TIME;
    sample_battery_history(0);
    assert(saves == 1 && battery_history.count == 1);
    /* Failed persistence is nonfatal and ten-second hardware reads in the
     * same minute do not cause repeated writes. */
    assert(C1_BATTERY_SAMPLE_INTERVAL_MS == 10000);
    for (unsigned second = 10; second < 60; second += 10) {
        wall = C1_BATTERY_MIN_TIME + second;
        sample_battery_history((int64_t)second * 1000);
    }
    assert(saves == 1 && battery_history.count == 1);
    /* One persisted point appears at each minute boundary. */
    for (unsigned second = 60; second <= 600; second += 10) {
        wall = C1_BATTERY_MIN_TIME + second;
        sample_battery_history((int64_t)second * 1000);
    }
    assert(saves == 11 && battery_history.count == 11 && battery_history.samples[1].connected);
    known = false; wall += 60;
    sample_battery_history(660000);
    assert(saves == 12 && !battery_history.samples[11].connected &&
           battery_history.samples[11].power == C1_BATTERY_POWER_UNKNOWN);
    available = false; wall += 60;
    sample_battery_history(720000);
    assert(saves == 12 && !battery_history.continuous);
    puts("battery runtime sampler passed: ten-second reads, minute writes, sensor gaps");
    return 0;
}
