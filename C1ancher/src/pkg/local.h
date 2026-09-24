#ifndef C1PKG_LOCAL_H
#define C1PKG_LOCAL_H

#include "pkg.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

/* Private path walker shared by local metadata and archive readers. Every
 * component is opened relative to an already-open directory, never through a
 * symlink. root == AT_FDCWD permits an absolute repository/key path; callers
 * using a pinned repository fd must first require c1pkg_safe_relpath(). */
static inline int c1pkg_local_openat(int root, const char *path, int directory,
                                    char *error, size_t error_size)
{
    char copy[C1PKG_PATH_MAX];
    char *component, *cursor;
    int fd, next, saved_errno;
    const int flags = O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK;
    if (path == NULL || path[0] == '\0' || strlen(path) >= sizeof(copy) ||
        (root != AT_FDCWD && !c1pkg_safe_relpath(path))) {
        errno = EINVAL;
        c1pkg_set_error(error, error_size, "unsafe local repository path");
        return -1;
    }
    (void)strcpy(copy, path);
    fd = openat(root, path[0] == '/' ? "/" : ".", flags | O_DIRECTORY);
    if (fd < 0) goto failed;
    component = copy;
    for (;;) {
        while (*component == '/') ++component;
        if (*component == '\0') {
            if (directory) return fd;
            errno = EINVAL;
            goto close_failed;
        }
        cursor = component;
        while (*cursor != '\0' && *cursor != '/') {
            if ((unsigned char)*cursor < 0x20U || *cursor == '\\' || *cursor == 0x7f) {
                errno = EINVAL;
                goto close_failed;
            }
            ++cursor;
        }
        if (*cursor != '\0') *cursor++ = '\0';
        while (*cursor == '/') ++cursor;
        if (strcmp(component, "..") == 0) { errno = EINVAL; goto close_failed; }
        if (strcmp(component, ".") == 0) {
            if (*cursor == '\0' && !directory) { errno = EINVAL; goto close_failed; }
            component = cursor;
            continue;
        }
        next = openat(fd, component, flags | ((*cursor != '\0' || directory) ? O_DIRECTORY : 0));
        if (next < 0) goto close_failed;
        (void)close(fd);
        fd = next;
        if (*cursor == '\0') return fd;
        component = cursor;
    }
close_failed:
    saved_errno = errno;
    (void)close(fd);
    errno = saved_errno;
failed:
    c1pkg_set_error(error, error_size, "open local path %s: %s", path, strerror(errno));
    return -1;
}

#endif
