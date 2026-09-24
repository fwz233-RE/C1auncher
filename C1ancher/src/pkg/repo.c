#include "pkg.h"
#include "local.h"
#include "text.h"
#include "storage_layout.h"
#include "ed25519.h"
#include "sha512.h"

#include <sys/file.h>
#include <stddef.h>
#include "platform/repository_endpoint.h"

#include <time.h>
#include <poll.h>
#include <fcntl.h>
#include <strings.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define C1PKG_INDEX_HEADER "C1PKG-INDEX 1"
#define C1PKG_LINE_MAX 1024U

static int valid_url_base(const char *base)
{
    size_t i;
    size_t length;

    if (base == NULL || (strncmp(base, "http://", 7U) != 0 &&
                         strncmp(base, "https://", 8U) != 0)) {
        return 0;
    }
    length = strlen(base);
    if (length < 9U || length > 1024U) {
        return 0;
    }
    for (i = 0U; i < length; ++i) {
        unsigned char character = (unsigned char)base[i];
        if (character <= 0x20U || character == 0x7fU || character == (unsigned char)'\\') {
            return 0;
        }
    }
    return 1;
}

static int make_url(char *url, size_t url_size, const char *base, const char *suffix)
{
    int count;
    const char *slash = base[strlen(base) - 1U] == '/' ? "" : "/";

    count = snprintf(url, url_size, "%s%s%s", base, slash, suffix);
    return count >= 0 && (size_t)count < url_size ? 0 : -1;
}

int c1pkg_repo_read_url(const char *path, char *url, size_t url_size,
                        char *error, size_t error_size)
{
    unsigned char *data = NULL;
    size_t size = 0U;
    if (url_size == 0U) return -1;
    url[0] = '\0';
    if (access(path, F_OK) != 0 && errno == ENOENT) return 0;
    if (c1pkg_read_file(path, &data, &size, 1026U, error, error_size) != 0) return -1;
    if (size > 0U && data[size - 1U] == '\n') --size;
    if (size > 0U && data[size - 1U] == '\r') --size;
    data[size] = '\0';
    if (size >= url_size || strlen((char *)data) != size || !valid_url_base((char *)data)) {
        free(data);
        c1pkg_set_error(error, error_size, "invalid repository.url; configure repository URL");
        return -1;
    }
    memcpy(url, data, size + 1U);
    free(data);
    return 0;
}

/* Each transport holds a per-repository cross-process lock through request and
 * response persistence. Memory below is only the active operation's view. */
#define C1PKG_AUTO_WAIT_MAX 3600U
#define COOLDOWN_SLOTS 32U
#define COOLDOWN_URL_MAX 1400U
#define COOLDOWN_RECOVERY_MS 60000U
#define COOLDOWN_READY "C1PKG-COOL-1"
#define COOLDOWN_PENDING "C1PKG-REQ-2"
struct cooldown_record {
    char magic[16];
    char scope[COOLDOWN_URL_MAX];
    char boot[37];
    uint64_t written_ms;
    uint64_t duration_ms;
    uint64_t until_ms;
    unsigned char checksum[64];
};
static struct {
    int root;
    int guard;
    unsigned int slot;
    struct cooldown_record record;
} cooldown_active = {.root = -1, .guard = -1};
static char transport_repo_scope[COOLDOWN_URL_MAX];
static uint64_t transport_not_before;

static uint64_t monotonic_milliseconds(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0 || now.tv_sec < 0 ||
        now.tv_nsec < 0 || now.tv_nsec >= 1000000000L ||
        (uint64_t)now.tv_sec > (UINT64_MAX - 999U) / 1000U) return UINT64_MAX;
    return (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
}

/* Canonicalize only equivalences we can prove locally. Preserve path case and
 * isolate different repository paths, but join the official HTTP alias pair.
 * Ambiguous credentials, query/fragment, encoded or dot-segment paths fail
 * closed rather than acquiring a different cooldown key for the same resource. */
static int cooldown_url(const char *input, char output[COOLDOWN_URL_MAX])
{
    size_t prefix, length, authority, host_end, i, used;
    const char *path;
    char host[512];
    if (input == NULL) return -1;
    prefix = strncasecmp(input, "http://", 7U) == 0 ? 7U :
             strncasecmp(input, "https://", 8U) == 0 ? 8U : 0U;
    length = strnlen(input, COOLDOWN_URL_MAX);
    if (prefix == 0U || length <= prefix || length >= COOLDOWN_URL_MAX) return -1;
    path = strchr(input + prefix, '/');
    authority = path != NULL ? (size_t)(path - input) : length;
    host_end = authority;
    if (prefix == 7U && host_end >= prefix + 3U && memcmp(input + host_end - 3U, ":80", 3U) == 0)
        host_end -= 3U;
    else if (prefix == 8U && host_end >= prefix + 4U && memcmp(input + host_end - 4U, ":443", 4U) == 0)
        host_end -= 4U;
    if (host_end <= prefix || host_end - prefix >= sizeof(host)) return -1;
    for (i = prefix; i < host_end; ++i) {
        unsigned char c = (unsigned char)input[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '-' || c == '.' || c == ':' || c == '[' || c == ']')) return -1;
        host[i - prefix] = (char)(c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c);
    }
    host[host_end - prefix] = '\0';
    if (host[0] == ':' || host[strlen(host) - 1U] == '.') return -1;
    if (prefix == 7U && strcmp(host, "www.fwz233.com") == 0) strcpy(host, "123.56.214.77");
    used = (size_t)snprintf(output, COOLDOWN_URL_MAX, "%s%s", prefix == 7U ? "http://" : "https://", host);
    if (used >= COOLDOWN_URL_MAX) return -1;
    if (path != NULL) {
        if (strstr(path, "//") != NULL || strstr(path, "/./") != NULL || strstr(path, "/../") != NULL)
            return -1;
        for (i = authority; i < length; ++i) {
            unsigned char c = (unsigned char)input[i];
            if (c <= 0x20U || c >= 0x7fU || strchr("\\%?#{}[]", c) != NULL || used + 1U >= COOLDOWN_URL_MAX)
                return -1;
            output[used++] = (char)c;
        }
    }
    while (used > prefix && output[used - 1U] == '/') --used;
    output[used] = '\0';
    if ((used >= 2U && strcmp(output + used - 2U, "/.") == 0) ||
        (used >= 3U && strcmp(output + used - 3U, "/..") == 0)) return -1;
    return 0;
}

int c1pkg_repo_bind_transport(const struct c1pkg_config *config)
{
    transport_repo_scope[0] = '\0';
    if (config == NULL || cooldown_url(config->repo_base, transport_repo_scope) != 0) {
        transport_repo_scope[0] = '\0';
        return -1;
    }
    return 0;
}

static int cooldown_contains(const char *scope, const char *url)
{
    size_t n = strlen(scope);
    return strncmp(scope, url, n) == 0 && (url[n] == '\0' || url[n] == '/');
}

static int cooldown_boot(char boot[37])
{
    char data[38];
    ssize_t n;
    size_t i;
    int fd = open("/proc/sys/kernel/random/boot_id", O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return -1;
    do { n = read(fd, data, sizeof(data)); } while (n < 0 && errno == EINTR);
    (void)close(fd);
    if (n != 37 || data[36] != '\n') return -1;
    for (i = 0U; i < 36U; ++i) {
        if (i == 8U || i == 13U || i == 18U || i == 23U) { if (data[i] != '-') return -1; }
        else if (!((data[i] >= '0' && data[i] <= '9') || (data[i] >= 'a' && data[i] <= 'f'))) return -1;
    }
    memcpy(boot, data, 36U); boot[36] = '\0';
    return 0;
}

static int cooldown_private(int fd, int directory)
{
    struct stat st;
    return fstat(fd, &st) == 0 && st.st_uid == geteuid() &&
           (st.st_mode & 0777) == (directory ? 0700 : 0600) &&
           (directory ? S_ISDIR(st.st_mode) : S_ISREG(st.st_mode) && st.st_nlink == 1);
}

static int cooldown_open_lock(int root, const char *name)
{
    int fd = openat(root, name, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
    if (fd >= 0 && !cooldown_private(fd, 0)) { (void)close(fd); fd = -1; errno = EACCES; }
    return fd;
}

static int cooldown_pause(char *error, size_t error_size)
{
    if (c1pkg_progress("正在等待同一仓库的下载操作") != 0) {
        errno = ECANCELED; c1pkg_set_error(error, error_size, "操作已取消"); return -1;
    }
    if (poll(NULL, 0U, 100) < 0 && errno != EINTR) return -1;
    return 0;
}

static int cooldown_read(int root, unsigned int slot, struct cooldown_record *record)
{
    char name[32];
    struct stat st;
    unsigned char digest[64], extra;
    char normalized[COOLDOWN_URL_MAX];
    size_t used = 0U;
    int fd;
    ssize_t n;
    (void)snprintf(name, sizeof(name), "%02u.state", slot);
    fd = openat(root, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) {
        if (errno != ENOENT) return -1;
        /* An assigned slot's lock outlives every request. A missing state with
         * a surviving lock is lost state, not a fresh zero-cooldown repository. */
        (void)snprintf(name, sizeof(name), "%02u.lock", slot);
        return fstatat(root, name, &st, AT_SYMLINK_NOFOLLOW) != 0 && errno == ENOENT ? 1 : -1;
    }
    (void)snprintf(name, sizeof(name), "%02u.lock", slot);
    if (fstatat(root, name, &st, AT_SYMLINK_NOFOLLOW) != 0 || !S_ISREG(st.st_mode) ||
        st.st_nlink != 1 || st.st_uid != geteuid() || (st.st_mode & 0777) != 0600) goto failed;
    if (!cooldown_private(fd, 0) || fstat(fd, &st) != 0 || st.st_size != (off_t)sizeof(*record)) goto failed;
    while (used < sizeof(*record)) {
        n = read(fd, (unsigned char *)record + used, sizeof(*record) - used);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) goto failed;
        used += (size_t)n;
    }
    do { n = read(fd, &extra, 1U); } while (n < 0 && errno == EINTR);
    (void)close(fd);
    if (n != 0 || sha512((const unsigned char *)record, offsetof(struct cooldown_record, checksum), digest) != 0 ||
        memcmp(digest, record->checksum, sizeof(digest)) != 0 ||
        (memcmp(record->magic, COOLDOWN_READY, sizeof(COOLDOWN_READY)) != 0 &&
         memcmp(record->magic, COOLDOWN_PENDING, sizeof(COOLDOWN_PENDING)) != 0) ||
        memchr(record->scope, 0, sizeof(record->scope)) == NULL ||
        cooldown_url(record->scope, normalized) != 0 || strcmp(normalized, record->scope) != 0 ||
        record->boot[36] != '\0' || strlen(record->boot) != 36U ||
        record->written_ms == UINT64_MAX || record->until_ms < record->written_ms ||
        record->until_ms != (record->duration_ms > UINT64_MAX - record->written_ms ?
                            UINT64_MAX : record->written_ms + record->duration_ms)) return -1;
    return 0;
failed:
    (void)close(fd);
    return -1;
}

static int cooldown_store(void)
{
    struct cooldown_record *record = &cooldown_active.record;
    char name[32], temp[32];
    int fd, result = -1;
    size_t used = 0U;
    (void)snprintf(name, sizeof(name), "%02u.state", cooldown_active.slot);
    (void)snprintf(temp, sizeof(temp), "%02u.tmp", cooldown_active.slot);
    if (sha512((const unsigned char *)record, offsetof(struct cooldown_record, checksum), record->checksum) != 0)
        return -1;
    (void)unlinkat(cooldown_active.root, temp, 0);
    fd = openat(cooldown_active.root, temp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return -1;
    while (used < sizeof(*record)) {
        ssize_t n = write(fd, (const unsigned char *)record + used, sizeof(*record) - used);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) goto done;
        used += (size_t)n;
    }
    if (fsync(fd) == 0) result = 0;
done:
    if (close(fd) != 0) result = -1;
    if (result == 0 && renameat(cooldown_active.root, temp, cooldown_active.root, name) == 0 &&
        fsync(cooldown_active.root) == 0) return 0;
    (void)unlinkat(cooldown_active.root, temp, 0);
    return -1;
}

static void cooldown_close(void)
{
    if (cooldown_active.guard >= 0) (void)close(cooldown_active.guard);
    if (cooldown_active.root >= 0) (void)close(cooldown_active.root);
    cooldown_active.guard = -1; cooldown_active.root = -1;
}

/* At most 32 immutable repository assignments, 32 state files and 32 locks.
 * The tiny registry lock protects assignment only; unrelated repositories can
 * transfer concurrently. Never evict a pending deadline to admit another URL. */
static int cooldown_open(const char *url, char *error, size_t error_size)
{
    char normalized[COOLDOWN_URL_MAX], scope[COOLDOWN_URL_MAX], boot[37], name[32];
    struct cooldown_record candidate, selected;
    struct stat st;
    uint64_t now;
    int parent = -1, registry = -1, guard = -1, choice, vacant, found, i, status;
    if (cooldown_active.root >= 0 || cooldown_url(url, normalized) != 0 || cooldown_boot(boot) != 0 ||
        (now = monotonic_milliseconds()) == UINT64_MAX) goto failed;
    if (lstat(C1PKG_STATE_ROOT, &st) == 0) {
        if (!S_ISDIR(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & 0777) != 0700) goto failed;
    } else if (errno != ENOENT || c1pkg_storage_state_init(error, error_size) != 0) goto failed;
    parent = open(C1PKG_STATE_ROOT, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (parent < 0 || !cooldown_private(parent, 1)) goto failed;
    if (mkdirat(parent, "cooldown", 0700) == 0) {
        if (fsync(parent) != 0) goto failed;
    } else if (errno != EEXIST) goto failed;
    cooldown_active.root = openat(parent, "cooldown", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    (void)close(parent); parent = -1;
    if (cooldown_active.root < 0 || !cooldown_private(cooldown_active.root, 1) ||
        (registry = cooldown_open_lock(cooldown_active.root, "registry.lock")) < 0) goto failed;
    for (;;) {
        if (flock(registry, LOCK_EX | LOCK_NB) != 0) {
            if (errno != EWOULDBLOCK && errno != EAGAIN) goto failed;
            if (cooldown_pause(error, error_size) != 0) goto cancelled;
            continue;
        }
        choice = -1; vacant = -1; found = 0; scope[0] = '\0';
        if (transport_repo_scope[0] != '\0' && cooldown_contains(transport_repo_scope, normalized)) {
            strcpy(scope, transport_repo_scope); found = 1;
        }
        for (i = 0; i < (int)COOLDOWN_SLOTS; ++i) {
            status = cooldown_read(cooldown_active.root, (unsigned int)i, &candidate);
            if (status < 0) goto failed;
            if (status == 1) { if (vacant < 0) vacant = i; continue; }
            if ((found && strcmp(scope, candidate.scope) == 0) ||
                (!found && cooldown_contains(candidate.scope, normalized) && strlen(candidate.scope) > strlen(scope))) {
                choice = i; selected = candidate;
                if (!found) strcpy(scope, candidate.scope);
            }
        }
        if (choice < 0) {
            if (vacant < 0) { errno = ENOSPC; goto failed; }
            if (!found) {
                char *slash;
                strcpy(scope, normalized);
                slash = strrchr(scope + (strncmp(scope, "https://", 8U) == 0 ? 8U : 7U), '/');
                if (slash != NULL) *slash = '\0';
            }
            choice = vacant;
            memset(&selected, 0, sizeof(selected));
            strcpy(selected.magic, COOLDOWN_READY); strcpy(selected.scope, scope); strcpy(selected.boot, boot);
            selected.written_ms = now; selected.until_ms = now;
        }
        (void)snprintf(name, sizeof(name), "%02u.lock", (unsigned int)choice);
        guard = cooldown_open_lock(cooldown_active.root, name);
        if (guard < 0) goto failed;
        if (flock(guard, LOCK_EX | LOCK_NB) == 0) break;
        status = errno;
        (void)close(guard); guard = -1;
        (void)flock(registry, LOCK_UN);
        if ((status != EWOULDBLOCK && status != EAGAIN) || cooldown_pause(error, error_size) != 0) goto cancelled;
    }
    /* Another transfer may have atomically updated this slot between our scan
     * and successful guard acquisition. Never overwrite its fresh deadline with
     * the scanned copy. Assignments themselves are immutable under registry. */
    if (choice != vacant) {
        status = cooldown_read(cooldown_active.root, (unsigned int)choice, &candidate);
        if (status != 0) goto failed;
        selected = candidate;
    }
    cooldown_active.guard = guard; guard = -1;
    cooldown_active.slot = (unsigned int)choice;
    cooldown_active.record = selected;
    /* A new boot has no trustworthy elapsed RTC time. Restart the accepted
     * remaining interval on its monotonic clock and persist conversion once.
     * A same-boot clock rollback remains an independent error. */
    now = monotonic_milliseconds();
    if (now == UINT64_MAX) goto failed;
    if (strcmp(selected.boot, boot) != 0) {
        strcpy(cooldown_active.record.boot, boot);
        cooldown_active.record.written_ms = now;
        cooldown_active.record.until_ms = selected.duration_ms > UINT64_MAX - now ? UINT64_MAX : now + selected.duration_ms;
    } else if (now < selected.written_ms) goto failed;
    if (strcmp(selected.magic, COOLDOWN_PENDING) == 0) {
        uint64_t recovery = now > UINT64_MAX - COOLDOWN_RECOVERY_MS ? UINT64_MAX : now + COOLDOWN_RECOVERY_MS;
        /* Guard ownership proves the previous request no longer owns the slot.
         * Consume this marker once; reopening/cancelling the GUI cannot restart
         * the recovery window. Known server deadlines are never replaced by a
         * shorter recovery delay. Unknown/lost responses use a bounded policy,
         * not a claim that their unseen server Retry-After is known. */
        if (recovery > cooldown_active.record.until_ms) cooldown_active.record.until_ms = recovery;
        cooldown_active.record.written_ms = now;
        cooldown_active.record.duration_ms = cooldown_active.record.until_ms == UINT64_MAX ? UINT64_MAX :
                                             cooldown_active.record.until_ms - now;
        strcpy(cooldown_active.record.magic, COOLDOWN_READY);
    }
    transport_not_before = cooldown_active.record.until_ms;
    if (cooldown_store() != 0) goto failed;
    (void)close(registry);
    return 0;
failed:
    c1pkg_set_error(error, error_size, "仓库冷却记录不可用或已损坏；为避免提前请求已停止联网");
    errno = EAGAIN;
cancelled:
    status = errno;
    if (parent >= 0) (void)close(parent);
    if (registry >= 0) (void)close(registry);
    if (guard >= 0) (void)close(guard);
    cooldown_close();
    errno = status;
    return -1;
}

static int cooldown_commit_state(uint64_t until, const char *magic)
{
    uint64_t now = monotonic_milliseconds();
    if (cooldown_active.root < 0) return 0; /* Pure clock helper tests / local publication delay. */
    if (now == UINT64_MAX || now < cooldown_active.record.written_ms) return -1;
    if (until < cooldown_active.record.until_ms) until = cooldown_active.record.until_ms;
    strcpy(cooldown_active.record.magic, magic);
    cooldown_active.record.written_ms = now;
    cooldown_active.record.duration_ms = until == UINT64_MAX ? UINT64_MAX : until > now ? until - now : 0U;
    cooldown_active.record.until_ms = until == UINT64_MAX ? UINT64_MAX : until > now ? until : now;
    return cooldown_store();
}

static int cooldown_commit(uint64_t until)
{
    return cooldown_commit_state(until, COOLDOWN_READY);
}

static int cooldown_begin_request(void)
{
    return cooldown_commit_state(transport_not_before, COOLDOWN_PENDING);
}

static int remember_cooldown(uint64_t seconds)
{
    uint64_t now = monotonic_milliseconds();
    uint64_t until = seconds > (UINT64_MAX - now) / 1000U ? UINT64_MAX : now + seconds * 1000U;
    if (until > transport_not_before) transport_not_before = until;
    return now == UINT64_MAX ? -1 : cooldown_commit(transport_not_before);
}

static int wait_for_cooldown(char *error, size_t error_size)
{
    uint64_t last_seconds = UINT64_MAX;
    for (;;) {
        uint64_t now = monotonic_milliseconds(), remaining, seconds;
        char message[160];
        if (now == UINT64_MAX || (cooldown_active.root >= 0 && now < cooldown_active.record.written_ms)) {
            c1pkg_set_error(error, error_size, "单调时钟异常；已停止仓库请求");
            errno = EAGAIN;
            return -1;
        }
        if (transport_not_before == UINT64_MAX) {
            c1pkg_set_error(error, error_size, "仓库冷却期限超出范围；已停止联网");
            errno = EAGAIN;
            return -1;
        }
        if (now >= transport_not_before) return 0;
        remaining = transport_not_before - now;
        seconds = remaining / 1000U + (remaining % 1000U != 0U);
        /* Refuse excessive waits instead of shortening the server's minimum.
         * Keep the deadline so a later action cannot retry early either. */
        if (seconds > C1PKG_AUTO_WAIT_MAX) {
            c1pkg_set_error(error, error_size, "服务器要求等待 %llu 秒，超过自动等待上限，请稍后重试",
                            (unsigned long long)seconds);
            errno = EAGAIN;
            return -1;
        }
        (void)snprintf(message, sizeof(message), "等待 %llu 秒后自动继续",
                       (unsigned long long)seconds);
        if (c1pkg_progress(seconds != last_seconds ? message : NULL) != 0) {
            c1pkg_set_error(error, error_size, "操作已取消");
            errno = ECANCELED;
            return -1;
        }
        last_seconds = seconds;
        if (poll(NULL, 0U, remaining < 100U ? (int)remaining : 100) < 0 && errno != EINTR) {
            c1pkg_set_error(error, error_size, "等待失败：%s", strerror(errno));
            return -1;
        }
    }
}

static int retry_wait(uint64_t seconds, char *error, size_t error_size)
{
    if (remember_cooldown(seconds) != 0) {
        c1pkg_set_error(error, error_size, "无法保存仓库冷却时间");
        errno = EAGAIN;
        return -1;
    }
    return wait_for_cooldown(error, error_size);
}

/* Parse HTTP dates without timegm/strptime, locale, or 32-bit time_t overflow.
 * Accept IMF-fixdate and the two obsolete HTTP date forms (RFC 9110). */
static int http_date(const char *text, int64_t *timestamp)
{
    char weekday[10], month[4], zone[4], extra;
    int day, year, hour, minute, second, m;
    int64_t y, era, yoe, days;
    static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    static const int lengths[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (sscanf(text, "%3[A-Za-z], %2d %3[A-Za-z] %4d %2d:%2d:%2d %3[A-Za-z]%c",
               weekday, &day, month, &year, &hour, &minute, &second, zone, &extra) == 8) {
        if (strcmp(zone, "GMT") != 0) return 0;
    } else if (sscanf(text, "%9[A-Za-z], %2d-%3[A-Za-z]-%2d %2d:%2d:%2d %3[A-Za-z]%c",
                      weekday, &day, month, &year, &hour, &minute, &second, zone, &extra) == 8) {
        time_t now = time(NULL);
        struct tm current;
        if (year < 0 || strcmp(zone, "GMT") != 0 || gmtime_r(&now, &current) == NULL) return 0;
        year += ((current.tm_year + 1900) / 100) * 100;
        if (year > current.tm_year + 1950) year -= 100;
    } else if (sscanf(text, "%3[A-Za-z] %3[A-Za-z] %2d %2d:%2d:%2d %4d%c",
                      weekday, month, &day, &hour, &minute, &second, &year, &extra) != 7) {
        return 0;
    }
    for (m = 0; m < 12 && strncmp(month, months + m * 3, 3U) != 0; ++m) {}
    if (m == 12 || year < 1601 || day < 1 ||
        day > lengths[m] + (m == 1 && year % 4 == 0 && (year % 100 != 0 || year % 400 == 0)) ||
        hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 59) return 0;
    y = year - (m < 2);
    era = y / 400;
    yoe = y - era * 400;
    days = era * 146097 + yoe * 365 + yoe / 4 - yoe / 100 +
           (153 * (m + (m > 1 ? -2 : 10)) + 2) / 5 + day - 1 - 719468;
    *timestamp = days * 86400 + hour * 3600 + minute * 60 + second;
    return 1;
}

static int retry_after_seconds(const char *text, int64_t reference, uint64_t *seconds)
{
    const char *cursor = text;
    uint64_t value = 0U;
    int64_t date;
    if (*cursor >= '0' && *cursor <= '9') {
        for (; *cursor >= '0' && *cursor <= '9'; ++cursor) {
            unsigned int digit = (unsigned int)(*cursor - '0');
            value = value > (UINT64_MAX - digit) / 10U ? UINT64_MAX : value * 10U + digit;
        }
        if (*cursor != '\0') return 0;
        *seconds = value;
        return 1;
    }
    if (!http_date(text, &date)) return 0;
    *seconds = date > reference ? (uint64_t)(date - reference) : 0U;
    return 1;
}

/* Last response only: redirect and proxy CONNECT headers must not leak into
 * the final result. Duplicate Retry-After values use the longest valid delay.
 * Date provides the server clock reference when available (device RTCs may
 * be wrong). Re-evaluate all values after Date, regardless of header order. */
static int response_status(const char *path, uint64_t *seconds, int *has_retry_after)
{
    unsigned char *data = NULL;
    size_t size = 0U;
    char *line, *final = NULL;
    size_t i;
    int status = 0;
    int64_t reference = (int64_t)time(NULL);
    *seconds = 0U;
    *has_retry_after = 0;
    if (c1pkg_read_file(path, &data, &size, 65536U, NULL, 0U) != 0) return 0;
    for (i = 0U; i < size; ++i) {
        if (data[i] == '\r' || data[i] == '\n') data[i] = '\0';
    }
    for (line = (char *)data; line < (char *)data + size; line += strlen(line) + 1U) {
        char *end = line + strlen(line);
        while (end > line && (end[-1] == ' ' || end[-1] == '\t')) *--end = '\0';
        if (strncmp(line, "HTTP/", 5U) == 0) {
            char *space = strchr(line, ' ');
            status = space != NULL ? atoi(space + 1) : 0;
            final = line;
            reference = (int64_t)time(NULL);
        } else if (strncasecmp(line, "Date:", 5U) == 0) {
            char *value = line + 5U;
            int64_t date;
            while (*value == ' ' || *value == '\t') ++value;
            if (http_date(value, &date)) reference = date;
        }
    }
    if (final != NULL) {
        char *end = (char *)data + size;
        for (line = final; line < end; ++line) {
            uint64_t delay;
            char *value;
            if (line != final && line[-1] != '\0') continue;
            if (strncasecmp(line, "Retry-After:", 12U) != 0) continue;
            value = line + 12U;
            while (*value == ' ' || *value == '\t') ++value;
            if (retry_after_seconds(value, reference, &delay)) {
                if (delay > *seconds) *seconds = delay;
                *has_retry_after = 1;
            }
        }
    }
    free(data);
    return status;
}

static int transient_status(int status)
{
    return status == 408 || status == 429 || status == 500 || status == 502 || status == 503 || status == 504;
}

static int transient_exit(int exit_code)
{
    return exit_code == 5 || exit_code == 6 || exit_code == 7 || exit_code == 18 ||
           exit_code == 28 || exit_code == 52 || exit_code == 55 || exit_code == 56;
}

/* Private to the package transport; keep the public package API unchanged. */
int c1pkg_run_bounded(char *const argv[], int output, uint64_t limit,
                      uint64_t *received, int *exit_code, char *error, size_t error_size);

/* Accept a single, numeric Content-Range in the final response only. Encoding
 * changes and multipart/duplicate ranges are deliberately not resumable. */
static int response_range(const char *path, uint64_t offset, uint64_t received,
                          uint64_t limit, uint64_t *total)
{
    unsigned char *data = NULL;
    size_t size = 0U;
    char *line, *save = NULL;
    unsigned int ranges = 0U;
    int valid = 0, encoded = 0;
    if (c1pkg_read_file(path, &data, &size, 65536U, NULL, 0U) != 0) return 0;
    for (line = strtok_r((char *)data, "\r\n", &save); line != NULL;
         line = strtok_r(NULL, "\r\n", &save)) {
        if (strncmp(line, "HTTP/", 5U) == 0) {
            ranges = 0U; valid = 0; encoded = 0;
        } else if (strncasecmp(line, "Content-Range:", 14U) == 0) {
            unsigned long long first, last, length;
            char extra;
            char *value = line + 14U;
            ++ranges;
            while (*value == ' ' || *value == '\t') ++value;
            /* Bound decimal fields before sscanf, which otherwise accepts signs
             * and has implementation-defined overflow handling. */
            if (strncmp(value, "bytes ", 6U) == 0) {
                char *cursor = value + 6U;
                unsigned int field;
                int numbers = 1;
                for (field = 0U; field < 3U; ++field) {
                    uint64_t number = 0U;
                    char *start = cursor;
                    while (*cursor >= '0' && *cursor <= '9') {
                        unsigned int digit = (unsigned int)(*cursor++ - '0');
                        if (number > (UINT64_MAX - digit) / 10U) { numbers = 0; break; }
                        number = number * 10U + digit;
                    }
                    if (!numbers || cursor == start ||
                        (field == 0U && *cursor != '-') ||
                        (field == 1U && *cursor != '/') ||
                        (field == 2U && *cursor != '\0')) { numbers = 0; break; }
                    if (field < 2U) ++cursor;
                }
                if (numbers && sscanf(value, "bytes %llu-%llu/%llu%c", &first, &last, &length, &extra) == 3 &&
                    first == offset && last >= first && last < length && length <= limit &&
                    received <= last - first + 1U) {
                    *total = (uint64_t)length;
                    valid = 1;
                }
            }
        } else if (strncasecmp(line, "Content-Encoding:", 17U) == 0) {
            char *value = line + 17U;
            while (*value == ' ' || *value == '\t') ++value;
            if (strcasecmp(value, "identity") != 0) encoded = 1;
        }
    }
    free(data);
    return valid && ranges == 1U && !encoded;
}

/* Four attempts per endpoint. Metadata gets 45 seconds of network time per
 * attempt; packages get 600 seconds (including the server's 120-second queue).
 * Cooldown is separate, so a valid Retry-After is never cut short by a network
 * timeout. Resume is confined to this call's unpublished temporary file.
 * -2: local I/O, cancellation, size violation; -3: server cooldown/retry limit.
 * Neither may be hidden by switching to the same server's fixed IP alias. */
static int fetch_endpoint(const char *url, const char *output, uint64_t limit,
                          uint64_t metadata, char *error, size_t error_size)
{
    const char *curl = c1pkg_helper("/usr/bin/curl", "curl");
    char headers[C1PKG_PATH_MAX];
    char timeout[32] = "600", range[64];
    char *arguments[] = {(char *)curl, "--disable", "--fail", "--location", "--proto", "=http,https",
                         "--proto-redir", strncmp(url, "http://", 7U) == 0 ? "=http" : "=http,https",
                         "--max-redirs", "3", "--connect-timeout", "15", "--max-time", timeout,
                         "--speed-limit", "1024", "--speed-time", "180",
                         "--dump-header", headers, "--silent", "--show-error",
                         "--header", "Accept-Encoding: identity", "--output", "-",
                         NULL, NULL, NULL, NULL};
    const size_t tail = sizeof(arguments) / sizeof(arguments[0]) - 4U;
    struct stat information;
    uint64_t offset = 0U;
    unsigned int attempt;
    int result = -1, descriptor = -1, header_fd = -1, headers_created = 0, saved_errno = 0;
    int length = snprintf(headers, sizeof(headers), "%s.headers.XXXXXX", output);
    if (length < 0 || (size_t)length >= sizeof(headers)) {
        c1pkg_set_error(error, error_size, "下载路径过长");
        errno = ENAMETOOLONG;
        return -2;
    }
    if (cooldown_open(url, error, error_size) != 0) return errno == ECANCELED ? -2 : -3;
    /* The caller supplies an installation temporary path, never a live package. */
    descriptor = open(output, O_WRONLY | O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
    if (descriptor < 0) goto io_failed;
    if (fstat(descriptor, &information) != 0) goto io_failed;
    if (!S_ISREG(information.st_mode) || information.st_nlink != 1) { errno = EINVAL; goto io_failed; }
    if (ftruncate(descriptor, 0) != 0) goto io_failed;
    header_fd = mkstemp(headers);
    if (header_fd < 0) goto io_failed;
    headers_created = 1;
    (void)close(header_fd);
    for (attempt = 0U; attempt < 4U; ++attempt) {
        uint64_t seconds, received = 0U, total = 0U;
        char stage[192];
        const char *action = metadata != 0U ? "正在检查软件仓库" :
                             offset != 0U ? "正在继续下载软件包" :
                             "正在下载软件包";
        int transfer, exit_code, status, transfer_errno, has_retry_after;
        if (wait_for_cooldown(error, error_size) != 0) {
            saved_errno = errno; result = saved_errno == EAGAIN ? -3 : -2; break;
        }
        (void)snprintf(timeout, sizeof(timeout), "%u", metadata != 0U ? 45U : 600U);
        (void)snprintf(stage, sizeof(stage), "第 %u/4 次尝试：%s", attempt + 1U, action);
        if (c1pkg_progress(stage) != 0) {
            c1pkg_set_error(error, error_size, "操作已取消");
            saved_errno = ECANCELED; result = -2; break;
        }
        if (offset != 0U) {
            (void)snprintf(range, sizeof(range), "%llu-", (unsigned long long)offset);
            arguments[tail] = "--range"; arguments[tail + 1U] = range;
            arguments[tail + 2U] = (char *)url; arguments[tail + 3U] = NULL;
        } else {
            arguments[tail] = (char *)url; arguments[tail + 1U] = NULL;
        }
        /* Clear old headers even when exec fails before curl opens the file. */
        header_fd = open(headers, O_WRONLY | O_TRUNC | O_CLOEXEC | O_NOFOLLOW);
        if (header_fd < 0) goto io_failed;
        (void)close(header_fd);
        /* Preserve the known deadline and mark the request before sending.
         * If interrupted, the next guard owner consumes the marker once into
         * a bounded recovery wait, never shortening a saved server deadline. */
        if (cooldown_begin_request() != 0) {
            saved_errno = EAGAIN; result = -3;
            c1pkg_set_error(error, error_size, "无法持久保存仓库请求状态；已停止联网");
            break;
        }
        transfer = c1pkg_run_bounded(arguments, descriptor, limit - offset, &received,
                                     &exit_code, error, error_size);
        transfer_errno = errno;
        status = response_status(headers, &seconds, &has_retry_after);
        /* Every response can constrain the next request, including a successful
         * index immediately followed by its signature. Headerless transient
         * failures back off 5/10/20/30 seconds; other responses leave a modest
         * one-second client pacing interval, not an assumed server quota. */
        if (!has_retry_after) {
            seconds = transient_status(status) || (transfer != 0 && transient_exit(exit_code)) ?
                      (attempt < 3U ? 5U << attempt : 30U) : 1U;
        }
        if (seconds == 0U) seconds = 1U; /* Avoid tight loops for Retry-After: 0. */
        if (remember_cooldown(seconds) != 0) {
            saved_errno = EAGAIN; result = -3;
            c1pkg_set_error(error, error_size, "无法保存服务器冷却时间；已停止后续请求");
            break;
        }
        if (transfer == -2 && transfer_errno != EFBIG) {
            saved_errno = transfer_errno; result = -2; break;
        }
        if (offset != 0U && (status == 200 || status == 416)) {
            /* Ignored Range and stale offsets are restart signals, never append
             * success. Discard every provisional byte before the fresh request. */
            offset = 0U;
            if (ftruncate(descriptor, 0) != 0 || lseek(descriptor, 0, SEEK_SET) < 0) goto io_failed;
            c1pkg_set_error(error, error_size, "服务器不支持断点续传，已达到自动重试上限");
            continue;
        }
        if (transfer == -2) { saved_errno = transfer_errno; result = -2; break; }
        if (transient_status(status) || (status == 0 && transfer != 0 && transient_exit(exit_code))) {
            if (ftruncate(descriptor, (off_t)offset) != 0 || lseek(descriptor, (off_t)offset, SEEK_SET) < 0) goto io_failed;
            if (attempt == 3U) {
                c1pkg_set_error(error, error_size, "服务器或网络暂不可用，已达到自动重试上限，请稍后重试");
                if (status == 429 || status == 503 || has_retry_after) {
                    saved_errno = EAGAIN; result = -3;
                }
                break;
            }
            continue;
        }
        if ((offset == 0U && status != 200) ||
            (offset != 0U && (status != 206 || !response_range(headers, offset, received, limit, &total)))) {
            c1pkg_set_error(error, error_size, "仓库 HTTP 响应或下载范围无效：%d", status);
            break;
        }
        offset += received;
        if (transfer == 0 && (status == 200 || offset == total)) {
            if (fsync(descriptor) != 0) goto io_failed;
            if (error != NULL && error_size != 0U) error[0] = '\0';
            result = 0; break;
        }
        /* Only transient transport errors are retried. Metadata always starts
         * afresh; packages retain only a validated prefix from this invocation. */
        if ((transfer != 0 && !transient_exit(exit_code)) || offset >= limit) break;
        c1pkg_set_error(error, error_size, "下载中断，已达到自动重试上限，请稍后重试");
        if (metadata != 0U) {
            offset = 0U;
            if (ftruncate(descriptor, 0) != 0 || lseek(descriptor, 0, SEEK_SET) < 0) goto io_failed;
        }
    }
    goto done;
io_failed:
    saved_errno = errno;
    result = -2;
    c1pkg_set_error(error, error_size, "下载文件读写失败：%s", strerror(saved_errno));
done:
    if (descriptor >= 0 && close(descriptor) != 0 && result == 0) {
        saved_errno = errno; result = -2;
        c1pkg_set_error(error, error_size, "关闭下载文件失败：%s", strerror(saved_errno));
    }
    if (headers_created) (void)unlink(headers);
    if (result != 0 && descriptor >= 0) (void)unlink(output);
    cooldown_close();
    errno = saved_errno;
    return result;
}

int c1pkg_fetch(const char *url, const char *output, uint64_t limit,
                char *error, size_t error_size)
{
    char fallback[1400];
    int result;
    if (!valid_url_base(url)) {
        c1pkg_set_error(error, error_size, "仓库下载地址无效");
        return -1;
    }
    result = fetch_endpoint(url, output, limit, 0U, error, error_size);
    if (result == 0) return 0;
    if (result == -2 || result == -3) return -1;
    if (c1pkg_progress(NULL) != 0) {
        c1pkg_set_error(error, error_size, "操作已取消");
        errno = ECANCELED;
        return -1;
    }
    if (!c1_repository_fallback_url(url, fallback, sizeof(fallback))) return -1;
    result = fetch_endpoint(fallback, output, limit, 0U, error, error_size);
    return result == 0 ? 0 : -1;
}

/* Read one opened regular inode with a hard ceiling, including an EOF probe.
 * The source can be read-only and untrusted; verify and parse these same bytes,
 * never reopen an index after signature verification. */
static int read_local_file(int fd, unsigned char **data, size_t *size, size_t limit,
                           char *error, size_t error_size)
{
    struct stat info;
    unsigned char *buffer = NULL, extra;
    size_t used = 0U, length;
    ssize_t amount;
    if (fstat(fd, &info) != 0) goto failed;
    if (!S_ISREG(info.st_mode) || info.st_nlink != 1 || info.st_size < 0 ||
        (uint64_t)info.st_size > limit) {
        errno = EINVAL;
        c1pkg_set_error(error, error_size, "local file is linked, special, or exceeds size limit");
        return -1;
    }
    length = (size_t)info.st_size;
    buffer = malloc(length + 1U);
    if (buffer == NULL) goto failed;
    while (used < length) {
        size_t chunk = length - used > 16384U ? 16384U : length - used;
        if (c1pkg_progress(NULL) != 0) { errno = ECANCELED; goto failed; }
        amount = read(fd, buffer + used, chunk);
        if (amount < 0 && errno == EINTR) continue;
        if (amount <= 0) { if (amount == 0) errno = EIO; goto failed; }
        used += (size_t)amount;
    }
    do { amount = read(fd, &extra, 1U); } while (amount < 0 && errno == EINTR);
    if (amount != 0) { if (amount > 0) errno = EFBIG; goto failed; }
    buffer[used] = 0U;
    *data = buffer;
    *size = used;
    return 0;
failed:
    free(buffer);
    c1pkg_set_error(error, error_size, "read local file: %s", strerror(errno));
    return -1;
}

static int verify_key(const char *key, unsigned char public_key[32],
                      char *error, size_t error_size)
{
    struct stat information;
    unsigned char *data = NULL;
    size_t size = 0U;
    int fd = -1, result = -1;

    /* Inspect and read the same inode, including all parent components. */
    if (key == NULL || key[0] != '/' ||
        (fd = c1pkg_local_openat(AT_FDCWD, key, 0, error, error_size)) < 0 ||
        fstat(fd, &information) != 0 || !S_ISREG(information.st_mode) ||
        information.st_nlink != 1 || information.st_size != 32 ||
        (information.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
        c1pkg_set_error(error, error_size,
                        "trusted Ed25519 key missing, unsafe, or not 32 bytes: %s",
                        key != NULL ? key : "(null)");
        goto done;
    }
    if (read_local_file(fd, &data, &size, 32U, error, error_size) != 0 || size != 32U) goto done;
    memcpy(public_key, data, 32U);
    result = 0;
done:
    free(data);
    if (fd >= 0) (void)close(fd);
    return result;
}

static int verify_signature(const char *key, const char *signature, const char *index_path,
                            char *error, size_t error_size)
{
    unsigned char public_key[32];
    unsigned char *signature_data = NULL;
    unsigned char *index_data = NULL;
    size_t signature_size = 0U;
    size_t index_size = 0U;
    int result = -1;

    if (verify_key(key, public_key, error, error_size) != 0 ||
        c1pkg_read_file(signature, &signature_data, &signature_size, 64U,
                        error, error_size) != 0 || signature_size != 64U ||
        c1pkg_read_file(index_path, &index_data, &index_size, C1PKG_INDEX_MAX,
                        error, error_size) != 0) {
        if (signature_data != NULL && signature_size != 64U) {
            c1pkg_set_error(error, error_size, "repository signature must be 64 bytes");
        }
        goto done;
    }
    if (ed25519_verify(signature_data, index_data, index_size, public_key) != 1) {
        c1pkg_set_error(error, error_size, "repository Ed25519 signature rejected");
        goto done;
    }
    result = 0;
done:
    free(signature_data);
    free(index_data);
    return result;
}

static int parse_u64(const char *text, uint64_t *value)
{
    uint64_t result = 0U;
    size_t i;

    if (text == NULL || text[0] == '\0' || (text[0] == '0' && text[1] != '\0')) {
        return -1;
    }
    for (i = 0U; text[i] != '\0'; ++i) {
        unsigned int digit;
        if (text[i] < '0' || text[i] > '9') {
            return -1;
        }
        digit = (unsigned int)(text[i] - '0');
        if (result > (UINT64_MAX - digit) / 10U) {
            return -1;
        }
        result = result * 10U + digit;
    }
    *value = result;
    return 0;
}

static int valid_name(const char *name)
{
    return c1pkg_valid_label(name);
}

static int valid_sha256(const char *digest)
{
    size_t i;

    if (strlen(digest) != 64U) {
        return 0;
    }
    for (i = 0U; i < 64U; ++i) {
        if (!((digest[i] >= '0' && digest[i] <= '9') ||
              (digest[i] >= 'a' && digest[i] <= 'f'))) {
            return 0;
        }
    }
    return 1;
}

static int parse_package_line(char *line, struct c1pkg_package *package, int format,
                              char *error, size_t error_size)
{
    char *fields[9];
    size_t field = 0U;
    char *cursor;

    fields[field++] = line;
    for (cursor = line; *cursor != '\0'; ++cursor) {
        if (*cursor == '\t') {
            if (field >= sizeof(fields) / sizeof(fields[0])) {
                c1pkg_set_error(error, error_size, "too many package fields");
                return -1;
            }
            *cursor = '\0';
            fields[field++] = cursor + 1;
        }
    }
    if (field != (format == 2 ? 9U : 8U) ||
        (format == 2 && !valid_name(fields[8])) ||
        strcmp(fields[0], "P") != 0 || !c1pkg_safe_id(fields[1]) ||
        !c1pkg_safe_version(fields[2]) || !valid_name(fields[3]) ||
        !c1pkg_safe_relpath(fields[4]) || !valid_sha256(fields[5]) ||
        parse_u64(fields[6], &package->size) != 0 || package->size == 0U ||
        package->size > C1PKG_PACKAGE_MAX || !c1pkg_safe_relpath(fields[7])) {
        c1pkg_set_error(error, error_size, "invalid package record");
        return -1;
    }
    (void)strcpy(package->id, fields[1]);
    (void)strcpy(package->version, fields[2]);
    (void)strcpy(package->name, fields[3]);
    (void)strcpy(package->author, format == 2 ? fields[8] : "Unknown");
    (void)strcpy(package->archive, fields[4]);
    (void)strcpy(package->sha256, fields[5]);
    (void)strcpy(package->entry, fields[7]);
    return 0;
}

static int parse_index_data(const unsigned char *input, size_t size, struct c1pkg_index *index,
                            char *error, size_t error_size)
{
    unsigned char *data = NULL;
    size_t offset = 0U;
    size_t line_number = 0U;
    int format = 1;

    index->sequence = 0U;
    index->count = 0U;
    if (input == NULL || size > C1PKG_INDEX_MAX || (data = malloc(size + 1U)) == NULL) {
        c1pkg_set_error(error, error_size, "invalid index size or out of memory");
        return -1;
    }
    memcpy(data, input, size);
    data[size] = 0U;
    if (size == 0U || data[size - 1U] != (unsigned char)'\n') {
        c1pkg_set_error(error, error_size, "index must end with LF");
        free(data);
        return -1;
    }
    while (offset < size) {
        size_t end = offset;
        char *line;
        while (end < size && data[end] != (unsigned char)'\n') {
            if (data[end] == 0U || data[end] == (unsigned char)'\r') {
                c1pkg_set_error(error, error_size, "index contains forbidden bytes");
                free(data);
                return -1;
            }
            ++end;
        }
        if (end - offset > C1PKG_LINE_MAX) {
            c1pkg_set_error(error, error_size, "index line is too long");
            free(data);
            return -1;
        }
        data[end] = 0U;
        line = (char *)(data + offset);
        ++line_number;
        if (line_number == 1U) {
            if (strcmp(line, "C1PKG-INDEX 2") == 0) {
                format = 2;
            } else if (strcmp(line, C1PKG_INDEX_HEADER) != 0) {
                c1pkg_set_error(error, error_size, "unsupported index header");
                free(data);
                return -1;
            }
        } else if (line_number == 2U) {
            if (strncmp(line, "S\t", 2U) != 0 ||
                parse_u64(line + 2U, &index->sequence) != 0 || index->sequence == 0U) {
                c1pkg_set_error(error, error_size, "invalid repository sequence");
                free(data);
                return -1;
            }
        } else {
            struct c1pkg_package *package;
            if (index->count >= C1PKG_MAX_PACKAGES || line[0] == '\0') {
                c1pkg_set_error(error, error_size, "empty or excessive package record");
                free(data);
                return -1;
            }
            package = &index->packages[index->count];
            if (parse_package_line(line, package, format, error, error_size) != 0 ||
                (index->count > 0U && strcmp(index->packages[index->count - 1U].id,
                                             package->id) >= 0)) {
                if (error != NULL && error[0] == '\0') {
                    c1pkg_set_error(error, error_size, "package IDs must be unique and sorted");
                }
                free(data);
                return -1;
            }
            ++index->count;
        }
        offset = end + 1U;
    }
    free(data);
    if (line_number < 2U || index->sequence == 0U) {
        c1pkg_set_error(error, error_size, "missing repository sequence");
        return -1;
    }
    return 0;
}

int c1pkg_repo_parse(const unsigned char *data, size_t size, struct c1pkg_index *index,
                     char *error, size_t error_size)
{
    struct c1pkg_index *candidate = calloc(1U, sizeof(*candidate));
    int result;
    if (index == NULL || candidate == NULL) {
        free(candidate);
        c1pkg_set_error(error, error_size, "missing index or out of memory");
        return -1;
    }
    result = parse_index_data(data, size, candidate, error, error_size);
    if (result == 0) *index = *candidate;
    free(candidate);
    return result;
}

int c1pkg_repo_open_local(const struct c1pkg_config *config, const char *directory,
                          struct c1pkg_index *index, char *error, size_t error_size)
{
    unsigned char key[32], *signature = NULL, *data = NULL;
    size_t signature_size = 0U, size = 0U;
    int root = -1, signature_fd = -1, index_fd = -1, result = -1;
    if (config == NULL || index == NULL) {
        c1pkg_set_error(error, error_size, "missing local repository configuration or index");
        return -1;
    }
    if (verify_key(config->public_key, key, error, error_size) != 0 ||
        (root = c1pkg_local_openat(AT_FDCWD, directory, 1, error, error_size)) < 0 ||
        (signature_fd = c1pkg_local_openat(root, "index.v1.sig", 0, error, error_size)) < 0 ||
        read_local_file(signature_fd, &signature, &signature_size, 64U, error, error_size) != 0)
        goto done;
    if (signature_size != 64U) {
        c1pkg_set_error(error, error_size, "repository signature must be 64 bytes");
        goto done;
    }
    if ((index_fd = c1pkg_local_openat(root, "index.v1", 0, error, error_size)) < 0 ||
        read_local_file(index_fd, &data, &size, C1PKG_INDEX_MAX, error, error_size) != 0) goto done;
    if (ed25519_verify(signature, data, size, key) != 1) {
        c1pkg_set_error(error, error_size, "repository Ed25519 signature rejected");
        goto done;
    }
    if (c1pkg_repo_parse(data, size, index, error, error_size) != 0) goto done;
    /* Offline bundles have independent sequences. Do not touch the online
     * cache or rollback floor; the store still enforces per-package versions. */
    result = root;
    root = -1;
done:
    if (signature_fd >= 0) (void)close(signature_fd);
    if (index_fd >= 0) (void)close(index_fd);
    if (root >= 0) (void)close(root);
    free(signature);
    free(data);
    return result;
}

static int parse_index_file(const char *path, struct c1pkg_index *index,
                            char *error, size_t error_size)
{
    unsigned char *data = NULL;
    size_t size = 0U;
    int result;
    if (c1pkg_read_file(path, &data, &size, C1PKG_INDEX_MAX, error, error_size) != 0) return -1;
    result = c1pkg_repo_parse(data, size, index, error, error_size);
    free(data);
    return result;
}

const struct c1pkg_package *c1pkg_repo_find(const struct c1pkg_index *index,
                                            const char *id)
{
    size_t i;
    for (i = 0U; i < index->count; ++i) {
        if (strcmp(index->packages[i].id, id) == 0) {
            return &index->packages[i];
        }
    }
    return NULL;
}

static int load_highest_sequence(uint64_t *sequence, char *error, size_t error_size)
{
    char path[C1PKG_PATH_MAX];
    unsigned char *data = NULL;
    size_t size = 0U;

    *sequence = 0U;
    if (c1pkg_join(path, sizeof(path), C1PKG_STATE_ROOT, "highest-sequence") != 0) {
        c1pkg_set_error(error, error_size, "sequence path is too long");
        return -1;
    }
    if (access(path, F_OK) != 0 && errno == ENOENT) {
        return 0;
    }
    if (c1pkg_read_file(path, &data, &size, 32U, error, error_size) != 0 ||
        size < 2U || memchr(data, '\0', size) != NULL || data[size - 1U] != (unsigned char)'\n') {
        free(data);
        c1pkg_set_error(error, error_size, "stored repository sequence is invalid");
        return -1;
    }
    data[size - 1U] = 0U;
    if (parse_u64((char *)data, sequence) != 0 || *sequence == 0U) {
        free(data);
        c1pkg_set_error(error, error_size, "stored repository sequence is invalid");
        return -1;
    }
    free(data);
    return 0;
}

static int store_highest_sequence(uint64_t sequence, char *error, size_t error_size)
{
    char path[C1PKG_PATH_MAX];
    char temporary[C1PKG_PATH_MAX];
    char text[32];
    int length;

    if (c1pkg_join(path, sizeof(path), C1PKG_STATE_ROOT, "highest-sequence") != 0 ||
        snprintf(temporary, sizeof(temporary), "%s.new.%ld", path, (long)getpid()) < 0) {
        c1pkg_set_error(error, error_size, "sequence path is too long");
        return -1;
    }
    length = snprintf(text, sizeof(text), "%llu\n", (unsigned long long)sequence);
    if (length <= 0 || (size_t)length >= sizeof(text) ||
        c1pkg_write_file(temporary, text, (size_t)length, 0600, error, error_size) != 0) {
        (void)unlink(temporary);
        return -1;
    }
    if (rename(temporary, path) != 0) {
        c1pkg_set_error(error, error_size, "commit repository sequence: %s", strerror(errno));
        (void)unlink(temporary);
        return -1;
    }
    return c1pkg_sync_directory(C1PKG_STATE_ROOT, error, error_size);
}

int c1pkg_repo_load_cached(const struct c1pkg_config *config, struct c1pkg_index *index,
                           char *error, size_t error_size)
{
    unsigned char *data = NULL;
    unsigned char key[32];
    size_t size = 0U;
    uint64_t highest = 0U;
    struct c1pkg_index *candidate = calloc(1U, sizeof(*candidate));
    int result = -1;
    if (candidate == NULL || config == NULL || index == NULL) goto done;
    /* This in-memory hint only identifies package URLs. The deadline itself is
     * always loaded under the persistent guard; a refresh child's hint need not
     * propagate to its parent, since the parent also matches registered scopes. */
    (void)c1pkg_repo_bind_transport(config);
    if (access(C1PKG_STATE_ROOT "/cache/verified.v1", F_OK) != 0 && errno == ENOENT) {
        /* Migrate legacy caches only after re-verification and rollback checking. */
        if (verify_signature(config->public_key, C1PKG_STATE_ROOT "/cache/index.v1.sig",
                              C1PKG_STATE_ROOT "/cache/index.v1", error, error_size) != 0 ||
            parse_index_file(C1PKG_STATE_ROOT "/cache/index.v1", candidate,
                              error, error_size) != 0) goto done;
    } else {
        if (verify_key(config->public_key, key, error, error_size) != 0 ||
            c1pkg_read_file(C1PKG_STATE_ROOT "/cache/verified.v1", &data, &size,
                            C1PKG_INDEX_MAX + 64U, error, error_size) != 0) goto done;
        if (size <= 64U || ed25519_verify(data, data + 64U, size - 64U, key) != 1) {
            c1pkg_set_error(error, error_size, "cached repository signature rejected");
            goto done;
        }
        if (c1pkg_repo_parse(data + 64U, size - 64U, candidate, error, error_size) != 0) goto done;
    }
    if (load_highest_sequence(&highest, error, error_size) != 0) goto done;
    if (candidate->sequence < highest) {
        c1pkg_set_error(error, error_size, "cached repository rollback rejected");
        goto done;
    }
    *index = *candidate;
    result = 0;
done:
    free(data);
    free(candidate);
    return result;
}

int c1pkg_repo_refresh(const struct c1pkg_config *config, struct c1pkg_index *index,
                       char *error, size_t error_size)
{
    const char *cache = C1PKG_STATE_ROOT "/cache";
    const char *index_tmp = C1PKG_STATE_ROOT "/cache/index.tmp";
    const char *signature_tmp = C1PKG_STATE_ROOT "/cache/signature.tmp";
    const char *bundle_tmp = C1PKG_STATE_ROOT "/cache/verified.tmp";
    unsigned char *data = NULL;
    unsigned char *signature = NULL;
    unsigned char *bundle = NULL;
    size_t size = 0U, signature_size = 0U;
    char url[1400], normalized[1400], fallback[1400];
    const char *bases[2];
    unsigned char trusted_key[32];
    unsigned int endpoint, endpoint_count = 1U;
    int verified = 0;
    uint64_t highest_sequence = 0U;
    struct c1pkg_index *candidate = NULL;
    struct flock operation;
    int lock = -1;
    unsigned int attempt;
    int result = -1;

    if (config == NULL || !valid_url_base(config->repo_base)) {
        c1pkg_set_error(error, error_size, "Configure repository.url or --repo URL");
        return -1;
    }
    if (c1pkg_repo_bind_transport(config) != 0) {
        transport_repo_scope[0] = '\0';
        c1pkg_set_error(error, error_size, "仓库地址不能安全确定冷却作用域");
        return -1;
    }
    if (index == NULL || c1pkg_storage_state_init(error, error_size) != 0 ||
        c1pkg_mkdir_p(cache, 0700, error, error_size) != 0) return -1;
    lock = open(C1PKG_STATE_ROOT "/repo.lock", O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    memset(&operation, 0, sizeof(operation));
    operation.l_type = F_WRLCK;
    operation.l_whence = SEEK_SET;
    if (lock < 0 || fcntl(lock, F_SETLK, &operation) != 0) {
        if (lock >= 0) (void)close(lock);
        c1pkg_set_error(error, error_size, "another repository refresh is in progress");
        return -1;
    }
    candidate = calloc(1U, sizeof(*candidate));
    if (candidate == NULL) {
        c1pkg_set_error(error, error_size, "out of memory");
        goto done;
    }
    if (verify_key(config->public_key, trusted_key, error, error_size) != 0 ||
        load_highest_sequence(&highest_sequence, error, error_size) != 0) goto done;
    bases[0] = config->repo_base;
    if (make_url(normalized, sizeof(normalized), config->repo_base, "") == 0 &&
        c1_repository_fallback_url(normalized, fallback, sizeof(fallback))) {
        bases[endpoint_count++] = fallback;
    }
    for (endpoint = 0U; endpoint < endpoint_count && !verified; ++endpoint) {
        /* Never combine an index from one origin with another origin's signature.
         * Retry publication races, then try the fixed alternate with a fresh pair.
         * Network timeouts apply per transfer, never to server cooldown waits. */
        for (attempt = 0U; attempt < 3U; ++attempt) {
            (void)unlink(index_tmp);
            (void)unlink(signature_tmp);
            if (c1pkg_progress(NULL) != 0) {
                c1pkg_set_error(error, error_size, "操作已取消");
                errno = ECANCELED;
                goto done;
            }
            if (error != NULL && error_size != 0U) error[0] = '\0';
            {
                int fetched;
                if (make_url(url, sizeof(url), bases[endpoint], "index.v1") != 0) break;
                fetched = fetch_endpoint(url, index_tmp, C1PKG_INDEX_MAX, 1U, error, error_size);
                if (fetched == -2 || fetched == -3) goto done;
                if (fetched != 0 || make_url(url, sizeof(url), bases[endpoint], "index.v1.sig") != 0) break;
                fetched = fetch_endpoint(url, signature_tmp, 64U, 1U, error, error_size);
                if (fetched == -2 || fetched == -3) goto done;
                if (fetched != 0) break;
            }
            if (verify_signature(config->public_key, signature_tmp, index_tmp, error, error_size) == 0 &&
                parse_index_file(index_tmp, candidate, error, error_size) == 0) {
                if (candidate->sequence >= highest_sequence) {
                    verified = 1;
                    break;
                }
                c1pkg_set_error(error, error_size, "repository rollback rejected: sequence %llu is below %llu",
                                (unsigned long long)candidate->sequence,
                                (unsigned long long)highest_sequence);
                break;
            }
            if (attempt == 2U) break;
            if (retry_wait(1U, error, error_size) != 0) goto done;
        }
        if (c1pkg_progress(NULL) != 0) {
            c1pkg_set_error(error, error_size, "操作已取消");
            errno = ECANCELED;
            goto done;
        }
    }
    if (!verified) goto done;
    if (c1pkg_read_file(index_tmp, &data, &size, C1PKG_INDEX_MAX, error, error_size) != 0 ||
        c1pkg_read_file(signature_tmp, &signature, &signature_size, 64U, error, error_size) != 0 ||
        signature_size != 64U || (bundle = malloc(size + 64U)) == NULL) goto done;
    memcpy(bundle, signature, 64U);
    memcpy(bundle + 64U, data, size);
    (void)unlink(bundle_tmp);
    if (c1pkg_write_file(bundle_tmp, bundle, size + 64U, 0600, error, error_size) != 0 ||
        (candidate->sequence > highest_sequence &&
         store_highest_sequence(candidate->sequence, error, error_size) != 0)) goto done;
    /* A single atomic file contains signature + signed bytes. Persist the rollback
     * floor first: after interrupted commits an older cache fails closed. */
    if (rename(bundle_tmp, C1PKG_STATE_ROOT "/cache/verified.v1") != 0) {
        c1pkg_set_error(error, error_size, "commit verified index: %s", strerror(errno));
        goto done;
    }
    if (c1pkg_sync_directory(cache, error, error_size) != 0) goto done;
    *index = *candidate;
    if (error != NULL && error_size != 0U) error[0] = '\0';
    result = 0;
done:
    {
        int saved_errno = errno;
        (void)unlink(index_tmp);
        (void)unlink(signature_tmp);
        (void)unlink(bundle_tmp);
        free(candidate);
        free(data);
        free(signature);
        free(bundle);
        (void)close(lock);
        errno = saved_errno;
    }
    return result;
}

int c1pkg_verify_sha256(const char *path, const char *expected,
                        char *error, size_t error_size)
{
    const char *sha256sum = c1pkg_helper("/usr/bin/sha256sum", "sha256sum");
    char output[C1PKG_PATH_MAX];
    char *arguments[] = {(char *)sha256sum, (char *)path, NULL};
    unsigned char *data = NULL;
    size_t size = 0U;
    int result = -1;

    if (!valid_sha256(expected) ||
        snprintf(output, sizeof(output), "%s.digest.%ld", path,
                 (long)getpid()) < 0) {
        c1pkg_set_error(error, error_size, "invalid expected SHA-256");
        return -1;
    }
    (void)unlink(output);
    if (c1pkg_run(arguments, output, 4096U, error, error_size) != 0 ||
        c1pkg_read_file(output, &data, &size, 4096U, error, error_size) != 0) {
        if (c1pkg_progress(NULL) != 0) {
            c1pkg_set_error(error, error_size, "Cancelled");
        } else {
            c1pkg_set_error(error, error_size, "SHA-256 verification unavailable (sha256sum required)");
        }
        goto done;
    }
    if (size < 66U || memcmp(data, expected, 64U) != 0 || data[64] != (unsigned char)' ') {
        c1pkg_set_error(error, error_size, "package SHA-256 mismatch");
        goto done;
    }
    result = 0;
done:
    free(data);
    (void)unlink(output);
    return result;
}