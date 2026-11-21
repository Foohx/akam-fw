/* See drift_match.h. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "drift_match.h"

#define CAP 250        /* saturation of the doubled-angle magnitude (a strong edge is a strong edge, whatever the contrast) */
#define KMIN 16        /* noise floor of the Sobel magnitude */
#define STRONG2 150    /* |w|^2 at which a field pixel counts as structure (|w| >= 20 of 125) */
#define T_FINE 250     /* permille: a patch needs this score at its peak */
#define UNIQ_R 3       /* a second peak counts outside this radius (L1 px) ... */
#define UNIQ_PCT 92    /* ... when it scores more than this percent of the best */
#define SELF_PCT 85    /* a reference patch whose second peak (in the search window of the reference itself) scores more than this percent of its best is refused */
#define SEL_RX 12      /* the search window of that test (L1 px) */
#define SEL_RY 8
#define BAD_R 8        /* candidates this close (L1 px) to a refused one are skipped: a straight edge is refused all along */
#define MAX_TRIES 300   /* candidates examined (each costs one full search) */
#define FLAT_PX 150    /* strong pixels (L1) a live view needs to be judged at all */

static uint32_t isqrt64(uint64_t v)
{
    uint64_t r = 0, b = (uint64_t)1 << 62;
    while (b > v)
        b >>= 2;
    while (b) {
        if (v >= r + b) {
            v -= r + b;
            r = (r >> 1) + b;
        } else
            r >>= 1;
        b >>= 2;
    }
    return (uint32_t)r;
}

const char *dm_verdict_name(int v)
{
    static const char *const n[] = { "ok", "inconclusive", "flat", "noref", "bad" };
    return v >= 0 && v <= DM_BAD ? n[v] : "?";
}

/* The doubled-angle vector of the Sobel gradient at (p): (gx^2 - gy^2, 2 gx gy) / (|g| + k) with its length saturated at CAP. The angle doubling makes it
 * polarity-invariant: a dark-on-light edge and a light-on-dark one point the same way. 0 on a flat pixel. */
static void vec(const uint8_t *p, int stride, int k, int *c, int *s)
{
    int64_t gx = (p[-stride + 1] + 2 * p[1] + p[stride + 1]) - (p[-stride - 1] + 2 * p[-1] + p[stride - 1]);
    int64_t gy = (p[stride - 1] + 2 * p[stride] + p[stride + 1]) - (p[-stride - 1] + 2 * p[-stride] + p[-stride + 1]);
    int64_t m2 = gx * gx + gy * gy, m, cc, ss, wm;
    *c = *s = 0;
    if (m2 == 0)
        return;
    m = isqrt64((uint64_t)m2);
    cc = (gx * gx - gy * gy) / (m + k);
    ss = 2 * gx * gy / (m + k);
    wm = m * m / (m + k);
    if (wm > CAP) { /* keep the direction, saturate the length */
        cc = cc * CAP / wm;
        ss = ss * CAP / wm;
    }
    *c = (int)cc;
    *s = (int)ss;
}

/* The noise floor k of an image: twice the median Sobel magnitude (weak texture and sensor noise, at night, vote with a small weight). */
static int noise_floor(const uint8_t *y, int w, int h, int stride)
{
    int hist[256];
    int x, yy, i, n = 0, acc = 0, med = 0, k;
    memset(hist, 0, sizeof hist);
    for (yy = 1; yy < h - 1; yy++)
        for (x = 1; x < w - 1; x++) {
            const uint8_t *p = y + yy * stride + x;
            int64_t gx = (p[-stride + 1] + 2 * p[1] + p[stride + 1]) - (p[-stride - 1] + 2 * p[-1] + p[stride - 1]);
            int64_t gy = (p[stride - 1] + 2 * p[stride] + p[stride + 1]) - (p[-stride - 1] + 2 * p[-stride] + p[-stride + 1]);
            int m = (int)isqrt64((uint64_t)(gx * gx + gy * gy)) >> 2;
            hist[m > 255 ? 255 : m]++;
            n++;
        }
    for (i = 0; i < 256 && acc * 2 < n; i++) {
        acc += hist[i];
        med = i * 4 + 2;
    }
    k = 2 * med;
    return k < KMIN ? KMIN : k;
}

/* The field at HALF resolution, built from the full-resolution vectors (so fine plaster relief survives), summed over each 2x2 block: out[2 * i], out[2 * i + 1]
 * in -125..125; the border is 0. */
static void field(const uint8_t *y, int w, int h, int stride, int k, int8_t *out)
{
    int x, yy, w1 = w / 2;
    memset(out, 0, (size_t)w1 * (h / 2) * 2);
    for (yy = 1; yy < h - 1; yy++)
        for (x = 1; x < w - 1; x++) {
            int c, s;
            if (x / 2 < w1 && yy / 2 < h / 2) {
                int8_t *o = out + 2 * ((yy / 2) * w1 + x / 2);
                int vx, vy;
                vec(y + yy * stride + x, stride, k, &c, &s);
                vx = o[0] + c / 4;
                vy = o[1] + s / 4;
                o[0] = (int8_t)(vx > 125 ? 125 : vx < -125 ? -125 : vx);
                o[1] = (int8_t)(vy > 125 ? 125 : vy < -125 ? -125 : vy);
            }
        }
}

/* The full-resolution field of the window (x0, y0, nw x nh) of the frame (zero where the Sobel kernel would leave the frame), c / 2 per component. */
static void field_win(const uint8_t *y, int w, int h, int stride, int k, int x0, int y0, int nw, int nh, int8_t *out)
{
    int x, yy;
    for (yy = 0; yy < nh; yy++)
        for (x = 0; x < nw; x++) {
            int c = 0, s = 0, X = x0 + x, Y = y0 + yy;
            if (X >= 1 && X < w - 1 && Y >= 1 && Y < h - 1)
                vec(y + Y * stride + X, stride, k, &c, &s);
            out[2 * (yy * nw + x)] = (int8_t)(c / 2);
            out[2 * (yy * nw + x) + 1] = (int8_t)(s / 2);
        }
}

static int energy(const int8_t *f, int w, int x0, int y0, int n)
{
    int x, y, e = 0;
    for (y = 0; y < n; y++)
        for (x = 0; x < n; x++) {
            const int8_t *p = f + 2 * ((y0 + y) * w + x0 + x);
            e += p[0] * p[0] + p[1] * p[1];
        }
    return e;
}

static int dot(const int8_t *pf, const int8_t *f, int w, int x0, int y0, int n)
{
    int x, y, d = 0;
    for (y = 0; y < n; y++) {
        const int8_t *a = pf + 2 * y * n, *b = f + 2 * ((y0 + y) * w + x0);
        for (x = 0; x < 2 * n; x++)
            d += a[x] * b[x];
    }
    return d;
}

/* permille cosine similarity (0 when negative) */
static int score(int d, int ea, int eb)
{
    uint32_t q;
    if (d <= 0 || ea <= 0 || eb <= 0)
        return 0;
    q = isqrt64((uint64_t)ea * (uint64_t)eb);
    return q ? (int)((int64_t)d * 1000 / q) : 0;
}

static int strong_px(const int8_t *f, int w, int x0, int y0, int nw, int nh)
{
    int x, y, c = 0;
    for (y = 0; y < nh; y++)
        for (x = 0; x < nw; x++) {
            const int8_t *p = f + 2 * ((y0 + y) * w + x0 + x);
            if (p[0] * p[0] + p[1] * p[1] >= STRONG2)
                c++;
        }
    return c;
}

static int prep(const uint8_t *y, unsigned w, unsigned h, unsigned stride, dm_work_t *wk)
{
    if (!y || !wk || w < 64 || h < 48 || w > DM_MAX_W || h > DM_MAX_H || (w & 3) || (h & 3) || stride < w)
        return -1;
    wk->k = noise_floor(y, (int)w, (int)h, (int)stride);
    field(y, (int)w, (int)h, (int)stride, wk->k, wk->f1);
    return 0;
}

static void integ(dm_work_t *wk, int w1, int h1)
{
    int x, y, W = w1 + 1; /* integral of the field energy */
    memset(wk->e1, 0, (size_t)W * (size_t)(h1 + 1) * sizeof(int32_t));
    for (y = 0; y < h1; y++) {
        int row = 0;
        for (x = 0; x < w1; x++) {
            const int8_t *p = wk->f1 + 2 * (y * w1 + x);
            row += p[0] * p[0] + p[1] * p[1];
            wk->e1[(y + 1) * W + x + 1] = wk->e1[y * W + x + 1] + row;
        }
    }
}

/* Scores a patch (field pf, energy pe, nominal L1 position px,py) at every displacement of +-DM_RX x +-DM_RY into surf (-1 = outside the frame). Returns the
 * best score; *bx, *by its displacement; *s2 the best score outside UNIQ_R of it (the second peak). */
static int search(const int8_t *pf, int pe, const dm_work_t *wk, int w1, int h1, int px, int py, int rx, int ry, int *surf, int *bx, int *by, int *s2)
{
    int a, b, ns = 0, bs = -1, W = w1 + 1;
    *bx = *by = *s2 = 0;
    for (b = -ry; b <= ry; b++)
        for (a = -rx; a <= rx; a++) {
            int tx = px + a, ty = py + b, s = -1;
            if (tx >= 0 && ty >= 0 && tx + DM_PS <= w1 && ty + DM_PS <= h1) {
                int ec = wk->e1[(ty + DM_PS) * W + tx + DM_PS] - wk->e1[ty * W + tx + DM_PS] - wk->e1[(ty + DM_PS) * W + tx] + wk->e1[ty * W + tx];
                s = score(dot(pf, wk->f1, w1, tx, ty, DM_PS), pe, ec);
                if (s > bs) {
                    bs = s;
                    *bx = a;
                    *by = b;
                }
            }
            surf[ns++] = s;
        }
    ns = 0;
    for (b = -ry; b <= ry; b++)
        for (a = -rx; a <= rx; a++, ns++)
            if ((abs(a - *bx) > UNIQ_R || abs(b - *by) > UNIQ_R) && surf[ns] > *s2)
                *s2 = surf[ns];
    return bs;
}

typedef struct { int x, y, e; } cand_t;

int dm_ref_build(const uint8_t *y, unsigned w, unsigned h, unsigned stride, dm_ref_t *ref, dm_work_t *wk)
{
    static cand_t cand[1200];
    static int surf[(2 * DM_RX + 1) * (2 * DM_RY + 1)];
    int w1, h1, pass, n = 0, quadmask = 0, cell[12], nc = 0, i, j, cx, cy, tried = 0, nbad = 0, bad[200][2];
    memset(ref, 0, sizeof *ref);
    if (prep(y, w, h, stride, wk) != 0)
        return -1;
    w1 = (int)w / 2;
    h1 = (int)h / 2;
    ref->w = w;
    ref->h = h;
    for (cy = 2; cy + DM_PS <= h1 - 2 && nc < 1200; cy += 4)
        for (cx = 2; cx + DM_PS <= w1 - 2 && nc < 1200; cx += 4) {
            if (strong_px(wk->f1, w1, cx, cy, DM_PS, DM_PS) < 3)
                continue;
            cand[nc].x = cx;
            cand[nc].y = cy;
            cand[nc].e = energy(wk->f1, w1, cx, cy, DM_PS);
            nc++;
        }
#ifdef DM_DEBUG
    fprintf(stderr, "cands %d\n", nc);
#endif
    for (i = 1; i < nc; i++) { /* insertion sort, descending energy */
        cand_t t = cand[i];
        for (j = i - 1; j >= 0 && cand[j].e < t.e; j--)
            cand[j + 1] = cand[j];
        cand[j + 1] = t;
    }
    memset(cell, 0, sizeof cell);
    for (pass = 1; pass <= 3; pass++)
        for (i = 0; i < nc && n < DM_NP; i++) {
            int c = ((cand[i].y + DM_PS / 2) * 3 / h1) * 4 + (cand[i].x + DM_PS / 2) * 4 / w1, ok = 1;
            dm_patch_t *p = &ref->p[n];
            if (cell[c] >= pass)
                continue;
            for (j = 0; j < n; j++)
                if (abs(ref->p[j].x - cand[i].x) < DM_PS && abs(ref->p[j].y - cand[i].y) < DM_PS)
                    ok = 0;
            if (!ok)
                continue;
            for (j = 0; j < DM_PS; j++)
                memcpy(p->f1 + j * DM_PS * 2, wk->f1 + 2 * ((cand[i].y + j) * w1 + cand[i].x), DM_PS * 2);
            for (j = 0; j < nbad; j++)
                if (abs(bad[j][0] - cand[i].x) < BAD_R && abs(bad[j][1] - cand[i].y) < BAD_R)
                    ok = 0;
            if (!ok)
                continue;
            if (tried++ >= MAX_TRIES)
                break;
            integ(wk, w1, h1);
            {
                int bx, by, s2, bs = search(p->f1, cand[i].e, wk, w1, h1, cand[i].x, cand[i].y, SEL_RX, SEL_RY, surf, &bx, &by, &s2);
                {
#ifdef DM_DEBUG
                    fprintf(stderr, "cand (%d,%d) e=%d s2=%d\n", cand[i].x * 2, cand[i].y * 2, cand[i].e, s2);
#endif
                }
                if (bs < 900 || s2 * 100 > bs * SELF_PCT) { /* ambiguous inside the search window: a bare straight edge, a repetitive texture */
                    if (nbad < 200) {
                        bad[nbad][0] = cand[i].x;
                        bad[nbad++][1] = cand[i].y;
                    }
                    continue;
                }
            }
            p->x = cand[i].x;
            p->y = cand[i].y;
            p->e1 = cand[i].e;
            field_win(y, (int)w, (int)h, (int)stride, wk->k, 2 * p->x, 2 * p->y, DM_P0, DM_P0, p->f0);
            p->e0 = energy(p->f0, DM_P0, 0, 0, DM_P0);
            cell[c]++;
            n++;
        }
    for (i = 0; i < n; i++)
        quadmask |= 1 << (((ref->p[i].y + DM_PS / 2) * 2 / h1) * 2 + (ref->p[i].x + DM_PS / 2) * 2 / w1);
    ref->n = n;
    ref->quads = __builtin_popcount((unsigned)quadmask);
    ref->valid = n >= DM_MIN_VALID;
    return n;
}

static int cmp_int(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }
static int median(int *v, int n)
{
    qsort(v, (size_t)n, sizeof *v, cmp_int);
    return v[n / 2];
}

/* parabola vertex offset in 1/8 of the step (so 1/4 px of the full frame per L1 step of 8), -4..4 */
static int vertex(int l, int c, int r)
{
    int dn = l - 2 * c + r, f;
    if (dn >= 0)
        return 0;
    f = (int)((int64_t)(l - r) * 8 / (2 * dn));
    return f > 4 ? 4 : f < -4 ? -4 : f;
}

int dm_match(const dm_ref_t *ref, const uint8_t *y, unsigned w, unsigned h, unsigned stride, dm_work_t *wk, dm_res_t *r)
{
    static int surf[(2 * DM_RX + 1) * (2 * DM_RY + 1)];
    int w1, h1, i, nv = 0, ni = 0, spread = 0, best = -1, bestn = 0, bests = 0, k = 0;
    int cd[DM_NP][2], cs[DM_NP], valid[DM_NP], inl[DM_NP], fx[DM_NP], fy[DM_NP], f1x[DM_NP], f1y[DM_NP], vx[DM_NP], vy[DM_NP];
    memset(r, 0, sizeof *r);
    r->verdict = DM_INCONC;
    r->npatch = ref ? ref->n : 0;
    if (!ref || !ref->valid) {
        r->verdict = DM_NOREF;
        r->why = "no usable reference";
        return 0;
    }
    if (w != ref->w || h != ref->h || prep(y, w, h, stride, wk) != 0) {
        r->verdict = DM_BAD;
        r->why = "bad frame";
        return 0;
    }
    w1 = (int)w / 2;
    h1 = (int)h / 2;
    if (strong_px(wk->f1, w1, 0, 0, w1, h1) < FLAT_PX) {
        r->verdict = DM_FLAT;
        r->why = "live view has no edge structure";
        return 0;
    }
    integ(wk, w1, h1);
    for (i = 0; i < ref->n; i++) {
        const dm_patch_t *p = &ref->p[i];
        int bx, by, s2, bs = search(p->f1, p->e1, wk, w1, h1, p->x, p->y, DM_RX, DM_RY, surf, &bx, &by, &s2);
        valid[i] = 0;
#ifdef DM_DEBUG
        fprintf(stderr, "patch %d (%d,%d) best %d at %d,%d second %d\n", i, p->x * 2, p->y * 2, bs, bx, by, s2);
#endif
        if (bs < T_FINE || s2 * 100 > bs * UNIQ_PCT) /* too weak, or not unique */
            continue;
        cd[i][0] = bx;
        cd[i][1] = by;
        cs[i] = bs;
        f1x[i] = bx * 8; /* quarter px of the full frame: one L1 px = 8 */
        f1y[i] = by * 8;
        {
            int idx = (by + DM_RY) * (2 * DM_RX + 1) + bx + DM_RX;
            if (bx > -DM_RX && bx < DM_RX && p->x + bx > 0 && p->x + bx + DM_PS < w1)
                f1x[i] += vertex(surf[idx - 1], bs, surf[idx + 1]);
            if (by > -DM_RY && by < DM_RY && p->y + by > 0 && p->y + by + DM_PS < h1)
                f1y[i] += vertex(surf[idx - (2 * DM_RX + 1)], bs, surf[idx + (2 * DM_RX + 1)]);
        }
        valid[i] = 1;
        nv++;
    }
    r->nvalid = nv;
    if (nv < DM_MIN_VALID) {
        r->why = "too few valid patches (flat view, an obstruction or another scene)";
        return 0;
    }
    for (i = 0; i < ref->n; i++) { /* consensus: the seed with the most agreeing patches */
        int c = 0, s = 0, j;
        if (!valid[i])
            continue;
        for (j = 0; j < ref->n; j++)
            if (valid[j] && abs(cd[j][0] - cd[i][0]) <= 1 + abs(cd[i][0]) / 8 && abs(cd[j][1] - cd[i][1]) <= 1 + abs(cd[i][1]) / 8) {
                c++;
                s += cs[j];
            }
        if (c > bestn || (c == bestn && s > bests)) {
            best = i;
            bestn = c;
            bests = s;
        }
    }
    if (best < 0 || bestn < DM_MIN_INL || bestn * 100 < nv * 60) {
        r->why = "no consensus between the patches";
        return 0;
    }
    {
        int n = 0, quad = 0, x0 = 9999, x1 = -9999, y0 = 9999, y1 = -9999, mx, my;
        static int8_t win[DM_W0 * DM_W0 * 2];
        for (i = 0; i < ref->n; i++) { /* the patches at the consensus displacement, refined at full resolution with a parabolic sub-pixel peak */
            const dm_patch_t *p = &ref->p[i];
            int a, b, bs = -1, bx = 0, by = 0, s9[2 * DM_RF + 1][2 * DM_RF + 1], ox, oy;
            inl[i] = valid[i] && abs(cd[i][0] - cd[best][0]) <= 1 + abs(cd[best][0]) / 8 && abs(cd[i][1] - cd[best][1]) <= 1 + abs(cd[best][1]) / 8;
            if (!inl[i])
                continue;
            inl[i] = 0;
            ox = 2 * (p->x + cd[i][0]) - DM_RF;
            oy = 2 * (p->y + cd[i][1]) - DM_RF;
            if (ox < 0 || oy < 0 || ox + DM_W0 > (int)w || oy + DM_W0 > (int)h)
                continue;
            field_win(y, (int)w, (int)h, (int)stride, wk->k, ox, oy, DM_W0, DM_W0, win);
            memset(s9, 0, sizeof s9);
            for (b = 0; b <= 2 * DM_RF; b++)
                for (a = 0; a <= 2 * DM_RF; a++) {
                    int sc = score(dot(p->f0, win, DM_W0, a, b, DM_P0), p->e0, energy(win, DM_W0, a, b, DM_P0));
                    s9[b][a] = sc;
                    if (sc > bs) {
                        bs = sc;
                        bx = a;
                        by = b;
                    }
                }
            if (bs < T_FINE)
                continue;
            fx[i] = (ox + bx - 2 * p->x) * 4;
            fy[i] = (oy + by - 2 * p->y) * 4;
            if (bx > 0 && bx < 2 * DM_RF)
                fx[i] += vertex(s9[by][bx - 1], s9[by][bx], s9[by][bx + 1]) / 2;
            if (by > 0 && by < 2 * DM_RF)
                fy[i] += vertex(s9[by - 1][bx], s9[by][bx], s9[by + 1][bx]) / 2;
            inl[i] = 1;
        }
        /* the full-resolution answer when enough patches support it, else the half-resolution one (day against night: the fine relief shading differs, the
         * edges do not; the caller sees r->fine = 0 and treats the number as good to about 3 px) */
        int stage;
        for (stage = 0; stage < 2; stage++) {
            const int *ax = stage == 0 ? fx : f1x, *ay = stage == 0 ? fy : f1y, tol = stage == 0 ? 8 : 12;
            int cnt = 0;
            n = 0;
            k = 0;
            quad = 0;
            x0 = y0 = 9999;
            x1 = y1 = -9999;
            for (i = 0; i < ref->n; i++)
                if (stage == 0 ? inl[i] : valid[i] && abs(cd[i][0] - cd[best][0]) <= 1 + abs(cd[best][0]) / 8 && abs(cd[i][1] - cd[best][1]) <= 1 + abs(cd[best][1]) / 8) {
                    vx[cnt] = ax[i];
                    vy[cnt] = ay[i];
                    cnt++;
                }
            if (cnt < DM_MIN_INL)
                continue;
            mx = median(vx, cnt);
            my = median(vy, cnt);
            for (i = 0; i < ref->n; i++) {
                int in = (stage == 0 ? inl[i] : valid[i] && abs(cd[i][0] - cd[best][0]) <= 1 + abs(cd[best][0]) / 8 && abs(cd[i][1] - cd[best][1]) <= 1 + abs(cd[best][1]) / 8) && abs(ax[i] - mx) <= tol && abs(ay[i] - my) <= tol;
                if (in) {
                    vx[n] = ax[i];
                    vy[n] = ay[i];
                    k += cs[i];
                    n++;
                    quad |= 1 << (((ref->p[i].y + DM_PS / 2) * 2 / h1) * 2 + (ref->p[i].x + DM_PS / 2) * 2 / w1);
                    if (ref->p[i].x < x0) x0 = ref->p[i].x;
                    if (ref->p[i].x > x1) x1 = ref->p[i].x;
                    if (ref->p[i].y < y0) y0 = ref->p[i].y;
                    if (ref->p[i].y > y1) y1 = ref->p[i].y;
                }
            }
            if (n >= DM_MIN_INL && n * 100 >= nv * 60) {
                r->fine = stage == 0;
                break;
            }
            n = 0;
        }
        ni = n;
        r->nquad = __builtin_popcount((unsigned)quad);
        spread = (x1 - x0) * 5 >= w1 || (y1 - y0) * 5 >= h1; /* the agreeing patches span a fifth of the frame */
    }
    if (ni < DM_MIN_INL || ni * 100 < nv * 60) {
        r->why = "too few patches agree";
        return 0;
    }
    if (!spread) {
        r->why = "the agreeing patches do not span the frame";
        return 0;
    }
    r->dx_q = median(vx, ni);
    r->dy_q = median(vy, ni);
    r->score = k / ni;
    r->dx = (r->dx_q >= 0 ? r->dx_q + 2 : r->dx_q - 2) / 4;
    r->dy = (r->dy_q >= 0 ? r->dy_q + 2 : r->dy_q - 2) / 4;
    r->ninl = ni;
    r->conf = ni * 1000 / nv;
    r->verdict = DM_OK;
    r->why = "consensus";
    return 0;
}

int dm_static_ok(const dm_res_t *r)
{
    if (r->verdict != DM_OK || r->dx_q > 2 || r->dx_q < -2 || r->dy_q > 2 || r->dy_q < -2)
        return 0;
    return r->conf >= 900 || (r->conf >= 750 && r->ninl >= 6 && r->nquad >= 2);
}
