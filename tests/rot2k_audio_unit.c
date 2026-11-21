/* Host unit test of the microphone hand-off of the Rotatable 2K own app (app/aring.c): the multi-reader ring
 * (fixed memory, drop-oldest for a slow reader, never blocks the producer), the wake pipes, the stream position over a
 * capture gap, the L16 payload byte order and the RTP timestamp, plus the start gate / audio clock of the shared
 * src/net/rtsp/rtp_clock.c (compiled unmodified) the way rtsp_session.c drives them. Prints "rot2k audio unit: N failed". */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../platforms/anyka-ak3918av130/aring.h"
#include "../src/net/rtsp/rtp_clock.h"

static int fails;
#define CHECK(c, msg) do { if (c) printf("  ok   - %s\n", msg); else { printf("  FAIL - %s\n", msg); fails++; } } while (0)

static a2k_aring_t R;

static void fill(int16_t *p, size_t n, int16_t v)
{
    size_t i;
    for (i = 0; i < n; i++)
        p[i] = v;
}

/* ---- threaded fan-out: one producer, a fast and a slow reader --------------------------------------------------- */
#define NPERIODS 4000
static volatile int prod_done;
static void *producer(void *arg)
{
    int16_t b[A2K_APERIOD_SAMPLES];
    int i;
    (void)arg;
    for (i = 0; i < NPERIODS; i++) {
        fill(b, A2K_APERIOD_SAMPLES, (int16_t)(i & 0x7fff));
        a2k_aring_publish(&R, b, A2K_APERIOD_SAMPLES, i);
        if (i % 64 == 0)
            usleep(200);
    }
    __atomic_store_n(&prod_done, 1, __ATOMIC_SEQ_CST);
    return NULL;
}

struct rd {
    int slow;
    unsigned long got, lost_events, torn, order_errs, content_errs;
};

static void *reader(void *arg)
{
    struct rd *r = arg;
    uint64_t cur = 0, last_first = 0;
    int have_last = 0, empty_after_done = 0;
    while (empty_after_done < 3) {
        int16_t b[A2K_APERIOD_SAMPLES];
        size_t n = 0, i;
        uint64_t first = 0;
        int64_t t = 0;
        int lost = 0;
        if (a2k_aring_next(&R, &cur, b, &n, &first, &t, &lost)) {
            empty_after_done = 0;
            r->got++;
            if (lost)
                r->lost_events++;
            for (i = 1; i < n; i++)
                if (b[i] != b[0])
                    r->torn++; /* a period is copied under the lock: it can never be half old, half new */
            if (n != A2K_APERIOD_SAMPLES || b[0] != (int16_t)(t & 0x7fff))
                r->content_errs++; /* t_end_us was set to the period index */
            if (first != (uint64_t)t * A2K_APERIOD_SAMPLES)
                r->content_errs++; /* first_sample keeps counting even where this reader lost periods */
            if (have_last && first <= last_first)
                r->order_errs++;
            last_first = first;
            have_last = 1;
            if (r->slow)
                usleep(300);
        } else {
            if (__atomic_load_n(&prod_done, __ATOMIC_SEQ_CST))
                empty_after_done++;
            usleep(100);
        }
    }
    return NULL;
}

int main(void)
{
    int16_t in[A2K_APERIOD_SAMPLES], out[A2K_APERIOD_SAMPLES];
    size_t n;
    uint64_t cur, first, c2;
    int64_t t;
    int lost, i, p[2];

    /* ---- memory is fixed and small ---- */
    CHECK(sizeof(a2k_aring_t) <= 20u * 1024, "the whole ring is at most 20 KiB (32 slots x 512 bytes + headers)");
    CHECK(A2K_APERIOD_SAMPLES * 2 == 512 && A2K_ASLOTS * A2K_APERIOD_SAMPLES * 1000 / A2K_AUDIO_RATE == 1024, "a period is the 512-byte capture period, the ring holds 1.024 s");

    a2k_aring_init(&R);
    cur = 0;
    CHECK(!a2k_aring_next(&R, &cur, out, &n, &first, &t, &lost) && !lost, "an empty ring gives nothing");

    /* ---- basic publish / read ---- */
    for (i = 0; i < (int)A2K_APERIOD_SAMPLES; i++)
        in[i] = (int16_t)(i - 100);
    CHECK(a2k_aring_publish(&R, in, A2K_APERIOD_SAMPLES, 1000) == 0, "publish a full period");
    CHECK(a2k_aring_publish(&R, in, 0, 1) == -1 && a2k_aring_publish(&R, NULL, 5, 1) == -1, "an empty period is refused");
    CHECK(a2k_aring_head(&R) == 1, "head counts published periods");
    cur = 0;
    CHECK(a2k_aring_next(&R, &cur, out, &n, &first, &t, &lost) == 1 && n == 256 && first == 0 && t == 1000 && !lost &&
              !memcmp(in, out, sizeof in) && cur == 1,
          "the period comes back intact with first_sample 0 and its capture time");
    CHECK(!a2k_aring_next(&R, &cur, out, &n, &first, &t, &lost), "and only once per reader");

    /* ---- the hardware reads 128..512 bytes: the stream position follows the real sample counts ---- */
    fill(in, 64, 7);
    a2k_aring_publish(&R, in, 64, 2000);  /* 128 bytes */
    fill(in, 100, 8);
    a2k_aring_publish(&R, in, 100, 3000); /* 200 bytes */
    CHECK(a2k_aring_next(&R, &cur, out, &n, &first, &t, &lost) && n == 64 && first == 256 && out[0] == 7,
          "a 128-byte read is a 64-sample period at stream position 256");
    CHECK(a2k_aring_next(&R, &cur, out, &n, &first, &t, &lost) && n == 100 && first == 320 && out[99] == 8,
          "a 200-byte read follows at 320 (positions advance by the samples actually read)");
    fill(in, A2K_APERIOD_SAMPLES, 1);
    a2k_aring_publish(&R, in, A2K_APERIOD_SAMPLES + 50, 4000);
    CHECK(R.truncated == 1 && a2k_aring_next(&R, &cur, out, &n, &first, &t, &lost) && n == A2K_APERIOD_SAMPLES,
          "an oversized read is cut to one period and counted (the buffer is never overrun)");

    /* ---- slow reader: drop-oldest, told once, resumes at the oldest period still held ---- */
    a2k_aring_destroy(&R);
    a2k_aring_init(&R);
    for (i = 0; i < A2K_ASLOTS + 8; i++) {
        fill(in, A2K_APERIOD_SAMPLES, (int16_t)i);
        a2k_aring_publish(&R, in, A2K_APERIOD_SAMPLES, i);
    }
    cur = 0;
    CHECK(a2k_aring_next(&R, &cur, out, &n, &first, &t, &lost) && lost == 1 && out[0] == 8 && cur == 9 &&
              first == 8u * A2K_APERIOD_SAMPLES,
          "a reader 40 periods behind jumps to the 32nd newest (period 8), is told, and first_sample shows the 8 lost periods");
    CHECK(a2k_aring_next(&R, &cur, out, &n, &first, &t, &lost) && !lost && out[0] == 9, "then it reads on in order, no further loss");
    c2 = a2k_aring_head(&R); /* a second reader is independent of the first */
    cur = c2 - 1;
    CHECK(a2k_aring_next(&R, &cur, out, &n, &first, &t, &lost) && out[0] == (int16_t)(A2K_ASLOTS + 7) && !lost,
          "readers have their own cursors (a reader at the head reads the newest period)");
    cur = c2;
    CHECK(!a2k_aring_next(&R, &cur, out, &n, &first, &t, &lost), "a reader at the head gets nothing until the next publish");

    /* ---- capture gap: the timeline keeps its place ---- */
    {
        uint64_t before = R.samples_total;
        a2k_aring_gap(&R, 1000000);
        CHECK(R.samples_total == before + 8000, "a 1 s capture gap advances the position by 8000 samples");
        a2k_aring_gap(&R, 125000);
        CHECK(R.samples_total == before + 9000, "125 ms is 1000 samples");
        a2k_aring_gap(&R, -5);
        a2k_aring_gap(&R, 0);
        CHECK(R.samples_total == before + 9000, "a zero or negative gap changes nothing");
        a2k_aring_publish(&R, in, A2K_APERIOD_SAMPLES, 1);
        cur = a2k_aring_head(&R) - 1;
        a2k_aring_next(&R, &cur, out, &n, &first, &t, &lost);
        CHECK(first == before + 9000, "the next period starts after the gap (the RTP timestamp jumps, no overlap)");
    }

    /* ---- wake pipes ---- */
    CHECK(pipe(p) == 0, "pipe");
    fcntl(p[0], F_SETFL, O_NONBLOCK);
    fcntl(p[1], F_SETFL, O_NONBLOCK);
    CHECK(a2k_aring_wake_add(&R, p[1]) == 0, "a reader registers its wake pipe");
    {
        char junk[8];
        a2k_aring_publish(&R, in, A2K_APERIOD_SAMPLES, 5);
        CHECK(read(p[0], junk, sizeof junk) == 1, "a publish writes one wake byte");
        for (i = 0; i < 200000; i++) /* nobody drains it: a full pipe must never block the capture thread */
            a2k_aring_publish(&R, in, A2K_APERIOD_SAMPLES, 6);
        CHECK(1, "200000 publishes into a full wake pipe did not block");
        a2k_aring_wake_del(&R, p[1]);
        while (read(p[0], junk, sizeof junk) > 0) {
        }
        a2k_aring_publish(&R, in, A2K_APERIOD_SAMPLES, 7);
        CHECK(read(p[0], junk, sizeof junk) < 0 && errno == EAGAIN, "after the delete no more wake bytes");
    }
    {
        int fds[A2K_AMAX_WAKE + 1], ok = 0;
        for (i = 0; i < A2K_AMAX_WAKE + 1; i++)
            fds[i] = 100 + i;
        for (i = 0; i < A2K_AMAX_WAKE; i++)
            ok += a2k_aring_wake_add(&R, fds[i]) == 0;
        CHECK(ok == A2K_AMAX_WAKE && a2k_aring_wake_add(&R, fds[A2K_AMAX_WAKE]) == -1, "at most A2K_AMAX_WAKE readers (the 9th is refused)");
        for (i = 0; i < A2K_AMAX_WAKE; i++)
            a2k_aring_wake_del(&R, fds[i]);
        CHECK(R.nwake == 0, "all removed again");
    }
    close(p[0]);
    close(p[1]);

    /* ---- L16 payload: network byte order whatever the host ---- */
    {
        const int16_t s[4] = { 0x1234, -2, 0, (int16_t)0x8000 };
        uint8_t b[8];
        a2k_audio_pack_be(b, s, 4);
        CHECK(b[0] == 0x12 && b[1] == 0x34 && b[2] == 0xff && b[3] == 0xfe && b[4] == 0 && b[5] == 0 && b[6] == 0x80 && b[7] == 0,
              "samples are big-endian (RFC 3551 L16), payload is 2 bytes per sample (512 for a full period)");
    }

    /* ---- RTP timestamps: origin + capture-stream sample index (8000 Hz clock) ---- */
    CHECK(a2k_audio_rtp_ts(1000, 0) == 1000 && a2k_audio_rtp_ts(1000, 256) == 1256, "timestamp = base + sample index");
    CHECK(a2k_audio_rtp_ts(0xFFFFFFF0u, 32) == 0x10u, "wraps modulo 2^32");
    CHECK(a2k_audio_rtp_ts(5, (1ull << 32) + 7) == 12u, "a 64-bit sample index is truncated like the 1080p (uint32_t)");
    {
        uint64_t f0 = 1000;
        CHECK(a2k_audio_rtp_ts(77, f0 + 256) - a2k_audio_rtp_ts(77, f0) == 256, "consecutive full periods are 256 ticks apart (32 ms at 8 kHz)");
    }

    /* ---- start gate and audio clock of the shared rtp_clock.c, as the RTSP client drives them ---- */
    {
        rtp_astart_t g;
        rtp_aclock_t ac;
        memset(&g, 0, sizeof g);
        CHECK(!rtp_astart_holding(&g), "a zeroed client (no audio track) is not holding");
        rtp_astart_arm(&g);
        CHECK(rtp_astart_holding(&g), "armed at PLAY: audio is held");
        rtp_astart_release(&g, 10000000); /* the first video frame was captured at t = 10 s */
        CHECK(!rtp_astart_holding(&g), "released by the first sent video frame");
        CHECK(rtp_astart_stale(&g, 9990000) && rtp_astart_stale(&g, 9999999), "periods that ended before the video frame are backlog");
        CHECK(!rtp_astart_stale(&g, 10000000 + 20000), "the first period after it passes");
        CHECK(!rtp_astart_stale(&g, 9000000), "and once one passed the gate stays open");
        rtp_astart_arm(&g); /* resync after a stall */
        CHECK(rtp_astart_holding(&g), "re-armed after a stall");
        rtp_astart_release(&g, 20000000);
        CHECK(rtp_astart_stale(&g, 19000000), "and drops the backlog again");

        rtp_aclock_init(&ac);
        rtp_aclock_observe(&ac, 256, 1000000 + 32000, A2K_AUDIO_RATE);
        rtp_aclock_observe(&ac, 512, 1000000 + 64000 + 3000, A2K_AUDIO_RATE); /* a late wake-up: the lower envelope holds */
        CHECK(rtp_aclock_samples_at(&ac, 1000000 + 32000 + 125000, A2K_AUDIO_RATE) >= 256 + 998 && rtp_aclock_samples_at(&ac, 1000000 + 32000 + 125000, A2K_AUDIO_RATE) <= 256 + 1000,
              "audio clock: 125 ms after a period end is 1000 samples further (sample counter, not wall time)");
    }

    /* ---- threads: a fast and a slow reader against a producer that never waits ---- */
    a2k_aring_destroy(&R);
    a2k_aring_init(&R);
    {
        pthread_t tp, tr[2];
        struct rd rd[2];
        memset(rd, 0, sizeof rd);
        rd[1].slow = 1;
        prod_done = 0;
        pthread_create(&tr[0], NULL, reader, &rd[0]);
        pthread_create(&tr[1], NULL, reader, &rd[1]);
        pthread_create(&tp, NULL, producer, NULL);
        pthread_join(tp, NULL);
        pthread_join(tr[0], NULL);
        pthread_join(tr[1], NULL);
        CHECK(R.periods == NPERIODS, "producer published every period without waiting for the readers");
        CHECK(rd[0].torn == 0 && rd[1].torn == 0, "no reader ever saw a half-written period");
        CHECK(rd[0].order_errs == 0 && rd[1].order_errs == 0 && rd[0].content_errs == 0 && rd[1].content_errs == 0,
              "periods arrive in order with the right stream position, also for the reader that lost some");
        CHECK(rd[0].got + rd[0].lost_events > 0 && rd[1].got < NPERIODS, "the slow reader dropped periods (drop oldest), the fast one kept up better");
        printf("  info - fast reader %lu periods (%lu losses), slow reader %lu (%lu losses)\n", rd[0].got, rd[0].lost_events,
               rd[1].got, rd[1].lost_events);
    }
    a2k_aring_destroy(&R);

    printf("rot2k audio unit: %d failed\n", fails);
    return fails ? 1 : 0;
}
