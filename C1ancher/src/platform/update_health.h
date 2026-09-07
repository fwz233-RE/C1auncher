#ifndef C1_PLATFORM_UPDATE_HEALTH_H
#define C1_PLATFORM_UPDATE_HEALTH_H

#include "update/update.h"

struct c1_update_health_config {
    const char *pkg_path;
    const char *updater_path;
    const char *state_root;
    const char *ready_file;
    unsigned int timeout_ms;
};

int c1_update_health_should_probe(const struct c1_update_state *state);
int c1_update_health_probe_running(const char *state_root, const char *ready_file,
                                   unsigned int timeout_ms);
/* Runs the candidate probes once; returns zero only after mark-ready succeeds. */
int c1_update_health_probe(const struct c1_update_health_config *config);

#endif