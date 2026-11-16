/* See track_logic.h. Integer arithmetic only. */
#include "track_logic.h"
#include <string.h>

void tl_cfg_defaults(struct tl_cfg *c) {
    memset(c, 0, sizeof *c);
    c->deadband_pct = 12;
    c->gain_pan = 1000;
    c->gain_tilt = 600;
    c->max_step = 300;
    c->min_interval_ms = 1500;
    c->confirm_ticks = 2;
    c->lost_s = 10;
    c->home_enable = 1;
    c->sticky_iou_pct = 10;
}

int tl_cfg_validate(const struct tl_cfg *c) {
    if (!c) return -1;
    if (c->deadband_pct < 0 || c->deadband_pct > 50) return -1;
    if (c->gain_pan <= 0 || c->gain_pan > 100000 || c->gain_tilt <= 0 || c->gain_tilt > 100000) return -1;
    if (c->max_step <= 0 || c->max_step > 100000) return -1;
    if (c->min_interval_ms < 0 || c->confirm_ticks < 1 || c->confirm_ticks > 100) return -1;
    if (c->lost_s < 1 || c->lost_s > 86400) return -1;
    if (c->sticky_iou_pct < 0 || c->sticky_iou_pct > 100) return -1;
    if ((c->home_enable & ~1) || (c->invert_pan & ~1) || (c->invert_tilt & ~1)) return -1;
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

/* IoU in percent. */
static int iou_pct(const struct tl_box *a, const struct tl_box *b) {
    int x1 = a->x1 > b->x1 ? a->x1 : b->x1, y1 = a->y1 > b->y1 ? a->y1 : b->y1;
    int x2 = a->x2 < b->x2 ? a->x2 : b->x2, y2 = a->y2 < b->y2 ? a->y2 : b->y2;
    int64_t i, u;
    if (x2 <= x1 || y2 <= y1) return 0;
    i = (int64_t)(x2 - x1) * (y2 - y1);
    u = area(a) + area(b) - i;
    return u > 0 ? (int)(i * 100 / u) : 0;
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

/* d2 = 2*centre - size (half-pixels from the frame centre): offset fraction = d2 / (2*size). */
static int axis_step(int d2, int size, int db_pct, int gain, int maxs, int invert) {
    int64_t a = d2 < 0 ? -(int64_t)d2 : d2;
    int s;
    if (a * 100 <= (int64_t)db_pct * 2 * size) return 0;
    s = (int)((int64_t)d2 * gain / (2 * (int64_t)size));
    s = clampi(s, -maxs, maxs);
    return invert ? -s : s;
}

void tl_update(tl_t *t, int64_t now_ms, const struct tl_box *boxes, int n, int fw, int fh, const struct tl_ptz *ptz,
               struct tl_out *out) {
    const struct tl_cfg *c = &t->cfg;
    const struct tl_box *pick = 0;
    int i, best_iou = 0, dp, dt, p, q;
    out->kind = TL_NONE;
    out->dpan = out->dtilt = 0;
    if (fw <= 0 || fh <= 0 || c->max_step <= 0) return; /* bad frame or tl_init failed */
    if (n > TL_MAX_BOXES) n = TL_MAX_BOXES;
    if (n < 0) n = 0;

    if (t->has_target) { /* sticky: the box overlapping the previous target the most */
        for (i = 0; i < n; i++) {
            int o;
            if (!valid(&boxes[i])) continue;
            o = iou_pct(&t->target, &boxes[i]);
            if (o > best_iou && o >= c->sticky_iou_pct) { best_iou = o; pick = &boxes[i]; }
        }
    }
    if (!pick) { /* new target: the largest box */
        for (i = 0; i < n; i++)
            if (valid(&boxes[i]) && (!pick || area(&boxes[i]) > area(pick))) pick = &boxes[i];
    }

    if (!pick) {
        t->has_target = 0;
        t->streak = 0;
        if (c->home_enable && t->home_pending && t->nudged && !ptz->moving &&
            now_ms - t->last_seen_ms >= (int64_t)c->lost_s * 1000) {
            t->home_pending = 0;
            t->nudged = 0;
            out->kind = TL_HOME;
        }
        return;
    }

    t->target = *pick;
    t->has_target = 1;
    t->streak++;
    t->last_seen_ms = now_ms;
    t->home_pending = 1;

    if (ptz->moving || t->streak < c->confirm_ticks) return;
    if (t->nudged && now_ms - t->last_nudge_ms < c->min_interval_ms) return;

    dp = axis_step(pick->x1 + pick->x2 - fw, fw, c->deadband_pct, c->gain_pan, c->max_step, c->invert_pan);
    dt = axis_step(pick->y1 + pick->y2 - fh, fh, c->deadband_pct, c->gain_tilt, c->max_step, c->invert_tilt);
    /* soft range: clip the resulting position, so a nudge never leaves [min,max] */
    p = clampi(ptz->pan + dp, ptz->pan_min, ptz->pan_max) - ptz->pan;
    q = clampi(ptz->tilt + dt, ptz->tilt_min, ptz->tilt_max) - ptz->tilt;
    if (p == 0 && q == 0) return;
    out->kind = TL_NUDGE;
    out->dpan = p;
    out->dtilt = q;
    t->last_nudge_ms = now_ms;
    t->nudged = 1;
}
