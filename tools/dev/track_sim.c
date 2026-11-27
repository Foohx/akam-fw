/* Closed-loop tracker bench (host): the REAL wiring (track.c + track_logic.c) against a simulated room and a fake continuous motor, long synthetic scenarios, and the
 * numbers that say whether a tracker change is better or worse. The room, the pinhole camera, the detector model (5 Hz, 63..110 ms inference, 50 ms video latency, jitter,
 * arm swing, motion blur) and the motor are the ones of tests/rot2k_track_cont_sim.c (copied: that file is a test with its own main).
 * Usage: track_sim [scenario ...]   (no argument: all). Output: one line per scenario, "name key=value ...", deterministic (fixed seeds), the format of tests/golden/track_bench.txt.
 * Keys: lock (% of the time the person's reference is inside the comfort zone), frame (% inside the frame), err_mean / err_p95 (px, horizontal, 160 = out of the frame),
 * rev_lat_mean / rev_lat_max (ms from a direction change of the person to the camera turning the same way, 4000 = never), still_travel (pan steps travelled while the person had
 * been still for 3 s), flips (camera pan direction reversals), cmds (velocity commands), starts (motion starts), reacq_mean / reacq_max (ms from the person (re)appearing to the
 * zone, 20000 = never; -1 = no such event), lost_ms (the person present but outside the frame), pan_range / tilt_range / tilt_travel (steps), err_drift (px, mean error of the
 * last third minus the first). */
#define _GNU_SOURCE
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "motion_grid.h"
#include "store.h"
#include "track.h"

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
#define MAXT 620000                /* ms */
#define RATE 1100.0                /* steps/s = RATE / divisor */
#define LEASE_MS 450

enum { M_DEV, M_STEPS, M_CONT };

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


static int g_search;               /* TRACK_SIM_SEARCH=legacy: the legacy lost-target search (search_mode); default the motion-aware one (the 2K default since 2026-11-26) */
static int g_home;                 /* the scenario goes home after a loss (return_home 1, the 30 s delay of the defaults) */
static void cfg_for(int mode, struct tl_cfg *c)
{
    (void)mode;
    tl_cfg_defaults(c);
    c->motion = TL_MOTION_CONT;
    c->invert_pan = 1;
    c->invert_tilt = 0;
    c->tilt_enable = 1;
    c->lead_pct = 70;
    c->return_home = g_home;
    c->search_mode = g_search ? TL_SEARCH_MOTION : TL_SEARCH_LEGACY;
    if (getenv("TRACK_SIM_REACQ")) c->reacq_s = atoi(getenv("TRACK_SIM_REACQ"));
}
static a2k_track_ops_t OPS;

/* ------------------------------------------------------------------ the path of the person: waypoints, linear in between */
typedef struct { long t; double x, z, h; int on; } wp_t;
#define MAXWP 4096
static wp_t WP[MAXWP];
static int NWP;
static void wp_reset(void) { NWP = 0; }
static void wp_add(long t, double x, double z, double h, int on) { if (NWP < MAXWP) WP[NWP++] = (wp_t){ t, x, z, h, on }; }
static void who_wp(long t, person_t pp[2])
{
    int i = 0;
    memset(pp, 0, 2 * sizeof pp[0]);
    if (NWP == 0) return;
    while (i + 1 < NWP && WP[i + 1].t <= t) i++;
    if (t <= WP[0].t || i + 1 >= NWP) { pp[0].x = WP[i].x; pp[0].z = WP[i].z; pp[0].h = WP[i].h; pp[0].on = WP[i].on; return; }
    {
        double f = (double)(t - WP[i].t) / (double)(WP[i + 1].t - WP[i].t);
        pp[0].x = WP[i].x + (WP[i + 1].x - WP[i].x) * f;
        pp[0].z = WP[i].z + (WP[i + 1].z - WP[i].z) * f;
        pp[0].h = WP[i].h + (WP[i + 1].h - WP[i].h) * f;
        pp[0].on = WP[i].on;
    }
}

/* ------------------------------------------------------------------ the scenarios (the person stands at x=0 for the first second) */
typedef struct { const char *name; int total_ms; void (*build)(void); } sc_t;
static double g_a, g_v, g_z;
static void sb_walk(void)   /* walk between -A and +A at v m/s, reversing, depth z */
{
    long t = 1000; double x = 0; int dir = 1, first = 1;
    wp_add(0, 0, g_z, 1.7, 1);
    wp_add(1000, 0, g_z, 1.7, 1);
    while (t < 60000) {
        double lim = dir > 0 ? g_a : -g_a, dt = fabs(lim - x) / g_v * 1000.0;
        (void)first;
        t += (long)dt; x = lim; wp_add(t, x, g_z, 1.7, 1); dir = -dir;
    }
}
static void sb_stand(void) { wp_add(0, 0.0, 3.0, 1.7, 1); wp_add(60000, 0.0, 3.0, 1.7, 1); }
static void sb_crouch(void) /* z 1.2: crouch (1.7 -> 1.0), stay, rise; twice */
{
    long t = 1000;
    wp_add(0, 0, 1.2, 1.7, 1); wp_add(t, 0, 1.2, 1.7, 1);
    for (int k = 0; k < 2; k++) {
        wp_add(t + 800, 0, 1.2, 1.0, 1); wp_add(t + 3000, 0, 1.2, 1.0, 1); wp_add(t + 3800, 0, 1.2, 1.7, 1); wp_add(t + 6000, 0, 1.2, 1.7, 1);
        t += 6000;
    }
}
static void sb_sit(void)    /* z 1.0: sits down slowly (1.7 -> 1.15), stays, stands up quickly */
{
    wp_add(0, 0.2, 1.0, 1.7, 1); wp_add(1000, 0.2, 1.0, 1.7, 1); wp_add(3500, 0.2, 1.0, 1.15, 1); wp_add(12000, 0.2, 1.0, 1.15, 1); wp_add(12600, 0.2, 1.0, 1.7, 1); wp_add(20000, 0.2, 1.0, 1.7, 1);
}
static void sb_leave(void)  /* walks out of the frame to the right, gone 10 s, comes back from the left at another depth; twice */
{
    wp_add(0, 0, 3.0, 1.7, 1); wp_add(1000, 0, 3.0, 1.7, 1); wp_add(4000, 3.5, 3.0, 1.7, 1); wp_add(4001, 3.5, 3.0, 1.7, 0);
    wp_add(14000, -4.0, 2.2, 1.7, 0); wp_add(14001, -4.0, 2.2, 1.7, 1); wp_add(17000, -1.0, 2.2, 1.7, 1); wp_add(22000, -1.0, 2.2, 1.7, 1);
    wp_add(24000, 2.0, 4.0, 1.7, 1); wp_add(24001, 2.0, 4.0, 1.7, 0); wp_add(34000, 0.5, 3.5, 1.7, 0); wp_add(34001, 0.5, 3.5, 1.7, 1); wp_add(40000, 0.5, 3.5, 1.7, 1);
}
/* A short pass-by: crosses the view from one side to the other at 1.2 m/s (about 4 s in view), then nobody. 45 s in all, the head goes home after the loss. */
static void sb_pass(void)
{
    wp_add(0, -4.0, 3.0, 1.7, 0); wp_add(2000, -4.0, 3.0, 1.7, 1); wp_add(2000 + 8000 * 1000 / 1200 + 1, 4.0, 3.0, 1.7, 1); wp_add(2000 + 8000 * 1000 / 1200 + 2, 4.0, 3.0, 1.7, 0); wp_add(45000, 4.0, 3.0, 1.7, 0);
}
static void sb_pass_rl(void)
{
    wp_add(0, 4.0, 3.0, 1.7, 0); wp_add(2000, 4.0, 3.0, 1.7, 1); wp_add(2000 + 8000 * 1000 / 1200 + 1, -4.0, 3.0, 1.7, 1); wp_add(2000 + 8000 * 1000 / 1200 + 2, -4.0, 3.0, 1.7, 0); wp_add(45000, -4.0, 3.0, 1.7, 0);
}
/* Leaves the frame to the right, is away for 6 s, comes back from the right and stays 10 s. */
static void sb_leave_back(void)
{
    wp_add(0, 0, 3.0, 1.7, 1); wp_add(1000, 0, 3.0, 1.7, 1); wp_add(4500, 3.6, 3.0, 1.7, 1); wp_add(4501, 3.6, 3.0, 1.7, 0);
    wp_add(10500, 3.6, 3.0, 1.7, 0); wp_add(10501, 3.6, 3.0, 1.7, 1); wp_add(13000, 0.5, 3.0, 1.7, 1); wp_add(25000, 0.5, 3.0, 1.7, 1); wp_add(25001, 0.5, 3.0, 1.7, 0); wp_add(45000, 0.5, 3.0, 1.7, 0);
}
/* Walks 2 s, stops inside the view and the detector loses him for 15 s (still, behind something: no motion either), then he is seen again. */
static void sb_hide_still(void)
{
    wp_add(0, 0, 3.0, 1.7, 1); wp_add(1000, 0, 3.0, 1.7, 1); wp_add(3000, 1.0, 3.0, 1.7, 1); wp_add(3001, 1.0, 3.0, 1.7, 0); wp_add(18000, 1.0, 3.0, 1.7, 0); wp_add(18001, 1.0, 3.0, 1.7, 1); wp_add(30000, 1.0, 3.0, 1.7, 1);
}
static unsigned g_ws;
static double wr(void) { g_ws = g_ws * 1103515245u + 12345u; return ((g_ws >> 16) & 0x7fff) / 32767.0; }
static void sb_long(void)   /* 10 minutes: pauses of 3..20 s, walks of 0.4..1.5 m/s with reversals, depths 1.5..4.5 m, 6 departures (8..25 s) */
{
    long t = 0; double x = 0, z = 3.0; int leaves = 0;
    g_ws = 4242;
    wp_add(0, 0, z, 1.7, 1);
    while (t < 600000 && NWP + 8 < MAXWP) {
        if (t > 60000L * (leaves + 1) && leaves < 6) {   /* leave and come back elsewhere */
            double side = wr() < 0.5 ? -1 : 1, nz = 1.5 + 3.0 * wr(), ex = side * (z * 1.4 + 1.0);
            t += 3000; x = ex; wp_add(t, x, z, 1.7, 1); wp_add(t + 1, x, z, 1.7, 0);
            t += 8000 + (long)(17000 * wr()); x = -side * (nz * 1.2); z = nz; wp_add(t, x, z, 1.7, 0); wp_add(t + 1, x, z, 1.7, 1);
            t += 2000; x *= 0.4; wp_add(t, x, z, 1.7, 1); leaves++;
            continue;
        }
        if (wr() < 0.4) { t += 3000 + (long)(17000 * wr()); wp_add(t, x, z, 1.7, 1); }    /* pause */
        else {
            double nz = 1.5 + 3.0 * wr(), nx = (wr() * 2 - 1) * nz * 0.9, v = 0.4 + 1.1 * wr();
            t += (long)(sqrt((nx - x) * (nx - x) + (nz - z) * (nz - z)) / v * 1000.0) + 200; x = nx; z = nz; wp_add(t, x, z, 1.7, 1);
        }
    }
    wp_add(600000, x, z, 1.7, 1);
}
static const sc_t SCS[] = {
    { "walk_z0.8_slow", 60000, sb_walk }, { "walk_z0.8_fast", 60000, sb_walk }, { "walk_z2_slow", 60000, sb_walk }, { "walk_z2_fast", 60000, sb_walk },
    { "walk_z5_slow", 60000, sb_walk }, { "walk_z5_fast", 60000, sb_walk }, { "stand_z3_60s", 60000, sb_stand }, { "crouch_z1.2", 13000, sb_crouch }, { "sit_stand_z1.0", 20000, sb_sit },
    { "leave_return", 40000, sb_leave }, { "long_10min", 600000, sb_long },
    { "pass_lr", 45000, sb_pass }, { "pass_rl", 45000, sb_pass_rl }, { "leave_back", 45000, sb_leave_back }, { "hide_still", 30000, sb_hide_still },
};
#define NSC ((int)(sizeof SCS / sizeof SCS[0]))
static void params_for(const char *n)
{
    g_z = 2; g_v = 0.6; g_a = 1.5;
    if (strstr(n, "z0.8")) { g_z = 0.8; g_a = 0.6; }
    if (strstr(n, "z2")) { g_z = 2.0; g_a = 1.2; }
    if (strstr(n, "z5")) { g_z = 5.0; g_a = 3.0; }
    g_v = strstr(n, "fast") ? 1.5 : 0.6;
    g_home = !strcmp(n, "pass_lr") || !strcmp(n, "pass_rl") || !strcmp(n, "leave_back") || !strcmp(n, "hide_still");
}

/* ------------------------------------------------------------------ one run and its metrics */
static int cmp_i(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }
static int g_err[MAXT / DT + 8];
static signed char g_vs[MAXT / DT + 8];
/* The synthetic motion grid: the person's box (at the pose of the frame) marks its cells when he MOVES (the waypoint speed over the last 200 ms), a standing person marks nothing; the head moving
 * masks it (the whole picture shifts), as the real wiring does. Fed to the REAL grid logic (motion_grid.c) and given to the policy. */
static mg_t g_mg;
static double g_last_pan = -1, g_last_tilt = -1;
static void mot_feed(a2k_track_t *t, long T)
{
    uint8_t act[MG_N];
    struct tl_mot mo;
    const struct mg_state *st;
    person_t a[2], b[2];
    struct tl_box bx;
    int cut = 0, cx, cy;
    double exv = 0, v;
    memset(act, 0, sizeof act);
    if (g_last_pan >= 0 && (fabs(P.pan - g_last_pan) > 0.4 || fabs(P.tilt - g_last_tilt) > 0.4)) mg_mask(&g_mg, T, MG_PTZ_AFTER_MS);
    g_last_pan = P.pan; g_last_tilt = P.tilt;
    who_wp(T, a); who_wp(T - 200, b);
    v = hypot(a[0].x - b[0].x, a[0].z - b[0].z) / 0.2;
    if (a[0].on && b[0].on && v > 0.15 && render(&a[0], P.pan, P.tilt, &bx, &cut, &exv)) {
        for (cy = 0; cy < MG_H; cy++) for (cx = 0; cx < MG_W; cx++) {
            int px = cx * FW / MG_W + FW / MG_W / 2, py = cy * FH / MG_H + FH / MG_H / 2;
            if (px >= bx.x1 && px <= bx.x2 && py >= bx.y1 && py <= bx.y2) act[cy * MG_W + cx] = 255;
        }
    }
    mg_feed_cells(&g_mg, T, act);
    st = mg_get(&g_mg);
    memset(&mo, 0, sizeof mo);
    mo.valid = st->valid; mo.masked = st->masked; mo.active = st->active; mo.cx = st->cx; mo.cy = st->cy;
    mo.last_dir = st->last_dir; mo.last_active_ms = st->last_active_ms; mo.dir_ms = st->dir_ms;
    tl_set_motion(&t->tl, &mo);
}

static void run(const sc_t *sc)
{
    a2k_track_t t;
    struct tl_cfg c;
    long nxt = 1000, lost_ms = 0;
    const double half = FW * 0.2;
    int ne = 0, nloc = 0, nfr = 0, i, flips = 0, last = 0, rl_n = 0, rl_max = 0, ra_n = 0, ra_max = 0, n1 = 0, n3 = 0;
    double esum = 0, e1 = 0, e3 = 0, still_travel = 0, rl_sum = 0, ra_sum = 0, pmin = 1e9, pmax = -1e9, tmin = 1e9, tmax = -1e9;
    person_t pp[2], pq[2];
    g_rng = 777;
    wp_reset();
    params_for(sc->name);
    sc->build();
    p_reset(M_CONT);
    mg_init(&g_mg, NULL);
    g_last_pan = -1;
    OPS = (a2k_track_ops_t){ f_info, f_range, f_nudge, f_home, f_priv, NULL, NULL, NULL, f_vel, f_pos_at, NULL, NULL, NULL, NULL, NULL };
    a2k_track_init(&t, &OPS, NULL);
    cfg_for(M_CONT, &c);
    (void)a2k_track_set_cfg(&t, &c);
    a2k_track_set_enabled(&t, 1);
    for (P.now = 0; P.now < sc->total_ms; P.now += DT) {
        long T = P.now;
        p_step();
        if (T < nxt) continue;
        {
            long infer = 63 + (long)(rndu() * 47), E = T - infer - 50, cap = T - infer;
            double pan, tilt, spd = P.speed_pan;
            struct tl_box bx[2], b;
            int nb = 0, cut = 0;
            double exv = 0;
            nxt = T + 200 + (long)(rndu() * 30);
            if (E < 0) continue;
            pose_at(E, &pan, &tilt);
            who_wp(E, pp);
            if (pp[0].on && render(&pp[0], pan, tilt, &b, &cut, &exv) && !(spd > 100 && rndu() < (spd > 300 ? 0.55 : 0.35))) {
                b.x1 += (int)(rndu() * 7) - 3; b.x2 += (int)(rndu() * 7) - 3; b.y1 += (int)(rndu() * 5) - 2; b.y2 += (int)(rndu() * 5) - 2;
                if (rndu() < 0.12) { int ar = (int)((b.x2 - b.x1) * 0.12); b.x1 -= ar; b.x2 += ar; }
                if (b.x1 < 0) b.x1 = 0;
                if (b.x2 > FW - 1) b.x2 = FW - 1;
                if (b.y1 < 0) b.y1 = 0;
                if (b.y2 > FH - 1) b.y2 = FH - 1;
                if (b.x2 - b.x1 >= 6 && b.y2 - b.y1 >= 10) bx[nb++] = b;
            }
            mot_feed(&t, T);
            a2k_track_tick_ts(&t, T, cap, bx, nb, FW, FH);
        }
    }
    /* post-hoc, from the 5 ms pose history and the truth */
    for (i = 500 / DT; i < P.nh; i++) {
        long T = (long)i * DT;
        double yaw = -(P.hp[i] - HOME_PAN) / KRAD, ex, a;
        int in, e;
        who_wp(T, pp);
        if (P.hp[i] < pmin) pmin = P.hp[i];
        if (P.hp[i] > pmax) pmax = P.hp[i];
        if (P.ht[i] < tmin) tmin = P.ht[i];
        if (P.ht[i] > tmax) tmax = P.ht[i];
        if (getenv("TRACK_SIM_PAN") && T % 500 == 0) fprintf(stderr, "%ld on=%d pan=%.0f tilt=%.0f\n", T, pp[0].on, P.hp[i], P.ht[i]);
        if (!pp[0].on) continue;   /* nobody there: no error */
        ex = FHZ * tan(atan2(pp[0].x, pp[0].z) - yaw);
        a = fabs(ex);
        in = a < FW / 2.0;
        e = in ? (int)(a + 0.5) : FW / 2;
        g_err[ne++] = e;
        if (a <= half) nloc++;
        if (in) nfr++; else lost_ms += DT;
        esum += e;
        if (getenv("TRACK_SIM_TRACE") && T % 100 == 0 && T >= atol(getenv("TRACK_SIM_TRACE"))) fprintf(stderr, "%ld ex=%.0f pan=%.0f v=%.0f\n", T, ex, P.hp[i], i >= 10 ? (P.hp[i] - P.hp[i - 10]) * 20 : 0.0);
        if (T < sc->total_ms / 3) { e1 += e; n1++; }
        if (T >= sc->total_ms * 2 / 3) { e3 += e; n3++; }
    }
    for (i = 10; i < P.nh; i++) {   /* camera direction reversals (pan velocity over 50 ms, > 20 steps/s) */
        double v = P.hp[i] - P.hp[i - 10];
        int s = v > 1.0 ? 1 : v < -1.0 ? -1 : 0;
        g_vs[i] = (signed char)s;
        if (s && last && s != last) flips++;
        if (s) last = s;
    }
    for (i = 3000 / DT; i < P.nh; i++) {   /* pan travel while the person had been still for 3 s */
        long T = (long)i * DT;
        who_wp(T, pp); who_wp(T - 3000, pq);
        if (pp[0].on && pq[0].on && pp[0].x == pq[0].x && pp[0].z == pq[0].z && pp[0].h == pq[0].h) still_travel += fabs(P.hp[i] - P.hp[i - 1]);
    }
    {   /* reaction latency: at each direction change of the person, until the camera turns the right way (pan decreases when the person goes right) */
        int lastdir = 0;
        for (int k = 0; k + 1 < NWP; k++) {
            double dx = WP[k + 1].x - WP[k].x;
            int d = dx > 0.05 ? 1 : dx < -0.05 ? -1 : 0, j, lat = 4000;
            if (!d || !WP[k].on || !WP[k + 1].on || WP[k + 1].t - WP[k].t < 400) continue;
            if (d == lastdir) continue;
            lastdir = d;
            if (WP[k].t < 1500) continue;
            for (j = (int)(WP[k].t / DT); j < P.nh && (long)j * DT < WP[k].t + 4000; j++)
                if (g_vs[j] == -d) { lat = (int)((long)j * DT - WP[k].t); break; }
            rl_sum += lat; rl_n++; if (lat > rl_max) rl_max = lat;
        }
    }
    for (int k = 1; k < NWP; k++) {   /* reacquire: from the person appearing (off -> on) until the reference is inside the comfort zone */
        long T0, T;
        if (!(WP[k].on && !WP[k - 1].on)) continue;
        T0 = WP[k].t;
        for (T = T0; T < T0 + 20000 && T < sc->total_ms; T += DT) {
            int j = (int)(T / DT);
            double yaw;
            if (j >= P.nh) break;
            yaw = -(P.hp[j] - HOME_PAN) / KRAD;
            who_wp(T, pp);
            if (fabs(FHZ * tan(atan2(pp[0].x, pp[0].z) - yaw)) <= half) break;
        }
        T -= T0;
        if (T >= 20000 || T0 + T >= sc->total_ms) T = 20000;
        ra_sum += (double)T; ra_n++; if (T > ra_max) ra_max = (int)T;
    }
    qsort(g_err, (size_t)ne, sizeof g_err[0], cmp_i);
    printf("%s lock=%.1f frame=%.1f err_mean=%.1f err_p95=%d rev_lat_mean=%.0f rev_lat_max=%d still_travel=%.0f flips=%d cmds=%d starts=%d reacq_mean=%.0f reacq_max=%d lost_ms=%ld pan_range=%.0f tilt_range=%.0f tilt_travel=%.0f err_drift=%.1f travel=%.0f\n",
           sc->name, ne ? 100.0 * nloc / ne : 0, ne ? 100.0 * nfr / ne : 0, ne ? esum / ne : 0, ne ? g_err[(long)ne * 95 / 100] : 0,
           rl_n ? rl_sum / rl_n : -1.0, rl_n ? rl_max : -1, still_travel, flips, P.vels, P.starts, ra_n ? ra_sum / ra_n : -1.0, ra_n ? ra_max : -1, lost_ms,
           pmax - pmin, tmax - tmin, P.tilt_travel, (n1 && n3) ? e3 / n3 - e1 / n1 : 0.0, P.pan_travel + P.tilt_travel);
    a2k_track_destroy(&t);
}

int main(int argc, char **argv)
{
    g_search = !(getenv("TRACK_SIM_SEARCH") && !strcmp(getenv("TRACK_SIM_SEARCH"), "legacy"));
    for (int k = 0; k < NSC; k++) {
        int want = argc < 2;
        for (int a = 1; a < argc; a++) if (!strcmp(argv[a], SCS[k].name)) want = 1;
        if (want) run(&SCS[k]);
    }
    return 0;
}
