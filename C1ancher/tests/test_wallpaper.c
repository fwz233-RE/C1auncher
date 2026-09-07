#define _POSIX_C_SOURCE 200809L
#include "ui/wallpaper.h"
#include "ui/wallpaper_default.h"

#include <assert.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static void write_file(const char *path, const void *data, size_t size)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    assert(fd >= 0);
    assert(write(fd, data, size) == (ssize_t)size);
    assert(close(fd) == 0);
}

static void expect_file(const char *path, const void *data, size_t size)
{
    unsigned char buffer[C1_DISPLAY_FRAME_BYTES + 1];
    int fd = open(path, O_RDONLY);
    assert(fd >= 0);
    assert(read(fd, buffer, sizeof(buffer)) == (ssize_t)size);
    assert(memcmp(buffer, data, size) == 0);
    assert(close(fd) == 0);
}

static void test_media_directories(void)
{
    char root[] = "/tmp/c1-media-XXXXXX";
    char path[256];
    const char *names[] = {"Pic", "Music", "Book"};
    struct stat information;
    size_t index;
    assert(strcmp(C1_WALLPAPER_DIRECTORY, "/storage/mtp/Pic") == 0);
    assert(strcmp(C1_MUSIC_DIRECTORY, "/storage/mtp/Music") == 0);
    assert(strcmp(C1_BOOKS_DIRECTORY, "/storage/mtp/Book") == 0);
    assert(mkdtemp(root) != NULL);
    assert(c1_media_directories_prepare_from(root));
    for (index = 0; index < 3; ++index) {
        (void)snprintf(path, sizeof(path), "%s/%s", root, names[index]);
        assert(lstat(path, &information) == 0 && S_ISDIR(information.st_mode));
    }
    (void)snprintf(path, sizeof(path), "%s/Book/user.txt", root);
    write_file(path, "keep", 4);
    assert(c1_media_directories_prepare_from(root));
    expect_file(path, "keep", 4);
    assert(unlink(path) == 0);
    (void)snprintf(path, sizeof(path), "%s/Book", root);
    assert(rmdir(path) == 0);
    assert(c1_media_directories_prepare_from(root));
    assert(rmdir(path) == 0);
    write_file(path, "keep", 4);
    assert(!c1_media_directories_prepare_from(root));
    expect_file(path, "keep", 4);
    assert(unlink(path) == 0);
    assert(symlink(root, path) == 0);
    assert(!c1_media_directories_prepare_from(root));
    assert(lstat(path, &information) == 0 && S_ISLNK(information.st_mode));
    assert(unlink(path) == 0);
    assert(c1_media_directories_prepare_from(root));
    for (index = 0; index < 3; ++index) {
        (void)snprintf(path, sizeof(path), "%s/%s", root, names[index]);
        assert(rmdir(path) == 0);
    }
    assert(rmdir(root) == 0);
    assert(!c1_media_directories_prepare_from(root));
}

int main(void)
{
    test_media_directories();
    char root[] = "/tmp/c1-wallpaper-XXXXXX";
    char directory[256], path[256], legacy[256], legacy_path[256], other[256], unavailable[256], alias[256];
    unsigned char custom[C1_DISPLAY_FRAME_BYTES];
    struct stat information;
    unsigned int index;
    DIR *listing;
    struct dirent *entry;

    assert(mkdtemp(root) != NULL);
    (void)snprintf(directory, sizeof(directory), "%s/Pic", root);
    (void)snprintf(path, sizeof(path), "%s/Pic/wallpaper.raw", root);
    (void)snprintf(legacy, sizeof(legacy), "%s/pic", root);
    (void)snprintf(legacy_path, sizeof(legacy_path), "%s/pic/wallpaper.raw", root);
    (void)snprintf(other, sizeof(other), "%s/Pic/random.raw", root);
    (void)snprintf(unavailable, sizeof(unavailable), "%s/unmounted/mtp/pic", root);
    (void)snprintf(alias, sizeof(alias), "%s/alias", root);
    memset(custom, 0x55, sizeof(custom));

    /* Fresh startup needs neither a picture app nor any user-provided image. */
    assert(c1_wallpaper_load_or_default_from(directory, NULL));
    expect_file(path, c1_wallpaper_default, sizeof(c1_wallpaper_default));
    assert(memcmp(c1_wallpaper_frame, c1_wallpaper_default, sizeof(c1_wallpaper_frame)) == 0);
    assert(stat(path, &information) == 0 && (information.st_mode & 0777) == 0644);
    assert(unlink(path) == 0);
    assert(c1_wallpaper_load_or_default_from(directory, NULL));
    expect_file(path, c1_wallpaper_default, sizeof(c1_wallpaper_default));

    /* Only the fixed file is read, and a user's replacement is never reset. */
    write_file(path, custom, sizeof(custom));
    assert(c1_wallpaper_load_or_default_from(directory, NULL));
    expect_file(path, custom, sizeof(custom));
    assert(memcmp(c1_wallpaper_frame, custom, sizeof(custom)) == 0);
    memset(custom, 0xaa, sizeof(custom));
    write_file(path, custom, sizeof(custom));
    assert(c1_wallpaper_load_or_default_from(directory, NULL));
    assert(memcmp(c1_wallpaper_frame, custom, sizeof(custom)) == 0);
    assert(unlink(path) == 0);
    write_file(other, custom, sizeof(custom));
    assert(c1_wallpaper_load_or_default_from(directory, NULL));
    expect_file(path, c1_wallpaper_default, sizeof(c1_wallpaper_default));

    /* Existing legacy content migrates only when the canonical file is absent. */
    assert(mkdir(legacy, 0755) == 0);
    write_file(legacy_path, custom, sizeof(custom));
    assert(unlink(path) == 0);
    assert(c1_wallpaper_load_or_default_from(directory, legacy));
    expect_file(path, custom, sizeof(custom));
    write_file(path, c1_wallpaper_default, sizeof(c1_wallpaper_default));
    assert(c1_wallpaper_load_or_default_from(directory, legacy));
    expect_file(path, c1_wallpaper_default, sizeof(c1_wallpaper_default));

    /* Malformed/special files are not overwritten; display the built-in image. */
    write_file(path, "invalid", 7U);
    assert(!c1_wallpaper_load_or_default_from(directory, legacy));
    expect_file(path, "invalid", 7U);
    assert(memcmp(c1_wallpaper_frame, c1_wallpaper_default, sizeof(c1_wallpaper_frame)) == 0);
    assert(unlink(path) == 0);
    assert(symlink(other, path) == 0);
    assert(!c1_wallpaper_load_or_default_from(directory, NULL));
    expect_file(other, custom, sizeof(custom));
    assert(lstat(path, &information) == 0 && S_ISLNK(information.st_mode));
    assert(unlink(path) == 0);
    assert(mkfifo(path, 0644) == 0);
    assert(!c1_wallpaper_load_or_default_from(directory, NULL));
    assert(lstat(path, &information) == 0 && S_ISFIFO(information.st_mode));
    assert(unlink(path) == 0);
    assert(symlink(root, alias) == 0);
    {
        char redirected[256];
        (void)snprintf(redirected, sizeof(redirected), "%s/alias/Pic", root);
        assert(!c1_wallpaper_load_or_default_from(redirected, NULL));
        assert(access(path, F_OK) != 0);
    }
    assert(unlink(alias) == 0);
    assert(!c1_wallpaper_load_or_default_from(unavailable, NULL));
    assert(memcmp(c1_wallpaper_frame, c1_wallpaper_default, sizeof(c1_wallpaper_frame)) == 0);

    /* Concurrent initialization publishes one complete frame, without leftovers. */
    for (index = 0U; index < 8U; ++index) {
        pid_t child = fork();
        assert(child >= 0);
        if (child == 0) _exit(c1_wallpaper_load_or_default_from(directory, NULL) ? 0 : 1);
    }
    for (index = 0U; index < 8U; ++index) {
        int status;
        assert(wait(&status) > 0);
        assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }
    expect_file(path, c1_wallpaper_default, sizeof(c1_wallpaper_default));
    listing = opendir(directory);
    assert(listing != NULL);
    while ((entry = readdir(listing)) != NULL) assert(strncmp(entry->d_name, ".wallpaper-", 11U) != 0);
    assert(closedir(listing) == 0);

    assert(unlink(path) == 0);
    assert(unlink(other) == 0);
    assert(unlink(legacy_path) == 0);
    assert(rmdir(directory) == 0);
    assert(rmdir(legacy) == 0);
    assert(rmdir(root) == 0);
    puts("Wallpaper tests passed: default provisioning, fixed name, preservation, migration, unsafe paths, concurrency.");
    return 0;
}
