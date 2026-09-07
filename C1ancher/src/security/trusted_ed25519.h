#ifndef C1_TRUSTED_ED25519_H
#define C1_TRUSTED_ED25519_H

#include <stddef.h>

#define C1_ED25519_PUBLIC_KEY_SIZE 32U
#define C1_ED25519_SIGNATURE_SIZE 64U
#define C1_ED25519_PAYLOAD_MAX (256U * 1024U)

int c1_trusted_ed25519_verify(const unsigned char signature[C1_ED25519_SIGNATURE_SIZE],
                              const unsigned char *payload, size_t payload_size,
                              const unsigned char public_key[C1_ED25519_PUBLIC_KEY_SIZE],
                              char *error, size_t error_size);
int c1_trusted_ed25519_verify_files(const char *key_path, const char *signature_path,
                                    const unsigned char *payload, size_t payload_size,
                                    char *error, size_t error_size);

#endif