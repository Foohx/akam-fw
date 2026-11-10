/* Host test of src/ak_rtsp/audio_vad.c: the energy gate of the half-duplex
 * rule. Build/run: tests/audio_vad_test.sh */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "audio_vad.h"

static int fails;
#define CHECK(cond, msg) do { if (cond) printf("  ok   - %s\n", msg); \
    else { printf("  FAIL - %s\n", msg); fails++; } } while (0)

/* n samples of a sine of the given RMS level in dBFS (at 440 Hz). */
static void sine(int16_t *out, size_t n, double dbfs)
{
    double amp = 32768.0 * pow(10.0, dbfs / 20.0) * 1.41421356;
    for (size_t i = 0; i < n; i++)
        out[i] = (int16_t)lrint(amp * sin(2 * M_PI * 440.0 * (double)i / 8000.0));
}

#define MS 1000LL

int main(void)
{
    int16_t buf[160 * 10];
    audio_vad_t v;

    printf("# threshold\n");
    audio_vad_init(&v, AVAD_DEFAULT_HOLD_MS);
    CHECK(!audio_vad_speech(&v, 0), "open at start");
    sine(buf, 1600, -60.0);
    audio_vad_feed(&v, buf, 1600, 1000 * MS, 0);
    CHECK(!audio_vad_speech(&v, 1000 * MS), "-60 dBFS noise does not close the gate");
    sine(buf, 1600, -47.0);
    audio_vad_feed(&v, buf, 1600, 1100 * MS, 0);
    CHECK(!audio_vad_speech(&v, 1100 * MS), "-47 dBFS stays below the threshold");
    sine(buf, 1600, -43.0);
    audio_vad_feed(&v, buf, 1600, 1200 * MS, 0);
    CHECK(audio_vad_speech(&v, 1200 * MS), "-43 dBFS is speech");

    printf("# hold time\n");
    audio_vad_init(&v, 300);
    sine(buf, 160, -20.0);
    audio_vad_feed(&v, buf, 160, 5000 * MS, 0);
    CHECK(audio_vad_speech(&v, 5000 * MS), "one loud 20 ms window closes the gate");
    CHECK(audio_vad_speech(&v, 5299 * MS), "still closed 299 ms after");
    CHECK(!audio_vad_speech(&v, 5300 * MS), "open again at 300 ms");
    memset(buf, 0, sizeof(buf));
    audio_vad_feed(&v, buf, 1600, 5100 * MS, 0);
    CHECK(audio_vad_speech(&v, 5299 * MS), "silence frames do not extend or end the hold");
    sine(buf, 160, -20.0);
    audio_vad_feed(&v, buf, 160, 5250 * MS, 0);
    CHECK(audio_vad_speech(&v, 5549 * MS) && !audio_vad_speech(&v, 5550 * MS), "new speech extends the hold from its own time");

    printf("# jitter delay\n");
    audio_vad_init(&v, 300);
    sine(buf, 160, -20.0);
    audio_vad_feed(&v, buf, 160, 0, 120);
    CHECK(audio_vad_speech(&v, 419 * MS) && !audio_vad_speech(&v, 420 * MS), "queued audio (120 ms) is not heard yet: hold is 300 + 120 ms");

    printf("# windows and packet sizes\n");
    audio_vad_init(&v, 300);
    sine(buf, 160 * 10, -20.0);
    for (int i = 0; i < 10; i++) audio_vad_feed(&v, buf + i * 160, 160, i * 20 * MS, 0);
    CHECK(v.speech_windows == 10, "10 packets of 20 ms: 10 windows");
    audio_vad_init(&v, 300);
    for (int i = 0; i < 160; i++) audio_vad_feed(&v, buf + i, 1, i * MS, 0);
    CHECK(v.speech_windows == 1 && audio_vad_speech(&v, 160 * MS), "sample by sample: windows still counted");
    audio_vad_init(&v, 300);
    sine(buf, 80, -20.0);
    audio_vad_feed(&v, buf, 80, 0, 0);
    CHECK(!audio_vad_speech(&v, 0), "half a window is not judged yet");
    audio_vad_feed(&v, buf, 80, 10 * MS, 0);
    CHECK(audio_vad_speech(&v, 10 * MS), "the window completes across two packets");

    printf("# extremes\n");
    audio_vad_init(&v, 300);
    for (int i = 0; i < 160; i++) buf[i] = (i & 1) ? 32767 : -32768;
    audio_vad_feed(&v, buf, 160, 0, 0);
    CHECK(audio_vad_speech(&v, 1), "full scale: no overflow, speech");
    audio_vad_reset(&v);
    CHECK(!audio_vad_speech(&v, 1), "reset reopens the gate");

    printf(fails ? "\nFAILED: %d\n" : "\naudio_vad_test: all passed\n", fails);
    return fails ? 1 : 0;
}
