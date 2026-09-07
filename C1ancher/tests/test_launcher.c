#include "launcher/policy.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

static int failures = 0;

static void expect(bool condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

int main(void)
{
    unsigned int exit_count;

    expect(c1_launcher_should_restart(false),
           "launcher restarts C1ancher after an exit");
    for (exit_count = 0U; exit_count < 100U; ++exit_count) {
        expect(c1_launcher_should_restart(false),
               "repeated exits never change the restart target");
    }
    expect(!c1_launcher_should_restart(true),
           "launcher stops restarting after a shutdown request");

    if (failures != 0) {
        fprintf(stderr, "%d launcher test(s) failed\n", failures);
        return EXIT_FAILURE;
    }
    puts("all launcher tests passed");
    return EXIT_SUCCESS;
}