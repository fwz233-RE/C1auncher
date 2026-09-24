#include "pkg.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

void c1pkg_set_error(char *error, size_t error_size, const char *format, ...)
{
    va_list arguments;

    if (error == NULL || error_size == 0U) {
        return;
    }
    va_start(arguments, format);
    (void)vsnprintf(error, error_size, format, arguments);
    va_end(arguments);
}

static int safe_token(const char *value, size_t maximum, int version)
{
    size_t i;
    size_t length;

    if (value == NULL) {
        return 0;
    }
    length = strlen(value);
    if (length == 0U || length > maximum || value[0] == '.' || value[length - 1U] == '.') {
        return 0;
    }
    for (i = 0U; i < length; ++i) {
        unsigned char character = (unsigned char)value[i];
        int valid = (character >= (unsigned char)'a' && character <= (unsigned char)'z') ||
                    (character >= (unsigned char)'A' && character <= (unsigned char)'Z') ||
                    (character >= (unsigned char)'0' && character <= (unsigned char)'9') ||
                    character == (unsigned char)'.' || character == (unsigned char)'_' ||
                    character == (unsigned char)'-' ||
                    (version != 0 && character == (unsigned char)'+');
        if (valid == 0 || (character == (unsigned char)'.' && i + 1U < length &&
                           value[i + 1U] == '.')) {
            return 0;
        }
    }
    return 1;
}

int c1pkg_safe_id(const char *value)
{
    return safe_token(value, C1PKG_ID_MAX, 0);
}

int c1pkg_safe_version(const char *value)
{
    return safe_token(value, C1PKG_VERSION_MAX, 1);
}

int c1pkg_safe_relpath(const char *value)
{
    const char *component;
    const char *cursor;
    size_t length;

    if (value == NULL) {
        return 0;
    }
    length = strlen(value);
    if (length == 0U || length > C1PKG_ENTRY_MAX || value[0] == '/' ||
        value[length - 1U] == '/') {
        return 0;
    }
    component = value;
    for (cursor = value;; ++cursor) {
        unsigned char character = (unsigned char)*cursor;
        if (character == (unsigned char)'/' || character == 0U) {
            size_t component_size = (size_t)(cursor - component);
            if (component_size == 0U ||
                (component_size == 1U && component[0] == '.') ||
                (component_size == 2U && component[0] == '.' && component[1] == '.')) {
                return 0;
            }
            if (character == 0U) {
                break;
            }
            component = cursor + 1;
        } else if (character < 0x21U || character > 0x7eU || character == (unsigned char)'\\' ||
                   character == (unsigned char)':' || character == (unsigned char)'*' ||
                   character == (unsigned char)'?' || character == (unsigned char)'[') {
            return 0;
        }
    }
    return 1;
}

int c1pkg_join(char *out, size_t out_size, const char *left, const char *right)
{
    int count;
    const char *separator;

    if (out == NULL || left == NULL || right == NULL || out_size == 0U) {
        return -1;
    }
    separator = left[0] != '\0' && left[strlen(left) - 1U] == '/' ? "" : "/";
    count = snprintf(out, out_size, "%s%s%s", left, separator, right);
    return count >= 0 && (size_t)count < out_size ? 0 : -1;
}

int c1pkg_mkdir_p(const char *path, mode_t mode, char *error, size_t error_size)
{
    char copy[C1PKG_PATH_MAX];
    char *cursor;

    if (path == NULL || strlen(path) >= sizeof(copy)) {
        c1pkg_set_error(error, error_size, "directory path is too long");
        return -1;
    }
    (void)strcpy(copy, path);
    for (cursor = copy + 1; *cursor != '\0'; ++cursor) {
        if (*cursor == '/') {
            *cursor = '\0';
            if (mkdir(copy, mode) != 0 && errno != EEXIST) {
                c1pkg_set_error(error, error_size, "mkdir %s: %s", copy, strerror(errno));
                return -1;
            }
            *cursor = '/';
        }
    }
    if (mkdir(copy, mode) != 0 && errno != EEXIST) {
        c1pkg_set_error(error, error_size, "mkdir %s: %s", copy, strerror(errno));
        return -1;
    }
    if (chmod(copy, mode) != 0) {
        c1pkg_set_error(error, error_size, "chmod %s: %s", copy, strerror(errno));
        return -1;
    }
    return 0;
}

int c1pkg_remove_tree(const char *path, char *error, size_t error_size)
{
    struct stat information;
    DIR *directory;
    struct dirent *entry;

    if (lstat(path, &information) != 0) {
        if (errno == ENOENT) {
            return 0;
        }
        c1pkg_set_error(error, error_size, "lstat %s: %s", path, strerror(errno));
        return -1;
    }
    if (!S_ISDIR(information.st_mode) || S_ISLNK(information.st_mode)) {
        if (unlink(path) != 0) {
            c1pkg_set_error(error, error_size, "unlink %s: %s", path, strerror(errno));
            return -1;
        }
        return 0;
    }
    directory = opendir(path);
    if (directory == NULL) {
        c1pkg_set_error(error, error_size, "opendir %s: %s", path, strerror(errno));
        return -1;
    }
    while ((entry = readdir(directory)) != NULL) {
        char child[C1PKG_PATH_MAX];
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        if (c1pkg_join(child, sizeof(child), path, entry->d_name) != 0 ||
            c1pkg_remove_tree(child, error, error_size) != 0) {
            (void)closedir(directory);
            return -1;
        }
    }
    if (closedir(directory) != 0 || rmdir(path) != 0) {
        c1pkg_set_error(error, error_size, "remove directory %s: %s", path, strerror(errno));
        return -1;
    }
    return 0;
}

int c1pkg_read_file(const char *path, unsigned char **data, size_t *size,
                    size_t limit, char *error, size_t error_size)
{
    struct stat information;
    unsigned char *buffer;
    size_t used = 0U;
    int descriptor;

    descriptor = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (descriptor < 0) {
        c1pkg_set_error(error, error_size, "open %s: %s", path, strerror(errno));
        return -1;
    }
    if (fstat(descriptor, &information) != 0 || !S_ISREG(information.st_mode) ||
        information.st_size < 0 || (uint64_t)information.st_size > (uint64_t)limit) {
        (void)close(descriptor);
        c1pkg_set_error(error, error_size, "invalid or oversized file: %s", path);
        return -1;
    }
    buffer = malloc((size_t)information.st_size + 1U);
    if (buffer == NULL) {
        (void)close(descriptor);
        c1pkg_set_error(error, error_size, "out of memory");
        return -1;
    }
    while (used < (size_t)information.st_size) {
        ssize_t amount = read(descriptor, buffer + used, (size_t)information.st_size - used);
        if (amount < 0 && errno == EINTR) {
            continue;
        }
        if (amount <= 0) {
            free(buffer);
            (void)close(descriptor);
            c1pkg_set_error(error, error_size, "short read from %s", path);
            return -1;
        }
        used += (size_t)amount;
    }
    buffer[used] = '\0';
    (void)close(descriptor);
    *data = buffer;
    *size = used;
    return 0;
}

int c1pkg_write_file(const char *path, const void *data, size_t size, mode_t mode,
                     char *error, size_t error_size)
{
    const unsigned char *bytes = data;
    size_t written = 0U;
    int descriptor = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, mode);

    if (descriptor < 0) {
        c1pkg_set_error(error, error_size, "open %s: %s", path, strerror(errno));
        return -1;
    }
    while (written < size) {
        ssize_t amount = write(descriptor, bytes + written, size - written);
        if (amount < 0 && errno == EINTR) {
            continue;
        }
        if (amount <= 0) {
            c1pkg_set_error(error, error_size, "write %s: %s", path, strerror(errno));
            (void)close(descriptor);
            return -1;
        }
        written += (size_t)amount;
    }
    {
        int sync_result = fsync(descriptor);
        int saved_errno = errno;
        int close_result = close(descriptor);
        if (sync_result != 0 || close_result != 0) {
            if (sync_result != 0) errno = saved_errno;
            c1pkg_set_error(error, error_size, "sync %s: %s", path, strerror(errno));
            return -1;
        }
    }
    return 0;
}

int c1pkg_sync_directory(const char *path, char *error, size_t error_size)
{
    struct stat information;
    int descriptor;

    if (stat(path, &information) != 0 || !S_ISDIR(information.st_mode)) {
        c1pkg_set_error(error, error_size, "invalid directory %s", path);
        return -1;
    }
    descriptor = open(path, O_RDONLY | O_CLOEXEC);
    if (descriptor < 0) {
        c1pkg_set_error(error, error_size, "open directory %s: %s", path,
                        strerror(errno));
        return -1;
    }
    if (fsync(descriptor) != 0) {
        int error_number = errno;

        (void)close(descriptor);
        c1pkg_set_error(error, error_size, "sync directory %s: %s", path,
                        strerror(error_number));
        return -1;
    }
    if (close(descriptor) != 0) {
        c1pkg_set_error(error, error_size, "close directory %s: %s", path,
                        strerror(errno));
        return -1;
    }
    return 0;
}

const char *c1pkg_helper(const char *absolute, const char *name)
{
    return access(absolute, X_OK) == 0 ? absolute : name;
}

int c1pkg_is_internal_id(const char *id)
{
    return id != NULL && strcasecmp(id, "c1-ime") == 0;
}

static int (*progress_callback)(const char *, void *);
static void *progress_context;
static int progress_cancelled;

void c1pkg_set_progress(int (*callback)(const char *, void *), void *context)
{
    progress_callback = callback;
    progress_context = context;
    progress_cancelled = 0;
}

int c1pkg_progress(const char *message)
{
    if (progress_callback != NULL && progress_callback(message, progress_context) != 0) {
        progress_cancelled = 1;
    }
    return progress_cancelled;
}

/* Private transport helper, declared locally in repo.c. Body bytes flow through
 * the parent so both the exact byte ceiling and persistent I/O errno survive.
 * The child may only write 64 KiB regular files (curl response headers). */
int c1pkg_run_bounded(char *const argv[], int output, uint64_t limit,
                      uint64_t *received, int *exit_code, char *error, size_t error_size)
{
    int pipes[2], status = 0, failure = 0, saved_errno = 0, eof = 0, reaped = 0;
    pid_t child;
    *received = 0U;
    *exit_code = -1;
    if (c1pkg_progress(NULL) != 0) {
        c1pkg_set_error(error, error_size, "Cancelled");
        errno = ECANCELED;
        return -2;
    }
    if (pipe(pipes) != 0) goto setup_failed;
    (void)fcntl(pipes[0], F_SETFD, FD_CLOEXEC);
    (void)fcntl(pipes[1], F_SETFD, FD_CLOEXEC);
    child = fork();
    if (child < 0) {
        saved_errno = errno;
        (void)close(pipes[0]); (void)close(pipes[1]);
        errno = saved_errno;
        goto setup_failed;
    }
    if (child == 0) {
        struct rlimit bound = {65536U, 65536U};
        (void)setpgid(0, 0);
        (void)close(pipes[0]);
        if (dup2(pipes[1], STDOUT_FILENO) < 0 ||
            setrlimit(RLIMIT_FSIZE, &bound) != 0 || setenv("LC_ALL", "C", 1) != 0) _exit(126);
        if (pipes[1] != STDOUT_FILENO) (void)close(pipes[1]);
        execvp(argv[0], argv);
        _exit(127);
    }
    (void)setpgid(child, child);
    (void)close(pipes[1]);
    while (!eof || !reaped) {
        struct pollfd event = {pipes[0], POLLIN, 0};
        if (c1pkg_progress(NULL) != 0) {
            saved_errno = ECANCELED;
            c1pkg_set_error(error, error_size, "Cancelled");
            failure = 1; break;
        }
        if (!eof) {
            int ready = poll(&event, 1U, 100);
            if (ready < 0 && errno != EINTR) { failure = 1; saved_errno = errno; break; }
            if (ready > 0) {
                unsigned char buffer[16384];
                ssize_t amount = read(pipes[0], buffer, sizeof(buffer));
                if (amount < 0 && errno != EINTR) { failure = 1; saved_errno = errno; break; }
                if (amount == 0) eof = 1;
                if (amount > 0) {
                    size_t used = 0U;
                    if ((uint64_t)amount > limit - *received) {
                        failure = 1; saved_errno = EFBIG;
                        c1pkg_set_error(error, error_size, "download exceeded size limit");
                        break;
                    }
                    while (used < (size_t)amount) {
                        ssize_t written = write(output, buffer + used, (size_t)amount - used);
                        if (written < 0 && errno == EINTR) continue;
                        if (written <= 0) { failure = 1; saved_errno = written == 0 ? EIO : errno; break; }
                        used += (size_t)written;
                        *received += (uint64_t)written;
                    }
                    if (failure) break;
                }
            }
        } else if (!reaped) {
            (void)poll(NULL, 0U, 100);
        }
        if (!reaped) {
            pid_t waited = waitpid(child, &status, WNOHANG);
            if (waited == child) reaped = 1;
            else if (waited < 0 && errno != EINTR) { failure = 1; saved_errno = errno; break; }
        }
    }
    if (failure) {
        /* Kill descendants too, even when the immediate helper already exited. */
        (void)kill(-child, SIGKILL);
        if (!reaped) (void)kill(child, SIGKILL);
    }
    if (!reaped) while (waitpid(child, &status, 0) < 0 && errno == EINTR) { }
    (void)close(pipes[0]);
    if (failure) {
        if (saved_errno != ECANCELED && saved_errno != EFBIG)
            c1pkg_set_error(error, error_size, "download I/O: %s", strerror(saved_errno));
        errno = saved_errno;
        return -2;
    }
    *exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    if (*exit_code == 0) { errno = 0; return 0; }
    c1pkg_set_error(error, error_size, "%s failed%s", argv[0],
                    *exit_code == 127 ? " (helper unavailable)" : "");
    errno = 0;
    return -1;
setup_failed:
    saved_errno = errno;
    c1pkg_set_error(error, error_size, "download helper setup: %s", strerror(saved_errno));
    errno = saved_errno;
    return -2;
}

int c1pkg_run(char *const argv[], const char *stdout_path, uint64_t file_limit,
              char *error, size_t error_size)
{
    pid_t child;
    int status;

    if (c1pkg_progress(NULL) != 0) {
        c1pkg_set_error(error, error_size, "Cancelled");
        errno = ECANCELED;
        return -1;
    }
    child = fork();

    if (child < 0) {
        c1pkg_set_error(error, error_size, "fork: %s", strerror(errno));
        return -1;
    }
    if (child == 0) {
        if (progress_callback != NULL) (void)setpgid(0, 0);
        if (setenv("LC_ALL", "C", 1) != 0) {
            _exit(126);
        }
        if (stdout_path != NULL) {
            int output = open(stdout_path, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
            if (output < 0 || dup2(output, STDOUT_FILENO) < 0) {
                _exit(126);
            }
            if (output != STDOUT_FILENO) {
                (void)close(output);
            }
        }
        if (file_limit > 0U) {
            struct rlimit limit;
            limit.rlim_cur = (rlim_t)file_limit;
            limit.rlim_max = (rlim_t)file_limit;
            if (setrlimit(RLIMIT_FSIZE, &limit) != 0) {
                _exit(126);
            }
        }
        execvp(argv[0], argv);
        _exit(127);
    }
    {
        pid_t waited;
        if (progress_callback != NULL) (void)setpgid(child, child);
        for (;;) {
            waited = waitpid(child, &status, WNOHANG);
            if (waited == child || (waited < 0 && errno != EINTR)) {
                break;
            }
            if (c1pkg_progress(NULL) != 0) {
                (void)kill(-child, SIGKILL);
                (void)kill(child, SIGKILL);
                while (waitpid(child, &status, 0) < 0 && errno == EINTR) { }
                c1pkg_set_error(error, error_size, "Cancelled");
                return -1;
            }
            (void)poll(NULL, 0U, 100);
        }
        if (waited != child) {
            c1pkg_set_error(error, error_size, "wait for %s: %s", argv[0], strerror(errno));
            return -1;
        }
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        c1pkg_set_error(error, error_size, "%s failed%s", argv[0],
                        WIFEXITED(status) && WEXITSTATUS(status) == 127 ? " (helper unavailable)" : "");
        return -1;
    }
    return 0;
}