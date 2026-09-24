#define _DEFAULT_SOURCE 1
#include "services/battery.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CACHE_BYTES (64U + C1_BATTERY_HISTORY_POINTS * 24U)
/* v1 observations remain real sparse points. Import their timestamps without
 * inventing minute samples or carrying hourly continuity into the new graph. */
static const char magic[] = "C1BATTERY 2\n";
static const char legacy_magic[] = "C1BATTERY 1\n";

void c1_battery_history_init(c1_battery_history *history)
{
    if (history) memset(history, 0, sizeof(*history));
}

static bool valid_history(const c1_battery_history *history)
{
    if (!history || history->count > C1_BATTERY_HISTORY_POINTS) return false;
    for (size_t i = 0; i < history->count; ++i) {
        const c1_battery_sample *p = &history->samples[i];
        if (p->timestamp < C1_BATTERY_MIN_TIME || p->timestamp >= C1_BATTERY_MAX_TIME ||
            p->percent > 100 || p->power > C1_BATTERY_PLUGGED ||
            (!i && p->connected) ||
            (i && p->timestamp / 60 <= history->samples[i - 1].timestamp / 60) ||
            (p->connected && p->timestamp / 60 != history->samples[i - 1].timestamp / 60 + 1)) return false;
    }
    return !history->count || history->samples[history->count - 1].timestamp / 60 -
                             history->samples[0].timestamp / 60 < C1_BATTERY_HISTORY_POINTS;
}

bool c1_battery_history_load(const char *path, c1_battery_history *history)
{
    char bytes[CACHE_BYTES];
    c1_battery_history parsed = {0};
    struct stat st;
    size_t used = 0;
    if (!history) return false;
    c1_battery_history_init(history);
    if (!path) return false;
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return false;
    bool ok = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_uid == geteuid() &&
              !(st.st_mode & 0077) && st.st_size > 0 && st.st_size < (off_t)sizeof(bytes);
    while (ok && used < (size_t)st.st_size) {
        ssize_t n = read(fd, bytes + used, (size_t)st.st_size - used);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { ok = false; break; }
        used += (size_t)n;
    }
    close(fd);
    if (!ok || used < sizeof(magic) - 1 || memchr(bytes, 0, used) || bytes[used - 1] != '\n') return false;
    bool legacy = memcmp(bytes, legacy_magic, sizeof(legacy_magic) - 1) == 0;
    if (!legacy && memcmp(bytes, magic, sizeof(magic) - 1)) return false;
    bytes[used] = 0;
    char *cursor = bytes + sizeof(magic) - 1;
    while (*cursor) {
        unsigned long long values[4] = {0};
        for (unsigned field = 0; field < 4; ++field) {
            if (*cursor < '0' || *cursor > '9') return false;
            unsigned digits = 0;
            while (*cursor >= '0' && *cursor <= '9') {
                if (++digits > 10) return false;
                values[field] = values[field] * 10 + (unsigned)(*cursor++ - '0');
            }
            if (*cursor++ != (field == 3 ? '\n' : ' ')) return false;
        }
        if (parsed.count == C1_BATTERY_HISTORY_POINTS || values[0] >= C1_BATTERY_MAX_TIME ||
            values[1] > 100 || values[2] > C1_BATTERY_PLUGGED || values[3] > 1) return false;
        if (legacy && (parsed.count >= 24U || (!parsed.count && values[3]) ||
            (parsed.count && values[0] / 3600 <= (uint64_t)parsed.samples[parsed.count - 1].timestamp / 3600) ||
            (values[3] && values[0] / 3600 != (uint64_t)parsed.samples[parsed.count - 1].timestamp / 3600 + 1)))
            return false;
        parsed.samples[parsed.count++] = (c1_battery_sample){
            (int64_t)values[0], (uint8_t)values[1], (uint8_t)values[2], !legacy && values[3] != 0};
    }
    if (!valid_history(&parsed)) return false;
    *history = parsed; /* History survives restart; continuity never does. */
    return true;
}

bool c1_battery_history_save(const char *path, const c1_battery_history *history)
{
    char bytes[CACHE_BYTES], temporary[1024];
    struct stat st;
    if (!path || !*path || !valid_history(history) ||
        snprintf(temporary, sizeof(temporary), "%s.XXXXXX", path) >= (int)sizeof(temporary)) return false;
    if (lstat(path, &st) == 0) {
        if (!S_ISREG(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & 0077)) return false;
    } else if (errno != ENOENT) return false;
    size_t length = sizeof(magic) - 1;
    memcpy(bytes, magic, length);
    for (size_t i = 0; i < history->count; ++i) {
        const c1_battery_sample *p = &history->samples[i];
        int n = snprintf(bytes + length, sizeof(bytes) - length, "%lld %u %u %u\n",
                         (long long)p->timestamp, (unsigned)p->percent, (unsigned)p->power, p->connected ? 1U : 0U);
        if (n < 0 || (size_t)n >= sizeof(bytes) - length) return false;
        length += (size_t)n;
    }
    int fd = mkstemp(temporary);
    if (fd < 0) return false;
    (void)fcntl(fd, F_SETFD, FD_CLOEXEC);
    size_t used = 0;
    bool ok = true;
    while (used < length) {
        ssize_t n = write(fd, bytes + used, length - used);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { ok = false; break; }
        used += (size_t)n;
    }
    if (ok && fsync(fd)) ok = false;
    if (close(fd)) ok = false;
    if (ok && rename(temporary, path)) ok = false;
    if (!ok) unlink(temporary);
    return ok;
}

bool c1_battery_history_observe(c1_battery_history *h, int64_t wall,
                                int64_t mono, bool available, uint32_t percent,
                                c1_battery_power power)
{
    if (!h || !valid_history(h)) return false;
    if (wall < C1_BATTERY_MIN_TIME || wall >= C1_BATTERY_MAX_TIME || mono < 0) {
        h->continuous = false; h->last_wall = 0;
        return false;
    }
    bool changed = false;
    /* Clock rollback invalidates the chronological view, not device settings. */
    if ((h->last_wall && wall < h->last_wall) || (h->count && wall < h->samples[h->count - 1].timestamp)) {
        changed = h->count != 0;
        c1_battery_history_init(h);
    }
    int64_t dt = wall - h->last_wall, dm = mono - h->last_monotonic;
    if (!h->last_wall || dt > 30 || dm < 0 || dm > 30000 ||
        llabs(dt * 1000 - dm) > 5000) h->continuous = false;
    h->last_wall = wall; h->last_monotonic = mono;
    int64_t oldest = (wall / 60 - (C1_BATTERY_HISTORY_POINTS - 1)) * 60;
    size_t first = 0;
    while (first < h->count && h->samples[first].timestamp < oldest) ++first;
    if (first) {
        memmove(h->samples, h->samples + first, (h->count - first) * sizeof(h->samples[0]));
        h->count -= first;
        if (h->count) h->samples[0].connected = false;
        changed = true;
    }
    if (!available || percent > 100 || power < C1_BATTERY_POWER_UNKNOWN || power > C1_BATTERY_PLUGGED) {
        h->continuous = false;
        return changed;
    }
    if (h->count && h->samples[h->count - 1].timestamp / 60 == wall / 60) return changed;
    bool connected = h->count && h->continuous &&
                     h->samples[h->count - 1].timestamp / 60 + 1 == wall / 60;
    h->samples[h->count++] = (c1_battery_sample){wall, (uint8_t)percent, (uint8_t)power, connected};
    h->continuous = true;
    return true;
}
