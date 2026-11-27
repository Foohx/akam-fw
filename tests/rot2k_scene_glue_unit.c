/* Host unit test of the runtime side of the scene check (app/scene.c, with the real settings store): the thumbnails fed by the detection
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
#include <pthread.h>
#include <time.h>
#include <unistd.h>

#include "../firmware/lsc-rotatable-2k-ak3918av130/night_ctl.h"
#include "../platforms/anyka-ak3918av130/orient.h"
#include "../firmware/lsc-rotatable-2k-ak3918av130/scene.h"
#include "../firmware/lsc-rotatable-2k-ak3918av130/store.h"
#include "../src/features/scene/scene_check.h"

static int fails;
#define CHECK(c, msg) do { if (c) printf("  ok   - %s\n", msg); else { printf("  FAIL - %s\n", msg); fails++; } } while (0)

static int g_night_valid = 1, g_night;
void a2k_night_get(a2k_night_info_t *o)
{
    memset(o, 0, sizeof *o);
    o->valid = g_night_valid;
    o->night = g_night;
}
static int g_probe_busy;
int a2k_night_probe_busy(void) { return g_probe_busy; }
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


/* ---- the drift hook (a2k_ptz_scene_t.drift): the reference is read, the live view compared over the wide range, a shift / recal verdict needs a SECOND later frame to agree ---- */
static pthread_mutex_t fmu = PTHREAD_MUTEX_INITIALIZER;
static uint8_t fpl[2][192][320]; /* the planes the feeder alternates between (fpl[0] only when alt = 0) */
static volatile int f_run, f_alt;
static void *feeder(void *a)
{
    unsigned k = 0;
    (void)a;
    while (f_run) {
        pthread_mutex_lock(&fmu);
        a2k_scene_feed(&fpl[f_alt ? (k & 1) : 0][0][0], 320, 192, 320);
        pthread_mutex_unlock(&fmu);
        k++;
        usleep(60000);
    }
    return NULL;
}

static int abort_yes(void) { return 1; }
static volatile int f_flip;
static int64_t f_t0;
static int64_t mono_ms_t(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000; }
static uint8_t fpl_flat[192][320];
static void *feeder_flip(void *a)
{
    (void)a;
    memset(fpl_flat, 128, sizeof fpl_flat);
    while (f_run) {
        pthread_mutex_lock(&fmu);
        if (mono_ms_t() - f_t0 < 250)
            a2k_scene_feed(&fpl_flat[0][0], 320, 192, 320);
        else
            a2k_scene_feed(&fpl[0][0][0], 320, 192, 320);
        pthread_mutex_unlock(&fmu);
        usleep(60000);
    }
    return NULL;
}

/* the content of the base scene moved right / down by (dx, dy) thumbnail px (4 source px each), edge clamped */
static void put_shifted(int which, int dx, int dy)
{
    int x, y;
    pthread_mutex_lock(&fmu);
    for (y = 0; y < 192; y++)
        for (x = 0; x < 320; x++) {
            int sx = x - 4 * dx, sy = y - 4 * dy;
            sx = sx < 0 ? 0 : sx > 319 ? 319 : sx;
            sy = sy < 0 ? 0 : sy > 191 ? 191 : sy;
            fpl[which][y][x] = plane[sy][sx];
        }
    pthread_mutex_unlock(&fmu);
}

static void drift_tests(const a2k_ptz_scene_t *h)
{
    pthread_t th;
    int dx, dy, v;
    uint8_t keep[192][320];
    unsigned w0;
    printf("== the drift check (reference read, wide search, a second frame must agree)\n");
    unlink(path);
    a2k_scene_test_clock(T0);
    g_night = 0;
    scene(7, 0);
    feed();
    CHECK(cap_commit(h, 1, NULL) == 0, "a home reference is saved (calibrated head)");
    w0 = wr(h);
    memcpy(keep, plane, sizeof keep); /* the base scene, as the thumbnail of the reference */
    CHECK(h->drift != NULL, "the scene hooks carry the drift check");
    f_run = 1;
    f_alt = 0;
    put_shifted(0, 0, 0);
    pthread_create(&th, NULL, feeder, NULL);
    v = h->drift(HOME_PAN, HOME_TILT, 0, &dx, &dy);
    CHECK(v == A2K_DRIFT_OK && dx == 0 && dy == 0, "the unmoved view: OK (0,0)");
    put_shifted(0, 2, -1);
    usleep(150000);
    v = h->drift(HOME_PAN, HOME_TILT, 0, &dx, &dy);
    CHECK(v == A2K_DRIFT_SHIFT && dx == 2 && dy == -1, "the content moved by (2,-1) px: SHIFT with that displacement (two frames agree)");
    put_shifted(0, -5, 4);
    usleep(150000);
    v = h->drift(HOME_PAN, HOME_TILT, 0, &dx, &dy);
    CHECK(v == A2K_DRIFT_SHIFT && dx == -5 && dy == 4, "(-5,4) px: SHIFT");
    put_shifted(0, 10, 0);
    usleep(150000);
    v = h->drift(HOME_PAN, HOME_TILT, 0, &dx, &dy);
    CHECK(v == A2K_DRIFT_RECAL, "10 px: beyond the correctable range: RECAL");
    put_shifted(0, 0, 0);
    usleep(150000);
    CHECK(h->drift(HOME_PAN + 10, HOME_TILT, 0, &dx, &dy) == A2K_DRIFT_SKIP, "the reference is keyed to another position than the configured home: SKIP");
    put_shifted(0, 10, 0); /* a displaced (dark) view that would read RECAL ... */
    usleep(150000);
    g_probe_busy = 1;
    CHECK(h->drift(HOME_PAN, HOME_TILT, 0, &dx, &dy) == A2K_DRIFT_SKIP, "during an IR-off probe / switch: SKIP, not RECAL");
    g_probe_busy = 0;
    CHECK(h->drift(HOME_PAN, HOME_TILT, 0, &dx, &dy) == A2K_DRIFT_RECAL, "... and RECAL again once the probe is over (same frames)");
    put_shifted(0, 0, 0);
    usleep(150000);
    g_night = 1;
    CHECK(h->drift(HOME_PAN, HOME_TILT, 0, &dx, &dy) == A2K_DRIFT_SKIP, "another day/night mode than the reference: SKIP");
    g_night = 0;
    /* two frames that disagree (the head shifted by 2 px in one, not in the other): never acted on */
    f_run = 0;
    pthread_join(th, NULL);
    put_shifted(0, 2, 0);
    put_shifted(1, 3, 0);
    f_alt = 1;
    f_run = 1;
    pthread_create(&th, NULL, feeder, NULL);
    usleep(300000); /* the newest frame is one of the alternating pair */
    v = h->drift(HOME_PAN, HOME_TILT, 0, &dx, &dy);
    CHECK(v == A2K_DRIFT_INCONC, "frames that alternate between a 2 px and a 3 px shift: the second frame disagrees: inconclusive, never a SHIFT");
    f_run = 0;
    pthread_join(th, NULL);
    CHECK(wr(h) == w0, "the drift check never writes the reference (flash write counter unchanged)");
    /* no frame newer than the last motion: the check waits for one, then gives up (SKIP) */
    a2k_scene_test_set(path, 0, 0, 1, 2000);
    a2k_scene_test_clock(T0);
    {
        struct timespec ts;
        int64_t now;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        now = (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
        CHECK(h->drift(HOME_PAN, HOME_TILT, now + 60000, &dx, &dy) == A2K_DRIFT_SKIP, "only frames older than the last motion exist: SKIP after the wait");
    }
    unlink(path);
    CHECK(h->drift(HOME_PAN, HOME_TILT, 0, &dx, &dy) == A2K_DRIFT_SKIP, "no reference file: SKIP");
}


/* ---- the drift reference (drift_match.h) behind hook_capture / hook_drift4: RAM only, one reference serves day and night ---- */
static void dm_render(int dx, int dy, int night)
{
    /* an edge-rich room: 70 filled rectangles of random grey, placed by a fixed seed; shifted by (dx, dy) px; night = inverted polarity, less contrast, a little noise */
    static int rx[70], ry[70], rw[70], rh[70], rg[70];
    static int init;
    int k, x, y;
    if (!init) {
        unsigned r = 4242;
        init = 1;
        for (k = 0; k < 70; k++) {
            r = r * 1664525u + 1013904223u; rx[k] = (int)((r >> 8) % 300);
            r = r * 1664525u + 1013904223u; ry[k] = (int)((r >> 8) % 170);
            r = r * 1664525u + 1013904223u; rw[k] = 10 + (int)((r >> 8) % 60);
            r = r * 1664525u + 1013904223u; rh[k] = 10 + (int)((r >> 8) % 50);
            r = r * 1664525u + 1013904223u; rg[k] = 40 + (int)((r >> 8) % 180);
        }
    }
    pthread_mutex_lock(&fmu);
    for (y = 0; y < 192; y++)
        for (x = 0; x < 320; x++) {
            int v = 128, sx = x - dx, sy = y - dy;
            for (k = 0; k < 70; k++)
                if (sx >= rx[k] && sx < rx[k] + rw[k] && sy >= ry[k] && sy < ry[k] + rh[k])
                    v = rg[k];
            if (night)
                v = 110 + (128 - v) / 2 + (int)((unsigned)(x * 7 + y * 13) % 5);
            fpl[0][y][x] = (uint8_t)v;
        }
    pthread_mutex_unlock(&fmu);
}

static void drift_ref_tests(const a2k_ptz_scene_t *h)
{
    pthread_t th;
    unsigned caps = 0, bridges = 0;
    int dxq = 0, dyq = 0, v, have_day = 0, have_night = 0;
    char path[256];
    printf("== the drift reference: captured in RAM at home, one reference for both modes, corrections in quarter px\n");
    snprintf(path, sizeof path, "%s/ptz.scene", dir);
    a2k_scene_test_set(path, 0, 0, 1, 2000);
    CHECK(h->drift4 != NULL, "the hooks carry the patch matcher drift check");
    dm_render(0, 0, 0);
    g_night = 0;
    g_probe_busy = 0;
    f_run = 1;
    f_alt = 0;
    pthread_create(&th, NULL, feeder, NULL);
    usleep(300000);
    v = h->drift4(HOME_PAN, HOME_TILT, 0, &dxq, &dyq);
    CHECK(v == A2K_DRIFT_SKIP, "ptz_drift_ref off (the default of the test build): no reference, the check says skip");
    a2k_scene_set_drift_ref(1);
    usleep(300000);
    v = h->drift4(HOME_PAN, HOME_TILT, 0, &dxq, &dyq);
    CHECK(v == A2K_DRIFT_SKIP, "drift_ref on but no capture yet: skip");
    (void)h->capture();
    a2k_scene_drift_ref_info(&caps, &bridges, &have_day, &have_night);
    CHECK(caps == 1 && have_day && !have_night, "the capture at home keeps the day reference (RAM only)");
    v = h->drift4(HOME_PAN, HOME_TILT, 0, &dxq, &dyq);
    CHECK(v == A2K_DRIFT_OK && dxq == 0 && dyq == 0, "the unchanged view reads zero");
    dm_render(7, -5, 0);
    usleep(300000);
    v = h->drift4(HOME_PAN, HOME_TILT, 0, &dxq, &dyq);
    CHECK(v == A2K_DRIFT_SHIFT && abs(dxq - 28) <= 3 && abs(dyq + 20) <= 3, "a view shifted 7,-5 px: SHIFT in quarter px (28,-20)");
    dm_render(60, 0, 0);
    usleep(300000);
    v = h->drift4(HOME_PAN, HOME_TILT, 0, &dxq, &dyq);
    CHECK(v != A2K_DRIFT_OK && v != A2K_DRIFT_SHIFT, "a shift beyond the range is no correction (recal / inconclusive)");
    /* a flat night view (IR, plain wall) against the day-only reference is no slip evidence: a skip, never FLAT (no re-home) */
    g_night = 1;
    {
        int x, y;
        pthread_mutex_lock(&fmu);
        for (y = 0; y < 192; y++)
            for (x = 0; x < 320; x++)
                fpl[0][y][x] = 128;
        pthread_mutex_unlock(&fmu);
    }
    usleep(300000);
    v = h->drift4(HOME_PAN, HOME_TILT, 0, &dxq, &dyq);
    CHECK(v == A2K_DRIFT_SKIP, "night: a flat view against a day-mode reference only is a skip, never FLAT (no false re-home)");
    a2k_scene_drift_ref_info(&caps, &bridges, &have_day, &have_night);
    CHECK(!have_night, "night: the flat view did not become a night reference");
    g_night = 0;
    dm_render(0, 0, 0);
    usleep(300000);
    /* the night mode with the day reference: the one reference works across modes; the proven view becomes the night reference (bridge) */
    g_night = 1;
    dm_render(0, 0, 1);
    usleep(300000);
    v = h->drift4(HOME_PAN, HOME_TILT, 0, &dxq, &dyq);
    CHECK(v == A2K_DRIFT_OK && abs(dxq) <= 12 && abs(dyq) <= 12, "night view at home against the day reference: zero within 3 px");
    a2k_scene_drift_ref_info(&caps, &bridges, &have_day, &have_night);
    CHECK(bridges == 1 && have_night, "the mode bridge kept the proven night view as the night reference");
    dm_render(-4, 6, 1);
    usleep(300000);
    v = h->drift4(HOME_PAN, HOME_TILT, 0, &dxq, &dyq);
    CHECK(v == A2K_DRIFT_SHIFT && abs(dxq + 16) <= 3 && abs(dyq - 24) <= 3, "night shift -4,6 px found with the night reference");
    g_probe_busy = 1;
    v = h->drift4(HOME_PAN, HOME_TILT, 0, &dxq, &dyq);
    CHECK(v == A2K_DRIFT_SKIP, "no drift check during an IR-off probe");
    g_probe_busy = 0;
    g_night = 0;
    /* a flat live view where the reference had detail (F3): FLAT only when a second frame agrees; a dark frame is a skip; a flat first frame that has structure again later is inconclusive */
    {
        int x, y;
        pthread_mutex_lock(&fmu);
        for (y = 0; y < 192; y++)
            for (x = 0; x < 320; x++)
                fpl[0][y][x] = 128;
        pthread_mutex_unlock(&fmu);
        usleep(300000);
        v = h->drift4(HOME_PAN, HOME_TILT, 0, &dxq, &dyq);
        CHECK(v == A2K_DRIFT_FLAT, "F3: a uniform grey view where the reference had detail, confirmed by a second frame: FLAT");
        pthread_mutex_lock(&fmu);
        for (y = 0; y < 192; y++)
            for (x = 0; x < 320; x++)
                fpl[0][y][x] = 3;
        pthread_mutex_unlock(&fmu);
        usleep(300000);
        v = h->drift4(HOME_PAN, HOME_TILT, 0, &dxq, &dyq);
        CHECK(v == A2K_DRIFT_SKIP, "F3: a dark flat view (lens covered, light off) is a skip, not a slip");
        f_run = 0;
        pthread_join(th, NULL);
        f_run = 1;
        dm_render(0, 0, 0);
        f_flip = 1; /* the feeder shows a flat frame for the first 250 ms after f_t0, then the scene again (a light that flickered) */
        f_t0 = mono_ms_t();
        pthread_create(&th, NULL, feeder_flip, NULL);
        usleep(100000);
        v = h->drift4(HOME_PAN, HOME_TILT, 0, &dxq, &dyq);
        CHECK(v == A2K_DRIFT_INCONC, "F3: a flat first frame that is not flat any more at the second one: inconclusive, never FLAT");
        /* F5: the drift reference has its own hooks: capture at a proven position, info (modes + age), drop on a calibration */
        {
            unsigned long age = 99;
            CHECK(h->dref_capture && h->dref_drop && h->dref_info, "F5: the hooks carry dref_capture / dref_drop / dref_info");
            usleep(400000);
            h->dref_drop();
            CHECK(h->dref_info(&age) == 0 && age == 0, "F5: dropped: no mode holds a reference");
            v = h->drift4(HOME_PAN, HOME_TILT, 0, &dxq, &dyq);
            CHECK(v == A2K_DRIFT_SKIP, "F5: no reference: the check skips");
            CHECK(h->dref_capture() == 0 && h->dref_info(&age) == 1 && age <= 2, "F5: dref_capture keeps the day reference, age in seconds");
            v = h->drift4(HOME_PAN, HOME_TILT, 0, &dxq, &dyq);
            CHECK(v == A2K_DRIFT_OK, "F5: and the check judges against it");
            /* F8: told to give way (a person arrived), the check waiting for a fresh frame returns at once instead of after its 4 s timeout */
            {
                int64_t t0;
                CHECK(h->set_abort != NULL, "F8: the hooks carry set_abort");
                h->set_abort(abort_yes);
                t0 = mono_ms_t();
                v = h->drift4(HOME_PAN, HOME_TILT, mono_ms_t() + 60000, &dxq, &dyq); /* no frame can be that new: it would wait the full 4 s */
                CHECK(v == A2K_DRIFT_SKIP && mono_ms_t() - t0 < 600, "F8: an abort request ends the wait for frames within 0.6 s (skip)");
                h->set_abort(NULL);
            }
            h->dref_drop();
            CHECK(h->dref_info(&age) == 0, "F5: dropped again");
        }
        f_flip = 0;
    }
    f_run = 0;
    pthread_join(th, NULL);
    a2k_scene_set_drift_ref(0);
    a2k_scene_test_set(path, 0, 0, 1, 2000);
}

/* The references are in the SENSOR frame (orient.h): with a mirror on, the frames the detection thread feeds are of the displayed frame and a2k_scene_feed converts them, and
 * it skips the settling window after a change. Behaviour of a2k_scene_feed, not a source grep. */
static void orient_tests(const a2k_ptz_scene_t *h)
{
    static uint8_t shown[192][320];
    char d2[220], p2[240];
    uint32_t pan = HOME_PAN, tilt = HOME_TILT;
    printf("== orientation: references in the sensor frame, frames of the displayed frame converted, settling window skipped\n");
    snprintf(d2, sizeof d2, "%s_orient", dir);
    mkdir(d2, 0755);
    snprintf(p2, sizeof p2, "%s/ptz.scene", d2);
    a2k_orient_test_reset();
    a2k_orient_test_clock(1000);
    a2k_scene_test_set(p2, 0, 0, 1, 2000);
    a2k_scene_test_clock(T0);
    scene(7, 0);
    feed();
    CHECK(h->capture() == 0 && h->commit(HOME_PAN, HOME_TILT, 1) == 0, "orientation off: the home reference is written (sensor frame)");
    CHECK(a2k_orient_plane(0, 1, &plane[0][0], 320, 192, 320, &shown[0][0]) == 0, "the mirrored picture the camera shows after the mount changed");
    a2k_scene_test_set(p2, 0, 0, 1, 2000); /* forgets the frame */
    a2k_orient_set(0, 1);
    a2k_scene_feed(&shown[0][0], 320, 192, 320);
    CHECK(h->capture() == -1, "inside the settling window the frame is skipped (no frame held)");
    a2k_orient_test_clock(1000 + A2K_ORIENT_SETTLE_MS + 1);
    a2k_scene_feed(&shown[0][0], 320, 192, 320);
    CHECK(h->boot_check(&pan, &tilt, NULL) == 1 && pan == HOME_PAN && tilt == HOME_TILT, "after the window: the displayed frame is converted back and matches the reference saved before the change");
    a2k_orient_test_reset();
    a2k_orient_test_clock(1000);
    a2k_scene_test_set(p2, 0, 0, 1, 2000);
    a2k_scene_feed(&shown[0][0], 320, 192, 320);
    pan = HOME_PAN;
    tilt = HOME_TILT;
    CHECK(h->boot_check(&pan, &tilt, NULL) != 1, "control: the same mirrored frame with the orientation off does NOT match (the conversion is what makes it match)");
    a2k_orient_test_reset();
    {
        char t2[260];
        unlink(p2);
        snprintf(t2, sizeof t2, "%s.tmp", p2);
        unlink(t2);
        rmdir(d2);
    }
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
    {
        /* any version of the format is dropped: the file of the previous version (scene1), the current one, a future one */
        static const char *ver[] = { "scene1 0 %u %u 80 48 3\n", "scene2 0 %u %u 80 48 3 0 0 900\n", "scene9 0 %u %u 80 48 3\n" };
        unsigned vi;
        for (vi = 0; vi < 3; vi++) {
            char hdr[120];
            size_t m;
            FILE *f = fopen(path, "wb");
            int hl = snprintf(hdr, sizeof hdr, ver[vi], (unsigned)HOME_PAN, (unsigned)HOME_TILT);
            if (f) {
                fwrite(hdr, 1, (size_t)hl, f);
                for (m = 0; m < 3840; m++)
                    fputc((int)(m * 7 % 251), f);
                fclose(f);
            }
            CHECK(h->drop() == 0, "drop of a reference file of another version: ok");
            d = slurp(&n);
            CHECK(d && !strncmp(d, "invalid", 7) && (vi == 2 || !strncmp(d, "invalid 3", 9)), "...invalidated (the boots count kept when the header is readable)");
        }
    }
    scene(7, 0);
    feed();
    CHECK(h->capture() == 0 && h->commit(HOME_PAN, HOME_TILT, 1) == 0, "a new reference is written again");
    if (geteuid() != 0) {
        chmod(dir, 0555); /* the directory cannot be written: the marker cannot be stored */
        CHECK(h->drop() == -1, "drop fails when the invalid marker cannot be written (reported, nothing is refused)");
        chmod(dir, 0755);
    } else {
        printf("  skip - running as root: a read-only directory is not enforced\n");
    }

    printf("== repetitive texture (blinds): never a single-view reference (the multi-view one is requested)\n");
    unlink(path); /* (with a recent reference on the flash the 24 h rule keeps it instead: capture 0) */
    {
        int x, y;
        for (y = 0; y < 192; y++)
            for (x = 0; x < 320; x++)
                plane[y][x] = (uint8_t)(((x / 4) % 7) < 3 ? 70 : 190); /* vertical bars, period 7 thumbnail px */
        feed();
        CHECK(h->capture() == 2 && h->commit(HOME_PAN, HOME_TILT, 0) == -1, "bars: no single-view reference: capture asks for the multi-view one (2), the plain commit refuses");
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

    printf("== a reference this boot just refused may be replaced once, whatever its age\n");
    {
        sc_ref_t cur;
        char buf2[4096];
        size_t k;
        FILE *f;
        a2k_scene_test_set(path, 0, 0, 1, 2000);
        a2k_scene_test_clock(T0 + 5 * DAY);
        g_night = 1;
        scene(7, 0);
        feed();
        CHECK(h->capture() == 0 && h->commit(1300, HOME_TILT, 1) == 0, "a reference (night, scene 7) is written");
        a2k_scene_test_set(path, 0, 0, 1, 2000);
        a2k_scene_test_clock(T0 + 5 * DAY + 3600); /* ...and is 1 h old */
        scene(99, 0);
        feed();
        pan = 1300;
        tilt = HOME_TILT;
        w = wr(h);
        CHECK(h->capture() == 0 && h->commit(1300, HOME_TILT, 1) == 1 && wr(h) == w, "no boot check refused it: the 24 h rule still blocks the rewrite");
        CHECK(h->boot_check(&pan, &tilt, NULL) == 0, "the boot check refuses the stored reference (the view differs)");
        w = wr(h);
        CHECK(h->capture() == 0 && h->commit(1300, HOME_TILT, 1) == 0 && wr(h) == w + 1, "...and the calibration's capture at home replaces it although it is 1 h old");
        d = slurp(&n);
        CHECK(d && sc_unpack(d, n, &r) == 0 && r.stamp == (uint32_t)(T0 + 5 * DAY + 3600), "...with the new stamp");
        scene(7, 0);
        feed();
        w = wr(h);
        CHECK(h->capture() == 0 && h->commit(1300, HOME_TILT, 1) == 1 && wr(h) == w, "a later change, right away: the one rewrite of this run is spent, the 24 h rule applies");
        /* a boot that matches does not unlock anything */
        a2k_scene_test_set(path, 0, 0, 1, 2000);
        a2k_scene_test_clock(T0 + 5 * DAY + 7200);
        scene(99, 0);
        feed();
        pan = 1300;
        tilt = HOME_TILT;
        CHECK(h->boot_check(&pan, &tilt, NULL) == 1, "boot check matches the new reference");
        scene(7, 0);
        feed();
        w = wr(h);
        CHECK(h->capture() == 0 && h->commit(1300, HOME_TILT, 0) == 1 && wr(h) == w, "a view change after a matching boot: still the 24 h rule");
        /* the repetitive verdict of an older file is stored back at the first boot (no screening at the next one) */
        d = slurp(&n);
        CHECK(d && sc_unpack(d, n, &cur) == 0, "reference readable");
        cur.rep = -1;
        cur.repmin = 0;
        k = sc_pack(&cur, buf2, sizeof buf2);
        f = fopen(path, "wb");
        fwrite(buf2, 1, k, f);
        fclose(f);
        scene(7, 0);
        feed();
        CHECK(h->boot_check(&pan, &tilt, NULL) == 0, "older file, the view differs: calibrate");
        d = slurp(&n);
        CHECK(d && sc_unpack(d, n, &cur) == 0 && cur.rep >= 0 && cur.repmin > 0, "...the screened verdict was stored back in the reference");
        g_night = 0;
    }

    /* ---- an AMBIGUOUS (repetitive / invariant) home view: the same multi-view mechanism, the home view is stored with its verdict ---- */
    printf("== multi-view reference: ambiguous (repetitive) rest view\n");
    unlink(path);
    a2k_scene_test_clock(T0 + 30 * DAY);
    {
        int x, y, i, dp, dt, ok = 1;
        sc_ref_t m;
        for (y = 0; y < 192; y++)
            for (x = 0; x < 320; x++)
                plane[y][x] = (uint8_t)(128 + 50 * ((((x % 32) < 16) ^ ((y % 32) < 16)) ? 1 : -1)); /* a tile lattice: a shift of a period would match */
        feed();
        CHECK(h->capture() == 2, "a repetitive rest view: no longer 'no reference': the multi-view reference is requested (2)");
        {
            /* the diagnosis dump (RAM only, next to the test reference here, /tmp/akam_scene_dbg on the camera): the thumbnail and one line with the reason */
            char dp[300];
            FILE *f;
            char line[700] = "";
            long sz = -1;
            snprintf(dp, sizeof dp, "%s.dbg/cap.gray", path);
            f = fopen(dp, "rb");
            if (f) {
                fseek(f, 0, SEEK_END);
                sz = ftell(f);
                fclose(f);
            }
            snprintf(dp, sizeof dp, "%s.dbg/cap.txt", path);
            f = fopen(dp, "rb");
            if (f) {
                if (!fgets(line, sizeof line, f))
                    line[0] = 0;
                fclose(f);
            }
            CHECK(sz == 3840 && strstr(line, "capture: ambiguous") && strstr(line, "repetitive 1 at") && strstr(line, "det "), "the capture dumped its thumbnail (3840 bytes) and a text line with the classification and the displacement that triggered the screening");
        }
        CHECK(h->commit(HOME_PAN, HOME_TILT, 1) == -1, "the plain commit refuses an ambiguous candidate");
        for (i = 0; i < 3; i++) {
            ok &= h->offset((unsigned)i, &dp, &dt) == 0;
            scene(31 + (unsigned)i, 0);
            feed();
            ok &= h->alt_capture((unsigned)i) == 0;
            scene(31 + (unsigned)i, 1);
            feed();
            ok &= h->alt_confirm((unsigned)i) == 0;
        }
        CHECK(ok, "informative offset views are kept and confirmed");
        for (y = 0; y < 192; y++)
            for (x = 0; x < 320; x++)
                plane[y][x] = (uint8_t)(128 + 50 * ((((x % 32) < 16) ^ ((y % 32) < 16)) ? 1 : -1));
        feed();
        CHECK(h->alt_capture(2) == 1, "an offset view that is itself repetitive is not kept (1)");
        CHECK(h->commit_multi(HOME_PAN, HOME_TILT, 1, 7) == 0, "multi-view reference written");
        d = slurp(&n);
        CHECK(d && sc_unpack(d, n, &m) == 0 && m.rep == 1 && m.repmin == SC_MIN_DEFAULT && m.pan == HOME_PAN, "the main file keeps the ambiguous home view with its verdict (rep 1)");
        /* the cold boot: the home view never decides, the probe is requested; a head at the offset sees its stored view */
        pan = HOME_PAN;
        tilt = HOME_TILT;
        feed(); /* the live rest view: the same lattice */
        CHECK(h->boot_check(&pan, &tilt, NULL) == 2 && pan == HOME_PAN && tilt == HOME_TILT, "boot: ambiguous saved view -> answer 2 (probe), nothing declared");
        {
            char dp[300];
            FILE *f;
            char line[700] = "";
            snprintf(dp, sizeof dp, "%s.dbg/ref.txt", path);
            f = fopen(dp, "rb");
            if (f) {
                if (!fgets(line, sizeof line, f))
                    line[0] = 0;
                fclose(f);
            }
            CHECK(strstr(line, "boot (ambiguous reference: probe)") && strstr(line, "verdict"), "the boot check dumped the reference thumbnail and its verdict");
        }
        CHECK(h->probe_offset(0, &dp, &dt) == 0 && h->probe_offset(1, &dp, &dt) == 0 && h->probe_offset(2, &dp, &dt) == 0, "the stored offset views are usable");
        scene(32, 1);
        feed();
        CHECK(h->probe(1) == 1, "the live view at offset 1 matches its stored view: the position is proven");
        scene(99, 0);
        feed();
        CHECK(h->probe(1) == 0, "a moved head (another view at the offset): refused");
        /* no usable offset view: refused as before, at once */
        unlink(path);
        {
            char p2[260];
            snprintf(p2, sizeof p2, "%s.a0", path);
            unlink(p2);
            snprintf(p2, sizeof p2, "%s.a1", path);
            unlink(p2);
            snprintf(p2, sizeof p2, "%s.a2", path);
            unlink(p2);
        }
    }
    drift_tests(h);
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
    drift_ref_tests(h);
    orient_tests(h);
    printf("== boot scene threshold: default 0.85, akam.conf key, API row overrides it\n");
    CHECK(SC_MIN_DEFAULT == 850 && a2k_scene_get_min() == 850, "the default threshold is 0.85");
    a2k_scene_set_min(900);
    CHECK(a2k_scene_get_min() == 900, "akam.conf ptz_scene_min 0.90 still works while the API row was never set");
    CHECK(a2k_scene_set_min_api(799) == -1 && a2k_scene_set_min_api(991) == -1 && a2k_scene_get_min() == 900, "the API row refuses below 0.80 and above 0.99 and keeps the value");
    CHECK(a2k_scene_set_min_api(870) == 0 && a2k_scene_get_min() == 870, "the API row sets 0.87");
    a2k_scene_set_min(900);
    CHECK(a2k_scene_get_min() == 870, "once set through the API, the akam.conf key no longer overrides it");
    printf("rot2k scene glue unit: %d failed\n", fails);
    return fails ? 1 : 0;
}
