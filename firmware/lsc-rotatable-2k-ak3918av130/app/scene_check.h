/* Scene check (portable, PURE: no I/O of its own, no hardware, no model name; host-tested by tests/rot2k_scene_unit.c).
 *
 * Question answered at a cold boot: "did the camera head provably NOT move while it was off?". If so the saved position can be declared to
 * the motor driver and the long calibration (about ten grinding clicks against the stops) is skipped. The evidence is a small grayscale
 * thumbnail (SC_W x SC_H, from the 320x192 detection channel, box filtered) saved with the position at a clean stop, and compared at boot
 * once the auto exposure has settled.
 *
 * Comparison: zero-mean normalised cross-correlation (ZNCC) on the central area, best of a +-SC_SHIFT px search (a thumbnail pixel is 4 source
 * pixels: a few source pixels of mechanical play or vibration are tolerated). The score is in permille (1000 = identical). ZNCC ignores a
 * global gain and offset: a brighter / darker room (lamp, sun, AE settling) still matches. LIMITS (honest): it cannot prove anything on a
 * scene without texture (uniform wall, lens covered, dark night view): both images must pass a minimum-detail check (standard deviation and
 * mean gradient) or the verdict is "low detail" -> calibrate. A very repetitive pattern (blinds, tiles) can match after a shift of a whole
 * period; a different day/night mode (IR cut, colour) is refused outright; furniture or people that moved lower the score and may force a
 * calibration (safe: the fallback is always the full calibration). A pattern fixed to the camera (dirt, overlay) is caught by the per-block rule. A camera rotated by more than about 10 thumbnail px (about 40 source
 * px, a few degrees) or another scene gives a low score. A non-linear lighting change (a lamp that lights only part of the scene) lowers
 * the score too. */
#ifndef SCENE_CHECK_H
#define SCENE_CHECK_H

#include <stddef.h>
#include <stdint.h>

#define SC_W 80
#define SC_H 48
#define SC_N (SC_W * SC_H)
#define SC_SHIFT 2
#define SC_MIN_DEFAULT 900   /* permille: strict match threshold (config ptz_scene_min = 0.90) */
#define SC_MIN_LOWEST 800    /* ptz_scene_min below this is refused */
#define SC_BX 4              /* the block grid of the per-block decision */
#define SC_BY 3
#define SC_BLOCKS_MIN 8      /* of SC_BX * SC_BY blocks: each must have detail in BOTH images AND match on its own */
#define SC_BLOCK_MARGIN 100  /* a block may score this much below the global threshold */
#define SC_FULL_EVERY 10     /* a saved reference is trusted for this many matched boots, then one full calibration */
#define SC_MIN_SD 8          /* minimum standard deviation of the gray levels (0..255) */
#define SC_MIN_GRAD_X10 15   /* minimum mean |dx| + |dy| of neighbouring thumbnail pixels, tenths of a gray level */
#define SC_FILE_MAX 4096     /* serialized reference: header + SC_N bytes < 4 KB (fits the settings store) */

typedef struct { uint8_t px[SC_N]; } sc_thumb_t;

typedef struct {
    int valid;               /* 1: a reference was loaded */
    int mode;                /* day / night at capture (0 day, 1 night) */
    uint32_t pan, tilt;      /* the position at capture */
    unsigned boots;          /* matched boots since the last full calibration */
    sc_thumb_t th;
} sc_ref_t;

enum { SC_MATCH = 0, SC_NO_REF, SC_LOW_DETAIL_REF, SC_LOW_DETAIL_CUR, SC_MODE_DIFF, SC_MISMATCH, SC_NO_FRAME, SC_CANCELLED, SC_MISMATCH_BLOCKS, SC_PERIODIC, SC_STORE_FAILED };
const char *sc_verdict_name(int v);

/* Box-filters the luma plane (w x h, line stride `stride`; w a multiple of SC_W, h a multiple of SC_H) to a thumbnail. 0, or -1. */
int sc_downscale_y(const uint8_t *y, unsigned w, unsigned h, unsigned stride, sc_thumb_t *out);
/* 1 when the thumbnail has enough texture to prove anything. */
int sc_has_detail(const sc_thumb_t *t);
/* Best ZNCC of cur against ref over the shift search, in permille (can be negative); -1000 when an area is flat. */
int sc_score(const sc_thumb_t *ref, const sc_thumb_t *cur);
/* How many of the SC_BX x SC_BY blocks have detail in both images and match on their own (best ZNCC of the block over the shift search >= min - margin). */
int sc_blocks_ok(const sc_thumb_t *ref, const sc_thumb_t *cur, int min_permille);
/* The verdict for a loaded reference: SC_MATCH only when valid, both detailed, same mode, score >= min_permille AND at least SC_BLOCKS_MIN blocks ok. *score gets the score (or -1000). */
int sc_decide(const sc_ref_t *ref, const sc_thumb_t *cur, int cur_mode, int min_permille, int *score);

/* The plain decimal "0.80".."0.999" (or "1" / "1.0") -> permille; -1 on anything else (below SC_MIN_LOWEST = 0.80 is refused). */
int sc_parse_min(const char *s);

/* Serialized form: "scene1 <mode> <pan> <tilt> <w> <h> <boots>\n" + SC_N raw bytes. The invalid marker is sc_invalid_marker.
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
} sc_env_t;

typedef struct {
    int min_permille;
    unsigned settle_ms;      /* AE settling: no capture before this long after the first frame (a mode change restarts it) */
    unsigned settle_frames;  /* ... and this many distinct frames */
    unsigned timeout_ms;     /* give up (verdict SC_NO_FRAME) */
} sc_cfg_t;

/* No reference, or one already trusted for SC_FULL_EVERY matched boots (SC_PERIODIC) -> answered at once (no waiting). Otherwise waits for the AE to settle, takes the thumbnail, decides. On SC_MATCH the
 * saved position is returned in *pan / *tilt. Every verdict is logged with the score. Anything but SC_MATCH means: calibrate. */
int sc_boot_check(const sc_env_t *e, const sc_cfg_t *c, uint32_t *pan, uint32_t *tilt);

#endif
