/*
 * g711.c -- ITU-T G.711 mu-law / A-law codecs (segment/mantissa algorithm,
 * bit-exact with the classic Sun reference). See g711.h.
 */
#include "g711.h"

#define ULAW_BIAS 0x84
#define ULAW_CLIP 32635

int16_t g711_ulaw_decode(uint8_t code)
{
    int u = ~code & 0xFF;
    int t = (((u & 0x0F) << 3) + ULAW_BIAS) << ((u & 0x70) >> 4);
    return (int16_t)((u & 0x80) ? (ULAW_BIAS - t) : (t - ULAW_BIAS));
}

int16_t g711_alaw_decode(uint8_t code)
{
    int a = (code ^ 0x55) & 0xFF;
    int t = (a & 0x0F) << 4;
    int seg = (a & 0x70) >> 4;

    if (seg == 0)
        t += 8;
    else
        t = (t + 0x108) << (seg - 1);
    return (int16_t)((a & 0x80) ? t : -t);
}

uint8_t g711_ulaw_encode(int16_t pcm)
{
    int v = pcm;
    int sign = 0;
    int exp, mant;

    if (v < 0) {
        v = -v;
        sign = 0x80;
    }
    if (v > ULAW_CLIP)
        v = ULAW_CLIP;
    v += ULAW_BIAS;
    for (exp = 7; exp > 0 && !(v & (0x80 << exp)); exp--)
        ;
    mant = (v >> (exp + 3)) & 0x0F;
    return (uint8_t)~(sign | (exp << 4) | mant);
}

uint8_t g711_alaw_encode(int16_t pcm)
{
    int v = pcm >> 3; /* 13-bit; arithmetic shift of negatives (gcc/clang) */
    int mask, seg, out;

    if (v >= 0) {
        mask = 0xD5;
    } else {
        mask = 0x55;
        v = -v - 1;
    }
    if (v > 0xFFF)
        v = 0xFFF;
    for (seg = 0; seg < 7 && v > ((0x20 << seg) - 1); seg++)
        ;
    out = seg << 4;
    out |= (seg < 2) ? ((v >> 1) & 0x0F) : ((v >> seg) & 0x0F);
    return (uint8_t)(out ^ mask);
}

void g711_ulaw_decode_buf(const uint8_t *in, int16_t *out, size_t n)
{
    for (size_t i = 0; i < n; i++)
        out[i] = g711_ulaw_decode(in[i]);
}

void g711_alaw_decode_buf(const uint8_t *in, int16_t *out, size_t n)
{
    for (size_t i = 0; i < n; i++)
        out[i] = g711_alaw_decode(in[i]);
}

void g711_ulaw_encode_buf(const int16_t *in, uint8_t *out, size_t n)
{
    for (size_t i = 0; i < n; i++)
        out[i] = g711_ulaw_encode(in[i]);
}

void g711_alaw_encode_buf(const int16_t *in, uint8_t *out, size_t n)
{
    for (size_t i = 0; i < n; i++)
        out[i] = g711_alaw_encode(in[i]);
}

int16_t g711_l16be_get(const uint8_t *p)
{
    return (int16_t)(uint16_t)((p[0] << 8) | p[1]);
}

void g711_l16be_put(uint8_t *p, int16_t s)
{
    p[0] = (uint8_t)((uint16_t)s >> 8);
    p[1] = (uint8_t)((uint16_t)s & 0xFF);
}

int16_t g711_l16le_get(const uint8_t *p)
{
    return (int16_t)(uint16_t)((p[1] << 8) | p[0]);
}

void g711_l16le_put(uint8_t *p, int16_t s)
{
    p[0] = (uint8_t)((uint16_t)s & 0xFF);
    p[1] = (uint8_t)((uint16_t)s >> 8);
}

void g711_l16be_decode_buf(const uint8_t *in, int16_t *out, size_t n)
{
    for (size_t i = 0; i < n; i++)
        out[i] = g711_l16be_get(in + 2 * i);
}

void g711_l16le_decode_buf(const uint8_t *in, int16_t *out, size_t n)
{
    for (size_t i = 0; i < n; i++)
        out[i] = g711_l16le_get(in + 2 * i);
}
