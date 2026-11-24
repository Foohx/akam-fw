#pragma once
/* Frame-age statistics of one RTSP client, for the periodic stats line.
 *
 * The age of a frame is "now minus the time it left the encoder" measured when its last packet has been handed to
 * the socket (or the private send queue). It is the camera's own share of the glass-to-glass latency: ring wait, pacing
 * and writing, but not the network, the player's buffer or its decoder. Together with the backlog (bytes the peer
 * still has to take) a hardware run can tell the camera's part from the rest without a stopwatch.
 *
 * Header only, no I/O, no clock: the caller passes the times. Host test: tests/rot2k_latency_test.sh. */
#include <stddef.h>
#include <stdint.h>

#define LAT_STAT_PERIOD_MS 10000 /* one stats line per client every this long while frames flow */

typedef struct {
    unsigned long n;       /* frames measured in the window */
    int64_t sum_us, max_us;
    size_t backlog_max;    /* largest unsent byte count seen at a frame start */
    int64_t window_ms;     /* start of the window, 0 = not started */
} lat_stat_t;

static inline void lat_stat_init(lat_stat_t *l)
{
    l->n = 0;
    l->sum_us = l->max_us = 0;
    l->backlog_max = 0;
    l->window_ms = 0;
}

static inline void lat_stat_add(lat_stat_t *l, int64_t age_us, size_t backlog, int64_t now_ms)
{
    if (age_us < 0) age_us = 0;
    if (l->window_ms == 0) l->window_ms = now_ms ? now_ms : 1;
    l->n++;
    l->sum_us += age_us;
    if (age_us > l->max_us) l->max_us = age_us;
    if (backlog > l->backlog_max) l->backlog_max = backlog;
}

/* 1 and the window's figures (ms, rounded down) when LAT_STAT_PERIOD_MS has passed and frames were measured; the window
 * restarts. 0 otherwise. */
static inline int lat_stat_take(lat_stat_t *l, int64_t now_ms, unsigned long *n, long *avg_ms, long *max_ms,
                                size_t *backlog_max)
{
    if (l->n == 0 || l->window_ms == 0 || now_ms - l->window_ms < LAT_STAT_PERIOD_MS) return 0;
    *n = l->n;
    *avg_ms = (long)(l->sum_us / (int64_t)l->n / 1000);
    *max_ms = (long)(l->max_us / 1000);
    *backlog_max = l->backlog_max;
    lat_stat_init(l);
    l->window_ms = now_ms ? now_ms : 1;
    return 1;
}
