/* akam2k_mic_core -- the pure part of the akam-mic proof of concept: WAV header, level statistics in dBFS, and the
 * output path rule. No I/O, no libm (the dB maths uses its own log2), so it is host-testable and links on the camera
 * with plain libc. Samples are native int16; the camera (ARM, little endian) and the host test are both LE. */
#ifndef AKAM2K_MIC_CORE_H
#define AKAM2K_MIC_CORE_H

#include <stddef.h>
#include <stdint.h>

#define MIC_WAV_HDR_LEN 44u
#define MIC_DBFS_SILENCE (-999.0) /* returned for "no signal" (all zero samples, or no samples) */

/* Canonical 44-byte PCM WAV header: mono, 16 bit, `rate` Hz, `data_bytes` bytes of samples. */
void mic_wav_header(uint8_t out[MIC_WAV_HDR_LEN], uint32_t rate, uint32_t data_bytes);

struct mic_stats {
    uint64_t n;      /* samples seen */
    uint64_t sumsq;  /* sum of squares (n * 2^30 fits in 64 bits for n < 2^34) */
    int16_t min, max;
};

void mic_stats_init(struct mic_stats *s);
void mic_stats_add(struct mic_stats *s, const int16_t *x, size_t n);
/* 20*log10(|v| / 32768); MIC_DBFS_SILENCE for v == 0. */
double mic_sample_dbfs(int v);
/* Largest |sample| (0..32768) in dBFS. */
double mic_peak_dbfs(const struct mic_stats *s);
/* 10*log10(mean square / 32768^2). */
double mic_rms_dbfs(const struct mic_stats *s);
/* log2 for x > 0, absolute error below 1e-6 (exposed for the unit test). */
double mic_log2(double x);

/* The only paths the tool writes to without -f: "/tmp/NAME" with NAME of 1..64 chars from [A-Za-z0-9._-], no "..".
 * With force any non-empty path below 256 chars passes. Returns 1 when acceptable. */
int mic_path_ok(const char *path, int force);

#endif
