#define _GNU_SOURCE 1

#include "launcher/cleanup.h"

#include <assert.h>
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void join_path(char *out, size_t size, const char *left, const char *right)
{
    int count = snprintf(out, size, "%s/%s", left, right);
    assert(count >= 0 && (size_t)count < size);
}

static void make_directory(const char *path)
{
    assert(mkdir(path, 0700) == 0);
}

static void write_file(const char *path, const char *text)
{
    int descriptor = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0700);
    size_t length = strlen(text);
    assert(descriptor >= 0);
    assert(write(descriptor, text, length) == (ssize_t)length);
    assert(close(descriptor) == 0);
}

static void create_release(const char *releases, const char *name, int launcher)
{
    char release[PATH_MAX], artifacts[PATH_MAX], path[PATH_MAX];
    join_path(release, sizeof(release), releases, name);
    join_path(artifacts, sizeof(artifacts), release, "artifacts");
    make_directory(release);
    make_directory(artifacts);
    join_path(path, sizeof(path), artifacts, launcher ? "C1ancher-launcher" : "payload");
    write_file(path, "release");
}

static void remove_tree(const char *path)
{
    struct stat information;
    DIR *directory;
    struct dirent *entry;

    if (lstat(path, &information) != 0) return;
    if (!S_ISDIR(information.st_mode) || S_ISLNK(information.st_mode)) {
        assert(unlink(path) == 0);
        return;
    }
    directory = opendir(path);
    assert(directory != NULL);
    while ((entry = readdir(directory)) != NULL) {
        char child[PATH_MAX];
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        join_path(child, sizeof(child), path, entry->d_name);
        remove_tree(child);
    }
    assert(closedir(directory) == 0);
    assert(rmdir(path) == 0);
}

int main(void)
{
    char root[] = "/tmp/c1-launcher-cleanup-XXXXXX";
    char core[PATH_MAX], releases[PATH_MAX], current[PATH_MAX], previous[PATH_MAX];
    char executable[PATH_MAX], old_tree[PATH_MAX], outside[PATH_MAX], link_path[PATH_MAX];
    char second_root[] = "/tmp/c1-launcher-cleanup-once-XXXXXX";
    char second_core[PATH_MAX], second_releases[PATH_MAX], second_executable[PATH_MAX];
    char second_old[PATH_MAX];

    assert(mkdtemp(root) != NULL);
    join_path(core, sizeof(core), root, "core");
    join_path(releases, sizeof(releases), core, "releases");
    make_directory(core);
    make_directory(releases);
    create_release(releases, "12-new", 1);
    create_release(releases, "9-previous", 0);
    create_release(releases, "4-old", 0);
    create_release(releases, "13-future", 0);
    create_release(releases, "not-a-release", 0);
    join_path(link_path, sizeof(link_path), releases, "10-link");
    assert(symlink("4-old", link_path) == 0);
    join_path(current, sizeof(current), core, "current");
    join_path(previous, sizeof(previous), core, "previous");
    assert(symlink("releases/12-new", current) == 0);
    assert(symlink("releases/9-previous", previous) == 0);
    join_path(executable, sizeof(executable), releases, "12-new/artifacts/C1ancher-launcher");

    join_path(old_tree, sizeof(old_tree), releases, "4-old/artifacts");
    join_path(outside, sizeof(outside), root, "outside");
    write_file(outside, "must survive");
    join_path(link_path, sizeof(link_path), old_tree, "outside-link");
    assert(symlink(outside, link_path) == 0);

    assert(c1_launcher_cleanup_once(executable) == 0);
    assert(access(executable, F_OK) == 0);
    join_path(old_tree, sizeof(old_tree), releases, "9-previous");
    assert(access(old_tree, F_OK) == 0);
    join_path(old_tree, sizeof(old_tree), releases, "4-old");
    assert(access(old_tree, F_OK) != 0);
    join_path(old_tree, sizeof(old_tree), releases, "13-future");
    assert(access(old_tree, F_OK) == 0);
    join_path(old_tree, sizeof(old_tree), releases, "not-a-release");
    assert(access(old_tree, F_OK) == 0);
    assert(access(link_path, F_OK) != 0); /* link_path now points into deleted old data */
    join_path(link_path, sizeof(link_path), releases, "10-link");
    assert(lstat(link_path, &(struct stat){0}) == 0);
    assert(access(outside, F_OK) == 0);
    assert(lstat(current, &(struct stat){0}) == 0);
    assert(lstat(previous, &(struct stat){0}) == 0);

    assert(mkdtemp(second_root) != NULL);
    join_path(second_core, sizeof(second_core), second_root, "core");
    join_path(second_releases, sizeof(second_releases), second_core, "releases");
    make_directory(second_core);
    make_directory(second_releases);
    create_release(second_releases, "1-new", 1);
    create_release(second_releases, "0-old", 0);
    join_path(second_executable, sizeof(second_executable),
              second_releases, "1-new/artifacts/C1ancher-launcher");
    assert(c1_launcher_cleanup_once(second_executable) == 0);
    join_path(second_old, sizeof(second_old), second_releases, "0-old");
    assert(access(second_old, F_OK) == 0);

    remove_tree(root);
    remove_tree(second_root);
    puts("all launcher cleanup tests passed");
    return EXIT_SUCCESS;
}
