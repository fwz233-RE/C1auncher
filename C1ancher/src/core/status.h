#ifndef C1_CORE_STATUS_H
#define C1_CORE_STATUS_H

typedef enum {
    C1_STATUS_OK = 0,
    C1_STATUS_INVALID_ARGUMENT,
    C1_STATUS_UNAVAILABLE,
    C1_STATUS_IO_ERROR,
    C1_STATUS_INTERRUPTED,
    C1_STATUS_UNSUPPORTED,
    C1_STATUS_UPDATE_REQUESTED
} c1_status;

const char *c1_status_name(c1_status status);
int c1_status_exit_code(c1_status status);

#endif