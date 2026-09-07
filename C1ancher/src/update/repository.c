#include "update/repository.h"
#include "update/io.h"
#include "platform/repository_endpoint.h"
#include "security/secure_file.h"
#include "security/sha256.h"
#include "security/trusted_ed25519.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

int c1_update_repository_validate_url(const char *base_url,
                                      char *error, size_t error_size)
{
    size_t i, scheme, length;
    const char *authority;
    const char *path;
    if (base_url == NULL) goto rejected;
    length = strlen(base_url);
    if (length == 0U || length > C1_UPDATE_URL_MAX) goto rejected;
    if (strncmp(base_url, "https://", 8U) == 0) scheme = 8U;
    else if (strncmp(base_url, "http://", 7U) == 0) scheme = 7U;
    else goto rejected;
    authority = base_url + scheme;
    path = strchr(authority, '/');
    if ((path == authority) || (path == NULL && *authority == '\0')) goto rejected;
    for (i = 0U; i < length; ++i) {
        unsigned char character = (unsigned char)base_url[i];
        if (character < 0x21U || character == 0x7fU || character == (unsigned char)'\\' ||
            character == (unsigned char)'#' || character == (unsigned char)'?') goto rejected;
    }
    {
        size_t authority_length = path == NULL ? strlen(authority) : (size_t)(path - authority);
        if (memchr(authority, '@', authority_length) != NULL || authority_length == 0U) goto rejected;
    }
    if (strstr(base_url, "/../") != NULL || (length >= 3U && strcmp(base_url + length - 3U, "/..") == 0)) goto rejected;
    return 0;
rejected:
    c1_secure_set_error(error, error_size, "repository base URL rejected");
    return -1;
}

int c1_update_repository_read_url(const char *path, char *url, size_t url_size,
                                  char *error, size_t error_size)
{
    struct stat information;
    unsigned char *data = NULL;
    size_t size = 0U;
    int result = -1;
    if (url == NULL || url_size == 0U || path == NULL) return -1;
    url[0] = '\0';
    if (lstat(path, &information) != 0) {
        if (errno == ENOENT && sizeof(C1_UPDATE_DEFAULT_REPOSITORY) <= url_size) {
            (void)memcpy(url, C1_UPDATE_DEFAULT_REPOSITORY, sizeof(C1_UPDATE_DEFAULT_REPOSITORY));
            return 0;
        }
        c1_secure_set_error(error, error_size, "core repository.url cannot be read or URL buffer is too small");
        return -1;
    }
    if (!S_ISREG(information.st_mode) || information.st_nlink != 1 ||
        information.st_uid != geteuid() ||
        c1_secure_read_file(path, &data, &size, C1_UPDATE_URL_MAX + 2U,
                            C1_SECURE_FILE_ANY_SIZE, error, error_size) != 0) goto done;
    if (size > 0U && data[size - 1U] == '\n') --size;
    if (size > 0U && data[size - 1U] == '\r') --size;
    data[size] = '\0';
    if (size == 0U || size >= url_size || strlen((const char *)data) != size ||
        c1_update_repository_validate_url((const char *)data, error, error_size) != 0) goto done;
    (void)memcpy(url, data, size + 1U);
    result = 0;
done:
    free(data);
    if (result != 0)
        c1_secure_set_error(error, error_size, "invalid core repository.url; install a valid repository profile");
    return result;
}

static int safe_relative_path(const char *path)
{
    size_t i, length;
    if (path == NULL || path[0] == '\0' || path[0] == '/') return 0;
    length = strlen(path);
    if (length > 255U || strstr(path, "..") != NULL) return 0;
    for (i = 0U; i < length; ++i) {
        unsigned char c = (unsigned char)path[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '/' || c == '.' || c == '_' || c == '-')) return 0;
    }
    return 1;
}

static const char *curl_path(void)
{
    static const char *const paths[] = { "/usr/bin/curl", "/bin/curl" };
    size_t i;
    for (i = 0U; i < sizeof(paths) / sizeof(paths[0]); ++i) {
        if (access(paths[i], X_OK) == 0) return paths[i];
    }
    return NULL;
}

/* Retry only explicit backpressure, independent of the installed curl version.
 * At most 120 seconds may elapse before starting a retry. A transfer gets 600
 * seconds: 32 MiB at 150000 B/s plus the server's 120-second queue fits within it.
 * The server enforces its global two-transfer / 300000 B/s budget. */
#define C1_UPDATE_HTTP_RETRY_WINDOW_SECONDS 120
#define C1_UPDATE_HTTP_RETRY_DELAY_SECONDS 5
#define C1_UPDATE_HTTP_MAX_ATTEMPTS 25U
/* Manifest and signature share one short deadline per origin. */
#define C1_UPDATE_METADATA_SECONDS 45

static int64_t repository_now(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return -1;
    return (int64_t)now.tv_sec;
}

static int download_attempt(const char *curl, const char *url, int descriptor,
                            uint64_t maximum_size, const char *limit_text,
                            const char *timeout_text, int *http_status)
{
    int reports[2], status = 0;
    char output[64], report[4] = {0};
    size_t used = 0U;
    pid_t child, waited;
    *http_status = 0;
    if (ftruncate(descriptor, 0) != 0 || lseek(descriptor, 0, SEEK_SET) < 0 ||
        snprintf(output, sizeof(output), "/proc/self/fd/%d", descriptor) < 0 ||
        pipe(reports) != 0) return -1;
    child = fork();
    if (child == 0) {
        struct rlimit limit;
        const char *protocols = strncmp(url, "http://", 7U) == 0 ? "=http" : "=http,https";
        const char *const arguments[] = {
            curl, "--disable", "--fail", "--silent", "--show-error", "--location",
            "--proto", protocols, "--proto-redir", protocols,
            "--max-redirs", "5", "--connect-timeout", "15", "--max-time", timeout_text,
            "--limit-rate", "150000", "--max-filesize", limit_text,
            "--output", output, "--write-out", "%{http_code}", "--", url, NULL
        };
        (void)close(reports[0]);
        limit.rlim_cur = (rlim_t)maximum_size;
        limit.rlim_max = (rlim_t)maximum_size;
        if ((uint64_t)limit.rlim_cur != maximum_size || setrlimit(RLIMIT_FSIZE, &limit) != 0 ||
            fcntl(descriptor, F_SETFD, 0) != 0 ||
            dup2(reports[1], STDOUT_FILENO) < 0) _exit(126);
        if (reports[1] != STDOUT_FILENO) (void)close(reports[1]);
        execv(curl, (char *const *)arguments);
        _exit(127);
    }
    (void)close(reports[1]);
    if (child < 0) {
        (void)close(reports[0]);
        return -1;
    }
    do {
        waited = waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    while (used < sizeof(report)) {
        ssize_t amount = read(reports[0], report + used, sizeof(report) - used);
        if (amount < 0 && errno == EINTR) continue;
        if (amount <= 0) break;
        used += (size_t)amount;
    }
    (void)close(reports[0]);
    if (used == 3U && report[0] >= '0' && report[0] <= '9' &&
        report[1] >= '0' && report[1] <= '9' && report[2] >= '0' && report[2] <= '9')
        *http_status = (report[0] - '0') * 100 + (report[1] - '0') * 10 + report[2] - '0';
    /* A terminated transfer must not start another origin after cancellation. */
    if (waited != child || !WIFEXITED(status)) return -2;
    /* curl can report 63 before --fail when a busy response's Content-Length
     * exceeds a small artifact/signature bound. Both are explicit HTTP failures;
     * a 429/503 error body is never accepted as payload. */
    if ((WEXITSTATUS(status) == 22 || WEXITSTATUS(status) == 63) &&
        (*http_status == 429 || *http_status == 503)) return 1;
    return WEXITSTATUS(status) == 0 && *http_status >= 200 && *http_status < 300 ? 0 : -1;
}

/* -2 is cancellation; -3 is a local/request error, neither permits failover. */
static int download_origin(const char *base_url, const char *relative_path,
                           const char *destination, uint64_t maximum_size,
                           uint64_t exact_size, int64_t deadline,
                           char *error, size_t error_size)
{
    char url[C1_UPDATE_URL_MAX + 257U];
    char limit_text[32], timeout_text[32];
    struct stat information;
    const char *curl = curl_path();
    int descriptor = -1, count, busy = 0, failure = -3;
    unsigned int attempt;
    int64_t started;
    unsigned char digest[C1_SHA256_SIZE];
    uint64_t downloaded;
    if (c1_update_repository_validate_url(base_url, error, error_size) != 0 ||
        !safe_relative_path(relative_path) || destination == NULL || curl == NULL ||
        maximum_size == 0U || maximum_size == C1_SHA256_ANY_SIZE ||
        (exact_size != C1_SHA256_ANY_SIZE && exact_size > maximum_size)) {
        c1_secure_set_error(error, error_size, "invalid repository download request");
        return -3;
    }
    count = snprintf(url, sizeof(url), "%s%s%s", base_url,
                     base_url[strlen(base_url) - 1U] == '/' ? "" : "/", relative_path);
    if (count < 0 || (size_t)count >= sizeof(url)) {
        c1_secure_set_error(error, error_size, "repository URL is too long");
        return -3;
    }
    count = snprintf(limit_text, sizeof(limit_text), "%llu", (unsigned long long)maximum_size);
    if (count < 0 || (size_t)count >= sizeof(limit_text)) return -3;
    descriptor = open(destination, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (descriptor < 0 || fstat(descriptor, &information) != 0 || !S_ISREG(information.st_mode) ||
        information.st_nlink != 1) {
        if (descriptor >= 0) (void)close(descriptor);
        c1_secure_set_error(error, error_size, "staging download file rejected");
        return -3;
    }
    if (descriptor <= STDERR_FILENO) {
        int duplicate = fcntl(descriptor, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
        (void)close(descriptor);
        descriptor = duplicate;
        if (descriptor < 0) goto failed;
    }
    started = repository_now();
    if (started < 0) goto failed;
    failure = -1;
    for (attempt = 0U; attempt < C1_UPDATE_HTTP_MAX_ATTEMPTS; ++attempt) {
        int http_status, result;
        int64_t now = repository_now();
        if (now < started || (deadline != 0 && now >= deadline) ||
            (attempt != 0U && now - started >= C1_UPDATE_HTTP_RETRY_WINDOW_SECONDS)) goto failed;
        (void)snprintf(timeout_text, sizeof(timeout_text), "%lld",
                       (long long)(deadline != 0 ? deadline - now : 600));
        result = download_attempt(curl, url, descriptor, maximum_size, limit_text,
                                  timeout_text, &http_status);
        if (result == 0) { busy = 0; break; }
        if (result == -2) { failure = -2; goto failed; }
        busy = result == 1;
        now = repository_now();
        if (!busy || attempt + 1U >= C1_UPDATE_HTTP_MAX_ATTEMPTS ||
            now < started ||
            (deadline != 0 && now + C1_UPDATE_HTTP_RETRY_DELAY_SECONDS >= deadline) ||
            now - started + C1_UPDATE_HTTP_RETRY_DELAY_SECONDS >=
                C1_UPDATE_HTTP_RETRY_WINDOW_SECONDS) goto failed;
        {
            /* The read-only proxy contract is Retry-After: 5. */
            struct timespec remaining = {C1_UPDATE_HTTP_RETRY_DELAY_SECONDS, 0};
            while (nanosleep(&remaining, &remaining) != 0) {
                if (errno != EINTR) goto failed;
            }
        }
    }
    failure = -3;
    if (fsync(descriptor) != 0) {
        goto failed;
    }
    if (close(descriptor) != 0) {
        descriptor = -1;
        goto failed;
    }
    descriptor = -1;
    failure = -1;
    if (c1_sha256_file(destination, maximum_size, exact_size, digest, &downloaded,
                       error, error_size) != 0 ||
        (exact_size == C1_SHA256_ANY_SIZE && downloaded > maximum_size)) goto failed;
    return 0;
failed:
    if (descriptor >= 0) (void)close(descriptor);
    (void)unlink(destination);
    c1_secure_set_error(error, error_size, busy ? "core repository busy (HTTP 429/503); retry window exhausted" :
                                                 "repository download failed");
    return failure;
}

int c1_update_repository_download(const char *base_url, const char *relative_path,
                                  const char *destination, uint64_t maximum_size,
                                  uint64_t exact_size, char *error, size_t error_size)
{
    char fallback[C1_UPDATE_URL_MAX + 1U];
    int result;
    int metadata = relative_path != NULL &&
        (strcmp(relative_path, C1_UPDATE_MANIFEST_NAME) == 0 ||
         strcmp(relative_path, C1_UPDATE_SIGNATURE_NAME) == 0);
    int64_t now = repository_now();
    if (now < 0) return -1;
    result = download_origin(base_url, relative_path, destination, maximum_size, exact_size,
                             metadata ? now + C1_UPDATE_METADATA_SECONDS : 0, error, error_size);
    if (result == -1 && c1_repository_fallback_url(base_url, fallback, sizeof(fallback))) {
        now = repository_now();
        if (now < 0) return -1;
        result = download_origin(fallback, relative_path, destination, maximum_size, exact_size,
                                 metadata ? now + C1_UPDATE_METADATA_SECONDS : 0, error, error_size);
    }
    if (result == 0 && error != NULL && error_size > 0U) error[0] = '\0';
    return result == 0 ? 0 : -1;
}

void c1_update_repository_release_free(struct c1_update_release *release)
{
    if (release != NULL) {
        free(release->manifest_data);
        (void)memset(release, 0, sizeof(*release));
    }
}

int c1_update_repository_load_local(const char *release_directory, const char *key_path,
                                    struct c1_update_release *release,
                                    char *error, size_t error_size)
{
    char manifest_path[C1_UPDATE_PATH_MAX], signature_path[C1_UPDATE_PATH_MAX];
    unsigned char *signature = NULL;
    size_t signature_size = 0U;
    if (release == NULL || c1_update_join_path(manifest_path, sizeof(manifest_path), release_directory,
                                               C1_UPDATE_MANIFEST_NAME) != 0 ||
        c1_update_join_path(signature_path, sizeof(signature_path), release_directory,
                            C1_UPDATE_SIGNATURE_NAME) != 0) return -1;
    (void)memset(release, 0, sizeof(*release));
    if (c1_secure_read_file(manifest_path, &release->manifest_data, &release->manifest_size,
                            C1_UPDATE_MANIFEST_MAX, C1_SECURE_FILE_ANY_SIZE, error, error_size) != 0 ||
        c1_secure_read_file(signature_path, &signature, &signature_size, 64U, 64U,
                            error, error_size) != 0 ||
        c1_trusted_ed25519_verify_files(key_path, signature_path, release->manifest_data,
                                       release->manifest_size, error, error_size) != 0 ||
        c1_update_parse_manifest(release->manifest_data, release->manifest_size,
                                 &release->manifest, error, error_size) != 0) {
        free(signature);
        c1_update_repository_release_free(release);
        return -1;
    }
    (void)memcpy(release->signature, signature, sizeof(release->signature));
    free(signature);
    return 0;
}

static int fetch_release_origin(const char *base_url, const char *staging_directory,
                                const char *key_path, struct c1_update_release *release,
                                const char *manifest_path, const char *signature_path,
                                char *error, size_t error_size)
{
    int result;
    int64_t now = repository_now();
    if (now < 0) return -3;
    result = download_origin(base_url, C1_UPDATE_MANIFEST_NAME, manifest_path,
                             C1_UPDATE_MANIFEST_MAX, C1_SHA256_ANY_SIZE,
                             now + C1_UPDATE_METADATA_SECONDS, error, error_size);
    if (result == 0)
        result = download_origin(base_url, C1_UPDATE_SIGNATURE_NAME, signature_path,
                                 64U, 64U, now + C1_UPDATE_METADATA_SECONDS, error, error_size);
    if (result == 0)
        result = c1_update_repository_load_local(staging_directory, key_path, release, error, error_size);
    return result;
}

int c1_update_repository_fetch_release(const char *base_url, const char *staging_directory,
                                       const char *key_path, struct c1_update_release *release,
                                       char *error, size_t error_size)
{
    char manifest_path[C1_UPDATE_PATH_MAX], signature_path[C1_UPDATE_PATH_MAX];
    char fallback[C1_UPDATE_URL_MAX + 1U];
    struct stat information;
    int result;
    if (release == NULL ||
        c1_update_join_path(manifest_path, sizeof(manifest_path), staging_directory,
                            C1_UPDATE_MANIFEST_NAME) != 0 ||
        c1_update_join_path(signature_path, sizeof(signature_path), staging_directory,
                            C1_UPDATE_SIGNATURE_NAME) != 0) return -1;
    /* Never remove caller-owned metadata when abandoning an origin. Transactions
     * supply a private, empty directory, but the public API also fails closed. */
    if (lstat(manifest_path, &information) == 0 || errno != ENOENT ||
        lstat(signature_path, &information) == 0 || errno != ENOENT) {
        c1_secure_set_error(error, error_size, "staging metadata already exists or cannot be checked");
        return -1;
    }
    (void)memset(release, 0, sizeof(*release));
    result = fetch_release_origin(base_url, staging_directory, key_path, release,
                                  manifest_path, signature_path, error, error_size);
    if (result == -1 && c1_repository_fallback_url(base_url, fallback, sizeof(fallback))) {
        /* Restart the signed pair together; never combine origins or recurse. */
        if ((unlink(manifest_path) != 0 && errno != ENOENT) ||
            (unlink(signature_path) != 0 && errno != ENOENT)) return -1;
        result = fetch_release_origin(fallback, staging_directory, key_path, release,
                                      manifest_path, signature_path, error, error_size);
    }
    if (result != 0) {
        (void)unlink(manifest_path);
        (void)unlink(signature_path);
    } else if (error != NULL && error_size > 0U) error[0] = '\0';
    return result == 0 ? 0 : -1;
}