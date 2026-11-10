#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include <unistd.h>

#include "ak_ioctls.h"
#include "isp.h"
#include "ircut.h"
#include "ae.h"
#include "night.h"
#include "night_logic.h"
#include "imgq.h"

/* -------------------------------------------------------------------------
 * Auto day/night mode.
 *
 * The decision itself (when to go to night, when to come back) lives in night_logic.c, a pure state machine with
 * its own host test; this file polls the AE, performs the switches (IR-cut filter, IR LED, frame rate, night /
 * day ISP calibration), owns the live tuning and the 8091 keys. The reasoning is in night_logic.h and in
 * specs/08-spec-image-quality.md (sections 3.4 and 11).
 *
 * Background: Ghidra reverse-engineering of stock anyka_ipc (night_mode_check @ 0x0012583c, day_mode_check @
 * 0x00124aa0, driven by ht_night_mode_auto_th @ 0x000d5abc) found a synthetic "EV" computed from the AE state
 * (ak_vpss_get_ev(): roughly gain * exposure) compared with thresholds from _ht_hw_settings.ini [night_mode]:
 *   ps_soft_day_to_night_lum   = 6,300,000  (day->night trigger)
 *   ps_soft_night_to_day_lum   = 2,000,000  (night->day trigger)
 *   ps_soft_day/night_check_frame_num = 10  (consecutive confirming samples)
 *   ps_soft_auto_lock_time_ms  = 60,000     (anti-flap lock after a switch)
 * A stock day/night capture (specs/08) shows the day->night switch happens when the AE is saturated (exposure
 * 16384 x ISP gain 1.5x = 6.29 M in Q8), and the night->day switch also depends on AWB statistics (the
 * "non infrared ratio") which are not decoded here: see night_logic.h for what is done instead.
 *
 * Fresh isp_hook capture confirmed the coordinated switch stock performs: IR-cut solenoid + WB table +
 * AK_ISP_set_frame_rate({15,2284,8,15} day <-> {10,3426,8,10} night) all change together.
 * ------------------------------------------------------------------------- */

#define DAY_FPS            15
#define DAY_VTS          2284
#define NIGHT_FPS          10
#define NIGHT_VTS        3426

#define POLL_US        (NL_POLL_MS * 1000)  /* 100 ms, matches stock's ht_night_mode_auto_th cadence */
#define STALE_POLLS    10                   /* no new AE sample for 1 s (it ticks at 15 Hz): treat as no sample */

static volatile int g_night_running;
static pthread_t    g_night_tid;

/* Live-tunable thresholds + manual override, exposed for control.c.
 * Guarded by g_night_lock below (unlike ae.c's g_tuning, which stays
 * lock-free). */
static night_tuning_t g_tuning = {
    .trigger_hw_exp      = NIGHT_DEF_TRIGGER_HW_EXP,
    .gain_pct            = NIGHT_DEF_GAIN_PCT,
    .day_hw_exp          = NIGHT_DEF_DAY_HW_EXP,
    .confirm_samples     = NIGHT_DEF_CONFIRM_SAMPLES,
    .day_confirm_samples = NIGHT_DEF_DAY_CONFIRM,
    .lock_ms             = NIGHT_DEF_LOCK_MS,
    .min_night_ms        = NIGHT_DEF_MIN_NIGHT_MS,
    .probe_ms            = NIGHT_DEF_PROBE_MS,
    .probe_settle_ms     = NIGHT_DEF_PROBE_SETTLE_MS,
    .override            = NIGHT_MODE_AUTO,
};
/* control.c updates g_tuning from a different pthread than night_loop().
 * Protect the multi-field tuning snapshot and use a generation counter so a
 * manual DAY/NIGHT/AUTO change invalidates stale debounce/lock state. */
static pthread_mutex_t g_night_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned        g_mode_generation = 0;
static volatile int g_is_night = 0;  /* current actual state, boot = day */
static char            g_logic_status[256] = "mode=day (monitor not started)";  /* night.logic, under g_night_lock */

static unsigned night_get_snapshot(night_tuning_t *out)
{
    unsigned generation;
    pthread_mutex_lock(&g_night_lock);
    *out = g_tuning;
    generation = g_mode_generation;
    pthread_mutex_unlock(&g_night_lock);
    return generation;
}

void night_get_tuning(night_tuning_t *out)
{
    (void)night_get_snapshot(out);
}

void night_set_tuning(const night_tuning_t *in)
{
    pthread_mutex_lock(&g_night_lock);
    if (g_tuning.override != in->override)
        ++g_mode_generation;
    g_tuning = *in;
    pthread_mutex_unlock(&g_night_lock);
}
int  night_is_night(void) { return g_is_night; }

static void set_frame_rate(int fps, int vts)
{
    uint32_t frame_rate[4] = { (uint32_t)fps, (uint32_t)vts, 8, (uint32_t)fps };
    isp_tunnel(ISP_INNER_SET_FRAME_RATE, frame_rate);
    uint32_t sensor_fps[4] = { (uint32_t)fps, 0, 0, (uint32_t)fps };
    isp_tunnel(ISP_INNER_SET_SENSOR_FPS, sensor_fps);

    /* SET_FRAME_RATE appears to reset the exposure attr's dead-band/speed
     * fields (exp_stable_range/exp_hold_range/exp_speed) back to their
     * sensor_isp.conf hardware defaults (4/5/16) — the same narrow/fast
     * values that caused the original bistable AE hunting before that fix.
     * Symptom without this: AE hunts normally after a night<->day switch
     * even though ae.c's tuning is unchanged, because the on-chip values
     * were silently reverted underneath it. Re-apply whatever is currently
     * configured (default or live-tuned via control.c) so it survives the
     * frame-rate reconfiguration. */
    ae_tuning_t t;
    ae_get_tuning(&t);
    ae_set_tuning(&t);
}

static void switch_to_night_locked(void)
{
    ircut_switch(1);
    set_frame_rate(NIGHT_FPS, NIGHT_VTS);
    /* Full night ISP calibration (2026-07-05) — replaces the old
     * isp_set_wb_manual(256,256,256)-only approach. That was a reasonable
     * guess before this was found, but it actively fights the real night
     * subfile's own WB config now: isp_pipeline_init_night() reloads all 24
     * modules (WDR/NR/gamma/CCM/sharpness/WB/etc — a real, separate
     * calibration found concatenated in sensor_isp.conf, not just a WB
     * tweak), so a manual WB override afterward would silently undo part of
     * it. See isp.c's isp_pipeline_init_night() for the full discovery
     * trail. If this subfile isn't present on some other unit/firmware,
     * isp_pipeline_init_night() logs and returns -1 without sending
     * anything — falls back to whatever calibration was already active
     * rather than risk sending garbage. */
    if (isp_pipeline_init_night() != 0) {
        fprintf(stderr, "[night] falling back: night ISP calibration unavailable, "
                        "day calibration + IR-cut/frame-rate switch only\n");
    } else {
        isp_reset_picture_baselines();
    }
    /* The pipeline reload just re-sent module 0x14 (AE), which includes its
     * own EXPOSURE sub-block — the exact same "reset the dead-band/exp_max
     * fields back to sensor_isp.conf's raw defaults" risk set_frame_rate()'s
     * own comment already documents for SET_FRAME_RATE, now happening AFTER
     * that reapplication instead of before it. Re-apply once more so
     * whatever AE tuning is currently configured survives the full reload,
     * not just the frame-rate call. */
    {
        ae_tuning_t t;
        ae_get_tuning(&t);
        ae_set_tuning(&t);
    }
    imgq_after_mode_switch(1);
}

static void switch_to_day_locked(void)
{
    ircut_switch(0);
    set_frame_rate(DAY_FPS, DAY_VTS);
    /* Restore the day ISP calibration (see switch_to_night()'s comment) —
     * replaces the old isp_set_wb_manual(460,256,504)-only restore. */
    if (isp_pipeline_init() != 0) {
        fprintf(stderr, "[night] day ISP calibration reload failed\n");
    } else {
        isp_reset_picture_baselines();
    }
    /* See switch_to_night()'s matching comment — the reload can reset AE
     * dead-band/exp_max fields, re-apply current tuning to be safe. */
    {
        ae_tuning_t t;
        ae_get_tuning(&t);
        ae_set_tuning(&t);
    }
    /* The day reload sent the file's WB block; img.wb=manual puts its own back. */
    imgq_after_mode_switch(0);
}

/* The whole switch runs under the ISP configuration lock, and g_is_night changes before it is released, so an
 * 8091 img.reload / img.wb / img.dump (isp_cfg_lock) never sees a half-switched ISP or a stale mode. */
static void switch_to_night(void)
{
    isp_cfg_lock();
    switch_to_night_locked();
    g_is_night = 1;
    isp_cfg_unlock();
}

static void switch_to_day(void)
{
    isp_cfg_lock();
    switch_to_day_locked();
    g_is_night = 0;
    isp_cfg_unlock();
}

static void publish_status(const nl_state_t *nl, const night_tuning_t *t)
{
    char line[sizeof(g_logic_status)];
    nl_status(nl, t, line, sizeof(line));
    pthread_mutex_lock(&g_night_lock);
    memcpy(g_logic_status, line, sizeof(g_logic_status));
    pthread_mutex_unlock(&g_night_lock);
}

static void *night_loop(void *arg)
{
    (void)arg;
    nl_state_t nl;
    unsigned   seen_mode_generation = 0;
    int        have_mode_generation = 0;
    unsigned   n = 0;
    uint32_t   seen_seq = ae_stats_seq();
    unsigned   stale = 0;
    night_tuning_t t;

    printf("[night] auto day/night monitor started (poll=%dms)\n", NL_POLL_MS);
    night_get_snapshot(&t);
    nl_reset(&nl, &t, g_is_night);

    while (g_night_running) {
        usleep(POLL_US);

        unsigned mode_generation = night_get_snapshot(&t);

        if (!have_mode_generation) {
            seen_mode_generation = mode_generation;
            have_mode_generation = 1;
        } else if (mode_generation != seen_mode_generation) {
            seen_mode_generation = mode_generation;
            if (nl.mode == NL_PROBING)
                ircut_ir_led_probe(0);
            nl_reset(&nl, &t, g_is_night);
            publish_status(&nl, &t);
            printf("[night] mode changed -> reset confirm/lock/probe state\n");
        }

        if (t.override == NIGHT_MODE_FORCE_DAY) {
            if (g_is_night)
                switch_to_day();
            continue;
        }

        if (t.override == NIGHT_MODE_FORCE_NIGHT) {
            if (!g_is_night)
                switch_to_night();
            continue;
        }

        nl_sample_t s;
        nl_limits_t lim;
        int         have = 0, lum = 0;
        have = ae_get_last_stats(&lum, &s.hw_exp, &s.sgain, &s.isp) == 0;
        /* A sample that stopped being refreshed (AE frozen by ae.enabled=0, ioctl failing) says nothing about the
         * light: in particular a probe must not end as "lit room" on the last IR lit value. */
        uint32_t seq = ae_stats_seq();
        if (seq != seen_seq) { seen_seq = seq; stale = 0; }
        else if (stale < STALE_POLLS) stale++;
        if (stale >= STALE_POLLS) have = 0;
        s.lum = lum;
        ae_get_limits(&lim.sgain_max, &lim.isp_max);

        char        why[sizeof(nl.last)];
        nl_action_t act = nl_step(&nl, &t, have ? &s : NULL, &lim, ircut_ir_led_allowed(), why, sizeof(why));

        switch (act) {
        case NL_GO_NIGHT:
            printf("[night] switching to NIGHT mode: %s\n", why);
            switch_to_night();
            printf("[night] transition complete -> state=night lock=%dms\n", t.lock_ms);
            break;
        case NL_GO_DAY:
            printf("[night] switching to DAY mode: %s\n", why);
            switch_to_day();
            printf("[night] transition complete -> state=day lock=%dms\n", t.lock_ms);
            break;
        case NL_PROBE_START:
            printf("[night] ambient light probe: %s\n", why);
            ircut_ir_led_probe(1);
            break;
        case NL_PROBE_END:
            printf("[night] %s\n", why);
            ircut_ir_led_probe(0);
            break;
        default:
            break;
        }
        if (act != NL_NONE || (++n % 5) == 0)
            publish_status(&nl, &t);
    }

    if (nl.mode == NL_PROBING)
        ircut_ir_led_probe(0);
    printf("[night] auto day/night monitor stopped\n");
    return NULL;
}

int night_start(void)
{
    g_night_running = 1;
    if (pthread_create(&g_night_tid, NULL, night_loop, NULL) != 0) {
        perror("[night] pthread_create");
        g_night_running = 0;
        return -1;
    }
    return 0;
}

void night_stop(void)
{
    g_night_running = 0;
    pthread_join(g_night_tid, NULL);
}

/* ---- 8091 keys (see night.h) ---- */

static int is_ours(const char *key)
{
    static const char *const keys[] = { "night.gain_pct", "night.day_confirm_samples", "night.min_night_ms",
                                        "night.probe_ms", "night.probe_settle_ms", "night.ae", "night.logic" };
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++)
        if (!strcmp(key, keys[i])) return 1;
    return 0;
}

static void q8(char *out, size_t cap, uint32_t v)
{
    snprintf(out, cap, "%u.%02ux", v / 256u, (v % 256u) * 100u / 256u);
}

int night_ctl_get(const char *key, char *out, size_t cap)
{
    if (!is_ours(key)) return -1;
    night_tuning_t t;
    night_get_snapshot(&t);
    if (!strcmp(key, "night.gain_pct"))                 snprintf(out, cap, "night.gain_pct=%u", t.gain_pct);
    else if (!strcmp(key, "night.day_confirm_samples")) snprintf(out, cap, "night.day_confirm_samples=%d", t.day_confirm_samples);
    else if (!strcmp(key, "night.min_night_ms"))        snprintf(out, cap, "night.min_night_ms=%d", t.min_night_ms);
    else if (!strcmp(key, "night.probe_ms"))            snprintf(out, cap, "night.probe_ms=%d", t.probe_ms);
    else if (!strcmp(key, "night.probe_settle_ms"))     snprintf(out, cap, "night.probe_settle_ms=%d", t.probe_settle_ms);
    else if (!strcmp(key, "night.logic")) {
        pthread_mutex_lock(&g_night_lock);
        snprintf(out, cap, "night.logic=%s (read-only)", g_logic_status);
        pthread_mutex_unlock(&g_night_lock);
    } else {
        /* night.ae: what the decision sees, live */
        int lum;
        nl_sample_t s;
        nl_limits_t lim;
        char sg[16], ip[16], ms[16], mi[16];
        ae_get_limits(&lim.sgain_max, &lim.isp_max);
        q8(ms, sizeof(ms), lim.sgain_max);
        q8(mi, sizeof(mi), lim.isp_max);
        if (ae_get_last_stats(&lum, &s.hw_exp, &s.sgain, &s.isp) != 0) {
            snprintf(out, cap, "night.ae=no AE sample yet max_sgain=%s max_isp=%s (read-only)", ms, mi);
        } else {
            s.lum = lum;
            q8(sg, sizeof(sg), s.sgain);
            q8(ip, sizeof(ip), s.isp);
            snprintf(out, cap, "night.ae=lum=%d hw_exp=%u sgain=%s isp=%s max_sgain=%s max_isp=%s saturated=%d "
                               "(read-only)", lum, s.hw_exp, sg, ip, ms, mi, nl_saturated(&t, &s, &lim));
        }
    }
    return 0;
}

void night_ctl_list(void (*emit)(void *arg, const char *line), void *arg)
{
    static const char *const keys[] = { "night.gain_pct", "night.day_confirm_samples", "night.min_night_ms",
                                        "night.probe_ms", "night.probe_settle_ms", "night.ae", "night.logic" };
    char line[320];
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++)
        if (night_ctl_get(keys[i], line, sizeof(line)) == 0) emit(arg, line);
}

static int parse_range(const char *val, long lo, long hi, long *out)
{
    char *end = NULL;
    long v = strtol(val, &end, 10);
    if (val[0] == '\0' || !end || *end != '\0' || v < lo || v > hi) return -1;
    *out = v;
    return 0;
}

int night_ctl_set(const char *key, const char *val, char *msg, size_t cap)
{
    if (!is_ours(key)) return -1;
    if (!strcmp(key, "night.ae") || !strcmp(key, "night.logic")) {
        snprintf(msg, cap, "ERR %s is read-only", key);
        return 1;
    }
    long lo = 0, hi = 0;
    if (!strcmp(key, "night.gain_pct"))                 { lo = 1;    hi = 100; }
    else if (!strcmp(key, "night.day_confirm_samples")) { lo = 1;    hi = 36000; }
    else if (!strcmp(key, "night.min_night_ms"))        { lo = 0;    hi = 86400000; }
    else if (!strcmp(key, "night.probe_ms"))            { lo = 10000; hi = 86400000; }
    else                                                { lo = 500;  hi = 60000; }  /* night.probe_settle_ms */
    long v;
    if (parse_range(val, lo, hi, &v) != 0) {
        snprintf(msg, cap, "ERR invalid %s value %s (want an integer in [%ld,%ld])", key, val, lo, hi);
        return 1;
    }
    night_tuning_t t;
    night_get_snapshot(&t);
    if (!strcmp(key, "night.gain_pct"))                 t.gain_pct = (uint32_t)v;
    else if (!strcmp(key, "night.day_confirm_samples")) t.day_confirm_samples = (int)v;
    else if (!strcmp(key, "night.min_night_ms"))        t.min_night_ms = (int)v;
    else if (!strcmp(key, "night.probe_ms"))            t.probe_ms = (int)v;
    else                                                t.probe_settle_ms = (int)v;
    night_set_tuning(&t);
    snprintf(msg, cap, "OK %s=%ld", key, v);
    return 0;
}
