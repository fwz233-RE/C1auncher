#define _DEFAULT_SOURCE 1
#include "ui/preferences.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

const char *c1_ui_tr(c1_language language, const char *zh, const char *en)
{
    return language == C1_LANGUAGE_EN ? en : zh;
}

c1_preferences c1_preferences_default(void)
{
    c1_preferences p = {
        .language = C1_LANGUAGE_ZH, .lock_style = C1_LOCK_WALLPAPER,
        .power_mode = C1_POWER_MODE_STANDARD, .utc_offset_minutes = 480,
        .network_time = true, .lock_text = "你好，世界",
        .terminal_enabled = true, .background_checks = true
    };
    return p;
}

c1_power_settings c1_preferences_power_settings(c1_power_mode mode)
{
    if (mode == C1_POWER_MODE_SAVING) return (c1_power_settings){1U, 2U, false};
    if (mode == C1_POWER_MODE_PERFORMANCE) return (c1_power_settings){5U, 0U, true};
    return (c1_power_settings){3U, 5U, false};
}

bool c1_preferences_valid_text(const char *text)
{
    if (!text) return false;
    size_t length = strnlen(text, C1_LOCK_TEXT_BYTES);
    if (!length || length >= C1_LOCK_TEXT_BYTES) return false;
    const unsigned char *s = (const unsigned char *)text;
    const unsigned char *end = s + length;
    while (s < end) {
        unsigned int ch = *s++, n;
        if (ch < 0x80) { if (ch < 0x20 || ch == 0x7f) return false; continue; }
        unsigned int minimum;
        if (ch >= 0xc2 && ch <= 0xdf) { n = 1; minimum = 0x80; ch &= 0x1f; }
        else if (ch >= 0xe0 && ch <= 0xef) { n = 2; minimum = 0x800; ch &= 0x0f; }
        else if (ch >= 0xf0 && ch <= 0xf4) { n = 3; minimum = 0x10000; ch &= 7; }
        else return false;
        if ((size_t)(end - s) < n) return false;
        while (n--) { if ((*s & 0xc0) != 0x80) return false; ch = (ch << 6) | (*s++ & 0x3f); }
        if (ch < minimum || ch > 0x10ffff || (ch >= 0xd800 && ch <= 0xdfff) ||
            (ch >= 0x80 && ch <= 0x9f) || (ch >= 0x200b && ch <= 0x200f) ||
            (ch >= 0x2028 && ch <= 0x202e) || (ch >= 0x2060 && ch <= 0x206f) || ch == 0xfeff)
            return false;
    }
    return true;
}

static bool number(const char *text, long min, long max, long *out)
{
    char *end;
    errno = 0;
    long value = strtol(text, &end, 10);
    if (errno || end == text || *end || value < min || value > max) return false;
    *out = value;
    return true;
}

bool c1_preferences_load(c1_preferences *prefs, const char *path)
{
    if (!prefs || !path) return false;
    *prefs = c1_preferences_default();
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    struct stat st;
    if (fd < 0) return false;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size > 4096) { close(fd); return false; }
    FILE *file = fdopen(fd, "r");
    if (!file) { close(fd); return false; }
    c1_preferences p = *prefs;
    char line[256];
    bool valid = true;
    while (fgets(line, sizeof(line), file)) {
        size_t len = strlen(line);
        if (len && line[len - 1] == '\n') line[--len] = 0;
        else if (!feof(file)) { valid = false; break; }
        if (len && line[len - 1] == '\r') line[--len] = 0;
        if (!line[0] || line[0] == '#') continue;
        char *value = strchr(line, '=');
        if (!value) { valid = false; break; }
        *value++ = 0;
        long n;
        if (!strcmp(line, "language")) {
            if (!strcmp(value, "zh")) p.language = C1_LANGUAGE_ZH;
            else if (!strcmp(value, "en")) p.language = C1_LANGUAGE_EN;
            else valid = false;
        } else if (!strcmp(line, "lock_text")) {
            if (!c1_preferences_valid_text(value)) valid = false;
            else memcpy(p.lock_text, value, strlen(value) + 1);
        } else if (!strcmp(line, "power_mode")) {
            if (!strcmp(value, "saving")) p.power_mode = C1_POWER_MODE_SAVING;
            else if (!strcmp(value, "standard")) p.power_mode = C1_POWER_MODE_STANDARD;
            else if (!strcmp(value, "performance")) p.power_mode = C1_POWER_MODE_PERFORMANCE;
            else valid = false;
        } else if (!strcmp(line, "idle_minutes")) {
            /* Validate old files without retaining arbitrary per-action timers.
             * Missing power_mode consistently migrates to Standard, independent
             * of field order, and leaves all non-power preferences intact. */
            if (!number(value, 0, 1440, &n)) valid = false;
        } else if (!strcmp(line, "suspend_seconds")) {
            if (!number(value, 0, 86400, &n)) valid = false;
        } else if (!strcmp(line, "shutdown_minutes")) {
            if (!number(value, 0, 10080, &n)) valid = false;
        } else if (!strcmp(line, "lock_style")) {
            if (!number(value, 0, 3, &n)) valid = false; else p.lock_style = (c1_lock_style)n;
        } else if (!strcmp(line, "utc_offset_minutes")) {
            if (!number(value, -720, 840, &n) || n % 15) valid = false; else p.utc_offset_minutes = (int)n;
        } else if (!strcmp(line, "terminal_enabled")) {
            if (!number(value, 0, 1, &n)) valid = false; else p.terminal_enabled = n != 0;
        } else if (!strcmp(line, "background_checks")) {
            if (!number(value, 0, 1, &n)) valid = false; else p.background_checks = n != 0;
        } else if (!strcmp(line, "network_time")) {
            if (!number(value, 0, 1, &n)) valid = false; else p.network_time = n != 0;
        }
        if (!valid) break;
    }
    if (ferror(file)) valid = false;
    if (fclose(file)) valid = false;
    if (valid) *prefs = p;
    return valid;
}

bool c1_preferences_save(const c1_preferences *p, const char *path)
{
    if (!p || !path || !c1_preferences_valid_text(p->lock_text) ||
        (unsigned int)p->language > (unsigned int)C1_LANGUAGE_EN ||
        (unsigned int)p->lock_style > (unsigned int)C1_LOCK_CALENDAR ||
        (unsigned int)p->power_mode > (unsigned int)C1_POWER_MODE_PERFORMANCE ||
        p->utc_offset_minutes < -720 || p->utc_offset_minutes > 840 ||
        p->utc_offset_minutes % 15) return false;
    char temporary[1024], parent[1024];
    size_t path_length = strnlen(path, sizeof(parent));
    if (!path_length || path_length >= sizeof(parent) ||
        snprintf(temporary, sizeof(temporary), "%s.XXXXXX", path) >= (int)sizeof(temporary)) return false;
    memcpy(parent, path, path_length + 1U);
    char *slash = strrchr(parent, '/');
    if (!slash) strcpy(parent, "."); else if (slash == parent) slash[1] = 0; else *slash = 0;
    int dirfd = open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dirfd < 0) return false;
    int fd = mkstemp(temporary);
    if (fd < 0) { close(dirfd); return false; }
    (void)fcntl(fd, F_SETFD, FD_CLOEXEC);
    FILE *file = fdopen(fd, "w");
    if (!file) { close(fd); unlink(temporary); close(dirfd); return false; }
    static const char *modes[] = {"saving", "standard", "performance"};
    c1_power_settings power = c1_preferences_power_settings(p->power_mode);
    /* Keep derived legacy durations for a signed rollback core. The old core
     * needs a positive sleep delay, so performance falls back to one second;
     * the current runtime uses the explicit suspend_on_lock flag instead. */
    bool ok = fprintf(file,
        "language=%s\npower_mode=%s\nidle_minutes=%u\nsuspend_seconds=%u\nshutdown_minutes=%u\n"
        "lock_style=%u\nutc_offset_minutes=%d\nnetwork_time=%d\nlock_text=%s\n"
        "terminal_enabled=%d\nbackground_checks=%d\n",
        p->language == C1_LANGUAGE_EN ? "en" : "zh", modes[p->power_mode], power.lock_minutes,
        power.suspend_on_lock ? 1U : 0U, power.shutdown_minutes, (unsigned)p->lock_style, p->utc_offset_minutes,
        p->network_time ? 1 : 0, p->lock_text, p->terminal_enabled ? 1 : 0, p->background_checks ? 1 : 0) > 0;
    if (fflush(file) || fsync(fd)) ok = false;
    if (fclose(file)) ok = false;
    if (ok && rename(temporary, path)) ok = false;
    if (ok && fsync(dirfd)) ok = false;
    if (!ok) unlink(temporary);
    close(dirfd);
    return ok;
}

void c1_preferences_cycle(c1_preferences *p, unsigned int index, int direction)
{
    if (!p) return;
    switch (index) {
    case C1_SETTING_LANGUAGE:
        p->language = p->language == C1_LANGUAGE_ZH ? C1_LANGUAGE_EN : C1_LANGUAGE_ZH;
        break;
    case C1_SETTING_POWER_MODE:
        p->power_mode = (c1_power_mode)(((unsigned int)p->power_mode + (direction < 0 ? 2U : 1U)) % 3U);
        break;
    case C1_SETTING_LOCK_STYLE:
        p->lock_style = (c1_lock_style)((p->lock_style + (direction < 0 ? 3 : 1)) % 4);
        break;
    case C1_SETTING_TIME_ZONE:
        p->utc_offset_minutes += direction < 0 ? -15 : 15;
        if (p->utc_offset_minutes > 840) p->utc_offset_minutes = -720;
        if (p->utc_offset_minutes < -720) p->utc_offset_minutes = 840;
        break;
    case C1_SETTING_NETWORK_TIME: p->network_time = !p->network_time; break;
    default: break;
    }
}
