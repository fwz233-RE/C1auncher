/* Host-only desktop protocol/cache tests. Link the actual production font.
 * cc -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Wpedantic -Werror
 *   -Isrc -Isrc/pkg tests/test_desktop_data.c src/services/desktop_data.c
 *   src/ui/preferences.c src/pkg/text.c -o build/host-desktop-data-tests
 * The executable creates/removes only its own mkdtemp directory.
 */
#define _DEFAULT_SOURCE 1
#include "services/desktop_data.h"
#include "pkg/text.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static unsigned checks, failures;
#define CHECK(name, condition) do { ++checks; if (!(condition)) { ++failures; \
    fprintf(stderr, "FAIL %s (%s:%d)\n", name, __FILE__, __LINE__); } } while (0)

static const char base[] = "C1DESKTOP 1\nD\t2026-09-20\nS\t41\nQ\tzh\t学而时习之，不亦说乎？\nQ\ten\tLost time is never found again.\n";

static size_t feed(char *out, size_t cap, const char *date, const char *sequence,
                   const char *zh, const char *en, const char *packages)
{
    int n = snprintf(out, cap, "C1DESKTOP 1\nD\t%s\nS\t%s\nQ\tzh\t%s\nQ\ten\t%s\n%s",
                     date, sequence, zh, en, packages);
    if (n < 0 || (size_t)n >= cap) abort();
    return (size_t)n;
}

static void rejected(const char *name, const char *body, size_t size)
{
    c1_desktop_data out, unchanged;
    memset(&out, 0xa5, sizeof(out));
    unchanged = out;
    bool accepted = c1_desktop_parse(body, size, &out);
    CHECK(name, !accepted);
    if (!accepted) CHECK("parse failure preserves caller output", !memcmp(&out, &unchanged, sizeof(out)));
}

static void repeat(char *out, const char *unit, unsigned count)
{
    size_t n = strlen(unit);
    for (unsigned i = 0; i < count; ++i) memcpy(out + n * i, unit, n);
    out[n * count] = 0;
}

static void widths_and_quotes(void)
{
    char quote[256], body[1024];
    c1_desktop_data data;
    repeat(quote, "学", 17);
    CHECK("actual font 17 Han = 272 pixels", c1pkg_text_width(quote) == 272);
    CHECK("17 Han fit", c1_desktop_quote_valid(quote));
    repeat(quote, "学", 18);
    CHECK("actual font 18 Han = 288 pixels", c1pkg_text_width(quote) == 288);
    CHECK("18 Han rejected", !c1_desktop_quote_valid(quote));
    repeat(quote, "W", 35);
    CHECK("actual font 35 ASCII = 280 pixels", c1pkg_text_width(quote) == 280);
    CHECK("35 ASCII fit", c1_desktop_quote_valid(quote));
    repeat(quote, "W", 36);
    CHECK("actual font 36 ASCII = 288 pixels", c1pkg_text_width(quote) == 288);
    CHECK("36 ASCII rejected", !c1_desktop_quote_valid(quote));
    repeat(quote, "学", 17); strcat(quote, "W");
    CHECK("mixed 17 Han + ASCII = 280 pixels", c1pkg_text_width(quote) == 280);
    CHECK("mixed 280 pixels fit", c1_desktop_quote_valid(quote));
    strcat(quote, "W");
    CHECK("mixed 288 pixels rejected", !c1_desktop_quote_valid(quote));
    repeat(quote, "W", 33); strcat(quote, "学");
    CHECK("mixed 33 ASCII + Han fit", c1pkg_text_width(quote) == 280 && c1_desktop_quote_valid(quote));
    repeat(quote, "W", 34); strcat(quote, "学");
    CHECK("mixed 34 ASCII + Han rejected", !c1_desktop_quote_valid(quote));
    CHECK("Chinese punctuation measured by production font", c1pkg_text_width("学，。！？") == 80);
    const char *invalid[] = {"", " ", " leading", "trailing ", "\t", "a\nb", "a\rb", "a\x01", "a\x7f",
        "\x80", "\xc0\xaf", "\xe0\x80\xaf", "\xed\xa0\x80", "\xf4\x90\x80\x80",
        "\xe5\xad", "\xf0\x9f", "a\xc2\x85", "a\xe2\x80\x8b", "a\xe2\x80\xae",
        "a\xe2\x81\xa6", "a\xef\xbb\xbf", "a\xc2\xad", "a\xef\xbf\xbd", "a\xef\xb7\x90"};
    CHECK("NULL quote rejected", !c1_desktop_quote_valid(NULL));
    for (size_t i = 0; i < sizeof(invalid)/sizeof(*invalid); ++i) {
        char name[80]; snprintf(name, sizeof(name), "invalid quote %zu", i);
        CHECK(name, !c1_desktop_quote_valid(invalid[i]));
        size_t n = feed(body, sizeof(body), "2026-09-20", "41", invalid[i], "Valid text.", "");
        rejected(name, body, n);
        n = feed(body, sizeof(body), "2026-09-20", "41", "Valid text.", invalid[i], "");
        rejected("invalid compatibility slot", body, n);
        n = feed(body, sizeof(body), "2026-09-20", "41", invalid[i], invalid[i], "");
        rejected("identical invalid slots do not bypass validation", body, n);
    }
    /* Current servers repeat the original, rather than provide translations.
     * Only exact equality broadens en beyond legacy printable ASCII. Keep the
     * old differing-Chinese/accented negative fixtures, with accurate names. */
    size_t n = feed(body, sizeof(body), "2026-09-20", "41", "学", "中文", "");
    rejected("different non-ASCII English slot is rejected", body, n);
    n = feed(body, sizeof(body), "2026-09-20", "41", "学", "caf\xc3\xa9", "");
    rejected("different accented English slot is rejected", body, n);
    n = feed(body, sizeof(body), "2026-09-20", "41", "中文原句", "中文原句", "");
    CHECK("same Chinese original is accepted", c1_desktop_parse(body, n, &data) &&
          !strcmp(data.quote_zh, "中文原句") && !strcmp(data.quote_en, "中文原句"));
    n = feed(body, sizeof(body), "2026-09-20", "41", "Live free or die.", "Live free or die.", "");
    CHECK("same English original is accepted", c1_desktop_parse(body, n, &data) &&
          !strcmp(data.quote_zh, "Live free or die.") && !strcmp(data.quote_en, "Live free or die."));
    repeat(quote, "学", 18);
    n = feed(body, sizeof(body), "2026-09-20", "41", quote, quote, "");
    rejected("same over-wide Chinese slots are rejected", body, n);
    n = feed(body, sizeof(body), "2026-09-20", "41", "a\x01", "a\x01", "");
    rejected("same control-character slots are rejected", body, n);
    repeat(quote, "W", 35);
    n = feed(body, sizeof(body), "2026-09-20", "41", "学", quote, "");
    CHECK("35-char English protocol accepted", c1_desktop_parse(body, n, &data));
    repeat(quote, "W", 36);
    n = feed(body, sizeof(body), "2026-09-20", "41", "学", quote, "");
    rejected("36-char English protocol rejected", body, n);
}

static void protocol(void)
{
    char body[C1_DESKTOP_BODY_MAX + 512];
    c1_desktop_data data;
    CHECK("empty catalog accepted", c1_desktop_parse(base, sizeof(base)-1, &data) && data.count == 0 && data.sequence == 41);
    CHECK("parse leaves source for caller binding", data.source[0] == 0);
    CHECK("date preserved", !strcmp(data.date, "2026-09-20"));
    CHECK("UTF-8 quotation preserved", !strcmp(data.quote_zh, "学而时习之，不亦说乎？"));
    rejected("NULL input", NULL, 20);
    CHECK("NULL output rejected", !c1_desktop_parse(base, sizeof(base)-1, NULL));
    for (size_t i = 0; i < sizeof(base)-1; ++i) rejected("every truncated header/required-field prefix", base, i);
    snprintf(body, sizeof(body), "%s", base); body[4] = 0;
    rejected("embedded NUL", body, sizeof(base)-1);
    snprintf(body, sizeof(body), "%s", base); body[10] = '\r';
    rejected("CRLF/control rejected", body, sizeof(base)-1);
    snprintf(body, sizeof(body), "\xef\xbb\xbf%s", base);
    rejected("BOM rejected", body, strlen(body));
    snprintf(body, sizeof(body), "%s\n", base);
    rejected("blank trailing record", body, strlen(body));
    const char *extra[] = {"D\t2026-09-20\n", "S\t41\n", "Q\tzh\t学\n", "Q\ten\tEnglish\n", "Q\tfr\tBonjour\n", "X\tignored\n"};
    for (size_t i = 0; i < sizeof(extra)/sizeof(*extra); ++i) {
        snprintf(body, sizeof(body), "%s%s", base, extra[i]); rejected("duplicate/unknown record", body, strlen(body));
    }
    const char *order[] = {
        "C1DESKTOP 1\nS\t41\nD\t2026-09-20\nQ\tzh\t学\nQ\ten\tText\n",
        "C1DESKTOP 1\nD\t2026-09-20\nS\t41\nQ\ten\tText\nQ\tzh\t学\n",
        "C1DESKTOP 1\nP\tapp\nD\t2026-09-20\nS\t41\nQ\tzh\t学\nQ\ten\tText\n"};
    for (size_t i = 0; i < sizeof(order)/sizeof(*order); ++i) rejected("required records have fixed order", order[i], strlen(order[i]));
    const char *dates[] = {"2026-00-01", "2026-13-01", "2026-01-00", "2026-01-32", "2026-02-29", "2024-02-30", "2026-04-31", "0000-01-01", "2026-9-20", "202a-09-20"};
    for (size_t i = 0; i < sizeof(dates)/sizeof(*dates); ++i) {
        size_t n = feed(body, sizeof(body), dates[i], "41", "学", "Text", "");
        rejected(dates[i], body, n);
    }
    size_t n = feed(body, sizeof(body), "2024-02-29", "18446744073709551615", "学", "Text", "");
    CHECK("leap day and full uint64 sequence accepted", c1_desktop_parse(body, n, &data) && data.sequence == UINT64_MAX);
    const char *sequences[] = {"", "0", "01", "-1", "+1", "1 ", " 1", "1x", "18446744073709551616"};
    for (size_t i = 0; i < sizeof(sequences)/sizeof(*sequences); ++i) {
        n = feed(body, sizeof(body), "2026-09-20", sequences[i], "学", "Text", "");
        rejected("invalid sequence", body, n);
    }
    const char *ids[] = {"", ".app", "-app", "_app", "a..b", "app.", "a/b", "a\\b", "a b", "a\tb", "中文", "c1pkg", "C1UPDATER", "app_daemon", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"};
    for (size_t i = 0; i < sizeof(ids)/sizeof(*ids); ++i) {
        snprintf(body, sizeof(body), "%sP\t%s\n", base, ids[i]); rejected(ids[i], body, strlen(body));
    }
    const char *bad_ids[] = {"P\ta\nP\ta\n", "P\tz\nP\ta\n", "P\tApp\nP\tapp\n"};
    for (size_t i = 0; i < sizeof(bad_ids)/sizeof(*bad_ids); ++i) {
        snprintf(body, sizeof(body), "%s%s", base, bad_ids[i]); rejected("duplicate/unsorted/case-colliding IDs", body, strlen(body));
    }
    n = feed(body, sizeof(body), "2026-09-20", "41", "学", "Text", "P\ta\nP\tc1-ime\nP\tz\n");
    CHECK("internal service package is omitted from desktop catalog", c1_desktop_parse(body, n, &data) &&
          data.count == 2 && !strcmp(data.ids[0], "a") && !strcmp(data.ids[1], "z"));
    n = feed(body, sizeof(body), "2026-09-20", "41", "学", "Text", "P\tC1-IME\nP\tc1-ime\n");
    rejected("case-colliding internal package IDs are still rejected", body, n);
    n = feed(body, sizeof(body), "2026-09-20", "41", "学", "Text", "P\t0-app\nP\tA_app\nP\tZ.app\nP\ta-app\nP\tz\n");
    CHECK("server uppercase IDs and ASCII order accepted", c1_desktop_parse(body, n, &data) && data.count == 5);
    n = (size_t)snprintf(body, sizeof(body), "%s", base);
    for (unsigned i = 0; i < 128; ++i) n += (size_t)snprintf(body+n, sizeof(body)-n, "P\tapp-%028u\n", i);
    CHECK("128 maximum-length IDs accepted", c1_desktop_parse(body, n, &data) && data.count == 128);
    n += (size_t)snprintf(body+n, sizeof(body)-n, "P\tzzz\n");
    rejected("129 IDs rejected", body, n);
    memset(body, 'x', sizeof(body)); memcpy(body, base, sizeof(base)-1); body[C1_DESKTOP_BODY_MAX] = '\n';
    rejected("body over 16384 bytes rejected", body, C1_DESKTOP_BODY_MAX+1);
    n = (size_t)snprintf(body, sizeof(body), "%sP\t", base);
    memset(body+n, 'a', 256); body[n+256] = '\n';
    rejected("overlong record rejected", body, n+257);
}

static c1_desktop_data fixture(void)
{
    c1_desktop_data data = {0};
    if (!c1_desktop_parse(base, sizeof(base)-1, &data)) abort();
    strcpy(data.source, "http://127.0.0.1:12345/c1/v2");
    data.count = 2; strcpy(data.ids[0], "alpha"); strcpy(data.ids[1], "beta");
    return data;
}

static void seen_algorithm(void)
{
    c1_desktop_data seen = fixture(), current = seen;
    CHECK("first use without seen baseline has no badge", c1_desktop_new_count(&current, NULL) == 0);
    CHECK("missing current has no badge", c1_desktop_new_count(NULL, &seen) == 0);
    CHECK("same snapshot no new IDs", c1_desktop_new_count(&current, &seen) == 0);
    current.sequence++;
    CHECK("version-only sequence change has no new IDs", c1_desktop_new_count(&current, &seen) == 0);
    current.count = 3; strcpy(current.ids[2], "gamma");
    CHECK("new ID counted", c1_desktop_new_count(&current, &seen) == 1);
    current = seen; current.sequence++; current.count = 1;
    CHECK("deletion has no new IDs", c1_desktop_new_count(&current, &seen) == 0);
    current = seen; current.sequence++; strcpy(current.ids[1], "gamma");
    CHECK("same-count replacement still new", c1_desktop_new_count(&current, &seen) == 1);
    current.sequence = seen.sequence-1;
    CHECK("sequence rollback suppressed", c1_desktop_new_count(&current, &seen) == 0);
    current.sequence = seen.sequence+1; strcpy(current.source, "http://127.0.0.1:12346/c1/v2");
    CHECK("different origin suppressed", c1_desktop_new_count(&current, &seen) == 0);
    strcpy(current.source, "http://127.0.0.1:12345/another/c1/v2");
    CHECK("different repository path suppressed", c1_desktop_new_count(&current, &seen) == 0);
    current = fixture(); seen = fixture();
    current.count = 3; strcpy(current.ids[2], "c1-ime");
    CHECK("internal service package is not counted as a new application", c1_desktop_new_count(&current, &seen) == 0);
    current = seen; seen.count = 0;
    CHECK("explicitly seen empty repository counts subsequent additions", c1_desktop_new_count(&current, &seen) == 2);
    current.count = C1_DESKTOP_PACKAGES+1;
    CHECK("invalid current count guarded", c1_desktop_new_count(&current, &seen) == 0);
    current = fixture(); seen.count = C1_DESKTOP_PACKAGES+1;
    CHECK("invalid seen count guarded", c1_desktop_new_count(&current, &seen) == 0);
}

static void write_raw(const char *path, const void *data, size_t size)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) abort();
    size_t offset = 0;
    while (offset < size) { ssize_t n = write(fd, (const char *)data+offset, size-offset); if (n <= 0) abort(); offset += (size_t)n; }
    if (close(fd)) abort();
}

static void private_cache(void)
{
    char directory[] = "/tmp/c1-desktop-data-test-XXXXXX", path[256], link[256], fifo[256], target[256];
    if (!mkdtemp(directory)) abort();
    snprintf(path, sizeof(path), "%s/cache", directory); snprintf(link, sizeof(link), "%s/link", directory);
    snprintf(fifo, sizeof(fifo), "%s/fifo", directory); snprintf(target, sizeof(target), "%s/target", directory);
    c1_desktop_data original = fixture(), loaded, untouched;
    struct stat st;
    mode_t old_mask = umask(0);
    static const char *const originals[] = {"中文原句", "Live free or die."};
    for (size_t i = 0; i < sizeof(originals) / sizeof(*originals); ++i) {
        char body[1024];
        c1_desktop_data same, round_trip;
        size_t n = feed(body, sizeof(body), "2026-09-20", "41", originals[i], originals[i], "");
        CHECK("same original parses before cache save", c1_desktop_parse(body, n, &same));
        strcpy(same.source, "http://127.0.0.1:12345/c1/v2");
        CHECK("same original cache saves and loads", c1_desktop_save(path, &same) &&
              c1_desktop_load(path, &round_trip) && !memcmp(&round_trip, &same, sizeof(same)));
    }
    CHECK("cache saves", c1_desktop_save(path, &original)); umask(old_mask);
    CHECK("cache private regular and owned", !lstat(path, &st) && S_ISREG(st.st_mode) && (st.st_mode & 0777) == 0600 && st.st_uid == geteuid());
    CHECK("cache round trip", c1_desktop_load(path, &loaded) && !memcmp(&loaded, &original, sizeof(loaded)));
    memset(&loaded, 0xa5, sizeof(loaded)); untouched = loaded;
    CHECK("missing cache fails", !c1_desktop_load(target, &loaded));
    CHECK("failed cache load does not overwrite caller", !memcmp(&loaded, &untouched, sizeof(loaded)));
    CHECK("directory rejected", !c1_desktop_load(directory, &loaded));
    if (symlink(path, link)) abort();
    CHECK("symlink cache rejected", !c1_desktop_load(link, &loaded));
    if (mkfifo(fifo, 0600)) abort();
    CHECK("FIFO cache rejected without blocking", !c1_desktop_load(fifo, &loaded));
    if (chmod(path, 0640)) abort();
    CHECK("group-readable cache rejected", !c1_desktop_load(path, &loaded));
    if (chmod(path, 0604)) abort();
    CHECK("other-readable cache rejected", !c1_desktop_load(path, &loaded));
    if (chmod(path, 0600)) abort();
    struct { char magic[16]; c1_desktop_data data; } record;
    int fd = open(path, O_RDONLY); if (fd < 0 || read(fd, &record, sizeof(record)) != (ssize_t)sizeof(record)) abort(); close(fd);
    CHECK("private cache magic/version", !memcmp(record.magic, "C1DESKCACHE1\0\0\0\0", 16));
    /* Construct a canonical record independently so corruption validation is
     * still exercised when the production writer itself has a magic bug. */
    memcpy(record.magic, "C1DESKCACHE1\0\0\0\0", sizeof(record.magic));
    write_raw(path, &record, sizeof(record)-1);
    CHECK("truncated cache rejected", !c1_desktop_load(path, &loaded));
    write_raw(path, &record, sizeof(record));
    fd = open(path, O_WRONLY | O_APPEND); if (fd < 0 || write(fd, "x", 1) != 1) abort(); close(fd);
    CHECK("oversized cache rejected", !c1_desktop_load(path, &loaded));
    record.magic[0] = '?'; write_raw(path, &record, sizeof(record));
    CHECK("bad cache magic rejected", !c1_desktop_load(path, &loaded));
    record.magic[0] = 'C';
    record.data.count = 129; write_raw(path, &record, sizeof(record));
    CHECK("corrupt cached count rejected", !c1_desktop_load(path, &loaded));
    record.data = original; memset(record.data.source, 'x', sizeof(record.data.source)); write_raw(path, &record, sizeof(record));
    CHECK("unterminated source rejected", !c1_desktop_load(path, &loaded));
    record.data = original; memset(record.data.quote_zh, 'x', sizeof(record.data.quote_zh)); write_raw(path, &record, sizeof(record));
    CHECK("unterminated quote rejected", !c1_desktop_load(path, &loaded));
    record.data = original; strcpy(record.data.ids[1], "alpha"); write_raw(path, &record, sizeof(record));
    CHECK("duplicate cached IDs rejected", !c1_desktop_load(path, &loaded));
    record.data = original; strcpy(record.data.date, "2026-02-30"); write_raw(path, &record, sizeof(record));
    CHECK("impossible cached date rejected", !c1_desktop_load(path, &loaded));
    record.data = original; record.data.sequence = 0; write_raw(path, &record, sizeof(record));
    CHECK("zero cached sequence rejected", !c1_desktop_load(path, &loaded));
    CHECK("atomic overwrite repairs corrupt cache", c1_desktop_save(path, &original) && c1_desktop_load(path, &loaded));
    record.data = original; record.data.count = 129;
    CHECK("save refuses invalid data", !c1_desktop_save(path, &record.data));
    CHECK("failed save preserves existing cache", c1_desktop_load(path, &loaded) && !memcmp(&loaded, &original, sizeof(loaded)));
    write_raw(target, "sentinel", 8); unlink(link); if (symlink(target, link)) abort();
    CHECK("safe atomic replacement of destination symlink", c1_desktop_save(link, &original));
    char sentinel[8]; fd = open(target, O_RDONLY); if (fd < 0 || read(fd, sentinel, sizeof(sentinel)) != 8) abort(); close(fd);
    CHECK("symlink target never overwritten", !memcmp(sentinel, "sentinel", 8));
    CHECK("replacement itself is regular", !lstat(link, &st) && S_ISREG(st.st_mode));
    unlink(path); unlink(link); unlink(fifo); unlink(target);
    CHECK("test leaves no temporary cache files", rmdir(directory) == 0);
}

int main(int argc, char **argv)
{
    const char *group = argc > 1 ? argv[1] : "all";
    if (!strcmp(group, "all") || !strcmp(group, "width")) widths_and_quotes();
    if (!strcmp(group, "all") || !strcmp(group, "protocol")) protocol();
    if (!strcmp(group, "all") || !strcmp(group, "seen")) seen_algorithm();
    if (!strcmp(group, "all") || !strcmp(group, "cache")) private_cache();
    if (!checks) return 2;
    printf("desktop data: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
