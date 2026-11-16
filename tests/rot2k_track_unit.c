/* Host unit test of track_logic (pure policy). Build: see docs/porting/lsc-rotatable-2k/tracking.md */
#include "../firmware/lsc-rotatable-2k-ak3918av130/app/track_logic.h"
#include <stdio.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)
#define FW 320
#define FH 192

static struct tl_ptz ptz(void) {
    struct tl_ptz p = {0, 0, 0, -2000, 2000, -500, 500};
    return p;
}
static struct tl_box box(int cx, int cy, int w, int h) {
    struct tl_box b = {cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2};
    return b;
}
static void init(tl_t *t) { struct tl_cfg c; tl_cfg_defaults(&c); CHECK(tl_init(t, &c) == 0); }
/* two ticks 200 ms apart (the confirm streak); returns the second result */
static struct tl_out two(tl_t *t, int64_t now, struct tl_box b, struct tl_ptz p) {
    struct tl_out o;
    tl_update(t, now, &b, 1, FW, FH, &p, &o);
    CHECK(o.kind == TL_NONE); /* the first tick of a new target never moves */
    tl_update(t, now + 200, &b, 1, FW, FH, &p, &o);
    return o;
}

int main(void) {
    tl_t t; struct tl_out o; struct tl_ptz p = ptz(); struct tl_cfg c; struct tl_box b, bs[2], z = {5, 5, 5, 5};

    tl_cfg_defaults(&c);
    CHECK(tl_cfg_validate(&c) == 0);
    c.deadband_pct = 60; CHECK(tl_cfg_validate(&c) != 0);
    tl_cfg_defaults(&c); c.max_step = 0; CHECK(tl_cfg_validate(&c) != 0);
    tl_cfg_defaults(&c); c.lost_s = 0; CHECK(tl_cfg_validate(&c) != 0);
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
    tl_cfg_defaults(&c); c.invert_pan = 1; tl_init(&t, &c);
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
    tl_cfg_defaults(&c); c.home_enable = 0; tl_init(&t, &c); o = two(&t, 1000, b, p);
    tl_update(&t, 60000, 0, 0, FW, FH, &p, &o); CHECK(o.kind == TL_NONE);

    /* soft range: clipped, none at the limit */
    init(&t); p = ptz(); p.pan = 1950;
    o = two(&t, 1000, box(310, 96, 20, 150), p); CHECK(o.kind == TL_NUDGE && o.dpan == 50);
    init(&t); p.pan = 2000;
    o = two(&t, 1000, box(310, 96, 20, 150), p); CHECK(o.kind == TL_NONE);
    init(&t); p = ptz(); p.tilt = -490;
    o = two(&t, 1000, box(160, 20, 40, 40), p); CHECK(o.kind == TL_NUDGE && o.dtilt == -10);

    /* garbage input is ignored */
    init(&t); p = ptz();
    tl_update(&t, 1, &z, 1, FW, FH, &p, &o); CHECK(o.kind == TL_NONE && !t.has_target);
    tl_update(&t, 1, &b, 1, 0, 0, &p, &o); CHECK(o.kind == TL_NONE);

    printf(fails ? "track: %d FAILED\n" : "track: all tests passed\n", fails);
    return fails != 0;
}
