/* Host test of the latency bounds of the RTSP video path (Rotatable 2K own app; the pacing part is shared with the 1080p):
 * stream.c (ring, join at a keyframe, catch-up), tx_pace.c (backlog, latency bound) and lat_stat.h (frame age
 * statistics), composed the way rtsp_session.c composes them, in virtual time. The encoder is a fake (frames at a fixed rate,
 * a keyframe every GOP or on request), the network a reader that drains the socket at a chosen byte rate. No sockets,
 * no sleeps, no hardware. Build/run: tests/rot2k_latency_test.sh */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lat_stat.h"
#include "stream.h"
#include "tx_pace.h"

static int fails;
#define CHECK(c, ...) do { if (c) { printf("  ok   - "); printf(__VA_ARGS__); printf("\n"); } \
    else { printf("  FAIL - "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

#define FPS 10
#define GOP 30
#define KBPS 3000
#define KEY_LEN (150u << 10)
#define P_LEN (26u << 10) /* (150 KiB + 29 x 26 KiB) per 3 s = about 2.6 Mbit/s: the stream is at its nominal rate */
#define FRAME_US (1000000 / FPS)

static a2k_stream_t g_s;

typedef struct {
    int use_lag;          /* the latency bound of tx_pace on (the 1080p and 2K default); off = the byte marks only */
    int64_t now_us;
    double backlog;       /* bytes the peer has not taken yet (kernel + private queue) */
    double rate;          /* what the peer takes, bytes per second */
    uint64_t cursor;
    int need_idr;
    tx_pace_t pace;
    unsigned long idx;    /* frames produced */
    unsigned long sent, skipped, catchups;
    double lat_max_ms;    /* worst time from the encoder to the last byte reaching the peer, over the measured window */
    int64_t measure_from_us;
    int64_t last_idr_us;
} sim_t;

static uint8_t *au_buf;

static void fill_au(size_t len, int key)
{
    memset(au_buf, 0, len);
    au_buf[3] = 1;
    au_buf[4] = key ? 0x65 : 0x41;
}

static void publish_at(size_t len, int key, int64_t rx_us)
{
    fill_au(len, key);
    a2k_stream_publish(&g_s, au_buf, len);
    g_s.ring[(g_s.head - 1) % A2K_RING]->rx_us = rx_us; /* virtual capture time */
}

/* The fake encoder: one access unit per frame interval; a keyframe every GOP, or at once when a client asked (merged to
 * one request per 300 ms like cap.c). */
static void encode_tick(sim_t *m)
{
    int key = (m->idx % GOP) == 0;
    if (a2k_stream_take_idr(&g_s) && m->now_us - m->last_idr_us >= 300000) {
        key = 1;
        m->last_idr_us = m->now_us;
    }
    publish_at(key ? KEY_LEN : P_LEN, key, m->now_us);
    m->idx++;
}

static void drain(sim_t *m, int64_t dt_us)
{
    m->backlog -= m->rate * (double)dt_us / 1e6;
    if (m->backlog < 0) m->backlog = 0;
}

/* The client loop body of rtsp_session.c (pump_video + send_video_frame) with the socket replaced by the byte counter. */
static void client_pump(sim_t *m)
{
    int n;
    tx_pace_cfg_t cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.base = tx_pace_base(KBPS);
    if (m->use_lag) {
        cfg.max_lag_ms = TX_PACE_MAX_LAG_MS;
        cfg.rate_bps = (size_t)KBPS * 125u;
    }
    for (n = 0; n < A2K_RING + 1; n++) {
        unsigned dropped = 0;
        int lost = 0;
        int cu = a2k_stream_catchup(&g_s, &m->cursor, m->now_us, (int64_t)A2K_MAX_AGE_MS * 1000, &dropped);
        a2k_frame_t *f;
        tx_pace_event_t ev;
        int send;
        if (cu) {
            m->need_idr = 1;
            m->catchups++;
            if (cu == 2) a2k_stream_want_idr(&g_s);
        }
        f = a2k_stream_next(&g_s, &m->cursor, &lost);
        if (lost && !m->need_idr) {
            m->need_idr = 1;
            a2k_stream_want_idr(&g_s);
        }
        if (!f) return;
        send = tx_pace_decide(&m->pace, &cfg, (size_t)m->backlog, f->len, SIZE_MAX, f->key, m->now_us / 1000, &ev) ==
               TX_PACE_SEND;
        if (send && m->need_idr) {
            if (f->key) m->need_idr = 0;
            else send = 0;
        }
        if (send) {
            double lat_ms;
            m->backlog += (double)tx_pace_wire_len(f->len);
            m->sent++;
            lat_ms = (double)(m->now_us - f->rx_us) / 1000.0 + (m->rate > 0 ? m->backlog / m->rate * 1000.0 : 1e9);
            if (m->now_us >= m->measure_from_us && lat_ms > m->lat_max_ms)
                m->lat_max_ms = lat_ms;
        } else {
            m->skipped++;
        }
        a2k_frame_unref(&g_s, f);
    }
}

static void sim_init(sim_t *m, int use_lag, double rate)
{
    memset(m, 0, sizeof *m);
    m->use_lag = use_lag;
    m->rate = rate;
    m->now_us = 1000000;
    tx_pace_init(&m->pace);
    a2k_stream_destroy(&g_s);
    a2k_stream_init(&g_s, "stream0", 2304, 1296, FPS, GOP, KBPS, A2K_MAX_FRAME_MAIN);
    m->need_idr = 1;
    if (!a2k_stream_join(&g_s, &m->cursor, m->now_us, (int64_t)A2K_MAX_AGE_MS * 1000))
        a2k_stream_want_idr(&g_s);
}

/* Runs `seconds` of stream. The reader takes m->rate bytes/s except in [stall_from, stall_to) where it takes nothing.
 * The client thread does not run in [freeze_from, freeze_to). */
static void run(sim_t *m, int seconds, int64_t stall_from, int64_t stall_to, int64_t freeze_from, int64_t freeze_to)
{
    double base_rate = m->rate;
    int i, total = seconds * FPS;
    for (i = 0; i < total; i++) {
        m->now_us += FRAME_US;
        m->rate = (m->now_us >= stall_from && m->now_us < stall_to) ? 0.0 : base_rate;
        drain(m, FRAME_US);
        encode_tick(m);
        if (m->now_us >= freeze_from && m->now_us < freeze_to) continue;
        client_pump(m);
    }
    m->rate = base_rate;
}

/* Six frames in the ring, keyframes at seq 0 and 3 (GOP of 3 for these parts), frame k captured at T + k frames. */
static void fill_ring(int64_t T)
{
    static const int kinds[] = { 1, 0, 0, 1, 0, 0 };
    int k;
    a2k_stream_destroy(&g_s);
    a2k_stream_init(&g_s, "stream0", 2304, 1296, FPS, 3, KBPS, A2K_MAX_FRAME_MAIN);
    for (k = 0; k < 6; k++)
        publish_at(P_LEN, kinds[k], T + (int64_t)k * FRAME_US);
}

int main(void)
{
    sim_t m;
    const double nominal = (double)KBPS * 125.0; /* bytes/s */
    uint64_t cur = 0;
    const int64_t T = 1000000;
    int k;

    au_buf = malloc(A2K_MAX_FRAME_MAIN);
    memset(&g_s, 0, sizeof g_s);
    pthread_mutex_init(&g_s.mu, NULL);

    printf("-- join: the newest recent keyframe, never the oldest frame\n");
    fill_ring(T);
    CHECK(a2k_stream_join(&g_s, &cur, T + 5 * FRAME_US, 600000) == 1 && cur == 3,
          "joining with two keyframes in the ring starts at the newest one (seq 3), not at the oldest frame (seq 0)");
    CHECK(a2k_stream_join(&g_s, &cur, T + 3 * FRAME_US + 301000, 300000) == 0 && cur == a2k_stream_head(&g_s),
          "a newest keyframe older than the age bound is not used: start at the head and ask for a fresh keyframe");
    a2k_stream_destroy(&g_s);
    a2k_stream_init(&g_s, "stream0", 2304, 1296, FPS, GOP, KBPS, A2K_MAX_FRAME_MAIN);
    for (k = 0; k < 4; k++)
        publish_at(P_LEN, 0, T);
    CHECK(a2k_stream_join(&g_s, &cur, T, 300000) == 0 && cur == 4, "no keyframe in the ring: start at the head");

    printf("-- catch-up: a client behind by more than the age bound skips ahead\n");
    {
        unsigned dropped = 9;
        fill_ring(T);
        cur = 0;
        CHECK(a2k_stream_catchup(&g_s, &cur, T + 200000, 300000, &dropped) == 0 && cur == 0 && dropped == 0,
              "a client whose next frame is younger than the bound is left alone");
        CHECK(a2k_stream_catchup(&g_s, &cur, T + 5 * FRAME_US + 100000, 400000, &dropped) == 1 && cur == 3 &&
                  dropped == 3,
              "a client behind jumps to the newest recent keyframe (3 frames skipped)");
        cur = 1;
        CHECK(a2k_stream_catchup(&g_s, &cur, T + 5 * FRAME_US + 380000, 400000, &dropped) == 2 && cur == 6 &&
                  dropped == 5,
              "no recent keyframe: everything is dropped and the client waits for a new one");
    }

    printf("-- healthy link (2x the stream rate): nothing is dropped, queue latency stays small\n");
    sim_init(&m, 1, nominal * 2);
    m.measure_from_us = m.now_us + 5000000;
    run(&m, 120, 0, 0, 0, 0);
    printf("       sent %lu skipped %lu worst latency %.0f ms\n", m.sent, m.skipped, m.lat_max_ms);
    CHECK(m.skipped < 5, "at most the frames before the first keyframe are skipped (%lu)", m.skipped);
    CHECK(m.lat_max_ms < 450.0, "worst encoder-to-peer latency %.0f ms < 450 ms (a keyframe at 2x rate takes ~250 ms)",
          m.lat_max_ms);

    printf("-- pacing adds no steady delay: a reader just above the stream rate never queues\n");
    sim_init(&m, 1, nominal * 1.15);
    m.measure_from_us = m.now_us + 5000000;
    run(&m, 300, 0, 0, 0, 0);
    printf("       sent %lu skipped %lu worst latency %.0f ms\n", m.sent, m.skipped, m.lat_max_ms);
    CHECK(m.skipped < 5, "no frame is dropped on a link that carries the stream (%lu)", m.skipped);
    CHECK(m.lat_max_ms < 800.0, "latency is the keyframe's own serialisation only: %.0f ms (no standing queue)",
          m.lat_max_ms);

    printf("-- a link that carries 60%% of the stream: latency is bounded, it does not grow with time\n");
    {
        sim_t a, b;
        double early;
        sim_init(&a, 0, nominal * 0.6); /* the byte marks only (the old policy) */
        a.measure_from_us = a.now_us + 20000000;
        run(&a, 120, 0, 0, 0, 0);
        sim_init(&b, 1, nominal * 0.6);
        b.measure_from_us = b.now_us + 20000000;
        run(&b, 120, 0, 0, 0, 0);
        printf("       byte marks only: worst %.0f ms, with the latency bound: worst %.0f ms (skipped %lu / %lu)\n",
               a.lat_max_ms, b.lat_max_ms, a.skipped, b.skipped);
        CHECK(b.lat_max_ms < 1500.0, "with the latency bound the worst latency is %.0f ms < 1500 ms", b.lat_max_ms);
        CHECK(b.lat_max_ms < a.lat_max_ms, "and it is lower than with the byte marks alone (%.0f ms)", a.lat_max_ms);
        sim_init(&b, 1, nominal * 0.6);
        b.measure_from_us = b.now_us + 5000000;
        run(&b, 30, 0, 0, 0, 0);
        early = b.lat_max_ms;
        sim_init(&b, 1, nominal * 0.6);
        b.measure_from_us = b.now_us + 60000000;
        run(&b, 600, 0, 0, 0, 0);
        CHECK(b.lat_max_ms < early * 1.25 + 100.0, "ten minutes later it is no worse than after 30 s (%.0f ms vs %.0f ms)",
              b.lat_max_ms, early);
    }

    printf("-- the reader stops for 2 s then resumes: the delay does not stay\n");
    sim_init(&m, 1, nominal * 2);
    m.measure_from_us = m.now_us + 8000000; /* 4 s after the stall ended */
    run(&m, 60, T + 4000000, T + 6000000, 0, 0);
    printf("       worst latency from 4 s after the stall %.0f ms (skipped %lu)\n", m.lat_max_ms, m.skipped);
    CHECK(m.lat_max_ms < 450.0, "4 s after the stall the latency is back to %.0f ms", m.lat_max_ms);

    printf("-- the client thread is starved for 1.5 s on a fast link: it jumps ahead instead of replaying\n");
    sim_init(&m, 1, nominal * 4);
    run(&m, 3, 0, 0, 0, 0);
    {
        int64_t t0 = m.now_us;
        unsigned long cu0 = m.catchups;
        m.measure_from_us = t0 + 1500000 + FRAME_US;
        run(&m, 10, 0, 0, t0, t0 + 1500000);
        printf("       catch-ups %lu, worst latency after the freeze %.0f ms\n", m.catchups - cu0, m.lat_max_ms);
        CHECK(m.catchups > cu0, "the starved client caught up (%lu times)", m.catchups - cu0);
        CHECK(m.lat_max_ms < 450.0, "after the freeze no frame is sent late: worst %.0f ms", m.lat_max_ms);
    }

    printf("-- the age statistics\n");
    {
        lat_stat_t l;
        unsigned long n = 0;
        long avg = 0, mx = 0;
        size_t bm = 0;
        lat_stat_init(&l);
        lat_stat_add(&l, 100000, 1000, 1000);
        lat_stat_add(&l, 300000, 5000, 2000);
        lat_stat_add(&l, -5, 10, 3000);
        CHECK(!lat_stat_take(&l, 5000, &n, &avg, &mx, &bm), "no line before the period is over");
        CHECK(lat_stat_take(&l, 1000 + LAT_STAT_PERIOD_MS, &n, &avg, &mx, &bm) && n == 3 && avg == 133 && mx == 300 &&
                  bm == 5000,
              "avg %ld ms max %ld ms over %lu frames, backlog max %zu", avg, mx, n, bm);
        CHECK(!lat_stat_take(&l, 1000 + LAT_STAT_PERIOD_MS + 1, &n, &avg, &mx, &bm), "the window restarts empty");
    }

    printf("-- constants\n");
    CHECK(TX_PACE_SNDBUF <= 64 * 1024, "SO_SNDBUF request is at most 64 KiB (%d)", TX_PACE_SNDBUF);
    CHECK(A2K_MAX_AGE_MS <= 300, "age bound %d ms", A2K_MAX_AGE_MS);

    a2k_stream_destroy(&g_s);
    free(au_buf);
    if (fails) {
        printf("%d check(s) FAILED\n", fails);
        return 1;
    }
    printf("all checks passed\n");
    return 0;
}
