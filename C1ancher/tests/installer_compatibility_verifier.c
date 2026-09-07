/* Private-chroot test driver. Links unchanged production compatibility code. */
#include "update/update.h"
#include <stdio.h>
#include <string.h>
int main(int argc, char **argv)
{
    struct c1_update_manifest manifest;
    char error[512] = "";
    memset(&manifest, 0, sizeof(manifest));
    if (argc != 3 || strlen(argv[1]) >= sizeof(manifest.min_bootstrap) ||
        strlen(argv[2]) >= sizeof(manifest.min_updater)) return 64;
    strcpy(manifest.min_bootstrap, argv[1]);
    strcpy(manifest.min_updater, argv[2]);
    if (c1_update_check_compatibility(&manifest, error, sizeof(error)) != 0) {
        fprintf(stderr, "%s\n", error);
        return 1;
    }
    puts("production compatibility accepted");
    return 0;
}
