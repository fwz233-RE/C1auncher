#include "core/record.h"
#include "core/status.h"
#include "hal/linux/ui.h"
#include "platform/stop.h"

#include <stdio.h>
#include <string.h>

#ifndef C1_VERSION
#define C1_VERSION "0.0.0-dev"
#endif

int main(int argument_count, char **arguments)
{
    c1_status status;

    if (argument_count == 2 && strcmp(arguments[1], "--version") == 0) {
        printf("C1ancher %s\n", C1_VERSION);
        return 0;
    }
    if (argument_count != 2 || strcmp(arguments[1], "app") != 0) {
        fprintf(stderr, "usage: %s app|--version\n", arguments[0]);
        return c1_status_exit_code(C1_STATUS_INVALID_ARGUMENT);
    }

    c1_stop_reset();
    c1_stop_install();
    status = c1_linux_ui_run(c1_ndjson_sink(stdout));
    return c1_status_exit_code(status);
}