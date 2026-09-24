#ifndef C1_UPDATE_CHECK_H
#define C1_UPDATE_CHECK_H

#include "update/update.h"

/* fetch_release has a 45-second budget per origin (primary and fixed fallback).
 * The checking process additionally bounds the whole fetch, even a stuck curl.
 * Send SIGTERM to cancel and allow cleanup; SIGKILL cannot run cleanup. */
#define C1_UPDATE_CHECK_TIMEOUT_SECONDS 95

struct c1_update_check_result {
    uint64_t sequence;
    char version[C1_UPDATE_TOKEN_MAX + 1U];
    int available;
};

/* Single-threaded CLI helper. Reads committed state without acquiring/creating
 * the transaction lock. Writes only its own private .check.* metadata directory
 * under the trusted staging root, and removes it before returning any result.
 * Rollback, identity conflict, unsupported compatibility and untrusted data are
 * errors, not successful checks. Equal signed identities return available=0;
 * any greater sequence can return available=1, even with the same version. */
int c1_update_check(const char *base_url, const char *staging_root,
                    const char *state_root, const char *key_path,
                    struct c1_update_check_result *result,
                    char *error, size_t error_size);

#endif
