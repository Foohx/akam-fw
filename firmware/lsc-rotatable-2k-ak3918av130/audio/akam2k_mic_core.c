#include "akam2k_mic_core.h"

#include <string.h>

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

void mic_wav_header(uint8_t out[MIC_WAV_HDR_LEN], uint32_t rate, uint32_t data_bytes)
{
    memcpy(out, "RIFF", 4);
    put_le32(out + 4, 36u + data_bytes);
    memcpy(out + 8, "WAVEfmt ", 8);
    put_le32(out + 16, 16);          /* fmt chunk size */
    out[20] = 1; out[21] = 0;        /* PCM */
    out[22] = 1; out[23] = 0;        /* mono */
    put_le32(out + 24, rate);
    put_le32(out + 28, rate * 2u);   /* byte rate */
    out[32] = 2; out[33] = 0;        /* block align */
    out[34] = 16; out[35] = 0;       /* bits */
    memcpy(out + 36, "data", 4);
    put_le32(out + 40, data_bytes);
}

void mic_stats_init(struct mic_stats *s)
{
    s->n = 0;
    s->sumsq = 0;
    s->min = 32767;
    s->max = -32768;
}

void mic_stats_add(struct mic_stats *s, const int16_t *x, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        int v = x[i];
        if (v < s->min) s->min = (int16_t)v;
        if (v > s->max) s->max = (int16_t)v;
        s->sumsq += (uint64_t)((int64_t)v * v);
    }
    s->n += n;
}

double mic_log2(double x)
{
    int e = 0;
    double frac = 0.0, bit = 0.5;
    while (x >= 2.0) { x /= 2.0; e++; }
    while (x < 1.0) { x *= 2.0; e--; }
    for (int i = 0; i < 40; i++) { /* binary digits of log2(x), x in [1,2) */
        x *= x;
        if (x >= 2.0) { x /= 2.0; frac += bit; }
        bit /= 2.0;
    }
    return (double)e + frac;
}

#define DB_PER_LOG2_AMP 6.020599913279624  /* 20*log10(2) */
#define DB_PER_LOG2_PWR 3.010299956639812  /* 10*log10(2) */

double mic_sample_dbfs(int v)
{
    if (v == 0) return MIC_DBFS_SILENCE;
    if (v < 0) v = -v;
    return DB_PER_LOG2_AMP * (mic_log2((double)v) - 15.0); /* 32768 = 2^15 */
}

double mic_peak_dbfs(const struct mic_stats *s)
{
    if (s->n == 0) return MIC_DBFS_SILENCE;
    int a = s->max, b = -(int)s->min;
    return mic_sample_dbfs(a > b ? a : b);
}

double mic_rms_dbfs(const struct mic_stats *s)
{
    if (s->n == 0 || s->sumsq == 0) return MIC_DBFS_SILENCE;
    double ms = (double)s->sumsq / (double)s->n; /* mean square */
    return DB_PER_LOG2_PWR * (mic_log2(ms) - 30.0);
}

int mic_path_ok(const char *path, int force)
{
    size_t n = path ? strlen(path) : 0;
    if (n == 0 || n >= 256) return 0;
    if (force) return 1;
    if (strncmp(path, "/tmp/", 5) != 0) return 0;
    const char *name = path + 5;
    size_t m = n - 5;
    if (m < 1 || m > 64) return 0;
    for (size_t i = 0; i < m; i++) {
        char c = name[i];
        int good = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        if (!good) return 0;
    }
    if (strstr(name, "..")) return 0;
    return 1;
}
