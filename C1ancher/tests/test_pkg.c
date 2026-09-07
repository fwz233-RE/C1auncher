#include "pkg.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    expect(c1pkg_version_compare("1.0", "1.0.0", &comparison) != 0,
           "versions without major minor and patch are rejected");
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

int main(void)
{
    test_versions();
    test_download_list();
    if (failures != 0) {
        fprintf(stderr, "%d package model test(s) failed\n", failures);
        return EXIT_FAILURE;
    }
    puts("all package model tests passed");
    return EXIT_SUCCESS;
}
