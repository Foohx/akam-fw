#pragma once
/* RTP media clocks: video and audio timestamps on ONE wall-clock basis.
 *
 * Why this exists: the Anyka encoder stamps frames from a NOMINAL clock
 * (frame index / ENC_FPS, in ms), not from the time the sensor delivered the
 * frame. Measured on hardware: the sensor really runs at 14.67 fps while the
 * encoder timestamps advance as if it were 15.00 fps, so the video RTP clock
 * fell 2.2% behind real time (and behind the audio clock, which is a true
 * sample counter). A receiver that syncs audio and video (VLC/live555) then
 * saw every frame arrive later and later than its timestamp and discarded
 * them as "too late".
 *
 * Both clocks below work in microseconds on CLOCK_MONOTONIC (through the
 * time32 shim on the camera), so an RTCP Sender Report can pair the SAME
 * instant with the RTP timestamp of each track. No dependency on SmolRTSP or
 * on the camera: host-testable (tests/rtp_clock_test.sh). */
#include <stdbool.h>
#include <stdint.h>

/* CLOCK_MONOTONIC in microseconds (0 if the clock cannot be read). */
int64_t rtp_mono_us(void);

/* CLOCK_REALTIME in microseconds since the Unix epoch (0 on failure). */
int64_t rtp_realtime_us(void);

/* ---- Video: arrival times -> smooth capture-time estimate --------------
 *
 * Frames come out of the encoder at a constant, sensor-driven rate, but we
 * only see the time we fetched each one, which is late by a variable queueing
 * delay (a blocking write of a 130 KB I-frame delays the next fetch by
 * 100 ms; a stall delays a whole burst). Using the raw fetch time would put
 * that jitter into the timestamps, visible as a stutter every GOP. So the
 * estimate is a small PLL: it advances one measured frame period per frame
 * (the period is itself learned from the arrivals, so the real sensor rate is
 * found on its own), is pulled slowly toward the arrivals, and is never
 * allowed ahead of an arrival (a frame cannot be captured in the future).
 * Estimates are strictly increasing. */
typedef struct {
    bool   init;
    double nominal_us; /* expected frame period, bounds the learned one */
    double period_us;  /* learned frame period */
    double est_us;     /* capture-time estimate of the last frame */
    double err_us;     /* last (arrival - prediction), for diagnostics */
    unsigned frames;   /* frames seen, for the faster start-up gains */
} rtp_vclock_t;

void rtp_vclock_init(rtp_vclock_t *c, double nominal_period_us);

/* Returns the capture-time estimate (monotonic us) of a frame fetched at
 * arrival_us. Call once per frame, in order. */
int64_t rtp_vclock_stamp(rtp_vclock_t *c, int64_t arrival_us);

/* 90 kHz RTP ticks of a microsecond instant, for a stream whose random
 * origin is base_us (same arithmetic as SmolRTSP's SysClockUs timestamps, so
 * the SR can use either). Wraps modulo 2^32 by design. */
uint32_t rtp_ticks_from_us(int64_t t_us, uint64_t base_us, uint32_t clock_rate);

/* ---- Audio: sample counter -> monotonic time ---------------------------
 *
 * Audio RTP timestamps stay an exact sample count (no gaps or overlaps in the
 * L16 stream). Only the RTCP mapping needs wall time: each captured period
 * says "my last sample was read at t_end", which is the true end of capture
 * plus scheduling latency (never earlier), so the offset is tracked as a
 * lower envelope that rises slowly (follows crystal skew). */
typedef struct {
    bool    init;
    int64_t off_us; /* t_mono - end_sample * 1e6 / rate */
} rtp_aclock_t;

void rtp_aclock_init(rtp_aclock_t *a);
void rtp_aclock_observe(rtp_aclock_t *a, uint64_t end_sample, int64_t t_end_us,
                        uint32_t rate);
/* Sample counter corresponding to monotonic instant now_us. */
uint64_t rtp_aclock_samples_at(const rtp_aclock_t *a, int64_t now_us,
                               uint32_t rate);

/* ---- NTP/RTP pairing ---------------------------------------------------- */
/* RTCP (RFC 3550 section 4) wants an NTP timestamp for the instant the RTP
 * timestamp refers to. Splits Unix microseconds into NTP seconds/fraction. */
void rtp_ntp_from_unix_us(int64_t unix_us, uint32_t *ntp_sec, uint32_t *ntp_frac);
