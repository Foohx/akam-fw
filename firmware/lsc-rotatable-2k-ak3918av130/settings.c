/* See settings.h. */
#include "settings.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "api_events.h"
#include "button_rt.h"
#include "detpub.h"
#include "isp.h"
#include "leds.h"
#include "log.h"
#include "cfgreg.h"
#include "netreg.h"
#include "night_ctl.h"
#include "orient.h"
#include "picture.h"
#include "patrol.h"
#include "ptz.h"
#include "scene.h"
#include "scene_check.h"
#include "rtsp_session.h"
#include "speaker.h"
#include "store.h"
#include "track_run.h"
#include "motion.h"
#include "motion_grid.h"
#include "video.h"
#include "api_keys.h"
#include "ptz_vel.h"

#define RW (ST_ACC_READ | ST_ACC_WRITE)
#define RO ST_ACC_READ
#define HM (ST_EXPOSE_HTTP | ST_EXPOSE_MQTT)
#define MODEL_2K 1u
#define MAX_ROWS 144 /* raise when the table grows: build_table() logs and the self-check fails on overflow */
static const char k_none[1] = { 0 }; /* the empty default of the patrol string rows */

/* ------------------------------------------------------------------ module state */
static streg_t g_reg;
static st_def_t g_defs[MAX_ROWS];
static int g_overflow; /* rows that did not fit MAX_ROWS */
static size_t g_nrows;
static a2k_settings_cfg_t g_cfg;
static char g_path[160], g_netdir[120], g_netreq[120], g_netres[128];
static char g_cfgdir[120], g_cfgreq[120], g_cfgres[128]; /* the user files (cfgreg.h) and the request of the supervisor */
static a2k_image_cfg_t g_img;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER; /* guards g_img, g_sys, g_keys */
static hal_sys_sample_t g_sys;
static int g_have_sys, g_keys;

/* ------------------------------------------------------------------ helpers */
static int put(char *out, size_t cap, long v)
{
    int n = snprintf(out, cap, "%ld", v);
    return n < 0 || (size_t)n >= cap ? ST_E_GETTER : n;
}

static int puts_(char *out, size_t cap, const char *s)
{
    int n = snprintf(out, cap, "%s", s);
    return n < 0 || (size_t)n >= cap ? ST_E_GETTER : n;
}

static int num_after(const char *key, const char *prefix)
{
    size_t n = strlen(prefix);
    return strncmp(key, prefix, n) == 0 ? atoi(key + n) : -1;
}

/* ------------------------------------------------------------------ led.* and night.ir_led */
static const char *const LED_RULE[A2K_RULE_N] = { "boot", "nowifi", "person", "stream", "night", "idle" };

static int led_rule_of(const char *key)
{
    int r;
    for (r = 0; r < A2K_RULE_N; r++)
        if (strcmp(key + 4, LED_RULE[r]) == 0)
            return r;
    return -1;
}

static int led_get(const char *key, char *out, size_t cap)
{
    int r;
    if (strcmp(key, "led.mode") == 0)
        return puts_(out, cap, a2k_leds_mode_name(a2k_leds_run_mode()));
    if (strcmp(key, "night.ir_led") == 0)
        return puts_(out, cap, a2k_night_ir_auto() ? "auto" : "off");
    r = led_rule_of(key);
    if (r < 0)
        return ST_E_GETTER;
    a2k_leds_run_pattern(r, out, cap);
    return (int)strlen(out);
}

static int led_set(const char *key, const char *val, st_apply_t *eff)
{
    int r;
    (void)eff;
    if (strcmp(key, "led.mode") == 0)
        return a2k_leds_run_set_mode(a2k_leds_parse_mode(val)) == 0 ? 0 : ST_E_SETTER;
    if (strcmp(key, "night.ir_led") == 0)
        return a2k_night_set_ir(strcmp(val, "auto") == 0) == 0 ? 0 : ST_E_SETTER;
    r = led_rule_of(key);
    return r >= 0 && a2k_leds_run_set_pattern(r, val) == 0 ? 0 : ST_E_SETTER;
}

/* ------------------------------------------------------------------ image.brightness / contrast / sharpness / mains_hz
 * The ISP keeps no setting of its own, so the registry saves them (written only when the user changes one, never at start or while idle): the loaded file
 * reaches img_set at start, and a2k_image_apply remembers what was applied so that a2k_image_replay puts it back after each day / night switch. */
static int img_field(const char *key, int **f)
{
    if (strcmp(key, "image.brightness") == 0)
        *f = &g_img.brightness;
    else if (strcmp(key, "image.contrast") == 0)
        *f = &g_img.contrast;
    else if (strcmp(key, "image.sharpness") == 0)
        *f = &g_img.sharpness;
    else if (strcmp(key, "image.mains_hz") == 0)
        *f = &g_img.mains_hz;
    else
        return -1;
    return 0;
}

static int img_get(const char *key, char *out, size_t cap)
{
    int *f, v;
    if (img_field(key, &f) != 0)
        return ST_E_GETTER;
    pthread_mutex_lock(&g_mu);
    v = *f;
    pthread_mutex_unlock(&g_mu);
    return put(out, cap, v < 0 ? 50 : v); /* "not applied" reads as the neutral 50 */
}

static int img_set(const char *key, const char *val, st_apply_t *eff)
{
    int *f, old, bad;
    a2k_image_cfg_t c;
    (void)eff;
    if (img_field(key, &f) != 0)
        return ST_E_SETTER;
    pthread_mutex_lock(&g_mu);
    old = *f;
    *f = atoi(val);
    c = g_img;
    pthread_mutex_unlock(&g_mu);
    bad = a2k_image_apply(&c); /* ISP calls: on the worker thread, never the HTTP thread */
    if (bad > 0) {
        pthread_mutex_lock(&g_mu);
        *f = old;
        pthread_mutex_unlock(&g_mu);
        return ST_E_SETTER;
    }
    return 0;
}

/* ------------------------------------------------------------------ isp.saturation / isp.denoise / img.wb (the 1080p's names) and isp.nr2 / isp.wdr (picture.h) */
static int pic_get(const char *key, char *out, size_t cap)
{
    if (strcmp(key, "isp.saturation") == 0)
        return put(out, cap, a2k_pic_get_saturation());
    if (strcmp(key, "isp.denoise") == 0)
        return put(out, cap, a2k_pic_get_denoise());
    if (strcmp(key, "isp.nr2") == 0)
        return put(out, cap, a2k_pic_get_nr2());
    if (strcmp(key, "isp.wdr") == 0)
        return put(out, cap, a2k_pic_get_wdr());
    if (strcmp(key, "img.wb") == 0)
        return puts_(out, cap, a2k_pic_get_wb() == A2K_PIC_WB_MANUAL ? "manual" : "conf");
    return ST_E_GETTER;
}

static int pic_set(const char *key, const char *val, st_apply_t *eff)
{
    int rc;
    (void)eff;
    if (strcmp(key, "isp.saturation") == 0)
        rc = a2k_pic_set_saturation(atoi(val));
    else if (strcmp(key, "isp.denoise") == 0)
        rc = a2k_pic_set_denoise(atoi(val));
    else if (strcmp(key, "isp.nr2") == 0)
        rc = a2k_pic_set_nr2(atoi(val));
    else if (strcmp(key, "isp.wdr") == 0)
        rc = a2k_pic_set_wdr(atoi(val));
    else if (strcmp(key, "img.wb") == 0)
        rc = a2k_pic_set_wb(strcmp(val, "manual") == 0 ? A2K_PIC_WB_MANUAL : A2K_PIC_WB_CONF);
    else
        return ST_E_SETTER;
    return rc == A2K_PIC_OK ? 0 : ST_E_SETTER;
}

/* ------------------------------------------------------------------ image.flip / image.mirror / image.ceiling (live, saved only on a user change)
 * flip = vertical, mirror = horizontal, ceiling = both (a 180 degree rotation: a camera mounted upside down). The orientation is recorded FIRST (orient.h opens the settling
 * window of the scene check and moves the tracker, the zones and the directions to the new frame), then the ISP is told; if the ISP refuses, the old orientation is restored. */
static int orient_get_row(const char *key, char *out, size_t cap)
{
    int f, m;
    a2k_orient_get(&f, &m);
    return put(out, cap, strcmp(key, "image.flip") == 0 ? f : strcmp(key, "image.mirror") == 0 ? m : (f && m));
}

static int orient_set_row(const char *key, const char *val, st_apply_t *eff)
{
    int f, m, of, om, v = atoi(val);
    (void)eff;
    a2k_orient_isp_lock();
    a2k_orient_get(&of, &om);
    f = of;
    m = om;
    if (strcmp(key, "image.flip") == 0)
        f = v;
    else if (strcmp(key, "image.mirror") == 0)
        m = v;
    else if (strcmp(key, "image.ceiling") == 0)
        f = m = v;
    else {
        a2k_orient_isp_unlock();
        return ST_E_SETTER;
    }
    if (f == of && m == om) {
        a2k_orient_isp_unlock();
        return 0;
    }
    a2k_orient_set(f, m);
    if (a2k_isp_flip_mirror(f, m) != 0) { /* ISP calls: on the worker thread, never the HTTP thread */
        a2k_orient_set(of, om);
        a2k_orient_isp_unlock();
        return ST_E_SETTER;
    }
    a2k_orient_isp_unlock();
    return 0;
}

/* ------------------------------------------------------------------ track.enabled */
static int track_get(const char *key, char *out, size_t cap)
{
    a2k_track_t *t = a2k_track_run_get();
    (void)key;
    return puts_(out, cap, t && a2k_track_enabled(t) ? "1" : "0");
}

static int track_set(const char *key, const char *val, st_apply_t *eff)
{
    a2k_track_t *t = a2k_track_run_get();
    (void)key;
    (void)eff;
    if (!t)
        return ST_E_SETTER;
    a2k_track_set_enabled(t, strcmp(val, "1") == 0);
    a2k_log("track: person tracking %s (API)", strcmp(val, "1") == 0 ? "on" : "off");
    return 0;
}

/* ------------------------------------------------------------------ ptz.patrol.idle_start_s, ptz.patrol.route_default (specs/ptz-patrol.md) */
static int pat_get(const char *key, char *out, size_t cap)
{
    char d[PP_NAME_MAX + 1];
    if (strcmp(key, "ptz.patrol.idle_start_s") == 0)
        return put(out, cap, a2k_patrol_get_idle_s());
    if (strcmp(key, "ptz.patrol.travel_budget") == 0)
        return put(out, cap, a2k_patrol_get_travel_budget());
    a2k_patrol_get_default(d, sizeof d);
    return puts_(out, cap, d);
}

static int pat_set(const char *key, const char *val, st_apply_t *eff)
{
    (void)eff;
    if (strcmp(key, "ptz.patrol.idle_start_s") == 0) {
        if (a2k_patrol_set_idle_s(atoi(val)) != 0)
            return ST_E_SETTER;
        a2k_log("patrol: idle start %s s (API)", val);
        return 0;
    }
    if (strcmp(key, "ptz.patrol.travel_budget") == 0)
        return a2k_patrol_set_travel_budget(atoi(val)) == 0 ? 0 : ST_E_SETTER;
    return a2k_patrol_set_default(val) == 0 ? 0 : ST_E_SETTER;
}

static int pat_check(const struct streg *reg, const char *key, const char *val)
{
    (void)reg;
    return strcmp(key, "ptz.patrol.route_default") == 0 && val[0] && !pp_name_ok(val) ? ST_E_CHECK : 0;
}

/* ptz.patrol.state / route / step (read only) */
static int pat_state_get(const char *key, char *out, size_t cap)
{
    char st[16], rt[PP_NAME_MAX + 1];
    int step;
    a2k_patrol_state(st, sizeof st, rt, sizeof rt, &step);
    if (strcmp(key, "ptz.patrol.state") == 0)
        return puts_(out, cap, st);
    if (strcmp(key, "ptz.patrol.route") == 0)
        return puts_(out, cap, rt);
    return put(out, cap, step);
}

/* ------------------------------------------------------------------ detect.publish_hz, detect.boxes */
static int dpub_get(const char *key, char *out, size_t cap)
{
    if (strcmp(key, "detect.boxes") == 0) {
        int n = a2k_dp_latest(out, cap);
        return n < 0 ? ST_E_GETTER : n;
    }
    return put(out, cap, a2k_dp_hz());
}

static int dpub_set(const char *key, const char *val, st_apply_t *eff)
{
    (void)key;
    (void)eff;
    a2k_dp_set_hz(atoi(val));
    return 0;
}

/* ------------------------------------------------------------------ track.return, track.return_delay_s */
static int tret_get(const char *key, char *out, size_t cap)
{
    a2k_track_t *t = a2k_track_run_get();
    int home = 1, delay = 30;
    if (t)
        a2k_track_get_return(t, &home, &delay);
    if (strcmp(key, "track.return") == 0)
        return puts_(out, cap, home ? "home" : "stay");
    return put(out, cap, delay);
}

static int tret_set(const char *key, const char *val, st_apply_t *eff)
{
    a2k_track_t *t = a2k_track_run_get();
    int home, delay;
    (void)eff;
    if (!t)
        return ST_E_SETTER;
    a2k_track_get_return(t, &home, &delay);
    if (strcmp(key, "track.return") == 0)
        home = strcmp(val, "home") == 0;
    else
        delay = atoi(val);
    a2k_track_set_return(t, home, delay);
    a2k_log("track: return %s, delay %d s (API)", home ? "home" : "stay", delay);
    return 0;
}

/* ------------------------------------------------------------------ track.band_pct, track.head_pct */
static int ttune_get(const char *key, char *out, size_t cap)
{
    a2k_track_t *t = a2k_track_run_get();
    int band = 6, head = 15;
    if (t)
        a2k_track_get_tune(t, &band, &head);
    return put(out, cap, strcmp(key, "track.band_pct") == 0 ? band : head);
}

static int ttune_set(const char *key, const char *val, st_apply_t *eff)
{
    a2k_track_t *t = a2k_track_run_get();
    int band, head;
    (void)eff;
    if (!t)
        return ST_E_SETTER;
    a2k_track_get_tune(t, &band, &head);
    if (strcmp(key, "track.band_pct") == 0)
        band = atoi(val);
    else
        head = atoi(val);
    a2k_track_set_tune(t, band, head);
    a2k_log("track: band %d%%, head %d%% (API)", band, head);
    return 0;
}

#define TL_STR2(x) #x
#define TL_STR(x) TL_STR2(x)
/* ------------------------------------------------------------------ track.gain_pan, track.gain_tilt, track.tilt_band_pct */
static int tgain_get(const char *key, char *out, size_t cap)
{
    a2k_track_t *t = a2k_track_run_get();
    int gp = TL_2K_GAIN_PAN, gt = TL_2K_GAIN_TILT, tb = 10;
    if (t)
        a2k_track_get_gains(t, &gp, &gt, &tb);
    return put(out, cap, strcmp(key, "track.gain_pan") == 0 ? gp : strcmp(key, "track.gain_tilt") == 0 ? gt : tb);
}

static int tgain_set(const char *key, const char *val, st_apply_t *eff)
{
    a2k_track_t *t = a2k_track_run_get();
    int gp, gt, tb;
    (void)eff;
    if (!t)
        return ST_E_SETTER;
    a2k_track_get_gains(t, &gp, &gt, &tb);
    if (strcmp(key, "track.gain_pan") == 0)
        gp = atoi(val);
    else if (strcmp(key, "track.gain_tilt") == 0)
        gt = atoi(val);
    else
        tb = atoi(val);
    a2k_track_set_gains(t, gp, gt, tb);
    a2k_log("track: gain pan %d, tilt %d, tilt band %d%% (API)", gp, gt, tb);
    return 0;
}

/* ------------------------------------------------------------------ track.tilt_return_s */
static int ttret_get(const char *key, char *out, size_t cap)
{
    a2k_track_t *t = a2k_track_run_get();
    int s = 0;
    (void)key;
    if (t)
        a2k_track_get_tilt_return(t, &s);
    return put(out, cap, s);
}

static int ttret_set(const char *key, const char *val, st_apply_t *eff)
{
    a2k_track_t *t = a2k_track_run_get();
    (void)key;
    (void)eff;
    if (!t)
        return ST_E_SETTER;
    a2k_track_set_tilt_return(t, atoi(val));
    a2k_log("track: tilt return %d s (API)", atoi(val));
    return 0;
}

/* ------------------------------------------------------------------ track.zone_pct, track.zone_y_pct, track.zone_head_pct, track.motion */
static const char *const OPT_TMOT[] = { "continuous", "steps", NULL };

/* ------------------------------------------------------------------ track.lag_ms */
static int tlag_get(const char *key, char *out, size_t cap)
{
    a2k_track_t *t = a2k_track_run_get();
    (void)key;
    return put(out, cap, t ? a2k_track_get_lag(t) : 50);
}

static int tlag_set(const char *key, const char *val, st_apply_t *eff)
{
    a2k_track_t *t = a2k_track_run_get();
    (void)key;
    (void)eff;
    if (!t)
        return ST_E_SETTER;
    a2k_track_set_lag(t, atoi(val));
    a2k_log("track: video lag %d ms (API)", atoi(val));
    return 0;
}

/* ------------------------------------------------------------------ track.tilt_back_s */
static int tback_get(const char *key, char *out, size_t cap)
{
    a2k_track_t *t = a2k_track_run_get();
    int s = 5;
    (void)key;
    if (t)
        a2k_track_get_tilt_back(t, &s);
    return put(out, cap, s);
}

static int tback_set(const char *key, const char *val, st_apply_t *eff)
{
    a2k_track_t *t = a2k_track_run_get();
    (void)key;
    (void)eff;
    if (!t)
        return ST_E_SETTER;
    a2k_track_set_tilt_back(t, atoi(val));
    a2k_log("track: tilt back after %d s (API)", atoi(val));
    return 0;
}

/* ------------------------------------------------------------------ track.tilt (0 = pan only tracking) */
static int ttilt_get(const char *key, char *out, size_t cap)
{
    a2k_track_t *t = a2k_track_run_get();
    (void)key;
    return puts_(out, cap, t && a2k_track_get_tilt(t) ? "1" : "0");
}

static int ttilt_set(const char *key, const char *val, st_apply_t *eff)
{
    a2k_track_t *t = a2k_track_run_get();
    (void)key;
    (void)eff;
    if (!t || a2k_track_set_tilt(t, strcmp(val, "1") == 0 ? 1 : 0) != 0)
        return ST_E_SETTER;
    a2k_log("track: tilt %s (API)", strcmp(val, "1") == 0 ? "follows (pan and tilt)" : "off (pan only)");
    return 0;
}

/* ------------------------------------------------------------------ ptz.scene_min (the boot scene check threshold, "0.80".."0.99") */
static int smin_get(const char *key, char *out, size_t cap)
{
    int v = a2k_scene_get_min();
    char b[16];
    (void)key;
    if (v % 10) snprintf(b, sizeof b, "0.%03d", v);
    else snprintf(b, sizeof b, "0.%02d", v / 10);
    return puts_(out, cap, b);
}

static int smin_set(const char *key, const char *val, st_apply_t *eff)
{
    int v = sc_parse_min(val);
    (void)key;
    (void)eff;
    if (v < 0 || a2k_scene_set_min_api(v) != 0) return ST_E_SETTER;
    a2k_log("ptz: scene check threshold %s (API)", val);
    return 0;
}

static int smin_check(const struct streg *reg, const char *key, const char *val)
{
    int v = sc_parse_min(val);
    (void)reg;
    (void)key;
    return v >= 800 && v <= 990 && strlen(val) == 4 ? 0 : ST_E_CHECK; /* two decimals, like the assistant sends */
}

/* ------------------------------------------------------------------ track.search (the lost-target search: legacy | motion) */
static const char *const OPT_TSEARCH[] = { "legacy", "motion", NULL };

static int tsearch_get(const char *key, char *out, size_t cap)
{
    a2k_track_t *t = a2k_track_run_get();
    (void)key;
    return puts_(out, cap, !t || a2k_track_get_search(t) == TL_SEARCH_MOTION ? "motion" : "legacy");
}

static int tsearch_set(const char *key, const char *val, st_apply_t *eff)
{
    a2k_track_t *t = a2k_track_run_get();
    (void)key;
    (void)eff;
    if (!t || a2k_track_set_search(t, strcmp(val, "motion") == 0 ? TL_SEARCH_MOTION : TL_SEARCH_LEGACY) != 0)
        return ST_E_SETTER;
    a2k_log("track: lost-target search %s (API)", val);
    return 0;
}

/* ------------------------------------------------------------------ motion.* (motion.h): enabled / sensitivity settings, active / dir / x / y / source read-only state */
static int motion_get(const char *key, char *out, size_t cap)
{
    struct a2k_motion_info i;
    a2k_motion_info(&i);
    if (strcmp(key, "motion.enabled") == 0) return puts_(out, cap, i.enabled ? "1" : "0");
    if (strcmp(key, "motion.sensitivity") == 0) return put(out, cap, a2k_motion_get_sensitivity());
    if (strcmp(key, "motion.active") == 0) return puts_(out, cap, i.active ? "1" : "0");
    if (strcmp(key, "motion.dir") == 0) return puts_(out, cap, mg_dir_name(i.dir));
    if (strcmp(key, "motion.x") == 0) return put(out, cap, i.x);
    if (strcmp(key, "motion.y") == 0) return put(out, cap, i.y);
    return puts_(out, cap, a2k_motion_source_name());
}

static int motion_set(const char *key, const char *val, st_apply_t *eff)
{
    (void)eff;
    if (strcmp(key, "motion.enabled") == 0) {
        a2k_motion_set_enabled(strcmp(val, "1") == 0);
        a2k_log("motion: detection %s (API)", strcmp(val, "1") == 0 ? "on" : "off");
        return 0;
    }
    a2k_motion_set_sensitivity(atoi(val));
    a2k_log("motion: sensitivity %d (API)", atoi(val));
    return 0;
}

/* ------------------------------------------------------------------ track.close_scan, track.close_scan_up, track.close_scan_down */
static int cscan_which(const char *key)
{
    return strcmp(key, "track.close_scan") == 0 ? 0 : strcmp(key, "track.close_scan_up") == 0 ? 1 : 2;
}

static int cscan_get(const char *key, char *out, size_t cap)
{
    a2k_track_t *t = a2k_track_run_get();
    int w = cscan_which(key);
    return put(out, cap, t ? a2k_track_get_close_scan(t, w) : (w == 0 ? 1 : w == 1 ? 250 : 300));
}

static int cscan_set(const char *key, const char *val, st_apply_t *eff)
{
    a2k_track_t *t = a2k_track_run_get();
    (void)eff;
    if (!t || a2k_track_set_close_scan(t, cscan_which(key), atoi(val)) != 0)
        return ST_E_SETTER;
    a2k_log("track: %s = %d (API)", key, atoi(val));
    return 0;
}

/* ------------------------------------------------------------------ track.lost_policy, track.lost_step */
static int tlost_get(const char *key, char *out, size_t cap)
{
    a2k_track_t *t = a2k_track_run_get();
    char buf[256];
    if (strcmp(key, "track.lost_step") == 0) {
        if (!t)
            return puts_(out, cap, "idle");
        a2k_track_get_lost_step(t, buf, sizeof buf);
        return puts_(out, cap, buf);
    }
    if (!t)
        return puts_(out, cap, TL_LOST_DEFAULT);
    a2k_track_get_lost_policy(t, buf, sizeof buf);
    return puts_(out, cap, buf);
}

static int tlost_check(const struct streg *reg, const char *key, const char *val)
{
    struct tl_cfg tmp;
    (void)reg;
    if (strcmp(key, "track.lost_policy") != 0)
        return 0;
    tl_cfg_defaults(&tmp);
    return tl_lost_parse(val, &tmp) == 0 ? 0 : ST_E_CHECK;
}

static int tlost_set(const char *key, const char *val, st_apply_t *eff)
{
    a2k_track_t *t = a2k_track_run_get();
    (void)key;
    (void)eff;
    if (!t)
        return ST_E_SETTER;
    if (a2k_track_set_lost_policy(t, val) != 0)
        return ST_E_SETTER;
    a2k_log("track: lost policy %s (API)", val);
    return 0;
}

static int tzone_get(const char *key, char *out, size_t cap)
{
    a2k_track_t *t = a2k_track_run_get();
    int x = 40, y = 30, h = 20;
    if (t && strcmp(key, "track.motion") == 0)
        return puts_(out, cap, a2k_track_get_motion(t) == TL_MOTION_CONT ? "continuous" : "steps");
    if (strcmp(key, "track.motion") == 0)
        return puts_(out, cap, "continuous");
    if (t)
        a2k_track_get_zones(t, &x, &y, &h);
    return put(out, cap, strcmp(key, "track.zone_pct") == 0 ? x : strcmp(key, "track.zone_y_pct") == 0 ? y : h);
}

static int tzone_set(const char *key, const char *val, st_apply_t *eff)
{
    a2k_track_t *t = a2k_track_run_get();
    int x, y, h;
    (void)eff;
    if (!t)
        return ST_E_SETTER;
    if (strcmp(key, "track.motion") == 0) {
        a2k_track_set_motion(t, strcmp(val, "steps") == 0 ? TL_MOTION_STEPS : TL_MOTION_CONT);
        a2k_log("track: motion %s (API)", val);
        return 0;
    }
    a2k_track_get_zones(t, &x, &y, &h);
    if (strcmp(key, "track.zone_pct") == 0)
        x = atoi(val);
    else if (strcmp(key, "track.zone_y_pct") == 0)
        y = atoi(val);
    else
        h = atoi(val);
    a2k_track_set_zones(t, x, y, h);
    a2k_log("track: comfort zone %d%% x %d%% (head %d%%) (API)", x, y, h);
    return 0;
}

/* ------------------------------------------------------------------ video<N>.* */
static const char *const VP_NAME[A2K_VP_N] = { "kbps", "max_kbps", "fps", "min_qp", "max_qp", "gop", "rc" };
static const char *const OPT_RC[] = { "vbr", "cbr", "avbr", NULL };
static const char *const OPT_AUTO[] = { "auto", "on", "off", NULL };
static const char *const OPT_IR[] = { "auto", "off", NULL };
static const char *const OPT_MAINS[] = { "50", "60", NULL };
static const char *const OPT_WB[] = { "conf", NULL }; /* "manual" is refused on the 2K (picture.h): there is no way to freeze the automatic gains */
static const char *const OPT_AMP[] = { "auto", "on", "off", NULL };
static const char *const OPT_DUPLEX[] = { "half", "full", NULL };
static const char *const OPT_TRET[] = { "stay", "home", NULL };
static const char *const G_LED[] = { "led", NULL };
static char g_vkey[A2K_VID_STREAMS][A2K_VP_N][20], g_vdef[A2K_VID_STREAMS][A2K_VP_N][12];

static int vid_of(const char *key, int *chn, int *p)
{
    int c = num_after(key, "video"), i;
    const char *dot = strchr(key, '.');
    if (c < 0 || c >= A2K_VID_STREAMS || !dot)
        return -1;
    for (i = 0; i < A2K_VP_N; i++)
        if (strcmp(dot + 1, VP_NAME[i]) == 0) {
            *chn = c;
            *p = i;
            return 0;
        }
    return -1;
}

static int vid_get(const char *key, char *out, size_t cap)
{
    int c, p;
    if (vid_of(key, &c, &p) != 0)
        return ST_E_GETTER;
    return a2k_vid_get_text(c, p, out, cap) < 0 ? ST_E_GETTER : (int)strlen(out);
}

static int vid_set(const char *key, const char *val, st_apply_t *eff)
{
    int c, p;
    (void)eff; /* the capture thread applies it live or re-opens that one channel; the video module persists /config/akam/video */
    if (vid_of(key, &c, &p) != 0)
        return ST_E_SETTER;
    return a2k_vid_set(c, p, val) == 0 ? 0 : ST_E_SETTER;
}

/* a2k_vid_valid() as a registry rule: min QP <= max QP, and the peak (0 = 1.25 x the target) not below the target. Reads the values of
 * the same PATCH through streg_peek, so the whole batch is judged before any setter runs. */
static long peek_num(const struct streg *reg, int chn, const char *name, long dflt)
{
    char k[24], v[STREG_VAL_MAX];
    snprintf(k, sizeof k, "video%d.%s", chn, name);
    return streg_peek(reg, k, v, sizeof v) > 0 ? atol(v) : dflt;
}

static int vid_check(const struct streg *reg, const char *key, const char *val)
{
    int c, p;
    long v = atol(val);
    if (vid_of(key, &c, &p) != 0)
        return 0;
    if (p == A2K_VP_MINQP || p == A2K_VP_MAXQP) {
        long other = peek_num(reg, c, p == A2K_VP_MINQP ? "max_qp" : "min_qp", p == A2K_VP_MINQP ? 51 : 0);
        return (p == A2K_VP_MINQP ? v <= other : other <= v) ? 0 : ST_E_CHECK;
    }
    if (p == A2K_VP_KBPS || p == A2K_VP_MAXKBPS) {
        long kbps = p == A2K_VP_KBPS ? v : peek_num(reg, c, "kbps", 0), max = p == A2K_VP_MAXKBPS ? v : peek_num(reg, c, "max_kbps", 0);
        long eff = max ? max : kbps + kbps / 4;
        return eff >= kbps ? 0 : ST_E_CHECK;
    }
    return 0;
}

/* ------------------------------------------------------------------ net.* backend (request file, see the header) */
static int net_read_value(const char *file, const char *name, char *out, size_t cap)
{
    char path[200], buf[A2K_STORE_MAX + 1], *line, *save = NULL;
    int n;
    snprintf(path, sizeof path, "%s/%s", g_netdir, file);
    n = a2k_store_read(path, buf, sizeof buf);
    if (n < 0)
        return -1;
    if (!name) { /* the whole first line */
        buf[strcspn(buf, "\r\n")] = '\0';
        return buf[0] ? puts_(out, cap, buf) : -1;
    }
    for (line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        size_t l = strlen(name);
        if (strncmp(line, name, l) == 0 && line[l] == '=')
            return puts_(out, cap, line + l + 1);
    }
    return -1;
}

static int netb_get(const char *key, char *out, size_t cap)
{
    if (strcmp(key, "net.hostname") == 0)
        return net_read_value("hostname", NULL, out, cap) < 0 ? puts_(out, cap, "akam-rot2k") : (int)strlen(out);
    if (strcmp(key, "net.wifi_country") == 0)
        return net_read_value("net", "wifi_country", out, cap) < 0 ? puts_(out, cap, "none") : (int)strlen(out);
    if (strcmp(key, "net.wifi_band") == 0)
        return net_read_value("net", "wifi_band", out, cap) < 0 ? puts_(out, cap, "2.4") : (int)strlen(out);
    return -1;
}

/* net.apply: what became of the last net.* request. "queued" while the request file waits for the supervisor, else the first word of
 * the result akam_net.sh left in the .res file (pending | ok | refused | reverted), "idle" when none. The PATCH reply only says queued. */
static int net_apply_state(char *out, size_t cap)
{
    char buf[40];
    int n = a2k_store_read(g_netreq, buf, sizeof buf);
    if (n >= 0 || n == -2)
        return puts_(out, cap, "queued");
    n = a2k_store_read(g_netres, buf, sizeof buf);
    if (n < 0)
        return puts_(out, cap, "idle");
    buf[strcspn(buf, " \r\n")] = '\0';
    return puts_(out, cap, buf[0] && strspn(buf, "abcdefghijklmnopqrstuvwxyz") == strlen(buf) ? buf : "idle");
}

static int netb_set(const char *key, const char *val)
{
    char line[96], old[8];
    int n;
    n = a2k_store_read(g_netreq, old, sizeof old);
    if (n >= 0 || n == -2)
        return -1; /* the previous request has not been taken yet (even a longer one): one at a time */
    if (strcmp(key, "net.hostname") == 0)
        n = snprintf(line, sizeof line, "hostname set %s\n", val);
    else if (strcmp(key, "net.wifi_country") == 0)
        n = snprintf(line, sizeof line, "wifi set country %s\n", val);
    else if (strcmp(key, "net.wifi_band") == 0)
        n = snprintf(line, sizeof line, "wifi set band %s\n", val);
    else
        return -1;
    if (n < 0 || (size_t)n >= sizeof line)
        return -1;
    a2k_log("net: request queued: %.*s", n - 1, line);
    return a2k_store_write(g_netreq, line, (size_t)n);
}

static const a2k_netreg_backend_t g_netb = { netb_get, netb_set };

/* ------------------------------------------------------------------ the user-file rows (cfgreg.h): MQTT, ONVIF, stream password, NTP, microphone
 * Getters read the files the app and the supervisor read (plain fopen: in the card-less image they are symlinks into /config/akam, which
 * a2k_store_read refuses). Setters queue the changes of ONE PATCH in RAM and cfg_flush (end of the PATCH, on the worker) writes them as ONE request
 * file in /tmp (mode 0600: it may hold a password) which the supervisor takes, validates again and applies (own_app_run.sh, cfg_request).
 * The app writes nothing to the flash here. */
#define CFG_PEND_MAX 12
static struct {
    char key[24];
    char val[132];
} g_cpend[CFG_PEND_MAX];
static int g_cpend_n;

static int cfg_file(const char *name, char *buf, size_t cap)
{
    char path[200];
    FILE *f;
    size_t n;
    snprintf(path, sizeof path, "%s/%s", g_cfgdir, name);
    f = fopen(path, "r");
    if (!f)
        return -1;
    n = fread(buf, 1, cap - 1, f);
    fclose(f);
    buf[n] = '\0';
    return (int)n;
}

/* key=value of a file (the last line of the key wins; "#" comments and blanks as the supervisor reads akam.conf); 1 found, 0 absent */
static int cfg_kv(const char *file, const char *key, char *out, size_t cap)
{
    char buf[1024], *save = NULL, *line;
    size_t kl = strlen(key);
    int found = 0;
    if (cfg_file(file, buf, sizeof buf) < 0)
        return 0;
    for (line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char *h = strchr(line, '#'), *w, *r;
        if (h)
            *h = '\0';
        for (w = r = line; *r; r++) /* the supervisor drops spaces, tabs and CR everywhere in the line */
            if (*r != ' ' && *r != '\t' && *r != '\r')
                *w++ = *r;
        *w = '\0';
        if (strncmp(line, key, kl) == 0 && line[kl] == '=') {
            snprintf(out, cap, "%s", line + kl + 1);
            found = 1;
        }
    }
    return found;
}

static int cfg_auth(char *user, size_t ucap, char *state, size_t scap)
{
    char buf[300], *c, *p;
    size_t i;
    user[0] = '\0';
    if (cfg_file("rtsp.conf", buf, sizeof buf) < 0) {
        snprintf(state, scap, "off");
        return 0;
    }
    buf[strcspn(buf, "\r\n")] = '\0';
    snprintf(state, scap, "invalid");
    c = strchr(buf, ':');
    if (!c || c == buf || (size_t)(c - buf) > 32 || strncmp(c, ":akam:", 6) != 0)
        return -1;
    p = c + 6;
    if (strlen(p) != 32)
        return -1;
    for (i = 0; i < 32; i++)
        if (!((p[i] >= '0' && p[i] <= '9') || (p[i] >= 'a' && p[i] <= 'f')))
            return -1;
    *c = '\0';
    snprintf(user, ucap, "%.32s", buf);
    snprintf(state, scap, "on");
    return 0;
}

static int cfg_apply_state(char *out, size_t cap)
{
    char buf[40];
    int n = a2k_store_read(g_cfgreq, buf, sizeof buf);
    if (n >= 0 || n == -2)
        return puts_(out, cap, "queued");
    n = a2k_store_read(g_cfgres, buf, sizeof buf);
    if (n < 0)
        return puts_(out, cap, "idle");
    buf[strcspn(buf, " \r\n")] = '\0';
    return puts_(out, cap, buf[0] && strspn(buf, "abcdefghijklmnopqrstuvwxyz") == strlen(buf) ? buf : "idle");
}

static int cfgb_get(const char *key, char *out, size_t cap)
{
    char v[200], u[40], st[16];
    if (strcmp(key, "cfg.apply") == 0)
        return cfg_apply_state(out, cap);
    if (strcmp(key, "rtsp.user") == 0 || strcmp(key, "rtsp.auth") == 0) {
        (void)cfg_auth(u, sizeof u, st, sizeof st);
        return puts_(out, cap, strcmp(key, "rtsp.user") == 0 ? u : st);
    }
    if (strcmp(key, "audio.mic_enable") == 0)
        return puts_(out, cap, cfg_kv("akam.conf", "audio", v, sizeof v) && !strcmp(v, "1") ? "1" : "0");
    if (strcmp(key, "onvif.enabled") == 0)
        return puts_(out, cap, cfg_kv("akam.conf", "onvif", v, sizeof v) && !strcmp(v, "1") ? "1" : "0");
    if (strcmp(key, "onvif.port") == 0)
        return puts_(out, cap, cfg_kv("akam.conf", "onvif_port", v, sizeof v) && atoi(v) >= 1024 && atoi(v) <= 65535 ? v : "8000");
    if (strncmp(key, "mqtt.", 5) == 0) {
        const char *k = key + 5, *def = !strcmp(k, "port") ? "1883" : !strcmp(k, "enabled") ? "0" : "";
        return puts_(out, cap, cfg_kv("mqtt", k, v, sizeof v) ? v : def);
    }
    if (strcmp(key, "ntp.servers") == 0) { /* the servers of the file ntp, space separated; "" while NTP is off (no file) */
        char buf[400], *save = NULL, *line;
        out[0] = '\0';
        if (cfg_file("ntp", buf, sizeof buf) < 0)
            return 0;
        for (line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
            size_t l = strlen(out), s;
            line[strcspn(line, "\r")] = '\0';
            if (strncmp(line, "server=", 7) != 0)
                continue;
            s = strlen(line + 7);
            if (s == 0 || l + s + 2 >= cap)
                continue;
            if (l) { /* servers are separated by one space */
                out[l++] = ' ';
                out[l] = '\0';
            }
            snprintf(out + l, cap - l, "%s", line + 7);
        }
        return (int)strlen(out);
    }
    return -1;
}

static int cfgb_set(const char *key, const char *val)
{
    int i;
    for (i = 0; i < g_cpend_n; i++)
        if (strcmp(g_cpend[i].key, key) == 0)
            break;
    if (i == g_cpend_n) {
        if (i >= CFG_PEND_MAX || strlen(key) >= sizeof g_cpend[0].key)
            return -1;
        g_cpend_n++;
    }
    if (strlen(val) >= sizeof g_cpend[0].val)
        return -1;
    snprintf(g_cpend[i].key, sizeof g_cpend[i].key, "%s", key);
    snprintf(g_cpend[i].val, sizeof g_cpend[i].val, "%s", val);
    return 0;
}

static void wipe(void *p, size_t n)
{
    volatile unsigned char *q = p;
    while (n--)
        *q++ = 0;
}

/* End of a PATCH: the queued changes become ONE request (after any request the supervisor has not taken yet: the later line of a key wins).
 * 0 nothing queued or written, < 0 the request could not be written (the caller fails the rows). Never logs a value. */
static int cfg_flush(void)
{
    char text[A2K_STORE_MAX + 1], old[A2K_STORE_MAX + 1];
    size_t o = 0;
    int i, n, rc;
    if (g_cpend_n == 0)
        return 0;
    n = a2k_store_read(g_cfgreq, old, sizeof old);
    if (n > 0 && (size_t)n < sizeof text) {
        memcpy(text, old, (size_t)n);
        o = (size_t)n;
    }
    for (i = 0; i < g_cpend_n; i++) {
        int w = snprintf(text + o, sizeof text - o, "%s=%s\n", g_cpend[i].key, g_cpend[i].val);
        if (w < 0 || (size_t)w >= sizeof text - o) {
            rc = -1;
            goto out;
        }
        o += (size_t)w;
    }
    rc = a2k_store_write_mode(g_cfgreq, text, o, 0600);
    a2k_log("cfg: request queued (%d key(s): %s%s)", g_cpend_n, g_cpend[0].key, g_cpend_n > 1 ? ", ..." : "");
out:
    wipe(text, sizeof text);
    wipe(old, sizeof old);
    wipe(g_cpend, sizeof g_cpend);
    g_cpend_n = 0;
    return rc == 0 ? 0 : -1;
}

static int is_cfg_key(const char *key)
{
    int i;
    for (i = 0; i < A2K_CFGREG_N; i++)
        if (strcmp(a2k_cfgreg_defs[i].key, key) == 0)
            return 1;
    return 0;
}

static const a2k_cfgreg_backend_t g_cfgb = { cfgb_get, cfgb_set };


/* ------------------------------------------------------------------ onvif.* (read only: akam.conf onvif=1 / onvif_port=N asks, the app reports; no flash write from the API) */
static void (*g_onvif_state)(char *, size_t);
static int (*g_onvif_wanted)(int *);

void a2k_settings_set_onvif(void (*state)(char *out, size_t cap), int (*wanted)(int *port))
{
    g_onvif_state = state;
    g_onvif_wanted = wanted;
}

static int onvif_get(const char *key, char *out, size_t cap)
{
    int port = 0, want = g_onvif_wanted ? g_onvif_wanted(&port) : 0;
    if (strcmp(key, "onvif.state") == 0) {
        char st[48] = "off";
        if (g_onvif_state)
            g_onvif_state(st, sizeof st);
        return puts_(out, cap, st);
    }
    if (strcmp(key, "onvif.enabled") == 0)
        return put(out, cap, want ? 1 : 0);
    return put(out, cap, port > 0 ? port : 8000);
}

/* ------------------------------------------------------------------ state rows (read only) */
static int st_get(const char *key, char *out, size_t cap)
{
    hal_sys_sample_t s;
    int have, keys;
    pthread_mutex_lock(&g_mu);
    s = g_sys;
    have = g_have_sys;
    keys = g_keys;
    pthread_mutex_unlock(&g_mu);
    if (strncmp(key, "sys.flash_writes_", 17) == 0) {
        unsigned pos, scene;
        a2k_ptz_wear_counts(&pos, &scene);
        if (strcmp(key + 17, "ptzpos") == 0) return put(out, cap, pos);
        if (strcmp(key + 17, "scene") == 0) return put(out, cap, scene);
        if (strcmp(key + 17, "patrol") == 0) return put(out, cap, a2k_patrol_writes());
        if (strcmp(key + 17, "button") == 0) return put(out, cap, a2k_btn_writes());
        return put(out, cap, a2k_store_writes());
    }
    if (strcmp(key, "ptz.travel_since_cal") == 0) return put(out, cap, (long)a2k_ptz_travel());
    if (strncmp(key, "ptz.drift_", 10) == 0 || strcmp(key, "ptz.recal_reason") == 0 || strcmp(key, "ptz.untrusted") == 0) {
        a2k_ptz_drift_t dr;
        a2k_ptz_drift_info(&dr);
        if (strcmp(key, "ptz.drift_corrections") == 0) return put(out, cap, (long)dr.corrections);
        if (strcmp(key, "ptz.drift_last") == 0) return puts_(out, cap, dr.last);
        if (strcmp(key, "ptz.recal_reason") == 0) return puts_(out, cap, dr.recal_reason);
        if (strcmp(key, "ptz.untrusted") == 0) return put(out, cap, dr.untrusted ? 1 : 0);
        if (strcmp(key, "ptz.drift_last_px") == 0) {
            char v[24];
            snprintf(v, sizeof v, dr.have ? "%d,%d" : "none", dr.dx, dr.dy);
            return puts_(out, cap, v);
        }
    }
    if (strcmp(key, "ptz.drift_ref") == 0 || strcmp(key, "ptz.drift_ref_age_s") == 0) {
        unsigned long age = 0;
        int m = a2k_ptz_drift_ref_info(&age);
        if (strcmp(key, "ptz.drift_ref") == 0) return puts_(out, cap, m == 3 ? "both" : m == 2 ? "night" : m == 1 ? "day" : "none");
        return put(out, cap, m ? (long)age : -1);
    }
    if (strcmp(key, "ptz.stall_steps_est") == 0 || strcmp(key, "ptz.cal_full") == 0 || strcmp(key, "ptz.cal_short") == 0) {
        unsigned long st;
        unsigned cf, cs;
        a2k_ptz_stall_info(&st, &cf, &cs, NULL);
        return put(out, cap, key[4] == 's' ? (long)st : key[8] == 'f' ? (long)cf : (long)cs);
    }
    if (strncmp(key, "ptz.rehome_", 11) == 0) {
        unsigned runs, canc;
        unsigned long ls;
        const char *why;
        a2k_ptz_rehome_info(&runs, &canc);
        a2k_ptz_rehome_last(&ls, &why);
        if (strcmp(key, "ptz.rehome_count") == 0) return put(out, cap, (long)runs);
        if (strcmp(key, "ptz.rehome_cancel") == 0) return put(out, cap, (long)canc);
        if (strcmp(key, "ptz.rehome_last_reason") == 0) return puts_(out, cap, why);
        if (strcmp(key, "ptz.rehome_last_s") == 0) {
            char v[24];
            if (ls) snprintf(v, sizeof v, "%lu", ls); else snprintf(v, sizeof v, "none");
            return puts_(out, cap, v);
        }
    }
    if (strcmp(key, "button.last_gesture") == 0) {
        char g[BG_NAME_MAX + 4];
        a2k_btn_last_gesture(g, sizeof g);
        return puts_(out, cap, g);
    }
    if (strcmp(key, "sys.cpu_pct") == 0) return put(out, cap, have ? s.cpu_pct : HAL_SYS_NA);
    if (strcmp(key, "sys.load1_x100") == 0) return put(out, cap, have ? s.load1_x100 : HAL_SYS_NA);
    if (strcmp(key, "sys.mem_total_kb") == 0) return put(out, cap, have ? s.mem_total_kb : HAL_SYS_NA);
    if (strcmp(key, "sys.mem_avail_kb") == 0) return put(out, cap, have ? s.mem_avail_kb : HAL_SYS_NA);
    if (strcmp(key, "sys.uptime_s") == 0) return put(out, cap, have ? s.uptime_s : HAL_SYS_NA);
    if (strcmp(key, "net.wifi_rssi") == 0) return put(out, cap, have ? s.rssi_dbm : HAL_SYS_NA);
    if (strcmp(key, "net.wifi_lq") == 0) return put(out, cap, have ? s.link_quality : HAL_SYS_NA);
    if (strcmp(key, "api.keys") == 0) return put(out, cap, keys);
    if (strcmp(key, "net.apply") == 0) return net_apply_state(out, cap);
    if (strcmp(key, "rtsp.clients") == 0) return put(out, cap, a2k_rtsp_clients());
    if (strcmp(key, "detect.person") == 0) return put(out, cap, a2k_apievt_person());
    if (strcmp(key, "night.active") == 0) {
        a2k_night_info_t i;
        a2k_night_get(&i);
        return put(out, cap, i.valid && i.night);
    }
    if (strncmp(key, "ptz.", 4) == 0) {
        a2k_ptz_info_t i;
        a2k_ptz_info(&i);
        if (strcmp(key, "ptz.state") == 0) return puts_(out, cap, a2k_ptz_state_name(i.state));
        if (strcmp(key, "ptz.pan") == 0) return put(out, cap, i.known ? (long)i.pan : -1);
        if (strcmp(key, "ptz.tilt") == 0) return put(out, cap, i.known ? (long)i.tilt : -1);
    }
    return ST_E_GETTER;
}

/* ------------------------------------------------------------------ the table */
static void row(const char *key, st_type_t type, long lo, long hi, const char *const *opts, const char *def, uint8_t persist,
                uint8_t access, int (*get)(const char *, char *, size_t),
                int (*set)(const char *, const char *, st_apply_t *))
{
    st_def_t *d;
    if (g_nrows >= MAX_ROWS) {
        g_overflow++; /* never silent: a2k_settings_init() reports it and refuses to start the registry */
        return;
    }
    d = &g_defs[g_nrows++];
    memset(d, 0, sizeof *d);
    d->key = key;
    d->type = type;
    d->lo = lo;
    d->hi = hi;
    d->opts = opts;
    d->def = def;
    d->apply = AP_LIVE;
    d->persist = persist;
    d->access = access;
    d->expose = HM;
    d->get = get;
    d->set = set;
}

static void build_table(void)
{
    static const char *const LED_KEY[A2K_RULE_N] = { "led.boot", "led.nowifi", "led.person", "led.stream", "led.night", "led.idle" };
    int c, p, r;
    size_t i;
    g_nrows = 0;
    for (i = 0; i < A2K_NETREG_N; i++)
        if (g_nrows < MAX_ROWS)
            g_defs[g_nrows++] = a2k_netreg_defs[i];
    row("led.mode", ST_ENUM, 0, 0, OPT_AUTO, "on", 1, RW, led_get, led_set);
    for (r = 0; r < A2K_RULE_N; r++)
        row(LED_KEY[r], ST_PATTERN, 0, 0, G_LED, a2k_rule_default(r), 1, RW, led_get, led_set);
    row("night.ir_led", ST_ENUM, 0, 0, OPT_IR, "auto", 1, RW, led_get, led_set);
    row("image.brightness", ST_INT, 0, 100, NULL, "50", 1, RW, img_get, img_set);
    row("image.contrast", ST_INT, 0, 100, NULL, "50", 1, RW, img_get, img_set);
    row("image.sharpness", ST_INT, 0, 100, NULL, "50", 1, RW, img_get, img_set);
    row("image.mains_hz", ST_ENUM, 0, 0, OPT_MAINS, "50", 1, RW, img_get, img_set);
    row("image.flip", ST_BOOL, 0, 0, NULL, "0", 1, RW, orient_get_row, orient_set_row);
    row("image.mirror", ST_BOOL, 0, 0, NULL, "0", 1, RW, orient_get_row, orient_set_row);
    row("image.ceiling", ST_BOOL, 0, 0, NULL, "0", 0, RW, orient_get_row, orient_set_row); /* flip + mirror: not saved itself, the two rows above are */
    /* The 1080p's picture settings (control port isp.saturation, isp.denoise, img.wb), through the 2K ISP (picture.h). Saturation and denoise are saved and replayed
     * after every day / night switch; img.wb accepts only conf on the 2K (manual is refused, picture.h). The denoise default is 0 here (the camera's
     * own tables; the 1080p defaults to 2) until it has been judged on the camera. */
    row("isp.saturation", ST_INT, -50, 50, NULL, "0", 1, RW, pic_get, pic_set);
    row("isp.denoise", ST_INT, 0, 4, NULL, "0", 1, RW, pic_get, pic_set);
    /* The 2K's own: isp.nr2 (spatial noise reduction, 0 = the camera's own table) and isp.wdr (backlight strength 0..100, 90 = the stock's effect 40), both saved,
     * live and replayed after every day / night switch (picture.h). */
    row("isp.nr2", ST_INT, 0, 4, NULL, "0", 1, RW, pic_get, pic_set);
    row("isp.wdr", ST_INT, 0, 100, NULL, "90", 1, RW, pic_get, pic_set);
    row("img.wb", ST_ENUM, 0, 0, OPT_WB, "conf", 0, RW, pic_get, pic_set);
    /* Speaker and two-way audio: the 1080p's names and ranges (control port audio.*), the shared engine behind them (speaker.h). The speaker is
     * opt-in here (default 0) until it has run on a camera; the volume default is the 1080p's. out_amp is RAM only (a bring-up knob). */
    row("audio.out_enable", ST_BOOL, 0, 0, NULL, "0", 1, RW, a2k_speaker_get, a2k_speaker_set);
    row("audio.out_volume", ST_INT, 0, 100, NULL, "100", 1, RW, a2k_speaker_get, a2k_speaker_set);
    row("audio.out_gain", ST_INT, 0, 5, NULL, "2", 1, RW, a2k_speaker_get, a2k_speaker_set);
    row("audio.out_amp", ST_ENUM, 0, 0, OPT_AMP, "auto", 0, RW, a2k_speaker_get, a2k_speaker_set);
    row("audio.talk_enable", ST_BOOL, 0, 0, NULL, "1", 1, RW, a2k_speaker_get, a2k_speaker_set);
    row("audio.talk_duplex", ST_ENUM, 0, 0, OPT_DUPLEX, "half", 1, RW, a2k_speaker_get, a2k_speaker_set);
    row("audio.out_state", ST_STRING, 1, 16, NULL, "off", 0, RO, a2k_speaker_get, NULL);
    row("audio.talk_state", ST_STRING, 1, 16, NULL, "idle", 0, RO, a2k_speaker_get, NULL);
    row("detect.publish_hz", ST_INT, 0, 10, NULL, "2", 1, RW, dpub_get, dpub_set);
    row("track.enabled", ST_BOOL, 0, 0, NULL, "0", 1, RW, track_get, track_set);
    row("track.return", ST_ENUM, 0, 0, OPT_TRET, "home", 1, RW, tret_get, tret_set);
    row("track.return_delay_s", ST_INT, 0, 86400, NULL, "30", 1, RW, tret_get, tret_set);
    row("track.band_pct", ST_INT, 2, 40, NULL, "6", 1, RW, ttune_get, ttune_set);
    row("track.head_pct", ST_INT, 0, 50, NULL, "15", 1, RW, ttune_get, ttune_set);
    row("track.gain_pan", ST_INT, 200, 4000, NULL, TL_STR(TL_2K_GAIN_PAN), 1, RW, tgain_get, tgain_set);
    row("track.gain_tilt", ST_INT, 200, 4000, NULL, TL_STR(TL_2K_GAIN_TILT), 1, RW, tgain_get, tgain_set);
    row("track.tilt_band_pct", ST_INT, 2, 40, NULL, "10", 1, RW, tgain_get, tgain_set);
    row("track.tilt_return_s", ST_INT, 0, 3600, NULL, "0", 1, RW, ttret_get, ttret_set);
    row("track.zone_pct", ST_INT, 0, 90, NULL, "40", 1, RW, tzone_get, tzone_set);
    row("track.zone_y_pct", ST_INT, 0, 90, NULL, "30", 1, RW, tzone_get, tzone_set);
    row("track.zone_head_pct", ST_INT, 0, 90, NULL, "20", 1, RW, tzone_get, tzone_set);
    row("track.motion", ST_ENUM, 0, 0, OPT_TMOT, "continuous", 1, RW, tzone_get, tzone_set);
    row("track.lost_policy", ST_STRING, 1, 255, NULL, TL_LOST_DEFAULT, 1, RW, tlost_get, tlost_set);
    g_defs[g_nrows - 1].check = tlost_check;
    row("track.lost_step", ST_STRING, 1, 40, NULL, "idle", 0, RO, tlost_get, NULL);
    row("track.tilt_back_s", ST_INT, 0, 600, NULL, "5", 1, RW, tback_get, tback_set);
    row("track.tilt", ST_BOOL, 0, 0, NULL, "0", 1, RW, ttilt_get, ttilt_set);
    row("track.close_scan", ST_INT, 0, 1, NULL, "1", 1, RW, cscan_get, cscan_set);
    row("track.close_scan_up", ST_INT, 0, 600, NULL, "250", 1, RW, cscan_get, cscan_set);
    row("track.close_scan_down", ST_INT, 0, 600, NULL, "300", 1, RW, cscan_get, cscan_set);
    row("track.lag_ms", ST_INT, -150, 300, NULL, "50", 1, RW, tlag_get, tlag_set);
    row("track.search", ST_ENUM, 0, 0, OPT_TSEARCH, "motion", 1, RW, tsearch_get, tsearch_set);
    row("ptz.scene_min", ST_STRING, 4, 5, NULL, "0.85", 1, RW, smin_get, smin_set);
    g_defs[g_nrows - 1].check = smin_check;
    row("motion.enabled", ST_BOOL, 0, 0, NULL, "1", 1, RW, motion_get, motion_set);
    row("motion.sensitivity", ST_INT, 1, 10, NULL, "5", 1, RW, motion_get, motion_set);
    row("motion.active", ST_BOOL, 0, 0, NULL, "0", 0, RO, motion_get, NULL);
    row("motion.dir", ST_STRING, 1, 8, NULL, "none", 0, RO, motion_get, NULL);
    row("motion.x", ST_INT, -1, 100, NULL, "-1", 0, RO, motion_get, NULL);
    row("motion.y", ST_INT, -1, 100, NULL, "-1", 0, RO, motion_get, NULL);
    row("motion.source", ST_STRING, 1, 8, NULL, "diff", 0, RO, motion_get, NULL);
    for (c = 0; c < A2K_VID_STREAMS; c++) {
        a2k_vid_t cur;
        a2k_vid_get(c, &cur);
        for (p = 0; p < A2K_VP_N; p++) {
            int lo = 0, hi = 0;
            a2k_vid_range(c, p, &lo, &hi);
            snprintf(g_vkey[c][p], sizeof g_vkey[c][p], "video%d.%s", c, VP_NAME[p]);
            a2k_vid_text(p, cur.v[p], g_vdef[c][p], sizeof g_vdef[c][p]);
            if (p == A2K_VP_RC)
                row(g_vkey[c][p], ST_ENUM, 0, 0, OPT_RC, g_vdef[c][p], 0, RW, vid_get, vid_set);
            else /* video<N>.max_kbps also takes 0 = automatic (1.25 x the target): the setter re-validates */
                row(g_vkey[c][p], ST_INT, p == A2K_VP_MAXKBPS ? 0 : lo, hi, NULL, g_vdef[c][p], 0, RW, vid_get, vid_set);
            g_defs[g_nrows - 1].check = vid_check;
        }
    }
    /* read-only state: a getter, no setter, never persisted */
    row("sys.cpu_pct", ST_INT, HAL_SYS_NA, 100, NULL, "-1000000", 0, RO, st_get, NULL);
    row("sys.load1_x100", ST_INT, HAL_SYS_NA, 100000, NULL, "-1000000", 0, RO, st_get, NULL);
    row("sys.mem_total_kb", ST_INT, HAL_SYS_NA, 100000000, NULL, "-1000000", 0, RO, st_get, NULL);
    row("sys.mem_avail_kb", ST_INT, HAL_SYS_NA, 100000000, NULL, "-1000000", 0, RO, st_get, NULL);
    row("sys.uptime_s", ST_INT, HAL_SYS_NA, 2000000000, NULL, "-1000000", 0, RO, st_get, NULL);
    row("net.wifi_rssi", ST_INT, HAL_SYS_NA, 0, NULL, "-1000000", 0, RO, st_get, NULL);
    row("net.wifi_lq", ST_INT, HAL_SYS_NA, 1000, NULL, "-1000000", 0, RO, st_get, NULL);
    row("rtsp.clients", ST_INT, 0, 16, NULL, "0", 0, RO, st_get, NULL);
    row("detect.person", ST_BOOL, 0, 0, NULL, "0", 0, RO, st_get, NULL);
    row("detect.boxes", ST_STRING, 0, STREG_VAL_MAX - 1, NULL, "{}", 0, RO, dpub_get, NULL);
    row("night.active", ST_BOOL, 0, 0, NULL, "0", 0, RO, st_get, NULL);
    row("ptz.state", ST_STRING, 1, 16, NULL, "unknown", 0, RO, st_get, NULL);
    row("ptz.pan", ST_INT, -1, 100000, NULL, "-1", 0, RO, st_get, NULL);
    row("ptz.tilt", ST_INT, -1, 100000, NULL, "-1", 0, RO, st_get, NULL);
    row("ptz.travel_since_cal", ST_INT, 0, 2000000000, NULL, "0", 0, RO, st_get, NULL);
    row("ptz.drift_last_px", ST_STRING, 1, 16, NULL, "none", 0, RO, st_get, NULL); /* the last displacement of the home view against the reference, QUARTER pixels of the 320 px detection frame "dx,dy" (4 per px; a quarter pixel is about 2.4 tilt steps), or none */
    row("ptz.drift_ref", ST_STRING, 1, 8, NULL, "none", 0, RO, st_get, NULL); /* the drift reference held in RAM: none, day, night, both (captured at a proven position; none = the drift check skips) */
    row("ptz.drift_ref_age_s", ST_INT, -1, 2000000000, NULL, "-1", 0, RO, st_get, NULL); /* age in seconds of the newest drift reference, -1 when none */
    row("ptz.drift_last", ST_STRING, 1, 16, NULL, "none", 0, RO, st_get, NULL); /* verdict of the last home-view check: none, ok, shift, recal, inconc, skip, skip-flat (flat view where the reference had detail: a re-home / recalibration is scheduled), flat-persist (still flat after a completed re-home: not a slip, nothing more is done) */
    row("ptz.drift_corrections", ST_INT, 0, 2000000000, NULL, "0", 0, RO, st_get, NULL);
    row("ptz.recal_reason", ST_STRING, 1, 71, NULL, "none", 0, RO, st_get, NULL);
    row("ptz.stall_steps_est", ST_INT, 0, 2000000000, NULL, "0", 0, RO, st_get, NULL); /* ESTIMATED steps the motor skipped against a stop since the app start (calibrations and re-homes: declared steps beyond the believed distance to the stop; a park never counts) */
    row("ptz.cal_full", ST_INT, 0, 2000000000, NULL, "0", 0, RO, st_get, NULL); /* full-stroke calibrations completed since the app start */
    row("ptz.cal_short", ST_INT, 0, 2000000000, NULL, "0", 0, RO, st_get, NULL); /* short calibrations (head very likely at home) completed since the app start */
    row("ptz.rehome_count", ST_INT, 0, 2000000000, NULL, "0", 0, RO, st_get, NULL); /* tilt-only re-homes completed in this run */
    row("ptz.rehome_cancel", ST_INT, 0, 2000000000, NULL, "0", 0, RO, st_get, NULL); /* tilt-only re-homes interrupted (a person, a command) in this run */
    row("ptz.rehome_last_s", ST_STRING, 1, 16, NULL, "none", 0, RO, st_get, NULL); /* uptime seconds when the last re-home completed, or none */
    row("ptz.rehome_last_reason", ST_STRING, 1, 8, NULL, "none", 0, RO, st_get, NULL); /* why the last completed re-home ran: gap (travel since the last one), flat (flat home view), none */
    row("ptz.untrusted", ST_BOOL, 0, 0, NULL, "0", 0, RO, st_get, NULL); /* the position is doubted (a drift recalibration is due or running): moves are still allowed, no scene reference is written */
    g_defs[g_nrows - 1].expose = ST_EXPOSE_HTTP;
    /* PTZ patrol (off by default): the two settings, then the state; HTTP only (no Home Assistant entity) */
    row("ptz.patrol.idle_start_s", ST_INT, 0, 86400, NULL, "0", 1, RW, pat_get, pat_set);
    g_defs[g_nrows - 1].expose = ST_EXPOSE_HTTP;
    row("ptz.patrol.travel_budget", ST_INT, 1000, 2000000000, NULL, "200000", 1, RW, pat_get, pat_set);
    g_defs[g_nrows - 1].expose = ST_EXPOSE_HTTP;
    row("ptz.patrol.route_default", ST_STRING, 0, PP_NAME_MAX, NULL, k_none, 1, RW, pat_get, pat_set);
    g_defs[g_nrows - 1].expose = ST_EXPOSE_HTTP;
    g_defs[g_nrows - 1].check = pat_check;
    row("ptz.patrol.state", ST_STRING, 1, 16, NULL, "idle", 0, RO, pat_state_get, NULL);
    g_defs[g_nrows - 1].expose = ST_EXPOSE_HTTP;
    row("ptz.patrol.route", ST_STRING, 0, PP_NAME_MAX, NULL, k_none, 0, RO, pat_state_get, NULL);
    g_defs[g_nrows - 1].expose = ST_EXPOSE_HTTP;
    row("ptz.patrol.step", ST_INT, -1, PP_STEPS, NULL, "-1", 0, RO, pat_state_get, NULL);
    g_defs[g_nrows - 1].expose = ST_EXPOSE_HTTP;
    row("sys.flash_writes_patrol", ST_INT, 0, 2000000000, NULL, "0", 0, RO, st_get, NULL);
    g_defs[g_nrows - 1].expose = ST_EXPOSE_HTTP;
    row("sys.flash_writes_button", ST_INT, 0, 2000000000, NULL, "0", 0, RO, st_get, NULL);
    g_defs[g_nrows - 1].expose = ST_EXPOSE_HTTP;
    row("button.last_gesture", ST_STRING, 1, 16, NULL, "none", 0, RO, st_get, NULL);
    g_defs[g_nrows - 1].expose = ST_EXPOSE_HTTP;
    row("sys.flash_writes_ptzpos", ST_INT, 0, 2000000000, NULL, "0", 0, RO, st_get, NULL);
    row("sys.flash_writes_scene", ST_INT, 0, 2000000000, NULL, "0", 0, RO, st_get, NULL);
    row("sys.flash_writes_store", ST_INT, 0, 2000000000, NULL, "0", 0, RO, st_get, NULL);
    row("net.apply", ST_STRING, 1, 16, NULL, "idle", 0, RO, st_get, NULL);
    row("api.keys", ST_INT, 0, AKEY_MAX, NULL, "0", 0, RO, st_get, NULL);
    for (i = 0; i < A2K_CFGREG_N; i++) /* onvif.enabled / onvif.port (the configured value, cfgreg.h), mqtt.*, rtsp.*, ntp.servers, audio.mic_enable */
        if (g_nrows < MAX_ROWS)
            g_defs[g_nrows++] = a2k_cfgreg_defs[i];
        else
            g_overflow++;
    row("onvif.state", ST_STRING, 1, 47, NULL, "off", 0, RO, onvif_get, NULL); /* off | on:<port> | no_password | error:<reason> */
    g_defs[g_nrows - 1].expose = ST_EXPOSE_HTTP;
}

const streg_t *a2k_settings_reg(void) { return &g_reg; }

void a2k_settings_note_sys(const hal_sys_sample_t *s)
{
    pthread_mutex_lock(&g_mu);
    g_sys = *s;
    g_have_sys = 1;
    pthread_mutex_unlock(&g_mu);
}

void a2k_settings_set_keys(int n)
{
    pthread_mutex_lock(&g_mu);
    g_keys = n;
    pthread_mutex_unlock(&g_mu);
}

/* ------------------------------------------------------------------ actions */
static const a2k_action_info_t g_actions[] = {
    { "ptz.home", AKEY_WRITE, NULL },
    { "ptz.park", AKEY_WRITE, NULL },
    { "ptz.recalibrate", AKEY_WRITE, NULL },
    { "ptz.step", AKEY_WRITE, "dir:left|right|up|down" },
    { "ptz.move", AKEY_WRITE, "dx:-400..400,dy:-400..400" },
    { "ptz.vel", AKEY_WRITE, "pan:-1..1,tilt:-1..1,lease_ms:150..2000 (decimals as strings; own rate budget)" },
    { "ptz.patrol.start", AKEY_WRITE, "route:<route name>" },
    { "ptz.patrol.stop", AKEY_WRITE, NULL },
    { "ptz.goto_preset", AKEY_WRITE, "name:<preset name>" },
    { "mode.apply", AKEY_WRITE, "name:<mode name>" },
    { "audio.beep", AKEY_WRITE, NULL },
    { "settings.reset", AKEY_WRITE, "key:<setting key> | prefix:<key prefix>" },
    { "settings.save", AKEY_WRITE, NULL },
};
#define N_ACTIONS (sizeof g_actions / sizeof g_actions[0])

size_t a2k_settings_action_count(void) { return N_ACTIONS; }
const a2k_action_info_t *a2k_settings_action(size_t i) { return i < N_ACTIONS ? &g_actions[i] : NULL; }

int a2k_settings_action_find(const char *name)
{
    size_t i;
    for (i = 0; i < N_ACTIONS; i++)
        if (strcmp(name, g_actions[i].name) == 0)
            return (int)i;
    return -1;
}

static const char *arg_of(const jflat_t *in, const char *name)
{
    size_t i;
    for (i = 0; i < in->n; i++)
        if (strcmp(in->key[i], name) == 0)
            return in->val[i];
    return NULL;
}

static int parse_step(const char *s, int *out)
{
    char *end;
    long v;
    if (!s || !*s || strlen(s) > 5)
        return -1;
    errno = 0;
    v = strtol(s, &end, 10);
    if (errno || *end || v < -400 || v > 400)
        return -1;
    *out = (int)v;
    return 0;
}

static long mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static void job_err(a2k_job_t *j, int http, const char *err, const char *detail)
{
    j->http = http;
    j->err = err;
    snprintf(j->detail, sizeof j->detail, "%s", detail ? detail : "-");
}

static int g_dirty;
/* A change of this row must reach the file. image.ceiling is not in the file itself (persist 0) but it sets image.flip and image.mirror, which are: a patch of
 * the ceiling alone has to mark them dirty too, or the mount is lost at the next boot. */
static int row_saves(const st_def_t *d) { return d->persist || strcmp(d->key, "image.ceiling") == 0; }
static void act_mode(a2k_job_t *j);

static void act_reset(a2k_job_t *j)
{
    const char *key = arg_of(&j->in, "key"), *pre = arg_of(&j->in, "prefix");
    size_t i;
    int done = 0, failed = 0;
    if ((key != NULL) == (pre != NULL)) {
        job_err(j, 422, "args", "exactly one of key or prefix");
        return;
    }
    for (i = 0; i < g_reg.n; i++) {
        const st_def_t *d = &g_reg.defs[i];
        st_result_t r;
        if (!(d->access & ST_ACC_WRITE) || d->secret || !d->def || !streg_find(&g_reg, d->key))
            continue;
        if (key ? strcmp(key, d->key) != 0 : strncmp(pre, d->key, strlen(pre)) != 0)
            continue;
        if (d->min_scope > j->scope)
            continue;
        if (streg_set(&g_reg, j->src, j->scope, d->key, d->def, &r) == 200)
            done++, g_dirty |= row_saves(d);
        else
            failed++;
    }
    if (!done && !failed) {
        job_err(j, 404, "unknown_key", "no such writable setting");
        return;
    }
    j->http = failed ? 207 : 200;
    snprintf(j->detail, sizeof j->detail, "reset %d failed %d", done, failed);
}

/* ptz.vel {"pan":-1..1,"tilt":-1..1,"lease_ms":150..2000}: the same continuous manual motion as the ONVIF ContinuousMove (a2k_ptz_vel_manual: the speed mapped to the motor
 * divisor, lease = lease_ms (default 450), a repeat refreshes it, soft limits, refused while parked / calibrating). Both axes in the dead zone (or 0) = stop. Fractions go in a JSON
 * string ("0.5"): the flat parser of the API has integers and strings only. The rate budget of this action (5 per second per peer, 8 overall) is in api_core.c, outside the 0.4/s of
 * the other signed requests. */
static void act_vel(a2k_job_t *j)
{
    ptzv_cmd_t m;
    int lease = 0, rc;
    const char *why = "-";
    if (ptzv_parse_args(arg_of(&j->in, "pan"), arg_of(&j->in, "tilt"), arg_of(&j->in, "lease_ms"), &m, &lease, &why) != 0) {
        job_err(j, 422, "args", why);
        return;
    }
    if (!m.sx && !m.sy) {
        (void)a2k_ptz_manual_stop();
        return;
    }
    rc = a2k_ptz_vel_manual(m.sx, m.sy, m.dx, m.dy, lease);
    if (rc == A2K_PTZ_E_PARKED)
        job_err(j, 409, "unavailable", "the camera is parked (privacy mode)");
    else if (rc == A2K_PTZ_E_NOTREADY)
        job_err(j, 409, "unavailable", "the motor is calibrating or its position is unknown");
    else if (rc != 0)
        job_err(j, 409, "unavailable", "the motor is not running or refused the command");
}

static void run_action(a2k_job_t *j)
{
    static long last_move;
    int idx = a2k_settings_action_find(j->name), rc = 0, dx = 0, dy = 0;
    const char *a;
    j->http = 200;
    j->err = NULL;
    snprintf(j->detail, sizeof j->detail, "ok");
    if (idx < 0) {
        job_err(j, 404, "unknown_action", NULL);
        return;
    }
    if (g_actions[idx].min_scope > j->scope) {
        job_err(j, 403, "scope", NULL);
        return;
    }
    if (strcmp(j->name, "ptz.home") == 0)
        rc = a2k_ptz_command(A2K_PTZ_HOME);
    else if (strcmp(j->name, "ptz.park") == 0)
        rc = a2k_ptz_command(A2K_PTZ_PARK);
    else if (strcmp(j->name, "ptz.recalibrate") == 0)
        rc = a2k_ptz_command(A2K_PTZ_RECAL);
    else if (strcmp(j->name, "ptz.step") == 0) {
        a = arg_of(&j->in, "dir");
        if (a && !strcmp(a, "left")) rc = a2k_ptz_command(A2K_PTZ_LEFT);
        else if (a && !strcmp(a, "right")) rc = a2k_ptz_command(A2K_PTZ_RIGHT);
        else if (a && !strcmp(a, "up")) rc = a2k_ptz_command(A2K_PTZ_UP);
        else if (a && !strcmp(a, "down")) rc = a2k_ptz_command(A2K_PTZ_DOWN);
        else {
            job_err(j, 422, "args", "dir is left, right, up or down");
            return;
        }
    } else if (strcmp(j->name, "ptz.move") == 0) {
        long now = mono_ms();
        const char *sx = arg_of(&j->in, "dx"), *sy = arg_of(&j->in, "dy");
        if ((sx && parse_step(sx, &dx)) || (sy && parse_step(sy, &dy)) || (!dx && !dy)) {
            job_err(j, 422, "args", "dx and dy are integers in -400..400, not both 0");
            return;
        }
        if (last_move && now - last_move < 150) {
            job_err(j, 429, "rate", "ptz.move at most 6 per second");
            return;
        }
        last_move = now;
        if (a2k_patrol_active()) { /* a manual move ends a patrol: stop it and give its running move the time to be cancelled */
            (void)a2k_patrol_stop_route();
            usleep(120000);
        }
        rc = a2k_ptz_nudge(dx, dy);
        if (rc == A2K_PTZ_E_LIMIT) {
            job_err(j, 409, "limit", "no room: the head is at the soft limit in that direction, nothing moved");
            return;
        }
    } else if (strcmp(j->name, "ptz.vel") == 0) {
        act_vel(j);
        return;
    } else if (strcmp(j->name, "ptz.patrol.start") == 0) {
        a = arg_of(&j->in, "route");
        if (!a || !pp_name_ok(a)) {
            job_err(j, 422, "args", "route is a route name");
            return;
        }
        rc = a2k_patrol_start_route(a);
        if (rc == PP_E_NOTFOUND) {
            job_err(j, 404, "not_found", "no such route");
            return;
        }
        if (rc != 0) {
            job_err(j, 409, "unavailable", "the motor is not ready (position unknown, calibrating or privacy)");
            return;
        }
    } else if (strcmp(j->name, "ptz.patrol.stop") == 0) {
        rc = a2k_patrol_stop_route();
    } else if (strcmp(j->name, "ptz.goto_preset") == 0) {
        a = arg_of(&j->in, "name");
        if (!a || !pp_name_ok(a)) {
            job_err(j, 422, "args", "name is a preset name");
            return;
        }
        rc = a2k_patrol_goto_preset(a);
        if (rc == PP_E_NOTFOUND) {
            job_err(j, 404, "not_found", "no such preset");
            return;
        }
        if (rc != 0)
            rc = -1;
    } else if (strcmp(j->name, "mode.apply") == 0) {
        act_mode(j);
        return;
    } else if (strcmp(j->name, "settings.reset") == 0) {
        act_reset(j);
        return;
    } else if (strcmp(j->name, "audio.beep") == 0) {
        if (a2k_speaker_beep() != 0) {
            job_err(j, 409, "unavailable", "the speaker is not running (audio.out_enable 1, see audio.out_state)");
            return;
        }
    } else if (strcmp(j->name, "settings.save") == 0) {
        g_dirty = 1;
        a2k_settings_test_flush();
        return;
    }
    if (rc != 0)
        job_err(j, 409, "unavailable", "the motor is not running or refused the command");
}

/* ------------------------------------------------------------------ worker */
static pthread_t g_tid;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static int g_started, g_stop;
static a2k_job_t *g_job;     /* posted, not yet taken by the worker */
static int g_running, g_done; /* a job is running; the posted job finished */
static double g_tokens = A2K_SET_FLASH_BURST;
static long g_tok_ms;

static void save_now(void)
{
    char buf[STREG_FILE_MAX + 1];
    int n = streg_save_text(&g_reg, buf, sizeof buf);
    if (n < 0) {
        a2k_log("settings: cannot build the save text (%d)", n);
        return;
    }
    if (a2k_store_write(g_path, buf, (size_t)n) == 0)
        g_dirty = 0;
    else
        a2k_log("settings: cannot write %s (will retry)", g_path);
}

void a2k_settings_test_flush(void)
{
    if (g_dirty)
        save_now();
}

static void flush_tick(void)
{
    long now = mono_ms();
    if (!g_dirty)
        return;
    g_tokens += (double)(now - g_tok_ms) / 1000.0 / A2K_SET_FLASH_GAP_S;
    g_tok_ms = now;
    if (g_tokens > A2K_SET_FLASH_BURST)
        g_tokens = A2K_SET_FLASH_BURST;
    if (g_tokens >= 1.0) {
        g_tokens -= 1.0;
        save_now();
    }
}

static void changed_event(const a2k_job_t *j)
{
    char d[A2K_EVT_DATA + 1], *p = d;
    size_t i, left = sizeof d - 8;
    int overflow = 0, n = 0;
    p += snprintf(d, sizeof d, "{\"keys\":%c", '"');
    for (i = 0; i < j->in.n; i++) {
        size_t l;
        if (!j->res[i].ok)
            continue;
        n++;
        l = strlen(j->in.key[i]);
        if (l + 1 + (size_t)(p - d) >= left) {
            overflow = 1;
            continue;
        }
        if (p[-1] != '"')
            *p++ = ',';
        memcpy(p, j->in.key[i], l);
        p += l;
    }
    if (!n)
        return;
    if (overflow)
        snprintf(d, sizeof d, "{\"n\":%d}", n);
    else
        snprintf(p, sizeof d - (size_t)(p - d), "%c}", '"');
    a2k_api_event("setting", d);
}

static void do_patch(a2k_job_t *j)
{
    const char *k[JFLAT_MAX], *v[JFLAT_MAX];
    size_t i;
    for (i = 0; i < j->in.n; i++) {
        k[i] = j->in.key[i];
        v[i] = j->in.val[i];
    }
    j->err = NULL;
    j->http = streg_patch(&g_reg, j->src, j->scope, j->in.n, k, v, j->res);
    if (cfg_flush() != 0) { /* the request of the user-file rows could not be queued: those rows did not apply */
        size_t okn = 0, n;
        for (i = 0; i < j->in.n; i++) {
            if (j->res[i].ok && is_cfg_key(k[i])) {
                j->res[i].ok = 0;
                j->res[i].err = ST_E_SETTER;
            }
            okn += j->res[i].ok ? 1 : 0;
        }
        n = j->in.n;
        j->http = okn == n ? 200 : okn == 0 ? 422 : 207;
    }
    for (i = 0; i < j->in.n; i++) {
        const st_def_t *d = j->res[i].ok ? streg_find(&g_reg, k[i]) : NULL;
        if (d && row_saves(d))
            g_dirty = 1;
    }
    changed_event(j);
}

/* ---- mode.apply and a2k_settings_apply: a list of key/value pairs through the registry as ONE patch (every pair is validated before any is
 * applied: streg_patch), with the earlier setters put back if a later one fails. Runs on the worker. ---- */
#define APPLY_OLD_MAX 160
typedef struct {
    a2k_job_t job;
    char old[BC_MAX_PAIRS][APPLY_OLD_MAX];
    int have[BC_MAX_PAIRS];
} apply_ctx_t;
static apply_ctx_t g_ap_act; /* the mode.apply action (the worker) */
static apply_ctx_t g_ap_ext; /* a2k_settings_apply of another thread (the button) */
static pthread_mutex_t g_ap_mu = PTHREAD_MUTEX_INITIALIZER;

static int apply_fill(apply_ctx_t *c, size_t n, const char *const *k, const char *const *v)
{
    size_t i, off = 0;
    if (n < 1 || n > BC_MAX_PAIRS)
        return -1;
    for (i = 0; i < n; i++) {
        size_t kl = strlen(k[i]), vl = strlen(v[i]);
        if (kl > JFLAT_KEY_MAX || vl > JFLAT_VAL_MAX || off + kl + vl + 2 > JFLAT_POOL)
            return -1;
        memcpy(c->job.in.pool + off, k[i], kl + 1);
        c->job.in.key[i] = c->job.in.pool + off;
        off += kl + 1;
        memcpy(c->job.in.pool + off, v[i], vl + 1);
        c->job.in.val[i] = c->job.in.pool + off;
        off += vl + 1;
    }
    c->job.in.n = n;
    c->job.kind = A2K_JOB_APPLY;
    c->job.scope = AKEY_WRITE;
    c->job.src = ST_SRC_HTTP;
    c->job.name = NULL;
    c->job.http = 0;
    return 0;
}

static void do_apply(apply_ctx_t *c)
{
    a2k_job_t *j = &c->job;
    size_t i;
    for (i = 0; i < j->in.n; i++) {
        int n = streg_get(&g_reg, ST_SRC_HTTP, j->in.key[i], c->old[i], sizeof c->old[i]);
        c->have[i] = n >= 0;
    }
    do_patch(j);
    if (j->http == 200)
        return;
    for (i = 0; j->http == 207 && i < j->in.n; i++) { /* a setter failed after others were applied: put those back */
        st_result_t r;
        if (j->res[i].ok && c->have[i] && streg_set(&g_reg, ST_SRC_HTTP, AKEY_WRITE, j->in.key[i], c->old[i], &r) == 200)
            g_dirty = 1;
    }
    j->http = j->http == 207 ? 409 : j->http;
}

static void act_mode(a2k_job_t *j)
{
    const char *name = arg_of(&j->in, "name");
    bc_mode_t m;
    const char *k[BC_MAX_PAIRS], *v[BC_MAX_PAIRS];
    int i;
    size_t f;
    if (!name || !pp_name_ok(name)) {
        job_err(j, 422, "args", "name is a mode name");
        return;
    }
    if (a2k_btn_mode_get(name, &m) != 0) {
        job_err(j, 404, "not_found", "no such mode");
        return;
    }
    for (i = 0; i < m.n; i++) {
        k[i] = m.key[i];
        v[i] = m.val[i];
    }
    if (apply_fill(&g_ap_act, (size_t)m.n, k, v) != 0) {
        job_err(j, 422, "args", "the mode is empty or too big");
        return;
    }
    do_apply(&g_ap_act);
    if (g_ap_act.job.http == 200)
        return;
    for (f = 0; f < g_ap_act.job.in.n; f++)
        if (!g_ap_act.job.res[f].ok && g_ap_act.job.res[f].err != ST_E_NOTAPPLIED)
            break;
    j->http = g_ap_act.job.http == 409 ? 409 : 422;
    j->err = "mode_refused";
    snprintf(j->detail, sizeof j->detail, "%.30s: %s", f < g_ap_act.job.in.n ? g_ap_act.job.in.key[f] : "-",
             f < g_ap_act.job.in.n ? streg_err_name(g_ap_act.job.res[f].err) : "refused");
}

static void *worker(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&g_mu);
    while (!g_stop) {
        struct timespec ts;
        a2k_job_t *j = g_job;
        if (!j) {
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += 1;
            pthread_cond_timedwait(&g_cv, &g_mu, &ts);
            if (!g_job) {
                pthread_mutex_unlock(&g_mu);
                flush_tick();
                pthread_mutex_lock(&g_mu);
            }
            continue;
        }
        g_job = NULL;
        g_running = 1;
        pthread_mutex_unlock(&g_mu);
        if (j->kind == A2K_JOB_PATCH)
            do_patch(j);
        else if (j->kind == A2K_JOB_APPLY)
            do_apply((apply_ctx_t *)j);
        else
            run_action(j);
        pthread_mutex_lock(&g_mu);
        g_running = 0;
        g_done = 1;
        pthread_cond_broadcast(&g_cv);
    }
    pthread_mutex_unlock(&g_mu);
    return NULL;
}

int a2k_settings_idle(void)
{
    int idle;
    pthread_mutex_lock(&g_mu);
    idle = !g_job && !g_running;
    pthread_mutex_unlock(&g_mu);
    return idle;
}

int a2k_settings_exec(a2k_job_t *j)
{
    struct timespec ts;
    int rc = 0;
    pthread_mutex_lock(&g_mu);
    if (!g_started || g_job || g_running) {
        pthread_mutex_unlock(&g_mu);
        return -2;
    }
    g_job = j;
    g_done = 0;
    pthread_cond_broadcast(&g_cv);
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += A2K_SET_WAIT_MS / 1000;
    ts.tv_nsec += (A2K_SET_WAIT_MS % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
    }
    while (!g_done && rc == 0)
        if (pthread_cond_timedwait(&g_cv, &g_mu, &ts) == ETIMEDOUT)
            rc = -1;
    if (g_done)
        rc = 0;
    pthread_mutex_unlock(&g_mu);
    return rc;
}

int a2k_settings_start(const a2k_settings_cfg_t *cfg)
{
    pthread_attr_t at;
    size_t bad = 0;
    int rc;
    if (g_started)
        return 0;
    memset(&g_cfg, 0, sizeof g_cfg);
    if (cfg)
        g_cfg = *cfg;
    snprintf(g_path, sizeof g_path, "%s", g_cfg.settings_path ? g_cfg.settings_path : A2K_SET_FILE);
    snprintf(g_netdir, sizeof g_netdir, "%s", g_cfg.net_dir ? g_cfg.net_dir : A2K_SET_NET_DIR);
    snprintf(g_netreq, sizeof g_netreq, "%s", g_cfg.net_req ? g_cfg.net_req : A2K_SET_NET_REQ);
    {
        size_t l = strlen(g_netreq);
        snprintf(g_netres, sizeof g_netres, "%.*s.res", (int)(l > 4 && !strcmp(g_netreq + l - 4, ".req") ? l - 4 : l), g_netreq);
    }
    if (g_cfg.image)
        g_img = *g_cfg.image;
    else
        a2k_image_defaults(&g_img);
    a2k_netreg_set_backend(&g_netb);
    snprintf(g_cfgdir, sizeof g_cfgdir, "%s", g_cfg.cfg_dir ? g_cfg.cfg_dir : A2K_SET_CFG_DIR);
    snprintf(g_cfgreq, sizeof g_cfgreq, "%s", g_cfg.cfg_req ? g_cfg.cfg_req : A2K_SET_CFG_REQ);
    {
        size_t l = strlen(g_cfgreq);
        snprintf(g_cfgres, sizeof g_cfgres, "%.*s.res", (int)(l > 4 && !strcmp(g_cfgreq + l - 4, ".req") ? l - 4 : l), g_cfgreq);
    }
    g_cpend_n = 0;
    a2k_cfgreg_set_backend(&g_cfgb);
    g_overflow = 0;
    build_table();
    if (g_overflow) {
        a2k_log("settings: %d registry row(s) do not fit MAX_ROWS (%d): raise it; API settings disabled", g_overflow, MAX_ROWS);
        return -1;
    }
    if (streg_init(&g_reg, g_defs, g_nrows, MODEL_2K, 0, &bad) != 0) {
        a2k_log("settings: the registry table is invalid at row %lu (%s): API settings disabled", (unsigned long)bad,
                bad < g_nrows ? g_defs[bad].key : "?");
        return -1;
    }
    g_stop = 0;
    g_job = NULL;
    g_running = 0;
    g_tok_ms = mono_ms();
    g_tokens = A2K_SET_FLASH_BURST;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 32 * 1024);
    rc = pthread_create(&g_tid, &at, worker, NULL);
    pthread_attr_destroy(&at);
    if (rc != 0) {
        a2k_log("settings: cannot start the worker thread");
        return -1;
    }
    g_started = 1;
    return 0;
}

void a2k_settings_load(void)
{
    char buf[A2K_STORE_MAX + 1];
    size_t err = 0, ignored = 0, failed = 0;
    int n, rc;
    if (!g_started)
        return;
    n = a2k_store_read(g_path, buf, sizeof buf);
    if (n < 0) {
        if (n == -2)
            a2k_log("settings: %s is larger than %d bytes: ignored", g_path, A2K_STORE_MAX);
        return; /* none yet: the defaults and akam.conf apply */
    }
    rc = streg_load_text(&g_reg, buf, (size_t)n, &err, &ignored, &failed);
    if (rc != 0)
        a2k_log("settings: %s is invalid at line %lu (%s): ignored as a whole", g_path, (unsigned long)err, streg_err_name(rc));
    else
        a2k_log("settings: loaded %s (%lu key(s) ignored, %lu setter(s) failed)", g_path, (unsigned long)ignored,
                (unsigned long)failed);
}

void a2k_settings_stop(void)
{
    if (!g_started)
        return;
    pthread_mutex_lock(&g_mu);
    g_stop = 1;
    pthread_cond_broadcast(&g_cv);
    pthread_mutex_unlock(&g_mu);
    pthread_join(g_tid, NULL);
    g_started = 0;
    if (g_dirty)
        save_now();
}

/* ------------------------------------------------------------------ for the button (a2k_settings.h) */
int a2k_settings_apply(size_t n, const char *const *keys, const char *const *vals, char *detail, size_t dcap)
{
    int rc = -1, http;
    if (detail && dcap)
        detail[0] = '\0';
    pthread_mutex_lock(&g_ap_mu);
    if (g_started && a2k_settings_idle() && apply_fill(&g_ap_ext, n, keys, vals) == 0) {
        /* the job is the first member of the context: the worker casts it back */
        rc = a2k_settings_exec(&g_ap_ext.job);
        http = g_ap_ext.job.http;
        if (rc == 0 && http == 200) {
            rc = 0;
        } else {
            size_t f;
            for (f = 0; rc == 0 && f < n; f++)
                if (!g_ap_ext.job.res[f].ok && g_ap_ext.job.res[f].err != ST_E_NOTAPPLIED) {
                    if (detail && dcap)
                        snprintf(detail, dcap, "%s: %s", keys[f], streg_err_name(g_ap_ext.job.res[f].err));
                    break;
                }
            rc = -1;
        }
    }
    pthread_mutex_unlock(&g_ap_mu);
    return rc;
}

int a2k_settings_mode_key_ok(const char *key, const char *val)
{
    const st_def_t *d = streg_find(&g_reg, key);
    if (!d || !(d->access & ST_ACC_WRITE) || d->secret || d->apply != AP_LIVE || !(d->expose & ST_EXPOSE_HTTP) || d->min_scope > AKEY_WRITE)
        return -1;
    if (!strncmp(key, "net.", 4) || !strncmp(key, "api.", 4) || !strncmp(key, "mqtt.", 5))
        return -1;
    return streg_check_one(&g_reg, ST_SRC_HTTP, AKEY_WRITE, key, val) == ST_OK ? 0 : -1;
}
