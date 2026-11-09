/* MD5 message digest, written for Akam from the public specification:
 * R. Rivest, "The MD5 Message-Digest Algorithm", RFC 1321 (April 1992).
 * The algorithm and its constants are public; this implementation is original
 * and, like the rest of the repository, MIT licensed (see LICENSE). It is
 * checked against the RFC 1321 test suite in tests/native/rtsp_auth_test.c. */
#include <stdarg.h>
#include <string.h>
#include "md5.h"

/* K[i] = floor(2^32 * abs(sin(i + 1))) */
static const uint32_t K[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
    0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
    0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
    0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
    0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
    0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391,
};

static const uint8_t S[64] = {
    7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
    5,  9, 14, 20, 5,  9, 14, 20, 5,  9, 14, 20, 5,  9, 14, 20,
    4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
    6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21,
};

static uint32_t rol(uint32_t x, unsigned n) { return (x << n) | (x >> (32 - n)); }

static void block(uint32_t st[4], const uint8_t p[64])
{
    uint32_t m[16];
    for (int i = 0; i < 16; i++)
        m[i] = (uint32_t)p[4 * i] | (uint32_t)p[4 * i + 1] << 8 |
               (uint32_t)p[4 * i + 2] << 16 | (uint32_t)p[4 * i + 3] << 24;

    uint32_t a = st[0], b = st[1], c = st[2], d = st[3];
    for (int i = 0; i < 64; i++) {
        uint32_t f;
        int g;
        if (i < 16)      { f = (b & c) | (~b & d); g = i; }
        else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) & 15; }
        else if (i < 48) { f = b ^ c ^ d;          g = (3 * i + 5) & 15; }
        else             { f = c ^ (b | ~d);       g = (7 * i) & 15; }
        uint32_t t = d;
        d = c;
        c = b;
        b = b + rol(a + f + K[i] + m[g], S[i]);
        a = t;
    }
    st[0] += a; st[1] += b; st[2] += c; st[3] += d;
}

void md5_init(md5_ctx *c)
{
    c->state[0] = 0x67452301;
    c->state[1] = 0xefcdab89;
    c->state[2] = 0x98badcfe;
    c->state[3] = 0x10325476;
    c->bytes = 0;
}

void md5_update(md5_ctx *c, const void *data, size_t len)
{
    const uint8_t *p = data;
    size_t have = (size_t)(c->bytes & 63);
    c->bytes += len;
    if (have) {
        size_t n = 64 - have;
        if (n > len) n = len;
        memcpy(c->buf + have, p, n);
        p += n;
        len -= n;
        if (have + n < 64) return;
        block(c->state, c->buf);
    }
    for (; len >= 64; p += 64, len -= 64) block(c->state, p);
    memcpy(c->buf, p, len);
}

void md5_final(md5_ctx *c, uint8_t out[16])
{
    uint64_t bits = c->bytes * 8;
    uint8_t pad[72] = { 0x80 };
    size_t have = (size_t)(c->bytes & 63);
    size_t padlen = (have < 56 ? 56 : 120) - have;
    for (int i = 0; i < 8; i++) pad[padlen + (size_t)i] = (uint8_t)(bits >> (8 * i));
    md5_update(c, pad, padlen + 8);
    for (int i = 0; i < 16; i++) out[i] = (uint8_t)(c->state[i / 4] >> (8 * (i % 4)));
}

void md5_hex_cat(char out[33], ...)
{
    static const char hex[] = "0123456789abcdef";
    md5_ctx c;
    uint8_t d[16];
    va_list ap;
    const char *s;
    md5_init(&c);
    va_start(ap, out);
    while ((s = va_arg(ap, const char *)) != NULL) md5_update(&c, s, strlen(s));
    va_end(ap);
    md5_final(&c, d);
    for (int i = 0; i < 16; i++) {
        out[2 * i] = hex[d[i] >> 4];
        out[2 * i + 1] = hex[d[i] & 15];
    }
    out[32] = '\0';
}
