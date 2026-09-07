#include "core/status.h"

const char *c1_status_name(c1_status status)
{
    switch (status) {
    case C1_STATUS_OK:
        return "ok";
    case C1_STATUS_INVALID_ARGUMENT:
        return "invalid_argument";
    case C1_STATUS_UNAVAILABLE:
        return "unavailable";
    case C1_STATUS_IO_ERROR:
        return "io_error";
    case C1_STATUS_INTERRUPTED:
        return "interrupted";
    case C1_STATUS_UNSUPPORTED:
        return "unsupported";
    case C1_STATUS_UPDATE_REQUESTED:
        return "update_requested";
    }

    return "unknown";
}

int c1_status_exit_code(c1_status status)
{
    switch (status) {
    case C1_STATUS_OK:
        return 0;
    case C1_STATUS_INVALID_ARGUMENT:
        return 64;
    case C1_STATUS_UNAVAILABLE:
        return 69;
    case C1_STATUS_IO_ERROR:
        return 74;
    case C1_STATUS_INTERRUPTED:
        return 130;
    case C1_STATUS_UNSUPPORTED:
        return 78;
    case C1_STATUS_UPDATE_REQUESTED:
        return 75;
    }

    return 70;
}