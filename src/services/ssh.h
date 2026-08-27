#ifndef C1_SERVICES_SSH_H
#define C1_SERVICES_SSH_H

#include "core/status.h"

#include <stdbool.h>

#define C1_SSH_IP_CAPACITY 16U
#define C1_SSH_ERROR_CAPACITY 64U

typedef struct {
    bool enabled;
    char ipv4[C1_SSH_IP_CAPACITY];
    char error[C1_SSH_ERROR_CAPACITY];
} c1_ssh_snapshot;

bool c1_ssh_read_snapshot(c1_ssh_snapshot *snapshot);
c1_status c1_ssh_enable(c1_ssh_snapshot *snapshot);
c1_status c1_ssh_disable(c1_ssh_snapshot *snapshot);

#endif