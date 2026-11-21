/* SHA-1 (FIPS 180-1), written from the standard. See sha1.h. */
#include <string.h>
#include "sha1.h"

#define ROL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

static void block(uint32_t h[5], const uint8_t *p)
{
    uint32_t w[80];
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 |
               (uint32_t)p[4 * i + 2] << 8 | (uint32_t)p[4 * i + 3];
    for (int i = 16; i < 80; i++) w[i] = ROL(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20)      { f = (b & c) | (~b & d);          k = 0x5a827999u; }
        else if (i < 40) { f = b ^ c ^ d;                   k = 0x6ed9eba1u; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8f1bbcdcu; }
        else             { f = b ^ c ^ d;                   k = 0xca62c1d6u; }
        uint32_t t = ROL(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = ROL(b, 30);
        b = a;
        a = t;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
}

void sha1_init(sha1_ctx *c)
{
    c->h[0] = 0x67452301u;
    c->h[1] = 0xefcdab89u;
    c->h[2] = 0x98badcfeu;
    c->h[3] = 0x10325476u;
    c->h[4] = 0xc3d2e1f0u;
    c->bytes = 0;
}

void sha1_update(sha1_ctx *c, const void *data, size_t len)
{
    const uint8_t *p = data;
    size_t fill = (size_t)(c->bytes & 63);
    c->bytes += len;
    if (fill) {
        size_t take = 64 - fill;
        if (take > len) take = len;
        memcpy(c->buf + fill, p, take);
        p += take;
        len -= take;
        if (fill + take < 64) return;
        block(c->h, c->buf);
    }
    for (; len >= 64; p += 64, len -= 64) block(c->h, p);
    if (len) memcpy(c->buf, p, len);
}

void sha1_final(sha1_ctx *c, uint8_t out[SHA1_DIGEST_LEN])
{
    uint64_t bits = c->bytes * 8;
    uint8_t pad[72] = { 0x80 };
    size_t fill = (size_t)(c->bytes & 63);
    size_t padlen = (fill < 56 ? 56 : 120) - fill;
    for (int i = 0; i < 8; i++) pad[padlen + (size_t)i] = (uint8_t)(bits >> (56 - 8 * i));
    sha1_update(c, pad, padlen + 8);
    for (int i = 0; i < 5; i++) {
        out[4 * i] = (uint8_t)(c->h[i] >> 24);
        out[4 * i + 1] = (uint8_t)(c->h[i] >> 16);
        out[4 * i + 2] = (uint8_t)(c->h[i] >> 8);
        out[4 * i + 3] = (uint8_t)c->h[i];
    }
    memset(c, 0, sizeof(*c));
}

void sha1(const void *data, size_t len, uint8_t out[SHA1_DIGEST_LEN])
{
    sha1_ctx c;
    sha1_init(&c);
    sha1_update(&c, data, len);
    sha1_final(&c, out);
}
