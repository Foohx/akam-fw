/* Base64, see b64.h. */
#include "b64.h"

static const char ALPHA[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

long b64_encode(const void *in, size_t n, char *out, size_t cap)
{
    const uint8_t *p = in;
    if (n > (SIZE_MAX / 4) * 3 - 3 || B64_ENCODED_LEN(n) + 1 > cap) return -1;
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)p[i] << 16;
        if (i + 1 < n) v |= (uint32_t)p[i + 1] << 8;
        if (i + 2 < n) v |= p[i + 2];
        out[o++] = ALPHA[(v >> 18) & 63];
        out[o++] = ALPHA[(v >> 12) & 63];
        out[o++] = i + 1 < n ? ALPHA[(v >> 6) & 63] : '=';
        out[o++] = i + 2 < n ? ALPHA[v & 63] : '=';
    }
    out[o] = '\0';
    return (long)o;
}

static int val(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

long b64_decode(const char *in, size_t n, uint8_t *out, size_t cap)
{
    if (n % 4 != 0) return -1;
    size_t o = 0;
    for (size_t i = 0; i < n; i += 4) {
        int pad = 0;
        uint32_t v = 0;
        for (int k = 0; k < 4; k++) {
            char c = in[i + (size_t)k];
            int d;
            if (c == '=') {
                /* padding: last quantum only, last one or two characters only */
                if (i + 4 != n || k < 2) return -1;
                pad++;
                d = 0;
            } else {
                if (pad) return -1;     /* data after '=' */
                d = val(c);
                if (d < 0) return -1;
            }
            v = v << 6 | (uint32_t)d;
        }
        size_t take = (size_t)(3 - pad);
        if (pad == 1 && (v & 0xff) != 0) return -1;     /* non-canonical trailing bits */
        if (pad == 2 && (v & 0xffff) != 0) return -1;
        if (o + take > cap) return -1;
        out[o++] = (uint8_t)(v >> 16);
        if (take > 1) out[o++] = (uint8_t)(v >> 8);
        if (take > 2) out[o++] = (uint8_t)v;
    }
    return (long)o;
}
