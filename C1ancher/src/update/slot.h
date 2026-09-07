#ifndef C1_UPDATE_SLOT_H
#define C1_UPDATE_SLOT_H

#include <stddef.h>

int c1_update_release_requires_slot_switch(const char *prepared_release,
                                           const char *key_path,
                                           int *required,
                                           char *error, size_t error_size);
int c1_update_record_active_slot(const char *update_root, char active_slot,
                                 char *error, size_t error_size);
int c1_update_install_prepared_slot(const char *slot_root, char active_slot,
                                    const char *core_root,
                                    const char *state_root,
                                    const char *key_path, char *installed_slot,
                                    char *error, size_t error_size);
int c1_update_install_inactive_slot(const char *slot_root, char active_slot,
                                    const char *prepared_release,
                                    const char *key_path, char *installed_slot,
                                    char *error, size_t error_size);

#endif