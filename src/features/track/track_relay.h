/* Detector-miss relay of the person tracker: PURE logic (no I/O, no clock, no vendor call), host-tested (tests/rot2k_track_relay_unit.c).
 * When the detector misses a close person (flapping at 0.5-1 m: 20-60 % of the inferences), the last target region is followed from frame to frame on the grayscale
 * detector frame (320x192 luma) by zero-mean normalised cross-correlation (ZNCC) of a template cut from the PREVIOUS frame, searched in a small window of the current one.
 * Confidence gated: the relay stops (until the detector sees the person again) when the best ZNCC is below TR_MIN_ZNCC_PCT, when the template is flat (no texture to
 * lock on), or after TR_MAX_MS without a detector box. The detector box always wins: a frame with a box only refreshes the region. Working resolution: the frame is box
 * filtered by 2 (160x96) and the template is at most 24x24 samples, so a step is about 440 positions x 576 samples (see the cost line of the unit test). */
#ifndef TRACK_RELAY_H
#define TRACK_RELAY_H
#include <stdint.h>

#define TR_DS 2              /* box filter factor of the working image */
#define TR_MAX_W 320         /* largest frame handled */
#define TR_MAX_H 192
#define TR_MAX_MS 3500       /* the relay never runs longer than this after the last detector box */
#define TR_MIN_ZNCC_PCT 60   /* the best match must correlate at least this % (0..100) */
#define TR_MIN_STD 6         /* a template with a grey-level standard deviation below this is flat: nothing to lock on */
#define TR_SEARCH 10         /* search window +-this many working pixels (+-20 px of the frame) per step */

typedef struct {
    uint8_t prev[(TR_MAX_W / TR_DS) * (TR_MAX_H / TR_DS)]; /* the previous frame, working resolution */
    int have_prev;
    int w, h;            /* working image size */
    int box[4];          /* the followed region in frame pixels: x1 y1 x2 y2 (may slide partly out of the frame) */
    int box_set;
    int64_t det_ms;      /* time of the last detector box */
    int dead;            /* the relay gave up (until the next detector box) */
    int last_zncc;       /* of the last relayed step, percent */
} tr_t;

void tr_init(tr_t *t);
/* One frame (luma plane y, frame fw x fh, fw <= 320, fh <= 192, stride). det = the detector's person box of this frame (x1,y1,x2,y2) or NULL when it saw nobody. Returns 1 with
 * the relayed region (clipped to the frame) in out[4] when the detector has no box and the relay holds; 0 otherwise (a detector box present, not started, gave up, out of time). */
int tr_step(tr_t *t, const uint8_t *y, unsigned fw, unsigned fh, unsigned stride, const int det[4], int64_t now_ms, int out[4]);
#endif
