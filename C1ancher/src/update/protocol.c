#include "update/update.h"
#include "security/secure_file.h"

#include "security/sha256.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* Only canonical numeric major.minor.patch capability versions are accepted.
 * A missing protected record means the deployed legacy bootstrap (1.0.0),
 * never the new version compiled into a downloadable updater. */
static int version_parts(const char *text, unsigned long parts[3])
{
    size_t i;
    if (text == NULL) return -1;
    for (i = 0; i < 3; ++i) {
        unsigned long value = 0;
        const char *start = text;
        if (*text < '0' || *text > '9') return -1;
        while (*text >= '0' && *text <= '9') {
            if (value > 1000000UL) return -1;
            value = value * 10UL + (unsigned long)(*text++ - '0');
        }
        if (text - start > 1 && *start == '0') return -1;
        parts[i] = value;
        if (i < 2) { if (*text++ != '.') return -1; }
        else if (*text != '\0') return -1;
    }
    return 0;
}

static int version_satisfies(const char *local, const char *minimum)
{
    unsigned long a[3], b[3];
    size_t i;
    if (version_parts(local, a) != 0 || version_parts(minimum, b) != 0) return 0;
    for (i = 0; i < 3; ++i) {
        if (a[i] != b[i]) return a[i] > b[i];
    }
    return 1;
}

int c1_update_check_compatibility_versions(const struct c1_update_manifest *manifest,
                                          const char *bootstrap, const char *updater,
                                          char *error, size_t error_size)
{
    if (manifest == NULL || !version_satisfies(bootstrap, manifest->min_bootstrap) ||
        !version_satisfies(updater, manifest->min_updater)) {
        c1_secure_set_error(error, error_size,
                            "release requires newer trusted bootstrap/updater; trusted maintenance required");
        return -1;
    }
    return 0;
}

int c1_update_check_compatibility(const struct c1_update_manifest *manifest,
                                 char *error, size_t error_size)
{
    const char *path = "/etc/c1updater/bootstrap.version";
    struct stat record, directory, script;
    unsigned char *data = NULL, hash[C1_SHA256_SIZE];
    char version[65], expected[65], actual[C1_SHA256_HEX_SIZE];
    size_t size = 0;
    int consumed = 0, result = -1;
    /* Legacy releases need only the original bootstrap contract. A missing or
     * damaged capability extension must not prevent rollback to that contract;
     * it can never grant a new capability. Updater minimum is still enforced. */
    if (c1_update_check_compatibility_versions(manifest, "1.0.0", C1_UPDATER_VERSION,
                                               NULL, 0) == 0) return 0;
    if (lstat(path, &record) != 0) {
        if (errno == ENOENT)
            return c1_update_check_compatibility_versions(manifest, "1.0.0", C1_UPDATER_VERSION,
                                                           error, error_size);
        goto done;
    }
    if (lstat("/etc/c1updater", &directory) != 0 || !S_ISDIR(directory.st_mode) ||
        directory.st_uid != 0 || (directory.st_mode & 0022U) != 0 ||
        !S_ISREG(record.st_mode) || record.st_uid != 0 || record.st_nlink != 1 ||
        (record.st_mode & 0777U) != 0600U ||
        lstat("/etc/app_daemon", &script) != 0 || !S_ISREG(script.st_mode) ||
        script.st_uid != 0 || (script.st_mode & 0022U) != 0 ||
        c1_secure_read_file(path, &data, &size, 132U, C1_SECURE_FILE_ANY_SIZE,
                            error, error_size) != 0 ||
        sscanf((const char *)data, "%64s %64s%n", version, expected, &consumed) != 2 ||
        consumed < 0 || (size_t)consumed + 1U != size || data[consumed] != '\n' ||
        c1_sha256_file("/etc/app_daemon", 65536U, C1_SHA256_ANY_SIZE, hash, NULL,
                       error, error_size) != 0) goto done;
    c1_sha256_hex(hash, actual);
    if (strcmp(expected, actual) != 0) goto done;
    result = c1_update_check_compatibility_versions(manifest, version, C1_UPDATER_VERSION,
                                                    error, error_size);
done:
    free(data);
    if (result != 0) c1_secure_set_error(error, error_size, "trusted bootstrap capability record or compatibility rejected");
    return result;
}

static int parse_u64(const char *text, uint64_t *value, int nonzero)
{
    uint64_t result = 0U;
    size_t i;

    if (text == NULL || value == NULL || text[0] == '\0' ||
        (text[0] == '0' && text[1] != '\0')) {
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
    if (nonzero != 0 && result == 0U) {
        return -1;
    }
    *value = result;
    return 0;
}

static int safe_token(const char *text)
{
    size_t i;
    size_t length;

    if (text == NULL) {
        return 0;
    }
    length = strlen(text);
    if (length == 0U || length > C1_UPDATE_TOKEN_MAX) {
        return 0;
    }
    for (i = 0U; i < length; ++i) {
        unsigned char character = (unsigned char)text[i];
        int alphanumeric = (character >= (unsigned char)'a' && character <= (unsigned char)'z') ||
                           (character >= (unsigned char)'A' && character <= (unsigned char)'Z') ||
                           (character >= (unsigned char)'0' && character <= (unsigned char)'9');
        int punctuation = character == (unsigned char)'.' || character == (unsigned char)'_' ||
                          character == (unsigned char)'-' || character == (unsigned char)'+';
        if ((!alphanumeric && !punctuation) ||
            ((i == 0U || i + 1U == length) && !alphanumeric)) {
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

static int parse_tagged_token(const char *line, char tag, char *destination)
{
    if (line[0] != tag || line[1] != '\t' || !safe_token(line + 2U) ||
        strchr(line + 2U, '\t') != NULL) {
        return -1;
    }
    (void)strcpy(destination, line + 2U);
    return 0;
}

static int parse_tagged_u64(const char *line, char tag, uint64_t *value)
{
    if (line[0] != tag || line[1] != '\t' || strchr(line + 2U, '\t') != NULL) {
        return -1;
    }
    return parse_u64(line + 2U, value, 1);
}

static int split_fields(char *line, char **fields, size_t expected)
{
    size_t count = 1U;
    char *cursor;

    fields[0] = line;
    for (cursor = line; *cursor != '\0'; ++cursor) {
        if (*cursor == '\t') {
            if (count >= expected) {
                return -1;
            }
            *cursor = '\0';
            fields[count++] = cursor + 1;
        }
    }
    return count == expected ? 0 : -1;
}

static int parse_component(char *line, size_t index,
                           struct c1_update_component *component)
{
    static const char *const roles[C1_UPDATE_COMPONENT_COUNT] = {
        "c1ancher", "c1pkg", "launcher", "updater"
    };
    static const char *const paths[C1_UPDATE_COMPONENT_COUNT] = {
        "artifacts/C1ancher", "artifacts/c1pkg",
        "artifacts/C1ancher-launcher", "artifacts/c1updater"
    };
    char *fields[6];

    if (split_fields(line, fields, 6U) != 0 || strcmp(fields[0], "F") != 0 ||
        strcmp(fields[1], roles[index]) != 0 || strcmp(fields[2], paths[index]) != 0 ||
        !valid_sha256(fields[3]) || parse_u64(fields[4], &component->size, 1) != 0 ||
        strcmp(fields[5], "700") != 0) {
        return -1;
    }
    (void)strcpy(component->role, fields[1]);
    (void)strcpy(component->path, fields[2]);
    (void)strcpy(component->sha256, fields[3]);
    component->mode = 0700U;
    return 0;
}

static int parse_line(size_t line_number, char *line,
                      struct c1_update_manifest *manifest)
{
    switch (line_number) {
    case 0U:
        return strcmp(line, "C1CORE-MANIFEST 1") == 0 ? 0 : -1;
    case 1U:
        return parse_tagged_u64(line, 'S', &manifest->sequence);
    case 2U:
        return parse_tagged_token(line, 'V', manifest->version);
    case 3U:
        return parse_tagged_u64(line, 'E', &manifest->security_epoch);
    case 4U:
        if (strncmp(line, "T\t", 2U) != 0 || strcmp(line + 2U, C1_UPDATE_TARGET) != 0) {
            return -1;
        }
        (void)strcpy(manifest->target, line + 2U);
        return 0;
    case 5U:
        return parse_tagged_token(line, 'B', manifest->min_bootstrap);
    case 6U:
        return parse_tagged_token(line, 'U', manifest->min_updater);
    case 7U:
        return parse_tagged_token(line, 'C', manifest->compatibility);
    case 8U:
        return parse_tagged_token(line, 'R', manifest->source_revision);
    case 9U:
        return parse_tagged_u64(line, 'D', &manifest->source_date_epoch);
    default:
        return parse_component(line, line_number - 10U,
                               &manifest->components[line_number - 10U]);
    }
}

int c1_update_parse_manifest(const unsigned char *data, size_t size,
                             struct c1_update_manifest *manifest,
                             char *error, size_t error_size)
{
    char line[C1_UPDATE_LINE_MAX + 1U];
    size_t offset = 0U;
    size_t line_number = 0U;
    uint64_t payload_size = 0U;

    if (data == NULL || manifest == NULL || size == 0U || size > C1_UPDATE_MANIFEST_MAX ||
        data[size - 1U] != (unsigned char)'\n') {
        c1_secure_set_error(error, error_size, "invalid manifest envelope");
        return -1;
    }
    (void)memset(manifest, 0, sizeof(*manifest));
    while (offset < size) {
        size_t end = offset;
        size_t length;
        while (end < size && data[end] != (unsigned char)'\n') {
            unsigned char character = data[end];
            if ((character < 0x20U && character != (unsigned char)'\t') ||
                character > 0x7eU) {
                c1_secure_set_error(error, error_size, "manifest contains forbidden bytes");
                return -1;
            }
            ++end;
        }
        length = end - offset;
        if (end == size || length == 0U || length > C1_UPDATE_LINE_MAX || line_number >= 14U) {
            c1_secure_set_error(error, error_size, "invalid manifest line structure");
            return -1;
        }
        (void)memcpy(line, data + offset, length);
        line[length] = '\0';
        if (parse_line(line_number, line, manifest) != 0) {
            c1_secure_set_error(error, error_size, "manifest field rejected");
            return -1;
        }
        if (line_number >= 10U) {
            uint64_t component_size = manifest->components[line_number - 10U].size;
            if (component_size > C1_UPDATE_COMPONENT_MAX ||
                payload_size > C1_UPDATE_PAYLOAD_MAX - component_size) {
                c1_secure_set_error(error, error_size, "manifest payload size rejected");
                return -1;
            }
            payload_size += component_size;
        }
        ++line_number;
        offset = end + 1U;
    }
    if (line_number != 14U) {
        c1_secure_set_error(error, error_size, "manifest record count rejected");
        return -1;
    }
    return 0;
}