/* Host unit test of the targeting policy and the continuous mode of track_logic (pure policy): comfort zone and hysteresis, body / head reference, outlier boxes, the robust
 * velocity (feints), the 3D Kalman state (distance from the box, near / far consistency, approach changes distance not angles), the continuous velocity command (start, refresh,
 * stop, direction change, soft range, overshoot guard), the sweep back after an overshoot, and the replays of two hardware logs (a partial torso box: the camera must not go
 * down; a full-frame box and a premature search: the camera must not go up, no search right after the camera's own move). Also the CPU cost of one tick.
 * Build: tests/rot2k_overlay_test.sh. Prints "rot2k track cont unit: N failed". */
#include "../src/features/track/track_logic.h"
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
    for (i = 0; i < 2; i++) o = tick(&t, 1800 + i * 200, box(160 - 120, 96, 40, 110), p);
    CHECK(!(o.kind == TL_VEL && o.pan_dir > 0), "person jumps to the left: the reversal is refused for 700 ms after the last command the other way (M3)");
    for (i = 2; i < 6; i++) o = tick(&t, 1800 + i * 200, box(160 - 120, 96, 40, 110), p);
    CHECK(o.kind == TL_VEL && o.pan_dir > 0, "... and goes through after that (no stop command needed)");
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
        cfg(&c, TL_MOTION_CONT); c.lead_pct = 0; c.close_scan = 0; tl_init(&t, &c);
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
    for (int i = 0; i < 8; i++, T += 200) { struct tl_box b = { 70, 0, 170, 160, 0, 0 }; o = tick(&t, T, b, p); apply(&p, &o, &T); if (p.tilt > maxtilt) maxtilt = p.tilt; }
    CHECK(t.dist_mm > 0 && t.dist_mm < 6000 && dist_before > 1500, "the distance of a head-clip box is carried (estimate kept), not the 3 m default");
    { struct tl_box b = { 10, 0, 315, 191, 0, 0 }; o = tick(&t, T, b, p); T += 200; }
    printf("   ambiguous frame: why %s, zone band hy %d, rule %d\n", t.dbg.why, t.dbg.hy, t.dbg.rule);
    CHECK(t.dbg.hy < (1 << 16) && t.dbg.ey == 0, "no 1048576 sentinel in the zone band on an ambiguous frame (the tilt is simply not decided)");
    /* the detections stop (only torso / legs: nothing detected) while the head is up */
    {
        int tilt0, t_home_reached = -1;
        p.tilt_home = 1300;
        for (int i = 0; i < 14; i++, T += 200) { struct tl_box b = { 70, 0, 170, 140, 0, 0 }; o = tick(&t, T, b, p); } /* the last boxes are head clips 73 % tall: not frame-filling, the close sighting is over after 2 s (a frame-filling loss gets no directed search: M1) */
        tilt0 = p.tilt;
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
        if (kind == 0) { b.x1 = 70; b.y1 = 0; b.x2 = 170; b.y2 = 160; } /* a head clip, 84 % tall: not frame-filling */
        if (kind == 3) { b.x1 = 90; b.y1 = 0; b.x2 = 150; b.y2 = 191; } /* frame-filling (>= 90 % tall, top clipped) */
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
        cfg(&c, TL_MOTION_CONT); c.lead_pct = 0; c.close_scan = 0; tl_init(&t, &c); p.tilt_home = 1300;
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
            cfg(&c, TL_MOTION_CONT); c.lead_pct = 0; c.close_scan = 0; tl_init(&t, &c); p.tilt_home = 1300;
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


/* the hardware loop of 2026-11-18: a person very close, frame-filling boxes that come and go every ~2.5 s. Each brief reacquire used to end the close-loss machine and the next loss
 * started a fresh one (HOLD, WAIT, a NEW VSCAN from the new tilt): the head went up / down in a loop. One VSCAN per loss episode now. */
typedef struct { int scans, scan_ticks, maxt, mint, tilt_end, ups, downs; } flap_res_t;
static flap_res_t flap_run(int cooldown_s, int on_ms, int off_ms, int total_ms) {
    tl_t t; struct tl_cfg c; struct tl_ptz p = clp(1500); struct tl_out o; int64_t T = 1000, t0; flap_res_t r; int in_scan = 0;
    static const struct tl_box fb[2] = { { 103, 9, 317, 188, 0, 0 }, { 1, 0, 249, 191, 0, 0 } };
    memset(&r, 0, sizeof r); r.mint = r.maxt = 1500;
    cfg(&c, TL_MOTION_CONT); c.lead_pct = 0; c.return_home = 1; (void)tl_lost_parse("hold:3,home", &c);
    if (cooldown_s >= 0) c.close_scan_cooldown_s = cooldown_s;
    tl_init(&t, &c); p.tilt_home = 1300;
    t0 = T;
    while (T - t0 < total_ms) {
        int64_t ph = (T - t0) % (on_ms + off_ms);
        const char *w;
        if (ph < on_ms) o = tick(&t, T, fb[(int)((T - t0) / (on_ms + off_ms)) & 1], p);
        else o = chain_none(&t, T, p);
        w = t.dbg.why ? t.dbg.why : "";
        if (!strncmp(w, "close-scan", 10)) { if (!in_scan) r.scans++; in_scan = 1; r.scan_ticks++; r.ups += !strcmp(w, "close-scan-up"); r.downs += !strcmp(w, "close-scan-down"); }
        else if (strcmp(w, "close-dwell")) in_scan = 0;
        head_loop(&p, &o, 0.2);
        if (p.tilt > r.maxt) r.maxt = p.tilt;
        if (p.tilt < r.mint) r.mint = p.tilt;
        T += 200;
    }
    r.tilt_end = p.tilt;
    return r;
}


/* episodes: steady near boxes, a loss of loss1_ms, a hold of hold_ms (steady box, the head stays where it is), a second loss of 14 s. Counts the VSCAN starts and records the logged events. */
typedef struct { int scans, ev_up, ev_down, ev_found, ev_nothing, ev_order_ok; } ep_res_t;
static ep_res_t ep_run(int cooldown_s, int loss1_ms, int hold_ms, int detect_in_loss1) {
    tl_t t; struct tl_cfg c; struct tl_ptz p = clp(1500); struct tl_out o; int64_t T = 1000, t0; ep_res_t r; int in_scan = 0, last_ev = 0;
    const struct tl_box nb = { 100, 60, 220, 191, 0, 0 };
    memset(&r, 0, sizeof r); r.ev_order_ok = 1;
    cfg(&c, TL_MOTION_CONT); c.lead_pct = 0; c.return_home = 1; (void)tl_lost_parse("none", &c); c.close_scan_cooldown_s = cooldown_s;
    tl_init(&t, &c); p.tilt_home = 1300;
    for (int i = 0; i < 10; i++, T += 200) { o = tick(&t, T, nb, p); head_loop(&p, &o, 0.2); }
    t0 = T;
    for (int phase = 0; phase < 3; phase++) {
        int64_t end = T + (phase == 0 ? loss1_ms : phase == 1 ? hold_ms : 14000);
        for (; T < end; T += 200) {
            const char *w;
            if (phase == 1 || (phase == 0 && detect_in_loss1 && T - t0 >= detect_in_loss1)) { o = tick(&t, T, nb, p); if (phase == 0) break; }
            else o = chain_none(&t, T, p);
            w = t.dbg.why ? t.dbg.why : "";
            if (!strncmp(w, "close-scan", 10)) { if (!in_scan) r.scans++; in_scan = 1; } else if (strcmp(w, "close-dwell")) in_scan = 0;
            head_loop(&p, &o, 0.2);
            if (t.vs_ev) {
                r.ev_up += t.vs_ev == VSEV_UP; r.ev_down += t.vs_ev == VSEV_DOWN; r.ev_found += t.vs_ev == VSEV_FOUND; r.ev_nothing += t.vs_ev == VSEV_NOTHING;
                if ((t.vs_ev == VSEV_FOUND || t.vs_ev == VSEV_NOTHING) && last_ev != VSEV_UP && last_ev != VSEV_DOWN) r.ev_order_ok = 0;
                last_ev = t.vs_ev; t.vs_ev = 0;
            }
        }
        if (phase == 0 && detect_in_loss1) { T += 200; o = tick(&t, T, nb, p); if (t.vs_ev) { r.ev_found += t.vs_ev == VSEV_FOUND; t.vs_ev = 0; } return r; }
        if (phase == 0) in_scan = 0;
    }
    return r;
}
static void episode_cases(void) {
    ep_res_t r;
    printf("== close-scan episodes: one VSCAN per loss episode, a cooldown between scans, events for the log\n");
    r = ep_run(30, 16000, 2000, 0);
    printf("   loss 16 s, 2 s box, loss 14 s (cooldown 30): scans %d, events up %d down %d found %d nothing %d\n", r.scans, r.ev_up, r.ev_down, r.ev_found, r.ev_nothing);
    CHECK(r.scans == 1 && r.ev_up == 1 && r.ev_nothing == 1 && r.ev_order_ok, "a box seen for 2 s (< 4 s) does not end the episode: the second loss runs no new VSCAN; logged once up, once done/nothing");
    r = ep_run(0, 16000, 2000, 0);
    CHECK(r.scans == 1, "the same with the cooldown 0: the episode rule alone");
    r = ep_run(30, 16000, 5000, 0);
    printf("   loss 16 s, 5 s box, loss 14 s (cooldown 30): scans %d\n", r.scans);
    CHECK(r.scans == 1, "the target held 5 s ends the episode, but the 30 s cooldown still forbids a second VSCAN");
    r = ep_run(0, 16000, 5000, 0);
    printf("   same, cooldown 0: scans %d\n", r.scans);
    CHECK(r.scans == 2, "the target held 5 s (>= 4 s) ends the episode: a new loss scans again once the cooldown is 0");
    r = ep_run(10, 16000, 5000, 0);
    CHECK(r.scans == 2, "... and with a cooldown of 10 s that has run out by then (the scan started > 10 s earlier)");
    r = ep_run(30, 16000, 3500, 0);
    CHECK(r.scans == 1, "the target held 3.5 s (< 4 s): still the same episode");
    r = ep_run(30, 6000, 2000, 3000);
    printf("   detection 3 s into the loss (mid scan): events found %d nothing %d\n", r.ev_found, r.ev_nothing);
    CHECK(r.ev_up + r.ev_down == 1 && r.ev_found == 1 && r.ev_nothing == 0, "a detection during the scan is logged as 'found', not 'nothing'");
}
static void flap_cases(void) {
    flap_res_t r;
    printf("== close-scan loop: a frame-filling person that comes and goes (hardware 2026-11-18)\n");
    r = flap_run(-1, 1400, 2600, 60000);
    printf("   flapping 1.4 s on / 2.6 s off for 60 s: VSCAN starts %d, scan ticks %d (up %d, down %d), tilt %d..%d, end %d\n", r.scans, r.scan_ticks, r.ups, r.downs, r.mint, r.maxt, r.tilt_end);
    CHECK(r.scans <= 1, "a flapping close person: at most one VSCAN per loss episode");
    r = flap_run(-1, 1400, 2600, 120000);
    printf("   same for 120 s: VSCAN starts %d, tilt %d..%d\n", r.scans, r.mint, r.maxt);
    CHECK(r.scans <= 1, "... and still one after 120 s of flapping");
    r = flap_run(-1, 800, 1700, 60000);
    printf("   flapping 0.8 s on / 1.7 s off: VSCAN starts %d, tilt %d..%d\n", r.scans, r.mint, r.maxt);
    CHECK(r.scans <= 1, "a shorter flap: at most one VSCAN");
}

/* ---------------------------------------------------------------- the close-scan (VSCAN): a close person lost with no evidence is searched up and down, then the lost policy runs */
typedef struct { int maxt, mint, ups, downs, backs, dwells, widens, home_at, up_at, down_at, tilt_end, first_scan_ms; } scan_res_t;
/* kind 2 = no evidence, 0 = clipped heads (E_UP), 1 = low crop at the bottom (E_DOWN); chain 1 = "hold:3,home", 0 = the legacy return policy with return_home 1 */
static scan_res_t scan_run(int tilt, int kind, int inv, int chain, int detect_at, int *stopped) {
    tl_t t; struct tl_cfg c; struct tl_ptz p = clp(tilt); struct tl_out o; int64_t T = 1000, t0; scan_res_t r;
    memset(&r, 0, sizeof r); r.mint = 9999; r.home_at = r.up_at = r.down_at = r.first_scan_ms = -1;
    cfg(&c, TL_MOTION_CONT); c.lead_pct = 0; c.invert_tilt = inv; c.return_home = 1;
    if (chain) (void)tl_lost_parse("hold:3,home", &c);
    tl_init(&t, &c); p.tilt_home = 1300;
    if (kind == 1) { for (int i = 0; i < 6; i++, T += 200) { struct tl_box b = { 100, 60 + i * 20, 220, 191, 0, 0 }; (void)tick(&t, T, b, p); } }
    else cl_prime(&t, p, kind, &T);
    t0 = T;
    if (stopped) *stopped = -1;
    for (int i = 0; i < 150; i++, T += 200) {
        const char *w;
        if (detect_at >= 0 && T - t0 >= detect_at) {
            struct tl_box b = { 140, 70, 180, 170, 0, 0 }; int was = t.vact;
            o = tick(&t, T, b, p);
            if (stopped) *stopped = t.nl_phase == 0 && (was ? o.kind == TL_STOP : o.kind != TL_VEL);
            break;
        }
        o = chain_none(&t, T, p);
        w = t.dbg.why ? t.dbg.why : "";
        if (o.kind == TL_HOME && r.home_at < 0) { r.home_at = (int)(T - t0); break; }
        if (o.kind == TL_VEL && !strcmp(w, "close-scan-up")) { r.ups++; if (r.up_at < 0) r.up_at = i; if (r.first_scan_ms < 0) r.first_scan_ms = (int)(T - t0); }
        if (o.kind == TL_VEL && !strcmp(w, "close-scan-down")) { r.downs++; if (r.down_at < 0) r.down_at = i; if (r.first_scan_ms < 0) r.first_scan_ms = (int)(T - t0); }
        if (o.kind == TL_VEL && !strcmp(w, "close-scan-back")) r.backs++;
        if (!strcmp(w, "close-dwell")) r.dwells++;
        if (!strcmp(w, "tilt-back")) r.widens++;
        head_loop(&p, &o, 0.2);
        if (p.tilt > r.maxt) r.maxt = p.tilt;
        if (p.tilt < r.mint) r.mint = p.tilt;
    }
    r.tilt_end = p.tilt;
    return r;
}
static void close_scan_cases(void) {
    scan_res_t r; int stopped;
    printf("== close-scan: VSCAN up and down around the loss pose, then the lost policy\n");
    /* the hardware sequence of 2026-11-18: lost at 0.6 m at tilt 1722, no evidence (home 1300) */
    r = scan_run(1722, 2, 0, 1, -1, NULL);
    printf("   loss at 1722, no evidence: first scan move after %d ms, max %d, min %d, up %d ticks, down %d, back %d, dwell ticks %d, widen %d, TL_HOME after %d ms\n", r.first_scan_ms, r.maxt, r.mint, r.ups, r.downs, r.backs, r.dwells, r.widens, r.home_at);
    CHECK(r.first_scan_ms >= 1500 && r.first_scan_ms <= 2300, "no evidence: WAIT until the prediction window (gap_predict_ms 1.5 s) is over, then the scan starts (not the tilt-back, never while relayed / predicted)");
    CHECK(r.ups >= 4 && r.maxt >= 1722 + 250 - 50 && r.maxt <= 1722 + 250 + 50, "VSCAN goes up to the loss tilt + 250");
    CHECK(r.downs >= 8 && r.mint >= 1722 - 300 - 60 && r.mint <= 1722 - 300 + 40, "... then down to the loss tilt - 300");
    CHECK(r.up_at >= 0 && r.down_at > r.up_at && r.dwells >= 6, "... up first, then down, with a dwell at each end");
    CHECK(r.backs >= 3 && r.widens == 0, "... back toward the loss tilt, never the old WIDEN straight to home");
    CHECK(r.home_at >= 0, "... and then the lost chain (hold, home) takes over");
    r = scan_run(1722, 2, 0, 0, -1, NULL);
    CHECK(r.ups >= 4 && r.downs >= 8, "the legacy return policy (return_home 1) also hands over after the scan");
    /* a detection ends it within one tick, in each part */
    {
        static const int at[] = { 1300, 2200, 3000, 4800, 6500, 9000 };
        for (unsigned k = 0; k < sizeof at / sizeof at[0]; k++) {
            r = scan_run(1722, 2, 0, 1, at[k], &stopped);
            printf("   detection %d ms after the loss: stopped within one tick: %d\n", at[k], stopped);
            CHECK(stopped == 1, "a detection during the scan (wait, up, dwell, down, back) ends the machine and stops the motion in the same tick");
        }
    }
    /* evidence UP: the directed search first (<= 400), then VSCAN covers only down */
    r = scan_run(1650, 0, 0, 1, -1, NULL);
    printf("   E_UP at 1650: max %d, min %d, scan up ticks %d, down %d\n", r.maxt, r.mint, r.ups, r.downs);
    CHECK(r.maxt > 1650 + 100 && r.maxt <= 1650 + 400 + 60, "evidence UP: the directed search climbs first, within its 400 steps");
    CHECK(r.ups == 0 && r.downs >= 8 && r.mint <= 1650 - 300 + 60, "... then the scan covers the other direction only (down to loss - 300)");
    r = scan_run(1500, 1, 0, 1, -1, NULL);
    printf("   E_DOWN at 1500: max %d, min %d, scan up ticks %d, down %d\n", r.maxt, r.mint, r.ups, r.downs);
    CHECK(r.downs == 0 && r.ups >= 4 && r.maxt >= 1500 + 250 - 50, "evidence DOWN: the directed search descends, the scan covers up only");
    /* soft limits: near the top, near the bottom, and with the tilt inverted */
    r = scan_run(2060, 2, 0, 1, -1, NULL);
    printf("   loss at 2060 (limit 2100): max %d min %d\n", r.maxt, r.mint);
    CHECK(r.ups == 0 && r.maxt <= 2100 - 45 + 50 && r.downs >= 5, "near the top soft limit: the up side is clipped away, the down side still runs");
    r = scan_run(2000, 2, 0, 1, -1, NULL);
    CHECK(r.ups >= 1 && r.maxt <= 2100 - 45 + 45 && r.maxt >= 2000 + 30, "the up side is clipped to the soft range minus its margin");
    r = scan_run(340, 2, 0, 1, -1, NULL);
    printf("   loss at 340 (limit 300): max %d min %d\n", r.maxt, r.mint);
    CHECK(r.downs == 0 && r.mint >= 300 && r.ups >= 4, "near the bottom soft limit: the down side is clipped away, the up side runs");
    r = scan_run(2060, 2, 1, 1, -1, NULL); /* inverted: up = lower tilt values */
    printf("   inverted, loss at 2060: max %d min %d, up %d down %d\n", r.maxt, r.mint, r.ups, r.downs);
    CHECK(r.ups >= 4 && r.downs == 0 && r.mint <= 2060 - 250 + 50 && r.mint >= 2060 - 250 - 50 && r.maxt <= 2100, "invert_tilt = 1: up is toward lower tilt values; the down side is clipped by the high limit");
    r = scan_run(340, 2, 1, 1, -1, NULL);
    printf("   inverted, loss at 340: max %d min %d, up %d down %d\n", r.maxt, r.mint, r.ups, r.downs);
    CHECK(r.ups == 0 && r.downs >= 5 && r.mint >= 300 && r.maxt >= 340 + 300 - 60 && r.maxt <= 340 + 300 + 50, "invert_tilt = 1 at the low limit: the up side is clipped away, down goes toward higher values");
    /* chain none / stay: the scan, then WIDEN back toward home (people must be detectable again) */
    {
        tl_t t; struct tl_cfg c; struct tl_ptz p = clp(1900); struct tl_out o; int64_t T = 1000; int widen = 0;
        cfg(&c, TL_MOTION_CONT); c.lead_pct = 0; (void)tl_lost_parse("stay", &c); tl_init(&t, &c); p.tilt_home = 1300;
        cl_prime(&t, p, 2, &T);
        for (int i = 0; i < 150; i++, T += 200) { o = chain_none(&t, T, p); widen += t.dbg.why && !strcmp(t.dbg.why, "tilt-back"); head_loop(&p, &o, 0.2); }
        printf("   chain stay: widen ticks %d, final tilt %d\n", widen, p.tilt);
        CHECK(widen > 0 && abs(p.tilt - 1300) <= 60, "chain 'stay': after the scan the WIDEN brings the tilt back to a level where people are detectable");
    }
    /* configurable: off = the old WAIT 2.5 s then WIDEN; up / down 0 = that side skipped */
    {
        tl_t t; struct tl_cfg c; struct tl_ptz p = clp(1722); struct tl_out o; int64_t T = 1000; int scans = 0;
        cfg(&c, TL_MOTION_CONT); c.lead_pct = 0; c.close_scan = 0; tl_init(&t, &c); p.tilt_home = 1300;
        cl_prime(&t, p, 2, &T);
        for (int i = 0; i < 80; i++, T += 200) { o = chain_none(&t, T, p); scans += t.dbg.why && !strncmp(t.dbg.why, "close-scan", 10); head_loop(&p, &o, 0.2); }
        CHECK(scans == 0 && abs(p.tilt - 1300) <= 60, "track_close_scan = 0: the old behaviour (WIDEN straight to home)");
        cfg(&c, TL_MOTION_CONT); c.lead_pct = 0; c.close_scan_up = 0; tl_init(&t, &c); p = clp(1722); p.tilt_home = 1300; T = 1000;
        cl_prime(&t, p, 2, &T);
        { int ups = 0, downs = 0; for (int i = 0; i < 80; i++, T += 200) { o = chain_none(&t, T, p); ups += t.dbg.why && !strcmp(t.dbg.why, "close-scan-up"); downs += t.dbg.why && !strcmp(t.dbg.why, "close-scan-down"); head_loop(&p, &o, 0.2); }
          CHECK(ups == 0 && downs > 0, "track_close_scan_up = 0: only the down side is scanned"); }
        tl_cfg_defaults(&c); c.close_scan_up = 601; CHECK(tl_cfg_validate(&c) != 0, "close_scan_up 601 is refused");
        tl_cfg_defaults(&c); c.close_scan = 2; CHECK(tl_cfg_validate(&c) != 0, "close_scan 2 is refused");
        tl_cfg_defaults(&c); CHECK(tl_cfg_validate(&c) == 0 && c.close_scan == 1 && c.close_scan_up == 250 && c.close_scan_down == 300, "defaults: scan on, 250 up, 300 down");
    }
    /* a far target never scans */
    {
        tl_t t; struct tl_cfg c; struct tl_ptz p = clp(1500); struct tl_out o; int64_t T = 1000; int scans = 0;
        cfg(&c, TL_MOTION_CONT); c.lead_pct = 0; tl_init(&t, &c); p.tilt_home = 1300;
        for (int i = 0; i < 8; i++, T += 200) { struct tl_box b = { 150, 60, 172, 120, 0, 0 }; o = tick(&t, T, b, p); }
        for (int i = 0; i < 60; i++, T += 200) { o = chain_none(&t, T, p); scans += t.dbg.why && !strncmp(t.dbg.why, "close-", 6); head_loop(&p, &o, 0.2); }
        CHECK(scans == 0, "a far target lost: no close-scan");
    }
}


/* M1 (Fable review 2026-11-18): a frame-filling loss has no UP evidence (it is too close, the head did not go up): no directed UP search, WAIT then the VSCAN under the cooldown;
 * the directed search of a plain head clip is under the cooldown too */
static void ff_loss_cases(void) {
    printf("== frame-filling loss: no directed UP search; the directed search obeys the cooldown\n");
    {
        tl_t t; struct tl_cfg c; struct tl_ptz p = clp(1650); struct tl_out o; int64_t T = 1000; int searches = 0, maxt = 0;
        cfg(&c, TL_MOTION_CONT); c.lead_pct = 0; tl_init(&t, &c); p.tilt_home = 1300;
        cl_prime(&t, p, 3, &T);
        for (int i = 0; i < 120; i++, T += 200) {
            o = chain_none(&t, T, p);
            searches += t.dbg.why && !strcmp(t.dbg.why, "exit-search");
            head_loop(&p, &o, 0.2);
            if (p.tilt > maxt) maxt = p.tilt;
        }
        printf("   frame-filling boxes then nothing for 24 s: exit-search ticks %d, nl_ev %d, max tilt %d\n", searches, t.nl_ev, maxt);
        CHECK(searches == 0 && t.nl_ev == 0, "a frame-filling person lost: no directed search (no evidence: WAIT, then the VSCAN)");
        CHECK(maxt <= 1650 + 250 + 50, "... and the tilt never goes above the VSCAN limit (loss + 250), not +380 of a directed UP search");
    }
    { /* S5: when the bridge expires the legacy exit search is explicitly off (a shorter bridge and a longer grace used to bring it back) */
        tl_t t; struct tl_cfg c; struct tl_ptz p = clp(1650); struct tl_out o; int64_t T = 1000; int searches = 0, sn = -1;
        cfg(&c, TL_MOTION_CONT); c.lead_pct = 0; c.close_scan = 0; c.close_bridge_ms = 2000; c.lost_grace_s = 8; tl_init(&t, &c); p.tilt_home = 1300;
        cl_prime(&t, p, 3, &T);
        for (int i = 0; i < 50; i++, T += 200) {
            o = chain_none(&t, T, p);
            searches += t.dbg.why && !strcmp(t.dbg.why, "exit-search");
            if (i == 12) sn = t.search_n; /* 2.4 s after the last box: the 2 s bridge is over */
            head_loop(&p, &o, 0.2);
        }
        printf("   bridge 2 s, grace 8 s: search_n %d after the bridge, exit-search ticks %d\n", sn, searches);
        CHECK(sn >= 2 && searches == 0, "the bridge expiring marks the exit search as used (search_n = SEARCH_MAX): no legacy exit search");
    }
    for (int cool = 30; cool >= 0; cool -= 30) { /* a plain head clip: search, an episode that ends (5 s held), a second loss 12 s later */
        tl_t t; struct tl_cfg c; struct tl_ptz p = clp(1650); struct tl_out o; int64_t T = 1000; int s1 = 0, s2 = 0;
        cfg(&c, TL_MOTION_CONT); c.lead_pct = 0; c.close_scan_cooldown_s = cool; tl_init(&t, &c); p.tilt_home = 1300;
        cl_prime(&t, p, 0, &T);
        for (int i = 0; i < 40; i++, T += 200) { o = chain_none(&t, T, p); s1 += t.dbg.why && !strcmp(t.dbg.why, "exit-search"); head_loop(&p, &o, 0.2); }
        p = clp(1650); cl_prime(&t, p, 0, &T);
        for (int i = 0; i < 25; i++, T += 200) { struct tl_box b = { 70, 0, 170, 160, 0, 0 }; o = tick(&t, T, b, p); }
        for (int i = 0; i < 60; i++, T += 200) { o = chain_none(&t, T, p); s2 += t.dbg.why && !strcmp(t.dbg.why, "exit-search"); head_loop(&p, &o, 0.2); }
        printf("   cooldown %d s: directed search ticks %d then %d\n", cool, s1, s2);
        CHECK(s1 >= 4, "the first loss of a head clip runs the directed search");
        if (cool) CHECK(s2 == 0, "a second loss inside the cooldown (the episode ended): no new directed search");
        else CHECK(s2 >= 4, "cooldown 0: the episode rule alone, a second search after the episode ended");
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
    cfg(&c, TL_MOTION_CONT); c.lead_pct = 0; c.close_bridge_ms = 0; /* the frame-filling dropout bridge off: this case is about the machine itself */ tl_init(&t, &c); p.tilt_home = 1300;
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
        cfg(&c, TL_MOTION_CONT); c.lead_pct = 0; c.close_scan = 0; tl_init(&t, &c);
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

/* P0-2: head boxes (class 1) ride along in the feed: steps mode ignores them; the continuous policy uses a head only when the person box is cut / wide / absent (rule 6, see heads6_cases),
 * so a whole-body walker (never cut) with heads around gives the same decisions as without them. */
static void heads_cases(void) {
    printf("== head boxes (class 1) change no decision\n");
    for (int mode = 0; mode < 2; mode++) {
        tl_t a, b; struct tl_cfg c; struct tl_ptz pa = ptz(1275, 1300), pb = ptz(1275, 1300); int diff = 0, moved = 0, ok;
        cfg(&c, mode); tl_init(&a, &c); tl_init(&b, &c);
        for (int k = 0; k < 120; k++) {
            int64_t T = 1000 + k * 200;
            double x = 1.6 * sin(k * 0.12), z = mode ? 3.4 + 0.8 * sin(k * 0.05) : 2.5 + 1.0 * sin(k * 0.05);
            struct tl_box pers = proj(x, z, 1.7, pa.pan, pa.tilt, &ok), in[TL_MAX_ALL]; struct tl_out oa, ob; int n = 0; memset(&oa, 0, sizeof oa); memset(&ob, 0, sizeof ob);
            if (k % 17 > 12) ok = 0;                                   /* some frames without a person */
            if (k % 7 == 3) { struct tl_box h = { 10, 5, 60, 55, TL_CLS_HEAD, 640 }; in[n++] = h; }  /* a head BEFORE the person, in a corner */
            if (ok) { pers.cls = TL_CLS_PERSON; pers.score = 810; in[n++] = pers; }
            { struct tl_box h = { pers.x1 + 4, pers.y1, pers.x1 + 30, pers.y1 + 26, TL_CLS_HEAD, 555 }; if (ok) in[n++] = h; }
            for (int j = 0; j < 3; j++) { struct tl_box h = { 100 + 20 * j, 40, 130 + 20 * j, 70, TL_CLS_HEAD, 300 + j }; in[n++] = h; }
            if (!ok && mode) n = 0; /* continuous: a head with no person box would keep the target alive (rule 6): not part of this case */
            pa.cap_ms = pb.cap_ms = T - 100; pa.has_cap = pb.has_cap = 1; pa.cap_pan = pb.cap_pan = pa.pan; pa.cap_tilt = pb.cap_tilt = pa.tilt;
            if (ok) tl_update(&a, T, &pers, 1, FW, FH, &pa, &oa); else tl_update(&a, T, NULL, 0, FW, FH, &pa, &oa);
            tl_update(&b, T, in, n, FW, FH, &pb, &ob);
            if (oa.kind != ob.kind || oa.dpan != ob.dpan || oa.dtilt != ob.dtilt || oa.pan_dir != ob.pan_dir || oa.pan_div != ob.pan_div || oa.tilt_dir != ob.tilt_dir || oa.tilt_div != ob.tilt_div || oa.speed != ob.speed ||
                strcmp(a.dbg.why, b.dbg.why) || a.has_target != b.has_target || a.dist_mm != b.dist_mm) diff++;
            moved += oa.kind != TL_NONE;
            if (oa.kind == TL_NUDGE) { pa.pan += oa.dpan; pa.tilt -= oa.dtilt; pa.motion_end_ms = T + 100; pb = pa; }
            if (oa.kind == TL_VEL) { pa.pan += oa.pan_dir * 80; pa.tilt -= oa.tilt_dir * 40; pb = pa; }
        }
        CHECK(moved > 10 && diff == 0, mode ? "continuous: a whole-body walker with head boxes of both classes around: identical decisions (heads unused)" : "steps: a walker with head boxes around: identical decisions (heads ignored)");
        /* only heads in a frame = nobody */
        { struct tl_box h[2] = { { 100, 20, 140, 60, TL_CLS_HEAD, 700 }, { 200, 20, 240, 60, TL_CLS_HEAD, 650 } }; struct tl_out o1; tl_t u; tl_init(&u, &c);
          tl_update(&u, 5000, h, 2, FW, FH, &pa, &o1); CHECK(!u.has_target && o1.kind == TL_NONE, "heads only: no target, no motion"); }
    }
}

/* P0-5: the video latency of the pose-at-capture lookup may be negative */
/* P1-1: rule 6, the head as the vertical reference (and the pan reference) when the person box is cut / wide / absent. */
static struct tl_box hbox(int x1, int y1, int x2, int y2) { struct tl_box b = { x1, y1, x2, y2, TL_CLS_HEAD, 700 }; return b; }
/* one tick with an optional person box and heads (heads after the person, like the detector feeds them) */
static struct tl_out tickh(tl_t *t, int64_t now, const struct tl_box *person, const struct tl_box *heads, int nh, struct tl_ptz p) {
    struct tl_out o; struct tl_box in[TL_MAX_ALL]; int n = 0;
    p.cap_ms = now - 100;
    if (person) { in[n] = *person; in[n].cls = TL_CLS_PERSON; n++; }
    for (int i = 0; i < nh; i++) in[n++] = heads[i];
    tl_update(t, now, in, n, FW, FH, &p, &o);
    return o;
}
/* a frame-filling, top-clipped person box (height >= 90 %): the body box never drives the tilt (hold; the head box is the reference when there is one) and whatever tilt travel remains is
 * bounded to 150 steps from the pose where it became frame-filling (hardware 2026-11-18: 1,0,249,191 took the tilt 1500 -> 2086 in 2 s, to the soft limit) */
static int ff_run(struct tl_box b, const struct tl_box *head, int *maxd, int *moves) {
    tl_t t; struct tl_cfg c; struct tl_ptz p = clp(1500); struct tl_out o; int64_t T = 1000; int mn = 0, mx = 0, mv = 0;
    cfg(&c, TL_MOTION_CONT); c.lead_pct = 0; tl_init(&t, &c); p.tilt_home = 1300;
    for (int i = 0; i < 100; i++, T += 200) {
        o = tickh(&t, T, &b, head, head ? 1 : 0, p);
        mv += o.kind == TL_VEL && o.tilt_dir;
        head_loop(&p, &o, 0.2);
        if (p.tilt - 1500 > mx) mx = p.tilt - 1500;
        if (p.tilt - 1500 < mn) mn = p.tilt - 1500;
    }
    *maxd = mx > -mn ? mx : -mn; *moves = mv;
    return p.tilt;
}
static void framefill_cases(void) {
    static const struct tl_box fb[] = { { 1, 0, 249, 191, 0, 0 }, { 0, 0, 319, 191, 0, 0 }, { 40, 0, 300, 186, 0, 0 }, { 103, 0, 200, 175, 0, 0 } };
    struct tl_box head = { 100, 0, 150, 40, 0, TL_CLS_HEAD };
    int mx, mv;
    printf("== frame-filling top-clipped box: tilt climb bounded to 150 steps\n");
    for (unsigned k = 0; k < sizeof fb / sizeof fb[0]; k++) {
        int tl = ff_run(fb[k], NULL, &mx, &mv);
        printf("   box %d,%d,%d,%d for 20 s: tilt 1500 -> %d (max travel %d), tilt commands %d\n", fb[k].x1, fb[k].y1, fb[k].x2, fb[k].y2, tl, mx, mv);
        CHECK(mx <= 150, "a frame-filling top-clipped box: at most 150 steps of tilt travel (never walks to the soft limit)");
    }
    (void)ff_run(fb[0], NULL, &mx, &mv);
    CHECK(mv >= 1 && mv <= 12, "... the first climb is allowed (a first head clip starts the tilt) but it stops at the cap: no repeated commands");
    (void)ff_run(fb[0], &head, &mx, &mv);
    printf("   with a clipped head box: max travel %d, tilt commands %d\n", mx, mv);
    CHECK(mx <= 150, "with a head box (rule 6) the excursion is bounded to 150 steps too");
}
/* M2 (Fable review 2026-11-18): the frame-filling cap does not ratchet: the origin survives a box that is only 88 % tall, a gap, and a loss / re-acquisition; it ends after 2 s of boxes
 * shorter than 77 % or when the head went home; the cap covers head-only frames while the sighting is fresh */
static void ff_ratchet_cases(void) {
    static const struct tl_box FF = { 1, 0, 249, 191, 0, 0 }, T88 = { 1, 0, 249, 169, 0, 0 };
    printf("== frame-filling cap: no ratchet (alternation, loss / re-acquisition, head-only frames)\n");
    { /* 88 % / frame-filling alternation for 60 s */
        tl_t t; struct tl_cfg c; struct tl_ptz p = clp(1500); struct tl_out o; int64_t T = 1000; int mx = 0;
        cfg(&c, TL_MOTION_CONT); c.lead_pct = 0; tl_init(&t, &c); p.tilt_home = 1300;
        for (int i = 0; i < 300; i++, T += 200) { o = tick(&t, T, i & 1 ? T88 : FF, p); head_loop(&p, &o, 0.2); if (p.tilt - 1500 > mx) mx = p.tilt - 1500; }
        printf("   alternating 88 %% / frame-filling boxes for 60 s: max climb %d, tilt %d\n", mx, p.tilt);
        CHECK(mx <= 150 + 60 && p.tilt < 2100 - 100, "alternating 88 % and frame-filling boxes: the tilt never goes more than 150 (+ lease slack) above the episode origin, never to the soft limit");
    }
    { /* repeated loss / re-acquisition episodes (gaps beyond the bridge, no scan / tilt-back: the cap alone is under test) */
        tl_t t; struct tl_cfg c; struct tl_ptz p = clp(1500); struct tl_out o; int64_t T = 1000; int mx = 0;
        cfg(&c, TL_MOTION_CONT); c.lead_pct = 0; c.close_scan = 0; c.tilt_back_s = 0; tl_init(&t, &c); p.tilt_home = 1300;
        for (int ep = 0; ep < 8; ep++) {
            for (int i = 0; i < 40; i++, T += 200) { o = tick(&t, T, ep & 1 ? T88 : FF, p); head_loop(&p, &o, 0.2); if (p.tilt - 1500 > mx) mx = p.tilt - 1500; }
            for (int i = 0; i < 40; i++, T += 200) { o = chain_none(&t, T, p); head_loop(&p, &o, 0.2); if (p.tilt - 1500 > mx) mx = p.tilt - 1500; }
        }
        printf("   8 x (8 s boxes, 8 s loss): max climb %d, tilt %d\n", mx, p.tilt);
        CHECK(mx <= 150 + 60 && p.tilt < 2100 - 100, "repeated loss / re-acquisition episodes: the climb stays within 150 (+ slack) of the first origin (a gap of more than 5 s no longer re-arms it)");
    }
    { /* the origin ends after 2 s of boxes shorter than 77 %, not before; and when the head went home */
        tl_t t; struct tl_cfg c; struct tl_ptz p = clp(1500); struct tl_out o; int64_t T = 1000; struct tl_box lowb = { 110, 40, 150, 140, 0, 0 }; /* 52 % tall */
        cfg(&c, TL_MOTION_CONT); c.lead_pct = 0; tl_init(&t, &c); p.tilt_home = 1300;
        for (int i = 0; i < 10; i++, T += 200) { o = tick(&t, T, FF, p); head_loop(&p, &o, 0.2); }
        CHECK(t.ff_set, "frame-filling boxes arm the cap");
        for (int i = 0; i < 4; i++, T += 200) { o = tick(&t, T, lowb, p); head_loop(&p, &o, 0.2); }
        CHECK(t.ff_set, "0.8 s of short boxes: the cap origin is kept");
        for (int i = 0; i < 12; i++, T += 200) { o = tick(&t, T, lowb, p); head_loop(&p, &o, 0.2); }
        CHECK(!t.ff_set, "after 2 s of boxes shorter than 77 % the sighting is over");
    }
    { /* head-only frames (no person box) of a close target: the cap applies while the sighting is fresh */
        tl_t t; struct tl_cfg c; struct tl_ptz p = clp(1500); struct tl_out o; int64_t T = 1000; int mx = 0; struct tl_box h = hbox(100, 0, 160, 70);
        cfg(&c, TL_MOTION_CONT); c.lead_pct = 0; tl_init(&t, &c); p.tilt_home = 1300;
        for (int i = 0; i < 10; i++, T += 200) { o = tick(&t, T, FF, p); head_loop(&p, &o, 0.2); }
        for (int i = 0; i < 100; i++, T += 200) { o = tickh(&t, T, NULL, &h, 1, p); head_loop(&p, &o, 0.2); if (p.tilt - 1500 > mx) mx = p.tilt - 1500; }
        printf("   close head alone (clipped) for 20 s after the frame-filling boxes: max climb %d, last_ff %d\n", mx, t.last_ff);
        CHECK(mx <= 150 + 60, "a clipped head alone after frame-filling boxes: the climb stays within the 150-step cap");
    }
}
/* A close person who rises (user test 2026-11-19: "when I move up it does not follow me"): the box stays top-clipped, its bottom gets higher. The camera must tilt up beyond the
 * frame-filling cap (150) by the steps of that drop, never beyond the extra bound, and never on flickering / alternating boxes. */
static void ff_rise_cases(void) {
    static const struct tl_box FF = { 1, 0, 249, 191, 0, 0 };
    printf("== frame-filling cap: a real rise is followed, a flicker is not\n");
    { /* 3 s frame-filling, then the visible part shrinks to y2 = 150 (top still clipped) and stays */
        tl_t t; struct tl_cfg c; struct tl_ptz p = clp(1500); struct tl_out o; int64_t T = 1000; int mx = 0; struct tl_box r = { 1, 0, 249, 150, 0, 0 };
        cfg(&c, TL_MOTION_CONT); c.lead_pct = 0; tl_init(&t, &c); p.tilt_home = 1300;
        for (int i = 0; i < 15; i++, T += 200) { o = tick(&t, T, FF, p); head_loop(&p, &o, 0.2); }
        for (int i = 0; i < 150; i++, T += 200) { o = tick(&t, T, r, p); head_loop(&p, &o, 0.2); if (p.tilt - 1500 > mx) mx = p.tilt - 1500; }
        printf("   rise 191 -> 150: max climb %d\n", mx);
        CHECK(mx >= 150 + 40, "a close person rising (box top clipped, bottom 41 px higher): the camera climbs past the 150 cap");
        CHECK(mx <= 150 + 300 + 60 && p.tilt < 2100 - 100, "... bounded by the extra budget, never to the soft limit (the box does not change: no ratchet)");
    }
    { /* flicker: frame-filling and a shorter clipped box alternate for 60 s: the bottoms of the last 3 boxes always include a tall one */
        tl_t t; struct tl_cfg c; struct tl_ptz p = clp(1500); struct tl_out o; int64_t T = 1000; int mx = 0; struct tl_box r = { 1, 0, 249, 150, 0, 0 };
        cfg(&c, TL_MOTION_CONT); c.lead_pct = 0; tl_init(&t, &c); p.tilt_home = 1300;
        for (int i = 0; i < 300; i++, T += 200) { o = tick(&t, T, i & 1 ? r : FF, p); head_loop(&p, &o, 0.2); if (p.tilt - 1500 > mx) mx = p.tilt - 1500; }
        printf("   flicker 191 / 150 for 60 s: max climb %d\n", mx);
        CHECK(mx <= 150 + 60, "alternating 191 / 150 boxes: no extra climb (a flicker is not a rise)");
    }
    { /* a flicker with a period of 3 (two short, one tall) */
        tl_t t; struct tl_cfg c; struct tl_ptz p = clp(1500); struct tl_out o; int64_t T = 1000; int mx = 0; struct tl_box r = { 1, 0, 249, 150, 0, 0 };
        cfg(&c, TL_MOTION_CONT); c.lead_pct = 0; tl_init(&t, &c); p.tilt_home = 1300;
        for (int i = 0; i < 300; i++, T += 200) { o = tick(&t, T, i % 3 == 0 ? FF : r, p); head_loop(&p, &o, 0.2); if (p.tilt - 1500 > mx) mx = p.tilt - 1500; }
        CHECK(mx <= 150 + 60, "a tall box every third tick: no extra climb");
    }
}
/* Sitting low (user 2026-11-19: "sitting really low it does not work"; session1 log: the head at row 113 of 192, the scan found it 300 steps below the loss pose; counter + = up,
 * checked on the log: u commands raise the counter and move the head row down). A person sits down: the box top goes from row 40 to row 140, the bottom stays cut at the frame
 * bottom. The camera must go DOWN (counter lower) within the soft range, bring the head back above the lower third, and never command up while the person is low. */
static void sit_low_cases(void) {
    for (int far = 0; far < 2; far++) {
        tl_t t; struct tl_cfg c; struct tl_ptz p = clp(1300); struct tl_out o; int64_t T = 1000; int ups = 0, top = 0, minn = 1300;
        printf("== sitting low (%s): the camera follows down\n", far ? "far, short box" : "near, box cut at the bottom");
        cfg(&c, TL_MOTION_CONT); c.lead_pct = 0; tl_init(&t, &c); p.tilt_home = 1300;
        for (int i = 0; i < 120; i++, T += 200) {
            double world = i < 20 ? 40 : i < 35 ? 40 + (i - 20) * 100.0 / 15 : 140; /* the box top, image rows at tilt 1300 */
            struct tl_box b;
            top = (int)(world + (p.tilt - 1300) * 0.274);
            if (top < 0) top = 0;
            b = far ? box(160, top + 25, 40, 50) : box(160, (top + 191) / 2, 110, 191 - top);
            if (!far) b.y2 = 191;
            o = tick(&t, T, b, p);
            head_loop(&p, &o, 0.2);
            if (i >= 35 && (o.kind == TL_VEL && o.tilt_dir && ((c.invert_tilt ? -1 : 1) * -o.tilt_dir > 0))) ups++;
            if (p.tilt < minn) minn = p.tilt;
        }
        printf("   final tilt %d (min %d), box top row %d, up commands after the sit %d\n", p.tilt, minn, top, ups);
        CHECK(p.tilt < 1300 - 120 && p.tilt >= 300, "the camera tilted down to follow the seated person, within the soft range");
        CHECK(top <= 110, "the seated head ends in the upper 60 % of the frame");
        CHECK(ups == 0, "no up command while the person stays low");
    }
}
/* A person who sits down and drops out of the frame (session1: last head rows 54 -> 73, then lost): the close scan must not start UP (250 steps, ~6 s the wrong way). */
static void sit_lost_cases(void) {
    tl_t t; struct tl_cfg c; struct tl_ptz p = clp(1300); struct tl_out o; int64_t T = 1000; int ups = 0, downs = 0, firstdir = 0, tl0 = 0, maxt = 0;
    printf("== sitting down and lost: the close scan goes down first\n");
    cfg(&c, TL_MOTION_CONT); c.lead_pct = 0; tl_init(&t, &c); p.tilt_home = 1300;
    for (int i = 0; i < 160; i++, T += 200) {
        int top = i < 20 ? 40 : 40 + (i - 20) * 8 + (int)((p.tilt - 1300) * 0.274); /* sinking 8 px per tick */
        if (i < 28) { o = tick(&t, T, box(160, (top + 191) / 2, 110, 191 - top), p); }
        else o = chain_none(&t, T, p);
        head_loop(&p, &o, 0.2);
        if (i == 28) tl0 = maxt = p.tilt;
        if (i > 28 && t.dbg.why && !strcmp(t.dbg.why, "close-scan-up")) maxt = p.tilt + 1000; /* an UP leg of the scan */
        if (i >= 28 && o.kind == TL_VEL && o.tilt_dir) { int d = (c.invert_tilt ? -1 : 1) * -o.tilt_dir; if (!firstdir) firstdir = d; if (d > 0) ups++; else downs++; }
    }
    printf("   first tilt command after the loss: %s, up-scan marker %d (loss pose %d), down %d\n", firstdir > 0 ? "up" : "down", maxt, tl0, downs);
    CHECK(downs > 0 && firstdir < 0 && maxt <= tl0, "a person last seen sinking: the loss scan goes down first and has no UP leg");
    (void)ups;
}
/* The reversal throttle must not blunt a genuine reversal: a person walking right at 250 steps/s turns and walks left at 250 steps/s. The robust velocity the new way is
 * established (consistent samples, net travel) after a few ticks, and the guard then waits half the time. A flicker / jump (rev_cases) keeps the full guard. */
static void rev_fast_cases(void) {
    tl_t t; struct tl_cfg c; struct tl_ptz p = clp(1300); struct tl_out o; int64_t T = 1000, last_old = 0, first_new = 0; double w = 1300; int d0 = 0;
    printf("== reversal throttle: a genuine reversal is answered faster than the full guard\n");
    cfg(&c, TL_MOTION_CONT); c.lead_pct = 0; tl_init(&t, &c);
    for (int i = 0; i < 70; i++, T += 200) { /* 14 s: one way for 6 s, then back */
        double v = i < 30 ? 250 : -250; /* world pan steps/s */
        int cx;
        w += v * 0.2;
        cx = 160 - (int)((w - p.pan) * 320.0 / 1000.0);
        cx = cx < 30 ? 30 : cx > 290 ? 290 : cx;
        o = tick(&t, T, box(cx, 96, 40, 110), p);
        head_loop(&p, &o, 0.2);
        if (o.kind == TL_VEL && o.pan_dir) {
            if (!d0) d0 = o.pan_dir;
            if (o.pan_dir == d0 && !first_new) last_old = T;
            else if (o.pan_dir == -d0 && i >= 30 && !first_new) first_new = T;
        }
    }
    printf("   last old-direction command %d, first reversed command %d (%d ms)\n", (int)last_old, (int)first_new, (int)(first_new - last_old));
    CHECK(first_new > 0 && first_new - last_old < 700, "a person who really turns: the opposite command comes before the full 700 ms guard (a velocity the new way is established)");
}
/* track.tilt 0 (pan only tracking): in no situation the tracker moves the tilt: follow of a head that rises / sinks, a top-clipped close person, loss with the close scan, the
 * lost chain (exit up / down, local, tilt-back), continuous output. */
static void pan_only_cases(void) {
    int leaks = 0, cmds = 0;
    printf("== pan only (tilt_enable 0): no tilt command in any situation\n");
    for (int sc = 0; sc < 6; sc++) {
        tl_t t; struct tl_cfg c; struct tl_ptz p = clp(sc == 5 ? 1700 : 1300); struct tl_out o; int64_t T = 1000; int ok;
        cfg(&c, TL_MOTION_CONT); c.lead_pct = 0; c.tilt_enable = 0; c.tilt_back_s = 5; c.close_scan = 1; c.return_home = 1; c.return_delay_s = 3; tl_init(&t, &c);
        p.tilt_home = 1300;
        for (int i = 0; i < 400; i++, T += 200) {
            struct tl_box b; int seen = 1;
            switch (sc) {
            case 0: b = box(160, 96 - (i < 100 ? i : 100), 60, 150); break;           /* a head that rises 100 px */
            case 1: b = box(160, 96 + (i < 100 ? i : 100), 60, 110); break;           /* sinks */
            case 2: { struct tl_box f = { 1, 0, 249, 191, 0, 0 }; b = f; seen = i < 40 || (i % 3 == 0 && i < 200); break; } /* frame-filling, flapping, then lost */
            case 3: { struct tl_box f = { 90, 37, 243, 191, 0, 0 }; b = f; seen = i < 40 || i % 3 == 0; if (i > 150) seen = 0; break; }
            case 4: b = proj(0.05, 3.0 - 0.01 * i, 1.7, p.pan, p.tilt, &ok); seen = ok && i < 120; break; /* approach, then gone */
            default: b = box(160, 5, 60, 150); seen = i < 20; break;                  /* top clipped far from home, then lost (tilt-back) */
            }
            if (sc == 0 || sc == 1 || sc == 5) { int sh = (i / 25 & 1) ? 70 : -70; b.x1 += sh; b.x2 += sh; } /* the person also walks sideways: pan commands */
            o = seen ? tick(&t, T, b, p) : chain_none(&t, T, p);
            head_loop(&p, &o, 0.2);
            cmds += o.kind == TL_VEL || o.kind == TL_NUDGE;
            if ((o.kind == TL_VEL && o.tilt_dir) || (o.kind == TL_NUDGE && o.dtilt)) { leaks++; if (getenv("TL_TRACE")) printf("    sc %d i %d tilt cmd why=%s step=%s\n", sc, i, t.dbg.why ? t.dbg.why : "?", t.lstep ? t.lstep : "?"); }
        }
    }
    printf("   %d pan commands in total, %d tilt commands\n", cmds, leaks);
    CHECK(cmds > 0 && leaks == 0, "with tilt disabled no scenario (follow, close, flapping, loss, scan, tilt-back) ever commands the tilt");
}
/* M3 (Fable review 2026-11-18): the reversal throttle at emit_vel, per axis: no opposite command within 700 ms of the last one the other way; the 3rd reversal in 6 s is refused and the axis held 3 s */
static void rev_cases(void) {
    printf("== reversal throttle (continuous): 700 ms rest, 4 reversals per 4 s, then a 1.5 s hold\n");
    for (int per = 2; per <= 5; per += 3) { /* a target that jumps left / right every `per` ticks (0.4 s / 1.0 s), the head follows */
        tl_t t; struct tl_cfg c; struct tl_ptz p = clp(1300); struct tl_out o; int64_t T = 1000, last_dir_t[3] = { 0, 0, 0 }, rev_t[200]; int nrev = 0, bad = 0, lastd = 0, flips_ticks = 0, win_bad = 0, pans = 0;
        cfg(&c, TL_MOTION_CONT); c.lead_pct = 0; tl_init(&t, &c);
        for (int i = 0; i < 150; i++, T += 200) { /* 30 s */
            int right = (i / per) & 1;
            o = tick(&t, T, box(right ? 160 + 120 : 160 - 120, 96, 40, 110), p);
            head_loop(&p, &o, 0.2);
            if (o.kind == TL_VEL && o.pan_dir) {
                int d = o.pan_dir; pans++;
                if (lastd && d != lastd) { /* a reversal: the last command the other way must be >= 700 ms old */
                    if (T - last_dir_t[lastd + 1] < 350) bad++; /* 700 ms, or half of it when the reversal is backed by an established velocity (REV_FAST_DIV) */
                    rev_t[nrev++] = T;
                }
                last_dir_t[d + 1] = T; lastd = d;
            }
            flips_ticks += o.kind == TL_VEL && o.pan_dir && lastd != o.pan_dir;
        }
        for (int a = 0; a < nrev; a++) { int in = 0; for (int b = 0; b < nrev; b++) if (rev_t[b] >= rev_t[a] && rev_t[b] < rev_t[a] + 4000) in++; if (in > 4) win_bad++; }
        printf("   target jumping every %d ticks: pan commands %d, reversals %d, too early %d, windows with > 4 reversals %d\n", per, pans, nrev, bad, win_bad);
        CHECK(pans > 0 && bad == 0, "no opposite pan command within 350 ms (700 ms without an established velocity) of the last one the other way");
        CHECK(win_bad == 0 && nrev <= 24, "at most 4 reversals in any 4 s; the 5th is refused and the axis held (a jumping target does not hunt the head)");
        (void)flips_ticks;
    }
    { /* the hold stops the axis: after the 1st reversal (allowed) the 2nd is refused, and the axis is not commanded again for 3 s */
        tl_t t; struct tl_cfg c; struct tl_ptz p = clp(1300); struct tl_out o; int64_t T = 1000, t_left = 0, t_right2 = 0;
        cfg(&c, TL_MOTION_CONT); c.lead_pct = 0; c.rev_n = 1; tl_init(&t, &c);
        for (int i = 0; i < 6; i++, T += 200) o = tick(&t, T, box(160 + 120, 96, 40, 110), p);      /* right (pan_dir < 0) */
        for (int i = 0; i < 8; i++, T += 200) { o = tick(&t, T, box(160 - 120, 96, 40, 110), p); if (o.kind == TL_VEL && o.pan_dir > 0) t_left = T; } /* left: the 1st reversal (allowed) */
        for (int i = 0; i < 40; i++, T += 200) { o = tick(&t, T, box(160 + 120, 96, 40, 110), p); if (!t_right2 && o.kind == TL_VEL && o.pan_dir < 0) t_right2 = T; } /* right again */
        printf("   hold: last left command at %d, right again at %d (%d ms later)\n", (int)t_left, (int)t_right2, (int)(t_right2 - t_left));
        CHECK(t_left > 0 && t_right2 > 0, "rev_n 1: the first reversal goes through, and the axis moves again later");
        CHECK(t_right2 - t_left >= 3000, "rev_n 1: the second reversal in 6 s is refused and the axis is held still for 3 s");
    }
    { /* hardware log 2026-11-19 (0.9 m, walking left/right every ~1.4 s): the 3rd reversal in 3 s was refused and the head stood still 3 s while the person left the frame; a walk of 5 direction changes must be followed at each one */
        tl_t t; struct tl_cfg c; struct tl_ptz p = clp(1300); struct tl_out o; int64_t T = 1000, flip_t = 0; int followed = 0, flips = 0, want = 0, got = 0;
        cfg(&c, TL_MOTION_CONT); tl_init(&t, &c);
        for (int i = 0; i < 60; i++, T += 200) {
            int seg = i / 7, right = seg & 1; /* a triangle wave: the person walks 200 px in 1.4 s, then back */
            if (i % 7 == 0 && i > 0) { flips++; flip_t = T; got = 0; want = right ? -1 : 1; }
            o = tick(&t, T, box(160 + (right ? 100 - 200 * (i - seg * 7) / 7 : -100 + 200 * (i - seg * 7) / 7), 96, 40, 110), p);
            head_loop(&p, &o, 0.2);
            if (getenv("TL_TRACE")) printf("    walk i %d x %s kind %d pan_dir %d why %s\n", i, right ? "R" : "L", o.kind, o.pan_dir, t.dbg.why ? t.dbg.why : "?");
            if (i > 0 && want && !got && o.kind == TL_VEL && o.pan_dir == want && T - flip_t <= 1400) { got = 1; followed++; }
        }
        printf("   walk: %d direction changes, followed %d\n", flips, followed);
        CHECK(flips >= 5 && followed >= 2 && followed < flips, "a person pacing every 1.4 s: the first turns are followed, then the pacing band takes over (the head aims at the middle of the band, it does not reverse at every turn)");
    }
    { /* "less stable and slower over time" (user 2026-11-19): 4 minutes of walking left / right (legs of 4 .. 6 s, not a pacer: the strict original checks), the reaction time to a turn must not grow: tracker state that accumulates (reversal holds, cooldowns, filters) would show */
        tl_t t; struct tl_cfg c; struct tl_ptz p = clp(1300); struct tl_out o; int64_t T = 1000, flip_t = 0; int want = 0, got = 1, n = 0, seg_len = 20, i = 0, i0 = 0, right = 0, ph = 0;
        int64_t lat_sum[3] = { 0, 0, 0 }; int lat_n[3] = { 0, 0, 0 }, miss[3] = { 0, 0, 0 }; int total = 1200;
        cfg(&c, TL_MOTION_CONT); tl_init(&t, &c);
        for (i = 0; i < total; i++, T += 200) {
            if (i - i0 >= seg_len) { /* a turn: alternate a 1.4 s and a 2.6 s leg */
                int third = i * 3 / total;
                if (!got && want) miss[third == 0 ? 0 : (i - 1) * 3 / total]++;
                i0 = i; right ^= 1; ph ^= 1; seg_len = 20 + (n * 7) % 11; (void)ph; n++; flip_t = T; got = 0; want = right ? -1 : 1;
            }
            { int k = i - i0, len = seg_len; int x = right ? 100 - 200 * k / len : -100 + 200 * k / len;
              o = tick(&t, T, box(160 + x, 96, 40, 110), p); head_loop(&p, &o, 0.2); }
            if (want && !got && o.kind == TL_VEL && o.pan_dir == want) { int th = i * 3 / total; got = 1; lat_sum[th] += T - flip_t; lat_n[th]++; }
        }
        printf("   long walk: %d turns; mean reaction first/middle/last third: %d / %d / %d ms; unanswered %d / %d / %d\n", n,
               lat_n[0] ? (int)(lat_sum[0] / lat_n[0]) : -1, lat_n[1] ? (int)(lat_sum[1] / lat_n[1]) : -1, lat_n[2] ? (int)(lat_sum[2] / lat_n[2]) : -1, miss[0], miss[1], miss[2]);
        CHECK(lat_n[0] > 10 && lat_n[2] > 10 && lat_sum[2] / lat_n[2] <= lat_sum[0] / lat_n[0] + 150, "4 minutes of walking: the reaction to a turn is not slower in the last third than in the first");
        CHECK(miss[2] <= miss[0] + 2, "4 minutes of walking: no more unanswered turns in the last third");
    }
    { /* a pacer (a turn every 1.4 s .. 2.6 s) for 4 minutes: the pacing band holds the whole time, the answered / unanswered share of the turns and the head excursion do not change from the first to the last third */
        tl_t t; struct tl_cfg c; struct tl_ptz p = clp(1300); struct tl_out o; int64_t T = 1000, flip_t = 0; int want = 0, got = 1, n = 0, seg_len = 7, i = 0, i0 = 0, right = 0, ph = 0;
        int64_t lat_sum[3] = { 0, 0, 0 }; int lat_n[3] = { 0, 0, 0 }, miss[3] = { 0, 0, 0 }; int total = 1200; int exc[3] = { 0, 0, 0 };
        cfg(&c, TL_MOTION_CONT); tl_init(&t, &c);
        for (i = 0; i < total; i++, T += 200) {
            if (i - i0 >= seg_len) { /* a turn: alternate a 1.4 s and a 2.6 s leg */
                int third = i * 3 / total;
                if (!got && want) miss[third == 0 ? 0 : (i - 1) * 3 / total]++;
                i0 = i; right ^= 1; ph ^= 1; seg_len = ph ? 13 : 7; n++; flip_t = T; got = 0; want = right ? -1 : 1;
            }
            { int k = i - i0, len = seg_len; int x = right ? 100 - 200 * k / len : -100 + 200 * k / len;
              o = tick(&t, T, box(160 + x, 96, 40, 110), p); head_loop(&p, &o, 0.2); { int th = i * 3 / total, d = abs(p.pan - 1300); if (d > exc[th]) exc[th] = d; } }
            if (want && !got && o.kind == TL_VEL && o.pan_dir == want) { int th = i * 3 / total; got = 1; lat_sum[th] += T - flip_t; lat_n[th]++; }
        }
        printf("   long walk: %d turns; mean reaction first/middle/last third: %d / %d / %d ms; unanswered %d / %d / %d\n", n,
               lat_n[0] ? (int)(lat_sum[0] / lat_n[0]) : -1, lat_n[1] ? (int)(lat_sum[1] / lat_n[1]) : -1, lat_n[2] ? (int)(lat_sum[2] / lat_n[2]) : -1, miss[0], miss[1], miss[2]);
        printf("   pacing: answered %d / %d / %d, head excursion %d / %d / %d\n", lat_n[0], lat_n[1], lat_n[2], exc[0], exc[1], exc[2]);
        CHECK(n > 100 && lat_n[0] <= 5 && labs((long)miss[2] - miss[1]) <= 1 && lat_n[1] == lat_n[2], "pacing for 4 minutes: the band is recognised in the first turns (<= 5 answered), then the same share of turns is answered in the middle and the last third (+-1)");
        CHECK(exc[2] <= exc[1] + 30 && exc[1] > 0 && exc[2] <= exc[0], "pacing for 4 minutes: the head excursion does not grow from the middle to the last third (no drift) and never exceeds the start-up one");
    }
    { struct tl_cfg c; tl_cfg_defaults(&c); CHECK(c.rev_guard_ms == 700 && c.rev_n == 4 && c.rev_window_ms == 4000 && c.rev_hold_ms == 1500 && tl_cfg_validate(&c) == 0, "defaults: 700 ms, 4 reversals, 4 s, 1.5 s hold");
      c.rev_n = 0; CHECK(tl_cfg_validate(&c) != 0, "rev_n 0 is refused");
      tl_cfg_defaults(&c); c.rev_guard_ms = 0; CHECK(tl_cfg_validate(&c) == 0, "rev_guard_ms 0 (off) is valid"); }
}
/* S2 (Fable review 2026-11-18): close_same only lifts the sticky gate, never zeroes the distance: two close people overlapping in the image keep the true target whatever the array order; the
 * overlap is compared in world pan coordinates (the head turned since the target box) */
static void close_same_cases(void) {
    printf("== close_same: lifts the gate only, world coordinates\n");
    { /* two close people overlap, the detector lists the other one first */
        tl_t t; struct tl_cfg c; struct tl_ptz p = clp(1500); struct tl_out o; int64_t T = 1000; struct tl_box A = { 40, 0, 200, 191, 0, 0 }, A2 = { 42, 0, 202, 191, 0, 0 }, B = { 110, 0, 319, 191, 0, 0 }, two[2];
        cfg(&c, TL_MOTION_CONT); c.lead_pct = 0; tl_init(&t, &c); p.tilt_home = 1300;
        for (int i = 0; i < 8; i++, T += 200) o = tick(&t, T, A, p);
        two[0] = B; two[1] = A2; p.cap_ms = T - 100;
        tl_update(&t, T, two, 2, FW, FH, &p, &o);
        printf("   [other person, target]: followed box x1 = %d (target 42, other 110)\n", t.target.x1);
        CHECK(t.target.x1 == 42, "two close people that overlap: the true target (the nearer in world terms) is kept, whatever the array order");
        tl_init(&t, &c); T = 1000;
        for (int i = 0; i < 8; i++, T += 200) o = tick(&t, T, A, p);
        two[0] = A2; two[1] = B; p.cap_ms = T - 100;
        tl_update(&t, T, two, 2, FW, FH, &p, &o);
        CHECK(t.target.x1 == 42, "... and the other way round");
    }
    { /* the head turned 450 steps since the target box: the same image columns are another person in world terms */
        tl_t t; struct tl_cfg c; struct tl_ptz p = clp(1500); struct tl_out o; int64_t T = 1000, seen; struct tl_box A = { 40, 0, 200, 191, 0, 0 };
        cfg(&c, TL_MOTION_CONT); c.lead_pct = 0; tl_init(&t, &c); p.tilt_home = 1300;
        for (int i = 0; i < 8; i++, T += 200) o = tick(&t, T, A, p);
        seen = t.last_seen_ms;
        p.pan += 450; p.cap_pan = p.pan;
        o = tick(&t, T, A, p); (void)o;
        printf("   the same columns after a 450-step turn: last seen %d -> %d\n", (int)seen, (int)t.last_seen_ms);
        CHECK(t.last_seen_ms == seen, "overlap in image columns of two poses is not the same close person: the box is not adopted as the target");
    }
}
/* S3 (Fable review 2026-11-18): the forced lateral update of a close person (kf_force) takes the measurement with an inflated noise instead of re-initialising the filter: a 77-90 % tall body box
 * whose centre wanders by more than the gate does not flip the pan direction every tick */
static void kf_force_cases(void) {
    printf("== kf_force: a wandering close box does not steer the pan\n");
    for (int amp = 60; amp <= 64; amp += 2) {
        tl_t t; struct tl_cfg c; struct tl_ptz p = clp(1500); struct tl_out o; int64_t T = 1000; int flips = 0, last = 0, cmds = 0;
        cfg(&c, TL_MOTION_CONT); c.lead_pct = 0; c.rev_guard_ms = 0; c.zone_x_pct = 20; tl_init(&t, &c); p.tilt_home = 1300; /* the reversal throttle off, a narrow zone (+-100 steps): the filter alone is under test */
        for (int i = 0; i < 10; i++, T += 200) o = tick(&t, T, box(160, 96, 160, 160), p);
        for (int i = 0; i < 40; i++, T += 200) {
            o = tick(&t, T, box(160 + (i & 1 ? amp : -amp), 96, 160, 160), p); /* 83 % tall, half the frame wide: a close person, not frame-filling; the centre jumps by 2 x amp px every tick */
            if (o.kind == TL_VEL && o.pan_dir) { cmds++; if (last && o.pan_dir != last) flips++; last = o.pan_dir; }
        }
        printf("   centre +-%d px every tick: pan commands %d, direction flips %d\n", amp, cmds, flips);
        CHECK(flips <= 2, "a wandering close box centre does not flip the pan direction every tick");
    }
}
/* S4 (Fable review 2026-11-18): tilt-back at rate_k / 6, its end (and the VSCAN's back leg) is the crossing of the target tilt with the direction latched, no restart for 10 s after a box interrupted it */
static void tback_cases(void) {
    static const struct tl_box FAR = { 149, 70, 171, 130, 0, 0 };
    printf("== tilt-back: slow, crossing-based, no restart after an interruption\n");
    { /* late ticks (0.5 s): the old window (+-40 steps) was jumped and the leg reversed */
        tl_t t; struct tl_cfg c; struct tl_ptz p = clp(1750); struct tl_out o; int64_t T = 1000; int maxdiv_seen = 99, rev = 0, last = 0, ticks = 0;
        cfg(&c, TL_MOTION_CONT); c.lead_pct = 0; tl_init(&t, &c); p.tilt_home = 1300;
        for (int i = 0; i < 8; i++, T += 200) o = tick(&t, T, FAR, p);
        for (int i = 0; i < 80; i++, T += 500) {
            o = chain_none(&t, T, p);
            if (o.kind == TL_VEL && o.tilt_dir && t.dbg.why && !strcmp(t.dbg.why, "tilt-back")) { ticks++; if (o.tilt_div < maxdiv_seen) maxdiv_seen = o.tilt_div; if (last && o.tilt_dir != last) rev++; last = o.tilt_dir; }
            head_loop(&p, &o, 0.5);
        }
        printf("   tilt-back from 450 above home with 0.5 s ticks: %d command ticks, smallest divisor %d, reversals %d, final tilt %d\n", ticks, maxdiv_seen, rev, p.tilt);
        CHECK(ticks > 0 && maxdiv_seen >= 6, "the tilt-back runs at rate_k / 6 (divisor 6), not rate_k / 3");
        CHECK(rev == 0, "a late tick that jumps the end window does not reverse the leg (the end is the crossing of home)");
    }
    for (int dt = 500; dt <= 800; dt += 300) { /* the VSCAN's back leg with late ticks: it ends on crossing the loss tilt, it never reverses */
        tl_t t; struct tl_cfg c; struct tl_ptz p = clp(1722); struct tl_out o; int64_t T = 1000; int rev = 0, last = 0, backs = 0;
        cfg(&c, TL_MOTION_CONT); c.lead_pct = 0; c.rev_guard_ms = 0; (void)tl_lost_parse("hold:3,home", &c); tl_init(&t, &c); p.tilt_home = 1300;
        cl_prime(&t, p, 2, &T);
        for (int i = 0; i < 400; i++, T += dt) {
            o = chain_none(&t, T, p);
            if (o.kind == TL_VEL && o.tilt_dir && t.dbg.why && !strcmp(t.dbg.why, "close-scan-back")) { backs++; if (last && o.tilt_dir != last) rev++; last = o.tilt_dir; }
            head_loop(&p, &o, dt / 1000.0);
        }
        printf("   VSCAN back leg with %d ms ticks: %d command ticks, reversals %d\n", dt, backs, rev);
        CHECK(backs > 0 && rev == 0, "the VSCAN back leg ends on crossing the loss tilt: no reversal with late ticks");
    }
    { /* a box interrupts the tilt-back: it does not start again for 10 s */
        tl_t t; struct tl_cfg c; struct tl_ptz p = clp(1750); struct tl_out o; int64_t T = 1000, tint = 0; int backs_in = 0, backs_after = 0;
        cfg(&c, TL_MOTION_CONT); c.lead_pct = 0; tl_init(&t, &c); p.tilt_home = 1300;
        for (int i = 0; i < 8; i++, T += 200) o = tick(&t, T, FAR, p);
        for (int i = 0; i < 60 && !t.tback_on; i++, T += 200) { o = chain_none(&t, T, p); head_loop(&p, &o, 0.2); }
        CHECK(t.tback_on, "the tilt-back starts after the loss");
        tint = T; /* the first box interrupts it */
        for (int i = 0; i < 3; i++, T += 200) { p.tilt = 1750; p.cap_tilt = 1750; o = tick(&t, T, FAR, p); } /* the person is back (the head is where it was) */
        for (int i = 0; i < 150; i++, T += 200) {
            o = chain_none(&t, T, p);
            if (t.dbg.why && !strcmp(t.dbg.why, "tilt-back")) { if (T - tint < 10000) backs_in++; else backs_after++; }
            head_loop(&p, &o, 0.2);
        }
        printf("   tilt-back ticks within 10 s of the interruption %d, after %d\n", backs_in, backs_after);
        CHECK(backs_in == 0, "no tilt-back restart for 10 s after a box interrupted it");
        CHECK(backs_after > 0, "... but it does run after that");
    }
}
/* NOTEs of the Fable review 2026-11-18: last_ff follows head-only frames; a refused home of the lost chain is asked again; the VSCAN's way back ends at a soft limit */
static void notes_cases(void) {
    printf("== review notes: head-only last_ff, home retry, VS_BACK at a soft limit\n");
    { /* a frame-filling target then held by a SMALL head alone: the stale flag is cleared (no 2.5 s bridge with no prediction, no doubled pan zone); a big head keeps it */
        tl_t t; struct tl_cfg c; struct tl_ptz p = clp(1500); struct tl_out o; int64_t T = 1000; struct tl_box small = hbox(100, 40, 130, 70), big = hbox(100, 0, 170, 80);
        cfg(&c, TL_MOTION_CONT); c.lead_pct = 0; tl_init(&t, &c); p.tilt_home = 1300;
        for (int i = 0; i < 8; i++, T += 200) o = tick(&t, T, (struct tl_box){ 1, 0, 249, 191, 0, 0 }, p);
        CHECK(t.last_ff, "frame-filling boxes: last_ff");
        for (int i = 0; i < 3; i++, T += 200) o = tickh(&t, T, NULL, &big, 1, p);
        CHECK(t.last_ff, "a big head alone (a person that fills the frame) keeps last_ff");
        for (int i = 0; i < 3; i++, T += 200) o = tickh(&t, T, NULL, &small, 1, p);
        CHECK(!t.last_ff, "a small head alone clears the stale last_ff"); (void)o;
    }
    { /* a refused home of the chain is asked again (tl_home_refused), bounded */
        tl_t t; struct tl_cfg c; struct tl_ptz p = clp(1500); struct tl_out o; int64_t T = 1000; int homes = 0, refused = 0;
        cfg(&c, TL_MOTION_CONT); c.lead_pct = 0; (void)tl_lost_parse("hold:2,home", &c); tl_init(&t, &c); p.tilt_home = 1300;
        for (int i = 0; i < 8; i++, T += 200) { struct tl_box b = { 150, 60, 172, 120, 0, 0 }; o = tick(&t, T, b, p); }
        for (int i = 0; i < 300; i++, T += 200) {
            o = chain_none(&t, T, p);
            if (o.kind == TL_HOME) { homes++; if (homes <= 2) { tl_home_refused(&t, T); refused++; } }
        }
        printf("   chain hold:2,home, the first 2 homes refused: %d TL_HOME in 60 s\n", homes);
        CHECK(homes == 3 && refused == 2, "a refused home of the lost chain is emitted again (until accepted)");
        tl_init(&t, &c); T = 1000;
        for (int i = 0; i < 8; i++, T += 200) { struct tl_box b = { 150, 60, 172, 120, 0, 0 }; o = tick(&t, T, b, p); }
        homes = 0;
        for (int i = 0; i < 600; i++, T += 200) { o = chain_none(&t, T, p); if (o.kind == TL_HOME) { homes++; tl_home_refused(&t, T); } }
        printf("   always refused: %d TL_HOME in 120 s\n", homes);
        CHECK(homes >= 2 && homes <= 6, "a home that is always refused is retried a bounded number of times");
    }
    { /* the VSCAN way back ends at a soft limit instead of dwelling until the 30 s timeout */
        tl_t t; struct tl_cfg c; struct tl_ptz p = clp(2100); struct tl_out o; int64_t T = 1000, t0; int late_dwell = 0, late_back = 0;
        cfg(&c, TL_MOTION_CONT); c.lead_pct = 0; c.rev_guard_ms = 0; (void)tl_lost_parse("hold:3,home", &c); tl_init(&t, &c); p.tilt_home = 1300;
        cl_prime(&t, p, 2, &T);
        t0 = T;
        for (int i = 0; i < 200; i++, T += 200) {
            o = chain_none(&t, T, p);
            if (T - t0 > 16000 && T - t0 < 28000) { late_dwell += t.dbg.why && !strcmp(t.dbg.why, "close-dwell"); late_back += t.dbg.why && !strcmp(t.dbg.why, "close-scan-back"); }
            head_loop(&p, &o, 0.2);
        }
        printf("   VSCAN from 2100 (limit 2100): scan ticks between 16 s and 28 s: dwell %d, back %d\n", late_dwell, late_back);
        CHECK(late_dwell == 0 && late_back == 0, "VS_BACK at a soft limit ends the scan (no dwell / turn-round loop until the timeout)");
    }
}
static void heads6_cases(void) {
    static const struct tl_box CUT = { 100, 20, 220, 191, 0, 0 };   /* a near person: tall, bottom cut (rule 2 without the head) */
    printf("== rule 6: the head as the vertical reference\n");
    { /* review 10b: a target held by a head alone that is close (head 48 px tall: about 1 m) and lost afterwards enters the close-loss machine (the person box before was far: no close evidence of its own) */
        tl_t t; struct tl_cfg c; struct tl_ptz p = clp(1300); struct tl_out o; int64_t T = 1000; struct tl_box h = hbox(136, 70, 170, 118); int nl = 0;
        cfg(&c, TL_MOTION_CONT); c.lead_pct = 0; tl_init(&t, &c);
        for (int i = 0; i < 8; i++, T += 200) { struct tl_box b = { 140, 60, 180, 150, 0, 0 }; struct tl_box hf = hbox(150, 62, 166, 78); o = tickh(&t, T, &b, &hf, 1, p); }
        CHECK(!t.last_nl, "review 10b: the person box was not close-range evidence");
        for (int i = 0; i < 6; i++, T += 200) o = tickh(&t, T, NULL, &h, 1, p); /* the head alone, close */
        CHECK(t.has_target && t.last_nl, "review 10b: a close head alone is close-range evidence (last_nl)");
        for (int i = 0; i < 10; i++, T += 200) { o = chain_none(&t, T, p); nl += t.nl_phase != 0; }
        CHECK(nl > 0, "review 10b: a close target held by a head alone and then lost enters the close-loss machine");
        (void)o;
    }
    { /* the P0-4 head-clip scenario with heads: continuous tracking, no loss machine, no search, even when the person box vanishes */
        tl_t t; struct tl_cfg c; struct tl_ptz p = clp(1300); struct tl_out o; int64_t T = 1000; struct tl_box h = hbox(140, 39, 170, 69); int searches = 0, nl = 0, lost = 0, tilt_moves = 0;
        cfg(&c, TL_MOTION_CONT); c.lead_pct = 0; tl_init(&t, &c);
        for (int i = 0; i < 8; i++, T += 200) { struct tl_box b = { 90, 0, 150, 191, 0, 0 }; h = hbox(105, 39, 135, 69); o = tickh(&t, T, &b, &h, 1, p); }
        CHECK(t.dbg.rule == 6 && t.dbg.head == 1, "a head clipped person box with its head: rule 6");
        for (int i = 0; i < 60; i++, T += 200) { /* only the head from now on */
            o = tickh(&t, T, NULL, &h, 1, p);
            nl += t.nl_phase != 0; lost += !t.has_target; searches += t.dbg.why && !strcmp(t.dbg.why, "exit-search"); tilt_moves += o.kind == TL_VEL && o.tilt_dir;
        }
        printf("   head only, 60 ticks: close-loss %d, lost %d, searches %d, tilt commands %d, why %s\n", nl, lost, searches, tilt_moves, t.dbg.why);
        CHECK(nl == 0 && lost == 0 && searches == 0 && tilt_moves == 0, "a head alone keeps the target alive: no loss, no close-loss machine, no search, no tilt (head in the band)");
        CHECK(t.dbg.rule == 6 && t.dbg.dcue && !strcmp(t.dbg.dcue, "head") && t.dist_mm > 1300 && t.dist_mm < 1800, "the distance comes from the head height (240 mm x focal / 30 px)");
    }
    { /* the low-crop scenario with a head: the head keeps the tilt where the head is, the torso crop alone would not */
        tl_t t; struct tl_cfg c; struct tl_ptz p = clp(1300); struct tl_out o; struct tl_box h = hbox(135, 20, 165, 50); int down = 0;
        cfg(&c, TL_MOTION_CONT); c.lead_pct = 0; tl_init(&t, &c);
        for (int i = 0; i < 12; i++) { struct tl_box b = { 110, 60, 210, 191, 0, 0 }; o = tickh(&t, 1000 + i * 200, &b, &h, 1, p); down += o.kind == TL_VEL && o.tilt_dir > 0; }
        CHECK(down == 0 && t.dbg.rule == 6, "a partial torso box with the head above it: no descent (the box top is ignored)");
        { tl_t u; tl_init(&u, &c); down = 0; for (int i = 0; i < 12; i++) { struct tl_box b = { 110, 60, 210, 191, 0, 0 }; o = tick(&u, 1000 + i * 200, b, p); down += o.kind == TL_VEL && o.tilt_dir > 0; }
          CHECK(down > 0, "... while the same torso box without the head does descend (the case the head rule removes)"); }
    }
    { /* head only at the bottom of the frame: the tilt goes down */
        tl_t t; struct tl_cfg c; struct tl_ptz p = clp(1300); struct tl_out o; struct tl_box h = hbox(140, 39, 170, 69); int first = -1, down = 0, lost = 0;
        cfg(&c, TL_MOTION_CONT); c.lead_pct = 0; tl_init(&t, &c);
        for (int i = 0; i < 8; i++) o = tickh(&t, 1000 + i * 200, &CUT, &h, 1, p);
        for (int i = 0; i < 12; i++) { int d = i < 6 ? 54 + 20 * (i + 1) - 54 : 120; struct tl_box hb = hbox(140, 39 + d, 170, 69 + d); o = tickh(&t, 3000 + i * 200, NULL, &hb, 1, p); if (o.kind == TL_VEL && o.tilt_dir > 0) { down++; if (first < 0) first = i; } lost += !t.has_target; }
        printf("   head only at the bottom: first tilt down at tick %d, %d down ticks\n", first, down);
        CHECK(first >= 0 && first <= 2 && down >= 6 && lost == 0, "a head alone at the bottom of the frame: tilt down, target kept");
    }
    { /* frames alternating head / no head on a cut person box: the tilt does not move more than 40 steps */
        tl_t t; struct tl_cfg c; struct tl_ptz p = clp(1300); struct tl_out o; struct tl_box h = hbox(140, 39, 170, 69); int tmin = 9999, tmax = 0;
        cfg(&c, TL_MOTION_CONT); c.lead_pct = 0; tl_init(&t, &c);
        for (int i = 0; i < 80; i++) {
            o = tickh(&t, 1000 + i * 200, &CUT, (i % 2) ? NULL : &h, (i % 2) ? 0 : 1, p);
            head_loop(&p, &o, 0.2);
            if (p.tilt < tmin) tmin = p.tilt;
            if (p.tilt > tmax) tmax = p.tilt;
        }
        printf("   alternating head / no head (80 ticks): tilt %d .. %d\n", tmin, tmax);
        CHECK(tmax - tmin <= 40, "alternating head / no-head frames: the tilt moves at most 40 steps");
        { tl_t u; struct tl_ptz q = clp(1300); int a = 9999, b2 = 0; tl_init(&u, &c);   /* two frames out of three without the head */
          for (int i = 0; i < 80; i++) { o = tickh(&u, 1000 + i * 200, &CUT, (i % 3) ? NULL : &h, (i % 3) ? 0 : 1, q); head_loop(&q, &o, 0.2); if (q.tilt < a) a = q.tilt; if (q.tilt > b2) b2 = q.tilt; }
          CHECK(b2 - a <= 40, "... also with 2 frames of 3 without a head"); }
    }
    { /* association: a head outside the person box, or far from the prediction, is not used */
        tl_t t; struct tl_cfg c; struct tl_ptz p = clp(1300); struct tl_out o; struct tl_box h = hbox(140, 39, 170, 69), far = hbox(280, 39, 310, 69), low = hbox(140, 150, 170, 190); int64_t last;
        cfg(&c, TL_MOTION_CONT); c.lead_pct = 0; tl_init(&t, &c);
        for (int i = 0; i < 8; i++) o = tickh(&t, 1000 + i * 200, &CUT, &h, 1, p);
        o = tickh(&t, 3000, &CUT, &far, 1, p);
        CHECK((t.dbg.head == 0 || t.dbg.head == 3) && t.dbg.rule != 6, "a head outside the person box (+-10 %) is not associated");
        o = tickh(&t, 3200, &CUT, &low, 1, p);
        CHECK(t.dbg.head != 1 && t.dbg.rule != 6, "a head in the lower half of the box is not associated");
        last = t.last_seen_ms;
        o = tickh(&t, 3400, NULL, &far, 1, p);
        CHECK(t.last_seen_ms == last && t.dbg.head == 0 && !t.dbg.has_box, "no person box: a head far from the predicted position (25 % of the width) does not keep the target alive");
        o = tickh(&t, 3600, NULL, &h, 1, p);
        CHECK(t.dbg.head == 1 && t.dbg.has_box && t.last_seen_ms == 3600, "... while the head at the predicted position does");
        { struct tl_box up = hbox(140, 0, 170, 30); tl_t u; tl_init(&u, &c); for (int i = 0; i < 8; i++) o = tickh(&u, 1000 + i * 200, &CUT, &h, 1, p);
          o = tickh(&u, 3000, NULL, &up, 1, p); (void)o; CHECK(u.dbg.head == 1 && u.dbg.rule == 6, "a head in the upper part of the frame keeps the target (head top clipped: tilt up)"); CHECK(o.kind == TL_VEL && o.tilt_dir < 0, "head top clipped: the tilt goes up"); }
    }
    { /* two people, two heads: only the target's head is used */
        tl_t t; struct tl_cfg c; struct tl_ptz p = clp(1300); struct tl_out o; struct tl_box A = { 120, 20, 200, 191, 0, 0 }, B = { 230, 20, 310, 191, 0, 0 }, ha = hbox(145, 39, 175, 69), hb = hbox(255, 39, 285, 69), hs[2];
        cfg(&c, TL_MOTION_CONT); c.lead_pct = 0; tl_init(&t, &c);
        for (int i = 0; i < 8; i++) o = tickh(&t, 1000 + i * 200, &A, &ha, 1, p);
        { struct tl_box in[4]; A.cls = B.cls = TL_CLS_PERSON; in[0] = B; in[1] = A; in[2] = hb; in[3] = ha; p.cap_ms = 2900; tl_update(&t, 3000, in, 4, FW, FH, &p, &o); }
        CHECK(t.dbg.head == 1 && t.dbg.box.x1 == 145 && t.dbg.rule == 6, "two people with two heads: the target's head is the one used");
        hs[0] = hb; (void)hs;
        { struct tl_box in[3]; in[0] = B; in[1] = A; in[2] = hb; p.cap_ms = 3100; tl_update(&t, 3200, in, 3, FW, FH, &p, &o); }
        CHECK(t.dbg.head != 1 && t.dbg.rule != 6, "the other person's head alone is not used for the target");
    }
    { /* whole body: the body rule keeps the centre (the head is not used, rule 4) */
        tl_t t; struct tl_cfg c; struct tl_ptz p = clp(1300); struct tl_out o; struct tl_box b = { 140, 60, 180, 160, 0, 0 }, h = hbox(150, 62, 170, 80);
        cfg(&c, TL_MOTION_CONT); tl_init(&t, &c);
        for (int i = 0; i < 6; i++) o = tickh(&t, 1000 + i * 200, &b, &h, 1, p);
        (void)o;
        CHECK(t.dbg.rule == 4 && t.dbg.head == 2, "a whole body with its head: the body rule (rule 4), the head only associated");
    }
}

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


/* ---------------------------------------------------------------- the close person of 2026-11-18 (night IR): frame-filling boxes from the hardware log, the detector reporting the person for
 * 25-27 s (but boxes on only 18 % / 61 % of the inferences), the tracker announced "lost target, step close" a dozen times and the head went down ~260 steps below home. */
typedef struct { int announces, outliers, maxt, mint, tdown_cmds, ticks_box; } cl_res_t;
/* boxes: the logged sequence, one per second; on_ms / off_ms: how long the detector delivers boxes / nothing (off_ms 0 = every tick); the tilt starts at t0 */
static cl_res_t closehold_run(int t0, int on_ms, int off_ms, int total_ms, int win) {
    static const struct tl_box seq[5] = { { 21, 0, 218, 191, 0, 0 }, { 30, 0, 206, 191, 0, 0 }, { 12, 9, 260, 191, 0, 0 }, { 55, 0, 319, 191, 0, 0 }, { 179, 17, 316, 191, 0, 0 } };
    tl_t t; struct tl_cfg c; struct tl_ptz p = clp(t0); struct tl_out o; int64_t T = 1000, s = T; cl_res_t r; char last[16] = "";
    memset(&r, 0, sizeof r); r.maxt = r.mint = t0;
    cfg(&c, TL_MOTION_CONT); c.lead_pct = 0; c.return_home = 1; (void)tl_lost_parse("hold:3,home", &c);
    tl_init(&t, &c); p.tilt_home = 1300;
    for (; T - s < total_ms; T += 200) {
        int64_t ph = off_ms ? (T - s) % (on_ms + off_ms) : 0;
        const char *w;
        if (!off_ms || ph < on_ms) { o = tick(&t, T, seq[((T - s) / 1000 + win) % 5], p); r.ticks_box++; }
        else o = chain_none(&t, T, p);
        w = t.lstep ? t.lstep : "idle"; if (getenv("CLDBG")) printf("T%lld box%d why=%s kf=%d lstep=%s tilt=%d\n", (long long)(T-s), (!off_ms || ph < on_ms), t.dbg.why, t.dbg.kf, w, p.tilt);
        if (strcmp(w, last)) { if (strcmp(w, "idle")) r.announces++; snprintf(last, sizeof last, "%s", w); }
        r.outliers += t.dbg.why && !strcmp(t.dbg.why, "outlier");
        r.tdown_cmds += o.kind == TL_VEL && o.tilt_dir && (c.invert_tilt ? o.tilt_dir < 0 : o.tilt_dir > 0);
        head_loop(&p, &o, 0.2);
        if (p.tilt > r.maxt) r.maxt = p.tilt;
        if (p.tilt < r.mint) r.mint = p.tilt;
    }
    return r;
}
static void closehold_cases(void) {
    cl_res_t r;
    printf("== close person (night IR 2026-11-18): frame-filling boxes keep the target, no lost, no tilt down\n");
    r = closehold_run(1500, 1000, 0, 25000, 0);
    printf("   a box every tick, jumping between the logged boxes: announces %d, outliers %d, tilt %d..%d (down cmds %d)\n", r.announces, r.outliers, r.mint, r.maxt, r.tdown_cmds);
    CHECK(r.announces == 0 && r.outliers == 0, "boxes every tick: no loss step, no outlier-held box");
    CHECK(r.mint >= 1500 - 30, "boxes every tick: the tilt does not go down");
    r = closehold_run(1300, 1000, 0, 25000, 2);
    CHECK(r.announces == 0 && r.mint >= 1300 - 30 && r.maxt <= 1300 + 150, "from home: no loss step, tilt within +150 / -30");
    r = closehold_run(1500, 400, 1200, 27000, 0);
    printf("   detector hits 0.4 s every 1.6 s (25 %%): announces %d, tilt %d..%d\n", r.announces, r.mint, r.maxt);
    CHECK(r.announces == 0, "detector dropouts shorter than the bridge (3 s, the presence OFF delay) are no loss for a frame-filling person");
    CHECK(r.mint >= 1500 - 30 && r.maxt <= 1500 + 150, "... and the tilt stays within the bounded hold");
    r = closehold_run(1500, 800, 2400, 27000, 1);
    printf("   hits 0.8 s every 3.2 s: announces %d, tilt %d..%d\n", r.announces, r.mint, r.maxt);
    CHECK(r.mint >= 1500 - 30 && r.maxt <= 1500 + 150 && r.announces == 0, "gaps of 2.4 s: no loss, the tilt never goes down and stays within the 150-step cap");
    r = closehold_run(1500, 800, 6000, 40000, 1);
    printf("   hits 0.8 s every 6.8 s (gaps beyond the bridge): announces %d, tilt %d..%d\n", r.announces, r.mint, r.maxt);
}
int main(void) {
    zone_cases();
    lag_cases();
    heads_cases();
    heads6_cases();
    framefill_cases();
    ff_loss_cases();
    ff_ratchet_cases();
    ff_rise_cases();
    sit_low_cases();
    sit_lost_cases();
    pan_only_cases();
    rev_fast_cases();
    rev_cases();
    close_same_cases();
    kf_force_cases();
    tback_cases();
    notes_cases();
    closehold_cases();
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
    close_scan_cases();
    flap_cases();
    episode_cases();
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
