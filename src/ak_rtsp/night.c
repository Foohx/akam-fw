#include <stdio.h>
#include <stdint.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>

#include "ak_ioctls.h"
#include "isp.h"
#include "ircut.h"
#include "ae.h"
#include "night.h"
#include "imgq.h"

/* -------------------------------------------------------------------------
 * Auto day/night mode.
 *
 * Ghidra reverse-engineering of stock anyka_ipc (night_mode_check @
 * 0x0012583c, day_mode_check @ 0x00124aa0, driven by ht_night_mode_auto_th
 * @ 0x000d5abc) found it computes a synthetic "EV" from AE state
 * (ak_vpss_get_ev(): roughly gain_r/g/b * exposure_time) and compares
 * against config-driven thresholds from _ht_hw_settings.ini [night_mode]:
 *   ps_soft_day_to_night_lum   = 6,300,000  (day->night trigger)
 *   ps_soft_night_to_day_lum   = 2,000,000  (night->day trigger)
 *   ps_soft_day/night_check_frame_num = 10  (consecutive confirming samples)
 *   ps_soft_auto_lock_time_ms  = 60,000     (anti-flap lock after a switch)
 *
 * We don't reproduce ak_vpss_get_ev()'s exact formula/units, so reusing
 * anyka_ipc's literal threshold numbers would be meaningless here. Instead
 * this uses hw_exp directly as the EV proxy (gain stayed 1.0x in every
 * ak_rtsp AE capture so far, so hw_exp is already the dominant brightness
 * driver) — thresholds below are picked relative to each frame-rate
 * config's VTS ceiling (2284 day / 3426 night) and NEED HARDWARE
 * CALIBRATION, unlike the AE dead-band tuning which had ground truth to
 * reverse from. The debounce/hysteresis/lock-timer *structure* mirrors
 * stock; the numbers are a starting point, not a confirmed match.
 *
 * Fresh isp_hook capture confirmed the coordinated switch stock performs:
 * IR-cut solenoid + WB table +
 * AK_ISP_set_frame_rate({15,2284,8,15} day <-> {10,3426,8,10} night) all
 * change together.
 * ------------------------------------------------------------------------- */

#define DAY_FPS            15
#define DAY_VTS          2284
#define NIGHT_FPS          10
#define NIGHT_VTS        3426

/* hw_exp proxy thresholds — NEEDS CALIBRATION (see header comment above). */
#define NIGHT_TRIGGER_HW_EXP   3000  /* day, hw_exp above this -> switch to night (approaching day VTS ceiling) */
#define DAY_TRIGGER_HW_EXP     1200  /* night, hw_exp below this -> switch to day */

#define CONFIRM_SAMPLES    10        /* matches stock's ps_soft_day/night_check_frame_num */
#define LOCK_MS         60000        /* matches stock's ps_soft_auto_lock_time_ms */
#define POLL_US        100000        /* 100ms, matches stock's ht_night_mode_auto_th cadence */

static volatile int g_night_running;
static pthread_t    g_night_tid;

/* Live-tunable thresholds + manual override, exposed for control.c.
 * Guarded by g_night_lock below (unlike ae.c's g_tuning, which stays
 * lock-free). */
static night_tuning_t g_tuning = {
    .trigger_hw_exp  = NIGHT_TRIGGER_HW_EXP,
    .day_hw_exp      = DAY_TRIGGER_HW_EXP,
    .confirm_samples = CONFIRM_SAMPLES,
    .lock_ms         = LOCK_MS,
    .override        = NIGHT_MODE_AUTO,
};
/* control.c updates g_tuning from a different pthread than night_loop().
 * Protect the multi-field tuning snapshot and use a generation counter so a
 * manual DAY/NIGHT/AUTO change invalidates stale debounce/lock state. */
static pthread_mutex_t g_night_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned        g_mode_generation = 0;
static volatile int g_is_night = 0;  /* current actual state, boot = day */

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

/* Monotonic milliseconds for the anti-flap lock.
 *
 * On the camera (kernel 4.4, which acknowledges clock_gettime64 without
 * executing it) stock musl's clock_gettime(CLOCK_MONOTONIC) returned garbage:
 * the 60 s lock was seen to expire within seconds. The root cause is fixed by
 * src/common/time32_shim.c; this tick based clock is kept as defence in depth
 * (the lock must never depend on one clock source). Take the time from the
 * kernel tick counter (raw times(2): 32-bit jiffies, no struct, no time_t
 * width) and accumulate it into 64 bits. Only the night thread calls
 * this, so the state is unlocked. Other targets fall back to clock_gettime(). The lock is additionally
 * counted in poll iterations (see night_loop), which needs no clock at all. */
static long long now_ms(void)
{
#if defined(__arm__) && defined(__linux__)
    static uint32_t last_ticks;
    static uint64_t total_ticks;
    static long     hz;
    register long r7 __asm__("r7") = 43; /* __NR_times */
    register long r0 __asm__("r0") = 0;  /* NULL struct tms */

    if (hz <= 0)
        hz = sysconf(_SC_CLK_TCK);
    if (hz > 0 && hz <= 1000) {
        __asm__ volatile("svc 0" : "+r"(r0) : "r"(r7) : "memory");
        uint32_t ticks = (uint32_t)r0;
        /* Start above zero: callers use 0 as "never". */
        if (total_ticks == 0)
            total_ticks = (uint64_t)ticks + (uint64_t)hz;
        else
            total_ticks += (uint32_t)(ticks - last_ticks);
        last_ticks = ticks;
        return (long long)(total_ticks * 1000u / (uint64_t)hz);
    }
#endif
    struct timespec ts = { 0, 0 };
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

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
    printf("[night] hw_exp sustained high -> switching to NIGHT mode\n");
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
}

static void switch_to_day_locked(void)
{
    printf("[night] hw_exp sustained low -> switching to DAY mode\n");
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

static void *night_loop(void *arg)
{
    (void)arg;
    int       confirm              = 0;
    long long last_switch_ms       = 0;
    /* Poll iterations left in the lock. usleep() never returns early on
     * this loop, so N polls last at least N * POLL_US whatever the clock says. */
    long      lock_polls_left      = 0;
    unsigned  seen_mode_generation = 0;
    int       have_mode_generation = 0;

    printf("[night] auto day/night monitor started (poll=%dms)\n", POLL_US / 1000);

    while (g_night_running) {
        usleep(POLL_US);

        night_tuning_t t;
        unsigned mode_generation = night_get_snapshot(&t);

        if (!have_mode_generation) {
            seen_mode_generation = mode_generation;
            have_mode_generation = 1;
        } else if (mode_generation != seen_mode_generation) {
            seen_mode_generation = mode_generation;
            confirm = 0;
            last_switch_ms = 0;
            lock_polls_left = 0;
            printf("[night] mode changed -> reset confirm/lock\n");
        }

        if (t.override == NIGHT_MODE_FORCE_DAY) {
            if (g_is_night) {
                switch_to_day();
                g_is_night = 0;
            }
            confirm = 0;
            last_switch_ms = 0;
            lock_polls_left = 0;
            continue;
        }

        if (t.override == NIGHT_MODE_FORCE_NIGHT) {
            if (!g_is_night) {
                switch_to_night();
                g_is_night = 1;
            }
            confirm = 0;
            last_switch_ms = 0;
            lock_polls_left = 0;
            continue;
        }

        long long now = now_ms();

        if (lock_polls_left > 0)
            --lock_polls_left;
        if (t.lock_ms > 0 &&
            (lock_polls_left > 0 ||
             (last_switch_ms != 0 &&
              now - last_switch_ms < (long long)t.lock_ms))) {
            confirm = 0;
            continue;
        }

        int      lum;
        uint32_t hw_exp, hw_sgain, hw_isp;
        if (ae_get_last_stats(&lum, &hw_exp, &hw_sgain, &hw_isp) != 0)
            continue;

        int desired_night = g_is_night;

        if (g_is_night) {
            if (hw_exp < t.day_hw_exp)
                desired_night = 0;
        } else {
            if (hw_exp > t.trigger_hw_exp)
                desired_night = 1;
        }

        if (desired_night == g_is_night) {
            confirm = 0;
            continue;
        }

        int needed = t.confirm_samples > 0 ? t.confirm_samples : 1;
        if (++confirm < needed)
            continue;

        if (desired_night)
            switch_to_night();
        else
            switch_to_day();

        g_is_night = desired_night;
        confirm = 0;
        last_switch_ms = now_ms();
        lock_polls_left = (long)(((long long)t.lock_ms * 1000 + POLL_US - 1) / POLL_US);

        printf("[night] transition complete -> state=%s lock=%dms\n",
               g_is_night ? "night" : "day", t.lock_ms);
    }

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
