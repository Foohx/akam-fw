/* Host simulation of the Rotatable 2K person tracking, three configurations on the SAME scenarios, so that the numbers are comparable:
 *   DEV   the policy before the targeting work: the upper-third / central-band rules, timed nudges (tl_cfg_dev)
 *   STEPS the new targeting (comfort zone, body / head reference, 3D Kalman state) but still timed nudges (track_motion=steps)
 *   CONT  the new targeting with continuous motion (track_motion=continuous): one ongoing move re-targeted without a stop, frames positioned by the driver counters
 * The REAL wiring (akam2k_track.c + track_logic.c) runs against a simulated room: persons walking / crouching / approaching in metres (a pinhole camera with the
 * lens the policy assumes: 1000 steps per frame width, 700 per height, 654 steps per radian; the camera 1.0 m above the floor), a detector at 5 Hz with 63..110 ms inference,
 * a 50 ms video latency, box jitter, arm swing, partial boxes at the frame edges, motion blur (misses while the head turns fast), scripted occlusions, and a FAKE MOTOR:
 *   steps: one nudge at a time (30 ms start, 1 step per divisor ms per axis, pan then tilt, 70 ms stop), the position is only known when idle;
 *   continuous: 1100 / divisor steps/s per axis at once (diagonals), 30 ms before a command takes effect, a stop is immediate, a lease of 450 ms (dead-man), soft range per axis,
 *               the position is sampled every 50 ms (pos_at).
 * Metrics per scenario: % of the time the person's reference is inside the comfort zone (+-20 % of the width) / the frame, motion starts (a nudge, or a continuous
 * move that begins from rest), the largest offset (px), tilt travel. Asserts: the person never leaves the frame because of the camera's own motion (feints, sprints), the
 * soft range is never left, the lease stops a move nobody refreshes, and the continuous mode starts / stops far less than the nudges.
 * Prints the tables and "rot2k track cont sim: N failed". */
#define _GNU_SOURCE
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "akam2k_store.h"
#include "akam2k_track.h"

static int fails;
#define CHECK(c, msg) do { if (c) printf("  ok   - %s\n", msg); else { printf("  FAIL - %s\n", msg); fails++; } } while (0)
void a2k_log(const char *fmt, ...) { (void)fmt; }

#define DT 5
#define FW 320
#define FH 192
#define PAN_MIN 300
#define PAN_MAX 3650
#define TILT_MIN 300
#define TILT_MAX 2100
#define HOME_PAN 1275
#define HOME_TILT 1300
#define KRAD 654.0                 /* motor steps per radian */
#define FHZ 209.5                  /* px per radian, horizontal: 320 px = 1000 steps */
#define FV 179.4                   /* px per radian, vertical: 192 px = 700 steps */
#define CAM_H 1.0                  /* camera height, m */
#define MAXT 20000                 /* ms */
#define RATE 1100.0                /* steps/s = RATE / divisor */
#define LEASE_MS 450

enum { M_DEV, M_STEPS, M_CONT };
static const char *const MODE[3] = { "DEV  ", "STEPS", "CONT " };

/* ------------------------------------------------------------------ the fake motor */
static struct {
    double pan, tilt;
    int mode;
    long now;
    /* steps */
    int active, phase, sp;
    double tp, tt;
    long start_at, settle_until, end_ms;
    int pend, pdx, pdy, psp;
    /* continuous */
    int sess;                      /* a move is running (commanded) */
    int csx, csy, cdx, cdy;        /* the command in effect */
    int nsx, nsy, ndx, ndy;        /* the command waiting for its latency */
    long napply, last_vel;
    /* history */
    double hp[MAXT / DT + 8], ht[MAXT / DT + 8];
    int nh;
    double rs_t[MAXT / 50 + 8], rs_p[MAXT / 50 + 8], rs_q[MAXT / 50 + 8];  /* the 50 ms position samples of a continuous session */
    int nrs;
    long last_rs;
    int starts, nudges, vels;
    int was_moving;
    double tilt_travel, pan_travel;
    int range_violation, bad_speed;
    double speed_pan;              /* current commanded pan speed, steps/s (for the blur model) */
} P;

static void p_reset(int mode)
{
    memset(&P, 0, sizeof P);
    P.mode = mode;
    P.pan = HOME_PAN;
    P.tilt = HOME_TILT;
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static void f_info(a2k_ptz_info_t *o)
{
    memset(o, 0, sizeof *o);
    o->state = HAL_PTZ_MOVED;
    {
        int moving = P.mode == M_CONT ? (P.sess || P.napply) : P.active;
        if (!moving && fabs(P.pan - HOME_PAN) < 6 && fabs(P.tilt - HOME_TILT) < 6) o->state = HAL_PTZ_HOME;
        o->known = !moving;
    }
    o->pan = (uint32_t)(P.pan + 0.5);
    o->tilt = (uint32_t)(P.tilt + 0.5);
    o->motion_end_ms = P.end_ms;
}
static int f_range(int r[4]) { r[0] = PAN_MIN; r[1] = PAN_MAX; r[2] = TILT_MIN; r[3] = TILT_MAX; return 0; }
static int f_priv(void) { return 0; }

static void p_start(int dx, int dy, int sp)
{
    P.tp = clampi((int)P.pan + dx, PAN_MIN, PAN_MAX);
    P.tt = clampi((int)P.tilt + dy, TILT_MIN, TILT_MAX);
    P.sp = sp > 0 ? sp : 3;
    P.active = 1;
    P.phase = 0;
    P.start_at = P.now + 30;
}

static int f_nudge(int dx, int dy, int sp)
{
    if (sp != 0 && (sp < 2 || sp > 20)) P.bad_speed++;
    if (P.active) return -1; /* the controller refuses a nudge while busy */
    P.nudges++;
    p_start(dx, dy, sp);
    return 0;
}
static int f_home(void) { if (!P.active) p_start(HOME_PAN - (int)P.pan, HOME_TILT - (int)P.tilt, 3); return 0; }

static int f_vel(int sx, int sy, int dx, int dy)
{
    if (P.active) return -1;
    if ((sx && (dx < 2 || dx > 20)) || (sy && (dy < 2 || dy > 20))) { P.bad_speed++; return -1; }
    P.vels++;
    if (!sx && !sy) { /* stop: immediate */
        P.csx = P.csy = P.nsx = P.nsy = 0;
        P.napply = 0;
        if (P.sess) { P.sess = 0; P.end_ms = P.now; }
        return 0;
    }
    P.last_vel = P.now;
    if (!P.sess && !P.napply) P.sess = 1, P.napply = 0;
    if (sx != P.csx || sy != P.csy || (sx && dx != P.cdx) || (sy && dy != P.cdy)) {
        P.nsx = sx; P.nsy = sy; P.ndx = dx; P.ndy = dy;
        P.napply = P.now + 30;
    }
    P.sess = 1;
    return 0;
}

static int f_pos_at(int64_t t, int *pan, int *tilt)
{
    int i;
    if (P.nrs == 0) { if (t >= 0) { *pan = (int)(P.pan + 0.5); *tilt = (int)(P.tilt + 0.5); return 0; } return -1; }
    if (t >= P.rs_t[P.nrs - 1]) {
        if (P.sess && t - (long)P.rs_t[P.nrs - 1] > 200) return -1;
        *pan = (int)(P.rs_p[P.nrs - 1] + 0.5);
        *tilt = (int)(P.rs_q[P.nrs - 1] + 0.5);
        return 0;
    }
    if (t < P.rs_t[0]) return -1;
    for (i = 0; i + 1 < P.nrs; i++)
        if (t >= P.rs_t[i] && t < P.rs_t[i + 1]) {
            double f = (t - P.rs_t[i]) / (P.rs_t[i + 1] - P.rs_t[i]);
            *pan = (int)(P.rs_p[i] + (P.rs_p[i + 1] - P.rs_p[i]) * f + 0.5);
            *tilt = (int)(P.rs_q[i] + (P.rs_q[i + 1] - P.rs_q[i]) * f + 0.5);
            return 0;
        }
    return -1;
}

static void p_step(void)
{
    double op = P.pan, ot = P.tilt;
    int moving = 0;
    P.speed_pan = 0;
    if (P.mode == M_CONT) {
        if (P.napply && P.now >= P.napply) { P.csx = P.nsx; P.csy = P.nsy; P.cdx = P.ndx; P.cdy = P.ndy; P.napply = 0; }
        if (P.sess && P.now - P.last_vel > LEASE_MS) { P.sess = 0; P.csx = P.csy = 0; P.end_ms = P.now; }
        if (P.sess) {
            double st = (double)DT / 1000.0;
            int sx = P.csx, sy = P.csy;
            /* the safe layer drops an axis near its soft limit */
            if (sx > 0 && PAN_MAX - P.pan <= 45) sx = 0;
            if (sx < 0 && P.pan - PAN_MIN <= 45) sx = 0;
            if (sy > 0 && TILT_MAX - P.tilt <= 45) sy = 0;
            if (sy < 0 && P.tilt - TILT_MIN <= 45) sy = 0;
            if (sx) { P.pan += sx * RATE / P.cdx * st; P.speed_pan = RATE / P.cdx; moving = 1; }
            if (sy) { P.tilt += sy * RATE / P.cdy * st; moving = 1; }
            if (!sx && !sy && (P.csx || P.csy)) { P.sess = 0; P.csx = P.csy = 0; P.end_ms = P.now; }
        }
        if (P.now - P.last_rs >= 50) {
            P.last_rs = P.now;
            if (P.nrs < MAXT / 50 + 4) { P.rs_t[P.nrs] = (double)P.now; P.rs_p[P.nrs] = P.pan; P.rs_q[P.nrs] = P.tilt; P.nrs++; }
        }
    } else if (P.active && P.now >= P.start_at) {
        double st = (double)DT / P.sp;
        moving = 1;
        if (P.phase == 0) {
            if (fabs(P.tp - P.pan) <= st) { P.pan = P.tp; P.phase = 1; }
            else { P.pan += P.tp > P.pan ? st : -st; P.speed_pan = 1000.0 / P.sp; }
        } else if (P.phase == 1) {
            if (fabs(P.tt - P.tilt) <= st) { P.tilt = P.tt; P.phase = 2; P.settle_until = P.now + 70; }
            else P.tilt += P.tt > P.tilt ? st : -st;
        } else if (P.now >= P.settle_until) {
            P.active = 0;
            P.end_ms = P.now;
        }
    }
    P.pan_travel += fabs(P.pan - op);
    P.tilt_travel += fabs(P.tilt - ot);
    {
        int any = moving || P.active || P.sess;
        if (any && !P.was_moving) P.starts++;
        P.was_moving = any;
    }
    if (P.pan < PAN_MIN - 1 || P.pan > PAN_MAX + 1 || P.tilt < TILT_MIN - 1 || P.tilt > TILT_MAX + 1) P.range_violation++;
    if (P.nh < MAXT / DT + 4) { P.hp[P.nh] = P.pan; P.ht[P.nh++] = P.tilt; }
}

/* ------------------------------------------------------------------ the room */
typedef struct { double x, z, h; int on; } person_t;   /* lateral position (m, right +), depth (m), height (m) */
typedef struct {
    const char *name;
    int total_ms;
    void (*who)(long t, person_t pp[2]);
    int occl[2][4][2];                              /* occlusion windows [person][k][from,to) ms */
    int track_person;                               /* 0 = person 0 is the one that counts */
    int blind_fast;                                 /* >0: no detection while the camera turns faster than this (steps/s) */
    int blind_until;                                /* ... only until this time (ms), 0 = always */
} scen_t;

static unsigned g_rng = 777;
static double rndu(void) { g_rng = g_rng * 1103515245u + 12345u; return ((g_rng >> 16) & 0x7fff) / 32767.0; }

/* pose at a past instant (ms) from the 5 ms history */
static void pose_at(long t, double *pan, double *tilt)
{
    int i = (int)(t / DT);
    if (i < 0) i = 0;
    if (i >= P.nh) i = P.nh - 1;
    *pan = P.hp[i];
    *tilt = P.ht[i];
}

/* the projection of person pp for a camera at (pan, tilt); 1 and the (clipped) box when detectable */
static int render(const person_t *pp, double pan, double tilt, struct tl_box *b, int *cut, double *exv)
{
    double yaw = -(pan - HOME_PAN) / KRAD, psi = (tilt - HOME_TILT) / KRAD, th = atan2(pp->x, pp->z), rho = hypot(pp->x, pp->z), d, dlt = th - yaw;
    double xc, hw, y1, y2, ar0, ar1, cx1, cx2, cy1, cy2;
    if (fabs(dlt) > 1.2) return 0;
    d = rho * cos(dlt);
    if (d < 0.3) return 0;
    xc = FW / 2.0 + FHZ * tan(dlt);
    hw = FHZ * 0.25 / d;
    y1 = FH / 2.0 - FV * tan(atan((pp->h - CAM_H) / rho) - psi);
    y2 = FH / 2.0 - FV * tan(atan((0.0 - CAM_H) / rho) - psi);
    cx1 = xc - hw; cx2 = xc + hw; cy1 = y1; cy2 = y2;
    ar0 = (cx2 - cx1) * (cy2 - cy1);
    if (cx1 < 0) cx1 = 0;
    if (cx2 > FW - 1) cx2 = FW - 1;
    if (cy1 < 0) cy1 = 0;
    if (cy2 > FH - 1) cy2 = FH - 1;
    if (cx2 <= cx1 || cy2 <= cy1) return 0;
    ar1 = (cx2 - cx1) * (cy2 - cy1);
    if (ar1 < 0.4 * ar0 || cy2 - cy1 < 18) return 0;
    *cut = (y1 < 0) || (y2 > FH - 1) || (xc - hw < 0) || (xc + hw > FW - 1);
    b->x1 = (int)(cx1 + 0.5); b->x2 = (int)(cx2 + 0.5); b->y1 = (int)(cy1 + 0.5); b->y2 = (int)(cy2 + 0.5);
    *exv = xc - FW / 2.0;
    return 1;
}

/* ------------------------------------------------------------------ one run */
typedef struct {
    double in_zone, in_frame, max_off, tilt_travel, pan_travel, follow_a;
    int starts, cmds, range_viol, bad_speed, left_frame_ms;
    int reacq_ms;                                   /* occlusion scenario: ms from the end of the occlusion until the reference is inside the zone again */
    int sweep_ok, sweeps;
    double pan_min, pan_max;
} res_t;


static void cfg_for(int mode, struct tl_cfg *c)
{
    if (mode == M_DEV) tl_cfg_dev(c);
    else { tl_cfg_defaults(c); c->motion = mode == M_CONT ? TL_MOTION_CONT : TL_MOTION_STEPS; }
    c->invert_pan = 1;
    c->invert_tilt = 0;
    c->tilt_enable = 1;
    c->lead_pct = 70;
    c->return_home = 0;
}

static a2k_track_ops_t OPS;
static int g_debug;                                   /* track_debug=1 for the run */
static const char *g_area_conf;                       /* a tracking area (akam2k.conf text) for the area scenario */

static res_t run(const scen_t *s, int mode, int verbose)
{
    a2k_track_t t;
    struct tl_cfg c;
    res_t r;
    long tick = 200, nxt = 1000;
    int zone_n = 0, frame_n = 0, n = 0, occl_end = 0, reacq = -1, out_prev = 0;
    double half_zone = FW * 0.2;
    person_t pp[2];
    char tb[64];
    memset(&r, 0, sizeof r);
    g_rng = 777;
    p_reset(mode);
    OPS = (a2k_track_ops_t){ f_info, f_range, f_nudge, f_home, f_priv, NULL, NULL, NULL, mode == M_CONT ? f_vel : NULL, mode == M_CONT ? f_pos_at : NULL, NULL, NULL, NULL, NULL, NULL };
    a2k_track_init(&t, &OPS, NULL);
    cfg_for(mode, &c);
    (void)a2k_track_set_cfg(&t, &c);
    if (g_area_conf) (void)a2k_track_load(&t, g_area_conf);
    if (g_debug) a2k_track_set_debug(&t, 1);
    a2k_track_set_enabled(&t, 1);
    for (int k = 0; k < 4; k++) if (s->occl[0][k][1] > occl_end) occl_end = s->occl[0][k][1];
    (void)tick;
    for (P.now = 0; P.now < s->total_ms; P.now += DT) {
        long T = P.now;
        p_step();
        /* the scoring: the ideal reference of person 0 vs the true camera pose */
        s->who(T, pp);
        if (T >= 500) {
            struct tl_box b; int cut = 0; double ex = 0;
            n++;
            if (render(&pp[0], P.pan, P.tilt, &b, &cut, &ex)) {
                double yaw = -(P.pan - HOME_PAN) / KRAD, th = atan2(pp[0].x, pp[0].z);
                ex = FHZ * tan(th - yaw);
                if (fabs(ex) <= half_zone) zone_n++;
                if (fabs(ex) < FW / 2.0) frame_n++;
                if (fabs(ex) > r.max_off) r.max_off = fabs(ex);
                if (out_prev && reacq < 0 && T > occl_end && occl_end) reacq = (int)(T - occl_end);
                out_prev = out_prev && T <= occl_end ? out_prev : 0;
            } else {
                r.left_frame_ms += DT;
                r.max_off = FW;
            }
            if (occl_end && T == occl_end) out_prev = 1;
            if (occl_end && T > occl_end && reacq < 0) {
                double yaw = -(P.pan - HOME_PAN) / KRAD, th = atan2(pp[0].x, pp[0].z), e = FHZ * tan(th - yaw);
                if (fabs(e) <= half_zone) reacq = (int)(T - occl_end);
            }
            if (s->track_person >= 0 && pp[1].on) { /* two persons: does the camera follow person 0? */
                double yaw = -(P.pan - HOME_PAN) / KRAD, a0 = fabs(atan2(pp[0].x, pp[0].z) - yaw), a1 = fabs(atan2(pp[1].x, pp[1].z) - yaw);
                if (a0 < a1) r.follow_a += 1.0;
            }
        }
        if (T < nxt) continue;
        {   /* a detection tick at T: the frame was exposed (inference 63..110 ms + 50 ms video) before */
            long infer = 63 + (long)(rndu() * 47), E = T - infer - 50, cap = T - infer;
            double pan, tilt, spd = P.speed_pan;
            struct tl_box bx[2];
            int nb = 0;
            nxt = T + 200 + (long)(rndu() * 30);
            if (E < 0) continue;
            pose_at(E, &pan, &tilt);
            s->who(E, pp);
            for (int k = 0; k < 2; k++) {
                struct tl_box b; int cut = 0, oc = 0; double exv = 0;
                if (!pp[k].on) continue;
                for (int w = 0; w < 4; w++) if (E >= s->occl[k][w][0] && E < s->occl[k][w][1]) oc = 1;
                if (oc || !render(&pp[k], pan, tilt, &b, &cut, &exv)) continue;
                if (spd > 100 && rndu() < (spd > 300 ? 0.55 : 0.35)) continue;       /* motion blur */
                if (s->blind_fast && spd > s->blind_fast && (!s->blind_until || T < s->blind_until)) continue;
                b.x1 += (int)(rndu() * 7) - 3; b.x2 += (int)(rndu() * 7) - 3; b.y1 += (int)(rndu() * 5) - 2; b.y2 += (int)(rndu() * 5) - 2;
                if (rndu() < 0.12) { int ar = (int)((b.x2 - b.x1) * 0.12); b.x1 -= ar; b.x2 += ar; } /* an arm out */
                if (b.x1 < 0) b.x1 = 0;
                if (b.x2 > FW - 1) b.x2 = FW - 1;
                if (b.y1 < 0) b.y1 = 0;
                if (b.y2 > FH - 1) b.y2 = FH - 1;
                if (b.x2 - b.x1 >= 6 && b.y2 - b.y1 >= 10) bx[nb++] = b;
            }
            a2k_track_tick_ts(&t, T, cap, bx, nb, FW, FH);
            if (verbose)
                printf("   [picked %d,%d,%d,%d] T=%5ld n=%d %s pan=%4.0f tilt=%4.0f cmd=%d/%d why=%s dist=%d vx=%d ex=%d sx=%d\n", t.tl.dbg.box.x1, t.tl.dbg.box.y1, t.tl.dbg.box.x2, t.tl.dbg.box.y2, T, nb, nb ? (snprintf(tb, sizeof tb, "%d boxes: %d,%d,%d,%d", nb, bx[0].x1, bx[0].y1, bx[0].x2, bx[0].y2), tb) : "no box",
                       P.pan, P.tilt, P.csx, P.cdx, t.tl.dbg.why ? t.tl.dbg.why : "?", t.tl.dist_mm, t.tl.vx, t.tl.dbg.dp, P.sess);
            r.sweeps += t.tl.sweep_n;
        }
    }
    r.in_zone = 100.0 * zone_n / (n ? n : 1);
    r.in_frame = 100.0 * frame_n / (n ? n : 1);
    r.starts = P.starts;
    r.cmds = mode == M_CONT ? P.vels : P.nudges;
    r.tilt_travel = P.tilt_travel;
    r.pan_travel = P.pan_travel;
    r.range_viol = P.range_violation;
    r.bad_speed = P.bad_speed;
    r.reacq_ms = reacq;
    r.pan_min = 1e9; r.pan_max = -1e9;
    for (int i = 0; i < P.nh; i++) { if (P.hp[i] < r.pan_min) r.pan_min = P.hp[i]; if (P.hp[i] > r.pan_max) r.pan_max = P.hp[i]; }
    r.follow_a = 100.0 * r.follow_a / (n ? n : 1);
    a2k_track_destroy(&t);
    return r;
}

/* ------------------------------------------------------------------ scenarios */
static double g_v, g_z;
static void who_walk(long t, person_t pp[2])
{
    memset(pp, 0, 2 * sizeof pp[0]);
    pp[0].on = 1;
    pp[0].z = g_z;
    pp[0].h = 1.7;
    pp[0].x = t < 1000 ? 0 : g_v * (t - 1000) / 1000.0;
    if (pp[0].x > g_v * 7) pp[0].x = g_v * 7;
}
static void who_crouch(long t, person_t pp[2])
{
    memset(pp, 0, 2 * sizeof pp[0]);
    pp[0].on = 1; pp[0].z = g_z; pp[0].x = 0;
    pp[0].h = t < 2500 ? 1.7 : t < 3300 ? 1.7 - 0.7 * (t - 2500) / 800.0 : t < 5500 ? 1.0 : t < 6300 ? 1.0 + 0.7 * (t - 5500) / 800.0 : 1.7;
}
static void who_approach(long t, person_t pp[2])
{
    memset(pp, 0, 2 * sizeof pp[0]);
    pp[0].on = 1; pp[0].x = 0.25; pp[0].h = 1.7;
    pp[0].z = t < 1500 ? 4.0 : t < 4500 ? 4.0 - 1.0 * (t - 1500) / 1000.0 : t < 6000 ? 1.0 : t < 9000 ? 1.0 + (t - 6000) / 1000.0 : 4.0;
}
static void who_stand(long t, person_t pp[2])
{
    (void)t;
    memset(pp, 0, 2 * sizeof pp[0]);
    pp[0].on = 1; pp[0].x = 0.0; pp[0].z = 3.0; pp[0].h = 1.7;
}
static void who_cross2(long t, person_t pp[2])
{
    memset(pp, 0, 2 * sizeof pp[0]);
    pp[0].on = 1; pp[0].h = 1.7; pp[0].z = 3.2; pp[0].x = -0.0 + 1.0 * (t - 1000) / 1000.0; if (t < 1000) pp[0].x = 0;       /* A walks right at 1 m/s */
    pp[1].on = t > 1500; pp[1].h = 1.65; pp[1].z = 2.4; pp[1].x = 3.2 - 1.1 * (t - 1500) / 1000.0;                          /* B walks left, nearer */
}
/* feint: left 2 m/s for 0.6 s then stop at 3 m; reverse: left 0.7 s then right; sprint: 2 m in 1.2 s then stop */
static void who_feint(long t, person_t pp[2])
{
    memset(pp, 0, 2 * sizeof pp[0]);
    pp[0].on = 1; pp[0].h = 1.7; pp[0].z = 3.0;
    pp[0].x = t < 2000 ? 0 : t < 2600 ? -2.0 * (t - 2000) / 1000.0 : -1.2;
}
static void who_feint_rev(long t, person_t pp[2])
{
    memset(pp, 0, 2 * sizeof pp[0]);
    pp[0].on = 1; pp[0].h = 1.7; pp[0].z = 3.0;
    pp[0].x = t < 2000 ? 0 : t < 2600 ? -2.0 * (t - 2000) / 1000.0 : t < 3800 ? -1.2 + 2.0 * (t - 2600) / 1000.0 : 1.2;
}
static void who_sprint(long t, person_t pp[2])
{
    memset(pp, 0, 2 * sizeof pp[0]);
    pp[0].on = 1; pp[0].h = 1.7; pp[0].z = 3.0;
    pp[0].x = t < 2000 ? 0 : t < 3200 ? 2.0 * (t - 2000) / 1200.0 * 1.0 : 2.0;
}

/* sprint right 2 m, hidden from 2.9 s, and back to 0.5 m while hidden: the head that keeps going overshoots */
static void who_sprint_back(long t, person_t pp[2])
{
    memset(pp, 0, 2 * sizeof pp[0]);
    pp[0].on = 1; pp[0].h = 1.7; pp[0].z = 3.0;
    pp[0].x = t < 2000 ? 0 : t < 3200 ? 2.0 * (t - 2000) / 1200.0 : t < 4000 ? 2.0 - 1.5 * (t - 3200) / 800.0 : 0.5;
}
static void who_feint_back(long t, person_t pp[2])
{
    memset(pp, 0, 2 * sizeof pp[0]);
    pp[0].on = 1; pp[0].h = 1.7; pp[0].z = 3.0;
    pp[0].x = t < 2000 ? 0 : t < 2700 ? -2.0 * (t - 2000) / 1000.0 : t < 3500 ? -1.4 + 1.2 * (t - 2700) / 800.0 : -0.2;
}

static scen_t SC_WALK = { "walk", 9000, who_walk, {{{0}}}, 0, 0, 0 };

static void line(const char *name, const res_t *r, int mode)
{
    printf("  %-26s %s in zone %5.1f %%  in frame %5.1f %%  max off %3.0f px  starts %2d  cmds %3d  tilt travel %4.0f  pan travel %5.0f\n", name, MODE[mode], r->in_zone, r->in_frame,
           r->max_off, r->starts, r->cmds, r->tilt_travel, r->pan_travel);
}

int main(int argc, char **argv)
{
    if (argc > 2) { /* debugging: sim MODE(0 dev,1 steps,2 cont) walk V Z | feint | feint_rev | sprint | crouch | occl | blind: one scenario with a trace per tick */
        scen_t d;
        memset(&d, 0, sizeof d);
        d.total_ms = 9000;
        d.who = who_walk; g_v = argc > 4 ? atof(argv[3]) : 1.0; g_z = argc > 4 ? atof(argv[4]) : 3.0;
        if (!strcmp(argv[2], "feint")) d.who = who_feint;
        if (!strcmp(argv[2], "cross2")) { d.who = who_cross2; d.total_ms = 8500; }
        if (!strcmp(argv[2], "feint_rev")) d.who = who_feint_rev;
        if (!strcmp(argv[2], "sprint")) d.who = who_sprint;
        if (!strcmp(argv[2], "crouch")) { d.who = who_crouch; g_z = 3.0; }
        if (!strcmp(argv[2], "approach")) { d.who = who_approach; d.total_ms = 10500; }
        if (!strcmp(argv[2], "occl")) { d.occl[0][0][0] = 3000; d.occl[0][0][1] = 4000; }
        if (!strcmp(argv[2], "blind")) { d.who = who_sprint; d.blind_fast = 180; d.blind_until = 5200; }
        { res_t q = run(&d, atoi(argv[1]), 1); line(argv[2], &q, atoi(argv[1])); }
        return 0;
    }
    res_t r[3];
    scen_t s;
    static const double VS[3] = { 0.5, 1.0, 1.5 }, ZS[3] = { 1.0, 2.0, 4.0 };
    double dev_starts = 0, cont_starts = 0, dev_zone = 0, cont_zone = 0;
    int ncross = 0;
    clock_t c0 = clock();

    printf("== walking person, 0.5 / 1.0 / 1.5 m/s crossing at 1 / 2 / 4 m (the camera turns at most 500 steps/s: 1.5 m/s at 1 m cannot be followed)\n");
    for (int vi = 0; vi < 3; vi++)
        for (int zi = 0; zi < 3; zi++) {
            char nm[40];
            s = SC_WALK;
            g_v = VS[vi]; g_z = ZS[zi];
            snprintf(nm, sizeof nm, "%.1f m/s at %.0f m", g_v, g_z);
            for (int m = 0; m < 3; m++) { r[m] = run(&s, m, 0); line(nm, &r[m], m); }
            /* only the cases the motor can follow count for the before / after sums */
            if (g_v / g_z * KRAD <= 420) { dev_starts += r[0].starts; cont_starts += r[2].starts; dev_zone += r[0].in_zone; cont_zone += r[2].in_zone; ncross++; }
            CHECK(r[0].range_viol + r[1].range_viol + r[2].range_viol == 0 && r[0].bad_speed + r[1].bad_speed + r[2].bad_speed == 0, "soft range kept, speed divisors valid");
            if (g_v / g_z * KRAD <= 330) {
                CHECK(r[2].in_frame >= 97.0, "continuous: the person stays in frame (a followable walk)");
                CHECK(r[2].in_zone >= 85.0 && r[2].max_off <= 110.0, "continuous: in the comfort zone >= 85 % of the time (the person may reach its edge: that is the hysteresis), offset <= 110 px");
            }
        }
    printf("  followable crossings: %d; mean motion starts per crossing DEV %.1f -> CONT %.1f; mean time in zone DEV %.1f %% -> CONT %.1f %%\n", ncross, dev_starts / ncross, cont_starts / ncross, dev_zone / ncross,
           cont_zone / ncross);
    CHECK(cont_starts < dev_starts * 0.5, "continuous: the number of motion starts per crossing at least halves vs the dev nudges");

    printf("== standing person at 3 m, crouch 1.7 -> 1.0 m and stand up (re-centring on the body for nothing)\n");
    memset(&s, 0, sizeof s);
    s.name = "crouch"; s.total_ms = 9000; s.who = who_crouch; g_z = 3.0;
    for (int m = 0; m < 3; m++) { r[m] = run(&s, m, 0); line("crouch / stand at 3 m", &r[m], m); }
    CHECK(r[2].tilt_travel < r[0].tilt_travel * 0.6 && r[2].tilt_travel <= 110, "continuous: far less tilt travel than the dev policy while the person crouches and stands");
    CHECK(r[2].pan_travel < 40, "continuous: no pan for a person who does not move sideways");
    s.who = who_stand;
    for (int m = 0; m < 3; m++) { r[m] = run(&s, m, 0); line("standing still 3 m", &r[m], m); }
    CHECK(r[2].starts == 0 && r[1].starts == 0, "a person standing in the zone never moves the camera (new policy)");

    printf("== approach (4 m -> 1 m at 1 m/s) and leave\n");
    memset(&s, 0, sizeof s);
    s.name = "approach"; s.total_ms = 10500; s.who = who_approach;
    for (int m = 0; m < 3; m++) { r[m] = run(&s, m, 0); line("approach / leave", &r[m], m); }
    CHECK(r[2].pan_travel < 200 && r[1].pan_travel < 200, "approach / recede changes the distance, not the angles: no useless pan (new policy)");
    CHECK(r[2].in_frame >= 99.0, "the person stays in frame all along the approach (the head is aimed without losing the body)");

    printf("== brief occlusion (1.0 s without detection at 3.0 s) of a walker at 1 m/s, 3 m\n");
    memset(&s, 0, sizeof s);
    s.name = "occl"; s.total_ms = 9000; s.who = who_walk; g_v = 1.0; g_z = 3.0;
    s.occl[0][0][0] = 3000; s.occl[0][0][1] = 4000;
    for (int m = 0; m < 3; m++) { r[m] = run(&s, m, 0); line("occlusion 1 s", &r[m], m); printf("    reacquired (reference back inside the zone) %d ms after the occlusion\n", r[m].reacq_ms); }
    CHECK(r[2].in_frame >= 90.0 && r[2].reacq_ms >= 0 && r[2].reacq_ms <= 1500, "continuous: the walker is back in the zone within 1.5 s after a 1 s occlusion, never lost");
    CHECK(r[2].in_frame >= r[0].in_frame - 0.5, "continuous: no worse than the dev policy");

    printf("== two people crossing (A walks right 1 m/s at 3.2 m, B walks left 1.1 m/s at 2.4 m)\n");
    memset(&s, 0, sizeof s);
    s.name = "cross2"; s.total_ms = 8500; s.who = who_cross2;
    for (int m = 0; m < 3; m++) { r[m] = run(&s, m, 0); line("two people crossing", &r[m], m); printf("    the camera is nearer to person A than to B %.0f %% of the time\n", r[m].follow_a); }
    CHECK(r[2].follow_a >= 60.0, "continuous: the camera stays with the person it followed (A) most of the time");

    printf("== feints: the person must never leave the frame because of the camera's own motion\n");
    memset(&s, 0, sizeof s);
    s.who = who_feint; s.total_ms = 7000;
    for (int m = 0; m < 3; m++) { r[m] = run(&s, m, 0); line("feint left (2 m/s, 0.6 s)", &r[m], m); printf("    person out of frame for %d ms\n", r[m].left_frame_ms); }
    CHECK(r[2].left_frame_ms == 0 && r[1].left_frame_ms == 0, "feint then stop: never out of frame (new policy, both motion modes)");
    CHECK(r[2].max_off <= 240, "feint then stop: the offset stays well inside the frame (CONT)");
    s.who = who_feint_rev;
    for (int m = 0; m < 3; m++) { r[m] = run(&s, m, 0); line("feint then reverse", &r[m], m); printf("    person out of frame for %d ms\n", r[m].left_frame_ms); }
    CHECK(r[2].left_frame_ms == 0, "feint then reverse: never out of frame (CONT; timed nudges cannot follow a reversal at 2 m/s, DEV and STEPS lose the person)");
    s.who = who_sprint; s.total_ms = 8000;
    for (int m = 0; m < 3; m++) { r[m] = run(&s, m, 0); line("sprint 2 m (1.7 m/s) then stop", &r[m], m); printf("    person out of frame for %d ms\n", r[m].left_frame_ms); }
    CHECK(r[2].left_frame_ms == 0, "sprint then stop: never out of frame (CONT)");

    printf("== overshoot: the detections stop while the head turns fast (blind run), then the person is found again between the two poses\n");
    memset(&s, 0, sizeof s);
    s.who = who_sprint; s.total_ms = 9000; s.blind_fast = 180; s.blind_until = 5200;
    for (int m = 1; m < 3; m++) { r[m] = run(&s, m, 0); line("sprint, blind while turning", &r[m], m); printf("    person out of frame for %d ms, sweep-backs %d\n", r[m].left_frame_ms, r[m].sweeps); }
    CHECK(r[2].in_frame >= 60.0, "continuous: the person is found again after a blind sprint (in frame most of the time)");
    printf("== track_debug=1 in continuous mode: the decision log\n");
    {
        char *buf = NULL; size_t len = 0; FILE *f;
        memset(&s, 0, sizeof s);
        s.who = who_walk; s.total_ms = 6000; g_v = 1.0; g_z = 3.0;
        remove("/tmp/akam2k_track.log");
        g_debug = 1; (void)run(&s, M_CONT, 0); g_debug = 0;
        f = fopen("/tmp/akam2k_track.log", "r");
        if (f) { buf = calloc(1, 70000); len = fread(buf, 1, 69999, f); fclose(f); }
        CHECK(len > 500 && strstr(buf, " CONT box body - ref=") && strstr(buf, "dist=") && strstr(buf, "kf=ok") && strstr(buf, "zone ex=") && strstr(buf, "cmd=") && strstr(buf, "lease=450") && strstr(buf, "why=vel") && strstr(buf, "hold=0 search=0 sweep=0"),
              "the log has the rule, the reference, the 3D state, the filter gate, the zone state, the command and the lease, the hold / search / sweep flags and the reason");
        CHECK(buf && strstr(buf, "why=in-zone"), "... and the reason when it does not move");
        { const char *l = buf ? strstr(buf, " CONT box") : NULL; if (l) { const char *e = strchr(l, '\n'); printf("   e.g. %.*s\n", e ? (int)(e - l) : 80, l); } }
        free(buf);
        remove("/tmp/akam2k_track.log");
    }
    printf("== tracking area: a walker leaves the area (pan 1000..1600): the head never passes its edge (the margin scales with speed x (tick + lease))\n");
    memset(&s, 0, sizeof s);
    s.who = who_walk; s.total_ms = 9000; g_v = 1.2; g_z = 2.5;
    g_area_conf = "track_area_a=1000:1600:300:2100\n";
    for (int m = 1; m < 3; m++) { r[m] = run(&s, m, 0); line("walker leaves the area", &r[m], m); printf("    pan range %.0f..%.0f (area 1000..1600)\n", r[m].pan_min, r[m].pan_max); }
    g_area_conf = NULL;
    CHECK(r[2].pan_min >= 1000 && r[2].pan_max <= 1600 && r[1].pan_min >= 1000 - 1, "CONT: the head stays inside the area (steps mode keeps its own clip)");
    printf("== overshoot after a feint / a sprint: the person is hidden for 1.4 s when it stops; the head searches BETWEEN its pose before the move and its pose now\n");
    memset(&s, 0, sizeof s);
    s.who = who_sprint_back; s.total_ms = 9000;
    s.occl[0][0][0] = 2900; s.occl[0][0][1] = 4400;
    for (int m = 1; m < 3; m++) { r[m] = run(&s, m, 0); line("sprint then hidden 1.4 s", &r[m], m); printf("    back in the zone %d ms after the occlusion, sweep-backs %d, pan range %.0f..%.0f (before the move: 1275)\n", r[m].reacq_ms, r[m].sweeps, r[m].pan_min, r[m].pan_max); }
    CHECK(r[2].reacq_ms >= 0 && r[2].reacq_ms <= 3000, "sprint then hidden: the person is back in the comfort zone within 3 s after it shows again");
    CHECK(r[2].pan_max <= HOME_PAN + 12, "... the head never went beyond the pose it had before the move on the other side (the sweep stops at it)");
    s.who = who_feint_back; s.total_ms = 9000;
    s.occl[0][0][0] = 2500; s.occl[0][0][1] = 4000;
    for (int m = 1; m < 3; m++) { r[m] = run(&s, m, 0); line("feint then hidden 1.5 s", &r[m], m); printf("    back in the zone %d ms after the occlusion, sweep-backs %d, pan range %.0f..%.0f\n", r[m].reacq_ms, r[m].sweeps, r[m].pan_min, r[m].pan_max); }
    CHECK(r[2].reacq_ms >= 0 && r[2].reacq_ms <= 3000, "feint then hidden: found again within 3 s after it shows again");
    CHECK(r[2].pan_min >= 1275 - 500 && r[2].pan_max <= 1275 + 700, "... the head stayed inside the interval of the feint (plus the overshoot margin)");
    printf("  cpu: %.2f s for the whole simulation (%ld ticks of the tracker)\n", (double)(clock() - c0) / CLOCKS_PER_SEC, 0L);
    printf("rot2k track cont sim: %d failed\n", fails);
    return fails ? 1 : 0;
}
