/* SHA-256 (FIPS 180-4), HMAC-SHA256 (RFC 2104) and a constant-time compare. Portable C99, no I/O, no allocation, no
 * dependency. Home: src/net/crypto (see specs/control-api.md, phase 1). */
#ifndef AKAM_SHA256_H
#define AKAM_SHA256_H
#include <stddef.h>
#include <stdint.h>

#define SHA256_LEN 32
#define SHA256_BLOCK 64

typedef struct {
    uint32_t h[8];
    uint64_t total; /* bytes hashed so far */
    uint8_t buf[SHA256_BLOCK];
    size_t n; /* bytes pending in buf */
} sha256_ctx;

void sha256_init(sha256_ctx *c);
void sha256_update(sha256_ctx *c, const void *data, size_t len);
void sha256_final(sha256_ctx *c, uint8_t out[SHA256_LEN]); /* the context is wiped */
void sha256(const void *data, size_t len, uint8_t out[SHA256_LEN]);
/* Any key length (> 64 bytes is hashed first, RFC 2104). */
void hmac_sha256(const void *key, size_t klen, const void *msg, size_t mlen, uint8_t out[SHA256_LEN]);
/* 1 when the n bytes are equal. Time depends on n only, never on the content. */
int ct_equal(const void *a, const void *b, size_t n);
/* Lower-case hex of n bytes into out (2n+1 bytes, NUL terminated). */
void akam_hex(const void *in, size_t n, char *out);
/* Decodes exactly 2*n hex digits (either case, then NUL) into out; 0, or -1 on any bad digit or length. */
int akam_unhex(const char *hex, size_t n, uint8_t *out);
/* memset that the compiler may not remove. */
void akam_wipe(void *p, size_t n);

#endif
