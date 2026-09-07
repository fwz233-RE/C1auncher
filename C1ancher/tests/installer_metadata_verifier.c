/* Host-only driver uses the production secure reader and Ed25519 verifier.
 * It only reads three explicit files. No install/update/device operations. */
#include "security/secure_file.h"
#include "security/trusted_ed25519.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    unsigned char *data = NULL;
    size_t size = 0U;
    char error[512] = "";
    int result = EXIT_FAILURE;
    if (argc != 4) return 2;
    if (c1_secure_read_file(argv[1], &data, &size, C1_ED25519_PAYLOAD_MAX,
                            C1_SECURE_FILE_ANY_SIZE, error, sizeof(error)) != 0 ||
        c1_trusted_ed25519_verify_files(argv[3], argv[2], data, size,
                                      error, sizeof(error)) != 0) {
        fprintf(stderr, "verification failed: %s\n", error);
    } else {
        puts("signature verified");
        result = EXIT_SUCCESS;
    }
    free(data);
    return result;
}
