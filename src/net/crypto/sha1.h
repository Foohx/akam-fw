/* SHA-1 (FIPS 180-1). Own implementation, MIT. Used only for the ONVIF
 * WS-Security UsernameToken PasswordDigest (Base64(SHA1(nonce+created+password))),
 * which mandates it; not for anything that needs collision resistance. */
#pragma once
#include <stddef.h>
#include <stdint.h>

#define SHA1_DIGEST_LEN 20

typedef struct {
    uint32_t h[5];
    uint64_t bytes;     /* total length so far */
    uint8_t  buf[64];   /* partial block */
} sha1_ctx;

void sha1_init(sha1_ctx *c);
void sha1_update(sha1_ctx *c, const void *data, size_t len);
void sha1_final(sha1_ctx *c, uint8_t out[SHA1_DIGEST_LEN]);
/* One-shot. */
void sha1(const void *data, size_t len, uint8_t out[SHA1_DIGEST_LEN]);
