#include "pkg.h"

#include <ctype.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
#include <poll.h>
#include <time.h>
#include <unistd.h>

uint64_t c1pkg_input_now_ms(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0U;
    return (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
}

/* Read exactly one key, never discard the rest of a burst (e.g. PI + Enter).
 * Escape sequences are consumed through their final byte, including unknown
 * CSI keys, so navigation bytes cannot become a name prefix. */
int c1pkg_input_read(int fd, int timeout_ms)
{
    struct pollfd input = {fd, POLLIN, 0};
    unsigned char byte;
    int ready = poll(&input, 1U, timeout_ms);
    if (ready < 0) return errno == EINTR ? C1PKG_KEY_NONE : C1PKG_KEY_EOF;
    if (ready == 0) return C1PKG_KEY_NONE;
    if (!(input.revents & POLLIN)) return C1PKG_KEY_EOF;
    if (read(fd, &byte, 1U) != 1) return C1PKG_KEY_EOF;
    if (byte == 27U) {
        unsigned char sequence[16];
        size_t count = 0U;
        if (poll(&input, 1U, 40) <= 0 || read(fd, &byte, 1U) != 1)
            return C1PKG_KEY_BACK;
        if (byte != '[' && byte != 'O') return C1PKG_KEY_NONE;
        while (count < sizeof(sequence) && poll(&input, 1U, 40) > 0) {
            if (read(fd, &sequence[count], 1U) != 1) break;
            byte = sequence[count++];
            if (byte >= 0x40U && byte <= 0x7eU) break;
        }
        if (count == 1U) {
            if (sequence[0] == 'A') return C1PKG_KEY_UP;
            if (sequence[0] == 'B') return C1PKG_KEY_DOWN;
            if (sequence[0] == 'C') return C1PKG_KEY_RIGHT;
            if (sequence[0] == 'D') return C1PKG_KEY_LEFT;
        }
        if (count == 2U && sequence[1] == '~') {
            if (sequence[0] == '5') return C1PKG_KEY_LEFT;
            if (sequence[0] == '6') return C1PKG_KEY_RIGHT;
        }
        return C1PKG_KEY_NONE;
    }
    if (byte == '\r' || byte == '\n') return C1PKG_KEY_ENTER;
    if (byte == ' ') return C1PKG_KEY_REFRESH;
    if (byte == 8U || byte == 127U) return C1PKG_KEY_ERASE;
    if (byte == 21U) return C1PKG_KEY_CLEAR; /* Ctrl-U */
    if (byte == 3U) return C1PKG_KEY_QUIT;
    return byte >= 33U ? (int)byte : C1PKG_KEY_NONE;
}

void c1pkg_prefix_clear(struct c1pkg_prefix *prefix)
{
    prefix->text[0] = '\0';
    prefix->updated_ms = 0U;
}

int c1pkg_prefix_expire(struct c1pkg_prefix *prefix, uint64_t now_ms)
{
    if (prefix->text[0] != '\0' &&
        (now_ms < prefix->updated_ms || now_ms - prefix->updated_ms >= C1PKG_PREFIX_TIMEOUT_MS)) {
        c1pkg_prefix_clear(prefix);
        return 1;
    }
    return 0;
}

int c1pkg_prefix_input(struct c1pkg_prefix *prefix, int key, uint64_t now_ms)
{
    size_t length;
    /* Commands must not expire or clear a prefix before the caller validates
     * Enter. Idle expiry is separately displayed by the GUI/TUI. */
    if (key != C1PKG_KEY_ERASE && key != C1PKG_KEY_CLEAR && (key < 33 || key > 126)) return 0;
    (void)c1pkg_prefix_expire(prefix, now_ms);
    length = strlen(prefix->text);
    if (key == C1PKG_KEY_ERASE) {
        if (length > 0U) prefix->text[length - 1U] = '\0';
    } else if (key == C1PKG_KEY_CLEAR) {
        c1pkg_prefix_clear(prefix);
    } else if (key >= 33 && key <= 126) {
        if (length < C1PKG_NAME_MAX) {
            prefix->text[length] = (char)(key >= 'A' && key <= 'Z' ? key + ('a' - 'A') : key);
            prefix->text[length + 1U] = '\0';
        }
    } else return 0;
    prefix->updated_ms = now_ms;
    return 1;
}

static int starts_with_ascii(const char *value, const char *prefix)
{
    if (value == NULL) return 0;
    while (*prefix != '\0') {
        unsigned char byte = (unsigned char)*value++;
        if (byte >= 'A' && byte <= 'Z') byte += 'a' - 'A';
        if (byte != (unsigned char)*prefix++) return 0;
    }
    return 1;
}

int c1pkg_prefix_match_rank(const struct c1pkg_prefix *prefix, const char *name, const char *id)
{
    size_t length = strlen(prefix->text);
    int id_matches, name_matches;
    if (length == 0U) return 0;
    id_matches = starts_with_ascii(id, prefix->text);
    name_matches = starts_with_ascii(name, prefix->text);
    if (id_matches && strlen(id) == length) return 3;
    if (name_matches && strlen(name) == length) return 2;
    return id_matches || name_matches ? 1 : 0;
}

int c1pkg_prefix_matches(const struct c1pkg_prefix *prefix, const char *name, const char *id)
{
    return c1pkg_prefix_match_rank(prefix, name, id) != 0;
}


struct semver_view {
    const char *major;
    size_t major_length;
    const char *minor;
    size_t minor_length;
    const char *patch;
    size_t patch_length;
    const char *prerelease;
    size_t prerelease_length;
};

static int parse_numeric_component(const char **cursor, const char *end,
                                   const char **value, size_t *length,
                                   char terminator)
{
    const char *start = *cursor;

    while (*cursor < end && isdigit((unsigned char)**cursor)) {
        ++*cursor;
    }
    *value = start;
    *length = (size_t)(*cursor - start);
    if (*length == 0U || (*length > 1U && start[0] == '0')) {
        return -1;
    }
    if (terminator != '\0') {
        if (*cursor >= end || **cursor != terminator) {
            return -1;
        }
        ++*cursor;
    }
    return 0;
}

static int validate_identifiers(const char *value, size_t length, int prerelease)
{
    size_t offset = 0U;

    if (length == 0U) {
        return -1;
    }
    while (offset < length) {
        size_t start = offset;
        int numeric = 1;

        while (offset < length && value[offset] != '.') {
            unsigned char character = (unsigned char)value[offset];
            if (!isalnum(character) && character != (unsigned char)'-') {
                return -1;
            }
            if (!isdigit(character)) {
                numeric = 0;
            }
            ++offset;
        }
        if (offset == start ||
            (prerelease != 0 && numeric != 0 && offset - start > 1U && value[start] == '0')) {
            return -1;
        }
        if (offset < length) {
            ++offset;
            if (offset == length) {
                return -1;
            }
        }
    }
    return 0;
}

static int parse_semver(const char *value, struct semver_view *view)
{
    const char *cursor;
    const char *end;
    const char *suffix;

    if (value == NULL || view == NULL || value[0] == '\0') {
        return -1;
    }
    (void)memset(view, 0, sizeof(*view));
    cursor = value;
    end = value + strlen(value);
    if (parse_numeric_component(&cursor, end, &view->major, &view->major_length, '.') != 0 ||
        parse_numeric_component(&cursor, end, &view->minor, &view->minor_length, '.') != 0 ||
        parse_numeric_component(&cursor, end, &view->patch, &view->patch_length, '\0') != 0) {
        return -1;
    }
    if (cursor < end && *cursor != '-' && *cursor != '+') {
        return -1;
    }
    if (cursor < end && *cursor == '-') {
        ++cursor;
        suffix = cursor;
        while (cursor < end && *cursor != '+') {
            ++cursor;
        }
        view->prerelease = suffix;
        view->prerelease_length = (size_t)(cursor - suffix);
        if (validate_identifiers(view->prerelease, view->prerelease_length, 1) != 0) {
            return -1;
        }
    }
    if (cursor < end && *cursor == '+') {
        ++cursor;
        if (validate_identifiers(cursor, (size_t)(end - cursor), 0) != 0) {
            return -1;
        }
        cursor = end;
    }
    return cursor == end ? 0 : -1;
}

static int compare_text_number(const char *left, size_t left_length,
                               const char *right, size_t right_length)
{
    int result;

    if (left_length != right_length) {
        return left_length < right_length ? -1 : 1;
    }
    result = memcmp(left, right, left_length);
    return result < 0 ? -1 : result > 0 ? 1 : 0;
}

static int identifier_is_numeric(const char *value, size_t length)
{
    size_t index;

    for (index = 0U; index < length; ++index) {
        if (!isdigit((unsigned char)value[index])) {
            return 0;
        }
    }
    return 1;
}

static int compare_identifier(const char *left, size_t left_length,
                              const char *right, size_t right_length)
{
    int left_numeric = identifier_is_numeric(left, left_length);
    int right_numeric = identifier_is_numeric(right, right_length);
    size_t common;
    int result;

    if (left_numeric != 0 && right_numeric != 0) {
        return compare_text_number(left, left_length, right, right_length);
    }
    if (left_numeric != right_numeric) {
        return left_numeric != 0 ? -1 : 1;
    }
    common = left_length < right_length ? left_length : right_length;
    result = memcmp(left, right, common);
    if (result != 0) {
        return result < 0 ? -1 : 1;
    }
    return left_length < right_length ? -1 : left_length > right_length ? 1 : 0;
}

static int compare_prerelease(const struct semver_view *left,
                              const struct semver_view *right)
{
    size_t left_offset = 0U;
    size_t right_offset = 0U;

    if (left->prerelease_length == 0U || right->prerelease_length == 0U) {
        if (left->prerelease_length == right->prerelease_length) {
            return 0;
        }
        return left->prerelease_length == 0U ? 1 : -1;
    }
    while (left_offset < left->prerelease_length &&
           right_offset < right->prerelease_length) {
        size_t left_end = left_offset;
        size_t right_end = right_offset;
        int result;

        while (left_end < left->prerelease_length && left->prerelease[left_end] != '.') {
            ++left_end;
        }
        while (right_end < right->prerelease_length && right->prerelease[right_end] != '.') {
            ++right_end;
        }
        result = compare_identifier(left->prerelease + left_offset, left_end - left_offset,
                                    right->prerelease + right_offset, right_end - right_offset);
        if (result != 0) {
            return result;
        }
        left_offset = left_end < left->prerelease_length ? left_end + 1U : left_end;
        right_offset = right_end < right->prerelease_length ? right_end + 1U : right_end;
    }
    if (left_offset == left->prerelease_length && right_offset == right->prerelease_length) {
        return 0;
    }
    return left_offset == left->prerelease_length ? -1 : 1;
}

/* Repository versions use two to four numeric components. Missing trailing
 * components compare as zero. Keep legacy three-component prerelease support
 * below so installed older releases retain their established ordering. */
static int parse_repository_version(const char *value, const char **parts, size_t *lengths)
{
    const char *cursor = value;
    size_t count = 0U, i;
    if (value == NULL || strlen(value) > C1PKG_VERSION_MAX) return -1;
    for (i = 0U; i < 4U; ++i) { parts[i] = "0"; lengths[i] = 1U; }
    while (*cursor != '\0' && count < 4U) {
        const char *start = cursor;
        while (isdigit((unsigned char)*cursor)) ++cursor;
        if (cursor == start || (cursor - start > 1 && start[0] == '0')) return -1;
        parts[count] = start;
        lengths[count++] = (size_t)(cursor - start);
        if (*cursor == '\0') break;
        if (*cursor++ != '.' || *cursor == '\0') return -1;
    }
    return *cursor == '\0' && count >= 2U ? 0 : -1;
}

int c1pkg_version_compare(const char *left, const char *right, int *comparison)
{
    const char *left_parts[4], *right_parts[4];
    size_t left_lengths[4], right_lengths[4], i;
    struct semver_view left_view;
    struct semver_view right_view;
    int result;

    if (comparison != NULL &&
        parse_repository_version(left, left_parts, left_lengths) == 0 &&
        parse_repository_version(right, right_parts, right_lengths) == 0) {
        result = 0;
        for (i = 0U; i < 4U && result == 0; ++i) {
            result = compare_text_number(left_parts[i], left_lengths[i], right_parts[i], right_lengths[i]);
        }
        *comparison = result;
        return 0;
    }
    if (comparison == NULL || parse_semver(left, &left_view) != 0 ||
        parse_semver(right, &right_view) != 0) {
        return -1;
    }
    result = compare_text_number(left_view.major, left_view.major_length,
                                 right_view.major, right_view.major_length);
    if (result == 0) {
        result = compare_text_number(left_view.minor, left_view.minor_length,
                                     right_view.minor, right_view.minor_length);
    }
    if (result == 0) {
        result = compare_text_number(left_view.patch, left_view.patch_length,
                                     right_view.patch, right_view.patch_length);
    }
    if (result == 0) {
        result = compare_prerelease(&left_view, &right_view);
    }
    *comparison = result;
    return 0;
}

int c1pkg_install_decision(const char *installed, const char *available)
{
    int comparison;
    if (available == NULL || available[0] == '\0') return -1;
    if (installed == NULL || installed[0] == '\0') return 1;
    if (strcmp(installed, available) == 0) return 0;
    if (c1pkg_version_compare(available, installed, &comparison) != 0) return -1;
    return comparison > 0 ? 1 : comparison == 0 ? 0 : -1;
}

static enum c1pkg_download_status installed_status(const char *installed,
                                                    const char *available)
{
    int comparison;

    if (strcmp(installed, available) == 0) {
        return C1PKG_DOWNLOAD_CURRENT;
    }
    if (c1pkg_version_compare(available, installed, &comparison) != 0) {
        return C1PKG_DOWNLOAD_VERSION_UNKNOWN;
    }
    if (comparison == 0) {
        return C1PKG_DOWNLOAD_CURRENT;
    }
    return comparison > 0 ? C1PKG_DOWNLOAD_UPDATE_AVAILABLE :
                            C1PKG_DOWNLOAD_INSTALLED_NEWER;
}

static void add_repository_item(struct c1pkg_download_list *list,
                                const struct c1pkg_package *package,
                                size_t package_index,
                                const struct c1pkg_installed *installed)
{
    struct c1pkg_download_item *item = &list->items[list->count++];

    (void)memset(item, 0, sizeof(*item));
    (void)strcpy(item->id, package->id);
    (void)strcpy(item->name, package->name);
    (void)strcpy(item->author, package->author[0] != '\0' ? package->author : "Unknown");
    (void)strcpy(item->available_version, package->version);
    item->package_index = package_index;
    if (installed == NULL) {
        item->status = C1PKG_DOWNLOAD_NOT_INSTALLED;
    } else {
        (void)strcpy(item->installed_version, installed->version);
        item->status = installed_status(installed->version, package->version);
    }
}

static void add_removed_item(struct c1pkg_download_list *list,
                             const struct c1pkg_installed *installed)
{
    struct c1pkg_download_item *item = &list->items[list->count++];

    (void)memset(item, 0, sizeof(*item));
    (void)strcpy(item->id, installed->id);
    (void)strcpy(item->name, installed->id);
    (void)strcpy(item->author, "Unknown");
    (void)strcpy(item->installed_version, installed->version);
    item->package_index = C1PKG_PACKAGE_NONE;
    item->status = C1PKG_DOWNLOAD_REMOVED;
}

int c1pkg_download_list_build(const struct c1pkg_index *index,
                              const struct c1pkg_installed_list *installed,
                              struct c1pkg_download_list *list)
{
    size_t package_index = 0U;
    size_t installed_index = 0U;

    if (index == NULL || installed == NULL || list == NULL) {
        return -1;
    }
    list->count = 0U;
    while (package_index < index->count || installed_index < installed->count) {
        int order;

        if (list->count >= C1PKG_MAX_DOWNLOAD_ITEMS) {
            return -1;
        }
        if (package_index >= index->count) {
            add_removed_item(list, &installed->items[installed_index++]);
            continue;
        }
        if (installed_index >= installed->count) {
            add_repository_item(list, &index->packages[package_index], package_index, NULL);
            ++package_index;
            continue;
        }
        order = strcmp(index->packages[package_index].id,
                       installed->items[installed_index].id);
        if (order < 0) {
            add_repository_item(list, &index->packages[package_index], package_index, NULL);
            ++package_index;
        } else if (order > 0) {
            add_removed_item(list, &installed->items[installed_index++]);
        } else {
            add_repository_item(list, &index->packages[package_index], package_index,
                                &installed->items[installed_index]);
            ++package_index;
            ++installed_index;
        }
    }
    return 0;
}
