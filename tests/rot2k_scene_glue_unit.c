/* Host unit test of the runtime side of the scene check (app/akam2k_scene.c, with the real settings store): the thumbnails fed by the detection
 * thread, the two-step HOME reference write (capture, then commit: written only when needed, a changed view at most once per 24 h), the matched-boots counter, the invalidation (and its
 * failure). The motor side (when these hooks are called, the counters re-read) is tested in rot2k_hal_ptz_unit.c.
 * Prints "rot2k scene glue unit: N failed". */
#define _GNU_SOURCE
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../firmware/lsc-rotatable-2k-ak3918av130/app/akam2k_night.h"
#include "../firmware/lsc-rotatable-2k-ak3918av130/app/akam2k_scene.h"
#include "../firmware/lsc-rotatable-2k-ak3918av130/app/akam2k_store.h"
#include "../firmware/lsc-rotatable-2k-ak3918av130/app/scene_check.h"

static int fails;
#define CHECK(c, msg) do { if (c) printf("  ok   - %s\n", msg); else { printf("  FAIL - %s\n", msg); fails++; } } while (0)

static int g_night_valid = 1, g_night;
void a2k_night_get(a2k_night_info_t *o)
{
    memset(o, 0, sizeof *o);
    o->valid = g_night_valid;
    o->night = g_night;
}
void a2k_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    printf("    log: ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
}

/* synthetic scene: smoothed noise on the 80x48 thumbnail grid, shown to the feed as a 320x192 luma plane (4x4 blocks) */
static unsigned rng;
static unsigned rnd(void) { rng = rng * 1664525u + 1013904223u; return rng >> 16; }
static uint8_t plane[192][320];

static void scene(unsigned seed, int noise)
{
    static int a[48][80], b[48][80];
    int x, y, pass, lo = 1 << 30, hi = -1;
    rng = seed;
    for (y = 0; y < 48; y++)
        for (x = 0; x < 80; x++)
            a[y][x] = (int)(rnd() & 255);
    for (pass = 0; pass < 3; pass++) {
        for (y = 0; y < 48; y++)
            for (x = 0; x < 80; x++) {
                int s = 0, n = 0, dx, dy;
                for (dy = -1; dy <= 1; dy++)
                    for (dx = -1; dx <= 1; dx++)
                        if (y + dy >= 0 && y + dy < 48 && x + dx >= 0 && x + dx < 80) {
                            s += a[y + dy][x + dx];
                            n++;
                        }
                b[y][x] = s / n;
            }
        memcpy(a, b, sizeof a);
    }
    for (y = 0; y < 48; y++)
        for (x = 0; x < 80; x++) {
            if (a[y][x] < lo) lo = a[y][x];
            if (a[y][x] > hi) hi = a[y][x];
        }
    for (y = 0; y < 192; y++)
        for (x = 0; x < 320; x++) {
            int v = (a[y / 4][x / 4] - lo) * 255 / (hi - lo) + (noise ? (int)(rnd() % (2 * noise + 1)) - noise : 0);
            plane[y][x] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
        }
}
static void feed(void) { a2k_scene_feed(&plane[0][0], 320, 192, 320); }

static char dir[200], path[220];
static char *slurp(size_t *n)
{
    static char buf[8192];
    FILE *f = fopen(path, "rb");
    *n = 0;
    if (!f)
        return NULL;
    *n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    return buf;
}

#define HOME_PAN 1275
#define HOME_TILT 1300
#define T0 1760000000LL /* a plausible wall clock (Oct 2025) */
#define DAY 86400LL

static unsigned wr(const a2k_ptz_scene_t *h) { return h->writes ? h->writes() : 0; }

/* a calibration ran and the head is at home: the candidate is the current view */
static int cap_commit(const a2k_ptz_scene_t *h, int fresh, int *cap_rc)
{
    int rc = h->capture();
    if (cap_rc)
        *cap_rc = rc;
    return rc == 0 ? h->commit(HOME_PAN, HOME_TILT, fresh) : -2;
}

int main(void)
{
    const a2k_ptz_scene_t *h = a2k_scene_hooks();
    uint32_t pan = HOME_PAN, tilt = HOME_TILT;
    size_t n;
    char *d;
    sc_ref_t r;
    unsigned w;
    int crc;
    snprintf(dir, sizeof dir, "/tmp/rot2k_scene_glue_%d", (int)getpid());
    mkdir(dir, 0755);
    snprintf(path, sizeof path, "%s/ptz.scene", dir);
    a2k_scene_test_set(path, 0, 0, 1, 2000);
    a2k_scene_test_clock(T0);

    printf("== capture and commit (the home reference; none on the flash yet)\n");
    CHECK(h->capture() == -1, "no frame fed yet: capture refused");
    scene(7, 0);
    feed();
    g_night_valid = 0;
    CHECK(h->capture() == -1, "day/night mode unknown: capture refused");
    g_night_valid = 1;
    CHECK(h->capture() == 0, "textured view, mode known: candidate taken");
    scene(7, 1);
    feed(); /* the same scene, sensor noise */
    w = wr(h);
    CHECK(h->commit(HOME_PAN, HOME_TILT, 1) == 0, "no reference yet, the view is still the candidate: written");
    d = slurp(&n);
    CHECK(d && n > 3840 && n < 5000 && sc_unpack(d, n, &r) == 0 && r.pan == HOME_PAN && r.tilt == HOME_TILT && r.mode == 0 && r.boots == 0 && r.stamp == (uint32_t)T0,
          "file: unpacks, home position, mode, boots 0, below 5 KB, the write time in the header");
    CHECK(wr(h) == w + 1, "one flash write counted");

    printf("== the view changes during the confirmation window\n");
    scene(7, 0);
    feed();
    CHECK(h->capture() == 0, "candidate of scene 7");
    scene(99, 0);
    feed(); /* somebody moved the head (or the scene): another view */
    w = wr(h);
    CHECK(h->commit(HOME_PAN, HOME_TILT, 0) == -1, "view changed: commit refused");
    d = slurp(&n);
    CHECK(d && sc_unpack(d, n, &r) == 0 && r.stamp == (uint32_t)T0 && wr(h) == w, "the file still holds the previous (consistent) reference, nothing written");
    scene(7, 0);
    feed();
    CHECK(h->capture() == 0, "candidate again");
    g_night = 1; /* day -> night during the window */
    CHECK(h->commit(HOME_PAN, HOME_TILT, 0) == -1, "day/night mode changed during the window: refused");
    g_night = 0;

    printf("== an unchanged home view writes nothing\n");
    scene(7, 1);
    feed();
    w = wr(h);
    CHECK(cap_commit(h, 0, &crc) == 1 && wr(h) == w, "the stored reference still matches: commit says 1 (nothing written), no flash write");
    scene(7, 0);
    feed();
    CHECK(cap_commit(h, 1, NULL) == 1 && wr(h) == w, "...also after a calibration when the counter is already 0 (fresh)");
    d = slurp(&n);
    CHECK(d && sc_unpack(d, n, &r) == 0 && r.boots == 0 && r.stamp == (uint32_t)T0, "file untouched");

    printf("== low detail\n");
    memset(plane, 120, sizeof plane);
    feed();
    w = wr(h);
    CHECK(h->capture() == 0 && h->commit(HOME_PAN, HOME_TILT, 0) == 1 && wr(h) == w, "a uniform view while a reference under 24 h old is in force: kept, no excursion, no write");
    a2k_scene_test_clock(T0 + 2 * DAY);
    CHECK(h->capture() == 2, "...a day later a uniform view is not a reference on its own (2: the controller may build a multi-view reference)");
    CHECK(h->commit(HOME_PAN, HOME_TILT, 0) == -1, "...and a plain commit of it is refused");
    a2k_scene_test_clock(T0);

    printf("== boots counter: bumped once per matched boot, restarted by a calibration, carried otherwise\n");
    a2k_scene_test_set(path, 0, 0, 1, 2000);
    a2k_scene_test_clock(T0 + 100);
    scene(7, 1);
    feed();
    w = wr(h);
    pan = HOME_PAN;
    tilt = HOME_TILT;
    CHECK(h->boot_check(&pan, &tilt, NULL) == 1 && pan == HOME_PAN && tilt == HOME_TILT, "boot check (in: home): match with the saved scene");
    d = slurp(&n);
    CHECK(d && sc_unpack(d, n, &r) == 0 && r.boots == 1 && r.stamp == (uint32_t)T0 && wr(h) == w + 1, "...the counter in the file is now 1 (ONE write for the boot), the stamp is kept");
    scene(7, 0);
    feed();
    CHECK(cap_commit(h, 0, NULL) == 1 && wr(h) == w + 1, "a later check without a calibration: unchanged, no write (the counter is carried by the file)");
    scene(7, 0);
    feed();
    CHECK(cap_commit(h, 1, NULL) == 0 && wr(h) == w + 2, "after a full calibration (fresh) with the view unchanged: ONE write, only the counter");
    d = slurp(&n);
    CHECK(d && sc_unpack(d, n, &r) == 0 && r.boots == 0 && r.stamp == (uint32_t)T0, "...restarts the counter at 0, the stamp (the view's age) is kept");
    scene(99, 0);
    feed();
    pan = HOME_PAN;
    tilt = HOME_TILT;
    CHECK(h->boot_check(&pan, &tilt, NULL) == 0, "boot check: another scene, no match");

    printf("== boot check refuses a reference that is not at HOME\n");
    scene(7, 1);
    feed();
    pan = 1500;
    tilt = 900;
    CHECK(h->boot_check(&pan, &tilt, NULL) == 0, "the home position differs from the reference position: calibrate (a reference proves 'at home' only)");

    printf("== rewrite of a changed home view: at most once per 24 h, the rate limit lives in the file\n");
    a2k_scene_test_set(path, 0, 0, 1, 2000);
    a2k_scene_test_clock(T0 + 3600);
    scene(99, 0);
    feed();
    w = wr(h);
    CHECK(cap_commit(h, 1, NULL) == 1 && wr(h) == w, "the home view changed but the reference is 1 h old: NOT rewritten (no write)");
    a2k_scene_test_clock(T0 + DAY - 10);
    scene(99, 0);
    feed();
    CHECK(cap_commit(h, 1, NULL) == 1 && wr(h) == w, "...nor at 24 h minus 10 s");
    a2k_scene_test_clock(T0 + DAY + 10);
    scene(99, 0);
    feed();
    CHECK(cap_commit(h, 1, NULL) == 0 && wr(h) == w + 1, "after 24 h: rewritten (one write)");
    d = slurp(&n);
    CHECK(d && sc_unpack(d, n, &r) == 0 && r.stamp == (uint32_t)(T0 + DAY + 10) && r.boots == 0, "...with the new stamp");
    scene(7, 0);
    feed();
    CHECK(cap_commit(h, 1, NULL) == 1 && wr(h) == w + 1, "and the next change, right away, is refused again (1 write per day)");
    a2k_scene_test_clock(500); /* a clock that is not set (no network): the limit cannot be checked */
    scene(7, 0);
    feed();
    CHECK(cap_commit(h, 1, NULL) == 1 && wr(h) == w + 1, "an unset clock: no rewrite of an existing reference (cannot rate-limit)");
    a2k_scene_test_clock(T0 + 2 * DAY + 100);
    scene(7, 0);
    feed();
    CHECK(h->capture() == 0 && h->commit(1300, HOME_TILT, 1) == 0 && wr(h) == w + 2, "a reference taken at ANOTHER position (home was reconfigured): replaced at once, the limit does not apply");
    d = slurp(&n);
    CHECK(d && sc_unpack(d, n, &r) == 0 && r.pan == 1300, "...at the new home");
    g_night = 1; /* a calibration at night, a reference of the day */
    scene(7, 0);
    feed();
    a2k_scene_test_clock(T0 + 2 * DAY + 3600);
    w = wr(h);
    CHECK(h->capture() == 0 && h->commit(1300, HOME_TILT, 0) == 1 && wr(h) == w, "another day/night mode is a changed view: under the 24 h rule it is NOT rewritten within a day");
    a2k_scene_test_clock(T0 + 5 * DAY);
    CHECK(h->capture() == 0 && h->commit(1300, HOME_TILT, 0) == 0, "...and replaced after 24 h (consistent rule: a reference in the other mode is replaceable)");
    d = slurp(&n);
    CHECK(d && sc_unpack(d, n, &r) == 0 && r.mode == 1, "the stored reference is now the night one");
    g_night = 0;

    printf("== an unknown stamp allows ONE rewrite per run\n");
    {
        sc_ref_t o;
        char b3[8192];
        size_t m;
        a2k_scene_test_set(path, 0, 0, 1, 2000);
        a2k_scene_test_clock(T0 + 5 * DAY + 10);
        scene(7, 0);
        feed();
        d = slurp(&n);
        if (d && sc_unpack(d, n, &o) == 0) {
            o.stamp = 0;
            m = sc_pack(&o, b3, sizeof b3);
            FILE *f = fopen(path, "wb");
            if (f) {
                fwrite(b3, 1, m, f);
                fclose(f);
            }
        }
        scene(99, 0);
        feed();
        CHECK(cap_commit(h, 1, NULL) == 0, "reference of unknown age, the view changed: rewritten (stamped now)");
        scene(7, 0);
        feed();
        CHECK(cap_commit(h, 1, NULL) == 1, "...a second change right after is refused (24 h from the new stamp)");
        d = slurp(&n);
        if (d && sc_unpack(d, n, &o) == 0) {
            o.stamp = 0;
            m = sc_pack(&o, b3, sizeof b3);
            FILE *f = fopen(path, "wb");
            if (f) {
                fwrite(b3, 1, m, f);
                fclose(f);
            }
        }
        scene(99, 0);
        feed();
        CHECK(cap_commit(h, 1, NULL) == 1, "an unstamped reference again in the same run: no second unstamped rewrite");
    }

    printf("== the matched-boots count survives a process restart and an invalidation\n");
    a2k_scene_test_set(path, 0, 0, 1, 2000);
    a2k_scene_test_clock(T0 + 6 * DAY);
    scene(7, 0);
    feed();
    CHECK(h->capture() == 0 && h->commit(HOME_PAN, HOME_TILT, 1) == 0, "reference written at home after a calibration (boots 0)");
    for (int k = 0; k < 4; k++) {
        scene(7, 1);
        feed();
        pan = HOME_PAN;
        tilt = HOME_TILT;
        CHECK(h->boot_check(&pan, &tilt, NULL) == 1, "boot match");
    }
    d = slurp(&n);
    CHECK(d && sc_unpack(d, n, &r) == 0 && r.boots == 4, "four matched boots: count 4 in the file");
    a2k_scene_test_set(path, 0, 0, 1, 2000); /* a new process: nothing in memory */
    a2k_scene_test_clock(T0 + 8 * DAY);
    scene(99, 0);
    feed();
    CHECK(h->capture() == 0 && h->commit(HOME_PAN, HOME_TILT, 0) == 0, "after an adoption (no calibration ran) a changed view, a day later: a write");
    d = slurp(&n);
    CHECK(d && sc_unpack(d, n, &r) == 0 && r.boots == 4, "...keeps the on-disk count (4): the periodic calibration is not postponed");
    CHECK(h->drop() == 0, "a failed calibration drops the reference");
    d = slurp(&n);
    CHECK(d && n > 8 && !strncmp(d, "invalid 4", 9), "...the marker remembers the count");
    a2k_scene_test_set(path, 0, 0, 1, 2000); /* the app restarts */
    a2k_scene_test_clock(T0 + 8 * DAY + 50);
    scene(7, 0);
    feed();
    CHECK(h->capture() == 0 && h->commit(HOME_PAN, HOME_TILT, 0) == 0, "a write after the restart: no valid reference, so no rate limit");
    d = slurp(&n);
    CHECK(d && sc_unpack(d, n, &r) == 0 && r.boots == 4, "...count still 4");
    a2k_scene_test_set(path, 0, 0, 1, 2000);
    a2k_scene_test_clock(T0 + 9 * DAY + 50);
    scene(7, 0);
    feed();
    CHECK(h->capture() == 0 && h->commit(HOME_PAN, HOME_TILT, 1) == 0, "a calibration, then the check: only the counter restarts");
    d = slurp(&n);
    CHECK(d && sc_unpack(d, n, &r) == 0 && r.boots == 0, "...restarts the count");

    printf("== invalidation\n");
    CHECK(h->drop() == 0, "drop: ok");
    d = slurp(&n);
    CHECK(d && n >= strlen(sc_invalid_marker) - 1 && !strncmp(d, "invalid", 7), "the reference is an invalid marker now");
    w = wr(h);
    CHECK(h->drop() == 0 && wr(h) == w, "drop again: nothing to do, no write");
    pan = HOME_PAN;
    tilt = HOME_TILT;
    CHECK(h->boot_check(&pan, &tilt, NULL) == 0, "an invalidated reference never matches");
    scene(7, 0);
    feed();
    CHECK(h->capture() == 0 && h->commit(HOME_PAN, HOME_TILT, 1) == 0, "a new reference is written");
    if (geteuid() != 0) {
        chmod(dir, 0555); /* the directory cannot be written: the marker cannot be stored */
        CHECK(h->drop() == -1, "drop fails when the invalid marker cannot be written (reported, nothing is refused)");
        chmod(dir, 0755);
    } else {
        printf("  skip - running as root: a read-only directory is not enforced\n");
    }

    printf("== repetitive texture (blinds): never a reference\n");
    {
        int x, y;
        for (y = 0; y < 192; y++)
            for (x = 0; x < 320; x++)
                plane[y][x] = (uint8_t)(((x / 4) % 7) < 3 ? 70 : 190); /* vertical bars, period 7 thumbnail px */
        feed();
        CHECK(h->capture() == 1 && h->commit(HOME_PAN, HOME_TILT, 0) == -1, "bars: capture refuses (1), no candidate, commit refused");
        CHECK(h->alt_capture(0) == 1, "bars at an offset: not kept either");
    }
    printf("== multi-view reference: featureless rest view + views at offsets\n");
    unlink(path);
    a2k_scene_test_set(path, 0, 0, 1, 2000);
    a2k_scene_test_clock(T0 + 10 * DAY);
    memset(plane, 120, sizeof plane);
    feed();
    CHECK(h->capture() == 2, "featureless rest view: candidate kept, multi-view requested");
    CHECK(h->commit(HOME_PAN, HOME_TILT, 1) == -1, "the plain commit refuses a featureless candidate");
    {
        int i, dp, dt, ok = 1;
        for (i = 0; i < 3; i++) {
            ok &= h->offset((unsigned)i, &dp, &dt) == 0;
            scene(21 + (unsigned)i, 0);
            feed();
            ok &= h->alt_capture((unsigned)i) == 0;
            scene(21 + (unsigned)i, 1);
            feed();
            ok &= h->alt_confirm((unsigned)i) == 0;
        }
        CHECK(ok && h->offset(3, &dp, &dt) == -1, "three offset views kept and confirmed (the same view + noise)");
        scene(25, 0);
        feed();
        CHECK(h->alt_capture(2) == 0, "alt candidate 2 = scene 25");
        scene(77, 0);
        feed();
        CHECK(h->alt_confirm(2) == -1, "an offset view that changed during the confirmation: refused");
        memset(plane, 120, sizeof plane);
        feed();
        CHECK(h->alt_capture(2) == 1, "a featureless view at an offset: nothing to keep (1), the earlier candidate is not clobbered");
    }
    CHECK(h->commit_multi(HOME_PAN, HOME_TILT, 1, 0) == -1, "no confirmed offset view: nothing is written");
    CHECK(h->commit_multi(HOME_PAN, HOME_TILT, 1, 7) == 0, "multi-view reference written (mask 7)");
    {
        char p2[260];
        FILE *f;
        sc_ref_t m, a1;
        char b2[8192];
        size_t k;
        d = slurp(&n);
        CHECK(d && sc_unpack(d, n, &m) == 0 && m.pan == HOME_PAN && m.tilt == HOME_TILT && !sc_has_detail(&m.th) && m.stamp == (uint32_t)(T0 + 10 * DAY), "main file: the featureless rest view at home, stamped");
        snprintf(p2, sizeof p2, "%s.a1", path);
        f = fopen(p2, "rb");
        k = f ? fread(b2, 1, sizeof b2, f) : 0;
        if (f)
            fclose(f);
        CHECK(k > 0 && sc_unpack(b2, k, &a1) == 0 && a1.pan == HOME_PAN - SC_OFF_PAN && a1.tilt == HOME_TILT && sc_has_detail(&a1.th), "stored view 1: position = rest + (-D, 0), detailed");
    }
    /* a recent flat reference is kept: no excursion again, the counter alone may restart */
    a2k_scene_test_clock(T0 + 10 * DAY + 3600);
    memset(plane, 120, sizeof plane);
    feed();
    w = wr(h);
    CHECK(h->capture() == 0 && h->commit(HOME_PAN, HOME_TILT, 1) == 1 && wr(h) == w, "a featureless home view again an hour later: the recent flat reference is kept (no multi-view rebuild, no write)");
    a2k_scene_test_clock(T0 + 12 * DAY);
    memset(plane, 120, sizeof plane);
    feed();
    CHECK(h->capture() == 2, "two days later the multi-view reference may be rebuilt (2)");
    /* the next cold boot: the rest view is featureless, the stored views decide */
    memset(plane, 120, sizeof plane);
    feed();
    pan = HOME_PAN;
    tilt = HOME_TILT;
    CHECK(h->boot_check(&pan, &tilt, NULL) == 2 && pan == HOME_PAN && tilt == HOME_TILT, "boot: featureless saved view -> answer 2 (probe), claimed position = home");
    {
        int dp, dt;
        CHECK(h->probe_offset(0, &dp, &dt) == 0 && dp == SC_OFF_PAN && h->probe_offset(1, &dp, &dt) == 0 && h->probe_offset(2, &dp, &dt) == 0 && h->probe_offset(3, &dp, &dt) == -1, "three stored views usable");
        scene(22, 1); /* the head at offset 1: the view of scene 22 */
        feed();
        w = wr(h);
        CHECK(h->probe(1) == 1, "the live view at offset 1 matches its stored view");
        d = slurp(&n);
        CHECK(d && sc_unpack(d, n, &r) == 0 && r.boots == 1 && wr(h) == w + 1, "the matched-boots counter was bumped in the main file (one write)");
        scene(99, 0); /* the head was NOT where the reference says */
        feed();
        CHECK(h->probe(1) == 0, "another view at the offset: no match");
        scene(22, 1);
        feed();
        CHECK(h->probe(0) == 0, "the view of offset 1 is not the stored view of offset 0");
    }
    CHECK(h->drop() == 0, "drop: the main reference is invalidated");
    memset(plane, 120, sizeof plane);
    feed();
    pan = HOME_PAN;
    tilt = HOME_TILT;
    CHECK(h->boot_check(&pan, &tilt, NULL) == 0, "...and the stored views are never used without a valid main reference");
    unlink(path);
    {
        char tmp[260];
        int i;
        snprintf(tmp, sizeof tmp, "%s.tmp", path);
        unlink(tmp);
        for (i = 0; i < 3; i++) {
            snprintf(tmp, sizeof tmp, "%s.a%d", path, i);
            unlink(tmp);
            snprintf(tmp, sizeof tmp, "%s.a%d.tmp", path, i);
            unlink(tmp);
        }
    }
    rmdir(dir);
    printf("rot2k scene glue unit: %d failed\n", fails);
    return fails ? 1 : 0;
}
