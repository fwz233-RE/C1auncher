/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdio.h>
#include <string.h>

#ifndef APP_VERSION
#define APP_VERSION "0.1.0"
#endif

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--version") == 0) {
        puts("c1-example " APP_VERSION);
        return 0;
    }
    puts("Hello, C1-Slim!");
    puts("Version: " APP_VERSION);
    puts("This is a terminal application. Press Enter to exit.");
    fflush(stdout);
    (void)getchar();
    return 0;
}
