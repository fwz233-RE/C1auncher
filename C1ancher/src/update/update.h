#ifndef C1_UPDATE_H
#define C1_UPDATE_H

#include <stddef.h>
#include <stdint.h>

#define C1_UPDATE_MANIFEST_MAX (64U * 1024U)
#define C1_UPDATE_LINE_MAX 512U
#define C1_UPDATE_TOKEN_MAX 64U
#define C1_UPDATE_PATH_MAX 4096U
#define C1_UPDATE_ERROR_MAX 256U
#define C1_UPDATE_COMPONENT_COUNT 4U
#define C1_UPDATE_COMPONENT_MAX (32U * 1024U * 1024U)
#define C1_UPDATE_PAYLOAD_MAX (96U * 1024U * 1024U)
#define C1_UPDATE_TARGET "mips32r2-little-o32-hard-float-double-static"
/* Capability versions are independent of the application release version. */
#define C1_UPDATER_VERSION "1.1.0"
#define C1_BOOTSTRAP_VERSION "1.1.0"
#define C1_UPDATE_BUSY (-2)
#define C1_UPDATE_PROTOCOL_VERSION 1U
#define C1_UPDATE_ABI_VERSION 1U
#define C1_UPDATE_DEFAULT_STATE_ROOT "/usr/data/c1/update/state"
#define C1_UPDATE_DEFAULT_STAGING_ROOT "/storage/c1/update/staging"
#define C1_UPDATE_DEFAULT_REPOSITORY "http://www.fwz233.com/c1/core/v1/stable"
#define C1_UPDATE_REPOSITORY_CONFIG "/usr/data/c1/update/repository.url"
#define C1_UPDATE_DEFAULT_CORE_ROOT "/usr/data/c1/core"
#define C1_UPDATE_DEFAULT_KEY "/etc/c1updater/core.ed25519.pub"
#define C1_UPDATE_DEFAULT_READY_FILE "/usr/data/c1/update/ready"

struct c1_update_component {
    char role[16];
    char path[64];
    char sha256[65];
    uint64_t size;
    unsigned int mode;
};

struct c1_update_manifest {
    uint64_t sequence;
    char version[C1_UPDATE_TOKEN_MAX + 1U];
    uint64_t security_epoch;
    char target[64];
    char min_bootstrap[C1_UPDATE_TOKEN_MAX + 1U];
    char min_updater[C1_UPDATE_TOKEN_MAX + 1U];
    char compatibility[C1_UPDATE_TOKEN_MAX + 1U];
    char source_revision[C1_UPDATE_TOKEN_MAX + 1U];
    uint64_t source_date_epoch;
    struct c1_update_component components[C1_UPDATE_COMPONENT_COUNT];
};

enum c1_update_phase {
    C1_UPDATE_IDLE,
    C1_UPDATE_DOWNLOADING,
    C1_UPDATE_VERIFIED,
    C1_UPDATE_PREPARED,
    C1_UPDATE_PENDING_BOOT,
    C1_UPDATE_CONFIRMED
};

struct c1_update_state {
    uint64_t generation;
    enum c1_update_phase phase;
    uint64_t sequence;
    uint64_t security_epoch;
    char release[C1_UPDATE_TOKEN_MAX + 1U];
    char digest[65];
};

int c1_update_check_compatibility_versions(const struct c1_update_manifest *manifest,
                                          const char *bootstrap, const char *updater,
                                          char *error, size_t error_size);
int c1_update_check_compatibility(const struct c1_update_manifest *manifest,
                                 char *error, size_t error_size);

int c1_update_parse_manifest(const unsigned char *data, size_t size,
                             struct c1_update_manifest *manifest,
                             char *error, size_t error_size);
const char *c1_update_phase_name(enum c1_update_phase phase);
int c1_update_phase_parse(const char *text, enum c1_update_phase *phase);
int c1_update_transition(const struct c1_update_state *previous,
                         enum c1_update_phase next_phase, uint64_t sequence,
                         uint64_t security_epoch, const char *release, const char *digest,
                         int failed, struct c1_update_state *next,
                         char *error, size_t error_size);
int c1_update_state_load(const char *root, struct c1_update_state *state,
                         char *error, size_t error_size);
int c1_update_state_commit(const char *root,
                           const struct c1_update_state *previous,
                           const struct c1_update_state *next,
                           char *error, size_t error_size);

#endif