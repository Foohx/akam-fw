/* See track_logic.h and docs/porting/lsc-rotatable-2k/tracking.md. Integer arithmetic only. */
#include "track_logic.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOSS_FRESH_MS 700  /* the exit search / sweep need this long of fresh no-box frames: never right after the camera's own move */
#define AMBIG_W_PCT 80     /* a box cut at top AND bottom and wider than this: a person extremely close, no tilt decision */
#define MIN_STEP 20        /* modern mode: a correction below this many steps (~2 deg) is not worth a motor command */
#define START_LAG_MS 80    /* a motor command takes this long to start moving: a frame captured just after it was sent still shows the old pose */
#define V_MIN 40           /* steps/s: a slower drift is noise, not a walking person */
#define SAMPLE_GAP_MS 1500 /* the velocity samples must be this close to each other */
#define REVERSE_MS 1500    /* a nudge against the previous one inside this time is halved (anti hunting) */
#define REV_WINDOW_MS 6000 /* duty cap: at most 2 reversals of direction inside this window, the 3rd is held back */
#define REV_HOLD_MS 3000   /* ... and the tracker then holds still this long */
#define HEAD_TALL_PCT 55   /* tilt: a box at least this % of the frame height (or cut by the frame bottom) is anchored on its top (head), a shorter one on its upper third */
#define HEAD_CUT_PCT 40    /* ... or at least this tall and cut by the frame bottom (a near, crouching person) */
#define WIDE_PCT 75        /* a box wider than this % of the frame width: no pan on its centre unless it touches exactly one side edge */
#define NEAR_W_PCT 50      /* a box wider than this: the pan nudge is capped */
#define NEAR_MAX_PAN 200
#define HEAD_GROW_PCT 130  /* a box area more than this % of the previous tick's (<= 0.8 s earlier): the person is walking at the camera, wait */
#define HEAD_CLIP_PX 2     /* a box top this close to the frame top is clipped by the frame: the head is out */
#define CLIP_MIN_PCT 40    /* a box touching the top edge is a head clip only if it is at least this % of the frame height: a small one is a far person on a ledge / a false positive */
#define TILT_UP_CAP 400    /* cumulative tilt UP (steps) per tracking episode, relative to the tilt at its start, unless the head clip is confirmed */
#define UP_CAP_NEAR 700    /* tilt UP budget of a near frame (bottom cut, >= NEAR_H_PCT tall, rule 1 / 2), from the tilt of the last well framed frame */
#define UP_CAP_AMB 300     /* ... of an ambiguous (rule 5) box read as a head-out by the up-trend latch, from the tilt where the latch started */
#define NEAR_H_PCT 55
#define UNCLIP_LO_PCT 5    /* a frame whose top is uncut and between these % of the height refills the origin of the near budget */
#define UNCLIP_HI_PCT 35
#define EXIT_UP_BUDGET 400 /* the UP exit search of a loss: at most this many steps up from the pose at the loss (its own budget, not the episode's cap) */
#define TBACK_NEAR_MM 1500 /* a lost target closer than this is a near one */
#define TBACK_RESTART_MS 10000 /* tilt-back does not start again this long after a box interrupted it */
#define TBACK_NEAR_S 2.5f  /* tilt-back after the loss of a near target starts after this many seconds (instead of tilt_back_s) */
#define CLIP_CONFIRM 3     /* ... consecutive fresh frames with a head clip confirm it (the person really is taller than the view) */
#define EPISODE_MS 60000   /* a person back after a loss longer than this starts a new episode (a new tilt reference) */
#define SEARCH_STEP 200    /* exit search: one nudge of this many steps, at most twice, within the grace */
#define SEARCH_MAX 2
#define SEARCH_GAP_MS 900  /* between two search nudges (the motor must have stopped and settled) */
#define EXIT_WIDE_PCT 60   /* a side / top exit is only believed for a box narrower than this: a close person flaps, it does not leave */
#define RV_GAP_MS 1500     /* robust velocity: samples further apart than this restart it */
#define RV_MIN_DIST 60     /* ... a velocity counts only after this much net travel (steps, ~6 % of the frame) over >= 3 consistent fresh samples */
#define RV_MIN_SPAN 350    /* ... spanning at least this long (ms) */
#define RV_MAX 600         /* steps/s: the fastest person the lead believes (a sprint at 2 m) */
#define NL_NEAR_MM 1200    /* close-loss machine: a lost target closer than this (or bottom cut and >= HEAD_CUT_PCT tall, or a low crop) is a near one */
#define NL_HOLD_MS 700     /* ... HOLD: pan prediction only */
#define NL_WAIT_MS 2500    /* ... WAIT (no evidence): no motion until this long after the first fresh no-box frame (close_scan = 0) */
#define NL_WAIT_SCAN_MS 1000 /* ... the same wait when close_scan = 1: the VSCAN follows */
#define NL_VS_DWELL_MS 600 /* ... VSCAN: a stop at each end of the scan, for the detector to see a steady picture */
#define NL_DOWN_MS 2500    /* ... the tracker's last tilt command (before the loss, not a scan) was DOWN this recently (a person sitting down): the scan goes down only */
#define NL_VS_DIV 5        /* ... VSCAN: tilt at rate_k / this (~220 steps/s) */
#define NL_VS_MIN 40       /* ... a side of the scan shorter than this is skipped */
#define NL_VS_MAX_MS 30000 /* ... the scan gives up after this long */
#define NL_BUDGET 400      /* ... SEARCH: 2 moves of SEARCH_MS at rate_k / 4, SEARCH_GAP_MS apart, at most this many steps from the loss pose */
#define LE_CONFIRM_MS 4000 /* ... the loss episode ends when the target has been held continuously for this long (or the head goes home): until then a re-loss never starts a new directed search / VSCAN */
#define LE_GAP_MS 1000     /* ... a gap between two accepted boxes longer than this breaks the continuous hold */
#define LE_STALE_MS 60000  /* ... an episode with no box for this long is over (a new person, a long absence) */
#define FF_H_PCT 90        /* a person box clipped at the top and at least this % of the frame height is frame-filling: the person is too close for the body box to say where to tilt */
#define FF_CAP 150         /* ... at most this many steps of tilt UP from the pose where it became frame-filling (head box or not) */
#define FF_TOP_PCT 10      /* ... a top within this % of the frame height counts as clipped for the frame-filling rule (night IR boxes 12,9,.. / 179,17,.. are frame-filling too) */
#define FF_NEAR_PCT 77     /* ... a box at least this % tall still belongs to the close sighting: the cap covers it too, and it does not end the sighting */
#define FF_CLEAR_MS 2000   /* ... the sighting (and its cap origin) ends only after this long of boxes shorter than FF_NEAR_PCT (or when the head went home) */
#define FF_HEAD_PCT 35     /* ... a head-only frame (no person box) keeps / sets last_ff only when its head box is at least this % of the frame height (a head at ~0.7 m or closer: the person fills the frame); a smaller one clears it, so a stale flag never bridges / doubles the zone of a target the head holds alone */
#define FF_RISE_MAX 300    /* a person who rises (box still top-clipped, its bottom clearly higher than the sighting's largest, on the last 3 consecutive boxes): the climb cap grows by the steps of that bottom drop, at most this much more */
#define FF_BRIDGE_MS 2500  /* a frame-filling / near person missed by the detector for less than this is no loss: nothing moves, no "lost" step (the detector flaps at close range; the presence OFF delay is 3 s) */
#define FF_GAP_MS 5000     /* ... the pose is kept over a gap of up to this long (a flapping detection is one sighting) */
#define NL_WIDEN_MIN 150   /* ... WIDEN: tilt back toward home while more than this far from it, */
#define NL_WIDEN_STOP 40   /* ... until within this, */
#define NL_WIDEN_MAX_MS 20000 /* ... at most this long, at rate_k / 6 */
#define PACE_WIN_MS 6000   /* pacing: 3 turning points of the person inside this window ... */
#define PACE_DEAD 110      /* ... while pacing the camera aims at the middle of the band and moves when it is this far (steps) from it */
#define PACE_LEG2_MS 2200  /* ... or 2 turning points this close (one short leg) */
#define PACE_TURN 100      /* ... a turning point: a move back of this many steps from the extreme */
#define PACE_GAP_MS 1500   /* ... samples further apart restart the detector */
#define PACE_SPAN_PCT 90   /* ... their positions span at most this % of the frame width (a person who leaves the view must be chased) */
#define PACE_HOLD_MS 3000  /* ... the last one at most this long ago */
#define PACE_ZONE_X10 20   /* ... the comfort zone is this x10 / 10 times wider while pacing */
#define REV_GUARD_MS 700   /* continuous: per axis, the opposite direction is refused this long after the last command in the old one (a stop / a reversal: the geared head needs to come to rest) */
#define REV_N 4            /* ... and after this many reversals ... */
#define REV_WINDOW_MS_V 4000 /* ... inside this window ... */
#define REV_HOLD_MS_V 1500 /* ... the next one is refused and the axis is held still this long */
#define REV_FAST_DIV 2     /* a reversal backed by an established robust velocity the new way (>= 3 consistent fresh samples, >= 60 steps of net travel: a person really walking back, not one flicker or a jump) waits only guard / this; the reversal count / hold still applies */
#define REV_RECENT_MS 2000 /* a change of direction counts as a reversal when the axis moved the other way less than this long before */
#define MOTOR_MIN_DIV 2
#define DIAG_MIN_DIV 3     /* both axes moving: the fastest divisor measured for a diagonal */
#define MOTOR_MAX_DIV 20
#define HEAD_X_MARGIN_PCT 10 /* head association: its centre x lies inside the person box +- this % of the box width, */
#define HEAD_Y_BOX_PCT 45    /* ... its centre y in the upper this % of the box (or up to 1.5 head heights above the box top: a partial torso box with the head over it), */
#define HEAD_GATE_X_PCT 25   /* ... without a person box: within this % of the frame width of the predicted world position of the target, */
#define HEAD_GATE_Y_PCT 35   /* ... and this % of the frame height of the last head (or box top) vertically */
#define HEAD_HOLD_MS 1500    /* after the head was the reference, a frame with a cut person box and no head keeps the vertical filter and does not use the box top for this long */
#define STALE_MAX_MS 2000  /* a frame is stale while the last nudge has no reported motion end, for at most this long */

void tl_cfg_defaults(struct tl_cfg *c) {
    memset(c, 0, sizeof *c);
    c->deadband_pct = 6;
    c->tband_pct = 10; /* the head band is half of it: +-5 % of the height */
    c->gain_pan = 1000;
    c->gain_tilt = 700;
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
    c->return_home = 1; /* guard position: back home after the grace + delay */
    c->return_delay_s = 30;
    c->head_pct = 15;
    c->tilt_return_s = 0; /* stay mode only, off by default (home returns both axes) */
    c->sticky_gate_pct = 35;
    c->zone_x_pct = 40;
    c->zone_y_pct = 30;
    c->zone_head_pct = 20;
    c->zone_stop_pct = 35;
    c->meas_px = 4;
    c->accel_mms2 = 4000;
    c->steps_rad = 654;
    c->person_h_mm = 1700;
    c->shoulder_mm = 450;
    c->full_pct = 85;
    c->predict_ms = 500;
    c->motion = TL_MOTION_CONT; /* tracker falls back to steps when the motor layer has no continuous mode */
    c->lease_ms = 450;
    c->rate_k = 1100;
    c->gap_predict_ms = 1500;
    c->lag_ms = 50;
    c->head_row_pct = 28;
    c->head_band_pct = 12;
    c->head_meas_px = 3;
    c->head_mm = 240;
    c->lost_n = -1;
    c->tilt_back_s = 5;
    c->tilt_back_min = 250;
    c->close_scan = 1;
    c->close_scan_up = 250;
    c->close_scan_down = 300;
    c->close_scan_cooldown_s = 30;
    c->close_bridge_ms = FF_BRIDGE_MS;
    c->rev_guard_ms = REV_GUARD_MS;
    c->rev_n = REV_N;
    c->rev_window_ms = REV_WINDOW_MS_V;
    c->rev_hold_ms = REV_HOLD_MS_V;
    c->search_mode = TL_SEARCH_LEGACY;
    c->pass_s = 4;
    c->pass_home_s = 3;
    c->search_cooldown_s = 30;
    c->reacq_s = 8;
}

void tl_cfg_dev(struct tl_cfg *c) {
    tl_cfg_defaults(c);
    c->zone_x_pct = c->zone_y_pct = c->zone_head_pct = 0;
    c->motion = TL_MOTION_STEPS;
}

typedef char tl_gain_pan_check[(TL_2K_GAIN_PAN == TL_2K_FRAME_W * TL_2K_STEPS_PER_PX_X10 / 10) ? 1 : -1];
typedef char tl_gain_tilt_check[(TL_2K_GAIN_TILT == TL_2K_FRAME_H * TL_2K_STEPS_PER_PX_X10 / 10) ? 1 : -1];
void tl_cfg_geometry_2k(struct tl_cfg *c) {
    c->steps_rad = c->steps_rad * TL_2K_GAIN_PAN / c->gain_pan; /* same angles: the radian scales with the pan gain (654 -> 418) */
    c->gain_pan = TL_2K_GAIN_PAN;
    c->gain_tilt = TL_2K_GAIN_TILT;
}

void tl_cfg_legacy(struct tl_cfg *c) {
    tl_cfg_defaults(c);
    c->deadband_pct = 12;
    c->gain_tilt = 600; /* the 0.1.0 values */
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
    c->tilt_return_s = 0;
    c->zone_x_pct = c->zone_y_pct = c->zone_head_pct = 0;
    c->motion = TL_MOTION_STEPS;
}

/* ---- the lost-target chain text ---- */
static int lost_num(const char *s, int lo, int hi, int *out) {
    int v = 0, d = 0;
    for (; *s >= '0' && *s <= '9'; s++) { if (++d > 5) return -1; v = v * 10 + (*s - '0'); }
    if (!d || *s || v < lo || v > hi) return -1;
    *out = v;
    return 0;
}

static int lost_name_ok(const char *s) {
    int n = 0;
    if (*s < 'a' || *s > 'z') return 0;
    for (; *s; s++, n++)
        if (!((*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') || *s == '_')) return 0;
    return n >= 1 && n <= 16;
}

int tl_lost_parse(const char *s, struct tl_cfg *c) {
    struct tl_step st[TL_MAX_STEPS];
    int n = 0, i;
    if (!s || !*s) return -1;
    if (!strcmp(s, "none")) { c->lost_n = 0; return 0; }
    memset(st, 0, sizeof st);
    for (;;) {
        char tok[48], *part[4];
        int np = 0, len = 0;
        struct tl_step *p;
        while (s[len] && s[len] != ',') len++;
        if (!len || len >= (int)sizeof tok || n >= TL_MAX_STEPS) return -1;
        memcpy(tok, s, (size_t)len);
        tok[len] = 0;
        part[np++] = tok;
        for (i = 0; i < len; i++)
            if (tok[i] == ':') { if (np >= 4) return -1; tok[i] = 0; part[np++] = tok + i + 1; }
        p = &st[n];
        if (!strcmp(part[0], "backsweep") && np == 1) p->kind = TLS_BACKSWEEP;
        else if (!strcmp(part[0], "exit") && np == 1) p->kind = TLS_EXIT;
        else if (!strcmp(part[0], "home") && np == 1) p->kind = TLS_HOME;
        else if (!strcmp(part[0], "stay") && np == 1) p->kind = TLS_STAY;
        else if (!strcmp(part[0], "hold") && np == 2) { p->kind = TLS_HOLD; if (lost_num(part[1], 1, 3600, &p->a)) return -1; }
        else if (!strcmp(part[0], "local") && np == 4) {
            p->kind = TLS_LOCAL;
            if (lost_num(part[1], 40, 1500, &p->a) || lost_num(part[2], MOTOR_MIN_DIV, MOTOR_MAX_DIV, &p->b) || lost_num(part[3], 1, 600, &p->c)) return -1;
        } else if (!strcmp(part[0], "route") && (np == 2 || np == 3)) {
            p->kind = TLS_ROUTE;
            if (!lost_name_ok(part[1])) return -1;
            strcpy(p->name, part[1]);
            p->b = 60;
            if (np == 3 && lost_num(part[2], 1, 3600, &p->b)) return -1;
        } else return -1;
        if (n > 0 && st[n - 1].kind == TLS_STAY) return -1; /* stay ends the chain: nothing may follow */
        n++;
        s += len;
        if (!*s) break;
        s++;
        if (!*s) return -1; /* a trailing comma */
    }
    /* the default chain is the legacy policy */
    if (n == 4 && st[0].kind == TLS_BACKSWEEP && st[1].kind == TLS_EXIT && st[2].kind == TLS_HOLD && st[2].a == 30 && st[3].kind == TLS_HOME) {
        c->lost_n = -1;
        return 0;
    }
    memcpy(c->lost, st, sizeof st);
    c->lost_n = n;
    return 0;
}

void tl_lost_format(const struct tl_cfg *c, char *out, size_t cap) {
    int i;
    size_t k = 0;
    if (!cap) return;
    if (c->lost_n < 0) { snprintf(out, cap, "%s", TL_LOST_DEFAULT); return; }
    if (c->lost_n == 0) { snprintf(out, cap, "none"); return; }
    out[0] = 0;
    for (i = 0; i < c->lost_n && k + 1 < cap; i++) {
        const struct tl_step *p = &c->lost[i];
        int w = 0;
        if (i) out[k++] = ',';
        switch (p->kind) {
        case TLS_HOLD: w = snprintf(out + k, cap - k, "hold:%d", p->a); break;
        case TLS_BACKSWEEP: w = snprintf(out + k, cap - k, "backsweep"); break;
        case TLS_EXIT: w = snprintf(out + k, cap - k, "exit"); break;
        case TLS_LOCAL: w = snprintf(out + k, cap - k, "local:%d:%d:%d", p->a, p->b, p->c); break;
        case TLS_ROUTE: w = snprintf(out + k, cap - k, "route:%s:%d", p->name, p->b); break;
        case TLS_HOME: w = snprintf(out + k, cap - k, "home"); break;
        default: w = snprintf(out + k, cap - k, "stay"); break;
        }
        if (w < 0 || (size_t)w >= cap - k) break;
        k += (size_t)w;
    }
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
    if (c->tilt_return_s < 0 || c->tilt_return_s > 3600) return -1;
    if (c->sticky_gate_pct < 0 || c->sticky_gate_pct > 100) return -1;
    if (c->zone_x_pct < 0 || c->zone_x_pct > 90 || c->zone_y_pct < 0 || c->zone_y_pct > 90 || c->zone_head_pct < 0 || c->zone_head_pct > 90) return -1;
    if (c->zone_stop_pct < 5 || c->zone_stop_pct > 100 || c->meas_px < 1 || c->meas_px > 40 || c->accel_mms2 < 200 || c->accel_mms2 > 10000 || c->steps_rad < 300 || c->steps_rad > 1500 || c->person_h_mm < 1000 || c->person_h_mm > 2500 || c->shoulder_mm < 250 || c->shoulder_mm > 800 || c->full_pct < 50 || c->full_pct > 100) return -1;
    if (c->predict_ms < 0 || c->predict_ms > 3000 || (c->motion != TL_MOTION_STEPS && c->motion != TL_MOTION_CONT)) return -1;
    if (c->lease_ms < 150 || c->lease_ms > 2000 || c->rate_k < 200 || c->rate_k > 5000 || c->gap_predict_ms < 0 || c->gap_predict_ms > 5000 || c->lag_ms < -150 || c->lag_ms > 300) return -1;
    if (c->head_row_pct < 5 || c->head_row_pct > 60 || c->head_band_pct < 2 || c->head_band_pct > 40 || c->head_meas_px < 1 || c->head_meas_px > 40 || c->head_mm < 120 || c->head_mm > 400) return -1;
    if (c->lost_n < -1 || c->lost_n > TL_MAX_STEPS || c->tilt_back_s < 0 || c->tilt_back_s > 600 || c->tilt_back_min < 40 || c->tilt_back_min > 1500) return -1;
    if ((c->close_scan & ~1) || c->close_scan_up < 0 || c->close_scan_up > 600 || c->close_scan_down < 0 || c->close_scan_down > 600 || c->close_scan_cooldown_s < 0 || c->close_scan_cooldown_s > 600 || c->close_bridge_ms < 0 || c->close_bridge_ms > 10000) return -1;
    if (c->rev_guard_ms < 0 || c->rev_guard_ms > 5000 || c->rev_n < 1 || c->rev_n > TL_REV_MAX || c->rev_window_ms < 1000 || c->rev_window_ms > 60000 || c->rev_hold_ms < 0 || c->rev_hold_ms > 60000) return -1;
    if ((c->search_mode != TL_SEARCH_LEGACY && c->search_mode != TL_SEARCH_MOTION) || c->pass_s < 0 || c->pass_s > 60 || c->pass_home_s < 0 || c->pass_home_s > 600 || c->search_cooldown_s < 0 || c->search_cooldown_s > 3600 || c->reacq_s < 0 || c->reacq_s > 60) return -1;
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

/* The head rule of a box: 0 = centre of the box (head_pct 0), 1 = head clipped at the top, 2 = near (top anchored), 3 = far (upper third).
 * *ty = the row aimed at. Used for the decision and, before the stale return, for the debug record. */
static int head_rule(tl_t *t, const struct tl_box *b, int fw, int fh, int *ty) {
    const struct tl_cfg *c = &t->cfg;
    int bh = b->y2 - b->y1;
    if (c->head_pct <= 0) { *ty = fh / 2; return 0; }
    *ty = (int)((int64_t)fh * c->head_pct / 100);
    t->amb_up = 0;
    if (b->y1 > HEAD_CLIP_PX) t->amb_latch = 0; /* an uncut top again: the climb is over */
    if (c->zone_x_pct > 0 && c->settle_ms > 0) { /* targeting policy: the whole body (not cut by the frame, shorter than full_pct) is aimed at by its centre, a close or cut one by its head */
        int cut_bot = b->y2 >= fh - HEAD_CLIP_PX, thr = t->near_mode ? c->full_pct - 8 : c->full_pct; /* hysteresis: no flapping between the two near the threshold */
        if (b->y1 <= HEAD_CLIP_PX && cut_bot && (int64_t)(b->x2 - b->x1) * 100 > (int64_t)AMBIG_W_PCT * fw) {
            t->near_mode = 1;
            if (t->amb_latch || (t->up_ms && t->tnow - t->up_ms <= 1500)) { t->amb_up = 1; t->amb_latch = 1; return 1; } /* the head had been rising: a real head-out (bounded by the sky-climb cap) */
            return 5;
        }
        if (b->y1 <= HEAD_CLIP_PX && (int64_t)bh * 100 >= (int64_t)CLIP_MIN_PCT * fh) { t->near_mode = 1; if (t->amb_latch) t->amb_up = 1; return 1; } /* after an ambiguous climb a cut top stays unconfirmed (the sky-climb cap holds) */
        if ((int64_t)bh * 100 >= (int64_t)thr * fh || (cut_bot && (int64_t)bh * 100 >= (int64_t)HEAD_CUT_PCT * fh)) { t->near_mode = 1; return 2; }
        if (cut_bot && b->y1 > HEAD_CLIP_PX) { t->near_mode = 1; return 7; } /* a low crop (top uncut, bottom cut, short): the top is the reference, never the centre of the box (rule 4) */
        t->near_mode = 0;
        *ty = fh / 2;
        return 4;
    }
    if (b->y1 <= HEAD_CLIP_PX && (int64_t)bh * 100 >= (int64_t)CLIP_MIN_PCT * fh) return 1;
    if ((int64_t)bh * 100 >= (int64_t)HEAD_TALL_PCT * fh || (b->y2 >= fh - HEAD_CLIP_PX && (int64_t)bh * 100 >= (int64_t)HEAD_CUT_PCT * fh)) return 2;
    *ty = fh / 2;
    return 3;
}

/* ---- robust velocity of the followed target (fresh detections only): the median of the last slopes, believed only when the last 2 slopes agree with it
 * in direction and the net travel / span are large enough (a feint of 1-2 ticks or a jittering box gives 0); when the target slows down (the last slope is
 * less than 40 % of the median) the velocity drops to the last slope at once, a reversal gives 0 ---- */
static int rv_axis(const tl_t *t, const long *w) {
    int n = t->vn, i, m, k;
    long s[4], tmp, med, last;
    int64_t span;
    if (n < 3) return 0;
    span = t->vt[n - 1] - t->vt[n - 3];
    if (span < RV_MIN_SPAN || iabs64(w[n - 1] - w[n - 3]) < RV_MIN_DIST) return 0;
    m = n - 1 > 4 ? 4 : n - 1;
    for (i = 0; i < m; i++) {
        int j = n - m + i;
        s[i] = (long)((w[j] - w[j - 1]) * 1000 / (t->vt[j] - t->vt[j - 1]));
    }
    last = s[m - 1];
    {   /* median of the m slopes (insertion sort of a copy) */
        long q[4];
        for (i = 0; i < m; i++) q[i] = s[i];
        for (i = 1; i < m; i++) for (k = i; k > 0 && q[k - 1] > q[k]; k--) { tmp = q[k]; q[k] = q[k - 1]; q[k - 1] = tmp; }
        med = (q[(m - 1) / 2] + q[m / 2]) / 2;
    }
    if (med == 0 || sgn((int)med) != sgn((int)last) || sgn((int)med) != sgn((int)s[m - 2 < 0 ? 0 : m - 2])) return 0;
    if (iabs64(last) * 100 < iabs64(med) * 40) med = last;     /* decelerating: follow the last slope */
    else if (iabs64(last) < iabs64(med)) med = last;           /* never faster than the latest slope */
    if (iabs64(med) < V_MIN) return 0;
    return (int)(med > RV_MAX ? RV_MAX : med < -RV_MAX ? -RV_MAX : med);
}

static void rv_reset(tl_t *t) { t->vn = 0; t->vx = t->vy = 0; }

static void rv_push(tl_t *t, int64_t cap, long wx, long wy) {
    if (t->vn > 0 && (cap - t->vt[t->vn - 1] > RV_GAP_MS || cap <= t->vt[t->vn - 1])) t->vn = 0;
    if (t->vn == 6) {
        memmove(&t->vt[0], &t->vt[1], sizeof t->vt[0] * 5);
        memmove(&t->vwx[0], &t->vwx[1], sizeof t->vwx[0] * 5);
        memmove(&t->vwy[0], &t->vwy[1], sizeof t->vwy[0] * 5);
        t->vn--;
    }
    t->vt[t->vn] = cap;
    t->vwx[t->vn] = wx;
    t->vwy[t->vn] = wy;
    t->vn++;
    t->vx = rv_axis(t, t->vwx);
    t->vy = rv_axis(t, t->vwy);
}

/* The pacing detector: the turning points of the filtered pan position of the person (a move back of PACE_TURN steps from the extreme so far; a stand-still jitter never counts).
 * Paces = 3 turning points inside PACE_WIN_MS, the last PACE_HOLD_MS ago at most, their positions (the extremes) spanning at most PACE_SPAN_PCT of the frame: a person walking back
 * and forth inside the view. The camera does not chase it (a wide zone, no feed-forward, no reversals: the fewest starts / stops / reversals for an open-loop stepper). */
static void pace_turn(tl_t *t) {
    t->pace_t[0] = t->pace_t[1]; t->pace_t[1] = t->pace_t[2]; t->pace_t[2] = t->pace_et;
    t->pace_w[0] = t->pace_w[1]; t->pace_w[1] = t->pace_w[2]; t->pace_w[2] = t->pace_ext;
}

static void pace_push(tl_t *t, int64_t cap, long w) {
    if (!t->pace_n || cap - t->pace_last > PACE_GAP_MS) { memset(t->pace_t, 0, sizeof t->pace_t); t->pace_dir = 0; t->pace_ext = w; t->pace_et = cap; t->pace_n = 1; }
    t->pace_last = cap;
    if (t->pace_dir == 0) {
        if (iabs64(w - t->pace_ext) >= PACE_TURN) { t->pace_dir = w > t->pace_ext ? 1 : -1; t->pace_ext = w; t->pace_et = cap; }
    } else if (t->pace_dir * (w - t->pace_ext) > 0) {
        t->pace_ext = w; t->pace_et = cap;
    } else if (iabs64(w - t->pace_ext) >= PACE_TURN) {
        pace_turn(t);
        t->pace_dir = -t->pace_dir; t->pace_ext = w; t->pace_et = cap;
    }
}

/* 1 while pacing; *mid = the middle of the extremes (pan, motor steps): the camera aims there, not at the person. */
static int pacing(const tl_t *t, int64_t now, int fw_steps, long *mid) {
    long lo, hi;
    int i, from;
    if (t->pace_t[2] <= 0 || now - t->pace_t[2] > PACE_HOLD_MS) return 0;
    if (t->pace_t[0] > 0 && t->pace_t[2] - t->pace_t[0] <= PACE_WIN_MS) from = 0;      /* 3 turning points inside the window */
    else if (t->pace_t[1] > 0 && t->pace_t[2] - t->pace_t[1] <= PACE_LEG2_MS) from = 1; /* or 2 with a short leg between them (a fast pacer is known after one leg) */
    else return 0;
    lo = hi = t->pace_w[from];
    for (i = from + 1; i < 3; i++) { if (t->pace_w[i] < lo) lo = t->pace_w[i]; if (t->pace_w[i] > hi) hi = t->pace_w[i]; }
    *mid = (lo + hi) / 2;
    return hi - lo <= (long)fw_steps * PACE_SPAN_PCT / 100;
}

/* ---- the 3D state of the followed person (see tracking.md "Person state"): distance from the box (monocular), angles from the pose at capture, one constant-velocity
 * Kalman filter per axis; the lateral velocity is in mm/s, so the angular rate is v * steps_rad / distance ---- */
#define KF_GATE2 25.0f     /* innovation gate: 5 sigma */
#define KF_V0 2000.0f      /* initial std of the lateral / depth velocity, mm/s */
#define CAP_UNC_MS 70.0f   /* uncertainty of the capture time against the driver samples (video latency model, ring interpolation), ms */
#define CAM_MOVING 60      /* steps/s: the camera counts as moving at the capture of a frame above this */
#define NEAR_MM 800        /* closer than this the box / head row is unreliable (the person fills the frame) */
#define KF_D_MIN 300.0f
#define KF_ANG_ACC 2000.0f /* cap of the angular process noise, steps/s^2 (a person at 0.6 m does not accelerate the angle by 4000 mm/s^2 * 654 / 600 = 4360 steps/s^2) */
#define KF_SHOULDER_MAX_MM 700.0f /* the widest a person gets (arms out): every box bounds the distance by it */
#define KF_D_MAX 12000.0f
#define KF_D_DEF 3000.0f   /* distance assumed until a box gives one */

static void kf1_init(struct tl_kf1 *k, float a, float r) { k->a = a; k->v = 0.0f; k->paa = r; k->pav = 0.0f; k->pvv = KF_V0 * KF_V0; }

/* cs = steps per (mm/s) = steps_rad / distance; dt seconds; acc = process noise (mm/s^2) */
static void kf1_predict(struct tl_kf1 *k, float cs, float dt, float acc) {
    float cdt = cs * dt, qv = acc * dt, qa = cs * acc * dt * dt * 0.5f;
    qv *= qv;
    qa *= qa;
    k->a += cdt * k->v;
    k->paa += 2.0f * cdt * k->pav + cdt * cdt * k->pvv + qa;
    k->pav += cdt * k->pvv + cdt * 0.5f * qv;
    k->pvv += qv;
}

/* 1 = rejected by the gate (the state is untouched) */
static int kf1_update(struct tl_kf1 *k, float z, float r, int gate) {
    float y = z - k->a, s = k->paa + r, g0, g1, paa = k->paa, pav = k->pav;
    if (gate && y * y > KF_GATE2 * s) return 1;
    g0 = paa / s;
    g1 = pav / s;
    k->a += g0 * y;
    k->v += g1 * y;
    k->paa = paa - g0 * paa;
    k->pav = pav - g0 * pav;
    k->pvv -= g1 * pav;
    return 0;
}

static void kfd_predict(struct tl_kfd *k, float dt, float acc) {
    float qv = acc * dt;
    qv *= qv;
    k->d += dt * k->v;
    k->pdd += 2.0f * dt * k->pdv + dt * dt * k->pvv + qv * dt * dt * 0.25f;
    k->pdv += dt * k->pvv + dt * 0.5f * qv;
    k->pvv += qv;
    if (k->d < KF_D_MIN) k->d = KF_D_MIN;
    if (k->d > KF_D_MAX) k->d = KF_D_MAX;
}

static void kfd_update(struct tl_kfd *k, float z, float r) {
    float y = z - k->d, s = k->pdd + r, g0, g1, pdd = k->pdd, pdv = k->pdv;
    if (y * y > KF_GATE2 * s) return; /* an inconsistent distance cue (partial box): ignored */
    g0 = pdd / s;
    g1 = pdv / s;
    k->d += g0 * y;
    k->v += g1 * y;
    k->pdd = pdd - g0 * pdd;
    k->pdv = pdv - g0 * pdv;
    k->pvv -= g1 * pdv;
}

static int roundf_i(float x) { return (int)(x < 0.0f ? x - 0.5f : x + 0.5f); }
static float fsqrt_(float x) { float r = x > 1.0f ? x : 1.0f; int i; if (x <= 0.0f) return 0.0f; for (i = 0; i < 12; i++) r = 0.5f * (r + x / r); return r; }
/* debug only: the innovation z - a normalised by its standard deviation, in 1/100 sigma */
static int kf1_nis100(const struct tl_kf1 *k, float z, float r) { float s = k->paa + r; return s > 0.0f ? roundf_i(100.0f * (z - k->a) / fsqrt_(s)) : 0; }

/* Folds a fresh box into the state. w / wy: the world position of the reference (motor steps, motor sign), rule: the reference rule, newtgt: a new target.
 * Returns 1 when the box is an inconsistent outlier (rejected twice in a row is accepted: the person really jumped). Sets sm_wx / sm_wy / vx / vy / dist_mm. */
/* rule 6: b is the HEAD box (head centre, 3 px noise, distance from the head height). noy: a pan-only measurement (a cut person box while the vertical reference is the head: the vertical filter, its rule and the vertical measurement are left alone). */
static int kf_measure(tl_t *t, const struct tl_box *b, int rule, int wide, int fw, int fh, int64_t cap, long w, long wy, int newtgt, int noy) {
    const struct tl_cfg *c = &t->cfg;
    t->kf_note = 0;
    float dt = 0.2f, acc = (float)c->accel_mms2, fv = (float)fh * c->steps_rad / c->gain_tilt, fhz = (float)fw * c->steps_rad / c->gain_pan;
    float dm = 0.0f, dr = 0.0f, D, cs, jx = (float)c->meas_px * c->gain_pan / fw, jy = (float)c->meas_px * c->gain_tilt / fh, qx = 1.0f, qy = 1.0f, rx, ry, ox, oy;
    int bh = b->y2 - b->y1, bw = b->x2 - b->x1, cut_t = b->y1 <= HEAD_CLIP_PX, cut_b = b->y2 >= fh - HEAD_CLIP_PX, cut_s = b->x1 <= HEAD_CLIP_PX || b->x2 >= fw - HEAD_CLIP_PX;
    {   /* measurement noise: the detector jitter scales with the box (5 % of its height for the top, 4 % of its width for the centre), never below meas_px */
        float sy = 0.05f * (float)bh, sx = 0.04f * (float)bw;
        jy = (sy > (float)c->meas_px ? sy : (float)c->meas_px) * c->gain_tilt / fh;
        jx = (sx > (float)c->meas_px ? sx : (float)c->meas_px) * c->gain_pan / fw;
        if (rule == 6) { jy = (float)c->head_meas_px * c->gain_tilt / fh; jx = (float)c->head_meas_px * c->gain_pan / fw; } /* the head centre: a fixed detector noise */
    }
    if (!newtgt && t->sm_valid && t->kf_t && cap > t->kf_t) dt = (float)(cap - t->kf_t) / 1000.0f;
    if (dt > 1.5f && (newtgt || !t->sm_valid)) newtgt = 1;
    if (dt > 1.5f && !newtgt) { /* the same person after a gap (a few seconds without a box): the lateral / vertical filters restart, the DISTANCE is kept (with a wide variance) */
        t->sm_valid = 0;
        if (t->kd_valid) { t->kd.pdd = (0.5f * t->kd.d) * (0.5f * t->kd.d); t->kd.v = 0.0f; t->kd.pdv = 0.0f; t->kd.pvv = KF_V0 * KF_V0; }
    }
    if (rule == 6) { if (!cut_t && !cut_b) { dm = (float)c->head_mm * fv / (float)bh; dr = 0.2f * dm; t->dbg.dcue = "head"; } } /* a whole head (not clipped by the frame): head_mm / its height, +-20 % */
    else if (!cut_t && !cut_b && (rule == 4 || rule == 0)) { dm = (float)c->person_h_mm * fv / (float)bh; dr = 0.2f * dm; t->dbg.dcue = "h"; }                /* whole person: the height cue, +-20 % */
    else if (!cut_s && (int64_t)bw * 100 < (int64_t)60 * fw) { dm = (float)c->shoulder_mm * fhz / (float)bw; dr = 0.4f * dm; t->dbg.dcue = "w"; }     /* cut: the width cue, weaker */
    if (dm > KF_D_MAX) dm = KF_D_MAX;
    if (dm > 0.0f && dm < KF_D_MIN) dm = KF_D_MIN;
    if (newtgt || !t->kd_valid) {
        if (dm > 0.0f) { t->kd.d = dm; t->kd.pdd = dr * dr; t->kd.v = 0.0f; t->kd.pdv = 0.0f; t->kd.pvv = KF_V0 * KF_V0; t->kd_valid = 1; t->kd_t = cap; }
        else if (newtgt && t->kd_valid && cap - t->kd_t < 60000) { /* a box with no distance cue (cut, close): the last estimate is CARRIED (wide variance), not reset to the 3 m default */
            t->kd.pdd = (0.5f * t->kd.d) * (0.5f * t->kd.d);
            t->kd.v = 0.0f;
            t->kd.pdv = 0.0f;
            t->kd.pvv = KF_V0 * KF_V0;
        } else if (newtgt) t->kd_valid = 0;
    } else {
        kfd_predict(&t->kd, dt, acc);
        if (dm > 0.0f) { kfd_update(&t->kd, dm, dr * dr); t->kd_t = cap; }
    }
    {   /* every box bounds the distance: a person is at most person_h tall and 700 mm wide, so a box cut at both ends (the person fills the frame) is never farther than ~1.6 m */
        float bnd = (float)c->person_h_mm * fv / (float)bh, bw_mm = KF_SHOULDER_MAX_MM * fhz / (float)bw;
        if (bw_mm < bnd) bnd = bw_mm;
        if (rule == 6) bnd = KF_D_MAX; /* b is a head: the person bounds do not apply */
        if (bnd < KF_D_MIN) bnd = KF_D_MIN;
        if (!t->kd_valid) { t->kd.d = bnd < KF_D_DEF ? bnd : KF_D_DEF; t->kd.pdd = (0.5f * t->kd.d) * (0.5f * t->kd.d); t->kd.v = 0.0f; t->kd.pdv = 0.0f; t->kd.pvv = KF_V0 * KF_V0; t->kd_valid = 1; t->kd_t = cap; }
        else if (t->kd.d > bnd) t->kd.d = bnd;
    }
    D = t->kd.d;
    t->dist_mm = (int)D;
    { float amax = KF_ANG_ACC * D / (float)c->steps_rad; if (amax < acc) acc = amax; } /* the angular process noise is capped: effective accel = min(accel_mms2, 2000 * D / steps_rad) */
    if (dm <= 0.0f) t->dbg.dcue = t->kd_valid ? "carried" : "none";
    cs = (float)c->steps_rad / D;
    if (wide) qx = 4.0f; else if (cut_s) qx = 3.0f;
    if (rule == 5) { qx = 6.0f; qy = 8.0f; } else if (rule == 1) qy = 8.0f; else if (rule == 4) qy = 2.0f; else if (rule == 6 && cut_t) qy = 3.0f;
    rx = jx * qx;
    rx *= rx;
    ry = jy * qy;
    ry *= ry;
    { /* the pose at capture is read from the driver samples with a video latency that is a constant of the model (lag_ms): while the camera moves, an error of CAP_UNC_MS in it shifts the measurement by rate x CAP_UNC_MS (and
       * a change of rate makes a velocity pulse out of it). The measurement noise grows with the camera rate: the filter trusts the frame less, it does not turn the error into a speed. */
        float ux = (float)(t->cam_rx < 0 ? -t->cam_rx : t->cam_rx) * CAP_UNC_MS / 1000.0f, uy = (float)(t->cam_ry < 0 ? -t->cam_ry : t->cam_ry) * CAP_UNC_MS / 1000.0f;
        rx += ux * ux;
        ry += uy * uy;
    }
    t->dbg.rsx = roundf_i(fsqrt_(rx)); t->dbg.rsy = roundf_i(fsqrt_(ry));
    if (newtgt || !t->sm_valid) {
        kf1_init(&t->kx, (float)w, rx);
        kf1_init(&t->ky, (float)wy, ry);
        t->kf_rej = 0;
    } else {
        kf1_predict(&t->kx, cs, dt, acc);
        kf1_predict(&t->ky, cs, dt, acc);
        t->dbg.innx = kf1_nis100(&t->kx, (float)w, rx);
        if (!noy && t->sm_rule == rule) t->dbg.inny = kf1_nis100(&t->ky, (float)wy, ry);
        if (!noy && t->sm_rule != rule) kf1_init(&t->ky, (float)wy, ry); /* another reference point (head <-> body): the vertical filter restarts */
        if (t->kf_force && ((float)w - t->kx.a) * ((float)w - t->kx.a) > KF_GATE2 * (t->kx.paa + rx)) { /* the same close person (close_same), but the centre of its body box wandered beyond the gate. The lateral
                            * filter takes the measurement with a 25 x larger variance, position only and no velocity (the wandering says nothing about it): it is neither held nor
                            * re-initialised (a re-init on each rejected measurement made the pan chase the centre) */
            float yy = (float)w - t->kx.a, ri = 25.0f * rx, g;
            g = t->kx.paa / (t->kx.paa + ri);
            t->kf_note = 2;
            t->kx.a += g * yy;
            t->kx.paa -= t->kx.paa / (t->kx.paa + rx) * t->kx.paa; /* the variance shrinks as for a normal measurement: the gate stays tight for a later frame of another person */
            t->kx.v = 0.0f;
            t->kx.pav = 0.0f;
            if (t->kx.pvv > 300.0f * 300.0f) t->kx.pvv = 300.0f * 300.0f; /* the wandering says nothing about the velocity, and the next accepted frame must not turn the jump into a speed */
            t->kf_rej = 0;
        } else if (kf1_update(&t->kx, (float)w, rx, 1)) {
            if (++t->kf_rej < 2) { t->kf_note = 1; return 1; } /* inconsistent with the track: held until a second frame agrees */
            t->kf_note = 2;
            kf1_init(&t->kx, (float)w, rx);
            if (!noy) kf1_init(&t->ky, (float)wy, ry);
            t->kf_rej = 0;
        } else {
            t->kf_rej = 0;
        }
        if (!noy && t->sm_rule == rule) (void)kf1_update(&t->ky, (float)wy, ry, 0);
    }
    if (t->kf_note != 2) t->kf_note = 0;
    t->sm_valid = 1;
    if (!noy) t->sm_rule = rule;
    t->kf_t = cap;
    t->sm_wx = roundf_i(t->kx.a);
    t->sm_wy = roundf_i(t->ky.a);
    ox = cs * t->kx.v;
    oy = cs * t->ky.v;
    t->kox = roundf_i(ox);
    t->koy = roundf_i(oy);
    return 0;
}

/* The velocity the lead / prediction / control believe: the Kalman angular rate, but only once the robust estimator (>= 3 consistent fresh samples, a minimum travel) agrees
 * in direction, and never more than twice its magnitude (a feint is not extrapolated; it fades when the person slows). */
static int vel_gate(int kal, int rob) {
    int s = sgn(rob), a;
    if (!s || sgn(kal) != s) return 0;
    a = (int)iabs64(kal);
    if (a > 2 * (int)iabs64(rob)) a = 2 * (int)iabs64(rob);
    if (a > RV_MAX) a = RV_MAX;
    return s * a;
}

static void hist_push(tl_t *t, const struct tl_box *b) {
    if (t->hn == 4) { memmove(&t->hist[0], &t->hist[1], sizeof t->hist[0] * 3); t->hn--; }
    t->hist[t->hn++] = *b;
}

#define SWAP_MS 1000       /* targeting policy: a box that does not match the followed target is adopted only after the target was absent this long */
#define JUMP_LO_PCT 10     /* a box top / centre that jumps more than clamp(0.6 head heights, 10 %, 30 % of the frame height) (world terms) vs the followed track is an outlier until confirmed */
#define JUMP_HI_PCT 30
#define HEAD_MM 250        /* a head is about this tall: the jump threshold scales with it, so a close person (a big head) tolerates a bigger jump of the box top */
#define BACK_WINDOW_MS 3000 /* a person lost within this long after a large move is searched for between the pose before it and the current one */
#define SWEEP_DIV 4        /* continuous sweep back: speed divisor (~275 steps/s) */
#define V_REF_DIV_MAX 20

static void dw_push(tl_t *t, long v, int cut, int fh) {
    float jy = (float)t->cfg.meas_px * (float)t->cfg.gain_tilt / (float)fh, uy = (float)(t->cam_ry < 0 ? -t->cam_ry : t->cam_ry) * CAP_UNC_MS / 1000.0f;
    if (t->dw_n == 4) { memmove(&t->dw[0], &t->dw[1], sizeof t->dw[0] * 3); memmove(&t->dwc[0], &t->dwc[1], sizeof t->dwc[0] * 3); memmove(&t->dws2[0], &t->dws2[1], sizeof t->dws2[0] * 3); t->dw_n--; }
    t->dwc[t->dw_n] = cut;
    t->dws2[t->dw_n] = jy * jy + uy * uy;
    t->dw[t->dw_n++] = v;
}

/* The head really rose before the frame cut it (an approaching person: the top of the box climbs over >= 3 fresh samples, the two older ones not cut, about 12 px in all): a box
 * cut at top AND bottom after that is a head-out to follow, not an ambiguous close person. */
static int up_trend(const tl_t *t, int fh) {
    int n = t->dw_n;
    long need = (long)12 * t->cfg.gain_tilt / fh, step = need / 2;
    if (n < 3 || t->dwc[n - 3] || t->dwc[n - 2]) return 0;
    return t->dw[n - 3] - t->dw[n - 1] >= need && t->dw[n - 3] - t->dw[n - 2] >= step && t->dw[n - 2] - t->dw[n - 1] >= step;
}

/* the Kalman state says the reference is moving DOWN (the person sits / crouches / walks away down the frame): motor sign of down is kt. The filter already trusts a frame less while the camera
 * moves (the measurement noise grows with the camera rate), so no "camera still" condition here. */
static int kf_down(const tl_t *t) {
    int kt = t->cfg.invert_tilt ? 1 : -1;
    return t->sm_valid && t->koy * kt >= 180;
}

/* the top of the followed boxes really sank (world terms, >= 3 fresh samples): a single ambiguous box never proves a downward exit. Each sample has its uncertainty
 * sigma_i^2 = jitter^2 + (camera rate_i x CAP_UNC_MS)^2; the sink over the three samples must be >= 12 px + 3 sigma (sigma of the difference), and no reversal inside them: a moving camera
 * does not forbid the trend, it only asks for a larger sink (a jump within the uncertainty of a moving camera is no trend). */
static int down_trend(const tl_t *t, int fh) {
    int n = t->dw_n;
    long need = (long)12 * t->cfg.gain_tilt / fh;
    float ex;
    if (n < 3) return 0;
    ex = (float)(t->dw[n - 1] - t->dw[n - 3] - need); /* the sink beyond 12 px must exceed 3 sigma (compared squared: no libm) */
    if (ex < 0.0f || ex * ex < 9.0f * (t->dws2[n - 1] + t->dws2[n - 3])) return 0;
    return t->dw[n - 1] >= t->dw[n - 2] - need / 3 && t->dw[n - 2] >= t->dw[n - 3] - need / 3; /* no reversal inside the three samples */
}

/* The jump threshold in motor steps (tilt axis): 0.6 head heights (250 mm at the distance of the target), clamped to 10 .. 30 % of the frame height. */
static long jump_lim(const tl_t *t, int fh) {
    const struct tl_cfg *c = &t->cfg;
    float D = t->kd_valid && t->dist_mm > 0 ? (float)t->dist_mm : KF_D_DEF, fv = (float)fh * c->steps_rad / c->gain_tilt, hp = 0.6f * (float)HEAD_MM * fv / D, lo = (float)JUMP_LO_PCT * fh / 100.0f, hi = (float)JUMP_HI_PCT * fh / 100.0f;
    if (hp < lo) hp = lo;
    if (hp > hi) hp = hi;
    return (long)(hp * c->gain_tilt / fh);
}

/* 1 = this frame is a sudden large jump of the followed box (top or centre, world terms) and no second consistent frame confirmed it yet: held, never used.
 * A near target whose top jumps DOWN while the bottom stays cut (a torso box after the head box) is held harder: the tilt and the vertical filter stay frozen until 2 further frames agree
 * with it within a third of the threshold (t->out_down); a return to the old top ends the hold. */
static int outlier(tl_t *t, const struct tl_box *pick, int64_t now, int fh, long topw, long cenw, int have_prev, long ptop, long pcen) {
    long lim = jump_lim(t, fh);
    int near_down;
    if (!t->has_target || !have_prev || now - t->last_seen_ms > 800) { t->out_n = 0; t->out_down = 0; t->out_dn = 0; return 0; }
    if (iabs64(topw - ptop) <= lim && iabs64(cenw - pcen) <= lim) { t->out_n = 0; t->out_down = 0; t->out_dn = 0; return 0; }
    near_down = t->kd_valid && t->dist_mm < NL_NEAR_MM && topw - ptop > lim && pick->y2 >= fh - HEAD_CLIP_PX && t->target.y2 >= fh - HEAD_CLIP_PX;
    if (t->out_down && near_down && iabs64(topw - t->out_topw) <= lim / 3) { /* a further frame agrees with the held jump */
        if (++t->out_n >= 3) { t->out_n = 0; t->out_down = 0; t->out_dn = 0; return 0; }
        t->out_dn++;
        return 1;
    }
    if (!t->out_down && t->out_n > 0 && !near_down) {
        int same = iabs64(pick->y1 - t->out_box.y1) * 100 <= (int64_t)10 * fh && iabs64(pick->y2 - t->out_box.y2) * 100 <= (int64_t)10 * fh;
        int cont = sgn((int)(topw - ptop)) == sgn((int)(t->out_topw - ptop)) && iabs64(topw - ptop) * 3 >= iabs64(t->out_topw - ptop) * 2; /* the move goes on the same way (a person sinking / rising over several ticks is a trend, not a jump) */
        if (same || cont) { t->out_n = 0; return 0; }
    }
    t->out_box = *pick;
    t->out_topw = topw;
    t->out_n = 1;
    t->out_dn = near_down && t->out_down ? t->out_dn + 1 : near_down ? 1 : 0; /* consecutive held near downward jumps (a sinking top that never settles) */
    t->out_down = near_down;
    return 1;
}

/* Which way did the person leave, from the last boxes (stale frames included)? Down: the box is cut at the bottom with its top low. Up / side: the box touches that edge and is not wide. */
static int exit_dir(const tl_t *t, int fw, int fh) {
    const struct tl_box *l;
    int w;
    if (t->hn < 1) return TL_EXIT_NONE;
    l = &t->hist[t->hn - 1];
    w = l->x2 - l->x1;
    if (l->y2 >= fh - HEAD_CLIP_PX && l->y1 >= fh * 25 / 100) {
        int small = (l->y2 - l->y1) * 100 < HEAD_CUT_PCT * fh, near = t->kd_valid && t->dist_mm < NEAR_MM;
        /* DOWN needs a trend / a downward rate measured on frames captured with the camera still (a rate made while it moved is compensation error), or a genuinely small head crop at the bottom;
         * very close (< 0.8 m) the box is always cut by the bottom and says nothing: only a small crop counts */
        if (t->cfg.zone_x_pct > 0 && ((near && !small) || !(down_trend(t, fh) || (small && kf_down(t))))) return TL_EXIT_NONE;
        return TL_EXIT_DOWN;
    } /* cut by the bottom with the top low: crouching / sitting / walking away (a sinking top is no proof: the camera tilting up does it too) */
    if (t->cfg.zone_x_pct > 0 && l->y1 <= HEAD_CLIP_PX && t->dw_n >= 2 && t->dwc[t->dw_n - 1] && t->dwc[t->dw_n - 2]) return TL_EXIT_UP; /* the last two fresh boxes were top-clipped (a head-clip episode): the head went up, a wide box too */
    if ((int64_t)w * 100 > (int64_t)EXIT_WIDE_PCT * fw) return TL_EXIT_NONE;
    if (l->y1 <= HEAD_CLIP_PX && (int64_t)(l->y2 - l->y1) * 100 >= (int64_t)CLIP_MIN_PCT * fh) return TL_EXIT_UP; /* a small box at the top edge is not a head leaving upward */
    if (l->x1 <= HEAD_CLIP_PX && l->x2 < fw - HEAD_CLIP_PX) return TL_EXIT_LEFT;
    if (l->x2 >= fw - HEAD_CLIP_PX && l->x1 > HEAD_CLIP_PX) return TL_EXIT_RIGHT;
    return TL_EXIT_NONE;
}

/* ---- close-loss machine: a near target (the person fills the frame: every box is partial, the loss means "too close / too low / too high", not "left") is searched for with a short, bounded
 * move chosen from the evidence of the last accepted frames, or waited for, and then the tilt is widened back toward home. Timed from the first fresh no-box frame; any box ends it. ---- */
enum { NL_NONE = 0, NL_HOLD, NL_SEARCH, NL_WAIT, NL_WIDEN, NL_DONE, NL_VSCAN };
enum { VS_INIT = 0, VS_UP, VS_DWELL_UP, VS_DOWN, VS_DWELL_DN, VS_BACK };
enum { E_NONE = 0, E_UP, E_DOWN, E_SIDE };

/* E_UP: the last 2 accepted tops were clipped; E_DOWN: the last frame was a low crop (rule 7), a downward jump of the top is being held, or the sigma-aware down trend ended on a bottom-cut box; E_SIDE: a box at a side edge; else E_NONE. *dir = TL_EXIT_*. */
static int loss_evidence(const tl_t *t, int fw, int fh, int *dir) {
    int n = t->dw_n, d;
    *dir = TL_EXIT_NONE;
    if (n >= 2 && t->dwc[n - 1] && t->dwc[n - 2]) { *dir = TL_EXIT_UP; return E_UP; }
    d = exit_dir(t, fw, fh); /* its DOWN needs a trend or a small crop with a downward rate (kf_down) */
    if (t->last_rule == 7 || (t->out_down && t->out_dn >= 2) || d == TL_EXIT_DOWN || (t->hn > 0 && t->hist[t->hn - 1].y2 >= fh - HEAD_CLIP_PX && down_trend(t, fh))) { *dir = TL_EXIT_DOWN; return E_DOWN; }
    if (d == TL_EXIT_LEFT || d == TL_EXIT_RIGHT) { *dir = d; return E_SIDE; }
    return E_NONE;
}

static int sweep_step(tl_t *t, int64_t now, const struct tl_ptz *ptz, struct tl_out *out);

/* The delay of the home return after the grace: the passer-by of the motion-aware search goes home sooner (pass_home_s), nobody else changes. */
static int ms_home_delay(const tl_t *t) {
    const struct tl_cfg *c = &t->cfg;
    if (c->search_mode == TL_SEARCH_MOTION && c->lost_n < 0 && t->ms_passer && t->ms_ep == t->last_seen_ms && c->pass_home_s < c->return_delay_s) return c->pass_home_s;
    return c->return_delay_s;
}

/* The loss is over (the grace ran out): the return home, or the tilt back to the home tilt in stay mode. Shared by both motion modes. 1 when *out was set. */
static int loss_tail(tl_t *t, int64_t now_ms, const struct tl_ptz *ptz, int stale, int modern, struct tl_out *out) {
    const struct tl_cfg *c = &t->cfg;
    if (modern && sweep_step(t, now_ms, ptz, out)) return 1;
    if (c->return_home && t->home_pending && t->nudged && !ptz->moving && now_ms >= t->home_retry_at &&
        now_ms - t->last_seen_ms >= ((int64_t)c->lost_grace_s + ms_home_delay(t)) * 1000) {
        t->home_pending = 0;
        t->nudged = 0;
        t->settle_wait = t->has_settled = 0;
        out->kind = TL_HOME;
        t->le_on = t->le_moved = 0;
        t->tilt_ref_set = 0;
        t->ff_set = 0;
        t->tilt_pending = 0;
        return 1;
    }
    /* stay: the pan stays where the person was last seen, the TILT goes back to the home tilt after the grace + tilt_return_s, so that the next person is
     * found at a neutral level (a head left looking up never sees a person walk in below it) */
    if (modern && !c->return_home && c->tilt_enable && c->tilt_return_s > 0 && t->tilt_pending && ptz->tilt_home > 0 && !ptz->moving && !stale &&
        now_ms - t->last_seen_ms >= ((int64_t)c->lost_grace_s + c->tilt_return_s) * 1000) {
        int hq = clampi(ptz->tilt_home, ptz->tilt_min, ptz->tilt_max) - ptz->tilt;
        t->tilt_pending = 0;
        if (iabs64(hq) >= 2 * MIN_STEP) {
            out->kind = TL_NUDGE;
            out->dtilt = -hq;
            out->speed = 0;
            t->last_nudge_ms = now_ms;
            t->nudged = 1;
            t->settle_wait = 1;
            t->has_settled = 0;
            t->dbg.dt = -hq;
            t->dbg.why = "tilt-return";
            return 1;
        }
    }
    return 0;
}

/* ====================================================================== continuous motion
 * One ongoing directional move per axis pair (8 directions, a speed divisor per axis) that the controller re-targets every tick (the lease): the policy
 * computes, per axis, a desired velocity from the predicted offset of the reference point (Kalman-filtered world position + angular rate) with the comfort
 * zone as the start condition and a smaller inner zone as the stop condition (hysteresis), feed-forward of the target velocity, and an overshoot guard
 * that never lets the commanded motion carry the predicted reference past the opposite zone edge. See docs/porting/lsc-rotatable-2k/tracking.md. */
#define TAU_MS 600        /* the offset is closed in about this long (proportional gain 1/TAU on top of the target velocity) */
#define GUARD_MS 400      /* overshoot guard horizon */
#define ACT_MS 120        /* command-to-motion latency added to the prediction */
#define ROOM_MIN 45       /* steps to a soft limit under which the axis is not commanded toward it */
#define PRED_FADE_MS 700  /* continuous: the constant-velocity prediction through a detection gap fades to zero in this time (never beyond gap_predict_ms) */
#define SEARCH_MS 700     /* continuous exit search: one move in the exit direction this long */

static int axis_div(const struct tl_cfg *c, int v) {
    int a = (int)iabs64(v), d;
    if (a <= 0) return MOTOR_MAX_DIV;
    d = (c->rate_k + a / 2) / a;
    return d < MOTOR_MIN_DIV ? MOTOR_MIN_DIV : d > MOTOR_MAX_DIV ? MOTOR_MAX_DIV : d;
}

/* Desired velocity (steps/s, signed, 0 = leave the axis) of one axis. e = predicted target - camera, v = target angular rate, half = zone half width (steps). */
static int axis_ctl(const struct tl_cfg *c, int e, int v, int half, int *mv, int room_neg, int room_pos, int cap_mag) {
    int stop = half * c->zone_stop_pct / 100, fl = c->rate_k / MOTOR_MAX_DIV, s, vd, mag, maxm;
    int64_t pr = e + (int64_t)v * c->predict_ms / 1000;
    if (stop < 20) stop = 20;
    if (!*mv) {
        if (iabs64(e) > half || (v && iabs64(pr) > half && iabs64(pr) > iabs64(e))) *mv = 1;
        else return 0;
    } else if (iabs64(e) <= stop && (!v || sgn(v) != sgn(e) || iabs64(v) < V_MIN)) {
        *mv = 0;
        return 0;
    }
    vd = v + (int)((int64_t)e * 1000 / TAU_MS);
    s = sgn(vd);
    if (!s || (e && s != sgn(e))) { *mv = 0; return 0; }
    mag = (int)iabs64(vd);
    if (mag > c->rate_k / MOTOR_MIN_DIV) mag = c->rate_k / MOTOR_MIN_DIV;
    if (cap_mag > 0 && mag > cap_mag) mag = cap_mag;
    maxm = s * v + (int)((int64_t)(s * e + half / 2) * 1000 / GUARD_MS); /* overshoot guard: after GUARD_MS the reference must not be past the opposite zone half-edge */
    if (mag > maxm) mag = maxm;
    if (mag < fl) {
        if (iabs64(e) > stop && maxm >= fl / 2) mag = fl;
        else { *mv = 0; return 0; }
    }
    if ((s > 0 && room_pos <= ROOM_MIN) || (s < 0 && room_neg <= ROOM_MIN)) { *mv = 0; return 0; }
    return s * mag;
}

/* The reversal throttle of one axis (0 pan, 1 tilt), at the single choke point of every velocity command (policy, searches, scans, tilt-back): the opposite direction is refused for
 * rev_guard_ms after the last command in the old one (the head comes to rest first), and after rev_n reversals inside rev_window_ms the next one is refused and the axis is held still
 * for rev_hold_ms (the steps policy's rule). Returns the velocity to use (0 = the axis is left still). */
static int rev_gate(tl_t *t, int ax, int64_t now, int v) {
    const struct tl_cfg *c = &t->cfg;
    struct tl_rev *r = &t->rev[ax];
    int d = sgn(v), i, n = 0;
    if (!d || c->rev_guard_ms <= 0) { if (d) { r->dir = d; r->t_cmd = now; } return v; }
    if (r->hold_until > now) return 0;
    if (r->dir && d == -r->dir) {
        int vt_ = ax ? t->vy : t->vx, guard = c->rev_guard_ms;
        if (vt_ && sgn(vt_) == d) guard /= REV_FAST_DIV;
        if (now - r->t_cmd < guard) return 0;
        if (now - r->t_cmd < REV_RECENT_MS) { /* a reversal */
            for (i = 0; i < r->nrev; i++) if (now - r->rev_t[i] <= c->rev_window_ms) r->rev_t[n++] = r->rev_t[i];
            r->nrev = n;
            if (r->nrev >= c->rev_n) { r->hold_until = now + c->rev_hold_ms; return 0; }
            r->rev_t[r->nrev++] = now;
        }
    }
    r->dir = d;
    r->t_cmd = now;
    return v;
}

/* Fills *out from the desired velocities (motor sign steps/s for both axes; the tilt is converted to the image sign here). */
static void emit_vel(tl_t *t, struct tl_out *out, int64_t now, int vx, int vy) {
    const struct tl_cfg *c = &t->cfg;
    vx = rev_gate(t, 0, now, vx);
    vy = rev_gate(t, 1, now, vy);
    if (vy && !t->nl_phase) t->down_ms = (c->invert_tilt ? vy > 0 : vy < 0) ? now : 0; /* the last tilt command of the follow: down = the person went down */
    if (!vx && !vy) {
        if (t->vact) { out->kind = TL_STOP; t->vact = 0; t->cmd_px = t->cmd_py = 0; t->ep_active = 0; }
        return;
    }
    out->kind = TL_VEL;
    out->pan_dir = sgn(vx);
    out->pan_div = vx ? axis_div(c, vx) : 0;
    out->tilt_dir = -sgn(vy);
    out->tilt_div = vy ? axis_div(c, vy) : 0;
    if (vx && vy) { /* a diagonal at divisor 2 was never measured: both axes at divisor 3 or slower (divisor 2 stays single-axis) */
        if (out->pan_div < DIAG_MIN_DIV) out->pan_div = DIAG_MIN_DIV;
        if (out->tilt_div < DIAG_MIN_DIV) out->tilt_div = DIAG_MIN_DIV;
    }
    t->cmd_px = out->pan_dir * (out->pan_div ? c->rate_k / out->pan_div : 0);
    t->cmd_py = sgn(vy) * (out->tilt_div ? c->rate_k / out->tilt_div : 0);
    t->vact = 1;
    t->cmd_ms = now;
    t->nudged = 1;
    t->last_move_ms = now;
    if (vy) t->tilt_pending = 1;
    t->lastv = *out;
}

static void refresh_last(tl_t *t, struct tl_out *out, int64_t now) {
    const struct tl_cfg *c = &t->cfg;
    /* the age is that of the last REAL command: a refresh never extends it (a stale stretch, e.g. a manual move, cannot resurrect a move forever) */
    if (t->vact && now - t->cmd_ms < c->lease_ms) { *out = t->lastv; out->kind = TL_VEL; }
}

/* ---- the full-range search: a person really lost (no box for lost_grace_s + SW_AFTER_MS, the close-loss machine and the exit search over) is looked for in the whole pan range, once per
 * loss: to the nearer end of the range at rate_k / 2, then across to the other end at rate_k / 3 with a stop of SW_DWELL_MS every SW_STRIDE steps (a steady picture for the detector, no
 * blur), then back to where the search began (the home return, when enabled, comes after it). Any accepted box ends it (sw_st = 0, the tracker takes over). Legacy policy only. ---- */
#define SW_AFTER_MS 10000  /* the search starts this long after the grace ran out ... */
#define SW_START_WIN_MS 4000 /* ... and only inside this window (a long absence is not searched: the head went home or stays) */
#define SW_STRIDE 700      /* a stop every this many steps (the frame is ~1000 steps wide) */
#define SW_DWELL_MS 900    /* ... for this long (longer than the reversal guard, so a turn at an end is never refused) */
#define SW_END_MARGIN 25   /* the end of the range: this far from the soft limit */
enum { SW_IDLE = 0, SW_A, SW_DW_A, SW_B, SW_DW_B, SW_C, SW_DONE };

static int sweep_step(tl_t *t, int64_t now, const struct tl_ptz *ptz, struct tl_out *out) {
    const struct tl_cfg *c = &t->cfg;
    int64_t lost = now - t->last_seen_ms;
    int room_lo = ptz->pan - ptz->pan_min, room_hi = ptz->pan_max - ptz->pan, rm = t->sw_dir > 0 ? room_hi : room_lo, v = 0;
    if (c->lost_n >= 0 || c->motion != TL_MOTION_CONT || c->search_mode == TL_SEARCH_MOTION || !t->last_seen_ms || t->sw_st == SW_DONE) return 0; /* the motion-aware search never sweeps the range */
    if (t->sw_st == SW_IDLE) {
        if (lost < ((int64_t)c->lost_grace_s * 1000 + SW_AFTER_MS)) return 0;
        if (lost > (int64_t)c->lost_grace_s * 1000 + SW_AFTER_MS + SW_START_WIN_MS || ptz->pan_max - ptz->pan_min < 2 * SW_STRIDE) { t->sw_st = SW_DONE; return 0; }
        t->sw_st = SW_A;
        t->sw_dir = room_lo < room_hi ? -1 : 1;
        t->sw_p0 = ptz->pan;
        rm = t->sw_dir > 0 ? room_hi : room_lo;
    }
    t->dbg.why = "full-search";
    switch (t->sw_st) {
    case SW_A:
        if (rm <= ROOM_MIN + SW_END_MARGIN) { t->sw_st = SW_DW_A; t->sw_until = now + SW_DWELL_MS; }
        else v = t->sw_dir * (c->rate_k / 2);
        break;
    case SW_DW_A:
        if (now >= t->sw_until) { t->sw_st = SW_B; t->sw_dir = -t->sw_dir; t->sw_stop = ptz->pan; }
        break;
    case SW_B:
        rm = t->sw_dir > 0 ? room_hi : room_lo;
        if (rm <= ROOM_MIN + SW_END_MARGIN) { t->sw_st = SW_DW_B; t->sw_until = now + SW_DWELL_MS; }
        else if (iabs64(ptz->pan - t->sw_stop) >= SW_STRIDE) { t->sw_st = SW_DW_B; t->sw_until = now + SW_DWELL_MS; }
        else v = t->sw_dir * (c->rate_k / 2);
        break;
    case SW_DW_B:
        if (now >= t->sw_until) {
            rm = t->sw_dir > 0 ? room_hi : room_lo;
            if (rm <= ROOM_MIN + SW_END_MARGIN) { if (c->return_home) t->sw_st = SW_DONE; else { t->sw_st = SW_C; t->sw_dir = ptz->pan > t->sw_p0 ? -1 : 1; } }
            else { t->sw_st = SW_B; t->sw_stop = ptz->pan; }
        }
        break;
    case SW_C:
        if (iabs64(ptz->pan - t->sw_p0) <= 60 || (ptz->pan - t->sw_p0) * t->sw_dir > 0) t->sw_st = SW_DONE;
        else v = t->sw_dir * (c->rate_k / 2);
        break;
    default: break;
    }
    if (t->sw_st == SW_DONE) { int was = t->vact; if (was) emit_vel(t, out, now, 0, 0); return was; }
    emit_vel(t, out, now, v, 0);
    return 1;
}

/* the chain ended (or `stay`): forget the target, stop, and remember that it is over until a new person is followed */
static void lost_end(tl_t *t, struct tl_out *out, int64_t now_ms) {
    t->prev_ms = 0;
    t->sm_valid = 0;
    t->hcw_valid = 0;
    rv_reset(t);
    t->has_target = 0;
    t->streak = 0;
    t->mv_x = t->mv_y = 0;
    t->sweeping = 0;
    t->search_until = 0;
    t->lost_done = 1;
    t->lost_i = -1;
    t->lstep = "done";
    t->ep_active = 0;
    if (t->vact) emit_vel(t, out, now_ms, 0, 0);
}

/* The tilt UP budget left for this frame (steps). Far frames (rules 3 / 4, rule 1 shorter than NEAR_H_PCT): TILT_UP_CAP from the tilt at the episode start. Near frames (bottom cut, tall, rule 1 / 2: the head is
 * out of the view because the person is close, climbing is right): UP_CAP_NEAR from the tilt of the last frame framed well (top uncut, 5-35 % of the height). An ambiguous box read as a head-out: UP_CAP_AMB
 * from where the latch started. */
static int up_allowed(const tl_t *t, const struct tl_ptz *ptz, int rule, const struct tl_box *b, int fh) {
    int up = t->cfg.invert_tilt ? -1 : 1, cap = TILT_UP_CAP, org = t->tilt_ref, bh = b->y2 - b->y1;
    if (t->amb_up) { cap = UP_CAP_AMB; if (t->amb_tilt_set) org = t->amb_tilt0; }
    else if ((rule == 1 || rule == 2) && b->y2 >= fh - HEAD_CLIP_PX && (int64_t)bh * 100 >= (int64_t)NEAR_H_PCT * fh) { cap = UP_CAP_NEAR; if (t->tilt_unclip_set) org = t->tilt_unclip; }
    return cap - up * (ptz->tilt - org);
}

/* a fresh frame: the bookkeeping of the up budgets (the well framed tilt, the start of an ambiguous climb) */
static void up_note(tl_t *t, const struct tl_ptz *ptz, const struct tl_box *b, int fh) {
    if (b->y1 > HEAD_CLIP_PX && (int64_t)b->y1 * 100 >= (int64_t)UNCLIP_LO_PCT * fh && (int64_t)b->y1 * 100 <= (int64_t)UNCLIP_HI_PCT * fh) { t->tilt_unclip_set = 1; t->tilt_unclip = ptz->tilt; }
    if (t->amb_up) { if (!t->amb_tilt_set) { t->amb_tilt_set = 1; t->amb_tilt0 = ptz->tilt; } }
    else if (!t->amb_latch) t->amb_tilt_set = 0;
}

/* The person was lost with the tilt well away from home (a climb for a head that is gone, a head-clip at close range, a descent after a person who sat down): after tilt_back_s (2.5 s when the lost
 * target was near) the tilt goes back toward the home tilt, from above or from below, at a moderate speed, whatever the return policy does for the pan, until it is within 40 steps of home (at most
 * 20 s, never into a soft limit). 1 when a command was emitted. */
static int tilt_back(tl_t *t, int64_t now_ms, const struct tl_ptz *ptz, int rt_lo, int rt_hi, struct tl_out *out) {
    const struct tl_cfg *c = &t->cfg;
    int up = c->invert_tilt ? -1 : 1, above, away, tdir;
    int64_t delay;
    if (!c->tilt_back_s || !c->tilt_enable || ptz->tilt_home <= 0 || !t->last_seen_ms || t->nl_phase) return 0; /* a near target: the close-loss machine widens the tilt instead */
    above = up * (ptz->tilt - ptz->tilt_home);
    away = above < 0 ? -above : above;
    delay = (int64_t)c->tilt_back_s * 1000;
    if (t->last_near && delay > (int64_t)(TBACK_NEAR_S * 1000)) delay = (int64_t)(TBACK_NEAR_S * 1000);
    if (!t->tback_on) {
        if (t->tback_done || away <= c->tilt_back_min || now_ms - t->last_seen_ms < delay || (t->tback_int_ms && now_ms - t->tback_int_ms < TBACK_RESTART_MS)) return 0; /* not for 10 s after a box interrupted it */
        t->tback_on = 1;
        t->tback_t0 = now_ms;
        t->tback_sgn = above > 0 ? 1 : -1; /* the side of home it starts from: the direction is latched, the end is the crossing of home */
    }
    tdir = t->tback_sgn > 0 ? -up : up;
    if (away <= 40 || sgn(above) != t->tback_sgn || now_ms - t->tback_t0 > 20000 || (tdir < 0 && rt_lo <= ROOM_MIN) || (tdir > 0 && rt_hi <= ROOM_MIN)) {
        t->tback_on = 0;
        if (away <= 40) t->ff_set = 0; /* the head went home: a new close sighting starts from there */
        if (t->vact) emit_vel(t, out, now_ms, 0, 0);
        t->tback_done = 1; /* once per loss */
        return out->kind == TL_STOP;
    }
    t->dbg.why = "tilt-back";
    t->lstep = t->lstep ? t->lstep : "idle";
    emit_vel(t, out, now_ms, 0, tdir * (c->rate_k / 6)); /* the speed of the WIDEN: slow enough that a late tick cannot jump the end window */
    return 1;
}

/* B: constant-velocity prediction through a detection gap (decaying, bounded), the same control law. 1 when a command was emitted. */
static int predict_gap(tl_t *t, int64_t now_ms, const struct tl_ptz *ptz, int64_t gap, int halfx, int rpan_lo, int rpan_hi, int rt_lo, int rt_hi, struct tl_out *out) {
    const struct tl_cfg *c = &t->cfg;
    int ex, ey, halfy, vdx = 0, vdy = 0;
    if (!(gap <= c->gap_predict_ms && t->sm_valid && !t->search_until)) return 0;
    {
        int64_t fade = gap >= PRED_FADE_MS ? 0 : PRED_FADE_MS - gap; /* the prediction fades out in PRED_FADE_MS: a feint or a stop must not be extrapolated for long */
        int vxe = (int)(t->vx * fade / PRED_FADE_MS), vye = (int)(t->vy * fade / PRED_FADE_MS);
        int64_t ah = (now_ms - t->lastcap_ms) + ACT_MS, lim = c->gain_pan / 2, px = (int64_t)vxe * ah / 1000, py = (int64_t)vye * ah / 1000;
        px = px > lim ? lim : px < -lim ? -lim : px;
        py = py > lim ? lim : py < -lim ? -lim : py;
        ex = (int)(t->sm_wx + px - ptz->pan);
        ey = (int)(t->sm_wy + py - ptz->tilt);
        halfy = (int)((int64_t)((t->sm_rule == 4 || t->sm_rule == 0) ? c->zone_y_pct : c->zone_head_pct) * c->gain_tilt / 200);
        vdx = axis_ctl(c, ex, vxe, halfx, &t->mv_x, rpan_lo, rpan_hi, 0);
        if (t->kd_valid && t->dist_mm < NEAR_MM) halfy *= 2;
        /* the prediction only CONTINUES a tilt move that was on, it never starts one: the vertical offset of a gone person is the last row of a box that was never aimed at */
        if (c->tilt_enable && t->sm_rule != 1 && t->sm_rule != 5 && t->mv_y) vdy = axis_ctl(c, ey, vye, halfy, &t->mv_y, rt_lo, rt_hi, 0);
        if (vdx || vdy) { t->dbg.why = "predict"; emit_vel(t, out, now_ms, vdx, vdy); return 1; }
    }
    return 0;
}

/* 1 when the lost policy does not bring the tilt home by itself (chain none / stay / without a home step, or the legacy policy with return_home 0): the close-loss machine then WIDENs
 * the tilt back to a level where people are detectable before it hands over. */
static int policy_keeps_tilt(const struct tl_cfg *c) {
    int i;
    if (c->lost_n < 0) return !c->return_home;
    for (i = 0; i < c->lost_n; i++) if (c->lost[i].kind == TLS_HOME) return 0;
    return 1;
}

/* ---- the motion-aware search (search_mode = TL_SEARCH_MOTION, continuous, legacy lost policy): SIMPLE and sparing the motors. At most MS_MAX_MOVES short moves per loss (one look toward where the person
 * went, and one steer toward motion that appears afterwards), each MS_LOOK_MS long at rate_k / SWEEP_DIV (about 190 steps), never within MS_ROOM of a soft limit, at most one look per search_cooldown_s,
 * no close scan, no backsweep, no full-range sweep. A passer-by (followed for less than pass_s) gets one look and only after an edge exit. No evidence = no move: the head waits, then goes home. 1 when a command was emitted. ---- */
#define MS_MAX_MOVES 2
#define MS_LOOK_MS 700
#define MS_ROOM 300        /* steps to a soft limit under which no look is started toward it (a look travels about 190 steps: a stop is never reached) */
#define MS_STEER_X 150     /* a motion centroid further than this (permille of the width) from the middle is steered to; closer, the person is in view and the detector gets it */
#define MS_STEER_Y 200
#define MS_DIR_AGE_MS 1500 /* the motion direction before the loss counts when it is this recent */
static int ms_loss(tl_t *t, int64_t now, const struct tl_ptz *ptz, int fw, int fh, int rpan_lo, int rpan_hi, int rt_lo, int rt_hi, struct tl_out *out) {
    const struct tl_cfg *c = &t->cfg;
    const struct tl_mot *mo = &t->mot;
    int m = c->invert_pan ? -1 : 1, kt = c->invert_tilt ? 1 : -1, sp = c->rate_k / SWEEP_DIV, dir = TL_EXIT_NONE, steer = 0, dx = 0, dy = 0;
    int64_t dur = MS_LOOK_MS;
    if (t->ms_ep != t->last_seen_ms) { /* a new loss */
        int ed = (t->last_ff || t->ff_set || t->last_nl) ? TL_EXIT_NONE : exit_dir(t, fw, fh), md = mo->last_dir;
        t->ms_ep = t->last_seen_ms;
        t->ms_moves = 0;
        t->ms_until = 0;
        t->ms_tilt0 = ptz->tilt;
        /* the motion heading the other way just before the loss (a person at the edge who turned back) cancels the look */
        if (mo->valid && md && mo->dir_ms && t->last_seen_ms - mo->dir_ms <= MS_DIR_AGE_MS &&
            (((ed == TL_EXIT_LEFT && md == TL_EXIT_RIGHT) || (ed == TL_EXIT_RIGHT && md == TL_EXIT_LEFT) || (ed == TL_EXIT_UP && md == TL_EXIT_DOWN))))
            ed = TL_EXIT_NONE;
        /* a passer-by: followed briefly AND gone through a side or the top (a short sighting that just vanishes in the middle is not "passing by") */
        t->ms_passer = t->seen0_ms && t->last_seen_ms - t->seen0_ms < (int64_t)c->pass_s * 1000 && (ed == TL_EXIT_LEFT || ed == TL_EXIT_RIGHT || ed == TL_EXIT_UP);
        t->ms_dir = ed;
        t->dbg.exit = ed;
    }
    if (t->ms_until > 0) { /* a look is running: bounded, re-commanded, checked against the soft limits */
        int sx = t->ms_vx, sy = t->ms_vy;
        if (now >= t->ms_until || (sx > 0 && rpan_hi <= ROOM_MIN) || (sx < 0 && rpan_lo <= ROOM_MIN) || (sy > 0 && rt_hi <= ROOM_MIN) || (sy < 0 && rt_lo <= ROOM_MIN)) {
            t->ms_until = -1;
            emit_vel(t, out, now, 0, 0);
            return 1;
        }
        t->dbg.why = "look";
        emit_vel(t, out, now, sx, sy);
        return 1;
    }
    if (t->ms_moves >= MS_MAX_MOVES || (t->ms_passer && t->ms_moves >= 1)) return 0;
    if (!t->fresh_loss_ms) t->fresh_loss_ms = now;
    if (now - t->fresh_loss_ms < LOSS_FRESH_MS) return 0; /* fresh no-box frames first: never right after the camera's own move */
    /* motion that appeared after the loss wins: steer toward it, the detector confirms */
    if (mo->valid && !mo->masked && mo->active && mo->last_active_ms > t->last_seen_ms && now - t->last_seen_ms <= ((int64_t)c->lost_grace_s + c->reacq_s) * 1000 && !t->ms_passer) {
        int ox = mo->cx - 500, oy = mo->cy - 500;
        if (iabs64(ox) >= MS_STEER_X && iabs64(ox) * 3 >= iabs64(oy) * 2) dir = ox < 0 ? TL_EXIT_LEFT : TL_EXIT_RIGHT;
        else if (c->tilt_enable && iabs64(oy) >= MS_STEER_Y) dir = oy < 0 ? TL_EXIT_UP : TL_EXIT_DOWN;
        if (dir) { steer = 1; dur = 300 + (int64_t)(iabs64(ox) > iabs64(oy) * fw / fh ? iabs64(ox) : iabs64(oy)) * 600 / 500; if (dur > MS_LOOK_MS + 200) dur = MS_LOOK_MS + 200; }
    }
    if (!dir && t->ms_moves == 0 && t->ms_dir && now - t->last_seen_ms <= ((int64_t)c->lost_grace_s + c->reacq_s) * 1000) dir = t->ms_dir;
    if (!dir) return 0;
    if (!steer && t->ms_last_look && now - t->ms_last_look < (int64_t)c->search_cooldown_s * 1000) { t->ms_moves = MS_MAX_MOVES; t->dbg.why = "look-cooldown"; return 0; }
    switch (dir) {
    case TL_EXIT_DOWN: dy = c->tilt_enable ? kt * sp : 0; break;
    case TL_EXIT_UP: dy = c->tilt_enable && (c->invert_tilt ? -1 : 1) * (ptz->tilt - t->ms_tilt0) < EXIT_UP_BUDGET ? -kt * sp : 0; break;
    case TL_EXIT_LEFT: dx = -m * sp; break;
    case TL_EXIT_RIGHT: dx = m * sp; break;
    default: break;
    }
    if ((dx > 0 && rpan_hi <= MS_ROOM) || (dx < 0 && rpan_lo <= MS_ROOM)) dx = 0;
    if ((dy > 0 && rt_hi <= MS_ROOM) || (dy < 0 && rt_lo <= MS_ROOM)) dy = 0;
    if (!dx && !dy) { t->ms_moves = MS_MAX_MOVES; t->dbg.why = "look-range"; return 0; } /* at a limit: nothing to look at that way */
    t->ms_moves++;
    if (!steer || !t->ms_last_look) t->ms_last_look = now;
    t->ms_vx = dx;
    t->ms_vy = dy;
    t->ms_until = now + dur;
    t->dbg.why = steer ? "steer" : "look";
    emit_vel(t, out, now, dx, dy);
    return 1;
}

/* 1 while the last scan / directed search (started at `last`, 0 = never) is younger than close_scan_cooldown_s */
static int scan_cooling(const struct tl_cfg *c, int64_t now, int64_t last) { return c->close_scan_cooldown_s > 0 && last && now - last < (int64_t)c->close_scan_cooldown_s * 1000; }

/* the search (or the wait) is over: the vertical scan, or the end of the machine */
static int nl_after_search(tl_t *t, int64_t now) {
    const struct tl_cfg *c = &t->cfg;
    if (!c->close_scan) return NL_WIDEN;
    if (c->tilt_enable && t->nl_ev != E_SIDE && !scan_cooling(c, now, t->vs_last_ms)) { t->vs_st = VS_INIT; return NL_VSCAN; } /* the cooldown: at most one scan per close_scan_cooldown_s */
    return policy_keeps_tilt(c) ? NL_WIDEN : NL_DONE;
}

/* 1 = the machine owns this tick (*out set, or deliberately nothing); 0 = it is not running / over (the existing lost policy goes on) */
static int near_loss(tl_t *t, int64_t now, const struct tl_ptz *ptz, int fw, int fh, int halfx, int rpan_lo, int rpan_hi, int rt_lo, int rt_hi, struct tl_out *out) {
    const struct tl_cfg *c = &t->cfg;
    int kt = c->invert_tilt ? 1 : -1, m = c->invert_pan ? -1 : 1, up = c->invert_tilt ? -1 : 1, sp = c->rate_k / SWEEP_DIV;
    int64_t el;
    if (t->nl_phase == NL_DONE) return 0;
    if (!t->nl_phase) {
        if (!t->last_nl || !t->has_target) return 0;
        if (t->le_on && t->le_last && now - t->le_last > LE_STALE_MS) t->le_on = t->le_moved = 0;
        if (t->le_on && t->le_moved) return 0; /* a re-loss inside the episode in which the head already searched: the normal lost policy, no new search / VSCAN */
        if (!t->le_on) { t->le_on = 1; t->le_moved = 0; t->le_hold0 = 0; t->le_last = t->last_seen_ms; }
        if (!t->fresh_loss_ms) t->fresh_loss_ms = now;
        t->nl_phase = NL_HOLD;
        t->nl_t0 = t->fresh_loss_ms;
        t->nl_ev = loss_evidence(t, fw, fh, &t->exit_dir);
        if (t->last_ff || t->ff_set) { t->nl_ev = E_NONE; t->exit_dir = TL_EXIT_NONE; } /* a frame-filling person lost (or one whose close sighting is still on: the last box was only 88 % tall): two clipped tops are no proof of a head-out (it is too close), no directed UP search: WAIT, then the VSCAN under the cooldown */
        t->nl_n = 0;
        t->nl_until = 0;
        t->nl_w0ok = 0;
        t->nl_pan0 = ptz->pan;
        t->nl_tilt0 = ptz->tilt;
        t->nl_down = t->nl_ev == E_NONE && t->down_ms && now - t->down_ms <= NL_DOWN_MS; /* hardware 2026-11-19: a person who sat low was searched UP first, 250 steps and 6 s the wrong way */
        t->mv_y = 0;
        t->search_until = 0;
        t->sweeping = 0;
    }
    t->dbg.exit = t->exit_dir;
    t->lstep = "close";
    el = now - t->nl_t0;
    if (t->nl_phase == NL_HOLD) {
        if (el < NL_HOLD_MS) {
            t->mv_y = 0; /* the prediction never starts or continues a tilt here */
            t->dbg.why = "close-hold";
            if (predict_gap(t, now, ptz, now - t->last_seen_ms, halfx, rpan_lo, rpan_hi, rt_lo, rt_hi, out)) return 1;
            if (t->vact) emit_vel(t, out, now, 0, 0);
            return 1;
        }
        t->nl_phase = t->nl_ev == E_NONE || scan_cooling(c, now, t->vs_last_ms) || scan_cooling(c, now, t->ds_last_ms) ? NL_WAIT : NL_SEARCH; /* the directed search is under the same cooldown as the scan */
        if (t->nl_phase == NL_SEARCH) t->ds_last_ms = now;
        t->vs_st = VS_INIT;
    }
    if (t->nl_phase == NL_WAIT) {
        if (el < (c->close_scan ? (NL_WAIT_SCAN_MS > c->gap_predict_ms ? NL_WAIT_SCAN_MS : c->gap_predict_ms) : NL_WAIT_MS)) { /* a scan never starts while the person may still be relayed / predicted (gap_predict_ms) */
            t->dbg.why = "close-wait";
            if (t->vact) emit_vel(t, out, now, 0, 0);
            return 1;
        }
        t->nl_phase = nl_after_search(t, now);
    }
    if (t->nl_phase == NL_SEARCH) {
        int dx = 0, dy = 0, lead = sp / 5, travel = 0;
        if (t->nl_until > 0 && now >= t->nl_until) { /* the move ended: stop, a gap before the next */
            t->nl_until = -now;
            if (t->nl_n >= 2) t->nl_phase = nl_after_search(t, now);
            emit_vel(t, out, now, 0, 0);
            return 1;
        }
        if (t->nl_until == 0 || (t->nl_until < 0 && now + t->nl_until >= SEARCH_GAP_MS)) {
            if (t->nl_n < 2) { t->nl_until = now + SEARCH_MS; t->nl_n++; t->le_moved = 1; }
            else { t->nl_phase = nl_after_search(t, now); }
        }
        if (t->nl_phase == NL_SEARCH && t->nl_until > 0) {
            switch (t->exit_dir) {
            case TL_EXIT_DOWN: travel = -up * (ptz->tilt - t->nl_tilt0); dy = c->tilt_enable ? kt * sp : 0; break;
            case TL_EXIT_UP: travel = up * (ptz->tilt - t->nl_tilt0); dy = c->tilt_enable ? -kt * sp : 0; break;
            case TL_EXIT_LEFT: travel = (int)iabs64(ptz->pan - t->nl_pan0); dx = -m * sp; break;
            case TL_EXIT_RIGHT: travel = (int)iabs64(ptz->pan - t->nl_pan0); dx = m * sp; break;
            default: break;
            }
            if (travel + lead >= NL_BUDGET) dx = dy = 0; /* its own budget from the loss pose */
            if ((dx > 0 && rpan_hi <= ROOM_MIN) || (dx < 0 && rpan_lo <= ROOM_MIN)) dx = 0;
            if ((dy > 0 && rt_hi <= ROOM_MIN) || (dy < 0 && rt_lo <= ROOM_MIN)) dy = 0;
            if (dx || dy) { t->dbg.why = "exit-search"; emit_vel(t, out, now, dx, dy); return 1; }
            t->nl_n = 2; /* the budget or the soft range is used up: no more searching */
            t->nl_until = -now;
            t->nl_phase = nl_after_search(t, now);
            if (t->vact) { emit_vel(t, out, now, 0, 0); return 1; }
        } else if (t->nl_phase == NL_SEARCH) {
            if (t->vact) emit_vel(t, out, now, 0, 0); /* the gap between two moves */
            return 1;
        }
    }
    if (t->nl_phase == NL_VSCAN) { /* the vertical scan: up to loss + U, a dwell, down to loss - D, a dwell, back to the loss tilt; the pan is held; limits are heights (up * tilt) */
        int h = up * ptz->tilt, h0 = up * t->nl_tilt0, vsp = c->rate_k / NL_VS_DIV, lead = vsp / 10;
        int hlo = up > 0 ? ptz->tilt_min : -ptz->tilt_max, hhi = up > 0 ? ptz->tilt_max : -ptz->tilt_min, dy = 0;
        if (t->vs_st == VS_INIT) {
            t->vs_bsgn = 0;
            t->le_moved = 1;
            t->vs_last_ms = now;
            t->vs_hu = h0 + c->close_scan_up;
            if (t->vs_hu > hhi - ROOM_MIN) t->vs_hu = hhi - ROOM_MIN; /* the soft range (and its margin) clips the scan */
            t->vs_hd = h0 - c->close_scan_down;
            if (t->vs_hd < hlo + ROOM_MIN) t->vs_hd = hlo + ROOM_MIN;
            t->vs_want_up = t->nl_ev != E_UP && !t->nl_down && t->vs_hu - h0 >= NL_VS_MIN; /* a directed search already covered its own side */
            t->vs_want_dn = t->nl_ev != E_DOWN && h0 - t->vs_hd >= NL_VS_MIN;
            t->vs_t0 = now;
            t->vs_st = VS_UP;
            t->vs_ev = t->vs_want_up ? VSEV_UP : VSEV_DOWN;
            t->vs_ev_tilt = t->nl_tilt0;
        }
        if (t->vs_st == VS_UP && !t->vs_want_up) t->vs_st = VS_DOWN; /* skip the sides with nothing to do */
        if (t->vs_st == VS_DOWN && !t->vs_want_dn) t->vs_st = VS_BACK;
        if (t->vs_st == VS_BACK) {
            if (!t->vs_bsgn) t->vs_bsgn = h > h0 ? 1 : -1; /* the side of the loss tilt it starts from: latched, the end is the crossing of the loss tilt */
            if ((h > h0 ? h - h0 : h0 - h) <= lead + NL_VS_MIN / 2 || policy_keeps_tilt(c) || (h > h0 ? 1 : h < h0 ? -1 : 0) != t->vs_bsgn) t->vs_st = -1; /* at the loss tilt already (or crossed it), or the WIDEN brings it home anyway */
        }
        if (t->vs_st < 0 || now - t->vs_t0 > NL_VS_MAX_MS) {
            t->vs_ev = VSEV_NOTHING;
            t->vs_ev_tilt = ptz->tilt;
            t->nl_phase = policy_keeps_tilt(c) ? NL_WIDEN : NL_DONE;
            t->vs_st = VS_INIT;
            if (t->vact) { emit_vel(t, out, now, 0, 0); return 1; }
        } else if (t->vs_st == VS_DWELL_UP || t->vs_st == VS_DWELL_DN) {
            if (now - t->vs_tdw >= NL_VS_DWELL_MS) { t->vs_st = t->vs_st == VS_DWELL_UP ? VS_DOWN : VS_BACK; return near_loss(t, now, ptz, fw, fh, halfx, rpan_lo, rpan_hi, rt_lo, rt_hi, out); }
            t->dbg.why = "close-dwell";
            if (t->vact) emit_vel(t, out, now, 0, 0);
            return 1;
        } else {
            int reached = 0;
            if (t->vs_st == VS_UP) { dy = up * vsp; reached = h + lead >= t->vs_hu; }
            else if (t->vs_st == VS_DOWN) { dy = -up * vsp; reached = h - lead <= t->vs_hd; }
            else { dy = (t->vs_bsgn > 0 ? -up : up) * vsp; }
            if ((dy > 0 && rt_hi <= ROOM_MIN) || (dy < 0 && rt_lo <= ROOM_MIN)) reached = 1; /* a soft limit */
            if (reached && t->vs_st == VS_BACK) { /* the way back ends at a soft limit: the scan is over (it used to dwell and turn round until the 30 s timeout) */
                t->vs_st = -1;
                return near_loss(t, now, ptz, fw, fh, halfx, rpan_lo, rpan_hi, rt_lo, rt_hi, out);
            }
            if (reached) {
                t->vs_st = t->vs_st == VS_UP ? VS_DWELL_UP : VS_DWELL_DN;
                t->vs_tdw = now;
                t->dbg.why = "close-dwell";
                if (t->vact) emit_vel(t, out, now, 0, 0);
                return 1;
            }
            t->dbg.why = t->vs_st == VS_UP ? "close-scan-up" : t->vs_st == VS_DOWN ? "close-scan-down" : "close-scan-back";
            emit_vel(t, out, now, 0, dy);
            return 1;
        }
    }
    if (t->nl_phase == NL_WIDEN) {
        int above = up * (ptz->tilt - ptz->tilt_home), away = above < 0 ? -above : above;
        if (!t->nl_w0ok) {
            t->nl_w0ok = 1;
            t->nl_w0 = now;
            if (!c->tilt_enable || !c->tilt_back_s || ptz->tilt_home <= 0 || away <= NL_WIDEN_MIN) away = 0;
            if (!away) { t->nl_phase = NL_DONE; t->tback_done = 1; if (t->vact) { emit_vel(t, out, now, 0, 0); return 1; } return 0; }
        }
        if (away <= NL_WIDEN_STOP || now - t->nl_w0 > NL_WIDEN_MAX_MS || ((above > 0 ? -up : up) < 0 && rt_lo <= ROOM_MIN) || ((above > 0 ? -up : up) > 0 && rt_hi <= ROOM_MIN)) {
            t->nl_phase = NL_DONE;
            t->tback_done = 1;
            if (away <= NL_WIDEN_STOP) t->ff_set = 0; /* the head went home */
            if (t->vact) { emit_vel(t, out, now, 0, 0); return 1; }
            return 0;
        }
        t->dbg.why = "tilt-back";
        emit_vel(t, out, now, 0, (above > 0 ? -up : up) * (c->rate_k / 6));
        return 1;
    }
    if (t->nl_phase == NL_DONE) { /* the scan ended: the lost policy takes over from the next tick */
        t->tback_done = 1;
        if (t->vact) { emit_vel(t, out, now, 0, 0); return 1; }
    }
    return 0;
}

/* The head box that belongs to the followed target, or NULL. With a person box: the head centre is inside the box +-10 % of its width and in its upper 45 % (or just above its top). Without one: a head whose
 * world pan position is within 25 % of the frame width of the predicted target and whose vertical position is within 35 % of the height of the last head / box top (only for a target being followed).
 * The nearest to the box centre / the prediction wins, so a second person's head is never used. */
static const struct tl_box *head_assoc(const tl_t *t, const struct tl_box *pb, const struct tl_box *hs, int nh, int64_t cap, int cpan, int ctilt, int fw, int fh) {
    const struct tl_cfg *c = &t->cfg;
    const struct tl_box *best = 0;
    int64_t bd = 0;
    int i, m = c->invert_pan ? -1 : 1, kt = c->invert_tilt ? 1 : -1;
    long tw = t->sm_valid ? t->sm_wx + (long)((int64_t)t->vx * (cap - t->lastcap_ms) / 1000) : t->target_w;
    if (!pb && !t->has_target) return 0;
    for (i = 0; i < nh; i++) {
        const struct tl_box *h = &hs[i];
        int cx2 = h->x1 + h->x2, cy2 = h->y1 + h->y2, hh = h->y2 - h->y1;
        int64_t d;
        if (!valid(h)) continue;
        if (pb) {
            int bw = pb->x2 - pb->x1, bh = pb->y2 - pb->y1, mg = bw * HEAD_X_MARGIN_PCT / 100;
            if (cx2 < 2 * (pb->x1 - mg) || cx2 > 2 * (pb->x2 + mg)) continue;
            if (cy2 > 2 * pb->y1 + bh * 2 * HEAD_Y_BOX_PCT / 100 || cy2 < 2 * pb->y1 - 3 * hh) continue;
            d = iabs64(cx2 - (pb->x1 + pb->x2));
        } else {
            long bw = cpan + m * offset_steps(cx2 - fw, fw, c->gain_pan), hw = kt * ctilt + offset_steps(cy2 - fh, fh, c->gain_tilt), ref;
            int have = t->hcw_valid || t->wprev;
            d = iabs64(bw - tw);
            if (iabs64(bw - t->target_w) < d) d = iabs64(bw - t->target_w);
            if (d > (int64_t)HEAD_GATE_X_PCT * c->gain_pan / 100) continue;
            ref = t->hcw_valid ? t->hcw : t->ptopw;
            if (have && iabs64(hw - ref) > (int64_t)HEAD_GATE_Y_PCT * c->gain_tilt / 100) continue;
        }
        if (!best || d < bd) { best = h; bd = d; }
    }
    return best;
}

/* A person box the head must replace as the vertical reference: touching the frame top / bottom, a near one (tall), or wide. A superset of rules 1, 2, 5, 7 (the outlier test runs before head_rule). */
static int box_cut_like(const struct tl_cfg *c, const struct tl_box *b, int fw, int fh) {
    return b->y1 <= HEAD_CLIP_PX || b->y2 >= fh - HEAD_CLIP_PX || (int64_t)(b->y2 - b->y1) * 100 >= (int64_t)(c->full_pct - 8) * fh || (int64_t)(b->x2 - b->x1) * 100 > (int64_t)WIDE_PCT * fw;
}

#define CLOSE_SAME_MS 1000 /* close_same: the followed target box must be younger than this */

/* b (a near box: >= full_pct - 8 % of the frame height, world pan position bw, steps) and the followed target (also near, seen less than CLOSE_SAME_MS ago) overlap in WORLD pan coordinates (the head
 * may have turned since the target box: the image columns of two poses say nothing) by at least 20 % of the narrower one: the same close person. It only lifts the sticky gate (the body box of a
 * person at arm length wanders); the candidates are still ordered by their distance, so a second close person never beats the true target by array order. */
static int close_same(const tl_t *t, const struct tl_box *b, long bw, int64_t now_ms, int fw, int fh) {
    const struct tl_cfg *c = &t->cfg;
    long wb, wt, lo, hi, wm;
    if (!t->has_target || !t->last_seen_ms || now_ms - t->last_seen_ms >= CLOSE_SAME_MS || t->box_w <= 0 || !valid(b)) return 0;
    if ((int64_t)(b->y2 - b->y1) * 100 < (int64_t)(c->full_pct - 8) * fh || (int64_t)t->box_h * 100 < (int64_t)(c->full_pct - 8) * fh) return 0;
    wb = (long)((int64_t)(b->x2 - b->x1) * c->gain_pan / fw);
    wt = (long)((int64_t)t->box_w * c->gain_pan / fw);
    lo = bw - wb / 2 > t->target_w - wt / 2 ? bw - wb / 2 : t->target_w - wt / 2;
    hi = bw + wb / 2 < t->target_w + wt / 2 ? bw + wb / 2 : t->target_w + wt / 2;
    wm = wb < wt ? wb : wt;
    return hi - lo > 0 && (int64_t)(hi - lo) * 100 >= (int64_t)20 * wm;
}

/* The frame-filling sighting (cap origin, last_ff) from an ACCEPTED box (called after the measurement was taken: a rejected box updates nothing). The origin (the tilt where the person
 * became frame-filling) is kept over gaps and over boxes that are not frame-filling; it ends after FF_CLEAR_MS of boxes shorter than FF_NEAR_PCT, or when the head goes home. */
/* Extra climb allowed for a person who rises while the box stays top-clipped: the steps of the drop of the box bottom below the sighting's largest, judged on the LAST 3
 * consecutive boxes (the largest of them: one tall box of a flicker cancels it). The camera's own climb moves the content down and shrinks the drop, so the budget is a
 * function of the picture, bounded by FF_RISE_MAX: an unchanged box, or alternating ones, never ratchets beyond it. */
static int ff_rise(const tl_t *t, int64_t now, int fh) {
    int i, hi, d;
    if (!t->ff_set || t->ff_y2n < 3 || now - t->ff_y2t > 1000) return 0;
    for (hi = t->ff_y2h[0], i = 1; i < 3; i++) if (t->ff_y2h[i] > hi) hi = t->ff_y2h[i];
    d = (t->ff_y2ref - hi) * t->cfg.gain_tilt / fh;
    return d < 0 ? 0 : d > FF_RISE_MAX ? FF_RISE_MAX : d;
}

static void ff_note(tl_t *t, int64_t now, int tilt, const struct tl_box *pick, const struct tl_box *hd, int ff, int ffw, int fh) {
    if (pick) {
        if ((int64_t)pick->y1 * 100 <= (int64_t)FF_TOP_PCT * fh) { /* the bottoms of the last 3 consecutive top-clipped boxes (a gap or an unclipped box restarts it) */
            if (!t->ff_y2n || now - t->ff_y2t > 1000) t->ff_y2n = 0;
            if (t->ff_y2n < 3) t->ff_y2h[t->ff_y2n++] = pick->y2;
            else { t->ff_y2h[0] = t->ff_y2h[1]; t->ff_y2h[1] = t->ff_y2h[2]; t->ff_y2h[2] = pick->y2; }
            t->ff_y2t = now;
        } else t->ff_y2n = 0;
        if (ff) {
            if (!t->ff_set) { t->ff_set = 1; t->ff_tilt0 = tilt; t->ff_y2ref = pick->y2; }
            if (pick->y2 > t->ff_y2ref) t->ff_y2ref = pick->y2;
            t->ff_last = now;
        } else if (ffw && t->ff_set) t->ff_last = now;
        if ((int64_t)(pick->y2 - pick->y1) * 100 >= (int64_t)FF_NEAR_PCT * fh) t->ff_low0 = 0;
        else if (t->ff_set) {
            if (!t->ff_low0 || now - t->ff_low_last > 1000) t->ff_low0 = now;
            t->ff_low_last = now;
            if (now - t->ff_low0 >= FF_CLEAR_MS) t->ff_set = 0;
        }
        t->last_ff = ffw;
    } else if (hd) { /* held by the head alone: a big head is a close person, a small one is not (the flag does not go stale) */
        int big = (int64_t)(hd->y2 - hd->y1) * 100 >= (int64_t)FF_HEAD_PCT * fh;
        t->last_ff = big;
        if (big && t->ff_set) t->ff_last = now;
    }
}

/* A frame-filling person the detector missed for a moment (night IR at close range: boxes on 18-61 % of the inferences): not lost, nothing moves, no "lost" step, no search. 1 = handled. */
static int ff_bridge(tl_t *t, int64_t now_ms, struct tl_out *out) {
    const struct tl_cfg *c = &t->cfg;
    int64_t lim = c->close_bridge_ms, gap = now_ms - t->last_seen_ms;
    if (!t->last_ff || !t->has_target || !t->last_seen_ms || t->nl_phase || t->lost_i >= 0 || t->lost_done) return 0;
    if (gap >= lim) { t->search_n = SEARCH_MAX; return 0; } /* the bridge ran out: the legacy exit search is explicitly off for this loss (it used to stay quiet only because bridge + 0.7 s > the 3 s grace) */
    t->dbg.why = "close-bridge";
    t->mv_x = t->mv_y = 0;
    if (t->vact) emit_vel(t, out, now_ms, 0, 0);
    return 1;
}

static void tl_update_v(tl_t *t, int64_t now_ms, const struct tl_box *boxes, int n, const struct tl_box *heads, int nh, int fw, int fh, const struct tl_ptz *ptz, struct tl_out *out) {
    const struct tl_cfg *c = &t->cfg;
    const struct tl_box *pick = 0;
    int i, m = c->invert_pan ? -1 : 1, kt = c->invert_tilt ? 1 : -1, sticky = 0, stale, cpan, ctilt, rule, tclip = 0, hold_tilt = 0, vdx = 0, vdy = 0, fresh_gap = 0;
    int ff = 0, ffw = 0, ex = 0, ey = 0, halfx, halfy, wide = 0, engaged, tilt_hold = 0, near = 0, hd_only = 0, use6 = 0, hold6 = 0, tclip6 = 0, keep_vy = 0;
    const struct tl_box *hd = 0;
    int64_t cap = ptz->cap_ms ? ptz->cap_ms : now_ms;
    int rpan_lo = ptz->pan - ptz->pan_min, rpan_hi = ptz->pan_max - ptz->pan, rt_lo = ptz->tilt - ptz->tilt_min, rt_hi = ptz->tilt_max - ptz->tilt;
    out->kind = TL_NONE;
    out->dpan = out->dtilt = out->speed = 0;
    out->pan_dir = out->pan_div = out->tilt_dir = out->tilt_div = 0;
    out->route_op = 0;
    out->route_name[0] = 0;
    out->route_secs = 0;
    memset(&t->dbg, 0, sizeof t->dbg);
    t->dbg.why = "no-input";
    if (n > TL_MAX_BOXES) n = TL_MAX_BOXES;
    if (n < 0) n = 0;
    if (nh > TL_MAX_HEADS) nh = TL_MAX_HEADS;
    if (nh < 0 || !heads) nh = 0;
    t->tnow = now_ms;
    cpan = ptz->has_cap ? ptz->cap_pan : ptz->pan;
    ctilt = ptz->has_cap ? ptz->cap_tilt : ptz->tilt;
    stale = !ptz->has_cap && (ptz->moving || (ptz->motion_end_ms && cap < ptz->motion_end_ms + c->settle_ms));
    halfx = (int)((int64_t)c->zone_x_pct * c->gain_pan / 200);

    if (stale) { /* the pose at capture is unknown: nothing is learnt, the running command is refreshed (bounded by its age) */
        t->dbg.why = "stale";
        t->fresh_loss_ms = 0;
        refresh_last(t, out, now_ms);
        return;
    }
    if (t->has_target) { /* sticky: nearest to the predicted position in WORLD terms, a size cue for people crossing */
        int64_t best = (int64_t)c->sticky_gate_pct * c->gain_pan / 100 + 1, bestc = 0;
        const struct tl_box *pickc = 0;
        long tw = t->sm_valid ? t->sm_wx + (long)((int64_t)t->vx * (cap - t->lastcap_ms) / 1000) : t->target_w;
        if (t->lastcap_ms && cap > t->lastcap_ms) best += (cap - t->lastcap_ms > 2000 ? 2000 : cap - t->lastcap_ms) * 600 / 1000; /* the gate grows with the time since the last fresh sighting (a person walks up to ~600 steps/s) */
        for (i = 0; i < n; i++) {
            long bw;
            int64_t d;
            if (!valid(&boxes[i])) continue;
            bw = cpan + m * offset_steps(boxes[i].x1 + boxes[i].x2 - fw, fw, c->gain_pan);
            d = iabs64(bw - tw);
            if (iabs64(bw - t->target_w) < d) d = iabs64(bw - t->target_w); /* the predicted position, or where it was last seen (a feint / reversal makes the prediction wrong) */
            if (t->box_h > 0) {
                int dh = (int)iabs64((boxes[i].y2 - boxes[i].y1) - t->box_h);
                d += dh * 6; /* the same person keeps its apparent size */
                if (boxes[i].y1 > HEAD_CLIP_PX && boxes[i].y2 < fh - HEAD_CLIP_PX && t->target.y1 > HEAD_CLIP_PX && t->target.y2 < fh - HEAD_CLIP_PX && dh * 100 > 12 * t->box_h &&
                    now_ms - t->last_seen_ms < 700) d += 1000; /* two whole bodies of clearly different apparent size: another person, not adopted while the target is only briefly missing */
            }
            if (iabs64(t->vx) >= V_MIN && cap > t->lastcap_ms && cap - t->lastcap_ms < 1500) { /* a candidate that would have to move against the established direction is another person */
                int64_t imp = (bw - t->target_w) * 1000 / (cap - t->lastcap_ms);
                if (iabs64(imp) > 100 && sgn((int)imp) != sgn(t->vx)) d += 150;
            }
            if (d < best) { best = d; pick = &boxes[i]; sticky = 1; }
            else if (close_same(t, &boxes[i], bw, now_ms, fw, fh) && (!pickc || d < bestc)) { bestc = d; pickc = &boxes[i]; } /* a close box that overlaps the target (world terms) lifts the gate: the nearest of them, only when no box is inside the gate */
        }
        if (!pick && pickc) { pick = pickc; sticky = 1; }
    }
    if (!pick && !(t->has_target && t->last_seen_ms && now_ms - t->last_seen_ms < SWAP_MS)) /* another person while the target is just occluded: not adopted yet */
        for (i = 0; i < n; i++)
            if (valid(&boxes[i]) && (!pick || area(&boxes[i]) > area(pick))) pick = &boxes[i];

    hd = head_assoc(t, pick, heads, nh, cap, cpan, ctilt, fw, fh);
    hd_only = !pick && hd;
    if (hd_only) sticky = 1; /* the head of the followed target: it continues the track (no new target, no loss) */
    if (pick || hd_only) { /* the loss episode: over once the target has been held continuously for LE_CONFIRM_MS */
        if (t->le_on) {
            if (!t->le_hold0 || now_ms - t->le_last > LE_GAP_MS) t->le_hold0 = now_ms;
            t->le_last = now_ms;
            if (now_ms - t->le_hold0 >= LE_CONFIRM_MS) t->le_on = t->le_moved = 0;
        }
        if (t->nl_phase == NL_VSCAN) { t->vs_ev = VSEV_FOUND; t->vs_ev_tilt = ptz->tilt; }
    }
    if ((pick || hd_only) && t->nl_phase) { /* any box ends the close-loss machine at once (a held one too) */
        t->nl_phase = 0;
        t->fresh_loss_ms = 0; /* a held (outlier) box does not end the loss for the other logic, but the next no-box tick starts a fresh one: HOLD again, a fresh search budget */
        if (t->vact) emit_vel(t, out, now_ms, 0, 0);
    }
    if (pick || hd_only) {
        long topw = 0, cenw = 0, w, wy;
        const struct tl_box *mb = pick ? pick : hd; /* the box the decision is logged / measured from (the head when there is no person box) */
        int d2x = mb->x1 + mb->x2 - fw, d2y = mb->y1 + mb->y2 - fh, ty, bw_ = 0;
        if (pick) {
            topw = kt * ctilt + offset_steps(2 * pick->y1 - fh, fh, c->gain_tilt);
            cenw = kt * ctilt + offset_steps(pick->y1 + pick->y2 - fh, fh, c->gain_tilt);
        }
        if (!sticky) { /* a new target: it must be seen confirm_ticks times before anything moves */
            t->streak = 0; t->hn = 0; t->sm_valid = 0; t->dw_n = 0; t->wprev = 0; t->out_n = 0; t->mv_x = t->mv_y = 0; t->up_ms = 0; t->amb_latch = 0; t->hcw_valid = 0; t->head_ms = 0;
            rv_reset(t);
        } else if (pick && !(hd && box_cut_like(c, pick, fw, fh)) && /* a head is the better vertical reference of a cut box: no outlier test on the torso top */
                   !(pick->y2 >= fh - HEAD_CLIP_PX && (pick->y2 - pick->y1) < t->box_h && (pick->y2 - pick->y1) * 100 < HEAD_CUT_PCT * fh && kf_down(t)) && /* a head-only crop at the bottom of a person who was moving down is no outlier */
                   outlier(t, pick, now_ms, fh, topw, cenw, t->wprev, t->ptopw, t->pcenw)) {
            t->dbg.why = "outlier"; /* an ambiguous partial box: held (the running command goes on, bounded by the lease) */
            t->dbg.hold = 1;
            t->dbg.has_box = 1;
            t->dbg.box = *pick;
            refresh_last(t, out, now_ms);
            if (t->out_down) { /* a downward jump of a near box: the tilt and the vertical filter are frozen, the pan goes on */
                t->mv_y = 0;
                if (out->kind == TL_VEL && out->tilt_dir) { out->tilt_dir = out->tilt_div = 0; if (!out->pan_dir) { out->kind = TL_STOP; t->vact = 0; t->cmd_px = t->cmd_py = 0; } else { t->lastv = *out; t->cmd_py = 0; } }
            }
            return;
        }
        if (!t->has_target && (!t->last_seen_ms || now_ms - t->last_seen_ms > EPISODE_MS)) { t->tilt_ref_set = 0; t->ff_set = 0; }
        if (pick) {
            rule = head_rule(t, pick, fw, fh, &ty);
            bw_ = pick->x2 - pick->x1;
            if ((int64_t)bw_ * 100 > (int64_t)WIDE_PCT * fw && ((pick->x1 <= HEAD_CLIP_PX) + (pick->x2 >= fw - HEAD_CLIP_PX)) != 1) { wide = 1; t->dbg.wide = 1; }
        } else { rule = 6; ty = 0; }
        if (hd) { /* rule 6: the head is the reference when the person box is cut / wide / absent (a whole body keeps its centre rule) */
            use6 = !pick || rule != 4 || wide;
            t->hcw = kt * ctilt + offset_steps(hd->y1 + hd->y2 - fh, fh, c->gain_tilt);
            t->hcw_valid = 1;
            t->dbg.head = use6 ? 1 : 2;
        }
        if (use6) {
            rule = 6;
            ty = (int)((int64_t)fh * c->head_row_pct / 100);
            d2x = hd->x1 + hd->x2 - fw;
            d2y = hd->y1 + hd->y2 - 2 * ty;
            tclip6 = hd->y1 <= HEAD_CLIP_PX;
            wide = 0; t->dbg.wide = 0; bw_ = 0;
            t->head_ms = now_ms;
        } else if (pick && !hd && t->sm_rule == 6 && t->sm_valid && t->head_ms && now_ms - t->head_ms <= HEAD_HOLD_MS && cap - t->kf_t < 1400 && (rule == 1 || rule == 2 || rule == 5 || rule == 7)) {
            hold6 = 1; /* a cut box with no head right after head frames: its top is not a vertical reference, the vertical filter stays on the head */
            t->dbg.head = 3;
        }
        t->dbg.rule = rule;
        t->dbg.ty = ty;
        t->dbg.has_box = 1;
        t->dbg.box = use6 ? *hd : *mb; /* rule 6: the head (the log prints its centre as the reference) */
        if (pick) { /* frame-filling: top-clipped and >= FF_H_PCT tall (the state is updated after the measurement, ff_note) */
            ffw = (int64_t)pick->y1 * 100 <= (int64_t)FF_TOP_PCT * fh && (int64_t)(pick->y2 - pick->y1) * 100 >= (int64_t)FF_H_PCT * fh; /* the top just inside the view counts for the episode (night IR 12,9,.. / 179,17,..) */
            ff = pick->y1 <= HEAD_CLIP_PX && ffw;
        }
        if (rule == 1) tclip = 1;
        else if (rule == 6) tclip = tclip6;
        else if (rule == 2 || rule == 7) d2y = 2 * (pick->y1 - ty);
        else if (rule == 5) d2y = 0;
        w = cpan + m * offset_steps(d2x, fw, c->gain_pan);
        wy = ctilt + kt * offset_steps(d2y, fh, c->gain_tilt);
        { /* the camera rate at the capture instant: a moving camera makes the pose read less certain (the filters trust the frame less, a vertical rate made then is not believed) */
            int fb = ptz->cont && t->vact ? 2 * CAM_MOVING : 0; /* no measured rate: the camera is taken as moving only while a velocity command is on */
            int rx = ptz->cap_rate_ok ? ptz->cap_rate_pan : fb, ry = ptz->cap_rate_ok ? ptz->cap_rate_tilt : fb;
            t->cam_rx = rx;
            t->cam_ry = ry;
            t->cmv = ((t->cmv << 1) | (abs(rx) > CAM_MOVING || abs(ry) > CAM_MOVING)) & 7;
            t->cmvy = ((t->cmvy << 1) | (abs(ry) > CAM_MOVING)) & 7;
        }
        t->kf_force = pick && sticky && close_same(t, pick, cpan + m * offset_steps(pick->x1 + pick->x2 - fw, fw, c->gain_pan), now_ms, fw, fh);
        if (kf_measure(t, use6 ? hd : pick, rule, wide, fw, fh, cap, w, wy, !sticky, hold6)) { /* inconsistent with the 3D track: held until a second frame agrees */
            t->dbg.why = "outlier";
            t->dbg.kf = 1;
            refresh_last(t, out, now_ms);
            return;
        }
        ff_note(t, now_ms, ptz->tilt, pick, hd, ff, ffw, fh);
        if (pick) { t->wprev = 1; t->ptopw = topw; t->pcenw = cenw; }
        if (pick && !use6) { /* a torso-only top is no history of the head */
            dw_push(t, topw, pick->y1 <= HEAD_CLIP_PX, fh);
            if (up_trend(t, fh)) t->up_ms = now_ms;
        }
        if (pick) hist_push(t, pick);
        t->last_rule = rule;
        if (pick) t->last_nl = rule == 7 || (pick->y2 >= fh - HEAD_CLIP_PX && (int64_t)(pick->y2 - pick->y1) * 100 >= (int64_t)HEAD_CUT_PCT * fh) || (t->kd_valid && t->dist_mm < NL_NEAR_MM);
        else if (hd) t->last_nl = (t->kd_valid && t->dist_mm < NL_NEAR_MM) || hd->y2 >= fh - HEAD_CLIP_PX; /* held by a head alone (rule 6): the close-range evidence still counts, a head-held close target lost afterwards enters the close-loss machine */
        t->nl_phase = 0;
        t->ms_ep = 0; t->ms_until = 0; t->ms_moves = 0; /* a person again: the motion-aware search of that loss is over (the cooldown stays) */
        if (!t->has_target) t->seen0_ms = now_ms;
        t->exit_set = 0; t->search_n = 0; t->sweep_n = 0; t->sweeping = 0; t->fresh_loss_ms = 0; t->search_until = 0;
        if (t->lost_i >= 0 || t->lost_done || t->lost_route_on) { /* a person again: the lost chain ends (a route it started is stopped) */
            if (t->lost_route_on) { out->route_op = 2; t->lost_route_on = 0; }
            t->lost_i = -1;
            t->lost_done = 0;
            t->ls_init = 0;
        }
        t->lstep = "idle";
        if (t->tback_on) t->tback_int_ms = now_ms; /* a box interrupted the tilt-back */
        t->tback_on = 0;
        t->tback_done = 0;
        if (pick) t->target = *pick;
        t->has_target = 1;
        t->streak++;
        t->last_seen_ms = now_ms;
        t->lastcap_ms = cap;
        if (pick) { t->box_h = pick->y2 - pick->y1; t->box_w = pick->x2 - pick->x1; }
        t->last_near = rule == 1 || (t->kd_valid && t->dist_mm < TBACK_NEAR_MM);
        t->target_w = w;
        t->home_pending = 1;
        t->home_retry_n = 0;
        t->home_retry_at = 0;
        if (!t->tilt_ref_set) { t->tilt_ref_set = 1; t->tilt_ref = ptz->tilt; t->clip_n = 0; t->tilt_unclip_set = 0; t->amb_tilt_set = 0; }
        if (pick && !use6) up_note(t, ptz, pick, fh);
        if (rule == 6) { if (tclip6) t->clip_n++; else t->clip_n = 0; }
        else if (rule == 1 && !t->amb_up) t->clip_n++; else if (!(rule == 2 && (int64_t)pick->y1 * 100 < (int64_t)8 * fh)) t->clip_n = 0; /* a near frame whose top is only just inside the view does not undo a clip confirmation */
        rv_push(t, cap, w, wy);
        t->sw_st = SW_IDLE;
        t->vx = vel_gate(t->kox, t->vx);
        t->vy = vel_gate(t->koy, t->vy);
        if (t->cmvy & 3) t->vy = 0; /* a vertical rate from frames captured while the tilt moved is compensation error */
        if (pick) {   /* an approaching person: the box grows fast, its vertical geometry says little */
            int64_t a = area(pick), pa = t->prev_area, pm = t->prev_ms;
            t->prev_area = a;
            t->prev_ms = now_ms;
            if (pm && now_ms - pm <= 800 && pa > 0 && a * 100 > pa * HEAD_GROW_PCT && !use6) hold_tilt = 1; /* the box grows fast: its centre says little (rule 4) and a DOWN command is not trusted; an UP one is never blocked */
        }
        engaged = sticky && t->nudged && now_ms - t->last_seen_ms <= (int64_t)c->lost_grace_s * 1000;
        if (t->streak < (engaged ? 1 : c->confirm_ticks)) { t->dbg.why = "confirm"; emit_vel(t, out, now_ms, 0, 0); return; }
        {
            int64_t ah = (now_ms - cap) + ACT_MS, lim = c->gain_pan / 2;
            int64_t px = (int64_t)t->vx * ah / 1000, py = (int64_t)t->vy * ah / 1000;
            px = px > lim ? lim : px < -lim ? -lim : px;
            py = py > lim ? lim : py < -lim ? -lim : py;
            ex = (int)(t->sm_wx + px - ptz->pan);
            ey = (int)(t->sm_wy + py - ptz->tilt);
        }
        halfy = (int)((int64_t)((rule == 4 || rule == 0) ? c->zone_y_pct : c->zone_head_pct) * c->gain_tilt / 200);
        if (rule == 6) halfy = (int)((int64_t)c->head_band_pct * c->gain_tilt / 100);
        if (tclip6) { /* the head top is clipped: up, at least one head height (the clipped centre under-reads it) */
            int mag = -kt * ey, hm = c->gain_tilt * (hd->y2 - hd->y1) / fh;
            ey = -kt * (mag > hm ? mag : hm);
            halfy = 0;
        } else if (tclip && rule != 6) { /* head out of the frame: up, in proportion to the box height (nearer = more is cut) */
            int hh = (pick->y2 - pick->y1) * 100 < HEAD_CUT_PCT * fh ? HEAD_CUT_PCT * fh / 100 : pick->y2 - pick->y1;
            ey = -kt * (c->gain_tilt * hh / fh / 3);
            halfy = 0;
        }
        if (hold6) { /* head frames just before, none now: the box top is not used; an ongoing tilt command goes on for one lease, the vertical filter is untouched */
            ey = 0;
            tilt_hold = 1;
            t->dbg.why = "head-hold";
            if (t->mv_y && now_ms - t->head_ms <= c->lease_ms) keep_vy = t->cmd_py; else t->mv_y = 0;
        } else if (rule == 5 || (hold_tilt && rule == 4)) { if (hold_tilt && rule != 5) t->dbg.why = "approach"; ey = 0; t->mv_y = 0; tilt_hold = 1; } /* no tilt decision (the real band stays in halfy, the log shows it) */
        near = t->kd_valid && t->dist_mm < NEAR_MM;
        if (near && !tclip && rule != 7 && rule != 6) halfy *= 2; /* closer than 0.8 m the head row is unreliable: a larger vertical dead band (not for a low crop: its top is the head) */
        t->dbg.d2y = d2y;
        t->dbg.upl = t->amb_up;
        int hx, pv, pcap;
        if (wide) { ex = 0; t->mv_x = 0; }
        {
            long pmid = 0;
            int pcg;
            pace_push(t, cap, t->sm_wx);
            pcg = pacing(t, now_ms, c->gain_pan, &pmid);
            if (pcg) { ex = (int)(pmid - ptz->pan); hx = PACE_DEAD; }
            else hx = t->last_ff ? 2 * halfx : halfx;
            pv = pcg ? 0 : t->vx;
            pcap = pcg ? c->rate_k / 6 : 0;
        }
        vdx = axis_ctl(c, ex, pv, hx, &t->mv_x, rpan_lo, rpan_hi, pcap ? pcap : (int64_t)bw_ * 100 > (int64_t)NEAR_W_PCT * fw ? c->rate_k / 5 : 0);
        if (c->tilt_enable && !tilt_hold) {
            int was = t->mv_y;
            vdy = axis_ctl(c, ey, t->vy, halfy, &t->mv_y, rt_lo, rt_hi, 0);
            if (near && !was && vdy && !tclip && rule != 7 && rule != 6 && iabs64(t->cam_rx) > 200) { vdy = 0; t->mv_y = 0; t->tstart_n = 0; } /* close: no tilt start while the camera pans fast (rules 1, 6, 7 excepted) */
        else if (near && !was && vdy && !tclip && rule != 7) { if (++t->tstart_n < 2) { vdy = 0; t->mv_y = 0; } } /* close: a tilt starts only on 2 consistent fresh frames */
            else if (!vdy || was) t->tstart_n = 0;
        } else if (keep_vy) {
            vdy = keep_vy;
        } else {
            t->mv_y = 0;
        }
        if (vdy && hold_tilt && (c->invert_tilt ? -1 : 1) * vdy < 0) { vdy = 0; t->mv_y = 0; t->dbg.why = "approach"; } /* the approach hold: a downward command only */
        if (vdy && t->tilt_ref_set && t->clip_n < CLIP_CONFIRM) { /* cumulative tilt UP budget (far: from the episode start; near: from the last well framed tilt; ambiguous latch: from its start) */
            int up = c->invert_tilt ? -1 : 1, allowed = up_allowed(t, ptz, rule, mb, fh), lead = c->rate_k / axis_div(c, vdy) * 3 / 10; /* the travel of the command (about 0.3 s of it: the budget is never overshot by a tick) */
            if (up * vdy > 0 && allowed - lead <= MIN_STEP) { vdy = 0; t->mv_y = 0; t->dbg.why = "tilt-cap"; }
        }
        if (vdy && t->ff_set && !t->amb_up && (pick ? (int64_t)(pick->y2 - pick->y1) * 100 >= (int64_t)FF_NEAR_PCT * fh : now_ms - t->ff_last <= FF_GAP_MS)) { /* a close person never walks the head up: at most FF_CAP steps from the pose where it became frame-filling, for every box that is still close (>= 77 % tall) and for a head-only frame while the sighting is fresh (a head-out confirmed by a rising head, amb_up, has its own cap) */
            int up = c->invert_tilt ? -1 : 1, lead = c->rate_k / axis_div(c, vdy) * 3 / 10;
            if (up * vdy > 0 && up * (ptz->tilt - t->ff_tilt0) + lead >= FF_CAP + ff_rise(t, now_ms, fh)) { vdy = 0; t->mv_y = 0; t->dbg.why = "tilt-cap"; }
        }
        t->dbg.dp = ex;
        t->dbg.dt = ey;
        t->dbg.ex = ex; t->dbg.ey = ey; t->dbg.hx = halfx; t->dbg.hy = halfy; t->dbg.mvx = t->mv_x; t->dbg.mvy = t->mv_y; t->dbg.vdx = vdx; t->dbg.vdy = vdy; t->dbg.kf = t->kf_note;
        if (!t->dbg.why || !strcmp(t->dbg.why, "no-input")) t->dbg.why = (vdx || vdy) ? "vel" : "in-zone";
        if ((vdx || vdy) && !t->ep_active) { /* a motion episode starts: its start pose is the pre-move pose (unless it follows another within 0.6 s) */
            if (!t->last_move_ms || now_ms - t->last_move_ms > 600) { t->pre_pan = ptz->pan; t->pre_tilt = ptz->tilt; }
            t->ep_active = 1;
            t->nstart++;
        }
        if (vdx || vdy) t->pre_valid = iabs64(ptz->pan - t->pre_pan) >= c->gain_pan / 3 || iabs64(ptz->tilt - t->pre_tilt) >= c->gain_tilt / 3;
        emit_vel(t, out, now_ms, vdx, vdy);
        return;
    }

    /* ---- no usable box: the loss. Sweep back after an overshoot, exit search, prediction, then the guard return ---- */
    t->dbg.why = n ? "no-valid-box" : "no-person";
    if (ff_bridge(t, now_ms, out)) return;
    /* ---- the lost-target chain (track_lost_policy): ordered recovery steps, a detection ends it ---- */
    if (c->lost_n >= 0) {
        int64_t gap2;
        int guard;
        if (t->lost_done || !t->has_target) { /* the chain is over (or nobody was followed): nothing more, a new person starts a new target */
            t->lstep = t->lost_done ? "done" : "idle";
            if (tilt_back(t, now_ms, ptz, rt_lo, rt_hi, out)) return;
            if (t->vact) emit_vel(t, out, now_ms, 0, 0);
            return;
        }
        gap2 = now_ms - t->last_seen_ms;
        if (!t->fresh_loss_ms) t->fresh_loss_ms = now_ms;
        fresh_gap = (int)(now_ms - t->fresh_loss_ms);
        if (!t->exit_set) { t->exit_set = 1; t->exit_dir = t->last_ff || t->ff_set ? TL_EXIT_NONE : exit_dir(t, fw, fh); t->exit_tilt0 = ptz->tilt; } /* a frame-filling loss has no direction: it is too close, not gone up */
        t->dbg.exit = t->exit_dir;
        if (near_loss(t, now_ms, ptz, fw, fh, halfx, rpan_lo, rpan_hi, rt_lo, rt_hi, out)) return;
        if (t->lost_i < 0) { t->lost_i = 0; t->ls_init = 0; }
        t->dbg.why = "lost";
        for (guard = 0; guard <= TL_MAX_STEPS; guard++) {
            const struct tl_step *st;
            int next = 0;
            if (t->lost_i >= c->lost_n) {
                lost_end(t, out, now_ms);
                return;
            }
            st = &c->lost[t->lost_i];
            if (!t->ls_init) { t->ls_init = 1; t->lost_t0 = now_ms; t->ls_route_seen = 0; if (st->kind == TLS_LOCAL) { t->ls_center = t->target_w; t->ls_dir = t->vx < 0 ? -1 : 1; } }
            switch (st->kind) {
            case TLS_HOLD:
                t->lstep = "hold";
                if (tilt_back(t, now_ms, ptz, rt_lo, rt_hi, out)) return;
                if (t->vact) { emit_vel(t, out, now_ms, 0, 0); return; }
                if (now_ms - t->lost_t0 >= (int64_t)st->a * 1000) next = 1;
                else { t->dbg.why = "lost-hold"; return; }
                break;
            case TLS_BACKSWEEP: {
                if (t->nl_phase) { next = 1; break; } /* a near target had its own search */
                int edge = t->exit_dir == TL_EXIT_LEFT || t->exit_dir == TL_EXIT_RIGHT || t->exit_dir == TL_EXIT_UP;
                int sx = 0, sy = 0;
                t->lstep = "backsweep";
                if (edge || !t->pre_valid || now_ms - t->lost_t0 > 8000 || (!t->sweeping && (t->sweep_n >= 1 || now_ms - t->last_move_ms > BACK_WINDOW_MS + 1500))) { t->sweeping = 0; next = 1; break; }
                if (!t->sweeping && fresh_gap < 500) { /* wait for fresh no-box frames: meanwhile the prediction */
                    if (predict_gap(t, now_ms, ptz, gap2, halfx, rpan_lo, rpan_hi, rt_lo, rt_hi, out)) return;
                    return;
                }
                {
                    int dpn = t->pre_pan - ptz->pan, dtl = t->pre_tilt - ptz->tilt;
                    if (!t->sweeping) { t->sweeping = 1; t->sweep_n++; t->sweep_sx = iabs64(dpn) > 40 ? sgn(dpn) : 0; t->sweep_sy = c->tilt_enable && iabs64(dtl) > 40 ? sgn(dtl) : 0; }
                    if (t->sweep_sx && sgn(dpn) == t->sweep_sx && iabs64(dpn) > 40) sx = t->sweep_sx * (c->rate_k / SWEEP_DIV);
                    if (t->sweep_sy && sgn(dtl) == t->sweep_sy && iabs64(dtl) > 40) sy = t->sweep_sy * (c->rate_k / SWEEP_DIV);
                    if ((sx > 0 && rpan_hi <= ROOM_MIN) || (sx < 0 && rpan_lo <= ROOM_MIN)) sx = 0;
                    if ((sy > 0 && rt_hi <= ROOM_MIN) || (sy < 0 && rt_lo <= ROOM_MIN)) sy = 0;
                    if (!sx && !sy) { t->sweeping = 0; t->pre_valid = 0; next = 1; break; }
                    t->dbg.why = "sweep-back";
                    emit_vel(t, out, now_ms, sx, sy);
                    return;
                }
            }
            case TLS_EXIT: {
                int ed = t->exit_dir, dx = 0, dy = 0;
                t->lstep = "exit";
                if (t->nl_phase) { next = 1; break; }
                if (!ed || (t->search_n >= SEARCH_MAX && t->search_until <= 0)) { next = 1; break; }
                if (t->search_until > 0 && now_ms >= t->search_until) { t->search_until = -now_ms; emit_vel(t, out, now_ms, 0, 0); return; }
                if (!t->search_until && fresh_gap < LOSS_FRESH_MS) { if (predict_gap(t, now_ms, ptz, gap2, halfx, rpan_lo, rpan_hi, rt_lo, rt_hi, out)) return; return; }
                if (!t->search_until && t->search_n < SEARCH_MAX) { t->search_until = now_ms + SEARCH_MS; t->search_n++; }
                else if (t->search_until < 0 && now_ms + t->search_until >= SEARCH_GAP_MS && t->search_n < SEARCH_MAX) { t->search_until = now_ms + SEARCH_MS; t->search_n++; }
                if (t->search_until > 0 && now_ms < t->search_until) {
                    int sp = c->rate_k / SWEEP_DIV;
                    switch (ed) {
                    case TL_EXIT_DOWN: dy = c->tilt_enable ? kt * sp : 0; break;
                    case TL_EXIT_UP: dy = c->tilt_enable && (c->invert_tilt ? -1 : 1) * (ptz->tilt - t->exit_tilt0) < EXIT_UP_BUDGET ? -kt * sp : 0; break; /* its own budget from the loss pose (the room check below clips it to the soft range) */
                    case TL_EXIT_LEFT: dx = -m * sp; break;
                    case TL_EXIT_RIGHT: dx = m * sp; break;
                    default: break;
                    }
                    if ((dx > 0 && rpan_hi <= ROOM_MIN) || (dx < 0 && rpan_lo <= ROOM_MIN)) dx = 0;
                    if ((dy > 0 && rt_hi <= ROOM_MIN) || (dy < 0 && rt_lo <= ROOM_MIN)) dy = 0;
                    if (dx || dy) { t->dbg.why = "exit-search"; emit_vel(t, out, now_ms, dx, dy); return; }
                    t->search_n = SEARCH_MAX; t->search_until = 0; /* at a limit: nothing to search that way */
                    next = 1;
                    break;
                }
                if (t->vact) { emit_vel(t, out, now_ms, 0, 0); return; }
                return; /* the gap between two searches */
            }
            case TLS_LOCAL: {
                int dx;
                long lo_ok = (long)ptz->pan_min + ROOM_MIN + st->a / 2, hi_ok = (long)ptz->pan_max - ROOM_MIN - st->a / 2;
                t->lstep = "local";
                if (ptz->over_budget || lo_ok > hi_ok) { next = 1; break; } /* the travel budget is used up, or the window does not fit the soft range: skipped */
                if (t->ls_center < lo_ok) t->ls_center = lo_ok; /* the whole window stays inside the soft range minus the margin */
                if (t->ls_center > hi_ok) t->ls_center = hi_ok;
                if (now_ms - t->lost_t0 >= (int64_t)st->c * 1000) { if (t->vact) { emit_vel(t, out, now_ms, 0, 0); return; } next = 1; break; }
                if (ptz->pan <= t->ls_center - st->a / 2) t->ls_dir = 1;
                else if (ptz->pan >= t->ls_center + st->a / 2) t->ls_dir = -1;
                dx = t->ls_dir * (c->rate_k / st->b);
                if ((dx > 0 && rpan_hi <= ROOM_MIN) || (dx < 0 && rpan_lo <= ROOM_MIN)) { t->ls_dir = -t->ls_dir; dx = -dx; }
                if ((dx > 0 && rpan_hi <= ROOM_MIN) || (dx < 0 && rpan_lo <= ROOM_MIN)) { next = 1; break; } /* both sides blocked */
                t->dbg.why = "local-sweep";
                emit_vel(t, out, now_ms, dx, 0);
                return;
            }
            case TLS_ROUTE: {
                int busy = ptz->route_busy;
                t->lstep = "route";
                snprintf(t->lroute, sizeof t->lroute, "%s", st->name);
                if (!t->lost_route_on && t->ls_route_seen == 0 && now_ms == t->lost_t0) { /* entering: start it, unless a patrol (the user's) already runs: never touched */
                    if (busy) { next = 1; break; }
                    out->route_op = 1;
                    snprintf(out->route_name, sizeof out->route_name, "%s", st->name);
                    out->route_secs = st->b;
                    t->lost_route_on = 1;
                    if (t->vact) emit_vel(t, out, now_ms, 0, 0);
                    return;
                }
                if (busy) t->ls_route_seen = 1;
                if (now_ms - t->lost_t0 >= (int64_t)st->b * 1000 || (t->ls_route_seen && !busy) || (!t->ls_route_seen && now_ms - t->lost_t0 > 3000)) {
                    if (t->lost_route_on) { out->route_op = 2; t->lost_route_on = 0; }
                    next = 1;
                    break;
                }
                t->dbg.why = "lost-route";
                return;
            }
            case TLS_HOME:
                t->lstep = "home";
                if (now_ms < t->home_retry_at) { t->dbg.why = "lost-home-wait"; return; } /* a refused home is asked again after a pause */
                t->home_pending = 0;
                t->nudged = 0;
                out->kind = TL_HOME;
                t->le_on = t->le_moved = 0; /* home: the loss episode is over */
                t->tilt_ref_set = 0;
                t->ff_set = 0;
                t->tilt_pending = 0;
                t->dbg.why = "lost-home";
                t->lost_i++;
                t->ls_init = 0;
                return;
            default: /* stay */
                t->lstep = "stay";
                lost_end(t, out, now_ms);
                return;
            }
            if (next) { t->lost_i++; t->ls_init = 0; }
        }
        return;
    }
    if (t->has_target && t->last_seen_ms && now_ms - t->last_seen_ms < (int64_t)c->lost_grace_s * 1000) {
        int64_t gap = now_ms - t->last_seen_ms;
        int sx = 0, sy = 0, edge;
        t->dbg.why = "gap-hold";
        if (!t->fresh_loss_ms) t->fresh_loss_ms = now_ms;
        fresh_gap = (int)(now_ms - t->fresh_loss_ms);
        if (!t->exit_set) { t->exit_set = 1; t->exit_dir = t->last_ff || t->ff_set ? TL_EXIT_NONE : exit_dir(t, fw, fh); t->exit_tilt0 = ptz->tilt; } /* a frame-filling loss has no direction: it is too close, not gone up */
        t->dbg.exit = t->exit_dir;
        if (c->search_mode == TL_SEARCH_MOTION && c->lost_n < 0) { /* the motion-aware search replaces the close scan, the sweep back and the exit search */
            if (ms_loss(t, now_ms, ptz, fw, fh, rpan_lo, rpan_hi, rt_lo, rt_hi, out)) return;
            goto grace_tail;
        }
        if (near_loss(t, now_ms, ptz, fw, fh, halfx, rpan_lo, rpan_hi, rt_lo, rt_hi, out)) return;
        edge = t->exit_dir == TL_EXIT_LEFT || t->exit_dir == TL_EXIT_RIGHT || t->exit_dir == TL_EXIT_UP;
        /* A: lost shortly after a large move and not through an edge: back toward the pose before it, never beyond; the next tick with a box takes over */
        if (!t->nl_phase && !edge && t->pre_valid && (t->sweeping || (fresh_gap >= 500 && now_ms - t->last_move_ms <= BACK_WINDOW_MS && t->sweep_n < 1))) {
            int dpn = t->pre_pan - ptz->pan, dtl = t->pre_tilt - ptz->tilt;
            if (!t->sweeping) { t->sweeping = 1; t->sweep_n++; t->sweep_sx = iabs64(dpn) > 40 ? sgn(dpn) : 0; t->sweep_sy = c->tilt_enable && iabs64(dtl) > 40 ? sgn(dtl) : 0; }
            if (t->sweep_sx && sgn(dpn) == t->sweep_sx && iabs64(dpn) > 40) sx = t->sweep_sx * (c->rate_k / SWEEP_DIV);
            if (t->sweep_sy && sgn(dtl) == t->sweep_sy && iabs64(dtl) > 40) sy = t->sweep_sy * (c->rate_k / SWEEP_DIV);
            if ((sx > 0 && rpan_hi <= ROOM_MIN) || (sx < 0 && rpan_lo <= ROOM_MIN)) sx = 0;
            if ((sy > 0 && rt_hi <= ROOM_MIN) || (sy < 0 && rt_lo <= ROOM_MIN)) sy = 0;
            if (!sx && !sy) { t->sweeping = 0; t->pre_valid = 0; }
            else { t->dbg.why = "sweep-back"; emit_vel(t, out, now_ms, sx, sy); return; }
        }
        /* C: the exit search: a bounded move in the exit direction, only after fresh no-box frames */
        {
            int ed = t->exit_dir, dx = 0, dy = 0;
            if (t->search_until > 0 && now_ms >= t->search_until) { t->search_until = -now_ms; emit_vel(t, out, now_ms, 0, 0); return; } /* ended: stop, a gap before the next */
            if (t->nl_phase) ed = 0; /* a near target had its own search */
            if (!t->search_until && ed && t->search_n < SEARCH_MAX && fresh_gap >= LOSS_FRESH_MS) { t->search_until = now_ms + SEARCH_MS; t->search_n++; }
            else if (t->search_until < 0 && now_ms + t->search_until >= SEARCH_GAP_MS && t->search_n < SEARCH_MAX) { t->search_until = now_ms + SEARCH_MS; t->search_n++; }
            if (t->search_until > 0 && now_ms < t->search_until) {
                int sp = c->rate_k / SWEEP_DIV;
                switch (ed) {
                case TL_EXIT_DOWN: dy = c->tilt_enable ? kt * sp : 0; break;
                case TL_EXIT_UP: dy = c->tilt_enable && (c->invert_tilt ? -1 : 1) * (ptz->tilt - t->exit_tilt0) < EXIT_UP_BUDGET ? -kt * sp : 0; break; /* its own budget from the loss pose (the room check below clips it to the soft range) */
                case TL_EXIT_LEFT: dx = -m * sp; break;
                case TL_EXIT_RIGHT: dx = m * sp; break;
                default: break;
                }
                if ((dx > 0 && rpan_hi <= ROOM_MIN) || (dx < 0 && rpan_lo <= ROOM_MIN)) dx = 0;
                if ((dy > 0 && rt_hi <= ROOM_MIN) || (dy < 0 && rt_lo <= ROOM_MIN)) dy = 0;
                if (dx || dy) { t->dbg.why = "exit-search"; emit_vel(t, out, now_ms, dx, dy); return; }
                t->dbg.why = "exit-search-range";
            }
        }
    grace_tail:
        if (predict_gap(t, now_ms, ptz, gap, halfx, rpan_lo, rpan_hi, rt_lo, rt_hi, out)) return;
        t->mv_x = t->mv_y = 0;
        if (t->vact) emit_vel(t, out, now_ms, 0, 0); /* nothing to follow: stop (the lease would too) */
        return;
    }
    /* the grace ran out: lost (a near target first finishes the close-loss machine: its widen phase can outlast the grace) */
    if (t->has_target && t->nl_phase && near_loss(t, now_ms, ptz, fw, fh, halfx, rpan_lo, rpan_hi, rt_lo, rt_hi, out)) return;
    t->prev_ms = 0;
    t->sm_valid = 0;
    t->hcw_valid = 0;
    rv_reset(t);
    t->has_target = 0;
    t->streak = 0;
    t->mv_x = t->mv_y = 0;
    t->sweeping = 0;
    t->search_until = 0;
    if (c->search_mode == TL_SEARCH_MOTION && c->lost_n < 0 && ms_loss(t, now_ms, ptz, fw, fh, rpan_lo, rpan_hi, rt_lo, rt_hi, out)) return;
    if (tilt_back(t, now_ms, ptz, rt_lo, rt_hi, out)) return;
    if (sweep_step(t, now_ms, ptz, out)) return; /* before the stop below: a running search is re-commanded, not stopped */
    if (t->vact) { emit_vel(t, out, now_ms, 0, 0); return; }
    t->ep_active = 0;
    (void)loss_tail(t, now_ms, ptz, 0, 1, out);
}

#define HOME_RETRY_MAX 5   /* a refused tracker home (a manual hold, a drift window, a busy motor) is asked again at most this many times ... */
#define HOME_RETRY_MS 1500 /* ... this long apart */

void tl_home_refused(tl_t *t, int64_t now_ms) {
    if (t->home_retry_n >= HOME_RETRY_MAX) { t->home_retry_n = 0; t->home_retry_at = 0; return; } /* gave up: the head stays (the next loss asks again) */
    t->home_retry_n++;
    t->home_retry_at = now_ms + HOME_RETRY_MS;
    t->home_pending = 1;
    t->nudged = 1;
    if (t->cfg.lost_n >= 0 && t->lost_i > 0 && t->lost_i <= t->cfg.lost_n && t->cfg.lost[t->lost_i - 1].kind == TLS_HOME) { /* the chain moved past its home step: back to it */
        t->lost_i--;
        t->ls_init = 0;
        t->lost_done = 0;
    }
}

static void tl_update_p(tl_t *t, int64_t now_ms, const struct tl_box *boxes, int n, const struct tl_box *heads, int nh, int fw, int fh, const struct tl_ptz *ptz,
                        struct tl_out *out) {
    const struct tl_cfg *c = &t->cfg;
    const struct tl_box *pick = 0;
    int allband = 0, phold = 0, sticky = 0, i, tclip = 0, hb = 0, hband = c->tband_pct / 2, dp = 0, dt = 0, p, q, stale, modern, engaged, minv = 0, fast = 0, m, zn, cpan, ctilt, kt, halfx = 0, halfy = 0, sy_img = 0, pan_out = 0, tilt_out = 0, hold_tilt = 0;
    int64_t cap;
    out->kind = TL_NONE;
    out->dpan = out->dtilt = out->speed = 0;
    memset(&t->dbg, 0, sizeof t->dbg);
    t->dbg.why = "no-input";
    if (fw <= 0 || fh <= 0 || c->max_step <= 0) return; /* bad frame or tl_init failed */
    if (c->motion == TL_MOTION_CONT && c->settle_ms > 0 && c->zone_x_pct > 0) { tl_update_v(t, now_ms, boxes, n, heads, nh, fw, fh, ptz, out); return; }
    if (n > TL_MAX_BOXES) n = TL_MAX_BOXES;
    if (n < 0) n = 0;
    modern = c->settle_ms > 0;
    zn = modern && c->zone_x_pct > 0;
    m = c->invert_pan ? -1 : 1;
    cap = ptz->cap_ms ? ptz->cap_ms : now_ms;
    t->tnow = now_ms;
    cpan = ptz->has_cap ? ptz->cap_pan : ptz->pan;
    ctilt = ptz->has_cap ? ptz->cap_tilt : ptz->tilt;

    /* A frame is stale when the camera was (or was about to be) moving while it was captured: its boxes belong to another pose. */
    stale = ptz->moving;
    if (modern && !stale) {
        if (ptz->motion_end_ms && cap < ptz->motion_end_ms + c->settle_ms) stale = 1;
        if (t->nudged && cap <= t->last_nudge_ms + START_LAG_MS) stale = 1;
        /* a nudge whose motion end is not reported yet (queued, refused, or not started): the pose is unknown, for a bounded time */
        if (t->nudged && ptz->motion_end_ms < t->last_nudge_ms && now_ms - t->last_nudge_ms < STALE_MAX_MS) stale = 1;
    }

    if (!ptz->moving) {
        /* the pose after the last tracking nudge is recorded once its motion ended; a different pose later was set by hand (API step / move / goto / home / park): it cancels the pending return */
        if (t->settle_wait) {
            if (ptz->motion_end_ms > t->last_nudge_ms || now_ms - t->last_nudge_ms >= STALE_MAX_MS) {
                t->settle_wait = 0;
                t->has_settled = 1;
                t->settled_pan = ptz->pan;
                t->settled_tilt = ptz->tilt;
            }
        } else if (t->has_settled && (iabs64(ptz->pan - t->settled_pan) >= 4 || iabs64(ptz->tilt - t->settled_tilt) >= 4)) {
            t->has_settled = 0;
            t->home_pending = 0;
            t->tilt_pending = 0;
        }
    }

    if (modern && stale && t->has_target && n > 0) {
        /* a decisive frame may be stale (the person leaves while the camera moves): keep what it shows for the exit logic, but never match it by
         * position (the pan read of a moving camera is unknown) and never touch the target state */
        for (i = 0; i < n; i++)
            if (valid(&boxes[i]) && (!pick || area(&boxes[i]) > area(pick))) pick = &boxes[i];
        if (pick) {
            hist_push(t, pick);
            t->fresh_loss_ms = 0;
            t->last_seen_ms = now_ms; /* exit_set / search_n are NOT reset here: only a non-stale sighting ends a loss */
            t->dbg.has_box = 1;
            t->dbg.box = *pick;
            t->dbg.rule = head_rule(t, pick, fw, fh, &t->dbg.ty);
            t->dbg.why = "stale";
            return;
        }
    }
    if (t->has_target) { /* sticky: the box nearest to the previous target in WORLD position (the camera may have turned since), inside a gate */
        int64_t best = (int64_t)c->sticky_gate_pct * c->gain_pan / 100 + 1;
        long tw = t->target_w;
        if (zn && t->lastcap_ms && cap > t->lastcap_ms) best += (cap - t->lastcap_ms > 2000 ? 2000 : cap - t->lastcap_ms) * 600 / 1000; /* stale frames did not update the position: the gate grows with the time (a person walks up to ~600 steps/s) */
        if (zn && t->vx) tw += (long)((int64_t)t->vx * (cap - t->lastcap_ms) / 1000); /* the target is predicted at constant velocity */
        for (i = 0; i < n; i++) {
            long bw;
            int64_t d;
            if (!valid(&boxes[i])) continue;
            bw = cpan + m * offset_steps(boxes[i].x1 + boxes[i].x2 - fw, fw, c->gain_pan);
            d = iabs64(bw - (zn ? tw : t->target_w));
            if (zn && t->box_h > 0) d += iabs64((boxes[i].y2 - boxes[i].y1) - t->box_h) * 3; /* the same person has the same size: a tie-break for people crossing */
            if (d < best) { best = d; pick = &boxes[i]; sticky = 1; }
        }
    }
    if (!pick && zn && t->has_target && t->last_seen_ms && now_ms - t->last_seen_ms < SWAP_MS) { /* another person while the target is just occluded: not adopted yet */ }
    else if (!pick) { /* new target: the largest box */
        for (i = 0; i < n; i++)
            if (valid(&boxes[i]) && (!pick || area(&boxes[i]) > area(pick))) pick = &boxes[i];
    }

    if (!pick) {
        t->dbg.why = n ? "no-valid-box" : "no-person";
        if (modern && stale) { t->dbg.why = "stale"; t->fresh_loss_ms = 0; return; }
        if (modern && t->has_target && t->last_seen_ms && now_ms - t->last_seen_ms < (int64_t)c->lost_grace_s * 1000) {
            t->ns = 0;
            t->dbg.why = "gap-hold"; /* a short detection gap (a close person flaps): keep the target, the streak and the sticky position */
            if (!t->fresh_loss_ms) t->fresh_loss_ms = now_ms;
            if (zn && n == 0 && !ptz->moving && t->pre_valid && t->sweep_n < 2 && t->last_move_ms && now_ms - t->last_move_ms <= BACK_WINDOW_MS &&
                now_ms - t->last_seen_ms >= 400 && now_ms - t->last_nudge_ms >= 500 && now_ms - t->fresh_loss_ms >= LOSS_FRESH_MS) {
                /* lost shortly after a large move (an overshoot): the person is between the pose before it and the current one. Not when it left through an edge. */
                int sp, sq;
                if (!t->exit_set) { t->exit_set = 1; t->exit_dir = exit_dir(t, fw, fh); }
                if (t->exit_dir != TL_EXIT_LEFT && t->exit_dir != TL_EXIT_RIGHT && t->exit_dir != TL_EXIT_UP) {
                    sp = (t->pre_pan - ptz->pan) * 60 / 100;
                    sq = c->tilt_enable ? (t->pre_tilt - ptz->tilt) * 60 / 100 : 0;
                    if (iabs64(sp) < 2 * MIN_STEP) sp = 0;
                    if (iabs64(sq) < 2 * MIN_STEP) sq = 0;
                    t->sweep_n++;
                    if (sp || sq) {
                        out->kind = TL_NUDGE;
                        out->dpan = sp;
                        out->dtilt = -sq;
                        t->last_nudge_ms = now_ms;
                        t->nudged = 1;
                        t->settle_wait = 1;
                        t->has_settled = 0;
                        if (sq) t->tilt_pending = 1;
                        t->dbg.dp = sp;
                        t->dbg.dt = -sq;
                        t->dbg.why = "sweep-back";
                        return;
                    }
                    t->pre_valid = 0;
                }
            }
            if (n == 0 && !ptz->moving && (t->streak >= c->confirm_ticks || t->nudged) && now_ms - t->last_seen_ms >= 2 * (int64_t)SEARCH_GAP_MS / 3 && t->search_n < SEARCH_MAX &&
                now_ms - t->last_nudge_ms >= SEARCH_GAP_MS && (!zn || now_ms - t->fresh_loss_ms >= LOSS_FRESH_MS)) {
                /* lost while heading for an edge: look that way, a bounded step, twice at most, inside the grace */
                int sdp = 0, sdt = 0, sp, sq;
                if (!t->exit_set) { t->exit_set = 1; t->exit_dir = exit_dir(t, fw, fh); }
                t->dbg.exit = t->exit_dir;
                switch (t->exit_dir) {
                case TL_EXIT_DOWN: sdt = SEARCH_STEP; break;
                case TL_EXIT_UP: sdt = -SEARCH_STEP; break;
                case TL_EXIT_LEFT: sdp = -m * SEARCH_STEP; break;
                case TL_EXIT_RIGHT: sdp = m * SEARCH_STEP; break;
                default: break;
                }
                if (sdt && !c->tilt_enable) sdt = 0;
                if (c->invert_tilt) sdt = -sdt;
                sp = clampi(ptz->pan + sdp, ptz->pan_min, ptz->pan_max) - ptz->pan;
                sq = clampi(ptz->tilt - sdt, ptz->tilt_min, ptz->tilt_max) - ptz->tilt;
                if (iabs64(sp) < MIN_STEP) sp = 0;
                if (iabs64(sq) < MIN_STEP) sq = 0;
                t->search_n++;
                if (sp || sq) {
                    out->kind = TL_NUDGE;
                    out->dpan = sp;
                    out->dtilt = -sq;
                    out->speed = 0;
                    t->last_nudge_ms = now_ms;
                    t->nudged = 1;
                    t->settle_wait = 1;
                    t->has_settled = 0;
                    if (sq) t->tilt_pending = 1;
                    t->dbg.dp = sp;
                    t->dbg.dt = -sq;
                    t->dbg.why = "exit-search";
                } else if (t->exit_dir) t->dbg.why = "exit-search-range";
            }
            return;
        } /* a blurred frame says nothing about the person */
        t->prev_ms = 0;
        t->ns = 0; /* the velocity of a person who is gone says nothing about the next one */
        t->sm_valid = 0;
        rv_reset(t);
        t->has_target = 0;
        t->streak = 0;
        loss_tail(t, now_ms, ptz, stale, modern, out);
        return;
    }

    engaged = modern && sticky && t->nudged && t->last_seen_ms && now_ms - t->last_seen_ms <= (int64_t)c->lost_grace_s * 1000;
    if (modern && !sticky) { /* a new target: no velocity from the old one, and it must be seen twice (even inside the grace) */
        t->ns = 0;
        t->streak = 0;
        t->hn = 0;
        t->sm_valid = 0;
        t->dw_n = 0;
        t->wprev = 0;
        t->out_n = 0;
        rv_reset(t);
    }
    if (modern && !t->has_target && (!t->last_seen_ms || now_ms - t->last_seen_ms > EPISODE_MS)) t->tilt_ref_set = 0; /* a new episode */
    if (zn && !stale) { /* a sudden jump of the box top / centre vs the followed track: an ambiguous partial detection, held until a second consistent frame */
        long kt0 = c->invert_tilt ? 1 : -1, topw = kt0 * ctilt + offset_steps(2 * pick->y1 - fh, fh, c->gain_tilt), cenw = kt0 * ctilt + offset_steps(pick->y1 + pick->y2 - fh, fh, c->gain_tilt);
        if (sticky && outlier(t, pick, now_ms, fh, topw, cenw, t->wprev, t->ptopw, t->pcenw)) { t->dbg.why = "outlier"; t->dbg.has_box = 1; t->dbg.box = *pick; return; }
        t->wprev = 1; t->ptopw = topw; t->pcenw = cenw;
        dw_push(t, topw, pick->y1 <= HEAD_CLIP_PX, fh);
        if (up_trend(t, fh)) t->up_ms = now_ms;
        t->box_h = pick->y2 - pick->y1;
        t->lastcap_ms = cap;
    }
    hist_push(t, pick);
    t->exit_set = 0;
    t->search_n = 0;
    t->sweep_n = 0;
    t->fresh_loss_ms = 0;
    t->dbg.rule = modern ? head_rule(t, pick, fw, fh, &t->dbg.ty) : 0;
    t->target = *pick;
    t->has_target = 1;
    t->streak++;
    t->last_seen_ms = now_ms;
    t->home_pending = 1;
    t->dbg.has_box = 1;
    t->dbg.box = *pick;
    t->dbg.why = "stale";
    if (stale) return;
    if (modern && !t->tilt_ref_set) { t->tilt_ref_set = 1; t->tilt_ref = ptz->tilt; t->clip_n = 0; t->tilt_unclip_set = 0; t->amb_tilt_set = 0; }
    up_note(t, ptz, pick, fh);
    if (t->dbg.rule == 1 && !t->amb_up) t->clip_n++; else if (!(t->dbg.rule == 2 && (int64_t)pick->y1 * 100 < (int64_t)8 * fh)) t->clip_n = 0; /* consecutive fresh frames */

    {
        int d2x = pick->x1 + pick->x2 - fw, d2y = pick->y1 + pick->y2 - fh;
        kt = c->invert_tilt ? 1 : -1;
        if (modern && c->head_pct > 0) { /* tilt follows the head (the top of the box), not the centre: a close person fills the frame height */
            int bh = pick->y2 - pick->y1, rule = t->dbg.rule, ty = t->dbg.ty;
            if (rule == 1) tclip = 1;
            else if (rule == 4) d2y = pick->y1 + pick->y2 - fh; /* whole body: its centre */
            else if (rule == 5) d2y = 0; /* ambiguous: nothing to aim at */
            else if (rule == 2 || rule == 7) d2y = 2 * (pick->y1 - ty), hb = 1;
            else if (rule == 3) { /* a far person: its upper third at the centre; a head already in the head band is fine too (else the camera tilting down,
                                   * which shrinks a cut box into this branch, would be pulled back up: hunting) */
                int d2h = 2 * (pick->y1 - (int)((int64_t)fh * c->head_pct / 100));
                d2y = 2 * (pick->y1 + bh / 3) - fh;
                if (in_band(d2h, fh, hband) && d2y > 0) d2y = 0; /* only the down side: a head high in the band with the upper third above the centre still tilts up (symmetric) */
            }
        }
        int ox;
        t->dbg.d2y = d2y;
        ox = offset_steps(d2x, fw, c->gain_pan);
        int base = m * ox, v, lead, dx;
        long w = cpan + base;
        t->target_w = w;
        if (modern) push_sample(t, cap, w);
        if (zn) { /* smoothed reference (EMA of the world position: head / arm jitter is ignored), robust velocity, comfort zone in steps */
            long wy = ctilt + kt * offset_steps(d2y, fh, c->gain_tilt);
            if (kf_measure(t, pick, t->dbg.rule, t->dbg.wide, fw, fh, cap, w, wy, !sticky, 0)) { t->dbg.why = "outlier"; return; }
            rv_push(t, cap, w, wy);
            t->sw_st = SW_IDLE;
            t->vx = vel_gate(t->kox, t->vx);
            t->vy = vel_gate(t->koy, t->vy);
            base = (int)(t->sm_wx - ptz->pan);
            sy_img = kt * (int)(t->sm_wy - ptz->tilt);
            halfx = (int)((int64_t)c->zone_x_pct * c->gain_pan / 200);
            halfy = (int)((int64_t)((t->dbg.rule == 4 || t->dbg.rule == 0) ? c->zone_y_pct : c->zone_head_pct) * c->gain_tilt / 200);
            pan_out = iabs64(base) > halfx;
            if (!pan_out && t->vx) { /* heading out: the predicted reference leaves the zone within predict_ms */
                int64_t pr = base + (int64_t)t->vx * c->predict_ms / 1000;
                if (iabs64(pr) > halfx && iabs64(pr) > iabs64(base)) pan_out = 1;
            }
            tilt_out = t->dbg.rule != 5 && (tclip || iabs64(sy_img) > halfy);
            if (t->dbg.rule == 5) sy_img = 0;
        }
        {   /* an approaching person: the box grows fast, its geometry says little: hold this tick */
            int64_t a = area(pick), pa = t->prev_area, pm = t->prev_ms;
            t->prev_area = a;
            t->prev_ms = now_ms;
            if (modern && pm && now_ms - pm <= 800 && pa > 0 && a * 100 > pa * HEAD_GROW_PCT) hold_tilt = 1; /* approach: the box grows fast. Like the continuous path: a rule-4 (centre) tilt decision and any DOWN tilt are held, the pan and an UP tilt are not */
        }
        if (hold_tilt && t->dbg.rule == 4) { tilt_out = 0; sy_img = 0; t->dbg.why = "approach"; }
        if (t->streak < (engaged ? 1 : c->confirm_ticks)) { t->dbg.why = "confirm"; return; }
        if (t->nudged && now_ms - t->last_nudge_ms < c->min_interval_ms) { t->dbg.why = "interval"; return; }
        allband = zn ? (!pan_out && !(c->tilt_enable && tilt_out)) : (t->dbg.wide || in_band(d2x, fw, c->deadband_pct)) && (!c->tilt_enable || (!tclip && in_band(d2y, fh, hb || t->dbg.rule == 3 ? hband : c->tband_pct)));
        phold = modern && now_ms < t->hold_until; /* the pan reversal hold stops the pan only: tilt is its own axis */

        if (modern) minv = MIN_STEP;
        if (modern && (int64_t)(pick->x2 - pick->x1) * 100 > (int64_t)WIDE_PCT * fw &&
            ((pick->x1 <= HEAD_CLIP_PX) + (pick->x2 >= fw - HEAD_CLIP_PX)) != 1) {
            /* a very wide box (a close person): its horizontal centre is unreliable; only a clear one-sided cut (it touches one side edge) is used */
            t->dbg.wide = 1;
        } else if (!phold && (zn ? pan_out : !in_band(d2x, fw, c->deadband_pct))) {
            dx = base;
            if (modern && c->lead_pct > 0 && (v = zn ? t->vx : velocity(t)) != 0) {
                /* where the person will be when the camera arrives: now - capture (detector latency) + the motion time + the start lag */
                int64_t sp = beyond(d2x, fw, c->fast_pct) && c->speed_fast ? c->speed_fast : 3;
                int64_t h = (now_ms - cap) + iabs64(base) * sp + START_LAG_MS + 70;
                h = h > (zn ? 800 : 1500) ? (zn ? 800 : 1500) : h < 0 ? 0 : h;
                lead = (int)(v * h / 1000 * c->lead_pct / 100);
                lead = clampi(lead, -c->lead_max, c->lead_max);
                if (zn) { /* never lead the person out of the frame: the lead keeps the box (half its width) inside, and never exceeds what the measured speed gives in 0.8 s */
                    int lim = ((fw - (pick->x2 - pick->x1)) / 2 - 12) * c->gain_pan / fw;
                    lead = clampi(lead, -(lim > 0 ? lim : 0), lim > 0 ? lim : 0);
                }
                dx = base + lead;
                if (sgn(dx) != sgn(base)) dx = 0; /* it is coming to the centre by itself: hold */
            }
            dx = dx * c->gain_pct / 100;
            if (modern && t->last_sign && sgn(dx) == -t->last_sign && now_ms - t->last_nudge_ms < REVERSE_MS) dx /= 2;
            dp = clampi(dx, -c->max_step, c->max_step);
            if (modern && (int64_t)(pick->x2 - pick->x1) * 100 > (int64_t)NEAR_W_PCT * fw) dp = clampi(dp, -NEAR_MAX_PAN, NEAR_MAX_PAN); /* near: small steps only */
            if (iabs64(dp) < minv) dp = 0;
            if (dp && beyond(d2x, fw, c->fast_pct)) fast = 1;
        }
        if (c->tilt_enable && !(hold_tilt && t->dbg.rule == 4) && (zn ? tilt_out : (tclip || !in_band(d2y, fh, hb || t->dbg.rule == 3 ? hband : c->tband_pct)))) {
            int s;
            if (tclip) { /* head out of the frame: up by a step growing with the box height (a taller box = nearer = more is cut), no lead */
                s = -clampi((int)((int64_t)c->gain_tilt * ((pick->y2 - pick->y1) * 100 < HEAD_CUT_PCT * fh ? HEAD_CUT_PCT * fh / 100 : pick->y2 - pick->y1) / fh / 5), /* a far (short) box clipped at the top: at least as much as a 40 % one */ minv > MIN_STEP ? minv : MIN_STEP, c->max_step);
            } else {
                s = clampi((zn ? sy_img : offset_steps(d2y, fh, c->gain_tilt)) * c->gain_pct / 100, -c->max_step, c->max_step);
                if (hold_tilt && s > 0) s = 0; /* the approach hold: no DOWN tilt */
                if (beyond(d2y, fh, c->fast_pct)) fast = 1;
            }
            if (modern && t->last_tsign && sgn(s) == -t->last_tsign && now_ms - t->last_tnudge_ms < REVERSE_MS && iabs64(s) >= 2 * minv) s /= 2; /* never below the minimum step: that would swallow a small correction */
            if (iabs64(s) >= minv) dt = c->invert_tilt ? -s : s;
            else if (!dp) fast = 0;
        }
    }
    /* soft range: clip the resulting motor position, so a nudge never leaves [min,max]; image "below" (dt > 0) = tilt DOWN = motor -dt */
    p = clampi(ptz->pan + dp, ptz->pan_min, ptz->pan_max) - ptz->pan;
    q = clampi(ptz->tilt - dt, ptz->tilt_min, ptz->tilt_max) - ptz->tilt;
    if (iabs64(p) < minv) p = 0; /* after the clip: a nudge of a few steps at a limit would loop (a motion, scene timers, MQTT) */
    if (iabs64(q) < minv) q = 0;
    if (modern && q && t->tilt_ref_set && t->clip_n < CLIP_CONFIRM) { /* cumulative tilt UP cap (a far person near the top, a false positive: never up to the sky) */
        int up = c->invert_tilt ? -1 : 1, allowed = up_allowed(t, ptz, t->dbg.rule, pick, fh);
        if (up * q > 0 && up * q > allowed) {
            q = up * (allowed > 0 ? allowed : 0);
            if (iabs64(q) < minv) { q = 0; if (!p) t->dbg.why = "tilt-cap"; }
        }
    }
    t->dbg.dp = dp;
    t->dbg.dt = dt;
    if (p == 0 && q == 0) {
        if (!strcmp(t->dbg.why, "tilt-cap")) return;
        t->dbg.why = phold && !dt ? "pan-hold" : (dp || dt) ? "range" : allband ? "in-band" : "min-step";
        return;
    }
    if (modern && p && t->last_sign && sgn(p) == -t->last_sign) { /* a reversal of direction: capped (hunting) */
        int k, n2 = 0;
        for (k = 0; k < t->nrev; k++)
            if (now_ms - t->rev_ms[k] < REV_WINDOW_MS) t->rev_ms[n2++] = t->rev_ms[k];
        t->nrev = n2;
        if (t->nrev >= 2) { t->hold_until = now_ms + REV_HOLD_MS; t->nrev = 0; p = 0; t->dbg.why = "pan-reversal-cap"; }
        else t->rev_ms[t->nrev++] = now_ms;
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
    }
    if (p == 0 && q == 0) { if (!t->dbg.why || !strcmp(t->dbg.why, "stale")) t->dbg.why = "tilt-reversal-cap"; return; }
    t->dbg.why = "nudge";
    if (q) t->tilt_pending = 1;
    out->kind = TL_NUDGE;
    out->dpan = p;
    out->dtilt = -q;
    out->speed = fast ? c->speed_fast : 0;
    t->last_nudge_ms = now_ms;
    t->nudged = 1;
    t->settle_wait = 1;
    t->has_settled = 0;
    if (zn) {
        t->last_move_ms = now_ms;
        if (iabs64(p) >= c->gain_pan / 3 || iabs64(q) >= c->gain_tilt / 3) { t->pre_valid = 1; t->pre_pan = ptz->pan; t->pre_tilt = ptz->tilt; } /* the pose before a large move */
    }
    if (t->hn > 1) { t->hist[0] = t->hist[t->hn - 1]; t->hn = 1; } /* the older boxes belong to the pose before this nudge: only the last one is kept */
    if (p) t->last_sign = sgn(p);
}

void tl_set_motion(tl_t *t, const struct tl_mot *m) {
    if (m) t->mot = *m;
}

/* The public entry: person boxes (at most TL_MAX_BOXES) and head boxes (at most TL_MAX_HEADS). The steps policy ignores the heads (the dev / legacy "before" of the simulations). */
void tl_update(tl_t *t, int64_t now_ms, const struct tl_box *boxes, int n, int fw, int fh, const struct tl_ptz *ptz, struct tl_out *out) {
    struct tl_box pb[TL_MAX_BOXES], hb[TL_MAX_HEADS];
    int np = 0, nh = 0, i;
    for (i = 0; boxes && i < n; i++) {
        if (boxes[i].cls == TL_CLS_HEAD) { if (nh < TL_MAX_HEADS) hb[nh++] = boxes[i]; }
        else if (np < TL_MAX_BOXES) pb[np++] = boxes[i];
    }
    tl_update_p(t, now_ms, pb, np, hb, nh, fw, fh, ptz, out);
}
