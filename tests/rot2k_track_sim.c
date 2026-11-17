/* Host simulation of the Rotatable 2K person tracking: a simulated walking person (constant speed, stop-and-go, reversal, brief detection
 * dropouts) seen through the detector (5 Hz, 63..110 ms inference, frames captured ~60 ms before the tick, box jitter, blur while the head moves)
 * and the REAL wiring (akam2k_track.c + track_logic.c) driving a FAKE PTZ with a realistic motion time (30 ms start, 1 step per `speed` ms,
 * 70 ms to stop and read, commands accepted only while idle, one pending nudge). Geometry: pan inverted like the 2K binding; the TRUE steps per
 * frame width may differ from the policy's assumption (gain_pan 1000), see "gain error".
 *
 * Build: tests/rot2k_overlay_test.sh (rot2k track sim). With -DSIM_LEGACY it builds against the firmware 0.1.0 sources (the "before" numbers of
 * docs/porting/lsc-rotatable-2k/tracking.md): same scenarios, no assertions.
 * Prints the metrics tables, then "rot2k track sim: N failed". */
#define _GNU_SOURCE
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "akam2k_store.h"
#include "akam2k_track.h"

static int fails;
#ifdef SIM_LEGACY
#define CHECK(c, msg) do { if (!(c)) printf("  (0.1.0: not met - %s)\n", msg); } while (0)
#else
#define CHECK(c, msg) do { if (c) printf("  ok   - %s\n", msg); else { printf("  FAIL - %s\n", msg); fails++; } } while (0)
#endif

void a2k_log(const char *fmt, ...) { (void)fmt; }

#define DT 5                       /* simulation step, ms */
#define PAN_MIN 300
#define PAN_MAX 3650
#define TILT_MIN 300
#define TILT_MAX 2100
#define HOME_PAN 1275
#define HOME_TILT 1300
#define MAXN 90000                 /* 450 s */
#define BAND_PX 26                 /* the default pan band: 8 % of 320 = 25.6 px */

/* ------------------------------------------------------------------ fake PTZ */
static struct {
    double pan, tilt;
    int moving, active, phase, sp, pend, pdx, pdy, psp;
    double tp, tt;
    int64_t start_at, settle_until, end_ms;
    int homes, nudges, bad_speed, reversals, last_dir;
    int64_t first_home_ms, first_nudge_ms;
    int64_t last_nudge_ms, min_gap_ms, now;
    int range_violation;
    int state_override;            /* 0 = from the position, else a forced hal state + 1 */
    int privacy;
} P;
static int64_t g_cap_cur;          /* capture time of the frame the current tick is computed from */
static int g_n_busy, g_n_stale;    /* nudges sent while the motor was busy / from a frame captured before the last motion end + settle (or before the last nudge started) */
static int g_force_state;          /* -1 = privacy on, else a forced hal state + 1 (0 = none) */

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static void p_reset(void)
{
    memset(&P, 0, sizeof P);
    P.pan = HOME_PAN;
    P.tilt = HOME_TILT;
    P.min_gap_ms = 1 << 30;
    P.last_nudge_ms = -1;
    P.first_home_ms = P.first_nudge_ms = -1;
    if (g_force_state < 0) P.privacy = 1;
    else P.state_override = g_force_state;
}

static void p_start(int dx, int dy, int sp)
{
    P.tp = clampi((int)P.pan + dx, PAN_MIN, PAN_MAX);
    P.tt = clampi((int)P.tilt + dy, TILT_MIN, TILT_MAX);
    P.sp = sp > 0 ? sp : 3;
    P.active = 1;
    P.moving = 1;
    P.phase = 0;
    P.start_at = P.now + 30;
}

static void f_info(a2k_ptz_info_t *o)
{
    memset(o, 0, sizeof *o);
    o->state = HAL_PTZ_MOVED;
    if (!P.moving && fabs(P.pan - HOME_PAN) < 6 && fabs(P.tilt - HOME_TILT) < 6)
        o->state = HAL_PTZ_HOME;
    if (P.state_override)
        o->state = P.state_override - 1;
    o->known = !P.moving;
    o->pan = (uint32_t)(P.pan + 0.5);
    o->tilt = (uint32_t)(P.tilt + 0.5);
#ifndef SIM_LEGACY
    o->motion_end_ms = P.end_ms;
#endif
}
static int f_range(int r[4]) { r[0] = PAN_MIN; r[1] = PAN_MAX; r[2] = TILT_MIN; r[3] = TILT_MAX; return 0; }
#ifdef SIM_LEGACY
static int f_nudge(int dx, int dy)
{
    int sp = 0;
#else
static int f_nudge(int dx, int dy, int sp)
{
#endif
    if (P.last_nudge_ms >= 0 && P.now - P.last_nudge_ms < P.min_gap_ms)
        P.min_gap_ms = P.now - P.last_nudge_ms;
    if (P.active) g_n_busy++;
    if (P.nudges > 0 && (g_cap_cur < P.end_ms + 220 || g_cap_cur <= P.last_nudge_ms + 80)) g_n_stale++;
    P.last_nudge_ms = P.now;
    if (P.first_nudge_ms < 0)
        P.first_nudge_ms = P.now;
    P.nudges++;
    if (dx) {
        int d = dx > 0 ? 1 : -1;
        if (P.last_dir && d != P.last_dir)
            P.reversals++;
        P.last_dir = d;
    }
    if (sp != 0 && (sp < 2 || sp > 20))
        P.bad_speed++;
    if (P.active) { /* the real controller refuses it while busy; the sim keeps it (the tracker never sends one) */
        P.pdx += dx;
        P.pdy += dy;
        if (sp && (!P.psp || sp < P.psp)) P.psp = sp;
        P.pend = 1;
        return 0;
    }
    p_start(dx, dy, sp);
    return 0;
}
static int f_home(void)
{
    P.homes++;
    if (P.first_home_ms < 0)
        P.first_home_ms = P.now;
    if (!P.active)
        p_start(HOME_PAN - (int)P.pan, HOME_TILT - (int)P.tilt, 3);
    return 0;
}
static int f_priv(void) { return P.privacy; }
static const a2k_track_ops_t OPS = { f_info, f_range, f_nudge, f_home, f_priv, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL };

static void p_step(void)
{
    double st;
    if (!P.active || P.now < P.start_at)
        return;
    st = (double)DT / P.sp;
    if (P.phase == 0) {
        if (fabs(P.tp - P.pan) <= st) { P.pan = P.tp; P.phase = 1; }
        else P.pan += P.tp > P.pan ? st : -st;
    } else if (P.phase == 1) {
        if (fabs(P.tt - P.tilt) <= st) { P.tilt = P.tt; P.phase = 2; P.settle_until = P.now + 70; }
        else P.tilt += P.tt > P.tilt ? st : -st;
    } else if (P.now >= P.settle_until) {
        P.active = 0;
        P.moving = 0;
        P.end_ms = P.now;
        if (P.pend) {
            int dx = P.pdx, dy = P.pdy, sp = P.psp;
            P.pend = P.pdx = P.pdy = P.psp = 0;
            p_start(dx, dy, sp);
        }
    }
    if (P.pan < PAN_MIN - 0.5 || P.pan > PAN_MAX + 0.5 || P.tilt < TILT_MIN - 0.5 || P.tilt > TILT_MAX + 0.5)
        P.range_violation++;
}

/* ------------------------------------------------------------------ scenario + person */
typedef struct { int ms; double v; } seg_t;         /* a walking phase: duration, speed in steps/s along the pan world axis (+ = pan_max side) */
typedef struct {
    const char *name;
    seg_t seg[8];
    int nseg, total_ms;
    double w0;                                      /* initial person world position, pan steps */
    double wt0;                                     /* person world tilt position (motor steps) */
    int bh;                                         /* person box height, px (0 = 170) */
    int vis_from, vis_to;                           /* the person exists in [from, to) */
    int vis_from2, vis_to2;                         /* and again in this window (0 = none) */
    int metric_to;                                  /* the in-frame % is measured over [0, metric_to) (0 = total_ms) */
    int drop[4][2];                                 /* detection dropouts [from, to) ms */
    int ndrop;
} scen_t;

static double person_w(const scen_t *s, int t)
{
    double w = s->w0;
    int acc = 0, i;
    for (i = 0; i < s->nseg && acc < t; i++) {
        int d = t - acc < s->seg[i].ms ? t - acc : s->seg[i].ms;
        w += s->seg[i].v * d / 1000.0;
        acc += s->seg[i].ms;
    }
    return w;
}

static unsigned g_rng = 12345;
static int rnd(int n) { g_rng = g_rng * 1103515245u + 12345u; return (int)((g_rng >> 16) % (unsigned)n); }

typedef struct {
    double inframe;                                  /* % of the measured time the person centre is inside the frame */
    int first_nudge_ms, in_band_ms, nudges, reversals, homes, bad_speed, range_violation, home_ms;
    int64_t min_gap_ms;
    double pan_end, tilt_end, max_off_px, end_off_px, end_off_y;
    double overshoot_px, mean_abs_px;               /* camera AHEAD of a walking person (max, px) / mean |offset| while it walks (cumulative error) */
    int n_busy, n_stale;
} res_t;

static int g_variant;       /* 0 = the responsive policy (defaults), 1 = tl_cfg_legacy through the new code (SIM_LEGACY: the 0.1.0 sources) */
static double g_true_gain = 1000;
static double g_true_gain_t = 600;
static int g_return_home = 0, g_return_delay = 30, g_tilt = 0;

static void configure(a2k_track_t *t)
{
#ifdef SIM_LEGACY
    t->cfg.invert_pan = 1;
    (void)tl_init(&t->tl, &t->cfg);
#else
    struct tl_cfg c;
    if (g_variant == 1) tl_cfg_legacy(&c); else tl_cfg_dev(&c);
    c.invert_pan = 1;
    if (g_variant == 0) c.tilt_enable = g_tilt;
    if (g_variant == 0) { c.return_home = g_return_home; c.return_delay_s = g_return_delay; }
    if (a2k_track_set_cfg(t, &c) != 0) { printf("bad cfg\n"); exit(2); }
#endif
}

static int exists(const scen_t *s, int64_t t)
{
    return (t >= s->vis_from && t < s->vis_to) || (s->vis_to2 && t >= s->vis_from2 && t < s->vis_to2);
}

static res_t run(const scen_t *s)
{
    static double hp[MAXN], ht[MAXN];                 /* camera pose history, per step */
    static unsigned char hm[MAXN];
    a2k_track_t t;
    res_t r;
    int n = s->total_ms / DT, k, vis = 0, in = 0, mto = s->metric_to ? s->metric_to : s->total_ms;
    int64_t next_tick = 100, deliver = -1, cap_true = 0, tick_t = 0;
    double off = 0, offy = 0, sum_abs = 0, vsign = 0;
    int n_abs = 0;

    memset(&r, 0, sizeof r);
    g_n_busy = g_n_stale = 0;
    g_rng = 12345;
    for (k = 0; k < s->nseg; k++) if (s->seg[k].v != 0) { vsign = s->seg[k].v > 0 ? 1 : -1; break; }
    p_reset();
    a2k_track_init(&t, &OPS, NULL);
    configure(&t);
    a2k_track_set_enabled(&t, 1);
    for (k = 0; k < n; k++) {
        int64_t now = (int64_t)k * DT;
        double w;
        P.now = now;
        p_step();
        hp[k] = P.pan;
        ht[k] = P.tilt;
        hm[k] = (unsigned char)P.moving;
        w = person_w(s, (int)now);
        off = -(w - P.pan) * 320.0 / g_true_gain;                 /* px right of the centre: pan is inverted, a person at a larger pan faces left */
        offy = (P.tilt - s->wt0) * 192.0 / g_true_gain_t;          /* px below the centre */
        if (exists(s, now) && now < mto) {
            vis++;
            if (fabs(off) <= 160)
                in++;
        }
        if (exists(s, now) && now < mto && now > 500) {
            double ahead = off * vsign;                          /* off > 0 for a person walking + means the camera is past it */
            sum_abs += fabs(off);
            n_abs++;
            if (vsign != 0 && ahead > r.overshoot_px) r.overshoot_px = ahead;
        }
        if (exists(s, now)) {
            if (fabs(off) > r.max_off_px)
                r.max_off_px = fabs(off);
            if (r.in_band_ms == 0 && now > 0 && fabs(off) <= BAND_PX && fabs(offy) <= 29)
                r.in_band_ms = (int)now;
        }
        if (now >= next_tick) {
            tick_t = next_tick;
            cap_true = tick_t - 60;                                /* the frame is captured ~60 ms before the tick; the tracker is told the tick time */
            deliver = tick_t + 65 + rnd(45);                       /* inference 63..110 ms */
            next_tick += 200;
        }
        if (deliver >= 0 && now >= deliver) {
            int ci = (int)(cap_true / DT), nb = 0, dropped = 0, i;
            struct tl_box b = { 0, 0, 0, 0, 0, 0 };
            if (ci < 0) ci = 0;
            for (i = 0; i < s->ndrop; i++)
                if (cap_true >= s->drop[i][0] && cap_true < s->drop[i][1])
                    dropped = 1;
            if (exists(s, cap_true) && !dropped && !(hm[ci] && rnd(100) < 50)) { /* blurred frames lose the person half of the time */
                double wc = person_w(s, (int)cap_true);
                double cx = 160 - (wc - hp[ci]) * 320.0 / g_true_gain + rnd(7) - 3 + (hm[ci] ? rnd(25) - 12 : 0);
                double cy = 96 + (ht[ci] - s->wt0) * 192.0 / g_true_gain_t + rnd(5) - 2;
                int x1 = (int)(cx - 35), x2 = (int)(cx + 35), y1 = (int)(cy - 85), y2 = (int)(cy + 85);
                x1 = clampi(x1, 0, 320); x2 = clampi(x2, 0, 320); y1 = clampi(y1, 0, 192); y2 = clampi(y2, 0, 192);
                if (x2 - x1 >= 15 && y2 - y1 >= 15) {
                    b.x1 = x1; b.x2 = x2; b.y1 = y1; b.y2 = y2;
                    nb = 1;
                }
            }
#ifdef SIM_LEGACY
            a2k_track_tick(&t, deliver, &b, nb, 320, 192);
#else
            g_cap_cur = tick_t;
            a2k_track_tick_ts(&t, deliver, tick_t, &b, nb, 320, 192);
#endif
            deliver = -1;
        }
    }
    r.inframe = vis ? 100.0 * in / vis : 0;
    r.n_busy = g_n_busy;
    r.n_stale = g_n_stale;
    r.mean_abs_px = n_abs ? sum_abs / n_abs : 0;
    r.nudges = P.nudges;
    r.reversals = P.reversals;
    r.homes = P.homes;
    r.first_nudge_ms = (int)P.first_nudge_ms;
    r.home_ms = (int)P.first_home_ms;
    r.bad_speed = P.bad_speed;
    r.range_violation = P.range_violation;
    r.min_gap_ms = P.min_gap_ms;
    r.pan_end = P.pan;
    r.tilt_end = P.tilt;
    r.end_off_px = off;
    r.end_off_y = offy;
    if (r.in_band_ms == 0)
        r.in_band_ms = -1;
    a2k_track_destroy(&t);
    return r;
}

/* ------------------------------------------------------------------ scenarios */
static scen_t base(const char *name, int total)
{
    scen_t s;
    memset(&s, 0, sizeof s);
    s.name = name;
    s.w0 = HOME_PAN;
    s.wt0 = HOME_TILT;
    s.total_ms = total;
    s.vis_to = total;
    return s;
}

static void seg(scen_t *s, int ms, double v) { s->seg[s->nseg].ms = ms; s->seg[s->nseg].v = v; s->nseg++; }

static scen_t walk(double v)
{
    int walk_ms = (int)fmin(8000, (3600.0 - HOME_PAN) / v * 1000);
    scen_t s = base("walk", walk_ms + 4000);
    seg(&s, walk_ms, v);
    seg(&s, 4000, 0);
    s.metric_to = walk_ms;
    return s;
}

static void row(const char *tag, const res_t *r)
{
    printf("  %-14s in frame %5.1f %%  max offset %5.0f px  first move %5d ms  nudges %3d  reversals %2d  end offset %4.0f px\n", tag, r->inframe,
           r->max_off_px, r->first_nudge_ms, r->nudges, r->reversals, r->end_off_px);
}

static const double SPEEDS[] = { 50, 100, 150, 200, 250, 300, 350, 400, 450, 500 };
#define NSP ((int)(sizeof SPEEDS / sizeof SPEEDS[0]))
#ifndef SIM_X
#define SIM_X 300                  /* the walking speed (steps/s) up to which the person stays in the frame (>= 90 % of the time) */
#endif

int main(void)
{
    res_t a, b;
    scen_t s;
    int i;
    double maxv_new = 0, maxv_old = 0;

#ifdef SIM_LEGACY
    printf("== firmware 0.1.0 sources (the 'before'), pan inverted, true gain 1000 steps/frame\n");
#else
    printf("== responsive policy vs the 0.1.0 policy (tl_cfg_legacy), pan inverted, true gain 1000 steps/frame\n");
#endif
    (void)b;
    printf("-- constant walking speed (steps/s: 100 = 8.8 deg/s ~ 0.23 m/s at 1.5 m), start centred, camera at home\n");
    for (i = 0; i < NSP; i++) {
        char tag[96];
        s = walk(SPEEDS[i]);
        g_variant = 0;
        a = run(&s);
        snprintf(tag, sizeof tag, "v=%3.0f", SPEEDS[i]);
        row(tag, &a);
        if (a.inframe >= 90) maxv_new = SPEEDS[i];
#ifndef SIM_LEGACY
        g_variant = 1;
        b = run(&s);
        row("   (0.1.0)", &b);
        if (b.inframe >= 90) maxv_old = SPEEDS[i];
        if (SPEEDS[i] <= SIM_X) {
            snprintf(tag, sizeof tag, "v=%.0f steps/s: the person stays in the frame (>= 90 %%)", SPEEDS[i]);
            CHECK(a.inframe >= 90, tag);
        }
        CHECK(a.inframe + 0.01 >= b.inframe, "never worse than the 0.1.0 policy");
        CHECK(a.range_violation == 0 && b.range_violation == 0, "the soft range is never left");
        CHECK(a.bad_speed == 0, "only valid speed divisors are asked for");
        CHECK(a.nudges < 2 || a.min_gap_ms >= 300, "motion commands keep the 300 ms rate limit");
        printf("   v=%3.0f: nudges while busy %d, from a stale frame %d, overshoot (camera ahead) %.0f px, mean |offset| %.0f px\n", SPEEDS[i], a.n_busy, a.n_stale, a.overshoot_px, a.mean_abs_px);
        CHECK(a.n_busy == 0, "no nudge is ever sent while the motor is busy (nothing stacks)");
        CHECK(a.n_stale == 0, "every nudge is computed from a frame captured after the last motion end + 220 ms settle and after the last nudge started");
        CHECK(a.overshoot_px <= 40, "the camera never runs more than 40 px ahead of a walking person (no integrated stale offsets / lead)");
#endif
    }
    printf("   fastest walking speed with >= 90 %% in frame: responsive %.0f steps/s, 0.1.0 %.0f steps/s\n", maxv_new, maxv_old);

    printf("-- stationary and acquisition\n");
    s = base("centred", 20000);
    g_variant = 0; a = run(&s); row("centred 20 s", &a);
    CHECK(a.nudges == 0, "a centred person never moves the camera (no hunting on detector jitter)");
    s = base("appear", 10000);
    s.w0 = HOME_PAN - 312;                                   /* 100 px right of the centre */
    g_variant = 0; a = run(&s); row("appear +100px", &a);
    printf("   first motion command %d ms, person inside the central band at %d ms, end offset %.0f px\n", a.first_nudge_ms, a.in_band_ms, a.end_off_px);
#ifndef SIM_LEGACY
    g_variant = 1; b = run(&s); row("   (0.1.0)", &b);
    printf("   0.1.0: first motion command %d ms, inside the band at %d ms\n", b.first_nudge_ms, b.in_band_ms);
    CHECK(a.first_nudge_ms >= 0 && a.first_nudge_ms <= 700, "reacts within 700 ms of the person appearing off-centre (2 detections)");
    CHECK(a.in_band_ms >= 0 && a.in_band_ms <= 2500, "back inside the band within 2.5 s");
    CHECK(fabs(a.end_off_px) <= BAND_PX && a.reversals == 0, "settles in the band without reversing");
    CHECK(b.in_band_ms < 0 || a.in_band_ms < b.in_band_ms, "back inside the band sooner than the 0.1.0 policy");
#endif
    s = base("edge", 10000);
    s.w0 = HOME_PAN - 100;                                   /* 32 px: just outside the band */
    g_variant = 0; a = run(&s); row("just outside", &a);
    CHECK(a.nudges <= 3 && a.reversals == 0 && fabs(a.end_off_px) <= BAND_PX, "a person just outside the band: at most 3 corrections, no oscillation");

    printf("-- stop and go (walk 2 s at 250 steps/s, stand 3 s, twice)\n");
    s = base("stopgo", 10000);
    seg(&s, 2000, 250); seg(&s, 3000, 0); seg(&s, 2000, 250); seg(&s, 3000, 0);
    s.metric_to = 7000;
    g_variant = 0; a = run(&s); row("stop-and-go", &a);
    g_variant = 1; b = run(&s); row("   (0.1.0)", &b);
    CHECK(a.inframe >= 90 && fabs(a.end_off_px) <= BAND_PX, "stays in frame and ends centred");
    CHECK(a.reversals <= 2, "no hunting when the person stops");

    printf("-- reversal (+250 for 3 s, then -250 for 4 s)\n");
    s = base("reversal", 9000);
    seg(&s, 3000, 250); seg(&s, 4000, -250);
    s.metric_to = 7000;
    g_variant = 0; a = run(&s); row("reversal", &a);
    g_variant = 1; b = run(&s); row("   (0.1.0)", &b);
    CHECK(a.inframe >= 85, "follows a person who turns around");
    CHECK(a.reversals <= 4, "a reversal costs a few corrections, no sustained oscillation");

    printf("-- brief detection dropouts (200 steps/s; 0.6 s, 0.6 s, 1 s without detection)\n");
    s = walk(200);
    s.ndrop = 3; s.drop[0][0] = 2000; s.drop[0][1] = 2600; s.drop[1][0] = 4000; s.drop[1][1] = 4600; s.drop[2][0] = 6000; s.drop[2][1] = 7000;
    g_variant = 0; a = run(&s); row("dropouts", &a);
    g_variant = 1; b = run(&s); row("   (0.1.0)", &b);
    CHECK(a.inframe >= 85, "dropouts of up to 1 s: the person stays in frame");

    printf("-- gain error (the true steps per frame width differ from the 1000 assumed), 200 steps/s\n");
    {
        static const double G[] = { 700, 800, 1250, 1500 };
        for (i = 0; i < 4; i++) {
            char tag[32];
            g_true_gain = G[i];
            s = walk(200);
            g_variant = 0; a = run(&s);
            snprintf(tag, sizeof tag, "true %4.0f", G[i]);
            row(tag, &a);
            CHECK(a.inframe >= 85 && a.reversals <= 4, "tolerates +-30 % error of the px-per-step assumption without hunting");
        }
        g_true_gain = 1000;
    }

    printf("-- tilt (opt-in: track_tilt=1)\n");
    g_tilt = 1;
    s = base("tilt", 10000);
    s.wt0 = HOME_TILT - 150;                                  /* 48 px below the centre (a small person: the box is not clipped by the frame) */
    s.bh = 90;
    g_variant = 0; a = run(&s); row("person below", &a);
    printf("   tilt %d -> %.0f, vertical offset at the end %.0f px\n", HOME_TILT, a.tilt_end, a.end_off_y);
    CHECK(a.tilt_end < HOME_TILT - 50 && fabs(a.end_off_y) <= 29, "tilt tracking: the camera tilts down until the person is in the vertical band");
    s = base("tiltlimit", 20000);
    s.wt0 = 100;                                              /* below the lowest tilt the camera may reach */
    g_variant = 0; a = run(&s); row("below the range", &a);
    CHECK(a.tilt_end >= TILT_MIN - 0.5 && a.range_violation == 0 && a.nudges <= 6, "the tilt soft range is respected, no endless pushing at the limit");
    s = base("panlimit", 20000);
    s.w0 = HOME_PAN + 450;
    seg(&s, 12000, 250);                                      /* walks on, far beyond the pan range: 5 s of standing past the limit at the end */
    g_variant = 0; a = run(&s); row("beyond pan max", &a);
    CHECK(a.pan_end <= PAN_MAX + 0.5 && a.range_violation == 0 && a.nudges <= 40, "the pan soft range is respected, no endless pushing at the limit");

    g_tilt = 0;
    printf("-- return policy (the person walks 2 s at 250 steps/s and disappears at 2.5 s)\n");
    s = base("return", 200000);
    seg(&s, 2000, 250);
    s.vis_to = 2500;
    g_return_home = 0;
    g_variant = 0; a = run(&s);
    printf("  stay            homes %d, pan at the end %.0f (the person was last seen near %.0f)\n", a.homes, a.pan_end, person_w(&s, 2400));
    CHECK(a.homes == 0 && a.pan_end > 1500, "stay (default): 200 s without a person, the camera never goes home and stays where it was");
#ifndef SIM_LEGACY
    g_return_home = 1; g_return_delay = 30;
    a = run(&s);
    printf("  home, delay 30  first home request at %.1f s, pan at the end %.0f\n", a.home_ms / 1000.0, a.pan_end);
    CHECK(a.homes == 1 && a.home_ms >= 2500 + 3000 + 30000 - 400 && a.home_ms <= 2500 + 3000 + 30000 + 1200, "home, delay 30: one request about 33 s after the last sighting (3 s grace + 30 s)");
    CHECK(fabs(a.pan_end - HOME_PAN) < 10, "and the camera is back home");
    g_return_delay = 0;
    a = run(&s);
    printf("  home, delay 0   first home request at %.1f s\n", a.home_ms / 1000.0);
    CHECK(a.homes == 1 && a.home_ms >= 2500 + 3000 - 400 && a.home_ms <= 2500 + 3000 + 1200, "home, delay 0: right after the 3 s grace");
    g_return_delay = 30;
    s.vis_from2 = 20000; s.vis_to2 = 26000;                   /* it comes back after 17 s: before the delay ends */
    a = run(&s);
    printf("  back after 17 s homes %d, first request at %.1f s\n", a.homes, a.home_ms / 1000.0);
    CHECK(a.homes == 1 && a.home_ms >= 26000 + 3000 + 30000 - 400, "a person back before the delay ends: no return; the delay restarts from the new last sighting");
    g_return_home = 0;
#else
    (void)maxv_old;
#endif

    printf("-- safety: parked / calibrating / unknown / privacy never move the camera\n");
    {
        static const int st[4] = { HAL_PTZ_PARKED + 1, HAL_PTZ_CALIBRATING + 1, HAL_PTZ_UNKNOWN + 1, -1 };
        for (i = 0; i < 4; i++) {
            s = base("safety", 6000);
            s.w0 = HOME_PAN - 312;
            g_force_state = st[i];
            a = run(&s);
            CHECK(a.nudges == 0, "no motion in this state");
        }
        g_force_state = 0;
    }

    printf("rot2k track sim: %d failed\n", fails);
    return fails ? 1 : 0;
}
