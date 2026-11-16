/* State and senders of the NR keys (img.nr2d_bias ... img.nr3d_motion, GET img.nr, SET img.dump nr): the knob values, the
 * cached pristine blocks, and the SETs. The tables themselves are composed by isp_nr.c. Every call here runs with the ISP
 * configuration lock held (imgq.c takes it). RAM only: nothing is saved, a reload or a restart returns to the vendor
 * bytes (the knobs are re-applied after a reload, isp_nr_reapply). specs/video-and-streaming.md 5.5.7. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ak_rtsp.h"
#include "ak_ioctls.h"
#include "isp.h"
#include "isp_nr.h"

#define EXPO_BUF 0x720
#define SCRATCH  65536

static isp_nr_knobs_t g_knobs = { { 0, 0, 0, 0, 100, 100, 100, 100 } };
static uint8_t *g_base[ISP_NR_SHARP]; /* pristine live block (NR1, NR2, UVNR, 3D_NR) from the first non-default use */
static int      g_dirty[ISP_NR_SHARP]; /* the block holds bytes other than the baseline */

const isp_nr_knobs_t *isp_nr_knobs(void) { return &g_knobs; }
int isp_nr_knob_get(int knob) { return (knob >= 0 && knob < ISP_KNOB_COUNT) ? g_knobs.v[knob] : 0; }

static int fetch_base(int b)
{
    if (g_base[b]) return 0;
    size_t len = isp_nr_block_len(b);
    uint8_t *scratch = calloc(1, SCRATCH);
    uint8_t *keep = malloc(len);
    int rc = -1;
    if (scratch && keep && isp_tunnel(isp_nr_get_cmd(b), scratch) == 0) {
        memcpy(keep, scratch, len);
        g_base[b] = keep;
        keep = NULL;
        rc = 0;
    }
    free(scratch);
    free(keep);
    return rc;
}

/* Re-sends the blocks first..last (NR1+NR2 is two, the others one). 0..2 = blocks sent, -1 = ISP refused or unreadable
 * (errno), ISP_NR_REFUSED = live table not as decoded (nothing sent). */
static int apply_blocks(int first, int last)
{
    uint8_t *out[ISP_NR_SHARP] = { NULL };
    int sent = 0, rc = 0;
    for (int b = first; b <= last; b++) {
        if (isp_nr_block_default(b, &g_knobs)) continue;
        if (fetch_base(b) != 0) { rc = -1; goto done; }
    }
    for (int b = first; b <= last; b++) {
        size_t len = isp_nr_block_len(b);
        if (isp_nr_block_default(b, &g_knobs)) {
            if (!g_dirty[b]) continue;   /* never changed: nothing to put back */
            out[b] = malloc(len);
            if (!out[b]) { rc = -1; goto done; }
            memcpy(out[b], g_base[b], len); /* back to the original bytes */
        } else {
            out[b] = malloc(len);
            if (!out[b]) { rc = -1; goto done; }
            rc = isp_nr_compose(b, g_base[b], len, &g_knobs, out[b]);
            if (rc != 0) { rc = rc == ISP_NR_REFUSED ? ISP_NR_REFUSED : -1; goto done; }
        }
    }
    for (int b = first; b <= last; b++) {
        if (!out[b]) continue;
        if (isp_tunnel(isp_nr_set_cmd(b), out[b]) != 0) { rc = -1; goto done; }
        g_dirty[b] = !isp_nr_block_default(b, &g_knobs);
        sent++;
    }
    rc = sent;
done:
    for (int b = first; b <= last; b++) free(out[b]);
    return rc;
}

static int apply_knob(int knob)
{
    switch (knob) {
    case ISP_KNOB_NR2D_BIAS: case ISP_KNOB_NR2D:           return apply_blocks(ISP_NR_NR1, ISP_NR_NR2);
    case ISP_KNOB_UVNR_BIAS: case ISP_KNOB_UVNR:           return apply_blocks(ISP_NR_UVNR, ISP_NR_UVNR);
    case ISP_KNOB_NR3D_BIAS: case ISP_KNOB_NR3D_MOTION:    return apply_blocks(ISP_NR_3DNR, ISP_NR_3DNR);
    default:                                               return isp_sharp_refresh();
    }
}

/* Value v (range already checked) for a knob. >= 0: done (the number of blocks sent, 0 = nothing to send), -1: the ISP
 * did not accept it (errno), ISP_NR_REFUSED: the live table is not as decoded. On failure the old value is kept. */
int isp_nr_knob_set(int knob, int v)
{
    if (knob < 0 || knob >= ISP_KNOB_COUNT) return -1;
    int old = g_knobs.v[knob];
    g_knobs.v[knob] = v;
    int rc = apply_knob(knob);
    if (rc < 0) g_knobs.v[knob] = old;
    return rc;
}

int isp_set_denoise(int bias)
{
    int rc = isp_nr_knob_set(ISP_KNOB_NR3D_BIAS, bias);
    return rc < 0 ? rc : 0;
}

/* After a pipeline reload (isp_reset_picture_baselines): the vendor blocks are back, so the cached baselines and the
 * "changed" marks are stale. */
void isp_nr_reset(void)
{
    for (int b = 0; b < ISP_NR_SHARP; b++) { free(g_base[b]); g_base[b] = NULL; g_dirty[b] = 0; }
}

/* Re-applies the non-default knobs of the NR blocks on the fresh baselines (SHARP is re-applied by isp.c). */
void isp_nr_reapply(void)
{
    int rc;
    if (!isp_nr_block_default(ISP_NR_NR1, &g_knobs) && (rc = apply_blocks(ISP_NR_NR1, ISP_NR_NR2)) < 0)
        fprintf(stderr, "[isp.nr] NR1/NR2 knobs not re-applied after the reload (%d)\n", rc);
    if (!isp_nr_block_default(ISP_NR_UVNR, &g_knobs) && (rc = apply_blocks(ISP_NR_UVNR, ISP_NR_UVNR)) < 0)
        fprintf(stderr, "[isp.nr] UVNR knobs not re-applied after the reload (%d)\n", rc);
    if (!isp_nr_block_default(ISP_NR_3DNR, &g_knobs) && (rc = apply_blocks(ISP_NR_3DNR, ISP_NR_3DNR)) < 0)
        fprintf(stderr, "[isp.nr] 3D_NR knobs not re-applied after the reload (%d)\n", rc);
}

static int tunnel_get(void *ctx, uint32_t cmd, void *buf)
{
    (void)ctx;
    return isp_tunnel(cmd, buf);
}

/* GET img.nr: knobs, live G and level, and the proven fields of that level of the live blocks (read-only). */
int isp_nr_status(char *out, size_t cap)
{
    uint8_t expo[EXPO_BUF], run[64];
    uint32_t g = 0;
    int lvl = -1;
    memset(expo, 0, sizeof(expo));
    memset(run, 0, sizeof(run));
    if (isp_tunnel(ISP_INNER_GET_EXPOSURE, expo) == 0 && isp_tunnel(ISP_INNER_GET_AE_RUN_INFO, run) == 0)
        lvl = isp_nr_level(expo, sizeof(expo), run, 36, &g);
    size_t w = (size_t)snprintf(out, cap, "img.nr=G=%u level=%d knobs=", (unsigned)g, lvl);
    for (int i = 0; i < ISP_KNOB_COUNT && w < cap; i++)
        w += (size_t)snprintf(out + w, cap - w, "%s%d", i ? "," : "", g_knobs.v[i]);
    if (lvl < 0) {
        if (w < cap) snprintf(out + w, cap - w, " (exposure not readable; knob order: nr2d_bias,uvnr_bias,nr3d_bias,sharp_bias,nr2d,uvnr,sharp_coring,nr3d_motion)");
        return 0;
    }
    isp_nr_live_t l;
    isp_nr_live_level(tunnel_get, NULL, lvl, &l);
    if (w < cap) w += (size_t)snprintf(out + w, cap - w, " NR1.s=%u/%u/%u NR2.s=%u UVNR.s=%u 3D.lo=%u 3D.hi=%u 3D.s=%u SHARP.coring=",
                                       l.nr1_s[0], l.nr1_s[1], l.nr1_s[2], l.nr2_s, l.uvnr_s, l.d3_lo, l.d3_hi, l.d3_s);
    for (int i = 0; i < 8 && w < cap; i++) w += (size_t)snprintf(out + w, cap - w, "%u%c", l.coring[i], i < 7 ? ',' : ' ');
    if (w < cap) w += (size_t)snprintf(out + w, cap - w, "q8=");
    for (int i = 0; i < 8 && w < cap; i++) w += (size_t)snprintf(out + w, cap - w, "%u%c", l.gain[i], i < 7 ? ',' : ' ');
    for (int b = 0; b < ISP_NR_BLOCKS; b++)
        if (!l.ok[b] && w < cap) w += (size_t)snprintf(out + w, cap - w, "[%s unreadable]", isp_nr_block_name(b));
    return 0;
}

static void dump_emit(void *ctx, const char *line)
{
    (void)ctx;
    printf("%s\n", line);
}

int isp_nr_dump_live(void) { return isp_nr_dump(tunnel_get, NULL, dump_emit, NULL); }
