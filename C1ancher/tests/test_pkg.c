#include "pkg.h"
#include "platform/app_lease.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures;

static void expect(bool condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

static void expect_version(const char *left, const char *right, int expected,
                           const char *message)
{
    int actual = 99;
    int result = c1pkg_version_compare(left, right, &actual);

    expect(result == 0 && actual == expected, message);
}

static void test_versions(void)
{
    int comparison = 0;

    expect(c1pkg_install_decision(NULL, "1.0.0") == 1, "new application can be installed");
    expect(c1pkg_install_decision("1.0.0", "1.1.0") == 1, "newer installed application can update");
    expect(c1pkg_install_decision("1.0.0", "1.0.0") == 0, "same version is a no-op");
    expect(c1pkg_install_decision("1.0.0+a", "1.0.0+b") == 0, "equal precedence avoids redownload");
    expect(c1pkg_install_decision("2.0.0", "1.0.0") == -1, "downgrade is rejected");
    expect(c1pkg_install_decision("snapshot", "rolling") == -1, "unordered versions cannot silently replace installed apps");
    expect_version("1.2.0", "1.2.0", 0, "equal semantic versions compare equal");
    expect_version("1.10.0", "1.9.9", 1, "numeric components are not compared lexically");
    expect_version("2.0.0", "10.0.0", -1, "major versions compare numerically");
    expect_version("1.0.0", "1.0.0-rc.1", 1, "a release follows its prerelease");
    expect_version("1.0.0-rc.2", "1.0.0-rc.10", -1,
                   "numeric prerelease identifiers compare numerically");
    expect_version("1.0.0-alpha", "1.0.0-1", 1,
                   "non-numeric prerelease identifiers follow numeric ones");
    expect_version("1.0.0+build.1", "1.0.0+build.2", 0,
                   "build metadata does not affect precedence");
    expect_version("1.0", "1.0.0", 0, "missing numeric components compare as zero");
    expect_version("1.0.0.0", "1.0", 0, "four-part trailing zeros compare equal");
    expect_version("1.0.0.12", "1.0.0.9", 1, "fourth component compares numerically");
    expect_version("1.2", "1.1.999.999", 1, "minor version takes precedence");
    expect(c1pkg_version_compare("1", "1.0.0", &comparison) != 0,
           "single-component versions are rejected");
    expect(c1pkg_version_compare("1.0.0.0.1", "1.0.0", &comparison) != 0,
           "five-component versions are rejected");
    expect(c1pkg_version_compare("01.0.0", "1.0.0", &comparison) != 0,
           "leading zero core versions are rejected");
    expect(c1pkg_version_compare("1.0.0-01", "1.0.0-1", &comparison) != 0,
           "leading zero prerelease numbers are rejected");
}

static void set_package(struct c1pkg_package *package, const char *id,
                        const char *version, const char *name)
{
    (void)memset(package, 0, sizeof(*package));
    (void)strcpy(package->id, id);
    (void)strcpy(package->version, version);
    (void)strcpy(package->name, name);
}

static void set_installed(struct c1pkg_installed *installed, const char *id,
                          const char *version)
{
    (void)memset(installed, 0, sizeof(*installed));
    (void)strcpy(installed->id, id);
    (void)strcpy(installed->version, version);
}

static void test_download_list(void)
{
    struct c1pkg_index index;
    struct c1pkg_installed_list installed;
    struct c1pkg_download_list list;

    (void)memset(&index, 0, sizeof(index));
    (void)memset(&installed, 0, sizeof(installed));
    index.count = 5U;
    set_package(&index.packages[0], "alpha", "1.0.0", "Alpha");
    (void)strcpy(index.packages[0].author, "Example Author");
    set_package(&index.packages[1], "current", "2.0.0+server", "Current");
    set_package(&index.packages[2], "newer", "1.0.0", "Newer");
    set_package(&index.packages[3], "unknown", "rolling", "Unknown");
    set_package(&index.packages[4], "update", "3.0.0", "Update");

    installed.count = 5U;
    set_installed(&installed.items[0], "current", "2.0.0+device");
    set_installed(&installed.items[1], "newer", "2.0.0");
    set_installed(&installed.items[2], "orphan", "4.0.0");
    set_installed(&installed.items[3], "unknown", "snapshot");
    set_installed(&installed.items[4], "update", "2.5.0");

    expect(c1pkg_download_list_build(&index, &installed, &list) == 0,
           "download list builds from sorted repository and installed lists");
    expect(list.count == 6U, "download list is the union of repository and installed apps");
    expect(strcmp(list.items[0].author, "Example Author") == 0, "signed author reaches download state");
    expect(strcmp(list.items[3].author, "Unknown") == 0, "removed application has Unknown author");
    expect(strcmp(list.items[0].id, "alpha") == 0 &&
               list.items[0].status == C1PKG_DOWNLOAD_NOT_INSTALLED,
           "repository-only app is installable");
    expect(strcmp(list.items[1].id, "current") == 0 &&
               list.items[1].status == C1PKG_DOWNLOAD_CURRENT,
           "semantically equal installed version is current");
    expect(strcmp(list.items[2].id, "newer") == 0 &&
               list.items[2].status == C1PKG_DOWNLOAD_INSTALLED_NEWER,
           "newer local version is not offered a downgrade");
    expect(strcmp(list.items[3].id, "orphan") == 0 &&
               list.items[3].status == C1PKG_DOWNLOAD_REMOVED &&
               list.items[3].package_index == C1PKG_PACKAGE_NONE,
           "installed app removed from repository remains removable");
    expect(strcmp(list.items[4].id, "unknown") == 0 &&
               list.items[4].status == C1PKG_DOWNLOAD_VERSION_UNKNOWN,
           "non-semantic versions are not treated as updates");
    expect(strcmp(list.items[5].id, "update") == 0 &&
               list.items[5].status == C1PKG_DOWNLOAD_UPDATE_AVAILABLE,
           "higher repository version is marked as an update");
}

static void test_app_lease(void)
{
    char path[128];
    int owner;
    int guard;

    (void)snprintf(path, sizeof(path), "/tmp/c1ancher-app-lease-%ld.lock", (long)getpid());
    (void)unlink(path);
    owner = c1_app_lease_acquire_at(path);
    expect(owner >= 0, "external app acquires the exclusive display lease");
    if (owner < 0) {
        return;
    }
    errno = 0;
    guard = c1_app_lease_guard_acquire_at(path);
    expect(guard < 0 && (errno == EWOULDBLOCK || errno == EAGAIN),
           "launcher display guard is blocked while an app owns the lease");
    expect(c1_app_lease_active_at(path), "active lease is observable by the UI runtime");

    c1_app_lease_release(owner);
    guard = c1_app_lease_guard_acquire_at(path);
    expect(guard >= 0, "launcher display guard is restored when the app exits");
    c1_app_lease_release(guard);
    expect(!c1_app_lease_active_at(path), "released app lease is no longer reported active");
    (void)unlink(path);
}

static void test_launch_modes(void)
{
    expect(c1pkg_app_uses_direct_io("book-reader"),
           "book reader receives the direct display lease");
    expect(c1pkg_app_uses_direct_io("music-player"),
           "music player receives the direct display lease");
    expect(c1pkg_app_uses_direct_io("hello"),
           "existing direct applications remain supported");
    expect(!c1pkg_app_uses_direct_io("terminal-tool"),
           "terminal applications retain terminal mode");
    expect(!c1pkg_app_uses_direct_io(NULL),
           "a missing application ID is not direct");
}

static void test_prefix_and_input(void)
{
    struct c1pkg_prefix prefix = {{0}, 0U};
    int input[2];
    size_t i;
    static const char burst[] = "PiAnOrQ\177\b\025 \r\n\033[A\033[6~\033[3~Z";
    static const int expected[] = {'P', 'i', 'A', 'n', 'O', 'r', 'Q',
        C1PKG_KEY_ERASE, C1PKG_KEY_ERASE, C1PKG_KEY_CLEAR, C1PKG_KEY_REFRESH,
        C1PKG_KEY_ENTER, C1PKG_KEY_ENTER, C1PKG_KEY_UP, C1PKG_KEY_RIGHT,
        C1PKG_KEY_NONE, 'Z'};
    expect(c1pkg_prefix_input(&prefix, 'P', 100U) && strcmp(prefix.text, "p") == 0,
           "uppercase starts a normalized prefix");
    expect(c1pkg_prefix_matches(&prefix, "Paint", "paint") &&
           c1pkg_prefix_matches(&prefix, "钢琴", "piano"), "match display name and ASCII ID fallback");
    c1pkg_prefix_input(&prefix, 'I', 200U);
    expect(c1pkg_prefix_matches(&prefix, "PIANO", "other") &&
           !c1pkg_prefix_matches(&prefix, "Paint", "paint"), "continuous PI narrows the prefix");
    c1pkg_prefix_input(&prefix, 'z', 300U);
    expect(!c1pkg_prefix_matches(&prefix, "Piano", "piano"), "unmatched suffix is retained");
    c1pkg_prefix_input(&prefix, C1PKG_KEY_ERASE, 400U);
    expect(strcmp(prefix.text, "pi") == 0, "backspace removes one prefix character");
    expect(!c1pkg_prefix_expire(&prefix, 1899U) && c1pkg_prefix_expire(&prefix, 1900U),
           "prefix expires at exactly 1500 milliseconds");
    c1pkg_prefix_input(&prefix, 'R', 2000U);
    c1pkg_prefix_input(&prefix, 'Q', 3500U);
    expect(strcmp(prefix.text, "q") == 0, "new input after timeout starts afresh, including R and Q");
    c1pkg_prefix_input(&prefix, C1PKG_KEY_CLEAR, 3600U);
    c1pkg_prefix_input(&prefix, C1PKG_KEY_ERASE, 3700U);
    expect(prefix.text[0] == '\0', "clear and empty backspace are safe");
    for (i = 0U; i < C1PKG_NAME_MAX + 10U; ++i) c1pkg_prefix_input(&prefix, 'x', 3800U);
    expect(strlen(prefix.text) == C1PKG_NAME_MAX, "prefix buffer is bounded and terminated");
    expect(c1pkg_prefix_expire(&prefix, 10U), "clock reversal clears stale prefix");
    strcpy(prefix.text, "piano"); prefix.updated_ms = 100U;
    expect(c1pkg_prefix_match_rank(&prefix, "Piano Lessons", "PIANO") == 3,
           "exact ID has highest case-insensitive priority");
    expect(c1pkg_prefix_match_rank(&prefix, "PIANO", "piano-tools") == 2,
           "exact display name has second priority");
    expect(c1pkg_prefix_match_rank(&prefix, "Piano Lessons", "piano-tools") == 1,
           "longer names and IDs are only prefix matches");
    expect(c1pkg_prefix_match_rank(&prefix, NULL, NULL) == 0,
           "missing name and ID do not match");
    expect(!c1pkg_prefix_input(&prefix, C1PKG_KEY_ENTER, 1700U) && strcmp(prefix.text, "piano") == 0,
           "Enter cannot silently expire a prefix before action validation");
    if (pipe(input) != 0) { expect(0, "input pipe opens"); return; }
    expect(write(input[1], burst, sizeof(burst) - 1U) == (ssize_t)(sizeof(burst) - 1U), "enqueue multi-key burst");
    close(input[1]);
    for (i = 0U; i < sizeof(expected) / sizeof(expected[0]); ++i)
        expect(c1pkg_input_read(input[0], 0) == expected[i], "decode burst without swallowing text or Enter");
    expect(c1pkg_input_read(input[0], 0) == C1PKG_KEY_EOF, "drain pending input before handling hangup");
    close(input[0]);
    if (pipe(input) != 0) { expect(0, "escape pipe opens"); return; }
    expect(write(input[1], "\033", 1U) == 1, "enqueue physical Back/Escape");
    expect(c1pkg_input_read(input[0], 0) == C1PKG_KEY_BACK, "standalone Escape is Back, not backspace");
    close(input[0]); close(input[1]);
}

int main(void)
{
    test_prefix_and_input();
    test_versions();
    test_download_list();
    test_app_lease();
    test_launch_modes();
    if (failures != 0) {
        fprintf(stderr, "%d package model test(s) failed\n", failures);
        return EXIT_FAILURE;
    }
    puts("all package model tests passed");
    return EXIT_SUCCESS;
}
