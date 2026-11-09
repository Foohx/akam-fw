/* Host test of src/ak_rtsp/g711.c: exhaustive decode vs an independent
 * reference, encode/decode round trips over all 65536 inputs, ITU vectors. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "g711.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; \
    printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* Independent reference: closed-form expansion per ITU-T G.711 tables. */
static int ref_ulaw(int code)
{
    int c = ~code & 0xFF, sign = c >> 7, e = (c >> 4) & 7, m = c & 15;
    int mag = (((2 * m + 33) << e) - 33) * 4;
    return sign ? -mag : mag;
}

static int ref_alaw(int code)
{
    int c = code ^ 0x55, sign = c >> 7, e = (c >> 4) & 7, m = c & 15;
    int mag = (e == 0 ? 2 * m + 1 : (2 * m + 33) << (e - 1)) * 8;
    return sign ? mag : -mag;
}

static int iabs(int x) { return x < 0 ? -x : x; }

/* Quantisation step of the segment holding x (loose bound for the error). */
static int ulaw_step(int x)
{
    int a = iabs(x) + 0x84, e = 0;
    while (e < 7 && a >= (0x100 << e)) e++;
    return 8 << e;
}

static int alaw_step(int x)
{
    int a = iabs(x) >> 3, e = 0;
    while (e < 7 && a > (0x20 << e) - 1) e++;
    return (e < 2 ? 2 : 1 << e) * 8;
}

int main(void)
{
    int i, x;

    for (i = 0; i < 256; i++) {
        CHECK(g711_ulaw_decode((uint8_t)i) == ref_ulaw(i), "ulaw dec %02x: %d vs %d",
              i, g711_ulaw_decode((uint8_t)i), ref_ulaw(i));
        CHECK(g711_alaw_decode((uint8_t)i) == ref_alaw(i), "alaw dec %02x: %d vs %d",
              i, g711_alaw_decode((uint8_t)i), ref_alaw(i));
        /* code -> pcm -> code is the identity (mu-law 0x7F is "-0": maps to 0xFF). */
        CHECK(g711_alaw_encode(g711_alaw_decode((uint8_t)i)) == i, "alaw rt %02x", i);
        CHECK(g711_ulaw_encode(g711_ulaw_decode((uint8_t)i)) == (i == 0x7F ? 0xFF : i),
              "ulaw rt %02x -> %02x", i, g711_ulaw_encode(g711_ulaw_decode((uint8_t)i)));
    }

    /* Known ITU vectors. */
    CHECK(g711_ulaw_decode(0xFF) == 0, "ulaw FF");
    CHECK(g711_ulaw_decode(0x7F) == 0, "ulaw 7F");
    CHECK(g711_ulaw_decode(0x80) == 32124, "ulaw 80");
    CHECK(g711_ulaw_decode(0x00) == -32124, "ulaw 00");
    CHECK(g711_alaw_decode(0xD5) == 8, "alaw D5");
    CHECK(g711_alaw_decode(0x55) == -8, "alaw 55");
    CHECK(g711_alaw_decode(0xAA) == 32256, "alaw AA");
    CHECK(g711_alaw_decode(0x2A) == -32256, "alaw 2A");
    CHECK(g711_ulaw_encode(0) == 0xFF, "ulaw enc 0");
    CHECK(g711_alaw_encode(0) == 0xD5, "alaw enc 0");
    CHECK(g711_alaw_encode(-1) == 0x55, "alaw enc -1");
    CHECK(g711_ulaw_encode(32767) == 0x80, "ulaw enc max");
    CHECK(g711_ulaw_encode(-32768) == 0x00, "ulaw enc min");
    CHECK(g711_alaw_encode(32767) == 0xAA, "alaw enc max");
    CHECK(g711_alaw_encode(-32768) == 0x2A, "alaw enc min");

    /* Every 16-bit input: error within one quantisation step, monotonic. */
    {
        int pu = -40000, pa = -40000;
        for (x = -32768; x <= 32767; x++) {
            int u = g711_ulaw_decode(g711_ulaw_encode((int16_t)x));
            int a = g711_alaw_decode(g711_alaw_encode((int16_t)x));
            CHECK(iabs(u - x) <= ulaw_step(x) || iabs(x) > 32635, "ulaw err x=%d u=%d", x, u);
            CHECK(iabs(a - x) <= alaw_step(x), "alaw err x=%d a=%d", x, a);
            CHECK(u >= pu, "ulaw not monotonic at %d", x);
            CHECK(a >= pa, "alaw not monotonic at %d", x);
            pu = u;
            pa = a;
            if (fails > 20) return 1;
        }
    }

    /* Buffer helpers and L16. */
    {
        uint8_t in[256], enc[256], b[4];
        int16_t pcm[256], one;

        for (i = 0; i < 256; i++) in[i] = (uint8_t)i;
        g711_ulaw_decode_buf(in, pcm, 256);
        g711_ulaw_encode_buf(pcm, enc, 256);
        for (i = 0; i < 256; i++)
            CHECK(enc[i] == (i == 0x7F ? 0xFF : i), "ulaw buf %d", i);
        g711_alaw_decode_buf(in, pcm, 256);
        g711_alaw_encode_buf(pcm, enc, 256);
        CHECK(memcmp(enc, in, 256) == 0, "alaw buf");

        g711_l16be_put(b, (int16_t)0x1234);
        CHECK(b[0] == 0x12 && b[1] == 0x34, "l16be put");
        CHECK(g711_l16be_get(b) == 0x1234, "l16be get");
        g711_l16le_put(b, (int16_t)-2);
        CHECK(b[0] == 0xFE && b[1] == 0xFF, "l16le put");
        CHECK(g711_l16le_get(b) == -2, "l16le get");
        g711_l16be_put(b, -32768);
        CHECK(g711_l16be_get(b) == -32768, "l16be min");
        for (i = 0; i < 256; i++) {
            int16_t s = (int16_t)(i * 257 - 32768);

            g711_l16be_put(b, s);
            g711_l16be_decode_buf(b, &one, 1);
            CHECK(one == s, "l16be buf");
            g711_l16le_put(b, s);
            g711_l16le_decode_buf(b, &one, 1);
            CHECK(one == s, "l16le buf");
        }
    }

    if (fails) {
        printf("g711_test: %d failure(s)\n", fails);
        return 1;
    }
    printf("g711_test: OK\n");
    return 0;
}
