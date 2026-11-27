/* Simple motion detection on a coarse grid (24 x 16 cells): PURE logic (no I/O, no clock, no vendor call), host-tested (tests/rot2k_motion_unit.c).
 * Input: a per-cell activity map, from the picture itself (frame differencing of the 320x192 luma plane of the detection channel, mg_feed_diff) or from the
 * vendor VPSS motion statistics (ak_vpss_md_get_stat, mg_feed_md). Output (struct mg_state): motion yes / no, the centroid and bounding box of the moving cells,
 * and a direction (left / right / up / down / none) from the velocity of the centroid over the last MG_HIST frames, with hysteresis.
 *
 * FRAME: everything here is in the PICTURE frame (what the user sees, what the detector and the tracker get). The diff source reads the detection channel, which
 * already is the picture (flip / mirror applied at the sensor readout). The vendor statistics may be in the SENSOR frame (mg_feed_md converts with the orientation
 * when told so). The direction constants are the TL_EXIT_* values of track_logic.h (down 1, up 2, left 3, right 4), so the tracker uses them as they are.
 *
 * MASKS: motion is ignored (state cleared, `masked` = 1) while the head moves and for a moment after (the whole picture shifts: mg_mask), during an orientation
 * change, after a day / night switch, a stream restart or a size change, and on a global change (more than MG_GLOBAL_PCT of the cells moving at once: a light, the
 * auto exposure, the IR cut): never a false motion from these. */
#ifndef AKAM_MOTION_GRID_H
#define AKAM_MOTION_GRID_H
#include <stdint.h>

#define MG_W 24
#define MG_H 16
#define MG_N (MG_W * MG_H)
#define MG_HIST 4            /* frames of centroid history for the velocity */
#define MG_GLOBAL_PCT 55     /* more cells than this % active at once = a global change, not an object */
#define MG_GLOBAL_MASK_MS 2000
#define MG_PTZ_AFTER_MS 700  /* the mask after the head stopped (the video latency, the settle) */
#define MG_DIR_HOLD_MS 800   /* the direction stays this long after the motion stopped (the tracker reads it at the loss) */
#define MG_MD_BYTES 1536     /* the vendor statistics block: 24 x 16 x 32 bit (layout assumed, see docs) */

enum { MG_DIR_NONE = 0, MG_DIR_DOWN, MG_DIR_UP, MG_DIR_LEFT, MG_DIR_RIGHT }; /* = TL_EXIT_* */
enum { MG_FRAME_PICTURE = 0, MG_FRAME_SENSOR = 1 };

struct mg_state {
    int valid;               /* a source feeds it and is usable */
    int masked;              /* the last frame was ignored by a mask */
    int active;              /* motion now */
    int cx, cy;              /* centroid of the active cells, permille of the width / height (500, 500 = the middle); valid when active */
    int x0, y0, x1, y1;      /* bounding box in cells (inclusive) */
    int cells;               /* number of active cells */
    int dir;                 /* the direction of the motion now (MG_DIR_*), held MG_DIR_HOLD_MS after it stopped */
    int64_t last_active_ms;  /* the last time `active` was 1 (0 = never) */
    int64_t dir_ms;          /* the last time `dir` was not none (0 = never) */
    int last_dir;            /* ... and that direction */
};

struct mg_cfg {
    int sensitivity;         /* 1 (least) .. 10 (most), default 5 */
    int min_cells;           /* fewer active cells = noise (default 2) */
};

typedef struct {
    struct mg_cfg cfg;
    struct mg_state st;
    int64_t mask_until;
    int hn;                  /* centroid history */
    int hx[MG_HIST], hy[MG_HIST];
    int dir_cur;
    /* frame differencing */
    uint16_t prev[MG_N];     /* mean luma per cell of the previous frame, x4 */
    int prev_ok;
    unsigned pw, ph;         /* the size of the previous frame */
    /* vendor statistics */
    uint32_t base[MG_N], dev[MG_N];
    int base_ok, md_zero_n, md_bad_n;
    int usable;
} mg_t;

void mg_defaults(struct mg_cfg *c);
void mg_init(mg_t *m, const struct mg_cfg *c); /* c NULL = defaults */
void mg_set_sensitivity(mg_t *m, int s);       /* clamped to 1..10 */
/* Ignore the motion until now_ms + ms (the head moves, the orientation changes, a day / night switch...); extends, never shortens. The history is dropped. */
void mg_mask(mg_t *m, int64_t now_ms, int ms);
/* The state forgotten and the sources restarted (a stream restart, a size change). */
void mg_reset(mg_t *m);
/* A cell activity map (0..255, > threshold = moving) in the PICTURE frame. */
void mg_feed_cells(mg_t *m, int64_t now_ms, const uint8_t act[MG_N]);
/* Frame differencing: the luma plane w x h (stride) of the detection channel. A different size than the last frame restarts it. */
void mg_feed_diff(mg_t *m, int64_t now_ms, const uint8_t *y, unsigned w, unsigned h, unsigned stride);
/* The vendor statistics: n bytes (MG_MD_BYTES), rc the return of ak_vpss_md_get_stat. frame = MG_FRAME_SENSOR: the grid is in the sensor frame, converted with flip / mirror. */
void mg_feed_md(mg_t *m, int64_t now_ms, int rc, const void *buf, unsigned n, int frame, int flip, int mirror);
/* 1 while the vendor statistics look usable (answers, not all zero for ever, not noise only). */
int mg_md_usable(const mg_t *m);
/* A sensor-frame grid into the picture frame (flip: rows reversed, mirror: columns reversed; an involution). */
void mg_grid_orient(int flip, int mirror, const uint8_t *in, uint8_t *out);
const struct mg_state *mg_get(const mg_t *m);
const char *mg_dir_name(int dir); /* none | left | right | up | down */

#endif
