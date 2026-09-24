#ifndef C1PKG_GUI_LOCALE_H
#define C1PKG_GUI_LOCALE_H

/* Private, header-only settings/status adapter. No desktop canvas linkage and
 * no network calls: timezone matches desktop.conf, default UTC+8. */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

enum gui_language { GUI_ZH, GUI_EN };
#define GUI_DESKTOP_CONFIG "/usr/data/c1/desktop.conf"
#define GUI_LABEL(s, zh, en) ((s)->language == GUI_EN ? (en) : (zh))

static int gui_parse_language(const char *value, enum gui_language *language)
{
    if (value && !strcmp(value, "zh")) { *language = GUI_ZH; return 1; }
    if (value && !strcmp(value, "en")) { *language = GUI_EN; return 1; }
    return 0;
}

static void gui_load_settings(const char *path, enum gui_language *language, int *offset)
{
    char data[16385], *line, *next;
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    ssize_t count = fd < 0 ? -1 : read(fd, data, sizeof(data) - 1U);
    *language = GUI_ZH; *offset = 480;
    if (fd >= 0) close(fd);
    if (count > 0) {
        data[count] = '\0';
        for (line = data; line && *line; line = next) {
            char *value, *end;
            next = strchr(line, '\n');
            if (next) *next++ = '\0';
            else if (count == (ssize_t)sizeof(data) - 1) break;
            if (strlen(line) >= 255U) continue;
            while (*line == ' ' || *line == '\t') ++line;
            if (*line == '#' || *line == ';') continue;
            value = strchr(line, '=');
            if (!value) continue;
            *value++ = '\0'; end = line + strlen(line);
            while (end > line && (end[-1] == ' ' || end[-1] == '\t')) *--end = '\0';
            while (*value == ' ' || *value == '\t') ++value;
            end = strpbrk(value, "#;"); if (end) *end = '\0';
            end = value + strlen(value);
            while (end > value && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r')) *--end = '\0';
            if (!strcmp(line, "language")) (void)gui_parse_language(value, language);
            else if (!strcmp(line, "utc_offset_minutes")) {
                long minutes = strtol(value, &end, 10);
                if (end != value && !*end && minutes >= -720 && minutes <= 840 && minutes % 15 == 0)
                    *offset = (int)minutes;
            }
        }
    }
    (void)gui_parse_language(getenv("C1_UI_LANGUAGE"), language);
}

static void gui_clock_text(char out[6], time_t now, int offset)
{
    struct tm value;
    /* Derive from UTC explicitly; host TZ must never replace desktop settings. */
    time_t utc = now;
    if (gmtime_r(&utc, &value)) {
        unsigned int minutes = (unsigned int)((value.tm_hour * 60 + value.tm_min + offset + 1440) % 1440);
        out[0] = (char)('0' + (minutes / 60U) / 10U); out[1] = (char)('0' + (minutes / 60U) % 10U);
        out[2] = ':'; out[3] = (char)('0' + (minutes % 60U) / 10U); out[4] = (char)('0' + minutes % 10U); out[5] = '\0';
    } else memcpy(out, "--:--", 6U);
}

static void gui_battery_text(char out[8], const char *path)
{
    char data[16], *end;
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    ssize_t count = fd < 0 ? -1 : read(fd, data, sizeof(data) - 1U);
    long percent;
    if (fd >= 0) close(fd);
    strcpy(out, "--%");
    if (count <= 0) return;
    data[count] = '\0'; percent = strtol(data, &end, 10);
    if (end == data || percent < 0 || percent > 100) return;
    while (*end == ' ' || *end == '\r' || *end == '\n') ++end;
    if (!*end) snprintf(out, 8U, "%ld%%", percent);
}
#endif
