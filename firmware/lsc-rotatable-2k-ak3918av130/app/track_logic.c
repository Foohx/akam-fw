/* See track_logic.h and docs/porting/lsc-rotatable-2k/tracking.md. Integer arithmetic only. */
#include "track_logic.h"
#include <string.h>

#define MIN_STEP 20        /* modern mode: a correction below this many steps (~2 deg) is not worth a motor command */
#define START_LAG_MS 80    /* a motor command takes this long to start moving: a frame captured just after it was sent still shows the old pose */
#define V_MIN 40           /* steps/s: a slower drift is noise, not a walking person */
#define SAMPLE_GAP_MS 1500 /* the velocity samples must be this close to each other */
#define REVERSE_MS 1500    /* a nudge against the previous one inside this time is halved (anti hunting) */
#define REV_WINDOW_MS 6000 /* duty cap: at most 2 reversals of direction inside this window, the 3rd is held back */
#define REV_HOLD_MS 3000   /* ... and the tracker then holds still this long */
#define HEAD_TALL_PCT 55   /* tilt: a box at least this % of the frame height (or cut by the frame bottom) is anchored on its top (head), a shorter one on its upper third */
#define HEAD_CLIP_PX 2     /* a box top this close to the frame top is clipped by the frame: the head is out */
#define STALE_MAX_MS 2000  /* a frame is stale while the last nudge has no reported motion end, for at most this long */

void tl_cfg_defaults(struct tl_cfg *c) {
    memset(c, 0, sizeof *c);
    c->deadband_pct = 8;
    c->tband_pct = 15;
    c->gain_pan = 1000;
    c->gain_tilt = 600;
    c->gain_pct = 85;
    c->max_step = 700;
    c->min_interval_ms = 300;
    c->confirm_ticks = 2;
    c->settle_ms = 220;
    c->fast_pct = 25;
    c->speed_fast = 2;
    c->lead_pct = 70;
    c->lead_max = 500;
    c->tilt_enable = 0; /* opt-in (track_tilt=1) until the tilt direction is verified on the camera */
    c->lost_grace_s = 3;
    c->return_home = 0;
    c->return_delay_s = 30;
    c->head_pct = 15;
    c->sticky_gate_pct = 35;
}

void tl_cfg_legacy(struct tl_cfg *c) {
    tl_cfg_defaults(c);
    c->deadband_pct = 12;
    c->tband_pct = 12;
    c->gain_pct = 100;
    c->max_step = 300;
    c->min_interval_ms = 1500;
    c->settle_ms = 0;
    c->fast_pct = 100;
    c->speed_fast = 0;
    c->lead_pct = 0;
    c->tilt_enable = 1;
    c->lost_grace_s = 10;
    c->return_home = 1;
    c->return_delay_s = 0;
}

int tl_cfg_validate(const struct tl_cfg *c) {
    if (!c) return -1;
    if (c->deadband_pct < 0 || c->deadband_pct > 50 || c->tband_pct < 0 || c->tband_pct > 50) return -1;
    if (c->gain_pan <= 0 || c->gain_pan > 100000 || c->gain_tilt <= 0 || c->gain_tilt > 100000) return -1;
    if (c->gain_pct < 10 || c->gain_pct > 100) return -1;
    if (c->max_step <= 0 || c->max_step > 100000) return -1;
    if (c->min_interval_ms < 0 || c->confirm_ticks < 1 || c->confirm_ticks > 100) return -1;
    if (c->settle_ms < 0 || c->settle_ms > 5000) return -1;
    if (c->fast_pct < 0 || c->fast_pct > 100) return -1;
    if (c->speed_fast != 0 && (c->speed_fast < 2 || c->speed_fast > 20)) return -1;
    if (c->lead_pct < 0 || c->lead_pct > 100 || c->lead_max < 0 || c->lead_max > 2000) return -1;
    if (c->lost_grace_s < 0 || c->lost_grace_s > 86400 || c->return_delay_s < 0 || c->return_delay_s > 86400) return -1;
    if (c->head_pct < 0 || c->head_pct > 50) return -1;
    if (c->sticky_gate_pct < 0 || c->sticky_gate_pct > 100) return -1;
    if ((c->return_home & ~1) || (c->tilt_enable & ~1) || (c->invert_pan & ~1) || (c->invert_tilt & ~1)) return -1;
    return 0;
}

int tl_init(tl_t *t, const struct tl_cfg *c) {
    memset(t, 0, sizeof *t);
    if (tl_cfg_validate(c) != 0) return -1;
    t->cfg = *c;
    return 0;
}

static int64_t area(const struct tl_box *b) { return (int64_t)(b->x2 - b->x1) * (b->y2 - b->y1); }
static int valid(const struct tl_box *b) { return b->x2 > b->x1 && b->y2 > b->y1; }
static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static int64_t iabs64(int64_t v) { return v < 0 ? -v : v; }
static int sgn(int v) { return v < 0 ? -1 : v > 0; }

/* d2 = 2*centre - size (half-pixels from the frame centre): the offset fraction is d2 / (2*size); the steps to centre it, image sign. */
static int offset_steps(int d2, int size, int gain) { return (int)((int64_t)d2 * gain / (2 * (int64_t)size)); }
static int in_band(int d2, int size, int pct) { return iabs64(d2) * 100 <= (int64_t)pct * 2 * size; }
static int beyond(int d2, int size, int pct) { return iabs64(d2) * 100 > (int64_t)pct * 2 * size; }

/* Walking speed of the target along pan, motor steps/s, from the oldest and the newest sample; 0 when there is not enough data. */
static int velocity(const tl_t *t) {
    int64_t dt, v;
    if (t->ns < 2) return 0;
    dt = t->st[t->ns - 1] - t->st[0];
    if (dt < 150 || dt > 2 * SAMPLE_GAP_MS) return 0;
    v = (int64_t)(t->sw[t->ns - 1] - t->sw[0]) * 1000 / dt;
    if (iabs64(v) < V_MIN) return 0;
    return (int)(v > 5000 ? 5000 : v < -5000 ? -5000 : v);
}

static void push_sample(tl_t *t, int64_t cap, long w) {
    if (t->ns > 0 && (cap - t->st[t->ns - 1] > SAMPLE_GAP_MS || cap <= t->st[t->ns - 1])) t->ns = 0;
    if (t->ns == TL_NS) {
        memmove(&t->st[0], &t->st[1], sizeof t->st[0] * (TL_NS - 1));
        memmove(&t->sw[0], &t->sw[1], sizeof t->sw[0] * (TL_NS - 1));
        t->ns--;
    }
    t->st[t->ns] = cap;
    t->sw[t->ns] = w;
    t->ns++;
}

void tl_update(tl_t *t, int64_t now_ms, const struct tl_box *boxes, int n, int fw, int fh, const struct tl_ptz *ptz,
               struct tl_out *out) {
    const struct tl_cfg *c = &t->cfg;
    const struct tl_box *pick = 0;
    int sticky = 0, i, tclip = 0, dp = 0, dt = 0, p, q, stale, modern, engaged, minv = 0, fast = 0, m;
    int64_t cap;
    out->kind = TL_NONE;
    out->dpan = out->dtilt = out->speed = 0;
    if (fw <= 0 || fh <= 0 || c->max_step <= 0) return; /* bad frame or tl_init failed */
    if (n > TL_MAX_BOXES) n = TL_MAX_BOXES;
    if (n < 0) n = 0;
    modern = c->settle_ms > 0;
    m = c->invert_pan ? -1 : 1;
    cap = ptz->cap_ms ? ptz->cap_ms : now_ms;

    /* A frame is stale when the camera was (or was about to be) moving while it was captured: its boxes belong to another pose. */
    stale = ptz->moving;
    if (modern && !stale) {
        if (ptz->motion_end_ms && cap < ptz->motion_end_ms + c->settle_ms) stale = 1;
        if (t->nudged && cap <= t->last_nudge_ms + START_LAG_MS) stale = 1;
        /* a nudge whose motion end is not reported yet (queued, refused, or not started): the pose is unknown, for a bounded time */
        if (t->nudged && ptz->motion_end_ms < t->last_nudge_ms && now_ms - t->last_nudge_ms < STALE_MAX_MS) stale = 1;
    }

    if (t->has_target) { /* sticky: the box nearest to the previous target in WORLD position (the camera may have turned since), inside a gate */
        int64_t best = (int64_t)c->sticky_gate_pct * c->gain_pan / 100 + 1;
        for (i = 0; i < n; i++) {
            long bw;
            int64_t d;
            if (!valid(&boxes[i])) continue;
            bw = ptz->pan + m * offset_steps(boxes[i].x1 + boxes[i].x2 - fw, fw, c->gain_pan);
            d = iabs64(bw - t->target_w);
            if (d < best) { best = d; pick = &boxes[i]; sticky = 1; }
        }
    }
    if (!pick) { /* new target: the largest box */
        for (i = 0; i < n; i++)
            if (valid(&boxes[i]) && (!pick || area(&boxes[i]) > area(pick))) pick = &boxes[i];
    }

    if (!pick) {
        if (modern && stale) return; /* a blurred frame says nothing about the person */
        t->ns = 0; /* the velocity of a person who is gone says nothing about the next one */
        t->has_target = 0;
        t->streak = 0;
        if (c->return_home && t->home_pending && t->nudged && !ptz->moving &&
            now_ms - t->last_seen_ms >= ((int64_t)c->lost_grace_s + c->return_delay_s) * 1000) {
            t->home_pending = 0;
            t->nudged = 0;
            out->kind = TL_HOME;
        }
        return;
    }

    engaged = modern && sticky && t->nudged && t->last_seen_ms && now_ms - t->last_seen_ms <= (int64_t)c->lost_grace_s * 1000;
    if (modern && !sticky) { /* a new target: no velocity from the old one, and it must be seen twice (even inside the grace) */
        t->ns = 0;
        t->streak = 0;
    }
    t->target = *pick;
    t->has_target = 1;
    t->streak++;
    t->last_seen_ms = now_ms;
    t->home_pending = 1;
    if (stale) return;

    {
        int d2x = pick->x1 + pick->x2 - fw, d2y = pick->y1 + pick->y2 - fh;
        if (modern && c->head_pct > 0) { /* tilt follows the head (the top of the box), not the centre: a close person fills the frame height */
            int bh = pick->y2 - pick->y1;
            if (pick->y1 <= HEAD_CLIP_PX) tclip = 1;
            else if ((int64_t)bh * 100 >= (int64_t)HEAD_TALL_PCT * fh || pick->y2 >= fh - HEAD_CLIP_PX) /* tall, or cut by the bottom: a near person */
                d2y = 2 * (pick->y1 - (int)((int64_t)fh * c->head_pct / 100));
            else { /* a far person: its upper third at the centre; a head already in the head band is fine too (else the camera tilting down, which
                    * shrinks a cut box into this branch, would be pulled back up: hunting) */
                int d2h = 2 * (pick->y1 - (int)((int64_t)fh * c->head_pct / 100));
                d2y = 2 * (pick->y1 + bh / 3) - fh;
                if (in_band(d2h, fh, c->tband_pct)) d2y = 0;
            }
        }
        int ox = offset_steps(d2x, fw, c->gain_pan);
        int base = m * ox, v, lead, dx;
        long w = ptz->pan + base;
        t->target_w = w;
        if (modern) push_sample(t, cap, w);
        if (t->streak < (engaged ? 1 : c->confirm_ticks)) return;
        if (t->nudged && now_ms - t->last_nudge_ms < c->min_interval_ms) return;
        if (modern && now_ms < t->hold_until) return;

        if (modern) minv = MIN_STEP;
        if (!in_band(d2x, fw, c->deadband_pct)) {
            dx = base;
            if (modern && c->lead_pct > 0 && (v = velocity(t)) != 0) {
                /* where the person will be when the camera arrives: now - capture (detector latency) + the motion time + the start lag */
                int64_t sp = beyond(d2x, fw, c->fast_pct) && c->speed_fast ? c->speed_fast : 3;
                int64_t h = (now_ms - cap) + iabs64(base) * sp + START_LAG_MS + 70;
                h = h > 1500 ? 1500 : h < 0 ? 0 : h;
                lead = (int)(v * h / 1000 * c->lead_pct / 100);
                lead = clampi(lead, -c->lead_max, c->lead_max);
                dx = base + lead;
                if (sgn(dx) != sgn(base)) dx = 0; /* it is coming to the centre by itself: hold */
            }
            dx = dx * c->gain_pct / 100;
            if (modern && t->last_sign && sgn(dx) == -t->last_sign && now_ms - t->last_nudge_ms < REVERSE_MS) dx /= 2;
            dp = clampi(dx, -c->max_step, c->max_step);
            if (iabs64(dp) < minv) dp = 0;
            if (dp && beyond(d2x, fw, c->fast_pct)) fast = 1;
        }
        if (c->tilt_enable && (tclip || !in_band(d2y, fh, c->tband_pct))) {
            int s;
            if (tclip) { /* head out of the frame: up by a step growing with the box height (a taller box = nearer = more is cut), no lead */
                s = -clampi((int)((int64_t)c->gain_tilt * (pick->y2 - pick->y1) / fh / 3), minv > MIN_STEP ? minv : MIN_STEP, c->max_step);
            } else {
                s = clampi(offset_steps(d2y, fh, c->gain_tilt) * c->gain_pct / 100, -c->max_step, c->max_step);
                if (beyond(d2y, fh, c->fast_pct)) fast = 1;
            }
            if (modern && t->last_tsign && sgn(s) == -t->last_tsign && now_ms - t->last_tnudge_ms < REVERSE_MS) s /= 2;
            if (iabs64(s) >= minv) dt = c->invert_tilt ? -s : s;
            else if (!dp) fast = 0;
        }
    }
    /* soft range: clip the resulting motor position, so a nudge never leaves [min,max]; image "below" (dt > 0) = tilt DOWN = motor -dt */
    p = clampi(ptz->pan + dp, ptz->pan_min, ptz->pan_max) - ptz->pan;
    q = clampi(ptz->tilt - dt, ptz->tilt_min, ptz->tilt_max) - ptz->tilt;
    if (iabs64(p) < minv) p = 0; /* after the clip: a nudge of a few steps at a limit would loop (a motion, scene timers, MQTT) */
    if (iabs64(q) < minv) q = 0;
    if (p == 0 && q == 0) return;
    if (modern && p && t->last_sign && sgn(p) == -t->last_sign) { /* a reversal of direction: capped (hunting) */
        int k, n2 = 0;
        for (k = 0; k < t->nrev; k++)
            if (now_ms - t->rev_ms[k] < REV_WINDOW_MS) t->rev_ms[n2++] = t->rev_ms[k];
        t->nrev = n2;
        if (t->nrev >= 2) { t->hold_until = now_ms + REV_HOLD_MS; t->nrev = 0; return; }
        t->rev_ms[t->nrev++] = now_ms;
    }
    if (modern && q && dt) { /* tilt duty cap, apart from the pan one: a 3rd reversal inside the window holds only the tilt */
        int ts = sgn(-q) * (c->invert_tilt ? -1 : 1), k, n2 = 0;
        if (now_ms < t->thold_until) q = 0;
        else if (t->last_tsign && ts == -t->last_tsign) {
            for (k = 0; k < t->ntrev; k++)
                if (now_ms - t->trev_ms[k] < REV_WINDOW_MS) t->trev_ms[n2++] = t->trev_ms[k];
            t->ntrev = n2;
            if (t->ntrev >= 2) { t->thold_until = now_ms + REV_HOLD_MS; t->ntrev = 0; q = 0; }
            else t->trev_ms[t->ntrev++] = now_ms;
        }
        if (q) { t->last_tsign = ts; t->last_tnudge_ms = now_ms; }
        if (p == 0 && q == 0) return;
    }
    out->kind = TL_NUDGE;
    out->dpan = p;
    out->dtilt = -q;
    out->speed = fast ? c->speed_fast : 0;
    t->last_nudge_ms = now_ms;
    t->nudged = 1;
    if (p) t->last_sign = sgn(p);
}
