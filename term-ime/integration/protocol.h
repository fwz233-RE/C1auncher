#ifndef C1_IME_PROTOCOL_H
#define C1_IME_PROTOCOL_H

#include "c1_ime_client.h"
#include <stddef.h>
#include <string.h>

#define C1_WIRE_MAGIC 0x4331494du /* ASCII C1IM */
#define C1_WIRE_HEADER 32u
#define C1_WIRE_REPLY_HEADER 40u
#define C1_WIRE_REPLY 0x8000u
#define C1_WIRE_MODIFIERS (C1_IME_MOD_SHIFT | C1_IME_MOD_LOCK | C1_IME_MOD_CONTROL | C1_IME_MOD_ALT | C1_IME_MOD_SUPER)
#define C1_WIRE_FLAGS 63u

static inline uint16_t c1_get16(const unsigned char *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}
static inline uint32_t c1_get32(const unsigned char *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static inline void c1_put16(unsigned char *p, uint16_t n) {
    p[0] = (unsigned char)(n >> 8); p[1] = (unsigned char)n;
}
static inline void c1_put32(unsigned char *p, uint32_t n) {
    p[0] = (unsigned char)(n >> 24); p[1] = (unsigned char)(n >> 16);
    p[2] = (unsigned char)(n >> 8); p[3] = (unsigned char)n;
}
static inline int c1_valid_request(const struct c1_ime_request *r) {
    if (!r || r->operation < C1_IME_OP_STATUS || r->operation > C1_IME_OP_CANCEL) return 0;
    if (r->operation == C1_IME_OP_KEY) {
        return r->keysym > 0 && r->keysym <= 0x1ffffffu &&
               !(r->modifiers & ~C1_WIRE_MODIFIERS) && !r->value;
    }
    if (r->keysym || r->modifiers) return 0;
    switch (r->operation) {
        case C1_IME_OP_MODE: case C1_IME_OP_PURPOSE: case C1_IME_OP_PAGE: return r->value <= 1;
        case C1_IME_OP_SELECT: return r->value < C1_IME_MAX_CANDIDATES;
        default: return r->value == 0;
    }
}
/* Strict Unicode scalar UTF-8, no embedded NULs or overlong encodings. */
static inline int c1_valid_utf8(const unsigned char *s, size_t n) {
    size_t i = 0;
    while (i < n) {
        uint32_t ch = s[i++], minimum;
        unsigned count, j;
        if (ch < 0x80) { if (!ch) return 0; continue; }
        if (ch >= 0xc2 && ch <= 0xdf) { count = 1; minimum = 0x80; ch &= 0x1f; }
        else if (ch >= 0xe0 && ch <= 0xef) { count = 2; minimum = 0x800; ch &= 0xf; }
        else if (ch >= 0xf0 && ch <= 0xf4) { count = 3; minimum = 0x10000; ch &= 7; }
        else return 0;
        if (n - i < count) return 0;
        for (j = 0; j < count; ++j) {
            if ((s[i] & 0xc0) != 0x80) return 0;
            ch = (ch << 6) | (s[i++] & 0x3f);
        }
        if (ch < minimum || ch > 0x10ffff || (ch >= 0xd800 && ch <= 0xdfff)) return 0;
    }
    return 1;
}
static inline void c1_encode_request(unsigned char *p, const struct c1_ime_request *r, uint32_t seq) {
    memset(p, 0, C1_WIRE_HEADER);
    c1_put32(p, C1_WIRE_MAGIC); c1_put16(p + 4, C1_IME_PROTOCOL_VERSION);
    c1_put16(p + 6, (uint16_t)r->operation); c1_put32(p + 8, seq);
    c1_put32(p + 12, C1_WIRE_HEADER); c1_put32(p + 16, r->keysym);
    c1_put32(p + 20, r->modifiers); c1_put32(p + 24, r->value);
}
static inline int c1_decode_request(const unsigned char *p, size_t n, struct c1_ime_request *r, uint32_t *seq) {
    if (n != C1_WIRE_HEADER || c1_get32(p) != C1_WIRE_MAGIC ||
        c1_get16(p + 4) != C1_IME_PROTOCOL_VERSION || c1_get32(p + 12) != n || c1_get32(p + 28)) return 0;
    *seq = c1_get32(p + 8);
    r->operation = c1_get16(p + 6); r->keysym = c1_get32(p + 16);
    r->modifiers = c1_get32(p + 20); r->value = c1_get32(p + 24);
    return *seq != 0 && c1_valid_request(r);
}
#endif
