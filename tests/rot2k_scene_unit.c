/* Host unit test of the scene check (app/scene_check.c): the comparison on synthetic scenes (identical, shifted 1..2 px, rotated / other scene,
 * uniform, lighting gain), the serialized reference, the config parser and the boot decision with a fake clock, fake frames and a fake store.
 * The boot decision wired to the motor (no motion on a match, calibration otherwise) is tested in rot2k_hal_ptz_unit.c.
 * Prints "rot2k scene unit: N failed". */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../firmware/lsc-rotatable-2k-ak3918av130/app/scene_check.h"

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
    CHECK(sc_decide(&r, &cur, 0, SC_MIN_DEFAULT, &s) == SC_MISMATCH && s < 900, "shifted by 3 px: beyond the search, mismatch");
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
    CHECK(sc_decide(&r, &cur, 0, SC_MIN_DEFAULT, &s) == SC_MISMATCH, "a lighting change over half the image only: not trusted (documented limit)");
    view(&cur, 80, 40, 1.0, 0, 1);
    CHECK(sc_decide(&r, &cur, 1, SC_MIN_DEFAULT, &s) == SC_MODE_DIFF, "day/night mode differs: refused even for the same image");
    CHECK(sc_decide(&r, &cur, -1, SC_MIN_DEFAULT, &s) == SC_MODE_DIFF, "mode unknown: refused");
    view(&cur, 80, 40, 1.0, 0, 12);
    CHECK(sc_decide(&r, &cur, 0, SC_MIN_DEFAULT, &s) == SC_MATCH && sc_decide(&r, &cur, 0, 1000, &s) == SC_MISMATCH, "strict threshold 1.000: a very noisy copy matches at 0.90 but not at 1.000");
    r.valid = 0;
    CHECK(sc_decide(&r, &cur, 0, SC_MIN_DEFAULT, &s) == SC_NO_REF, "no reference");
}

/* a pattern fixed to the camera (lens dirt, an overlay, a sensor artefact): the same high-contrast texture in the same blocks of every image */
static void fixed_pattern(sc_thumb_t *t)
{
    int x, y;
    for (y = 0; y < SC_H; y++)
        for (x = 20; x < 60; x++) /* blocks columns 1 and 2: 6 of the 12 blocks */
            t->px[y * SC_W + x] = (uint8_t)(((x * 7 + y * 13) % 5) < 2 ? 5 : 250);
}

static void test_fixed_pattern(void)
{
    sc_thumb_t ref, cur, plain;
    sc_ref_t r;
    int s;
    printf("== camera-fixed pattern (per-block decision)\n");
    make_scene(7);
    view(&ref, 80, 40, 0.3, 60, 0);
    fixed_pattern(&ref);
    make_scene(99); /* ANOTHER scene behind the same fixed pattern */
    view(&cur, 80, 40, 0.3, 60, 1);
    fixed_pattern(&cur);
    memset(&r, 0, sizeof r);
    r.valid = 1;
    r.th = ref;
    s = sc_score(&ref, &cur);
    CHECK(s >= 900, "the fixed pattern alone gives a high GLOBAL score (the old single-score rule would match)");
    CHECK(sc_blocks_ok(&ref, &cur, SC_MIN_DEFAULT) < SC_BLOCKS_MIN, "...but only the blocks it covers match on their own");
    CHECK(sc_decide(&r, &cur, 0, SC_MIN_DEFAULT, &s) == SC_MISMATCH_BLOCKS, "another scene behind a fixed pattern: mismatch (blocks)");
    make_scene(7);
    view(&cur, 80, 40, 0.3, 60, 1);
    fixed_pattern(&cur);
    CHECK(sc_decide(&r, &cur, 0, SC_MIN_DEFAULT, &s) == SC_MATCH, "the same scene behind the same fixed pattern still matches");
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
    n = sc_pack(&r, buf, sizeof buf);
    CHECK(n > SC_N && n < 5000 && n <= SC_FILE_MAX, "packed size below 5 KB (and the 4 KB store limit)");
    CHECK(sc_unpack(buf, n, &o) == 0 && o.valid && o.mode == 1 && o.pan == 1275 && o.tilt == 1300 && o.boots == 7 && !memcmp(o.th.px, r.th.px, SC_N), "round trip");
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
    int frames_from_ms, mode, mode_after_ms, mode_after;
    sc_thumb_t th, dark;
    int dark_until_ms; /* before this time the frames are dark (AE not settled) */
    unsigned seq;
    int cancel_at_ms;
    int sleeps, nstore, store_fail;
    char log[1024];
} F;

static int f_load(sc_ref_t *r) { F.ref_load_calls++; if (!F.have_ref) return -1; *r = F.ref; return 0; }
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
static const sc_env_t ENV = { f_load, f_store, f_frame, f_mode, f_now, f_sleep, f_cancel, f_log };
static const sc_cfg_t CFG = { SC_MIN_DEFAULT, 4000, 6, 30000 };

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

static void test_boot(void)
{
    uint32_t pan = 0, tilt = 0;
    printf("== boot decision\n");
    fresh();
    CHECK(sc_boot_check(&ENV, &CFG, &pan, &tilt) == SC_MATCH && pan == 1275 && tilt == 1300, "same scene: match, the saved position is returned");
    CHECK(strstr(F.log, "MATCH") && strstr(F.log, "score "), "the score and the decision are logged");
    CHECK(F.now >= 4000, "...after waiting for the AE to settle (4 s)");
    fresh();
    F.have_ref = 0;
    CHECK(sc_boot_check(&ENV, &CFG, &pan, &tilt) == SC_NO_REF && F.sleeps == 0 && strstr(F.log, "calibrating"), "no saved reference: calibrate at once, no waiting");
    fresh();
    view(&F.th, 95, 40, 1.0, 0, 1);
    CHECK(sc_boot_check(&ENV, &CFG, &pan, &tilt) == SC_MISMATCH && strstr(F.log, "scene differs") && strstr(F.log, "calibrating"), "the head was turned: mismatch, calibrate, logged");
    fresh();
    memset(F.th.px, 100, sizeof F.th.px);
    CHECK(sc_boot_check(&ENV, &CFG, &pan, &tilt) == SC_LOW_DETAIL_CUR, "a uniform live view: low detail, calibrate");
    fresh();
    memset(F.ref.th.px, 100, sizeof F.ref.th.px);
    CHECK(sc_boot_check(&ENV, &CFG, &pan, &tilt) == SC_LOW_DETAIL_REF, "a uniform saved reference: low detail, calibrate");
    fresh();
    F.mode = 1;
    CHECK(sc_boot_check(&ENV, &CFG, &pan, &tilt) == SC_MODE_DIFF, "night now, day at capture: calibrate");
    fresh();
    F.frames_from_ms = 1000000;
    CHECK(sc_boot_check(&ENV, &CFG, &pan, &tilt) == SC_NO_FRAME && F.now >= 30000 && F.now < 31000, "no frame (camera error): timeout 30 s, calibrate");
    fresh();
    F.mode = -1;
    CHECK(sc_boot_check(&ENV, &CFG, &pan, &tilt) == SC_NO_FRAME, "day/night never known: timeout, calibrate");
    fresh();
    F.cancel_at_ms = 1500;
    CHECK(sc_boot_check(&ENV, &CFG, &pan, &tilt) == SC_CANCELLED && F.now < 2000, "shutdown during the wait: cancelled promptly");
    fresh();
    F.dark_until_ms = 3000; /* the AE is still converging: dark frames until 3 s */
    CHECK(sc_boot_check(&ENV, &CFG, &pan, &tilt) == SC_MATCH && F.now >= 4000, "dark frames while the AE converges are not compared: the settled one is");
    fresh();
    F.frames_from_ms = 8000; /* the first frame comes late: the settle time counts from it */
    CHECK(sc_boot_check(&ENV, &CFG, &pan, &tilt) == SC_MATCH && F.now >= 12000, "late first frame: settle counted from the first frame");
    fresh();
    F.mode = 1;
    F.ref.mode = 1;
    F.mode_after_ms = 2500;
    F.mode_after = 0;
    F.ref.mode = 0;
    CHECK(sc_boot_check(&ENV, &CFG, &pan, &tilt) == SC_MATCH && F.now >= 2500 + 4000, "a mode change restarts the settling (compared in the final mode)");
    fresh();
    view(&F.th, 80, 40, 0.5, 10, 1);
    CHECK(sc_boot_check(&ENV, &CFG, &pan, &tilt) == SC_MATCH, "a darker room (gain 0.5): still a match");
    /* a reference is trusted for SC_FULL_EVERY matched boots, then one full calibration */
    fresh();
    {
        int n, matched = 0, v = SC_MATCH;
        for (n = 0; n < 12 && v == SC_MATCH; n++) {
            F.now = 0;
            F.log[0] = 0;
            v = sc_boot_check(&ENV, &CFG, &pan, &tilt);
            if (v == SC_MATCH)
                matched++;
        }
        CHECK(matched == SC_FULL_EVERY && v == SC_PERIODIC && F.ref.boots == SC_FULL_EVERY, "every 10th matched boot: a full calibration (periodic), the counter lives in the reference");
        CHECK(strstr(F.log, "periodic") && F.now == 0, "...answered at once, without waiting for a frame");
        CHECK(F.nstore == SC_FULL_EVERY, "each matched boot rewrote the counter once");
    }
    fresh();
    F.ref.boots = 9;
    CHECK(sc_boot_check(&ENV, &CFG, &pan, &tilt) == SC_MATCH && F.ref.boots == 10, "the 10th match is still allowed and counts");
    fresh();
    F.store_fail = 1;
    CHECK(sc_boot_check(&ENV, &CFG, &pan, &tilt) == SC_STORE_FAILED && strstr(F.log, "calibrating"), "the counter cannot be saved: no match (a reference must not live forever), calibrate");
    fresh();
    F.ref.boots = 3;
    memset(F.th.px, 100, sizeof F.th.px);
    CHECK(sc_boot_check(&ENV, &CFG, &pan, &tilt) == SC_LOW_DETAIL_CUR && F.nstore == 0 && F.ref.boots == 3, "a refused boot does not touch the counter");
}

int main(void)
{
    test_compare();
    test_fixed_pattern();
    test_detail();
    test_pack();
    test_boot();
    printf("rot2k scene unit: %d failed\n", fails);
    return fails ? 1 : 0;
}
