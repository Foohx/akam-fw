/* Day / night of the Rotatable 2K own app (RE of yy_cam, see stock-firmware-reference.md "Day/night and image").
 *
 * MEASUREMENT: the stock does not measure anything itself on this camera: it registers its thresholds with the vendor lib
 * (ak_vpss_set_soft_ps_param, a2k_isp_dn_init) and reads the lib's own decision once per second (ak_vpss_get_soft_ps_level,
 * a2k_isp_dn_level): "given that the ISP runs in day / night, should it run in day / night", with the lib's hysteresis, its
 * frame counts and its non-infrared check (meant to keep the IR light of the camera itself out: it does not, see below). We do the same.
 * HARDWARE (2026-11-15, dark room): 4.5 min into night (IR LED on, filter out) the verdict said "day" and a 5 s confirmation
 * switched back: the vendor verdict is only trusted for "day" with the IR LED OFF.
 *
 * POLICY: the hardware independent policy of the 1080p firmware (src/common/logic/night_logic.c, compiled UNCHANGED) sits on top
 * of that decision: the confirmation count, the anti-flap lock, the minimum night dwell, the dwell doubling after a flap and,
 * with ir_active = 1 (IR LED in night mode), its IR-off probe: periodic (and early when the verdict turns to day), the IR
 * LED is switched off for about 10 s and the vendor verdict is read again; only a verdict of "day" for the last
 * A2K_NIGHT_PROBE_NEED readings of that dark second half lets the policy go back to day (see tick_sample in akam2k_night.c).
 * It was written for the 1080p AE numbers (exposure, gain): the adapter feeds it the vendor decision as a saturated sample
 * ("dark": the exposure at its ceiling, the gain at its maximum) or an idle one ("bright"). With ir_led = off there is no
 * IR light of our own: ir_active = 0, the verdict is read directly and must hold 15 s.
 * Phase B (PLAN 8ter) would feed it real AE numbers from ak_vpss_get_ae_run_info once their layout is read.
 *
 * SWITCH (as the stock yy_dn_day_to_night / yy_dn_night_to_day):
 *   night: IR LED on (ir_led auto), 100 ms, ISP night mode (ak_vi_switch_mode 1), IR-cut pulse to "filter out"
 *   day:   IR-cut pulse to "filter in", IR LED off, 100 ms, ISP day mode (ak_vi_switch_mode 0)
 * and a keyframe request on both streams after the ISP mode change, then (stock yy_media_av130_isp_set_running_mode) 100 ms later the WDR
 * is applied again (the mode switch resets it): ak_vpss_effect_set(0, 5, 40), under the same vendor lock as the switch; a failure is logged once.
 *
 * SAFETY: all of it is RAM state (pins, ISP), a reboot restores it. At start the filter is moved to DAY (its state after a
 * crash is unknown, the stock does the same at init); at stop the filter is left in the DAY position, the coil released and
 * the IR LED off. At least 60 s between two switches (the policy lock, plus a hard guard here). After 3 failed switches in a
 * row the automatic mode switches itself off (day position, logged).
 *
 * Config (akam2k.conf, passed by the supervisor): night = auto | day | night (default auto), ir_led = auto | off (default
 * auto: the IR LED lights in night mode; off: never), wdr = 1 | 0 (default 1: re-apply WDR after every ISP mode switch). */
#ifndef AKAM2K_NIGHT_H
#define AKAM2K_NIGHT_H

#include <pthread.h>
#include <stdint.h>

#include "night_logic.h"

#define A2K_NIGHT_MIN_SWITCH_MS 60000 /* the stock lock_time ([dn] lock_time 60 s) */
#define A2K_NIGHT_READ_EVERY 10       /* polls (100 ms) between two reads of the vendor decision: 1 s, the stock cadence (start to start, reader thread) */
#define A2K_NIGHT_SETTLE_MS 100       /* between the IR LED and the ISP switch, as the stock */
#define A2K_NIGHT_MAX_FAILS 3
/* The IR-off probe (the shared policy's own mechanism, as the 1080p): the vendor verdict is only trusted for "day" with the
 * camera's IR LED off. The AE needs a moment after the light goes off: readings before the grace time are not counted; the
 * probe passes when the last A2K_PROBE_NEED readings of the settling time all say day, and fails at the first night one. */
#define A2K_NIGHT_PROBE_MS NIGHT_DEF_PROBE_MS /* the 1080p default (5 min; the policy doubles it per failed probe, up to x8 = 40 min) */
#define A2K_NIGHT_PROBE_SETTLE_MS 8000   /* IR LED off at least this long before the verdict counts */
#define A2K_NIGHT_PROBE_GRACE_MS 2000
#define A2K_NIGHT_PROBE_NEED 3
/* Without an IR LED of our own (ir_led = off) the vendor verdict is not fooled: it is read directly, held this long */
#define A2K_NIGHT_DAY_CONFIRM_NOIR 150   /* polls (100 ms): 15 s */

enum { A2K_NIGHT_AUTO = 0, A2K_NIGHT_DAY = 1, A2K_NIGHT_NIGHT = 2 };

typedef struct {
    int mode;        /* A2K_NIGHT_* */
    int ir_led_auto; /* 1 auto, 0 off */
    int wdr;         /* 1 (default, the stock [dsp] wdr_onoff) re-applies WDR after every ISP mode switch, 0 does not */
} a2k_night_cfg_t;

typedef struct {
    int      valid;              /* 0 until the start sequence ran */
    int      night;              /* 1 = night mode (filter out, ISP night) */
    int      mode;               /* A2K_NIGHT_*: the configured mode */
    int      ir_on;              /* the IR LED is lit */
    int      auto_off;           /* the automatic mode gave up after repeated failures */
    unsigned switches;           /* switches since start (the start itself is not counted) */
    int64_t  last_switch_epoch;  /* wall clock seconds of the last switch, 0 unknown */
    char     why[96];            /* the last decision, one line */
} a2k_night_info_t;

void a2k_night_defaults(a2k_night_cfg_t *c);
/* 1 when valid. */
int a2k_night_cfg_ok(const a2k_night_cfg_t *c);
/* Exact words only: "auto" "day" "night" -> A2K_NIGHT_*; "auto" "off" -> 1 / 0. -1 for anything else. */
int a2k_night_parse_mode(const char *s);
int a2k_night_parse_ir(const char *s);

/* ---- the engine: no thread, no hardware of its own (everything goes through ops), unit tested on the host ---- */

typedef struct {
    int (*init)(void);                               /* a2k_ircut_init: pins low */
    int (*dn_init)(void);                            /* a2k_isp_dn_init */
    int (*dn_level)(int cur_night, int *want_night); /* a2k_isp_dn_level */
    int (*isp_mode)(int night);                      /* a2k_isp_mode */
    int (*ircut)(int day);                           /* a2k_ircut_pulse */
    int (*led)(int on);                              /* a2k_ircut_led */
    int (*release)(void);                            /* a2k_ircut_release */
    void (*sleep_ms)(unsigned ms);
    int64_t (*epoch)(void);                          /* wall clock seconds, 0 unknown */
    void (*notify)(const a2k_night_info_t *i);       /* after every state change (may be NULL) */
    int64_t (*now_ms)(void);                         /* monotonic ms, stamps a reading of the reader thread (may be NULL) */
    int (*wdr)(int on);                              /* a2k_isp_wdr: replayed after a successful isp_mode (may be NULL) */
} a2k_night_ops_t;

typedef struct {
    a2k_night_cfg_t cfg;
    const a2k_night_ops_t *ops;
    night_tuning_t tun;
    nl_state_t st;
    nl_limits_t lim;
    nl_sample_t held;
    int held_valid, read_due, night, fails;
    int probe_streak, probe_night_seen; /* the vendor verdicts read during an IR-off probe (see akam2k_night.c) */
    int64_t last_switch_ms;
    a2k_night_info_t info;
    /* Asynchronous reader (see a2k_night_ctl_set_async): the vendor read blocks 1..1.5 s, so it must not sit inside the 100 ms
     * tick or every policy duration is stretched by the same factor. The reader publishes "latest verdict + sequence number +
     * timestamp" here; the tick consumes each one at most once. gen is bumped on every ISP / IR LED change: a reading that
     * began under an older gen described another scene and is dropped. */
    int async;
    pthread_mutex_t mu;
    uint32_t gen, seq, seen_seq;
    int night_mirror, v_ok, v_want, kick;
    int64_t v_ts, probe_start_ms, last_tick_ms;
    /* Vendor lock: the vendor ISP library is not known to be thread safe (the stock calls get_soft_ps_level and the ISP mode
     * switch from ONE thread). It is held around ops->dn_level (reader), ops->isp_mode and ops->dn_init (tick / start), so
     * they never overlap; the tick may therefore block for up to one read (~1.5 s) inside a switch, acceptable. IR-cut and LED
     * calls do not take it. LOCK ORDER: vmu is taken first and never while mu is held; mu is never held across a vendor call
     * (so a thread blocked in dn_level holds no mu, and the reader never waits for mu while holding vmu). */
    pthread_mutex_t vmu;
    int ir_req; /* pending runtime ir_led (-1 none, 0 off, 1 auto), applied by the tick thread */
    int wdr_warned; /* the WDR re-apply failure is logged once */
} a2k_night_ctl_t;

/* One reading in flight: what the reader captured under the lock before the blocking vendor call. */
typedef struct {
    uint32_t gen;
    int cur_night;
} a2k_night_rd_t;

void a2k_night_ctl_init(a2k_night_ctl_t *c, const a2k_night_cfg_t *cfg, const a2k_night_ops_t *ops);
/* The start sequence: pins, filter to DAY (or NIGHT when forced), vendor parameters for auto. 0, or -1 when the pins cannot
 * be opened (nothing was moved: no day/night at all). now_ms: monotonic. */
int a2k_night_ctl_start(a2k_night_ctl_t *c, int64_t now_ms);
/* Switches the engine to the reader-thread mode: the tick no longer calls dn_level, it consumes the verdicts published by
 * a2k_night_ctl_read_* (any thread). Call after a2k_night_ctl_init, before the first poll. */
void a2k_night_ctl_set_async(a2k_night_ctl_t *c);
/* The reader side. read_once = begin + dn_level (blocking, no lock held) + end, stamped with ops->now_ms. begin / end are
 * split so a host test can interleave ticks with a slow read. end drops the reading when the gen changed since begin. */
void a2k_night_ctl_read_begin(a2k_night_ctl_t *c, a2k_night_rd_t *rd);
void a2k_night_ctl_read_end(a2k_night_ctl_t *c, const a2k_night_rd_t *rd, int rc, int want_night, int64_t ts_ms);
void a2k_night_ctl_read_once(a2k_night_ctl_t *c);
/* 1 (and cleared) when an ISP / IR LED change asks the reader to read again without waiting out its period. */
int a2k_night_ctl_take_kick(a2k_night_ctl_t *c);
/* One 100 ms tick of the automatic mode (no-op for forced modes). */
void a2k_night_ctl_poll(a2k_night_ctl_t *c, int64_t now_ms);
/* The safe end state: filter in (DAY position), coil released, IR LED off. */
void a2k_night_ctl_stop(a2k_night_ctl_t *c);
/* Runtime ir_led (1 auto, 0 off): request from any thread, apply from the tick thread (poll does it; with a forced mode there is
 * no tick thread and the caller applies). off: the LED goes dark at once, the filter and the night image still switch. */
void a2k_night_ctl_ir_request(a2k_night_ctl_t *c, int ir_auto);
void a2k_night_ctl_ir_apply(a2k_night_ctl_t *c);

/* ---- the app side: a tick thread (100 ms wall) and a reader thread (the blocking vendor read), the real pins and the ISP ---- */

/* Called from the day/night thread after every state change; must not block. */
void a2k_night_set_notify(void (*cb)(const a2k_night_info_t *i));
/* After a2k_cap_start. 0 started, -1 not started (logged; the app goes on without day/night). */
int a2k_night_start(const a2k_night_cfg_t *cfg);
/* Before a2k_cap_stop: joins both threads (at most one 300 ms pulse and one vendor read, ~1.5 s) and leaves the safe state. Safe when never started. */
void a2k_night_stop(void);
void a2k_night_get(a2k_night_info_t *out);
/* Runtime ir_led: 0, or -1 when the engine is not running. a2k_night_ir_auto: 1 auto, 0 off (the configured value when not running). */
int a2k_night_set_ir(int ir_auto);
int a2k_night_ir_auto(void);

#endif
