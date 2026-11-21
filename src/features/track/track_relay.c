/* See track_relay.h. Integer arithmetic for the correlation sums, one double division and a few iterations per candidate (no libm). */
#include "track_relay.h"
#include <string.h>

static void downsample(const uint8_t *y, unsigned fw, unsigned fh, unsigned stride, uint8_t *dst) {
    int w = (int)(fw / TR_DS), h = (int)(fh / TR_DS), r, c, i, j;
    for (r = 0; r < h; r++)
        for (c = 0; c < w; c++) {
            unsigned s = 0;
            const uint8_t *p = y + (size_t)r * TR_DS * stride + (size_t)c * TR_DS;
            for (i = 0; i < TR_DS; i++, p += stride)
                for (j = 0; j < TR_DS; j++) s += p[j];
            dst[r * w + c] = (uint8_t)(s / (TR_DS * TR_DS));
        }
}

void tr_init(tr_t *t) { memset(t, 0, sizeof *t); }

/* The ZNCC in percent (negative = anti-correlated) of the template (nx x ny samples every sx / sy from (tx0, ty0) of prev) against cur shifted by (dx, dy). *flat = 1 when the template has no texture. */
static int zncc(const uint8_t *prev, const uint8_t *cur, int w, int tx0, int ty0, int nx, int ny, int sx, int sy, int dx, int dy, int *flat) {
    long n = 0, sa = 0, sb = 0, saa = 0, sbb = 0, sab = 0, va, vb, cov;
    int r, c, k;
    double z, x;
    for (r = 0; r < ny; r++) {
        const uint8_t *a = prev + (ty0 + r * sy) * w + tx0, *b = cur + (ty0 + r * sy + dy) * w + tx0 + dx;
        for (c = 0; c < nx; c++) {
            int va_ = a[c * sx], vb_ = b[c * sx];
            sa += va_; sb += vb_; saa += va_ * va_; sbb += vb_ * vb_; sab += va_ * vb_; n++;
        }
    }
    va = n * saa - sa * sa; vb = n * sbb - sb * sb; cov = n * sab - sa * sb;
    if (va < n * n * (long)TR_MIN_STD * TR_MIN_STD) { *flat = 1; return -100; }
    if (vb <= 0) return -100;
    z = (double)cov * (double)cov / ((double)va * (double)vb); /* r^2, 0..1 */
    for (x = 1.0, k = 0; k < 8; k++) x = 0.5 * (x + z / x);    /* sqrt, from above (z > 0 here unless cov == 0) */
    if (cov == 0) return 0;
    return (int)((cov < 0 ? -x : x) * 100.0 + 0.5);
}

/* Sub-pixel position of the peak from the scores around it (a parabola), in frame pixels (-TR_DS/2 .. TR_DS/2); a neighbour that was not evaluated (the window edge) gives 0. */
static int subpix(int l, int c, int r) {
    int den = l - 2 * c + r;
    if (l == 0 || r == 0 || den >= 0) return 0;
    { int num = (l - r) * TR_DS, d = 2 * den; /* d < 0: round to nearest */
      return (num * 2 + (num > 0 ? -d : d)) / (2 * d); }
}

int tr_step(tr_t *t, const uint8_t *y, unsigned fw, unsigned fh, unsigned stride, const int det[4], int64_t now_ms, int out[4]) {
    uint8_t cur[(TR_MAX_W / TR_DS) * (TR_MAX_H / TR_DS)];
    int w, h, ok = 0;
    if (!y || fw < 64 || fh < 64 || fw > TR_MAX_W || fh > TR_MAX_H || stride < fw) return 0;
    w = (int)(fw / TR_DS); h = (int)(fh / TR_DS);
    downsample(y, fw, fh, stride, cur);
    if (t->have_prev && (t->w != w || t->h != h)) t->have_prev = 0;
    if (det) {
        memcpy(t->box, det, sizeof t->box);
        t->box_set = 1; t->det_ms = now_ms; t->dead = 0;
    } else if (t->box_set && !t->dead && t->have_prev && now_ms - t->det_ms <= TR_MAX_MS) {
        /* template: the inner 70 % of the region (the body, not the background at its edges), clipped to the frame, in working pixels */
        int bx1 = t->box[0] / TR_DS, by1 = t->box[1] / TR_DS, bx2 = t->box[2] / TR_DS, by2 = t->box[3] / TR_DS;
        int mx = (bx2 - bx1) * 15 / 100, my = (by2 - by1) * 15 / 100;
        int tx0 = bx1 + mx, tx1 = bx2 - mx, ty0 = by1 + my, ty1 = by2 - my;
        int nx, ny, sx, sy, dx, dy, bdx = 0, bdy = 0, best = -101, flat = 0, grid[2 * TR_SEARCH + 1][2 * TR_SEARCH + 1];
        memset(grid, 0, sizeof grid);
        if (tx0 < 0) tx0 = 0;
        if (ty0 < 0) ty0 = 0;
        if (tx1 > w) tx1 = w;
        if (ty1 > h) ty1 = h;
        if (tx1 - tx0 < 6 || ty1 - ty0 < 6) t->dead = 1; /* too small / outside the frame: nothing to match */
        else {
            sx = (tx1 - tx0 + 23) / 24; sy = (ty1 - ty0 + 23) / 24;
            nx = (tx1 - tx0) / sx; ny = (ty1 - ty0) / sy;
            for (dy = -TR_SEARCH; dy <= TR_SEARCH && !flat; dy++)
                for (dx = -TR_SEARCH; dx <= TR_SEARCH && !flat; dx++) {
                    int z;
                    if (tx0 + dx < 0 || ty0 + dy < 0 || tx0 + dx + (nx - 1) * sx >= w || ty0 + dy + (ny - 1) * sy >= h) continue;
                    z = zncc(t->prev, cur, w, tx0, ty0, nx, ny, sx, sy, dx, dy, &flat);
                    grid[dy + TR_SEARCH][dx + TR_SEARCH] = z + 1000; /* 0 = not evaluated */
                    if (z > best || (z == best && dx * dx + dy * dy < bdx * bdx + bdy * bdy)) { best = z; bdx = dx; bdy = dy; }
                }
            if (flat || best < TR_MIN_ZNCC_PCT) t->dead = 1;
            else {
                int sh_x = bdx * TR_DS + subpix(grid[bdy + TR_SEARCH][bdx + TR_SEARCH - 1], grid[bdy + TR_SEARCH][bdx + TR_SEARCH], grid[bdy + TR_SEARCH][bdx + TR_SEARCH + 1]);
                int sh_y = bdy * TR_DS + subpix(grid[bdy + TR_SEARCH - 1][bdx + TR_SEARCH], grid[bdy + TR_SEARCH][bdx + TR_SEARCH], grid[bdy + TR_SEARCH + 1][bdx + TR_SEARCH]);
                t->box[0] += sh_x; t->box[2] += sh_x;
                t->box[1] += sh_y; t->box[3] += sh_y;
                t->last_zncc = best;
                ok = 1;
            }
        }
    }
    memcpy(t->prev, cur, (size_t)w * (size_t)h);
    t->have_prev = 1; t->w = w; t->h = h;
    if (ok) { /* reported clipped to the frame like the detector's boxes; a region that has left the frame ends the relay */
        int i;
        for (i = 0; i < 4; i++) out[i] = t->box[i];
        if (out[0] < 0) out[0] = 0;
        if (out[1] < 0) out[1] = 0;
        if (out[2] > (int)fw - 1) out[2] = (int)fw - 1;
        if (out[3] > (int)fh - 1) out[3] = (int)fh - 1;
        if (out[2] - out[0] < 8 || out[3] - out[1] < 8) { t->dead = 1; return 0; }
    }
    return ok;
}
