/* Drift matcher (portable, PURE: no I/O, no clock, no hardware, no libm; host-tested by tests/rot2k_drift_unit.c).
 *
 * Question: "by how many pixels is the live view displaced from the view taken at HOME?" Works with ONE reference by day and by night: the
 * features compared are the ORIENTATIONS of strong edges (polarity-invariant doubled-angle gradient vectors, magnitude saturated), not grey levels,
 * so a window that is bright by day and dark under IR, gain, gamma, colour vs grayscale do not matter; only the geometry (frames, sills, doors) does.
 *
 *   reference: DM_NP (16) patches of 16x16 (at half resolution, "L1") picked on strong 2-D edge structure, spread over a 4x3 grid (at most 3 per cell);
 *              patches whose own correlation stays high when shifted by 3 px (a bare straight edge, a repetitive texture) are refused.
 *   match:     every patch is searched over +-DM_RX x +-DM_RY L1 px with a uniqueness test, a consensus vote of the patches that agree, a parabolic
 *              sub-pixel peak; the answer is the median of the inliers, in 1/4 px of the full frame.
 *   refusal:   too few valid patches (flat frame, a person covering them, another scene) or no consensus -> DM_INCONC; never a guess.
 * Frame: 8-bit luma, w and h multiples of 4, at most DM_MAX_W x DM_MAX_H (the 320x192 detection channel; 320x180 works too).
 * Cost (one check): about 10 M integer multiply-adds; RAM: dm_work_t about 55 KB (caller-owned, static), dm_ref_t about 14 KB. */
#ifndef DRIFT_MATCH_H
#define DRIFT_MATCH_H

#include <stdint.h>

#define DM_MAX_W 320
#define DM_MAX_H 192
#define DM_NP 16             /* patches in the reference */
#define DM_PS 16             /* patch size at L1 (32 px of the full frame) */
#define DM_P0 (2 * DM_PS)    /* the same patch at full resolution */
#define DM_RF 3              /* full-resolution refinement range around the L1 peak (px) */
#define DM_W0 (DM_P0 + 2 * DM_RF)
#define DM_RX 24             /* search range, L1 px (2 px of the full frame each): +-48 x +-32 px of the full frame */
#define DM_RY 16
#define DM_MIN_VALID 4       /* valid patches needed in the live view */
#define DM_MIN_INL 4         /* inliers needed */

typedef struct {
    int x, y;                /* L1 top-left (even) */
    int e1, e0;              /* energy (sum of squares) of the fields */
    int8_t f1[DM_PS * DM_PS * 2];
    int8_t f0[DM_P0 * DM_P0 * 2]; /* full-resolution field, for the final refinement */
} dm_patch_t;

typedef struct {
    int valid;
    unsigned w, h;           /* frame size of the reference */
    int n;                   /* patches */
    int quads;               /* distinct frame quadrants the patches cover */
    dm_patch_t p[DM_NP];
} dm_ref_t;

typedef struct {
    int8_t f1[(DM_MAX_W / 2) * (DM_MAX_H / 2) * 2];
    int32_t e1[(DM_MAX_W / 2 + 1) * (DM_MAX_H / 2 + 1)];
    int k;                   /* noise floor of the last frame prepared */
} dm_work_t;

enum { DM_OK = 0, DM_INCONC, DM_FLAT, DM_NOREF, DM_BAD };

typedef struct {
    int verdict;
    int dx_q, dy_q;          /* displacement of the live content against the reference, 1/4 px of the full frame (+ = content moved right / down) */
    int dx, dy;              /* the same rounded to px */
    int conf;                /* permille: inliers / valid patches */
    int npatch, nvalid, ninl, nquad;
    int score;               /* permille: mean score of the inliers (half resolution) */
    int fine;                /* 1: the full-resolution refinement agrees (about 0.5 px); 0: only the half-resolution estimate (about 3 px, e.g. day against night) */
    const char *why;
} dm_res_t;

/* Builds the reference from a frame. Returns the patches kept (below DM_MIN_VALID the reference is unusable), or -1 for a bad frame. */
int dm_ref_build(const uint8_t *y, unsigned w, unsigned h, unsigned stride, dm_ref_t *ref, dm_work_t *wk);
/* Matches a live frame (same size) against the reference. Always fills *r. */
int dm_match(const dm_ref_t *ref, const uint8_t *y, unsigned w, unsigned h, unsigned stride, dm_work_t *wk, dm_res_t *r);
/* 1 when a match of two frames a moment apart proves a STATIC view (the drift reference may be captured): a consensus at zero displacement (+-0.5 px). Agreement of 90 % of the
 * valid patches is enough; 75 % is too, when at least 6 patches on 2 quadrants agree: the outliers are a person or a lamp in a few patches, a real move of the view shifts all of
 * them together (hardware 2026-11-19: 0,0 with 833 permille agreeing was refused). */
int dm_static_ok(const dm_res_t *r);
const char *dm_verdict_name(int v);

#endif
