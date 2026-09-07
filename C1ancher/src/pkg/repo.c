#include "pkg.h"
#include "ed25519.h"

#include <ctype.h>
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

int c1pkg_fetch(const char *url, const char *output, uint64_t limit,
                char *error, size_t error_size)
{
    const char *curl = c1pkg_helper("/usr/bin/curl", "curl");
    char *arguments[] = {(char *)curl, "--fail", "--location", "--proto", "=http,https",
                         "--max-redirs", "3", "--connect-timeout", "15", "--max-time", "180",
                         "--silent", "--show-error", "--output", "-", (char *)url, NULL};
    struct stat information;

    if (c1pkg_run(arguments, output, limit, error, error_size) != 0) {
        return -1;
    }
    if (stat(output, &information) != 0 || information.st_size < 0 ||
        (uint64_t)information.st_size > limit) {
        c1pkg_set_error(error, error_size, "download exceeded size limit");
        (void)unlink(output);
        return -1;
    }
    return 0;
}

static int verify_key(const char *key, unsigned char public_key[32],
                      char *error, size_t error_size)
{
    struct stat information;
    unsigned char *data = NULL;
    size_t size = 0U;

    if (key == NULL || key[0] != '/' || lstat(key, &information) != 0 ||
        !S_ISREG(information.st_mode) || information.st_nlink != 1 ||
        information.st_size != 32 || (information.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
        c1pkg_set_error(error, error_size,
                        "trusted Ed25519 key missing, unsafe, or not 32 bytes: %s",
                        key != NULL ? key : "(null)");
        return -1;
    }
    if (c1pkg_read_file(key, &data, &size, 32U, error, error_size) != 0 || size != 32U) {
        free(data);
        return -1;
    }
    memcpy(public_key, data, 32U);
    free(data);
    return 0;
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
    size_t i;
    size_t length = strlen(name);

    if (length == 0U || length > C1PKG_NAME_MAX || name[0] == ' ' || name[length - 1U] == ' ') {
        return 0;
    }
    for (i = 0U; i < length; ++i) {
        unsigned char character = (unsigned char)name[i];
        if (character < 0x20U || character > 0x7eU || character == (unsigned char)'\t') {
            return 0;
        }
    }
    return 1;
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

static int parse_package_line(char *line, struct c1pkg_package *package,
                              char *error, size_t error_size)
{
    char *fields[8];
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
    if (field != 8U || strcmp(fields[0], "P") != 0 || !c1pkg_safe_id(fields[1]) ||
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
    (void)strcpy(package->archive, fields[4]);
    (void)strcpy(package->sha256, fields[5]);
    (void)strcpy(package->entry, fields[7]);
    return 0;
}

static int parse_index_file(const char *path, struct c1pkg_index *index,
                            char *error, size_t error_size)
{
    unsigned char *data = NULL;
    size_t size = 0U;
    size_t offset = 0U;
    size_t line_number = 0U;

    index->sequence = 0U;
    index->count = 0U;
    if (c1pkg_read_file(path, &data, &size, C1PKG_INDEX_MAX, error, error_size) != 0) {
        return -1;
    }
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
            if (strcmp(line, C1PKG_INDEX_HEADER) != 0) {
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
            if (parse_package_line(line, package, error, error_size) != 0 ||
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
    return 0;
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
        size < 2U || data[size - 1U] != (unsigned char)'\n') {
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
    return 0;
}

int c1pkg_repo_load_cached(const struct c1pkg_config *config, struct c1pkg_index *index,
                           char *error, size_t error_size)
{
    char path[C1PKG_PATH_MAX];
    char signature[C1PKG_PATH_MAX];
    if (c1pkg_join(path, sizeof(path), C1PKG_STATE_ROOT, "cache/index.v1") != 0 ||
        c1pkg_join(signature, sizeof(signature), C1PKG_STATE_ROOT,
                   "cache/index.v1.sig") != 0) {
        c1pkg_set_error(error, error_size, "cache path is too long");
        return -1;
    }
    if (verify_signature(config->public_key, signature, path, error, error_size) != 0) {
        return -1;
    }
    return parse_index_file(path, index, error, error_size);
}

int c1pkg_repo_refresh(const struct c1pkg_config *config, struct c1pkg_index *index,
                       char *error, size_t error_size)
{
    char cache[C1PKG_PATH_MAX];
    char index_tmp[C1PKG_PATH_MAX];
    char signature_tmp[C1PKG_PATH_MAX];
    char index_final[C1PKG_PATH_MAX];
    char signature_final[C1PKG_PATH_MAX];
    char url[1400];
    uint64_t highest_sequence = 0U;
    int result = -1;

    if (config == NULL || !valid_url_base(config->repo_base) ||
        c1pkg_store_init(error, error_size) != 0 ||
        c1pkg_join(cache, sizeof(cache), C1PKG_STATE_ROOT, "cache") != 0 ||
        c1pkg_mkdir_p(cache, 0700, error, error_size) != 0) {
        if (error != NULL && error[0] == '\0') {
            c1pkg_set_error(error, error_size, "invalid repository base URL");
        }
        return -1;
    }
    if (snprintf(index_tmp, sizeof(index_tmp), "%s/index.%ld.tmp", cache, (long)getpid()) < 0 ||
        snprintf(signature_tmp, sizeof(signature_tmp), "%s/index.%ld.sig.tmp", cache,
                 (long)getpid()) < 0 ||
        c1pkg_join(index_final, sizeof(index_final), cache, "index.v1") != 0 ||
        c1pkg_join(signature_final, sizeof(signature_final), cache, "index.v1.sig") != 0) {
        c1pkg_set_error(error, error_size, "cache path is too long");
        return -1;
    }
    (void)unlink(index_tmp);
    (void)unlink(signature_tmp);
    if (make_url(url, sizeof(url), config->repo_base, "index.v1") != 0 ||
        c1pkg_fetch(url, index_tmp, C1PKG_INDEX_MAX, error, error_size) != 0 ||
        make_url(url, sizeof(url), config->repo_base, "index.v1.sig") != 0 ||
        c1pkg_fetch(url, signature_tmp, 65536U, error, error_size) != 0 ||
        verify_signature(config->public_key, signature_tmp, index_tmp, error, error_size) != 0 ||
        parse_index_file(index_tmp, index, error, error_size) != 0 ||
        load_highest_sequence(&highest_sequence, error, error_size) != 0) {
        goto done;
    }
    if (index->sequence < highest_sequence) {
        c1pkg_set_error(error, error_size,
                        "repository rollback rejected: sequence %llu is below %llu",
                        (unsigned long long)index->sequence,
                        (unsigned long long)highest_sequence);
        goto done;
    }
    if (rename(signature_tmp, signature_final) != 0 || rename(index_tmp, index_final) != 0 ||
        (index->sequence > highest_sequence &&
         store_highest_sequence(index->sequence, error, error_size) != 0)) {
        c1pkg_set_error(error, error_size, "commit verified index: %s", strerror(errno));
        goto done;
    }
    result = 0;
done:
    (void)unlink(index_tmp);
    (void)unlink(signature_tmp);
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
        snprintf(output, sizeof(output), "%s/staging/digest.%ld", C1PKG_STATE_ROOT,
                 (long)getpid()) < 0) {
        c1pkg_set_error(error, error_size, "invalid expected SHA-256");
        return -1;
    }
    (void)unlink(output);
    if (c1pkg_run(arguments, output, 4096U, error, error_size) != 0 ||
        c1pkg_read_file(output, &data, &size, 4096U, error, error_size) != 0) {
        c1pkg_set_error(error, error_size, "SHA-256 verification unavailable (sha256sum required)");
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