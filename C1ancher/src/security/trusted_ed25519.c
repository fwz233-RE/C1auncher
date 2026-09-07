#include "security/trusted_ed25519.h"
#include "security/secure_file.h"
#include "ed25519.h"

#include <stdlib.h>

int c1_trusted_ed25519_verify(const unsigned char signature[C1_ED25519_SIGNATURE_SIZE],
                              const unsigned char *payload, size_t payload_size,
                              const unsigned char public_key[C1_ED25519_PUBLIC_KEY_SIZE],
                              char *error, size_t error_size)
{
    static const unsigned char empty_payload = 0U;
    const unsigned char *message = payload;

    if (signature == NULL || public_key == NULL ||
        (payload == NULL && payload_size != 0U) || payload_size > C1_ED25519_PAYLOAD_MAX) {
        c1_secure_set_error(error, error_size, "invalid Ed25519 verification request");
        return -1;
    }
    if (message == NULL) {
        message = &empty_payload;
    }
    if (ed25519_verify(signature, message, payload_size, public_key) != 1) {
        c1_secure_set_error(error, error_size, "Ed25519 signature rejected");
        return -1;
    }
    return 0;
}

int c1_trusted_ed25519_verify_files(const char *key_path, const char *signature_path,
                                    const unsigned char *payload, size_t payload_size,
                                    char *error, size_t error_size)
{
    unsigned char *public_key = NULL;
    unsigned char *signature = NULL;
    size_t public_key_size = 0U;
    size_t signature_size = 0U;
    int result = -1;

    if (payload_size > C1_ED25519_PAYLOAD_MAX ||
        (payload == NULL && payload_size != 0U)) {
        c1_secure_set_error(error, error_size, "invalid Ed25519 verification request");
        return -1;
    }
    if (c1_secure_read_file(key_path, &public_key, &public_key_size,
                            C1_ED25519_PUBLIC_KEY_SIZE, C1_ED25519_PUBLIC_KEY_SIZE,
                            error, error_size) != 0) {
        c1_secure_set_error(error, error_size, "trusted Ed25519 key rejected");
        goto done;
    }
    if (c1_secure_read_file(signature_path, &signature, &signature_size,
                            C1_ED25519_SIGNATURE_SIZE, C1_ED25519_SIGNATURE_SIZE,
                            error, error_size) != 0) {
        c1_secure_set_error(error, error_size, "Ed25519 signature file rejected");
        goto done;
    }
    result = c1_trusted_ed25519_verify(signature, payload, payload_size,
                                       public_key, error, error_size);
done:
    free(public_key);
    free(signature);
    return result;
}