/* Person-tracking policy of the Rotatable 2K own app: PURE logic (no I/O, no clock, no vendor call), host-tested
 * (tests/rot2k_track_unit.c). Fed once per detection tick (~5 Hz) with the person boxes, it returns at most one pan/tilt
 * nudge in motor steps, a "return home" request, or nothing. Model-agnostic: boxes are plain pixel corners like
 * struct a2k_det_box (x1,y1,x2,y2 in the detector frame, 320x192 on the 2K), the frame size is passed in.
 * Sign convention: dpan > 0 = target is right of centre, dtilt > 0 = target is below centre (image axes); the caller maps
 * them to the motor direction (cfg invert_pan/invert_tilt flip them here). */
#ifndef TRACK_LOGIC_H
#define TRACK_LOGIC_H
#include <stdint.h>

#define TL_MAX_BOXES 8

struct tl_box { int x1, y1, x2, y2; };

struct tl_cfg {
    int deadband_pct;        /* pan: no move while the box centre is within +-this % of the frame width from the centre (the "central band", 8) */
    int tband_pct;           /* tilt: same, % of the frame height (15) */
    int gain_pan;            /* steps per full frame width of offset (1000: ~4000 steps/350 deg, ~90 deg HFOV; see tracking.md) */
    int gain_tilt;           /* steps per full frame height of offset (600: ~50 deg VFOV) */
    int gain_pct;            /* damping: % of the measured offset corrected by one nudge (85: < 100 so a stale measure cannot overshoot) */
    int max_step;            /* clamp of one nudge, per axis (700) */
    int min_interval_ms;     /* rate limit between two nudges (300) */
    int confirm_ticks;       /* consecutive ticks with a target before the first move of a NEW target (2); a followed target needs 1 */
    int settle_ms;           /* >0: frames captured less than this after the motion ended (or before the last nudge was sent) are dropped (220);
                              * 0 = legacy: only the ticks while the motor runs are dropped */
    int fast_pct;            /* offset beyond this % of the frame uses speed_fast (25) */
    int speed_fast;          /* speed divisor of a large correction (2 = ~500 steps/s), 0 = the default speed */
    int lead_pct;            /* velocity lead: % of the predicted target travel added to a nudge, 0 = off (70) */
    int lead_max;            /* cap of the lead, steps (500) */
    int tilt_enable;         /* 1 = tilt tracking; default 0 (opt-in: track_tilt=1) until the tilt direction is verified on the camera */
    int lost_grace_s;        /* no person for this long = lost; nothing moves before (3) */
    int return_home;         /* 1 = go home after the loss (track_return=home), 0 = stay where the person was last seen (0) */
    int return_delay_s;      /* home mode: seconds after the end of the grace before going home, 0 = right after the grace (30) */
    int head_pct;            /* modern mode, tilt: the vertical reference is the TOP of the box (the head), kept this % of the frame height below the frame top (15);
                              * a box whose top touches the frame edge = head out = tilt up; a short box (far person) is centred on its upper third; 0 = centre of the box */
    int sticky_gate_pct;     /* the previous target is kept if a box lies within this % of the frame width of it (world position) (35) */
    int invert_pan, invert_tilt;
};

struct tl_ptz {
    int moving;              /* 1 while the motor runs: never nudge */
    int pan, tilt;           /* current position, motor steps */
    int pan_min, pan_max, tilt_min, tilt_max; /* soft range, inclusive */
    int64_t cap_ms;          /* capture time of the frame (same clock as now_ms), 0 = unknown (= now) */
    int64_t motion_end_ms;   /* when the last motion ended (same clock), 0 = unknown */
};

enum tl_kind { TL_NONE = 0, TL_NUDGE = 1, TL_HOME = 2 };

struct tl_out {
    enum tl_kind kind;
    int dpan, dtilt;         /* TL_NUDGE only, already clipped to the soft range, never both 0 */
    int speed;               /* TL_NUDGE: speed divisor (2..20), 0 = the default speed */
};

#define TL_NS 3              /* position samples kept for the velocity */
typedef struct {
    struct tl_cfg cfg;
    int has_target;          /* a target was seen in the previous tick */
    struct tl_box target;
    long target_w;           /* its world position along pan (motor steps) */
    int streak;              /* consecutive ticks with a target */
    int64_t last_nudge_ms;
    int last_sign;           /* sign of the last pan nudge */
    int nudged;              /* a nudge was sent since the last home / init */
    int64_t last_seen_ms;
    int home_pending;        /* home request not yet emitted for the current loss */
    int ns;                  /* velocity samples: capture time + world pan position */
    int last_tsign;          /* sign of the last tilt nudge (image sign) */
    int64_t last_tnudge_ms;
    int64_t thold_until;     /* the tilt reversal cap holds the tilt still until then */
    int64_t trev_ms[2];
    int ntrev;
    int64_t hold_until;      /* the reversal cap holds the tracker still until then */
    int64_t rev_ms[2];       /* times of the last direction reversals */
    int nrev;
    int64_t st[TL_NS];
    long sw[TL_NS];
} tl_t;

void tl_cfg_defaults(struct tl_cfg *c);
/* The slow policy of firmware 0.1.0 (12 % band, 1.5 s rate limit, fixed 300 step nudges, no lead, home after 10 s): the "before" of the simulation. */
void tl_cfg_legacy(struct tl_cfg *c);
/* 0 when valid, -1 otherwise. */
int tl_cfg_validate(const struct tl_cfg *c);
/* -1 on an invalid cfg (state zeroed, tl_update then does nothing), 0 otherwise. */
int tl_init(tl_t *t, const struct tl_cfg *c);
/* One tick. n boxes (invalid/empty boxes are ignored, n > TL_MAX_BOXES is truncated). Fills *out. */
void tl_update(tl_t *t, int64_t now_ms, const struct tl_box *boxes, int n, int fw, int fh, const struct tl_ptz *ptz,
               struct tl_out *out);
#endif
