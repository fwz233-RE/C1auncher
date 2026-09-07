#include "security/sha256.h"
#include "security/secure_file.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

static uint32_t rotate_right(uint32_t value, unsigned int count)
{
    return (value >> count) | (value << (32U - count));
}

static uint32_t load_be32(const unsigned char *data)
{
    return ((uint32_t)data[0] << 24U) | ((uint32_t)data[1] << 16U) |
           ((uint32_t)data[2] << 8U) | (uint32_t)data[3];
}

static void store_be32(unsigned char *data, uint32_t value)
{
    data[0] = (unsigned char)(value >> 24U);
    data[1] = (unsigned char)(value >> 16U);
    data[2] = (unsigned char)(value >> 8U);
    data[3] = (unsigned char)value;
}

static void transform(struct c1_sha256_context *context, const unsigned char block[64])
{
    static const uint32_t constants[64] = {
        0x428a2f98U,0x71374491U,0xb5c0fbcfU,0xe9b5dba5U,0x3956c25bU,0x59f111f1U,0x923f82a4U,0xab1c5ed5U,
        0xd807aa98U,0x12835b01U,0x243185beU,0x550c7dc3U,0x72be5d74U,0x80deb1feU,0x9bdc06a7U,0xc19bf174U,
        0xe49b69c1U,0xefbe4786U,0x0fc19dc6U,0x240ca1ccU,0x2de92c6fU,0x4a7484aaU,0x5cb0a9dcU,0x76f988daU,
        0x983e5152U,0xa831c66dU,0xb00327c8U,0xbf597fc7U,0xc6e00bf3U,0xd5a79147U,0x06ca6351U,0x14292967U,
        0x27b70a85U,0x2e1b2138U,0x4d2c6dfcU,0x53380d13U,0x650a7354U,0x766a0abbU,0x81c2c92eU,0x92722c85U,
        0xa2bfe8a1U,0xa81a664bU,0xc24b8b70U,0xc76c51a3U,0xd192e819U,0xd6990624U,0xf40e3585U,0x106aa070U,
        0x19a4c116U,0x1e376c08U,0x2748774cU,0x34b0bcb5U,0x391c0cb3U,0x4ed8aa4aU,0x5b9cca4fU,0x682e6ff3U,
        0x748f82eeU,0x78a5636fU,0x84c87814U,0x8cc70208U,0x90befffaU,0xa4506cebU,0xbef9a3f7U,0xc67178f2U
    };
    uint32_t words[64];
    uint32_t a = context->state[0], b = context->state[1], c = context->state[2], d = context->state[3];
    uint32_t e = context->state[4], f = context->state[5], g = context->state[6], h = context->state[7];
    size_t i;

    for (i = 0U; i < 16U; ++i) words[i] = load_be32(block + i * 4U);
    for (; i < 64U; ++i) {
        uint32_t s0 = rotate_right(words[i - 15U], 7U) ^ rotate_right(words[i - 15U], 18U) ^ (words[i - 15U] >> 3U);
        uint32_t s1 = rotate_right(words[i - 2U], 17U) ^ rotate_right(words[i - 2U], 19U) ^ (words[i - 2U] >> 10U);
        words[i] = words[i - 16U] + s0 + words[i - 7U] + s1;
    }
    for (i = 0U; i < 64U; ++i) {
        uint32_t s1 = rotate_right(e, 6U) ^ rotate_right(e, 11U) ^ rotate_right(e, 25U);
        uint32_t choice = (e & f) ^ ((~e) & g);
        uint32_t temporary1 = h + s1 + choice + constants[i] + words[i];
        uint32_t s0 = rotate_right(a, 2U) ^ rotate_right(a, 13U) ^ rotate_right(a, 22U);
        uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        uint32_t temporary2 = s0 + majority;
        h = g; g = f; f = e; e = d + temporary1;
        d = c; c = b; b = a; a = temporary1 + temporary2;
    }
    context->state[0] += a; context->state[1] += b; context->state[2] += c; context->state[3] += d;
    context->state[4] += e; context->state[5] += f; context->state[6] += g; context->state[7] += h;
}

void c1_sha256_init(struct c1_sha256_context *context)
{
    static const uint32_t initial[8] = {0x6a09e667U,0xbb67ae85U,0x3c6ef372U,0xa54ff53aU,0x510e527fU,0x9b05688cU,0x1f83d9abU,0x5be0cd19U};
    (void)memset(context, 0, sizeof(*context));
    (void)memcpy(context->state, initial, sizeof(initial));
}

void c1_sha256_update(struct c1_sha256_context *context, const void *data, size_t size)
{
    const unsigned char *bytes = (const unsigned char *)data;
    context->length += (uint64_t)size;
    while (size != 0U) {
        size_t amount = sizeof(context->block) - context->used;
        if (amount > size) amount = size;
        (void)memcpy(context->block + context->used, bytes, amount);
        context->used += amount; bytes += amount; size -= amount;
        if (context->used == sizeof(context->block)) {
            transform(context, context->block);
            context->used = 0U;
        }
    }
}

void c1_sha256_final(struct c1_sha256_context *context, unsigned char digest[C1_SHA256_SIZE])
{
    uint64_t bits = context->length * 8U;
    size_t i;
    context->block[context->used++] = 0x80U;
    if (context->used > 56U) {
        (void)memset(context->block + context->used, 0, 64U - context->used);
        transform(context, context->block);
        context->used = 0U;
    }
    (void)memset(context->block + context->used, 0, 56U - context->used);
    for (i = 0U; i < 8U; ++i) context->block[63U - i] = (unsigned char)(bits >> (i * 8U));
    transform(context, context->block);
    for (i = 0U; i < 8U; ++i) store_be32(digest + i * 4U, context->state[i]);
    (void)memset(context, 0, sizeof(*context));
}

void c1_sha256(const void *data, size_t size, unsigned char digest[C1_SHA256_SIZE])
{
    static const unsigned char empty = 0U;
    struct c1_sha256_context context;
    c1_sha256_init(&context);
    c1_sha256_update(&context, data == NULL ? &empty : data, size);
    c1_sha256_final(&context, digest);
}

void c1_sha256_hex(const unsigned char digest[C1_SHA256_SIZE], char hex[C1_SHA256_HEX_SIZE])
{
    static const char digits[] = "0123456789abcdef";
    size_t i;
    for (i = 0U; i < C1_SHA256_SIZE; ++i) {
        hex[i * 2U] = digits[digest[i] >> 4U];
        hex[i * 2U + 1U] = digits[digest[i] & 15U];
    }
    hex[64] = '\0';
}

int c1_sha256_file(const char *path, uint64_t maximum_size, uint64_t exact_size,
                   unsigned char digest[C1_SHA256_SIZE], uint64_t *size,
                   char *error, size_t error_size)
{
    struct stat before, after;
    struct c1_sha256_context context;
    unsigned char buffer[16384];
    uint64_t total = 0U;
    int descriptor;

    if (path == NULL || digest == NULL || maximum_size == C1_SHA256_ANY_SIZE ||
        (exact_size != C1_SHA256_ANY_SIZE && exact_size > maximum_size)) {
        c1_secure_set_error(error, error_size, "invalid SHA-256 file request");
        return -1;
    }
    descriptor = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0) {
        c1_secure_set_error(error, error_size, "SHA-256 file open failed: %s", strerror(errno));
        return -1;
    }
    if (fstat(descriptor, &before) != 0 || !S_ISREG(before.st_mode) || before.st_nlink != 1 ||
        before.st_size < 0 || (uint64_t)before.st_size > maximum_size ||
        (exact_size != C1_SHA256_ANY_SIZE && (uint64_t)before.st_size != exact_size)) {
        c1_secure_set_error(error, error_size, "SHA-256 file metadata rejected");
        (void)close(descriptor);
        return -1;
    }
    c1_sha256_init(&context);
    for (;;) {
        ssize_t amount = read(descriptor, buffer, sizeof(buffer));
        if (amount < 0 && errno == EINTR) continue;
        if (amount < 0 || total > maximum_size - (uint64_t)(amount > 0 ? amount : 0)) {
            c1_secure_set_error(error, error_size, "SHA-256 file read failed");
            (void)close(descriptor);
            return -1;
        }
        if (amount == 0) break;
        total += (uint64_t)amount;
        c1_sha256_update(&context, buffer, (size_t)amount);
    }
    if (fstat(descriptor, &after) != 0 || total != (uint64_t)before.st_size ||
        before.st_dev != after.st_dev || before.st_ino != after.st_ino ||
        before.st_size != after.st_size || before.st_mtime != after.st_mtime ||
        before.st_mtim.tv_nsec != after.st_mtim.tv_nsec ||
        before.st_ctime != after.st_ctime || before.st_ctim.tv_nsec != after.st_ctim.tv_nsec ||
        !S_ISREG(after.st_mode) || after.st_nlink != 1 ||
        close(descriptor) != 0) {
        c1_secure_set_error(error, error_size, "SHA-256 file changed during read");
        return -1;
    }
    c1_sha256_final(&context, digest);
    if (size != NULL) *size = total;
    return 0;
}