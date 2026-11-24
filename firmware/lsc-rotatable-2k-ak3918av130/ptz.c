/* PTZ controller of the Rotatable 2K own app (ptz.h): one thread, one pending command, hal_ptz underneath. */
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "hal.h"
#include "log.h"
#include "orient.h"
#include "ptz.h"
#include "ptz_lease.h"

enum { K_NONE, K_STEP, K_HOME, K_PARK, K_NUDGE, K_RECAL, K_GOTO, K_VEL, K_REHOME };
#define RING_N 64 /* position samples of a continuous session (about 3 s at 20 Hz) */


static struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    pthread_t tid;
    int running, stop, starting;
    struct { int kind, a, b, sp, patrol; unsigned ticket; int auto_home; } pend; /* auto_home: the K_HOME is the controller's own return home (it gives way to a tracker command) */
    int cur_patrol;             /* the running command is a patrol move */
    int cur_auto;               /* the running K_HOME is the controller's own return home */
    unsigned cur_ticket, pt_next, pt_done;  /* patrol move tickets: running, last issued, last finished */
    int pt_rc;                  /* HAL_PTZ_* of the last finished patrol move */
    void (*cmd_hook)(int kind); /* a manual command was accepted (atomic stores only: called under the lock) */
    int64_t end_ms;             /* CLOCK_MONOTONIC ms of the last motion end */
    a2k_ptz_info_t info;
    int cal_mode, privacy_opt;
    hal_ptz_t *hp;
    hal_ptz_ops_t ops;
    int custom_ops;
    char conf_path[96];
    unsigned scene_ms, scene_confirm_ms, scene_gap_ms;
    a2k_ptz_scene_t scene;
    int scene_on;          /* ptz_scene_check: boot check + saves (the drop always runs) */
    int drift_ref;         /* ptz_drift_ref: the drift check (patch reference) may run; 0 = no capture, no correction, no drift recalibration */
    int drift_mode;        /* ptz_drift_correct: A2K_PTZ_DC_* */
    int boot_always;       /* ptz_boot_calibrate=always: the scene proof never skips the boot calibration */
    int scene_dirty;       /* a calibration ran in this process: the home reference is due (checked, written only if needed) once the head is at home and quiet */
    int scene_tries;
    int scene_cand;        /* a candidate view was captured (waiting for the confirmation) */
    int scene_fresh;       /* a calibration ran since the last reference write */
    int stop_commit;       /* a2k_ptz_stop asked the thread to write the reference before it ends */
    int64_t cand_ms;
    int busy;              /* the thread runs a command */
    int mv_req;            /* the rest view is featureless: build the multi-view reference (offset views) when quiet */
    int mv_failed;         /* multi-view builds that really failed in this run (no usable view, a failed move, a write error): 3 at most */
    int mv_built;          /* a multi-view reference was written in this run: rebuilt only after a calibration (fresh) */
    int64_t last_nudge_ms; /* the last tracking nudge: no excursion while a tracker is active */
    unsigned long travel_budget;
    unsigned scene_writes;
    int budget_logged;
    int64_t last_end_ms, last_save_ms;
    void (*notify)(const a2k_ptz_info_t *);
    /* continuous motion */
    int vending;                /* the session is ending: a2k_ptz_vel is refused (-1) until it is cleared */
    int vsess;                  /* a session runs (a2k_ptz_vel updates the mailbox instead of queueing) */
    struct { int sx, sy, dx, dy; } vel;
    unsigned vel_seq;           /* bumped by every a2k_ptz_vel (a refresh included) */
    ptzl_t dm;                  /* the real-time dead-man of the current velocity commands (armed by every command, a repeat included) */
    int lease_ms;
    int vel_lease;              /* the real-time lease of the CURRENT velocity commands (the tracker's lease_ms, or a manual command's own) */
    int vel_man;                /* the current velocity commands are MANUAL (a2k_ptz_vel_manual): the safe layer's polled lease is refreshed while the real-time one holds */
    ptzl_t man;                 /* the manual velocity HOLD (armed by every manual command): while live, tracking commands are refused (manual wins) */
    int cur_kind;               /* K_* of the command the thread runs (K_NONE: none) */
    struct { int64_t t; int pan, tilt; } ring[RING_N];
    int ring_n, ring_head;      /* count, index of the next write */
    /* drift: the scene-based correction of lost steps */
    int drift_due;              /* a drift check is due at drift_at (the head returned home) */
    int64_t drift_at;
    unsigned drift_tries;
    unsigned long drift_mark;   /* hal travel at the last check / calibration */
    int drift_untrusted;        /* the counters are doubted (a recalibration is due or running): no scene reference writes */
    int drift_corrected;        /* the counters were corrected in this run: no scene reference write until a calibration */
    int recal_pending;          /* a drift-triggered recalibration waits for an idle period */
    int drift_busy;             /* the drift SHIFT window runs (re-declare + move home + re-verify): manual commands are refused (NOTREADY) and a manual Stop does not cancel it */
    int64_t last_manual_ms;     /* the end of the last manual hold (the last manual command + its lease, or the Stop): counts as motion activity for the idle timers of the drift machine */
    int corr_unconfirmed;       /* a SHIFT correction whose re-verify was inconclusive / skipped is not confirmed yet: a further SHIFT verdict schedules a recalibration instead of a second declare (cleared by a re-verify ok or a calibration) */
    long cum_steps[2];          /* signed sum of the steps corrected (declared) per axis since the last calibration: beyond +-cum_max a recalibration replaces the next declare */
    unsigned d_cum_max, d_retry_ms;
    unsigned drift_recals;
    int64_t drift_recal_ms;
    int drift_have, drift_dx, drift_dy;
    unsigned drift_corrections, drift_checks;
    char recal_reason[72];
    int64_t last_person_ms;
    unsigned d_idle_ms, d_frame_ms, d_min_travel, d_recal_idle_ms;
    /* tilt-only re-home */
    int rh_mode;                /* A2K_PTZ_RH_* */
    int rh_due;                 /* the head returned home and enough was travelled: a re-home is due once quiet */
    int64_t rh_last;            /* start of the last attempt (0: none in this run) */
    unsigned long rh_mark;      /* hal travel at the last re-home / calibration */
    unsigned rh_gap_ms, rh_gap_ov, rh_idle_ms, rh_min_travel; /* rh_gap_ov: a test override of the gap (ms), survives a start */
    unsigned rh_runs, rh_cancels;
    int rh_cur_urgent;          /* the running/last attempt was started by a flat home view (flat) rather than the travel gap (gap) */
    int64_t rh_done_ms;         /* mono_ms() of the last completed re-home (0: none) */
    int rh_done_urgent;
    int rh_urgent;              /* a flat home view where the reference had detail: the re-home (or a recalibration) is wanted now, the normal gap does not apply */
    int64_t rh_urg_last;        /* start of the last urgent re-home (0: none) */
    char drift_last[16];        /* verdict of the last drift check (RAM) */
    unsigned inconc_runs;       /* consecutive returns home whose checks all gave up inconclusive (nothing was proven either way) */
    int64_t drift_last_ms;      /* when the picture was last looked at (any verdict) or the position last proven: the idle timer of the periodic check counts from it */
    unsigned d_periodic_ms, d_verify_travel; /* periodic check at home (0 = default); travel since the last look beyond which the controller goes home to verify (0 = default) */
    unsigned long verify_mark;  /* hal travel at the last look at the picture / calibration */
    int64_t ok_ms;              /* when the picture last said ok (0: not in this run); the tilt was proven by it */
    unsigned long ok_travel;    /* hal travel at that moment */
    int dref_due;               /* a drift reference (RAM) is wanted: captured once the head is quiet at home at a proven position (own rule, not the flash reference's) */
    unsigned dref_tries;
    int64_t dref_retry_at, dref_ms; /* next try after a failed capture; when the last one succeeded (0 = none in this run) */
    unsigned dref_refresh_ms;
    int rung;                   /* escalation ladder: 0 none, 1 an urgent tilt re-home was asked for by a doubt (the check waits for it), 2 it completed and the check that follows decides: ok -> trusted again, still wrong -> recalibration */
    int drift_force;            /* the next return home / re-home completion checks the picture whatever was travelled */
    int64_t recal_hist[A2K_PTZ_RECAL_MAX]; /* start times of the drift recalibrations of the sliding window (0 = unused slot) */
    int64_t recal_not_before;   /* a held-back recalibration runs from this time on (the window, or the retry after a failure) */
    int rh_exact;               /* a re-home completed (the tilt is exact by construction) and no conclusive picture verdict (ok / shift) has been seen since: a flat view now is not a slip */
    int rh_owed;                /* a re-home is wanted (a cancelled / failed attempt, a flat view): survives a cancel, cleared by a completed re-home or a calibration */
    int go_home;                /* the head was left off home by an interrupted re-home: the controller returns it home once quiet (a tracker command preempts it) */
    int64_t rh_retry_at, rh_last_prev, rh_urg_last_prev;
    unsigned rh_cancel_streak;  /* consecutive cancelled attempts (the retry gap grows to the normal gap after A2K_PTZ_REHOME_RETRY_MAX) */
    unsigned urg_gap_ms, rh_retry_ms, recal_win_ms, recal_retry_ms; /* tunables of the escalation ladder (test overrides, 0 = the defaults) */
} G = { .mu = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER,
        .conf_path = A2K_PTZ_CONF, .lease_ms = A2K_PTZ_LEASE_MS, .vel_lease = A2K_PTZ_LEASE_MS,
        .scene_ms = A2K_PTZ_SCENE_MS, .scene_confirm_ms = A2K_PTZ_SCENE_CONFIRM_MS, .scene_gap_ms = A2K_PTZ_SCENE_GAP_MS,
        .d_idle_ms = A2K_PTZ_DRIFT_IDLE_MS, .d_frame_ms = A2K_PTZ_DRIFT_FRAME_MS, .d_min_travel = A2K_PTZ_DRIFT_MIN_TRAVEL,   .d_recal_idle_ms = A2K_PTZ_RECAL_IDLE_MS, .d_cum_max = A2K_PTZ_DRIFT_CUM_MAX, .d_retry_ms = A2K_PTZ_DRIFT_RETRY_MS, .recal_reason = "none", .drift_last = "none",
        .rh_idle_ms = A2K_PTZ_REHOME_IDLE_MS, .rh_min_travel = A2K_PTZ_REHOME_MIN_TRAVEL, .rh_gap_ms = A2K_PTZ_REHOME_GAP_MIN * 60000u };
static volatile int g_privacy;
static int g_hook_abort; /* atomic: set by any thread, read by the hook */ /* a person / a tracker command arrived: a scene hook in progress must return, its verdict is void */
static int hook_abort_fn(void) { return __atomic_load_n(&g_hook_abort, __ATOMIC_SEQ_CST); }

void a2k_ptz_defaults(a2k_ptz_cfg_t *c)
{
    c->cal = HAL_PTZ_CAL_AUTO;
    c->park_pan = c->park_tilt = -1;
    c->privacy = 0;
    c->scene_check = 1;
    c->drift_ref = 1;
    c->drift_correct = A2K_PTZ_DC_FULL;
    c->boot_always = 0;
    c->tilt_rehome = A2K_PTZ_RH_IDLE;
    c->rehome_gap_min = A2K_PTZ_REHOME_GAP_MIN;
    c->travel_budget = A2K_PTZ_TRAVEL_BUDGET;
}

int a2k_ptz_parse_drift_correct(const char *s)
{
    return !strcmp(s, "off") ? A2K_PTZ_DC_OFF : !strcmp(s, "adjust") ? A2K_PTZ_DC_ADJUST : !strcmp(s, "full") ? A2K_PTZ_DC_FULL : -1;
}

int a2k_ptz_parse_tilt_rehome(const char *s)
{
    return !strcmp(s, "off") ? A2K_PTZ_RH_OFF : !strcmp(s, "idle") ? A2K_PTZ_RH_IDLE : -1;
}

int a2k_ptz_parse_boot_calibrate(const char *s)
{
    return !strcmp(s, "auto") ? 0 : !strcmp(s, "always") ? 1 : -1;
}

int a2k_ptz_parse_cal(const char *s)
{
    return !strcmp(s, "auto") ? HAL_PTZ_CAL_AUTO : !strcmp(s, "off") ? HAL_PTZ_CAL_OFF : -1;
}

int a2k_ptz_parse_park(const char *s, int *pan, int *tilt)
{
    long v[2] = { 0, 0 };
    int i, n;
    for (i = 0; i < 2; i++) {
        for (n = 0; *s >= '0' && *s <= '9'; s++, n++)
            if ((v[i] = v[i] * 10 + (*s - '0')) > 65535)
                return -1;
        if (!n || (i == 0 ? *s++ != ':' : *s != '\0'))
            return -1;
    }
    *pan = (int)v[0];
    *tilt = (int)v[1];
    return 0;
}

const char *a2k_ptz_state_name(int state)
{
    switch (state) {
    case HAL_PTZ_HOME: return "home";
    case HAL_PTZ_PARKED: return "parked";
    case HAL_PTZ_MOVED: return "moved";
    case HAL_PTZ_CALIBRATING: return "calibrating";
    default: return "unknown";
    }
}

static void log_line(const char *line) { a2k_log("%s", line); }

static int64_t mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void a2k_ptz_set_scene(const a2k_ptz_scene_t *s)
{
    memset(&G.scene, 0, sizeof G.scene);
    if (s)
        G.scene = *s;
    if (G.scene.set_abort)
        G.scene.set_abort(hook_abort_fn);
}

void a2k_ptz_test_set_scene_timing(unsigned scene_ms, unsigned confirm_ms, unsigned gap_ms)
{
    G.scene_ms = scene_ms ? scene_ms : A2K_PTZ_SCENE_MS;
    G.scene_confirm_ms = confirm_ms ? confirm_ms : A2K_PTZ_SCENE_CONFIRM_MS;
    G.scene_gap_ms = gap_ms;
}

static int stopping(void) { return __atomic_load_n(&G.stop, __ATOMIC_SEQ_CST); }

/* hal_ptz ops: the scene check before the first motor command (the claim is HOME), and the rare drop of the reference */
static int hook_cold_check(uint32_t *pan, uint32_t *tilt)
{
    int rc;
    if (!G.scene_on || !G.scene.boot_check)
        return 0;
    if (G.boot_always) { /* ptz_boot_calibrate=always: the saved view is not even looked at, the motor calibrates */
        a2k_log("ptz: ptz_boot_calibrate=always: the scene proof is not used, calibrating");
        return 0;
    }
    rc = G.scene.boot_check(pan, tilt, stopping);
    return rc == 1 ? 1 : rc == 3 ? 3 : rc == 2 && G.scene.probe_offset && G.scene.probe ? 2 : 0;
}

static int hook_cold_offset(unsigned i, int *dpan, int *dtilt) { return G.scene.probe_offset ? G.scene.probe_offset(i, dpan, dtilt) : -1; }

/* 1: the live view at the offset matches the stored one (home is proven) */
static int hook_cold_probe(unsigned i)
{
    int rc = stopping() ? -1 : G.scene.probe ? G.scene.probe(i) : 0;
    return stopping() && rc != 1 ? -1 : rc;
}

/* hal_ptz: the reference must not be used any more (a calibration failed, a claimed position was lost). Rare, best effort. 0 ok, -1 not done. */
static int hook_drop(void) { return G.scene.drop ? G.scene.drop() : 0; }

/* Publish the state and the privacy flag after a change; `moving` forces "moved" with no position (the camera leaves home /
 * park at once, so the privacy ends with the first motion). */
/* The position history of the controller (G.mu held): samples of a session and the position at the end of every motion. */
static void ring_reset_locked(void) { G.ring_n = G.ring_head = 0; }
static void (*volatile g_ring_hook)(int64_t t, int pan, int tilt);
void a2k_ptz_set_ring_hook(void (*cb)(int64_t t, int pan, int tilt)) { g_ring_hook = cb; }
static void ring_push_locked(int64_t t, int pan, int tilt)
{
    if (G.vsess && g_ring_hook)
        g_ring_hook(t, pan, tilt); /* the raw recorder of the tracker (track_debug=2): every sample of a continuous session */
    G.ring[G.ring_head].t = t;
    G.ring[G.ring_head].pan = pan;
    G.ring[G.ring_head].tilt = tilt;
    G.ring_head = (G.ring_head + 1) % RING_N;
    if (G.ring_n < RING_N)
        G.ring_n++;
}

int a2k_ptz_pos_at(int64_t t, int *pan, int *tilt)
{
    int rc = -1, n, i, ia = -1, ib = -1;
    pthread_mutex_lock(&G.mu);
    n = G.ring_n;
    if (G.running && n > 0) {
        int newest = (G.ring_head + RING_N - 1) % RING_N, oldest = (G.ring_head + RING_N - n) % RING_N;
        if (t >= G.ring[newest].t) { /* after the last sample: the head is where it was (idle) or just there (a session samples every 50 ms) */
            if (!G.vsess || t - G.ring[newest].t <= 200) {
                *pan = G.ring[newest].pan;
                *tilt = G.ring[newest].tilt;
                rc = 0;
            }
        } else if (t >= G.ring[oldest].t) {
            for (i = 0; i < n; i++) { /* oldest -> newest: the last sample at or before t, and the first after it */
                int k = (oldest + i) % RING_N;
                if (G.ring[k].t <= t) ia = k;
                else { ib = k; break; }
            }
            if (ia >= 0 && ib >= 0) {
                int64_t span = G.ring[ib].t - G.ring[ia].t;
                int dp = G.ring[ib].pan - G.ring[ia].pan, dt = G.ring[ib].tilt - G.ring[ia].tilt;
                if (span <= 0 || span <= 120) { /* samples of a session: linear */
                    int64_t f = span > 0 ? t - G.ring[ia].t : 0;
                    *pan = G.ring[ia].pan + (span > 0 ? (int)(dp * f / span) : 0);
                    *tilt = G.ring[ia].tilt + (span > 0 ? (int)(dt * f / span) : 0);
                    rc = 0;
                } else if (abs(dp) <= 2 && abs(dt) <= 2) { /* a long gap with the same position: it did not move */
                    *pan = G.ring[ia].pan;
                    *tilt = G.ring[ia].tilt;
                    rc = 0;
                }
            }
        }
    }
    pthread_mutex_unlock(&G.mu);
    return rc;
}

void a2k_ptz_emergency_stop(void) { hal_ptz_emergency_stop(); }

int a2k_ptz_oneshot_stop(void) { return hal_ptz_oneshot_stop(hal_ptz_linux_ops(a2k_ptz_desc.dev)); }

void a2k_ptz_set_lease_ms(int ms)
{
    pthread_mutex_lock(&G.mu);
    G.lease_ms = ms >= 150 && ms <= 2000 ? ms : A2K_PTZ_LEASE_MS;
    pthread_mutex_unlock(&G.mu);
}

static void publish(int state, int moving)
{
    a2k_ptz_info_t i;
    void (*fn)(const a2k_ptz_info_t *);
    memset(&i, 0, sizeof i);
    i.state = state;
    if (!moving)
        G.end_ms = mono_ms();
    i.motion_end_ms = G.end_ms;
    if (!moving && G.hp && hal_ptz_pos(G.hp, &i.pan, &i.tilt))
        i.known = 1;
    pthread_mutex_lock(&G.mu);
    G.info = i;
    if (!moving && i.known)
        ring_push_locked(G.end_ms, (int)i.pan, (int)i.tilt); /* the position at the end of a motion: later instants resolve to it */
    g_privacy = G.privacy_opt && state == HAL_PTZ_PARKED;
    fn = G.notify;
    pthread_mutex_unlock(&G.mu);
    if (fn)
        fn(&i);
}

/* The continuous session (PTZ thread, G.mu NOT held): the mailbox of a2k_ptz_vel is polled after every position poll (~50 ms). */
struct vctx { unsigned seen; int published; };

static int vel_cb(void *ctx, const hal_ptz_cstate_t *st, hal_ptz_cmd_t *cmd)
{
    struct vctx *v = ctx;
    int64_t now = mono_ms();
    int k = HAL_PTZ_CONT_KEEP;
    if (!v->published) { /* the session really runs: now the state says moved (position unknown while it runs) */
        v->published = 1;
        publish(HAL_PTZ_MOVED, 1);
    }
    pthread_mutex_lock(&G.mu);
    ring_push_locked(now, (int)st->pan, (int)st->tilt);
    if (G.stop || G.pend.kind != K_NONE) { /* a manual command / the shutdown is waiting: it also cancelled the motion */
        k = HAL_PTZ_CONT_STOP;
        G.vending = 1;
    } else if (G.vel_seq != v->seen) {
        v->seen = G.vel_seq;
        cmd->sx = G.vel.sx;
        cmd->sy = G.vel.sy;
        cmd->dx = G.vel.dx;
        cmd->dy = G.vel.dy;
        k = cmd->sx || cmd->sy ? HAL_PTZ_CONT_CMD : HAL_PTZ_CONT_STOP;
        if (k == HAL_PTZ_CONT_STOP)
            G.vending = 1;
    } else if (ptzl_expired(&G.dm, now)) { /* the real-time dead-man (the safe layer has its own, counted in polls) */
        k = HAL_PTZ_CONT_STOP;
        G.vending = 1;
    } else if (G.vel_man) { /* a manual hold repeats its command at its own pace (up to 2 s): the same command again only restarts the polled lease of the safe layer, nothing is sent to the driver */
        cmd->sx = G.vel.sx;
        cmd->sy = G.vel.sy;
        cmd->dx = G.vel.dx;
        cmd->dy = G.vel.dy;
        k = HAL_PTZ_CONT_CMD;
    }
    pthread_mutex_unlock(&G.mu);
    return k;
}

static int run_session(int a, int b)
{
    struct vctx v;
    hal_ptz_cmd_t first;
    unsigned lease;
    int rc;
    int64_t now = mono_ms();
    uint32_t pan, tilt;
    pthread_mutex_lock(&G.mu);
    first.sx = a / 4 - 1; /* packed by a2k_ptz_vel: (sx + 1) * 4 + (sy + 1) */
    first.sy = a % 4 - 1;
    first.dx = b / 32;
    first.dy = b % 32;
    v.seen = G.vel_seq;
    v.published = 0;
    G.vending = 0;
    ptzl_arm(&G.dm, now, G.vel_lease);
    G.vel.sx = first.sx; /* the mailbox starts with the first command (the manual refresh above repeats it) */
    G.vel.sy = first.sy;
    G.vel.dx = first.dx;
    G.vel.dy = first.dy;
    lease = (unsigned)G.vel_lease + 150; /* the polled-time lease of the safe layer: a little longer than the real-time one above */
    if (lease > 2000)
        lease = 2000; /* PTZS_LEASE_MAX */
    pthread_mutex_unlock(&G.mu);
    if (hal_ptz_pos(G.hp, &pan, &tilt)) { /* the position before the first poll */
        pthread_mutex_lock(&G.mu);
        ring_push_locked(now, (int)pan, (int)tilt);
        pthread_mutex_unlock(&G.mu);
    }
    rc = hal_ptz_cont_run(G.hp, &first, lease, vel_cb, &v);
    pthread_mutex_lock(&G.mu);
    G.vending = 1; /* no more mailbox updates: a2k_ptz_vel is refused until the session is cleared (the tracker asks again next tick) */
    if (rc == HAL_PTZ_OK && !G.stop && G.pend.kind == K_NONE && G.vel_seq != v.seen && (G.vel.sx || G.vel.sy)) { /* one that slipped in at the very end starts a new session */
        G.pend.kind = K_VEL;
        G.pend.a = (G.vel.sx + 1) * 4 + (G.vel.sy + 1);
        G.pend.b = G.vel.dx * 32 + G.vel.dy;
        G.pend.sp = 0;
        G.pend.patrol = 0;
    }
    pthread_mutex_unlock(&G.mu);
    return rc;
}

static int run_cmd(int kind, int a, int b, int sp, int quiet)
{
    static const char *const names[] = { "none", "step", "home", "park", "nudge", "recalibrate", "goto", "velocity", "tilt re-home" };
    static const char *const dirs[] = { " up", " right", " down", " left" };
    int rc = HAL_PTZ_EREFUSED;
    a2k_ptz_info_t i;
    if (kind != K_VEL)
        publish(kind == K_RECAL ? HAL_PTZ_CALIBRATING : HAL_PTZ_MOVED, 1); /* a session publishes at its first poll (vel_cb): a refused one leaves no trace */
    switch (kind) {
    case K_RECAL: rc = hal_ptz_recalibrate(G.hp); break;
    case K_REHOME: rc = hal_ptz_tilt_rehome(G.hp); break;
    case K_STEP: rc = hal_ptz_step(G.hp, a); break;
    case K_HOME: rc = hal_ptz_home(G.hp); break;
    case K_PARK: rc = hal_ptz_park(G.hp); break;
    case K_NUDGE: rc = hal_ptz_nudge_sp(G.hp, a, b, sp); break;
    case K_GOTO: rc = hal_ptz_goto_sp(G.hp, (uint32_t)a, (uint32_t)b, sp); break;
    case K_VEL: rc = run_session(a, b); break;
    }
    if (kind == K_VEL && rc == HAL_PTZ_EREFUSED)
        return rc; /* at a soft limit / unknown position: nothing moved, nothing published, nothing logged (the tracker asks again every tick) */
    if (kind == K_RECAL && rc == HAL_PTZ_OK) {
        pthread_mutex_lock(&G.mu);
        G.scene_fresh = 1; /* a full calibration ran: the matched-boots counter restarts, the home reference is checked again */
        G.scene_dirty = G.scene_on && G.scene.capture != NULL;
        G.scene_tries = 0;
        pthread_mutex_unlock(&G.mu);
    }
    publish(hal_ptz_state(G.hp), 0);
    a2k_ptz_info(&i);
    if ((quiet || kind == K_VEL) && (rc == HAL_PTZ_OK || rc == HAL_PTZ_ECANCEL)) /* a patrol move or a tracker session: only start / stop / interrupt are logged (by the patrol), not every move */
        return rc;
    a2k_log("ptz: %s%s -> %s, %s, pan %u tilt %u", names[kind], kind == K_STEP && a >= 0 && a < 4 ? dirs[a] : "-",
            rc == HAL_PTZ_OK ? "ok" : rc == HAL_PTZ_ECANCEL ? "cancelled" : rc == HAL_PTZ_EREFUSED ? "refused (limit or unknown position)" : "FAILED",
            a2k_ptz_state_name(i.state), (unsigned)i.pan, (unsigned)i.tilt);
    return rc;
}

/* The home reference, on the PTZ thread, idle (nothing queued, not busy), only after a calibration of this process (the head is at home then; moves
 * never make it due: the reference is keyed to home and survives them). Two steps, so that the thumbnail and the position belong to the same moment:
 * (1) capture: the candidate view is kept in RAM once the motor has been quiet for A2K_PTZ_SCENE_MS (the AE has settled) and no tracker nudged
 *     for A2K_PTZ_MV_TRACK_QUIET_MS; it runs WITHOUT the lock (the repetitive-texture screening takes a while);
 * (2) commit, A2K_PTZ_SCENE_CONFIRM_MS later (or at the shutdown): the driver counters are re-read (they must still equal the cached position)
 *     and the scene module compares the candidate with the stored reference and writes only when needed (see scene.c: unchanged view =
 *     no write, a changed one at most once per 24 h). Anything else: nothing is written. The write runs with the lock held: a command can only be
 *     queued after it. */
static void scene_fail_locked(const char *why)
{
    G.scene_cand = 0;
    if (++G.scene_tries >= 3) {
        G.scene_dirty = 0;
        a2k_log("ptz: home scene reference not saved (%s), the next cold boot calibrates unless the old reference still matches", why);
    }
}

/* Steps travelled since the last calibration; an app start without a calibration counts as half the budget (the earlier travel is not known). */
static unsigned long travel_locked(void)
{
    return G.hp ? hal_ptz_travel(G.hp) + (hal_ptz_cal_count(G.hp) ? 0 : G.travel_budget / 2) : 0;
}

/* the head is at the configured home (a few steps): the reference is the HOME view */
static int at_home_locked(void)
{
    const hal_ptz_desc_t *d = G.hp ? hal_ptz_desc(G.hp) : NULL;
    uint32_t dp, dt;
    if (!d || !G.info.known)
        return 0;
    dp = G.info.pan > d->pan_home ? G.info.pan - d->pan_home : d->pan_home - G.info.pan;
    dt = G.info.tilt > d->tilt_home ? G.info.tilt - d->tilt_home : d->tilt_home - G.info.tilt;
    return dp <= A2K_PTZ_HOME_TOL && dt <= A2K_PTZ_HOME_TOL;
}

/* -1: nothing to do (wait for a signal), else the ms to wait before the next step. Under G.mu. */
static long scene_wait_locked(void)
{
    long w;
    int64_t now;
    if (!G.scene_dirty || !G.scene_on || !at_home_locked())
        return -1;
    if (G.scene_cand)
        return (long)G.scene_confirm_ms;
    now = mono_ms();
    w = (long)G.scene_ms;
    if (G.last_save_ms && G.last_save_ms + (int64_t)G.scene_gap_ms - now > w)
        w = (long)(G.last_save_ms + (int64_t)G.scene_gap_ms - now);
    if (G.last_nudge_ms && G.last_nudge_ms + A2K_PTZ_MV_TRACK_QUIET_MS - now > w) /* never during / just after tracking */
        w = (long)(G.last_nudge_ms + A2K_PTZ_MV_TRACK_QUIET_MS - now);
    return w;
}

/* Under G.mu: 1 when a capture may run now (the budget is checked here), 0 and the flags set when not. */
static int scene_capture_start_locked(void)
{
    if (!G.scene.capture || !G.scene.commit || !G.scene_on || G.busy || G.pend.kind != K_NONE || !G.info.known || !at_home_locked() || G.drift_untrusted || G.drift_corrected) { /* a doubted or corrected position never writes the reference */
        G.scene_dirty = 0;
        return 0;
    }
    if (travel_locked() > G.travel_budget || travel_locked() > A2K_PTZ_REF_TRAVEL) { /* the head has travelled since the calibration: lost steps are possible, no (re)write, the old reference stays */
        G.scene_dirty = 0;
        if (!G.budget_logged) {
            G.budget_logged = 1;
            a2k_log("ptz: %lu steps travelled since the last calibration (limit %lu): no scene reference check", travel_locked(),
                    G.travel_budget < A2K_PTZ_REF_TRAVEL ? G.travel_budget : (unsigned long)A2K_PTZ_REF_TRAVEL);
        }
        return 0;
    }
    return 1;
}

/* Under G.mu again after the capture ran unlocked with result rc (a command that was queued meanwhile supersedes it: scene_cand is reset at its end). */
static void scene_capture_done_locked(int rc)
{
    if (G.stop || G.pend.kind != K_NONE) {
        G.scene_cand = 0;
        return;
    }
    if (rc == 0) {
        G.scene_cand = 1;
        G.cand_ms = mono_ms();
    } else if (rc == 2 && G.scene.offset && G.scene.alt_capture && G.scene.alt_confirm && G.scene.commit_multi && G.mv_failed < 3 &&
               (G.scene_fresh || !G.mv_built) && (!G.last_nudge_ms || mono_ms() - G.last_nudge_ms > A2K_PTZ_MV_TRACK_QUIET_MS) && at_home_locked()) {
        G.mv_req = 1; /* a featureless home view: the multi-view reference is built by the thread loop (it moves the head, outside the lock) */
    } else if (rc > 0) {
        G.scene_dirty = 0; /* nothing worth saving: no log noise, no write */
    } else {
        scene_fail_locked("no fresh frame or unknown day/night mode");
    }
}

static void scene_commit_locked(const char *why)
{
    a2k_ptz_info_t i = G.info;
    int rc;
    if (!G.scene.commit || !G.scene_on || G.busy || G.pend.kind != K_NONE || !i.known || !G.scene_cand || !at_home_locked() || G.drift_untrusted || G.drift_corrected) {
        G.scene_dirty = G.scene_cand = 0;
        return;
    }
    if (hal_ptz_verify_pos(G.hp) != 0) {
        G.scene_dirty = G.scene_cand = 0;
        a2k_log("ptz: scene reference not saved: the driver counters no longer match the position %u/%u", (unsigned)i.pan, (unsigned)i.tilt);
        return;
    }
    if (G.scene_writes >= A2K_PTZ_SCENE_MAX_WRITES) { /* belt and braces: a bounded number of reference writes per run */
        G.scene_dirty = G.scene_cand = 0;
        return;
    }
    {
        const hal_ptz_desc_t *d = hal_ptz_desc(G.hp); /* keyed to the configured home, not to the reported position (a few steps off) */
        rc = G.scene.commit(d->pan_home, d->tilt_home, G.scene_fresh);
    }
    if (rc == 0) {
        G.scene_dirty = G.scene_cand = 0;
        G.scene_writes++;
        G.scene_fresh = 0;
        G.last_save_ms = mono_ms();
        a2k_log("ptz: scene reference step done (%s), home position %u/%u", why, (unsigned)i.pan, (unsigned)i.tilt);
    } else if (rc > 0) {
        G.scene_dirty = G.scene_cand = 0;
        G.scene_fresh = 0;
    } else {
        scene_fail_locked("the view changed or the write failed");
    }
}

/* the multi-view reference is abandoned: a stop, or a command (it supersedes everything). Under G.mu. */
static int mv_abort_locked(void) { return G.stop || G.pend.kind != K_NONE; }

/* sleeps `ms` in slices; 1 when the build must be abandoned. Called with G.mu NOT held. */
static int mv_wait(unsigned ms)
{
    unsigned t;
    for (t = 0; t < ms || !ms; t += 20) {
        int ab;
        struct timespec ts = { 0, 20 * 1000000L };
        pthread_mutex_lock(&G.mu);
        ab = mv_abort_locked();
        pthread_mutex_unlock(&G.mu);
        if (ab)
            return 1;
        if (t >= ms)
            break;
        nanosleep(&ts, NULL);
    }
    return 0;
}

/* Called by the thread, G.mu NOT held, G.busy set, nothing pending: the rest view (base) is featureless. For each offset of the scene module the
 * head moves there (the safe-motion layer, a relative move of a few hundred steps at most, inside the soft range), keeps the view, confirms it
 * later, and the head returns to base; then ONE write of the whole multi-view reference. The old reference is not touched until the new one is complete.
 * Returns 1 written, 0 a real failure (no usable view, a failed move, a write error), -1 interrupted (a command, the shutdown: nothing is written, the
 * head is left where the next command takes it, it supersedes this). */
static int run_multi(uint32_t bpan, uint32_t btilt)
{
    const hal_ptz_desc_t *d = hal_ptz_desc(G.hp);
    unsigned mask = 0, i;
    int ok = 1, rc, fresh, intr = 0;
    pthread_mutex_lock(&G.mu);
    rc = G.scene_writes >= A2K_PTZ_SCENE_MAX_WRITES; /* the same per-run cap as the single view */
    pthread_mutex_unlock(&G.mu);
    if (rc)
        return 0;
    a2k_log("ptz: scene: featureless rest view %u/%u: storing views at nearby offsets for the boot check", (unsigned)bpan, (unsigned)btilt);
    publish(HAL_PTZ_MOVED, 1);
    for (i = 0; i < 8 && ok; i++) {
        int dp, dt, c;
        int64_t tp, tt;
        if (G.scene.offset(i, &dp, &dt) != 0)
            break;
        tp = (int64_t)bpan + dp;
        tt = (int64_t)btilt + dt;
        if (tp < (int64_t)d->pan_min || tp > (int64_t)d->pan_max || tt < (int64_t)d->tilt_min || tt > (int64_t)d->tilt_max)
            continue;
        if (mv_wait(0)) {
            ok = 0;
            intr = 1;
            break;
        }
        rc = hal_ptz_goto(G.hp, (uint32_t)tp, (uint32_t)tt);
        if (rc != HAL_PTZ_OK) {
            ok = 0;
            intr = rc == HAL_PTZ_ECANCEL;
            break;
        }
        if (mv_wait(G.scene_ms)) {
            ok = 0;
            intr = 1;
            break;
        }
        c = G.scene.alt_capture(i);
        if (c < 0) {
            ok = 0;
            break;
        }
        if (c > 0)
            continue;
        if (mv_wait(G.scene_confirm_ms)) {
            ok = 0;
            intr = 1;
            break;
        }
        if (G.scene.alt_confirm(i) == 0)
            mask |= 1u << i;
    }
    if (ok && mv_wait(0)) {
        ok = 0;
        intr = 1;
    }
    if (ok) {
        rc = hal_ptz_goto(G.hp, bpan, btilt);
        if (rc != HAL_PTZ_OK) {
            ok = 0;
            intr = rc == HAL_PTZ_ECANCEL;
        }
    }
    if (ok && hal_ptz_verify_pos(G.hp) == 0) {
        pthread_mutex_lock(&G.mu);
        fresh = G.scene_fresh;
        pthread_mutex_unlock(&G.mu);
        if (mask && G.scene.commit_multi(bpan, btilt, fresh, mask) == 0) {
            pthread_mutex_lock(&G.mu);
            G.scene_writes++;
            G.scene_fresh = 0;
            G.last_save_ms = mono_ms();
            pthread_mutex_unlock(&G.mu);
            publish(hal_ptz_state(G.hp), 0);
            return 1;
        }
        a2k_log("ptz: scene: no usable view at the offsets, or the write failed: the featureless rest view cannot be a reference (the next cold boot calibrates)");
    } else if (intr) {
        a2k_log("ptz: scene: multi-view reference interrupted (a command or the shutdown): nothing written, the next cold boot calibrates");
    } else {
        a2k_log("ptz: scene: multi-view reference not built (a move or a view failed): the next cold boot calibrates");
    }
    publish(hal_ptz_state(G.hp), 0);
    return intr ? -1 : 0;
}

/* ---- drift: lost steps are found by the scene, not by the counters. When the head returns HOME and stays idle (no person, no motion) for A2K_PTZ_DRIFT_IDLE_MS, and enough steps were travelled since the
 * last check, the live view is compared with the home reference (the scene module, over a wider displacement range). OK: nothing. A small displacement with the strict proof (same rules as the boot
 * proof): the counters are re-declared to the measured position (hal_ptz_declare: no flash write, no motion), the head goes home (a few steps), and a second check must give (0, 0). Anything else
 * (no strict proof, a larger shift, the correction not confirmed): the position is marked untrusted (no reference write) and a full recalibration is scheduled for when nobody was around for
 * A2K_PTZ_RECAL_IDLE_MS (at most A2K_PTZ_RECAL_MAX per sliding window of A2K_PTZ_RECAL_WINDOW_MS, a held-back one is retried). Every check is logged; the state is RAM only (a2k_ptz_drift_info). ---- */
static void drift_note_motion_locked(int kind, int rc)
{
    if (kind == K_RECAL) {
        if (rc == HAL_PTZ_OK) { /* a real calibration: the counters are right again, the reference rules start over */
            G.drift_untrusted = G.drift_corrected = G.recal_pending = G.drift_due = 0;
            G.corr_unconfirmed = 0;
            G.cum_steps[0] = G.cum_steps[1] = 0;
            G.drift_mark = 0;
            G.rh_urgent = 0;
            G.rung = G.drift_force = 0;
            G.ok_ms = 0;
            G.inconc_runs = 0;
            G.verify_mark = 0;
            G.drift_last_ms = mono_ms();
            G.recal_not_before = 0;
            if (G.scene.dref_drop)
                G.scene.dref_drop(); /* the old picture belongs to the old counters: both modes are dropped, a fresh one is taken at home */
            G.dref_ms = 0;
            G.dref_due = G.scene_on && G.drift_ref && G.scene.dref_capture != NULL;
            G.dref_tries = 0;
        } else if (!G.stop && G.cal_mode != HAL_PTZ_CAL_OFF && (G.drift_untrusted || !G.info.known)) { /* a failed / cancelled calibration of a doubted or unknown position is retried later (never left for a restart) */
            G.recal_pending = 1;
            G.drift_untrusted = 1;
            G.recal_not_before = mono_ms() + (G.recal_retry_ms ? (int64_t)G.recal_retry_ms : (int64_t)A2K_PTZ_RECAL_RETRY_MS);
            a2k_log("ptz: the recalibration failed or was interrupted (position %s): retried in %u s", G.info.known ? "doubted" : "unknown", (unsigned)((G.recal_not_before - mono_ms()) / 1000));
        } else {
            G.recal_pending = 0; /* a user-requested calibration of a trusted position that failed: the user repeats it */
        }
        return;
    }
    if (!G.scene_on || !G.drift_ref || !(G.scene.drift4 || G.scene.drift) || !G.hp || G.recal_pending || rc != HAL_PTZ_OK || !G.info.known || !at_home_locked()) {
        G.drift_due = 0; /* not at home: the check waits for the next return */
        return;
    }
    if (G.rung == 1)
        return; /* a re-home was asked for by a doubt: the picture is judged after it, not before */
    if (!G.drift_force && hal_ptz_travel(G.hp) < G.drift_mark + G.d_min_travel)
        return; /* nothing worth checking yet (a pending check stays) */
    G.drift_due = 1;
    G.drift_tries = 0;
    G.drift_at = mono_ms() + (int64_t)G.d_idle_ms;
}

static int64_t quiet_until_locked(unsigned idle_ms)
{
    int64_t t = G.last_nudge_ms > G.last_person_ms ? G.last_nudge_ms : G.last_person_ms;
    if (G.last_manual_ms > t) /* a manual hold (live or just ended) is activity: the idle period starts after it */
        t = G.last_manual_ms;
    return t ? t + (int64_t)idle_ms : 0;
}

/* a pending drift recalibration never runs while the head is parked (a privacy park or a manual one) and never when calibration is off: it must not undo a park. Under G.mu. */
static int recal_blocked_locked(void) { return G.info.state == HAL_PTZ_PARKED || g_privacy; }

static unsigned drift_period_ms(void) { return G.d_periodic_ms ? G.d_periodic_ms : A2K_PTZ_DRIFT_PERIODIC_MS; }

/* the picture check also runs on an idle timer while the head sits at home (it costs no motor wear: two frames and a few hundred ms of CPU): a head pushed by hand or one bad reversal is not left to the travel gate. Under G.mu. */
static int drift_periodic_locked(void)
{
    return G.scene_on && G.drift_ref && (G.scene.drift4 || G.scene.drift) && !G.recal_pending && G.rung != 1 && G.hp && !G.starting && G.info.known && G.info.state == HAL_PTZ_HOME && at_home_locked() && !g_privacy;
}

/* -1: nothing to do, else the ms to wait. Under G.mu. */
static long drift_wait_locked(void)
{
    int64_t now = mono_ms(), at;
    if (G.recal_pending && !G.starting) {
        if (recal_blocked_locked())
            return -1; /* parked / privacy: the recalibration stays pending, nothing to wait for (a motion that ends re-evaluates) */
        at = quiet_until_locked(G.d_recal_idle_ms);
        if (G.recal_not_before > at)
            at = G.recal_not_before; /* held back by the window / the retry gap */
    } else if (G.drift_due && G.scene_on && G.drift_ref && (G.scene.drift4 || G.scene.drift) && G.rung != 1) {
        at = quiet_until_locked(G.d_idle_ms);
        if (G.drift_at > at)
            at = G.drift_at;
    } else if (drift_periodic_locked()) { /* nothing owed, but the picture has not been looked at for a while */
        at = quiet_until_locked(G.d_idle_ms);
        if (G.last_end_ms + (int64_t)G.d_idle_ms > at)
            at = G.last_end_ms + (int64_t)G.d_idle_ms;
        if (G.drift_last_ms + (int64_t)drift_period_ms() > at)
            at = G.drift_last_ms + (int64_t)drift_period_ms();
    } else {
        return -1;
    }
    return at > now ? (long)(at - now) : 0;
}

/* A real recalibration (the last rung): at most A2K_PTZ_RECAL_MAX in a sliding window; a held-back one is retried when the window has passed, never dropped. With the HAL position unknown there is no limit
 * (every motion is refused until a calibration worked). Under G.mu. */
static void recal_real_locked(const char *why)
{
    int64_t now = mono_ms(), win = G.recal_win_ms ? (int64_t)G.recal_win_ms : (int64_t)A2K_PTZ_RECAL_WINDOW_MS, oldest = 0;
    unsigned used = 0, i;
    G.rung = 0;
    G.drift_force = 0;
    G.recal_not_before = 0;
    for (i = 0; i < A2K_PTZ_RECAL_MAX; i++)
        if (G.recal_hist[i] && now - G.recal_hist[i] < win) {
            used++;
            if (!oldest || G.recal_hist[i] < oldest)
                oldest = G.recal_hist[i];
        }
    G.recal_pending = 1;
    if (used >= A2K_PTZ_RECAL_MAX && G.info.known) {
        G.recal_not_before = oldest + win;
        a2k_log("ptz: position untrusted (%s): %u recalibrations in the last %u min, the next one is held back for %u s (then it runs by itself); no scene reference is written", why, used, (unsigned)(win / 60000), (unsigned)((G.recal_not_before - now) / 1000));
        return;
    }
    a2k_log("ptz: position untrusted (%s): a full recalibration runs once nobody was around for %u s; no scene reference is written meanwhile", why, G.d_recal_idle_ms / 1000);
}

/* The ladder, entered by every doubt of the position (a RECAL verdict, an unconfirmed or impossible correction, a failed re-home, inconclusive checks): first the cheap exact remedy, the urgent tilt re-home, then
 * a check of the picture, and only if that still says no, the recalibration. allow_rehome = 0 skips the first rung (the re-home itself failed). Under G.mu. */
static void recal_schedule_ex_locked(const char *why, int allow_rehome)
{
    const int known = G.info.known;
    if (G.drift_mode != A2K_PTZ_DC_FULL && known) { /* ptz_drift_correct below full: a drift verdict never starts a recalibration (a position the HAL lost is another matter: nothing moves until it is calibrated) */
        G.drift_due = 0;
        a2k_log("ptz: position doubted (%s): ptz_drift_correct=%s, no recalibration", why, G.drift_mode == A2K_PTZ_DC_OFF ? "off" : "adjust");
        return;
    }
    G.drift_untrusted = 1;
    G.drift_due = 0;
    snprintf(G.recal_reason, sizeof G.recal_reason, "%s", why);
    if (G.cal_mode == HAL_PTZ_CAL_OFF) {
        a2k_log("ptz: position untrusted (%s): ptz_cal=off, no recalibration is scheduled (the motor is not calibrated by the app)", why);
        return;
    }
    if (G.rung == 1) /* the urgent re-home is already on its way: judged after it */
        return;
    if (allow_rehome && G.rung == 0 && known && G.rh_mode == A2K_PTZ_RH_IDLE && G.drift_mode == A2K_PTZ_DC_FULL) {
        G.rung = 1;
        G.rh_urgent = 1;
        G.rh_owed = 1;
        G.rh_due = at_home_locked(); /* back at home it runs at the next quiet moment; away from home it waits for the return */
        a2k_log("ptz: position doubted (%s): an urgent tilt re-home first, then the picture is checked again; a recalibration only if that still does not match", why);
        return;
    }
    recal_real_locked(why);
}

static void recal_schedule_locked(const char *why) { recal_schedule_ex_locked(why, 1); }

/* PTZ thread, G.mu held, idle, nothing pending: the correction of a measured displacement (G.mu is released while the motor and the frames are used). */
static void drift_correct_locked(int dx, int dy, int refine)
{
    const hal_ptz_desc_t *d = hal_ptz_desc(G.hp);
    unsigned kp, kt;
    int64_t tp, tt, sp, st;
    uint32_t pan0 = G.info.pan, tilt0 = G.info.tilt;
    int rc, v2, ex = 0, ey = 0, div = G.scene.drift4 ? 16 : 1; /* the unit of dx, dy: 1 thumbnail px, or 1/16 of it (a quarter px of the 320 px frame) */
    char why[72];
    hal_ptz_px_steps(G.hp, &kp, &kt);
    if (G.corr_unconfirmed) { /* the previous correction was never confirmed by a re-verify: declaring again would pile guesses on guesses */
        snprintf(why, sizeof why, "another shift (%d,%d px) while the previous correction is unconfirmed", dx, dy);
        recal_schedule_locked(why);
        return;
    }
    sp = (int64_t)A2K_PTZ_DRIFT_PAN_SIGN * dx * (int64_t)kp;
    st = (int64_t)A2K_PTZ_DRIFT_TILT_SIGN * dy * (int64_t)kt;
    sp = (sp >= 0 ? sp + 5 * div : sp - 5 * div) / (10 * div);
    st = (st >= 0 ? st + 5 * div : st - 5 * div) / (10 * div);
    tp = (int64_t)d->pan_home + sp;
    tt = (int64_t)d->tilt_home + st;
    if (tp < (int64_t)d->pan_min || tp > (int64_t)d->pan_max || tt < (int64_t)d->tilt_min || tt > (int64_t)d->tilt_max) {
        snprintf(why, sizeof why, "drift correction outside the soft range (%d,%d px)", dx, dy);
        recal_schedule_locked(why);
        return;
    }
    if (G.d_cum_max && (labs(G.cum_steps[0] + (long)sp) > (long)G.d_cum_max || labs(G.cum_steps[1] + (long)st) > (long)G.d_cum_max)) {
        snprintf(why, sizeof why, "corrected steps since the calibration beyond +-%u (%ld,%ld then %d,%d)", G.d_cum_max, G.cum_steps[0], G.cum_steps[1], (int)sp, (int)st);
        recal_schedule_locked(why);
        return;
    }
    G.busy = 1;
    G.drift_busy = 1;
    __atomic_store_n(&g_hook_abort, 0, __ATOMIC_SEQ_CST);
    pthread_mutex_unlock(&G.mu);
    publish(HAL_PTZ_MOVED, 1);
    rc = hal_ptz_declare(G.hp, (uint32_t)tp, (uint32_t)tt); /* the counters say where the head really is (no motion) */
    if (rc == HAL_PTZ_OK)
        rc = hal_ptz_home(G.hp); /* then home: a few steps, the final approach from below like a calibration's last leg */
    publish(hal_ptz_state(G.hp), 0);
    v2 = A2K_DRIFT_SKIP;
    if (rc == HAL_PTZ_OK && (G.scene.drift4 || G.scene.drift))
        v2 = G.scene.drift4 ? G.scene.drift4(d->pan_home, d->tilt_home, G.end_ms + (int64_t)G.d_frame_ms, &ex, &ey) : G.scene.drift(d->pan_home, d->tilt_home, G.end_ms + (int64_t)G.d_frame_ms, &ex, &ey);
    pthread_mutex_lock(&G.mu);
    G.busy = 0;
    G.drift_busy = 0;
    G.last_end_ms = mono_ms();
    if (rc == HAL_PTZ_ECANCEL) { /* the shutdown ended it (a command cannot: submit2 refuses them during the window): the declared counters are right, the head is not home: re-check later, no recalibration */
        G.cum_steps[0] += (long)sp;
        G.cum_steps[1] += (long)st;
        G.corr_unconfirmed = 1;
        G.drift_due = 1;
        G.drift_tries = 1;
        G.drift_at = mono_ms() + (int64_t)G.d_retry_ms;
        a2k_log("ptz: drift correction interrupted: checked again later");
        return;
    }
    if (rc != HAL_PTZ_OK) {
        snprintf(why, sizeof why, "drift correction failed (%d)", rc);
        recal_schedule_locked(why);
        return;
    }
    G.drift_corrections++;
    G.drift_corrected = 1;
    G.cum_steps[0] += (long)sp;
    G.cum_steps[1] += (long)st;
    G.drift_mark = hal_ptz_travel(G.hp);
    a2k_log("ptz: drift corrected: the view was displaced by %.2f,%.2f px: counters %u/%u re-declared as %u/%u (pan %+d, tilt %+d steps), head moved back home %u/%u; check afterwards: %s %.2f,%.2f",
            dx * (div == 16 ? 0.25 : 4.0), dy * (div == 16 ? 0.25 : 4.0), (unsigned)pan0, (unsigned)tilt0, (unsigned)tp, (unsigned)tt, (int)sp, (int)st, (unsigned)d->pan_home, (unsigned)d->tilt_home,
            v2 == A2K_DRIFT_OK ? "ok" : v2 == A2K_DRIFT_SHIFT ? "shift" : v2 == A2K_DRIFT_RECAL ? "recal" : v2 == A2K_DRIFT_INCONC ? "inconclusive" : "skipped", (double)ex * (div == 16 ? 0.25 : 4.0), (double)ey * (div == 16 ? 0.25 : 4.0));
    if (v2 == A2K_DRIFT_OK) {
        G.drift_dx = G.drift_dy = 0;
        G.corr_unconfirmed = 0;
        return;
    }
    if (v2 == A2K_DRIFT_SHIFT && div == 16 && !refine && abs(ex) <= abs(dx) / 2 + 4 && abs(ey) <= abs(dy) / 2 + 4) { /* converging (the steps per px are a measurement, not a constant): one more correction by the residual */
        a2k_log("ptz: drift correction left a residual of %.2f,%.2f px: one refinement", ex / 4.0, ey / 4.0);
        drift_correct_locked(ex, ey, 1);
        return;
    }
    if (v2 == A2K_DRIFT_INCONC || v2 == A2K_DRIFT_SKIP) { /* not disproved (a person came in): the next check confirms */
        G.corr_unconfirmed = 1; /* ... and until it does, a further shift verdict means a recalibration, not a second declare */
        G.drift_due = 1;
        G.drift_tries = 1;
        G.drift_at = mono_ms() + (int64_t)G.d_retry_ms;
        return;
    }
    snprintf(why, sizeof why, "drift correction not confirmed (%d,%d then %d,%d, %s)", dx, dy, ex, ey, div == 16 ? "1/4 px" : "px");
    recal_schedule_locked(why);
}

/* the timer of drift_wait_locked fired: a drift-triggered recalibration, or a drift check. G.mu held on entry and on return. */
static void drift_run_locked(void)
{
    const hal_ptz_desc_t *d;
    int v, dx = 0, dy = 0;
    if (G.stop || G.busy || G.pend.kind != K_NONE || !G.hp)
        return;
    if (G.recal_pending) {
        if (G.cal_mode == HAL_PTZ_CAL_OFF) { /* ptz_cal=off: the motor is never calibrated by the app */
            G.recal_pending = 0;
            a2k_log("ptz: pending recalibration dropped (ptz_cal=off)");
            return;
        }
        if (recal_blocked_locked()) /* parked: stays pending (a park scheduled after the recalibration must not be undone by it), it runs after the head is back */
            return;
        if (quiet_until_locked(G.d_recal_idle_ms) > mono_ms() || G.recal_not_before > mono_ms())
            return;
        G.recal_pending = 0;
        G.drift_recals++;
        G.drift_recal_ms = mono_ms();
        {
            unsigned k, oldest = 0; /* the sliding window: this start replaces the oldest slot */
            for (k = 1; k < A2K_PTZ_RECAL_MAX; k++)
                if (G.recal_hist[k] < G.recal_hist[oldest])
                    oldest = k;
            G.recal_hist[oldest] = G.drift_recal_ms;
        }
        a2k_log("ptz: recalibrating (%s)", G.recal_reason);
        G.pend.kind = K_RECAL; /* no manual command hook: a patrol is not stopped by it, it is idle (the wait above) */
        G.pend.a = G.pend.b = G.pend.sp = G.pend.patrol = 0;
        return;
    }
    if (quiet_until_locked(G.d_idle_ms) > mono_ms()) /* a person was seen while the timer ran (a detection does not wake the thread): wait again */
        return;
    G.drift_due = 0;
    if (!G.scene_on || !G.drift_ref || !(G.scene.drift4 || G.scene.drift) || G.rung == 1 || !G.info.known || !at_home_locked())
        return;
    G.drift_force = 0;
    d = hal_ptz_desc(G.hp);
    G.busy = 1;
    __atomic_store_n(&g_hook_abort, 0, __ATOMIC_SEQ_CST);
    pthread_mutex_unlock(&G.mu);
    v = G.scene.drift4 ? G.scene.drift4(d->pan_home, d->tilt_home, G.end_ms + (int64_t)G.d_frame_ms, &dx, &dy) : G.scene.drift(d->pan_home, d->tilt_home, G.end_ms + (int64_t)G.d_frame_ms, &dx, &dy);
    pthread_mutex_lock(&G.mu);
    G.busy = 0;
    if (G.stop || G.pend.kind != K_NONE) /* a command came meanwhile: the head may have moved, the verdict is void */
        return;
    if (__atomic_load_n(&g_hook_abort, __ATOMIC_SEQ_CST) || quiet_until_locked(G.d_idle_ms) > mono_ms()) { /* somebody arrived during the check (the tracker is waiting for the head): whatever it said is void, it is repeated once they are gone */
        G.drift_due = 1;
        G.drift_at = mono_ms();
        a2k_log("ptz: drift check voided: a person / a tracker command arrived while it ran (verdict discarded, checked again once quiet)");
        return;
    }
    G.drift_checks++;
    G.drift_last_ms = mono_ms();
    snprintf(G.drift_last, sizeof G.drift_last, "%s", v == A2K_DRIFT_OK ? "ok" : v == A2K_DRIFT_SHIFT ? "shift" : v == A2K_DRIFT_RECAL ? "recal" : v == A2K_DRIFT_INCONC ? "inconc" : v == A2K_DRIFT_FLAT ? "skip-flat" : "skip");
    if (v != A2K_DRIFT_SKIP && v != A2K_DRIFT_INCONC && v != A2K_DRIFT_FLAT) {
        G.drift_have = 1;
        G.drift_dx = dx;
        G.drift_dy = dy;
    }
    if (v == A2K_DRIFT_OK || v == A2K_DRIFT_SHIFT)
        G.rh_exact = 0; /* a structured view was judged: a later flat one is news again */
    if (v != A2K_DRIFT_INCONC) {
        G.verify_mark = hal_ptz_travel(G.hp); /* looked at (any verdict but a retry): the travel budget of the go-home-and-verify rule restarts */
        if (v != A2K_DRIFT_SKIP)
            G.inconc_runs = 0; /* something was proven (ok / shift / recal / flat): the run of inconclusive returns is over */
    }
    if (v == A2K_DRIFT_OK) {
        G.drift_mark = hal_ptz_travel(G.hp);
        G.ok_ms = mono_ms();
        G.ok_travel = G.drift_mark;
        G.corr_unconfirmed = 0; /* the view matches: an earlier unconfirmed correction is confirmed */
        if (G.rung == 2 || (G.drift_untrusted && !G.recal_pending)) { /* the doubt is cleared: the re-home made the tilt exact and the picture agrees */
            G.drift_untrusted = 0;
            G.rung = 0;
            a2k_log("ptz: the position is trusted again (the picture matches after the tilt re-home)");
            G.dref_due = G.drift_ref && G.scene.dref_capture != NULL; /* a position proven by the picture: the reference is taken (again) from it */
            G.dref_tries = 0;
        } else if (G.scene.dref_capture && G.drift_ref && (!G.dref_ms || mono_ms() - G.dref_ms > (int64_t)(G.dref_refresh_ms ? G.dref_refresh_ms : A2K_PTZ_DREF_REFRESH_MS))) {
            G.dref_due = 1; /* a proven zero: the reference follows the furniture and the light instead of ageing */
            G.dref_tries = 0;
        }
        a2k_log("ptz: drift check: displacement 0,0 px (the counters are right): nothing to do");
    } else if (v == A2K_DRIFT_SHIFT) {
        if (G.drift_mode == A2K_PTZ_DC_OFF) {
            G.drift_mark = hal_ptz_travel(G.hp);
            a2k_log("ptz: drift check: displacement %d,%d (%s) proven: ptz_drift_correct=off, not corrected", dx, dy, G.scene.drift4 ? "1/4 px" : "px");
        } else {
            a2k_log("ptz: drift check: displacement %d,%d (%s) proven: correcting the counters", dx, dy, G.scene.drift4 ? "1/4 px" : "px");
            drift_correct_locked(dx, dy, 0);
        }
    } else if (v == A2K_DRIFT_RECAL) {
        char why[72];
        snprintf(why, sizeof why, "home view does not match the reference (peak %d,%d)", dx, dy);
        recal_schedule_locked(why);
    } else if (v == A2K_DRIFT_INCONC && ++G.drift_tries < A2K_PTZ_DRIFT_TRIES) {
        G.drift_due = 1;
        G.drift_at = mono_ms() + (int64_t)G.d_retry_ms;
        a2k_log("ptz: drift check: inconclusive, again in %u s", G.d_retry_ms / 1000);
    } else if (v == A2K_DRIFT_INCONC) {
        G.drift_mark = hal_ptz_travel(G.hp); /* gave up for this return */
        G.verify_mark = G.drift_mark;
        G.inconc_runs++;
        a2k_log("ptz: drift check: inconclusive %u times: given up until the next return home (%u return(s) in a row)", A2K_PTZ_DRIFT_TRIES, G.inconc_runs);
        if (G.rung == 2) { /* the tilt re-home was done and the picture still cannot confirm the position: the last rung */
            recal_schedule_locked("the picture stays inconclusive after the tilt re-home");
        } else if (G.inconc_runs >= A2K_PTZ_INCONC_ESCALATE) { /* nothing proven for several returns in a row: a slip beyond the matcher range looks exactly like this */
            G.inconc_runs = 0;
            recal_schedule_locked("inconclusive checks on several returns home in a row");
        }
    } else if (v == A2K_DRIFT_FLAT) {
        /* the reference had detail, the view at the same counters has none: the head is not where the counters say (ceiling). The travel mark is NOT reset: the check is still owed. */
        const int rh = G.rh_mode == A2K_PTZ_RH_IDLE && G.cal_mode != HAL_PTZ_CAL_OFF;
        if (rh && G.rh_exact) { /* the tilt was just made exact and the view is still flat: not a slip (a covered lens, a dark room, a wall): nothing more to do, the state says so */
            G.drift_mark = hal_ptz_travel(G.hp);
            snprintf(G.drift_last, sizeof G.drift_last, "flat-persist");
            a2k_log("ptz: drift check: the home view is still flat after a completed tilt re-home: the view cannot be judged (lens covered, dark, a wall?), no further re-home (ptz.drift_last = flat-persist)");
            return;
        }
        a2k_log("ptz: drift check: flat view at home where the reference had detail: the head is probably far off (a slip), %s", rh ? "a tilt re-home is scheduled for the next quiet moment" : "recalibrating");
        if (rh) {
            G.rh_urgent = 1;
            G.rh_due = 1;
        } else {
            recal_schedule_locked("home view flat where the reference had detail");
        }
    } else {
        /* skipped: nothing can be judged now (no reference, no fresh frame, a probe). Logged; the travel mark is kept, the check stays owed for the next return. */
        a2k_log("ptz: drift check skipped (nothing to compare): %lu steps travelled since the last successful check", (unsigned long)(hal_ptz_travel(G.hp) > G.drift_mark ? hal_ptz_travel(G.hp) - G.drift_mark : 0));
    }
}

/* ---- the tilt-only re-home (hal_ptz_tilt_rehome): lost TILT steps never accumulate. Due when a motion ends at HOME (the tracker's return home, a home command), the mode is idle, the position is trusted
 * and enough (weighted) steps were travelled since the last re-home or calibration. It starts after A2K_PTZ_REHOME_IDLE_MS of quiet (no person seen, no tracker command, no manual hold, no motion), at
 * most once per gap (an interrupted attempt counts), after a pending drift check (which looks at the same counters), never parked / privacy / ptz_cal=off / untrusted. A person detected, a tracker
 * command or a user PTZ command cancels it (the believed position of before is kept, nothing worse). RAM only. ---- */
static int manual_hold_active_locked(void);
static int64_t urg_gap(void) { return G.urg_gap_ms ? (int64_t)G.urg_gap_ms : (int64_t)A2K_PTZ_REHOME_URGENT_GAP_MS; }
static unsigned rh_retry_ms(void) { return G.rh_retry_ms ? G.rh_retry_ms : A2K_PTZ_REHOME_RETRY_MS; }

static void rehome_note_motion_locked(int kind, int rc)
{
    unsigned long tr;
    if (rc == HAL_PTZ_OK && G.info.known && at_home_locked())
        G.go_home = 0; /* the head is home: the automatic return is done (whatever brought it there) */
    if (kind == K_RECAL) {
        if (rc == HAL_PTZ_OK) { /* the counters are the stop-derived ones again */
            G.rh_mark = 0;
            G.rh_due = 0;
            G.rh_owed = 0;
            G.rh_exact = 0;
            G.go_home = 0;
        }
        return;
    }
    if (kind == K_REHOME) {
        G.rh_due = 0;
        if (rc == HAL_PTZ_OK) {
            G.rh_runs++;
            G.rh_done_ms = mono_ms();
            G.rh_done_urgent = G.rh_cur_urgent;
            a2k_log("ptz: tilt re-home done (%s), %u completed, %u cancelled", G.rh_cur_urgent ? "flat" : "gap", G.rh_runs, G.rh_cancels);
            G.rh_mark = G.hp ? hal_ptz_travel(G.hp) : 0;
            G.cum_steps[1] = 0; /* the tilt counter is exact again: the signed sum of the tilt corrections starts over */
            G.rh_owed = 0;
            G.rh_exact = 1;
            G.rh_cancel_streak = 0;
            G.go_home = 0;
            if (G.rung == 1) { /* the re-home asked for by a doubt is done: the picture now decides (the check is forced: it is not worth the travel gate, it is the verdict of the ladder) */
                G.rung = 2;
                G.drift_force = 1;
            }
        } else if (rc == HAL_PTZ_ECANCEL || G.info.known) { /* interrupted (a person, a tracker), or failed with the believed position still valid: nothing was corrected, it stays wanted */
            const int canc = rc == HAL_PTZ_ECANCEL;
            if (canc)
                G.rh_cancels++;
            G.rh_owed = 1;
            if (G.rh_cur_urgent)
                G.rh_urgent = 1; /* the flat / recal request is not lost with the attempt */
            G.rh_last = G.rh_last_prev; /* the attempt did not correct anything: it does not consume the gap, a short retry gap applies instead */
            G.rh_urg_last = G.rh_urg_last_prev;
            G.rh_cancel_streak++;
            G.rh_retry_at = mono_ms() + (G.rh_cancel_streak >= A2K_PTZ_REHOME_RETRY_MAX ? (int64_t)G.rh_gap_ms : (int64_t)rh_retry_ms());
            G.go_home = G.info.known && !at_home_locked(); /* the head stands wherever the interruption left it: back home once quiet */
            a2k_log("ptz: tilt re-home %s (%s), %u completed, %u cancelled; %s, retry in %u s", canc ? "cancelled" : "interrupted by a failure", G.rh_cur_urgent ? "flat" : "gap", G.rh_runs, G.rh_cancels,
                    G.go_home ? "returning home by itself once quiet" : "head at home", (unsigned)((G.rh_retry_at - mono_ms()) / 1000));
        } else {
            recal_schedule_ex_locked("tilt re-home failed", 0);
        }
        return;
    }
    if (G.rh_mode != A2K_PTZ_RH_IDLE || !G.hp || rc != HAL_PTZ_OK || !G.info.known || !at_home_locked() || G.recal_pending) {
        G.rh_due = 0; /* not at home: the re-home waits for the next return */
        return;
    }
    if (G.rh_urgent || G.rh_owed) { /* a flat home view was seen / an attempt was interrupted: back at home, the re-home is wanted whatever was travelled */
        G.rh_due = 1;
        return;
    }
    tr = hal_ptz_travel(G.hp);
    if (tr < G.rh_mark) /* the budget restarted (a calibration) */
        G.rh_mark = 0;
    if (tr < G.rh_mark + G.rh_min_travel)
        return; /* nothing worth correcting yet (a pending one stays) */
    G.rh_due = 1;
}

/* -1: nothing to do, else the ms to wait. Under G.mu. */
static long rehome_wait_locked(void)
{
    int64_t now = mono_ms(), at;
    if (!G.rh_due || G.rh_mode != A2K_PTZ_RH_IDLE || G.starting || G.drift_busy || G.recal_pending || !G.hp || G.cal_mode == HAL_PTZ_CAL_OFF)
        return -1;
    if (G.rung != 1 && G.drift_due && G.scene_on && G.drift_ref && (G.scene.drift4 || G.scene.drift)) /* a drift check is pending: it looks at the counters first (not when the re-home was asked for by a doubt: it goes first) */
        return -1;
    at = quiet_until_locked(G.rh_idle_ms);
    if (G.last_end_ms + (int64_t)G.rh_idle_ms > at)
        at = G.last_end_ms + (int64_t)G.rh_idle_ms;
    if (G.rh_urgent) {
        if (G.rh_urg_last && G.rh_urg_last + urg_gap() > at)
            at = G.rh_urg_last + urg_gap();
    } else if (G.rh_last && G.rh_last + (int64_t)G.rh_gap_ms > at) {
        at = G.rh_last + (int64_t)G.rh_gap_ms;
    }
    if (G.rh_retry_at > at)
        at = G.rh_retry_at;
    return at > now ? (long)(at - now) : 0;
}

/* the timer of rehome_wait_locked fired. G.mu held on entry and on return. */
static void rehome_run_locked(void)
{
    if (G.stop || G.busy || G.pend.kind != K_NONE || !G.hp)
        return;
    if (G.rh_retry_at > mono_ms() || quiet_until_locked(G.rh_idle_ms) > mono_ms() || (G.rh_urgent ? (G.rh_urg_last && G.rh_urg_last + urg_gap() > mono_ms()) : (G.rh_last && G.rh_last + (int64_t)G.rh_gap_ms > mono_ms()))) /* somebody was around meanwhile: wait again */
        return;
    G.rh_due = 0;
    if (G.rh_mode != A2K_PTZ_RH_IDLE || !G.info.known || !at_home_locked() || G.recal_pending || recal_blocked_locked() || G.cal_mode == HAL_PTZ_CAL_OFF)
        return;
    {
        const unsigned long tr = hal_ptz_travel(G.hp);
        if (!G.rh_urgent && G.rung == 0 && G.ok_ms && mono_ms() - G.ok_ms < (int64_t)G.rh_gap_ms && tr < G.ok_travel + G.rh_min_travel) { /* the picture said ok within the gap and little was travelled since: the tilt is proven, the re-home would only grind the stop */
            G.rh_due = 0;
            G.rh_owed = 0;
            G.rh_mark = tr;
            a2k_log("ptz: tilt re-home skipped: the picture checked ok %u s ago and only %lu steps were travelled since", (unsigned)((mono_ms() - G.ok_ms) / 1000), tr - G.ok_travel);
            return;
        }
    }
    G.rh_last_prev = G.rh_last;
    G.rh_urg_last_prev = G.rh_urg_last;
    G.rh_last = mono_ms();
    G.rh_cur_urgent = G.rh_urgent;
    if (G.rh_urgent) {
        G.rh_urg_last = G.rh_last;
        G.rh_urgent = 0;
    }
    a2k_log("ptz: tilt re-home starting (nobody around, %lu steps travelled since the last one)", (unsigned long)(hal_ptz_travel(G.hp) - (hal_ptz_travel(G.hp) > G.rh_mark ? G.rh_mark : hal_ptz_travel(G.hp))));
    G.pend.kind = K_REHOME;
    G.pend.a = G.pend.b = G.pend.sp = G.pend.patrol = 0;
}

/* a person is in view / a tracker command arrived: a running re-home gives way (under G.mu) */
static void rehome_yield_locked(void)
{
    if (G.busy && G.cur_kind == K_REHOME && G.hp)
        hal_ptz_cancel(G.hp);
}

/* a tracker command arrived: the controller's own return home gives way (under G.mu) */
static void autohome_yield_locked(void)
{
    if (G.busy && G.cur_auto && G.hp)
        hal_ptz_cancel(G.hp);
}

/* -1: nothing to do, else the ms to wait. Under G.mu. The head was left off home by an interrupted re-home: it returns home once nobody was around for the re-home idle time. */
static unsigned verify_travel(void) { return G.d_verify_travel ? G.d_verify_travel : A2K_PTZ_VERIFY_TRAVEL; }

/* A person who keeps the camera away from home (the tracker follows, the head never returns) lets lost steps pile up unseen, and a slip beyond the soft margins drives the tracker into a mechanical stop where every
 * further step is lost. Past a travel budget since the picture was last looked at, the controller goes home by itself (nobody in view: never while somebody is being tracked) and verifies there. */
static void verify_budget_locked(void)
{
    if (G.go_home || !G.hp || !G.info.known || G.info.state != HAL_PTZ_MOVED || at_home_locked() || !G.scene_on || !G.drift_ref || !(G.scene.drift4 || G.scene.drift) || G.recal_pending)
        return;
    if (hal_ptz_travel(G.hp) < G.verify_mark + verify_travel())
        return;
    G.go_home = 1;
    G.drift_force = 1; /* the check at home is not worth the usual travel gate: it is the point of the trip */
    a2k_log("ptz: %lu steps travelled since the picture was last checked: going home to verify the position once nobody is in view", (unsigned long)(hal_ptz_travel(G.hp) - G.verify_mark));
}

static long autohome_wait_locked(void)
{
    int64_t now = mono_ms(), at;
    verify_budget_locked();
    if (!G.go_home || G.starting || G.drift_busy || !G.hp || !G.info.known || at_home_locked() || G.info.state == HAL_PTZ_PARKED || g_privacy || manual_hold_active_locked())
        return -1;
    at = quiet_until_locked(G.rh_idle_ms);
    if (G.last_end_ms + (int64_t)G.rh_idle_ms > at)
        at = G.last_end_ms + (int64_t)G.rh_idle_ms;
    return at > now ? (long)(at - now) : 0;
}

/* the timer of autohome_wait_locked fired. G.mu held on entry and on return. */
static void autohome_run_locked(void)
{
    if (G.stop || G.busy || G.pend.kind != K_NONE || !G.hp || !G.go_home || !G.info.known || at_home_locked() || quiet_until_locked(G.rh_idle_ms) > mono_ms())
        return;
    a2k_log("ptz: returning home by itself (the head was left at %u/%u by an interrupted re-home)", (unsigned)G.info.pan, (unsigned)G.info.tilt);
    G.pend.kind = K_HOME; /* no manual hook: a patrol is not stopped by it (it is idle: nothing pending, not busy) */
    G.pend.a = G.pend.b = G.pend.sp = G.pend.patrol = 0;
    G.pend.auto_home = 1;
}

void a2k_ptz_test_set_rehome_timing(unsigned idle_ms, unsigned gap_ms, unsigned min_travel)
{
    G.rh_idle_ms = idle_ms ? idle_ms : A2K_PTZ_REHOME_IDLE_MS;
    G.rh_gap_ov = gap_ms;
    G.rh_gap_ms = gap_ms ? gap_ms : A2K_PTZ_REHOME_GAP_MIN * 60000u;
    G.rh_min_travel = min_travel ? min_travel : A2K_PTZ_REHOME_MIN_TRAVEL;
}

void a2k_ptz_rehome_info(unsigned *runs, unsigned *cancels)
{
    pthread_mutex_lock(&G.mu);
    if (runs)
        *runs = G.rh_runs;
    if (cancels)
        *cancels = G.rh_cancels;
    pthread_mutex_unlock(&G.mu);
}

void a2k_ptz_rehome_last(unsigned long *last_s, const char **reason)
{
    pthread_mutex_lock(&G.mu);
    if (last_s)
        *last_s = G.rh_done_ms ? (unsigned long)(G.rh_done_ms / 1000) : 0;
    if (reason)
        *reason = G.rh_done_ms ? (G.rh_done_urgent ? "flat" : "gap") : "none";
    pthread_mutex_unlock(&G.mu);
}

void a2k_ptz_drift_info(a2k_ptz_drift_t *out)
{
    pthread_mutex_lock(&G.mu);
    out->have = G.drift_have;
    out->dx = G.drift_dx;
    out->dy = G.drift_dy;
    out->corrections = G.drift_corrections;
    out->checks = G.drift_checks;
    out->recals = G.drift_recals;
    out->untrusted = G.drift_untrusted;
    snprintf(out->recal_reason, sizeof out->recal_reason, "%s", G.recal_reason);
    snprintf(out->last, sizeof out->last, "%s", G.drift_last);
    pthread_mutex_unlock(&G.mu);
}

void a2k_ptz_note_person(void)
{
    __atomic_store_n(&g_hook_abort, 1, __ATOMIC_SEQ_CST);
    pthread_mutex_lock(&G.mu);
    G.last_person_ms = mono_ms();
    rehome_yield_locked();
    pthread_mutex_unlock(&G.mu);
}

void a2k_ptz_test_set_drift_timing(unsigned idle_ms, unsigned frame_ms, unsigned min_travel, unsigned recal_idle_ms)
{
    G.d_idle_ms = idle_ms ? idle_ms : A2K_PTZ_DRIFT_IDLE_MS;
    G.d_frame_ms = frame_ms ? frame_ms : A2K_PTZ_DRIFT_FRAME_MS;
    G.d_min_travel = min_travel ? min_travel : A2K_PTZ_DRIFT_MIN_TRAVEL;
    G.d_recal_idle_ms = recal_idle_ms ? recal_idle_ms : A2K_PTZ_RECAL_IDLE_MS;
}

void a2k_ptz_test_set_verify_timing(unsigned dref_refresh_ms, unsigned periodic_ms, unsigned verify_travel)
{
    G.dref_refresh_ms = dref_refresh_ms;
    G.d_periodic_ms = periodic_ms;
    G.d_verify_travel = verify_travel;
}

void a2k_ptz_test_set_ladder_timing(unsigned urgent_gap_ms, unsigned rehome_retry_ms, unsigned recal_window_ms, unsigned recal_retry_ms)
{
    G.urg_gap_ms = urgent_gap_ms;
    G.rh_retry_ms = rehome_retry_ms;
    G.recal_win_ms = recal_window_ms;
    G.recal_retry_ms = recal_retry_ms;
}

void a2k_ptz_test_set_drift_retry(unsigned retry_ms) { G.d_retry_ms = retry_ms ? retry_ms : A2K_PTZ_DRIFT_RETRY_MS; }

void a2k_ptz_test_force_recal_pending(void)
{
    pthread_mutex_lock(&G.mu);
    G.drift_untrusted = 1;
    G.recal_pending = 1;
    snprintf(G.recal_reason, sizeof G.recal_reason, "test");
    pthread_cond_signal(&G.cv);
    pthread_mutex_unlock(&G.mu);
}

/* ---- the drift reference (RAM): own capture rule. Wanted (dref_due) at a proven position: a proven boot, a restart that adopted the driver position, after a calibration, after a tilt re-home followed by a
 * matching picture, as a refresh after a proven zero. Taken once the head is quiet at home for A2K_PTZ_SCENE_MS and nobody is around (a short quiet time: the 10 min tracker rule belongs to the multi-view
 * excursion, not to a still picture); the hook itself requires two steady frames. Nothing is written to the flash. ---- */
static int dref_usable_locked(void)
{
    return G.dref_due && G.scene_on && G.drift_ref && G.scene.dref_capture && G.hp && !G.starting && G.info.known && at_home_locked() && G.info.state == HAL_PTZ_HOME && !G.recal_pending && !G.drift_untrusted && G.rung != 1;
}

/* -1: nothing to do, else the ms to wait. Under G.mu. */
static long dref_wait_locked(void)
{
    int64_t now = mono_ms(), at;
    if (!dref_usable_locked())
        return -1;
    at = quiet_until_locked(G.scene_ms);
    if (G.last_end_ms + (int64_t)G.scene_ms > at)
        at = G.last_end_ms + (int64_t)G.scene_ms;
    if (G.dref_retry_at > at)
        at = G.dref_retry_at;
    return at > now ? (long)(at - now) : 0;
}

/* the timer of dref_wait_locked fired. G.mu held on entry and on return; released while the hook waits for frames. */
static void dref_run_locked(void)
{
    int rc;
    if (G.stop || G.busy || G.pend.kind != K_NONE || !dref_usable_locked() || quiet_until_locked(G.scene_ms) > mono_ms() || G.dref_retry_at > mono_ms())
        return;
    G.busy = 1;
    __atomic_store_n(&g_hook_abort, 0, __ATOMIC_SEQ_CST);
    pthread_mutex_unlock(&G.mu);
    rc = G.scene.dref_capture();
    pthread_mutex_lock(&G.mu);
    G.busy = 0;
    if (G.stop || G.pend.kind != K_NONE)
        return; /* a command came meanwhile: the head may have moved, the picture is void: still wanted */
    if (quiet_until_locked(G.scene_ms) > mono_ms()) { /* somebody arrived during the capture: still wanted, later */
        G.dref_retry_at = mono_ms() + (int64_t)G.d_retry_ms;
        return;
    }
    if (rc == 0) {
        G.dref_due = 0;
        G.dref_tries = 0;
        G.dref_ms = mono_ms();
    } else if (++G.dref_tries >= 3) {
        G.dref_due = 0; /* three failed tries (no steady frames, too little structure): until the next proven position or refresh */
        a2k_log("ptz: no drift reference could be captured (3 tries): the next proven position tries again");
    } else {
        G.dref_retry_at = mono_ms() + (int64_t)G.d_retry_ms;
    }
}

int a2k_ptz_drift_ref_info(unsigned long *age_s)
{
    int m = 0;
    unsigned long a = 0;
    pthread_mutex_lock(&G.mu);
    if (G.scene.dref_info)
        m = G.scene.dref_info(&a);
    pthread_mutex_unlock(&G.mu);
    if (age_s)
        *age_s = a;
    return m;
}

static void *ptz_thread(void *arg)
{
    a2k_ptz_info_t i;
    (void)arg;
    publish(G.cal_mode == HAL_PTZ_CAL_OFF ? HAL_PTZ_UNKNOWN : HAL_PTZ_CALIBRATING, 1);
    if (hal_ptz_start(G.hp, G.cal_mode) != 0)
        a2k_log("ptz: not available");
    publish(hal_ptz_state(G.hp), 0);
    a2k_ptz_info(&i);
    a2k_log("ptz: ready, state %s%s", a2k_ptz_state_name(i.state), i.known ? "." : " (position unknown)");
    pthread_mutex_lock(&G.mu);
    G.starting = 0; /* commands are accepted from now on */
    G.last_end_ms = mono_ms();
    G.scene_fresh = hal_ptz_cal_count(G.hp) > 0; /* a full calibration ran in this start: the home reference is checked, the matched-boots counter restarts */
    G.scene_dirty = G.scene_on && G.scene.capture && i.known && G.scene_fresh; /* a start that matched the reference or adopted the frame writes nothing */
    G.dref_due = G.scene_on && G.drift_ref && G.scene.dref_capture && i.known; /* a proven boot, an adopted restart or a calibration: the position is proven, the drift reference is taken */
    G.dref_tries = 0;
    G.dref_ms = G.dref_retry_at = 0;
    G.drift_last_ms = mono_ms();
    for (;;) {
        int kind, a, b, sp, patrol, rc;
        unsigned ticket;
        while (!G.stop && G.pend.kind == K_NONE) {
            const long ws[5] = { autohome_wait_locked(), drift_wait_locked(), rehome_wait_locked(), dref_wait_locked(), scene_wait_locked() }; /* priority on a tie: automatic return, drift, re-home, drift reference, flash reference */
            int pick = -1, ki;
            long wait_ms;
            for (ki = 0; ki < 5; ki++)
                if (ws[ki] >= 0 && (pick < 0 || ws[ki] < ws[pick]))
                    pick = ki;
            wait_ms = pick >= 0 ? ws[pick] : -1;
            const int auto_first = pick == 0, drift_first = pick == 1, rehome_first = pick == 2, dref_first = pick == 3;
            if (wait_ms >= 0) {
                struct timespec ts;
                clock_gettime(CLOCK_REALTIME, &ts);
                ts.tv_sec += wait_ms / 1000;
                ts.tv_nsec += (wait_ms % 1000) * 1000000L;
                if (ts.tv_nsec >= 1000000000L) {
                    ts.tv_sec++;
                    ts.tv_nsec -= 1000000000L;
                }
                if (pthread_cond_timedwait(&G.cv, &G.mu, &ts) == ETIMEDOUT && !G.stop && G.pend.kind == K_NONE) {
                    if (auto_first) {
                        autohome_run_locked();
                    } else if (rehome_first) {
                        rehome_run_locked();
                    } else if (drift_first) {
                        drift_run_locked();
                    } else if (dref_first) {
                        dref_run_locked();
                    } else if (G.scene_cand) {
                        scene_commit_locked("idle");
                    } else if (scene_capture_start_locked()) {
                        int rc;
                        G.busy = 1; /* nothing else runs on this thread meanwhile; a command is only queued (and runs after) */
                        __atomic_store_n(&g_hook_abort, 0, __ATOMIC_SEQ_CST);
                        pthread_mutex_unlock(&G.mu);
                        rc = G.scene.capture(); /* outside the lock: the repetitive-texture screening takes a while */
                        pthread_mutex_lock(&G.mu);
                        G.busy = 0;
                        scene_capture_done_locked(rc);
                        if (G.mv_req) {
                            const hal_ptz_desc_t *hd = hal_ptz_desc(G.hp);
                            a2k_ptz_info_t bi = G.info;
                            int done;
                            bi.pan = hd->pan_home; /* the multi-view reference is keyed to the configured home too */
                            bi.tilt = hd->tilt_home;
                            G.mv_req = 0;
                            G.busy = 1;
                            pthread_mutex_unlock(&G.mu);
                            done = run_multi(bi.pan, bi.tilt);
                            pthread_mutex_lock(&G.mu);
                            G.busy = 0;
                            G.last_end_ms = mono_ms();
                            G.scene_cand = 0;
                            G.scene_tries = 0;
                            if (done != -1) /* an interruption (a command, the shutdown) is no failure: the reference stays due, retried at home */
                                G.scene_dirty = 0;
                            if (done == 1) {
                                G.mv_built = 1;
                                G.mv_failed = 0;
                            } else if (done == 0) {
                                G.mv_failed++; /* a real failure: 3 tries per run at most */
                            }
                        }
                    }
                }
            } else {
                pthread_cond_wait(&G.cv, &G.mu);
            }
        }
        if (G.stop)
            break;
        kind = G.pend.kind;
        a = G.pend.a;
        b = G.pend.b;
        sp = G.pend.sp;
        patrol = G.pend.patrol;
        ticket = G.pend.ticket;
        G.cur_auto = kind == K_HOME && G.pend.auto_home;
        G.pend.auto_home = 0;
        G.pend.kind = K_NONE;
        G.pend.patrol = 0;
        if (kind == K_VEL) {
            G.vsess = 1; /* a2k_ptz_vel now updates the mailbox */
            G.vending = 0;
        }
        else
            ring_reset_locked(); /* another kind of motion: positions in between are not sampled */
        hal_ptz_mark(G.hp); /* under the lock: a cancel from submit() after this point aborts the command at its start */
        G.busy = 1;
        G.cur_patrol = patrol;
        G.cur_ticket = ticket;
        G.cur_kind = kind;
        if (kind == K_RECAL)
            G.starting = 1; /* no command is accepted (and none cancels it) while the calibration runs */
        pthread_mutex_unlock(&G.mu);
        rc = run_cmd(kind, a, b, sp, patrol);
        pthread_mutex_lock(&G.mu);
        if (G.cur_patrol) {
            G.pt_done = G.cur_ticket;
            G.pt_rc = rc;
        }
        G.cur_patrol = 0;
        G.cur_auto = 0;
        G.cur_kind = K_NONE;
        G.vsess = 0;
        G.vending = 0;
        if (kind == K_VEL && G.pend.kind != K_VEL)
            ptzl_clear(&G.man); /* the manual hold ends with its session (the next tracker command may run) */
        G.busy = 0;
        G.starting = 0;
        G.last_end_ms = mono_ms();
        G.scene_cand = 0; /* a candidate taken before the command is void (the head moved) */
        G.scene_tries = 0;
        rehome_note_motion_locked(kind, rc);
        drift_note_motion_locked(kind, rc);
    }
    if (G.stop_commit) /* a2k_ptz_stop: a settled candidate waits for its write; this thread owns the driver */
        scene_commit_locked("shutdown");
    pthread_mutex_unlock(&G.mu);
    hal_ptz_close(G.hp);
    return NULL;
}

void a2k_ptz_test_set(const hal_ptz_ops_t *ops, const char *conf_path)
{
    if (ops) {
        G.ops = *ops;
        G.custom_ops = 1;
    } else {
        G.custom_ops = 0;
    }
    snprintf(G.conf_path, sizeof G.conf_path, "%s", conf_path ? conf_path : A2K_PTZ_CONF);
}

int a2k_ptz_start(const a2k_ptz_cfg_t *cfg)
{
    hal_ptz_desc_t d = a2k_ptz_desc;
    char conf[1024], err[96] = { 0 };
    int n;
    pthread_attr_t at;
    if (G.running)
        return 0;
    if (!G.custom_ops)
        G.ops = *hal_ptz_linux_ops(d.dev);
    G.ops.log = log_line;
    G.ops.cold_check = hook_cold_check;
    G.ops.cold_offset = hook_cold_offset;
    G.ops.cold_probe = hook_cold_probe;
    G.ops.scene_drop = hook_drop;
    errno = 0;
    n = G.ops.load_file(G.conf_path, conf, sizeof conf);
    if (n < 0 && errno != ENOENT) {
        a2k_log("ptz: cannot read %s (%s): PTZ off", G.conf_path, strerror(errno));
        return -1;
    }
    if (cfg->park_pan >= 0 && cfg->park_tilt >= 0) {
        d.pan_park = (uint32_t)cfg->park_pan;
        d.tilt_park = (uint32_t)cfg->park_tilt;
    }
    G.hp = hal_ptz_new(&d, &G.ops, n >= 0 ? conf : NULL, err, sizeof err);
    if (!G.hp) {
        a2k_log("ptz: bad %s: %s: PTZ off (the motor is never touched)", G.conf_path, err);
        return -1;
    }
    G.stop = 0;
    G.pend.kind = K_NONE;
    G.starting = 1;
    G.cal_mode = cfg->cal;
    G.scene_on = cfg->scene_check;
    G.drift_ref = cfg->drift_ref;
    G.drift_mode = cfg->drift_correct;
    G.boot_always = cfg->boot_always;
    G.rh_mode = cfg->tilt_rehome;
    G.rh_gap_ms = G.rh_gap_ov ? G.rh_gap_ov : (cfg->rehome_gap_min ? cfg->rehome_gap_min : A2K_PTZ_REHOME_GAP_MIN) * 60000u;
    G.rh_due = 0;
    G.rh_last = 0;
    G.rh_mark = 0;
    G.rh_runs = G.rh_cancels = 0;
    G.travel_budget = cfg->travel_budget ? cfg->travel_budget : A2K_PTZ_TRAVEL_BUDGET;
    G.scene_writes = 0;
    G.budget_logged = 0;
    G.scene_dirty = G.scene_tries = G.busy = G.scene_cand = G.scene_fresh = G.stop_commit = G.mv_req = G.mv_failed = G.mv_built = 0;
    G.last_nudge_ms = 0;
    G.last_save_ms = 0;
    G.vsess = 0;
    G.vel_seq = 0;
    G.drift_due = G.drift_untrusted = G.drift_corrected = G.recal_pending = G.drift_have = 0;
    G.drift_tries = G.drift_recals = G.drift_corrections = G.drift_checks = 0;
    G.corr_unconfirmed = 0;
    G.cum_steps[0] = G.cum_steps[1] = 0;
    G.d_cum_max = hal_ptz_drift_cum_max(G.hp);
    G.drift_mark = 0;
    G.drift_recal_ms = G.last_person_ms = G.last_manual_ms = 0;
    G.drift_busy = 0;
    snprintf(G.recal_reason, sizeof G.recal_reason, "none");
    snprintf(G.drift_last, sizeof G.drift_last, "none");
    G.rh_urgent = 0;
    G.rh_urg_last = 0;
    G.rh_owed = G.go_home = G.cur_auto = G.pend.auto_home = G.rh_exact = 0;
    G.rung = G.drift_force = 0;
    G.inconc_runs = 0;
    G.drift_last_ms = 0;
    G.verify_mark = 0;
    G.dref_due = 0;
    G.dref_tries = 0;
    G.dref_ms = G.dref_retry_at = 0;
    memset(G.recal_hist, 0, sizeof G.recal_hist);
    G.recal_not_before = 0;
    G.rh_retry_at = G.rh_last_prev = G.rh_urg_last_prev = 0;
    G.rh_cancel_streak = 0;
    ring_reset_locked();
    G.cur_patrol = 0;
    G.pend.patrol = 0;
    G.privacy_opt = cfg->privacy;
    memset(&G.info, 0, sizeof G.info);
    g_privacy = 0;
    d = *hal_ptz_desc(G.hp);
    a2k_log("ptz: soft range pan %u..%u tilt %u..%u, home %u/%u, park %u/%u, speed %u, ptz_cal %s, ptz_privacy %s, ptz_scene_check %s",
            (unsigned)d.pan_min, (unsigned)d.pan_max, (unsigned)d.tilt_min, (unsigned)d.tilt_max, (unsigned)d.pan_home,
            (unsigned)d.tilt_home, (unsigned)d.pan_park, (unsigned)d.tilt_park, (unsigned)d.speed,
            cfg->cal == HAL_PTZ_CAL_OFF ? "off" : "auto", cfg->privacy ? "on" : "off", cfg->scene_check && G.scene.boot_check ? "on" : "off");
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 64 * 1024);
    if (pthread_create(&G.tid, &at, ptz_thread, NULL) != 0) {
        pthread_attr_destroy(&at);
        a2k_log("ptz: thread start failed: PTZ off");
        hal_ptz_free(G.hp);
        G.hp = NULL;
        return -1;
    }
    pthread_attr_destroy(&at);
    G.running = 1;
    return 0;
}

void a2k_ptz_stop(void)
{
    hal_ptz_t *hp;
    if (!G.running)
        return;
    pthread_mutex_lock(&G.mu);
    /* A clean stop at a known, settled position: the reference is saved before the thread ends (a frame taken right after a motion is not). */
    G.stop_commit = G.scene_dirty && G.scene_cand && !G.starting && !G.busy && G.pend.kind == K_NONE &&
                    mono_ms() - G.cand_ms >= A2K_PTZ_SCENE_QUIET_STOP_MS;
    G.stop = 1;
    if (G.hp && !G.stop_commit)
        hal_ptz_cancel(G.hp); /* the running motion (or calibration) ends within about 50 ms (idle: nothing to cancel, the driver is read by the thread) */
    pthread_cond_signal(&G.cv);
    pthread_mutex_unlock(&G.mu);
    pthread_join(G.tid, NULL);
    pthread_mutex_lock(&G.mu); /* a2k_ptz_range / submit (the tracker, the detection thread) read these under the lock */
    G.running = 0;
    g_privacy = 0;
    hp = G.hp;
    G.hp = NULL;
    pthread_mutex_unlock(&G.mu);
    hal_ptz_free(hp);
}

/* 1 while a MANUAL velocity hold is on (under G.mu): the dead-man time of the last manual command has not run out. */
static int manual_hold_locked(void) { return ptzl_live(&G.man, mono_ms()); }
static int manual_hold_active_locked(void) { return manual_hold_locked(); }

/* Queue under the lock. A nudge is refused unless idle (never queues, never cancels); any other command replaces the pending one and cancels the running motion.
 * A patrol move (patrol = 1) is accepted only while the controller is completely idle (it never preempts anything, 1 = busy, retry); a manual command
 * (hook >= 0) tells the patrol hook once it is accepted. */
static int submit2(int kind, int a, int b, int sp, int patrol, unsigned *ticket, int hook)
{
    int rc = -1;
    pthread_mutex_lock(&G.mu);
    if (kind == K_NUDGE || patrol)
        G.last_nudge_ms = mono_ms(); /* a tracker or a patrol is active (also when its move is refused): no multi-view excursion */
    if (!patrol)
        __atomic_store_n(&g_hook_abort, 1, __ATOMIC_SEQ_CST); /* any command or tracker nudge: a check / capture in progress gives way */
    if (G.running && !G.stop && !G.starting) {
        if (patrol && (G.busy || G.pend.kind != K_NONE)) {
            pthread_mutex_unlock(&G.mu);
            return 1;
        }
        if (G.drift_busy) { /* the SHIFT window (declare + home + re-verify, about 2 s) is a calibration in all but name: every queued command (step, home, park, goto, tracker home) is refused with the
                             * code of the manual gate, none cancels the correction. A manual Stop (a2k_ptz_manual_stop) has nothing to stop: the window itself is never cancelled and no command is pending. */
            pthread_mutex_unlock(&G.mu);
            return A2K_PTZ_E_NOTREADY;
        }
        if (hook < 0 && !patrol && G.busy && (G.cur_kind == K_REHOME || (G.cur_auto && kind != K_HOME))) { /* a tracker command: somebody is around, the re-home / the automatic return gives way (the tracker asks again next tick) */
            rehome_yield_locked();
            autohome_yield_locked();
            pthread_mutex_unlock(&G.mu);
            return -1;
        }
        if (hook < 0 && !patrol && manual_hold_locked()) { /* a MANUAL velocity hold (ONVIF / API / MQTT) wins over the tracker's nudge and home return */
            pthread_mutex_unlock(&G.mu);
            return -1;
        }
        if (kind == K_NUDGE && hook >= 0 && ((G.vsess && G.pend.kind == K_NONE) || (G.busy && G.cur_kind == K_REHOME))) { /* a MANUAL nudge (API ptz.move) ends a tracker's continuous session / a re-home and runs */
            hal_ptz_cancel(G.hp);
        } else if (kind == K_NUDGE && (G.busy || G.pend.kind != K_NONE)) {
            pthread_mutex_unlock(&G.mu); /* a nudge (tracking) is accepted only while idle: it never queues behind, replaces or outlives a step, home or park (no un-park) */
            return -1;
        }
        {
            if (hook >= 0 && !patrol)
                ptzl_clear(&G.man); /* an accepted manual command supersedes the velocity hold */
            G.pend.kind = kind;
            G.pend.auto_home = 0;
            if (hook >= 0 && !patrol)
                G.go_home = 0; /* a manual command: the user decides where the head goes */
            G.pend.a = a;
            G.pend.b = b;
            G.pend.sp = sp;
            G.pend.patrol = patrol;
            if (patrol) {
                G.pend.ticket = ++G.pt_next;
                if (ticket)
                    *ticket = G.pend.ticket;
            }
            if (kind != K_NUDGE)
                hal_ptz_cancel(G.hp);
        }
        if (!patrol && hook >= 0 && G.cmd_hook)
            G.cmd_hook(hook);
        pthread_cond_signal(&G.cv);
        rc = 0;
    }
    pthread_mutex_unlock(&G.mu);
    return rc;
}

static int submit(int kind, int a, int b, int sp, int hook) { return submit2(kind, a, b, sp, 0, NULL, hook); }

void a2k_ptz_set_cmd_hook(void (*fn)(int kind))
{
    pthread_mutex_lock(&G.mu);
    G.cmd_hook = fn;
    pthread_mutex_unlock(&G.mu);
}

int a2k_ptz_park(int *pan, int *tilt)
{
    int rc = -1;
    pthread_mutex_lock(&G.mu);
    if (G.running && G.hp) {
        const hal_ptz_desc_t *d = hal_ptz_desc(G.hp);
        *pan = (int)d->pan_park;
        *tilt = (int)d->tilt_park;
        rc = 0;
    }
    pthread_mutex_unlock(&G.mu);
    return rc;
}

int a2k_ptz_scene_pending(void)
{
    int v;
    pthread_mutex_lock(&G.mu);
    v = G.scene_dirty || G.scene_cand || G.mv_req;
    pthread_mutex_unlock(&G.mu);
    return v;
}

int a2k_ptz_home_tracker(void) { return submit(K_HOME, 0, 0, 0, -1); }

int a2k_ptz_speeds(int *min, int *max, int *def)
{
    int rc = -1;
    pthread_mutex_lock(&G.mu);
    if (G.running && G.hp) {
        *min = HAL_PTZ_SPEED_MIN;
        *max = HAL_PTZ_SPEED_MAX;
        *def = (int)hal_ptz_desc(G.hp)->speed;
        rc = 0;
    }
    pthread_mutex_unlock(&G.mu);
    return rc;
}

static int goto_checked(int pan, int tilt, int speed, int patrol, unsigned *ticket, int hook)
{
    uint32_t a, b, c, d;
    if (speed != 0 && (speed < HAL_PTZ_SPEED_MIN || speed > HAL_PTZ_SPEED_MAX))
        return -1;
    if (a2k_ptz_range(&a, &b, &c, &d) != 0 || pan < (int)a || pan > (int)b || tilt < (int)c || tilt > (int)d)
        return -1;
    if (patrol) { /* the patrol never lands on the park position: that would turn the privacy mode on */
        int pp, pt;
        if (a2k_ptz_park(&pp, &pt) != 0 || (abs(pan - pp) <= A2K_PTZ_PARK_TOL && abs(tilt - pt) <= A2K_PTZ_PARK_TOL))
            return -1;
    }
    return submit2(K_GOTO, pan, tilt, speed, patrol, ticket, hook);
}

int a2k_ptz_goto(int pan, int tilt, int speed) { return goto_checked(pan, tilt, speed, 0, NULL, A2K_PTZ_HOOK_GOTO); }

int a2k_ptz_patrol_goto(int pan, int tilt, int speed, unsigned *ticket) { return goto_checked(pan, tilt, speed, 1, ticket, -1); }

int a2k_ptz_patrol_result(unsigned ticket, int *rc)
{
    int done = 1;
    pthread_mutex_lock(&G.mu);
    if ((G.pend.kind == K_GOTO && G.pend.patrol && G.pend.ticket == ticket) || (G.busy && G.cur_patrol && G.cur_ticket == ticket))
        done = 0;
    else
        *rc = G.pt_done == ticket ? G.pt_rc : HAL_PTZ_ECANCEL; /* a move that never ran (replaced by a command) counts as cancelled */
    pthread_mutex_unlock(&G.mu);
    return done;
}

void a2k_ptz_patrol_cancel(void)
{
    pthread_mutex_lock(&G.mu);
    if (G.pend.kind == K_GOTO && G.pend.patrol) {
        G.pend.kind = K_NONE;
        G.pend.patrol = 0;
    }
    if (G.busy && G.cur_patrol && G.hp)
        hal_ptz_cancel(G.hp);
    pthread_mutex_unlock(&G.mu);
}

/* The step directions are what the viewer sees (displayed frame); the driver's HAL_PTZ_* are sensor-frame directions (orient.h): a mirror swaps left / right, a flip up / down. */
static int step_dir(int cmd)
{
    int flip, mirror, sx = 0, sy = 0;
    a2k_orient_get(&flip, &mirror);
    if (cmd == A2K_PTZ_LEFT || cmd == A2K_PTZ_RIGHT)
        sx = a2k_orient_pan_sign(cmd == A2K_PTZ_RIGHT ? 1 : -1, mirror);
    else
        sy = a2k_orient_tilt_sign(cmd == A2K_PTZ_UP ? 1 : -1, flip);
    return sx ? (sx > 0 ? HAL_PTZ_RIGHT : HAL_PTZ_LEFT) : (sy > 0 ? HAL_PTZ_UP : HAL_PTZ_DOWN);
}

int a2k_ptz_command(int cmd)
{
    switch (cmd) {
    case A2K_PTZ_LEFT:
    case A2K_PTZ_RIGHT:
    case A2K_PTZ_UP:
    case A2K_PTZ_DOWN: return submit(K_STEP, step_dir(cmd), 0, 0, cmd);
    case A2K_PTZ_HOME: return submit(K_HOME, 0, 0, 0, cmd);
    case A2K_PTZ_PARK: return submit(K_PARK, 0, 0, 0, cmd);
    case A2K_PTZ_RECAL: return G.cal_mode == HAL_PTZ_CAL_OFF ? -1 : submit(K_RECAL, 0, 0, 0, cmd); /* ptz_cal=off: the motor is never touched */
    default: return -1;
    }
}

int a2k_ptz_toggle_park(void)
{
    a2k_ptz_info_t i;
    a2k_ptz_info(&i);
    return a2k_ptz_command(i.state == HAL_PTZ_PARKED ? A2K_PTZ_HOME : A2K_PTZ_PARK);
}

/* the tracker's nudge (no manual hook: a tracker never stops a patrol, it pauses it) */
int a2k_ptz_nudge_sp(int dx, int dy, int speed)
{
    if (!dx && !dy)
        return 0;
    if (speed != 0 && (speed < 2 || speed > 20))
        return -1;
    return submit(K_NUDGE, dx, dy, speed, -1);
}

int a2k_ptz_vel(int sx, int sy, int dx, int dy)
{
    int rc = -1;
    if (sx < -1 || sx > 1 || sy < -1 || sy > 1 || (sx && dx && (dx < HAL_PTZ_SPEED_MIN || dx > HAL_PTZ_SPEED_MAX)) || (sy && dy && (dy < HAL_PTZ_SPEED_MIN || dy > HAL_PTZ_SPEED_MAX)))
        return -1;
    pthread_mutex_lock(&G.mu);
    if (sx || sy) {
        __atomic_store_n(&g_hook_abort, 1, __ATOMIC_SEQ_CST);            /* a check / capture in progress gives way */
        G.last_nudge_ms = mono_ms(); /* a tracker is active: no multi-view excursion */
        rehome_yield_locked();       /* ... and a running tilt re-home gives way (this command is refused while it ends, the tracker asks again) */
        autohome_yield_locked();     /* ... and so does the automatic return home */
    }
    if (G.running && !G.stop && !G.starting) {
        if (manual_hold_locked()) { /* a manual velocity command is live: it wins, the tracker asks again next tick */
            rc = -1;
        } else if (G.vsess && G.vending) { /* the session is ending: not accepted (a refresh would be lost) */
            rc = -1;
        } else if (G.vsess) { /* a session runs: the next poll (<= 50 ms) takes it */
            G.vel_man = 0;
            G.vel_lease = G.lease_ms;
            G.vel.sx = sx;
            G.vel.sy = sy;
            G.vel.dx = sx ? dx : 0;
            G.vel.dy = sy ? dy : 0;
            G.vel_seq++;
            ptzl_arm(&G.dm, mono_ms(), G.vel_lease);
            rc = 0;
        } else if (!sx && !sy) {
            if (G.pend.kind == K_VEL)
                G.pend.kind = K_NONE; /* a stop cancels a start that has not run yet */
            rc = 0; /* nothing runs: nothing to stop */
        } else if (!G.busy && G.pend.kind == K_NONE) {
            G.vel_man = 0;
            G.vel_lease = G.lease_ms;
            G.pend.kind = K_VEL;
            G.pend.a = (sx + 1) * 4 + (sy + 1);
            G.pend.b = (sx ? dx : 0) * 32 + (sy ? dy : 0);
            G.pend.sp = 0;
            G.pend.patrol = 0;
            pthread_cond_signal(&G.cv);
            rc = 0;
        }
    }
    pthread_mutex_unlock(&G.mu);
    return rc;
}

/* ---- MANUAL continuous motion (ONVIF ContinuousMove / Stop, the API ptz.vel, MQTT): the same session as the tracker's, with its own lease and the priority of a manual command. ---- */

/* The state gate of a manual motion (under G.mu): 0, or A2K_PTZ_E_*. Parked (privacy) and calibrating are refused; a head whose position is unknown (ptz_cal=off) too. */
static int manual_gate_locked(void)
{
    if (!G.running || G.stop || !G.hp)
        return A2K_PTZ_E_REFUSED;
    if (G.starting || G.drift_busy || G.info.state == HAL_PTZ_CALIBRATING || G.info.state == HAL_PTZ_UNKNOWN) /* drift_busy: the SHIFT re-declare + re-verify window is a calibration in all but name */
        return A2K_PTZ_E_NOTREADY;
    if (G.info.state == HAL_PTZ_PARKED || g_privacy)
        return A2K_PTZ_E_PARKED;
    return 0;
}

int a2k_ptz_manual_gate(void)
{
    int rc;
    pthread_mutex_lock(&G.mu);
    rc = manual_gate_locked();
    pthread_mutex_unlock(&G.mu);
    return rc;
}

int a2k_ptz_vel_manual(int sx, int sy, int dx, int dy, int lease_ms)
{
    int rc;
    int64_t now;
    if (sx < -1 || sx > 1 || sy < -1 || sy > 1 || (sx && (dx < HAL_PTZ_SPEED_MIN || dx > HAL_PTZ_SPEED_MAX)) ||
        (sy && (dy < HAL_PTZ_SPEED_MIN || dy > HAL_PTZ_SPEED_MAX)) || lease_ms < 150 || lease_ms > 2000)
        return A2K_PTZ_E_ARGS;
    if (!sx && !sy)
        return a2k_ptz_manual_stop();
    { /* right / up as the viewer sees them (displayed frame, what ONVIF, ptz.vel and MQTT mean) -> the sensor-frame directions of the driver (orient.h) */
        int flip, mirror;
        a2k_orient_get(&flip, &mirror);
        sx = a2k_orient_pan_sign(sx, mirror);
        sy = a2k_orient_tilt_sign(sy, flip);
    }
    pthread_mutex_lock(&G.mu);
    rc = manual_gate_locked();
    if (rc) {
        pthread_mutex_unlock(&G.mu);
        return rc;
    }
    now = mono_ms();
    __atomic_store_n(&g_hook_abort, 1, __ATOMIC_SEQ_CST);
    G.last_nudge_ms = now; /* no multi-view excursion */
    G.last_manual_ms = now + lease_ms; /* drift machine: a hold is motion activity, its idle timers (check, recalibration) start after it */
    ptzl_arm(&G.man, now, lease_ms);
    G.vel_man = 1;
    G.vel_lease = lease_ms;
    if (G.vsess && !G.vending) { /* a session runs (the tracker's or ours): re-target / refresh it, the next poll (<= 50 ms) takes it; an identical command sends nothing to the driver */
        G.vel.sx = sx;
        G.vel.sy = sy;
        G.vel.dx = sx ? dx : 0;
        G.vel.dy = sy ? dy : 0;
        G.vel_seq++;
        ptzl_arm(&G.dm, now, lease_ms);
    } else { /* idle, another command runs, or the session is ending: like any manual command it replaces the pending one and cancels the running motion */
        G.pend.kind = K_VEL;
        G.pend.a = (sx + 1) * 4 + (sy + 1);
        G.pend.b = (sx ? dx : 0) * 32 + (sy ? dy : 0);
        G.pend.sp = 0;
        G.pend.patrol = 0;
        if (G.busy)
            hal_ptz_cancel(G.hp);
        pthread_cond_signal(&G.cv);
    }
    if (G.cmd_hook)
        G.cmd_hook(A2K_PTZ_HOOK_NUDGE); /* a manual command: it stops a patrol (atomic stores only) */
    pthread_mutex_unlock(&G.mu);
    return 0;
}

/* Stop at once: ends the hold, drops a pending goto / step / nudge / velocity start and cancels the running motion (about 50 ms). A running park, calibration or
 * boot start is never cancelled by a stop. Always 0 (nothing to stop is not an error). */
int a2k_ptz_manual_stop(void)
{
    pthread_mutex_lock(&G.mu);
    ptzl_clear(&G.man);
    if (G.last_manual_ms > mono_ms())
        G.last_manual_ms = mono_ms(); /* the idle timers count from the Stop */
    if (G.running && !G.stop && !G.starting) {
        int k = G.pend.kind;
        if (!G.pend.patrol && (k == K_VEL || k == K_GOTO || k == K_STEP || k == K_NUDGE || k == K_HOME))
            G.pend.kind = K_NONE;
        if (G.busy && G.hp && !G.drift_busy && G.info.state != HAL_PTZ_CALIBRATING && G.cur_kind != K_PARK && G.cur_kind != K_RECAL) { /* the drift SHIFT window is never cancelled either */
            if (G.vsess && !G.vending) { /* the session ends by its own STOP (the mailbox), cleanly */
                G.vel.sx = G.vel.sy = G.vel.dx = G.vel.dy = 0;
                G.vel_seq++;
            }
            hal_ptz_cancel(G.hp);
            if (G.cur_patrol && G.cmd_hook)
                G.cmd_hook(A2K_PTZ_HOOK_NUDGE); /* the stop was aimed at a patrol move: the patrol ends */
        }
    }
    pthread_mutex_unlock(&G.mu);
    return 0;
}

/* a manual nudge (the API ptz.move): accepted only while idle, and a patrol stops when it is accepted */
int a2k_ptz_nudge(int dx, int dy)
{
    uint32_t pa, pb, ta, tb;
    a2k_ptz_info_t i;
    if (!dx && !dy)
        return 0;
    a2k_ptz_info(&i);
    if (i.known && a2k_ptz_range(&pa, &pb, &ta, &tb) == 0) { /* the HAL clamps a nudge to the soft range: with no room on every commanded axis it moves nothing, which must not be reported as done */
        int room_p = dx > 0 ? (int)pb - (int)i.pan : dx < 0 ? (int)i.pan - (int)pa : 1, room_t = dy > 0 ? (int)tb - (int)i.tilt : dy < 0 ? (int)i.tilt - (int)ta : 1;
        if ((!dx || room_p <= 0) && (!dy || room_t <= 0))
            return A2K_PTZ_E_LIMIT;
    }
    return submit(K_NUDGE, dx, dy, 0, A2K_PTZ_HOOK_NUDGE);
}

void a2k_ptz_info(a2k_ptz_info_t *out)
{
    pthread_mutex_lock(&G.mu);
    *out = G.info;
    pthread_mutex_unlock(&G.mu);
}

int a2k_ptz_range(uint32_t *pan_min, uint32_t *pan_max, uint32_t *tilt_min, uint32_t *tilt_max)
{
    int rc = -1;
    pthread_mutex_lock(&G.mu);
    if (G.running && G.hp) {
        const hal_ptz_desc_t *d = hal_ptz_desc(G.hp);
        *pan_min = d->pan_min;
        *pan_max = d->pan_max;
        *tilt_min = d->tilt_min;
        *tilt_max = d->tilt_max;
        rc = 0;
    }
    pthread_mutex_unlock(&G.mu);
    return rc;
}

int a2k_ptz_tilt_home(void)
{
    int v = 0;
    pthread_mutex_lock(&G.mu);
    if (G.running && G.hp)
        v = (int)hal_ptz_desc(G.hp)->tilt_home;
    pthread_mutex_unlock(&G.mu);
    return v;
}

int a2k_ptz_privacy_active(void) { return g_privacy; }

unsigned long a2k_ptz_travel(void)
{
    unsigned long v;
    pthread_mutex_lock(&G.mu);
    v = travel_locked();
    pthread_mutex_unlock(&G.mu);
    return v;
}

void a2k_ptz_wear_counts(unsigned *pos, unsigned *scene)
{
    pthread_mutex_lock(&G.mu);
    *pos = 0; /* the position lives in RAM: no ptz.pos write exists any more (the key stays for the state API) */
    *scene = G.scene.writes ? G.scene.writes() : G.scene_writes;
    pthread_mutex_unlock(&G.mu);
}

void a2k_ptz_set_notify(void (*fn)(const a2k_ptz_info_t *))
{
    pthread_mutex_lock(&G.mu);
    G.notify = fn;
    pthread_mutex_unlock(&G.mu);
}
