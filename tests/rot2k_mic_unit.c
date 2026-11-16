/* Host unit test of the pure part of akam2k_mic (audio/akam2k_mic_core.c): the WAV header, the dBFS and RMS maths and
 * the output path rule. Prints "rot2k mic unit: N failed". */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "akam2k_mic_core.h"

static int fails;
#define CHECK(c, msg) do { if (c) printf("  ok   - %s\n", msg); else { printf("  FAIL - %s\n", msg); fails++; } } while (0)

static uint32_t le32(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

int main(void)
{
    uint8_t h[MIC_WAV_HDR_LEN + 4];
    memset(h, 0xAA, sizeof(h));
    mic_wav_header(h, 8000, 80000);
    CHECK(!memcmp(h, "RIFF", 4) && !memcmp(h + 8, "WAVEfmt ", 8) && !memcmp(h + 36, "data", 4), "header tags");
    CHECK(le32(h + 4) == 36 + 80000 && le32(h + 40) == 80000, "RIFF size = 36 + data, data size");
    CHECK(le32(h + 16) == 16 && le16(h + 20) == 1 && le16(h + 22) == 1 && le16(h + 34) == 16, "fmt: 16 bytes, PCM, mono, 16 bit");
    CHECK(le32(h + 24) == 8000 && le32(h + 28) == 16000 && le16(h + 32) == 2, "rate, byte rate, block align (8 kHz)");
    CHECK(h[44] == 0xAA && h[47] == 0xAA, "writes exactly 44 bytes");
    mic_wav_header(h, 16000, 960000);
    CHECK(le32(h + 24) == 16000 && le32(h + 28) == 32000 && le32(h + 4) == 960036, "16 kHz, 30 s");
    mic_wav_header(h, 8000, 0);
    CHECK(le32(h + 4) == 36 && le32(h + 40) == 0, "empty file header");
    mic_wav_header(h, 8000, 0xFFFFFFFFu - 36u);
    CHECK(le32(h + 4) == 0xFFFFFFFFu, "largest data size does not overflow RIFF size");

    double worst = 0;
    for (double x = 1e-3; x < 1e9; x *= 1.37) {
        double e = fabs(mic_log2(x) - log2(x));
        if (e > worst) worst = e;
    }
    CHECK(worst < 1e-6, "own log2 matches libm log2 over 1e-3..1e9");

    CHECK(fabs(mic_sample_dbfs(32768) - 0.0) < 1e-6 && fabs(mic_sample_dbfs(-32768) - 0.0) < 1e-6, "full scale = 0 dBFS (both signs)");
    CHECK(fabs(mic_sample_dbfs(16384) + 6.0206) < 1e-3, "half scale = -6.02 dBFS");
    CHECK(fabs(mic_sample_dbfs(32767) - 20 * log10(32767.0 / 32768)) < 1e-4, "32767 just under 0 dBFS");
    CHECK(fabs(mic_sample_dbfs(1) - 20 * log10(1.0 / 32768)) < 1e-4, "1 LSB = -90.31 dBFS");
    CHECK(mic_sample_dbfs(0) == MIC_DBFS_SILENCE, "zero sample = silence marker");

    struct mic_stats s;
    mic_stats_init(&s);
    CHECK(mic_peak_dbfs(&s) == MIC_DBFS_SILENCE && mic_rms_dbfs(&s) == MIC_DBFS_SILENCE, "no samples = silence marker");
    int16_t zeros[64] = {0};
    mic_stats_add(&s, zeros, 64);
    CHECK(s.n == 64 && mic_rms_dbfs(&s) == MIC_DBFS_SILENCE && mic_peak_dbfs(&s) == MIC_DBFS_SILENCE, "all zeros = silence marker");

    mic_stats_init(&s);
    int16_t sq[4] = {16384, -16384, 16384, -16384};
    mic_stats_add(&s, sq, 4);
    CHECK(s.min == -16384 && s.max == 16384, "min/max");
    CHECK(fabs(mic_rms_dbfs(&s) + 6.0206) < 1e-3, "square wave at half scale: RMS = -6.02 dBFS");
    CHECK(fabs(mic_peak_dbfs(&s) + 6.0206) < 1e-3, "square wave at half scale: peak = -6.02 dBFS");

    mic_stats_init(&s);
    int16_t neg[3] = {-32768, 100, 5};
    mic_stats_add(&s, neg, 3);
    CHECK(fabs(mic_peak_dbfs(&s)) < 1e-6, "most negative sample = 0 dBFS peak (|-32768|)");
    CHECK(s.n == 3 && s.sumsq == 1073741824ull + 10000 + 25, "sum of squares is exact in 64 bits");

    /* a 1 kHz-like sine at amplitude 8192: RMS = -12.04 - 3.01 dBFS, accumulated over two calls */
    mic_stats_init(&s);
    int16_t sine[8000];
    for (int i = 0; i < 8000; i++) sine[i] = (int16_t)lround(8192 * sin(2 * M_PI * 1000 * i / 8000.0));
    mic_stats_add(&s, sine, 4000);
    mic_stats_add(&s, sine + 4000, 4000);
    CHECK(s.n == 8000 && fabs(mic_rms_dbfs(&s) - (20 * log10(8192.0 / 32768) - 3.0103)) < 0.01, "sine amplitude 8192: RMS about -15.05 dBFS");
    CHECK(fabs(mic_peak_dbfs(&s) - (-12.0412)) < 0.01, "sine amplitude 8192: peak about -12.04 dBFS");

    CHECK(mic_path_ok("/tmp/mic.wav", 0) && mic_path_ok("/tmp/a-B_c.1.wav", 0), "paths under /tmp");
    CHECK(!mic_path_ok(NULL, 0) && !mic_path_ok("", 0) && !mic_path_ok("/tmp/", 0), "empty / bare dir refused");
    CHECK(!mic_path_ok("/tmp/../etc/passwd", 0) && !mic_path_ok("/tmp/a..b", 0), "dot-dot refused");
    CHECK(!mic_path_ok("/tmp/sub/x.wav", 0), "no subdirectories (no slash in the name)");
    CHECK(!mic_path_ok("/mnt/sdcard/x.wav", 0) && !mic_path_ok("mic.wav", 0) && !mic_path_ok("/tmpx/a", 0), "outside /tmp refused");
    CHECK(!mic_path_ok("/tmp/a b", 0) && !mic_path_ok("/tmp/a;b", 0), "odd characters refused");
    char longp[300];
    memset(longp, 'a', sizeof(longp));
    memcpy(longp, "/tmp/", 5);
    longp[5 + 64] = 0;
    CHECK(mic_path_ok(longp, 0), "64-char name accepted");
    longp[5 + 65] = 'a';
    longp[5 + 65 + 1] = 0;
    longp[5 + 64] = 'a';
    CHECK(!mic_path_ok(longp, 0), "65-char name refused");
    CHECK(mic_path_ok("/mnt/sdcard/x.wav", 1) && !mic_path_ok("", 1), "-f accepts any non-empty path");
    longp[299] = 0;
    memset(longp + 5, 'a', 294);
    CHECK(!mic_path_ok(longp, 1), "path of 256+ chars refused even with -f");

    printf("rot2k mic unit: %d failed\n", fails);
    return fails ? 1 : 0;
}
