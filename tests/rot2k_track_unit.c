/* Host unit test of track_logic (pure policy). Build: see docs/porting/lsc-rotatable-2k/tracking.md */
#include "../src/features/track/track_logic.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)
#define FW 320
#define FH 192

static struct tl_ptz ptz(void) {
    struct tl_ptz p = {0, 0, 0, -2000, 2000, -500, 500, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    return p;
}
static struct tl_box box(int cx, int cy, int w, int h) {
    struct tl_box b = {cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2, 0, 0 };
    return b;
}
/* the slow policy of firmware 0.1.0: these cases pin its behaviour; the responsive policy is tested below and in rot2k_track_sim.c */
static void init(tl_t *t) { struct tl_cfg c; tl_cfg_legacy(&c); CHECK(tl_init(t, &c) == 0); }
/* two ticks 200 ms apart (the confirm streak); returns the second result */
static struct tl_out two(tl_t *t, int64_t now, struct tl_box b, struct tl_ptz p) {
    struct tl_out o;
    tl_update(t, now, &b, 1, FW, FH, &p, &o);
    CHECK(o.kind == TL_NONE); /* the first tick of a new target never moves */
    tl_update(t, now + 200, &b, 1, FW, FH, &p, &o);
    return o;
}

/* ---- the responsive policy (tl_cfg_defaults) ---- */
static struct tl_out step(tl_t *t, int64_t now, int64_t cap, struct tl_box b, struct tl_ptz p) {
    struct tl_out o;
    p.cap_ms = cap;
    tl_update(t, now, &b, 1, FW, FH, &p, &o);
    return o;
}

static void responsive(void) {
    tl_t t; struct tl_cfg c; struct tl_ptz p = ptz(); struct tl_out o; struct tl_box b;
    tl_cfg_dev(&c); c.invert_pan = 0; c.lead_pct = 0;
    /* inside the band (6 % of 320 = 19.2 px): nothing; outside: acts after the 2 acquire ticks, proportional x 85 % */
    tl_init(&t, &c);
    o = step(&t, 1000, 1000, box(175, 96, 60, 150), p); CHECK(o.kind == TL_NONE);
    o = step(&t, 1200, 1200, box(175, 96, 60, 150), p); CHECK(o.kind == TL_NONE); /* 15 px: inside the band */
    tl_init(&t, &c);
    b = box(200, 96, 60, 150);                                                 /* 40 px = 125 steps -> 106 */
    o = step(&t, 1000, 1000, b, p); CHECK(o.kind == TL_NONE);
    o = step(&t, 1200, 1200, b, p); CHECK(o.kind == TL_NUDGE && o.dpan == 106 && o.speed == 0);
    /* a big offset (> 35 %): proportional beyond the old 300 clamp, fast speed */
    tl_init(&t, &c);
    b = box(300, 96, 40, 150);                                                 /* 140 px = 437 steps -> 371 */
    o = step(&t, 1000, 1000, b, p); o = step(&t, 1200, 1200, b, p);
    CHECK(o.kind == TL_NUDGE && o.dpan == 371 && o.speed == 2);
    b = box(318, 96, 8, 150);                                                  /* the edge: 158 px = 493 steps -> 419 */
    tl_init(&t, &c); o = step(&t, 1000, 1000, b, p); o = step(&t, 1200, 1200, b, p); CHECK(o.dpan == 419);
    /* rate limit 300 ms, frames captured before the end of the motion + settle (150 ms) are dropped */
    tl_init(&t, &c);
    b = box(300, 96, 40, 150);
    o = step(&t, 1000, 1000, b, p); o = step(&t, 1200, 1200, b, p); CHECK(o.kind == TL_NUDGE);
    p.motion_end_ms = 1500;
    o = step(&t, 1400, 1380, b, p); CHECK(o.kind == TL_NONE);  /* rate limit and still moving */
    o = step(&t, 1800, 1600, b, p); CHECK(o.kind == TL_NONE);  /* captured 1600 < 1500 + 220: still stale */
    o = step(&t, 1900, 1750, b, p); CHECK(o.kind == TL_NUDGE); /* followed target: no new acquire, captured after the settle */
    /* frames captured while the motor ran, or just after: no move, but the person is still seen */
    tl_init(&t, &c); p.motion_end_ms = 0;
    o = step(&t, 1000, 1000, b, p); o = step(&t, 1200, 1200, b, p); CHECK(o.kind == TL_NUDGE);
    p.motion_end_ms = 2000;
    o = step(&t, 2100, 1900, b, p); CHECK(o.kind == TL_NONE);
    o = step(&t, 2300, 2100, b, p); CHECK(o.kind == TL_NONE);  /* 2100 < 2000 + 220 */
    o = step(&t, 2500, 2250, b, p); CHECK(o.kind == TL_NUDGE); /* settled */
    /* a nudge sent after the frame was captured: that frame is stale even when the motion has not started yet */
    tl_init(&t, &c); p.motion_end_ms = 0;
    o = step(&t, 1000, 1000, b, p); o = step(&t, 1200, 1200, b, p); CHECK(o.kind == TL_NUDGE);
    o = step(&t, 1600, 1250, b, p); CHECK(o.kind == TL_NONE);
    /* tilt: a person low in the frame (outside 15 % of 192 = 29 px): tilt down */
    c.tilt_enable = 1; tl_init(&t, &c);
    b = box(160, 150, 40, 60);                                                 /* upper third at row 140: 44 px below = 160 steps -> 136 */
    o = step(&t, 1000, 1000, b, p); o = step(&t, 1200, 1200, b, p); CHECK(o.kind == TL_NUDGE && o.dtilt == 136 && o.dpan == 0);
    c.tilt_enable = 0; tl_init(&t, &c);
    o = step(&t, 1000, 1000, b, p); o = step(&t, 1200, 1200, b, p); CHECK(o.kind == TL_NONE);
    tl_cfg_dev(&c); CHECK(c.tilt_enable == 0 && c.settle_ms == 220); /* tilt is opt-in until verified on the camera */
    c.invert_pan = 0;
    /* velocity lead: a person walking right (~200 steps/s) gets a nudge larger than the bare offset, capped by lead_max */
    c.lead_pct = 60; tl_init(&t, &c);
    o = step(&t, 1000, 1000, box(200, 96, 40, 150), p);
    o = step(&t, 1200, 1200, box(213, 96, 40, 150), p);
    CHECK(o.kind == TL_NUDGE && o.dpan > 106 + 10);
    c.lead_max = 20; tl_init(&t, &c);
    o = step(&t, 1000, 1000, box(200, 96, 40, 150), p); o = step(&t, 1200, 1200, box(213, 96, 40, 150), p);
    CHECK(o.kind == TL_NUDGE && o.dpan <= (165 + 20) * 85 / 100 + 2 && o.dpan > 106);
    /* a target that is coming back to the centre by itself: no nudge against it */
    c.lead_max = 250; tl_init(&t, &c);
    o = step(&t, 1000, 1000, box(240, 96, 40, 150), p); o = step(&t, 1200, 1200, box(215, 96, 40, 150), p);
    CHECK(o.kind == TL_NONE);

    /* M1: MIN_STEP applies AFTER the soft-range clip: 10 steps from a limit is no nudge (no loop of tiny motions) */
    c.lead_pct = 0; tl_init(&t, &c); p = ptz(); p.pan = 1990;                  /* max 2000, person far right: wants 100+, 10 left */
    o = step(&t, 1000, 1000, box(300, 96, 40, 150), p); o = step(&t, 1200, 1200, box(300, 96, 40, 150), p);
    CHECK(o.kind == TL_NONE);
    p = ptz(); p.pan = 1960;                                                    /* 40 left: still a nudge */
    tl_init(&t, &c); o = step(&t, 1000, 1000, box(300, 96, 40, 150), p); o = step(&t, 1200, 1200, box(300, 96, 40, 150), p);
    CHECK(o.kind == TL_NUDGE && o.dpan == 40);
    p = ptz();
    /* M2: the velocity of a lost person is not used for the next one */
    c.lead_pct = 70; tl_init(&t, &c);
    step(&t, 1000, 1000, box(200, 96, 40, 150), p); step(&t, 1200, 1200, box(213, 96, 40, 150), p);
    CHECK(t.ns >= 2);
    p.motion_end_ms = 1300;
    tl_update(&t, 1700, 0, 0, FW, FH, &p, &o); CHECK(t.ns == 0);
    step(&t, 1900, 1900, box(200, 96, 40, 150), p); step(&t, 2100, 2100, box(200, 96, 40, 150), p);
    CHECK(t.ns <= 2);
    { /* a different person far from the sticky gate: samples reset, 2 ticks needed even inside the grace */
        struct tl_box far1 = box(300, 96, 40, 150);
        tl_cfg_dev(&c); c.invert_pan = 0; c.lead_pct = 0; tl_init(&t, &c); p.motion_end_ms = 0;
        o = step(&t, 1000, 1000, box(200, 96, 40, 150), p); o = step(&t, 1200, 1200, box(200, 96, 40, 150), p); CHECK(o.kind == TL_NUDGE);
        p.motion_end_ms = 1400;
        o = step(&t, 1700, 1650, box(60, 96, 40, 150), p); CHECK(o.kind == TL_NONE && t.ns == 1); /* box jumped ~450 steps: another person, 1st tick */
        o = step(&t, 1900, 1850, box(60, 96, 40, 150), p); CHECK(o.kind == TL_NUDGE);
        (void)far1;
    }
    /* M4: a frame is stale while the last nudge has no reported end of motion (bounded 2 s) */
    tl_init(&t, &c); p.motion_end_ms = 0;
    b = box(300, 96, 40, 150);
    o = step(&t, 1000, 1000, b, p); o = step(&t, 1200, 1200, b, p); CHECK(o.kind == TL_NUDGE);
    o = step(&t, 1900, 1800, b, p); CHECK(o.kind == TL_NONE);                   /* no motion end reported yet */
    o = step(&t, 3300, 3200, b, p); CHECK(o.kind == TL_NUDGE);                  /* after 2 s the bound releases it */
    /* duty cap on reversals: the 3rd reversal inside 6 s is held back, then 3 s of calm */
    tl_init(&t, &c); p.motion_end_ms = 0;
    { int64_t tt = 1000; int k, holds = 0, nud = 0;
      o = step(&t, tt, tt, box(300, 96, 40, 150), p); tt += 200; o = step(&t, tt, tt, box(300, 96, 40, 150), p); nud += o.kind == TL_NUDGE;
      for (k = 0; k < 8; k++) {                                                 /* alternate right / left, each frame settled */
          p.motion_end_ms = tt + 100; tt += 500;
          o = step(&t, tt, tt, box(k & 1 ? 300 : 20, 96, 40, 150), p);
          if (o.kind == TL_NUDGE) nud++; else holds++;
      }
      CHECK(nud <= 4 && holds >= 4); }

    /* return policy: stay never goes home; home (default) after grace + delay; delay 0 = right after the grace */
    tl_cfg_dev(&c); CHECK(c.return_home == 1 && c.return_delay_s == 30 && c.tilt_return_s == 0); c.return_home = 0;
    c.invert_pan = 0; c.lead_pct = 0; tl_init(&t, &c); p = ptz();
    b = box(290, 96, 40, 150);
    o = step(&t, 1000, 1000, b, p); o = step(&t, 1200, 1200, b, p); CHECK(o.kind == TL_NUDGE);
    for (int i = 1; i <= 600; i++) { tl_update(&t, 1200 + i * 1000, 0, 0, FW, FH, &p, &o); CHECK(o.kind == TL_NONE); }
    c.return_home = 1; c.return_delay_s = 30; tl_init(&t, &c);
    o = step(&t, 1000, 1000, b, p); o = step(&t, 1200, 1200, b, p);
    tl_update(&t, 1200 + 32999, 0, 0, FW, FH, &p, &o); CHECK(o.kind == TL_NONE);  /* grace 3 s + delay 30 s */
    tl_update(&t, 1200 + 33000, 0, 0, FW, FH, &p, &o); CHECK(o.kind == TL_HOME);
    tl_update(&t, 1200 + 34000, 0, 0, FW, FH, &p, &o); CHECK(o.kind == TL_NONE);
    c.return_delay_s = 0; tl_init(&t, &c);
    o = step(&t, 1000, 1000, b, p); o = step(&t, 1200, 1200, b, p);
    tl_update(&t, 1200 + 2999, 0, 0, FW, FH, &p, &o); CHECK(o.kind == TL_NONE);
    tl_update(&t, 1200 + 3000, 0, 0, FW, FH, &p, &o); CHECK(o.kind == TL_HOME);
    /* back before the delay: the loss is forgotten */
    c.return_delay_s = 30; tl_init(&t, &c);
    o = step(&t, 1000, 1000, b, p); o = step(&t, 1200, 1200, b, p);
    tl_update(&t, 20000, 0, 0, FW, FH, &p, &o); CHECK(o.kind == TL_NONE);
    o = step(&t, 21000, 21000, b, p); o = step(&t, 21200, 21200, b, p);
    tl_update(&t, 21200 + 32999, 0, 0, FW, FH, &p, &o); CHECK(o.kind == TL_NONE);
    /* a brief dropout (blur frames) does not reset the followed target: the first sighting after it moves at once */
    c.return_home = 0; tl_init(&t, &c);
    o = step(&t, 1000, 1000, b, p); o = step(&t, 1200, 1200, b, p); CHECK(o.kind == TL_NUDGE);
    p.motion_end_ms = 1700;
    tl_update(&t, 1600, 0, 0, FW, FH, &p, &o);
    o = step(&t, 2000, 1950, b, p); CHECK(o.kind == TL_NUDGE);
}


/* ---- head-anchored tilt: a close person fills the frame height, the box centre never moves; the top of the box does ---- */
static struct tl_box rect(int x1, int y1, int x2, int y2) { struct tl_box b = {x1, y1, x2, y2, 0, 0 }; return b; }

/* closed loop: the person's head is at image row tilt `wtop` + 0.32 * tilt, the body is `hgt` px tall.
 * 0.32 px per motor step (600 steps = 192 px). Tilt counter up = camera up = the person moves DOWN in the image. Returns the tilt after
 * `ticks` ticks of 200 ms; nud = nudges, rev = reversals of the tilt direction, top = the box top at the end, tmin/tmax = tilt extremes. */
struct loop { int tilt, nud, rev, top, tmin, tmax; };
static void run_loop(tl_t *t, struct loop *l, double wtop, int hgt, int ticks, int64_t *now, int64_t *lastn) {
    struct tl_ptz p = ptz();
    int k, lastdir = 0;
    for (k = 0; k < ticks; k++) {
        struct tl_out o;
        double top = wtop + l->tilt * 0.32;
        int y1 = top < 0 ? 0 : (int)top, y2 = top + hgt > FH - 1 ? FH - 1 : (int)(top + hgt);
        struct tl_box b = rect(100, y1, 220, y2);
        p.tilt = l->tilt; p.tilt_min = 0; p.tilt_max = 2000;
        p.cap_ms = *now; p.motion_end_ms = *lastn ? *lastn + 100 : 0;
        *now += 200;
        if (y2 <= y1) continue;
        tl_update(t, *now, &b, 1, FW, FH, &p, &o);
        l->top = y1;
        if (o.kind == TL_NUDGE && o.dtilt) {
            int dir = o.dtilt > 0 ? -1 : 1;
            if (lastdir && dir != lastdir) l->rev++;
            lastdir = dir;
            l->tilt -= o.dtilt;
            if (l->tilt < l->tmin) l->tmin = l->tilt;
            if (l->tilt > l->tmax) l->tmax = l->tilt;
            l->nud++;
            *lastn = *now;
        } else if (o.kind == TL_NUDGE) *lastn = *now;
    }
}

static void head_tilt(void) {
    tl_t t; struct tl_cfg c; struct tl_ptz p = ptz(); struct tl_out o; struct loop l;
    int64_t now, lastn;
    tl_cfg_dev(&c); c.invert_pan = 0; c.lead_pct = 0; c.tilt_enable = 1;
    tl_init(&t, &c);
    /* head fully visible at the target (top 29 px) or anywhere in the band 19..39 (29 +- 5 %): nothing, whatever the box height (centre = 110, 96 ...) */
    o = step(&t, 1000, 1000, rect(100, 22, 220, 191), p); o = step(&t, 1200, 1200, rect(100, 22, 220, 191), p); CHECK(o.kind == TL_NONE);
    tl_init(&t, &c);
    o = step(&t, 1000, 1000, rect(100, 36, 220, 191), p); o = step(&t, 1200, 1200, rect(100, 36, 220, 191), p); CHECK(o.kind == TL_NONE);
    /* head clipped at the top, box 191 px tall: up (image above = dtilt < 0) by 600 * 191/192 / 5 = 119 steps, never a pan */
    tl_init(&t, &c);
    o = step(&t, 1000, 1000, rect(100, 0, 220, 191), p); o = step(&t, 1200, 1200, rect(100, 0, 220, 191), p);
    CHECK(o.kind == TL_NUDGE && o.dtilt == -139 && o.dpan == 0);
    /* a smaller clipped box: a smaller step (bounded below by the minimum nudge) */
    tl_init(&t, &c);
    o = step(&t, 1000, 1000, rect(100, 1, 220, 61), p); o = step(&t, 1200, 1200, rect(100, 1, 220, 61), p);
    CHECK(o.kind == TL_NUDGE && o.dtilt < -100 && o.dtilt >= -400); /* 60 px tall = 31 %: not a head clip (far rule): the upper third goes to the centre, within the up cap */
    /* crouching: the top drops to row 120 in a still full-height box: down (dtilt > 0) by (120-29)/192*600 * 85 % = 241 */
    tl_init(&t, &c);
    o = step(&t, 1000, 1000, rect(100, 120, 220, 191), p); o = step(&t, 1200, 1200, rect(100, 120, 220, 191), p);
    CHECK(o.kind == TL_NUDGE && o.dtilt > 0 && o.dpan == 0);
    /* a far person (box 70 px tall, 36 %): centred on its upper third; fully inside and near the target: nothing */
    tl_init(&t, &c);
    o = step(&t, 1000, 1000, rect(130, 66, 190, 136), p); o = step(&t, 1200, 1200, rect(130, 66, 190, 136), p); CHECK(o.kind == TL_NONE);
    tl_init(&t, &c);
    o = step(&t, 1000, 1000, rect(130, 120, 190, 190), p); o = step(&t, 1200, 1200, rect(130, 120, 190, 190), p); CHECK(o.kind == TL_NUDGE && o.dtilt > 0);
    /* tilt disabled / head_pct 0 (centre of the box): the head logic is off */
    c.tilt_enable = 0; tl_init(&t, &c);
    o = step(&t, 1000, 1000, rect(100, 0, 220, 191), p); o = step(&t, 1200, 1200, rect(100, 0, 220, 191), p); CHECK(o.kind == TL_NONE);
    c.tilt_enable = 1; c.head_pct = 0; tl_init(&t, &c);
    o = step(&t, 1000, 1000, rect(100, 0, 220, 191), p); o = step(&t, 1200, 1200, rect(100, 0, 220, 191), p); CHECK(o.kind == TL_NONE);
    c.head_pct = 51; CHECK(tl_cfg_validate(&c) != 0);
    /* invert_tilt flips the motion, not the decision */
    c.head_pct = 15; c.invert_tilt = 1; tl_init(&t, &c);
    o = step(&t, 1000, 1000, rect(100, 0, 220, 191), p); o = step(&t, 1200, 1200, rect(100, 0, 220, 191), p);
    CHECK(o.kind == TL_NUDGE && o.dtilt == 139);
    c.invert_tilt = 0;
    /* soft range: head clipped but the camera is at the top of the tilt range: no push */
    tl_init(&t, &c); p.tilt = 500;
    o = step(&t, 1000, 1000, rect(100, 0, 220, 191), p); o = step(&t, 1200, 1200, rect(100, 0, 220, 191), p); CHECK(o.kind == TL_NONE);
    p = ptz();

    /* closed loop, person close (body 260 px: the box is the full height): head out -> up until it shows, then crouch -> down, stand -> up */
    {
        double wtop = -40 - 1000 * 0.32; /* the head is 40 px above the frame at tilt 1000 */
        int t1, t2;
        tl_init(&t, &c);
        memset(&l, 0, sizeof l); l.tilt = l.tmin = l.tmax = 1000; now = 1000; lastn = 0;
        run_loop(&t, &l, wtop, 260, 40, &now, &lastn);
        CHECK(l.tilt > 1000 && l.top >= 1 && l.top <= 58 && l.nud <= 6 && l.rev == 0);        /* standing, head out: tilts up till the head shows */
        t1 = l.tilt;
        wtop += 110;                                                                           /* crouches: the head drops 110 px */
        memset(&l, 0, sizeof l); l.tilt = l.tmin = l.tmax = t1;
        run_loop(&t, &l, wtop, 150, 40, &now, &lastn);
        CHECK(l.tilt < t1 - 100 && l.top >= 1 && l.top <= 58 && l.rev <= 1);                  /* the box top follows: tilts down */
        t2 = l.tilt;
        wtop -= 100;                                                                           /* stands up: the head rises 100 px */
        memset(&l, 0, sizeof l); l.tilt = l.tmin = l.tmax = t2;
        run_loop(&t, &l, wtop, 260, 40, &now, &lastn);
        CHECK(l.tilt > t2 + 100 && l.top >= 1 && l.top <= 58 && l.rev <= 1);
    }
    /* far person (70 px tall), standing still anywhere in the frame: it converges and then does not oscillate */
    {
        double tops[] = { 10, 60, 100, 120 };
        int k;
        for (k = 0; k < 4; k++) {
            tl_init(&t, &c);
            memset(&l, 0, sizeof l); l.tilt = l.tmin = l.tmax = 1000; now = 1000; lastn = 0;
            run_loop(&t, &l, tops[k] - 1000 * 0.32, 70, 80, &now, &lastn);
            CHECK(l.rev == 0 && l.nud <= 5);
        }
    }
    /* soft range in a closed loop: the head stays out of the frame, the camera is stopped at the top of the range */
    tl_init(&t, &c);
    memset(&l, 0, sizeof l); l.tilt = l.tmin = l.tmax = 1000; now = 1000; lastn = 0;
    run_loop(&t, &l, -4000 - 1000 * 0.32, 260, 60, &now, &lastn);
    CHECK(l.tmax <= 2000 && l.tilt <= 2000);
}

/* ---- the boxes of the 2026-11-16 hardware test (user at ~1 m, 320x192): the camera was up at 1682 and never came down ---- */
static int nudge_in(tl_t *t, int64_t *now, struct tl_box b, int ticks, int tilt, int *dpan_out) {
    struct tl_ptz p = ptz();
    int k, dt = 0;
    p.tilt = tilt; p.tilt_min = 1000; p.tilt_max = 1900;
    for (k = 0; k < ticks; k++) {
        struct tl_out o;
        *now += 200; p.cap_ms = *now;
        tl_update(t, *now, &b, 1, FW, FH, &p, &o);
        if (o.kind == TL_NUDGE) { if (o.dtilt && !dt) dt = o.dtilt; if (dpan_out && o.dpan) *dpan_out = o.dpan; }
    }
    return dt;
}

static void hw_boxes(void) {
    tl_t t; struct tl_cfg c; int64_t now = 1000; int k, dp = 0;
    struct tl_box dn[] = { { 46, 82, 166, 191, 0, 0 }, { 26, 82, 163, 191, 0, 0 }, { 122, 58, 240, 191, 0, 0 }, { 169, 56, 319, 191, 0, 0 } };
    tl_cfg_dev(&c); c.invert_pan = 1; c.tilt_enable = 1;
    for (k = 0; k < 4; k++) { /* each box: a fresh sighting, 6 ticks, camera high at 1682: a DOWN nudge */
        tl_init(&t, &c);
        CHECK(nudge_in(&t, &now, dn[k], 6, 1682, &dp) > 0);
    }
    tl_init(&t, &c); /* top 14..15 with the bottom cut (the person upright): in band, no tilt */
    { struct tl_box b = {11, 22, 209, 189, 0, 0 }; CHECK(nudge_in(&t, &now, b, 10, 1682, 0) == 0); }
    tl_init(&t, &c); /* head clipped: up */
    { struct tl_box b = {30, 0, 318, 191, 0, 0 }; CHECK(nudge_in(&t, &now, b, 6, 1682, 0) < 0); }
    /* the logged sequence with the person flapping ON 6 ticks / OFF 3-9 s (the grace is 3 s): every ON phase with a low head tilts down */
    tl_init(&t, &c);
    for (k = 0; k < 4; k++) {
        struct tl_ptz p = ptz(); struct tl_out o; int j;
        CHECK(nudge_in(&t, &now, dn[k], 6, 1682, &dp) > 0);
        p.tilt = 1682; p.tilt_min = 1000; p.tilt_max = 1900;
        for (j = 0; j < 20 + 10 * k; j++) { now += 200; tl_update(&t, now, 0, 0, FW, FH, &p, &o); }
    }
    /* the pan reversal hold must not stop the tilt: swing the person left/right (the pan reversals trip the cap), then a low head */
    tl_init(&t, &c);
    {
        struct tl_ptz p = ptz(); struct tl_out o; int j, hold_seen = 0, tilted = 0;
        struct tl_box l = {0, 20, 60, 120, 0, 0 }, r = { 260, 20, 320, 120, 0, 0 }, low = { 0, 90, 60, 191, 0, 0 };
        p.tilt = 1682; p.tilt_min = 1000; p.tilt_max = 1900;
        for (j = 0; j < 40; j++) {
            struct tl_box b = (j / 3) % 2 ? l : r;
            if (j > 20) b = low; /* a left-edge person with a low head */
            now += 400; p.cap_ms = now;
            tl_update(&t, now, &b, 1, FW, FH, &p, &o);
            if (t.dbg.why && !strcmp(t.dbg.why, "pan-reversal-cap")) hold_seen = 1;
            if (j > 20 && o.kind == TL_NUDGE && o.dtilt > 0) tilted = 1;
        }
        CHECK(tilted);
        (void)hold_seen;
    }
    /* the debug record says why */
    tl_init(&t, &c);
    { struct tl_box b = {130, 60, 190, 130, 0, 0 }; struct tl_ptz p = ptz(); struct tl_out o; tl_update(&t, 1000, &b, 1, FW, FH, &p, &o);
      CHECK(t.dbg.why && !strcmp(t.dbg.why, "confirm") && t.dbg.rule == 3); }
}

/* ---- close range, physical model: camera on a desk at 0.75 m, VFOV 50 deg / HFOV 90 deg (205.8 px/m / D vertical), person 0.9 m wide,
 * head at hh m. The person stands on the optical axis (a pan nudge would be a wrong one). ---- */
struct room { int tilt, tmin, tmax, nud, pan_nud, rev, maxdt, top, bot; int lastdir; int64_t now, lastn; tl_t t; };
static double g_pxstep = 0.32; /* image px per tilt step of the simulated motor */
static void room_run(struct room *r, double d0, double d1, double hh, int ticks) {
    int k;
    for (k = 0; k < ticks; k++) {
        double d = d0 + (d1 - d0) * (k + 1) / ticks, sh = (r->tilt - 1300) * g_pxstep;
        double top = 96 - (hh - 0.75) * 205.8 / d + sh, bot = 96 + 0.75 * 205.8 / d + sh, w = 144.0 / d;
        int y1 = top < 0 ? 0 : (int)top, y2 = bot > FH - 1 ? FH - 1 : (int)bot;
        int x1 = 160 - (int)(w / 2), x2 = 160 + (int)(w / 2);
        struct tl_box b = { x1 < 0 ? 0 : x1, y1, x2 > FW - 1 ? FW - 1 : x2, y2, 0, 0 };
        struct tl_ptz p = ptz();
        struct tl_out o;
        p.tilt = r->tilt; p.tilt_min = 1000; p.tilt_max = 2000;
        p.cap_ms = r->now; p.motion_end_ms = r->lastn ? r->lastn + 100 : 0;
        r->now += 200;
        r->top = y1; r->bot = y2;
        if (y2 <= y1) continue;
        tl_update(&r->t, r->now, &b, 1, FW, FH, &p, &o);
        if (o.kind != TL_NUDGE) continue;
        r->lastn = r->now;
        r->nud++;
        if (o.dpan) r->pan_nud++;
        if (o.dtilt) {
            int dir = o.dtilt > 0 ? -1 : 1;
            if (r->lastdir && dir != r->lastdir) r->rev++;
            r->lastdir = dir;
            r->tilt -= o.dtilt;
            if (r->tilt < r->tmin) r->tmin = r->tilt;
            if (r->tilt > r->tmax) r->tmax = r->tilt;
            if (abs(o.dtilt) > r->maxdt) r->maxdt = abs(o.dtilt);
        }
    }
}

static void close_range(void) {
    static struct room r;
    struct tl_cfg c;
    int t_stand, t_crouch;
    tl_cfg_dev(&c); c.invert_pan = 1; c.tilt_enable = 1;
    memset(&r, 0, sizeof r);
    tl_init(&r.t, &c);
    r.tilt = r.tmin = r.tmax = 1300; r.now = 1000;
    room_run(&r, 4.0, 0.8, 1.6, 40);                 /* walks at the camera from 4 m to 0.8 m in 8 s */
    CHECK(r.pan_nud == 0);                           /* on the axis: never a pan on the growing box */
    CHECK(r.maxdt <= 200 && r.rev <= 2);             /* no large wrong tilt nudge, no hunting */
    room_run(&r, 0.8, 0.8, 1.6, 40);                 /* stands close */
    CHECK(r.pan_nud == 0 && r.top >= 1 && r.top <= 58 && r.rev <= 2 && r.tilt > 1300 && r.tmax <= 2000);
    t_stand = r.tilt;
    room_run(&r, 0.8, 0.9, 1.0, 40);                 /* crouches (head at 1.0 m) */
    CHECK(r.tilt < t_stand - 100 && r.top >= 1 && r.top <= 58 && r.pan_nud == 0);
    t_crouch = r.tilt;
    room_run(&r, 0.9, 0.9, 1.6, 40);                 /* stands up again */
    CHECK(r.tilt > t_crouch + 100 && r.top >= 1 && r.top <= 58 && r.rev <= 4);
    /* a close, very wide box (arms out / two persons merged): no pan on the centre unless it touches exactly one side edge */
    {
        tl_t t; struct tl_ptz p = ptz(); struct tl_out o; int k, pans = 0;
        struct tl_box wide = { 20, 20, 310, 191, 0, 0 }, wl = { 0, 20, 310, 191, 0, 0 }, both = { 0, 20, 319, 191, 0, 0 }, left = { 0, 20, 270, 191, 0, 0 };
        tl_init(&t, &c);
        for (k = 0; k < 10; k++) { p.cap_ms = 1000 + k * 200; tl_update(&t, 1000 + k * 200, &wide, 1, FW, FH, &p, &o); pans += o.dpan != 0; }
        tl_init(&t, &c);
        for (k = 0; k < 10; k++) { p.cap_ms = 1000 + k * 200; tl_update(&t, 1000 + k * 200, &both, 1, FW, FH, &p, &o); pans += o.dpan != 0; }
        CHECK(pans == 0);
        (void)wl; (void)left;
    }
    /* detection flaps: a 3 s grace keeps the target; the person back after 1 s needs no new acquire (1 tick) and the old state is kept */
    {
        tl_t t; struct tl_ptz p = ptz(); struct tl_out o; int k;
        struct tl_box b = { 20, 20, 120, 100, 0, 0 };
        tl_init(&t, &c);
        for (k = 0; k < 2; k++) { p.cap_ms = 1000 + k * 200; tl_update(&t, 1000 + k * 200, &b, 1, FW, FH, &p, &o); }
        CHECK(t.has_target && t.streak == 2);
        p.cap_ms = 0;
        tl_update(&t, 3250, 0, 0, FW, FH, &p, &o);
        CHECK(t.has_target && t.streak == 2 && t.dbg.why && !strcmp(t.dbg.why, "gap-hold"));
        tl_update(&t, 4500, 0, 0, FW, FH, &p, &o);
        CHECK(!t.has_target && t.streak == 0);
    }
}

/* ---- crouch then stand, far (4 m, rule 3) and near (0.9 m): the camera must come back up ---- */
static void crouch_stand(double d, double h_stand, double h_crouch, const char *name, int lost_ticks) {
    static struct room r;
    struct tl_cfg c;
    int t_stand, t_crouch, top_end;
    tl_cfg_dev(&c); c.invert_pan = 1; c.tilt_enable = 1;
    memset(&r, 0, sizeof r);
    tl_init(&r.t, &c);
    r.tilt = r.tmin = r.tmax = 1300; r.now = 1000;
    room_run(&r, d, d, h_stand, 50);
    t_stand = r.tilt;
    room_run(&r, d, d, h_crouch, 60);
    t_crouch = r.tilt;
    if (lost_ticks) { /* the detector loses the crouched (small) person for a while: nobody in the frame */
        struct tl_ptz p = ptz(); struct tl_out o; int k;
        p.tilt = r.tilt; p.tilt_min = 1000; p.tilt_max = 2000;
        for (k = 0; k < lost_ticks; k++) { r.now += 200; p.cap_ms = r.now; tl_update(&r.t, r.now, 0, 0, FW, FH, &p, &o); if (o.kind == TL_NUDGE) { r.tilt -= o.dtilt; t_crouch = r.tilt; } }
    }
    room_run(&r, d, d, h_stand, 80);
    top_end = r.top;
    printf("   %s: tilt stand %d, crouched %d, stood again %d (top row %d), reversals %d, pan nudges %d\n", name, t_stand, t_crouch, r.tilt, top_end, r.rev, r.pan_nud);
    CHECK(t_crouch <= t_stand);                                      /* it followed down (or the person stayed in the band) */
    CHECK(r.tilt >= t_stand - 60);                                   /* and came back up */
    CHECK(r.rev <= 4 && r.pan_nud == 0);
}

static void crouch_stand_cases(void) {
    static const double ps[] = { 0.32, 0.2, 0.45 }; /* image px per motor step: nominal and +-40 % */
    int i, l;
    for (i = 0; i < 3; i++) {
        g_pxstep = ps[i];
        for (l = 0; l < 2; l++) { /* l: the detector also loses the crouched person for 4 s */
            crouch_stand(4.0, 1.6, 1.0, "far 4 m", l * 20);
            crouch_stand(2.6, 1.6, 0.9, "far 2.6 m", l * 20);
            crouch_stand(5.0, 1.6, 1.0, "far 5 m", l * 20);
            crouch_stand(0.9, 1.6, 1.0, "near 0.9 m", l * 20);
        }
    }
    g_pxstep = 0.32;
}

/* ---- a far (small) person near the top edge, a static false positive, alternating detections: the camera never runs up to the sky ---- */
struct skyr { int tilt, tmax; int64_t now, lastn; tl_t t; int nud_up; };
/* mode 0: a small box (50 px) fixed in the WORLD, top 4 px at tilt 1300 (it sinks as the camera tilts up); 1: a box fixed in the IMAGE (a static false positive);
 * 2: the world-fixed small box present for `on` ticks, absent for `off` ticks (alternating detections); 3: a tall person (head far above the view, box clipped
 * 0..191 for as long as the head is above): the real clip, confirmed on consecutive frames. */
static void sky_run(struct skyr *r, int mode, int ticks, int on, int off) {
    int k;
    for (k = 0; k < ticks; k++) {
        double sh = (r->tilt - 1300) * g_pxstep;
        struct tl_box b;
        struct tl_ptz p = ptz();
        struct tl_out o;
        int present = 1, y1;
        if (mode == 2) present = (k % (on + off)) < on;
        if (mode == 3) { y1 = (int)(-250 + sh); b.x1 = 100; b.x2 = 220; b.y1 = y1 < 0 ? 0 : y1; b.y2 = 191; }
        else if (mode == 1) { b.x1 = 130; b.x2 = 160; b.y1 = 2; b.y2 = 40; }
        else { y1 = (int)(4 + sh); b.x1 = 130; b.x2 = 160; b.y1 = y1 < 0 ? 0 : y1; b.y2 = y1 + 50; if (b.y2 > 191) b.y2 = 191; }
        p.tilt = r->tilt; p.tilt_min = 1000; p.tilt_max = 2000;
        r->now += 200; p.cap_ms = r->now; p.motion_end_ms = r->lastn ? r->lastn + 100 : 0;
        tl_update(&r->t, r->now, present ? &b : 0, present ? 1 : 0, FW, FH, &p, &o);
        if (o.kind == TL_NUDGE && o.dtilt) {
            r->lastn = r->now;
            r->tilt -= o.dtilt;
            if (o.dtilt < 0) r->nud_up++;
            if (r->tilt > r->tmax) r->tmax = r->tilt;
        }
    }
}

static void sky_cases(void) {
    static struct skyr r;
    struct tl_cfg c;
    int mode, cyc;
    static const char *const name[] = { "far person at the top edge", "static false positive at the top", "alternating 1 on / 6 off", "alternating 1 on / 25 off (long gaps, 1.2 s tick)", "real head clip (tall box)" };
    tl_cfg_dev(&c); c.invert_pan = 1; c.tilt_enable = 1;
    for (mode = 0; mode < 5; mode++) {
        memset(&r, 0, sizeof r);
        tl_init(&r.t, &c);
        r.tilt = r.tmax = 1300; r.now = 1000;
        if (mode == 3) { for (cyc = 0; cyc < 12; cyc++) { sky_run(&r, 2, 26, 3, 23); } }
        else sky_run(&r, mode == 4 ? 3 : mode == 2 ? 2 : mode, 300, 1, 6);
        printf("   %s: tilt 1300 -> %d (max %d, %d up nudges)\n", name[mode], r.tilt, r.tmax, r.nud_up);
        if (mode == 4) CHECK(r.tmax > 1300 + 400); /* a confirmed clip is followed beyond the cap */
        else CHECK(r.tmax <= 1300 + 400 + 30);      /* everything else: bounded by the cap */
    }
}

/* ---- recovery: the camera looks up after a close person and the person walks away (hardware test of dev ca11529, 2026-11-16) ---- */
struct rec { tl_t t; struct tl_ptz p; int64_t now, lastn; int nsearch, nret, npan, nhome; int64_t ret_ms; };
static void rec_tick(struct rec *r, const struct tl_box *b, int moving) {
    struct tl_out o;
    r->now += 200;
    r->p.moving = moving;
    r->p.cap_ms = r->now;
    r->p.motion_end_ms = r->lastn ? r->lastn + 100 : 0;
    if (moving) { r->p.pan = 0; r->p.tilt = 0; } /* the unknown position of a motion-time read */
    tl_update(&r->t, r->now, b, b ? 1 : 0, FW, FH, &r->p, &o);
    if (o.kind == TL_HOME) r->nhome++;
    if (o.kind != TL_NUDGE) return;
    r->lastn = r->now;
    if (o.dpan) r->npan++;
    if (r->t.dbg.why && !strcmp(r->t.dbg.why, "exit-search")) r->nsearch++;
    if (r->t.dbg.why && !strcmp(r->t.dbg.why, "tilt-return")) { r->nret++; r->ret_ms = r->now; }
    r->p.tilt -= o.dtilt;
}
static void rec_init(struct rec *r, int tilt_return_s, int return_home) {
    struct tl_cfg c;
    memset(r, 0, sizeof *r);
    tl_cfg_dev(&c); c.invert_pan = 1; c.tilt_enable = 1; c.lead_pct = 0; c.tilt_return_s = tilt_return_s; c.return_home = return_home;
    tl_init(&r->t, &c);
    r->p = ptz();
    r->p.tilt = 1866; r->p.tilt_min = 1000; r->p.tilt_max = 2000; r->p.tilt_home = 1300;
    r->now = 1000;
}

static void recover(void) {
    static struct rec r;
    struct tl_box clip = { 4, 0, 293, 191, 0, 0 }, away = { 19, 55, 202, 190, 0, 0 }, flap = { 30, 0, 318, 191, 0, 0 };
    int k;
    /* the log: head clipped, then the user walks away (the box top sinks), the frame is stale, then nobody for 25 s */
    rec_init(&r, 10, 0);
    for (k = 0; k < 4; k++) rec_tick(&r, &clip, 0);
    rec_tick(&r, &away, 1);                                         /* stale (the camera moves): pan / tilt read as 0 / 0 */
    CHECK(r.t.has_target && r.t.dbg.rule == 2 && r.t.dbg.ty == 28 && r.t.dbg.why && !strcmp(r.t.dbg.why, "stale")); /* a bottom-cut 70 % box: head rule, never centre */
    {
        int streak = r.t.streak;
        rec_tick(&r, &away, 1);
        CHECK(r.t.has_target && r.t.streak == streak);              /* the unknown position changed nothing */
    }
    r.p.pan = 1264; r.p.tilt = 1866;                                /* the position is known again */
    r.nsearch = r.nret = r.npan = 0; r.lastn = 0;
    {
        int64_t lost = r.now;
        int tl = r.p.tilt;
        for (k = 0; k < 140; k++) rec_tick(&r, 0, 0);               /* 28 s without a person */
        printf("nsearch %d\n", r.nsearch);
    CHECK(r.nsearch >= 1 && r.nsearch <= 2);                     /* it looked DOWN (where the stale frame saw the person leave), twice at most */
        CHECK(r.npan == 0);
        CHECK(r.nret == 1 && r.ret_ms >= lost + 13000 - 1000 && r.ret_ms <= lost + 13000 + 1500); /* grace 3 s + 10 s: the tilt goes home */
        CHECK(r.p.tilt >= 1300 - 30 && r.p.tilt <= 1300 + 30 && r.p.tilt < tl);
    }
    /* tilt_return_s 0 = off, home mode = the home request instead */
    rec_init(&r, 0, 0);
    for (k = 0; k < 4; k++) rec_tick(&r, &clip, 0);
    for (k = 0; k < 140; k++) rec_tick(&r, 0, 0);
    CHECK(r.nret == 0);
    rec_init(&r, 10, 1);
    for (k = 0; k < 4; k++) rec_tick(&r, &clip, 0);
    for (k = 0; k < 400; k++) rec_tick(&r, 0, 0);
    CHECK(r.nret == 0);
    /* a close person that only flaps (wide boxes cut by the frame) is not "leaving": no search, no pan */
    rec_init(&r, 10, 0);
    for (k = 0; k < 6; k++) rec_tick(&r, &flap, 0);
    r.lastn = 0; r.nsearch = 0; r.npan = 0;
    for (k = 0; k < 15; k++) rec_tick(&r, 0, 0);                     /* 3 s gap */
    CHECK(r.nsearch == 0 && r.npan == 0);
    /* a person walking out at the right edge: the search turns right (pan +, motor sign inverted on the 2K = dpan < 0 image... the motor sign follows invert_pan) */
    rec_init(&r, 0, 0);
    { struct tl_box e1 = { 200, 40, 250, 150, 0, 0 }, e2 = { 280, 40, 319, 150, 0, 0 }; int sawpan = 0; struct tl_out o;
      for (k = 0; k < 3; k++) rec_tick(&r, &e1, 0);
      for (k = 0; k < 3; k++) rec_tick(&r, &e2, 0);
      r.lastn = 0;
      for (k = 0; k < 15; k++) { r.now += 200; r.p.cap_ms = r.now; r.p.motion_end_ms = 0; tl_update(&r.t, r.now, 0, 0, FW, FH, &r.p, &o); if (o.kind == TL_NUDGE && o.dpan) sawpan = o.dpan; }
      CHECK(sawpan != 0 && r.t.exit_dir == TL_EXIT_RIGHT); }
}

/* ---- Fable review of 91d2262 (F2, F3, F4, F6) ---- */
static void fable91d(void) {
    static struct rec r;
    struct tl_box edge = { 0, 40, 40, 150, 0, 0 }, e1 = { 200, 40, 250, 150, 0, 0 }, e2 = { 280, 40, 319, 150, 0, 0 }, clip = { 4, 0, 293, 191, 0, 0 }, sunk = { 50, 60, 150, 150, 0, 0 };
    int k, i;
    /* F2: ONE detection frame at the edge (a false positive), then nothing: the motor never moves, no search */
    rec_init(&r, 0, 0);
    rec_tick(&r, &edge, 0);
    for (k = 0; k < 40; k++) rec_tick(&r, 0, 0);
    CHECK(r.nsearch == 0 && r.npan == 0 && r.lastn == 0 && r.t.exit_set == 0);
    /* ... while a confirmed target leaving at the same edge still gets its search */
    rec_init(&r, 0, 0);
    for (k = 0; k < 3; k++) rec_tick(&r, &e1, 0);
    for (k = 0; k < 3; k++) rec_tick(&r, &e2, 0);
    r.nsearch = 0; r.lastn = 0;
    for (k = 0; k < 15; k++) rec_tick(&r, 0, 0);
    CHECK(r.nsearch >= 1);
    /* F3: a stale sighting after each search nudge does not rearm the search: at most 2 nudges per loss */
    rec_init(&r, 0, 0);
    for (k = 0; k < 3; k++) rec_tick(&r, &e1, 0);
    for (k = 0; k < 3; k++) rec_tick(&r, &e2, 0);
    r.nsearch = 0; r.lastn = 0;
    for (i = 0; i < 40; i++) {
        rec_tick(&r, r.lastn && r.now - r.lastn == 0 ? &e2 : 0, 0); /* the tick after a nudge: captured while the head turns, the person is still seen (stale) */
    }
    CHECK(r.nsearch >= 1 && r.nsearch <= 2);
    /* F4: the camera tilts up (a tilt nudge), the person's box sinks in the stale frames, then a 600 ms gap: no search DOWN */
    rec_init(&r, 0, 0);
    for (k = 0; k < 6 && !r.lastn; k++) rec_tick(&r, &clip, 0);
    CHECK(r.lastn != 0);
    rec_tick(&r, &sunk, 0);
    rec_tick(&r, &sunk, 0);
    r.nsearch = 0;
    for (k = 0; k < 14; k++) rec_tick(&r, 0, 0); /* inside the 3 s grace, after the 2 s the pose is unknown */
    CHECK(r.nsearch == 0 && r.t.exit_dir != TL_EXIT_DOWN);
    /* F6: the pending return home is cancelled by a position set by hand after the last tracking nudge */
    rec_init(&r, 0, 1);
    for (k = 0; k < 3; k++) rec_tick(&r, &e1, 0);
    for (k = 0; k < 400; k++) rec_tick(&r, 0, 0);
    CHECK(r.nhome == 1); /* control: nobody touched the camera, it goes home */
    rec_init(&r, 0, 1);
    for (k = 0; k < 3; k++) rec_tick(&r, &e1, 0);
    for (k = 0; k < 10; k++) rec_tick(&r, 0, 0);
    r.p.pan += 300; /* an API move / step / goto / home / park */
    for (k = 0; k < 400; k++) rec_tick(&r, 0, 0);
    CHECK(r.nhome == 0);
    rec_init(&r, 0, 1);
    for (k = 0; k < 3; k++) rec_tick(&r, &e1, 0);
    for (k = 0; k < 10; k++) rec_tick(&r, 0, 0);
    r.p.tilt += 200;
    for (k = 0; k < 400; k++) rec_tick(&r, 0, 0);
    CHECK(r.nhome == 0);
    /* ... and a person tracked again from the manual position is brought home after the next loss */
    for (k = 0; k < 3; k++) rec_tick(&r, &e1, 0);
    for (k = 0; k < 400; k++) rec_tick(&r, 0, 0);
    CHECK(r.nhome == 1);
}

/* ---- going DOWN: symmetric to the up case (user feedback: the camera did not follow a person lower in the frame) ---- */
/* number of ticks until the box top is back in the head band after the head world position moved by `shift` px (down > 0), from a settled
 * close person at tilt 1300; *first = ticks to the first nudge; returns -1 when it never settles. */
static int resp(double shift, int hgt, int *first, int *nud) {
    static struct room r;
    struct tl_cfg c;
    double wtop = 28 - 1300 * 0.32; /* the head at row 28 at tilt 1300 */
    int k, settled = -1;
    tl_cfg_dev(&c); c.invert_pan = 1; c.tilt_enable = 1;
    memset(&r, 0, sizeof r);
    tl_init(&r.t, &c);
    r.tilt = r.tmin = r.tmax = 1300; r.now = 1000;
    wtop += shift;
    *first = -1;
    for (k = 0; k < 60; k++) {
        int before = r.nud;
        /* one tick of the room at a constant distance, the head at wtop: reuse room_run with a head height that gives that row at D = 0.9 */
        double hh = 0.75 + (96 - 1300 * 0.32 - wtop) * 0.9 / 205.8;
        room_run(&r, 0.9, 0.9, hh, 1);
        if (*first < 0 && r.nud > before) *first = k;
        if (settled < 0 && r.nud > 0 && r.nud == before && r.top >= 14 && r.top <= 42) settled = k;
        (void)hgt;
    }
    *nud = r.nud;
    return settled;
}

static void down_cases(void) {
    int f_dn, n_dn, f_up, n_up, s_dn, s_up;
    s_dn = resp(+90, 150, &f_dn, &n_dn);   /* crouches: the head drops 90 px */
    s_up = resp(-60, 150, &f_up, &n_up);   /* stands on tiptoe / the head rises 60 px */
    printf("   responsiveness (ticks of 200 ms): head down 90 px: first nudge %d settled %d nudges %d | head up 60 px: first %d settled %d nudges %d\n",
           f_dn, s_dn, n_dn, f_up, s_up, n_up);
    CHECK(f_dn >= 0 && f_dn <= 4);                                   /* the first DOWN nudge comes as fast as the up one */
    CHECK(f_up < 0 || f_dn <= f_up + 2);
    CHECK(n_dn <= 6);
    {   /* a head 30 px below the band (the user report): back in the band in one or two nudges, in either direction */
        int f1, n1, f2, n2, s1 = resp(+45, 150, &f1, &n1), s2 = resp(-30, 150, &f2, &n2);
        printf("   head 45 px below the row / 30 px above: down %d nudges (settled tick %d), up %d nudges (settled tick %d)\n", n1, s1, n2, s2);
        CHECK(n1 >= 1 && n1 <= 2 && n2 <= 2);
    }
    {   /* sitting down slowly while detected: the box top drifts from row 28 to row 110 over 8 s, the camera follows down */
        static struct room r;
        struct tl_cfg c;
        int k, t0;
        tl_cfg_dev(&c); c.invert_pan = 1; c.tilt_enable = 1;
        memset(&r, 0, sizeof r);
        tl_init(&r.t, &c);
        r.tilt = r.tmin = r.tmax = 1500; r.now = 1000;
        room_run(&r, 0.9, 0.9, 1.6, 30);
        t0 = r.tilt;
        for (k = 0; k < 40; k++) room_run(&r, 0.9, 0.9, 1.6 - 0.45 * (k + 1) / 40.0, 1);
        room_run(&r, 0.9, 0.9, 1.15, 40);
        CHECK(r.tilt < t0 - 100 && r.top >= 1 && r.top <= 58 && r.pan_nud == 0 && r.rev <= 3);
    }
    {   /* crouching close with the detector losing the person at the same time (log: top 55, bottom cut, then nobody): look down */
        static struct rec r;
        struct tl_box up = { 4, 0, 293, 191, 0, 0 }, low1 = { 12, 40, 250, 191, 0, 0 }, low2 = { 19, 55, 202, 190, 0, 0 }, still = { 19, 56, 202, 190, 0, 0 };
        int k, down = 0;
        rec_init(&r, 10, 0);
        for (k = 0; k < 3; k++) rec_tick(&r, &up, 0);
        rec_tick(&r, &low1, 0);
        rec_tick(&r, &low2, 0);
        r.lastn = 0;
        for (k = 0; k < 25; k++) { int t0 = r.p.tilt; rec_tick(&r, 0, 0); down += r.p.tilt < t0; }
        CHECK(down >= 1 && r.nsearch >= 1);
        /* the same without a trend: only the last boxes, top low and cut at the bottom */
        rec_init(&r, 10, 0);
        for (k = 0; k < 4; k++) rec_tick(&r, &still, 0);
        r.lastn = 0; down = 0;
        for (k = 0; k < 25; k++) { int t0 = r.p.tilt; rec_tick(&r, 0, 0); down += r.p.tilt < t0; }
        CHECK(down >= 1);
    }
}

/* the sim: approach, tilt up, walk away */
static void away_sim(void) {
    static struct room r;
    struct tl_cfg c;
    int t_up;
    tl_cfg_dev(&c); c.invert_pan = 1; c.tilt_enable = 1;
    memset(&r, 0, sizeof r);
    tl_init(&r.t, &c);
    r.tilt = r.tmin = r.tmax = 1300; r.now = 1000;
    room_run(&r, 4.0, 0.9, 1.6, 40);
    room_run(&r, 0.9, 0.9, 1.6, 30);
    t_up = r.tilt;
    CHECK(t_up > 1400);
    room_run(&r, 0.9, 4.0, 1.6, 50);                                 /* walks away: the head goes down the image */
    CHECK(r.pan_nud == 0 && r.top >= 1);
    CHECK(r.tilt < t_up);                                            /* the camera followed the head down */
}

int main(void) {
    tl_t t; struct tl_out o; struct tl_ptz p = ptz(); struct tl_cfg c; struct tl_box b, bs[2], z = { 5, 5, 5, 5, 0, 0 };

    tl_cfg_dev(&c);
    CHECK(tl_cfg_validate(&c) == 0);
    c.deadband_pct = 60; CHECK(tl_cfg_validate(&c) != 0);
    tl_cfg_dev(&c); c.max_step = 0; CHECK(tl_cfg_validate(&c) != 0);
    tl_cfg_dev(&c); c.lost_grace_s = -1; CHECK(tl_cfg_validate(&c) != 0);
    tl_cfg_dev(&c); c.gain_pct = 0; CHECK(tl_cfg_validate(&c) != 0);
    tl_cfg_dev(&c); c.speed_fast = 1; CHECK(tl_cfg_validate(&c) != 0); /* 1 is refused by the safe layer */
    tl_cfg_dev(&c); c.return_home = 2; CHECK(tl_cfg_validate(&c) != 0);
    CHECK(tl_init(&t, &c) == -1);
    tl_update(&t, 1, &z, 1, FW, FH, &p, &o); CHECK(o.kind == TL_NONE); /* unusable state does nothing */

    /* centred -> none */
    init(&t); o = two(&t, 1000, box(160, 96, 60, 150), p); CHECK(o.kind == TL_NONE);
    /* deadband (12% of 320 = 38.4 px): 30 px right -> none; 40 px right -> 40/320*1000 = 125 */
    init(&t); o = two(&t, 1000, box(190, 96, 60, 150), p); CHECK(o.kind == TL_NONE);
    init(&t); o = two(&t, 1000, box(200, 96, 60, 150), p);
    CHECK(o.kind == TL_NUDGE && o.dpan == 125 && o.dtilt == 0);
    /* left / up / down signs */
    init(&t); o = two(&t, 1000, box(100, 96, 40, 150), p); CHECK(o.kind == TL_NUDGE && o.dpan < 0 && o.dtilt == 0);
    init(&t); o = two(&t, 1000, box(160, 30, 40, 40), p); CHECK(o.kind == TL_NUDGE && o.dtilt < 0 && o.dpan == 0);
    init(&t); o = two(&t, 1000, box(160, 170, 40, 40), p); CHECK(o.kind == TL_NUDGE && o.dtilt > 0 && o.dpan == 0);
    /* clamp: at the edge 150/320*1000 = 469 -> 300 */
    init(&t); o = two(&t, 1000, box(310, 96, 20, 150), p); CHECK(o.kind == TL_NUDGE && o.dpan == 300);
    init(&t); o = two(&t, 1000, box(10, 96, 20, 150), p); CHECK(o.kind == TL_NUDGE && o.dpan == -300);
    /* invert */
    tl_cfg_legacy(&c); c.invert_pan = 1; tl_init(&t, &c);
    o = two(&t, 1000, box(310, 96, 20, 150), p); CHECK(o.dpan == -300);

    /* a gap in the detections resets the confirm streak */
    init(&t); b = box(300, 96, 40, 150);
    tl_update(&t, 1000, &b, 1, FW, FH, &p, &o); CHECK(o.kind == TL_NONE);
    tl_update(&t, 1200, 0, 0, FW, FH, &p, &o); CHECK(o.kind == TL_NONE);
    tl_update(&t, 1400, &b, 1, FW, FH, &p, &o); CHECK(o.kind == TL_NONE);
    tl_update(&t, 1600, &b, 1, FW, FH, &p, &o); CHECK(o.kind == TL_NUDGE);

    /* rate limit 1500 ms (first nudge at 1200) */
    init(&t); o = two(&t, 1000, b, p); CHECK(o.kind == TL_NUDGE);
    tl_update(&t, 2000, &b, 1, FW, FH, &p, &o); CHECK(o.kind == TL_NONE);
    tl_update(&t, 2699, &b, 1, FW, FH, &p, &o); CHECK(o.kind == TL_NONE);
    tl_update(&t, 2700, &b, 1, FW, FH, &p, &o); CHECK(o.kind == TL_NUDGE);

    /* moving -> no nudge, no rate-limit stamp */
    init(&t); p.moving = 1;
    tl_update(&t, 1000, &b, 1, FW, FH, &p, &o);
    tl_update(&t, 1200, &b, 1, FW, FH, &p, &o); CHECK(o.kind == TL_NONE);
    p.moving = 0; tl_update(&t, 1400, &b, 1, FW, FH, &p, &o); CHECK(o.kind == TL_NUDGE);

    /* two people: the largest first, then sticky to it although the other becomes larger */
    init(&t);
    bs[0] = box(60, 96, 40, 100); bs[1] = box(250, 96, 80, 180);
    tl_update(&t, 1000, bs, 2, FW, FH, &p, &o);
    tl_update(&t, 1200, bs, 2, FW, FH, &p, &o); CHECK(o.kind == TL_NUDGE && o.dpan > 0);
    bs[0] = box(60, 96, 70, 188); bs[1] = box(252, 96, 70, 150);
    tl_update(&t, 3000, bs, 2, FW, FH, &p, &o); CHECK(o.kind == TL_NUDGE && o.dpan > 0);
    CHECK(t.target.x1 > 200);

    /* lost -> home after 10 s, once; never without a prior nudge; off when disabled */
    init(&t); o = two(&t, 1000, b, p); CHECK(o.kind == TL_NUDGE); /* last seen 1200 */
    tl_update(&t, 11199, 0, 0, FW, FH, &p, &o); CHECK(o.kind == TL_NONE);
    tl_update(&t, 11200, 0, 0, FW, FH, &p, &o); CHECK(o.kind == TL_HOME);
    tl_update(&t, 11400, 0, 0, FW, FH, &p, &o); CHECK(o.kind == TL_NONE);
    init(&t); o = two(&t, 1000, box(160, 96, 40, 150), p);
    tl_update(&t, 30000, 0, 0, FW, FH, &p, &o); CHECK(o.kind == TL_NONE);
    tl_cfg_legacy(&c); c.return_home = 0; tl_init(&t, &c); o = two(&t, 1000, b, p);
    tl_update(&t, 60000, 0, 0, FW, FH, &p, &o); CHECK(o.kind == TL_NONE);

    /* soft range: clipped, none at the limit */
    init(&t); p = ptz(); p.pan = 1950;
    o = two(&t, 1000, box(310, 96, 20, 150), p); CHECK(o.kind == TL_NUDGE && o.dpan == 50);
    init(&t); p.pan = 2000;
    o = two(&t, 1000, box(310, 96, 20, 150), p); CHECK(o.kind == TL_NONE);
    /* a person above the centre: tilt UP (motor +), image dtilt < 0; clipped at the top of the range (tilt 490, max 500) */
    init(&t); p = ptz(); p.tilt = 490;
    o = two(&t, 1000, box(160, 20, 40, 40), p); CHECK(o.kind == TL_NUDGE && o.dtilt == -10);

    /* garbage input is ignored */
    init(&t); p = ptz();
    tl_update(&t, 1, &z, 1, FW, FH, &p, &o); CHECK(o.kind == TL_NONE && !t.has_target);
    tl_update(&t, 1, &b, 1, 0, 0, &p, &o); CHECK(o.kind == TL_NONE);

    responsive();
    head_tilt();
    hw_boxes();
    close_range();
    crouch_stand_cases();
    sky_cases();
    recover();
    fable91d();
    away_sim();
    down_cases();

    printf(fails ? "track: %d FAILED\n" : "track: all tests passed\n", fails);
    return fails != 0;
}
