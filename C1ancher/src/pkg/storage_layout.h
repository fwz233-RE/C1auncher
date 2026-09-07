#ifndef C1PKG_STORAGE_LAYOUT_H
#define C1PKG_STORAGE_LAYOUT_H

#include "pkg.h"

#include <stddef.h>
#include <stdint.h>

enum c1pkg_storage_access {
    C1PKG_STORAGE_READ,
    C1PKG_STORAGE_WRITE,
    /* Delete/recover without a free-space reserve or allocating a write probe. */
    C1PKG_STORAGE_MAINTENANCE
};

struct c1pkg_storage_layout {
    const char *system_root;
    const char *mount_root;
    const char *storage_root;
    const char *apps_root;
    const char *work_root;
    const char *staging_root;
    const char *trash_root;
    const char *state_root;
    int require_distinct_mount;
};

const struct c1pkg_storage_layout *c1pkg_storage_default_layout(void);
int c1pkg_storage_state_init_at(const struct c1pkg_storage_layout *layout,
                                char *error, size_t error_size);
int c1pkg_storage_prepare_at(const struct c1pkg_storage_layout *layout,
                             enum c1pkg_storage_access access,
                             uint64_t required_bytes,
                             char *error, size_t error_size);
int c1pkg_storage_state_init(char *error, size_t error_size);
int c1pkg_storage_prepare(enum c1pkg_storage_access access,
                          uint64_t required_bytes,
                          char *error, size_t error_size);

#endif