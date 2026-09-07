#define _GNU_SOURCE
#include "ui/wallpaper.h"
#include "ui/wallpaper_default.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

uint8_t c1_wallpaper_frame[C1_DISPLAY_FRAME_BYTES];

static bool read_frame(int parent)
{
    struct stat information;
    size_t used = 0U;
    bool loaded = false;
    uint8_t extra;
    ssize_t amount;
    int descriptor = openat(parent, C1_WALLPAPER_FILENAME,
                            O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (descriptor < 0) return false;
    if (fstat(descriptor, &information) != 0 ||
        !S_ISREG(information.st_mode) || information.st_nlink != 1 ||
        information.st_size != (off_t)sizeof(c1_wallpaper_frame)) goto done;
    while (used < sizeof(c1_wallpaper_frame)) {
        amount = read(descriptor, c1_wallpaper_frame + used,
                      sizeof(c1_wallpaper_frame) - used);
        if (amount < 0 && errno == EINTR) continue;
        if (amount <= 0) goto done;
        used += (size_t)amount;
    }
    do {
        amount = read(descriptor, &extra, 1U);
    } while (amount < 0 && errno == EINTR);
    loaded = amount == 0 && fstat(descriptor, &information) == 0 &&
             information.st_size == (off_t)sizeof(c1_wallpaper_frame);
done:
    (void)close(descriptor);
    return loaded;
}

bool c1_wallpaper_load_from(const char *directory)
{
    int parent = c1_media_open_directory(directory, false);
    bool loaded = parent >= 0 && read_frame(parent);
    if (parent >= 0) (void)close(parent);
    if (!loaded) memset(c1_wallpaper_frame, 0, sizeof(c1_wallpaper_frame));
    return loaded;
}

/* Publish a complete frame without replacing a file created by another process.
 * Linux 5.10 on the device supports RENAME_NOREPLACE on its storage filesystem. */
static bool publish_missing(int parent)
{
    char temporary[80];
    unsigned int attempt;
    int descriptor = -1;
    size_t used = 0U;
    bool published = false;
    for (attempt = 0U; attempt < 32U; ++attempt) {
        (void)snprintf(temporary, sizeof(temporary), ".wallpaper-%ld-%u.tmp", (long)getpid(), attempt);
        descriptor = openat(parent, temporary, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0644);
        if (descriptor >= 0 || errno != EEXIST) break;
    }
    if (descriptor < 0) return false;
    while (used < sizeof(c1_wallpaper_frame)) {
        ssize_t amount = write(descriptor, c1_wallpaper_frame + used, sizeof(c1_wallpaper_frame) - used);
        if (amount < 0 && errno == EINTR) continue;
        if (amount <= 0) goto done;
        used += (size_t)amount;
    }
    if (fchmod(descriptor, 0644) != 0 || fsync(descriptor) != 0) goto done;
    if (close(descriptor) != 0) {
        descriptor = -1;
        goto done;
    }
    descriptor = -1;
#ifdef SYS_renameat2
    if (syscall(SYS_renameat2, parent, temporary, parent, C1_WALLPAPER_FILENAME, 1U) == 0) {
        published = fsync(parent) == 0;
    } else if (errno == EEXIST) {
        published = read_frame(parent);
    }
#else
    /* Safe fallback for older host toolchains; never use replacing rename(). */
    if (linkat(parent, temporary, parent, C1_WALLPAPER_FILENAME, 0) == 0) {
        (void)unlinkat(parent, temporary, 0);
        published = fsync(parent) == 0;
    } else if (errno == EEXIST) {
        published = read_frame(parent);
    }
#endif
done:
    if (descriptor >= 0) (void)close(descriptor);
    (void)unlinkat(parent, temporary, 0);
    return published;
}

bool c1_wallpaper_load_or_default_from(const char *directory, const char *legacy)
{
    struct stat information;
    bool loaded = false;
    int parent = c1_media_open_directory(directory, true);
    if (parent >= 0) {
        if (fstatat(parent, C1_WALLPAPER_FILENAME, &information, AT_SYMLINK_NOFOLLOW) == 0) {
            loaded = read_frame(parent);
        } else if (errno == ENOENT) {
            /* Migrate only the old fixed file, never an arbitrary image. */
            if (!c1_wallpaper_load_from(legacy)) {
                memcpy(c1_wallpaper_frame, c1_wallpaper_default, sizeof(c1_wallpaper_frame));
            }
            loaded = publish_missing(parent);
        }
        (void)close(parent);
    }
    if (!loaded) memcpy(c1_wallpaper_frame, c1_wallpaper_default, sizeof(c1_wallpaper_frame));
    return loaded;
}

void c1_wallpaper_load(void)
{
    (void)c1_wallpaper_load_or_default_from(C1_WALLPAPER_DIRECTORY, "/storage/mtp/pic");
}
