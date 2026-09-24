#include "metrics.h"
#include "security/sha256.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define REPO_MAX 1024U
#define EVENT_MAX 1280U
#define WIRE_MAX (C1PKG_METRICS_BODY_MAX + 8192U)
#define CACHE_MAGIC "C1PKG-METRIC-1"
#define EVENT_HEADER "C1PKG-INSTALL 1\n"
#define COUNTS_HEADER "C1PKG-METRICS 1\n"
#define ACK_HEADER "C1PKG-INSTALL-ACK 1\nE\t"

struct metrics_cache {
    char magic[16];
    char repo[REPO_MAX + 1U];
    uint64_t retry_at;
    struct c1pkg_metrics_snapshot snapshot;
};
struct metrics_event {
    char token[33];
    char body[256];
};
struct c1pkg_metrics_job {
    pid_t pid;
    int fd;
    int cancelled;
    size_t used;
    struct c1pkg_metrics_snapshot result;
};
static volatile sig_atomic_t worker_cancelled;

static int decimal(const char *text, uint64_t *value)
{
    uint64_t n = 0U;
    if (*text == '\0' || (*text == '0' && text[1] != '\0')) return -1;
    for (; *text != '\0'; ++text) {
        unsigned int d = (unsigned int)(unsigned char)*text - (unsigned int)'0';
        if (d > 9U || n > (UINT64_MAX - d) / 10U) return -1;
        n = n * 10U + d;
    }
    *value = n;
    return 0;
}

static int parse_counts(const unsigned char *data, size_t size,
                         struct c1pkg_metrics_snapshot *snapshot, const char *header)
{
    struct c1pkg_metrics_snapshot candidate = {0};
    size_t offset = sizeof(COUNTS_HEADER) - 1U;
    if (snapshot == NULL) return -1;
    memset(snapshot, 0, sizeof(*snapshot));
    if (data == NULL || size < offset || size > C1PKG_METRICS_BODY_MAX ||
        memcmp(data, header, offset) != 0 || data[size - 1U] != '\n' ||
        memchr(data, 0, size) != NULL || memchr(data, '\r', size) != NULL) return -1;
    while (offset < size) {
        size_t end = offset;
        char line[64], *tab;
        struct c1pkg_metrics_item *item;
        while (end < size && data[end] != '\n') ++end;
        if (end == size || end - offset >= sizeof(line) || candidate.count >= C1PKG_MAX_PACKAGES)
            return -1;
        memcpy(line, data + offset, end - offset);
        line[end - offset] = '\0';
        if (strncmp(line, "I\t", 2U) != 0 || (tab = strchr(line + 2U, '\t')) == NULL) return -1;
        *tab++ = '\0';
        item = &candidate.items[candidate.count];
        if (!c1pkg_safe_id(line + 2U) || decimal(tab, &item->installations) != 0 ||
            (candidate.count != 0U && strcmp(candidate.items[candidate.count - 1U].id, line + 2U) >= 0))
            return -1;
        strcpy(item->id, line + 2U);
        ++candidate.count;
        offset = end + 1U;
    }
    candidate.available = 1;
    *snapshot = candidate;
    return 0;
}

int c1pkg_metrics_parse(const unsigned char *data, size_t size,
                        struct c1pkg_metrics_snapshot *snapshot)
{
    return parse_counts(data, size, snapshot, COUNTS_HEADER);
}

int c1pkg_device_metrics_parse(const unsigned char *data, size_t size,
                               struct c1pkg_metrics_snapshot *snapshot)
{
    return parse_counts(data, size, snapshot, "C1PKG-METRICS 2\n");
}

int c1pkg_metrics_lookup(const struct c1pkg_metrics_snapshot *snapshot,
                         const char *id, uint64_t *value)
{
    size_t i;
    if (snapshot == NULL || id == NULL || value == NULL || !snapshot->available ||
        snapshot->count > C1PKG_MAX_PACKAGES) return 0;
    for (i = 0U; i < snapshot->count; ++i) {
        if (strcmp(snapshot->items[i].id, id) == 0) {
            *value = snapshot->items[i].installations;
            return 1;
        }
    }
    return 0;
}

/* Conservative endpoint grammar: credentials, queries, fragments, URL globbing,
 * percent escapes and ambiguous dot segments are not accepted for telemetry.
 * Rejecting a statistics URL never rejects its signed repository. */
static int normalize_repo(const char *input, char output[REPO_MAX + 1U])
{
    size_t i, n, prefix;
    const char *path;
    if (input == NULL) return -1;
    n = strnlen(input, REPO_MAX + 1U);
    prefix = strncmp(input, "https://", 8U) == 0 ? 8U :
             strncmp(input, "http://", 7U) == 0 ? 7U : 0U;
    if (prefix == 0U || n <= prefix || n > REPO_MAX) return -1;
    for (i = prefix; i < n; ++i) {
        unsigned char c = (unsigned char)input[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '.' || c == '-' || c == '_' || c == ':' || c == '/')) return -1;
    }
    path = strchr(input + prefix, '/');
    if (input[prefix] == '/' || input[prefix] == ':' ||
        (path != NULL && (strstr(path, "//") != NULL || strstr(path, "/../") != NULL ||
                         strstr(path, "/./") != NULL))) return -1;
    while (n > prefix && input[n - 1U] == '/') --n;
    if ((n >= 2U && memcmp(input + n - 2U, "/.", 2U) == 0) ||
        (n >= 3U && memcmp(input + n - 3U, "/..", 3U) == 0)) return -1;
    memcpy(output, input, n);
    output[n] = '\0';
    return 0;
}

static int private_inode(int fd, int directory)
{
    struct stat st;
    return fstat(fd, &st) == 0 && st.st_uid == geteuid() &&
           (st.st_mode & 0777) == (directory ? 0700 : 0600) &&
           (directory ? S_ISDIR(st.st_mode) : S_ISREG(st.st_mode) && st.st_nlink == 1);
}

static int open_root(void)
{
    int parent, root;
    /* The store owns creation and validation of its state root. Never create or
     * chmod package state from optional telemetry, and never follow a symlink. */
    parent = open(C1PKG_STATE_ROOT, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (parent < 0) return -1;
    if (!private_inode(parent, 1) || (mkdirat(parent, "metrics", 0700) != 0 && errno != EEXIST)) {
        (void)close(parent);
        return -1;
    }
    root = openat(parent, "metrics", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    (void)close(parent);
    if (root >= 0 && !private_inode(root, 1)) { (void)close(root); return -1; }
    return root;
}

static int lock_file(int root, const char *name)
{
    int fd = openat(root, name, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
    if (fd >= 0 && (!private_inode(fd, 0) || flock(fd, LOCK_EX | LOCK_NB) != 0)) {
        (void)close(fd); fd = -1;
    }
    return fd;
}

static DIR *open_events(int root)
{
    int fd = openat(root, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    DIR *directory;
    if (fd < 0) return NULL;
    directory = fdopendir(fd);
    if (directory == NULL) (void)close(fd);
    return directory;
}

static int read_private(int root, const char *name, void *buffer, size_t capacity, size_t *used)
{
    struct stat st;
    unsigned char extra;
    ssize_t amount;
    int fd = openat(root, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    *used = 0U;
    if (fd < 0) return -1;
    if (!private_inode(fd, 0) || fstat(fd, &st) != 0 || st.st_size < 0 ||
        (uint64_t)st.st_size > capacity) goto failed;
    while (*used < (size_t)st.st_size) {
        amount = read(fd, (unsigned char *)buffer + *used, (size_t)st.st_size - *used);
        if (amount < 0 && errno == EINTR) continue;
        if (amount <= 0) goto failed;
        *used += (size_t)amount;
    }
    do { amount = read(fd, &extra, 1U); } while (amount < 0 && errno == EINTR);
    (void)close(fd);
    return amount == 0 ? 0 : -1;
failed:
    (void)close(fd);
    return -1;
}

static int write_all(int fd, const void *data, size_t size)
{
    size_t used = 0U;
    while (used < size) {
        ssize_t n = write(fd, (const unsigned char *)data + used, size - used);
        if (n < 0 && errno == EINTR && !worker_cancelled) continue;
        if (n <= 0) return -1;
        used += (size_t)n;
    }
    return 0;
}

/* Caller owns the corresponding short queue lock or the worker-only lock. */
static int atomic_file(int root, const char *temporary, const char *name,
                        const void *data, size_t size)
{
    int fd, result = -1;
    (void)unlinkat(root, temporary, 0);
    fd = openat(root, temporary, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return -1;
    if (write_all(fd, data, size) == 0 && fsync(fd) == 0) result = 0;
    if (close(fd) != 0) result = -1;
    if (result == 0 && renameat(root, temporary, root, name) == 0 && fsync(root) == 0) return 0;
    (void)unlinkat(root, temporary, 0);
    return -1;
}

static int valid_token(const char *name)
{
    size_t i;
    if (strlen(name) != 32U) return 0;
    for (i = 0U; i < 32U; ++i)
        if (!((name[i] >= '0' && name[i] <= '9') || (name[i] >= 'a' && name[i] <= 'f'))) return 0;
    return 1;
}

static int random_token(char token[33])
{
    static const char hex[] = "0123456789abcdef";
    unsigned char bytes[16];
    size_t used = 0U, i;
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) return -1;
    while (used < sizeof(bytes)) {
        ssize_t n = read(fd, bytes + used, sizeof(bytes) - used);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { (void)close(fd); return -1; }
        used += (size_t)n;
    }
    (void)close(fd);
    for (i = 0U; i < sizeof(bytes); ++i) {
        token[i * 2U] = hex[bytes[i] >> 4U];
        token[i * 2U + 1U] = hex[bytes[i] & 15U];
    }
    token[32] = '\0';
    return 0;
}

int c1pkg_metrics_record_install(const struct c1pkg_config *config,
                                 const struct c1pkg_package *package)
{
    char repo[REPO_MAX + 1U], token[33], content[EVENT_MAX];
    struct stat st;
    struct dirent *entry;
    DIR *directory = NULL;
    size_t count = 0U, scanned = 0U;
    int root = -1, lock = -1, result = -1, length;
    if (config == NULL || package == NULL || normalize_repo(config->repo_base, repo) != 0 ||
        memchr(package->id, 0, sizeof(package->id)) == NULL || !c1pkg_safe_id(package->id) ||
        memchr(package->version, 0, sizeof(package->version)) == NULL || !c1pkg_safe_version(package->version) ||
        (root = open_root()) < 0 || (lock = lock_file(root, "queue.lock")) < 0) goto done;
    directory = open_events(root);
    if (directory == NULL) goto done;
    while ((entry = readdir(directory)) != NULL) {
        if (++scanned > 128U) goto done;
        if (valid_token(entry->d_name) && ++count >= C1PKG_METRICS_QUEUE_MAX) goto done;
    }
    if (random_token(token) != 0 || fstatat(root, token, &st, AT_SYMLINK_NOFOLLOW) == 0 || errno != ENOENT)
        goto done;
    length = snprintf(content, sizeof(content), "%s\n" EVENT_HEADER "E\t%s\t%s\t%s\n",
                      repo, token, package->id, package->version);
    if (length > 0 && (size_t)length < sizeof(content))
        result = atomic_file(root, "queue.tmp", token, content, (size_t)length);
done:
    if (directory != NULL) (void)closedir(directory);
    if (lock >= 0) (void)close(lock);
    if (root >= 0) (void)close(root);
    return result;
}

static int next_event(int root, const char *repo, struct metrics_event *event)
{
    int lock = lock_file(root, "queue.lock"), result = -1;
    DIR *directory;
    struct dirent *entry;
    size_t scanned = 0U;
    if (lock < 0) return -1;
    directory = open_events(root);
    if (directory == NULL) { (void)close(lock); return -1; }
    while ((entry = readdir(directory)) != NULL && ++scanned <= 128U) {
        char content[EVENT_MAX + 1U], expected[256], token[33], id[C1PKG_ID_MAX + 1U];
        char version[C1PKG_VERSION_MAX + 1U], *body;
        size_t size = 0U, prefix = strlen(repo);
        int length;
        if (!valid_token(entry->d_name) || read_private(root, entry->d_name, content, EVENT_MAX, &size) != 0 ||
            size <= prefix + 1U || memchr(content, 0, size) != NULL) continue;
        content[size] = '\0';
        if (memcmp(content, repo, prefix) != 0 || content[prefix] != '\n') continue;
        body = content + prefix + 1U;
        if (sscanf(body, EVENT_HEADER "E\t%32[0-9a-f]\t%32[^\t\n]\t%48[^\t\n]\n", token, id, version) != 3 ||
            strcmp(token, entry->d_name) != 0 || !c1pkg_safe_id(id) || !c1pkg_safe_version(version)) continue;
        length = snprintf(expected, sizeof(expected), EVENT_HEADER "E\t%s\t%s\t%s\n", token, id, version);
        if (length <= 0 || (size_t)length >= sizeof(expected) || strcmp(body, expected) != 0) continue;
        strcpy(event->token, token);
        strcpy(event->body, body);
        result = 0;
        break;
    }
    (void)closedir(directory);
    (void)close(lock);
    return result;
}

static void acknowledge_event(int root, const char *token)
{
    int lock = lock_file(root, "queue.lock");
    if (lock < 0) return; /* Safe duplicate on next attempt: ID never changes. */
    if (unlinkat(root, token, 0) == 0) (void)fsync(root);
    (void)close(lock);
}

static uint64_t monotonic_ms(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t) != 0) return 0U;
    return (uint64_t)t.tv_sec * 1000U + (uint64_t)t.tv_nsec / 1000000U;
}
static void cancel_worker(int signal_number) { (void)signal_number; worker_cancelled = 1; }

/* Child-only cleanup: the statistics worker must not keep GUI framebuffer,
 * input, terminal, or package/application locks alive after the parent exits. */
static void close_inherited(int keep)
{
    DIR *directory = opendir("/proc/self/fd");
    if (directory != NULL) {
        struct dirent *entry;
        int own = dirfd(directory);
        while ((entry = readdir(directory)) != NULL) {
            char *end;
            long fd = strtol(entry->d_name, &end, 10);
            if (*end == '\0' && fd >= 0 && fd != own && fd != keep) (void)close((int)fd);
        }
        (void)closedir(directory);
    } else {
        long maximum = sysconf(_SC_OPEN_MAX), fd;
        if (maximum < 0) maximum = 65536;
        for (fd = 0; fd < maximum; ++fd) if (fd != keep) (void)close((int)fd);
    }
    {
        int null_fd = open("/dev/null", O_RDWR);
        if (null_fd >= 0) {
            int fd;
            for (fd = 0; fd <= 2; ++fd) if (fd != keep) (void)dup2(null_fd, fd);
            if (null_fd > 2 && null_fd != keep) (void)close(null_fd);
        }
    }
}

/* Independent small transport: no signed-index locks, install progress callbacks,
 * credentials, redirects, URL alias failover, or unbounded output files. */
static int http_request(const char *url, const char *body, char wire[WIRE_MAX + 1U], size_t *used)
{
    int pipes[2], status = 0, failed = 0;
    pid_t pid;
    uint64_t started = monotonic_ms();
    if (worker_cancelled || pipe(pipes) != 0) return -1;
    pid = fork();
    if (pid == 0) {
        char *args[] = {"curl", "--disable", "--silent", "--globoff", "--proxy", "",
            "--proto", "=http,https", "--connect-timeout", "2", "--max-time", "4",
            "--max-redirs", "0", "--include", "--output", "-", "--user-agent", "c1pkg-metrics/1",
            "--header", "Accept: text/plain", "--header", "Content-Type: text/plain",
            "--header", "Expect:", "--url", (char *)url, NULL, NULL, NULL};
        size_t tail = sizeof(args) / sizeof(args[0]) - 3U;
        (void)signal(SIGTERM, SIG_DFL);
        (void)signal(SIGPIPE, SIG_DFL);
        (void)dup2(pipes[1], STDOUT_FILENO);
        (void)close(pipes[0]); (void)close(pipes[1]);
        if (body != NULL) { args[tail] = "--data-binary"; args[tail + 1U] = (char *)body; }
        execvp(c1pkg_helper("/usr/bin/curl", "curl"), args);
        _exit(127);
    }
    (void)close(pipes[1]);
    if (pid < 0) { (void)close(pipes[0]); return -1; }
    if (fcntl(pipes[0], F_SETFL, O_NONBLOCK) != 0) failed = 1;
    *used = 0U;
    while (!failed) {
        struct pollfd pfd = {pipes[0], POLLIN | POLLHUP, 0};
        unsigned char chunk[2048];
        ssize_t n;
        if (worker_cancelled || monotonic_ms() - started >= 6000U) { failed = 1; break; }
        n = read(pipes[0], chunk, sizeof(chunk));
        if (n > 0) {
            if ((size_t)n > WIRE_MAX - *used) { failed = 1; break; }
            memcpy(wire + *used, chunk, (size_t)n);
            *used += (size_t)n;
        } else if (n == 0) break;
        else if (errno != EAGAIN && errno != EINTR) { failed = 1; break; }
        else (void)poll(&pfd, 1U, 20);
    }
    (void)close(pipes[0]);
    /* EOF need not mean child exit; poll with the same absolute deadline. */
    for (;;) {
        pid_t waited = waitpid(pid, &status, WNOHANG);
        if (waited == pid) break;
        if (waited < 0 && errno != EINTR) { failed = 1; break; }
        if (failed || worker_cancelled || monotonic_ms() - started >= 6000U) {
            failed = 1;
            (void)kill(pid, SIGKILL);
            while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
            break;
        }
        (void)poll(NULL, 0U, 10);
    }
    wire[*used] = '\0';
    return !failed && WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
}

/* Our v1 endpoint contract uses numeric Retry-After. Unknown/date/overflow
 * delays fail closed (automatic telemetry pauses), never bypass a server limit.
 * A future server integration may explicitly support HTTP-date conversion. */
static char *response_body(char *wire, size_t size, uint64_t *delay, int *status)
{
    char *end, *line, *next;
    *delay = 0U; *status = 0;
    if (memchr(wire, 0, size) != NULL || (end = strstr(wire, "\r\n\r\n")) == NULL ||
        (size_t)(end - wire) > 8192U || strncmp(wire, "HTTP/", 5U) != 0) return NULL;
    line = strchr(wire, ' ');
    if (line == NULL || line > end || strlen(line) < 4U ||
        line[1] < '1' || line[1] > '5' || line[2] < '0' || line[2] > '9' ||
        line[3] < '0' || line[3] > '9' || (line[4] != ' ' && line[4] != '\r')) return NULL;
    *status = (line[1] - '0') * 100 + (line[2] - '0') * 10 + line[3] - '0';
    for (line = wire; line < end; line = next + 2U) {
        next = strstr(line, "\r\n");
        if (next == NULL || next > end) return NULL;
        if ((size_t)(next - line) >= 12U && strncasecmp(line, "Retry-After:", 12U) == 0) {
            char text[128];
            char *first = line + 12U, *last = next;
            uint64_t n;
            while (first < last && (*first == ' ' || *first == '\t')) ++first;
            while (last > first && (last[-1] == ' ' || last[-1] == '\t')) --last;
            if ((size_t)(last - first) >= sizeof(text)) { *delay = UINT64_MAX; continue; }
            memcpy(text, first, (size_t)(last - first)); text[last - first] = '\0';
            if (decimal(text, &n) != 0) n = UINT64_MAX;
            if (n > *delay) *delay = n;
        }
    }
    return end + 4U;
}

static int valid_snapshot(const struct c1pkg_metrics_snapshot *s)
{
    size_t i;
    if ((s->available != 0 && s->available != 1) || s->count > C1PKG_MAX_PACKAGES ||
        (!s->available && s->count != 0U)) return 0;
    for (i = 0U; i < s->count; ++i)
        if (memchr(s->items[i].id, 0, sizeof(s->items[i].id)) == NULL || !c1pkg_safe_id(s->items[i].id) ||
            (i != 0U && strcmp(s->items[i - 1U].id, s->items[i].id) >= 0)) return 0;
    return 1;
}

static void load_cache(int root, struct metrics_cache *cache)
{
    size_t size;
    char normalized[REPO_MAX + 1U];
    if (read_private(root, "cache", cache, sizeof(*cache), &size) != 0 || size != sizeof(*cache) ||
        memcmp(cache->magic, CACHE_MAGIC, sizeof(CACHE_MAGIC)) != 0 ||
        memchr(cache->repo, 0, sizeof(cache->repo)) == NULL || normalize_repo(cache->repo, normalized) != 0 ||
        strcmp(normalized, cache->repo) != 0 || !valid_snapshot(&cache->snapshot))
        memset(cache, 0, sizeof(*cache));
}
static void save_delay(struct metrics_cache *cache, uint64_t now, uint64_t delay)
{
    /* Retry-After starts at response receipt, not the beginning of a slow
     * transfer. A forward RTC adjustment must not shorten that minimum. */
    time_t received = time(NULL);
    uint64_t deadline;
    if (received > 0 && (uint64_t)received > now) now = (uint64_t)received;
    deadline = delay > UINT64_MAX - now ? UINT64_MAX : now + delay;
    if (deadline > cache->retry_at) cache->retry_at = deadline;
}

static void synchronize(const char *repo, struct c1pkg_metrics_snapshot *snapshot)
{
    struct metrics_cache cache;
    struct metrics_event event;
    char url[REPO_MAX + 64U], wire[WIRE_MAX + 1U] = {0}, *body;
    int root = open_root(), lock, status;
    time_t clock = time(NULL);
    uint64_t now, delay;
    size_t size;
    unsigned int attempt;
    memset(snapshot, 0, sizeof(*snapshot));
    if (root < 0) return;
    lock = lock_file(root, "worker.lock");
    load_cache(root, &cache);
    if (clock > 0 && strcmp(cache.repo, repo) == 0 && cache.retry_at > (uint64_t)clock &&
        cache.retry_at - (uint64_t)clock <= C1PKG_METRICS_INTERVAL)
        *snapshot = cache.snapshot;
    if (lock < 0) { (void)close(root); return; }
    if (clock <= 0 || cache.retry_at > (uint64_t)clock || worker_cancelled) goto done;
    now = (uint64_t)clock;
    memset(&cache, 0, sizeof(cache));
    strcpy(cache.magic, CACHE_MAGIC); strcpy(cache.repo, repo);
    save_delay(&cache, now, C1PKG_METRICS_INTERVAL);
    /* Persist the gate BEFORE HTTP: concurrent GUI processes and crashes cannot
     * create a request storm. If telemetry storage fails, just skip networking. */
    if (atomic_file(root, "cache.tmp", "cache", &cache, sizeof(cache)) != 0) goto done;
    (void)snprintf(url, sizeof(url), "%s/metrics.v1", repo);
    if (http_request(url, NULL, wire, &size) != 0 ||
        (body = response_body(wire, size, &delay, &status)) == NULL) goto save;
    save_delay(&cache, now, delay);
    if (status != 200 || c1pkg_metrics_parse((const unsigned char *)body,
          size - (size_t)(body - wire), &cache.snapshot) != 0) goto save;
    if (delay != 0U) goto save;
    (void)snprintf(url, sizeof(url), "%s/install-events.v1", repo);
    for (attempt = 0U; attempt < 4U && !worker_cancelled; ++attempt) {
        char expected[128];
        if (next_event(root, repo, &event) != 0) break;
        if (http_request(url, event.body, wire, &size) != 0 ||
            (body = response_body(wire, size, &delay, &status)) == NULL) break;
        save_delay(&cache, now, delay);
        (void)snprintf(expected, sizeof(expected), ACK_HEADER "%s\n", event.token);
        if (status != 200 || strcmp(body, expected) != 0) break;
        acknowledge_event(root, event.token);
        if (delay != 0U) break;
    }
save:
    (void)atomic_file(root, "cache.tmp", "cache", &cache, sizeof(cache));
    *snapshot = cache.snapshot;
done:
    (void)close(lock); (void)close(root);
}

/* v2 state deliberately never shares the v1 snapshot representation on disk.
 * The old cache is read ONCE for its gate only; it is never deleted or rewritten.
 * All repos share this worker gate, so switching URLs cannot evade Retry-After. */
#define DEVICE_CACHE_MAGIC "C1PKG-DEVICE-2"
#define DEVICE_EVENT_HEADER "C1PKG-INSTALL 2\n"
#define DEVICE_ACK_HEADER "C1PKG-INSTALL-ACK 2\nE\t"
#define DEVICE_CACHE_MARKER "C1PKG-DEVICE-CACHE-2\n"
struct device_cache {
    char magic[16];
    char repo[REPO_MAX + 1U];
    uint64_t retry_at;
    uint64_t read_at;
    unsigned int failures;
    struct c1pkg_metrics_snapshot snapshot;
    unsigned char digest[C1_SHA256_SIZE];
};
struct device_event {
    struct metrics_event wire;
    char id[C1PKG_ID_MAX + 1U];
};
struct device_identity_marker {
    char magic[24];
    unsigned char digest[C1_SHA256_SIZE];
};

static int file_absent(int root, const char *name)
{
    struct stat st;
    return fstatat(root, name, &st, AT_SYMLINK_NOFOLLOW) != 0 && errno == ENOENT;
}

/* Never unlink/replace an identity or initialization marker, including on a
 * partial write. A crash during first initialization intentionally fails closed. */
static int create_private(int root, const char *name, const void *data, size_t size)
{
    int result, fd = openat(root, name, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return -1;
    result = write_all(fd, data, size) == 0 && fsync(fd) == 0 ? 0 : -1;
    if (close(fd) != 0 || fsync(root) != 0) result = -1;
    return result;
}

/* A remaining v2 queue is also evidence of prior identity initialization.
 * Losing both identity files must not silently re-key those installations. */
static int only_legacy_events(int root)
{
    DIR *directory = open_events(root);
    struct dirent *entry;
    size_t scanned = 0U;
    int result = 0;
    if (directory == NULL) return 0;
    while ((entry = readdir(directory)) != NULL) {
        char content[EVENT_MAX + 1U], *body;
        size_t size;
        if (++scanned > 128U) goto done;
        if (!valid_token(entry->d_name)) continue;
        if (read_private(root, entry->d_name, content, EVENT_MAX, &size) != 0 ||
            memchr(content, 0, size) != NULL) goto done;
        content[size] = '\0';
        body = strchr(content, '\n');
        if (body == NULL || strncmp(body + 1U, EVENT_HEADER, sizeof(EVENT_HEADER) - 1U) != 0) goto done;
    }
    result = 1;
done:
    (void)closedir(directory);
    return result;
}

/* Queue lock required. Only the seed is secret; the marker detects even
 * same-length corruption. Neither file belongs to any application's directory. */
static int device_seed(int root, unsigned char seed[32])
{
    static const char magic[24] = "C1PKG-DEVICE-SEED-2";
    struct device_identity_marker marker = {0};
    unsigned char digest[32];
    size_t used = 0U;
    if (file_absent(root, "device-identity.init")) {
        int fd;
        if (!file_absent(root, "device-seed") || !file_absent(root, "device-cache.init") ||
            !only_legacy_events(root)) return -1;
        fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC | O_NONBLOCK);
        if (fd < 0) return -1;
        while (used < 32U) {
            ssize_t n = read(fd, seed + used, 32U - used);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) { (void)close(fd); return -1; }
            used += (size_t)n;
        }
        (void)close(fd);
        strcpy(marker.magic, "C1PKG-DEVICE-SEED-2");
        c1_sha256(seed, 32U, marker.digest);
        if (create_private(root, "device-identity.init", &marker, sizeof(marker)) != 0 ||
            create_private(root, "device-seed", seed, 32U) != 0) return -1;
        return 0;
    }
    if (read_private(root, "device-identity.init", &marker, sizeof(marker), &used) != 0 ||
        used != sizeof(marker) || memcmp(marker.magic, magic, sizeof(magic)) != 0 ||
        read_private(root, "device-seed", seed, 32U, &used) != 0 || used != 32U) return -1;
    c1_sha256(seed, 32U, digest);
    return memcmp(digest, marker.digest, sizeof(digest)) == 0 ? 0 : -1;
}

static void device_scope_field(struct c1_sha256_context *context, const char *field)
{
    size_t size = strlen(field);
    unsigned char length[4];
    length[0] = (unsigned char)(size >> 24U); length[1] = (unsigned char)(size >> 16U);
    length[2] = (unsigned char)(size >> 8U); length[3] = (unsigned char)size;
    c1_sha256_update(context, length, sizeof(length));
    c1_sha256_update(context, field, size);
}

/* Identity only: these four explicit official HTTP bases share one database.
 * Do NOT use this scope for network URLs, cache origins or queue matching.
 * All other normalized bases (including HTTPS, ports and path suffixes) retain
 * their exact scope; there is deliberately no generic host/path rewriting. */
static const char *device_repo_scope(const char *repo)
{
    static const char *const official[] = {
        "http://www.fwz233.com/c1/v1", "http://www.fwz233.com/c1/v2",
        "http://123.56.214.77/c1/v1", "http://123.56.214.77/c1/v2"
    };
    size_t i;
    for (i = 0U; i < sizeof(official) / sizeof(official[0]); ++i)
        if (strcmp(repo, official[i]) == 0) return "http://123.56.214.77/c1";
    return repo;
}

/* RFC 2104 HMAC-SHA256. Domain includes its trailing NUL; each following field
 * is a four-byte big-endian length followed by its exact UTF-8/ASCII bytes.
 * Version is deliberately NOT in the scope. No hardware/network ID is read. */
static void device_key(const unsigned char seed[32], const char *repo, const char *id, char key[65])
{
    static const char domain[] = "C1PKG-DEVICE-APP-2";
    unsigned char pad[64], digest[32];
    struct c1_sha256_context context;
    size_t i;
    for (i = 0U; i < sizeof(pad); ++i) pad[i] = (unsigned char)((i < 32U ? seed[i] : 0U) ^ 0x36U);
    c1_sha256_init(&context);
    c1_sha256_update(&context, pad, sizeof(pad));
    c1_sha256_update(&context, domain, sizeof(domain));
    device_scope_field(&context, device_repo_scope(repo)); device_scope_field(&context, id);
    c1_sha256_final(&context, digest);
    for (i = 0U; i < sizeof(pad); ++i) pad[i] = (unsigned char)((i < 32U ? seed[i] : 0U) ^ 0x5cU);
    c1_sha256_init(&context);
    c1_sha256_update(&context, pad, sizeof(pad));
    c1_sha256_update(&context, digest, sizeof(digest));
    c1_sha256_final(&context, digest);
    c1_sha256_hex(digest, key);
    memset(pad, 0, sizeof(pad)); memset(digest, 0, sizeof(digest));
}

int c1pkg_device_metrics_record_install(const struct c1pkg_config *config,
                                        const struct c1pkg_package *package)
{
    char repo[REPO_MAX + 1U], token[33], key[65], content[EVENT_MAX];
    unsigned char seed[32] = {0};
    struct dirent *entry;
    DIR *directory = NULL;
    size_t count = 0U, scanned = 0U;
    int root = -1, lock = -1, result = -1, length;
    if (config == NULL || package == NULL || normalize_repo(config->repo_base, repo) != 0 ||
        memchr(package->id, 0, sizeof(package->id)) == NULL || !c1pkg_safe_id(package->id) ||
        memchr(package->version, 0, sizeof(package->version)) == NULL || !c1pkg_safe_version(package->version) ||
        (root = open_root()) < 0 || (lock = lock_file(root, "queue.lock")) < 0) goto done;
    directory = open_events(root);
    if (directory == NULL) goto done;
    while ((entry = readdir(directory)) != NULL) {
        if (++scanned > 128U) goto done;
        if (valid_token(entry->d_name) && ++count >= C1PKG_METRICS_QUEUE_MAX) goto done;
    }
    if (device_seed(root, seed) != 0 || random_token(token) != 0 || !file_absent(root, token)) goto done;
    device_key(seed, repo, package->id, key);
    length = snprintf(content, sizeof(content), "%s\n" DEVICE_EVENT_HEADER "E\t%s\t%s\t%s\t%s\n",
                      repo, token, package->id, package->version, key);
    if (length > 0 && (size_t)length < sizeof(content))
        result = atomic_file(root, "queue.tmp", token, content, (size_t)length);
done:
    memset(seed, 0, sizeof(seed));
    if (directory != NULL) (void)closedir(directory);
    if (lock >= 0) (void)close(lock);
    if (root >= 0) (void)close(root);
    return result;
}

/* Adoption changes the on-disk record BEFORE any POST, retaining the legacy
 * event ID. Only an EXACT normalized selected repo matches, never an alias. */
static int next_device_event(int root, const char *repo, struct device_event *event)
{
    int lock = lock_file(root, "queue.lock"), result = -1;
    DIR *directory = NULL;
    struct dirent *entry;
    unsigned char seed[32] = {0};
    size_t scanned = 0U;
    if (lock < 0) return -1;
    if (device_seed(root, seed) != 0 || (directory = open_events(root)) == NULL) goto done;
    while ((entry = readdir(directory)) != NULL && ++scanned <= 128U) {
        char content[EVENT_MAX + 1U], expected[256], token[33], id[C1PKG_ID_MAX + 1U];
        char version[C1PKG_VERSION_MAX + 1U], key[65], saved_key[65], *body;
        size_t size = 0U, prefix = strlen(repo);
        int length, legacy;
        if (!valid_token(entry->d_name) || read_private(root, entry->d_name, content, EVENT_MAX, &size) != 0 ||
            size <= prefix + 1U || memchr(content, 0, size) != NULL) continue;
        content[size] = '\0';
        if (memcmp(content, repo, prefix) != 0 || content[prefix] != '\n') continue;
        body = content + prefix + 1U;
        legacy = strncmp(body, EVENT_HEADER, sizeof(EVENT_HEADER) - 1U) == 0;
        if (legacy) {
            if (sscanf(body, EVENT_HEADER "E\t%32[0-9a-f]\t%32[^\t\n]\t%48[^\t\n]\n",
                       token, id, version) != 3) continue;
        } else {
            if (sscanf(body, DEVICE_EVENT_HEADER "E\t%32[0-9a-f]\t%32[^\t\n]\t%48[^\t\n]\t%64[0-9a-f]\n",
                       token, id, version, saved_key) != 4) continue;
        }
        if (strcmp(token, entry->d_name) != 0 || !c1pkg_safe_id(id) || !c1pkg_safe_version(version)) continue;
        device_key(seed, repo, id, key);
        if (!legacy && strcmp(key, saved_key) != 0) continue;
        if (legacy) length = snprintf(expected, sizeof(expected), EVENT_HEADER "E\t%s\t%s\t%s\n", token, id, version);
        else length = snprintf(expected, sizeof(expected), DEVICE_EVENT_HEADER "E\t%s\t%s\t%s\t%s\n", token, id, version, key);
        if (length <= 0 || (size_t)length >= sizeof(expected) || strcmp(body, expected) != 0) continue;
        length = snprintf(event->wire.body, sizeof(event->wire.body), DEVICE_EVENT_HEADER "E\t%s\t%s\t%s\t%s\n",
                          token, id, version, key);
        if (length <= 0 || (size_t)length >= sizeof(event->wire.body)) continue;
        if (legacy) {
            length = snprintf(content, sizeof(content), "%s\n%s", repo, event->wire.body);
            if (length <= 0 || (size_t)length >= sizeof(content) ||
                atomic_file(root, "queue.tmp", token, content, (size_t)length) != 0) break;
        }
        strcpy(event->wire.token, token); strcpy(event->id, id);
        result = 0;
        break;
    }
done:
    memset(seed, 0, sizeof(seed));
    if (directory != NULL) (void)closedir(directory);
    (void)close(lock);
    return result;
}

static int save_device_cache(int root, struct device_cache *cache)
{
    c1_sha256(cache, offsetof(struct device_cache, digest), cache->digest);
    return atomic_file(root, "device-cache.tmp", "device-cache", cache, sizeof(*cache));
}

static int load_device_cache(int root, const char *repo, struct device_cache *cache)
{
    char marker[sizeof(DEVICE_CACHE_MARKER)], normalized[REPO_MAX + 1U];
    unsigned char digest[C1_SHA256_SIZE];
    size_t size;
    if (file_absent(root, "device-cache.init")) {
        struct metrics_cache legacy;
        if (!file_absent(root, "device-cache")) return -1;
        memset(cache, 0, sizeof(*cache));
        strcpy(cache->magic, DEVICE_CACHE_MAGIC); strcpy(cache->repo, repo);
        if (!file_absent(root, "cache")) {
            /* Unlike load_cache(), malformed old state must not reset a gate. */
            if (read_private(root, "cache", &legacy, sizeof(legacy), &size) != 0 || size != sizeof(legacy) ||
                memcmp(legacy.magic, CACHE_MAGIC, sizeof(CACHE_MAGIC)) != 0 ||
                memchr(legacy.repo, 0, sizeof(legacy.repo)) == NULL || normalize_repo(legacy.repo, normalized) != 0 ||
                strcmp(normalized, legacy.repo) != 0 || !valid_snapshot(&legacy.snapshot)) return -1;
            cache->retry_at = legacy.retry_at; /* Including UINT64_MAX; counts ignored. */
        }
        if (create_private(root, "device-cache.init", DEVICE_CACHE_MARKER, sizeof(DEVICE_CACHE_MARKER)) != 0 ||
            save_device_cache(root, cache) != 0) return -1;
        return 0;
    }
    if (read_private(root, "device-cache.init", marker, sizeof(marker), &size) != 0 || size != sizeof(marker) ||
        memcmp(marker, DEVICE_CACHE_MARKER, sizeof(marker)) != 0 ||
        read_private(root, "device-cache", cache, sizeof(*cache), &size) != 0 || size != sizeof(*cache) ||
        memcmp(cache->magic, DEVICE_CACHE_MAGIC, sizeof(DEVICE_CACHE_MAGIC)) != 0 ||
        memchr(cache->repo, 0, sizeof(cache->repo)) == NULL || normalize_repo(cache->repo, normalized) != 0 ||
        strcmp(normalized, cache->repo) != 0 || cache->failures > 5U || !valid_snapshot(&cache->snapshot)) return -1;
    c1_sha256(cache, offsetof(struct device_cache, digest), digest);
    return memcmp(digest, cache->digest, sizeof(digest)) == 0 ? 0 : -1;
}

static uint64_t device_deadline(uint64_t now, uint64_t delay)
{
    time_t received = time(NULL);
    if (received > 0 && (uint64_t)received > now) now = (uint64_t)received;
    return delay > UINT64_MAX - now ? UINT64_MAX : now + delay;
}
static void device_delay(struct device_cache *cache, uint64_t now, uint64_t delay)
{
    uint64_t deadline = device_deadline(now, delay);
    if (deadline > cache->retry_at) cache->retry_at = deadline;
}
static void device_failure(struct device_cache *cache, uint64_t now)
{
    uint64_t delay;
    if (cache->failures < 5U) ++cache->failures;
    delay = (uint64_t)C1PKG_DEVICE_METRICS_RETRY_INTERVAL << cache->failures;
    if (delay > C1PKG_DEVICE_METRICS_BACKOFF_MAX) delay = C1PKG_DEVICE_METRICS_BACKOFF_MAX;
    device_delay(cache, now, delay);
}

/* Strict token + ID + canonical decimal count, with no trailing fields/bytes. */
static int device_ack(const char *body, const struct device_event *event, uint64_t *count)
{
    char expected[160];
    int length = snprintf(expected, sizeof(expected), DEVICE_ACK_HEADER "%s\nI\t%s\t",
                          event->wire.token, event->id);
    char number[24];
    size_t size;
    if (length <= 0 || (size_t)length >= sizeof(expected) || strncmp(body, expected, (size_t)length) != 0) return -1;
    body += (size_t)length;
    size = strlen(body);
    if (size < 2U || size >= sizeof(number) || body[size - 1U] != '\n') return -1;
    memcpy(number, body, size - 1U); number[size - 1U] = '\0';
    return decimal(number, count);
}
static int device_snapshot_count(struct c1pkg_metrics_snapshot *snapshot, const char *id, uint64_t count)
{
    size_t i = 0U;
    while (i < snapshot->count && strcmp(snapshot->items[i].id, id) < 0) ++i;
    if (i == snapshot->count || strcmp(snapshot->items[i].id, id) != 0) {
        if (snapshot->count >= C1PKG_MAX_PACKAGES) return -1;
        memmove(&snapshot->items[i + 1U], &snapshot->items[i], (snapshot->count - i) * sizeof(snapshot->items[0]));
        strcpy(snapshot->items[i].id, id); ++snapshot->count;
    }
    snapshot->items[i].installations = count;
    return 0;
}

static void synchronize_device(const char *repo, struct c1pkg_metrics_snapshot *snapshot)
{
    struct device_cache cache;
    struct device_event event;
    unsigned char seed[32] = {0};
    char url[REPO_MAX + 64U], wire[WIRE_MAX + 1U] = {0}, *body;
    int root = open_root(), lock, queue_lock, status;
    time_t clock = time(NULL);
    uint64_t now, delay;
    size_t size;
    unsigned int attempt;
    int have_event, needs_read;
    memset(snapshot, 0, sizeof(*snapshot));
    if (root < 0) return;
    lock = lock_file(root, "worker.lock");
    if (lock < 0) { (void)close(root); return; }
    queue_lock = lock_file(root, "queue.lock");
    if (queue_lock < 0) goto done;
    status = device_seed(root, seed);
    memset(seed, 0, sizeof(seed));
    (void)close(queue_lock);
    if (status != 0 || load_device_cache(root, repo, &cache) != 0) goto done;
    if (strcmp(cache.repo, repo) == 0) *snapshot = cache.snapshot;
    if (clock <= 0 || cache.retry_at > (uint64_t)clock || cache.retry_at == UINT64_MAX || worker_cancelled) goto done;
    now = (uint64_t)clock;
    if (strcmp(cache.repo, repo) != 0) {
        strcpy(cache.repo, repo); cache.read_at = 0U;
        memset(&cache.snapshot, 0, sizeof(cache.snapshot));
    }
    needs_read = !cache.snapshot.available || cache.read_at <= now;
    have_event = next_device_event(root, repo, &event) == 0;
    if (!needs_read && !have_event) goto done;
    /* Admit a bounded batch BEFORE its first request, and refresh the minimum
     * before each further request. Backoff/server deadlines are never reduced. */
    device_delay(&cache, now, C1PKG_DEVICE_METRICS_RETRY_INTERVAL);
    if (save_device_cache(root, &cache) != 0) goto done;
    if (needs_read) {
        memset(&cache.snapshot, 0, sizeof(cache.snapshot));
        cache.read_at = 0U;
        (void)snprintf(url, sizeof(url), "%s/metrics.v2", repo);
        if (http_request(url, NULL, wire, &size) != 0 ||
            (body = response_body(wire, size, &delay, &status)) == NULL) goto failed;
        device_delay(&cache, now, delay);
        if (status != 200 || c1pkg_device_metrics_parse((const unsigned char *)body,
            size - (size_t)(body - wire), &cache.snapshot) != 0) goto failed;
        cache.read_at = device_deadline(now, C1PKG_METRICS_INTERVAL);
        if (delay != 0U) goto failed;
        if (save_device_cache(root, &cache) != 0) goto done;
    }
    (void)snprintf(url, sizeof(url), "%s/install-events.v2", repo);
    for (attempt = 0U; attempt < 4U && !worker_cancelled; ++attempt) {
        uint64_t count;
        if (attempt != 0U) have_event = next_device_event(root, repo, &event) == 0;
        if (!have_event) break;
        device_delay(&cache, now, C1PKG_DEVICE_METRICS_RETRY_INTERVAL);
        if (save_device_cache(root, &cache) != 0) goto done;
        if (http_request(url, event.wire.body, wire, &size) != 0 ||
            (body = response_body(wire, size, &delay, &status)) == NULL) goto failed;
        device_delay(&cache, now, delay);
        if (status != 200 || delay != 0U || device_ack(body, &event, &count) != 0 ||
            device_snapshot_count(&cache.snapshot, event.id, count) != 0) goto failed;
        /* Persist the returned count before removal. Either crash ordering
         * yields an idempotent duplicate, never an invented count increment. */
        cache.failures = 0U;
        if (save_device_cache(root, &cache) != 0) goto done;
        acknowledge_event(root, event.wire.token);
    }
    cache.failures = 0U;
    goto save;
failed:
    device_failure(&cache, now);
save:
    (void)save_device_cache(root, &cache);
    *snapshot = cache.snapshot;
done:
    (void)close(lock); (void)close(root);
}

static struct c1pkg_metrics_job *start_metrics_job(const struct c1pkg_config *config,
    void (*synchronize_fn)(const char *, struct c1pkg_metrics_snapshot *))
{
    char repo[REPO_MAX + 1U];
    struct c1pkg_metrics_job *job;
    int pipes[2];
    if (config == NULL || normalize_repo(config->repo_base, repo) != 0) return NULL;
    job = calloc(1U, sizeof(*job));
    if (job == NULL) return NULL;
    if (pipe(pipes) != 0) { free(job); return NULL; }
    if (fcntl(pipes[0], F_SETFD, FD_CLOEXEC) != 0 || fcntl(pipes[1], F_SETFD, FD_CLOEXEC) != 0 ||
        fcntl(pipes[0], F_SETFL, O_NONBLOCK) != 0) {
        (void)close(pipes[0]); (void)close(pipes[1]); free(job); return NULL;
    }
    job->pid = fork();
    if (job->pid == 0) {
        struct c1pkg_metrics_snapshot snapshot;
        struct sigaction action;
        memset(&action, 0, sizeof(action));
        action.sa_handler = cancel_worker;
        sigemptyset(&action.sa_mask);
        worker_cancelled = 0;
        (void)sigaction(SIGTERM, &action, NULL);
        (void)signal(SIGCHLD, SIG_DFL);
        (void)signal(SIGPIPE, SIG_IGN);
        close_inherited(pipes[1]);
        synchronize_fn(repo, &snapshot);
        if (!worker_cancelled) (void)write_all(pipes[1], &snapshot, sizeof(snapshot));
        (void)close(pipes[1]);
        _exit(0);
    }
    (void)close(pipes[1]);
    if (job->pid < 0) { (void)close(pipes[0]); free(job); return NULL; }
    job->fd = pipes[0];
    return job;
}

struct c1pkg_metrics_job *c1pkg_metrics_start(const struct c1pkg_config *config)
{
    return start_metrics_job(config, synchronize);
}

struct c1pkg_metrics_job *c1pkg_device_metrics_start(const struct c1pkg_config *config)
{
    return start_metrics_job(config, synchronize_device);
}

void c1pkg_device_metrics_cancel(struct c1pkg_metrics_job *job)
{
    c1pkg_metrics_cancel(job);
}

int c1pkg_device_metrics_poll(struct c1pkg_metrics_job **job,
                              struct c1pkg_metrics_snapshot *snapshot)
{
    return c1pkg_metrics_poll(job, snapshot);
}

void c1pkg_metrics_cancel(struct c1pkg_metrics_job *job)
{
    if (job != NULL) { job->cancelled = 1; (void)kill(job->pid, SIGTERM); }
}

int c1pkg_metrics_poll(struct c1pkg_metrics_job **pointer, struct c1pkg_metrics_snapshot *snapshot)
{
    struct c1pkg_metrics_job *job;
    int status = 0, result;
    pid_t waited;
    if (pointer == NULL || *pointer == NULL || snapshot == NULL) return -1;
    job = *pointer;
    while (job->used < sizeof(job->result)) {
        ssize_t n = read(job->fd, (unsigned char *)&job->result + job->used, sizeof(job->result) - job->used);
        if (n > 0) job->used += (size_t)n;
        else if (n < 0 && errno == EINTR) continue;
        else break;
    }
    waited = waitpid(job->pid, &status, WNOHANG);
    if (waited == 0 || (waited < 0 && errno == EINTR)) return 0;
    /* The child may have written between the first drain and waitpid. */
    while (job->used < sizeof(job->result)) {
        ssize_t n = read(job->fd, (unsigned char *)&job->result + job->used, sizeof(job->result) - job->used);
        if (n > 0) job->used += (size_t)n;
        else if (n < 0 && errno == EINTR) continue;
        else break;
    }
    result = waited == job->pid && WIFEXITED(status) && WEXITSTATUS(status) == 0 &&
             !job->cancelled && job->used == sizeof(job->result) && valid_snapshot(&job->result) ? 1 : -1;
    memset(snapshot, 0, sizeof(*snapshot));
    if (result == 1) *snapshot = job->result;
    (void)close(job->fd); free(job); *pointer = NULL;
    return result;
}
