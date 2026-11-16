#pragma once
/* Noise-reduction and sharpening knobs of the 1080p ISP: pure composers (no ioctl, no float, host-testable) that turn
 * the live vendor table of a block into the table to send, plus the ISO-level selection and the decoders behind
 * GET img.nr and SET img.dump nr. Layouts and evidence: specs/video-and-streaming.md 5.5, isp_nr_layout.h.
 * Rules (5.5.7): RAM only; a block whose knobs are all at the default is never rewritten; every composer first checks
 * that the baseline is what the decode says (length, mode word 1 (NR1: or 192, as read on the 1080p cameras), enable words 0/1, weight table = formula within +-1) and refuses
 * (ISP_NR_REFUSED) when it is not. Entry 0 (manual) and the mode word are never changed. */

#include <stddef.h>
#include <stdint.h>
#include "isp_blocks.h"

enum { ISP_NR_NR1, ISP_NR_NR2, ISP_NR_UVNR, ISP_NR_3DNR, ISP_NR_SHARP, ISP_NR_BLOCKS };

enum {
    ISP_KNOB_NR2D_BIAS, ISP_KNOB_UVNR_BIAS, ISP_KNOB_NR3D_BIAS, ISP_KNOB_SHARP_BIAS,
    ISP_KNOB_NR2D, ISP_KNOB_UVNR, ISP_KNOB_CORING, ISP_KNOB_NR3D_MOTION,
    ISP_KNOB_COUNT
};

#define ISP_NR_REFUSED (-2) /* the live table is not as decoded: nothing is sent */

typedef struct isp_nr_knob_info {
    const char *key;     /* 8091 key, "img.nr2d_bias" */
    int         min, max, def;
    const char *blocks;  /* blocks the key re-sends, for the answer line */
    const char *want;    /* range text for the error line */
} isp_nr_knob_info_t;

const isp_nr_knob_info_t *isp_nr_knob_info(int knob);
int  isp_nr_knob_find(const char *key); /* knob index of an 8091 key, -1 if it is none */

typedef struct isp_nr_knobs { int v[ISP_KNOB_COUNT]; } isp_nr_knobs_t;
void isp_nr_knobs_default(isp_nr_knobs_t *k);
int  isp_nr_block_default(int block, const isp_nr_knobs_t *k); /* 1 when every key of the block is at its default */

size_t isp_nr_block_len(int block);       /* bytes of the GET / SET buffer (3D_NR: 7180) */
uint32_t isp_nr_get_cmd(int block);
uint32_t isp_nr_set_cmd(int block);
const char *isp_nr_block_name(int block);

/* lut[1..15] = max(floor, round(1023 * exp(-256 * i * i / s))), integer arithmetic only. lut[0] and lut[16] untouched. */
void isp_nr_gauss(uint16_t lut[17], uint32_t s, uint16_t floor_v);

/* NR1, NR2, UVNR, 3DNR: out (len bytes, must not alias base) = base with the bias, the strength (NR1, NR2, UVNR) or the
 * motion ramp (3DNR) of the knobs. All knobs of the block at default: a plain copy. 0, ISP_NR_REFUSED, or -1 (bad block/len).
 * 3DNR: the 104 bytes after the 7076 table bytes are copied from the baseline (the rc tail is never edited). */
int isp_nr_compose(int block, const uint8_t *base, size_t len, const isp_nr_knobs_t *k, uint8_t *out);

/* SHARP, in the order bias -> slider (isp.c) -> coring. check: length, mode 1, coring fields <= 255 (0 or REFUSED).
 * bias: out = base with entries 1+L <- entry 1+min(L+n,15). coring: in place, entries 1..16, min(255, v * pct / 100). */
int isp_nr_sharp_check(const uint8_t *base, size_t len);
void isp_nr_sharp_bias(const uint8_t *base, int n, uint8_t *out);
void isp_nr_sharp_coring(uint8_t *buf, int pct);

/* ISO level (5.5.1) from the GET_EXPOSURE block (1824 B) and the AE run info (36 B): *g = exposure index G, returns the
 * level 0..15. The pairs are compared with G as stored (the file scales them by 256 already). */
int isp_nr_level(const uint8_t *expo, size_t expo_len, const uint8_t *run, size_t run_len, uint32_t *g);

/* Proven fields of one ISO level of the live blocks, for GET img.nr. ok[b] = 0 when block b could not be read or its
 * mode is not 1 (then its fields are zero). */
typedef struct isp_nr_live {
    int      ok[ISP_NR_BLOCKS];
    uint16_t nr1_s[3], nr2_s, uvnr_s, d3_lo, d3_hi, d3_s, coring[8], gain[8];
} isp_nr_live_t;
void isp_nr_live_level(isp_get_fn get, void *gctx, int level, isp_nr_live_t *out);

/* SET img.dump nr: one line per block and entry with the mode and the decoded proven fields, from live GETs. Returns the
 * number of blocks read. */
int isp_nr_dump(isp_get_fn get, void *gctx, isp_emit_fn emit, void *ectx);
