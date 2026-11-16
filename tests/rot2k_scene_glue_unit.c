/* Host unit test of the runtime side of the scene check (app/akam2k_scene.c, with the real settings store): the thumbnails fed by the detection
 * thread, the two-step reference write (capture, then commit only if the view is unchanged), the matched-boots counter, the invalidation (and its
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

int main(void)
{
    const a2k_ptz_scene_t *h = a2k_scene_hooks();
    uint32_t pan = 0, tilt = 0;
    size_t n;
    char *d;
    sc_ref_t r;
    snprintf(dir, sizeof dir, "/tmp/rot2k_scene_glue_%d", (int)getpid());
    mkdir(dir, 0755);
    snprintf(path, sizeof path, "%s/ptz.scene", dir);
    a2k_scene_test_set(path, 0, 0, 1, 2000);

    printf("== capture and commit\n");
    CHECK(h->capture() == -1, "no frame fed yet: capture refused");
    scene(7, 0);
    feed();
    g_night_valid = 0;
    CHECK(h->capture() == -1, "day/night mode unknown: capture refused");
    g_night_valid = 1;
    CHECK(h->capture() == 0, "textured view, mode known: candidate taken");
    scene(7, 1);
    feed(); /* the same scene, sensor noise */
    CHECK(h->commit(1500, 900, 1) == 0, "the view is still the candidate: written");
    d = slurp(&n);
    CHECK(d && n > 3840 && n < 5000 && sc_unpack(d, n, &r) == 0 && r.pan == 1500 && r.tilt == 900 && r.mode == 0 && r.boots == 0, "file: unpacks, position, mode, boots 0, below 5 KB");

    printf("== the view changes during the confirmation window\n");
    scene(7, 0);
    feed();
    CHECK(h->capture() == 0, "candidate of scene 7");
    scene(99, 0);
    feed(); /* somebody moved the head (or the scene): another view */
    CHECK(h->commit(1600, 900, 0) == -1, "view changed: commit refused");
    d = slurp(&n);
    CHECK(d && sc_unpack(d, n, &r) == 0 && r.pan == 1500, "the file still holds the previous (consistent) reference");
    scene(7, 0);
    feed();
    g_night = 1; /* day -> night during the window */
    CHECK(h->commit(1600, 900, 0) == -1, "day/night mode changed during the window: refused");
    g_night = 0;

    printf("== low detail, fresh flag, boots counter\n");
    memset(plane, 120, sizeof plane);
    feed();
    CHECK(h->capture() == 1, "a uniform view: nothing to save (1), no candidate");
    CHECK(h->commit(1600, 900, 0) == -1, "...and a commit without a candidate is refused");
    scene(7, 0);
    feed();
    CHECK(h->capture() == 0 && h->commit(1500, 900, 0) == 0, "re-captured and written (not fresh)");
    d = slurp(&n);
    CHECK(d && sc_unpack(d, n, &r) == 0 && r.boots == 0, "boots 0 so far");
    /* a boot match bumps the counter in the file; the next write carries it on unless a calibration ran */
    a2k_scene_test_set(path, 0, 0, 1, 2000);
    scene(7, 1);
    feed();
    CHECK(h->boot_check(&pan, &tilt, NULL) == 1 && pan == 1500 && tilt == 900, "boot check: match with the saved scene");
    d = slurp(&n);
    CHECK(d && sc_unpack(d, n, &r) == 0 && r.boots == 1, "...the counter in the file is now 1");
    scene(7, 0);
    feed();
    CHECK(h->capture() == 0 && h->commit(1500, 900, 0) == 0, "a later write after a motion (no calibration)");
    d = slurp(&n);
    CHECK(d && sc_unpack(d, n, &r) == 0 && r.boots == 1, "...carries the counter on (1)");
    scene(7, 0);
    feed();
    CHECK(h->capture() == 0 && h->commit(1275, 1300, 1) == 0, "a write after a full calibration (fresh)");
    d = slurp(&n);
    CHECK(d && sc_unpack(d, n, &r) == 0 && r.boots == 0, "...restarts the counter at 0");
    /* different scene at boot: no match, counter forgotten */
    scene(99, 0);
    feed();
    CHECK(h->boot_check(&pan, &tilt, NULL) == 0, "boot check: another scene, no match");

    printf("== the matched-boots count survives a process restart (adoption)\n");
    scene(7, 0);
    feed();
    CHECK(h->capture() == 0 && h->commit(1500, 900, 1) == 0, "reference written after a calibration (boots 0)");
    for (int k = 0; k < 4; k++) {
        scene(7, 1);
        feed();
        CHECK(h->boot_check(&pan, &tilt, NULL) == 1, "boot match");
    }
    a2k_scene_test_set(path, 0, 0, 1, 2000); /* a new process: nothing in memory */
    scene(7, 0);
    feed();
    CHECK(h->capture() == 0 && h->commit(1500, 900, 0) == 0, "after an adoption (no calibration ran): a write");
    d = slurp(&n);
    CHECK(d && sc_unpack(d, n, &r) == 0 && r.boots == 4, "...keeps the on-disk count (4): the periodic calibration is not postponed");
    CHECK(h->drop() == 0, "a motion invalidates the reference");
    d = slurp(&n);
    CHECK(d && n > 8 && !strncmp(d, "invalid 4", 9), "...the marker remembers the count");
    a2k_scene_test_set(path, 0, 0, 1, 2000); /* the app restarts again */
    scene(7, 0);
    feed();
    CHECK(h->capture() == 0 && h->commit(1500, 900, 0) == 0, "a write after the restart");
    d = slurp(&n);
    CHECK(d && sc_unpack(d, n, &r) == 0 && r.boots == 4, "...count still 4");
    a2k_scene_test_set(path, 0, 0, 1, 2000);
    scene(7, 0);
    feed();
    CHECK(h->capture() == 0 && h->commit(1500, 900, 1) == 0, "a write after a full calibration (fresh)");
    d = slurp(&n);
    CHECK(d && sc_unpack(d, n, &r) == 0 && r.boots == 0, "...restarts the count");

    printf("== invalidation\n");
    CHECK(h->drop() == 0, "drop: ok");
    d = slurp(&n);
    CHECK(d && n >= strlen(sc_invalid_marker) - 1 && !strncmp(d, "invalid", 7), "the reference is an invalid marker now");
    CHECK(h->drop() == 0, "drop again: nothing to do");
    CHECK(h->boot_check(&pan, &tilt, NULL) == 0, "an invalidated reference never matches");
    scene(7, 0);
    feed();
    CHECK(h->capture() == 0 && h->commit(1500, 900, 1) == 0, "a new reference is written");
    if (geteuid() != 0) {
        chmod(dir, 0555); /* the directory cannot be written: the marker cannot be stored */
        CHECK(h->drop() == -1, "drop fails when the invalid marker cannot be written (the caller refuses to move)");
        chmod(dir, 0755);
    } else {
        printf("  skip - running as root: a read-only directory is not enforced\n");
    }
    unlink(path);
    {
        char tmp[240];
        snprintf(tmp, sizeof tmp, "%s.tmp", path);
        unlink(tmp);
    }
    rmdir(dir);
    printf("rot2k scene glue unit: %d failed\n", fails);
    return fails ? 1 : 0;
}
