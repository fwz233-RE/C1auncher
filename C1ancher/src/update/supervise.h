#ifndef C1_UPDATE_SUPERVISE_H
#define C1_UPDATE_SUPERVISE_H

#include <stddef.h>

#define C1_UPDATER_FATAL_EXIT 71
#define C1_UPDATER_SLOT_SWITCH_EXIT 72
#define C1_UPDATER_TRANSIENT_EXIT 75

int c1_update_supervise(const char *state_root, const char *core_root,
                        const char *key_path, const char *ready_file,
                        const char *launcher_path,
                        char *error, size_t error_size);

#endif