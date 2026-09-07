/* Host-only fault injection. No production environment-variable test hooks. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

ssize_t write(int fd, const void *data, size_t size)
{
    static ssize_t (*real_write)(int, const void *, size_t);
    static unsigned int calls;
    const char *target = getenv("C1_TEST_FAIL_LOG");
    char descriptor[64], path[PATH_MAX];
    ssize_t length;
    if (!real_write) *(void **)(&real_write) = dlsym(RTLD_NEXT, "write");
    (void)snprintf(descriptor, sizeof(descriptor), "/proc/self/fd/%d", fd);
    length = readlink(descriptor, path, sizeof(path) - 1);
    if (length >= 0) {
        path[length] = '\0';
        if (target && strcmp(path, target) == 0) {
            if (calls++ > 0) { errno = ENOSPC; return -1; }
            if (size > 7) size = 7;
        }
    }
    return real_write(fd, data, size);
}

int pipe2(int descriptors[2], int flags)
{
    static int (*real_pipe2)(int [2], int);
    if (getenv("C1_TEST_FAIL_PIPE")) { errno = EMFILE; return -1; }
    if (!real_pipe2) *(void **)(&real_pipe2) = dlsym(RTLD_NEXT, "pipe2");
    return real_pipe2(descriptors, flags);
}
