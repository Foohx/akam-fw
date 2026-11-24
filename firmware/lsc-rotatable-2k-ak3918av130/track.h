/* Person tracking of the Rotatable 2K own app: the wiring between the detection ticks (~5 Hz, boxes in the 320x192 detector frame),
 * the pure policy track_logic.h and the motor (ptz.h), plus the tracking AREAS. The core (this file's .c) calls the motor only
 * through the ops table, so tests/rot2k_track_wire_unit.c runs it on a fake motor; track_run.c binds the real one.
 *
 * Tracking is OFF by default (akam.conf track=1, or the Home Assistant switch "Person tracking"). It never runs while the camera is
 * parked, calibrating, its position is unknown, the privacy mode hides the video, or the position is outside every enabled area.
 * Every detection tick feeds the policy (with n = 0 when nobody is seen); TL_NUDGE becomes a2k_ptz_nudge, TL_HOME the home command.
 * Signs: a target right of the centre -> pan right (dx > 0); a target BELOW the centre (dtilt > 0) -> tilt DOWN (dy < 0): the motor
 * convention of hal_ptz.h is pan + = right, tilt + = up.
 *
 * AREAS (motor coordinates): up to A2K_TAREA_MAX named rectangles pan_min..pan_max x tilt_min..tilt_max inside the soft range, each
 * enabled or disabled. No complete area = the whole soft range. With areas: tracking is allowed only while the current position lies
 * inside at least one ENABLED complete area (all disabled = never), and a nudge is clipped to the enabled area that contains the
 * current position (when several overlap, the one that lets the nudge go the furthest). An area is made by name, then its two corners are
 * set to the current position of the camera (A and B, any order); it is "incomplete" (ignored) until both are set.
 * Persisted in /config/akam/track_areas (atomic, see store.h), lines:
 *   track_area_<name>=PMIN:PMAX:TMIN:TMAX[:0|1]      complete, optional enabled flag (default 1)
 *   track_area_<name>=none[:0|1]                      incomplete (the corners are not saved) */
#ifndef AKAM2K_TRACK_H
#define AKAM2K_TRACK_H
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

#include "ptz.h"
#include "zones.h"
#include "track_logic.h"

#define A2K_TAREA_MAX 4
#define A2K_TAREA_NAME_MAX A2K_ZONE_NAME_MAX

struct a2k_tarea {
    char name[A2K_TAREA_NAME_MAX + 1];
    int enabled;
    int complete;
    int pmin, pmax, tmin, tmax;   /* valid when complete */
    int has_a, has_b, a_pan, a_tilt, b_pan, b_tilt; /* the corners being set (RAM only) */
};

typedef struct {
    int n;
    struct a2k_tarea a[A2K_TAREA_MAX];
} a2k_tareas_t;

/* ---- pure area logic ---- */
/* Strict parse of the persisted text (LF lines). 0, or -1 (invalid: *out untouched). */
int a2k_tareas_parse(const char *text, size_t len, a2k_tareas_t *out);
/* The persisted text. Length, or -1 when it does not fit. */
int a2k_tareas_format(const a2k_tareas_t *t, char *out, size_t cap);
/* Name rule = the zone names (a-z first, a-z 0-9 _, 1..16). */
int a2k_tarea_name_ok(const char *s);
/* 1 when tracking is allowed at this position (no complete area, or inside an enabled complete one). */
int a2k_tareas_allowed(const a2k_tareas_t *t, int pan, int tilt);
/* Clips the nudge (dx right+, dy up+) from (pan, tilt) to the enabled area containing the position. 1 and *cx / *cy filled (maybe 0, 0),
 * 0 when the position is in no enabled area (and areas exist). No complete area: the nudge is returned as is. */
int a2k_tareas_clip(const a2k_tareas_t *t, int pan, int tilt, int dx, int dy, int *cx, int *cy);
/* 1 when every complete area lies inside the soft range r = {pan_min, pan_max, tilt_min, tilt_max}. */
int a2k_tareas_fit(const a2k_tareas_t *t, const int r[4]);

/* ---- the wiring core ---- */
typedef struct {
    void (*info)(a2k_ptz_info_t *out);
    int (*range)(int r[4]);          /* soft range: pan_min, pan_max, tilt_min, tilt_max; 0 ok, -1 unknown */
    int (*nudge)(int dx, int dy, int speed); /* relative steps (right / up positive); speed divisor 2..20, 0 = the default */
    int (*home)(void);
    int (*privacy)(void);            /* 1: the video is hidden */
    int (*tilt_home)(void);          /* the home tilt in motor steps, 0 = unknown; may be NULL */
    /* PTZ patrol hook, may be NULL: called on every tick that sees a person (n > 0), tracking enabled or not. tracking = 1 when the tracker acts on it (enabled, allowed
     * here, not parked / calibrating / private): the patrol pauses. resume_ms = lost-person grace + return delay: the patrol resumes after that long without a person. */
    void (*person)(int tracking, int resume_ms);
    int (*home_hold)(void);          /* may be NULL: 1 = the head must NOT be sent home now (a patrol runs or waits to resume: it resumes at the interrupted step instead) */
    /* Continuous motion (both or none; NULL = the tracker uses nudges, track_motion=steps): vel(sx, sy, dx, dy) keeps ONE ongoing directional move, refreshed every tick (the
     * motor stops by itself when no refresh arrives: the lease): sx right+ / sy up+ in {-1,0,1}, dx / dy the speed divisors 2..20; all zero = stop. Returns 0, or -1 when refused (another
     * command runs). pos_at(t_ms, &pan, &tilt): the driver position at that time (CLOCK_MONOTONIC ms) from the polled counters, 0 known, -1 unknown. */
    int (*vel)(int sx, int sy, int dx, int dy);
    int (*pos_at)(int64_t t_ms, int *pan, int *tilt);
    /* The lost-target chain (all optional, NULL = absent): route_start(name) starts a stored patrol route (0 ok), route_stop() stops the one the chain started, route_busy() = 1 while a route runs,
     * lost_event(step) is told every change of the current step ("" = tracking again), never blocks. */
    int (*route_start)(const char *name);
    int (*route_stop)(void);
    int (*route_busy)(void);
    void (*lost_event)(const char *step);
    int (*over_budget)(void); /* 1 when the steps travelled since the last calibration exceed the travel budget: the lost chain skips its local sweep */
} a2k_track_ops_t;

typedef struct {
    a2k_track_ops_t ops;
    struct tl_cfg cfg;
    tl_t tl;
    pthread_mutex_t mu;
    int enabled, active;
    a2k_tareas_t areas;
    char path[160];                  /* the areas file, "" = not persisted */
    void (*notify)(void);            /* after an edit of the areas / the switch (cheap, never blocks on the network) */
    unsigned nudges, homes, vels;
    char lost_last[40];              /* the last lost step announced (change detection) */
    int debug;
    int64_t dbg_last_ms;
    const char *dbg_why;             /* the reason of the last logged tick (the continuous log skips repeated idle ticks) */
} a2k_track_t;

void a2k_track_init(a2k_track_t *t, const a2k_track_ops_t *ops, const char *store_path);
void a2k_track_destroy(a2k_track_t *t);
/* conf: the akam.conf areas ("track_area_<name>=..." LF lines, may be NULL). The stored file, when present, wins (even empty). Areas that
 * do not fit the soft range are dropped with a log line. Returns the number of areas. */
int a2k_track_load(a2k_track_t *t, const char *conf);
void a2k_track_set_enabled(a2k_track_t *t, int on);
/* The return policy (live): home = 1 / stay = 0, and the delay in seconds after the lost-person grace before going home. */
void a2k_track_set_return(a2k_track_t *t, int home, int delay_s);
void a2k_track_get_return(a2k_track_t *t, int *home, int *delay_s);
/* The tuning (init time, before the detection runs): replaces the policy config (areas and the switch are kept). -1 when invalid. */
int a2k_track_set_cfg(a2k_track_t *t, const struct tl_cfg *c);
/* Live tuning (API track.band_pct 2..40 = pan band, track.head_pct 0..50 = the tilt head row), applied on the running policy without resetting its state. */
/* Live tilt/pan gains (API track.gain_pan / track.gain_tilt 200..4000 steps per frame width / height, track.tilt_band_pct 2..40), same rules. */
/* Live video latency of the pose-at-capture lookup (API track.lag_ms -150..300, default 50; calibrate with tools/track_lagfit, see tracking.md). */
void a2k_track_set_lag(a2k_track_t *t, int lag_ms);
int a2k_track_get_lag(a2k_track_t *t);
void a2k_track_get_gains(a2k_track_t *t, int *gain_pan, int *gain_tilt, int *tilt_band_pct);
int a2k_track_set_invert(a2k_track_t *t, int invert_pan, int invert_tilt); /* live motor direction flags (the base combined with the image orientation), -1 for a value other than 0 / 1 */
int a2k_track_set_tilt(a2k_track_t *t, int on); /* track.tilt 0/1 live (0 = pan only), -1 for another value */
int a2k_track_get_tilt(a2k_track_t *t);
void a2k_track_set_gains(a2k_track_t *t, int gain_pan, int gain_tilt, int tilt_band_pct);
/* Live comfort zones (API track.zone_pct 0..90 = the horizontal zone width in % of the frame width, 0 = the dev targeting; track.zone_y_pct = body rule vertical band; track.zone_head_pct = head rule band). */
void a2k_track_get_zones(a2k_track_t *t, int *x_pct, int *y_pct, int *head_pct);
void a2k_track_set_zones(a2k_track_t *t, int x_pct, int y_pct, int head_pct);
/* Live motion mode (API track.motion): TL_MOTION_CONT or TL_MOTION_STEPS (the fallback, also used when the motor layer has no continuous mode). */
int a2k_track_get_motion(a2k_track_t *t);
/* The lost-target chain (API track.lost_policy, conf track_lost_policy; syntax in track_logic.h tl_lost_parse). set: all or nothing, 0 or -1 (nothing changed). */
int a2k_track_set_lost_policy(a2k_track_t *t, const char *text);
void a2k_track_get_lost_policy(a2k_track_t *t, char *out, size_t cap);
/* The current step for /state (track.lost_step): "idle" while the person is followed or nobody was lost, else backsweep | exit | hold | local | route:<name> | home | stay | done. */
void a2k_track_get_lost_step(a2k_track_t *t, char *out, size_t cap);
void a2k_track_set_motion(a2k_track_t *t, int motion);
void a2k_track_get_tune(a2k_track_t *t, int *band_pct, int *head_pct);
void a2k_track_set_tune(a2k_track_t *t, int band_pct, int head_pct);
/* Opt-in decision log (akam.conf track_debug=1; 2 adds the raw recorder below): one line per decision to a RAM file (/tmp/akam2k_track.log, capped, 2 lines/s at most). */
void a2k_track_set_debug(a2k_track_t *t, int on);
/* track_debug=2: the raw recorder, /tmp/akam2k_track.raw (RAM, rolling, 1 MB): C (config), I (every tl_update input), D (the decision line), R t pan tilt (every position-ring push of a
 * continuous session: wire a2k_track_rec_ring to a2k_ptz_set_ring_hook). Input of tests/rot2k_track_replay.c and tools/track_lagfit. */
void a2k_track_rec_ring(int64_t t_ms, int pan, int tilt);
/* Live tilt return (API track.tilt_return_s 0..3600): stay mode, seconds after the lost-person grace before the tilt goes back home; 0 = never. */
void a2k_track_get_tilt_return(a2k_track_t *t, int *s);
/* Live: continuous mode, seconds after the person was lost with the tilt more than 250 steps above home before the tilt goes back to the home tilt (API track.tilt_back_s 0..600, 5; 0 = off). */
void a2k_track_get_tilt_back(a2k_track_t *t, int *s);
/* Live: continuous mode, the close-loss vertical scan (API track.close_scan 0|1 default 1, track.close_scan_up 0..600 default 250, track.close_scan_down 0..600 default 300): which 0 = close_scan, 1 = up, 2 = down.
 * set returns -1 for a value out of range. */
int a2k_track_set_close_scan(a2k_track_t *t, int which, int v);
int a2k_track_get_close_scan(a2k_track_t *t, int which);
void a2k_track_set_tilt_back(a2k_track_t *t, int s);
void a2k_track_set_tilt_return(a2k_track_t *t, int s);
int a2k_track_enabled(a2k_track_t *t);
/* One detection tick; n = 0 when nobody is seen. Boxes in the detector frame fw x fh. cap_ms: when the frame was captured (same clock as
 * now_ms; 0 = unknown): frames captured while the motor ran are dropped (a2k_ptz_info_t.motion_end_ms). */
void a2k_track_tick_ts(a2k_track_t *t, int64_t now_ms, int64_t cap_ms, const struct tl_box *boxes, int n, int fw, int fh);
void a2k_track_tick(a2k_track_t *t, int64_t now_ms, const struct tl_box *boxes, int n, int fw, int fh);
void a2k_track_areas(a2k_track_t *t, a2k_tareas_t *out);
/* Area editing; 0 ok, -1 refused (full, bad or duplicate name, no such slot, position unknown, degenerate area). Each ok edit is persisted. */
int a2k_track_area_new(a2k_track_t *t, const char *name);
/* corner 0 = A, 1 = B: stores the CURRENT position of the camera; with both set the area is complete (min / max of the two). */
int a2k_track_area_corner(a2k_track_t *t, int slot, int corner);
int a2k_track_area_enable(a2k_track_t *t, int slot, int on);
int a2k_track_area_delete(a2k_track_t *t, int slot);
/* Text of the areas for display: "name=PMIN:PMAX:TMIN:TMAX[:off]" / "name=incomplete" joined by ';', "none" without any. */
void a2k_track_areas_text(a2k_track_t *t, char *out, size_t cap);

#endif
