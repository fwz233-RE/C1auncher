#define _POSIX_C_SOURCE 200809L
#include "platform/media_directories.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int c1_media_open_directory(const char *directory, bool create)
{
    char *copy, *part, *next, *state = NULL;
    int parent;
    if (directory == NULL || directory[0] != '/') return -1;
    copy = strdup(directory);
    if (copy == NULL) return -1;
    parent = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    part = strtok_r(copy, "/", &state);
    while (parent >= 0 && part != NULL) {
        int child;
        next = strtok_r(NULL, "/", &state);
        if (strcmp(part, ".") == 0 || strcmp(part, "..") == 0) {
            (void)close(parent);
            parent = -1;
            break;
        }
        if (create && next == NULL && mkdirat(parent, part, 0755) != 0 && errno != EEXIST) {
            (void)close(parent);
            parent = -1;
            break;
        }
        child = openat(parent, part, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        (void)close(parent);
        parent = child;
        part = next;
    }
    free(copy);
    return parent;
}

bool c1_media_directories_prepare_from(const char *root)
{
    static const char *const names[] = {"Pic", "Music", "Book"};
    size_t index;
    bool prepared = true;
    int parent = c1_media_open_directory(root, false);
    if (parent < 0) return false;
    for (index = 0U; index < sizeof(names) / sizeof(names[0]); ++index) {
        int child;
        if (mkdirat(parent, names[index], 0755) != 0 && errno != EEXIST) {
            prepared = false;
            continue;
        }
        child = openat(parent, names[index], O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (child < 0) prepared = false;
        else (void)close(child);
    }
    if (fsync(parent) != 0) prepared = false;
    (void)close(parent);
    return prepared;
}
