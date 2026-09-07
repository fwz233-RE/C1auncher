/* Standalone integration test. Link util.c and Ed25519 verify/keypair/sign sources.
 * Include repo.c to exercise HTTP header handling and isolated cache paths without
 * exposing production-only test APIs or touching device paths. */
#ifndef C1PKG_STATE_ROOT
#define C1PKG_STATE_ROOT "/tmp/c1pkg-repo-unit"
#endif
#include "pkg_transport_clock.h"
#define clock_gettime test_transport_clock_gettime
#define time test_transport_time
#define poll test_transport_poll
#define c1pkg_helper test_repo_helper
#include "../src/pkg/repo.c"
#undef c1pkg_helper
#undef clock_gettime
#undef time
#undef poll

const char *test_repo_helper(const char *absolute, const char *name)
{
    return strcmp(name, "curl") == 0 ? C1PKG_STATE_ROOT "/curl-shim" : absolute;
}
#include <arpa/inet.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>

static int failures;
static unsigned char public_key[32], private_key[64];
static const char digest[] = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

static void expect(int condition, const char *message)
{
    if (!condition) { fprintf(stderr, "FAIL: %s\n", message); ++failures; }
}

int c1pkg_storage_state_init(char *error, size_t error_size)
{
    return c1pkg_mkdir_p(C1PKG_STATE_ROOT, 0700, error, error_size);
}

static void write_text(const char *path, const char *text)
{
    (void)unlink(path);
    expect(c1pkg_write_file(path, text, strlen(text), 0600, NULL, 0U) == 0, "write fixture");
}

static void index_text(char *text, size_t size, int format, unsigned int sequence, const char *author)
{
    (void)snprintf(text, size, "C1PKG-INDEX %d\nS\t%u\nP\tapp\t1.0.0\tApp\tapp.tar.gz\t%s\t100\tbin/app%s%s\n",
                   format, sequence, digest, format == 2 ? "\t" : "", format == 2 ? author : "");
}

static int parse(const char *text, struct c1pkg_index *index)
{
    char error[256] = "";
    return c1pkg_repo_parse((const unsigned char *)text, strlen(text), index, error, sizeof(error));
}

static void test_parser(void)
{
    struct c1pkg_index index = {0};
    struct c1pkg_index saved;
    char text[2048];
    char author[42];
    const char *invalid[] = {"", " leading", "trailing ", "bad\tfield", "bad\001", "bad\177", "bad\200"};
    size_t i;
    index_text(text, sizeof(text), 1, 1U, "");
    expect(parse(text, &index) == 0 && strcmp(index.packages[0].author, "Unknown") == 0, "v1 uses Unknown author");
    index_text(text, sizeof(text), 2, 2U, "Example Author");
    expect(parse(text, &index) == 0 && strcmp(index.packages[0].author, "Example Author") == 0, "v2 accepts ninth author field");
    saved = index;
    for (i = 0U; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        index_text(text, sizeof(text), 2, 3U, invalid[i]);
        expect(parse(text, &index) != 0 && memcmp(&index, &saved, sizeof(index)) == 0, "invalid author rejected without publishing partial index");
    }
    memset(author, 'A', 40U); author[40] = '\0';
    index_text(text, sizeof(text), 2, 3U, author);
    expect(parse(text, &index) == 0, "40-byte author accepted");
    author[40] = 'A'; author[41] = '\0';
    index_text(text, sizeof(text), 2, 3U, author);
    expect(parse(text, &index) != 0, "41-byte author rejected");
    expect(parse("C1PKG-INDEX 2\n", &index) != 0, "header-only truncated index rejected");
    expect(parse("C1PKG-INDEX 1\nS\t0\n", &index) != 0, "zero sequence rejected");
    expect(parse("C1PKG-INDEX 2\nS\t4", &index) != 0, "missing LF rejected");
    expect(parse("C1PKG-INDEX 2\nS\t4\n", &index) == 0 && index.count == 0U, "signed empty repository accepted with sequence");
    index_text(text, sizeof(text), 2, 4U, "Author");
    strcat(text, "invalid\n");
    saved = index;
    expect(parse(text, &index) != 0 && memcmp(&index, &saved, sizeof(index)) == 0, "valid first record followed by malformed record is never published");
}

static void test_config_headers(void)
{
    char url[1025], error[256] = "";
    uint64_t seconds = 0U;
    int has_retry_after;
    const char *path = C1PKG_STATE_ROOT "/repository.url";
    expect(strcmp(C1PKG_REPO_DEFAULT, "http://www.fwz233.com/c1/v2") == 0, "HTTP domain is default repository");
    expect(c1pkg_repo_read_url(path, url, sizeof(url), error, sizeof(error)) == 0 && url[0] == '\0', "absent persistent URL leaves caller default in place");
    expect(c1_repository_fallback_url("http://www.fwz233.com/c1/v2/app.tar.gz", url, sizeof(url)) &&
           strcmp(url, "http://123.56.214.77/c1/v2/app.tar.gz") == 0, "fixed fallback preserves package path");
    expect(c1_repository_fallback_url("http://www.fwz233.com:80/c1/v2", url, sizeof(url)) &&
           strcmp(url, "http://123.56.214.77/c1/v2") == 0, "explicit HTTP port maps");
    {
        const char *unmapped[] = {"http://www.fwz233.com.evil/c1/v2", "http://www.fwz233.com@evil/c1/v2",
            "http://www.fwz233.com:81/c1/v2", "https://www.fwz233.com/c1/v2", "http://other.example/c1/v2",
            "http://123.56.214.77/c1/v2"};
        size_t i;
        for (i = 0U; i < sizeof(unmapped) / sizeof(unmapped[0]); ++i)
            expect(!c1_repository_fallback_url(unmapped[i], url, sizeof(url)), "custom and spoofed hostnames never map");
        expect(!c1_repository_fallback_url(C1PKG_REPO_DEFAULT, url, 5U), "fallback output truncation rejected");
    }
    write_text(path, "https://example.org/c1/v2\r\n");
    expect(c1pkg_repo_read_url(path, url, sizeof(url), error, sizeof(error)) == 0 && strcmp(url, "https://example.org/c1/v2") == 0, "persistent URL allows LF or CRLF");
    write_text(path, "https://example.org/c1/v2\nextra\n");
    expect(c1pkg_repo_read_url(path, url, sizeof(url), error, sizeof(error)) != 0 && url[0] == '\0', "multiline configuration rejected");
    write_text(path, "HTTP/1.1 503 Busy\r\nRetry-After: 999999999999999999999999\r\n\r\n");
    expect(response_status(path, &seconds, &has_retry_after) == 503 && has_retry_after && seconds == UINT64_MAX,
           "Retry-After overflow is retained, never clamped to an earlier request");
    write_text(path, "HTTP/1.1 302 Redirect\r\nRetry-After: 29\r\n\r\nHTTP/1.1 429 Busy\r\nRetry-After: 5\r\n\r\n");
    expect(response_status(path, &seconds, &has_retry_after) == 429 && has_retry_after && seconds == 5U,
           "final response Retry-After controls waiting");
    write_text(path, "HTTP/1.1 503 Busy\r\nRetry-After: -1\r\n\r\n");
    expect(response_status(path, &seconds, &has_retry_after) == 503 && !has_retry_after,
           "invalid delay requests bounded client fallback");
    write_text(path, "HTTP/1.1 404 Missing\r\nRetry-After: 5\r\n\r\n");
    expect(!transient_status(response_status(path, &seconds, &has_retry_after)), "permanent HTTP error is not retried");
}

static void send_bytes(int fd, const void *data, size_t size)
{
    const unsigned char *bytes = data;
    while (size != 0U) {
        ssize_t amount = write(fd, bytes, size);
        if (amount <= 0) return;
        bytes += (size_t)amount; size -= (size_t)amount;
    }
}

/* modes: 0 signed pair, 1 initial mismatched pair, 2 persistent bad signature,
 * 3 busy twice then success, 4 always busy, 5 stall response. */
static pid_t server_start(char *base, size_t base_size, int mode, const char *text)
{
    struct sockaddr_in address;
    socklen_t length = sizeof(address);
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    pid_t child;
    unsigned char signature[64];
    expect(listener >= 0, "create HTTP listener");
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    expect(bind(listener, (struct sockaddr *)&address, sizeof(address)) == 0 && listen(listener, 8) == 0 &&
           getsockname(listener, (struct sockaddr *)&address, &length) == 0, "bind local HTTP server");
    (void)snprintf(base, base_size, "http://127.0.0.1:%u/c1/v2", (unsigned int)ntohs(address.sin_port));
    ed25519_sign(signature, (const unsigned char *)text, strlen(text), public_key, private_key);
    child = fork();
    expect(child >= 0, "fork local HTTP server");
    if (child == 0) {
        unsigned int requests = 0U;
        (void)signal(SIGPIPE, SIG_IGN);
        for (;;) {
            char request[4096], header[256];
            const void *body = text;
            size_t body_size = strlen(text);
            int fd = accept(listener, NULL, NULL);
            ssize_t amount;
            int status = 200;
            if (fd < 0) _exit(1);
            amount = read(fd, request, sizeof(request) - 1U);
            if (amount <= 0) { (void)close(fd); continue; }
            request[amount] = '\0';
            ++requests;
            if (mode == 5) { (void)poll(NULL, 0U, 3000); }
            if (mode == 9) { (void)poll(NULL, 0U, 50000); }
            if (strstr(request, "index.v1.sig") != NULL) {
                body = signature; body_size = sizeof(signature);
                if (mode == 2) { body = "invalid"; body_size = 7U; }
            } else if (mode == 1 && requests == 1U) {
                body = "C1PKG-INDEX 2\nS\t1\n"; body_size = strlen(body);
            }
            if (mode == 4 || (mode == 3 && requests <= 2U)) {
                status = requests == 1U ? 429 : 503;
                body = "busy"; body_size = 4U;
            }
            if (mode == 6) { status = 403; body = "forbidden"; body_size = 9U; }
            if (mode == 7 || mode == 8) status = 308;
            (void)snprintf(header, sizeof(header), "HTTP/1.1 %d Test\r\nContent-Length: %lu\r\nRetry-After: %u\r\n%sConnection: close\r\n\r\n", status, (unsigned long)body_size,
                           mode == 10 ? 75U : 0U, mode == 7 ? "Location: https://127.0.0.1:1/forbidden\r\n" : "");
            send_bytes(fd, header, strlen(header)); send_bytes(fd, body, body_size);
            (void)close(fd);
        }
    }
    (void)close(listener);
    return child;
}

static void server_stop(pid_t child)
{
    if (child > 0) { (void)kill(child, SIGTERM); while (waitpid(child, NULL, 0) < 0 && errno == EINTR) { } }
}

static int cancel_wait(const char *message, void *context)
{
    int *calls = context;
    if (message != NULL && strstr(message, "等待") != NULL) { ++*calls; return 1; }
    return 0;
}

static void test_cooldown_headers(void)
{
    const char *path = C1PKG_STATE_ROOT "/headers-test";
    uint64_t seconds;
    int present;
    char error[256] = "";
    int calls = 0;
    static const char *dates[] = {
        "Sun, 06 Nov 1994 08:50:52 GMT",
        "Sunday, 06-Nov-94 08:50:52 GMT",
        "Sun Nov  6 08:50:52 1994"
    };
    size_t i;
    for (i = 0U; i < sizeof(dates) / sizeof(dates[0]); ++i) {
        char text[256];
        (void)snprintf(text, sizeof(text), "HTTP/1.1 200 OK\r\nRetry-After: %s \t\r\nDate: Sun, 06 Nov 1994 08:49:37 GMT\r\n\r\n", dates[i]);
        write_text(path, text);
        expect(response_status(path, &seconds, &present) == 200 && present && seconds == 75U,
               "all HTTP-date forms use server Date regardless of order or clock skew");
    }
    write_text(path, "HTTP/1.1 200 OK\r\nRetry-After: 75\t \r\nRetry-After: 5\r\nRetry-After: invalid\r\n\r\n");
    expect(response_status(path, &seconds, &present) == 200 && present && seconds == 75U,
           "duplicates retain longest valid cooldown including success");
    write_text(path, "HTTP/1.1 302 Found\r\nRetry-After: 999\r\n\r\nHTTP/1.1 200 OK\r\n\r\n");
    expect(response_status(path, &seconds, &present) == 200 && !present,
           "redirect cooldown is not mistaken for final response header");
    expect(retry_after_seconds("Sun, 06 Nov 1994 08:49:37 GMT", 2000000000, &seconds) && seconds == 0U,
           "past HTTP date permits immediate retry");
    expect(!retry_after_seconds("Sun, 31 Feb 2026 08:49:37 GMT", 0, &seconds) &&
           !retry_after_seconds("+75", 0, &seconds) && !retry_after_seconds("75junk", 0, &seconds),
           "invalid dates, signs, and suffixes are rejected");
    transport_not_before = 0U;
    remember_cooldown(UINT64_MAX);
    expect(wait_for_cooldown(error, sizeof(error)) != 0 && errno == EAGAIN &&
           transport_not_before == UINT64_MAX, "excessive delay refuses early requests without overflow");
    transport_not_before = 0U;
    remember_cooldown(75U);
    c1pkg_set_progress(cancel_wait, &calls);
    expect(wait_for_cooldown(error, sizeof(error)) != 0 && errno == ECANCELED && calls == 1,
           "countdown cancellation preserves errno and pending deadline");
    c1pkg_set_progress(NULL, NULL);
    {
        uint64_t start = test_transport_ms;
        expect(wait_for_cooldown(error, sizeof(error)) == 0 && test_transport_ms - start == 75000U,
               "next action honors remaining cooldown after cancellation");
    }
    (void)unlink(path);
}

static int cancel_transfer(const char *message, void *context)
{
    int *calls = context;
    (void)message;
    return ++*calls >= 3;
}

static void test_network_cache(void)
{
    char text[2048], base[256], error[256] = "";
    const char *output = C1PKG_STATE_ROOT "/download";
    struct c1pkg_config config = {base, C1PKG_STATE_ROOT "/key"};
    struct c1pkg_index index = {0}, saved;
    pid_t server;
    int calls = 0;
    index_text(text, sizeof(text), 2, 10U, "Signed Author");
    server = server_start(base, sizeof(base), 1, text);
    expect(c1pkg_repo_refresh(&config, &index, error, sizeof(error)) == 0 && index.sequence == 10U, "index/signature publication race retries complete pair");
    server_stop(server);
    saved = index;
    expect(c1pkg_repo_load_cached(&config, &index, error, sizeof(error)) == 0 && strcmp(index.packages[0].author, "Signed Author") == 0, "atomic signed cache reloads author");
    {
        uint64_t start = test_transport_ms;
        server = server_start(base, sizeof(base), 10, text);
        expect(c1pkg_repo_refresh(&config, &index, error, sizeof(error)) == 0 &&
               test_transport_ms - start >= 75000U && transport_not_before == test_transport_ms + 75000U,
               "successful index delays signature 75 seconds and signature retains next-action cooldown");
        server_stop(server);
    }
    server = server_start(base, sizeof(base), 2, text);
    expect(c1pkg_repo_refresh(&config, &index, error, sizeof(error)) != 0 && memcmp(&index, &saved, sizeof(index)) == 0, "persistent signature rejection leaves caller index unchanged");
    server_stop(server);
    expect(c1pkg_repo_load_cached(&config, &index, error, sizeof(error)) == 0, "failed refresh preserves verified cache");
    index_text(text, sizeof(text), 2, 9U, "Author");
    server = server_start(base, sizeof(base), 0, text);
    expect(c1pkg_repo_refresh(&config, &index, error, sizeof(error)) != 0 && strstr(error, "rollback") != NULL && index.sequence == 10U, "network sequence rollback rejected without exposing packages");
    server_stop(server);
    server = server_start(base, sizeof(base), 0, "C1PKG-INDEX 2\n");
    expect(c1pkg_repo_refresh(&config, &index, error, sizeof(error)) != 0 && index.sequence == 10U, "signed truncated index rejected");
    server_stop(server);
    write_text(C1PKG_STATE_ROOT "/highest-sequence", "11\n");
    expect(c1pkg_repo_load_cached(&config, &index, error, sizeof(error)) != 0 && index.sequence == 10U, "cache below durable rollback floor rejected");
    expect(rename(C1PKG_STATE_ROOT "/cache/verified.v1", C1PKG_STATE_ROOT "/cache/saved") == 0 &&
           mkdir(C1PKG_STATE_ROOT "/cache/verified.v1", 0700) == 0, "inject cache commit failure");
    index_text(text, sizeof(text), 2, 12U, "Author");
    server = server_start(base, sizeof(base), 0, text);
    expect(c1pkg_repo_refresh(&config, &index, error, sizeof(error)) != 0 && index.sequence == 10U,
           "cache commit failure never publishes candidate items");
    server_stop(server);
    expect(rmdir(C1PKG_STATE_ROOT "/cache/verified.v1") == 0 &&
           rename(C1PKG_STATE_ROOT "/cache/saved", C1PKG_STATE_ROOT "/cache/verified.v1") == 0,
           "remove injected commit failure");
    expect(c1pkg_repo_load_cached(&config, &index, error, sizeof(error)) != 0,
           "interrupted cache commit cannot lower the persisted rollback floor");
    server = server_start(base, sizeof(base), 0, text);
    expect(c1pkg_repo_refresh(&config, &index, error, sizeof(error)) == 0 && index.sequence == 12U,
           "verified refresh recovers after interrupted cache commit");
    server_stop(server);
    server = server_start(base, sizeof(base), 3, "payload");
    expect(c1pkg_fetch(base, output, 100U, error, sizeof(error)) == 0, "429 and 503 Retry-After retries eventually download");
    server_stop(server);
    server = server_start(base, sizeof(base), 4, "payload");
    c1pkg_set_progress(cancel_wait, &calls);
    expect(c1pkg_fetch(base, output, 100U, error, sizeof(error)) != 0 && calls == 1 && access(output, F_OK) != 0, "visible waiting callback permits cancellation and removes partial file");
    c1pkg_set_progress(NULL, NULL);
    server_stop(server);
    server = server_start(base, sizeof(base), 4, "payload");
    expect(c1pkg_fetch(base, output, 100U, error, sizeof(error)) != 0 && strstr(error, "重试上限") != NULL, "busy retries have a finite limit");
    server_stop(server);
    calls = 0;
    server = server_start(base, sizeof(base), 5, "payload");
    c1pkg_set_progress(cancel_transfer, &calls);
    expect(c1pkg_fetch(base, output, 100U, error, sizeof(error)) != 0 && errno == ECANCELED, "active transfer can be cancelled");
    c1pkg_set_progress(NULL, NULL);
    server_stop(server);
    server = server_start(base, sizeof(base), 0, "too-large");
    expect(c1pkg_fetch(base, output, 2U, error, sizeof(error)) != 0 && access(output, F_OK) != 0, "oversized download removed");
    server_stop(server);
}

static size_t logged_requests(const char *role)
{
    unsigned char *data = NULL;
    size_t size = 0U, count = 0U;
    char marker[64];
    char *cursor;
    (void)snprintf(marker, sizeof(marker), "\"role\": \"%s\"", role);
    if (c1pkg_read_file(C1PKG_STATE_ROOT "/curl.log", &data, &size, 65536U, NULL, 0U) != 0) return 0U;
    cursor = (char *)data;
    while ((cursor = strstr(cursor, marker)) != NULL) { ++count; ++cursor; }
    free(data);
    return count;
}

static void clear_log(void)
{
    (void)unlink(C1PKG_STATE_ROOT "/curl.log");
}

static void test_fixed_fallback(void)
{
    char text[2048], primary[256], fallback[256], error[256] = "";
    struct c1pkg_config config = {C1PKG_REPO_DEFAULT, C1PKG_STATE_ROOT "/key"};
    struct c1pkg_index index = {0}, saved;
    const char *output = C1PKG_STATE_ROOT "/download";
    pid_t first, second;
    int modes[] = {6, 7, 8, 2};
    size_t i;
    int calls = 0;
    index_text(text, sizeof(text), 2, 20U, "Fallback Author");
    second = server_start(fallback, sizeof(fallback), 0, text);
    expect(setenv("C1PKG_TEST_FALLBACK", fallback, 1) == 0, "configure loopback fallback");
    for (i = 0U; i < 2U; ++i) {
        expect(setenv("C1PKG_TEST_PRIMARY", i == 0U ? "dns-error" : "connect-error", 1) == 0, "simulate transport failure");
        clear_log();
        expect(c1pkg_repo_refresh(&config, &index, error, sizeof(error)) == 0 && index.sequence == 20U &&
               logged_requests("primary") == 4U && logged_requests("fallback") == 2U,
               "DNS/connect failure retries a complete signed pair on fixed IP");
        clear_log();
        expect(c1pkg_fetch("http://www.fwz233.com/c1/v2/app.tar.gz", output, 4096U, error, sizeof(error)) == 0 &&
               logged_requests("primary") == 4U && logged_requests("fallback") == 1U,
               "package transport failure falls back to fixed IP");
    }
    for (i = 0U; i < sizeof(modes) / sizeof(modes[0]); ++i) {
        first = server_start(primary, sizeof(primary), modes[i], text);
        expect(setenv("C1PKG_TEST_PRIMARY", primary, 1) == 0, "configure primary fixture");
        clear_log();
        expect(c1pkg_repo_refresh(&config, &index, error, sizeof(error)) == 0 &&
               strcmp(index.packages[0].author, "Fallback Author") == 0 && logged_requests("fallback") == 2U,
               "403/HTTPS redirect/incomplete redirect/bad signature uses verified fallback pair");
        if (modes[i] != 2) {
            clear_log();
            expect(c1pkg_fetch("http://www.fwz233.com/c1/v2/app.tar.gz", output, 4096U, error, sizeof(error)) == 0 &&
                   logged_requests("primary") == 1U && logged_requests("fallback") == 1U,
                   "package 403 and unsupported redirects use fixed fallback");
        }
        server_stop(first);
    }
    first = server_start(primary, sizeof(primary), 4, text);
    expect(setenv("C1PKG_TEST_PRIMARY", primary, 1) == 0, "configure congested primary");
    clear_log();
    expect(c1pkg_repo_refresh(&config, &index, error, sizeof(error)) != 0 && errno == EAGAIN &&
           logged_requests("primary") == 4U && logged_requests("fallback") == 0U,
           "exhausted busy metadata never bypasses server limit through IP alias");
    clear_log();
    expect(c1pkg_fetch("http://www.fwz233.com/c1/v2/app.tar.gz", output, 4096U, error, sizeof(error)) != 0 &&
           errno == EAGAIN && logged_requests("primary") == 4U && logged_requests("fallback") == 0U &&
           transport_not_before > test_transport_ms,
           "exhausted busy packages retain next-action cooldown and never contact alias");
    server_stop(first);
    first = server_start(primary, sizeof(primary), 0, text);
    expect(setenv("C1PKG_TEST_PRIMARY", primary, 1) == 0, "configure working primary");
    clear_log();
    expect(c1pkg_repo_refresh(&config, &index, error, sizeof(error)) == 0 &&
           logged_requests("primary") == 2U && logged_requests("fallback") == 0U,
           "working primary never contacts fallback");
    server_stop(first);
    expect(setenv("C1PKG_TEST_PRIMARY", "timeout-error", 1) == 0, "simulate primary metadata timeout");
    clear_log();
    expect(c1pkg_repo_refresh(&config, &index, error, sizeof(error)) == 0 &&
           logged_requests("primary") == 4U && logged_requests("fallback") == 2U,
           "network timeouts retry within finite attempt bounds then verify fallback");
    saved = index;
    expect(setenv("C1PKG_TEST_PRIMARY", "dns-error", 1) == 0 &&
           setenv("C1PKG_TEST_FALLBACK", "connect-error", 1) == 0, "configure both transport failures");
    clear_log();
    expect(c1pkg_repo_refresh(&config, &index, error, sizeof(error)) != 0 &&
           memcmp(&index, &saved, sizeof(index)) == 0 && logged_requests("fallback") == 4U,
           "both endpoints failing leaves caller unchanged");
    expect(c1pkg_repo_load_cached(&config, &index, error, sizeof(error)) == 0 && index.sequence == 20U,
           "both failures preserve verified cache for display only");
    expect(c1pkg_fetch("http://www.fwz233.com/c1/v2/app.tar.gz", output, 4096U, error, sizeof(error)) != 0 &&
           access(output, F_OK) != 0, "both package failures remove partial output");
    expect(setenv("C1PKG_TEST_FALLBACK", fallback, 1) == 0, "restore fallback fixture");
    first = server_start(primary, sizeof(primary), 5, text);
    expect(setenv("C1PKG_TEST_PRIMARY", primary, 1) == 0, "configure stalled primary");
    clear_log();
    c1pkg_set_progress(cancel_transfer, &calls);
    expect(c1pkg_repo_refresh(&config, &index, error, sizeof(error)) != 0 && errno == ECANCELED &&
           logged_requests("fallback") == 0U, "metadata cancellation never contacts fallback");
    c1pkg_set_progress(NULL, NULL);
    clear_log(); calls = 0;
    c1pkg_set_progress(cancel_transfer, &calls);
    expect(c1pkg_fetch("http://www.fwz233.com/c1/v2/app.tar.gz", output, 4096U, error, sizeof(error)) != 0 &&
           errno == ECANCELED && logged_requests("fallback") == 0U,
           "package cancellation never contacts fallback");
    c1pkg_set_progress(NULL, NULL);
    server_stop(first);
    server_stop(second);
    index_text(text, sizeof(text), 2, 19U, "Old Author");
    second = server_start(fallback, sizeof(fallback), 0, text);
    expect(setenv("C1PKG_TEST_PRIMARY", "dns-error", 1) == 0 &&
           setenv("C1PKG_TEST_FALLBACK", fallback, 1) == 0, "configure rollback fallback");
    expect(c1pkg_repo_refresh(&config, &index, error, sizeof(error)) != 0 && strstr(error, "rollback") != NULL &&
           memcmp(&index, &saved, sizeof(index)) == 0, "valid fallback signature cannot bypass sequence floor");
    server_stop(second);
    second = server_start(fallback, sizeof(fallback), 2, text);
    expect(setenv("C1PKG_TEST_FALLBACK", fallback, 1) == 0, "configure invalid fallback signature");
    expect(c1pkg_repo_refresh(&config, &index, error, sizeof(error)) != 0 && memcmp(&index, &saved, sizeof(index)) == 0,
           "invalid fallback signature never publishes content");
    server_stop(second);
    config.repo_base = "http://www.fwz233.com.evil/c1/v2";
    clear_log();
    expect(c1pkg_repo_refresh(&config, &index, error, sizeof(error)) != 0 && logged_requests("fallback") == 0U,
           "spoofed repository is not silently redirected to fixed IP");
    clear_log();
    expect(c1pkg_fetch("https://custom.example/app.tar.gz", output, 4096U, error, sizeof(error)) != 0 &&
           logged_requests("fallback") == 0U, "custom HTTPS keeps its redirect policy without implicit IP fallback");
}

int main(void)
{
    unsigned char seed[32] = {1};
    char cwd[C1PKG_PATH_MAX], shim[C1PKG_PATH_MAX + 128U];
    /* Never delete a directory belonging to another test invocation. */
    if (mkdir(C1PKG_STATE_ROOT, 0700) != 0) { perror("create isolated test directory"); return 1; }
    expect(getcwd(cwd, sizeof(cwd)) != NULL, "locate test curl shim");
    (void)snprintf(shim, sizeof(shim), "#!/bin/sh\nexec python3 '%s/tests/pkg_curl_shim.py' \"$@\"\n", cwd);
    write_text(C1PKG_STATE_ROOT "/curl-shim", shim);
    expect(chmod(C1PKG_STATE_ROOT "/curl-shim", 0700) == 0 &&
           setenv("C1PKG_TEST_CURL_LOG", C1PKG_STATE_ROOT "/curl.log", 1) == 0, "install test-only curl shim");
    ed25519_create_keypair(public_key, private_key, seed);
    expect(c1pkg_write_file(C1PKG_STATE_ROOT "/key", public_key, sizeof(public_key), 0600, NULL, 0U) == 0, "write trusted test key");
    test_parser();
    test_config_headers();
    test_cooldown_headers();
    test_network_cache();
    test_fixed_fallback();
    expect(c1pkg_remove_tree(C1PKG_STATE_ROOT, NULL, 0U) == 0, "remove isolated test directory");
    if (failures != 0) { fprintf(stderr, "%d repository test(s) failed\n", failures); return 1; }
    puts("All package repository tests passed.");
    return 0;
}
