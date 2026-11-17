/* Host unit test of the scene check (app/scene_check.c): the comparison on synthetic scenes (identical, shifted 1..2 px, rotated / other scene,
 * uniform, lighting gain), the serialized reference, the config parser and the boot decision with a fake clock, fake frames and a fake store.
 * The boot decision wired to the motor (no motion on a match, calibration otherwise) is tested in rot2k_hal_ptz_unit.c.
 * Prints "rot2k scene unit: N failed". */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "../firmware/lsc-rotatable-2k-ak3918av130/app/scene_check.h"

#ifndef FIXTURE_DIR
#define FIXTURE_DIR "tests/fixtures/scene"
#endif

static int fails;
#define CHECK(c, msg) do { if (c) printf("  ok   - %s\n", msg); else { printf("  FAIL - %s\n", msg); fails++; } } while (0)

#define CW 240
#define CH 144
static uint8_t canvas[CH][CW];
static unsigned rng;
static unsigned rnd(void) { rng = rng * 1664525u + 1013904223u; return rng >> 16; }

/* a blobby, textured scene: smoothed noise stretched to the full gray range */
static void make_scene(unsigned seed)
{
    static int a[CH][CW], b[CH][CW];
    int x, y, pass, lo = 1 << 30, hi = -1;
    rng = seed;
    for (y = 0; y < CH; y++)
        for (x = 0; x < CW; x++)
            a[y][x] = (int)(rnd() & 255);
    for (pass = 0; pass < 3; pass++) {
        for (y = 0; y < CH; y++)
            for (x = 0; x < CW; x++) {
                int s = 0, n = 0, dx, dy;
                for (dy = -1; dy <= 1; dy++)
                    for (dx = -1; dx <= 1; dx++)
                        if (y + dy >= 0 && y + dy < CH && x + dx >= 0 && x + dx < CW) {
                            s += a[y + dy][x + dx];
                            n++;
                        }
                b[y][x] = s / n;
            }
        memcpy(a, b, sizeof a);
    }
    for (y = 0; y < CH; y++)
        for (x = 0; x < CW; x++) {
            if (a[y][x] < lo) lo = a[y][x];
            if (a[y][x] > hi) hi = a[y][x];
        }
    for (y = 0; y < CH; y++)
        for (x = 0; x < CW; x++)
            canvas[y][x] = (uint8_t)((a[y][x] - lo) * 255 / (hi - lo));
}

/* the camera looks at canvas[oy..][ox..]; gain / offset model the lighting, noise the sensor */
static void view(sc_thumb_t *t, int ox, int oy, double gain, int off, int noise)
{
    int x, y;
    for (y = 0; y < SC_H; y++)
        for (x = 0; x < SC_W; x++) {
            int v = (int)(canvas[oy + y][ox + x] * gain) + off + (noise ? (int)(rnd() % (2 * noise + 1)) - noise : 0);
            t->px[y * SC_W + x] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
        }
}

static void test_compare(void)
{
    sc_thumb_t ref, cur;
    sc_ref_t r;
    int s;
    printf("== comparison\n");
    make_scene(7);
    view(&ref, 80, 40, 1.0, 0, 0);
    memset(&r, 0, sizeof r);
    r.valid = 1;
    r.th = ref;
    CHECK(sc_has_detail(&ref), "a textured scene has detail");
    view(&cur, 80, 40, 1.0, 0, 0);
    CHECK(sc_decide(&r, &cur, 0, SC_MIN_DEFAULT, &s) == SC_MATCH && s == 1000, "identical image: match, score 1.000");
    view(&cur, 80, 40, 1.0, 0, 2);
    CHECK(sc_decide(&r, &cur, 0, SC_MIN_DEFAULT, &s) == SC_MATCH && s >= 950, "sensor noise: match");
    view(&cur, 81, 40, 1.0, 0, 1);
    CHECK(sc_decide(&r, &cur, 0, SC_MIN_DEFAULT, &s) == SC_MATCH, "shifted by 1 px: match");
    view(&cur, 82, 41, 1.0, 0, 1);
    CHECK(sc_decide(&r, &cur, 0, SC_MIN_DEFAULT, &s) == SC_MATCH && s >= 900, "shifted by 2 px (both axes): match");
    view(&cur, 78, 38, 1.0, 0, 1);
    CHECK(sc_decide(&r, &cur, 0, SC_MIN_DEFAULT, &s) == SC_MATCH, "shifted by -2 px (both axes): match");
    view(&cur, 83, 40, 1.0, 0, 0);
    CHECK(sc_decide(&r, &cur, 0, SC_MIN_DEFAULT, &s) == SC_MISMATCH, "shifted by 3 px: beyond the tolerance (2 px), mismatch");
    view(&cur, 86, 40, 1.0, 0, 0);
    CHECK(sc_decide(&r, &cur, 0, SC_MIN_DEFAULT, &s) == SC_MISMATCH, "shifted by 6 px: beyond the search, mismatch");
    view(&cur, 92, 40, 1.0, 0, 1);
    CHECK(sc_decide(&r, &cur, 0, SC_MIN_DEFAULT, &s) == SC_MISMATCH && s < 500, "camera rotated (12 px pan): mismatch, low score");
    view(&cur, 80, 52, 1.0, 0, 1);
    CHECK(sc_decide(&r, &cur, 0, SC_MIN_DEFAULT, &s) == SC_MISMATCH, "camera tilted (12 px): mismatch");
    make_scene(99);
    view(&cur, 80, 40, 1.0, 0, 1);
    CHECK(sc_decide(&r, &cur, 0, SC_MIN_DEFAULT, &s) == SC_MISMATCH && s < 500, "a different scene: mismatch");
    make_scene(7);
    view(&cur, 80, 40, 0.6, 20, 1);
    CHECK(sc_decide(&r, &cur, 0, SC_MIN_DEFAULT, &s) == SC_MATCH && s >= 950, "global lighting change (gain 0.6, offset 20): ZNCC still matches");
    view(&cur, 80, 40, 1.0, 0, 1);
    {
        int x, y;
        for (y = 0; y < SC_H; y++)
            for (x = 0; x < SC_W / 2; x++)
                cur.px[y * SC_W + x] = (uint8_t)(cur.px[y * SC_W + x] / 4); /* a lamp off over half the room: documented limit, not a match */
    }
    CHECK(sc_decide(&r, &cur, 0, SC_MIN_DEFAULT, &s) == SC_MATCH, "a lighting change over half the image only: the structure still matches block by block (the old global ZNCC refused it)");
    view(&cur, 80, 40, 1.0, 0, 1);
    CHECK(sc_decide(&r, &cur, 1, SC_MIN_DEFAULT, &s) == SC_MODE_DIFF, "day/night mode differs: refused even for the same image");
    CHECK(sc_decide(&r, &cur, -1, SC_MIN_DEFAULT, &s) == SC_MODE_DIFF, "mode unknown: refused");
    view(&cur, 80, 40, 1.0, 0, 12);
    CHECK(sc_decide(&r, &cur, 0, SC_MIN_DEFAULT, &s) == SC_MATCH, "a very noisy copy (+-12): still a match (the noise is estimated and compensated)");
    {
        /* the same view with 30% of another scene mixed in (something changed in the room): a partial match, decided by the threshold */
        sc_thumb_t other;
        int i;
        make_scene(99);
        view(&other, 80, 40, 1.0, 0, 0);
        make_scene(7);
        view(&cur, 80, 40, 1.0, 0, 0);
        for (i = 0; i < SC_N; i++)
            cur.px[i] = (uint8_t)((cur.px[i] * 70 + other.px[i] * 30) / 100);
        CHECK(sc_decide(&r, &cur, 0, 800, &s) == SC_MATCH && sc_decide(&r, &cur, 0, 990, &s) == SC_MISMATCH, "strict threshold: a view with 30% of another scene mixed in matches at 0.80 but not at 0.99");
    }
    view(&cur, 80, 40, 1.0, 0, 1);
    r.valid = 0;
    CHECK(sc_decide(&r, &cur, 0, SC_MIN_DEFAULT, &s) == SC_NO_REF, "no reference");
}

/* a pattern fixed to the camera (lens dirt, an overlay, a sensor artefact): the same high-contrast texture in the same blocks of every image */
static void fixed_pattern(sc_thumb_t *t)
{
    int x, y;
    for (y = 0; y < SC_H; y++)
        for (x = 20; x < 60; x++) /* blocks columns 1 and 2: 6 of the 12 blocks */
            t->px[y * SC_W + x] = (uint8_t)((((x / 3) + (y / 3 + x / 7)) % 3) == 0 ? 5 : 250); /* blotches a few thumbnail px wide, not pixel noise */
}

static double gauss_(void);
static uint8_t clamp8(double v);
static void test_fixed_pattern(void)
{
    sc_thumb_t ref, cur, plain;
    sc_ref_t r;
    int s;
    printf("== camera-fixed pattern (per-block decision)\n");
    make_scene(7);
    view(&ref, 80, 40, 1.0, 0, 0);
    fixed_pattern(&ref);
    make_scene(99); /* ANOTHER scene behind the same fixed pattern */
    view(&cur, 80, 40, 1.0, 0, 1);
    fixed_pattern(&cur);
    memset(&r, 0, sizeof r);
    r.valid = 1;
    r.th = ref;
    s = sc_score(&ref, &cur);
    CHECK(s >= 900, "the fixed pattern gives a high score on the blocks it covers (a single global score would match)");
    {
        sc_cmp_t c;
        sc_compare(&ref, &cur, SC_MIN_DEFAULT, &c);
        CHECK(c.both >= 10 && c.matched * 3 < c.both * 2, "...but only the blocks it covers match on their own (less than 2/3 of the detailed blocks)");
    }
    CHECK(sc_decide(&r, &cur, 0, SC_MIN_DEFAULT, &s) != SC_MATCH, "another scene behind a fixed pattern: never a match (at most \"likely\", which only allows the short calibration and its proof)");
    make_scene(7);
    view(&cur, 80, 40, 1.0, 0, 1);
    fixed_pattern(&cur);
    CHECK(sc_decide(&r, &cur, 0, SC_MIN_DEFAULT, &s) == SC_MATCH, "the same scene behind the same fixed pattern still matches");
    /* a scene that vanishes while the fixed pattern stays (lens covered except the dirt, scene blocks flat now): the blocks detailed in the reference
     * only count AGAINST, they cannot be ignored (the old voting over the blocks detailed in both said match) */
    {
        int x, y;
        make_scene(7);
        view(&ref, 80, 40, 1.0, 0, 0);
        view(&cur, 80, 40, 1.0, 0, 0);
        for (y = 0; y < SC_H; y++)
            for (x = 0; x < SC_W; x++) {
                if (x >= 24 && x < 56 && y >= 4 && y < 28) { /* the camera-fixed pattern: inside 4 blocks (4 px clear of the others), in both */
                    uint8_t v = (uint8_t)((((x / 3) + (y / 3 + x / 7)) % 3) == 0 ? 5 : 250);
                    ref.px[y * SC_W + x] = v;
                    cur.px[y * SC_W + x] = v;
                } else {
                    cur.px[y * SC_W + x] = clamp8(128 + gauss_() * 1.0); /* the scene is gone: flat + noise */
                }
            }
        {
            sc_cmp_t c;
            sc_compare(&ref, &cur, SC_MIN_DEFAULT, &c);
            printf("  info: scene vanishes: ref %d cur %d both %d vote_n %d matched %d\n", c.ref_detail, c.cur_detail, c.both, c.vote_n, c.matched);
            memset(&r, 0, sizeof r);
            r.valid = 1;
            r.th = ref;
            CHECK(c.both >= SC_DETAIL_BLOCKS_MIN && c.matched * 3 >= c.both * 2, "(the 4 pattern blocks alone would match)");
            CHECK(sc_decide(&r, &cur, 0, SC_MIN_DEFAULT, &s) != SC_MATCH, "8 scene blocks + 4 pattern blocks in the reference, the pattern only now: refused (union vote)");
        }
    }
    make_scene(7);
    view(&plain, 80, 40, 1.0, 0, 0);
    CHECK(sc_blocks_ok(&plain, &plain, SC_MIN_DEFAULT) == SC_BX * SC_BY, "an ordinary textured scene: every block has detail and matches");
}

static void test_detail(void)
{
    sc_thumb_t u, g, n;
    sc_ref_t r;
    int i, s;
    printf("== low detail\n");
    memset(u.px, 128, sizeof u.px);
    CHECK(!sc_has_detail(&u), "a uniform image has no detail");
    rng = 3;
    for (i = 0; i < SC_N; i++)
        n.px[i] = (uint8_t)(128 + (int)(rnd() % 3) - 1);
    CHECK(!sc_has_detail(&n), "a uniform wall with sensor noise (+-1) has no detail");
    for (i = 0; i < SC_N; i++)
        g.px[i] = (uint8_t)(40 + (i % SC_W)); /* a smooth horizontal ramp: variance but no texture */
    CHECK(!sc_has_detail(&g), "a smooth gradient has no detail");
    memset(&r, 0, sizeof r);
    r.valid = 1;
    r.th = u;
    CHECK(sc_decide(&r, &u, 0, SC_MIN_DEFAULT, &s) == SC_LOW_DETAIL_REF && s == -1000, "uniform reference: rejected, never a match (even identical)");
    make_scene(7);
    view(&r.th, 80, 40, 1.0, 0, 0);
    CHECK(sc_decide(&r, &u, 0, SC_MIN_DEFAULT, &s) == SC_LOW_DETAIL_CUR, "uniform current image: rejected");
    CHECK(sc_decide(&r, &g, 0, SC_MIN_DEFAULT, &s) == SC_LOW_DETAIL_CUR, "smooth gradient current image: rejected");
    memset(u.px, 5, sizeof u.px); /* lens covered / dark night */
    CHECK(!sc_has_detail(&u), "a black image has no detail");
}

static void test_pack(void)
{
    static sc_ref_t r, o;
    static char buf[SC_FILE_MAX];
    size_t n;
    int i;
    printf("== serialized reference and parser\n");
    make_scene(5);
    view(&r.th, 60, 30, 1.0, 0, 0);
    r.valid = 1;
    r.mode = 1;
    r.pan = 1275;
    r.tilt = 1300;
    r.boots = 7;
    r.stamp = 1760000000u;
    n = sc_pack(&r, buf, sizeof buf);
    CHECK(n > SC_N && n < 5000 && n <= SC_FILE_MAX, "packed size below 5 KB (and the 4 KB store limit)");
    CHECK(sc_unpack(buf, n, &o) == 0 && o.valid && o.mode == 1 && o.pan == 1275 && o.tilt == 1300 && o.boots == 7 && o.stamp == 1760000000u && !memcmp(o.th.px, r.th.px, SC_N), "round trip (the write time stamp rides in the header: the rewrite rate limit needs no extra file)");
    {
        /* a file of the previous version: "... <boots>\n" without a stamp */
        char old[SC_FILE_MAX + 64];
        int h = snprintf(old, sizeof old, "scene1 1 1275 1300 %d %d 7\n", SC_W, SC_H);
        memcpy(old + h, r.th.px, SC_N);
        CHECK(sc_unpack(old, (size_t)h + SC_N, &o) == 0 && o.boots == 7 && o.stamp == 0 && o.rep == -1, "a reference without a stamp (older version): boots kept, stamp 0 (unknown), repetitive verdict unknown");
        h = snprintf(old, sizeof old, "scene1 1 1275 1300 %d %d 7 x\n", SC_W, SC_H);
        memcpy(old + h, r.th.px, SC_N);
        CHECK(sc_unpack(old, (size_t)h + SC_N, &o) == -1, "a garbage stamp: refused");
    }
    CHECK(!strncmp(buf, "scene2 ", 7), "the header is versioned: scene2 (carries the repetitive verdict)");
    r.rep = 0;
    r.repmin = 900;
    n = sc_pack(&r, buf, sizeof buf);
    CHECK(sc_unpack(buf, n, &o) == 0 && o.rep == 0 && o.repmin == 900, "scene2 round trip: repetitive verdict 0, screened at 0.900");
    r.rep = 1;
    r.repmin = 850;
    n = sc_pack(&r, buf, sizeof buf);
    CHECK(sc_unpack(buf, n, &o) == 0 && o.rep == 1 && o.repmin == 850, "scene2 round trip: repetitive verdict 1, screened at 0.850");
    r.rep = -1; /* unknown (a reference built without screening) */
    r.repmin = 0;
    n = sc_pack(&r, buf, sizeof buf);
    CHECK(sc_unpack(buf, n, &o) == 0 && o.rep == -1 && o.repmin == 0, "an unknown verdict stays unknown (screened at the boot)");
    buf[strchr(buf, '\n') - buf - 3] = '2'; /* "... <stamp> 2 0": not a verdict */
    CHECK(sc_unpack(buf, n, &o) == -1, "scene2 with a verdict other than 0 / 1: refused");
    r.rep = 0;
    r.repmin = 900;
    n = sc_pack(&r, buf, sizeof buf);
    {
        char cut[SC_FILE_MAX + 64];
        int h = (int)(strchr(buf, '\n') - buf);
        memcpy(cut, buf, (size_t)h - 6);
        cut[h - 6] = '\n'; /* scene2 header without the verdict fields */
        memcpy(cut + h - 5, buf + h + 1, SC_N);
        CHECK(sc_unpack(cut, (size_t)h - 5 + SC_N, &o) == -1, "scene2 without the verdict fields: refused");
    }
    {
        char low[SC_FILE_MAX + 64];
        int h = snprintf(low, sizeof low, "scene2 0 1275 1300 %d %d 0 0 0 500\n", SC_W, SC_H);
        memcpy(low + h, r.th.px, SC_N);
        CHECK(sc_unpack(low, (size_t)h + SC_N, &o) == -1, "scene2 screened below the lowest configurable threshold (0.500 < 0.800): refused");
        h = snprintf(low, sizeof low, "scene2 0 1275 1300 %d %d 0 0 0 800\n", SC_W, SC_H);
        memcpy(low + h, r.th.px, SC_N);
        CHECK(sc_unpack(low, (size_t)h + SC_N, &o) == 0 && o.repmin == 800, "...0.800 is the lowest accepted");
    }
    CHECK(sc_unpack(buf, n - 1, &o) == -1 && !o.valid, "truncated: refused");
    CHECK(sc_unpack(sc_invalid_marker, strlen(sc_invalid_marker), &o) == -1, "the invalid marker: refused");
    buf[0] = 'x';
    CHECK(sc_unpack(buf, n, &o) == -1, "bad magic: refused");
    buf[0] = 's';
    buf[7] = '2'; /* mode 2 */
    CHECK(sc_unpack(buf, n, &o) == -1, "bad mode: refused");
    buf[7] = '1';
    CHECK(sc_pack(&r, buf, 100) == 0, "a too small buffer: 0");
    for (i = 0; i < 8; i++)
        buf[i] = 0;
    CHECK(sc_unpack(buf, n, &o) == -1, "garbage header: refused");
    CHECK(sc_parse_min("0.90") == 900 && sc_parse_min("0.9") == 900 && sc_parse_min("0.95") == 950 && sc_parse_min("0.999") == 999 &&
          sc_parse_min("1") == 1000 && sc_parse_min("0.8") == 800, "ptz_scene_min parses to permille");
    CHECK(sc_parse_min("0.79") == -1 && sc_parse_min("0.5") == -1 && sc_parse_min("0.7") == -1 && sc_parse_min("0") == -1 && sc_parse_min("2") == -1 && sc_parse_min("abc") == -1 && sc_parse_min("0.") == -1 &&
          sc_parse_min("0.9x") == -1 && sc_parse_min("") == -1 && sc_parse_min(NULL) == -1 && sc_parse_min("-0.9") == -1, "bad values refused (below 0.80 too)");
}

/* ---- the boot decision: fake clock, store and frames ---- */
static struct {
    int64_t now;
    int have_ref, ref_load_calls;
    sc_ref_t ref;
    int have_alt[SC_OFFS];
    sc_ref_t alt[SC_OFFS];
    int frames_from_ms, mode, mode_after_ms, mode_after;
    sc_thumb_t th, dark;
    int dark_until_ms; /* before this time the frames are dark (AE not settled) */
    unsigned seq;
    int cancel_at_ms;
    int sleeps, nstore, store_fail, nstore_alt, dump_live;
    char dump_ref[700];
    char log[1024];
} F;

static int f_load(sc_ref_t *r) { F.ref_load_calls++; if (!F.have_ref) return -1; *r = F.ref; return 0; }
static int f_load_alt(unsigned i, sc_ref_t *r) { if (i >= SC_OFFS || !F.have_alt[i]) return -1; *r = F.alt[i]; return 0; }
static int f_store(const sc_ref_t *r) { F.nstore++; if (F.store_fail) return -1; F.ref = *r; return 0; }
static int f_frame(sc_thumb_t *t, unsigned *seq)
{
    if (F.now < F.frames_from_ms) return -1;
    *t = F.now < F.dark_until_ms ? F.dark : F.th;
    F.seq = (unsigned)(F.now / 100);
    *seq = F.seq;
    return 0;
}
static int f_mode(void) { return F.mode_after_ms && F.now >= F.mode_after_ms ? F.mode_after : F.mode; }
static int64_t f_now(void) { return F.now; }
static void f_sleep(unsigned ms) { F.now += ms; F.sleeps++; }
static int f_cancel(void) { return F.cancel_at_ms && F.now >= F.cancel_at_ms; }
static void f_log(const char *l) { strncat(F.log, l, sizeof F.log - strlen(F.log) - 2); strcat(F.log, "\n"); }
static int f_load_alt(unsigned i, sc_ref_t *r);
static int f_store_alt(unsigned i, const sc_ref_t *r) { if (i >= SC_OFFS) return -1; F.alt[i] = *r; F.nstore_alt++; return 0; }
static void f_dump(const char *name, const sc_thumb_t *t, const char *text) { (void)t; if (!strcmp(name, "ref.gray")) strncpy(F.dump_ref, text, sizeof F.dump_ref - 1); else F.dump_live++; }
static const sc_env_t ENV = { f_load, f_store, f_frame, f_mode, f_now, f_sleep, f_cancel, f_log, f_load_alt, f_store_alt, f_dump };
static const sc_cfg_t CFG = { SC_MIN_DEFAULT, 4000, 6, 30000, 1500 };
/* the boot check takes the HOME position in (the reference is keyed to home) */
#define BOOT() (pan = 1275, tilt = 1300, sc_boot_check(&ENV, &CFG, &pan, &tilt))

static void fresh(void)
{
    memset(&F, 0, sizeof F);
    make_scene(7);
    view(&F.ref.th, 80, 40, 1.0, 0, 0);
    F.ref.valid = 1;
    F.ref.mode = 0;
    F.ref.pan = 1275;
    F.ref.tilt = 1300;
    F.have_ref = 1;
    view(&F.th, 80, 40, 1.0, 0, 1);
    memset(F.dark.px, 3, sizeof F.dark.px);
}

static int load_file(const char *name, sc_thumb_t *t);
static void test_boot(void)
{
    uint32_t pan = 0, tilt = 0;
    printf("== boot decision\n");
    fresh();
    CHECK(BOOT() == SC_MATCH && pan == 1275 && tilt == 1300, "same scene: match, the saved position is returned");
    CHECK(F.dump_live == 1 && strstr(F.dump_ref, "boot:") && strstr(F.dump_ref, "verdict match") && strstr(F.dump_ref, "SAME"), "every boot check dumps the reference and live thumbnails with a diagnosis line (verdict, SAME blocks, shift, score)");
    CHECK(strstr(F.log, "MATCH") && strstr(F.log, "score "), "the score and the decision are logged");
    CHECK(F.now >= 4000, "...after waiting for the AE to settle (4 s)");
    fresh();
    F.have_ref = 0;
    CHECK(BOOT() == SC_NO_REF && F.sleeps == 0 && strstr(F.log, "calibrating"), "no saved reference: calibrate at once, no waiting");
    fresh();
    view(&F.th, 95, 40, 1.0, 0, 1);
    CHECK(BOOT() == SC_MISMATCH && strstr(F.log, "scene differs") && strstr(F.log, "calibrating"), "the head was turned: mismatch, calibrate, logged");
    fresh();
    memset(F.th.px, 100, sizeof F.th.px);
    CHECK(BOOT() == SC_LOW_DETAIL_CUR, "a uniform live view: low detail, calibrate");
    fresh();
    memset(F.ref.th.px, 100, sizeof F.ref.th.px);
    CHECK(BOOT() == SC_LOW_DETAIL_REF, "a uniform saved reference: low detail, calibrate");
    fresh();
    F.mode = 1;
    CHECK(BOOT() == SC_MODE_DIFF, "night now, day at capture: calibrate");
    fresh();
    F.frames_from_ms = 1000000;
    CHECK(BOOT() == SC_NO_FRAME && F.now >= 30000 && F.now < 31000, "no frame (camera error): timeout 30 s, calibrate");
    fresh();
    F.mode = -1;
    CHECK(BOOT() == SC_NO_FRAME, "day/night never known: timeout, calibrate");
    fresh();
    F.cancel_at_ms = 1500;
    CHECK(BOOT() == SC_CANCELLED && F.now < 2000, "shutdown during the wait: cancelled promptly");
    fresh();
    F.dark_until_ms = 3000; /* the AE is still converging: dark frames until 3 s */
    CHECK(BOOT() == SC_MATCH && F.now >= 4000, "dark frames while the AE converges are not compared: the settled one is");
    fresh();
    F.frames_from_ms = 8000; /* the first frame comes late: the settle time counts from it */
    CHECK(BOOT() == SC_MATCH && F.now >= 12000, "late first frame: settle counted from the first frame");
    /* the repetitive verdict is read from the reference: no screening at the boot */
    fresh();
    F.ref.rep = 0;
    F.ref.repmin = SC_MIN_DEFAULT;
    CHECK(BOOT() == SC_MATCH && strstr(F.log, "read from the reference") && !strstr(F.log, "screened now"), "a reference with its verdict (screened at capture): the boot reads it, no screening");
    fresh();
    F.ref.rep = 1;
    F.ref.repmin = SC_MIN_DEFAULT;
    CHECK(BOOT() == SC_REPETITIVE && F.nstore == 0, "a stored repetitive verdict and no usable offset view: refused (calibrate), nothing stored, whatever the image");
    {
        /* an ambiguous home view with stored, informative offset views: the home view never decides, the probe is requested (as for a featureless one) */
        int i, dp, dt;
        sc_thumb_t night;
        fresh();
        F.ref.rep = 1;
        F.ref.repmin = SC_MIN_DEFAULT;
        for (i = 0; i < SC_OFFS; i++) {
            int a2, b2;
            sc_offset((unsigned)i, &a2, &b2);
            make_scene(40 + (unsigned)i);
            view(&F.alt[i].th, 80, 40, 1.0, 0, 0);
            F.alt[i].valid = 1;
            F.alt[i].mode = 0;
            F.alt[i].rep = 0;
            F.alt[i].repmin = SC_MIN_DEFAULT;
            F.alt[i].pan = (uint32_t)(1275 + a2);
            F.alt[i].tilt = (uint32_t)(1300 + b2);
            F.have_alt[i] = 1;
        }
        CHECK(BOOT() == SC_NEED_PROBE && F.nstore == 0 && strstr(F.log, "multi-view"), "an ambiguous saved view with stored offset views: the probe is requested, nothing declared");
        CHECK(sc_probe_offset(0, &dp, &dt) == 0 && dp == SC_OFF_PAN, "the offset views are usable");
        /* F5: like the flat path, the probe needs a live frame in the same day / night mode as the reference, else the view changed: calibrate */
        F.mode = 1;
        F.log[0] = 0;
        CHECK(BOOT() == SC_REPETITIVE && strstr(F.log, "another day / night mode") && !strstr(F.log, "multi-view"), "ambiguous saved view, the live mode is another one (night now, day at capture): no probe, calibrate");
        F.mode = 0;
        F.frames_from_ms = 1000000;
        CHECK(BOOT() == SC_NO_FRAME, "ambiguous saved view and no frame: no probe request (the verdict is the missing frame)");
        F.frames_from_ms = 0;
        CHECK(BOOT() == SC_NEED_PROBE, "...and with the frame back in the same mode the probe is requested again");
        make_scene(41);
        view(&F.th, 80, 40, 1.0, 0, 1);
        F.now += 3000;
        CHECK(sc_probe_check(&ENV, &CFG, 1, &pan, &tilt) == SC_MATCH && pan == 1275 && tilt == 1300, "the live view at offset 1 matches its stored view: proven");
        make_scene(99);
        view(&F.th, 80, 40, 1.0, 0, 1);
        F.now += 3000;
        CHECK(sc_probe_check(&ENV, &CFG, 1, &pan, &tilt) != SC_MATCH, "a moved head (another view at the offset): refused");
        fresh();
        F.ref.rep = 1;
        F.ref.repmin = SC_MIN_DEFAULT;
        F.have_alt[0] = F.have_alt[1] = F.have_alt[2] = 0;
        CHECK(BOOT() == SC_REPETITIVE, "no usable offset view: refused, as before");
        fresh();
        F.ref.rep = 1;
        F.ref.repmin = SC_MIN_DEFAULT;
        for (i = 0; i < SC_OFFS; i++) {
            F.alt[i] = F.ref;
            F.alt[i].rep = 1; /* the offset view is itself repetitive: not usable */
            F.have_alt[i] = 1;
        }
        CHECK(BOOT() == SC_REPETITIVE, "offset views that are repetitive themselves are ignored");
        if (load_file("ptz_home_night_lamp.gray", &night) == 0)
            CHECK(sc_has_detail(&night) && !sc_repetitive(&night, SC_MIN_DEFAULT), "the real home view of the lamp night (fixture): informative, usable as a stored view");
    }
    fresh();
    F.ref.rep = -1;
    F.ref.repmin = 0;
    CHECK(BOOT() == SC_MATCH && strstr(F.log, "screened now") && F.nstore == 1 && F.ref.rep == 0 && F.ref.repmin == SC_MIN_DEFAULT, "an older file (verdict unknown): screened once, the match store writes the verdict: next boots read it");
    fresh();
    F.ref.rep = 0;
    F.ref.repmin = 950;
    CHECK(BOOT() == SC_MATCH && strstr(F.log, "screened now") && F.ref.repmin == SC_MIN_DEFAULT, "a verdict screened at a stricter threshold than the configured one (0.95 > 0.90) is not trusted: screened again");
    fresh();
    F.ref.rep = 0;
    F.ref.repmin = 800;
    CHECK(BOOT() == SC_MATCH && strstr(F.log, "screened now") && F.ref.repmin == SC_MIN_DEFAULT && F.ref.rep == 0, "a verdict screened at another threshold (0.80 != 0.90) is not trusted either: screened once, stored back at 0.90");
    {
        int x, y;
        fresh();
        for (y = 0; y < SC_H; y++)
            for (x = 0; x < SC_W; x++)
                F.ref.th.px[y * SC_W + x] = clamp8(128 + 50 * (((x % 8) < 4) ^ ((y % 8) < 4) ? 1 : -1) + gauss_());
        F.th = F.ref.th;
        F.ref.rep = -1;
        F.ref.repmin = 0;
        CHECK(BOOT() == SC_REPETITIVE && strstr(F.log, "repetitive"), "an older file whose view is a tile lattice: screened at the boot, refused");
        {
            int wx = 0, wy = 0, wk = 0;
            CHECK(sc_repetitive_why(&F.ref.th, SC_MIN_DEFAULT, &wx, &wy, &wk) == 1 && (wx || wy) && (wk == 1 || wk == 2), "the screening says why: the displacement and the kind of lobe that triggered it");
        }
    }
    fresh();
    F.mode = 1;
    F.ref.mode = 1;
    F.mode_after_ms = 2500;
    F.mode_after = 0;
    F.ref.mode = 0;
    CHECK(BOOT() == SC_MATCH && F.now >= 2500 + 4000, "a mode change restarts the settling (compared in the final mode)");
    fresh();
    view(&F.th, 80, 40, 0.5, 10, 1);
    CHECK(BOOT() == SC_MATCH, "a darker room (gain 0.5): still a match");
    /* a reference is trusted for SC_FULL_EVERY matched boots, then one full calibration */
    fresh();
    {
        int n, matched = 0, v = SC_MATCH;
        for (n = 0; n < 12 && v == SC_MATCH; n++) {
            F.now = 0;
            F.log[0] = 0;
            v = BOOT();
            if (v == SC_MATCH)
                matched++;
        }
        CHECK(matched == SC_FULL_EVERY && v == SC_PERIODIC && F.ref.boots == SC_FULL_EVERY, "every 10th matched boot: a full calibration (periodic), the counter lives in the reference");
        CHECK(strstr(F.log, "periodic") && F.now == 0, "...answered at once, without waiting for a frame");
        CHECK(F.nstore == SC_FULL_EVERY, "each matched boot rewrote the counter once");
    }
    fresh();
    F.ref.boots = 9;
    CHECK(BOOT() == SC_MATCH && F.ref.boots == 10, "the 10th match is still allowed and counts");
    fresh();
    F.store_fail = 1;
    CHECK(BOOT() == SC_STORE_FAILED && strstr(F.log, "calibrating"), "the counter cannot be saved: no match (a reference must not live forever), calibrate");
    fresh();
    F.ref.boots = 3;
    memset(F.th.px, 100, sizeof F.th.px);
    CHECK(BOOT() == SC_LOW_DETAIL_CUR && F.nstore <= 1 && F.ref.boots == 3, "a refused boot does not touch the counter (at most the screened verdict is stored back)");
}

/* ---- the REAL wall: tests/fixtures/scene/wall_80x48.gray is the 80x48 luma thumbnail (the one the app computes: box filter of the 320x192 detection
 * frame) of the home view of the Rotatable 2K: a close white plastered wall, a window edge at the right, a blue cloth at the bottom right
 * (1152x648 snapshot -> 320x192 area filter -> sc_downscale_y). The reference taken on this view was refused at commit by the old 8-of-12 rule
 * ("too few image blocks match on their own", score 0.991). The derived views are synthetic: a shift of whole thumbnail pixels (edge clamped), a
 * gain / offset, white noise, an IR-like vignette, a blur (the same wall without relief). */
static sc_thumb_t wall;

static int load_wall(void)
{
    FILE *f = fopen(FIXTURE_DIR "/wall_80x48.gray", "rb");
    size_t n;
    if (!f)
        return -1;
    n = fread(wall.px, 1, SC_N, f);
    fclose(f);
    return n == SC_N ? 0 : -1;
}

static double gauss_(void)
{
    return ((int)(rnd() & 1023) + (int)(rnd() & 1023) + (int)(rnd() & 1023) + (int)(rnd() & 1023) - 2046) / 1024.0 * 1.73; /* about N(0,1) */
}

static uint8_t clamp8(double v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v + 0.5); }

/* the wall seen with a shift (dx,dy thumbnail px, edge clamped), a gain / offset, white noise of std `sigma`, an optional radial vignette factor */
static void wview(sc_thumb_t *t, int dx, int dy, double gain, double off, double sigma, double vig)
{
    int x, y;
    for (y = 0; y < SC_H; y++)
        for (x = 0; x < SC_W; x++) {
            int sx = x + dx, sy = y + dy;
            double ddx = (x - SC_W / 2) / 46.0, ddy = (y - SC_H / 2) / 28.0, d2 = (ddx * ddx + ddy * ddy) / 2, v;
            sx = sx < 0 ? 0 : sx >= SC_W ? SC_W - 1 : sx;
            sy = sy < 0 ? 0 : sy >= SC_H ? SC_H - 1 : sy;
            v = wall.px[sy * SC_W + sx] * gain + off;
            v *= 1.0 - vig * d2;
            t->px[y * SC_W + x] = clamp8(v + (sigma > 0 ? gauss_() * sigma : 0));
        }
}

/* The same wall without any relief: the least-squares plane through the plaster area of the real thumbnail (no window, no cloth, no wall edge),
 * extended over the whole image: its real brightness and illumination gradient, none of its texture. */
static void blurred(sc_thumb_t *t)
{
    double sx = 0, sy = 0, sz = 0, sxx = 0, syy = 0, sxy = 0, sxz = 0, syz = 0, n = 0, det, a0, a1, a2;
    int x, y;
    for (y = 4; y < 40; y++)
        for (x = 14; x < 54; x++) {
            double z = wall.px[y * SC_W + x];
            n++; sx += x; sy += y; sz += z; sxx += (double)x * x; syy += (double)y * y; sxy += (double)x * y; sxz += x * z; syz += y * z;
        }
    /* normal equations of z = a0 + a1 x + a2 y, Cramer */
    det = n * (sxx * syy - sxy * sxy) - sx * (sx * syy - sxy * sy) + sy * (sx * sxy - sxx * sy);
    a0 = (sz * (sxx * syy - sxy * sxy) - sx * (sxz * syy - sxy * syz) + sy * (sxz * sxy - sxx * syz)) / det;
    a1 = (n * (sxz * syy - sxy * syz) - sz * (sx * syy - sxy * sy) + sy * (sx * syz - sxz * sy)) / det;
    a2 = (n * (sxx * syz - sxz * sxy) - sx * (sx * syz - sxz * sy) + sz * (sx * sxy - sxx * sy)) / det;
    for (y = 0; y < SC_H; y++)
        for (x = 0; x < SC_W; x++)
            t->px[y * SC_W + x] = clamp8(a0 + a1 * x + a2 * y);
}

static int verdict(const sc_thumb_t *ref, const sc_thumb_t *cur, int *score)
{
    static sc_ref_t r;
    int s = 0;
    r.valid = 1;
    r.th = *ref;
    r.mode = 0;
    return sc_decide(&r, cur, 0, SC_MIN_DEFAULT, score ? score : &s);
}

/* ---- illumination robustness: the real dusk pair + synthetic lighting changes. ----
 * tests/fixtures/scene/ptz_dusk_lamp_ref.gray is the home reference the camera wrote at dusk with a lamp on (the stored thumbnail of
 * /config/akam/ptz.scene), ptz_dusk_now.gray the thumbnail of the same view after a cold boot (head not moved) a few minutes later: the window got
 * darker, the lamp flattened the plaster relief. The old rule refused it ("too few of the detailed image blocks match", 5 of 12 blocks above 0.80
 * on their own: the plaster blocks, which are barely above the sensor noise, and the window / cloth blocks, which changed, voted against).
 * The now frame was rebuilt from a stream-1 snapshot (640x360 area filtered to 320x192, then sc_downscale_y), the reference is the stored one. */
static int load_file(const char *name, sc_thumb_t *t)
{
    char path[512];
    FILE *f;
    size_t n;
    snprintf(path, sizeof path, "%s/%s", FIXTURE_DIR, name);
    f = fopen(path, "rb");
    if (!f)
        return -1;
    n = fread(t->px, 1, SC_N, f);
    fclose(f);
    return n == SC_N ? 0 : -1;
}

/* the wall under a lamp: a bright blob (smooth, added), the window area (right quarter) scaled and clipped */
static void lamp_view(sc_thumb_t *t, double gain, double off, double lamp, double window, double relief, double sigma)
{
    sc_thumb_t soft;
    int x, y;
    blurred(&soft);
    for (y = 0; y < SC_H; y++)
        for (x = 0; x < SC_W; x++) {
            double d2 = ((x - 20) * (x - 20) + (y - 14) * (y - 14)) / 400.0, v = soft.px[y * SC_W + x] + relief * (wall.px[y * SC_W + x] - soft.px[y * SC_W + x]);
            v = v * gain + off + lamp * 60.0 / (1.0 + d2);
            if (x >= 3 * SC_W / 4)
                v *= window;
            t->px[y * SC_W + x] = clamp8(v + (sigma > 0 ? gauss_() * sigma : 0));
        }
}

static void test_lighting(void)
{
    sc_thumb_t ref, now, cur;
    sc_cmp_t c;
    int s, i, k, bad, tot;
    static const int shifts[][2] = { { 3, 0 }, { -3, 0 }, { 0, 3 }, { 0, -3 }, { 4, 2 }, { 6, 0 }, { 0, 6 }, { -9, 4 }, { 12, 0 }, { 0, 14 }, { 20, 8 } };
    printf("== illumination robustness (real dusk pair, lamp, window, IR-like flat light)\n");
    if (load_file("ptz_dusk_lamp_ref.gray", &ref) || load_file("ptz_dusk_now.gray", &now) || load_wall() != 0) {
        CHECK(0, "fixtures ptz_dusk_lamp_ref.gray / ptz_dusk_now.gray / wall_80x48.gray present");
        return;
    }
    sc_compare(&ref, &now, SC_MIN_DEFAULT, &c);
    printf("  info: real pair: both %d, vote %d, SAME %d, MOVED %d, neutral %d (weak %d, mean %d), doubt %d, shift %d,%d, score %d\n", c.both, c.vote_n, c.matched, c.moved, c.neutral,
           c.weak_n, c.weak_mean, c.doubt, c.shift_x, c.shift_y, c.score);
    CHECK(verdict(&ref, &now, &s) == SC_MATCH && s >= SC_MIN_DEFAULT - SC_MEAN_SLACK, "the real dusk pair (head not moved, window darker, lamp flattening the plaster): MATCH");
    CHECK(c.peak_ok && c.shift_x == 0 && c.shift_y == 0 && c.matched >= SC_SAME_BLOCKS_MIN && c.moved <= 1, "...the common displacement is zero, at least 4 blocks peak at zero shift, at most one block votes a displacement");
    CHECK(verdict(&now, &ref, &s) == SC_MATCH, "...and the other way round (the dusk view as the reference)");
    {
        static char dl[700];
        sc_diag_text(&ref, &now, SC_MIN_DEFAULT, "test", 1, dl, sizeof dl);
        CHECK(strstr(dl, "SAME 5") && strstr(dl, "shift 0,0") && strstr(dl, "repetitive 0") && strstr(dl, "det fff"), "the diagnosis line carries the block masks, the comparison and the repetitive verdict");
    }
    {
        /* a global gain / contrast change of the now frame (AE, WDR): the jump ratio is normalised by the median block-energy ratio */
        static const double gains[] = { 0.8, 0.9, 1.1, 1.25, 1.5 };
        int gi, x, nok = 0, nn = 0, nz = 0;
        for (gi = 0; gi < 5; gi++) {
            for (x = 0; x < SC_N; x++)
                cur.px[x] = clamp8(128 + gains[gi] * (now.px[x] - 128.0));
            nok += verdict(&ref, &cur, &s) == SC_MATCH;
        }
        CHECK(nok == 5, "the real dusk pair with a global gain of 0.8 .. 1.5 on the now frame (about the mid gray): still a match");
        rng = 77;
        for (gi = 0; gi < 100; gi++) {
            for (x = 0; x < SC_N; x++)
                cur.px[x] = clamp8(now.px[x] + 0.5 * gauss_());
            nn++;
            nz += verdict(&ref, &cur, &s) == SC_MATCH;
        }
        CHECK(nz == nn, "the real dusk pair + 0.5 gray levels of extra sensor noise: 100 of 100 match");
    }
    CHECK(!sc_repetitive(&ref, SC_MIN_DEFAULT) && !sc_repetitive(&now, SC_MIN_DEFAULT) && sc_has_detail(&ref), "...neither is a repetitive texture");
    {
        /* the screening runs once at capture on whatever frame the detection thread holds: sensor noise must not flag the home view (the main lobe of
         * a broad vertical structure has a gap at its edge in some draws: SC_REP_BRIDGE) */
        int fl = 0;
        rng = 5;
        for (k = 0; k < 60; k++) {
            for (i = 0; i < SC_N; i++)
                cur.px[i] = clamp8(ref.px[i] + 0.5 * gauss_());
            fl += sc_repetitive(&cur, SC_MIN_DEFAULT);
        }
        CHECK(fl == 0, "the real home view + 0.5 gray levels of noise, 60 draws: never flagged repetitive (was 12 of 60 before the lobe bridge)");
    }
    /* the real pair, moved: the head is turned by one thumbnail-pixel step beyond the tolerance or more (edge clamped), with the real lighting change on top */
    tot = bad = 0;
    for (i = 0; i < (int)(sizeof shifts / sizeof shifts[0]); i++) {
        int sx = shifts[i][0], sy = shifts[i][1], x, y;
        for (k = 0; k < 3; k++) {
            const sc_thumb_t *src = k == 2 ? &ref : &now;
            for (y = 0; y < SC_H; y++)
                for (x = 0; x < SC_W; x++) {
                    int px = x + sx, py = y + sy;
                    px = px < 0 ? 0 : px >= SC_W ? SC_W - 1 : px;
                    py = py < 0 ? 0 : py >= SC_H ? SC_H - 1 : py;
                    cur.px[y * SC_W + x] = clamp8(src->px[py * SC_W + px] * (k == 1 ? 0.8 : 1.0) + (k == 1 ? 12 : 0));
                }
            tot++;
            bad += verdict(&ref, &cur, &s) == SC_MATCH;
        }
    }
    CHECK(tot == 33 && bad == 0, "the real dusk pair with the head moved by 3..20 thumbnail px (also with a further gain / offset, and the reference against itself shifted): 0 of 33 accepted");
    for (k = 0; k < 4; k++) { /* mirrored, flipped, the other real view, a rotated sampling */
        int x, y;
        for (y = 0; y < SC_H; y++)
            for (x = 0; x < SC_W; x++)
                cur.px[y * SC_W + x] = k == 0 ? now.px[y * SC_W + SC_W - 1 - x] : k == 1 ? now.px[(SC_H - 1 - y) * SC_W + x] : k == 2 ? wall.px[y * SC_W + x] : now.px[x % SC_H * SC_W + y % SC_W];
        CHECK(verdict(&ref, &cur, &s) != SC_MATCH, k == 0 ? "the dusk view mirrored: refused" : k == 1 ? "the dusk view flipped: refused" : k == 2 ? "a pixel-shuffled copy: refused" : "a transposed sampling: refused");
    }
    /* synthetic lighting changes on the real wall (reference: the wall under flat daylight, noise 1.0) */
    rng = 31;
    lamp_view(&ref, 1.0, 0, 0.0, 1.0, 1.0, 1.0);
    CHECK(sc_has_detail(&ref), "the synthetic reference (the real wall) has detail");
    lamp_view(&cur, 1.0, 0, 0.0, 1.0, 1.0, 1.0);
    CHECK(verdict(&ref, &cur, &s) == SC_MATCH, "control: same view, other noise: match");
    lamp_view(&cur, 1.0, 0, 1.0, 1.0, 1.0, 1.0);
    CHECK(verdict(&ref, &cur, &s) == SC_MATCH, "a lamp switched on (a smooth bright blob): match");
    lamp_view(&cur, 1.0, 0, 0.0, 0.7, 1.0, 1.0);
    CHECK(verdict(&ref, &cur, &s) == SC_MATCH, "the window 30 % darker: match");
    lamp_view(&cur, 1.0, 0, 0.0, 1.3, 1.0, 1.0);
    /* relaxed from 1.6 when SC_SAME_BELOW went 0.25 -> 0.10 (fixed-pattern false accepts back to the old level): a window 60 % brighter clips and
     * its blocks stop correlating, the rest of the view no longer carries 4 SAME blocks: refused (calibrates), which is the safe side */
    CHECK(verdict(&ref, &cur, &s) == SC_MATCH, "the window 30 % brighter (clipped): match");
    lamp_view(&cur, 0.55, 18, 0.5, 0.8, 1.0, 1.0);
    CHECK(verdict(&ref, &cur, &s) == SC_MATCH, "gain 0.55, offset 18, a dimmer lamp, the window a little darker: match");
    lamp_view(&cur, 1.0, 0, 1.0, 1.0, 0.6, 1.0);
    CHECK(verdict(&ref, &cur, &s) == SC_MATCH, "a lamp that flattens the plaster relief to 60 %: match");
    lamp_view(&cur, 1.0, 0, 0.0, 1.0, 0.5, 1.2);
    CHECK(verdict(&ref, &cur, &s) == SC_MATCH, "IR-like flat light (half the relief, more noise): match");
    {
        sc_thumb_t ir;
        int x, y;
        for (y = 0; y < SC_H; y++) /* an IR-lit view: the radial vignette of the LEDs on top of the flattened relief */
            for (x = 0; x < SC_W; x++) {
                double ddx = (x - SC_W / 2) / 46.0, ddy = (y - SC_H / 2) / 28.0;
                ir.px[y * SC_W + x] = clamp8(cur.px[y * SC_W + x] * (1.0 - 0.45 * (ddx * ddx + ddy * ddy) / 2) + 20);
            }
        CHECK(verdict(&ref, &ir, &s) == SC_MATCH, "...with the IR vignette on top: match");
    }
    /* the same lighting changes with the head moved: still refused */
    tot = bad = 0;
    for (i = 0; i < (int)(sizeof shifts / sizeof shifts[0]); i++) {
        double g[3] = { 1.0, 0.55, 1.0 }, lp[3] = { 0.0, 0.5, 1.0 }, wd[3] = { 1.0, 0.7, 0.5 }, rl[3] = { 1.0, 1.0, 0.6 };
        for (k = 0; k < 3; k++) {
            wview(&cur, shifts[i][0], shifts[i][1], 1.0, 0, 0, 0); /* the shifted real wall, then the lighting on top */
            {
                sc_thumb_t sh = cur;
                int x, y;
                blurred(&cur);
                for (y = 0; y < SC_H; y++)
                    for (x = 0; x < SC_W; x++) {
                        double d2 = ((x - 20) * (x - 20) + (y - 14) * (y - 14)) / 400.0, v = cur.px[y * SC_W + x] + rl[k] * (sh.px[y * SC_W + x] - cur.px[y * SC_W + x]);
                        v = v * g[k] + lp[k] * 20.0 / (1.0 + d2) + 10 * k;
                        if (x >= 3 * SC_W / 4)
                            v *= wd[k];
                        cur.px[y * SC_W + x] = clamp8(v + gauss_());
                    }
            }
            tot++;
            bad += verdict(&ref, &cur, &s) == SC_MATCH;
        }
    }
    CHECK(tot == 33 && bad == 0, "lighting changes (lamp, window, gain, flat) AND the head moved by 3..20 px: 0 of 33 accepted");
    /* anti fixed-pattern: ANOTHER plaster wall (a relief of a few gray levels, unrelated) behind the same camera-fixed pattern is refused, the same
     * wall behind it is not. (Residual, measured in the harness of the notes: when the relief of the other wall is only just above the sensor noise,
     * the weak blocks are neutral and a pattern covering 4 to 6 blocks can carry the vote: the old rule had the same hole for a relief below the detail
     * threshold, and refused one block-width more of it. See ptz-findings.md.) */
    {
        sc_thumb_t a, b, plane;
        static double t1[SC_H + 4][SC_W + 4], t2[SC_H + 4][SC_W + 4];
        int x, y, n_ok = 0, n_same = 0;
        blurred(&plane);
        for (k = 0; k < 20; k++) {
            for (y = 0; y < SC_H + 4; y++)
                for (x = 0; x < SC_W + 4; x++) {
                    t1[y][x] = gauss_();
                    t2[y][x] = gauss_();
                }
            for (y = 0; y < SC_H; y++)
                for (x = 0; x < SC_W; x++) {
                    double pat = (x >= 8 && x < 56 && y >= 6 && y < 22) ? 60.0 * ((((x / 3) + (y / 3 + x / 7)) % 3) == 0 ? 1 : -1) : 0.0; /* 6 blocks of camera-fixed dirt */
                    double s1 = (t1[y][x] + t1[y + 1][x] + t1[y][x + 1] + t1[y + 1][x + 1]) * 0.8, s2 = (t2[y][x] + t2[y + 1][x] + t2[y][x + 1] + t2[y + 1][x + 1]) * 0.8;
                    a.px[y * SC_W + x] = clamp8(plane.px[y * SC_W + x] + pat + s1 + gauss_() * 0.6);
                    b.px[y * SC_W + x] = clamp8(plane.px[y * SC_W + x] + pat + s2 + gauss_() * 0.6);
                    cur.px[y * SC_W + x] = clamp8(plane.px[y * SC_W + x] + pat + s1 + gauss_() * 0.6);
                }
            if (k == 0) {
                sc_compare(&a, &b, SC_MIN_DEFAULT, &c);
                printf("  info: another wall behind the same pattern: both %d, SAME %d, neutral %d (weak %d, mean %d), vote %d\n", c.both, c.matched, c.neutral, c.weak_n, c.weak_mean, c.vote_n);
                sc_compare(&a, &cur, SC_MIN_DEFAULT, &c);
                printf("  info: the same wall behind it: both %d, SAME %d, neutral %d (weak %d, mean %d), vote %d\n", c.both, c.matched, c.neutral, c.weak_n, c.weak_mean, c.vote_n);
            }
            n_ok += verdict(&a, &b, &s) == SC_MATCH;
            n_same += verdict(&a, &cur, &s) == SC_MATCH;
        }
        CHECK(n_ok == 0, "another wall (relief a few gray levels) behind the same camera-fixed pattern (the pattern blocks agree, the wall blocks do not): 0 of 20 accepted");
        CHECK(n_same >= 18, "...the same plaster wall behind it still matches (at least 18 of 20)");
    }
}

static void test_real_wall(void)
{
    sc_thumb_t ref, cur, flat, other;
    sc_cmp_t c;
    int s, i, ok_noise = 0, ok_exp = 0, rej_shift3 = 0, rej_shift6 = 0;
    printf("== real wall frame (low contrast plaster, window edge, blue cloth)\n");
    if (load_wall() != 0) {
        CHECK(0, "fixture tests/fixtures/scene/wall_80x48.gray present");
        return;
    }
    rng = 11;
    wview(&ref, 0, 0, 1.0, 0, 1.5, 0);
    sc_compare(&ref, &ref, SC_MIN_DEFAULT, &c);
    printf("  info: detailed blocks of the wall reference: %d of %d\n", c.ref_detail, SC_BX * SC_BY);
    CHECK(sc_has_detail(&ref) && c.ref_detail >= SC_DETAIL_BLOCKS_MIN + 2, "the wall view is a usable reference (the old rule refused it at commit)");
    for (i = 0; i < 20; i++) { /* same view, other noise each time */
        wview(&cur, 0, 0, 1.0, 0, 1.5, 0);
        if (verdict(&ref, &cur, &s) == SC_MATCH)
            ok_noise++;
    }
    CHECK(ok_noise == 20, "same view + sensor noise (sigma 1.5 gray levels, 20 draws): always a match");
    wview(&cur, 0, 0, 1.0, 0, 1.5, 0);
    verdict(&ref, &cur, &s);
    printf("  info: score of the same view + noise: %d\n", s);
    for (i = 0; i < 20; i++) { /* mild exposure changes */
        wview(&cur, 0, 0, 0.85 + 0.015 * i, (i % 5) * 3 - 6, 1.5, 0);
        if (verdict(&ref, &cur, &s) == SC_MATCH)
            ok_exp++;
    }
    CHECK(ok_exp == 20, "mild exposure change (gain 0.85..1.15, offset -6..+6): always a match");
    wview(&cur, 0, 0, 1.0, 0, 1.5, 0);
    for (i = 0; i < SC_N; i++) { /* a lamp: the left part up to 25% brighter, a smooth falloff between x = 20 and 60 */
        int x = i % SC_W;
        double g = x < 20 ? 1.25 : x > 60 ? 1.0 : 1.25 - 0.25 * (x - 20) / 40.0;
        cur.px[i] = clamp8(cur.px[i] * g);
    }
    CHECK(verdict(&ref, &cur, &s) == SC_MATCH, "a lamp lighting the left part of the wall (smooth falloff): still a match (block by block)");
    wview(&cur, 1, 0, 1.0, 0, 1.5, 0);
    CHECK(verdict(&ref, &cur, &s) == SC_MATCH, "shifted by 1 px (about 4 source px, mechanical play): match");
    wview(&cur, 0, -2, 1.0, 0, 1.5, 0);
    CHECK(verdict(&ref, &cur, &s) == SC_MATCH, "shifted by 2 px vertically (the tolerance): match");
    for (i = 0; i < 4; i++) { /* the tolerance is 2 px: 3 px and more are refused, in every direction */
        static const int d[4][2] = { { 3, 0 }, { -3, 0 }, { 0, 3 }, { 0, -3 } };
        wview(&cur, d[i][0], d[i][1], 1.0, 0, 1.5, 0);
        if (verdict(&ref, &cur, &s) != SC_MATCH)
            rej_shift3++;
    }
    CHECK(rej_shift3 == 4, "shifted by 3 px (just beyond the tolerance), 4 directions: refused");
    for (i = 0; i < 4; i++) {
        static const int d[4][2] = { { 6, 0 }, { -6, 0 }, { 0, 6 }, { 0, -6 } };
        wview(&cur, d[i][0], d[i][1], 1.0, 0, 1.5, 0);
        if (verdict(&ref, &cur, &s) != SC_MATCH)
            rej_shift6++;
    }
    CHECK(rej_shift6 == 4, "shifted by 6 px: refused");
    wview(&cur, 10, 4, 1.0, 0, 1.5, 0);
    CHECK(verdict(&ref, &cur, &s) != SC_MATCH, "a head turned by 10 px / 4 px: refused");
    wview(&cur, 0, 0, 1.0, 0, 1.5, 0);
    for (i = 0; i < SC_N; i++) /* the wall mirrored: a different view with the same statistics */
        cur.px[i] = wall.px[(i / SC_W) * SC_W + (SC_W - 1 - i % SC_W)];
    CHECK(verdict(&ref, &cur, &s) != SC_MATCH, "the mirrored wall (another view, same statistics): refused");
    /* featureless views */
    blurred(&flat);
    for (i = 0; i < SC_N; i++)
        flat.px[i] = clamp8(flat.px[i] + gauss_() * 1.5);
    CHECK(!sc_has_detail(&flat), "the same wall without relief (the plane through its plaster) + noise: no detail, nothing to prove");
    CHECK(verdict(&flat, &flat, &s) == SC_LOW_DETAIL_REF && verdict(&ref, &flat, &s) == SC_LOW_DETAIL_CUR && verdict(&flat, &ref, &s) == SC_LOW_DETAIL_REF,
          "flat reference, flat current view, flat vs real: never a match");
    for (i = 0; i < SC_N; i++)
        other.px[i] = clamp8(150 + gauss_() * 2);
    CHECK(!sc_has_detail(&other) && verdict(&other, &other, &s) == SC_LOW_DETAIL_REF, "a fully flat image: no detail, refused");
    /* IR hot spot / vignetting: smooth, camera-fixed, correlates at every position */
    for (i = 0; i < SC_N; i++) {
        double dx = (i % SC_W - 40) / 46.0, dy = (i / SC_W - 24) / 28.0;
        other.px[i] = clamp8(200 - 120 * (dx * dx + dy * dy) / 2 + gauss_() * 1.5);
    }
    CHECK(!sc_has_detail(&other) && verdict(&other, &other, &s) != SC_MATCH, "an IR-like vignette alone (a smooth fixed pattern): no detail, refused");
    wview(&ref, 0, 0, 1.0, 0, 1.5, 0);
    wview(&cur, 0, 0, 1.0, 0, 1.5, 0.45);
    CHECK(verdict(&ref, &cur, &s) == SC_MATCH, "the wall + a vignette that appears (e.g. IR light): the relief still matches");
    wview(&ref, 0, 0, 1.0, 0, 1.5, 0.45);
    wview(&cur, 0, 0, 1.0, 0, 1.5, 0.45);
    CHECK(verdict(&ref, &cur, &s) == SC_MATCH, "wall + vignette against wall + vignette: match");
    /* the vignette is a fixed pattern: it must not make another view match */
    wview(&cur, 12, 7, 1.0, 0, 1.5, 0.45);
    CHECK(verdict(&ref, &cur, &s) != SC_MATCH, "wall + vignette, head turned: the vignette (fixed to the camera) does not vouch for the view");
    /* a camera-fixed blotch on a featureless scene covering a single block: not enough to prove anything */
    for (i = 0; i < SC_N; i++)
        flat.px[i] = clamp8(150 + gauss_() * 1.5);
    {
        int x, y;
        for (y = 5; y < 11; y++)
            for (x = 26; x < 34; x++)
                flat.px[y * SC_W + x] = (uint8_t)((((x / 3) + (y / 3 + x / 7)) % 3) == 0 ? 60 : 230); /* dirt on the lens */
    }
    CHECK(!sc_has_detail(&flat), "a lens dirt spot (8x6 px) on a featureless view: not enough to prove anything");
}

/* ---- multi-view: the boot decision with a featureless saved view and stored views at offsets ---- */
static void test_probe(void)
{
    uint32_t pan = 0, tilt = 0;
    int dp, dt, i;
    sc_thumb_t flat;
    printf("== multi-view fallback (featureless rest view, views at offsets)\n");
    CHECK(sc_offset(0, &dp, &dt) == 0 && dp == SC_OFF_PAN && dt == 0 && sc_offset(1, &dp, &dt) == 0 && dp == -SC_OFF_PAN && dt == 0 &&
          sc_offset(2, &dp, &dt) == 0 && dp == 0 && dt == -SC_OFF_TILT && sc_offset(3, &dp, &dt) == -1, "the offsets: pan +D, pan -D, tilt -D (never up), then none");
    /* the numbers of the risk analysis (ptz-findings.md): soft range 300..3650 / 300..2100, stops 0..3932+ / 0..2290+ */
    CHECK(300 - SC_OFF_PAN > 0 && 3932 - (3650 + SC_OFF_PAN) > 100 && 300 - SC_OFF_TILT > 100 && SC_OFF_PAN <= 200 && SC_OFF_TILT <= 200,
          "a move of D steps from ANY position of the soft range cannot reach a mechanical stop (margins >= 132 pan right, 150 pan left, 200 tilt bottom)");
    if (load_wall() != 0) {
        CHECK(0, "fixture present");
        return;
    }
    rng = 5;
    blurred(&flat);
    fresh();
    F.ref.th = flat; /* the saved rest view: featureless */
    F.ref.pan = 1275;
    F.ref.tilt = 1300;
    for (i = 0; i < SC_OFFS; i++) {
        int a, b;
        sc_offset((unsigned)i, &a, &b);
        wview(&F.alt[i].th, i * 7, i * 3, 1.0, 0, 1.5, 0); /* different "views" at the offsets */
        F.alt[i].valid = 1;
        F.alt[i].mode = 0;
        F.alt[i].pan = (uint32_t)(1275 + a);
        F.alt[i].tilt = (uint32_t)(1300 + b);
        F.have_alt[i] = 1;
    }
    F.th = flat; /* the live rest view is featureless too: undecidable alone -> the probe is requested */
    CHECK(BOOT() == SC_NEED_PROBE && F.nstore == 0 && strstr(F.log, "multi-view"), "featureless saved view + stored offset views: the probe is requested, nothing declared yet");
    CHECK(F.nstore_alt == SC_OFFS && F.alt[0].rep == 0 && F.alt[0].repmin == SC_MIN_DEFAULT, "legacy offset views were screened once and stored back with the verdict (not re-screened at every boot)");
    CHECK(sc_probe_offset(0, &dp, &dt) == 0 && dp == SC_OFF_PAN && sc_probe_offset(1, &dp, &dt) == 0 && sc_probe_offset(2, &dp, &dt) == 0 && sc_probe_offset(3, &dp, &dt) == -1, "the three stored views are usable");
    /* the head moved to offset 0: the live view = the stored one (+ noise): match, the REFERENCE position comes back, the counter is bumped */
    F.now = 0;
    F.log[0] = 0;
    wview(&F.th, 0, 0, 1.0, 0, 1.5, 0);
    wview(&F.alt[0].th, 0, 0, 1.0, 0, 1.5, 0);
    CHECK(sc_probe_check(&ENV, &CFG, 0, &pan, &tilt) == SC_MATCH && pan == 1275 && tilt == 1300 && F.ref.boots == 1 && F.nstore == 1 && strstr(F.log, "MATCH"),
          "the live view at offset 0 matches its stored view: the claimed position is proven (reference position returned, boots 1)");
    CHECK(F.now >= 1500, "...after the motion settled (probe_ms)");
    /* the head was not where the reference says: at the offset the live view is something else */
    F.now = 0;
    F.log[0] = 0;
    wview(&F.th, 14, 6, 1.0, 0, 1.5, 0);
    CHECK(sc_probe_check(&ENV, &CFG, 0, &pan, &tilt) != SC_MATCH && strstr(F.log, "probe 0"), "the live view at the offset is another view: refused (calibrate)");
    F.now = 0;
    F.th = flat;
    CHECK(sc_probe_check(&ENV, &CFG, 1, &pan, &tilt) != SC_MATCH, "a featureless live view at the offset: refused");
    CHECK(sc_probe_check(&ENV, &CFG, 5, &pan, &tilt) == SC_NO_REF, "an offset that was never stored: refused");
    /* alt views that do not belong to this reference are not used */
    F.now = 0;
    F.th = flat;
    F.alt[0].pan += 10;                /* another position */
    F.alt[1].mode = 1;                 /* another day / night mode */
    memset(F.alt[2].th.px, 100, SC_N); /* a featureless stored view */
    CHECK(BOOT() == SC_LOW_DETAIL_REF && sc_probe_offset(0, &dp, &dt) == -1 && F.nstore == 1, "stored views of another position / mode / without detail are ignored: no probe, calibrate");
    /* a detailed saved view never probes */
    fresh();
    for (i = 0; i < SC_OFFS; i++)
        F.have_alt[i] = 1;
    CHECK(BOOT() == SC_MATCH, "a detailed saved view decides by itself");
    /* no stored views at all */
    fresh();
    F.ref.th = flat;
    F.th = flat;
    CHECK(BOOT() == SC_LOW_DETAIL_REF && strstr(F.log, "too uniform") && strstr(F.log, "calibrating"), "featureless saved view and nothing stored: \"too uniform\", calibrate");
    CHECK(!strstr(F.log, "fixed pattern"), "the old \"fixed pattern?\" wording is gone");
    /* L1: the probe only when the LIVE rest view is featureless too (and in the same mode) */
    fresh();
    F.ref.th = flat;
    F.ref.pan = 1275;
    F.ref.tilt = 1300;
    for (i = 0; i < SC_OFFS; i++) {
        int a, b;
        sc_offset((unsigned)i, &a, &b);
        wview(&F.alt[i].th, i * 7, i * 3, 1.0, 0, 1.5, 0);
        F.alt[i].valid = 1;
        F.alt[i].mode = 0;
        F.alt[i].pan = (uint32_t)(1275 + a);
        F.alt[i].tilt = (uint32_t)(1300 + b);
        F.have_alt[i] = 1;
    }
    wview(&F.th, 0, 0, 1.0, 0, 1.5, 0); /* the live view has detail: the view changed, nothing to probe */
    CHECK(BOOT() == SC_LOW_DETAIL_REF && sc_probe_offset(0, &dp, &dt) == -1, "featureless saved view but a DETAILED live view: no probe, calibrate");
    F.th = flat;
    F.now = 0;
    F.mode = 1; /* night now, day at capture */
    CHECK(BOOT() == SC_LOW_DETAIL_REF && sc_probe_offset(0, &dp, &dt) == -1, "another day / night mode: no probe, calibrate");
}

/* Fable review of 91d2262, F1: a repetitive home (bars, blinds, tiles) with a stored offset view that is NOT repetitive. A head that did not move shows the
 * stored view again; a head displaced by more than the tolerance may still pass the gate (the offset view shares the periodic texture), and the multi-view
 * check must then not declare the position: the LIVE offset view has to be non-repetitive too. Fuzz: three texture kinds, periods, unique content
 * strength / position, noise; every displaced live view (all offsets) at every case. */
#define FWW 400
#define FWH 48
static double FW_[FWH][FWW];
static unsigned frs = 1;
static double frnd(void) { frs = frs * 1103515245u + 12345u; return ((frs >> 8) & 0xffff) / 65535.0; }
static void fz_world(int kind, int p, int fx0, int fx1, int fstrength, unsigned seed)
{
    int x, y, k;
    frs = seed;
    for (y = 0; y < FWH; y++)
        for (x = 0; x < FWW; x++) {
            double v = 110;
            if (kind == 0) v += 50 * sin(2 * 3.14159265358979 * x / p);
            else if (kind == 1) v += 50 * sin(2 * 3.14159265358979 * y / p);
            else v += 35 * sin(2 * 3.14159265358979 * x / p) + 35 * sin(2 * 3.14159265358979 * y / p);
            FW_[y][x] = v;
        }
    for (k = 0; k < (fx1 - fx0) * 2 / 3 + 1; k++) {
        int cx = fx0 + (int)(frnd() * (fx1 - fx0)), cy = (int)(frnd() * FWH), r = 2 + (int)(frnd() * 4);
        double a = (frnd() - 0.5) * 2 * fstrength;
        for (y = cy - r; y <= cy + r; y++)
            for (x = cx - r; x <= cx + r; x++)
                if (y >= 0 && y < FWH && x >= fx0 && x < fx1 && x < FWW) FW_[y][x] += a;
    }
}
static void fz_view(sc_thumb_t *t, int x0, int noise)
{
    int x, y;
    for (y = 0; y < SC_H; y++)
        for (x = 0; x < SC_W; x++) {
            double v = FW_[y][x0 + x] + (frnd() - 0.5) * 2 * noise;
            t->px[y * SC_W + x] = v < 0 ? 0 : v > 255 ? 255 : (uint8_t)v;
        }
}
static void test_probe_repetitive_home(void)
{
    int kind, p, st, fstart, seed, side, d, cases = 0, rephome = 0, probed = 0, falsep = 0, truem = 0, truem_ok = 0;
    uint32_t pan, tilt;
    printf("== multi-view check on a repetitive home: no false proof from an offset view (F1)\n");
    for (kind = 0; kind < 3; kind++)
        for (p = 5; p <= 26; p += 7)
            for (st = 30; st <= 90; st += 30)
                for (fstart = -20; fstart <= 8; fstart += 7)
                    for (seed = 1; seed <= 2; seed++)
                        for (side = 0; side < 2; side++) {
                            int x0 = 160, off = side ? -12 : 12, any = 0;
                            sc_thumb_t home, alt;
                            if (side) fz_world(kind, p, x0 - 60 - fstart, x0 - fstart, st, (unsigned)(seed * 7919 + kind * 31 + p));
                            else fz_world(kind, p, x0 + SC_W + fstart, x0 + SC_W + fstart + 60, st, (unsigned)(seed * 7919 + kind * 31 + p));
                            fz_view(&home, x0, 2);
                            cases++;
                            if (!sc_has_detail(&home) || !sc_repetitive(&home, SC_MIN_DEFAULT)) continue;
                            rephome++;
                            fz_view(&alt, x0 + off, 2);
                            if (!sc_has_detail(&alt) || sc_repetitive(&alt, SC_MIN_DEFAULT)) continue;
                            fresh();
                            F.ref.th = home;
                            F.ref.rep = -1;
                            F.alt[0].th = alt;
                            F.alt[0].valid = 1;
                            F.alt[0].mode = 0;
                            F.alt[0].pan = 1275 + SC_OFF_PAN;
                            F.alt[0].tilt = 1300;
                            F.alt[0].rep = 0;
                            F.alt[0].repmin = SC_MIN_DEFAULT;
                            F.have_alt[0] = 1;
                            F.th = home;
                            if (BOOT() != SC_NEED_PROBE) continue;
                            probed++;
                            for (d = -140; d <= 140; d += 4) {
                                int v;
                                fz_view(&F.th, x0 + off + d, 2);
                                F.now = 0;
                                F.log[0] = 0;
                                v = sc_probe_check(&ENV, &CFG, 0, &pan, &tilt);
                                if (d == 0) {
                                    truem++;
                                    truem_ok += v == SC_MATCH;
                                } else if (abs(d) > 4 && v == SC_MATCH) any = 1;
                            }
                            falsep += any;
                        }
    printf("  cases %d, repetitive home %d, probed %d, true matches %d/%d, configurations with a false proof %d\n", cases, rephome, probed, truem_ok, truem, falsep);
    CHECK(probed > 50, "the fuzz reached the probe on enough repetitive homes");
    CHECK(falsep == 0, "no displaced head is ever proven by an offset view (0 false proofs)");
    CHECK(truem_ok > truem * 9 / 10, "the head that did not move is still proven by its stored view (a non-repetitive live view)");
}

/* M1: a periodic texture (blinds, tiles, bars) aliases after a shift of a whole period: never a reference */
static void test_repetitive(void)
{
    int period, x, y, vert, nrep = 0, nbar = 0;
    sc_thumb_t t;
    printf("== repetitive textures\n");
    for (vert = 0; vert < 2; vert++)
        for (period = 5; period <= 10; period++) {
            rng = 3;
            for (y = 0; y < SC_H; y++)
                for (x = 0; x < SC_W; x++) {
                    int u = vert ? x : y;
                    t.px[y * SC_W + x] = clamp8(128 + 60 * ((u % period) < period / 2 ? 1 : -1) + gauss_() * 1.0);
                }
            if (sc_has_detail(&t))
                nbar++;
            if (sc_repetitive(&t, SC_MIN_DEFAULT))
                nrep++;
        }
    CHECK(nbar == 12, "bars of period 5..10 px, vertical and horizontal, are detailed views (so only the repetition check refuses them)");
    CHECK(nrep == 12, "...and all 12 are repetitive: no reference");
    /* a lattice (tiles) */
    for (y = 0; y < SC_H; y++)
        for (x = 0; x < SC_W; x++)
            t.px[y * SC_W + x] = clamp8(128 + 50 * (((x % 8) < 4) ^ ((y % 8) < 4) ? 1 : -1) + gauss_());
    CHECK(sc_has_detail(&t) && sc_repetitive(&t, SC_MIN_DEFAULT), "a 8 px tile lattice: repetitive");
    CHECK(load_wall() == 0 && !sc_repetitive(&wall, SC_MIN_DEFAULT), "the real wall is not repetitive");
    make_scene(7);
    view(&t, 80, 40, 1.0, 0, 0);
    CHECK(!sc_repetitive(&t, SC_MIN_DEFAULT), "an ordinary textured synthetic scene is not repetitive");
    memset(t.px, 120, SC_N);
    CHECK(!sc_repetitive(&t, SC_MIN_DEFAULT), "a flat image: not 'repetitive' (it is 'too uniform')");
}


/* R1 / R2 / R8: the one-sided strongly detailed block, the repetitive screening of the saved reference, vertical edges, large periods, home keying */
static void test_residuals(void)
{
    uint32_t pan, tilt;
    sc_thumb_t t, u;
    int x, y, period, flagged = 0, total = 0, others = 0;
    printf("== home keying, reference screening, repetitive alias cases\n");
    fresh();
    pan = 1500;
    tilt = 900;
    CHECK(sc_boot_check(&ENV, &CFG, &pan, &tilt) == SC_NOT_HOME && F.nstore == 0 && strstr(F.log, "home is 1500/900"), "a reference taken at another position than HOME: calibrate (the claim is home only)");
    fresh();
    for (y = 0; y < SC_H; y++)
        for (x = 0; x < SC_W; x++)
            F.ref.th.px[y * SC_W + x] = clamp8(128 + 60 * ((x % 6) < 3 ? 1 : -1) + gauss_() * 1.0);
    F.th = F.ref.th;
    CHECK(BOOT() == SC_REPETITIVE && F.nstore == 1 && F.ref.rep == 1 && strstr(F.log, "repetitive"), "a repetitive reference already on the flash is refused at the boot check (calibrate) even when the live view matches it; its screened verdict is stored back once");
    /* a period beyond the old +-16 px range, a sine, in two thirds of the image (the rest an ordinary scene): flagged */
    for (period = 26; period <= 40; period += 2) {
        make_scene(11);
        for (y = 0; y < SC_H; y++)
            for (x = 0; x < SC_W; x++)
                t.px[y * SC_W + x] = y < 30 ? clamp8(120 + ((x % period) < period / 2 ? 25 : -25) + gauss_() * 1.5) : clamp8(120 + (canvas[y][x + 100] - 128) / 5 + gauss_() * 1.5);
        total++;
        flagged += sc_repetitive(&t, SC_MIN_DEFAULT);
    }
    CHECK(flagged == total, "square bars of period 26..40 px (beyond the former search range) in most of the view: all flagged");
    /* 1D invariance: vertical edges only (every row equal): the lobe is wider than SC_REP_LOBE_MAX, flagged */
    make_scene(13);
    for (y = 0; y < SC_H; y++)
        for (x = 0; x < SC_W; x++)
            t.px[y * SC_W + x] = clamp8(120 + (canvas[10][x + 40] - 128) + gauss_() * 1.0);
    CHECK(sc_has_detail(&t) && sc_repetitive(&t, SC_MIN_DEFAULT), "pure invariance along y (every row equal): its main lobe reaches beyond 8 px: flagged (conservative; the match refuses it anyway)");
    /* ordinary scenes: never flagged */
    for (period = 0; period < 20; period++) {
        make_scene(100 + (unsigned)period);
        view(&t, 40 + period, 20, 1.0, 0, 1);
        others += sc_repetitive(&t, SC_MIN_DEFAULT);
    }
    CHECK(others == 0, "20 ordinary textured scenes: none flagged");
    CHECK(load_wall() == 0 && !sc_repetitive(&wall, SC_MIN_DEFAULT) && !sc_repetitive(&wall, 800), "the real wall fixture: not flagged (also at the lowest configurable threshold)");
    /* M2: a noisier live view (sigma 2 against the clean reference) must still match: the strongly-detailed one-sided vote uses the larger noise */
    if (load_wall() == 0) {
        sc_ref_t rr;
        int k, ok = 0, sc;
        memset(&rr, 0, sizeof rr);
        rr.valid = 1;
        rr.th = wall;
        for (k = 0; k < 40; k++) {
            for (x = 0; x < SC_N; x++)
                u.px[x] = clamp8(wall.px[x] + gauss_() * 2.0);
            ok += sc_decide(&rr, &u, 0, SC_MIN_DEFAULT, &sc) == SC_MATCH;
        }
        CHECK(ok >= 36, "the real wall against itself + noise (sigma 2): still a match (at least 36 of 40)");
    }
    /* R1: a block STRONGLY detailed on one side counts against the match whatever the other side shows there (it was ignored in the borderline
     * band, 1.5 to 3 x the noise, of the other side: a scene present in one image and nearly gone in the other must not be waved through) */
    {
        sc_cmp_t c;
        int k, tested = 0, bad = 0;
        make_scene(21);
        view(&t, 80, 40, 1.0, 0, 1);
        for (k = 4; k <= 40; k += 2) {
            u = t;
            for (y = 0; y < 20; y++) /* beyond the block + the 3 px band margin: no artificial edge inside the block */
                for (x = 0; x < 24; x++)
                    u.px[y * SC_W + x] = clamp8(128 + (t.px[y * SC_W + x] - 128) * k / 100.0);
            sc_compare(&t, &u, SC_MIN_DEFAULT, &c);
            if (c.cur_detail < c.ref_detail) { /* the damped block fell out of the detailed set */
                tested++;
                if (c.vote_n != c.both + 1)
                    bad++;
            }
        }
        CHECK(tested >= 3 && bad == 0, "a strongly detailed reference block that the live view damped below detail (flat or borderline) counts against, at every damping");
    }
}

/* changed blocks (people, furniture) next to a head that did not move: displacement evidence first */
static void test_half_same(void)
{
    sc_thumb_t ref, cur, other;
    sc_ref_t r;
    int s, x, y, nrep;
    printf("== zero shift, no moved block, matching blocks: SC_LIKELY, never a proof (the relaxed rule only leads to the short calibration)\n");
    make_scene(7);
    view(&ref, 80, 40, 1.0, 0, 0);
    memset(&r, 0, sizeof r);
    r.valid = 1;
    r.th = ref;
    make_scene(99);
    view(&other, 80, 40, 1.0, 0, 1);
    for (nrep = 3; nrep <= 6; nrep++) { /* 3 .. 6 of the 12 blocks replaced by another scene (a person in front of the view) */
        sc_cmp_t c;
        make_scene(7);
        view(&cur, 80, 40, 1.0, 0, 1);
        for (y = 0; y < SC_H; y++)
            for (x = 0; x < SC_W; x++) {
                int bx = x / (SC_W / SC_BX), by = y / (SC_H / SC_BY), b = by * SC_BX + bx;
                if (b < nrep)
                    cur.px[y * SC_W + x] = other.px[y * SC_W + x];
            }
        sc_compare(&ref, &cur, SC_MIN_DEFAULT, &c);
        printf("   %d blocks replaced: matched %d of %d, moved %d, shift %d/%d, decision %d\n", nrep, c.matched, c.both, c.moved, c.shift_x, c.shift_y, sc_decide(&r, &cur, 0, SC_MIN_DEFAULT, &s));
        if (nrep <= 4)
            CHECK(sc_decide(&r, &cur, 0, SC_MIN_DEFAULT, &s) == SC_MATCH, "up to 4 blocks replaced: the strict 2/3 vote still proves it (unchanged)");
        else
            CHECK(sc_decide(&r, &cur, 0, SC_MIN_DEFAULT, &s) == SC_LIKELY, "5 or 6 blocks replaced: NOT a match (the strict vote refuses), only \"very likely home\" (short calibration, then the strict proof)");
    }
    /* a moved head is still refused, with or without changed blocks */
    for (nrep = 0; nrep <= 4; nrep += 4) {
        make_scene(7);
        view(&cur, 83, 40, 1.0, 0, 1); /* 3 px: beyond the tolerance */
        for (y = 0; y < SC_H; y++)
            for (x = 0; x < SC_W; x++) {
                int bx = x / (SC_W / SC_BX), by = y / (SC_H / SC_BY), b = by * SC_BX + bx;
                if (b < nrep)
                    cur.px[y * SC_W + x] = other.px[y * SC_W + x];
            }
        CHECK(sc_decide(&r, &cur, 0, SC_MIN_DEFAULT, &s) != SC_MATCH && sc_decide(&r, &cur, 0, SC_MIN_DEFAULT, &s) != SC_LIKELY, "a head moved by 3 px is neither a match nor likely, whatever else changed (fail closed)");
    }
    make_scene(99);
    view(&cur, 80, 40, 1.0, 0, 1);
    CHECK(sc_decide(&r, &cur, 0, SC_MIN_DEFAULT, &s) != SC_MATCH, "another scene entirely is refused");
}

int main(void)
{
    test_compare();
    test_fixed_pattern();
    test_half_same();
    test_detail();
    test_pack();
    test_boot();
    test_real_wall();
    test_lighting();
    test_probe();
    test_probe_repetitive_home();
    test_repetitive();
    test_residuals();
    printf("rot2k scene unit: %d failed\n", fails);
    return fails ? 1 : 0;
}
