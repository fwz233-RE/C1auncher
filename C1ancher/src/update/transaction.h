#ifndef C1_UPDATE_TRANSACTION_H
#define C1_UPDATE_TRANSACTION_H

#include "update/update.h"

#include <stddef.h>
#include <stdint.h>

#define C1_UPDATE_SPACE_RESERVE (4U * 1024U * 1024U)

typedef int (*c1_update_space_check_fn)(const char *path, uint64_t required,
                                        void *context, char *error, size_t error_size);

struct c1_update_transaction_config {
    const char *staging_root;
    const char *core_root;
    const char *state_root;
    const char *key_path;
    c1_update_space_check_fn space_check;
    void *space_context;
};

struct c1_update_transaction_result {
    /* On success: PREPARED needs confirmation; CONFIRMED is up-to-date and
     * must not trigger activation or restart. No state/artifacts are changed
     * for an exact, revalidated confirmed release. */
    enum c1_update_phase phase;
    uint64_t sequence;
    char release[C1_UPDATE_TOKEN_MAX + 1U];
};

int c1_update_prepare_local(const struct c1_update_transaction_config *config,
                            const char *release_directory,
                            struct c1_update_transaction_result *result,
                            char *error, size_t error_size);
int c1_update_prepare(const struct c1_update_transaction_config *config,
                      const char *base_url,
                      struct c1_update_transaction_result *result,
                      char *error, size_t error_size);

#endif