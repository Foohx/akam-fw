/* Host unit test of the drift matcher (src/features/track/drift_match.c): synthetic scenes shifted by known sub-pixel amounts (procedural, so the shift is exact),
 * day against night transforms (inverted polarity, gain, gamma, different fine relief, sensor noise), a person / a furniture block in the view, flat and repeated
 * scenes, other scenes, bad frames; and, when the (git-ignored) corpus of real frames is present in DRIFT_CORPUS_DIR, the real day / night pairs of the 2K camera.
 * Prints the CPU time of one check. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "drift_match.h"

#define W 320
#define H 180
static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static dm_work_t wk;
static dm_ref_t ref;
static uint32_t rng = 12345;
static uint32_t rnd(void) { rng = rng * 1664525u + 1013904223u; return rng >> 8; }
static double frnd(void) { return (rnd() & 0xffff) / 65536.0 - 0.5; }

static double lat(int ix, int iy, int seed)
{
    uint32_t h = (uint32_t)ix * 374761393u + (uint32_t)iy * 668265263u + (uint32_t)seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return ((h ^ (h >> 16)) & 0xffff) / 65536.0 - 0.5;
}
static double vn(double x, double y, int seed)
{
    int ix = (int)floor(x), iy = (int)floor(y);
    double fx = x - ix, fy = y - iy, a, b;
    fx = fx * fx * (3 - 2 * fx);
    fy = fy * fy * (3 - 2 * fy);
    a = lat(ix, iy, seed) * (1 - fx) + lat(ix + 1, iy, seed) * fx;
    b = lat(ix, iy + 1, seed) * (1 - fx) + lat(ix + 1, iy + 1, seed) * fx;
    return a * (1 - fy) + b * fy;
}

/* the scene: smooth wall, plaster relief, a window with a frame, a pillar strip, a shelf edge; mode 0 day, 1 night (polarity of the geometry inverted, other relief
 * shading, other gain / gamma); seed picks the room. Evaluated at fractional positions: a sub-pixel shift is exact. */
static double scene(double x, double y, int seed, int mode)
{
    double v;
    double geo = 0, ox = x, oy = y;
    x += (seed - 1) * 41; /* another room: the same kind of furniture elsewhere */
    y += (seed - 1) * 23;
    v = 150 + 40 * vn(ox / 50, oy / 50, seed) + 30 * vn(ox / 14, oy / 14, seed + 1) + (mode ? 6 : 14) * vn(ox / 3.5, oy / 3.5, seed + (mode ? 77 : 2));
    if (x > 200 && x < 270 && y > 25 && y < 100) /* window glass */
        geo += 70;
    if ((x > 196 && x < 202 && y > 21 && y < 104) || (x > 268 && x < 274 && y > 21 && y < 104) || (y > 21 && y < 27 && x > 196 && x < 274) || (y > 98 && y < 104 && x > 196 && x < 274))
        geo -= 60; /* its frame */
    if (x > 58 && x < 70)
        geo -= 45; /* a pillar strip */
    if (y > 130 && x > 100)
        geo += 35; /* a shelf / floor edge */
    if (x > 110 && x < 150 && y > 40 && y < 75 && ((int)(x - 110) / 10 + (int)(y - 40) / 8) % 2 == 0)
        geo -= 30; /* a poster with blocks */
    if (mode)
        v = 90 + (v - 150) * 0.5 - geo * 0.6 + 0 * v; /* night: IR, the geometry inverts, less contrast */
    else
        v += geo;
    if (mode)
        v = 255.0 * pow(v < 0 ? 0 : v / 255.0, 0.8);
    return v;
}

static void render(uint8_t *out, int seed, int mode, double dx, double dy, double noise)
{
    int x, y;
    for (y = 0; y < H; y++)
        for (x = 0; x < W; x++) {
            double v = scene(x - dx, y - dy, seed, mode) + noise * frnd() * 3.4;
            out[y * W + x] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
        }
}

static void paste_person(uint8_t *f, int x0, int y0, int w, int h)
{
    int x, y;
    for (y = y0; y < y0 + h && y < H; y++)
        for (x = x0; x < x0 + w && x < W; x++)
            f[y * W + x] = (uint8_t)(60 + 30 * ((x / 5 + y / 7) % 3) + (rnd() & 15));
}

static double ab(double v) { return v < 0 ? -v : v; }

static int run(const uint8_t *live, dm_res_t *r)
{
    clock_t t = clock();
    dm_match(&ref, live, W, H, W, &wk, r);
    return (int)((clock() - t) * 1000000 / CLOCKS_PER_SEC);
}

static void synthetic(void)
{
    static uint8_t a[W * H], b[W * H];
    static const double sh[][2] = { { 0, 0 }, { 3, 0 }, { 0, -6 }, { 5.5, 7.25 }, { -12, 9 }, { 20, -15.5 }, { -30, 25 }, { 40, -20 } };
    unsigned i;
    dm_res_t r;
    int us, n;
    printf("== synthetic scenes\n");
    render(a, 1, 0, 0, 0, 1);
    n = dm_ref_build(a, W, H, W, &ref, &wk);
    CHECK(n >= 8 && ref.valid && ref.quads >= 2, "the reference of the synthetic room has patches (%d, quads %d)", n, ref.quads);
    for (i = 0; i < sizeof sh / sizeof sh[0]; i++) {
        rng = 99 + i;
        render(b, 1, 0, sh[i][0], sh[i][1], 1);
        us = run(b, &r);
        CHECK(r.verdict == DM_OK, "day shift %.2f,%.2f found (%s)", sh[i][0], sh[i][1], r.why);
        CHECK(r.verdict == DM_OK && ab(r.dx_q / 4.0 - sh[i][0]) <= 0.75 && ab(r.dy_q / 4.0 - sh[i][1]) <= 0.75, "day shift %.2f,%.2f measured %.2f,%.2f within 0.75 px", sh[i][0], sh[i][1], r.dx_q / 4.0, r.dy_q / 4.0);
        if (i == 3)
            printf("   one check: %d us on this PC, %d valid of %d patches, %d inliers, fine=%d\n", us, r.nvalid, r.npatch, r.ninl, r.fine);
    }
    /* zero false correction: the unchanged view, many noise draws, a gain change */
    for (i = 0; i < 20; i++) {
        rng = 500 + i;
        render(b, 1, 0, 0, 0, 1 + i % 4);
        run(b, &r);
        CHECK(r.verdict == DM_OK && r.dx_q == 0 && r.dy_q == 0, "unchanged view, noise draw %u: reads zero (%s, %d,%d)", i, r.why, r.dx_q, r.dy_q);
    }
    /* capture acceptance: a person in a few patches of a static view is still a static view, a real move is not (hardware 2026-11-19: 0,0 with 833 permille agreeing was refused) */
    for (i = 0; i < 4; i++) {
        static const int pw[] = { 40, 70, 100, 130 };
        rng = 900 + i;
        render(b, 1, 0, 0, 0, 1);
        paste_person(b, 110, 40, pw[i], 120);
        run(b, &r);
        printf("   person %d px wide: %s, %d,%d, conf %d (%d of %d patches)\n", pw[i], r.why, r.dx_q, r.dy_q, r.conf, r.ninl, r.nvalid);
        if (r.verdict == DM_OK && r.conf >= 750 && r.conf < 900)
            CHECK(dm_static_ok(&r), "a static view with a person in some patches (%d permille agree) is accepted for the capture", r.conf);
        if (r.verdict == DM_OK && r.conf >= 900)
            CHECK(dm_static_ok(&r), "a static view (%d permille agree) is accepted for the capture", r.conf);
    }
    { dm_res_t q; memset(&q, 0, sizeof q); q.verdict = DM_OK; q.conf = 833; q.ninl = 10; q.nquad = 3;
      CHECK(dm_static_ok(&q), "0,0 with 833 permille on 3 quadrants is a static view");
      q.dx_q = 3; CHECK(!dm_static_ok(&q), "a displacement of 0.75 px is not static");
      q.dx_q = 0; q.conf = 700; CHECK(!dm_static_ok(&q), "700 permille is too few");
      q.conf = 833; q.nquad = 1; CHECK(!dm_static_ok(&q), "agreement on one quadrant only is not enough");
      q.nquad = 3; q.ninl = 5; CHECK(!dm_static_ok(&q), "5 agreeing patches are too few for 75..90 %%");
      q.ninl = 10; q.verdict = DM_INCONC; CHECK(!dm_static_ok(&q), "no consensus is not static"); }
    for (i = 0; i < 4; i++) { /* a real move of the view, with a person in it */
        rng = 950 + i;
        render(b, 1, 0, 2.5 + i, -1.5 * i, 1);
        paste_person(b, 110, 40, 70, 120);
        run(b, &r);
        CHECK(!dm_static_ok(&r), "a view moved by %.1f,%.1f px with a person in it is not accepted for the capture", 2.5 + i, -1.5 * i);
    }
    /* day reference against the night view: the one reference works in both modes */
    printf("== day reference, night live view\n");
    for (i = 0; i < 6; i++) {
        double dx = sh[i + 1][0], dy = sh[i + 1][1];
        rng = 700 + i;
        render(b, 1, 1, dx, dy, 2);
        run(b, &r);
        CHECK(r.verdict == DM_OK, "night view shifted %.2f,%.2f found (%s)", dx, dy, r.why);
        CHECK(r.verdict == DM_OK && ab(r.dx_q / 4.0 - dx) <= 2.0 && ab(r.dy_q / 4.0 - dy) <= 2.0, "night shift %.2f,%.2f measured %.2f,%.2f within 2 px", dx, dy, r.dx_q / 4.0, r.dy_q / 4.0);
    }
    rng = 800;
    render(b, 1, 1, 0, 0, 2);
    run(b, &r);
    CHECK(r.verdict == DM_OK && ab(r.dx_q / 4.0) <= 1.5 && ab(r.dy_q / 4.0) <= 1.5, "night view at home reads zero within 1.5 px (%s, %.2f,%.2f)", r.why, r.dx_q / 4.0, r.dy_q / 4.0);
    /* a night reference against the day view */
    render(a, 1, 1, 0, 0, 2);
    dm_ref_build(a, W, H, W, &ref, &wk);
    render(b, 1, 0, 5, -8, 1);
    run(b, &r);
    CHECK(r.verdict == DM_OK && ab(r.dx_q / 4.0 - 5) <= 2.0 && ab(r.dy_q / 4.0 + 8) <= 2.0, "night reference, day view shifted 5,-8: %s %.2f,%.2f", r.why, r.dx_q / 4.0, r.dy_q / 4.0);
    /* people and furniture: outliers are rejected, the rest votes; a covered view is refused, never a wrong number */
    printf("== a person in the patches\n");
    render(a, 1, 0, 0, 0, 1);
    dm_ref_build(a, W, H, W, &ref, &wk);
    rng = 900;
    render(b, 1, 0, 6, -4, 1);
    paste_person(b, 180, 20, 60, 140);
    run(b, &r);
    CHECK(r.verdict == DM_OK && ab(r.dx_q / 4.0 - 6) <= 1 && ab(r.dy_q / 4.0 + 4) <= 1, "a person over some patches: shift 6,-4 still found (%s %.2f,%.2f)", r.why, r.dx_q / 4.0, r.dy_q / 4.0);
    render(b, 1, 0, 0, 0, 1);
    paste_person(b, 150, 0, 150, 180);
    run(b, &r);
    CHECK(!(r.verdict == DM_OK && (r.dx_q || r.dy_q)), "half the view covered: never a wrong displacement (%s %d,%d)", dm_verdict_name(r.verdict), r.dx_q, r.dy_q);
    render(b, 1, 0, 9, 9, 1);
    paste_person(b, 0, 0, 320, 180);
    run(b, &r);
    CHECK(r.verdict != DM_OK, "a covered view is refused (%s)", dm_verdict_name(r.verdict));
    /* another room, a flat view, a repeated pattern, bad sizes */
    printf("== refusals\n");
    render(b, 7, 0, 0, 0, 1);
    run(b, &r);
    CHECK(r.verdict != DM_OK, "another room is not a displacement (%s, %d,%d)", dm_verdict_name(r.verdict), r.dx_q, r.dy_q);
    memset(b, 128, sizeof b);
    run(b, &r);
    CHECK(r.verdict == DM_FLAT, "a flat live view is refused as flat (%s)", dm_verdict_name(r.verdict));
    {
        static dm_ref_t flat;
        memset(a, 90, sizeof a);
        CHECK(dm_ref_build(a, W, H, W, &flat, &wk) < DM_MIN_VALID && !flat.valid, "a flat frame yields no usable reference");
        dm_match(&flat, b, W, H, W, &wk, &r);
        CHECK(r.verdict == DM_NOREF, "matching against an unusable reference says NOREF");
        for (i = 0; i < W * H; i++) /* vertical stripes, period 12 */
            a[i] = ((i % W) / 6) % 2 ? 200 : 60;
        dm_ref_build(a, W, H, W, &flat, &wk);
        CHECK(!flat.valid, "a bare repeated pattern gives no usable reference (%d patches)", flat.n);
    }
    CHECK(dm_ref_build(a, 322, 180, 322, &ref, &wk) == -1, "a size that is not a multiple of 4 is a bad frame");
    CHECK(dm_ref_build(NULL, W, H, W, &ref, &wk) == -1, "no frame is a bad frame");
    render(a, 1, 0, 0, 0, 1);
    dm_ref_build(a, W, H, W, &ref, &wk);
    dm_match(&ref, b, 304, 176, 304, &wk, &r);
    CHECK(r.verdict == DM_BAD, "a live frame of another size is refused");
}

static uint8_t *corpus(const char *name)
{
#ifdef DRIFT_CORPUS_DIR
    char p[512];
    FILE *f;
    uint8_t *b = malloc(W * H);
    snprintf(p, sizeof p, "%s/%s.gray", DRIFT_CORPUS_DIR, name);
    f = fopen(p, "rb");
    if (!f || fread(b, 1, W * H, f) != (size_t)(W * H)) {
        if (f)
            fclose(f);
        free(b);
        return NULL;
    }
    fclose(f);
    return b;
#else
    (void)name;
    return NULL;
#endif
}

static int measure(const uint8_t *r0, const uint8_t *live, dm_res_t *r)
{
    dm_ref_build(r0, W, H, W, &ref, &wk);
    return run(live, r);
}

static void real(void)
{
    uint8_t *dbefore = corpus("day_before"), *drecal = corpus("day_recal"), *dhome = corpus("day_home"), *n0 = corpus("night_home0");
    static const char *const nh[] = { "night_dy_50_home", "night_dy_-50_home", "night_dx_-50_home", "night_dx_50_home", "night_dy_-100_home" };
    /* the shifts the brute-force ZNCC of the whole frame finds between night_home0 and the frame, 320x180 px (kept from the corpus run, rounded) */
    static const double ndx[] = { 0, 0, -13, 0, 0 }, ndy[] = { 4, 2, 2, 1, 1 };
    dm_res_t r;
    unsigned i;
    uint8_t *f;
    if (!dbefore || !drecal || !dhome || !n0) {
        printf("== real frames: SKIPPED (the git-ignored corpus is not in %s)\n", "tests/fixtures/drift");
        goto out;
    }
    printf("== real frames of the 2K camera\n");
    measure(dbefore, drecal, &r); /* the day case lost steps: the content is displaced 28 px up (7 px of the 80x48 thumbnail; brute-force ZNCC of the frame: 1,-28) */
    CHECK(r.verdict == DM_OK && ab(r.dx_q / 4.0 - 1) <= 2 && ab(r.dy_q / 4.0 + 28) <= 1.5, "day pair: displacement %.2f,%.2f (%s), expected 1,-28", r.dx_q / 4.0, r.dy_q / 4.0, r.why);
    measure(dbefore, dbefore, &r);
    CHECK(r.verdict == DM_OK && r.dx_q == 0 && r.dy_q == 0, "day view against itself: zero");
    measure(dbefore, dhome, &r);
    CHECK(r.verdict != DM_OK, "day_before against another scene is refused (%s)", dm_verdict_name(r.verdict));
    measure(dbefore, n0, &r);
    CHECK(r.verdict != DM_OK, "day_before against another scene at night is refused (%s)", dm_verdict_name(r.verdict));
    measure(n0, n0, &r);
    CHECK(r.verdict == DM_OK && r.dx_q == 0 && r.dy_q == 0, "night view against itself: zero");
    for (i = 0; i < sizeof nh / sizeof nh[0]; i++) {
        f = corpus(nh[i]);
        if (!f)
            continue;
        measure(n0, f, &r);
        CHECK(r.verdict == DM_OK && ab(r.dx_q / 4.0 - ndx[i]) <= 1.0 && ab(r.dy_q / 4.0 - ndy[i]) <= 1.0, "%s: %.2f,%.2f expected %.0f,%.0f within 1 px (%s)", nh[i], r.dx_q / 4.0, r.dy_q / 4.0, ndx[i], ndy[i], r.why);
        free(f);
    }
    f = corpus("night_dy_50");
    if (f) {
        measure(n0, f, &r);
        CHECK(r.verdict == DM_OK && ab(r.dy_q / 4.0 - 33) <= 1.5 && ab(r.dx_q / 4.0) <= 1.5, "night, tilt +50 steps: %.2f,%.2f (the thumbnail correlation of the same frames: 33 px)", r.dx_q / 4.0, r.dy_q / 4.0);
        free(f);
    }
    /* day reference, night views: relative to each other they must agree within 2 px with the night-only measurement (one reference, both modes) */
    {
        double bx = 0, by = 0;
        int have = 0;
        measure(dhome, n0, &r);
        if (r.verdict == DM_OK) {
            bx = r.dx_q / 4.0;
            by = r.dy_q / 4.0;
            have = 1;
        }
        CHECK(have, "day reference: the night home view is matched (%s)", r.why);
        for (i = 0; have && i < sizeof nh / sizeof nh[0]; i++) {
            f = corpus(nh[i]);
            if (!f)
                continue;
            measure(dhome, f, &r);
            if (r.verdict == DM_OK)
                CHECK(ab(r.dx_q / 4.0 - (bx + ndx[i])) <= 3.0 && ab(r.dy_q / 4.0 - (by + ndy[i])) <= 2.0, "day reference, %s: %.2f,%.2f vs %.2f,%.2f expected (cross-mode)", nh[i], r.dx_q / 4.0, r.dy_q / 4.0, bx + ndx[i], by + ndy[i]);
            else
                printf("   day reference, %s: %s (cross-mode may refuse, never guess)\n", nh[i], dm_verdict_name(r.verdict));
            free(f);
        }
    }
out:
    free(dbefore);
    free(drecal);
    free(dhome);
    free(n0);
}

int main(void)
{
    synthetic();
    real();
    printf("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
