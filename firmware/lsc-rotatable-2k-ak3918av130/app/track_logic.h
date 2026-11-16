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
    int deadband_pct;        /* no move while |offset| <= this % of the frame width / height (12) */
    int gain_pan;            /* steps per full frame width of offset (1000: ~4000 steps/350 deg, ~90 deg HFOV) */
    int gain_tilt;           /* steps per full frame height of offset (600: ~50 deg VFOV) */
    int max_step;            /* clamp of one nudge, per axis (300) */
    int min_interval_ms;     /* between two nudges (1500) */
    int confirm_ticks;       /* consecutive ticks with a target before the first move (2) */
    int lost_s;              /* no person for this long -> home request (10) */
    int home_enable;         /* 1 = emit the home request (1) */
    int sticky_iou_pct;      /* min overlap (IoU %) to keep the previous target (10) */
    int invert_pan, invert_tilt;
};

struct tl_ptz {
    int moving;              /* 1 while the motor runs: never nudge */
    int pan, tilt;           /* current position, motor steps */
    int pan_min, pan_max, tilt_min, tilt_max; /* soft range, inclusive */
};

enum tl_kind { TL_NONE = 0, TL_NUDGE = 1, TL_HOME = 2 };

struct tl_out {
    enum tl_kind kind;
    int dpan, dtilt;         /* TL_NUDGE only, already clipped to the soft range, never both 0 */
};

typedef struct {
    struct tl_cfg cfg;
    int has_target;          /* a target was seen in the previous tick */
    struct tl_box target;
    int streak;              /* consecutive ticks with a target */
    int64_t last_nudge_ms;
    int nudged;              /* a nudge was sent since the last home / init */
    int64_t last_seen_ms;
    int home_pending;        /* home request not yet emitted for the current loss */
} tl_t;

void tl_cfg_defaults(struct tl_cfg *c);
/* 0 when valid, -1 otherwise. */
int tl_cfg_validate(const struct tl_cfg *c);
/* -1 on an invalid cfg (state zeroed, tl_update then does nothing), 0 otherwise. */
int tl_init(tl_t *t, const struct tl_cfg *c);
/* One tick. n boxes (invalid/empty boxes are ignored, n > TL_MAX_BOXES is truncated). Fills *out. */
void tl_update(tl_t *t, int64_t now_ms, const struct tl_box *boxes, int n, int fw, int fh, const struct tl_ptz *ptz,
               struct tl_out *out);
#endif
