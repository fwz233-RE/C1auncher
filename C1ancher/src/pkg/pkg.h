#ifndef C1PKG_H
#define C1PKG_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define C1PKG_REPO_DEFAULT "http://www.fwz233.com/c1/v2"
#define C1PKG_KEY_DEFAULT "/usr/data/c1/pkg/repository.ed25519.pub"
#ifndef C1PKG_STATE_ROOT
#define C1PKG_STATE_ROOT "/usr/data/c1/pkg"
#endif
#define C1PKG_REPO_FILE C1PKG_STATE_ROOT "/repository.url"
#define C1PKG_STORAGE_MOUNT "/storage"
#define C1PKG_STORAGE_ROOT "/storage/c1"
#ifndef C1PKG_APPS_ROOT
#define C1PKG_APPS_ROOT "/storage/c1/apps"
#endif
#define C1PKG_WORK_ROOT "/storage/c1/pkg"
#ifndef C1PKG_STAGING_ROOT
#define C1PKG_STAGING_ROOT "/storage/c1/pkg/staging"
#endif
#ifndef C1PKG_TRASH_ROOT
#define C1PKG_TRASH_ROOT "/storage/c1/pkg/trash"
#endif
#define C1PKG_INDEX_MAX (256U * 1024U)
#define C1PKG_PACKAGE_MAX (32U * 1024U * 1024U)
#define C1PKG_FILE_MAX (16U * 1024U * 1024U)
#define C1PKG_UNPACKED_MAX (64U * 1024U * 1024U)
#define C1PKG_MAX_PACKAGES 128U
#define C1PKG_MAX_DOWNLOAD_ITEMS (C1PKG_MAX_PACKAGES * 2U)
#define C1PKG_PACKAGE_NONE C1PKG_MAX_PACKAGES
#define C1PKG_PATH_MAX 4096U
#define C1PKG_ID_MAX 32U
#define C1PKG_VERSION_MAX 48U
#define C1PKG_NAME_MAX 40U
#define C1PKG_ENTRY_MAX 192U
#define C1PKG_ERROR_MAX 256U

struct c1pkg_package {
    char id[C1PKG_ID_MAX + 1U];
    char version[C1PKG_VERSION_MAX + 1U];
    char name[C1PKG_NAME_MAX + 1U];
    char author[C1PKG_NAME_MAX + 1U];
    char archive[C1PKG_ENTRY_MAX + 1U];
    char entry[C1PKG_ENTRY_MAX + 1U];
    char sha256[65];
    uint64_t size;
};

struct c1pkg_index {
    uint64_t sequence;
    struct c1pkg_package packages[C1PKG_MAX_PACKAGES];
    size_t count;
};

struct c1pkg_installed {
    char id[C1PKG_ID_MAX + 1U];
    char version[C1PKG_VERSION_MAX + 1U];
};

struct c1pkg_installed_list {
    struct c1pkg_installed items[C1PKG_MAX_PACKAGES];
    size_t count;
};

enum c1pkg_download_status {
    C1PKG_DOWNLOAD_NOT_INSTALLED,
    C1PKG_DOWNLOAD_CURRENT,
    C1PKG_DOWNLOAD_UPDATE_AVAILABLE,
    C1PKG_DOWNLOAD_INSTALLED_NEWER,
    C1PKG_DOWNLOAD_VERSION_UNKNOWN,
    C1PKG_DOWNLOAD_REMOVED
};

struct c1pkg_download_item {
    char id[C1PKG_ID_MAX + 1U];
    char name[C1PKG_NAME_MAX + 1U];
    char author[C1PKG_NAME_MAX + 1U];
    char installed_version[C1PKG_VERSION_MAX + 1U];
    char available_version[C1PKG_VERSION_MAX + 1U];
    size_t package_index;
    enum c1pkg_download_status status;
};

struct c1pkg_download_list {
    struct c1pkg_download_item items[C1PKG_MAX_DOWNLOAD_ITEMS];
    size_t count;
};

struct c1pkg_config {
    const char *repo_base;
    const char *public_key;
};

int c1pkg_safe_id(const char *value);
int c1pkg_safe_version(const char *value);
int c1pkg_safe_relpath(const char *value);
int c1pkg_join(char *out, size_t out_size, const char *left, const char *right);
int c1pkg_mkdir_p(const char *path, mode_t mode, char *error, size_t error_size);
int c1pkg_remove_tree(const char *path, char *error, size_t error_size);
int c1pkg_read_file(const char *path, unsigned char **data, size_t *size,
                    size_t limit, char *error, size_t error_size);
int c1pkg_write_file(const char *path, const void *data, size_t size, mode_t mode,
                     char *error, size_t error_size);
int c1pkg_sync_directory(const char *path, char *error, size_t error_size);
int c1pkg_run(char *const argv[], const char *stdout_path, uint64_t file_limit,
              char *error, size_t error_size);
const char *c1pkg_helper(const char *absolute, const char *name);
/* Internal service packages remain installable but are omitted from user-facing
 * application lists and desktop application counts. */
int c1pkg_is_internal_id(const char *id);
void c1pkg_set_error(char *error, size_t error_size, const char *format, ...);

/* Progress callbacks return nonzero to cancel; NULL messages only poll input. */
void c1pkg_set_progress(int (*callback)(const char *, void *), void *context);
int c1pkg_progress(const char *message);
int c1pkg_repo_read_url(const char *path, char *url, size_t url_size,
                        char *error, size_t error_size);
int c1pkg_repo_parse(const unsigned char *data, size_t size, struct c1pkg_index *index,
                     char *error, size_t error_size);
/* Pure configuration binding: no I/O, no wait. GUI parents receiving indexes
 * from forked workers should call this on initialization/config changes, before
 * installing, to disambiguate nested repositories on one origin. refresh and
 * load_cached bind internally. Deadlines are ALWAYS loaded from private durable
 * state, never inherited from this hint. Returns -1 for ambiguous/unsafe URL. */
int c1pkg_repo_bind_transport(const struct c1pkg_config *config);
int c1pkg_repo_refresh(const struct c1pkg_config *config, struct c1pkg_index *index,
                       char *error, size_t error_size);
/* Verify a local signed index in memory without changing online cache/sequence.
 * Returns an owned repository directory fd (close it), or -1. Keep this fd
 * through store_install_local so a renamed source cannot redirect the root. */
int c1pkg_repo_open_local(const struct c1pkg_config *config, const char *directory,
                          struct c1pkg_index *index, char *error, size_t error_size);
int c1pkg_repo_load_cached(const struct c1pkg_config *config, struct c1pkg_index *index,
                           char *error, size_t error_size);
const struct c1pkg_package *c1pkg_repo_find(const struct c1pkg_index *index,
                                            const char *id);
int c1pkg_verify_sha256(const char *path, const char *expected,
                        char *error, size_t error_size);
int c1pkg_fetch(const char *url, const char *output, uint64_t limit,
                char *error, size_t error_size);

/* Install results are machine-readable; batch callers stop only on CANCELLED
 * or STORAGE. SKIPPED is a safe no-op, including legacy retained payloads. */
enum c1pkg_install_result {
    C1PKG_INSTALL_OK = 0,
    C1PKG_INSTALL_SKIPPED = 1,
    C1PKG_INSTALL_PACKAGE_ERROR = -1,
    C1PKG_INSTALL_CANCELLED = -2,
    C1PKG_INSTALL_STORAGE_ERROR = -3
};

int c1pkg_store_init(char *error, size_t error_size);
int c1pkg_store_list(struct c1pkg_installed_list *list, char *error, size_t error_size);
int c1pkg_store_install(const struct c1pkg_config *config,
                        const struct c1pkg_package *package,
                        char *error, size_t error_size);
/* Only pass a package from the index verified by repo_open_local and its fd.
 * An already-current version is SKIPPED before archive I/O, just as online.
 * Callers must verify the index even for this no-op; it is not a repair. */
int c1pkg_store_install_local(int repository_fd, const struct c1pkg_package *package,
                              char *error, size_t error_size);
int c1pkg_store_remove(const char *id, char *error, size_t error_size);
int c1pkg_store_rollback(const char *id, char *error, size_t error_size);
int c1pkg_app_uses_direct_io(const char *id);
int c1pkg_store_launch(const char *id, char *const extra_argv[],
                       char *error, size_t error_size);
int c1pkg_store_is_installed(const struct c1pkg_installed_list *list,
                             const char *id, const char **version);

/* 1 = install/update, 0 = current, -1 = downgrade/unordered version. */
int c1pkg_install_decision(const char *installed, const char *available);
int c1pkg_version_compare(const char *left, const char *right, int *comparison);
int c1pkg_download_list_build(const struct c1pkg_index *index,
                              const struct c1pkg_installed_list *installed,
                              struct c1pkg_download_list *list);

/* Shared terminal input. Printable UTF-8 bytes are returned unchanged;
 * commands are outside the byte range so no application-name byte is a list shortcut. */
enum c1pkg_input_key {
    C1PKG_KEY_NONE = 256, C1PKG_KEY_UP, C1PKG_KEY_DOWN,
    C1PKG_KEY_LEFT, C1PKG_KEY_RIGHT, C1PKG_KEY_ENTER,
    C1PKG_KEY_BACK, C1PKG_KEY_REFRESH, C1PKG_KEY_ERASE,
    C1PKG_KEY_CLEAR, C1PKG_KEY_EOF, C1PKG_KEY_QUIT
};
#define C1PKG_PREFIX_TIMEOUT_MS 1500U
struct c1pkg_prefix {
    char text[C1PKG_NAME_MAX + 1U];
    uint64_t updated_ms;
};
int c1pkg_input_read(int fd, int timeout_ms);
uint64_t c1pkg_input_now_ms(void);
void c1pkg_prefix_clear(struct c1pkg_prefix *prefix);
int c1pkg_prefix_expire(struct c1pkg_prefix *prefix, uint64_t now_ms);
int c1pkg_prefix_input(struct c1pkg_prefix *prefix, int key, uint64_t now_ms);
/* Higher rank wins: exact ID, exact display name, then a prefix match. */
int c1pkg_prefix_match_rank(const struct c1pkg_prefix *prefix, const char *name,
                            const char *id);
int c1pkg_prefix_matches(const struct c1pkg_prefix *prefix, const char *name,
                         const char *id);

int c1pkg_tui(const struct c1pkg_config *config);

#endif
