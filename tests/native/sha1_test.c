/* Host test of src/ak_rtsp/sha1.c (FIPS 180-1 vectors, padding boundaries,
 * incremental feeding) and b64.c (RFC 4648 vectors, strict decoding, bounds).
 * Reference values for the padding boundaries come from `openssl dgst -sha1`.
 * Build/run: tests/sha1_test.sh */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "b64.h"
#include "sha1.h"

static int fails;
#define CHECK(cond, msg) do { if (cond) printf("  ok   - %s\n", msg); \
    else { printf("  FAIL - %s (line %d)\n", msg, __LINE__); fails++; } } while (0)

static void hex_of(const uint8_t d[20], char out[41])
{
    for (int i = 0; i < 20; i++) sprintf(out + 2 * i, "%02x", d[i]);
}

static void test_sha1(void)
{
    static const struct { const char *in; const char *hex; } v[] = {
        { "", "da39a3ee5e6b4b0d3255bfef95601890afd80709" },
        { "abc", "a9993e364706816aba3e25717850c26c9cd0d89d" },
        { "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
          "84983e441c3bd26ebaae4aa1f95129e5e54670f1" },
        { "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu",
          "a49b2446a02c645bf419f995b67091253a04a259" },
    };
    printf("SHA-1 (FIPS 180-1 / 180-2 vectors)\n");
    char hex[41], m[80];
    uint8_t d[20];
    for (size_t i = 0; i < sizeof(v) / sizeof(v[0]); i++) {
        sha1(v[i].in, strlen(v[i].in), d);
        hex_of(d, hex);
        snprintf(m, sizeof(m), "vector %zu (%zu bytes)", i, strlen(v[i].in));
        CHECK(strcmp(hex, v[i].hex) == 0, m);
    }

    /* One million 'a', fed in 1000-byte pieces. */
    sha1_ctx c;
    char chunk[1000];
    memset(chunk, 'a', sizeof(chunk));
    sha1_init(&c);
    for (int i = 0; i < 1000; i++) sha1_update(&c, chunk, sizeof(chunk));
    sha1_final(&c, d);
    hex_of(d, hex);
    CHECK(strcmp(hex, "34aa973cd4c4daa4f61eeb2bdbad27316534016f") == 0, "one million 'a'");

    static const struct { int n; const char *hex; } b[] = {
        { 55, "cef734ba81a024479e09eb5a75b6ddae62e6abf1" }, { 56, "901305367c259952f4e7af8323f480d59f81335b" },
        { 57, "025ecbd5d70f8fb3c5457cd96bab13fda305dc59" }, { 63, "0ddc4e0cccd9a12850deb5abb0853a4425559fec" },
        { 64, "bb2fa3ee7afb9f54c6dfb5d021f14b1ffe40c163" }, { 65, "78c741ddc482e4cdf8c474a0876347a0905b6233" },
        { 119, "4300320394f7ee239bcdce7d3b8bcee173a0cd5c" }, { 120, "ceb2821639c4b6dcb10bce0e522ca2e608ce056d" },
        { 121, "f8b9fb92a99638962adc81b9e0270ab9c6dd6c84" }, { 128, "150fa3fbdc899bd0b8f95a9fb6027f564d953762" },
    };
    char x[130];
    memset(x, 'x', sizeof(x));
    int ok_once = 1, ok_inc = 1;
    for (size_t i = 0; i < sizeof(b) / sizeof(b[0]); i++) {
        sha1(x, (size_t)b[i].n, d);
        hex_of(d, hex);
        if (strcmp(hex, b[i].hex) != 0) ok_once = 0;
        /* fed in uneven pieces: 1, 2, 3, ... bytes */
        sha1_init(&c);
        int left = b[i].n, step = 1, off = 0;
        while (left > 0) {
            int k = step < left ? step : left;
            sha1_update(&c, x + off, (size_t)k);
            off += k; left -= k; step++;
        }
        sha1_final(&c, d);
        hex_of(d, hex);
        if (strcmp(hex, b[i].hex) != 0) ok_inc = 0;
    }
    CHECK(ok_once, "padding boundaries 55..128 bytes, one call");
    CHECK(ok_inc, "padding boundaries 55..128 bytes, uneven pieces");

    sha1_init(&c);
    sha1_update(&c, NULL, 0);
    sha1_update(&c, "abc", 3);
    sha1_update(&c, "", 0);
    sha1_final(&c, d);
    hex_of(d, hex);
    CHECK(strcmp(hex, "a9993e364706816aba3e25717850c26c9cd0d89d") == 0, "empty updates are harmless");
}

static void test_b64(void)
{
    static const struct { const char *raw; const char *enc; } v[] = {
        { "", "" }, { "f", "Zg==" }, { "fo", "Zm8=" }, { "foo", "Zm9v" },
        { "foob", "Zm9vYg==" }, { "fooba", "Zm9vYmE=" }, { "foobar", "Zm9vYmFy" },
    };
    printf("Base64 (RFC 4648 section 10)\n");
    char out[64], m[80];
    uint8_t raw[64];
    for (size_t i = 0; i < sizeof(v) / sizeof(v[0]); i++) {
        size_t n = strlen(v[i].raw);
        long e = b64_encode(v[i].raw, n, out, sizeof(out));
        snprintf(m, sizeof(m), "encode \"%s\"", v[i].raw);
        CHECK(e == (long)strlen(v[i].enc) && strcmp(out, v[i].enc) == 0, m);
        long d = b64_decode(v[i].enc, strlen(v[i].enc), raw, sizeof(raw));
        snprintf(m, sizeof(m), "decode \"%s\"", v[i].enc);
        CHECK(d == (long)n && memcmp(raw, v[i].raw, n) == 0, m);
    }

    /* All 256 byte values, every length mod 3, round trip. */
    uint8_t all[256], back[256];
    char big[B64_ENCODED_LEN(256) + 1];
    for (int i = 0; i < 256; i++) all[i] = (uint8_t)i;
    int rt = 1;
    for (size_t n = 0; n <= 256; n++) {
        long e = b64_encode(all, n, big, sizeof(big));
        long d = e < 0 ? -1 : b64_decode(big, (size_t)e, back, sizeof(back));
        if (e != (long)B64_ENCODED_LEN(n) || d != (long)n || memcmp(all, back, n) != 0) rt = 0;
    }
    CHECK(rt, "round trip of 0..256 bytes");
    CHECK(b64_encode("\xfb\xff\xbf", 3, out, sizeof(out)) == 4 && strcmp(out, "+/+/") == 0, "'+' and '/' in the alphabet");

    /* Encode bounds: needs the NUL too. */
    char tiny[4];
    CHECK(b64_encode("foo", 3, tiny, sizeof(tiny)) == -1, "encode: no room for the NUL is refused");
    char five[5];
    CHECK(b64_encode("foo", 3, five, sizeof(five)) == 4 && strcmp(five, "Zm9v") == 0, "encode: exact fit");
    CHECK(b64_encode("", 0, tiny, 1) == 0 && tiny[0] == '\0', "encode: empty input into 1 byte");
    CHECK(b64_encode("", 0, tiny, 0) == -1, "encode: cap 0 refused");

    /* Strict decode refusals. */
    static const char *bad[] = {
        "Zg=", "Zg", "Zm9", "Zm9vY", "Zg===", "Zg=a", "=Zg=", "Z===", "====", "Zm9v====",
        "Zm9v Zg==", "Zm9v\n", " Zm9v", "Zm9v\r\n", "Zm-v", "Zm_v", "Zm9\x01", "Zm9\xff",
        "Zh==", "Zm9=", "Zg==Zg==", "Zm9vZg=a",
    };
    int ok = 1;
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        if (b64_decode(bad[i], strlen(bad[i]), raw, sizeof(raw)) != -1) {
            printf("    accepted: \"%s\"\n", bad[i]);
            ok = 0;
        }
    }
    CHECK(ok, "decode refuses bad length, alphabet, padding position, whitespace, non-canonical bits");
    CHECK(b64_decode("Zm9v", 4, raw, 2) == -1, "decode: output buffer too small");
    CHECK(b64_decode("Zm9v", 4, raw, 3) == 3, "decode: exact output buffer");
    CHECK(b64_decode("Zm8=", 4, raw, 1) == -1, "decode: too small for the padded tail");
    CHECK(b64_decode("", 0, raw, 0) == 0, "decode: empty input");
    /* Not NUL terminated: only n characters are read. */
    CHECK(b64_decode("Zm9vYmFy", 4, raw, sizeof(raw)) == 3 && memcmp(raw, "foo", 3) == 0, "decode reads exactly n characters");
}

int main(void)
{
    test_sha1();
    test_b64();
    printf("%s\n", fails ? "FAILED" : "all ok");
    return fails ? 1 : 0;
}
