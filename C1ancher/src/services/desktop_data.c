#define _DEFAULT_SOURCE 1
#include "services/desktop_data.h"
#include "pkg/text.h"
#include "ui/preferences.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

static bool valid_date(const char *date)
{
    if (!date || strlen(date) != 10 || date[4] != '-' || date[7] != '-') return false;
    for (unsigned i = 0; i < 10; ++i)
        if (i != 4 && i != 7 && (date[i] < '0' || date[i] > '9')) return false;
    unsigned year = (unsigned)(date[0]-'0')*1000 + (unsigned)(date[1]-'0')*100 +
                    (unsigned)(date[2]-'0')*10 + (unsigned)(date[3]-'0');
    unsigned month = (unsigned)(date[5]-'0')*10 + (unsigned)(date[6]-'0');
    unsigned day = (unsigned)(date[8]-'0')*10 + (unsigned)(date[9]-'0');
    static const unsigned days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    return year != 0 && month >= 1 && month <= 12 && day >= 1 &&
           day <= days[month - 1] + (month == 2 && year % 4 == 0 && (year % 100 || year % 400 == 0));
}

bool c1_desktop_quote_valid(const char *quote)
{
    if (!c1_preferences_valid_text(quote) || c1pkg_text_width(quote) > C1_DESKTOP_BANNER_WIDTH ||
        quote[0] == ' ' || quote[strlen(quote) - 1] == ' ') return false;
    /* Reuse the production label scalar policy without its 40-byte label cap.
     * Ordinary spaces are allowed inside a sentence, not as an empty quote. */
    for (const unsigned char *s = (const unsigned char *)quote; *s;) {
        size_t n = *s < 0x80 ? 1 : *s < 0xe0 ? 2 : *s < 0xf0 ? 3 : 4;
        char scalar[5] = {0}; memcpy(scalar, s, n);
        if (*s != ' ' && !c1pkg_valid_label(scalar)) return false;
        s += n;
    }
    return true;
}

static bool internal_package_id(const char *id)
{
    return id != NULL && !strcasecmp(id, "c1-ime");
}

static bool decimal(const char *s, uint64_t *out)
{
    uint64_t n = 0;
    if (!*s || (*s == '0' && s[1])) return false;
    for (; *s; ++s) {
        unsigned d = (unsigned char)*s - '0';
        if (d > 9 || n > (UINT64_MAX - d) / 10) return false;
        n = n * 10 + d;
    }
    *out = n;
    return true;
}

static bool valid_id(const char *id)
{
    size_t n = strlen(id);
    if (!n || n > 32 || id[0] == '.' || id[0] == '-' || id[0] == '_' || id[n - 1] == '.' || strstr(id, "..")) return false;
    const char *reserved[] = {"c1pkg", "c1updater", "c1ancher", "c1ancher-launcher", "app_daemon"};
    for (unsigned i = 0; i < sizeof(reserved) / sizeof(*reserved); ++i)
        if (!strcasecmp(id, reserved[i])) return false;
    for (; *id; ++id)
        if (!((*id >= 'a' && *id <= 'z') || (*id >= 'A' && *id <= 'Z') || (*id >= '0' && *id <= '9') ||
              *id == '-' || *id == '_' || *id == '.')) return false;
    return true;
}

bool c1_desktop_parse(const char *data, size_t size, c1_desktop_data *out)
{
    const char header[] = "C1DESKTOP 1\n";
    c1_desktop_data parsed = {0};
    char last_id[33] = {0};
    if (!data || !out || size < sizeof(header) - 1 || size > C1_DESKTOP_BODY_MAX ||
        memcmp(data, header, sizeof(header) - 1) || data[size - 1] != '\n' ||
        memchr(data, 0, size) || memchr(data, '\r', size)) return false;
    size_t offset = sizeof(header) - 1;
    unsigned fields = 0;
    while (offset < size) {
        const char *end = memchr(data + offset, '\n', size - offset);
        char line[256];
        size_t n = end ? (size_t)(end - data - offset) : sizeof(line);
        if (n >= sizeof(line)) return false;
        memcpy(line, data + offset, n); line[n] = 0;
        offset += n + 1;
        if (!strncmp(line, "D\t", 2) && fields == 0) {
            const char *d = line + 2;
            if (strlen(d) != 10 || d[4] != '-' || d[7] != '-') return false;
            for (unsigned i = 0; i < 10; ++i)
                if (i != 4 && i != 7 && (d[i] < '0' || d[i] > '9')) return false;
            unsigned month = (unsigned)(d[5]-'0')*10 + (unsigned)(d[6]-'0');
            unsigned day = (unsigned)(d[8]-'0')*10 + (unsigned)(d[9]-'0');
            unsigned year = (unsigned)(d[0]-'0')*1000 + (unsigned)(d[1]-'0')*100 + (unsigned)(d[2]-'0')*10 + (unsigned)(d[3]-'0');
            static const unsigned days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
            if (!year || month < 1 || month > 12) return false;
            unsigned maximum = days[month - 1] + (month == 2 && year % 4 == 0 && (year % 100 || year % 400 == 0));
            if (day < 1 || day > maximum) return false;
            strcpy(parsed.date, d); fields |= 1;
        } else if (!strncmp(line, "S\t", 2) && fields == 1) {
            if (!decimal(line + 2, &parsed.sequence) || !parsed.sequence) return false;
            fields |= 2;
        } else if ((!strncmp(line, "Q\tzh\t", 5) && fields == 3) ||
                   (!strncmp(line, "Q\ten\t", 5) && fields == 7)) {
            const char *quote = line + 5;
            bool zh = line[2] == 'z';
            if (!c1_desktop_quote_valid(quote)) return false;
            /* The two Q fields are retained as wire/cache compatibility slots.
             * A current response stores one original quote in both slots. Keep
             * accepting legacy ASCII English when it differs, but allow a
             * non-ASCII value only when it is exactly the authoritative zh
             * slot; a translated or otherwise different Chinese value is not
             * a valid current response. */
            if (!zh && strcmp(quote, parsed.quote_zh))
                for (const unsigned char *s = (const unsigned char *)quote; *s; ++s)
                    if (*s < 32 || *s > 126) return false;
            strcpy(zh ? parsed.quote_zh : parsed.quote_en, quote);
            fields |= zh ? 4 : 8;
        } else if (!strncmp(line, "P\t", 2) && fields == 15) {
            if (!valid_id(line + 2) ||
                (last_id[0] && (!strcasecmp(last_id, line + 2) || strcmp(last_id, line + 2) >= 0))) return false;
            for (unsigned i = 0; i < parsed.count; ++i)
                if (!strcasecmp(parsed.ids[i], line + 2)) return false;
            strcpy(last_id, line + 2);
            if (internal_package_id(line + 2)) continue;
            if (parsed.count >= C1_DESKTOP_PACKAGES) return false;
            strcpy(parsed.ids[parsed.count++], line + 2);
        } else return false;
    }
    if (fields != 15) return false;
    *out = parsed;
    return true;
}

/* Private local cache, bounded and versioned; symlinks/special files rejected.
 * Only the GUI writes the viewed baseline, except initial empty onboarding. */
static bool valid_cache(const c1_desktop_data *d)
{
    if (d->count > C1_DESKTOP_PACKAGES || d->sequence == 0 || !memchr(d->source, 0, sizeof(d->source)) ||
        !d->source[0] || !memchr(d->date, 0, sizeof(d->date)) || !valid_date(d->date) ||
        !c1_desktop_quote_valid(d->quote_zh) || !c1_desktop_quote_valid(d->quote_en)) return false;
    for (unsigned i = 0; i < d->count; ++i)
        if (!memchr(d->ids[i], 0, sizeof(d->ids[i])) || !valid_id(d->ids[i]) ||
            (i && strcmp(d->ids[i - 1], d->ids[i]) >= 0)) return false;
    return true;
}

bool c1_desktop_load(const char *path, c1_desktop_data *out)
{
    struct { char magic[16]; c1_desktop_data data; } record;
    struct stat st;
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return false;
    bool ok = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_uid == geteuid() &&
              !(st.st_mode & 0077) && st.st_size == (off_t)sizeof(record);
    size_t used = 0;
    while (ok && used < sizeof(record)) {
        ssize_t n = read(fd, (char *)&record + used, sizeof(record) - used);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { ok = false; break; }
        used += (size_t)n;
    }
    close(fd);
    ok = ok && !memcmp(record.magic, "C1DESKCACHE1\0\0\0\0", 16) && valid_cache(&record.data);
    if (ok) *out = record.data;
    return ok;
}

bool c1_desktop_save(const char *path, const c1_desktop_data *data)
{
    struct { char magic[16]; c1_desktop_data data; } record = {0};
    char temporary[1024];
    if (!data || !valid_cache(data) || snprintf(temporary, sizeof(temporary), "%s.XXXXXX", path) >= (int)sizeof(temporary)) return false;
    memcpy(record.magic, "C1DESKCACHE1", 12); record.data = *data;
    int fd = mkstemp(temporary);
    if (fd < 0) return false;
    (void)fcntl(fd, F_SETFD, FD_CLOEXEC);
    size_t used = 0;
    bool ok = true;
    while (used < sizeof(record)) {
        ssize_t n = write(fd, (char *)&record + used, sizeof(record) - used);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { ok = false; break; }
        used += (size_t)n;
    }
    if (fsync(fd)) ok = false;
    if (close(fd)) ok = false;
    if (ok && rename(temporary, path)) ok = false;
    if (!ok) unlink(temporary);
    return ok;
}

unsigned c1_desktop_new_count(const c1_desktop_data *current, const c1_desktop_data *seen)
{
    if (!current || !seen || current->count > C1_DESKTOP_PACKAGES || seen->count > C1_DESKTOP_PACKAGES ||
        strcmp(current->source, seen->source) || current->sequence < seen->sequence) return 0;
    unsigned count = 0;
    for (unsigned i = 0; i < current->count; ++i) {
        if (internal_package_id(current->ids[i])) continue;
        unsigned j = 0;
        while (j < seen->count && strcmp(current->ids[i], seen->ids[j])) ++j;
        if (j == seen->count) ++count;
    }
    return count;
}
