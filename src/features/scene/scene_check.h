/* Scene check (portable, PURE: no I/O of its own, no hardware, no model name; host-tested by tests/rot2k_scene_unit.c).
 *
 * Question answered at a cold boot: "did the camera head provably NOT move while it was off?". If so the saved position can be declared to
 * the motor driver and the long calibration (about ten grinding clicks against the stops) is skipped. The evidence is a small grayscale
 * thumbnail (SC_W x SC_H, from the 320x192 detection channel, box filtered) saved with the position (HOME) after a calibration, and compared at boot
 * once the auto exposure has settled.
 *
 * Comparison (contrast normalised, works on a plastered white wall): both thumbnails are high-passed (Laplacian: gradients, vignetting, a
 * global gain / offset vanish; edges, plaster relief, texture stay). The sensor noise is estimated from the image itself (the quietest
 * sub-blocks), and each block of the 4x3 grid is "detailed" only when its high-pass energy is clearly above that noise (SC_DETAIL_RATIO).
 * The displacement is the peak of the surface summed over the blocks detailed in both (within SC_TOL, and SC_PEAK_MARGIN above every larger
 * displacement). Each block is then classified at that common displacement by DISPLACEMENT EVIDENCE, not by its raw score (a lamp, dusk, the IR
 * light or a window change the scores, not where the correlation peaks): SAME = the block peaks there (within SC_SAME_SLACK of its best
 * displacement) and scores >= min - SC_SAME_BELOW; MOVED = a shifted peak clearly beats it (votes against); a strong block with no peak anywhere is
 * another scene (against). Neutral (no vote): a weak block (high-pass energy < SC_WEAK_RATIO x the noise: plaster relief) that is not SAME, a
 * lighting-lowered strong block still peaking at the common displacement, and up to SC_JUMP_MAX blocks whose signal energy changed by more than
 * SC_JUMP_RATIO without correlating (a window); the latter two weigh half a vote against. The weak blocks are pooled (mean score >= SC_WEAK_POOL).
 * MATCH needs a peak_ok displacement, at least SC_SAME_BLOCKS_MIN SAME blocks that are >= 1/3 of the blocks detailed in common and >= 2/3 of the
 * voting blocks (SAME + MOVED + mismatches + blocks detailed on one side only and clearly flat on the other), and a mean SAME score >= min -
 * SC_MEAN_SLACK. Anything else is refused (fail closed: the full calibration).
 * Anti fixed-pattern: lens dirt, an overlay or an IR hot spot covers a few blocks while the rest of the scene votes it down; a smooth
 * one (vignetting, hot spot) is removed by the high-pass and yields no detailed block. A truly featureless view (lens covered, blank wall with
 * no relief above the noise, dark night) cannot be decided: "view too uniform" -> calibrate, or the multi-view fallback below.
 * LIMITS (honest): a very repetitive pattern (blinds, tiles) can match after a shift of a whole period; a different day/night mode (IR cut,
 * colour) is refused outright; furniture or people that moved up to a third of the detailed blocks are tolerated; a camera rotated by 2 or
 * more thumbnail px (>= about 4 degrees) or another scene is refused.
 *
 * Multi-view fallback (the saved view is too uniform, < SC_DETAIL_BLOCKS_MIN detailed blocks): at reference time the head also stores
 * the views at up to SC_OFFS offsets from the saved position (sc_offset: pan +D, pan -D, tilt -D, kept inside the soft range); at boot an
 * undecidable reference answers SC_NEED_PROBE: the motor layer declares the claimed position, moves by one offset with the safe-motion layer only,
 * and sc_probe_check compares the live view with the stored view of that offset. Any failure -> full calibration. */
#ifndef SCENE_CHECK_H
#define SCENE_CHECK_H

#include <stddef.h>
#include <stdint.h>

#define SC_W 80
#define SC_H 48
#define SC_N (SC_W * SC_H)
#define SC_SHIFT 4           /* displacement search, thumbnail px */
#define SC_TOL 2             /* the head did "not move" when the best correlation peak lies within this many px (about 4 source px) */
#define SC_PEAK_MARGIN 20    /* permille: the peak inside the tolerance must beat the best score outside it by this much */
#define SC_REP_RANGE 40      /* repetitive-texture check: displacements searched (step 1), thumbnail px */
#define SC_REP_LOBE_MAX 8    /* ... the main lobe of the self-correlation is no alias only up to this displacement (px); wider is flagged */
#define SC_REP_BRIDGE 3      /* ... passing displacements up to SC_TOL + this from the origin seed the main lobe (sampling noise leaves a gap at the edge of a broad lobe); a period still repeats at 2p, outside */
#define SC_REP_MARGIN 50     /* ... the match rule applied to the image against itself, with this much margin below the match threshold (permille) */
#define SC_DETAIL_RATIO 3    /* a block is detailed when its high-pass energy is >= this times the image noise energy */
#define SC_DETAIL_BLOCKS_MIN 4 /* of SC_BX * SC_BY: detailed blocks needed in the reference, in the current view and in common */
#define SC_MIN_DEFAULT 850   /* permille: strict match threshold (config ptz_scene_min / API ptz.scene_min = 0.85; was 0.90 until 2026-11-26) */
#define SC_MIN_LOWEST 800    /* ptz_scene_min below this is refused */
#define SC_BX 4              /* the block grid of the per-block decision */
#define SC_BY 3
#define SC_BLOCK_MARGIN 100  /* (kept for the callers) */
/* Displacement-evidence vote (illumination robust, see the header comment): all relative to the configured threshold, in permille. */
#define SC_WEAK_RATIO 12     /* a block whose high-pass energy is below this times the noise (in either image) is weak: noise dominates its score, it is neutral */
#define SC_JUMP_RATIO 2      /* a block whose signal energy changed by more than this factor between the images (a window, a screen) is neutral */
#define SC_SAME_BELOW 100    /* a block is SAME (the head did not move) when its zero-shift score >= min - this, and zero shift is its peak */
#define SC_SAME_SLACK 120    /* ... zero within this of the best score at any searched displacement */
#define SC_WEAK_SAME_BELOW 100 /* a weak block (noise-compensated correlation of a barely detailed block is unstable) is SAME only at min - this */
#define SC_MOVE_BELOW 200    /* a block is MOVED (votes against) when a shifted peak >= min - this beats zero shift by SC_MOVE_MARGIN */
#define SC_MOVE_MARGIN 120
#define SC_SOFT_BELOW 250    /* a strong block whose peak stays at zero shift with a score >= min - this is lighting-lowered: neutral, not a mismatch */
#define SC_JUMP_MAX 2        /* at most this many blocks may be neutral for a drastic change of their content (more: another scene, they count against) */
#define SC_WEAK_POOL 250     /* the weak blocks are neutral one by one, but their mean score at the common displacement must be >= this (permille) */
#define SC_SAME_BLOCKS_MIN 4 /* SAME blocks needed (and at least a third of the blocks detailed in both) */
#define SC_MEAN_SLACK 50     /* the mean zero-shift score of the SAME blocks must be >= min - this */
#define SC_FULL_EVERY 10     /* a saved reference is trusted for this many matched boots, then one full calibration */
#define SC_FILE_MAX 4096     /* serialized reference: header + SC_N bytes < 4 KB (fits the settings store) */

typedef struct { uint8_t px[SC_N]; } sc_thumb_t;

typedef struct {
    int valid;               /* 1: a reference was loaded */
    int mode;                /* day / night at capture (0 day, 1 night) */
    uint32_t pan, tilt;      /* the position at capture */
    unsigned boots;          /* matched boots since the last full calibration */
    uint32_t stamp;          /* seconds since the epoch when the reference was written (0 = unknown): the rewrite rate limit lives in the file itself */
    int rep;                 /* the repetitive-texture verdict, screened when the reference was captured: 1 yes, 0 no; -1 unknown (older file) */
    int repmin;              /* the ptz_scene_min (permille) the screening ran at (0 = unknown): valid for any boot threshold >= it */
    sc_thumb_t th;
} sc_ref_t;

enum { SC_MATCH = 0, SC_NO_REF, SC_LOW_DETAIL_REF, SC_LOW_DETAIL_CUR, SC_MODE_DIFF, SC_MISMATCH, SC_NO_FRAME, SC_CANCELLED, SC_MISMATCH_BLOCKS, SC_PERIODIC, SC_STORE_FAILED, SC_NEED_PROBE, SC_NOT_HOME, SC_REPETITIVE, SC_LIKELY };
/* SC_LIKELY (sc_decide, not a match): zero shift, no MOVED block, at least SC_SAME_BLOCKS_MIN blocks matching on their own with a mean score >= the threshold, but too few of the others to prove it: "very likely home".
 * Never a proof: the motor layer may then run the SHORT calibration (sweep a little past home, go home) and ask for the proof again before trusting it. */
const char *sc_verdict_name(int v);

/* Box-filters the luma plane (w x h, line stride `stride`; w a multiple of SC_W, h a multiple of SC_H) to a thumbnail. 0, or -1. */
int sc_downscale_y(const uint8_t *y, unsigned w, unsigned h, unsigned stride, sc_thumb_t *out);
/* 1 when the thumbnail has enough texture above its own noise to prove anything (>= SC_DETAIL_BLOCKS_MIN detailed blocks). */
int sc_has_detail(const sc_thumb_t *t);
typedef struct {
    int ref_detail, cur_detail; /* detailed blocks of each image */
    int both;                   /* detailed in both: the blocks that vote */
    int vote_n;                 /* both + the blocks detailed on one side only and clearly flat on the other (they count against) */
    int matched;                /* of those, SAME (zero shift is the block's own peak, score above the floor): positive evidence */
    int weak_n, weak_mean;      /* the weak blocks (not changed ones) and their mean score at the common displacement (1000 when none) */
    unsigned same_mask;         /* bit per block: SAME */
    int doubt;                  /* neutral blocks that are not weak (changed energy, lighting-lowered): half a vote against */
    int moved, neutral;         /* blocks voting a displacement (against), blocks left out (weak / changed drastically / lighting-lowered) */
    int shift_x, shift_y;       /* the best displacement (summed over the voting blocks), thumbnail px */
    int peak_ok;                /* it lies within SC_TOL and clearly beats every larger displacement */
    int score;                  /* permille: mean block score of the matching blocks (of all voting blocks when none matches); -1000 with no voting block */
} sc_cmp_t;
int sc_compare(const sc_thumb_t *ref, const sc_thumb_t *cur, int min_permille, sc_cmp_t *out); /* 0 */
/* ---- Drift check (the head is idle at HOME and the counters may have lost steps: the head is physically somewhere near home). The same comparison as the boot proof, with the
 * displacement search widened to SC_DRIFT_R px and the common displacement allowed anywhere in it. The evidence rules are the SAME strict ones (a unique peak, at least
 * SC_SAME_BLOCKS_MIN SAME blocks, 2/3 of the voting blocks, a mean score, the weak pool), plus: the reference must be a day/night mode like the live view, detailed and screened as NOT
 * repetitive (a shift of a whole period would match). Verdicts:
 *   SC_DRIFT_OK     strict proof at zero displacement: the counters are right;
 *   SC_DRIFT_SHIFT  strict proof at a displacement (dx, dy) of at most SC_DRIFT_MAX_PX per axis: the content of the live view is the reference content moved by (dx, dy) px (right / down
 *                   positive), i.e. the head is physically elsewhere than home by that much (see ptz.c for the conversion to steps);
 *   SC_DRIFT_RECAL  no strict proof anywhere, or a proof beyond SC_DRIFT_MAX_PX: the counters cannot be corrected, only a calibration can fix them;
 *   SC_DRIFT_INCONC zero displacement, at least SC_SAME_BLOCKS_MIN blocks peaking there (and at most half as many blocks voting a displacement) but too few for the strict vote (a person, the light):
 *                   the counters are probably right, nothing is proven, try again later;
 *   SC_DRIFT_SKIP   nothing can be judged: no reference, another day/night mode, a featureless / repetitive / unscreened reference, a featureless live view. */
#define SC_DRIFT_R 8
#define SC_DRIFT_MAX_PX 6
enum { SC_DRIFT_OK = 0, SC_DRIFT_SHIFT, SC_DRIFT_RECAL, SC_DRIFT_INCONC, SC_DRIFT_SKIP };
typedef struct {
    int verdict;
    int dx, dy;                 /* the displacement of the best peak, thumbnail px (valid for OK / SHIFT, informative otherwise) */
    int score;                  /* mean SAME score, permille */
    int matched, vote_n, both, moved;
    const char *why;            /* a short reason (static string) */
} sc_drift_t;
int sc_drift(const sc_ref_t *ref, const sc_thumb_t *cur, int cur_mode, int min_permille, sc_drift_t *out);
const char *sc_drift_name(int v);

/* 1 when the thumbnail is a repetitive texture (blinds, tiles, bars): the image against itself, displaced beyond SC_TOL (up to +-SC_REP_RANGE px), would
 * pass the match rule of sc_compare at the configured threshold (less SC_REP_MARGIN). Such a view can match after a shift of a whole period: it is never
 * saved as a reference. The verdict is computed ONCE, when the reference is captured, and stored in it (sc_ref_t rep / repmin, header scene2): the boot only
 * reads it (a reference of the older format, or one screened at a stricter threshold than the configured one, is screened at the boot, once: the store of
 * the first matched boot writes the verdict). The broad main lobe (and a pure invariance along one axis, vertical edges) is not flagged: the match refuses
 * it by itself. Integer arithmetic; a coarse pass on a quarter of the pixels with a much lower floor and an early exit select the displacements that are
 * evaluated in full (about 7x cheaper than evaluating all). Never call it under a lock. */
int sc_repetitive(const sc_thumb_t *t, int min_permille);
/* The same, and WHY: the first displacement (dx, dy, thumbnail px) that triggered it and kind 1 (a separate lobe: a period) or 2 (the main lobe reaches beyond SC_REP_LOBE_MAX: invariance along an axis). */
int sc_repetitive_why(const sc_thumb_t *t, int min_permille, int *dx, int *dy, int *kind);
/* One text line for a diagnosis dump: per image noise / detailed / strong block masks, and (cur != NULL) the comparison (counts, SAME mask, shift, score), and (with_rep) the repetitive screening with its reason. */
void sc_diag_text(const sc_thumb_t *ref, const sc_thumb_t *cur, int min_permille, const char *what, int with_rep, char *out, size_t cap);
/* Mean block score (permille) as in sc_cmp_t; -1000 when no block has detail in both. */
int sc_score(const sc_thumb_t *ref, const sc_thumb_t *cur);
/* How many blocks have detail in both images and match on their own. */
int sc_blocks_ok(const sc_thumb_t *ref, const sc_thumb_t *cur, int min_permille);
/* The verdict for a loaded reference: SC_MATCH only when valid, enough detail, same mode, >= 2/3 of the voting blocks matching and score >= min_permille. *score gets the score (or -1000). */
int sc_decide(const sc_ref_t *ref, const sc_thumb_t *cur, int cur_mode, int min_permille, int *score);

/* The plain decimal "0.80".."0.999" (or "1" / "1.0") -> permille; -1 on anything else (below SC_MIN_LOWEST = 0.80 is refused). */
int sc_parse_min(const char *s);

/* Serialized form: "scene2 <mode> <pan> <tilt> <w> <h> <boots> <stamp> <rep> <repmin>\n" + SC_N raw bytes (scene1, without stamp / rep / repmin, is still read: rep unknown). The invalid marker is sc_invalid_marker.
 * sc_pack: bytes written (<= cap), or 0 when cap is too small. sc_unpack: 0 ok (ref->valid = 1), -1 anything else (including the marker). */
size_t sc_pack(const sc_ref_t *ref, char *out, size_t cap);
int sc_unpack(const char *buf, size_t len, sc_ref_t *ref);
extern const char sc_invalid_marker[];

/* The boot decision, with its environment injected (the app wires the saved file, the detection thread's thumbnails, the day/night state). */
typedef struct {
    int (*load)(sc_ref_t *ref);                 /* 0 loaded, -1 absent / invalid */
    int (*store)(const sc_ref_t *ref);          /* rewrites the reference (boots counter bumped after a match): 0, or -1 (then no match) */
    int (*frame)(sc_thumb_t *t, unsigned *seq); /* newest thumbnail: 0, or -1 (none / stale); *seq changes with every new frame */
    int (*mode)(void);                          /* 0 day, 1 night, -1 unknown */
    int64_t (*now_ms)(void);
    void (*sleep_ms)(unsigned ms);
    int (*cancelled)(void);                     /* may be NULL */
    void (*log)(const char *line);              /* may be NULL */
    int (*load_alt)(unsigned i, sc_ref_t *ref); /* the stored view of offset i (multi-view fallback): 0 loaded, -1 absent / invalid; may be NULL (none) */
    int (*store_alt)(unsigned i, const sc_ref_t *ref); /* writes a stored offset view back with the repetitive verdict screened at the boot (an older file): may be NULL */
    void (*dump)(const char *name, const sc_thumb_t *t, const char *text); /* diagnosis aid (RAM only, never the flash): a thumbnail the check used and a text line; may be NULL */
} sc_env_t;

typedef struct {
    int min_permille;
    unsigned settle_ms;      /* AE settling: no capture before this long after the first frame (a mode change restarts it) */
    unsigned settle_frames;  /* ... and this many distinct frames */
    unsigned timeout_ms;     /* give up (verdict SC_NO_FRAME) */
    unsigned probe_ms;       /* multi-view probe: no capture before this long after the head arrived at an offset (0 = SC_PROBE_MS_DEFAULT) */
} sc_cfg_t;

/* *pan / *tilt come IN as the home position: the reference is keyed to home and proves only "the head is at home" (SC_NOT_HOME when it was taken elsewhere, SC_REPETITIVE
 * when the stored view is a repetitive texture). No reference, or one already trusted for SC_FULL_EVERY matched boots (SC_PERIODIC) -> answered at once (no waiting). Otherwise waits for the AE to settle, takes the thumbnail, decides. On SC_MATCH the
 * reference position (= home) is returned in *pan / *tilt. Every verdict is logged with the score. Anything but SC_MATCH means: calibrate. */
int sc_boot_check(const sc_env_t *e, const sc_cfg_t *c, uint32_t *pan, uint32_t *tilt);

/* Multi-view fallback. The offsets from the saved position where a second view is stored when the rest view is featureless, in steps:
 *   0: pan +SC_OFF_PAN   1: pan -SC_OFF_PAN   2: tilt -SC_OFF_TILT (never up: the top tilt stop is the closest one).
 * RISK ANALYSIS (ptz-findings.md "Scene check: low-texture views"): the position is only CLAIMED at boot (the head may have been turned by hand
 * anywhere). The offset move is a relative move of at most SC_OFF_PAN / SC_OFF_TILT steps by the safe-motion layer, whose driver limits are the
 * soft range, so in the worst case the head ends SC_OFF_* steps beyond where it really was. Soft range pan 300..3650 vs stops at 0 / >= 3932:
 * 150 steps leave >= 132 (right) and 150 (left) steps to the stops; tilt 300..2100 vs stops at 0 / 2290..2340: the move is DOWN only, 100 steps
 * leave 200 steps to the bottom stop (and tilt never goes up, where the margin would be only 190 steps). Even a head turned to a mechanical
 * stop only ever clicks against it (the calibration does that on purpose, about ten times). */
#define SC_OFFS 3
#define SC_OFF_PAN 150
#define SC_OFF_TILT 100
#define SC_PROBE_MS_DEFAULT 1500
/* 0 and the offset in steps, -1 when i >= SC_OFFS. */
int sc_offset(unsigned i, int *dpan, int *dtilt);
/* After a SC_NEED_PROBE answer of sc_boot_check: 0 when the stored view of offset i is usable (same mode, same position + offset as the reference). */
int sc_probe_offset(unsigned i, int *dpan, int *dtilt);
/* The head is now at the offset i: waits for fresh frames, compares the live view with the stored view of that offset (all the rules of sc_decide).
 * SC_MATCH: the reference position (the claimed one) is proven, the matched-boots counter is bumped and stored, *pan / *tilt = the reference position.
 * Anything else: calibrate. Logged. */
int sc_probe_check(const sc_env_t *e, const sc_cfg_t *c, unsigned i, uint32_t *pan, uint32_t *tilt);

#endif
