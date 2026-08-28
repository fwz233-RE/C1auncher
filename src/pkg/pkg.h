#ifndef C1PKG_H
#define C1PKG_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define C1PKG_REPO_DEFAULT "http://120.26.180.48/c1/v1"
#define C1PKG_KEY_DEFAULT "/usr/data/c1/pkg/repository.ed25519.pub"
#define C1PKG_STATE_ROOT "/usr/data/c1/pkg"
#define C1PKG_APPS_ROOT "/usr/data/c1/apps"
#define C1PKG_INDEX_MAX (256U * 1024U)
#define C1PKG_PACKAGE_MAX (32U * 1024U * 1024U)
#define C1PKG_FILE_MAX (16U * 1024U * 1024U)
#define C1PKG_UNPACKED_MAX (64U * 1024U * 1024U)
#define C1PKG_MAX_PACKAGES 128U
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
int c1pkg_run(char *const argv[], const char *stdout_path, uint64_t file_limit,
              char *error, size_t error_size);
const char *c1pkg_helper(const char *absolute, const char *name);
void c1pkg_set_error(char *error, size_t error_size, const char *format, ...);

int c1pkg_repo_refresh(const struct c1pkg_config *config, struct c1pkg_index *index,
                       char *error, size_t error_size);
int c1pkg_repo_load_cached(const struct c1pkg_config *config, struct c1pkg_index *index,
                           char *error, size_t error_size);
const struct c1pkg_package *c1pkg_repo_find(const struct c1pkg_index *index,
                                            const char *id);
int c1pkg_verify_sha256(const char *path, const char *expected,
                        char *error, size_t error_size);
int c1pkg_fetch(const char *url, const char *output, uint64_t limit,
                char *error, size_t error_size);

int c1pkg_store_init(char *error, size_t error_size);
int c1pkg_store_list(struct c1pkg_installed_list *list, char *error, size_t error_size);
int c1pkg_store_install(const struct c1pkg_config *config,
                        const struct c1pkg_package *package,
                        char *error, size_t error_size);
int c1pkg_store_remove(const char *id, char *error, size_t error_size);
int c1pkg_store_rollback(const char *id, char *error, size_t error_size);
int c1pkg_store_launch(const char *id, char *const extra_argv[],
                       char *error, size_t error_size);
int c1pkg_store_is_installed(const struct c1pkg_installed_list *list,
                             const char *id, const char **version);

int c1pkg_tui(const struct c1pkg_config *config);

#endif