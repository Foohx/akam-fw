/* Host unit test of the image orientation (flip / mirror) of the Rotatable 2K app and of every consumer that must follow it: the pure mappings and the state
 * (orient.c), the detection zones (zones.c: stored in the sensor frame, matched in the displayed frame), the live zone store and its text entities (zlive.c),
 * the tracker's motor direction (track_logic.c with the flags orient.c computes), and the plane conversion the scene / drift references use. The manual PTZ
 * directions are in rot2k_ptz_cont_unit.c (they need the fake driver), the settings rows in rot2k_control_api_http_test.sh.
 * Usage: rot2k_orient_unit <scratch dir>. Prints "rot2k orient unit: N failed". */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "orient.h"
#include "store.h"
#include "track_logic.h"
#include "zlive.h"
#include "zones.h"

static int fails;
#define CHECK(c, msg) do { if (c) printf("  ok   - %s\n", msg); else { printf("  FAIL - %s\n", msg); fails++; } } while (0)

void a2k_log(const char *fmt, ...) { (void)fmt; }

static int seen_f = -1, seen_m = -1, seen_n;
static void listener(int f, int m) { seen_f = f; seen_m = m; seen_n++; }

static void test_state(void)
{
    int f, m, k, ok = 1;
    unsigned g0;
    printf("== state, listeners, settling window\n");
    a2k_orient_test_reset();
    a2k_orient_test_clock(1000);
    a2k_orient_get(&f, &m);
    CHECK(f == 0 && m == 0 && !a2k_orient_settling(), "default: off, not settling");
    CHECK(a2k_orient_listen(listener) == 0, "a listener registers");
    g0 = a2k_orient_gen();
    a2k_orient_set(1, 0);
    a2k_orient_get(&f, &m);
    CHECK(f == 1 && m == 0 && a2k_orient_gen() == g0 + 1 && seen_n == 1 && seen_f == 1 && seen_m == 0, "a change is stored, counted and announced once");
    a2k_orient_set(1, 0);
    CHECK(a2k_orient_gen() == g0 + 1 && seen_n == 1, "the same orientation again: no new generation, no announcement");
    a2k_orient_set(5, -3);
    a2k_orient_get(&f, &m);
    CHECK(f == 1 && m == 1, "any non-zero value counts as on (stored as 0 / 1)");
    CHECK(a2k_orient_settling(), "settling right after a change");
    a2k_orient_test_clock(1000 + A2K_ORIENT_SETTLE_MS - 1);
    CHECK(a2k_orient_settling(), "still settling just before the window ends");
    a2k_orient_test_clock(1000 + A2K_ORIENT_SETTLE_MS);
    CHECK(!a2k_orient_settling(), "settled when the window is over");
    {
        unsigned g = a2k_orient_gen();
        a2k_orient_hold();
        CHECK(a2k_orient_settling() && a2k_orient_gen() == g, "a hold opens the settling window without a change (no new generation)");
        a2k_orient_test_clock(1000 + A2K_ORIENT_SETTLE_MS * 2 + 5);
        CHECK(!a2k_orient_settling(), "...and it ends like the window of a change");
    }
    for (k = 0; k < 3; k++)
        ok &= a2k_orient_listen(listener) == 0;
    CHECK(ok && a2k_orient_listen(listener) == -1 && a2k_orient_listen(NULL) == -1, "at most 4 listeners (one taken above, three more fit), NULL refused");
    a2k_orient_test_reset();
}

static void test_directions(void)
{
    int s, ok = 1;
    printf("== direction signs (ONVIF / ptz.vel / step buttons, the tracker)\n");
    for (s = -1; s <= 1; s++)
        ok &= a2k_orient_pan_sign(s, 0) == s && a2k_orient_tilt_sign(s, 0) == s;
    CHECK(ok, "orientation off: directions unchanged");
    CHECK(a2k_orient_pan_sign(1, 1) == -1 && a2k_orient_pan_sign(-1, 1) == 1 && a2k_orient_pan_sign(0, 1) == 0, "mirror reverses the pan direction (right <-> left, 0 stays 0)");
    CHECK(a2k_orient_tilt_sign(1, 1) == -1 && a2k_orient_tilt_sign(-1, 1) == 1 && a2k_orient_tilt_sign(0, 1) == 0, "flip reverses the tilt direction (up <-> down)");
    /* the tracker's flags: the 2K binding measured invert_pan = 1, invert_tilt = 0 with the orientation off */
    CHECK(a2k_orient_invert_pan(1, 0) == 1 && a2k_orient_invert_tilt(0, 0) == 0, "orientation off: the measured binding (invert_pan 1, invert_tilt 0)");
    CHECK(a2k_orient_invert_pan(1, 1) == 0 && a2k_orient_invert_tilt(0, 1) == 1, "mirror toggles invert_pan, flip toggles invert_tilt");
    CHECK(a2k_orient_invert_pan(0, 1) == 1 && a2k_orient_invert_tilt(1, 1) == 0, "toggling is symmetric for the other base values");
}

static void test_geometry(void)
{
    int a, b, c, d, f, m;
    printf("== points and rectangles between the sensor and the displayed frame\n");
    for (f = 0; f <= 1; f++)
        for (m = 0; m <= 1; m++) {
            a = 10; b = 20; c = 40; d = 90;
            a2k_orient_rect(f, m, 100, 100, &a, &b, &c, &d);
            CHECK(a < c && b < d, "a rectangle stays well formed (x1 < x2, y1 < y2)");
            a2k_orient_rect(f, m, 100, 100, &a, &b, &c, &d);
            CHECK(a == 10 && b == 20 && c == 40 && d == 90, "converting twice gives the original (an involution)");
        }
    a = 10; b = 20; c = 40; d = 90;
    a2k_orient_rect(0, 1, 100, 100, &a, &b, &c, &d);
    CHECK(a == 60 && b == 20 && c == 90 && d == 90, "mirror: x1,x2 -> 100-x2,100-x1, y untouched");
    a = 10; b = 20; c = 40; d = 90;
    a2k_orient_rect(1, 0, 100, 100, &a, &b, &c, &d);
    CHECK(a == 10 && b == 10 && c == 40 && d == 80, "flip: y1,y2 -> 100-y2,100-y1, x untouched");
}

static void test_plane(void)
{
    enum { W = 8, H = 4 };
    uint8_t src[W * H], dst[W * H], back[W * H], big[(W + 3) * H];
    unsigned r, c;
    int ok;
    printf("== luma plane conversion (scene and drift references are kept in the sensor frame)\n");
    for (r = 0; r < H; r++)
        for (c = 0; c < W; c++)
            src[r * W + c] = (uint8_t)(r * 16 + c);
    CHECK(a2k_orient_plane(0, 0, src, W, H, W, dst) == 0 && memcmp(src, dst, sizeof src) == 0, "off: a plain copy");
    CHECK(a2k_orient_plane(0, 1, src, W, H, W, dst) == 0, "mirror");
    for (ok = 1, r = 0; r < H; r++)
        for (c = 0; c < W; c++)
            ok &= dst[r * W + c] == src[r * W + (W - 1 - c)];
    CHECK(ok, "mirror reverses every row");
    CHECK(a2k_orient_plane(1, 0, src, W, H, W, dst) == 0, "flip");
    for (ok = 1, r = 0; r < H; r++)
        for (c = 0; c < W; c++)
            ok &= dst[r * W + c] == src[(H - 1 - r) * W + c];
    CHECK(ok, "flip reverses the row order");
    CHECK(a2k_orient_plane(1, 1, src, W, H, W, dst) == 0, "flip + mirror");
    for (ok = 1, r = 0; r < H; r++)
        for (c = 0; c < W; c++)
            ok &= dst[r * W + c] == src[(H - 1 - r) * W + (W - 1 - c)];
    CHECK(ok, "ceiling = a 180 degree rotation");
    CHECK(a2k_orient_plane(1, 1, dst, W, H, W, back) == 0 && memcmp(back, src, sizeof src) == 0, "converting back gives the original (the same call, both ways)");
    for (r = 0; r < H; r++)
        for (c = 0; c < W + 3; c++)
            big[r * (W + 3) + c] = c < W ? src[r * W + c] : 0xEE;
    CHECK(a2k_orient_plane(0, 1, big, W, H, W + 3, dst) == 0 && dst[0] == src[W - 1] && dst[W - 1] == src[0], "a stride larger than the width is honoured, the padding is not read");
    CHECK(a2k_orient_plane(0, 0, src, 0, H, W, dst) == -1 && a2k_orient_plane(0, 0, src, W, H, W - 1, dst) == -1 && a2k_orient_plane(0, 0, src, W, H, W, src) == -1 &&
              a2k_orient_plane(0, 0, NULL, W, H, W, dst) == -1,
          "bad sizes, an in-place call and NULL are refused");
}

static struct a2k_det_box feet(int x, int y) /* a box whose feet point is (x%, y%) of the 320x192 detection image */
{
    struct a2k_det_box b = { 0, 900, 0, 0, 0, 0 };
    b.x1 = b.x2 = x * 320 / 100 - 1 < 0 ? 0 : x * 320 / 100 - 1;
    b.y1 = 0;
    b.y2 = y * 192 / 100 - 1 < 0 ? 0 : y * 192 / 100 - 1;
    return b;
}

static void test_zones(void)
{
    a2k_zones_t stored, shown;
    struct a2k_det_box here, mirrored_here, flipped_here, sky_person = feet(50, 15), floor_person = feet(50, 95);
    printf("== detection zones: stored in the sensor frame, matched in the displayed frame\n");
    memset(&stored, 0, sizeof stored);
    CHECK(a2k_zones_set_text(&stored, A2K_ZF_ZONES, "door=0,50,30,100;zone_mode=all") == 0 && a2k_zones_set_text(&stored, A2K_ZF_IGNORE, "sky=0,0,100,20") == 0,
          "a zone at the bottom left and an ignore band at the top, in the sensor frame");
    here = feet(15, 90); /* a person standing in the zone, as the sensor sees it */
    CHECK(a2k_zones_hit(&stored, &here) == 1u, "orientation off: the person in the zone hits it");
    shown = stored;
    a2k_zones_orient(&shown, 0, 0);
    CHECK(memcmp(&shown, &stored, sizeof shown) == 0, "orientation off: the conversion changes nothing");
    mirrored_here = feet(85, 90); /* the same person, in a mirrored picture */
    shown = stored;
    a2k_zones_orient(&shown, 0, 1);
    CHECK(a2k_zones_hit(&shown, &mirrored_here) == 1u && a2k_zones_hit(&shown, &here) == 0u, "mirror: the person at the right of the displayed picture hits the zone, the old spot does not");
    CHECK(shown.z[0].x1 == 70 && shown.z[0].x2 == 100 && shown.z[0].y1 == 50 && shown.z[0].y2 == 100 && shown.n == 1 && shown.mode == A2K_ZONE_MODE_ALL &&
              !strcmp(shown.z[0].name, "door"),
          "mirror: x reversed, y kept, name and mode kept");
    flipped_here = feet(15, 10); /* flipped: the same person is at the top */
    shown = stored;
    a2k_zones_orient(&shown, 1, 0);
    CHECK(shown.z[0].y1 == 0 && shown.z[0].y2 == 50 && a2k_zones_hit(&shown, &flipped_here) == 1u, "flip: the person at the top of the displayed picture hits the zone");
    CHECK(shown.ig[0].y1 == 80 && shown.ig[0].y2 == 100 && shown.ni == 1, "flip: the ignore band moves to the bottom with the scene");
    CHECK(a2k_zones_ignored(&stored, &sky_person) == 1 && a2k_zones_ignored(&stored, &floor_person) == 0, "orientation off: a person at the top is in the ignore band");
    CHECK(a2k_zones_ignored(&shown, &floor_person) == 1 && a2k_zones_ignored(&shown, &sky_person) == 0, "flip: the ignore band follows the scene to the bottom of the displayed picture");
    {
        a2k_zones_t twice = stored;
        a2k_zones_orient(&twice, 1, 1);
        a2k_zones_orient(&twice, 1, 1);
        CHECK(memcmp(&twice, &stored, sizeof twice) == 0, "ceiling twice: the original zones (an involution)");
    }
    {
        a2k_zones_t eff, cz = stored;
        cz.z[0].anchor = A2K_ZA_SCREEN;
        a2k_zones_effective(&cz, 0, &eff); /* the camera is moved: @screen zones stay in force, in the displayed frame too */
        a2k_zones_orient(&eff, 0, 1);
        CHECK(eff.n == 1 && !eff.z[0].off && eff.z[0].x1 == 70 && eff.z[0].anchor == A2K_ZA_SCREEN, "an @screen zone is converted like the others and stays active away from home");
    }
}

static void test_zlive(const char *dir)
{
    char p[300], buf[A2K_STORE_MAX + 1], text[300];
    a2k_zones_t card, z;
    printf("== live zones and their text entities across an orientation change\n");
    a2k_orient_test_reset();
    snprintf(p, sizeof p, "%s/zones", dir);
    memset(&card, 0, sizeof card);
    CHECK(a2k_zones_set_text(&card, A2K_ZF_ZONES, "door=0,50,30,100") == 0 && a2k_zlive_init(&card, p) == 0, "a zone drawn with the orientation off");
    a2k_zlive_get_text(A2K_ZF_ZONES, text, sizeof text);
    CHECK(!strcmp(text, "door=0,50,30,100"), "off: the text shows the stored zone");
    a2k_orient_set(0, 1);
    a2k_zlive_get_text(A2K_ZF_ZONES, text, sizeof text);
    CHECK(!strcmp(text, "door=70,50,100,100"), "mirror on: the same zone is shown where it now appears in the picture");
    a2k_zlive_get(&z, NULL);
    CHECK(z.z[0].x1 == 0 && z.z[0].x2 == 30, "the stored copy is untouched by the orientation change (still the sensor frame)");
    CHECK(a2k_zlive_set_text(A2K_ZF_ZONES, "porch=0,0,40,40") == 0, "an edit made while mirrored (the text is what the viewer sees)");
    a2k_zlive_get(&z, NULL);
    CHECK(z.n == 1 && z.z[0].x1 == 60 && z.z[0].x2 == 100 && z.z[0].y1 == 0 && z.z[0].y2 == 40, "it is stored in the sensor frame");
    CHECK(a2k_store_read(p, buf, sizeof buf) > 0 && !strcmp(buf, "porch=60,0,100,40\n"), "the persisted file is in the sensor frame too");
    a2k_zlive_get_text(A2K_ZF_ZONES, text, sizeof text);
    CHECK(!strcmp(text, "porch=0,0,40,40"), "the text reads back exactly as it was entered");
    a2k_orient_set(0, 0);
    a2k_zlive_get_text(A2K_ZF_ZONES, text, sizeof text);
    CHECK(!strcmp(text, "porch=60,0,100,40"), "orientation off again: the zone shows where the scene is without the mirror");
    a2k_orient_set(1, 1);
    CHECK(a2k_zlive_set_text(A2K_ZF_IGNORE, "sky=0,0,100,20") == 0, "an ignore rectangle edited with the ceiling mount on");
    a2k_zlive_get(&z, NULL);
    CHECK(z.ni == 1 && z.ig[0].y1 == 80 && z.ig[0].y2 == 100 && z.ig[0].x1 == 0 && z.ig[0].x2 == 100, "stored in the sensor frame (the band at the bottom)");
    CHECK(z.n == 1 && z.z[0].x1 == 60 && z.z[0].x2 == 100 && z.z[0].y1 == 0, "the zones edited before are not touched by an ignore edit");
    a2k_orient_test_reset();
}

/* The tracker works on the displayed frame; the motor direction it commands follows the flags orient.c computes. */
static struct tl_out run2(int invert_pan, int invert_tilt, struct tl_box b)
{
    tl_t t;
    struct tl_cfg c;
    struct tl_ptz p = { 0, 0, 0, -2000, 2000, -500, 500, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    struct tl_out o;
    tl_cfg_dev(&c);
    c.invert_pan = invert_pan;
    c.invert_tilt = invert_tilt;
    c.lead_pct = 0;
    c.tilt_enable = 1;
    tl_init(&t, &c);
    p.cap_ms = 1000;
    tl_update(&t, 1000, &b, 1, 320, 192, &p, &o);
    p.cap_ms = 1200;
    tl_update(&t, 1200, &b, 1, 320, 192, &p, &o);
    return o;
}

static void test_tracker(void)
{
    struct tl_box right = { 200 - 20, 96 - 75, 200 + 20, 96 + 75, 0, 0 }, low = { 160 - 20, 150 - 30, 160 + 20, 150 + 30, 0, 0 };
    struct tl_out base_r, base_l, m_r, f_l, both;
    printf("== person tracking: the motor direction for a person seen at one side of the displayed picture\n");
    /* the 2K binding with the orientation off: invert_pan 1, invert_tilt 0 */
    base_r = run2(a2k_orient_invert_pan(1, 0), a2k_orient_invert_tilt(0, 0), right);
    CHECK(base_r.kind == TL_NUDGE && base_r.dpan != 0, "orientation off: a person right of the centre gives a pan command");
    base_l = run2(a2k_orient_invert_pan(1, 0), a2k_orient_invert_tilt(0, 0), low);
    CHECK(base_l.kind == TL_NUDGE && base_l.dtilt != 0, "orientation off: a person below the centre gives a tilt command");
    m_r = run2(a2k_orient_invert_pan(1, 1), a2k_orient_invert_tilt(0, 0), right);
    CHECK(m_r.kind == TL_NUDGE && m_r.dpan == -base_r.dpan && m_r.dtilt == base_r.dtilt, "mirror: the pan command has the opposite motor sign, the tilt is unchanged");
    f_l = run2(a2k_orient_invert_pan(1, 0), a2k_orient_invert_tilt(0, 1), low);
    CHECK(f_l.kind == TL_NUDGE && f_l.dtilt == -base_l.dtilt && f_l.dpan == base_l.dpan, "flip: the tilt command has the opposite motor sign, the pan is unchanged");
    both = run2(a2k_orient_invert_pan(1, 1), a2k_orient_invert_tilt(0, 1), right);
    CHECK(both.kind == TL_NUDGE && both.dpan == -base_r.dpan && both.dtilt == base_r.dtilt * -1, "ceiling: both signs reversed, the magnitudes are the same");
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <scratch dir>\n", argv[0]);
        return 2;
    }
    test_state();
    test_directions();
    test_geometry();
    test_plane();
    test_zones();
    test_zlive(argv[1]);
    test_tracker();
    printf("rot2k orient unit: %d failed\n", fails);
    return fails ? 1 : 0;
}
