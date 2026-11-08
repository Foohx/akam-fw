#include "rtp_clock.h"

#include <time.h>

int64_t rtp_mono_us(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

int64_t rtp_realtime_us(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) return 0;
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

/* Loop gains, per frame. ALPHA pulls the phase toward the arrivals (time
 * constant ~2 s at 15 fps); BETA ~ ALPHA^2 / (2 - ALPHA) is the critically
 * damped rate gain of an alpha-beta tracker, so the period converges without
 * ringing in ~10 s. */
#define VCLOCK_ALPHA      (1.0 / 64.0)
#define VCLOCK_BETA       (1.0 / 8192.0)
/* The correction a single frame may apply to the period is limited, so one
 * long stall cannot be mistaken for a change of the sensor rate. */
#define VCLOCK_LATE_WEIGHT 0.2
#define VCLOCK_WARM_FRAMES 900    /* ~1 min at 15 fps */
#define VCLOCK_WARM_BOOST  15.0   /* extra period gain at frame 0 */
#define VCLOCK_ERR_CLAMP  0.5    /* fraction of a period */
#define VCLOCK_MIN_PERIOD 0.5     /* learned period bounds, x nominal */
#define VCLOCK_MAX_PERIOD 1.5
/* Past this much lateness the model is not worth following: re-lock on the
 * arrival (a real pause, e.g. between two clients). */
#define VCLOCK_RESYNC_US  1000000.0
/* Estimates always advance by at least this share of a period. */
#define VCLOCK_MIN_STEP   0.25

void rtp_vclock_init(rtp_vclock_t *c, double nominal_period_us)
{
    c->init = false;
    c->nominal_us = nominal_period_us;
    c->period_us = nominal_period_us;
    c->est_us = 0;
    c->err_us = 0;
    c->frames = 0;
}

int64_t rtp_vclock_stamp(rtp_vclock_t *c, int64_t arrival_us)
{
    double arr = (double)arrival_us;
    if (!c->init) {
        c->init = true;
        c->est_us = arr;
        return arrival_us;
    }

    /* The very first frames find the period at its nominal value, off by
     * the 2% this exists to correct: learn faster until warm. */
    double boost = 1.0;
    if (c->frames < VCLOCK_WARM_FRAMES)
        boost += VCLOCK_WARM_BOOST * (1.0 - (double)c->frames / VCLOCK_WARM_FRAMES);
    c->frames++;

    double pred = c->est_us + c->period_us;
    double e = arr - pred;
    c->err_us = e;

    double next;
    if (e > VCLOCK_RESYNC_US) {
        next = arr;
    } else {
        double lim = c->period_us * VCLOCK_ERR_CLAMP;
        double ec = e > lim ? lim : (e < -lim ? -lim : e);
        /* Late arrivals are mostly queueing noise, so they count for less
         * than early ones when learning the period (a low-quantile fit of
         * the arrivals; equal weights would learn a period that is too long
         * and have the ceiling below do the tracking). */
        c->period_us += ec * VCLOCK_BETA * boost *
                        (ec > 0 ? VCLOCK_LATE_WEIGHT : 1.0);
        if (c->period_us < c->nominal_us * VCLOCK_MIN_PERIOD)
            c->period_us = c->nominal_us * VCLOCK_MIN_PERIOD;
        if (c->period_us > c->nominal_us * VCLOCK_MAX_PERIOD)
            c->period_us = c->nominal_us * VCLOCK_MAX_PERIOD;

        /* Lateness (e > 0) is queueing delay: follow it slowly. Earliness
         * (e < 0) is impossible, so the arrival is a hard ceiling. */
        next = e < 0 ? arr : pred + e * VCLOCK_ALPHA;
    }

    double floor_est = c->est_us + c->period_us * VCLOCK_MIN_STEP;
    if (next < floor_est) next = floor_est;
    c->est_us = next;
    return (int64_t)next;
}

uint32_t rtp_ticks_from_us(int64_t t_us, uint64_t base_us, uint32_t clock_rate)
{
    uint64_t t = (uint64_t)t_us + base_us;
    uint64_t sec = t / 1000000, rem = t % 1000000;
    return (uint32_t)(sec * clock_rate + rem * clock_rate / 1000000);
}

void rtp_aclock_init(rtp_aclock_t *a)
{
    a->init = false;
    a->off_us = 0;
}

void rtp_aclock_observe(rtp_aclock_t *a, uint64_t end_sample, int64_t t_end_us,
                        uint32_t rate)
{
    int64_t cap_us = (int64_t)(end_sample * 1000000ull / rate);
    int64_t off = t_end_us - cap_us;
    if (!a->init) {
        a->init = true;
        a->off_us = off;
    } else if (off < a->off_us) {
        a->off_us = off;
    } else {
        a->off_us += (off - a->off_us) / 256;
    }
}

uint64_t rtp_aclock_samples_at(const rtp_aclock_t *a, int64_t now_us,
                               uint32_t rate)
{
    int64_t d = now_us - a->off_us;
    if (d <= 0) return 0;
    return (uint64_t)d * rate / 1000000ull;
}

void rtp_ntp_from_unix_us(int64_t unix_us, uint32_t *ntp_sec, uint32_t *ntp_frac)
{
    int64_t sec = unix_us / 1000000;
    int64_t us = unix_us % 1000000;
    if (us < 0) { us += 1000000; sec--; }
    /* Unix epoch (1970) -> NTP epoch (1900): 70 years. */
    *ntp_sec = (uint32_t)((uint64_t)sec + 2208988800ull);
    *ntp_frac = (uint32_t)(((uint64_t)us << 32) / 1000000ull);
}
