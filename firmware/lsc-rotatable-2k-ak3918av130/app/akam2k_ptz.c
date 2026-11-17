/* PTZ controller of the Rotatable 2K own app (akam2k_ptz.h): one thread, one pending command, hal_ptz underneath. */
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "akam2k_hal.h"
#include "akam2k_log.h"
#include "akam2k_ptz.h"

enum { K_NONE, K_STEP, K_HOME, K_PARK, K_NUDGE, K_RECAL, K_GOTO, K_VEL };
#define RING_N 64 /* position samples of a continuous session (about 3 s at 20 Hz) */


static struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    pthread_t tid;
    int running, stop, starting;
    struct { int kind, a, b, sp, patrol; unsigned ticket; } pend;
    int cur_patrol;             /* the running command is a patrol move */
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
    int64_t vel_ms;             /* when the last one arrived */
    int lease_ms;
    struct { int64_t t; int pan, tilt; } ring[RING_N];
    int ring_n, ring_head;      /* count, index of the next write */
} G = { .mu = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER,
        .conf_path = A2K_PTZ_CONF, .lease_ms = A2K_PTZ_LEASE_MS,
        .scene_ms = A2K_PTZ_SCENE_MS, .scene_confirm_ms = A2K_PTZ_SCENE_CONFIRM_MS, .scene_gap_ms = A2K_PTZ_SCENE_GAP_MS };
static volatile int g_privacy;

void a2k_ptz_defaults(a2k_ptz_cfg_t *c)
{
    c->cal = HAL_PTZ_CAL_AUTO;
    c->park_pan = c->park_tilt = -1;
    c->privacy = 0;
    c->scene_check = 1;
    c->travel_budget = A2K_PTZ_TRAVEL_BUDGET;
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
    } else if (now - G.vel_ms > G.lease_ms) { /* the real-time dead-man (the safe layer has its own, counted in polls) */
        k = HAL_PTZ_CONT_STOP;
        G.vending = 1;
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
    G.vel_ms = now;
    lease = (unsigned)G.lease_ms + 150; /* the polled-time lease of the safe layer: a little longer than the real-time one above */
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
    static const char *const names[] = { "none", "step", "home", "park", "nudge", "recalibrate", "goto", "velocity" };
    static const char *const dirs[] = { " up", " right", " down", " left" };
    int rc = HAL_PTZ_EREFUSED;
    a2k_ptz_info_t i;
    if (kind != K_VEL)
        publish(kind == K_RECAL ? HAL_PTZ_CALIBRATING : HAL_PTZ_MOVED, 1); /* a session publishes at its first poll (vel_cb): a refused one leaves no trace */
    switch (kind) {
    case K_RECAL: rc = hal_ptz_recalibrate(G.hp); break;
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
 *     and the scene module compares the candidate with the stored reference and writes only when needed (see akam2k_scene.c: unchanged view =
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
    if (!G.scene.capture || !G.scene.commit || !G.scene_on || G.busy || G.pend.kind != K_NONE || !G.info.known || !at_home_locked()) {
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
    if (!G.scene.commit || !G.scene_on || G.busy || G.pend.kind != K_NONE || !i.known || !G.scene_cand || !at_home_locked()) {
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
    for (;;) {
        int kind, a, b, sp, patrol, rc;
        unsigned ticket;
        while (!G.stop && G.pend.kind == K_NONE) {
            long wait_ms = scene_wait_locked();
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
                    if (G.scene_cand) {
                        scene_commit_locked("idle");
                    } else if (scene_capture_start_locked()) {
                        int rc;
                        G.busy = 1; /* nothing else runs on this thread meanwhile; a command is only queued (and runs after) */
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
        G.vsess = 0;
        G.vending = 0;
        G.busy = 0;
        G.starting = 0;
        G.last_end_ms = mono_ms();
        G.scene_cand = 0; /* a candidate taken before the command is void (the head moved) */
        G.scene_tries = 0;
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
    G.travel_budget = cfg->travel_budget ? cfg->travel_budget : A2K_PTZ_TRAVEL_BUDGET;
    G.scene_writes = 0;
    G.budget_logged = 0;
    G.scene_dirty = G.scene_tries = G.busy = G.scene_cand = G.scene_fresh = G.stop_commit = G.mv_req = G.mv_failed = G.mv_built = 0;
    G.last_nudge_ms = 0;
    G.last_save_ms = 0;
    G.vsess = 0;
    G.vel_seq = 0;
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

/* Queue under the lock. A nudge is refused unless idle (never queues, never cancels); any other command replaces the pending one and cancels the running motion.
 * A patrol move (patrol = 1) is accepted only while the controller is completely idle (it never preempts anything, 1 = busy, retry); a manual command
 * (hook >= 0) tells the patrol hook once it is accepted. */
static int submit2(int kind, int a, int b, int sp, int patrol, unsigned *ticket, int hook)
{
    int rc = -1;
    pthread_mutex_lock(&G.mu);
    if (kind == K_NUDGE || patrol)
        G.last_nudge_ms = mono_ms(); /* a tracker or a patrol is active (also when its move is refused): no multi-view excursion */
    if (G.running && !G.stop && !G.starting) {
        if (patrol && (G.busy || G.pend.kind != K_NONE)) {
            pthread_mutex_unlock(&G.mu);
            return 1;
        }
        if (kind == K_NUDGE && hook >= 0 && G.vsess && G.pend.kind == K_NONE) { /* a MANUAL nudge (API ptz.move) ends a tracker's continuous session and runs */
            hal_ptz_cancel(G.hp);
        } else if (kind == K_NUDGE && (G.busy || G.pend.kind != K_NONE)) {
            pthread_mutex_unlock(&G.mu); /* a nudge (tracking) is accepted only while idle: it never queues behind, replaces or outlives a step, home or park (no un-park) */
            return -1;
        }
        {
            G.pend.kind = kind;
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

int a2k_ptz_command(int cmd)
{
    switch (cmd) {
    case A2K_PTZ_LEFT: return submit(K_STEP, HAL_PTZ_LEFT, 0, 0, cmd);
    case A2K_PTZ_RIGHT: return submit(K_STEP, HAL_PTZ_RIGHT, 0, 0, cmd);
    case A2K_PTZ_UP: return submit(K_STEP, HAL_PTZ_UP, 0, 0, cmd);
    case A2K_PTZ_DOWN: return submit(K_STEP, HAL_PTZ_DOWN, 0, 0, cmd);
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
    if (sx || sy)
        G.last_nudge_ms = mono_ms(); /* a tracker is active: no multi-view excursion */
    if (G.running && !G.stop && !G.starting) {
        if (G.vsess && G.vending) { /* the session is ending: not accepted (a refresh would be lost) */
            rc = -1;
        } else if (G.vsess) { /* a session runs: the next poll (<= 50 ms) takes it */
            G.vel.sx = sx;
            G.vel.sy = sy;
            G.vel.dx = sx ? dx : 0;
            G.vel.dy = sy ? dy : 0;
            G.vel_seq++;
            G.vel_ms = mono_ms();
            rc = 0;
        } else if (!sx && !sy) {
            if (G.pend.kind == K_VEL)
                G.pend.kind = K_NONE; /* a stop cancels a start that has not run yet */
            rc = 0; /* nothing runs: nothing to stop */
        } else if (!G.busy && G.pend.kind == K_NONE) {
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

/* a manual nudge (the API ptz.move): accepted only while idle, and a patrol stops when it is accepted */
int a2k_ptz_nudge(int dx, int dy)
{
    if (!dx && !dy)
        return 0;
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
