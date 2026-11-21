/* Base64 (RFC 4648 section 4, standard alphabet, '=' padding). Own implementation, MIT. */
#pragma once
#include <stddef.h>
#include <stdint.h>

/* Characters needed to encode n bytes, without the terminating NUL. */
#define B64_ENCODED_LEN(n) ((((n) + 2) / 3) * 4)

/* Encode n bytes into out as a NUL-terminated string. Returns the length
 * written (without the NUL), or -1 when cap is too small for it plus the NUL. */
long b64_encode(const void *in, size_t n, char *out, size_t cap);

/* Strict decode of exactly n characters (the input need not be NUL-terminated):
 * the length must be a multiple of 4, only the standard alphabet, '=' only as the
 * one or two last characters, and the unused bits of the last quantum must be zero
 * (the canonical encoding). No whitespace is skipped. Returns the number of bytes
 * written, or -1 on any error or when cap is too small. */
long b64_decode(const char *in, size_t n, uint8_t *out, size_t cap);
