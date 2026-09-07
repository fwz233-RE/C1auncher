#ifndef C1_UPDATE_BOOT_H
#define C1_UPDATE_BOOT_H

#include "update/update.h"

#include <stddef.h>

#define C1_UPDATE_PENDING_BOOT_LIMIT 3U
/* Explicit boot identity entry point for isolated tests; production reads procfs. */
int c1_update_recover_boot(const char *state_root, const char *core_root,
                           const char *key_path, const char *boot_id,
                           char *error, size_t error_size);
int c1_update_recovery_exec(const char *state_root, const char *core_root,
                            const char *key_path, const char *which,
                            const char *ready_file, const char *launcher_path,
                            char *error, size_t error_size);

int c1_update_validate_release(const char *core_root, const char *target,
                               const char *key_path,
                               const struct c1_update_state *identity,
                               char *error, size_t error_size);
int c1_update_validate_current(const char *core_root, const char *key_path,
                               const struct c1_update_state *identity,
                               char *error, size_t error_size);
int c1_update_activate(const char *state_root, const char *core_root,
                       const char *key_path, char *error, size_t error_size);
int c1_update_bootstrap_activate(const char *state_root, const char *core_root,
                                 const char *key_path, char *error, size_t error_size);
int c1_update_recover(const char *state_root, const char *core_root,
                      const char *key_path, char *error, size_t error_size);
int c1_update_confirm(const char *state_root, const char *core_root,
                      const char *key_path, char *error, size_t error_size);
int c1_update_rollback(const char *state_root, const char *core_root,
                       const char *key_path, char *error, size_t error_size);
int c1_update_mark_ready(const char *state_root, const char *ready_file,
                         char *error, size_t error_size);
int c1_update_ready_matches(const char *ready_file, const char *digest,
                            char *error, size_t error_size);

#endif