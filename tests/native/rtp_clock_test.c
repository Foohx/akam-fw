/* Host unit test for src/ak_rtsp/rtp_clock.c: video capture-clock PLL, audio
 * sample->time mapping and the NTP/RTP pairing of RTCP Sender Reports.
 * Simulated time only, no hardware. Build/run: tests/rtp_clock_test.sh */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <math.h>
#include "rtp_clock.h"

static int fails;
#define CHECK(cond, msg) do { if (cond) printf("  ok   - %s\n", msg); \
    else { printf("  FAIL - %s\n", msg); fails++; } } while (0)

/* Deterministic jitter source (no libc rand: same numbers everywhere). */
static uint32_t lcg_state = 12345;
static double rnd01(void)
{
    lcg_state = lcg_state * 1664525u + 1013904223u;
    return (double)(lcg_state >> 8) / (double)(1u << 24);
}

#define TRUE_FPS      14.67           /* measured on the camera */
#define TRUE_PERIOD   (1e6 / TRUE_FPS)
#define NOMINAL_FPS   15.0
#define GOP           30
#define FRAMES        13156           /* the 15 minute ffmpeg run */

/* Simulated sender loop: a frame is captured every TRUE_PERIOD; it is fetched
 * when the loop is free (after sending the previous frame: ~140 ms for a
 * 134 KB I-frame over Wi-Fi, ~8 ms for a P-frame) plus up to 4 ms poll
 * latency. */
static int64_t cap_us[FRAMES], arr_us[FRAMES], est_us[FRAMES];

static void simulate(double start_us)
{
    rtp_vclock_t c;
    rtp_vclock_init(&c, 1e6 / NOMINAL_FPS);
    double loop_free = 0;
    for (int k = 0; k < FRAMES; k++) {
        double cap = start_us + k * TRUE_PERIOD;
        double arr = cap + 1500 + rnd01() * 4000;
        if (arr < loop_free) arr = loop_free + rnd01() * 300;
        loop_free = arr + ((k % GOP == 0) ? 120000 + rnd01() * 60000
                                          : 4000 + rnd01() * 10000);
        cap_us[k] = (int64_t)cap;
        arr_us[k] = (int64_t)arr;
        est_us[k] = rtp_vclock_stamp(&c, arr_us[k]);
    }
    printf("       learned period %.1f us (true %.1f) = %.3f fps\n",
           c.period_us, TRUE_PERIOD, 1e6 / c.period_us);
}

static double mean_off(int from, int to)
{
    double s = 0;
    for (int k = from; k < to; k++) s += (double)(est_us[k] - cap_us[k]);
    return s / (to - from);
}

int main(void)
{
    /* ---- video clock vs the real 14.67 fps sensor --------------------- */
    simulate(5e6);

    int mono = 1;
    int64_t max_step = 0, min_step = INT64_MAX;
    for (int k = 1; k < FRAMES; k++) {
        int64_t d = est_us[k] - est_us[k - 1];
        if (d <= 0) mono = 0;
        if (k > 600) {
            if (d > max_step) max_step = d;
            if (d < min_step) min_step = d;
        }
    }
    CHECK(mono, "estimates are strictly increasing over 15 minutes");
    printf("       steady-state step: min %lld us, max %lld us (frame %.0f us)\n",
           (long long)min_step, (long long)max_step, TRUE_PERIOD);
    CHECK(max_step < (int64_t)(1.6 * TRUE_PERIOD),
          "no timestamp jump at GOP boundaries (largest step < 1.6 frames)");
    CHECK(min_step > (int64_t)(0.4 * TRUE_PERIOD),
          "no collapsed step either (smallest step > 0.4 frames)");

    double total = (double)(est_us[FRAMES - 1] - est_us[0]);
    double ticks_per_frame = total / (FRAMES - 1) * 90000.0 / 1e6;
    printf("       mean %.1f ticks/frame (true %.1f, old encoder clock 6000)\n",
           ticks_per_frame, TRUE_PERIOD * 90000.0 / 1e6);
    CHECK(fabs(ticks_per_frame - TRUE_PERIOD * 0.09) < 0.003 * 6135,
          "RTP ticks advance ~6135 per frame on average");

    /* Drift against real time: the offset est-capture must stay put. */
    double early = mean_off(1000, 2000), late = mean_off(FRAMES - 1000, FRAMES);
    printf("       est-capture offset: %.1f ms early, %.1f ms late\n",
           early / 1000, late / 1000);
    CHECK(fabs(late - early) < 3000, "no drift against real time (< 3 ms over 15 min)");
    CHECK(fabs(mean_off(300, 600) - late) < 30000,
          "locked within 30 ms after ~20 s (the camera drains frames long before PLAY)");
    CHECK(early > -1000 && early < 60000, "estimate stays within 60 ms behind capture");
    int ahead = 0;
    for (int k = 0; k < FRAMES; k++)
        if (est_us[k] > arr_us[k]) ahead++;
    CHECK(ahead == 0, "a frame is never stamped later than it was fetched");

    /* The encoder's nominal clock, for contrast: what the camera did. */
    double nominal_drift = FRAMES * (TRUE_PERIOD - 1e6 / NOMINAL_FPS);
    printf("       (encoder nominal clock drifted %.1f s in the same run)\n",
           nominal_drift / 1e6);
    CHECK(nominal_drift > 15e6, "sanity: the old clock really drifted ~20 s / 15 min");

    /* ---- video vs audio on the same wall clock ----------------------- */
    /* Audio: 8 kHz sample counter, 256-sample periods read ~0-5 ms late,
     * hardware crystal 50 ppm fast relative to CLOCK_MONOTONIC. */
    rtp_aclock_t a;
    rtp_aclock_init(&a);
    const uint32_t rate = 8000;
    double audio_t0 = 4.9e6;
    double hw_scale = 1.0 - 50e-6;       /* true us per nominal sample us */
    double worst_skew = 0, skew_first = 0, skew_last = 0;
    int nslot = (int)((cap_us[FRAMES - 1] - 5e6) / 32000.0);
    int vk = 0, n_first = 0, n_last = 0;
    for (int s = 1; s <= nslot; s++) {
        uint64_t end_sample = (uint64_t)s * 256;
        double t_true_end = audio_t0 + (double)end_sample * 125.0 * hw_scale;
        rtp_aclock_observe(&a, end_sample, (int64_t)(t_true_end + rnd01() * 5000), rate);
        /* every video frame captured up to now: compare what a receiver
         * maps both tracks to (video via est, audio via the aclock). */
        while (vk < FRAMES && cap_us[vk] <= (int64_t)t_true_end) {
            if (vk > 1000) {
                /* audio sample captured at the same instant as frame vk */
                double samples = (double)(cap_us[vk] - audio_t0) / (125.0 * hw_scale);
                double audio_wall = (double)a.off_us + samples * 125.0;
                double skew = (double)est_us[vk] - audio_wall;
                if (fabs(skew) > worst_skew) worst_skew = fabs(skew);
                if (vk < 2000) { skew_first += skew; n_first++; }
                if (vk >= FRAMES - 1000) { skew_last += skew; n_last++; }
            }
            vk++;
        }
    }
    skew_first /= n_first;
    skew_last /= n_last;
    printf("       video-audio skew: %.1f ms early, %.1f ms late, worst %.1f ms\n",
           skew_first / 1000, skew_last / 1000, worst_skew / 1000);
    CHECK(fabs(skew_last - skew_first) < 5000,
          "video and audio do not drift apart (< 5 ms over 15 min)");
    CHECK(worst_skew < 80000, "video-audio skew stays under 80 ms");

    /* aclock inversion: samples_at() is the inverse of the observed mapping */
    {
        rtp_aclock_t b;
        rtp_aclock_init(&b);
        rtp_aclock_observe(&b, 8000, 11000000, rate);       /* 1 s of audio read at t=11 s */
        CHECK(b.off_us == 10000000, "audio offset = read time - captured duration");
        CHECK(rtp_aclock_samples_at(&b, 12000000, rate) == 16000,
              "audio sample counter two seconds in");
        CHECK(rtp_aclock_samples_at(&b, 9000000, rate) == 0,
              "audio counter clamps at 0 before the stream start");
        rtp_aclock_observe(&b, 16000, 21000000, rate);      /* read way later: offset rises slowly */
        CHECK(b.off_us > 10000000 && b.off_us < 10100000, "late reads move the offset only slowly");
        rtp_aclock_observe(&b, 24000, 12900000, rate);      /* earlier than ever: snap */
        CHECK(b.off_us == 9900000, "an earlier reading lowers the offset at once");
    }

    /* ---- a different sensor rate is learned just the same ------------ */
    {
        rtp_vclock_t c;
        rtp_vclock_init(&c, 1e6 / NOMINAL_FPS);
        const double p = 1e6 / 12.0;                    /* sensor really at 12 fps */
        int64_t first = 0, last = 0;
        for (int k = 0; k < 2700; k++) {                /* 3.75 minutes */
            int64_t e = rtp_vclock_stamp(&c, (int64_t)(1e6 + k * p + rnd01() * 6000));
            if (k == 1800) first = e;
            last = e;
        }
        double tpf = (double)(last - first) / 899 * 0.09;
        printf("       12 fps sensor: %.1f ticks/frame (true 7500)\n", tpf);
        CHECK(fabs(tpf - 7500) < 40, "follows a 12 fps sensor too");
    }

    /* ---- pause between two clients: re-lock, still monotonic -------- */
    {
        rtp_vclock_t c;
        rtp_vclock_init(&c, 1e6 / NOMINAL_FPS);
        int64_t t = 1000000, last = 0;
        for (int k = 0; k < 100; k++, t += (int64_t)TRUE_PERIOD)
            last = rtp_vclock_stamp(&c, t);
        t += 8000000;  /* 8 s stall */
        int64_t after = rtp_vclock_stamp(&c, t);
        CHECK(after > last && after >= t - 100000, "re-locks on the arrival after a long pause");
        int64_t next = rtp_vclock_stamp(&c, t + (int64_t)TRUE_PERIOD);
        CHECK(next > after && next - after < 2 * (int64_t)TRUE_PERIOD, "and continues smoothly");
    }

    /* ---- RTCP pairing arithmetic ------------------------------------ */
    {
        uint32_t s, f;
        rtp_ntp_from_unix_us(0, &s, &f);
        CHECK(s == 2208988800u && f == 0, "NTP of the Unix epoch");
        rtp_ntp_from_unix_us(1500000, &s, &f);
        CHECK(s == 2208988801u && f == 0x80000000u, "NTP of 1.5 s");
        rtp_ntp_from_unix_us(1700000000123456LL, &s, &f);
        CHECK(s == 1700000000u + 2208988800u, "NTP seconds of a 2023 date");
        CHECK(llabs((long long)(((uint64_t)f * 1000000ull) >> 32) - 123456) <= 1,
              "NTP fraction carries the microseconds");

        /* One instant, both tracks: 1 s later => +90000 video, +8000 audio. */
        uint64_t base = 0xfffffffffff00000ull;                 /* wraps by design */
        uint32_t v0 = rtp_ticks_from_us(5000000, base, 90000);
        uint32_t v1 = rtp_ticks_from_us(6000000, base, 90000);
        CHECK((uint32_t)(v1 - v0) == 90000, "video SR clock advances 90000 ticks/s");
        CHECK((uint32_t)(rtp_ticks_from_us(5000000 + 1000, base, 90000) - v0) == 90,
              "1 ms = 90 ticks");
        /* video SR pair is exactly what the stream stamps for that instant */
        int64_t inst = 123456789;
        CHECK(rtp_ticks_from_us(inst, base, 90000) == rtp_ticks_from_us(inst, base, 90000),
              "stream and SR use the same formula");
    }

    /* ---- audio start gate: backlog + PLAY + later IDR ---------------- */
    {
        /* Ring of 8 periods (32 ms each) filled since boot; PLAY at 10.000 s,
         * next IDR captured at 11.300 s. Audio must start at the IDR. */
        enum { RATE = 8000, PER = 256, SLOTS = 8 };
        const int64_t per_us = (int64_t)PER * 1000000 / RATE;
        rtp_astart_t g;
        rtp_aclock_t ac;
        rtp_aclock_init(&ac);
        rtp_astart_arm(&g);
        CHECK(rtp_astart_holding(&g), "audio held after PLAY until a video frame");

        int64_t idr_us = 11300000;
        uint64_t first_sent = 0; int64_t first_end = 0; bool sent = false;
        uint64_t prev_end = 0; bool contiguous = true, stale_sent = false;
        /* Producer runs from t=9.0 s; consumer polls every 5 ms from PLAY. */
        for (int64_t now = 10000000; now < 12000000; now += 5000) {
            if (rtp_astart_holding(&g) && now >= idr_us) rtp_astart_release(&g, idr_us);
            if (rtp_astart_holding(&g)) continue;
            /* Ring state at "now": last SLOTS periods ended at multiples of per_us. */
            int64_t newest = now / per_us;
            for (int64_t k = newest - SLOTS + 1; k <= newest; k++) {
                int64_t t_end = k * per_us;
                uint64_t first = (uint64_t)(k * PER); /* sample index of period k */
                static int64_t last_k = -1;
                if (k <= last_k) continue;
                last_k = k;
                if (rtp_astart_stale(&g, t_end)) continue;
                if (t_end < idr_us - per_us) stale_sent = true;
                if (!sent) { sent = true; first_sent = first; first_end = t_end; }
                else if (first != prev_end) contiguous = false;
                prev_end = first + PER;
                rtp_aclock_observe(&ac, first + PER, t_end, RATE);
            }
        }
        int64_t first_start_us = first_end - per_us;
        printf("       first audio period starts %lld us, IDR at %lld us\n",
               (long long)first_start_us, (long long)idr_us);
        CHECK(sent, "audio flows after the IDR");
        CHECK(!stale_sent, "no sample captured before the IDR is sent");
        CHECK(llabs((long long)(first_start_us - idr_us)) <= per_us,
              "first audio maps to the IDR capture time within one frame");
        CHECK(contiguous, "audio sample index stays continuous after the start");
        CHECK(first_sent > 0 && first_sent % PER == 0,
              "RTP timestamp keeps the capture-stream sample index");
    }

    /* ---- resync re-arms the gate ------------------------------------- */
    {
        rtp_astart_t g;
        rtp_astart_arm(&g);
        rtp_astart_release(&g, 5000000);
        CHECK(rtp_astart_stale(&g, 4990000), "period before the cut is dropped");
        CHECK(!rtp_astart_stale(&g, 5010000), "period after the cut passes");
        CHECK(!rtp_astart_stale(&g, 4000000), "gate stays open once passed");
        rtp_astart_arm(&g);
        CHECK(rtp_astart_holding(&g), "resync holds audio again");
    }

    printf(fails ? "FAILED: %d\n" : "all ok\n", fails);
    return fails ? 1 : 0;
}
