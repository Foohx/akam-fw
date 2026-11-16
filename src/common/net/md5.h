/* MD5 message digest (RFC 1321). Used only for RTSP Digest authentication
 * (RFC 2617 / RFC 7616 "MD5" algorithm), which mandates it; not for anything
 * that needs collision resistance. See md5.c for the credits. */
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t state[4];
    uint64_t bytes;     /* total length so far */
    uint8_t  buf[64];   /* partial block */
} md5_ctx;

void md5_init(md5_ctx *c);
void md5_update(md5_ctx *c, const void *data, size_t len);
void md5_final(md5_ctx *c, uint8_t out[16]);

/* Lowercase hex of the MD5 of the concatenation of the NULL-terminated list
 * of C strings that follows: md5_hex_cat(out, "a", ":", "b", (char *)NULL).
 * out must hold 33 bytes. */
void md5_hex_cat(char out[33], ...);
