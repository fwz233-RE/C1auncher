#include "update/update.h"
#include "update/boot.h"
#include "update/slot.h"
#include "update/supervise.h"
#include "update/supervise_policy.h"
#include "update/io.h"
#include "update/repository.h"
#include "update/transaction.h"
#include "security/sha256.h"
#include "security/secure_file.h"
#include "security/trusted_ed25519.h"
#include "platform/update_request.h"
#include "platform/repository_endpoint.h"
#include "ed25519.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int failures;

/* Exercise the real CLI dispatcher with isolated paths and bounded input.
 * Only its network entry points are redirected to the signed local fixture. */
static const char *cli_state_root, *cli_core_root, *cli_staging_root, *cli_key;
static const char *cli_request_path, *cli_release_dir;
static int cli_repository_reads, cli_prepare_calls, cli_input_reads, cli_input = 'q';
static int cli_prepare(const struct c1_update_transaction_config *config, const char *url,
                       struct c1_update_transaction_result *result, char *error, size_t size)
{
    (void)url;
    ++cli_prepare_calls;
    return c1_update_prepare_local(config, cli_release_dir, result, error, size);
}
static int cli_read_url(const char *path, char *url, size_t size, char *error, size_t error_size)
{
    (void)path; (void)error; (void)error_size;
    ++cli_repository_reads;
    return snprintf(url, size, "http://unused.invalid") > 0 ? 0 : -1;
}
static int cli_getchar(void)
{
    ++cli_input_reads;
    return cli_input;
}
#undef C1_UPDATE_DEFAULT_STATE_ROOT
#undef C1_UPDATE_DEFAULT_CORE_ROOT
#undef C1_UPDATE_DEFAULT_STAGING_ROOT
#undef C1_UPDATE_DEFAULT_KEY
#undef C1_UPDATE_REQUEST_DEFAULT_PATH
#define C1_UPDATE_DEFAULT_STATE_ROOT cli_state_root
#define C1_UPDATE_DEFAULT_CORE_ROOT cli_core_root
#define C1_UPDATE_DEFAULT_STAGING_ROOT cli_staging_root
#define C1_UPDATE_DEFAULT_KEY cli_key
#define C1_UPDATE_REQUEST_DEFAULT_PATH cli_request_path
#define c1_update_prepare cli_prepare
#define c1_update_repository_read_url cli_read_url
#define getchar cli_getchar
#define main c1updater_program_main
#include "../src/update/main.c"
#undef main
#undef getchar
#undef c1_update_repository_read_url
#undef c1_update_prepare

static const char valid_manifest[] =
    "C1CORE-MANIFEST 1\n"
    "S\t42\n"
    "V\t1.2.3\n"
    "E\t7\n"
    "T\tmips32r2-little-o32-hard-float-double-static\n"
    "B\t1.0.0\n"
    "U\t1.0.0\n"
    "C\tc1-core-v1\n"
    "R\tabcdef012345\n"
    "D\t1700000000\n"
    "F\tc1ancher\tartifacts/C1ancher\t0000000000000000000000000000000000000000000000000000000000000001\t100\t700\n"
    "F\tc1pkg\tartifacts/c1pkg\t0000000000000000000000000000000000000000000000000000000000000002\t200\t700\n"
    "F\tlauncher\tartifacts/C1ancher-launcher\t0000000000000000000000000000000000000000000000000000000000000003\t300\t700\n"
    "F\tupdater\tartifacts/c1updater\t0000000000000000000000000000000000000000000000000000000000000004\t400\t700\n";

static void expect(bool condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

static int parse_text(const unsigned char *data, size_t size)
{
    struct c1_update_manifest manifest;
    char error[C1_UPDATE_ERROR_MAX] = "";

    return c1_update_parse_manifest(data, size, &manifest, error, sizeof(error));
}

static void reject_replacement(const char *needle, const char *replacement,
                               const char *message)
{
    char copy[sizeof(valid_manifest) + C1_UPDATE_LINE_MAX];
    char *where;
    size_t prefix;

    (void)strcpy(copy, valid_manifest);
    where = strstr(copy, needle);
    if (where == NULL) {
        expect(false, "test replacement exists");
        return;
    }
    prefix = (size_t)(where - copy);
    (void)memmove(copy + prefix + strlen(replacement), where + strlen(needle),
                  strlen(where + strlen(needle)) + 1U);
    (void)memcpy(copy + prefix, replacement, strlen(replacement));
    expect(parse_text((const unsigned char *)copy, strlen(copy)) != 0, message);
}

static void test_manifest(void)
{
    struct c1_update_manifest manifest;
    unsigned char binary[sizeof(valid_manifest)];
    char error[C1_UPDATE_ERROR_MAX] = "";

    expect(c1_update_parse_manifest((const unsigned char *)valid_manifest,
                                    strlen(valid_manifest), &manifest,
                                    error, sizeof(error)) == 0,
           "valid strict manifest parses");
    expect(manifest.sequence == 42U && manifest.security_epoch == 7U &&
               manifest.source_date_epoch == 1700000000U,
           "manifest numeric headers are retained");
    expect(strcmp(manifest.components[0].role, "c1ancher") == 0 &&
               strcmp(manifest.components[1].role, "c1pkg") == 0 &&
               strcmp(manifest.components[2].role, "launcher") == 0 &&
               strcmp(manifest.components[3].role, "updater") == 0,
           "all four sorted roles are retained");
    expect(manifest.components[0].mode == 0700U && manifest.components[3].size == 400U,
           "component mode and size are retained");
    reject_replacement("\t100\t700", "\t33554433\t700",
                       "oversized component is rejected");
    reject_replacement("\t100\t700", "\t18446744073709551615\t700",
                       "payload size overflow is rejected");

    expect(parse_text((const unsigned char *)valid_manifest,
                      strlen(valid_manifest) - 1U) != 0,
           "missing final LF is rejected");
    (void)memcpy(binary, valid_manifest, sizeof(valid_manifest));
    binary[5] = 0U;
    expect(parse_text(binary, strlen(valid_manifest)) != 0, "NUL is rejected");
    (void)memcpy(binary, valid_manifest, sizeof(valid_manifest));
    binary[5] = (unsigned char)'\r';
    expect(parse_text(binary, strlen(valid_manifest)) != 0, "CR is rejected");
    (void)memcpy(binary, valid_manifest, sizeof(valid_manifest));
    binary[5] = 0x80U;
    expect(parse_text(binary, strlen(valid_manifest)) != 0, "non-ASCII is rejected");
    {
        unsigned char long_line[C1_UPDATE_LINE_MAX + 2U];
        (void)memset(long_line, 'A', sizeof(long_line));
        long_line[sizeof(long_line) - 1U] = (unsigned char)'\n';
        expect(parse_text(long_line, sizeof(long_line)) != 0, "overlong line is rejected");
    }
    {
        size_t oversized_size = C1_UPDATE_MANIFEST_MAX + 1U;
        unsigned char *oversized = malloc(oversized_size);
        expect(oversized != NULL, "oversized manifest fixture allocates");
        if (oversized != NULL) {
            (void)memset(oversized, 'A', oversized_size);
            oversized[oversized_size - 1U] = (unsigned char)'\n';
            expect(parse_text(oversized, oversized_size) != 0,
                   "oversized manifest is rejected");
            free(oversized);
        }
    }

    reject_replacement("C1CORE-MANIFEST 1", "C1CORE-MANIFEST 2", "unknown header is rejected");
    reject_replacement("S\t42", "X\t42", "unknown field is rejected");
    reject_replacement("S\t42\nV\t1.2.3", "V\t1.2.3\nS\t42", "field reordering is rejected");
    reject_replacement("V\t1.2.3", "S\t43", "duplicate field is rejected");
    reject_replacement("S\t42", "S\t042", "noncanonical uint64 is rejected");
    reject_replacement("S\t42", "S\t18446744073709551616", "uint64 overflow is rejected");
    reject_replacement("E\t7", "E\t0", "zero security epoch is rejected");
    reject_replacement("D\t1700000000", "D\t0", "zero source date epoch is rejected");
    reject_replacement("V\t1.2.3", "V\tbad value", "unsafe token is rejected");
    reject_replacement(C1_UPDATE_TARGET, "mips64", "wrong target is rejected");
    reject_replacement("F\tc1ancher", "F\tc1pkg", "duplicate or unsorted role is rejected");
    reject_replacement("artifacts/C1ancher\t", "artifacts/wrong\t", "wrong role path is rejected");
    reject_replacement("\t100\t700", "\t100\t755", "wrong component mode is rejected");
    reject_replacement("0000000000000000000000000000000000000000000000000000000000000001",
                       "000000000000000000000000000000000000000000000000000000000000000A",
                       "uppercase digest is rejected");
    reject_replacement("\t100\t700", "\t0\t700", "zero component size is rejected");
    reject_replacement("F\tupdater\tartifacts/c1updater\t0000000000000000000000000000000000000000000000000000000000000004\t400\t700\n",
                       "", "missing component is rejected");
    reject_replacement("\t400\t700\n", "\t400\t700\nF\textra\tx\t0000000000000000000000000000000000000000000000000000000000000005\t1\t700\n",
                       "extra component is rejected");
}

static void fill_initial(struct c1_update_state *state)
{
    (void)memset(state, 0, sizeof(*state));
    state->phase = C1_UPDATE_IDLE;
}

static void test_transitions(void)
{
    static const char digest_a[] = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    static const char digest_b[] = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
    struct c1_update_state states[7];
    char error[C1_UPDATE_ERROR_MAX] = "";
    size_t i;

    fill_initial(&states[0]);
    for (i = 1U; i <= 5U; ++i) {
        expect(c1_update_transition(&states[i - 1U], (enum c1_update_phase)i,
                                    5U, 2U, "1.0.0", digest_a, 0, &states[i],
                                    error, sizeof(error)) == 0,
               "adjacent forward transition is accepted");
    }
    expect(c1_update_transition(&states[5], C1_UPDATE_DOWNLOADING, 6U, 2U, "1.1.0", digest_b,
                                0, &states[6], error, sizeof(error)) == 0,
           "confirmed state may begin a newer update transaction");
    expect(c1_update_transition(&states[4], C1_UPDATE_PREPARED, 5U, 2U, "1.0.0", digest_a,
                                0, &states[6], error, sizeof(error)) == 0,
           "pending boot may roll back to prepared");
    expect(c1_update_transition(&states[2], C1_UPDATE_IDLE, 5U, 2U, "1.0.0", digest_a,
                                1, &states[6], error, sizeof(error)) == 0 &&
               states[6].phase == C1_UPDATE_IDLE,
           "non-idle failure cleans up to idle");
    expect(c1_update_transition(&states[0], C1_UPDATE_VERIFIED, 5U, 2U, "1.0.0", digest_a,
                                0, &states[6], error, sizeof(error)) != 0,
           "phase skipping is rejected");
    expect(c1_update_transition(&states[2], C1_UPDATE_PREPARED, 4U, 2U, "0.9.0", digest_a,
                                0, &states[6], error, sizeof(error)) != 0,
           "sequence rollback is rejected");
    expect(c1_update_transition(&states[2], C1_UPDATE_PREPARED, 6U, 1U, "1.1.0", digest_a,
                                0, &states[6], error, sizeof(error)) != 0,
           "security epoch rollback is rejected");
    expect(c1_update_transition(&states[2], C1_UPDATE_PREPARED, 5U, 2U, "1.0.0", digest_b,
                                0, &states[6], error, sizeof(error)) != 0,
           "same sequence with different digest is rejected");
    expect(c1_update_transition(&states[0], C1_UPDATE_IDLE, 5U, 2U, "1.0.0", digest_a,
                                1, &states[6], error, sizeof(error)) != 0,
           "idle failure transition is rejected");
}

static int write_bytes(const char *path, const unsigned char *data, size_t size, mode_t mode)
{
    size_t written = 0U;
    int descriptor = open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);

    if (descriptor < 0 || fchmod(descriptor, mode) != 0) {
        if (descriptor >= 0) {
            (void)close(descriptor);
        }
        return -1;
    }
    while (written < size) {
        ssize_t amount = write(descriptor, data + written, size - written);
        if (amount <= 0) {
            (void)close(descriptor);
            return -1;
        }
        written += (size_t)amount;
    }
    return close(descriptor);
}

static void test_sha256(void)
{
    static const char empty_expected[] =
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
    static const char abc_expected[] =
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
    unsigned char digest[C1_SHA256_SIZE];
    char hex[C1_SHA256_HEX_SIZE];
    char root[] = "/tmp/c1update-sha-XXXXXX";
    char file[C1_UPDATE_PATH_MAX];
    char link_path[C1_UPDATE_PATH_MAX];
    uint64_t size = 0U;
    char error[C1_UPDATE_ERROR_MAX] = "";

    c1_sha256(NULL, 0U, digest);
    c1_sha256_hex(digest, hex);
    expect(strcmp(hex, empty_expected) == 0, "SHA-256 empty vector matches");
    c1_sha256("abc", 3U, digest);
    c1_sha256_hex(digest, hex);
    expect(strcmp(hex, abc_expected) == 0, "SHA-256 abc vector matches");
    expect(mkdtemp(root) != NULL, "SHA-256 temporary root is created");
    (void)snprintf(file, sizeof(file), "%s/file", root);
    (void)snprintf(link_path, sizeof(link_path), "%s/link", root);
    expect(write_bytes(file, (const unsigned char *)"abc", 3U, 0600) == 0 &&
               c1_sha256_file(file, 3U, 3U, digest, &size, error, sizeof(error)) == 0 &&
               size == 3U,
           "secure file SHA-256 succeeds");
    c1_sha256_hex(digest, hex);
    expect(strcmp(hex, abc_expected) == 0, "secure file SHA-256 matches vector");
    expect(symlink(file, link_path) == 0 &&
               c1_sha256_file(link_path, 3U, 3U, digest, NULL, error, sizeof(error)) != 0,
           "secure file SHA-256 rejects symlink");
    (void)unlink(link_path);
    expect(link(file, link_path) == 0 &&
               c1_sha256_file(file, 3U, 3U, digest, NULL, error, sizeof(error)) != 0,
           "secure file SHA-256 rejects multiple links");
    (void)unlink(link_path);
    (void)unlink(file);
    (void)rmdir(root);
}

static void test_update_lock(void)
{
    char root[] = "/tmp/c1update-lock-XXXXXX";
    char lock_path[C1_UPDATE_PATH_MAX] = "";
    char second_path[C1_UPDATE_PATH_MAX] = "";
    char error[C1_UPDATE_ERROR_MAX] = "";
    int descriptor, second, status;
    pid_t child;

    expect(mkdtemp(root) != NULL, "lock temporary root is created");
    descriptor = c1_update_lock(root, lock_path, sizeof(lock_path), error, sizeof(error));
    expect(descriptor >= 0, "kernel-backed update lock is acquired");
    child = fork();
    if (child == 0) {
        int contender = c1_update_lock(root, second_path, sizeof(second_path), error, sizeof(error));
        if (contender >= 0) c1_update_unlock(contender, second_path);
        if (contender != C1_UPDATE_BUSY ||
            c1_update_recover(root, root, "/missing-test-key", error, sizeof(error)) != C1_UPDATE_BUSY)
            _exit(1);
        {
            char launcher[C1_UPDATE_PATH_MAX];
            (void)snprintf(launcher, sizeof(launcher), "%s/current/artifacts/C1ancher-launcher", root);
            if (c1_update_supervise(root, root, "/missing-test-key", "/missing-test-ready", launcher,
                                    error, sizeof(error)) != C1_UPDATER_TRANSIENT_EXIT) _exit(1);
        }
        _exit(0);
    }
    expect(child > 0 && waitpid(child, &status, 0) == child && WIFEXITED(status) &&
               WEXITSTATUS(status) == 0,
           "concurrent process cannot acquire update lock");
    c1_update_unlock(descriptor, lock_path);
    expect(access(lock_path, F_OK) == 0,
           "persistent lock inode remains after owner exits");
    second = c1_update_lock(root, second_path, sizeof(second_path), error, sizeof(error));
    expect(second >= 0, "stale lock path does not block a later process");
    c1_update_unlock(second, second_path);
    expect(unlink(lock_path) == 0 && symlink("missing", lock_path) == 0 &&
               c1_update_lock(root, second_path, sizeof(second_path), error, sizeof(error)) < 0,
           "update lock rejects a symlink inode");
    (void)unlink(lock_path);
    (void)rmdir(root);
}

static void test_repository_url(void)
{
    char error[C1_UPDATE_ERROR_MAX] = "";
    char fallback[C1_UPDATE_URL_MAX + 1U];
    static const char *const unmapped[] = {
        "https://www.fwz233.com/c1/core/v1/stable",
        "http://123.56.214.77/c1/core/v1/stable",
        "http://www.fwz233.com.attacker/c1/core/v1/stable",
        "http://www.fwz233.com@attacker/c1/core/v1/stable",
        "http://www.fwz233.com:8080/c1/core/v1/stable",
        "http://custom.example/c1/core/v1/stable"
    };
    size_t i;
    expect(c1_repository_fallback_url(C1_UPDATE_DEFAULT_REPOSITORY, fallback, sizeof(fallback)) == 1 &&
               strcmp(fallback, "http://123.56.214.77/c1/core/v1/stable") == 0,
           "default HTTP domain maps to the fixed IP with the same path");
    expect(c1_repository_fallback_url("http://www.fwz233.com:80/c1/core/v1/canary/", fallback,
                                       sizeof(fallback)) == 1 &&
               strcmp(fallback, "http://123.56.214.77/c1/core/v1/canary/") == 0,
           "explicit HTTP port and custom channel preserve fallback path");
    for (i = 0U; i < sizeof(unmapped) / sizeof(unmapped[0]); ++i)
        expect(c1_repository_fallback_url(unmapped[i], fallback, sizeof(fallback)) == 0,
               "unrelated origins and fixed IP cannot trigger implicit fallback");
    expect(c1_repository_fallback_url(C1_UPDATE_DEFAULT_REPOSITORY, fallback, 8U) == 0,
           "fallback rejects undersized output");
    expect(c1_update_repository_validate_url("https://updates.example/core", error, sizeof(error)) == 0,
           "strict HTTPS repository URL is accepted");
    expect(c1_update_repository_validate_url("http://127.0.0.1:8080/releases", error, sizeof(error)) == 0,
           "strict HTTP repository URL is accepted");
    expect(c1_update_repository_validate_url("file:///tmp/release", error, sizeof(error)) != 0,
           "non-HTTP repository URL is rejected");
    expect(c1_update_repository_validate_url("https://user@example/core", error, sizeof(error)) != 0,
           "repository userinfo is rejected");
    expect(c1_update_repository_validate_url("https://example/core#fragment", error, sizeof(error)) != 0,
           "repository fragment is rejected");
    expect(c1_update_repository_validate_url("https://example\\core", error, sizeof(error)) != 0,
           "repository backslash is rejected");
}

static void test_repository_config(void)
{
    static const char url[] = "https://updates.example/c1/core/v1/canary";
    char root[] = "/tmp/c1update-repository-config-XXXXXX";
    char path[C1_UPDATE_PATH_MAX], link_path[C1_UPDATE_PATH_MAX];
    char loaded[C1_UPDATE_URL_MAX + 1U], error[C1_UPDATE_ERROR_MAX] = "";
    char text[C1_UPDATE_URL_MAX + 3U];
    expect(mkdtemp(root) != NULL, "repository config fixture created");
    (void)snprintf(path, sizeof(path), "%s/repository.url", root);
    (void)snprintf(link_path, sizeof(link_path), "%s/link", root);
    expect(c1_update_repository_read_url(path, loaded, sizeof(loaded), error, sizeof(error)) == 0 &&
               strcmp(loaded, C1_UPDATE_DEFAULT_REPOSITORY) == 0,
           "missing core profile selects domain-first stable HTTP default");
    expect(c1_update_repository_read_url(path, loaded, 8U, error, sizeof(error)) != 0 && loaded[0] == '\0',
           "missing profile still rejects undersized URL output");
    (void)snprintf(text, sizeof(text), "%s\r\n", url);
    expect(write_bytes(path, (const unsigned char *)text, strlen(text), 0600) == 0 &&
               c1_update_repository_read_url(path, loaded, sizeof(loaded), error, sizeof(error)) == 0 &&
               strcmp(loaded, url) == 0,
           "core profile supports a configured canary endpoint and CRLF");
    expect(c1_update_repository_read_url(path, loaded, 8U, error, sizeof(error)) != 0,
           "undersized URL output is rejected");
    expect(symlink(path, link_path) == 0 &&
               c1_update_repository_read_url(link_path, loaded, sizeof(loaded), error, sizeof(error)) != 0,
           "core profile rejects symlinks");
    (void)unlink(link_path);
    expect(chmod(path, 0666) == 0 &&
               c1_update_repository_read_url(path, loaded, sizeof(loaded), error, sizeof(error)) != 0,
           "core profile rejects writable-by-others configuration");
    expect(write_bytes(path, (const unsigned char *)"http://example\nhttp://other\n", 28U, 0600) == 0 &&
               c1_update_repository_read_url(path, loaded, sizeof(loaded), error, sizeof(error)) != 0,
           "core profile rejects multiple lines");
    expect(write_bytes(path, (const unsigned char *)"http://example\0bad", 18U, 0600) == 0 &&
               c1_update_repository_read_url(path, loaded, sizeof(loaded), error, sizeof(error)) != 0,
           "core profile rejects embedded NULs");
    expect(write_bytes(path, (const unsigned char *)"", 0U, 0600) == 0 &&
               c1_update_repository_read_url(path, loaded, sizeof(loaded), error, sizeof(error)) != 0,
           "empty core profile fails explicitly");
    (void)memset(text, 'a', sizeof(text));
    expect(write_bytes(path, (const unsigned char *)text, sizeof(text), 0600) == 0 &&
               c1_update_repository_read_url(path, loaded, sizeof(loaded), error, sizeof(error)) != 0,
           "oversized core profile is rejected");
    (void)unlink(path);
    (void)rmdir(root);
}

static int make_release_fixture(const char *release_dir, const char *key_path,
                                uint64_t sequence, const char *version,
                                unsigned char first_byte)
{
    static const char *const names[C1_UPDATE_COMPONENT_COUNT] = {
        "C1ancher", "c1pkg", "C1ancher-launcher", "c1updater"
    };
    unsigned char seed[32], public_key[32], private_key[64], signature[64];
    unsigned char payloads[C1_UPDATE_COMPONENT_COUNT][128];
    size_t sizes[C1_UPDATE_COMPONENT_COUNT] = {3U, 4U, 5U, 6U};
    char artifacts[C1_UPDATE_PATH_MAX], path[C1_UPDATE_PATH_MAX];
    char hashes[C1_UPDATE_COMPONENT_COUNT][C1_SHA256_HEX_SIZE];
    char manifest[4096];
    unsigned char digest[C1_SHA256_SIZE];
    int length;
    size_t i;

    (void)memset(seed, 7, sizeof(seed));
    ed25519_create_keypair(public_key, private_key, seed);
    if (mkdir(release_dir, 0700) != 0 ||
        snprintf(artifacts, sizeof(artifacts), "%s/artifacts", release_dir) < 0 ||
        mkdir(artifacts, 0700) != 0 || write_bytes(key_path, public_key, sizeof(public_key), 0600) != 0) return -1;
    for (i = 0U; i < C1_UPDATE_COMPONENT_COUNT; ++i) {
        size_t j;
        if (i == 3U &&
            (first_byte == 100U || first_byte == 101U || first_byte == 102U ||
             first_byte == 103U)) {
            char descendant_script[128];
            const char *script;

            if (first_byte == 100U) script = "#!/bin/sh\nexit 0\n";
            else if (first_byte == 101U) script = "#!/bin/sh\nexit 1\n";
            else if (first_byte == 102U) script = "#!/bin/sh\nsleep 10\n";
            else {
                length = snprintf(descendant_script, sizeof(descendant_script),
                                  "#!/bin/sh\nsleep 30 &\necho $! >%s\nexit 1\n",
                                  "/tmp/c1update-self-test-child");
                if (length <= 0 || (size_t)length >= sizeof(descendant_script)) return -1;
                script = descendant_script;
            }
            sizes[i] = strlen(script);
            (void)memcpy(payloads[i], script, sizes[i]);
        } else {
            for (j = 0U; j < sizes[i]; ++j) payloads[i][j] = (unsigned char)(first_byte + i + j);
        }
        c1_sha256(payloads[i], sizes[i], digest);
        c1_sha256_hex(digest, hashes[i]);
        if (snprintf(path, sizeof(path), "%s/%s", artifacts, names[i]) < 0 ||
            write_bytes(path, payloads[i], sizes[i], 0600) != 0) return -1;
    }
    length = snprintf(manifest, sizeof(manifest),
        "C1CORE-MANIFEST 1\nS\t%llu\nV\t%s\nE\t7\nT\t%s\nB\t1.0.0\nU\t1.0.0\nC\tc1-core-v1\nR\tabcdef012345\nD\t1700000000\n"
        "F\tc1ancher\tartifacts/C1ancher\t%s\t%llu\t700\n"
        "F\tc1pkg\tartifacts/c1pkg\t%s\t%llu\t700\n"
        "F\tlauncher\tartifacts/C1ancher-launcher\t%s\t%llu\t700\n"
        "F\tupdater\tartifacts/c1updater\t%s\t%llu\t700\n",
        (unsigned long long)sequence, version, C1_UPDATE_TARGET,
        hashes[0], (unsigned long long)sizes[0], hashes[1], (unsigned long long)sizes[1],
        hashes[2], (unsigned long long)sizes[2], hashes[3], (unsigned long long)sizes[3]);
    if (length <= 0 || (size_t)length >= sizeof(manifest)) return -1;
    ed25519_sign(signature, (const unsigned char *)manifest, (size_t)length, public_key, private_key);
    if (snprintf(path, sizeof(path), "%s/manifest.v1", release_dir) < 0 ||
        write_bytes(path, (const unsigned char *)manifest, (size_t)length, 0600) != 0 ||
        snprintf(path, sizeof(path), "%s/manifest.v1.sig", release_dir) < 0 ||
        write_bytes(path, signature, sizeof(signature), 0600) != 0) return -1;
    return 0;
}

/* Hash contents, symlink targets and mutation-sensitive metadata. Reads may
 * change atime; the existing persistent lock may be chmod'ed on acquisition. */
static int snapshot_tree(const char *path, struct c1_sha256_context *hash)
{
    struct stat info;
    unsigned char digest[C1_SHA256_SIZE];
    char error[C1_UPDATE_ERROR_MAX] = "", target[C1_UPDATE_PATH_MAX];
    if (lstat(path, &info) != 0) return -1;
    c1_sha256_update(hash, path, strlen(path));
    c1_sha256_update(hash, &info.st_ino, sizeof(info.st_ino));
    c1_sha256_update(hash, &info.st_mode, sizeof(info.st_mode));
    c1_sha256_update(hash, &info.st_size, sizeof(info.st_size));
    c1_sha256_update(hash, &info.st_mtim.tv_sec, sizeof(info.st_mtim.tv_sec));
    c1_sha256_update(hash, &info.st_mtim.tv_nsec, sizeof(info.st_mtim.tv_nsec));
    c1_sha256_update(hash, &info.st_ctim.tv_sec, sizeof(info.st_ctim.tv_sec));
    c1_sha256_update(hash, &info.st_ctim.tv_nsec, sizeof(info.st_ctim.tv_nsec));
    if (S_ISREG(info.st_mode)) {
        if (c1_sha256_file(path, (uint64_t)info.st_size, (uint64_t)info.st_size,
                           digest, NULL, error, sizeof(error)) != 0) return -1;
        c1_sha256_update(hash, digest, sizeof(digest));
    } else if (S_ISLNK(info.st_mode)) {
        ssize_t size = readlink(path, target, sizeof(target));
        if (size < 0 || (size_t)size >= sizeof(target)) return -1;
        c1_sha256_update(hash, target, (size_t)size);
    } else if (S_ISDIR(info.st_mode)) {
        struct dirent **entries;
        int count = scandir(path, &entries, NULL, alphasort), i, rc = 0;
        if (count < 0) return -1;
        for (i = 0; i < count; ++i) {
            const char *name = entries[i]->d_name;
            if (strcmp(name, ".") != 0 && strcmp(name, "..") != 0 &&
                strcmp(name, ".c1updater.lock") != 0 &&
                (c1_update_join_path(target, sizeof(target), path, name) != 0 ||
                 snapshot_tree(target, hash) != 0)) rc = -1;
            free(entries[i]);
        }
        free(entries);
        if (rc != 0) return rc;
    } else return -1;
    return 0;
}

static int transaction_snapshot(const struct c1_update_transaction_config *config,
                                unsigned char digest[C1_SHA256_SIZE])
{
    struct c1_sha256_context hash;
    c1_sha256_init(&hash);
    if (snapshot_tree(config->state_root, &hash) != 0 ||
        snapshot_tree(config->core_root, &hash) != 0) return -1;
    c1_sha256_final(&hash, digest);
    return 0;
}

static int prepare_unchanged(const struct c1_update_transaction_config *config,
                             const char *release_dir,
                             struct c1_update_transaction_result *result,
                             char *error, size_t size)
{
    unsigned char before[C1_SHA256_SIZE], after[C1_SHA256_SIZE];
    int rc;
    expect(transaction_snapshot(config, before) == 0, "transaction snapshot before prepare succeeds");
    rc = c1_update_prepare_local(config, release_dir, result, error, size);
    expect(transaction_snapshot(config, after) == 0 && memcmp(before, after, sizeof(before)) == 0,
           "prepare preserves all state generations, pointers and immutable release bytes/metadata");
    return rc;
}

static int resign_fixture_field(const char *release_dir, const char *needle, const char *replacement)
{
    unsigned char *data = NULL, seed[32], public_key[32], private_key[64], signature[64];
    size_t size;
    char path[C1_UPDATE_PATH_MAX], error[C1_UPDATE_ERROR_MAX] = "";
    unsigned char *where;
    int rc = -1;
    if (strlen(needle) != strlen(replacement) ||
        c1_update_join_path(path, sizeof(path), release_dir, C1_UPDATE_MANIFEST_NAME) != 0 ||
        c1_secure_read_file(path, &data, &size, C1_UPDATE_MANIFEST_MAX,
                            C1_SECURE_FILE_ANY_SIZE, error, sizeof(error)) != 0) return -1;
    where = (unsigned char *)strstr((const char *)data, needle);
    if (where == NULL) goto done;
    (void)memcpy(where, replacement, strlen(replacement));
    (void)memset(seed, 7, sizeof(seed));
    ed25519_create_keypair(public_key, private_key, seed);
    ed25519_sign(signature, data, size, public_key, private_key);
    if (write_bytes(path, data, size, 0600) != 0 ||
        c1_update_join_path(path, sizeof(path), release_dir, C1_UPDATE_SIGNATURE_NAME) != 0 ||
        write_bytes(path, signature, sizeof(signature), 0600) != 0) goto done;
    rc = 0;
done:
    free(data);
    return rc;
}

static int reject_space(const char *path, uint64_t required, void *context,
                        char *error, size_t error_size)
{
    (void)path;
    (void)required;
    (void)context;
    (void)snprintf(error, error_size, "injected space failure");
    return -1;
}

static void test_copy_preserves_destination(void)
{
    char root[] = "/tmp/c1update-copy-XXXXXX";
    char source[C1_UPDATE_PATH_MAX], destination[C1_UPDATE_PATH_MAX];
    char error[C1_UPDATE_ERROR_MAX] = "";
    unsigned char *data = NULL;
    size_t size = 0U;

    expect(mkdtemp(root) != NULL, "copy temporary root is created");
    (void)snprintf(source, sizeof(source), "%s/source", root);
    (void)snprintf(destination, sizeof(destination), "%s/destination", root);
    expect(write_bytes(source, (const unsigned char *)"abc", 3U, 0600) == 0 &&
               write_bytes(destination, (const unsigned char *)"old", 3U, 0700) == 0 &&
               c1_update_copy_file(source, destination, 3U,
                   "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
                   0700, error, sizeof(error)) != 0 &&
               c1_secure_read_file(destination, &data, &size, 3U, 3U,
                                    error, sizeof(error)) == 0 &&
               memcmp(data, "old", 3U) == 0,
           "exclusive copy failure preserves an existing destination");
    free(data);
    (void)c1_update_remove_tree(root);
}

static void test_verified_resume(void)
{
    char root[] = "/tmp/c1update-resume-XXXXXX";
    char release_dir[C1_UPDATE_PATH_MAX], staging[C1_UPDATE_PATH_MAX];
    char core[C1_UPDATE_PATH_MAX], state[C1_UPDATE_PATH_MAX], key[C1_UPDATE_PATH_MAX];
    char orphan[C1_UPDATE_PATH_MAX], draft[C1_UPDATE_PATH_MAX];
    struct c1_update_state initial, downloading, verified, loaded;
    struct c1_update_release release;
    unsigned char hash[C1_SHA256_SIZE];
    char digest[C1_SHA256_HEX_SIZE], error[C1_UPDATE_ERROR_MAX] = "";
    struct c1_update_transaction_config config;
    struct c1_update_transaction_result result;

    expect(mkdtemp(root) != NULL, "resume temporary root is created");
    (void)snprintf(release_dir, sizeof(release_dir), "%s/release", root);
    (void)snprintf(staging, sizeof(staging), "%s/staging", root);
    (void)snprintf(core, sizeof(core), "%s/core", root);
    (void)snprintf(state, sizeof(state), "%s/state", root);
    (void)snprintf(key, sizeof(key), "%s/key", root);
    (void)memset(&release, 0, sizeof(release));
    expect(mkdir(staging, 0700) == 0 && mkdir(core, 0700) == 0 && mkdir(state, 0700) == 0 &&
               make_release_fixture(release_dir, key, 42U, "1.2.3", 1U) == 0 &&
               c1_update_repository_load_local(release_dir, key, &release, error, sizeof(error)) == 0,
           "resume signed fixture is loaded");
    c1_sha256(release.manifest_data, release.manifest_size, hash);
    c1_sha256_hex(hash, digest);
    c1_update_repository_release_free(&release);
    fill_initial(&initial);
    expect(c1_update_transition(&initial, C1_UPDATE_DOWNLOADING, 42U, 7U, "1.2.3", digest,
                                0, &downloading, error, sizeof(error)) == 0 &&
               c1_update_state_commit(state, &initial, &downloading, error, sizeof(error)) == 0 &&
               c1_update_transition(&downloading, C1_UPDATE_VERIFIED, 42U, 7U, "1.2.3", digest,
                                    0, &verified, error, sizeof(error)) == 0 &&
               c1_update_state_commit(state, &downloading, &verified, error, sizeof(error)) == 0,
           "interrupted verified state is persisted without staging payloads");
    (void)memset(&config, 0, sizeof(config));
    config.staging_root = staging;
    config.core_root = core;
    config.state_root = state;
    config.key_path = key;
    loaded = verified;
    expect(c1_update_prepare_local(&config, release_dir, &result, error, sizeof(error)) == 0 &&
               c1_update_state_load(state, &loaded, error, sizeof(error)) == 0 &&
               loaded.phase == C1_UPDATE_PREPARED,
           "verified update resumes in one attempt after staging loss");
    (void)snprintf(orphan, sizeof(orphan), "%s/staging/.txn.%ld", root, (long)getpid());
    expect(mkdir(orphan, 0700) == 0 &&
               c1_update_prepare_local(&config, release_dir, &result, error, sizeof(error)) == 0,
           "stale transaction directory from reused PID does not block prepare");
    /* An interrupted state draft must not prevent the next commit after PID reuse. */
    (void)snprintf(draft, sizeof(draft), "%s/state/generations/.new.%llu.%ld", root,
                   (unsigned long long)(loaded.generation + 1U), (long)getpid());
    expect(mkdir(draft, 0700) == 0 &&
               c1_update_transition(&loaded, C1_UPDATE_PENDING_BOOT, loaded.sequence,
                                    loaded.security_epoch, loaded.release, loaded.digest,
                                    0, &verified, error, sizeof(error)) == 0 &&
               c1_update_state_commit(state, &loaded, &verified, error, sizeof(error)) == 0,
           "stale state draft from reused PID does not block commit");
    (void)c1_update_remove_tree(root);
}

static void test_transaction(void)
{
    char root[] = "/tmp/c1update-transaction-XXXXXX";
    char release_dir[C1_UPDATE_PATH_MAX], staging[C1_UPDATE_PATH_MAX];
    char core[C1_UPDATE_PATH_MAX], state[C1_UPDATE_PATH_MAX], key[C1_UPDATE_PATH_MAX];
    char artifact[C1_UPDATE_PATH_MAX], final_path[C1_UPDATE_PATH_MAX];
    struct c1_update_transaction_config config;
    struct c1_update_transaction_result result;
    struct c1_update_state loaded;
    char error[C1_UPDATE_ERROR_MAX] = "";
    unsigned char bad[3] = {0xffU, 0xfeU, 0xfdU};

    expect(mkdtemp(root) != NULL, "transaction temporary root is created");
    (void)snprintf(release_dir, sizeof(release_dir), "%s/release", root);
    (void)snprintf(staging, sizeof(staging), "%s/staging", root);
    (void)snprintf(core, sizeof(core), "%s/core", root);
    (void)snprintf(state, sizeof(state), "%s/state", root);
    (void)snprintf(key, sizeof(key), "%s/key", root);
    expect(mkdir(staging, 0700) == 0 && mkdir(core, 0700) == 0 && mkdir(state, 0700) == 0 &&
               make_release_fixture(release_dir, key, 42U, "1.2.3", 1U) == 0,
           "signed local release fixture is created");
    (void)memset(&config, 0, sizeof(config));
    config.staging_root = staging;
    config.core_root = core;
    config.state_root = state;
    config.key_path = key;
    config.space_check = reject_space;
    expect(c1_update_prepare_local(&config, release_dir, &result, error, sizeof(error)) != 0 &&
               c1_update_state_load(state, &loaded, error, sizeof(error)) == 0 &&
               loaded.phase == C1_UPDATE_IDLE && loaded.sequence == 42U &&
               loaded.security_epoch == 7U,
           "injected space failure returns transaction to idle");
    config.space_check = NULL;
    expect(c1_update_join_path(artifact, sizeof(artifact), release_dir,
                               "artifacts/C1ancher") == 0 &&
               write_bytes(artifact, bad, 1U, 0600) == 0 &&
               c1_update_prepare_local(&config, release_dir, &result, error, sizeof(error)) != 0 &&
               c1_update_state_load(state, &loaded, error, sizeof(error)) == 0 &&
               loaded.phase == C1_UPDATE_IDLE && loaded.sequence == 42U &&
               loaded.security_epoch == 7U,
           "bad local artifact size fails and transaction returns to idle");
    expect(write_bytes(artifact, bad, sizeof(bad), 0600) == 0 &&
               c1_update_prepare_local(&config, release_dir, &result, error, sizeof(error)) != 0 &&
               c1_update_state_load(state, &loaded, error, sizeof(error)) == 0 &&
               loaded.phase == C1_UPDATE_IDLE && loaded.sequence == 42U &&
               loaded.security_epoch == 7U,
           "bad local artifact hash fails and transaction returns to idle");
    expect(c1_update_remove_tree(release_dir) == 0 && unlink(key) == 0 &&
               make_release_fixture(release_dir, key, 42U, "1.2.3", 1U) == 0,
           "valid signed local release fixture is restored");
    expect(c1_update_prepare_local(&config, release_dir, &result, error, sizeof(error)) == 0 &&
               result.phase == C1_UPDATE_PREPARED && result.sequence == 42U &&
               c1_update_state_load(state, &loaded, error, sizeof(error)) == 0 &&
               loaded.phase == C1_UPDATE_PREPARED,
           "local release completes downloading verified prepared states");
    expect(c1_update_join_path(final_path, sizeof(final_path), core,
                               "releases/42-1.2.3") == 0 && access(final_path, F_OK) == 0,
           "immutable prepared release is committed");
    expect(c1_update_prepare_local(&config, release_dir, &result, error, sizeof(error)) == 0,
           "repeated local prepare is idempotent");
    expect(c1_update_remove_tree(release_dir) == 0 && unlink(key) == 0 &&
               make_release_fixture(release_dir, key, 42U, "1.2.3", 9U) == 0 &&
               c1_update_prepare_local(&config, release_dir, &result, error, sizeof(error)) != 0,
           "same release with different signed content is rejected");
    expect(c1_update_remove_tree(staging) == 0 && symlink(release_dir, staging) == 0 &&
               c1_update_prepare_local(&config, release_dir, &result, error, sizeof(error)) != 0,
           "symlink staging root is rejected");
    (void)unlink(staging);
    (void)c1_update_remove_tree(release_dir);
    (void)unlink(key);
    (void)c1_update_remove_tree(core);
    (void)c1_update_remove_tree(state);
    (void)rmdir(root);
}

static int replace_symlink(const char *path, const char *target)
{
    (void)unlink(path);
    return symlink(target, path);
}

static int prepare_fixture(const char *release_dir, const char *staging,
                           const char *core, const char *state, const char *key)
{
    struct c1_update_transaction_config config;
    struct c1_update_transaction_result result;
    char error[C1_UPDATE_ERROR_MAX] = "";
    (void)memset(&config, 0, sizeof(config));
    config.staging_root = staging;
    config.core_root = core;
    config.state_root = state;
    config.key_path = key;
    return c1_update_prepare_local(&config, release_dir, &result, error, sizeof(error));
}

static int run_cli(int argc, char **argv, char *output, size_t size)
{
    FILE *capture = tmpfile();
    int saved, rc;
    size_t used;
    if (capture == NULL) return -1;
    (void)fflush(stdout);
    saved = dup(STDOUT_FILENO);
    if (saved < 0 || dup2(fileno(capture), STDOUT_FILENO) < 0) {
        if (saved >= 0) (void)close(saved);
        (void)fclose(capture);
        return -1;
    }
    cli_repository_reads = cli_prepare_calls = cli_input_reads = 0;
    rc = c1updater_program_main(argc, argv);
    (void)fflush(stdout);
    (void)dup2(saved, STDOUT_FILENO);
    (void)close(saved);
    rewind(capture);
    used = fread(output, 1U, size - 1U, capture);
    output[used] = '\0';
    (void)fclose(capture);
    return rc;
}

static void test_confirmed_noop(void)
{
    char root[] = "/tmp/c1update-confirmed-XXXXXX";
    char release_dir[C1_UPDATE_PATH_MAX], staging[C1_UPDATE_PATH_MAX];
    char core[C1_UPDATE_PATH_MAX], state[C1_UPDATE_PATH_MAX], key[C1_UPDATE_PATH_MAX];
    char artifact[C1_UPDATE_PATH_MAX], signature[C1_UPDATE_PATH_MAX], request[C1_UPDATE_PATH_MAX];
    char output[1024], error[C1_UPDATE_ERROR_MAX] = "";
    struct c1_update_transaction_config config;
    struct c1_update_transaction_result result;
    struct c1_update_state confirmed, loaded;
    unsigned char before[C1_SHA256_SIZE], after[C1_SHA256_SIZE], bad_signature[64] = {0};
    unsigned char payload[3] = {1U, 2U, 3U};
    char *prepared_args[] = {"c1updater", "tui-prepared", NULL};
    char *tui_args[] = {"c1updater", "tui", NULL};
    char *prepare_args[] = {"c1updater", "prepare-local", release_dir, staging, core, state, key, NULL};
    size_t i;
    static const struct {
        uint64_t sequence;
        const char *version;
        unsigned char payload;
        const char *needle, *replacement;
    } rejected[] = {
        {41U, "1.2.2", 1U, NULL, NULL},
        {42U, "1.2.4", 1U, NULL, NULL},
        {42U, "1.2.3", 9U, NULL, NULL},
        {42U, "1.2.3", 1U, "E\t7\n", "E\t6\n"},
        {43U, "1.2.4", 1U, "E\t7\n", "E\t6\n"},
        {42U, "1.2.3", 1U, "E\t7\n", "E\t8\n"},
        {42U, "1.2.3", 1U, "B\t1.0.0\n", "B\t9.0.0\n"}
    };

    expect(mkdtemp(root) != NULL, "confirmed no-op fixture root created");
    (void)snprintf(release_dir, sizeof(release_dir), "%s/release", root);
    (void)snprintf(staging, sizeof(staging), "%s/staging", root);
    (void)snprintf(core, sizeof(core), "%s/core", root);
    (void)snprintf(state, sizeof(state), "%s/state", root);
    (void)snprintf(key, sizeof(key), "%s/key", root);
    (void)snprintf(request, sizeof(request), "%s/restart-request", root);
    cli_state_root = state; cli_core_root = core; cli_staging_root = staging;
    cli_key = key; cli_request_path = request; cli_release_dir = release_dir;
    cli_input = '\n';
    expect(run_cli(2, prepared_args, output, sizeof(output)) == EXIT_SUCCESS &&
               strstr(output, "No prepared core update") != NULL &&
               cli_repository_reads == 0 && cli_prepare_calls == 0 && cli_input_reads == 0 &&
               access(state, F_OK) != 0 && access(request, F_OK) != 0,
           "tui-prepared with missing state exits successfully without network, input or mutation");
    expect(mkdir(staging, 0700) == 0 && mkdir(core, 0700) == 0 && mkdir(state, 0700) == 0 &&
               make_release_fixture(release_dir, key, 42U, "1.2.3", 1U) == 0,
           "confirmed signed fixture created");
    (void)memset(&config, 0, sizeof(config));
    config.staging_root = staging; config.core_root = core;
    config.state_root = state; config.key_path = key;
    expect(run_cli(2, prepared_args, output, sizeof(output)) == EXIT_SUCCESS &&
               cli_repository_reads == 0 && cli_prepare_calls == 0 && cli_input_reads == 0 &&
               access(request, F_OK) != 0,
           "tui-prepared with idle state never checks or prepares an update");
    expect(c1_update_prepare_local(&config, release_dir, &result, error, sizeof(error)) == 0 &&
               result.phase == C1_UPDATE_PREPARED,
           "candidate prepared before no-op tests");
    cli_input = 'q';
    expect(run_cli(2, prepared_args, output, sizeof(output)) == EXIT_SUCCESS &&
               strstr(output, "Prepared core 1.2.3") != NULL &&
               strstr(output, "Update cancelled") != NULL && cli_input_reads == 1 &&
               cli_repository_reads == 0 && cli_prepare_calls == 0 && access(request, F_OK) != 0,
           "tui-prepared displays and cancels an existing update without network");
    cli_input = '\n';
    expect(run_cli(2, prepared_args, output, sizeof(output)) == EXIT_SUCCESS &&
               strstr(output, "Restart requested") != NULL && cli_input_reads == 1 &&
               cli_repository_reads == 0 && cli_prepare_calls == 0 &&
               c1_update_state_load(state, &loaded, error, sizeof(error)) == 0 &&
               c1_update_request_read(request, output, error, sizeof(error)) == 0 &&
               strcmp(output, loaded.digest) == 0,
           "tui-prepared retains normal digest-bound restart confirmation");
    expect(unlink(request) == 0 &&
               c1_update_bootstrap_activate(state, core, key, error, sizeof(error)) == 0 &&
               c1_update_state_load(state, &confirmed, error, sizeof(error)) == 0 &&
               confirmed.phase == C1_UPDATE_CONFIRMED,
           "candidate bootstrap confirms for exact identity regression");
    config.space_check = reject_space;
    expect(prepare_unchanged(&config, release_dir, &result, error, sizeof(error)) == 0 &&
               result.phase == C1_UPDATE_CONFIRMED && result.sequence == 42U &&
               strcmp(result.release, "1.2.3") == 0 && error[0] == '\0',
           "exact signed confirmed release succeeds up-to-date even with no free space");
    expect(transaction_snapshot(&config, before) == 0 &&
               run_cli(7, prepare_args, output, sizeof(output)) == EXIT_SUCCESS &&
               strstr(output, "up-to-date") != NULL && strstr(output, "phase=confirmed") != NULL &&
               run_cli(2, tui_args, output, sizeof(output)) == EXIT_SUCCESS &&
               strstr(output, "up-to-date") != NULL && strstr(output, "Press Enter") == NULL &&
               cli_repository_reads == 1 && cli_prepare_calls == 1 && cli_input_reads == 0 &&
               access(request, F_OK) != 0 && transaction_snapshot(&config, after) == 0 &&
               memcmp(before, after, sizeof(before)) == 0,
           "prepare CLI and explicit TUI report up-to-date without state mutation or restart prompt");
    expect(run_cli(2, prepared_args, output, sizeof(output)) == EXIT_SUCCESS &&
               strstr(output, "No prepared core update") != NULL &&
               cli_repository_reads == 0 && cli_prepare_calls == 0 && cli_input_reads == 0 &&
               access(request, F_OK) != 0,
           "tui-prepared with confirmed state exits without network or restart");

    for (i = 0U; i < sizeof(rejected) / sizeof(rejected[0]); ++i) {
        expect(c1_update_remove_tree(release_dir) == 0 &&
                   make_release_fixture(release_dir, key, rejected[i].sequence,
                                        rejected[i].version, rejected[i].payload) == 0 &&
                   (rejected[i].needle == NULL ||
                    resign_fixture_field(release_dir, rejected[i].needle, rejected[i].replacement) == 0),
               "signed conflicting or rollback release fixture created");
        expect(prepare_unchanged(&config, release_dir, &result, error, sizeof(error)) != 0,
               "confirmed identity still rejects older sequence, changed version/content/epoch and incompatibility");
    }
    expect(c1_update_remove_tree(release_dir) == 0 &&
               make_release_fixture(release_dir, key, 42U, "1.2.3", 1U) == 0 &&
               c1_update_join_path(signature, sizeof(signature), release_dir, C1_UPDATE_SIGNATURE_NAME) == 0 &&
               write_bytes(signature, bad_signature, sizeof(bad_signature), 0600) == 0 &&
               prepare_unchanged(&config, release_dir, &result, error, sizeof(error)) != 0,
           "exact confirmed manifest with invalid signature is rejected without mutation");
    expect(c1_update_remove_tree(release_dir) == 0 &&
               make_release_fixture(release_dir, key, 42U, "1.2.3", 1U) == 0 &&
               c1_update_join_path(artifact, sizeof(artifact), core,
                                   "releases/42-1.2.3/artifacts/C1ancher") == 0 &&
               write_bytes(artifact, (const unsigned char *)"bad", 3U, 0700) == 0 &&
               prepare_unchanged(&config, release_dir, &result, error, sizeof(error)) != 0,
           "confirmed immutable artifact tampering fails closed without repair");
    expect(write_bytes(artifact, payload, sizeof(payload), 0755) == 0 &&
               prepare_unchanged(&config, release_dir, &result, error, sizeof(error)) != 0,
           "confirmed immutable artifact permissions remain enforced");
    expect(chmod(artifact, 0700) == 0 &&
               prepare_unchanged(&config, release_dir, &result, error, sizeof(error)) == 0,
           "restored confirmed artifact permits the exact signed no-op");
    expect(unlink(artifact) == 0 &&
               prepare_unchanged(&config, release_dir, &result, error, sizeof(error)) != 0 &&
               c1_update_state_load(state, &loaded, error, sizeof(error)) == 0 &&
               loaded.generation == confirmed.generation && loaded.phase == C1_UPDATE_CONFIRMED &&
               strcmp(loaded.digest, confirmed.digest) == 0,
           "missing confirmed artifact is rejected without advancing or resetting confirmed state");
    expect(rmdir(staging) == 0, "no-op and rejected transactions leave no staging artifacts");
    (void)c1_update_remove_tree(root);
}

static void test_bootstrap_activation(void)
{
    char root[] = "/tmp/c1update-bootstrap-XXXXXX";
    char release_dir[C1_UPDATE_PATH_MAX], staging[C1_UPDATE_PATH_MAX];
    char core[C1_UPDATE_PATH_MAX], state[C1_UPDATE_PATH_MAX], key[C1_UPDATE_PATH_MAX];
    char current[C1_UPDATE_PATH_MAX], previous[C1_UPDATE_PATH_MAX], target[192];
    struct c1_update_state loaded;
    char error[C1_UPDATE_ERROR_MAX] = "";
    ssize_t length;

    expect(mkdtemp(root) != NULL, "bootstrap temporary root is created");
    (void)snprintf(release_dir, sizeof(release_dir), "%s/release", root);
    (void)snprintf(staging, sizeof(staging), "%s/staging", root);
    (void)snprintf(core, sizeof(core), "%s/core", root);
    (void)snprintf(state, sizeof(state), "%s/state", root);
    (void)snprintf(key, sizeof(key), "%s/key", root);
    expect(c1_update_join_path(current, sizeof(current), core, "current") == 0 &&
               c1_update_join_path(previous, sizeof(previous), core, "previous") == 0,
           "bootstrap pointer paths are constructed");
    expect(mkdir(staging, 0700) == 0 && mkdir(core, 0700) == 0 &&
               mkdir(state, 0700) == 0 &&
               make_release_fixture(release_dir, key, 40U, "1.0.0", 30U) == 0 &&
               prepare_fixture(release_dir, staging, core, state, key) == 0,
           "bootstrap candidate is prepared without existing pointers");
    expect(c1_update_bootstrap_activate(state, core, key, error, sizeof(error)) == 0 &&
               c1_update_state_load(state, &loaded, error, sizeof(error)) == 0 &&
               loaded.phase == C1_UPDATE_CONFIRMED,
           "first signed generation bootstrap activates and confirms");
    length = readlink(current, target, sizeof(target) - 1U);
    if (length > 0) target[(size_t)length] = '\0';
    expect(length > 0 && strcmp(target, "releases/40-1.0.0") == 0,
           "bootstrap current points at the signed generation");
    length = readlink(previous, target, sizeof(target) - 1U);
    if (length > 0) target[(size_t)length] = '\0';
    expect(length > 0 && strcmp(target, "releases/40-1.0.0") == 0,
           "bootstrap previous starts at the same recoverable generation");
    expect(c1_update_bootstrap_activate(state, core, key, error, sizeof(error)) != 0,
           "bootstrap activation cannot be reused after confirmation");
    (void)c1_update_remove_tree(root);
}

static void test_bootstrap_resume(void)
{
    char root[] = "/tmp/c1update-bootstrap-resume-XXXXXX";
    char release_dir[C1_UPDATE_PATH_MAX], staging[C1_UPDATE_PATH_MAX];
    char core[C1_UPDATE_PATH_MAX], state[C1_UPDATE_PATH_MAX], key[C1_UPDATE_PATH_MAX];
    struct c1_update_state prepared, pending, loaded;
    char error[C1_UPDATE_ERROR_MAX] = "";

    expect(mkdtemp(root) != NULL, "bootstrap resume temporary root is created");
    (void)snprintf(release_dir, sizeof(release_dir), "%s/release", root);
    (void)snprintf(staging, sizeof(staging), "%s/staging", root);
    (void)snprintf(core, sizeof(core), "%s/core", root);
    (void)snprintf(state, sizeof(state), "%s/state", root);
    (void)snprintf(key, sizeof(key), "%s/key", root);
    expect(mkdir(staging, 0700) == 0 && mkdir(core, 0700) == 0 &&
               mkdir(state, 0700) == 0 &&
               make_release_fixture(release_dir, key, 41U, "1.0.1", 31U) == 0 &&
               prepare_fixture(release_dir, staging, core, state, key) == 0 &&
               c1_update_state_load(state, &prepared, error, sizeof(error)) == 0 &&
               prepared.phase == C1_UPDATE_PREPARED,
           "bootstrap resume candidate reaches prepared");
    expect(c1_update_transition(&prepared, C1_UPDATE_PENDING_BOOT,
                                prepared.sequence, prepared.security_epoch,
                                prepared.release, prepared.digest, 0, &pending,
                                error, sizeof(error)) == 0 &&
               c1_update_state_commit(state, &prepared, &pending,
                                      error, sizeof(error)) == 0,
           "injected interruption persists pending before pointer creation");
    expect(c1_update_bootstrap_activate(state, core, key, error, sizeof(error)) == 0 &&
               c1_update_state_load(state, &loaded, error, sizeof(error)) == 0 &&
               loaded.phase == C1_UPDATE_CONFIRMED,
           "bootstrap activation resumes after pending-state interruption");
    (void)c1_update_remove_tree(root);
}

static void test_boot(void)
{
    char root[] = "/tmp/c1update-boot-XXXXXX";
    char old_release[C1_UPDATE_PATH_MAX], candidate_release[C1_UPDATE_PATH_MAX];
    char staging[C1_UPDATE_PATH_MAX], old_staging[C1_UPDATE_PATH_MAX];
    char core[C1_UPDATE_PATH_MAX], state[C1_UPDATE_PATH_MAX], old_state[C1_UPDATE_PATH_MAX];
    char key[C1_UPDATE_PATH_MAX], current[C1_UPDATE_PATH_MAX], artifact[C1_UPDATE_PATH_MAX];
    char launcher_path[C1_UPDATE_PATH_MAX];
    char ready[C1_UPDATE_PATH_MAX], rollback_marker[C1_UPDATE_PATH_MAX], target[192];
    char rollback_text[66];
    struct c1_update_state loaded;
    char error[C1_UPDATE_ERROR_MAX] = "";
    unsigned char original[3] = {20U, 21U, 22U};
    ssize_t length;

    expect(mkdtemp(root) != NULL, "boot temporary root is created");
    (void)snprintf(old_release, sizeof(old_release), "%s/old-release", root);
    (void)snprintf(candidate_release, sizeof(candidate_release), "%s/candidate-release", root);
    (void)snprintf(staging, sizeof(staging), "%s/staging", root);
    (void)snprintf(old_staging, sizeof(old_staging), "%s/old-staging", root);
    (void)snprintf(core, sizeof(core), "%s/core", root);
    (void)snprintf(state, sizeof(state), "%s/state", root);
    (void)snprintf(old_state, sizeof(old_state), "%s/old-state", root);
    (void)snprintf(key, sizeof(key), "%s/key", root);
    expect(c1_update_join_path(current, sizeof(current), core, "current") == 0,
           "boot current path is constructed");
    (void)snprintf(ready, sizeof(ready), "%s/ready", root);
    expect(c1_update_join_path(rollback_marker, sizeof(rollback_marker),
                               state, "rollback.v1") == 0 &&
               c1_update_join_path(launcher_path, sizeof(launcher_path), core,
                                   "current/artifacts/C1ancher-launcher") == 0,
           "rollback marker and launcher paths are constructed");
    expect(mkdir(staging, 0700) == 0 && mkdir(old_staging, 0700) == 0 &&
               mkdir(core, 0700) == 0 && mkdir(state, 0700) == 0 && mkdir(old_state, 0700) == 0 &&
               make_release_fixture(old_release, key, 41U, "1.2.2", 10U) == 0 &&
               prepare_fixture(old_release, old_staging, core, old_state, key) == 0 &&
               make_release_fixture(candidate_release, key, 42U, "1.2.3", 20U) == 0 &&
               prepare_fixture(candidate_release, staging, core, state, key) == 0 &&
               symlink("releases/41-1.2.2", current) == 0,
           "old and candidate immutable releases are prepared");

    expect(c1_update_join_path(artifact, sizeof(artifact), core,
                               "releases/42-1.2.3/artifacts/C1ancher") == 0,
           "candidate artifact path is constructed");
    expect(chmod(artifact, 0755) == 0 &&
               c1_update_activate(state, core, key, error, sizeof(error)) != 0,
           "candidate artifact mode mismatch rejects activation");
    expect(chmod(artifact, 0700) == 0 && write_bytes(artifact, (const unsigned char *)"bad", 3U, 0700) == 0 &&
               c1_update_activate(state, core, key, error, sizeof(error)) != 0,
           "candidate artifact hash mismatch rejects activation");
    expect(write_bytes(artifact, original, sizeof(original), 0700) == 0 &&
               c1_update_activate(state, core, key, error, sizeof(error)) == 0 &&
               c1_update_state_load(state, &loaded, error, sizeof(error)) == 0 &&
               loaded.phase == C1_UPDATE_PENDING_BOOT,
           "verified candidate activates into pending boot");

    expect(replace_symlink(current, "releases/41-1.2.2") == 0 &&
               c1_update_recover(state, core, key, error, sizeof(error)) == 0,
           "pending state with old current completes candidate switch");
    length = readlink(current, target, sizeof(target) - 1U);
    if (length > 0) target[(size_t)length] = '\0';
    expect(length > 0 && strcmp(target, "releases/42-1.2.3") == 0,
           "recover points current at candidate");
    expect(c1_update_mark_ready(state, ready, error, sizeof(error)) == 0 &&
               c1_update_ready_matches(ready, loaded.digest, error, sizeof(error)) == 0,
           "pending candidate writes exact ready digest marker");
    expect(chmod(ready, 0644) == 0 &&
               c1_update_ready_matches(ready, loaded.digest, error, sizeof(error)) != 0,
           "ready marker mode is enforced");
    (void)chmod(ready, 0600);
    expect(c1_update_rollback(state, core, key, error, sizeof(error)) == 0 &&
               c1_update_state_load(state, &loaded, error, sizeof(error)) == 0 &&
               loaded.phase == C1_UPDATE_IDLE && loaded.sequence == 42U &&
               loaded.security_epoch == 7U && loaded.digest[0] != '\0',
           "rollback returns idle while preserving anti-rollback identity");

    expect(prepare_fixture(candidate_release, staging, core, state, key) == 0 &&
               c1_update_activate(state, core, key, error, sizeof(error)) == 0 &&
               c1_update_state_load(state, &loaded, error, sizeof(error)) == 0 &&
               snprintf(rollback_text, sizeof(rollback_text), "%s\n", loaded.digest) == 65 &&
               write_bytes(rollback_marker, (const unsigned char *)rollback_text,
                           65U, 0600) == 0 &&
               replace_symlink(current, "releases/41-1.2.2") == 0 &&
               c1_update_recover(state, core, key, error, sizeof(error)) == 0 &&
               c1_update_state_load(state, &loaded, error, sizeof(error)) == 0 &&
               loaded.phase == C1_UPDATE_IDLE && access(rollback_marker, F_OK) != 0,
           "rollback marker resumes toward previous after pointer-switch interruption");
    length = readlink(current, target, sizeof(target) - 1U);
    if (length > 0) target[(size_t)length] = '\0';
    expect(length > 0 && strcmp(target, "releases/41-1.2.2") == 0,
           "interrupted rollback never reactivates the failed candidate");

    expect(prepare_fixture(candidate_release, staging, core, state, key) == 0 &&
               c1_update_activate(state, core, key, error, sizeof(error)) == 0 &&
               replace_symlink(current, "../escape") == 0 &&
               c1_update_recover(state, core, key, error, sizeof(error)) == 0 &&
               c1_update_state_load(state, &loaded, error, sizeof(error)) == 0 &&
               loaded.phase == C1_UPDATE_IDLE,
           "escaping current pointer rolls back and suppresses automatic retry");
    length = readlink(current, target, sizeof(target) - 1U);
    if (length > 0) target[(size_t)length] = '\0';
    expect(length > 0 && strcmp(target, "releases/41-1.2.2") == 0,
           "abnormal pointer rolls back to previous");

    expect(prepare_fixture(candidate_release, staging, core, state, key) == 0 &&
               c1_update_activate(state, core, key, error, sizeof(error)) == 0 &&
               c1_update_confirm(state, core, key, error, sizeof(error)) == 0 &&
               c1_update_state_load(state, &loaded, error, sizeof(error)) == 0 &&
               loaded.phase == C1_UPDATE_CONFIRMED,
           "matching pending current confirms after release revalidation");
    (void)unlink(ready);
    expect(symlink("missing-ready-target", ready) == 0 &&
               c1_update_supervise(state, core, key, ready, launcher_path,
                                   error, sizeof(error)) == C1_UPDATER_FATAL_EXIT,
           "stale ready symlink fails closed before launching a confirmed core");
    (void)unlink(ready);
    expect(c1_update_rollback(state, core, key, error, sizeof(error)) == 0 &&
               c1_update_state_load(state, &loaded, error, sizeof(error)) == 0 &&
               loaded.phase == C1_UPDATE_IDLE,
           "confirmed crash recovery can roll back to a distinct previous release");
    length = readlink(current, target, sizeof(target) - 1U);
    if (length > 0) target[(size_t)length] = '\0';
    expect(length > 0 && strcmp(target, "releases/41-1.2.2") == 0,
           "confirmed rollback restores the previous release");
    {
        unsigned int boot, repeat;
        char boot_id[37], attempts_path[C1_UPDATE_PATH_MAX];
        expect(c1_update_join_path(attempts_path, sizeof(attempts_path), state, "pending-boots.v1") == 0 &&
                   prepare_fixture(candidate_release, staging, core, state, key) == 0 &&
                   c1_update_activate(state, core, key, error, sizeof(error)) == 0,
               "candidate prepares for persistent boot budget test");
        for (boot = 1; boot <= 4; ++boot) {
            (void)snprintf(boot_id, sizeof(boot_id), "00000000-0000-0000-0000-%012u", boot);
            for (repeat = 0; repeat < 5; ++repeat) {
                expect(c1_update_recover_boot(state, core, key, boot_id, error, sizeof(error)) == 0 &&
                           c1_update_state_load(state, &loaded, error, sizeof(error)) == 0 &&
                           loaded.phase == (boot <= 3 ? C1_UPDATE_PENDING_BOOT : C1_UPDATE_IDLE),
                       "same boot is counted once; fourth distinct boot rolls back");
            }
        }
        expect(access(attempts_path, F_OK) != 0,
               "rollback durably removes pending boot attempt state");
        expect(prepare_fixture(candidate_release, staging, core, state, key) == 0 &&
                   c1_update_activate(state, core, key, error, sizeof(error)) == 0 &&
                   c1_update_recover_boot(state, core, key, boot_id, error, sizeof(error)) == 0 &&
                   access(attempts_path, F_OK) == 0 &&
                   c1_update_confirm(state, core, key, error, sizeof(error)) == 0 &&
                   access(attempts_path, F_OK) != 0,
               "new attempt counts anew and confirmation clears pending state");
        expect(c1_update_rollback(state, core, key, error, sizeof(error)) == 0 &&
                   prepare_fixture(candidate_release, staging, core, state, key) == 0 &&
                   c1_update_activate(state, core, key, error, sizeof(error)) == 0 &&
                   write_bytes(attempts_path, (const unsigned char *)"corrupt\n", 8U, 0600) == 0 &&
                   c1_update_recover_boot(state, core, key, boot_id, error, sizeof(error)) == 0 &&
                   c1_update_state_load(state, &loaded, error, sizeof(error)) == 0 && loaded.phase == C1_UPDATE_IDLE,
               "corrupt persistent attempt record rolls back instead of resetting budget");
    }
    (void)c1_update_remove_tree(root);
}

static int process_stopped(pid_t process)
{
    unsigned int attempt;

    for (attempt = 0U; attempt < 40U; ++attempt) {
        struct timespec delay = {0, 50000000L};
        if (kill(process, 0) != 0 && errno == ESRCH) return 1;
        (void)nanosleep(&delay, NULL);
    }
    return 0;
}

static pid_t read_process_id(const char *path)
{
    FILE *stream = fopen(path, "r");
    long value = -1;

    if (stream == NULL) return (pid_t)-1;
    if (fscanf(stream, "%ld", &value) != 1) value = -1;
    (void)fclose(stream);
    return value > 0 ? (pid_t)value : (pid_t)-1;
}

static void test_slots(void)
{
    char root[] = "/tmp/c1update-slot-XXXXXX";
    char good[C1_UPDATE_PATH_MAX], bad[C1_UPDATE_PATH_MAX], hanging[C1_UPDATE_PATH_MAX];
    char slots[C1_UPDATE_PATH_MAX];
    char staging[C1_UPDATE_PATH_MAX], core[C1_UPDATE_PATH_MAX], state[C1_UPDATE_PATH_MAX];
    char slot_a[C1_UPDATE_PATH_MAX], installed_path[C1_UPDATE_PATH_MAX], key[C1_UPDATE_PATH_MAX];
    char lock_path[C1_UPDATE_PATH_MAX] = "";
    char installed = '\0', error[C1_UPDATE_ERROR_MAX] = "";
    unsigned char *data = NULL;
    size_t size = 0U;
    int held_lock = -1;
    pid_t child;
    int child_status;

    expect(mkdtemp(root) != NULL, "slot temporary root is created");
    (void)snprintf(good, sizeof(good), "%s/good", root);
    (void)snprintf(bad, sizeof(bad), "%s/bad", root);
    (void)snprintf(hanging, sizeof(hanging), "%s/hanging", root);
    (void)snprintf(slots, sizeof(slots), "%s/slots", root);
    (void)snprintf(staging, sizeof(staging), "%s/staging", root);
    (void)snprintf(core, sizeof(core), "%s/core", root);
    (void)snprintf(state, sizeof(state), "%s/state", root);
    expect(c1_update_join_path(slot_a, sizeof(slot_a), slots, "slot-a") == 0 &&
               c1_update_join_path(installed_path, sizeof(installed_path), slot_a, "c1updater") == 0,
           "slot test paths are constructed");
    (void)snprintf(key, sizeof(key), "%s/key", root);
    expect(mkdir(slots, 0700) == 0 && mkdir(slot_a, 0700) == 0 &&
               mkdir(staging, 0700) == 0 && mkdir(core, 0700) == 0 &&
               mkdir(state, 0700) == 0 &&
               write_bytes(installed_path, (const unsigned char *)"old", 3U, 0700) == 0 &&
               make_release_fixture(good, key, 50U, "2.0.0", 100U) == 0 &&
               c1_update_install_inactive_slot(slots, 'a', good, key, &installed,
                                               error, sizeof(error)) == 0 && installed == 'b',
           "A/B install selects inactive slot and accepts updater self-test");
    expect(make_release_fixture(bad, key, 51U, "2.0.1", 101U) == 0 &&
               c1_update_install_inactive_slot(slots, 'b', bad, key, &installed,
                                               error, sizeof(error)) != 0 &&
               c1_secure_read_file(installed_path, &data, &size, 3U, 3U,
                                   error, sizeof(error)) == 0 &&
               memcmp(data, "old", 3U) == 0,
           "failed candidate self-test does not replace inactive slot");
    free(data);
    data = NULL;
    expect(prepare_fixture(good, staging, core, state, key) == 0 &&
               (held_lock = c1_update_lock(state, lock_path, sizeof(lock_path),
                                            error, sizeof(error))) >= 0,
           "prepared installation fixture acquires the update lock");
    child = fork();
    if (child == 0) {
        char child_installed = '\0';
        char child_error[C1_UPDATE_ERROR_MAX] = "";
        int child_result = c1_update_install_prepared_slot(
            slots, 'b', core, state, key, &child_installed,
            child_error, sizeof(child_error));
        _exit(child_result != 0 ? EXIT_SUCCESS : EXIT_FAILURE);
    }
    expect(child > 0 && waitpid(child, &child_status, 0) == child &&
               WIFEXITED(child_status) && WEXITSTATUS(child_status) == EXIT_SUCCESS,
           "prepared installation rejects a concurrent transaction");
    c1_update_unlock(held_lock, lock_path);
    held_lock = -1;
    expect(c1_update_install_prepared_slot(slots, 'b', core, state, key,
                                           &installed, error, sizeof(error)) == 0 &&
               installed == 'a',
           "prepared transaction installs updater into the inactive root slot");
    expect(c1_secure_read_file(installed_path, &data, &size, 64U,
                               C1_SECURE_FILE_ANY_SIZE,
                               error, sizeof(error)) == 0 &&
               size == strlen("#!/bin/sh\nexit 0\n") &&
               memcmp(data, "#!/bin/sh\nexit 0\n", size) == 0,
           "prepared updater slot contains the signed release artifact");
    free(data);
    data = NULL;
    expect(make_release_fixture(hanging, key, 52U, "2.0.2", 102U) == 0 &&
               c1_update_install_inactive_slot(slots, 'b', hanging, key, &installed,
                                               error, sizeof(error)) != 0,
           "hung updater self-test is killed at the bounded timeout");
    expect(c1_secure_read_file(installed_path, &data, &size, 64U,
                               C1_SECURE_FILE_ANY_SIZE,
                               error, sizeof(error)) == 0 &&
               size == strlen("#!/bin/sh\nexit 0\n") &&
               memcmp(data, "#!/bin/sh\nexit 0\n", size) == 0,
           "timed-out updater does not replace the known-good inactive slot");
    free(data);
    data = NULL;
    (void)unlink("/tmp/c1update-self-test-child");
    expect(c1_update_remove_tree(hanging) == 0 &&
               make_release_fixture(hanging, key, 53U, "2.0.3", 103U) == 0 &&
               c1_update_install_inactive_slot(slots, 'b', hanging, key, &installed,
                                               error, sizeof(error)) != 0,
           "failed updater self-test terminates the candidate process group");
    {
        pid_t descendant = read_process_id("/tmp/c1update-self-test-child");
        expect(descendant > 0 && process_stopped(descendant) != 0,
               "failed updater self-test does not leave a descendant process");
    }
    (void)unlink("/tmp/c1update-self-test-child");
    (void)c1_update_remove_tree(root);
}

static int hex_decode(const char *hex, unsigned char *data, size_t size)
{
    size_t i;

    for (i = 0U; i < size; ++i) {
        unsigned int high;
        unsigned int low;
        char a = hex[i * 2U];
        char b = hex[i * 2U + 1U];
        high = (unsigned int)(a <= '9' ? a - '0' : a - 'a' + 10);
        low = (unsigned int)(b <= '9' ? b - '0' : b - 'a' + 10);
        data[i] = (unsigned char)((high << 4U) | low);
    }
    return 0;
}

static void cleanup_state_root(const char *root, uint64_t generation)
{
    char path[C1_UPDATE_PATH_MAX];

    (void)snprintf(path, sizeof(path), "%s/current", root);
    (void)unlink(path);
    (void)snprintf(path, sizeof(path), "%s/generations/%llu/state.v1", root,
                   (unsigned long long)generation);
    (void)unlink(path);
    (void)snprintf(path, sizeof(path), "%s/generations/%llu", root,
                   (unsigned long long)generation);
    (void)rmdir(path);
}

static void test_state_storage(void)
{
    static const char digest[] = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    char root[] = "/tmp/c1update-state-XXXXXX";
    char current[C1_UPDATE_PATH_MAX];
    char orphan[C1_UPDATE_PATH_MAX];
    char orphan_state[C1_UPDATE_PATH_MAX];
    char state_text[256];
    char error[C1_UPDATE_ERROR_MAX] = "";
    struct c1_update_state initial;
    struct c1_update_state next;
    struct c1_update_state candidate;
    struct c1_update_state loaded;

    expect(mkdtemp(root) != NULL, "state temporary root is created");
    fill_initial(&initial);
    expect(c1_update_transition(&initial, C1_UPDATE_DOWNLOADING, 9U, 3U, "2.0.0", digest,
                                0, &next, error, sizeof(error)) == 0,
           "initial state transition is constructed");
    expect(c1_update_state_commit(root, &initial, &next, error, sizeof(error)) == 0,
           "state generation commits atomically");
    expect(c1_update_state_load(root, &loaded, error, sizeof(error)) == 0 &&
               loaded.generation == 1U && loaded.phase == C1_UPDATE_DOWNLOADING &&
               loaded.sequence == 9U && loaded.security_epoch == 3U &&
               strcmp(loaded.digest, digest) == 0,
           "committed state generation loads");

    (void)snprintf(orphan, sizeof(orphan), "%s/generations/1/state.v1", root);
    expect(chmod(root, 0750) == 0 &&
               c1_update_state_load(root, &loaded, error, sizeof(error)) != 0 &&
               chmod(root, 0700) == 0,
           "state root rejects non-private permissions");
    expect(chmod(orphan, 0640) == 0 &&
               c1_update_state_load(root, &loaded, error, sizeof(error)) != 0 &&
               chmod(orphan, 0600) == 0,
           "state file rejects non-private permissions");
    (void)snprintf(current, sizeof(current), "%s/generations/1", root);
    expect(chmod(current, 0750) == 0 &&
               c1_update_state_load(root, &loaded, error, sizeof(error)) != 0 &&
               chmod(current, 0700) == 0,
           "state generation rejects non-private permissions");
    expect(c1_update_state_load(root, &loaded, error, sizeof(error)) == 0,
           "state loads after private permissions are restored");

    candidate = loaded;
    candidate.generation = 1U;
    candidate.phase = C1_UPDATE_VERIFIED;
    expect(c1_update_state_commit(root, &loaded, &candidate, error, sizeof(error)) != 0,
           "generation must strictly increase");
    candidate.generation = 2U;
    candidate.sequence = 8U;
    expect(c1_update_state_commit(root, &loaded, &candidate, error, sizeof(error)) != 0,
           "commit rejects sequence rollback");
    candidate.sequence = 9U;
    (void)strcpy(candidate.digest,
                 "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
    expect(c1_update_state_commit(root, &loaded, &candidate, error, sizeof(error)) != 0,
           "commit rejects same sequence with different digest");
    expect(c1_update_transition(&loaded, C1_UPDATE_VERIFIED, 9U, 3U, "2.0.0", digest,
                                0, &candidate, error, sizeof(error)) == 0 &&
               c1_update_state_commit(root, &loaded, &candidate, error, sizeof(error)) == 0 &&
               c1_update_state_load(root, &loaded, error, sizeof(error)) == 0 &&
               loaded.generation == 2U && loaded.phase == C1_UPDATE_VERIFIED,
           "a subsequent generation commits and loads");

    (void)snprintf(orphan, sizeof(orphan), "%s/generations/3", root);
    expect(mkdir(orphan, 0700) == 0 &&
               c1_update_state_load(root, &loaded, error, sizeof(error)) == 0 &&
               loaded.generation == 2U,
           "unreferenced incomplete generation is ignored");
    expect(rmdir(orphan) == 0, "unreferenced generation fixture is removed");

    next = loaded;
    expect(c1_update_transition(&next, C1_UPDATE_PREPARED, 9U, 3U,
                                "2.0.0", digest, 0, &candidate,
                                error, sizeof(error)) == 0,
           "prepared state for orphan recovery is constructed");
    (void)snprintf(orphan, sizeof(orphan), "%s/generations/3", root);
    expect(c1_update_join_path(orphan_state, sizeof(orphan_state),
                               orphan, "state.v1") == 0,
           "orphan generation state path is constructed");
    expect(snprintf(state_text, sizeof(state_text),
                    "C1CORE-STATE 1\nP\tprepared\nS\t9\nE\t3\nR\t2.0.0\nD\t%s\n",
                    digest) > 0 && mkdir(orphan, 0700) == 0 &&
               write_bytes(orphan_state, (const unsigned char *)state_text,
                           strlen(state_text), 0600) == 0 &&
               c1_update_state_commit(root, &next, &candidate,
                                      error, sizeof(error)) == 0 &&
               c1_update_state_load(root, &loaded, error, sizeof(error)) == 0 &&
               loaded.generation == 3U && loaded.phase == C1_UPDATE_PREPARED,
           "complete orphan generation is adopted after pointer-switch interruption");
    expect(c1_update_state_commit(root, &next, &candidate,
                                  error, sizeof(error)) == 0,
           "state commit retry is idempotent after pointer switch");

    (void)snprintf(current, sizeof(current), "%s/current", root);
    (void)unlink(current);
    expect(symlink("../escape", current) == 0, "escaping current symlink fixture is created");
    expect(c1_update_state_load(root, &loaded, error, sizeof(error)) != 0,
           "escaping current symlink is rejected");
    (void)unlink(current);
    cleanup_state_root(root, 3U);
    cleanup_state_root(root, 2U);
    cleanup_state_root(root, 1U);
    (void)snprintf(current, sizeof(current), "%s/generations", root);
    (void)rmdir(current);
    (void)rmdir(root);
}

static void test_ed25519(void)
{
    static const char public_hex[] =
        "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a";
    static const char signature_hex[] =
        "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e06522490155"
        "5fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b";
    unsigned char public_key[C1_ED25519_PUBLIC_KEY_SIZE];
    unsigned char signature[C1_ED25519_SIGNATURE_SIZE];
    char root[] = "/tmp/c1update-key-XXXXXX";
    char key_path[C1_UPDATE_PATH_MAX];
    char signature_path[C1_UPDATE_PATH_MAX];
    char link_path[C1_UPDATE_PATH_MAX];
    char error[C1_UPDATE_ERROR_MAX] = "";

    (void)hex_decode(public_hex, public_key, sizeof(public_key));
    (void)hex_decode(signature_hex, signature, sizeof(signature));
    expect(c1_trusted_ed25519_verify(signature, NULL, 0U, public_key,
                                     error, sizeof(error)) == 0,
           "RFC8032 empty-message Ed25519 vector verifies");
    signature[0] ^= 1U;
    expect(c1_trusted_ed25519_verify(signature, NULL, 0U, public_key,
                                     error, sizeof(error)) != 0,
           "modified Ed25519 signature is rejected");
    signature[0] ^= 1U;

    expect(mkdtemp(root) != NULL, "key temporary root is created");
    (void)snprintf(key_path, sizeof(key_path), "%s/key", root);
    (void)snprintf(signature_path, sizeof(signature_path), "%s/signature", root);
    (void)snprintf(link_path, sizeof(link_path), "%s/link", root);
    expect(write_bytes(key_path, public_key, sizeof(public_key), 0600) == 0 &&
               write_bytes(signature_path, signature, sizeof(signature), 0600) == 0,
           "secure Ed25519 fixtures are written");
    expect(c1_trusted_ed25519_verify_files(key_path, signature_path, NULL, 0U,
                                           error, sizeof(error)) == 0,
           "secure key and signature files verify");
    expect(chmod(key_path, 0660) == 0 &&
               c1_trusted_ed25519_verify_files(key_path, signature_path, NULL, 0U,
                                                error, sizeof(error)) != 0,
           "group-writable trusted key is rejected");
    expect(chmod(key_path, 0600) == 0, "trusted key mode is restored");
    expect(symlink(key_path, link_path) == 0 &&
               c1_trusted_ed25519_verify_files(link_path, signature_path, NULL, 0U,
                                                error, sizeof(error)) != 0,
           "trusted key symlink is rejected");
    (void)unlink(link_path);
    expect(link(key_path, link_path) == 0 &&
               c1_trusted_ed25519_verify_files(key_path, signature_path, NULL, 0U,
                                                error, sizeof(error)) != 0,
           "hard-linked trusted key is rejected");
    (void)unlink(link_path);
    (void)unlink(key_path);
    (void)unlink(signature_path);
    (void)rmdir(root);
}

static void test_update_request(void)
{
    static const char digest[] = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    static const char next_digest[] = "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff";
    char root[] = "/tmp/c1update-request-XXXXXX";
    char path[C1_UPDATE_PATH_MAX];
    char isolated_path[C1_UPDATE_PATH_MAX];
    char link_path[C1_UPDATE_PATH_MAX];
    char read_digest[C1_UPDATE_REQUEST_FILE_SIZE];
    struct stat information;
    char error[C1_UPDATE_ERROR_MAX] = "";

    expect(mkdtemp(root) != NULL, "request temporary root is created");
    (void)snprintf(path, sizeof(path), "%s/restart-request", root);
    (void)snprintf(isolated_path, sizeof(isolated_path), "%s/restart-request.consuming", root);
    (void)snprintf(link_path, sizeof(link_path), "%s/link", root);
    expect(c1_update_request_write(path, digest, error, sizeof(error)) == 0 &&
               stat(path, &information) == 0 && (information.st_mode & 0777U) == 0600U,
           "request writes exact digest with private mode");
    expect(c1_update_request_read(path, read_digest, error, sizeof(error)) == 0 &&
               strcmp(read_digest, digest) == 0,
           "request read validates lowercase digest and LF");
    expect(c1_update_request_consume(path, digest, error, sizeof(error)) == 0 &&
               access(path, F_OK) != 0 && access(isolated_path, F_OK) != 0,
           "request consume durably removes the isolated request");
    expect(c1_update_request_consume(path, digest, error, sizeof(error)) != 0,
           "consumed request cannot be consumed twice");

    expect(c1_update_request_write(path, digest, error, sizeof(error)) == 0 &&
               rename(path, isolated_path) == 0 &&
               c1_update_request_consume(path, digest, error, sizeof(error)) == 0 &&
               access(isolated_path, F_OK) != 0,
           "request consume resumes an interruption after isolation");

    expect(c1_update_request_write(path, digest, error, sizeof(error)) == 0 &&
               rename(path, isolated_path) == 0 &&
               c1_update_request_write(path, next_digest, error, sizeof(error)) == 0 &&
               c1_update_request_consume(path, digest, error, sizeof(error)) == 0 &&
               c1_update_request_read(path, read_digest, error, sizeof(error)) == 0 &&
               strcmp(read_digest, next_digest) == 0,
           "resumed consumption preserves a newer queued request");
    expect(c1_update_request_consume(path, next_digest, error, sizeof(error)) == 0,
           "queued request is consumed on the following attempt");
    expect(symlink(link_path, path) != 0 ||
               c1_update_request_read(path, read_digest, error, sizeof(error)) != 0,
           "request reader rejects symlink path");
    (void)unlink(path);
    (void)unlink(link_path);
    (void)rmdir(root);
}

static void test_supervise_policy(void)
{
    struct c1_update_pending_observation observation;

    (void)memset(&observation, 0, sizeof(observation));
    observation.started_ms = 1000;
    observation.now_ms = 1000;
    observation.ready_at_ms = -1;
    expect(c1_update_pending_decide(NULL) == C1_UPDATE_PENDING_ROLLBACK,
           "missing pending observation fails closed");
    expect(c1_update_pending_decide(&observation) == C1_UPDATE_PENDING_WAIT,
           "candidate waits before readiness or timeout");
    observation.now_ms = observation.started_ms + C1_UPDATE_READY_TIMEOUT_MS - 1;
    expect(c1_update_pending_decide(&observation) == C1_UPDATE_PENDING_WAIT,
           "missing or wrong ready marker waits until timeout boundary");
    observation.now_ms = observation.started_ms + C1_UPDATE_READY_TIMEOUT_MS;
    expect(c1_update_pending_decide(&observation) == C1_UPDATE_PENDING_ROLLBACK,
           "missing or wrong ready marker rolls back at timeout");

    observation.ready_at_ms = 2000;
    observation.now_ms = observation.ready_at_ms + C1_UPDATE_READY_STABLE_MS - 1;
    expect(c1_update_pending_decide(&observation) == C1_UPDATE_PENDING_WAIT,
           "healthy candidate waits for the full stability interval");
    observation.now_ms = observation.ready_at_ms + C1_UPDATE_READY_STABLE_MS;
    expect(c1_update_pending_decide(&observation) == C1_UPDATE_PENDING_CONFIRM,
           "healthy candidate confirms at the stability boundary");

    observation.ready_at_ms = observation.started_ms + C1_UPDATE_READY_TIMEOUT_MS;
    observation.now_ms = observation.ready_at_ms + C1_UPDATE_READY_STABLE_MS;
    expect(c1_update_pending_decide(&observation) == C1_UPDATE_PENDING_ROLLBACK,
           "late or repeatedly replaced readiness cannot extend the boot deadline");

    observation.ready_at_ms = observation.started_ms - 1;
    expect(c1_update_pending_decide(&observation) == C1_UPDATE_PENDING_ROLLBACK,
           "ready timestamp before launch fails closed");
    observation.ready_at_ms = -1;
    observation.now_ms = observation.started_ms - 1;
    expect(c1_update_pending_decide(&observation) == C1_UPDATE_PENDING_ROLLBACK,
           "non-monotonic clock observation fails closed");

    observation.now_ms = observation.started_ms;
    observation.stop_requested = true;
    expect(c1_update_pending_decide(&observation) == C1_UPDATE_PENDING_STOP,
           "stop request terminates pending supervision");
    observation.child_exited = true;
    expect(c1_update_pending_decide(&observation) == C1_UPDATE_PENDING_CHILD_EXIT,
           "candidate exit takes precedence and triggers rollback handling");
}

static void test_compatibility(void)
{
    struct c1_update_manifest manifest;
    char error[C1_UPDATE_ERROR_MAX] = "";
    expect(c1_update_parse_manifest((const unsigned char *)valid_manifest, strlen(valid_manifest),
                                    &manifest, error, sizeof(error)) == 0,
           "compatibility fixture parses");
    expect(c1_update_check_compatibility_versions(&manifest, "1.0.0", "1.0.0", error, sizeof(error)) == 0,
           "legacy signed B/U 1.0.0 remains runnable for safe rollback");
    (void)strcpy(manifest.min_bootstrap, "1.1.0");
    expect(c1_update_check_compatibility_versions(&manifest, "1.0.0", "99.0.0", error, sizeof(error)) != 0,
           "new updater cannot invent a newer local bootstrap capability");
    expect(c1_update_check_compatibility_versions(&manifest, "1.1.0", "1.1.0", error, sizeof(error)) == 0,
           "matching trusted minimum capability is accepted");
    (void)strcpy(manifest.min_updater, "1.10.0");
    expect(c1_update_check_compatibility_versions(&manifest, "1.1.0", "1.9.0", error, sizeof(error)) != 0 &&
               c1_update_check_compatibility_versions(&manifest, "1.1.0", "1.10.0", error, sizeof(error)) == 0,
           "capability comparison is numeric rather than lexical");
    (void)strcpy(manifest.min_updater, "1.1.0-beta");
    expect(c1_update_check_compatibility_versions(&manifest, "1.1.0", "1.1.0", error, sizeof(error)) != 0,
           "ambiguous prerelease capability fails closed");
}

int main(void)
{
    test_manifest();
    test_compatibility();
    test_transitions();
    test_supervise_policy();
    test_state_storage();
    test_sha256();
    test_update_lock();
    test_ed25519();
    test_repository_url();
    test_repository_config();
    test_copy_preserves_destination();
    test_verified_resume();
    test_transaction();
    test_confirmed_noop();
    test_bootstrap_activation();
    test_bootstrap_resume();
    test_boot();
    test_slots();
    test_update_request();
    if (failures != 0) {
        fprintf(stderr, "%d update test(s) failed\n", failures);
        return EXIT_FAILURE;
    }
    puts("all update tests passed");
    return EXIT_SUCCESS;
}