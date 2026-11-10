/*
 * g711.h -- ITU-T G.711 mu-law / A-law codecs and L16 sample helpers.
 *
 * Pure code: no I/O, no allocation, no global state. Host-testable.
 * Samples are 16-bit signed linear PCM (full scale, as in ITU-T G.191 /
 * Sun g711.c); 8 kHz mono is the only format Akam uses.
 */
#ifndef AKAM_G711_H
#define AKAM_G711_H

#include <stddef.h>
#include <stdint.h>

/* Decode one G.711 byte to a 16-bit linear sample. */
int16_t g711_ulaw_decode(uint8_t code);
int16_t g711_alaw_decode(uint8_t code);

/* Encode one 16-bit linear sample (clipped at the codec's range). */
uint8_t g711_ulaw_encode(int16_t pcm);
uint8_t g711_alaw_encode(int16_t pcm);

/* Bulk helpers: n input items -> n output items. */
void g711_ulaw_decode_buf(const uint8_t *in, int16_t *out, size_t n);
void g711_alaw_decode_buf(const uint8_t *in, int16_t *out, size_t n);
void g711_ulaw_encode_buf(const int16_t *in, uint8_t *out, size_t n);
void g711_alaw_encode_buf(const int16_t *in, uint8_t *out, size_t n);

/* L16: two bytes per sample. "be" is network order (RFC 3551), "le" is the
 * little-endian layout of the wire protocol / ALSA S16_LE. */
int16_t g711_l16be_get(const uint8_t *p);
void    g711_l16be_put(uint8_t *p, int16_t s);
int16_t g711_l16le_get(const uint8_t *p);
void    g711_l16le_put(uint8_t *p, int16_t s);

/* Decode n_samples L16 samples (2*n_samples bytes) into out. */
void g711_l16be_decode_buf(const uint8_t *in, int16_t *out, size_t n_samples);
void g711_l16le_decode_buf(const uint8_t *in, int16_t *out, size_t n_samples);

#endif /* AKAM_G711_H */
