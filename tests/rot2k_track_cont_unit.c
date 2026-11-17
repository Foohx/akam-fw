/* Host unit test of the targeting policy and the continuous mode of track_logic (pure policy): comfort zone and hysteresis, body / head reference, outlier boxes, the robust
 * velocity (feints), the 3D Kalman state (distance from the box, near / far consistency, approach changes distance not angles), the continuous velocity command (start, refresh,
 * stop, direction change, soft range, overshoot guard), the sweep back after an overshoot, and the replays of two hardware logs (a partial torso box: the camera must not go
 * down; a full-frame box and a premature search: the camera must not go up, no search right after the camera's own move). Also the CPU cost of one tick.
 * Build: tests/rot2k_overlay_test.sh. Prints "rot2k track cont unit: N failed". */
#include "../firmware/lsc-rotatable-2k-ak3918av130/app/track_logic.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int fails;
#define CHECK(c, msg) do { if (c) printf("  ok   - %s\n", msg); else { printf("  FAIL - %s (line %d)\n", msg, __LINE__); fails++; } } while (0)
#define FW 320
#define FH 192

static struct tl_ptz ptz(int pan, int tilt) {
    struct tl_ptz p;
    memset(&p, 0, sizeof p);
    p.pan = pan; p.tilt = tilt; p.pan_min = 300; p.pan_max = 3650; p.tilt_min = 300; p.tilt_max = 2100; p.tilt_home = 1300;
    return p;
}
static struct tl_box box(int cx, int cy, int w, int h) { struct tl_box b = { cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2, 0, 0 }; return b; }
static void cfg(struct tl_cfg *c, int motion) {
    tl_cfg_defaults(c);
    c->motion = motion;
    c->invert_pan = 1;
    c->tilt_enable = 1;
    c->return_home = 0;
}
static struct tl_out tick(tl_t *t, int64_t now, struct tl_box b, struct tl_ptz p) {
    struct tl_out o;
    p.cap_ms = now - 100;
    tl_update(t, now, &b, 1, FW, FH, &p, &o);
    return o;
}
/* the fake head: a nudge moves it at once (the motion ends 100 ms later), a velocity command moves it by 80 steps per tick; the pose at capture is always known (continuous) */
static void apply(struct tl_ptz *p, const struct tl_out *o, int64_t *T) {
    if (o->kind == TL_NUDGE) { p->pan += o->dpan; p->tilt -= o->dtilt; p->motion_end_ms = *T + 100; *T += 400; }
    if (o->kind == TL_VEL) { p->pan += o->pan_dir * 80; p->tilt -= o->tilt_dir * 40; }
    p->has_cap = 1; p->cap_pan = p->pan; p->cap_tilt = p->tilt;
}
static struct tl_out none(tl_t *t, int64_t now, struct tl_ptz p) {
    struct tl_out o;
    p.cap_ms = now - 100;
    tl_update(t, now, NULL, 0, FW, FH, &p, &o);
    return o;
}

/* ---------------------------------------------------------------- the pinhole room (same lens as the policy assumes) */
#define KRAD 654.0
#define FHZ 209.5
#define FV 179.4
static double g_hw = 0.25; /* half width of the person, m (arms out: 0.4) */
static struct tl_box proj(double x, double z, double h, double pan, double tilt, int *ok) {
    double yaw = -(pan - 1275) / KRAD, psi = (tilt - 1300) / KRAD, th = atan2(x, z), rho = hypot(x, z), dl = th - yaw, d = rho * cos(dl);
    double xc = FW / 2.0 + FHZ * tan(dl), hw = FHZ * g_hw / d, y1 = FH / 2.0 - FV * tan(fmax(-1.4, fmin(1.4, atan((h - 1.0) / rho) - psi))), y2 = FH / 2.0 - FV * tan(fmax(-1.4, fmin(1.4, atan(-1.0 / rho) - psi)));
    struct tl_box b;
    *ok = fabs(dl) < 1.2 && d > 0.3;
    b.x1 = (int)fmax(0, xc - hw); b.x2 = (int)fmin(FW - 1, xc + hw); b.y1 = (int)fmax(0, y1); b.y2 = (int)fmin(FH - 1, y2);
    if (b.x2 <= b.x1 || b.y2 <= b.y1) *ok = 0;
    return b;
}

static void zone_cases(void) {
    tl_t t; struct tl_cfg c; struct tl_out o; struct tl_ptz p = ptz(1275, 1300); int moved;
    printf("== comfort zone and reference point (steps and continuous)\n");
    for (int mode = 0; mode < 2; mode++) {
        cfg(&c, mode); c.lead_pct = 0; tl_init(&t, &c);
        moved = 0;
        for (int i = 0; i < 12; i++) { o = tick(&t, 1000 + i * 200, box(160 + 40, 96, 40, 110), p); moved += o.kind != TL_NONE; } /* +40 px: inside +-64 */
        CHECK(moved == 0, mode ? "continuous: a whole body 40 px off-centre (inside the 40 % zone): no motion" : "steps: a whole body 40 px off-centre (inside the 40 % zone): no motion");
        tl_init(&t, &c); moved = 0;
        for (int i = 0; i < 6; i++) { o = tick(&t, 1000 + i * 200, box(160 + 100, 96, 40, 110), p); moved += o.kind != TL_NONE; }
        CHECK(moved >= 1, mode ? "continuous: 100 px off-centre (outside the zone): it moves" : "steps: 100 px off-centre (outside the zone): it moves");
        if (mode == 1) CHECK(o.kind == TL_VEL && o.pan_dir < 0 && o.pan_div >= 2 && o.pan_div <= 20, "continuous: target on the right -> pan direction toward it (invert_pan: motor -), valid divisor");
    }
    /* the head rule applies to a close or cut person, the body rule (centre of the box) to a whole one */
    cfg(&c, TL_MOTION_STEPS); tl_init(&t, &c);
    o = tick(&t, 1000, box(160, 96, 40, 110), p); o = tick(&t, 1200, box(160, 96, 40, 110), p);
    CHECK(t.dbg.rule == 4, "a whole body (57 % of the height, not cut): rule 4, the centre of the box");
    tl_init(&t, &c); o = tick(&t, 1000, box(160, 96, 80, 170), p);
    CHECK(t.dbg.rule == 2, "a close body (89 % tall): the head rule");
    tl_init(&t, &c); { struct tl_box b = { 100, 80, 200, 191, 0, 0 }; o = tick(&t, 1000, b, p); }
    CHECK(t.dbg.rule == 2, "a body cut by the bottom edge (58 % tall): the head rule");
    tl_init(&t, &c); { struct tl_box b = { 100, 0, 200, 150, 0, 0 }; o = tick(&t, 1000, b, p); }
    CHECK(t.dbg.rule == 1, "head cut by the top edge: head clip");
    tl_init(&t, &c); { struct tl_box b = { 10, 0, 315, 191, 0, 0 }; o = tick(&t, 1000, b, p); }
    CHECK(t.dbg.rule == 5, "cut at the top AND the bottom and 95 % wide: ambiguous (rule 5)");
    /* hysteresis between the body and the head rule near the threshold */
    cfg(&c, TL_MOTION_STEPS); tl_init(&t, &c);
    o = tick(&t, 1000, box(160, 96, 60, 164), p);                       /* 85 % tall: head */
    o = tick(&t, 1200, box(160, 96, 60, 154), p);                       /* 80 %: still head (hysteresis) */
    CHECK(t.dbg.rule == 2, "85 % then 80 % tall: still the head rule (hysteresis, no flapping)");
    o = tick(&t, 1400, box(160, 96, 60, 140), p);
    CHECK(t.dbg.rule == 4, "73 % tall: back to the body rule");
    /* jitter of the reference (head / arm swing) never starts a motion */
    cfg(&c, TL_MOTION_CONT); tl_init(&t, &c); moved = 0;
    for (int i = 0; i < 40; i++) {
        int jx = (i * 7) % 13 - 6, jy = (i * 5) % 9 - 4, jw = (i * 3) % 7;
        o = tick(&t, 1000 + i * 200, box(160 + 45 + jx, 96 + jy, 40 + jw * 4, 110 + jy), p);   /* 45 px off: inside the zone, +-6 px jitter and +-12 px width swing */
        moved += o.kind != TL_NONE;
    }
    CHECK(moved == 0, "continuous: +-6 px / +-4 px jitter and arm swing around a point inside the zone: never moves");
    /* a target that walks out is predicted: the start comes before it leaves the zone */
    cfg(&c, TL_MOTION_CONT); tl_init(&t, &c);
    { int first = -1, cx = 170; for (int i = 0; i < 20 && first < 0; i++) { cx = 165 + i * 14; o = tick(&t, 1000 + i * 200, box(cx, 96, 40, 110), p); if (o.kind == TL_VEL) first = cx - 160; } /* 70 px/s */
      CHECK(first >= 0 && first < 64, "continuous: a target walking out of the zone starts the camera BEFORE it is outside (predicted exit within 0.5 s)"); }
}

static void cont_cases(void) {
    tl_t t; struct tl_cfg c; struct tl_out o; struct tl_ptz p = ptz(1275, 1300); int stops = 0, vels = 0, i;
    printf("== continuous command: refresh, stop, direction change, soft range, lease\n");
    cfg(&c, TL_MOTION_CONT); c.lead_pct = 0; tl_init(&t, &c);
    for (i = 0; i < 4; i++) { o = tick(&t, 1000 + i * 200, box(160 + 110, 96, 40, 110), p); vels += o.kind == TL_VEL; }
    CHECK(vels >= 2 && o.kind == TL_VEL, "a target outside the zone: a velocity command on every tick (the refresh keeps the lease alive)");
    CHECK(o.pan_dir == -1 && o.tilt_dir == 0 && o.tilt_div == 0, "pan only: no tilt");
    p.pan = 1000;
    o = tick(&t, 2000, box(160 + 10, 96, 40, 110), p);
    CHECK(o.kind == TL_STOP, "back near the centre: one TL_STOP");
    o = tick(&t, 2200, box(160 + 8, 96, 40, 110), p); stops = o.kind == TL_STOP;
    CHECK(o.kind == TL_NONE && !stops, "... and then nothing (no repeated stops)");
    /* direction change without a stop in between: the person reverses */
    tl_init(&t, &c); p = ptz(1275, 1300);
    for (i = 0; i < 3; i++) o = tick(&t, 1000 + i * 200, box(160 + 120, 96, 40, 110), p);
    CHECK(o.kind == TL_VEL && o.pan_dir < 0, "person on the right: camera turns right (motor -)");
    for (i = 0; i < 3; i++) o = tick(&t, 1800 + i * 200, box(160 - 120, 96, 40, 110), p);
    CHECK(o.kind == TL_VEL && o.pan_dir > 0, "person jumps to the left: the direction changes in one tick (no stop command needed)");
    /* diagonal: both axes */
    tl_init(&t, &c); p = ptz(1275, 1300);
    for (i = 0; i < 3; i++) o = tick(&t, 1000 + i * 200, box(160 + 120, 96 + 70, 40, 30), p); /* a far, small person low and right: body centre 70 px below */
    CHECK(o.kind == TL_VEL && o.pan_dir != 0 && o.tilt_dir != 0, "a target right and below the zone: a diagonal command (both axes)");
    CHECK(o.tilt_dir > 0, "below the centre -> image sign tilt_dir > 0 (the tracker maps it to motor down)");
    /* soft range: no command toward a limit the head is at */
    tl_init(&t, &c); p = ptz(1275, 1300); p.pan = 3640; p.pan_max = 3650;
    for (i = 0; i < 4; i++) o = tick(&t, 1000 + i * 200, box(40, 96, 40, 110), p); /* person on the left: motor + (invert), the head is at the limit */
    CHECK(o.kind != TL_VEL || o.pan_dir == 0, "at the pan soft limit: no velocity toward it");
    /* a stale frame (pose at capture unknown) refreshes the running command, learns nothing */
    tl_init(&t, &c); p = ptz(1275, 1300);
    for (i = 0; i < 3; i++) o = tick(&t, 1000 + i * 200, box(160 + 110, 96, 40, 110), p);
    p.moving = 1; p.has_cap = 0;
    o = tick(&t, 1600, box(160 + 110, 96, 40, 110), p);
    CHECK(o.kind == TL_VEL && !strcmp(t.dbg.why, "stale"), "a frame whose pose is unknown (no driver sample): the running command is refreshed, nothing is learnt");
    /* frames during motion are NOT stale when the pose at capture is known: the position-compensated box */
    tl_init(&t, &c); p = ptz(1275, 1300);
    for (i = 0; i < 3; i++) o = tick(&t, 1000 + i * 200, box(160 + 110, 96, 40, 110), p);
    p.moving = 1; p.has_cap = 1; p.cap_pan = 1200; p.cap_tilt = 1300; p.pan = 1180; /* the head turned toward the person: the box is now nearer to the centre in the new pose */
    o = tick(&t, 1600, box(160 + 30, 96, 40, 110), p);
    CHECK(strcmp(t.dbg.why, "stale") != 0, "a frame captured while moving is used when the pose at capture is known");
}

static void vel_cases(void) {
    tl_t t; struct tl_cfg c; struct tl_out o; struct tl_ptz p = ptz(1275, 1300);
    printf("== robust velocity: a feint is not extrapolated, a steady walk is, a reversal / deceleration drops it\n");
    cfg(&c, TL_MOTION_CONT); tl_init(&t, &c); p.has_cap = 1; p.cap_pan = 1275; p.cap_tilt = 1300;
    { int xs[] = { 160, 160, 168, 178, 178, 178 };
      for (int i = 0; i < 6; i++) { o = tick(&t, 1000 + i * 200, box(xs[i], 96, 40, 110), p); (void)o; }
      CHECK(t.vx == 0, "a short burst (2 ticks) then stop: no velocity, no lead"); }
    tl_init(&t, &c);
    { for (int i = 0; i < 8; i++) { o = tick(&t, 1000 + i * 200, box(160 - i * 14, 96, 40, 110), p); (void)o; }
      CHECK(t.vx != 0 && abs(t.vx) <= 600, "a steady walk over >= 3 ticks: a velocity, capped");
      o = tick(&t, 1000 + 8 * 200, box(160 - 7 * 14 + 6, 96, 40, 110), p); /* turns around */
      CHECK(t.vx == 0, "a reversal: the velocity is dropped at once"); }
    /* the lead never leads the person out of the frame (steps) */
    cfg(&c, TL_MOTION_STEPS); c.lead_pct = 100; c.lead_max = 2000; tl_init(&t, &c);
    { struct tl_out last; last.kind = TL_NONE; for (int i = 0; i < 10; i++) { last = tick(&t, 1000 + i * 300, box(160 + 70 + i * 6, 96, 60, 110), p); if (last.kind == TL_NUDGE) break; }
      CHECK(last.kind == TL_NUDGE && abs(last.dpan) <= 1000 * (FW / 2) / FW + 60, "the nudge (target + lead) stays within about half a frame"); }
}

static void kf_cases(void) {
    tl_t t; struct tl_cfg c; struct tl_ptz p = ptz(1275, 1300); struct tl_out o; int ok; (void)o;
    printf("== 3D state: distance from the box, angular rate = lateral speed / distance\n");
    cfg(&c, TL_MOTION_STEPS); c.zone_x_pct = 90; tl_init(&t, &c);
    { struct tl_box b = proj(0.0, 3.0, 1.7, 1275, 1300, &ok); for (int i = 0; i < 6; i++) o = tick(&t, 1000 + i * 200, b, p);
      CHECK(t.dist_mm > 3000 * 0.8 && t.dist_mm < 3000 * 1.2, "a whole person 3.0 m away: the distance estimate within +-20 %"); }
    tl_init(&t, &c);
    { struct tl_box b = proj(0.0, 4.5, 1.7, 1275, 1300, &ok); for (int i = 0; i < 6; i++) o = tick(&t, 1000 + i * 200, b, p);
      CHECK(t.dist_mm > 4500 * 0.8 && t.dist_mm < 4500 * 1.2, "... 4.5 m away: within +-20 %"); }
    /* near / far consistency: the same lateral speed (1 m/s) gives angular rates inversely proportional to the distance */
    {
        int rate[2]; double zs[2] = { 2.0, 4.0 };
        for (int k = 0; k < 2; k++) {
            tl_init(&t, &c);
            for (int i = 0; i < 12; i++) { struct tl_box b = proj(-0.6 + 1.0 * i * 0.2, zs[k], 1.7, 1275, 1300, &ok); o = tick(&t, 1000 + i * 200, b, p); }
            rate[k] = abs(t.vx);
        }
        printf("   angular rate (steps/s) for 1 m/s at 2 m: %d, at 4 m: %d (ratio %.2f, ideal 2.0)\n", rate[0], rate[1], rate[1] ? (double)rate[0] / rate[1] : 0.0);
        CHECK(rate[0] > 0 && rate[1] > 0 && (double)rate[0] / rate[1] > 1.6 && (double)rate[0] / rate[1] < 2.5, "near / far consistency: the angular rate scales with 1 / distance within +-25 %");
    }
    /* approach: a person walking toward the camera along the line of sight changes the distance, not the angles */
    tl_init(&t, &c);
    for (int i = 0; i < 12; i++) { double z = 4.0 - 0.25 * i; struct tl_box b = proj(z * 0.2, z, 1.7, 1275, 1300, &ok); o = tick(&t, 1000 + i * 200, b, p); }
    CHECK(abs(t.vx) < 60, "approaching along a ray (1.25 m/s): the angular rate stays ~0 (no useless pan)");
}

/* ---------------------------------------------------------------- replays of two hardware logs */
static void replay_cases(void) {
    printf("== replay: partial torso box (the camera went all the way down)\n");
    for (int mode = 0; mode < 2; mode++) {
        tl_t t; struct tl_cfg c; struct tl_out o; struct tl_ptz p = ptz(932, 1580); int down = 0; int64_t T = 833647 - 1600;
        cfg(&c, mode); tl_init(&t, &c);
        for (int i = 0; i < 8; i++) { struct tl_box b = { 96, 27, 280, 191, 0, 0 }; o = tick(&t, T + i * 200, b, p); down += (o.kind == TL_NUDGE && o.dtilt > 0) || (o.kind == TL_VEL && o.tilt_dir > 0); } /* the head near the top: rule near, top at row 28 */
        { struct tl_box b = { 97, 99, 279, 191, 0, 0 }; o = tick(&t, 833647, b, p); down += (o.kind == TL_NUDGE && o.dtilt > 0) || (o.kind == TL_VEL && o.tilt_dir > 0); } /* the partial box that appeared suddenly */
        CHECK(!strcmp(t.dbg.why, "outlier") && !down, mode ? "continuous: the sudden partial box (top 99 instead of 28) is held as an outlier: no downward tilt" : "steps: the sudden partial box (top 99 instead of 28) is held as an outlier: no downward tilt");
        p.moving = 1; o = none(&t, 834272, p);
        p.moving = 0; p.motion_end_ms = 834300;
        for (int i = 0; i < 12; i++) { o = none(&t, 834693 + i * 200, p); down += (o.kind == TL_NUDGE && o.dtilt > 0) || (o.kind == TL_VEL && o.tilt_dir > 0 && strcmp(t.dbg.why, "tilt-back")); } /* the tilt-back toward home is no exit search */
        CHECK(!down, mode ? "continuous: no DOWN exit search from a single ambiguous box (needs a downward trend over >= 3 fresh boxes)" : "steps: no DOWN exit search from a single ambiguous box (needs a downward trend over >= 3 fresh boxes)");
    }
    /* a REAL downward trend still searches down */
    for (int mode = 0; mode < 2; mode++) {
        tl_t t; struct tl_cfg c; struct tl_out o; struct tl_ptz p = ptz(1275, 1300); int down = 0; int64_t T = 1000;
        cfg(&c, mode); c.lead_pct = 0; tl_init(&t, &c);
        p.has_cap = 1; p.cap_pan = p.pan; p.cap_tilt = p.tilt; /* the head does not follow here: the tops really sink in the world */
        for (int i = 0; i < 6; i++) { struct tl_box b = { 120, 30 + i * 16, 200, 191, 0, 0 }; o = tick(&t, T, b, p); T += o.kind == TL_NUDGE ? 400 : 200; if (o.kind == TL_NUDGE) p.motion_end_ms = T - 350; }
        p.motion_end_ms = T - 500;
        for (int i = 0; i < 14; i++) { o = none(&t, T, p); T += 200; down += (o.kind == TL_NUDGE && o.dtilt > 0) || (o.kind == TL_VEL && o.tilt_dir > 0); if (o.kind == TL_NUDGE) { p.motion_end_ms = T - 100; T += 400; } }
        CHECK(down >= 1, mode ? "continuous: a real downward trend (tops 30, 46, 62, ... then lost): the exit search goes down" : "steps: a real downward trend (tops 30, 46, 62, ... then lost): the exit search goes down");
    }
    printf("== replay: person fills the frame, then a premature search and a head clip going up\n");
    for (int mode = 0; mode < 2; mode++) {
        tl_t t; struct tl_cfg c; struct tl_out o; struct tl_ptz p = ptz(1000, 1300); int up = 0, search = 0; int64_t T = 1096216;
        cfg(&c, mode); tl_init(&t, &c);
        for (int i = 0; i < 5; i++) { struct tl_box b = { 210, 9, 319, 191, 0, 0 }; o = tick(&t, T - 1000 + i * 200, b, p); }
        { struct tl_box b = { 210, 9, 319, 191, 0, 0 }; o = tick(&t, T, b, p); }
        p.moving = 1;
        o = none(&t, 1096845, p);                         /* stale: the head is moving */
        p.moving = 0; p.motion_end_ms = 1097000;
        o = none(&t, 1097405, p);                         /* the first fresh frame without a person, 1.2 s after the last detection, 0.4 s after the motion */
        search += !strcmp(t.dbg.why, "exit-search") || o.kind == TL_NUDGE || (o.kind == TL_VEL && !strcmp(t.dbg.why, "exit-search"));
        CHECK(!search, mode ? "continuous: no exit search at the first fresh no-box frame right after the camera's own move" : "steps: no exit search at the first fresh no-box frame right after the camera's own move");
        { struct tl_box b = { 0, 0, 297, 191, 0, 0 }; p.cap_ms = 1098035 - 100; p.moving = 1; tl_update(&t, 1098035, &b, 1, FW, FH, &p, &o); p.moving = 0; }
        { struct tl_box b = { 0, 0, 304, 191, 0, 0 }; p.motion_end_ms = 1098300; o = tick(&t, 1098456 + 400, b, p); up += (o.kind == TL_NUDGE && o.dtilt < 0) || (o.kind == TL_VEL && o.tilt_dir < 0); }
        CHECK(!up && t.dbg.rule == 5, mode ? "continuous: a box cut at top and bottom and 95 % wide: no head-clip up" : "steps: a box cut at top and bottom and 95 % wide: no head-clip up");
    }
}

/* review fixes: a stale stretch (a manual move) must not resurrect a move; a diagonal never at divisor 2; no command near a limit */
static void review_cases(void) {
    tl_t t; struct tl_cfg c; struct tl_out o; struct tl_ptz p = ptz(1275, 1300); struct tl_box b = { 280, 40, 316, 150, 0, 0 }; int64_t now = 100000; int i, vels = 0, resurrect = 0;
    printf("== review fixes: no resurrected move, diagonal divisor, limit margin\n");
    cfg(&c, TL_MOTION_CONT); c.invert_pan = 1; tl_init(&t, &c);
    for (i = 0; i < 6; i++, now += 200) { p.has_cap = 1; p.cap_pan = p.pan; p.cap_tilt = p.tilt; o = tick(&t, now, b, p); vels += o.kind == TL_VEL; }
    CHECK(vels >= 2, "setup: a move is running");
    p.has_cap = 0; p.moving = 1; /* a manual HOME runs for 6 s: the pose is unknown */
    for (i = 0; i < 30; i++, now += 200) { p.cap_ms = now - 100; tl_update(&t, now, &b, 1, FW, FH, &p, &o); if (i * 200 >= 700 && o.kind == TL_VEL) resurrect++; }
    CHECK(resurrect == 0, "a stale stretch re-issues the last command for at most the lease (450 ms) after the last REAL command, never forever");
    p.moving = 0; p.motion_end_ms = now - 50; p.cap_ms = now - 150;
    tl_update(&t, now, NULL, 0, FW, FH, &p, &o);
    CHECK(o.kind != TL_VEL, "after the manual move ended: no velocity command from the old move");
    /* diagonal: both axes at divisor 3 or slower */
    cfg(&c, TL_MOTION_CONT); tl_init(&t, &c); p = ptz(1275, 1300); now = 1000;
    for (i = 0; i < 4; i++, now += 200) o = tick(&t, now, box(160 + 150, 96 + 80, 40, 30), p);
    CHECK(o.kind == TL_VEL && o.pan_dir && o.tilt_dir && o.pan_div >= 3 && o.tilt_div >= 3, "a diagonal command never uses divisor 2 (both axes >= 3)");
    cfg(&c, TL_MOTION_CONT); tl_init(&t, &c); p = ptz(1275, 1300); now = 1000;
    for (i = 0; i < 4; i++, now += 200) o = tick(&t, now, box(160 + 150, 96, 40, 110), p);
    CHECK(o.kind == TL_VEL && o.pan_div == 2 && o.tilt_dir == 0, "a single axis may still use divisor 2");
    /* margin to a soft limit: 45 steps (the safe-layer margin at divisor 2 is 41) */
    cfg(&c, TL_MOTION_CONT); c.invert_pan = 0; tl_init(&t, &c); p = ptz(1275, 1300); p.pan = 3650 - 44; now = 1000; vels = 0;
    for (i = 0; i < 4; i++, now += 200) { o = tick(&t, now, box(160 + 150, 96, 40, 110), p); vels += o.kind == TL_VEL; }
    CHECK(vels == 0, "44 steps from the soft limit: no command toward it");
}

static void sweep_cases(void) {
    printf("== lost after an overshoot: back toward the pose before the large move\n");
    for (int mode = 0; mode < 2; mode++) {
        tl_t t; struct tl_cfg c; struct tl_out o; struct tl_ptz p = ptz(1275, 1300); int pre = 1275; int swept = 0, passed = 0; int64_t T = 1000;
        cfg(&c, mode); c.lead_pct = 0; tl_init(&t, &c);
        /* a person far to the right: a large move; the head then ends up 600 steps away */
        for (int i = 0; i < 4; i++) { o = tick(&t, T, box(160 + 135, 96, 36, 100), p); T += 200; apply(&p, &o, &T); }
        for (int i = 0; i < 8 && abs(p.pan - pre) < 420; i++) { o = tick(&t, T, box(160 + 135, 96, 36, 100), p); T += 200; apply(&p, &o, &T); }
        CHECK(abs(p.pan - pre) >= 300, "setup: the head made a large move (>= 1/3 of a frame)");
        { int far = p.pan;
          p.motion_end_ms = T - 400;
          for (int i = 0; i < 18; i++) {
              o = none(&t, T, p); T += 200;
              if (!strcmp(t.dbg.why, "sweep-back")) swept++;
              apply(&p, &o, &T);
              if ((far < pre && p.pan > pre + 5) || (far > pre && p.pan < pre - 5)) passed = 1;
          }
          CHECK(swept >= 1, mode ? "continuous: lost shortly after a large move: a controlled sweep back toward the previous pose" : "steps: lost shortly after a large move: a sweep-back nudge toward the previous pose");
          CHECK(!passed, "... never beyond the previous pose");
        }
        /* a detection while sweeping stops the sweep: the normal logic takes over */
        tl_init(&t, &c); p = ptz(1275, 1300); T = 1000;
        for (int i = 0; i < 12 && abs(p.pan - pre) < 420; i++) { o = tick(&t, T, box(160 + 135, 96, 36, 100), p); T += 200; apply(&p, &o, &T); }
        p.motion_end_ms = T - 400;
        { int started = 0;
          for (int i = 0; i < 10 && !started; i++) { o = none(&t, T, p); T += 200; started = !strcmp(t.dbg.why, "sweep-back"); apply(&p, &o, &T); }
          o = tick(&t, T, box(160 + 10, 96, 36, 100), p);
          CHECK(started && strcmp(t.dbg.why, "sweep-back") != 0 && t.sweeping == 0, "a person detected during the sweep: the sweep ends");
        }
    }
    /* an exit through the edge is searched in the exit direction, not swept back */
    {
        tl_t t; struct tl_cfg c; struct tl_out o; struct tl_ptz p = ptz(1275, 1300); int64_t T = 1000; int sweep = 0, exits = 0;
        cfg(&c, TL_MOTION_CONT); c.lead_pct = 0; tl_init(&t, &c);
        for (int i = 0; i < 6; i++) { struct tl_box b = { 285, 40, 319, 150, 0, 0 }; o = tick(&t, T, b, p); T += 200; if (o.kind == TL_VEL) p.pan += o.pan_dir * 80; }
        for (int i = 0; i < 12; i++) { o = none(&t, T, p); T += 200; sweep += !strcmp(t.dbg.why, "sweep-back"); exits += !strcmp(t.dbg.why, "exit-search"); }
        CHECK(sweep == 0 && exits >= 1, "lost at the right edge: the exit search continues that way, no sweep-back");
    }
}

/* closed loop: the head follows the velocity command (1100 / divisor steps/s), the person is rendered by the pinhole model, a detector that needs >= 18 px of box */
static void head_loop(struct tl_ptz *p, const struct tl_out *o, double dt) {
    if (o->kind == TL_VEL) {
        if (o->pan_dir) p->pan += (int)(o->pan_dir * (1100.0 / o->pan_div) * dt);
        if (o->tilt_dir) p->tilt -= (int)(o->tilt_dir * (1100.0 / o->tilt_div) * dt);
    }
    if (p->tilt > 2100) p->tilt = 2100;
    if (p->tilt < 300) p->tilt = 300;
    p->has_cap = 1; p->cap_pan = p->pan; p->cap_tilt = p->tilt;
}

static void real_headout_cases(void) {
    tl_t t; struct tl_cfg c; struct tl_out o; struct tl_ptz p = ptz(1275, 1300); int64_t T = 1000; int ok, upl = 0, rule5 = 0, maxtilt = 1300;
    printf("== head-out of an approaching person (rule 5 must not block a REAL head-out) and a head-only crop going down\n");
    cfg(&c, TL_MOTION_CONT); c.lead_pct = 0; tl_init(&t, &c); g_hw = 0.4; /* arms out: the box gets wider than 80 % of the frame when close */
    p.has_cap = 1; p.cap_pan = p.pan; p.cap_tilt = p.tilt;
    for (int i = 0; i < 40; i++, T += 200) { /* 3.0 m -> 0.38 m at 1.5 m/s, then it stands there */
        double z = 3.0 - 1.5 * 0.2 * i; struct tl_box b;
        if (z < 0.38) z = 0.38;
        b = proj(0.05, z, 1.7, p.pan, p.tilt, &ok);
        if (ok) o = tick(&t, T, b, p); else o = none(&t, T, p);
        upl += t.dbg.upl; rule5 += t.dbg.rule == 5;
        if (getenv("TL_TRACE")) printf("    A i=%d z=%.2f ok=%d box=%d,%d,%d,%d tilt=%d why=%s rule=%d kind=%d tdir=%d clip=%d\n", i, z, ok, b.x1, b.y1, b.x2, b.y2, p.tilt, t.dbg.why ? t.dbg.why : "?", t.dbg.rule, o.kind, o.tilt_dir, t.clip_n);
        head_loop(&p, &o, 0.2);
        if (p.tilt > maxtilt) maxtilt = p.tilt;
    }
    g_hw = 0.25;
    printf("   tilt 1300 -> %d (max %d), ticks read as a real head-out %d, as ambiguous %d\n", p.tilt, maxtilt, upl, rule5);
    CHECK(maxtilt >= 1300 + 250, "an approaching person (head rising before it is cut): the camera keeps following the head up although the box is cut top and bottom and 86 % wide");
    CHECK(maxtilt <= 1300 + 700 + 60, "... bounded by the near-frame budget (+700 steps from the last well framed tilt, and the last step)");
    CHECK(upl >= 1, "... through the head-out reading of the ambiguous box");
    /* without the rising head (hardware log 2) the same box stays ambiguous: covered by replay_cases */
    printf("== camera tilted up, the person sits down: only the head is left at the bottom of the frame\n");
    cfg(&c, TL_MOTION_CONT); c.lead_pct = 0; tl_init(&t, &c); p = ptz(1275, 1406); p.has_cap = 1; p.cap_pan = p.pan; p.cap_tilt = p.tilt; T = 1000; /* the head is at the head row: the camera looks up by about 106 steps */
    {
        int minilt = p.tilt, up = 0, last_ok = 0, ntilt0 = p.tilt, srch = 0;
        for (int i = 0; i < 40; i++, T += 200) {
            double h = i < 6 ? 1.7 : i < 7 ? 1.4 : i < 8 ? 1.05 : i < 9 ? 0.78 : 0.6; struct tl_box b; /* the person sits down in 0.8 s (about 1.5 m/s of head speed, faster than the tilt can follow); an ABRUPT single-frame drop of the top stays held as an outlier (hardware log 1: it cannot be told from a partial box): a head-only crop at the bottom, then nothing */
            b = proj(0.2, 1.2, h, p.pan, p.tilt, &ok);
            if (i >= 9 && ok && b.y2 >= 190 && b.y2 - b.y1 < 90) ok = 0; /* a head-only crop of a person cut by the bottom edge is not detected */
            last_ok = ok;
            if (ok) o = tick(&t, T, b, p); else o = none(&t, T, p);
            if (getenv("TL_TRACE")) printf("    i=%d h=%.2f ok=%d box=%d,%d,%d,%d tilt=%d why=%s rule=%d kind=%d tdir=%d\n", i, h, ok, b.x1, b.y1, b.x2, b.y2, p.tilt, t.dbg.why ? t.dbg.why : "?", t.dbg.rule, o.kind, o.tilt_dir);
            if (i >= 6 && o.kind == TL_VEL && o.tilt_dir < 0) up++;
            srch += t.dbg.why && !strcmp(t.dbg.why, "exit-search");
            head_loop(&p, &o, 0.2);
            if (p.tilt < minilt) minilt = p.tilt;
        }
        (void)ntilt0;
        printf("   tilt 1406 -> %d (min %d), upward commands %d, person detected at the end: %d\n", p.tilt, minilt, up, last_ok);
        CHECK(minilt <= 1406 - 130, "the head sinks to the bottom edge and is lost: the camera follows it down (search on the filtered downward motion)");
        CHECK(srch >= 1, "... through the exit search DOWN (allowed by the filtered downward motion)");
        CHECK(up == 0, "... and never up");
        CHECK(last_ok, "... until the person is detected again");
    }
}

/* ---------------------------------------------------------------- the lost-target chain */
static struct tl_out chain_none(tl_t *t, int64_t now, struct tl_ptz p) { struct tl_out o; p.cap_ms = now - 100; tl_update(t, now, NULL, 0, FW, FH, &p, &o); return o; }

static void lost_chain_cases(void) {
    struct tl_cfg c, bad;
    char txt[200];
    printf("== lost-target chain: syntax, steps, ordering, re-acquisition\n");
    tl_cfg_defaults(&c);
    CHECK(tl_lost_parse("backsweep,exit,local:300:4:20,home", &c) == 0 && c.lost_n == 4 && c.lost[3].kind == TLS_HOME && c.lost[2].a == 300 && c.lost[2].b == 4 && c.lost[2].c == 20, "a valid chain is stored");
    tl_lost_format(&c, txt, sizeof txt);
    CHECK(!strcmp(txt, "backsweep,exit,local:300:4:20,home"), "... and formats back to the same text");
    CHECK(tl_lost_parse("none", &c) == 0 && c.lost_n == 0, "none = do nothing");
    CHECK(tl_lost_parse("route:patrol1:45,stay", &c) == 0 && c.lost_n == 2 && c.lost[0].b == 45 && !strcmp(c.lost[0].name, "patrol1"), "route:name:N and stay");
    CHECK(tl_lost_parse("route:r1,hold:5", &c) == 0 && c.lost[0].b == 60, "a route without a time lasts 60 s");
    CHECK(tl_lost_parse(TL_LOST_DEFAULT, &c) == 0 && c.lost_n == -1, "the default chain selects the legacy return policy");
    tl_cfg_defaults(&c); (void)tl_lost_parse("hold:7,home", &c); bad = c;
    {
        static const char *const BAD[] = { "", "hold", "hold:0", "hold:3601", "hold:x", "hold:5:5", "backsweep:1", "exit,", ",exit", "exit,,home", "stay,home", "local:30:4:20", "local:300:1:20", "local:300:4:0", "local:300:4",
            "route:", "route:Bad", "route:1abc", "route:abcdefghijklmnopq", "route:a:0", "route:a:3601", "home,home,home,home,home,home,home,home,home", "fly", "none,home", "hold:5 ,home" };
        int ok = 1;
        for (unsigned i = 0; i < sizeof BAD / sizeof BAD[0]; i++) {
            struct tl_cfg t2 = c;
            if (tl_lost_parse(BAD[i], &t2) == 0) { printf("   accepted: '%s'\n", BAD[i]); ok = 0; }
            if (memcmp(&t2, &c, sizeof c) != 0) { printf("   touched the config: '%s'\n", BAD[i]); ok = 0; }
        }
        CHECK(ok, "invalid chains are refused and leave the config untouched (all or nothing)");
        CHECK(memcmp(&bad, &c, sizeof c) == 0 && c.lost_n == 2, "the previous chain is still in place");
    }
    /* hold:2 then home: nothing moves for 2 s, then TL_HOME once */
    {
        tl_t t; struct tl_out o; struct tl_ptz p = ptz(1275, 1300); int64_t T = 1000; int homes = 0, moved = 0, held = 0;
        cfg(&c, TL_MOTION_CONT); c.lead_pct = 0; (void)tl_lost_parse("hold:2,home", &c); tl_init(&t, &c);
        p.has_cap = 1; p.cap_pan = p.pan; p.cap_tilt = p.tilt;
        for (int i = 0; i < 5; i++, T += 200) o = tick(&t, T, box(160, 96, 40, 110), p);
        for (int i = 0; i < 25; i++, T += 200) { o = chain_none(&t, T, p); homes += o.kind == TL_HOME; moved += o.kind == TL_VEL; held += !strcmp(t.lstep, "hold"); }
        CHECK(held >= 8 && moved == 0, "hold:2: the head stays for the time");
        CHECK(homes == 1, "... then home, once");
        CHECK(!strcmp(t.lstep, "done") || !strcmp(t.lstep, "home"), "the chain ends after its last step");
        o = chain_none(&t, T + 200, p); CHECK(o.kind == TL_NONE, "... and then nothing more");
    }
    /* none: nothing at all */
    {
        tl_t t; struct tl_out o; struct tl_ptz p = ptz(1275, 1300); int64_t T = 1000; int acts = 0;
        cfg(&c, TL_MOTION_CONT); c.lead_pct = 0; (void)tl_lost_parse("none", &c); tl_init(&t, &c);
        p.has_cap = 1; p.cap_pan = p.pan; p.cap_tilt = p.tilt;
        for (int i = 0; i < 6; i++, T += 200) { o = tick(&t, T, box(160 + 140, 96, 36, 100), p); if (o.kind == TL_VEL) p.pan += o.pan_dir * 80; }
        for (int i = 0; i < 80; i++, T += 200) { o = chain_none(&t, T, p); acts += o.kind == TL_HOME || o.kind == TL_NUDGE || (o.kind == TL_VEL && !t.vact ? 0 : o.kind == TL_VEL && strcmp(t.dbg.why, "predict")); }
        CHECK(acts == 0, "none: no sweep, no search, no home (only the short prediction may finish the running move)");
    }
    /* ordering: exit first (person left at the right edge), then local around the last position, then home */
    {
        tl_t t; struct tl_out o; struct tl_ptz p = ptz(1275, 1300); int64_t T = 1000; int seq[8], ns = 0, last = 0, locals = 0, panmin = 9999, panmax = 0;
        cfg(&c, TL_MOTION_CONT); c.lead_pct = 0; (void)tl_lost_parse("backsweep,exit,local:300:4:6,home", &c); tl_init(&t, &c);
        p.has_cap = 1; p.cap_pan = p.pan; p.cap_tilt = p.tilt;
        for (int i = 0; i < 6; i++, T += 200) { struct tl_box b = { 285, 40, 319, 150, 0, 0 }; o = tick(&t, T, b, p); apply(&p, &o, &T); }
        for (int i = 0; i < 120; i++, T += 200) {
            int id = !strcmp(t.lstep ? t.lstep : "", "backsweep") ? 1 : !strcmp(t.lstep ? t.lstep : "", "exit") ? 2 : !strcmp(t.lstep ? t.lstep : "", "local") ? 3 : !strcmp(t.lstep ? t.lstep : "", "home") ? 4 : 0;
            o = chain_none(&t, T, p);
            id = !strcmp(t.lstep, "backsweep") ? 1 : !strcmp(t.lstep, "exit") ? 2 : !strcmp(t.lstep, "local") ? 3 : !strcmp(t.lstep, "home") ? 4 : 0;
            if (id && id != last && ns < 8) seq[ns++] = id;
            if (id) last = id;
            if (id == 3) { locals++; if (p.pan < panmin) panmin = p.pan; if (p.pan > panmax) panmax = p.pan; }
            apply(&p, &o, &T);
            if (o.kind == TL_HOME) break;
        }
        printf("   steps seen:"); for (int i = 0; i < ns; i++) printf(" %d", seq[i]); printf("  (1 backsweep 2 exit 3 local 4 home); local pan %d..%d\n", panmin, panmax);
        CHECK(ns >= 2 && seq[ns - 1] == 4 && seq[ns - 2] == 3 && (ns < 3 || seq[ns - 3] == 2), "the steps run in the order of the chain (exit, local, home; backsweep is skipped after an edge exit)");
        CHECK(locals >= 10 && panmax - panmin >= 100 && panmax - panmin <= 300 + 140, "local: an oscillating sweep around the last position, about the configured width");
    }
    /* re-acquisition in the middle of a chain ends it (a route it started is stopped) */
    {
        tl_t t; struct tl_out o; struct tl_ptz p = ptz(1275, 1300); int64_t T = 1000; int started = 0, stopped = 0, busy = 0;
        cfg(&c, TL_MOTION_CONT); c.lead_pct = 0; (void)tl_lost_parse("route:walk:30,home", &c); tl_init(&t, &c);
        p.has_cap = 1; p.cap_pan = p.pan; p.cap_tilt = p.tilt;
        for (int i = 0; i < 5; i++, T += 200) o = tick(&t, T, box(160, 96, 40, 110), p);
        for (int i = 0; i < 6; i++, T += 200) { p.route_busy = busy; o = chain_none(&t, T, p); if (o.route_op == 1) { started++; busy = 1; CHECK(!strcmp(o.route_name, "walk") && o.route_secs == 30, "the route step starts the named route for its time"); } }
        CHECK(started == 1 && !strcmp(t.lstep, "route"), "the route is started once and the step waits for it");
        p.route_busy = 1; o = tick(&t, T, box(160 + 20, 96, 40, 110), p); stopped = o.route_op == 2;
        CHECK(stopped, "a person detected while the route runs: the route is stopped");
        CHECK(!strcmp(t.lstep, "idle") && t.lost_i < 0, "... and the chain is over (the person is followed again)");
        for (int i = 0; i < 5; i++, T += 200) o = tick(&t, T, box(160 + 20, 96, 40, 110), p);
        CHECK(o.route_op == 0, "no further route operation");
    }
    /* a route that ends by itself moves the chain on */
    {
        tl_t t; struct tl_out o; struct tl_ptz p = ptz(1275, 1300); int64_t T = 1000; int homes = 0, busy = 0;
        cfg(&c, TL_MOTION_CONT); c.lead_pct = 0; (void)tl_lost_parse("route:walk:30,home", &c); tl_init(&t, &c);
        p.has_cap = 1; p.cap_pan = p.pan; p.cap_tilt = p.tilt;
        for (int i = 0; i < 5; i++, T += 200) o = tick(&t, T, box(160, 96, 40, 110), p);
        for (int i = 0; i < 30; i++, T += 200) {
            p.route_busy = busy; o = chain_none(&t, T, p);
            if (o.route_op == 1) busy = 1;
            if (i == 12) busy = 0; /* the route finished */
            homes += o.kind == TL_HOME;
        }
        CHECK(homes == 1, "the route ended by itself: the chain goes on to home");
    }
}

/* review round 2: M2 the ambiguous climb stays under the sky-climb cap, M3 the local window fits the soft range / budget, M4 a user's patrol is never touched */
static void review2_cases(void) {
    tl_t t; struct tl_cfg c; struct tl_out o; struct tl_ptz p = ptz(1275, 1300); int64_t T = 1000; int ok, maxtilt = 1300;
    printf("== review round 2: climb cap after an ambiguous head-out, local window, user patrol\n");
    /* M2: the approach of real_headout_cases, then the person stays so close that the top stays cut (the box never shows an uncut top again) */
    cfg(&c, TL_MOTION_CONT); c.lead_pct = 0; tl_init(&t, &c); g_hw = 0.4;
    p.has_cap = 1; p.cap_pan = p.pan; p.cap_tilt = p.tilt;
    for (int i = 0; i < 60; i++, T += 200) {
        double z = 3.0 - 1.5 * 0.2 * i; struct tl_box b;
        if (z < 0.38) z = 0.38;
        b = proj(0.05, z, 1.7, p.pan, p.tilt, &ok);
        if (i >= 12) { b.x1 = 0; b.y1 = 0; b.x2 = 319; b.y2 = 191; ok = 1; } /* the head stays out of the frame whatever the camera does */
        if (ok) o = tick(&t, T, b, p); else o = none(&t, T, p);
        head_loop(&p, &o, 0.2);
        if (p.tilt > maxtilt) maxtilt = p.tilt;
    }
    g_hw = 0.25;
    printf("   tilt max %d\n", maxtilt);
    CHECK(maxtilt <= 1300 + 700 + 60, "after an ambiguous climb a permanently cut top does not confirm the clip: the budgets hold (no climb to the soft max)");
    {   tl_t t2; cfg(&c, TL_MOTION_CONT); tl_init(&t2, &c); t2.up_ms = 5; t2.amb_latch = 1; p = ptz(1275, 1300);
        o = tick(&t2, 100000, box(160, 96, 40, 110), p); o = tick(&t2, 100200, box(160, 96, 40, 110), p);
        CHECK(t2.up_ms == 0 && t2.amb_latch == 0, "a new target clears the head-rising memory and the climb latch"); }
    /* M3: local window inside the soft range; skipped over the travel budget or when it cannot fit */
    for (int k = 0; k < 3; k++) {
        tl_t t3; struct tl_out o3; struct tl_ptz p3 = ptz(1275, 1300); int64_t T3 = 1000, lo = 99999, hi = 0; int locals = 0;
        cfg(&c, TL_MOTION_CONT); c.lead_pct = 0; (void)tl_lost_parse("local:600:4:8,hold:1", &c); tl_init(&t3, &c);
        p3.pan = k == 0 ? 3500 : 1275; p3.has_cap = 1; p3.cap_pan = p3.pan; p3.cap_tilt = p3.tilt;
        if (k == 2) { p3.pan_min = 1200; p3.pan_max = 1400; } /* a range too narrow for the window */
        for (int i = 0; i < 5; i++, T3 += 200) o3 = tick(&t3, T3, box(160, 96, 40, 110), p3);
        t3.target_w = k == 0 ? 3600 : 1275; /* the last seen position: near the soft limit in the first case */
        if (k == 1) p3.over_budget = 1;
        for (int i = 0; i < 60; i++, T3 += 200) {
            o3 = chain_none(&t3, T3, p3);
            if (!strcmp(t3.lstep, "local")) locals++;
            apply(&p3, &o3, &T3);
            if (p3.pan < lo) lo = p3.pan;
            if (p3.pan > hi) hi = p3.pan;
        }
        if (k == 0) CHECK(locals > 5 && hi <= 3650 - 45 + 90, "local near the soft limit: the window is moved inside the soft range minus the margin (the head never presses on the limit)");
        if (k == 1) CHECK(locals == 0, "local is skipped when the travel budget is used up");
        if (k == 2) CHECK(locals == 0, "local is skipped when the window cannot fit the soft range");
    }
    /* M4: a user patrol that already runs when the route step is reached: the step is skipped, nothing is started or stopped */
    {
        tl_t t4; struct tl_out o4; struct tl_ptz p4 = ptz(1275, 1300); int64_t T4 = 1000; int ops = 0, homes = 0;
        cfg(&c, TL_MOTION_CONT); c.lead_pct = 0; (void)tl_lost_parse("route:walk:30,home", &c); tl_init(&t4, &c);
        p4.has_cap = 1; p4.cap_pan = p4.pan; p4.cap_tilt = p4.tilt; p4.route_busy = 1;
        for (int i = 0; i < 5; i++, T4 += 200) o4 = tick(&t4, T4, box(160, 96, 40, 110), p4);
        for (int i = 0; i < 20; i++, T4 += 200) { o4 = chain_none(&t4, T4, p4); ops += o4.route_op != 0; homes += o4.kind == TL_HOME; }
        CHECK(ops == 0 && homes == 1 && !t4.lost_route_on, "a patrol the user started is never started again nor stopped by the chain; the step is skipped");
    }
}

/* ---------------------------------------------------------------- close-range descent (hardware log of 462356f) and the capture-time audit */
#define HN 2000
static double H_PAN[HN], H_TILT[HN];     /* the true pose every 10 ms */
static double hpose(int64_t t, int axis) { int i = (int)(t / 10); if (i < 0) i = 0; if (i >= HN - 1) i = HN - 2; return axis ? H_TILT[i] : H_PAN[i]; }

/* a static person at z = 0.6 m while the camera is moved by a script (pan triangle +-370 steps/s with reversals, tilt +-150 steps/s) and the video latency is `lag_err` ms longer than the model's 50 ms */
static void audit_run(int lag_err, int *max_koy, int *max_kox, int *max_vy, int *down_search) {
    tl_t t; struct tl_cfg c; struct tl_ptz p = ptz(1751, 1405); struct tl_out o; int ok;
    cfg(&c, TL_MOTION_CONT); c.lead_pct = 0; tl_init(&t, &c);
    *max_koy = *max_kox = *max_vy = *down_search = 0;
    for (int i = 0; i < HN; i++) { /* the script, per 10 ms */
        double tt = i * 0.01; double ph = fmod(tt, 1.4), th = fmod(tt, 1.0);
        H_PAN[i] = 1751 + (ph < 0.7 ? 370 * ph : 370 * (1.4 - ph)) - 90;
        H_TILT[i] = 1405 + (th < 0.5 ? 150 * th : 150 * (1.0 - th)) - 40;
    }
    for (int k = 0; k < 40; k++) {
        int64_t T = 1000 + k * 200, cap = T - 100;
        double pan_b = hpose(cap - 50, 0), tilt_b = hpose(cap - 50, 1);            /* what the tracker believes (model latency 50 ms) */
        double pan_e = hpose(cap - 50 - lag_err, 0), tilt_e = hpose(cap - 50 - lag_err, 1); /* where the camera really was at the exposure */
        struct tl_box b = proj(-0.4, 0.6, 1.2, pan_e, tilt_e, &ok);
        p = ptz(hpose(T, 0), hpose(T, 1));
        p.has_cap = 1; p.cap_pan = (int)pan_b; p.cap_tilt = (int)tilt_b;
#ifndef OLD
        p.cap_rate_ok = 1;
        p.cap_rate_pan = (int)((hpose(cap - 25, 0) - hpose(cap - 75, 0)) * 20);
        p.cap_rate_tilt = (int)((hpose(cap - 25, 1) - hpose(cap - 75, 1)) * 20);
#endif
        p.cap_ms = cap;
        if (k < 20 && ok) tl_update(&t, T, &b, 1, FW, FH, &p, &o); else tl_update(&t, T, NULL, 0, FW, FH, &p, &o);
        if (k >= 4) {
            if (abs(t.koy) > *max_koy) *max_koy = abs(t.koy);
            if (abs(t.kox) > *max_kox) *max_kox = abs(t.kox);
            if (abs(t.vy) > *max_vy) *max_vy = abs(t.vy);
        }
        *down_search += o.kind == TL_VEL && o.tilt_dir > 0 && t.dbg.why && !strcmp(t.dbg.why, "exit-search");
    }
}

static void replay06(int home) {
    /* the exact sequence: the person is static at 0.6 m, off-centre, the head row jumps while the camera pans diagonally, then detections stop: no descent beyond the first correction, no DOWN search */
    {
        tl_t t; struct tl_cfg c; struct tl_ptz p = ptz(1751, 1405); struct tl_out o; int ok, dsearch = 0, tilt_at_loss = 0, base21 = 0, early = 0, late_down = 0, tilt_min = 99999, ntick = 0, T0 = 1000;
        double pan = 1751, tilt = 1405;
        memset(&o, 0, sizeof o);
        cfg(&c, TL_MOTION_CONT); c.lead_pct = 0; tl_init(&t, &c);
        for (int i = 0; i < HN; i++) { H_PAN[i] = pan; H_TILT[i] = tilt; }
        for (int k = 0; k < 60; k++) {
            int64_t T = T0 + k * 200, cap = T - 100;
            int visible = k < 20;                                   /* 4 s of detections, then nothing (the detector loses the person at this range) */
            double pan_b, tilt_b, pan_e, tilt_e;
            struct tl_box b;
            /* the head moves for 200 ms with the last command; the pose history is filled per 10 ms */
            int base = (int)(T / 10);
            if (base + 20 < HN) for (int j = 0; j < 20; j++) {
                if (o.kind == TL_VEL && k > 0) {
                    if (o.pan_dir && o.pan_div) pan += o.pan_dir * (1100.0 / o.pan_div) * 0.01;
                    if (o.tilt_dir && o.tilt_div) tilt -= o.tilt_dir * (1100.0 / o.tilt_div) * 0.01;
                }
                H_PAN[base + j] = pan; H_TILT[base + j] = tilt;
            }
            for (int j = base + 20; j < HN && j < base + 40; j++) { H_PAN[j] = pan; H_TILT[j] = tilt; }
            pan_b = hpose(cap - 50, 0); tilt_b = hpose(cap - 50, 1);
            pan_e = hpose(cap - 50 - 60, 0); tilt_e = hpose(cap - 50 - 60, 1);   /* 60 ms more video latency than modelled */
            b = proj(-0.5, 0.6, 1.3, pan_e, tilt_e, &ok);
            p = ptz((int)hpose(T, 0), (int)hpose(T, 1)); p.tilt_home = home; p.has_cap = 1; p.cap_pan = (int)pan_b; p.cap_tilt = (int)tilt_b; p.cap_ms = cap;
#ifndef OLD
            p.cap_rate_ok = 1;
            p.cap_rate_pan = (int)((hpose(cap - 25, 0) - hpose(cap - 75, 0)) * 20);
            p.cap_rate_tilt = (int)((hpose(cap - 25, 1) - hpose(cap - 75, 1)) * 20);
#endif
            if (visible && ok) tl_update(&t, T, &b, 1, FW, FH, &p, &o); else tl_update(&t, T, NULL, 0, FW, FH, &p, &o);
            if (getenv("TL_TRACE")) printf("    R k=%d ok=%d box=%d,%d,%d,%d tilt=%.0f pan=%.0f why=%s rule=%d kind=%d tdir=%d dist=%d hy=%d ey=%d\n", k, ok, b.x1, b.y1, b.x2, b.y2, tilt, pan, t.dbg.why ? t.dbg.why : "?", t.dbg.rule, o.kind, o.tilt_dir, t.dist_mm, t.dbg.hy, t.dbg.ey);
            if (k == 19) tilt_at_loss = (int)tilt;
            if (k == 21) base21 = (int)tilt;
            if (k > 21 && k <= 32 && abs((int)tilt - base21) > early) early = abs((int)tilt - base21); /* the loss is at k = 20: 2.5 s are k <= 32 */
            if (k > 32 && (int)tilt < base21 - 40) late_down = 1;
            if (k >= 20) { if ((int)tilt < tilt_min) tilt_min = (int)tilt; dsearch += o.kind == TL_VEL && o.tilt_dir > 0 && t.dbg.why && !strcmp(t.dbg.why, "exit-search"); }
            ntick++;
        }
        printf("   replay: tilt 1405 -> %d at the loss, lowest after the loss %d, DOWN searches %d (%d ticks)\n", tilt_at_loss, tilt_min, dsearch, ntick);
        CHECK(dsearch == 0, "replay of the log: no DOWN exit search after the loss at 0.6 m");
        CHECK(early <= 8, "... zero tilt travel for the first 2.5 s after the loss (HOLD / WAIT)");
        if (home >= 1300) CHECK(tilt_min >= tilt_at_loss - 30 && tilt_at_loss >= 1405 - 30, "... and the camera does not descend for a head row that is merely low at this range (the vertical dead band is doubled below 0.8 m): no descent at all, none after the loss (home within 150 steps: no WIDEN)");
        else CHECK(late_down, "... home more than 150 steps away: WIDEN then goes toward home, after the 2.5 s");
    }
}

static void close_down_cases(void) {
    printf("== close range (0.6 m): the camera must not descend for nothing (hardware log of 462356f)\n");
    printf("   static person at 0.6 m, camera moved by a script, video latency error: residual vertical / lateral rate of the compensated person (steps/s), rate used for control, DOWN exit searches\n");
    for (int err = 0; err <= 120; err += 40) {
        int koy, kox, vy, ds;
        audit_run(err, &koy, &kox, &vy, &ds);
        printf("   latency error %3d ms: |koy| %3d  |kox| %3d  used |vy| %3d  down searches %d\n", err, koy, kox, vy, ds);
        if (err == 0 || err == 40) CHECK(vy == 0 && ds == 0, "the rate used for control never follows a compensation error; no DOWN exit search");
        if (err >= 80) CHECK(vy == 0 && ds == 0, "... also with a 80-120 ms video latency error");
    }
    replay06(1300);
    replay06(1000);
}

/* the second report: climbing for the head at 0.6 m, then only torso / legs are detected: no sentinel band in the zone, the distance is carried, a bounded UP search, the tilt goes back to home */
#ifdef OLD
static void close_up_cases(void) {}
#else
static void close_up_cases(void) {
    tl_t t; struct tl_cfg c; struct tl_ptz p = ptz(1277, 1300); struct tl_out o; int64_t T = 1000; int dist_before, upsearch = 0, maxtilt = 1277;
    printf("== close range, head out of the frame: climb, loss, search, tilt back\n");
    cfg(&c, TL_MOTION_CONT); c.lead_pct = 0; tl_init(&t, &c);
    p.has_cap = 1; p.cap_pan = p.pan; p.cap_tilt = p.tilt;
    /* a whole person first (a distance estimate), then it comes close: head-clip boxes */
    { int ok; struct tl_box b = proj(0.0, 2.0, 1.7, p.pan, p.tilt, &ok); for (int i = 0; i < 6; i++, T += 200) o = tick(&t, T, b, p); }
    dist_before = t.dist_mm;
    for (int i = 0; i < 8; i++, T += 200) { struct tl_box b = { 90, 0, 150, 191, 0, 0 }; o = tick(&t, T, b, p); apply(&p, &o, &T); if (p.tilt > maxtilt) maxtilt = p.tilt; }
    CHECK(t.dist_mm > 0 && t.dist_mm < 6000 && dist_before > 1500, "the distance of a head-clip box is carried (estimate kept), not the 3 m default");
    { struct tl_box b = { 10, 0, 315, 191, 0, 0 }; o = tick(&t, T, b, p); T += 200; }
    printf("   ambiguous frame: why %s, zone band hy %d, rule %d\n", t.dbg.why, t.dbg.hy, t.dbg.rule);
    CHECK(t.dbg.hy < (1 << 16) && t.dbg.ey == 0, "no 1048576 sentinel in the zone band on an ambiguous frame (the tilt is simply not decided)");
    /* the detections stop (only torso / legs: nothing detected) while the head is up */
    {
        int tilt0 = p.tilt, t_home_reached = -1;
        p.tilt_home = 1300;
        for (int i = 0; i < 160; i++, T += 200) {
            o = chain_none(&t, T, p);
            upsearch += o.kind == TL_VEL && o.tilt_dir < 0 && t.dbg.why && !strcmp(t.dbg.why, "exit-search");
            if (p.tilt > maxtilt) maxtilt = p.tilt;
            apply(&p, &o, &T);
            if (t_home_reached < 0 && abs(p.tilt - 1300) <= 60) t_home_reached = i;
        }
        printf("   tilt at the loss %d, UP exit searches %d, max tilt %d, back within 60 steps of home after %d ticks\n", tilt0, upsearch, maxtilt, t_home_reached);
        CHECK(upsearch >= 1 && upsearch <= 8 && maxtilt <= tilt0 + 460, "after a head-clip episode a loss runs a bounded UP exit search (its own 400-step budget from the loss pose)");
        CHECK(t_home_reached >= 0 && abs(p.tilt - 1300) <= 60, "the tilt goes back to the home tilt when the person stays lost (it did not stay up until the guard return)");
        CHECK(t_home_reached >= 12, "... after the delay (2.5 s for a near target), not at once");
    }
    /* configurable: 0 = off; a small elevation (below 250 steps above home) is left alone; a person back stops it */
    {
        tl_t t2; struct tl_cfg c2; struct tl_ptz p2 = ptz(1277, 1300 + 400); int64_t T2 = 1000; int moved = 0;
        cfg(&c2, TL_MOTION_CONT); c2.tilt_back_s = 0; tl_init(&t2, &c2); p2.tilt_home = 1300; p2.has_cap = 1; p2.cap_pan = p2.pan; p2.cap_tilt = p2.tilt;
        for (int i = 0; i < 6; i++, T2 += 200) o = tick(&t2, T2, box(160, 96, 40, 110), p2);
        for (int i = 0; i < 100; i++, T2 += 200) { o = chain_none(&t2, T2, p2); moved += o.kind == TL_VEL && o.tilt_dir; }
        CHECK(moved == 0, "track_tilt_back_s = 0: the tilt stays where it is");
        cfg(&c2, TL_MOTION_CONT); tl_init(&t2, &c2); p2 = ptz(1277, 1300 + 200); p2.tilt_home = 1300; p2.has_cap = 1; p2.cap_pan = p2.pan; p2.cap_tilt = p2.tilt; T2 = 1000; moved = 0;
        for (int i = 0; i < 6; i++, T2 += 200) o = tick(&t2, T2, box(160, 96, 40, 110), p2);
        for (int i = 0; i < 100; i++, T2 += 200) { o = chain_none(&t2, T2, p2); moved += o.kind == TL_VEL && o.tilt_dir && t2.dbg.why && !strcmp(t2.dbg.why, "tilt-back"); }
        CHECK(moved == 0, "200 steps above home (< 250): left alone");
        cfg(&c2, TL_MOTION_CONT); tl_init(&t2, &c2); p2 = ptz(1277, 1300 + 450); p2.tilt_home = 1300; p2.has_cap = 1; p2.cap_pan = p2.pan; p2.cap_tilt = p2.tilt; T2 = 1000;
        for (int i = 0; i < 6; i++, T2 += 200) o = tick(&t2, T2, box(160, 96, 40, 110), p2);
        { int pans = 0, backs = 0; for (int i = 0; i < 60; i++, T2 += 200) { o = chain_none(&t2, T2, p2); pans += o.kind == TL_VEL && o.pan_dir && t2.dbg.why && !strcmp(t2.dbg.why, "tilt-back"); backs += t2.dbg.why && !strcmp(t2.dbg.why, "tilt-back"); apply(&p2, &o, &T2); if (backs == 3) break; }
          o = tick(&t2, T2, box(160, 96, 40, 110), p2); T2 += 200; o = tick(&t2, T2, box(160, 96, 40, 110), p2);
          CHECK(pans == 0 && !t2.tback_on, "the tilt-back moves only the tilt, and a person seen again ends it"); }
    }
}

#endif


/* ---------------------------------------------------------------- the close-loss machine (HOLD / SEARCH / WAIT / WIDEN) and the low crop (rule 7) */
static struct tl_ptz clp(int tilt) { struct tl_ptz p = ptz(1275, tilt); p.has_cap = 1; p.cap_pan = p.pan; p.cap_tilt = p.tilt; return p; }
/* feeds scripted boxes at a fixed pose (kind 0: the head clipped at the top, E_UP; 2: a steady near box, no evidence) */
static void cl_prime(tl_t *t, struct tl_ptz p, int kind, int64_t *T) {
    for (int i = 0; i < 8; i++, *T += 200) {
        struct tl_box b = { 100, 60, 220, 191, 0, 0 };
        if (kind == 0) { b.x1 = 90; b.y1 = 0; b.x2 = 150; b.y2 = 191; }
        (void)tick(t, *T, b, p);
    }
}
static void close_loss_cases(void) {
    printf("== close-loss machine: low crop, HOLD / SEARCH / WAIT / WIDEN\n");
    { /* (2) the camera is high, the top sinks over 5 frames to a 30 px crop at the bottom, then nothing: the tilt goes down at least 300 */
        tl_t t; struct tl_cfg c; struct tl_ptz p = clp(1500); struct tl_out o; int64_t T = 1000; int tmin = 9999;
        cfg(&c, TL_MOTION_CONT); c.lead_pct = 0; tl_init(&t, &c); p.tilt_home = 1300;
        { static const int sink[5] = { 60, 80, 105, 130, 162 }; for (int i = 0; i < 5; i++, T += 200) { struct tl_box b = { 100, sink[i], 220, 191, 0, 0 }; o = tick(&t, T, b, p); } }
        CHECK(t.dbg.rule == 7 && t.last_rule == 7, "a 30 px crop at the bottom with the top uncut is rule 7, never rule 4");
        for (int i = 0; i < 80; i++, T += 200) { o = chain_none(&t, T, p); head_loop(&p, &o, 0.2); if (p.tilt < tmin) tmin = p.tilt; }
        printf("   low crop: tilt 1500 -> lowest %d (E_DOWN search), final %d\n", tmin, p.tilt);
        CHECK(t.nl_ev == 2 && 1500 - tmin >= 300 && 1500 - tmin <= 460, "camera high, the top sank to a low crop and then nothing: the tilt goes down (>= 300, within the search budget)");
    }
    { /* a low crop is aimed at by its top: the tilt starts at once (no two-frame start, no doubled band) */
        tl_t t; struct tl_cfg c; struct tl_ptz p = clp(1300); struct tl_out o; int first = -1;
        cfg(&c, TL_MOTION_CONT); c.lead_pct = 0; tl_init(&t, &c);
        for (int i = 0; i < 10 && first < 0; i++) { struct tl_box b = { 100, 150, 220, 191, 0, 0 }; o = tick(&t, 1000 + i * 200, b, p); if (o.kind == TL_VEL && o.tilt_dir > 0) first = i; }
        printf("   low crop at row 150: tilt starts at tick %d (rule %d)\n", first, t.dbg.rule);
        CHECK(first >= 0 && first <= 1, "a low crop far below the head row: the tilt down starts on the first confirmed tick");
    }
    { /* (4) clipped boxes, then nothing: an UP search of at most 400, back to the home tilt within 12 s */
        tl_t t; struct tl_cfg c; struct tl_ptz p = clp(1650); struct tl_out o; int64_t T = 1000, t0; int maxt = 0, home_at = -1, ups = 0;
        cfg(&c, TL_MOTION_CONT); c.lead_pct = 0; tl_init(&t, &c); p.tilt_home = 1300;
        cl_prime(&t, p, 0, &T);
        t0 = T;
        for (int i = 0; i < 100; i++, T += 200) {
            o = chain_none(&t, T, p);
            ups += o.kind == TL_VEL && o.tilt_dir < 0 && t.dbg.why && !strcmp(t.dbg.why, "exit-search");
            head_loop(&p, &o, 0.2);
            if (p.tilt > maxt) maxt = p.tilt;
            if (home_at < 0 && maxt > 1650 + 100 && abs(p.tilt - 1300) <= 60) home_at = (int)(T - t0);
        }
        printf("   clipped boxes: UP search ticks %d, max tilt %d (+%d), home within 60 steps after %d ms\n", ups, maxt, maxt - 1650, home_at);
        CHECK(t.nl_ev == 1 && ups >= 4 && maxt - 1650 <= 400, "clipped boxes then nothing: an UP search of at most 400 steps from the loss pose");
        CHECK(home_at >= 0 && home_at <= 12000, "... then the tilt is back at the home tilt within 12 s");
    }
    { /* a detection in each phase stops it within one tick */
        static const int at[] = { 300, 1000, 1800, 2900, 4500 };
        for (unsigned k = 0; k < sizeof at / sizeof at[0]; k++) {
            tl_t t; struct tl_cfg c; struct tl_ptz p = clp(1650); struct tl_out o; int64_t T = 1000, t0; int done = 0, vact_before = 0, ok_stop = 0;
            cfg(&c, TL_MOTION_CONT); c.lead_pct = 0; tl_init(&t, &c); p.tilt_home = 1300;
            cl_prime(&t, p, 0, &T);
            t0 = T;
            for (int i = 0; i < 60 && !done; i++, T += 200) {
                if (T - t0 >= at[k]) {
                    struct tl_box b = { 140, 70, 180, 170, 0, 0 };
                    vact_before = t.vact;
                    o = tick(&t, T, b, p);
                    ok_stop = t.nl_phase == 0 && (vact_before ? o.kind == TL_STOP : o.kind != TL_VEL);
                    done = 1;
                } else { o = chain_none(&t, T, p); head_loop(&p, &o, 0.2); }
            }
            printf("   detection %d ms after the loss: machine %s, command %d (was moving: %d)\n", at[k], t.nl_phase ? "still on" : "ended", o.kind, vact_before);
            CHECK(done && ok_stop, "a detection ends the machine at once and stops its motion (same tick)");
        }
    }
    { /* no evidence: WAIT, no motion for 2.5 s; widen back toward home only if more than 150 steps away */
        for (int far = 0; far < 2; far++) {
            tl_t t; struct tl_cfg c; struct tl_ptz p = clp(far ? 1500 : 1400); struct tl_out o; int64_t T = 1000, t0; int moved_early = 0, moved_late = 0;
            cfg(&c, TL_MOTION_CONT); c.lead_pct = 0; tl_init(&t, &c); p.tilt_home = 1300;
            cl_prime(&t, p, 2, &T);
            t0 = T;
            for (int i = 0; i < 40; i++, T += 200) {
                o = chain_none(&t, T, p);
                if (o.kind == TL_VEL && o.tilt_dir) { if (T - t0 < 2500) moved_early++; else moved_late++; }
                head_loop(&p, &o, 0.2);
            }
            printf("   no evidence, %d steps from home: moved before 2.5 s %d ticks, after %d ticks, final tilt %d\n", far ? 200 : 100, moved_early, moved_late, p.tilt);
            CHECK(moved_early == 0, "no evidence: zero tilt travel for the first 2.5 s");
            CHECK(far ? (moved_late > 0 && abs(p.tilt - 1300) <= 60) : (moved_late == 0 && p.tilt == 1400), far ? "... then WIDEN brings the tilt home when it is more than 150 steps away" : "... and nothing at all when it is within 150 steps of home");
        }
    }
    { /* a far target is not touched: the legacy chain (exit search at 700 ms, tilt-back after tilt_back_s) */
        tl_t t; struct tl_cfg c; struct tl_ptz p = clp(1500); struct tl_out o; int64_t T = 1000; int searches = 0, closes = 0;
        cfg(&c, TL_MOTION_CONT); c.lead_pct = 0; tl_init(&t, &c); p.tilt_home = 1300;
        for (int i = 0; i < 8; i++, T += 200) { struct tl_box b = { 150, 0, 172, 60, 0, 0 }; o = tick(&t, T, b, p); } /* a far person at the top edge */
        for (int i = 0; i < 30; i++, T += 200) { o = chain_none(&t, T, p); closes += t.nl_phase != 0; searches += t.dbg.why && !strcmp(t.dbg.why, "exit-search"); head_loop(&p, &o, 0.2); }
        CHECK(closes == 0, "a far target never enters the close-loss machine");
        (void)searches;
    }
}


/* P1-2: the measurement model (jitter scales with the box, angular process noise capped, every box bounds the distance) */
static void model_cases(void) {
    printf("== measurement / distance model\n");
    { /* a static person at 0.6 m, the top jitters +-10 px: the vertical rate stays small */
        tl_t t; struct tl_cfg c; struct tl_ptz p = clp(1300); int maxk = 0, maxvy = 0; unsigned seed = 7;
        cfg(&c, TL_MOTION_CONT); c.lead_pct = 0; tl_init(&t, &c);
        for (int i = 0; i < 80; i++) {
            struct tl_box b = { 50, 40, 270, 191, 0, 0 };
            seed = seed * 1103515245u + 12345u; b.y1 += ((seed >> 16) & 0x7fff) % 21 - 10;
            (void)tick(&t, 1000 + i * 200, b, p);
            if (i >= 10 && abs(t.koy) > maxk) maxk = abs(t.koy);
            if (abs(t.vy) > maxvy) maxvy = abs(t.vy);
        }
        printf("   static box, top +-10 px (uniform) at %d mm: max |koy| %d steps/s, rate used for control %d\n", t.dist_mm, maxk, maxvy);
        CHECK(t.dist_mm < 900 && maxk < 400 && maxvy <= 130, "a static box at 0.6 m with +-10 px of top jitter: distance known (no 3 m default), Kalman rate bounded (< 400, was 303 at the 3 m default; the < 60 target is NOT reached with the 2000 steps/s^2 cap, see the commit message)");
    }
    { /* a box cut at both ends bounds the distance by the person's height: ~1.6 m, never the 3 m default */
        tl_t t; struct tl_cfg c; struct tl_ptz p = clp(1300);
        cfg(&c, TL_MOTION_CONT); tl_init(&t, &c);
        { struct tl_box b = { 100, 0, 220, 191, 0, 0 }; (void)tick(&t, 1000, b, p); (void)tick(&t, 1200, b, p); }
        printf("   cut-both box on a new target: distance %d mm\n", t.dist_mm);
        CHECK(t.dist_mm > 0 && t.dist_mm <= 1600, "a box cut at top and bottom on a new target: distance <= 1600 mm");
    }
}


/* P1-3: the jump threshold (0.6 head heights, 10..30 % of the height), the frozen tilt after a near downward jump, no tilt start while the camera pans fast */
static void jump_cases(void) {
    printf("== jump threshold, torso box, sit-down, pan rate\n");
    for (int ticks = 1; ticks <= 3; ticks++) {
        tl_t t; struct tl_cfg c; struct tl_ptz p = clp(1580); struct tl_out o; int64_t T = 1000, t0; int down = 0, first = -1;
        cfg(&c, TL_MOTION_CONT); c.lead_pct = 0; tl_init(&t, &c);
        for (int i = 0; i < 8; i++, T += 200) { struct tl_box b = { 96, 27, 280, 191, 0, 0 }; (void)tick(&t, T, b, p); }
        t0 = T;
        for (int i = 0; i < 12; i++, T += 200) {
            struct tl_box b = { 96, i < ticks ? 99 : 27, 280, 191, 0, 0 };
            if (ticks == 3) b.y1 = 99; /* a sit-down: the top stays at the new level */
            o = tick(&t, T, b, p);
            if (o.kind == TL_VEL && o.tilt_dir > 0) { down++; if (first < 0) first = (int)(T - t0); }
        }
        if (ticks == 3) { printf("   sit-down (top 27 -> 99 and staying): tilt down starts after %d ms\n", first); CHECK(first >= 0 && first <= 600, "a three-tick sit-down starts the tilt within 600 ms"); }
        else { printf("   %d-tick torso box (top 27 -> 99 -> 27): %d tilt-down commands\n", ticks, down); CHECK(down == 0, ticks == 1 ? "a one-tick torso box: zero tilt" : "a two-tick torso box that returns to the old top: zero tilt"); }
    }
    { /* the steps path holds an approach like the continuous one: the rule-4 tilt decision only, the pan goes on */
        tl_t t; struct tl_cfg c; struct tl_ptz p = ptz(1275, 1300); struct tl_out o; int pans = 0, tilts = 0, hold = 0, w = 20; (void)hold;
        cfg(&c, TL_MOTION_STEPS); c.lead_pct = 0; tl_init(&t, &c);
        for (int i = 0; i < 6; i++, w = w * 3 / 2) {
            o = tick(&t, 1000 + i * 600, box(250, 55, w, 100), p);
            if (getenv("TL_TRACE")) printf("    S i=%d w=%d kind=%d dp=%d dt=%d why=%s rule=%d\n", i, w, o.kind, o.dpan, o.dtilt, t.dbg.why, t.dbg.rule);
            if (o.kind == TL_NUDGE) p.motion_end_ms = 1000 + i * 600 + 100;
            pans += o.kind == TL_NUDGE && o.dpan; tilts += o.kind == TL_NUDGE && o.dtilt; hold += 0;
        }
        printf("   steps path, approaching body (rule 4) off-centre: pan nudges %d, tilt nudges %d\n", pans, tilts);
        CHECK(pans >= 1 && tilts == 0, "steps path: an approaching person (box area growing) holds the rule-4 tilt decision but not the pan");
    }
    { /* near: no tilt start while the camera pans fast, except a head clip (rule 1) */
        for (int clip = 0; clip < 3; clip++) {
            tl_t t; struct tl_cfg c; struct tl_ptz p = clp(1300); struct tl_out o; int started = 0;
            cfg(&c, TL_MOTION_CONT); c.lead_pct = 0; tl_init(&t, &c);
            p.cap_rate_ok = 1; p.cap_rate_pan = clip == 2 ? 0 : 300;
            for (int i = 0; i < 6; i++) {
                struct tl_box b = clip == 1 ? (struct tl_box){ 40, 0, 280, 191, 0, 0 } : (struct tl_box){ 40, 90, 200, 191, 0, 0 };
                o = tick(&t, 1000 + i * 200, b, p);
                started += o.kind == TL_VEL && o.tilt_dir;
            }
            printf("   near box, camera pan rate %d steps/s, %s: tilt starts %d\n", p.cap_rate_pan, clip == 1 ? "head clip" : "rule 2", started);
            CHECK(clip ? started > 0 : started == 0, clip == 1 ? "... a head clip (rule 1) still starts the tilt" : clip == 2 ? "... the same rule-2 box with a still camera starts it" : "near (rule 2): no tilt start while the camera pans faster than 200 steps/s");
        }
    }
}


/* L1: tilt-back and WIDEN test the soft-range room in the direction of travel, also with invert_tilt */
static void room_cases(void) {
    printf("== tilt-back / WIDEN near a tilt limit, both inversions\n");
    for (int inv = 0; inv < 2; inv++)
        for (int side = 0; side < 2; side++) { /* side 0: the camera sits 20 steps from the low limit, home above; 1: 20 from the high limit, home below */
            for (int near = 0; near < 2; near++) {
                tl_t t; struct tl_cfg c; struct tl_ptz p = clp(side ? 2080 : 320); struct tl_out o; int64_t T = 1000; int moved = 0, start = side ? 2080 : 320;
                cfg(&c, TL_MOTION_CONT); c.lead_pct = 0; c.invert_tilt = inv; tl_init(&t, &c);
                p.tilt_home = side ? 1500 : 900;
                for (int i = 0; i < 8; i++, T += 200) { struct tl_box b = near ? (struct tl_box){ 100, 60, 220, 191, 0, 0 } : (struct tl_box){ 150, 90, 170, 130, 0, 0 }; o = tick(&t, T, b, p); }
                for (int i = 0; i < 60; i++, T += 200) { o = chain_none(&t, T, p); if (o.kind == TL_VEL && o.tilt_dir) moved++; head_loop(&p, &o, 0.2); }
                printf("   invert_tilt %d, %s limit, %s target: tilt %d -> %d\n", inv, side ? "high" : "low", near ? "near" : "far", start, p.tilt);
                /* the home is on the free side: the camera must leave the limit toward it (the motor sign of the direction depends on invert_tilt) */
                CHECK(moved > 0 && abs(p.tilt - p.tilt_home) < abs(start - p.tilt_home), "the tilt goes back toward home from 20 steps of a tilt limit");
            }
        }
}


/* L3: the same approach at 1.3 m/s: the camera climbs earlier and keeps the head in view, so the head-out latch is not asserted (it is at 1.5 m/s), the functional checks are */
static void approach13_cases(void) {
    printf("== approach at 1.3 m/s (functional checks only)\n");
    for (int forced = 0; forced < 2; forced++) {
        tl_t t; struct tl_cfg c; struct tl_out o; struct tl_ptz p = ptz(1275, 1300); int64_t T = 1000; int ok, maxtilt = 1300, moves = 0;
        cfg(&c, TL_MOTION_CONT); c.lead_pct = 0; tl_init(&t, &c); g_hw = 0.4;
        p.has_cap = 1; p.cap_pan = p.pan; p.cap_tilt = p.tilt;
        for (int i = 0; i < 60; i++, T += 200) {
            double z = 3.0 - 1.3 * 0.2 * i; struct tl_box b;
            if (z < 0.38) z = 0.38;
            b = proj(0.05, z, 1.7, p.pan, p.tilt, &ok);
            if (forced && i >= 12) { b.x1 = 0; b.y1 = 0; b.x2 = 319; b.y2 = 191; ok = 1; } /* the head stays out of the frame whatever the camera does */
            if (ok) o = tick(&t, T, b, p); else o = none(&t, T, p);
            moves += o.kind == TL_VEL && o.tilt_dir < 0;
            head_loop(&p, &o, 0.2);
            if (p.tilt > maxtilt) maxtilt = p.tilt;
        }
        g_hw = 0.25;
        printf("   %s: tilt max %d, up commands %d\n", forced ? "head permanently out" : "free", maxtilt, moves);
        CHECK(moves >= 1 && maxtilt >= 1300 + 250, "the camera follows the approaching head up");
        CHECK(maxtilt <= 1300 + 700 + 60, "... and the near-frame budget (+700) holds");
    }
}


/* L4: a box held as an outlier ends the close-loss machine and the next no-box tick starts a fresh HOLD */
static void held_box_cases(void) {
    tl_t t; struct tl_cfg c; struct tl_ptz p = clp(1650); struct tl_out o; int64_t T = 1000, t0; int search = 0, hold = 0; (void)o;
    printf("== a held box ends the close-loss machine\n");
    cfg(&c, TL_MOTION_CONT); c.lead_pct = 0; tl_init(&t, &c); p.tilt_home = 1300;
    cl_prime(&t, p, 0, &T);
    t0 = T;
    for (int i = 0; i < 2; i++, T += 200) o = chain_none(&t, T, p); /* HOLD at +0, +200 */
    { struct tl_box b = { 90, 70, 150, 191, 0, 0 }; o = tick(&t, T, b, p); } /* +400: a jumping box, held as an outlier */
    CHECK(t.nl_phase == 0 && t.dbg.why && !strcmp(t.dbg.why, "outlier"), "the jumping box is held as an outlier and ends the machine");
    T += 200;
    for (int i = 0; i < 2; i++, T += 200) { o = chain_none(&t, T, p); hold += t.dbg.why && !strcmp(t.dbg.why, "close-hold"); search += t.dbg.why && !strcmp(t.dbg.why, "exit-search"); }
    printf("   after the held box (loss started %d ms before): hold ticks %d, search ticks %d\n", (int)(T - t0), hold, search);
    CHECK(hold == 2 && search == 0, "the next no-box ticks are a fresh HOLD, not a skipped one (no early SEARCH)");
}

/* the down trend (P1-3 rule): top sank >= 12 px + 3 sigma over >= 3 fresh frames, sigma from the jitter and the camera rate x the residual lag; no "camera still" condition */
static int down_exit_after(const int *y1s, int n, int rate) {
    tl_t t; struct tl_cfg c; struct tl_ptz p = ptz(1275, 1300); int64_t T = 1000; int i;
    cfg(&c, TL_MOTION_CONT); c.lead_pct = 0; tl_init(&t, &c);
    p.has_cap = 1; p.cap_pan = p.pan; p.cap_tilt = p.tilt; p.cap_rate_ok = 1; p.cap_rate_tilt = rate;
    for (i = 0; i < 3; i++, T += 200) (void)tick(&t, T, box(160, 100, 80, 160), ptz(1275, 1300)); /* a settled start, rate 0 */
    for (i = 0; i < n; i++, T += 200) { struct tl_box b = { 120, y1s[i], 200, 191, 0, 0 }; (void)tick(&t, T, b, p); }
    (void)chain_none(&t, T, p);
    return t.dbg.exit == TL_EXIT_DOWN;
}
static void down_trend_cases(void) {
    const int sink[] = { 50, 80, 110 }, jump[] = { 50, 50, 90 }, jump2[] = { 50, 50, 50, 50, 90 };
    printf("== down trend: sigma-aware, valid on moving-camera frames\n");
    CHECK(down_exit_after(sink, 3, 300), "camera moving at 300 steps/s, the top sinks 60 px over 3 frames: DOWN exit accepted");
    CHECK(!down_exit_after(jump2, 5, 300), "camera moving at 300 steps/s, a single-tick 40 px jump: no DOWN exit (inside 12 px + 3 sigma)");
    CHECK(!down_exit_after(jump, 3, 300), "... also as the third frame of a short run");
    CHECK(down_exit_after(sink, 3, 0), "camera still: the same sink is accepted");
}

/* loss policy: the UP exit search has its own budget, the tilt-back goes both ways and starts earlier for a near target, the moving-camera fallback needs a velocity command on */
static void loss_policy_cases(void) {
    tl_t t; struct tl_cfg c; struct tl_out o; struct tl_ptz p = ptz(1275, 1300 + 450); int64_t T = 1000; int first_back = -1, up = 0;
    printf("== loss policy: exit-up budget, tilt-back from below, rate fallback\n");
    cfg(&c, TL_MOTION_CONT); c.lead_pct = 0; tl_init(&t, &c);
    p.has_cap = 1; p.cap_pan = p.pan; p.cap_tilt = p.tilt; p.cont = 1;
    o = tick(&t, T, box(160, 96, 40, 110), p);
    CHECK(t.cam_ry == 0, "no measured camera rate and no velocity command on: the camera is not taken as moving");
    { /* a near, head-clipped person at the top, then lost, with the episode already 450 steps up: the search still has its own 400 steps */
        struct tl_ptz q = ptz(1275, 1300); int maxt = 0, tl0;
        cfg(&c, TL_MOTION_CONT); c.lead_pct = 0; tl_init(&t, &c); q.has_cap = 1; q.cap_pan = q.pan; q.cap_tilt = q.tilt; T = 1000;
        for (int i = 0; i < 6; i++, T += 200) o = tick(&t, T, box(160, 96, 40, 110), q);
        q.tilt = 1750; q.cap_tilt = q.tilt; tl0 = q.tilt;
        for (int i = 0; i < 6; i++, T += 200) { struct tl_box b = { 90, 0, 150, 120, 0, 0 }; o = tick(&t, T, b, q); }
        for (int i = 0; i < 40; i++, T += 200) { o = chain_none(&t, T, q); if (t.dbg.why && !strcmp(t.dbg.why, "exit-search") && o.tilt_dir < 0) up++; head_loop(&q, &o, 0.2); if (q.tilt > maxt) maxt = q.tilt; }
        printf("   exit-up from %d: max tilt %d, up ticks %d\n", tl0, maxt, up);
        CHECK(up >= 1 && maxt > 1300 + 400 && maxt <= tl0 + 400 + 120, "the UP exit search climbs by its own budget (<= 400 steps from the loss pose), not the 400 from the episode start");
    }
    { /* below home: after a near loss the tilt goes back up to home after 2.5 s */
        struct tl_ptz q = ptz(1275, 1300 - 400); T = 1000; q.has_cap = 1; q.cap_pan = q.pan; q.cap_tilt = q.tilt;
        cfg(&c, TL_MOTION_CONT); c.lead_pct = 0; tl_init(&t, &c);
        for (int i = 0; i < 6; i++, T += 200) { struct tl_box b = { 100, 60, 220, 191, 0, 0 }; o = tick(&t, T, b, q); }
        for (int i = 0; i < 80; i++, T += 200) {
            o = chain_none(&t, T, q);
            if (first_back < 0 && t.dbg.why && !strcmp(t.dbg.why, "tilt-back") && o.kind == TL_VEL && o.tilt_dir < 0) first_back = i;
            head_loop(&q, &o, 0.2);
        }
        printf("   below home by 400: first tilt-back tick %d, final tilt %d\n", first_back, q.tilt);
        CHECK(first_back >= 8 && first_back <= 14, "a near target lost with the tilt below home: the tilt-back (upward) starts after 2.5 s");
        CHECK(abs(q.tilt - 1300) <= 60, "... and reaches home");
    }
}

/* ---------------------------------------------------------------- approach: a person walks at a desk camera (0.75 m high) */
static double g_camh = 0.75;
static struct tl_box proj_c(double x, double z, double h, double pan, double tilt, int *ok, double *top_unclamped) {
    double yaw = -(pan - 1275) / KRAD, psi = (tilt - 1300) / KRAD, th = atan2(x, z), rho = hypot(x, z), dl = th - yaw, d = rho * cos(dl);
    double xc = FW / 2.0 + FHZ * tan(dl), hw = FHZ * g_hw / d;
    double y1 = FH / 2.0 - FV * tan(fmax(-1.4, fmin(1.4, atan((h - g_camh) / rho) - psi))), y2 = FH / 2.0 - FV * tan(fmax(-1.4, fmin(1.4, atan(-g_camh / rho) - psi)));
    struct tl_box b;
    *top_unclamped = y1;
    *ok = fabs(dl) < 1.2 && d > 0.3;
    b.x1 = (int)fmax(0, xc - hw); b.x2 = (int)fmin(FW - 1, xc + hw); b.y1 = (int)fmax(0, y1); b.y2 = (int)fmin(FH - 1, y2);
    if (b.x2 <= b.x1 || b.y2 <= b.y1) *ok = 0;
    return b;
}
static double aim_tilt(double z, double h) { /* the tilt that puts the head top at the head row (15 % of the height) */
    double ang = atan((h - g_camh) / z), ty = FH * 0.15;
    return 1300 + KRAD * (ang - atan((FH / 2.0 - ty) / FV));
}

static void approach_cases(void) {
    tl_t t; struct tl_cfg c; struct tl_out o; struct tl_ptz p = ptz(1275, 1300); int64_t T = 1000; int ok, nclip = 0, worst_hi = 0;
    double top, worst_lo = 1e9, hi = -1e9;
    printf("== approach 3 m -> 0.6 m at 1 m/s, camera on a desk (0.75 m), person 1.7 m\n");
    cfg(&c, TL_MOTION_CONT); c.lead_pct = 0; tl_init(&t, &c);
    p.has_cap = 1; p.cap_pan = p.pan; p.cap_tilt = p.tilt;
    for (int i = 0; i < 60; i++, T += 200) {
        double z = 3.0 - 1.0 * 0.2 * i; struct tl_box b;
        if (z < 0.6) z = 0.6;
        b = proj_c(0.0, z, 1.7, p.pan, p.tilt, &ok, &top);
        if (i >= 3) { if (top < worst_lo) worst_lo = top; if (top > hi) hi = top; nclip += top < 0; }
        if (ok) o = tick(&t, T, b, p); else o = none(&t, T, p);
        if (getenv("TL_TRACE")) printf("    P i=%d z=%.2f top=%.0f box=%d,%d,%d,%d tilt=%d why=%s rule=%d kind=%d tdir=%d clip=%d\n", i, z, top, b.x1, b.y1, b.x2, b.y2, p.tilt, t.dbg.why ? t.dbg.why : "?", t.dbg.rule, o.kind, o.tilt_dir, t.clip_n);
        head_loop(&p, &o, 0.2);
    }
    (void)worst_hi;
    printf("   head top row over the walk: min %.0f (%.0f %%), max %.0f (%.0f %%); final tilt %d, geometric aim %.0f\n", worst_lo, worst_lo * 100 / FH, hi, hi * 100 / FH, p.tilt, aim_tilt(0.6, 1.7));
    CHECK(worst_lo >= 0 && hi <= 0.35 * FH, "the head top stays inside 0-35 % of the frame all along the approach");
    CHECK(abs(p.tilt - (int)aim_tilt(0.6, 1.7)) <= 60, "the final tilt is within 60 steps of the geometric aim");
    /* a far person at the top edge: the up search stops at +400 */
    {
        struct tl_ptz q = ptz(1275, 1300); int maxt = 1300;
        cfg(&c, TL_MOTION_CONT); c.lead_pct = 0; tl_init(&t, &c); q.has_cap = 1; q.cap_pan = q.pan; q.cap_tilt = q.tilt; T = 1000;
        for (int i = 0; i < 80; i++, T += 200) { struct tl_box b = { 150, 0, 172, 60, 0, 0 }; o = tick(&t, T, b, q); head_loop(&q, &o, 0.2); if (q.tilt > maxt) maxt = q.tilt; }
        printf("   far person (22 x 60 px) stuck at the top edge: max tilt %d\n", maxt);
        CHECK(maxt >= 1300 + 280 && maxt <= 1300 + 400 + 60, "a far person at the top edge: the climb stops at about +400 (the 10 % jump floor holds a tick or two)");
    }
    /* a rule-5 box (cut top and bottom, 95 % wide) after a rising head: held 10 s climbs at most 300 from where the latch started */
    {
        struct tl_ptz q = ptz(1275, 1300); int maxt = 0, t0 = 0, got = 0;
        cfg(&c, TL_MOTION_CONT); c.lead_pct = 0; tl_init(&t, &c); q.has_cap = 1; q.cap_pan = q.pan; q.cap_tilt = q.tilt; T = 1000;
        for (int i = 0; i < 6; i++, T += 200) { struct tl_box b = { 90, 40 - i * 8, 230, 191, 0, 0 }; o = tick(&t, T, b, q); }  /* the top climbs 8 px per tick: a real rise */
        for (int i = 0; i < 60; i++, T += 200) {
            struct tl_box b = { 10, 0, 315, 191, 0, 0 };
            o = tick(&t, T, b, q);
            if (getenv("TL_TRACE")) printf("    R5 tilt=%d why=%s rule=%d kind=%d tdir=%d tdiv=%d upl=%d\n", q.tilt, t.dbg.why ? t.dbg.why : "?", t.dbg.rule, o.kind, o.tilt_dir, o.tilt_div, t.amb_up);
            if (t.amb_up && !got) { got = 1; t0 = q.tilt; }
            head_loop(&q, &o, 0.2);
            if (got && q.tilt > maxt) maxt = q.tilt;
        }
        printf("   rule-5 box held 10 s: latch %d, tilt at the latch %d, max %d (climb %d)\n", got, t0, maxt, maxt - t0);
        CHECK(got && maxt - t0 <= 300, "a rule-5 box with the up-trend latch held 10 s climbs at most 300 steps");
    }
    (void)nclip;
}

static void cpu_case(void) {
    tl_t t; struct tl_cfg c; struct tl_out o; struct tl_ptz p = ptz(1275, 1300); clock_t c0; int n = 200000; int ok; double us;
    printf("== CPU cost of one tick (host, -O0 sanitizers off)\n");
    cfg(&c, TL_MOTION_CONT); tl_init(&t, &c);
    c0 = clock();
    for (int i = 0; i < n; i++) { struct tl_box b = proj(1.0 * sin(i * 0.01), 3.0, 1.7, 1275 - 100 * sin(i * 0.01), 1300, &ok); o = tick(&t, 1000 + (int64_t)i * 200, b, p); }
    us = (double)(clock() - c0) / CLOCKS_PER_SEC * 1e6 / n;
    printf("   %.2f us per tick including the box projection (a Cortex-A5-class armv5 soft-float core is ~30x slower: a few tens of us at 5 Hz)\n", us);
    CHECK(us < 50.0, "one tick of the 3D-state tracker costs well under 50 us on the host");
    (void)o;
}

/* P0-2: head boxes (class 1) ride along in the feed but the policy ignores them: the decisions with extra head boxes are identical to those without. */
static void heads_cases(void) {
    printf("== head boxes (class 1) change no decision\n");
    for (int mode = 0; mode < 2; mode++) {
        tl_t a, b; struct tl_cfg c; struct tl_ptz pa = ptz(1275, 1300), pb = ptz(1275, 1300); int diff = 0, moved = 0, ok;
        cfg(&c, mode); tl_init(&a, &c); tl_init(&b, &c);
        for (int k = 0; k < 120; k++) {
            int64_t T = 1000 + k * 200;
            double x = 1.6 * sin(k * 0.12), z = 2.5 + 1.0 * sin(k * 0.05);
            struct tl_box pers = proj(x, z, 1.7, pa.pan, pa.tilt, &ok), in[TL_MAX_ALL]; struct tl_out oa, ob; int n = 0; memset(&oa, 0, sizeof oa); memset(&ob, 0, sizeof ob);
            if (k % 17 > 12) ok = 0;                                   /* some frames without a person */
            if (k % 7 == 3) { struct tl_box h = { 10, 5, 60, 55, TL_CLS_HEAD, 640 }; in[n++] = h; }  /* a head BEFORE the person, in a corner */
            if (ok) { pers.cls = TL_CLS_PERSON; pers.score = 810; in[n++] = pers; }
            { struct tl_box h = { pers.x1 + 4, pers.y1, pers.x1 + 30, pers.y1 + 26, TL_CLS_HEAD, 555 }; if (ok) in[n++] = h; }
            for (int j = 0; j < 3; j++) { struct tl_box h = { 100 + 20 * j, 40, 130 + 20 * j, 70, TL_CLS_HEAD, 300 + j }; in[n++] = h; }
            pa.cap_ms = pb.cap_ms = T - 100; pa.has_cap = pb.has_cap = 1; pa.cap_pan = pb.cap_pan = pa.pan; pa.cap_tilt = pb.cap_tilt = pa.tilt;
            if (ok) tl_update(&a, T, &pers, 1, FW, FH, &pa, &oa); else tl_update(&a, T, NULL, 0, FW, FH, &pa, &oa);
            tl_update(&b, T, in, n, FW, FH, &pb, &ob);
            if (oa.kind != ob.kind || oa.dpan != ob.dpan || oa.dtilt != ob.dtilt || oa.pan_dir != ob.pan_dir || oa.pan_div != ob.pan_div || oa.tilt_dir != ob.tilt_dir || oa.tilt_div != ob.tilt_div || oa.speed != ob.speed ||
                strcmp(a.dbg.why, b.dbg.why) || a.has_target != b.has_target || a.dist_mm != b.dist_mm) diff++;
            moved += oa.kind != TL_NONE;
            if (oa.kind == TL_NUDGE) { pa.pan += oa.dpan; pa.tilt -= oa.dtilt; pa.motion_end_ms = T + 100; pb = pa; }
            if (oa.kind == TL_VEL) { pa.pan += oa.pan_dir * 80; pa.tilt -= oa.tilt_dir * 40; pb = pa; }
        }
        CHECK(moved > 10 && diff == 0, mode ? "continuous: a walker with head boxes of both classes around: identical decisions (heads ignored)" : "steps: a walker with head boxes around: identical decisions (heads ignored)");
        /* only heads in a frame = nobody */
        { struct tl_box h[2] = { { 100, 20, 140, 60, TL_CLS_HEAD, 700 }, { 200, 20, 240, 60, TL_CLS_HEAD, 650 } }; struct tl_out o1; tl_t u; tl_init(&u, &c);
          tl_update(&u, 5000, h, 2, FW, FH, &pa, &o1); CHECK(!u.has_target && o1.kind == TL_NONE, "heads only: no target, no motion"); }
    }
}

/* P0-5: the video latency of the pose-at-capture lookup may be negative */
static void lag_cases(void) {
    struct tl_cfg c;
    printf("== lag_ms range\n");
    cfg(&c, TL_MOTION_CONT);
    c.lag_ms = -150; CHECK(tl_cfg_validate(&c) == 0, "lag_ms -150 is valid");
    c.lag_ms = 300; CHECK(tl_cfg_validate(&c) == 0, "lag_ms 300 is valid");
    c.lag_ms = -151; CHECK(tl_cfg_validate(&c) != 0, "lag_ms -151 is refused");
    c.lag_ms = 301; CHECK(tl_cfg_validate(&c) != 0, "lag_ms 301 is refused");
    tl_cfg_defaults(&c); CHECK(c.lag_ms == 50 && c.gain_pan == 1000 && c.gain_tilt == 700, "defaults: lag 50 ms, gains 1000 / 700");
}

int main(void) {
    zone_cases();
    lag_cases();
    heads_cases();
    cont_cases();
    vel_cases();
    kf_cases();
    replay_cases();
    sweep_cases();
    review_cases();
    real_headout_cases();
    close_down_cases();
    close_up_cases();
    close_loss_cases();
    model_cases();
    jump_cases();
    room_cases();
    approach13_cases();
    held_box_cases();
    down_trend_cases();
    approach_cases();
    loss_policy_cases();
    lost_chain_cases();
    review2_cases();
    cpu_case();
    printf("rot2k track cont unit: %d failed\n", fails);
    return fails ? 1 : 0;
}
