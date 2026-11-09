/* Host test of src/ak_rtsp/audio_jitter.c: prefill, bursts, underrun,
 * overrun, randomized comparison with a naive model, timing simulation and
 * a two-thread smoke test. Run under ASan/UBSan by tests/jitter_test.sh. */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio_jitter.h"

#define PERIOD 256 /* 32 ms at 8 kHz */

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; \
    printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static audio_jitter_t J; /* too big for the stack comfortably */

static void fill_seq(int16_t *p, size_t n, int *seq)
{
    for (size_t i = 0; i < n; i++)
        p[i] = (int16_t)(1 + (*seq)++ % 30000);
}

static void test_prefill_and_underrun(void)
{
    int16_t in[PERIOD], out[PERIOD];
    int seq = 0;
    size_t got;

    audio_jitter_init(&J);
    /* Pull before anything: silence, no underrun counted while pre-filling. */
    memset(out, 0x55, sizeof out);
    got = audio_jitter_pull(&J, out, PERIOD);
    CHECK(got == 0 && out[0] == 0 && out[PERIOD - 1] == 0, "silence before data");
    CHECK(audio_jitter_underruns(&J) == 0, "no underrun during prefill");

    /* Below prefill: still silence, data retained. */
    fill_seq(in, PERIOD, &seq);
    audio_jitter_push(&J, in, PERIOD);
    audio_jitter_push(&J, in, PERIOD);
    audio_jitter_push(&J, in, PERIOD); /* 768 < 960 */
    got = audio_jitter_pull(&J, out, PERIOD);
    CHECK(got == 0 && out[5] == 0, "still pre-filling");
    CHECK(audio_jitter_level(&J) == 768, "level kept");

    /* Reach prefill: audio flows in order. */
    audio_jitter_reset(&J);
    seq = 0;
    for (int k = 0; k < 4; k++) { /* 1024 >= 960 */
        fill_seq(in, PERIOD, &seq);
        audio_jitter_push(&J, in, PERIOD);
    }
    CHECK(audio_jitter_level_ms(&J) == 128, "level ms");
    for (int k = 0; k < 4; k++) {
        got = audio_jitter_pull(&J, out, PERIOD);
        CHECK(got == PERIOD, "full period %d", k);
        for (int i = 0; i < PERIOD; i++)
            if (out[i] != (int16_t)(1 + (k * PERIOD + i) % 30000)) {
                CHECK(0, "order k=%d i=%d", k, i);
                break;
            }
    }
    CHECK(audio_jitter_underruns(&J) == 0, "drained exactly: no underrun yet");

    /* Next pull: empty -> silence + underrun, back to prefill. */
    got = audio_jitter_pull(&J, out, PERIOD);
    CHECK(got == 0 && out[0] == 0, "underrun silence");
    CHECK(audio_jitter_underruns(&J) == 1, "underrun counted");
    got = audio_jitter_pull(&J, out, PERIOD);
    CHECK(audio_jitter_underruns(&J) == 1, "not counted twice while re-priming");

    /* Partial period: tail is zero-padded. */
    audio_jitter_reset(&J);
    {
        int16_t big[JITTER_PREFILL_SAMPLES + 100];
        int s2 = 0;
        fill_seq(big, 1000, &s2);
        audio_jitter_push(&J, big, 1000);
        for (int k = 0; k < 3; k++)
            audio_jitter_pull(&J, out, PERIOD); /* 768 consumed, 232 left */
        got = audio_jitter_pull(&J, out, PERIOD);
        CHECK(got == 232, "partial got %zu", got);
        CHECK(out[231] != 0 && out[232] == 0 && out[255] == 0, "zero padded");
        CHECK(audio_jitter_underruns(&J) == 2, "partial counts underrun");
    }
    audio_jitter_destroy(&J);
}

static void test_overrun(void)
{
    int16_t in[1400], out[JITTER_PREFILL_SAMPLES];
    int seq = 0;

    audio_jitter_init(&J);
    /* 3 x 1400 = 4200 samples > 3200 max. */
    fill_seq(in, 1400, &seq);
    audio_jitter_push(&J, in, 1400);
    fill_seq(in, 1400, &seq);
    audio_jitter_push(&J, in, 1400);
    CHECK(audio_jitter_level(&J) == 2800 && audio_jitter_drops(&J) == 0, "below max");
    fill_seq(in, 1400, &seq); /* newest sample value: seq=4200 -> 1+4199 */
    audio_jitter_push(&J, in, 1400);
    CHECK(audio_jitter_level(&J) == JITTER_PREFILL_SAMPLES, "trimmed to prefill: %zu",
          audio_jitter_level(&J));
    CHECK(audio_jitter_drops(&J) == 4200 - JITTER_PREFILL_SAMPLES, "drops %u",
          audio_jitter_drops(&J));
    CHECK(audio_jitter_pull(&J, out, JITTER_PREFILL_SAMPLES) == JITTER_PREFILL_SAMPLES, "pull");
    CHECK(out[JITTER_PREFILL_SAMPLES - 1] == 1 + 4199, "newest kept");
    CHECK(out[0] == 1 + 4200 - JITTER_PREFILL_SAMPLES, "contiguous tail");

    /* Single push larger than the whole ring. */
    {
        static int16_t huge[JITTER_MAX_SAMPLES * 3];
        audio_jitter_reset(&J);
        for (size_t i = 0; i < sizeof huge / sizeof *huge; i++)
            huge[i] = (int16_t)(i % 20000 + 1);
        audio_jitter_push(&J, huge, sizeof huge / sizeof *huge);
        CHECK(audio_jitter_level(&J) == JITTER_PREFILL_SAMPLES, "huge push level");
        audio_jitter_pull(&J, out, JITTER_PREFILL_SAMPLES);
        CHECK(out[JITTER_PREFILL_SAMPLES - 1] ==
                  (int16_t)((sizeof huge / sizeof *huge - 1) % 20000 + 1), "huge newest");
    }
    audio_jitter_destroy(&J);
}

/* Naive model: unbounded FIFO with the same rules. */
typedef struct { int16_t q[20000]; size_t n; int playing; uint32_t under, drops; } model_t;

static void model_push(model_t *m, const int16_t *in, size_t n)
{
    memcpy(m->q + m->n, in, n * 2);
    m->n += n;
    if (m->n > JITTER_MAX_SAMPLES) {
        size_t d = m->n - JITTER_PREFILL_SAMPLES;
        memmove(m->q, m->q + d, JITTER_PREFILL_SAMPLES * 2);
        m->drops += (uint32_t)d;
        m->n = JITTER_PREFILL_SAMPLES;
    }
    if (!m->playing && m->n >= JITTER_PREFILL_SAMPLES) m->playing = 1;
}

static size_t model_pull(model_t *m, int16_t *out, size_t n)
{
    size_t got = 0;
    memset(out, 0, n * 2);
    if (m->playing) {
        got = m->n < n ? m->n : n;
        memcpy(out, m->q, got * 2);
        memmove(m->q, m->q + got, (m->n - got) * 2);
        m->n -= got;
        if (got < n) { m->under++; m->playing = 0; }
    }
    return got;
}

static void test_random_vs_model(void)
{
    static model_t m;
    int16_t in[1400], a[PERIOD * 2], b[PERIOD * 2];
    int seq = 0;

    srand(12345);
    memset(&m, 0, sizeof m);
    audio_jitter_init(&J);
    for (int step = 0; step < 20000; step++) {
        if (rand() % 2) {
            size_t n = 1 + (size_t)(rand() % 1400);
            fill_seq(in, n, &seq);
            audio_jitter_push(&J, in, n);
            model_push(&m, in, n);
        } else {
            size_t n = 1 + (size_t)(rand() % (PERIOD * 2));
            size_t g1 = audio_jitter_pull(&J, a, n);
            size_t g2 = model_pull(&m, b, n);
            if (g1 != g2 || memcmp(a, b, n * 2) != 0) {
                CHECK(0, "model mismatch at step %d", step);
                break;
            }
        }
        if (audio_jitter_level(&J) != m.n || audio_jitter_underruns(&J) != m.under ||
            audio_jitter_drops(&J) != m.drops) {
            CHECK(0, "state mismatch at step %d", step);
            break;
        }
    }
    audio_jitter_destroy(&J);
}

/* Timing simulation: producer sends 20 ms packets with jitter, consumer
 * pulls 32 ms periods. Virtual clock in ms. */
static void test_timing(void)
{
    int16_t in[160], out[PERIOD];
    int seq = 0;
    long next_push = 0, next_pull = 0;

    audio_jitter_init(&J);
    srand(7);
    /* Phase 1: steady 20 ms packets with up to 30 ms jitter, 10 s. */
    for (long t = 0; t < 10000; t++) {
        if (t == next_push) {
            fill_seq(in, 160, &seq);
            audio_jitter_push(&J, in, 160);
            next_push += 20 + rand() % 3; /* slightly slow: 20..22 ms avg 21 */
        }
        if (t == next_pull) {
            audio_jitter_pull(&J, out, PERIOD);
            next_pull += 32;
        }
    }
    CHECK(audio_jitter_drops(&J) == 0, "no overrun in nominal run");
    /* 160 samples/20ms vs 256/32ms are both 8 samples/ms; sender at 21 ms avg
     * is slower, so underruns are expected but must stay bounded and recover. */
    CHECK(audio_jitter_underruns(&J) < 200, "bounded underruns: %u", audio_jitter_underruns(&J));

    /* Phase 2: sender stalls 1 s then sends a 1 s burst at once: overrun. */
    audio_jitter_reset(&J);
    {
        uint32_t d0 = audio_jitter_drops(&J);
        for (int k = 0; k < 50; k++) {
            fill_seq(in, 160, &seq);
            audio_jitter_push(&J, in, 160);
        }
        CHECK(audio_jitter_drops(&J) > d0, "burst causes drops");
        CHECK(audio_jitter_level_ms(&J) <= JITTER_MAX_MS, "bounded latency");
    }
    audio_jitter_destroy(&J);
}

static void *producer(void *arg)
{
    int16_t in[64];
    int seq = 0;
    (void)arg;
    for (int i = 0; i < 200000; i++) {
        fill_seq(in, 64, &seq);
        audio_jitter_push(&J, in, 64);
    }
    return NULL;
}

static void test_threads(void)
{
    pthread_t th;
    int16_t out[PERIOD];
    int16_t last = 0;
    int bad = 0;

    audio_jitter_init(&J);
    pthread_create(&th, NULL, producer, NULL);
    for (int i = 0; i < 200000; i++) {
        size_t got = audio_jitter_pull(&J, out, PERIOD);
        /* Delivered samples are a subsequence of 1..30000 cyclic: increasing
         * except at wrap; drops may skip. Just check non-zero values valid. */
        for (size_t k = 0; k < got; k++) {
            if (out[k] <= 0) bad++;
            last = out[k];
        }
    }
    pthread_join(th, NULL);
    (void)last;
    CHECK(bad == 0, "corrupt samples in threaded run: %d", bad);
    audio_jitter_destroy(&J);
}

int main(void)
{
    test_prefill_and_underrun();
    test_overrun();
    test_random_vs_model();
    test_timing();
    test_threads();
    if (fails) {
        printf("jitter_test: %d failure(s)\n", fails);
        return 1;
    }
    printf("jitter_test: OK\n");
    return 0;
}
