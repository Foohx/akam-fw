/* See scene_check.h. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "scene_check.h"

const char sc_invalid_marker[] = "invalid\n";

const char *sc_verdict_name(int v)
{
    switch (v) {
    case SC_MATCH: return "match";
    case SC_NO_REF: return "no saved reference";
    case SC_LOW_DETAIL_REF: return "the saved view is too uniform to prove anything";
    case SC_LOW_DETAIL_CUR: return "current view too uniform (too little detail in common with the saved view)";
    case SC_MODE_DIFF: return "day/night mode differs";
    case SC_MISMATCH: return "scene differs";
    case SC_MISMATCH_BLOCKS: return "too few of the detailed image blocks match on their own";
    case SC_PERIODIC: return "periodic full calibration due";
    case SC_STORE_FAILED: return "the boot counter cannot be saved";
    case SC_NEED_PROBE: return "the saved view is too uniform: multi-view check";
    case SC_NOT_HOME: return "the saved reference was not taken at the home position";
    case SC_REPETITIVE: return "the saved view is a repetitive texture (a shift of a period would match)";
    case SC_LIKELY: return "very likely home (zero shift, matching blocks) but not proven";
    case SC_NO_FRAME: return "no usable frame in time";
    case SC_CANCELLED: return "cancelled";
    default: return "?";
    }
}

int sc_downscale_y(const uint8_t *y, unsigned w, unsigned h, unsigned stride, sc_thumb_t *out)
{
    unsigned bx, by, x, yy, ox, oy;
    if (!y || !out || w < SC_W || h < SC_H || w % SC_W || h % SC_H || stride < w)
        return -1;
    bx = w / SC_W;
    by = h / SC_H;
    for (oy = 0; oy < SC_H; oy++)
        for (ox = 0; ox < SC_W; ox++) {
            unsigned long sum = 0;
            for (yy = 0; yy < by; yy++)
                for (x = 0; x < bx; x++)
                    sum += y[(size_t)(oy * by + yy) * stride + ox * bx + x];
            out->px[oy * SC_W + ox] = (uint8_t)((sum + bx * by / 2) / (bx * by));
        }
    return 0;
}

/* ---- comparison: contrast-normalised, noise-compensated block correlation of the band-passed images ----
 * band(x,y) = 49 * (sum of the 3x3 pixels) - 9 * (sum of the 7x7 pixels): a difference of boxes, zero on any flat area and on a linear
 * gradient. A global gain / offset, vignetting and a slowly varying illumination vanish; edges, plaster relief and texture of a few
 * thumbnail pixels stay. The sensor noise (white, so about 0.3 times weaker after the 3x3 average) is estimated from the image itself (a low
 * quantile of the band energy of 8x6 sub-blocks: the quietest part of the picture is noise only), so "detail" means "clearly above the
 * noise of this image" instead of an absolute level. */
#define NSX 8                /* noise estimation grid */
#define NSY 6
#define BM 3                 /* the band image exists from BM to SIZE-1-BM */
#define NV_CREDIBLE (2 * NV_MIN) /* above this the noise estimate is mostly texture (a fixed pattern over a quarter of the view, a fine scene) or a noisy sensor: no block is called weak (neutral), they all vote */
#define NV_MIN 6000          /* floor of the noise energy of the band image: sigma 0.6 gray level on the thumbnail (8-bit rounding, banding of a perfect synthetic ramp) */

typedef struct {
    int32_t b[SC_N];             /* the band image */
    long long nv;                /* noise energy per band sample (>= NV_MIN) */
    unsigned det;                /* bit per block (SC_BX x SC_BY): clearly above the noise */
    unsigned strong;             /* bit per block: well above the noise (SC_WEAK_RATIO): its correlation is meaningful on its own */
    long long en[SC_BX * SC_BY]; /* band energy per sample of each block */
    int ndet;
} prep_t;

static void block_rect(int bx, int by, int *x0, int *y0, int *x1, int *y1)
{
    const int bw = SC_W / SC_BX, bh = SC_H / SC_BY;
    *x0 = bx * bw; *y0 = by * bh; *x1 = *x0 + bw; *y1 = *y0 + bh;
    if (*x0 < BM) *x0 = BM;
    if (*y0 < BM) *y0 = BM;
    if (*x1 > SC_W - BM) *x1 = SC_W - BM;
    if (*y1 > SC_H - BM) *y1 = SC_H - BM;
}

static long long energy(const prep_t *a, int x0, int y0, int x1, int y1, long long *n)
{
    long long s = 0;
    int x, y;
    for (y = y0; y < y1; y++)
        for (x = x0; x < x1; x++)
            s += (long long)a->b[y * SC_W + x] * a->b[y * SC_W + x];
    *n = (long long)(x1 - x0) * (y1 - y0);
    return s;
}

static int cmp_int(const void *a, const void *b) { return (*(const int *)a > *(const int *)b) - (*(const int *)a < *(const int *)b); }

static void prepare(const sc_thumb_t *t, prep_t *a)
{
    int x, y, i, bx, by, e[NSX * NSY];
    const int lo = BM, hi_x = SC_W - BM, hi_y = SC_H - BM;
    const int sw = (hi_x - lo) / NSX, sh = (hi_y - lo) / NSY;
    memset(a->b, 0, sizeof a->b);
    for (y = lo; y < hi_y; y++)
        for (x = lo; x < hi_x; x++) {
            int s3 = 0, s7 = 0, u, v;
            for (v = -3; v <= 3; v++)
                for (u = -3; u <= 3; u++) {
                    int p = t->px[(y + v) * SC_W + x + u];
                    s7 += p;
                    if (v >= -1 && v <= 1 && u >= -1 && u <= 1)
                        s3 += p;
                }
            a->b[y * SC_W + x] = 49 * s3 - 9 * s7;
        }
    /* noise: the Laplacian 4p - (4 neighbours) is the finest scale of the picture, where a camera image is mostly noise (white noise of variance
     * s2 gives 20 s2 there, 17640 s2 in the band image). Its 25th percentile over the 8x6 sub-blocks bounds the noise from above (texture only adds):
     * an overestimate makes the check refuse more, never accept more. */
    for (i = 0; i < NSX * NSY; i++) {
        int x0 = lo + (i % NSX) * sw, y0 = lo + (i / NSX) * sh;
        long long n = 0, sum = 0;
        for (y = y0; y < y0 + sh; y++)
            for (x = x0; x < x0 + sw; x++) {
                const uint8_t *q = &t->px[y * SC_W + x];
                int r = 4 * q[0] - q[-1] - q[1] - q[-SC_W] - q[SC_W];
                sum += (long long)r * r;
                n++;
            }
        e[i] = (int)(sum / n);
    }
    qsort(e, NSX * NSY, sizeof e[0], cmp_int);
    a->nv = 882LL * e[(NSX * NSY) / 4];
    if (a->nv < NV_MIN)
        a->nv = NV_MIN;
    a->det = 0;
    a->strong = 0;
    a->ndet = 0;
    for (by = 0; by < SC_BY; by++)
        for (bx = 0; bx < SC_BX; bx++) {
            int x0, y0, x1, y1;
            long long n, s;
            block_rect(bx, by, &x0, &y0, &x1, &y1);
            s = energy(a, x0, y0, x1, y1, &n);
            a->en[by * SC_BX + bx] = n ? s / n : 0; /* (long long: a strong edge squares to more than 2^31) */
            if (n && s >= (long long)SC_DETAIL_RATIO * a->nv * n) {
                a->det |= 1u << (by * SC_BX + bx);
                a->ndet++;
                if (s >= (long long)SC_WEAK_RATIO * a->nv * n || a->nv > NV_CREDIBLE)
                    a->strong |= 1u << (by * SC_BX + bx);
            }
        }
}

int sc_has_detail(const sc_thumb_t *t)
{
    static prep_t a;
    prepare(t, &a);
    return a.ndet >= SC_DETAIL_BLOCKS_MIN;
}

/* square root by Newton (the camera libc needs no libm for this). The start is the exponent-halving bit trick (within a few percent), so 5 iterations
 * reach the full double precision: the camera has no FPU, each iteration is a software division (the old start from x itself needed up to 60 of them:
 * it was most of the 3 s of the repetitive screening and of the 65 ms of the comparison). */
static double dsqrt(double x)
{
    union { double d; uint64_t u; } v;
    double r;
    if (x <= 0.0)
        return 0.0;
    v.d = x;
    v.u = (v.u >> 1) + (1023ULL << 51);
    r = v.d;
    for (int i = 0; i < 5; i++)
        r = 0.5 * (r + x / r);
    return r;
}

/* Noise-compensated zero-mean normalised correlation (permille, -1000..1000) of the band image of ref over the block against the one of cur
 * displaced by (dx,dy), on the part of the block where both exist. The independent noise of each image is subtracted from its variance (not from
 * the covariance, which it does not touch): a clean copy of the same view scores about 1000 whatever the noise level, as long as the signal is
 * above the noise. */
static int corr_ex(const prep_t *ref, const prep_t *cur, int blk, int dx, int dy, int *offimg)
{
    long long n = 0, sa = 0, sb = 0, saa = 0, sbb = 0, sab = 0; /* integer sums (exact; cheap on a CPU without an FPU) */
    double va, vb, cov, r;
    int x0, y0, x1, y1, x, y;
    block_rect(blk % SC_BX, blk / SC_BX, &x0, &y0, &x1, &y1);
    if (x0 + dx < BM) x0 = BM - dx;
    if (y0 + dy < BM) y0 = BM - dy;
    if (x1 + dx > SC_W - BM) x1 = SC_W - BM - dx;
    if (y1 + dy > SC_H - BM) y1 = SC_H - BM - dy;
    if (offimg)
        *offimg = 0;
    if (x1 - x0 < 6 || y1 - y0 < 6) {
        if (offimg)
            *offimg = 1;
        return -1000;
    }
    for (y = y0; y < y1; y++)
        for (x = x0; x < x1; x++) {
            long long a = ref->b[y * SC_W + x], b = cur->b[(y + dy) * SC_W + x + dx];
            n++;
            sa += a;
            sb += b;
            saa += a * a;
            sbb += b * b;
            sab += a * b;
        }
    va = (double)saa - (double)sa * (double)sa / (double)n - (double)n * (double)ref->nv;
    vb = (double)sbb - (double)sb * (double)sb / (double)n - (double)n * (double)cur->nv;
    cov = (double)sab - (double)sa * (double)sb / (double)n;
    if (va <= 0.0 || vb <= 0.0)
        return -1000;
    r = cov / dsqrt(va * vb) * 1000.0;
    if (r > 1000.0) r = 1000.0;
    if (r < -1000.0) r = -1000.0;
    return (int)(r < 0 ? r - 0.5 : r + 0.5);
}

static int corr(const prep_t *ref, const prep_t *cur, int blk, int dx, int dy) { return corr_ex(ref, cur, blk, dx, dy, NULL); }

#define SURF_MAX ((2 * SC_DRIFT_R + 1) * (2 * SC_DRIFT_R + 1))

/* The comparison. R: the displacement search radius (thumbnail px). centered = 0: the boot proof (the common displacement must lie within SC_TOL of zero, the margin is over every displacement
 * outside that box). centered = 1: the drift check (the common displacement may be anywhere in the search window: the box of SC_TOL px for the margin is centred on the best displacement, so
 * "the peak is unique" is judged the same way). Everything else (the per-block classification at the common displacement) is identical. */
static int compare_core(const sc_thumb_t *ref, const sc_thumb_t *cur, int min_permille, int R, int centered, sc_cmp_t *o)
{
    static prep_t ar, ac; /* 2 x 15 KB: not on the (small) thread stack; the PTZ thread is the only caller (not re-entrant) */
    static int surf[SC_BX * SC_BY][SURF_MAX];
    int vote[SC_BX * SC_BY], jump[SC_BX * SC_BY], same[SC_BX * SC_BY], nvote = 0, nrel = 0, njump = 0, b, k, best;
    const int nsh = 2 * R + 1, zk = R * nsh + R, floor_same = min_permille - SC_SAME_BELOW, floor_move = min_permille - SC_MOVE_BELOW, floor_weak = min_permille - SC_WEAK_SAME_BELOW,
              floor_soft = min_permille - SC_SOFT_BELOW;
    long long tot[SURF_MAX], bestsum, outsum, sum = 0, wsum = 0, jsr[SC_BX * SC_BY], jsc[SC_BX * SC_BY];
    memset(o, 0, sizeof *o);
    o->score = -1000;
    prepare(ref, &ar);
    prepare(cur, &ac);
    o->ref_detail = ar.ndet;
    o->cur_detail = ac.ndet;
    for (b = 0; b < SC_BX * SC_BY; b++)
        if ((ar.det >> b) & (ac.det >> b) & 1u)
            vote[nvote++] = b;
    o->both = nvote;
    o->vote_n = 0;
    /* A block detailed on one side only counts AGAINST the match when the other side is clearly flat there (below 1.5x its noise): the scene
     * (or a fixed pattern) is present in one image and absent in the other. A borderline block (1.5 to 3x the noise: exposure, flicker) is ignored. */
    for (b = 0; b < SC_BX * SC_BY; b++) {
        unsigned r = (ar.det >> b) & 1u, c = (ac.det >> b) & 1u;
        /* ... and so does a block STRONGLY detailed on one side (>= 2x the detail threshold) whatever the other side shows: such a view cannot be
         * the same scene with the other one merely a little noisier. */
        const long long nmax = ar.nv > ac.nv ? ar.nv : ac.nv; /* the larger noise of the two images: a noisy live view must not turn a strong block into a vote against */
        if ((r && !c && (2 * ac.en[b] < 3 * ac.nv || ar.en[b] >= 2LL * SC_DETAIL_RATIO * nmax)) ||
            (c && !r && (2 * ar.en[b] < 3 * ar.nv || ac.en[b] >= 2LL * SC_DETAIL_RATIO * nmax)))
            o->vote_n++;
    }
    if (!nvote)
        return 0;
    /* Displacement evidence per block. A moved head shifts the CONTENT: the block's correlation peak leaves the common displacement. A changed
     * illumination (a lamp, dusk, the IR light, a window) lowers the scores but leaves the peak where it was, so a drop of the score alone is
     * no mismatch. Pass 1 finds the common displacement, pass 2 classifies each block at it:
     *   SAME:    the block's peak is at the common displacement (its score there is within SC_SAME_SLACK of its best at any searched
     *            displacement: a broad edge keeps a high score a pixel or two away, so a margin per block would be noise; the unique-peak
     *            margin is on the summed surface) and it scores >= min - SC_SAME_BELOW. Positive evidence.
     *   MOVED:   (strong blocks) a shifted peak >= min - SC_MOVE_BELOW beats the common displacement by SC_MOVE_MARGIN. Against.
     *   mismatch: (strong blocks) no peak at the common displacement, none elsewhere either (another scene), unless the peak is still at
     *            the common displacement with a score >= min - SC_SOFT_BELOW (lighting lowered it). Against, except for that case.
     *   neutral: weak blocks (high-pass energy < SC_WEAK_RATIO x noise: noise dominates their score) that are not SAME, and the blocks that
     *            are not SAME and whose signal energy changed by more than SC_JUMP_RATIO (a window, a screen; at most SC_JUMP_MAX of them,
     *            more is another scene). A block that correlates keeps its SAME vote whatever its energy did (a gain, a lamp over half the view).
     * Neutral is no free pass: the weak blocks are pooled (mean score at the common displacement >= SC_WEAK_POOL: the same wall under another
     * light is still positively correlated, another wall is not). */
    memset(tot, 0, sizeof tot);
    for (b = 0; b < nvote; b++) {
        const int blk = vote[b];
        long long sr = ar.en[blk] - ar.nv, sc = ac.en[blk] - ac.nv;
        for (k = 0; k < nsh * nsh; k++)
            surf[b][k] = corr(&ar, &ac, blk, k % nsh - R, k / nsh - R);
        jsr[b] = sr;
        jsc[b] = sc;
        nrel++;
        for (k = 0; k < nsh * nsh; k++)
            tot[k] += surf[b][k];
    }
    { /* the global gain between the images: the median block energy ratio (AE, contrast); a block JUMPS only relative to it */
        double q[SC_BX * SC_BY], gm = 1.0;
        int nq = 0, i, j;
        for (b = 0; b < nvote; b++)
            if (jsr[b] > 0 && jsc[b] > 0)
                q[nq++] = (double)jsc[b] / (double)jsr[b];
        for (i = 1; i < nq; i++) {
            double v = q[i];
            for (j = i; j > 0 && q[j - 1] > v; j--)
                q[j] = q[j - 1];
            q[j] = v;
        }
        if (nq)
            gm = nq & 1 ? q[nq / 2] : 0.5 * (q[nq / 2 - 1] + q[nq / 2]);
        for (b = 0; b < nvote; b++) {
            double sr = (double)jsr[b] * gm, sc = (double)jsc[b];
            jump[b] = sr > SC_JUMP_RATIO * sc || sc > SC_JUMP_RATIO * sr;
        }
    }
    bestsum = -(1LL << 60);
    outsum = -(1LL << 60);
    best = zk;
    for (k = 0; k < nsh * nsh; k++)
        if (tot[k] > bestsum) {
            bestsum = tot[k];
            best = k;
        }
    for (k = 0; k < nsh * nsh; k++) {
        int dx = k % nsh - R - (centered ? best % nsh - R : 0), dy = k / nsh - R - (centered ? best / nsh - R : 0);
        if ((abs(dx) > SC_TOL || abs(dy) > SC_TOL) && tot[k] > outsum)
            outsum = tot[k];
    }
    if (nrel) {
        o->shift_x = best % nsh - R;
        o->shift_y = best / nsh - R;
        o->peak_ok = (centered || (abs(o->shift_x) <= SC_TOL && abs(o->shift_y) <= SC_TOL)) && (bestsum - outsum) / nrel >= SC_PEAK_MARGIN;
    }
    for (b = 0; b < nvote; b++) {
        int ball = -2000;
        for (k = 0; k < nsh * nsh; k++)
            if (surf[b][k] > ball)
                ball = surf[b][k];
        same[b] = surf[b][best] >= (((ar.strong & ac.strong) >> vote[b]) & 1u ? floor_same : floor_weak) && surf[b][best] >= ball - SC_SAME_SLACK;
        njump += jump[b] && !same[b]; /* a block whose energy changed but whose content still correlates is only a changed gain: SAME */
    }
    for (b = 0; b < nvote; b++) {
        const int at = surf[b][best], strong = (((ar.strong & ac.strong) >> vote[b]) & 1u) != 0;
        int ball = -2000, cls; /* cls: 0 neutral, 1 SAME, 2 MOVED, 3 mismatch */
        for (k = 0; k < nsh * nsh; k++)
            if (surf[b][k] > ball)
                ball = surf[b][k];
        if (same[b])
            cls = 1;
        else if (jump[b])
            cls = njump > SC_JUMP_MAX ? 3 : 0;
        else if (!strong)
            cls = 0;
        else if (ball >= floor_move && ball >= at + SC_MOVE_MARGIN)
            cls = 2;
        else if (at >= floor_soft && at >= ball - SC_SAME_SLACK)
            cls = 0; /* lighting-lowered, still peaking where it should */
        else
            cls = 3;
        if (cls == 1) {
            o->matched++;
            o->same_mask |= 1u << vote[b];
            sum += at;
        } else if (cls == 2) {
            o->moved++;
        } else if (cls == 0) {
            o->neutral++;
            if (jump[b] || strong)
                o->doubt++; /* a changed block / a lighting-lowered one: no vote, but it weighs half against (below) */
        }
        if (!strong && cls != 3 && !(jump[b] && cls != 1)) {
            o->weak_n++; /* the weak pool: every weak block that is not a changed one, SAME or not */
            wsum += at;
        }
        if (cls)
            o->vote_n++;
    }
    o->score = o->matched ? (int)(sum / o->matched) : -1000;
    o->weak_mean = o->weak_n ? (int)(wsum / o->weak_n) : 1000;
    return 0;
}

int sc_compare(const sc_thumb_t *ref, const sc_thumb_t *cur, int min_permille, sc_cmp_t *o) { return compare_core(ref, cur, min_permille, SC_SHIFT, 0, o); }

/* ---- the drift check (scene_check.h) ---- */
const char *sc_drift_name(int v)
{
    static const char *const n[] = { "ok", "shift", "recal", "inconclusive", "skip" };
    return v >= 0 && v < 5 ? n[v] : "?";
}

int sc_drift(const sc_ref_t *ref, const sc_thumb_t *cur, int cur_mode, int min_permille, sc_drift_t *out)
{
    sc_cmp_t c;
    int strict;
    memset(out, 0, sizeof *out);
    out->verdict = SC_DRIFT_SKIP;
    out->score = -1000;
    if (!ref || !ref->valid) {
        out->why = "no reference";
        return out->verdict;
    }
    if (cur_mode < 0 || cur_mode != ref->mode) {
        out->why = "day/night mode differs from the reference";
        return out->verdict;
    }
    if (ref->rep != 0 || ref->repmin <= 0 || ref->repmin > min_permille) {
        out->why = ref->rep == 1 ? "reference is ambiguous (repetitive)" : "reference not screened for repetitive texture at this threshold";
        return out->verdict;
    }
    compare_core(&ref->th, cur, min_permille, SC_DRIFT_R, 1, &c);
    if (c.ref_detail < SC_DETAIL_BLOCKS_MIN) {
        out->why = "reference featureless";
        return out->verdict;
    }
    if (c.cur_detail < SC_DETAIL_BLOCKS_MIN || c.both < SC_DETAIL_BLOCKS_MIN) {
        out->why = "live view featureless";
        return out->verdict;
    }
    out->dx = c.shift_x;
    out->dy = c.shift_y;
    out->score = c.score;
    out->matched = c.matched;
    out->vote_n = c.vote_n;
    out->both = c.both;
    out->moved = c.moved;
    strict = c.peak_ok && c.matched >= SC_SAME_BLOCKS_MIN && c.score >= min_permille - SC_MEAN_SLACK && c.matched * 3 >= c.vote_n * 2 + c.doubt && c.matched * 3 >= c.both &&
             c.weak_mean >= SC_WEAK_POOL;
    if (strict) {
        if (!c.shift_x && !c.shift_y) {
            out->verdict = SC_DRIFT_OK;
            out->why = "zero displacement proven";
        } else if (abs(c.shift_x) <= SC_DRIFT_MAX_PX && abs(c.shift_y) <= SC_DRIFT_MAX_PX) {
            out->verdict = SC_DRIFT_SHIFT;
            out->why = "displacement proven";
        } else {
            out->verdict = SC_DRIFT_RECAL;
            out->why = "displacement proven but beyond the correctable range";
        }
    } else if (!c.shift_x && !c.shift_y && c.moved * 2 <= c.matched && c.matched >= SC_SAME_BLOCKS_MIN && c.score >= min_permille) {
        out->verdict = SC_DRIFT_INCONC; /* at least SC_SAME_BLOCKS_MIN blocks peak at zero displacement (a moved head would move them): the counters are probably right */
        out->why = "zero displacement, but too many blocks changed for the strict vote (a person, the light)";
    } else {
        out->verdict = SC_DRIFT_RECAL;
        out->why = c.peak_ok ? "no strict proof (mismatch)" : "no unique peak (ambiguous)";
    }
    return out->verdict;
}

/* ---- the repetitive-texture screening ----
 * The image against itself at every displacement beyond the tolerance (+-SC_REP_RANGE px, step 1: a period p aliases at 2p, 3p too, and an
 * odd period is not seen by even steps). Only the blocks still inside the image vote (a block partly off the image is no vote, not a -1000).
 * A displacement PASSES when the match rule of sc_compare would accept the aliased view: at least SC_SAME_BLOCKS_MIN strong blocks score
 * >= min - SC_SAME_BELOW - SC_REP_MARGIN, they are >= 2/3 of the strong blocks and >= 1/3 of all detailed blocks that are still in the image,
 * and their mean is >= min - SC_MEAN_SLACK - SC_REP_MARGIN (a clean self-correlation is an upper bound of what a live, noisy alias would score).
 * The passing displacements connected to the tolerance box are the broad main lobe of the correlation (a blurry texture, vertical edges =
 * invariance along one axis): not a period, and the match refuses it by itself (the peak is not unique), so they are not flagged; a separate
 * passing lobe is.
 * Cost: every displacement is evaluated in full, with an early exit as soon as too few blocks are left to pass (the verdict is computed once, at
 * capture, and stored in the reference: the boot only reads it). */
static int rep_pass(const prep_t *a, const int *vote, int nv, int dx, int dy, int min_permille)
{
    const int bmin = min_permille - SC_SAME_BELOW - SC_REP_MARGIN, tmin = min_permille - SC_MEAN_SLACK - SC_REP_MARGIN;
    int ok[SC_BX * SC_BY], valid = 0, left, b, m = 0, fs = 0, need;
    long long sum = 0;
    for (b = 0; b < nv; b++) {
        int x0, y0, x1, y1;
        block_rect(vote[b] % SC_BX, vote[b] / SC_BX, &x0, &y0, &x1, &y1);
        ok[b] = !((x0 + dx < BM ? BM - dx : x0) > (x1 + dx > SC_W - BM ? SC_W - BM - dx : x1) - 6 || (y0 + dy < BM ? BM - dy : y0) > (y1 + dy > SC_H - BM ? SC_H - BM - dy : y1) - 6);
        valid += ok[b];
    }
    need = (valid + 2) / 3;
    if (need < SC_SAME_BLOCKS_MIN)
        need = SC_SAME_BLOCKS_MIN;
    if (valid < need)
        return 0;
    for (b = 0, left = valid; b < nv; b++) {
        int off, c;
        if (!ok[b])
            continue;
        left--;
        c = corr_ex(a, a, vote[b], dx, dy, &off);
        if (!off && c >= bmin) {
            m++;
            sum += c;
        } else if ((a->strong >> vote[b]) & 1u) {
            fs++;
        }
        if (m + left < need || 2 * fs > m + left)
            return 0;
    }
    return m >= need && 2 * fs <= m && sum / m >= tmin;
}

int sc_repetitive_why(const sc_thumb_t *t, int min_permille, int *wdx, int *wdy, int *wkind)
{
    static prep_t a;
    static unsigned char pass[2 * SC_REP_RANGE + 1][2 * SC_REP_RANGE + 1];
    int vote[SC_BX * SC_BY], nv = 0, b, dx, dy;
    prepare(t, &a);
    for (b = 0; b < SC_BX * SC_BY; b++)
        if ((a.det >> b) & 1u)
            vote[nv++] = b;
    if (nv < SC_DETAIL_BLOCKS_MIN)
        return 0;
    for (dy = -SC_REP_RANGE; dy <= SC_REP_RANGE; dy++)
        for (dx = -SC_REP_RANGE; dx <= SC_REP_RANGE; dx++) {
            unsigned char *p = &pass[dy + SC_REP_RANGE][dx + SC_REP_RANGE];
            *p = 0;
            if (abs(dx) <= SC_TOL && abs(dy) <= SC_TOL)
                continue;
            *p = rep_pass(&a, vote, nv, dx, dy, min_permille);
        }
    {
        static short queue[(2 * SC_REP_RANGE + 1) * (2 * SC_REP_RANGE + 1)];
        int head = 0, tail = 0;
        for (dy = -SC_TOL - SC_REP_BRIDGE; dy <= SC_TOL + SC_REP_BRIDGE; dy++)
            for (dx = -SC_TOL - SC_REP_BRIDGE; dx <= SC_TOL + SC_REP_BRIDGE; dx++)
                if ((abs(dx) > SC_TOL || abs(dy) > SC_TOL) && pass[dy + SC_REP_RANGE][dx + SC_REP_RANGE]) {
                    pass[dy + SC_REP_RANGE][dx + SC_REP_RANGE] = 2; /* seen: main lobe */
                    queue[tail++] = (short)((dy + SC_REP_RANGE) * (2 * SC_REP_RANGE + 1) + dx + SC_REP_RANGE);
                }
        while (head < tail) {
            int cy = queue[head] / (2 * SC_REP_RANGE + 1), cx = queue[head] % (2 * SC_REP_RANGE + 1), ny, nx;
            head++;
            for (ny = cy - 1; ny <= cy + 1; ny++)
                for (nx = cx - 1; nx <= cx + 1; nx++)
                    if (ny >= 0 && ny <= 2 * SC_REP_RANGE && nx >= 0 && nx <= 2 * SC_REP_RANGE && pass[ny][nx] == 1) {
                        pass[ny][nx] = 2;
                        queue[tail++] = (short)(ny * (2 * SC_REP_RANGE + 1) + nx);
                    }
        }
    }
    for (dy = 0; dy <= 2 * SC_REP_RANGE; dy++)
        for (dx = 0; dx <= 2 * SC_REP_RANGE; dx++) {
            int ex = abs(dx - SC_REP_RANGE), ey = abs(dy - SC_REP_RANGE);
            /* a separate lobe, or a main lobe that reaches beyond SC_REP_LOBE_MAX px (a lobe that wide is an alias chain, not a broad peak) */
            if (pass[dy][dx] == 1 || (pass[dy][dx] == 2 && (ex > SC_REP_LOBE_MAX || ey > SC_REP_LOBE_MAX))) {
                if (wdx) *wdx = dx - SC_REP_RANGE;
                if (wdy) *wdy = dy - SC_REP_RANGE;
                if (wkind) *wkind = pass[dy][dx]; /* 1: a separate lobe (a period), 2: the main lobe reaches beyond SC_REP_LOBE_MAX px (invariance along an axis) */
                return 1;
            }
        }
    return 0;
}

int sc_repetitive(const sc_thumb_t *t, int min_permille) { return sc_repetitive_why(t, min_permille, NULL, NULL, NULL); }

/* One line of diagnosis for the thumbnails the check used (see scene_check.h). */
void sc_diag_text(const sc_thumb_t *ref, const sc_thumb_t *cur, int min_permille, const char *what, int with_rep, char *out, size_t cap)
{
    static prep_t pa, pb;
    sc_cmp_t c;
    int dx = 0, dy = 0, kind = 0, rr;
    size_t n;
    prepare(ref, &pa);
    n = (size_t)snprintf(out, cap, "%s: ref noise %lld det %03x strong %03x", what, pa.nv, pa.det, pa.strong);
    if (cur) {
        prepare(cur, &pb);
        sc_compare(ref, cur, min_permille, &c);
        if (n < cap)
            n += (size_t)snprintf(out + n, cap - n, " | live noise %lld det %03x strong %03x | both %d vote %d SAME %d (%03x) MOVED %d neutral %d weak %d/%d doubt %d shift %d,%d peak_ok %d score %d",
                                  pb.nv, pb.det, pb.strong, c.both, c.vote_n, c.matched, c.same_mask, c.moved, c.neutral, c.weak_n, c.weak_mean, c.doubt, c.shift_x, c.shift_y, c.peak_ok, c.score);
    }
    if (!with_rep)
        return; /* the boot does not screen again (it reads the verdict stored in the reference): no extra time */
    rr = sc_repetitive_why(ref, min_permille, &dx, &dy, &kind);
    if (n < cap)
        snprintf(out + n, cap - n, " | repetitive %d%s", rr, kind == 1 ? " (separate lobe at" : kind == 2 ? " (main lobe reaches beyond 8 px at" : " (none)");
    if (rr && n + 40 < cap) {
        n = strlen(out);
        snprintf(out + n, cap - n, " %d,%d)", dx, dy);
    }
}

int sc_score(const sc_thumb_t *ref, const sc_thumb_t *cur)
{
    sc_cmp_t c;
    sc_compare(ref, cur, SC_MIN_DEFAULT, &c);
    return c.score;
}

int sc_blocks_ok(const sc_thumb_t *ref, const sc_thumb_t *cur, int min_permille)
{
    sc_cmp_t c;
    sc_compare(ref, cur, min_permille, &c);
    return c.matched;
}

int sc_decide(const sc_ref_t *ref, const sc_thumb_t *cur, int cur_mode, int min_permille, int *score)
{
    sc_cmp_t c;
    if (score)
        *score = -1000;
    if (!ref || !ref->valid)
        return SC_NO_REF;
    sc_compare(&ref->th, cur, min_permille, &c);
    if (c.ref_detail < SC_DETAIL_BLOCKS_MIN)
        return SC_LOW_DETAIL_REF;
    if (c.cur_detail < SC_DETAIL_BLOCKS_MIN || c.both < SC_DETAIL_BLOCKS_MIN)
        return SC_LOW_DETAIL_CUR;
    if (cur_mode < 0 || cur_mode != ref->mode)
        return SC_MODE_DIFF;
    if (score)
        *score = c.score;
    /* the detailed blocks vote: at least 2/3 of them must match on their own (peak inside the tolerance, clearly above any larger shift). A
     * pattern fixed to the camera (dirt, overlay, hot spot) covers a few blocks only, the rest of the scene (or another scene) votes it down;
     * a smooth fixed pattern (vignetting) is removed by the high-pass and gives no detailed block at all. */
    if (!c.peak_ok || c.matched < SC_SAME_BLOCKS_MIN || c.score < min_permille - SC_MEAN_SLACK)
        return SC_MISMATCH;
    if (c.matched * 3 >= c.vote_n * 2 + c.doubt && c.matched * 3 >= c.both && c.weak_mean >= SC_WEAK_POOL)
        return SC_MATCH;
    /* Displacement evidence (never a proof): at EXACTLY zero shift, with no block that shows the content shifted (MOVED) and enough blocks matching on their own with a mean score >=
     * the threshold itself, the head very likely did not move; what is against it is a person, furniture, a window, the light, or a fixed pattern: the strict vote above cannot tell, so
     * this is only SC_LIKELY. The motor layer then runs the short calibration and asks again; the answer to THAT question is the strict vote (sc_boot_check is the same code). */
    if (c.shift_x == 0 && c.shift_y == 0 && c.moved == 0 && c.matched >= SC_SAME_BLOCKS_MIN && c.score >= min_permille)
        return SC_LIKELY;
    return SC_MISMATCH_BLOCKS;
}

int sc_parse_min(const char *s)
{
    int v = 0, n = 0;
    if (!s)
        return -1;
    if (!strcmp(s, "1") || !strcmp(s, "1.0") || !strcmp(s, "1.00") || !strcmp(s, "1.000"))
        return 1000;
    if (s[0] != '0' || s[1] != '.')
        return -1;
    for (s += 2; *s >= '0' && *s <= '9' && n < 3; s++, n++)
        v = v * 10 + (*s - '0');
    if (*s || n == 0)
        return -1;
    for (int k = n; k < 3; k++)
        v *= 10;
    return v >= SC_MIN_LOWEST ? v : -1;
}

size_t sc_pack(const sc_ref_t *ref, char *out, size_t cap)
{
    int h = snprintf(out, cap, "scene2 %d %u %u %d %d %u %u %d %d\n", ref->mode ? 1 : 0, (unsigned)ref->pan, (unsigned)ref->tilt, SC_W, SC_H,
                    (unsigned)ref->boots, (unsigned)ref->stamp, ref->rep == 1 ? 1 : 0, ref->rep >= 0 ? ref->repmin : 0);
    if (h < 0 || (size_t)h + SC_N > cap)
        return 0;
    memcpy(out + h, ref->th.px, SC_N);
    return (size_t)h + SC_N;
}

/* one unsigned field of the header, followed by `sep` */
static int field(const char **p, unsigned long max, char sep, unsigned long *v)
{
    char *e;
    if (**p < '0' || **p > '9')
        return -1;
    *v = strtoul(*p, &e, 10);
    if (*e != sep || *v > max)
        return -1;
    *p = e + (sep ? 1 : 0);
    return 0;
}

int sc_unpack(const char *buf, size_t len, sc_ref_t *ref)
{
    char hdr[80];
    const char *nl = memchr(buf, '\n', len < sizeof hdr ? len : sizeof hdr - 1), *p;
    unsigned long mode, pan, tilt, w, h, boots, stamp = 0, repv = 0, repmin = 0;
    int v2 = 0;
    size_t hl;
    memset(ref, 0, sizeof *ref);
    if (!nl)
        return -1;
    hl = (size_t)(nl - buf) + 1;
    if (len != hl + SC_N)
        return -1;
    memcpy(hdr, buf, hl - 1);
    hdr[hl - 1] = '\0';
    if (!strncmp(hdr, "scene2 ", 7))
        v2 = 1;
    else if (strncmp(hdr, "scene1 ", 7))
        return -1;
    p = hdr + 7;
    if (field(&p, 1, ' ', &mode) || field(&p, 65535, ' ', &pan) || field(&p, 65535, ' ', &tilt) || field(&p, SC_W, ' ', &w) ||
        field(&p, SC_H, ' ', &h) || w != SC_W || h != SC_H)
        return -1;
    if (v2) { /* scene2: boots, stamp, the repetitive verdict (0 / 1), the threshold it was screened at; all mandatory */
        if (field(&p, 1000000, ' ', &boots) || field(&p, 4294967295UL, ' ', &stamp) || field(&p, 1, ' ', &repv) || field(&p, 1000, '\0', &repmin) || (repmin && repmin < SC_MIN_LOWEST))
            return -1;
    } else if (field(&p, 1000000, ' ', &boots)) { /* the last fields: the boots counter alone (a file of an older version has no stamp), or boots + stamp */
        if (field(&p, 1000000, '\0', &boots))
            return -1;
    } else if (field(&p, 4294967295UL, '\0', &stamp)) {
        return -1;
    }
    ref->mode = (int)mode;
    ref->pan = (uint32_t)pan;
    ref->tilt = (uint32_t)tilt;
    ref->boots = (unsigned)boots;
    ref->stamp = (uint32_t)stamp;
    ref->rep = v2 && repmin ? (int)repv : -1; /* an older file (scene1) carries no verdict: screened at the boot */
    ref->repmin = v2 && repmin ? (int)repmin : 0;
    memcpy(ref->th.px, buf + hl, SC_N);
    ref->valid = 1;
    return 0;
}

static void slog(const sc_env_t *e, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void slog(const sc_env_t *e, const char *fmt, ...)
{
    char line[200];
    va_list ap;
    if (!e->log)
        return;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    e->log(line);
}

/* "0.934" / "-0.120" */
static const char *fmt3(char *b, size_t n, int permille)
{
    if (permille < 0)
        snprintf(b, n, "-%d.%03d", abs(permille) / 1000, abs(permille) % 1000);
    else
        snprintf(b, n, "%d.%03d", permille / 1000, permille % 1000);
    return b;
}

/* Diagnosis aid: the thumbnails the check used (ref.gray, live.gray: 80x48 gray) and one text line, for the next hardware run (RAM only: the app wires it to /tmp). */
static void diag(const sc_env_t *e, int min_permille, const char *what, const sc_thumb_t *ref, const sc_thumb_t *cur, int verdict)
{
    static char line[640];
    int n;
    if (!e->dump)
        return;
    sc_diag_text(ref, cur, min_permille, what, 0, line, sizeof line - 80);
    n = (int)strlen(line);
    snprintf(line + n, 80, " | verdict %s", sc_verdict_name(verdict));
    e->dump("ref.gray", ref, line);
    if (cur)
        e->dump("live.gray", cur, line);
}

static sc_ref_t g_ref, g_alt[SC_OFFS]; /* 3.9 KB each: not on the (small) thread stack; one boot check per process */
static unsigned g_alt_mask;
static sc_thumb_t g_cur;

int sc_offset(unsigned i, int *dpan, int *dtilt)
{
    static const int off[SC_OFFS][2] = { { SC_OFF_PAN, 0 }, { -SC_OFF_PAN, 0 }, { 0, -SC_OFF_TILT } };
    if (i >= SC_OFFS)
        return -1;
    *dpan = off[i][0];
    *dtilt = off[i][1];
    return 0;
}

int sc_probe_offset(unsigned i, int *dpan, int *dtilt)
{
    if (i >= SC_OFFS || !((g_alt_mask >> i) & 1u))
        return -1;
    return sc_offset(i, dpan, dtilt);
}

/* Waits for the AE to settle and takes a thumbnail into g_cur. 0 ok (*mode_out = the day / night state), else the verdict (SC_NO_FRAME / SC_CANCELLED). */
static int wait_frame(const sc_env_t *e, const sc_cfg_t *c, int64_t t0, int *mode_out)
{
    int64_t t_first = -1, t_mode = -1;
    unsigned seq = 0, last_seq = 0, frames = 0;
    int mode, last_mode = -2, have = 0;
    for (;;) {
        int64_t now = e->now_ms();
        if (e->cancelled && e->cancelled())
            return SC_CANCELLED;
        if (now - t0 > (int64_t)c->timeout_ms)
            return SC_NO_FRAME;
        mode = e->mode();
        if (e->frame(&g_cur, &seq) == 0) {
            if (!have || seq != last_seq) {
                frames++;
                last_seq = seq;
            }
            have = 1;
            if (t_first < 0)
                t_first = now;
        }
        if (mode != last_mode) { /* the day/night state changed (or became known): the exposure restarts */
            last_mode = mode;
            t_mode = now;
            frames = have ? 1 : 0;
        }
        if (have && mode >= 0 && now - t_first >= (int64_t)c->settle_ms && now - t_mode >= (int64_t)c->settle_ms &&
            frames >= c->settle_frames) {
            *mode_out = mode;
            return 0;
        }
        e->sleep_ms(100);
    }
}

/* The repetitive verdict of a reference: the one stored with it when it was captured, trusted only when it was screened at the configured threshold
 * (repmin == min: the screening depends on it, and a lowered ptz_scene_min must not inherit a stricter verdict nor the other way round); else
 * (an older file, another threshold) screened now, once: *screened tells, and the caller stores the verdict back. */
static int rep_verdict(sc_ref_t *r, int min_permille, int *screened)
{
    *screened = 0;
    if (r->rep >= 0 && r->repmin == min_permille)
        return r->rep;
    *screened = 1;
    r->rep = sc_repetitive(&r->th, min_permille);
    r->repmin = min_permille;
    return r->rep;
}

/* The stored views of the offsets that belong to this reference: same mode, position = reference + offset, a detailed view. */
static unsigned load_alts(const sc_env_t *e, const sc_cfg_t *c, const sc_ref_t *ref)
{
    unsigned mask = 0, i;
    int scr;
    if (!e->load_alt)
        return 0;
    for (i = 0; i < SC_OFFS; i++) {
        int dp, dt;
        if (sc_offset(i, &dp, &dt) || e->load_alt(i, &g_alt[i]) != 0 || !g_alt[i].valid || g_alt[i].mode != ref->mode ||
            (int)g_alt[i].pan != (int)ref->pan + dp || (int)g_alt[i].tilt != (int)ref->tilt + dt || !sc_has_detail(&g_alt[i].th) || rep_verdict(&g_alt[i], c->min_permille, &scr))
            continue;
        if (scr && e->store_alt)
            e->store_alt(i, &g_alt[i]); /* screened now (an older file or another threshold): keep the verdict, no screening at the next boot */
        mask |= 1u << i;
    }
    return mask;
}

int sc_boot_check(const sc_env_t *e, const sc_cfg_t *c, uint32_t *pan, uint32_t *tilt)
{
    int64_t t0 = e->now_ms();
    int mode = -1, v, score = -1000, screened = 0, rep_amb;
    int64_t t_rep = 0;
    char b1[16], b2[16];
    g_alt_mask = 0;
    if (e->load(&g_ref) != 0 || !g_ref.valid) {
        slog(e, "ptz: scene check: no saved reference: calibrating");
        return SC_NO_REF;
    }
    if (g_ref.pan != *pan || g_ref.tilt != *tilt) { /* in: the home position; the reference proves "at home" only */
        slog(e, "ptz: scene check: the reference was taken at %u/%u, home is %u/%u: calibrating", (unsigned)g_ref.pan, (unsigned)g_ref.tilt, (unsigned)*pan, (unsigned)*tilt);
        return SC_NOT_HOME;
    }
    t_rep = e->now_ms();
    rep_amb = rep_verdict(&g_ref, c->min_permille, &screened); /* screened once, when the reference was captured (stored in it); an older file is screened here and keeps the verdict at its next store */
    t_rep = e->now_ms() - t_rep;
    if (g_ref.boots >= SC_FULL_EVERY) {
        slog(e, "ptz: scene check: the reference was trusted for %u boots: periodic full calibration", g_ref.boots);
        if (screened && e->store)
            e->store(&g_ref); /* the screened verdict is kept (a match stores it with the counter anyway) */
        return SC_PERIODIC;
    }
    if (rep_amb) { /* the saved home view is ambiguous (a shift would match): it never decides; the stored views at the offsets do, or nothing does */
        if (screened && e->store)
            e->store(&g_ref);
        int rv = wait_frame(e, c, t0, &mode); /* like the flat path: a frame in the same day / night mode, else the view changed: calibrate */
        if (rv != 0) {
            slog(e, "ptz: scene check: %s: calibrating", sc_verdict_name(rv));
            return rv;
        }
        if (mode != g_ref.mode) {
            slog(e, "ptz: scene check: %s and another day / night mode than the reference: calibrating", sc_verdict_name(SC_REPETITIVE));
            return SC_REPETITIVE;
        }
        if ((g_alt_mask = load_alts(e, c, &g_ref)) == 0) {
            slog(e, "ptz: scene check: %s and no usable stored view at the offsets: calibrating", sc_verdict_name(SC_REPETITIVE));
            diag(e, c->min_permille, "boot (ambiguous reference, no offset views)", &g_ref.th, NULL, SC_REPETITIVE);
            return SC_REPETITIVE;
        }
        slog(e, "ptz: scene check: %s: %u stored view(s) at nearby offsets, multi-view check", sc_verdict_name(SC_REPETITIVE), (unsigned)__builtin_popcount(g_alt_mask));
        diag(e, c->min_permille, "boot (ambiguous reference: probe)", &g_ref.th, NULL, SC_NEED_PROBE);
        return SC_NEED_PROBE;
    }
    v = wait_frame(e, c, t0, &mode);
    if (v == 0)
    {
        int64_t t_dec = e->now_ms();
        v = sc_decide(&g_ref, &g_cur, mode, c->min_permille, &score);
        slog(e, "ptz: scene check: timing: repetitive verdict %s %lld ms, compare %lld ms", screened ? "screened now (older reference file)" : "read from the reference", (long long)t_rep, (long long)(e->now_ms() - t_dec));
    }
    if (v == SC_NO_FRAME || v == SC_CANCELLED) {
        /* nothing to dump */
    } else {
        diag(e, c->min_permille, "boot", &g_ref.th, &g_cur, v);
    }
    /* the probe only when the live rest view is itself featureless and in the same day / night mode (else the view changed: calibrate) */
    if (v == SC_LOW_DETAIL_REF && mode == g_ref.mode && !sc_has_detail(&g_cur) && (g_alt_mask = load_alts(e, c, &g_ref)) != 0) {
        slog(e, "ptz: scene check: the saved view is too uniform to prove anything alone: %u stored view(s) at nearby offsets, multi-view check", (unsigned)__builtin_popcount(g_alt_mask));
        return SC_NEED_PROBE;
    }
    if (v != SC_MATCH && v != SC_LIKELY && screened && e->store)
        e->store(&g_ref); /* a refused older file / another threshold: the screened verdict is stored back now (one write), the next boot reads it instead of screening again */
    if (v == SC_MATCH) {
        g_ref.boots++;
        if (e->store && e->store(&g_ref) != 0) { /* the counter cannot advance: never trust a reference that would live forever */
            slog(e, "ptz: scene check: match, but the boot counter cannot be saved: calibrating");
            return SC_STORE_FAILED;
        }
        *pan = g_ref.pan;
        *tilt = g_ref.tilt;
        slog(e, "ptz: scene check: MATCH, score %s >= %s (boot %u of %d since the last full calibration): the head did not move, position %u/%u declared, no calibration",
             fmt3(b1, sizeof b1, score), fmt3(b2, sizeof b2, c->min_permille), g_ref.boots, SC_FULL_EVERY, (unsigned)g_ref.pan, (unsigned)g_ref.tilt);
    } else if (score > -1000) {
        slog(e, "ptz: scene check: %s (score %s, needs %s): calibrating", sc_verdict_name(v), fmt3(b1, sizeof b1, score),
             fmt3(b2, sizeof b2, c->min_permille));
    } else {
        slog(e, "ptz: scene check: %s: calibrating", sc_verdict_name(v));
    }
    return v;
}

int sc_probe_check(const sc_env_t *e, const sc_cfg_t *c, unsigned i, uint32_t *pan, uint32_t *tilt)
{
    int64_t t0 = e->now_ms(), t_end;
    unsigned seq = 0, first_seq = 0, frames = 0, wait = c->probe_ms ? c->probe_ms : SC_PROBE_MS_DEFAULT;
    int mode = -1, v = SC_NO_FRAME, score = -1000, got = 0;
    char b1[16], b2[16];
    if (i >= SC_OFFS || !((g_alt_mask >> i) & 1u) || !g_ref.valid) {
        slog(e, "ptz: scene probe %u: no stored view: calibrating", i);
        return SC_NO_REF;
    }
    t_end = t0 + wait;
    for (;;) { /* the head has just stopped: wait the motion out, then for a few frames newer than the arrival */
        int64_t now = e->now_ms();
        if (e->cancelled && e->cancelled()) {
            v = SC_CANCELLED;
            break;
        }
        if (now - t0 > (int64_t)c->timeout_ms) {
            v = SC_NO_FRAME;
            break;
        }
        if (now >= t_end && e->frame(&g_cur, &seq) == 0) {
            if (!got) {
                got = 1;
                first_seq = seq;
            }
            frames = seq - first_seq + 1;
            mode = e->mode();
            if (frames >= (c->settle_frames + 1) / 2 && mode >= 0) {
                v = sc_decide(&g_alt[i], &g_cur, mode, c->min_permille, &score);
                break;
            }
        }
        e->sleep_ms(100);
    }
    if (got && v != SC_CANCELLED && v != SC_NO_FRAME)
        diag(e, c->min_permille, "probe", &g_alt[i].th, &g_cur, v);
    /* a repetitive home: the offset view proves the position only when the LIVE view is itself not repetitive (a displaced head can pass the gate on an offset view that a shift would match) */
    if (v == SC_MATCH && g_ref.rep == 1 && sc_repetitive(&g_cur, c->min_permille))
        v = SC_REPETITIVE;
    if (v == SC_MATCH) {
        g_ref.boots++;
        if (e->store && e->store(&g_ref) != 0) {
            slog(e, "ptz: scene probe: match, but the boot counter cannot be saved: calibrating");
            return SC_STORE_FAILED;
        }
        *pan = g_ref.pan;
        *tilt = g_ref.tilt;
        slog(e, "ptz: scene probe %u: MATCH, score %s >= %s: the head did not move (seen at the offset), position %u/%u declared (boot %u of %d)", i,
             fmt3(b1, sizeof b1, score), fmt3(b2, sizeof b2, c->min_permille), (unsigned)g_ref.pan, (unsigned)g_ref.tilt, g_ref.boots, SC_FULL_EVERY);
    } else if (score > -1000) {
        slog(e, "ptz: scene probe %u: %s (score %s, needs %s)", i, sc_verdict_name(v), fmt3(b1, sizeof b1, score), fmt3(b2, sizeof b2, c->min_permille));
    } else {
        slog(e, "ptz: scene probe %u: %s", i, sc_verdict_name(v));
    }
    return v;
}
