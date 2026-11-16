/* Host unit test of track_logic (pure policy). Build: see docs/porting/lsc-rotatable-2k/tracking.md */
#include "../firmware/lsc-rotatable-2k-ak3918av130/app/track_logic.h"
#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)
#define FW 320
#define FH 192

static struct tl_ptz ptz(void) {
    struct tl_ptz p = {0, 0, 0, -2000, 2000, -500, 500, 0, 0};
    return p;
}
static struct tl_box box(int cx, int cy, int w, int h) {
    struct tl_box b = {cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2};
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
    tl_cfg_defaults(&c); c.invert_pan = 0; c.lead_pct = 0;
    /* inside the band (8 % of 320 = 25.6 px): nothing; outside: acts after the 2 acquire ticks, proportional x 85 % */
    tl_init(&t, &c);
    o = step(&t, 1000, 1000, box(180, 96, 60, 150), p); CHECK(o.kind == TL_NONE);
    o = step(&t, 1200, 1200, box(180, 96, 60, 150), p); CHECK(o.kind == TL_NONE); /* 20 px: inside the band */
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
    b = box(160, 150, 40, 60);                                                 /* upper third at row 140: 44 px below = 137 steps -> 116 */
    o = step(&t, 1000, 1000, b, p); o = step(&t, 1200, 1200, b, p); CHECK(o.kind == TL_NUDGE && o.dtilt == 116 && o.dpan == 0);
    c.tilt_enable = 0; tl_init(&t, &c);
    o = step(&t, 1000, 1000, b, p); o = step(&t, 1200, 1200, b, p); CHECK(o.kind == TL_NONE);
    tl_cfg_defaults(&c); CHECK(c.tilt_enable == 0 && c.settle_ms == 220); /* tilt is opt-in until verified on the camera */
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
        tl_cfg_defaults(&c); c.invert_pan = 0; c.lead_pct = 0; tl_init(&t, &c); p.motion_end_ms = 0;
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

    /* return policy: stay (default) never goes home; home after grace + delay; delay 0 = right after the grace */
    tl_cfg_defaults(&c); c.invert_pan = 0; c.lead_pct = 0; tl_init(&t, &c); p = ptz();
    b = box(300, 96, 40, 150);
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
static struct tl_box rect(int x1, int y1, int x2, int y2) { struct tl_box b = {x1, y1, x2, y2}; return b; }

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
    tl_cfg_defaults(&c); c.invert_pan = 0; c.lead_pct = 0; c.tilt_enable = 1;
    tl_init(&t, &c);
    /* head fully visible at the target (top 29 px) or anywhere in the band 0..58: nothing, whatever the box height (centre = 110, 96 ...) */
    o = step(&t, 1000, 1000, rect(100, 15, 220, 191), p); o = step(&t, 1200, 1200, rect(100, 15, 220, 191), p); CHECK(o.kind == TL_NONE);
    tl_init(&t, &c);
    o = step(&t, 1000, 1000, rect(100, 40, 220, 191), p); o = step(&t, 1200, 1200, rect(100, 40, 220, 191), p); CHECK(o.kind == TL_NONE);
    /* head clipped at the top, box 191 px tall: up (image above = dtilt < 0) by 600 * 191/192 / 3 = 198 steps, never a pan */
    tl_init(&t, &c);
    o = step(&t, 1000, 1000, rect(100, 0, 220, 191), p); o = step(&t, 1200, 1200, rect(100, 0, 220, 191), p);
    CHECK(o.kind == TL_NUDGE && o.dtilt == -198 && o.dpan == 0);
    /* a smaller clipped box: a smaller step (bounded below by the minimum nudge) */
    tl_init(&t, &c);
    o = step(&t, 1000, 1000, rect(100, 1, 220, 61), p); o = step(&t, 1200, 1200, rect(100, 1, 220, 61), p);
    CHECK(o.kind == TL_NUDGE && o.dtilt == -62);
    /* crouching: the top drops to row 120 in a still full-height box: down (dtilt > 0) by (120-29)/192*600 * 85 % = 241 */
    tl_init(&t, &c);
    o = step(&t, 1000, 1000, rect(100, 120, 220, 191), p); o = step(&t, 1200, 1200, rect(100, 120, 220, 191), p);
    CHECK(o.kind == TL_NUDGE && o.dtilt > 0 && o.dpan == 0);
    /* a far person (box 70 px tall, 36 %): centred on its upper third; fully inside and near the target: nothing */
    tl_init(&t, &c);
    o = step(&t, 1000, 1000, rect(130, 60, 190, 130), p); o = step(&t, 1200, 1200, rect(130, 60, 190, 130), p); CHECK(o.kind == TL_NONE);
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
    CHECK(o.kind == TL_NUDGE && o.dtilt == 198);
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

int main(void) {
    tl_t t; struct tl_out o; struct tl_ptz p = ptz(); struct tl_cfg c; struct tl_box b, bs[2], z = {5, 5, 5, 5};

    tl_cfg_defaults(&c);
    CHECK(tl_cfg_validate(&c) == 0);
    c.deadband_pct = 60; CHECK(tl_cfg_validate(&c) != 0);
    tl_cfg_defaults(&c); c.max_step = 0; CHECK(tl_cfg_validate(&c) != 0);
    tl_cfg_defaults(&c); c.lost_grace_s = -1; CHECK(tl_cfg_validate(&c) != 0);
    tl_cfg_defaults(&c); c.gain_pct = 0; CHECK(tl_cfg_validate(&c) != 0);
    tl_cfg_defaults(&c); c.speed_fast = 1; CHECK(tl_cfg_validate(&c) != 0); /* 1 is refused by the safe layer */
    tl_cfg_defaults(&c); c.return_home = 2; CHECK(tl_cfg_validate(&c) != 0);
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

    printf(fails ? "track: %d FAILED\n" : "track: all tests passed\n", fails);
    return fails != 0;
}
