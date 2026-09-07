#ifndef C1_SHA256_H
#define C1_SHA256_H

#include <stddef.h>
#include <stdint.h>

#define C1_SHA256_SIZE 32U
#define C1_SHA256_HEX_SIZE 65U

struct c1_sha256_context {
    uint32_t state[8];
    uint64_t length;
    unsigned char block[64];
    size_t used;
};

void c1_sha256_init(struct c1_sha256_context *context);
void c1_sha256_update(struct c1_sha256_context *context, const void *data, size_t size);
void c1_sha256_final(struct c1_sha256_context *context, unsigned char digest[C1_SHA256_SIZE]);
void c1_sha256(const void *data, size_t size, unsigned char digest[C1_SHA256_SIZE]);
void c1_sha256_hex(const unsigned char digest[C1_SHA256_SIZE], char hex[C1_SHA256_HEX_SIZE]);
int c1_sha256_file(const char *path, uint64_t maximum_size, uint64_t exact_size,
                   unsigned char digest[C1_SHA256_SIZE], uint64_t *size,
                   char *error, size_t error_size);

#define C1_SHA256_ANY_SIZE UINT64_MAX

#endif