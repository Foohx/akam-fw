/* Host unit test of the motion detection (src/features/motion/motion_grid.c, firmware motion.c) and of its use by the tracker's lost-target search (track_logic.c, TL_SEARCH_MOTION).
 *  - the grid: noise, a moving blob gives the centroid and a direction (left / right / up / down) with hysteresis, the direction survives a short stop, masks (head moving, global change),
 *  - frame differencing on luma planes: a moving square, brightness steps (day / night, exposure) and size changes / restarts never make a false motion,
 *  - the four orientations (none, flip, mirror, ceiling): the vendor grid in the SENSOR frame is converted to the PICTURE frame, the same physical motion gives the right picture direction,
 *    and the tracker's look / steer moves the same PHYSICAL way (the motor sign follows the orientation-aware invert flags); an orientation change in the middle of a motion makes no false motion,
 *  - the search itself: one short look toward the exit direction, a passer-by gets one look and goes home early, a cooldown, no search without evidence, the steer toward new motion, no move near a soft limit.
 * Build: tests/rot2k_motion_test.sh. Prints "rot2k motion unit: N failed". */
#define _GNU_SOURCE
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "motion.h"
#include "motion_grid.h"
#include "orient.h"
#include "ptz.h"
#include "track_logic.h"

static int fails;
#define CHECK(c, msg) do { if (c) printf("  ok   - %s\n", msg); else { printf("  FAIL - %s (line %d)\n", msg, __LINE__); fails++; } } while (0)

void a2k_log(const char *fmt, ...) { (void)fmt; }
static a2k_ptz_info_t g_pi;
void a2k_ptz_info(a2k_ptz_info_t *o) { *o = g_pi; }

#define FW 320
#define FH 192

/* ---------------------------------------------------------------- the grid */
static void blob(uint8_t *act, int cx, int cy, int r)
{
    memset(act, 0, MG_N);
    for (int y = cy - r; y <= cy + r; y++)
        for (int x = cx - r; x <= cx + r; x++)
            if (x >= 0 && x < MG_W && y >= 0 && y < MG_H) act[y * MG_W + x] = 255;
}

static int run_blob(mg_t *m, int64_t *T, int x0, int y0, int dx, int dy, int n)
{
    uint8_t a[MG_N];
    for (int i = 0; i < n; i++) {
        blob(a, x0 + dx * i, y0 + dy * i, 1);
        mg_feed_cells(m, *T, a);
        *T += 200;
    }
    return mg_get(m)->dir;
}

static void grid_cases(void)
{
    mg_t m;
    int64_t T = 1000;
    uint8_t a[MG_N];
    printf("== the grid: presence, centroid, direction\n");
    mg_init(&m, NULL);
    memset(a, 0, sizeof a);
    a[5] = 255;
    mg_feed_cells(&m, T, a);
    CHECK(!mg_get(&m)->active, "one isolated cell is noise");
    CHECK(run_blob(&m, &T, 4, 8, 1, 0, 6) == MG_DIR_RIGHT && mg_get(&m)->active, "a blob moving right: active, direction right");
    CHECK(mg_get(&m)->cx > 500 * 0 && mg_get(&m)->x1 >= mg_get(&m)->x0 && mg_get(&m)->cells == 9, "centroid and bounding box of the 3x3 blob");
    CHECK(mg_get(&m)->cx == (9 * 1000 + 500) / MG_W, "centroid x is the cell centre in permille");
    mg_init(&m, NULL); T = 1000;
    CHECK(run_blob(&m, &T, 18, 8, -1, 0, 6) == MG_DIR_LEFT, "a blob moving left: direction left");
    mg_init(&m, NULL); T = 1000;
    CHECK(run_blob(&m, &T, 10, 4, 0, 1, 6) == MG_DIR_DOWN, "a blob moving down: direction down");
    mg_init(&m, NULL); T = 1000;
    CHECK(run_blob(&m, &T, 10, 11, 0, -1, 6) == MG_DIR_UP, "a blob moving up: direction up");
    mg_init(&m, NULL); T = 1000;
    CHECK(run_blob(&m, &T, 10, 8, 0, 0, 8) == MG_DIR_NONE && mg_get(&m)->active, "a blob that does not move: motion yes, direction none");
    /* hysteresis: a slow drift keeps the direction once set, jitter does not flip it */
    mg_init(&m, NULL); T = 1000;
    (void)run_blob(&m, &T, 4, 8, 1, 0, 6);
    blob(a, 11, 8, 1); mg_feed_cells(&m, T, a); T += 200;
    blob(a, 11, 8, 1); mg_feed_cells(&m, T, a); T += 200;
    CHECK(mg_get(&m)->dir == MG_DIR_RIGHT || mg_get(&m)->dir == MG_DIR_NONE, "after the blob stops the direction never turns around");
    blob(a, 12, 9, 1); mg_feed_cells(&m, T, a); T += 200;
    CHECK(mg_get(&m)->dir != MG_DIR_LEFT, "jitter of one cell does not flip it");
    /* the direction is held after the motion ends (the tracker reads it at the loss), then dropped */
    mg_init(&m, NULL); T = 1000;
    (void)run_blob(&m, &T, 4, 8, 1, 0, 6);
    memset(a, 0, sizeof a);
    mg_feed_cells(&m, T, a);
    CHECK(!mg_get(&m)->active && mg_get(&m)->dir == MG_DIR_RIGHT && mg_get(&m)->last_dir == MG_DIR_RIGHT, "motion ended: no activity, direction held for a moment");
    T += 1500; mg_feed_cells(&m, T, a);
    CHECK(mg_get(&m)->dir == MG_DIR_NONE && mg_get(&m)->last_dir == MG_DIR_RIGHT && mg_get(&m)->dir_ms > 0, "1.5 s later the direction is none, the last one is remembered");
    /* masks */
    mg_init(&m, NULL); T = 1000;
    mg_mask(&m, T, 700);
    blob(a, 8, 8, 2);
    mg_feed_cells(&m, T + 200, a);
    CHECK(!mg_get(&m)->active && mg_get(&m)->masked, "head moving (mask): the blob is ignored");
    mg_feed_cells(&m, T + 900, a);
    CHECK(mg_get(&m)->active && !mg_get(&m)->masked, "after the mask the same blob counts");
    mg_init(&m, NULL); T = 1000;
    (void)run_blob(&m, &T, 4, 8, 1, 0, 3);
    mg_mask(&m, T, 700);
    T += 800;
    (void)run_blob(&m, &T, 9, 8, 0, 0, 1);
    CHECK(mg_get(&m)->dir == MG_DIR_NONE, "a mask forgets the history: no direction from the positions before it");
    mg_init(&m, NULL); T = 1000;
    memset(a, 255, sizeof a);
    mg_feed_cells(&m, T, a);
    CHECK(!mg_get(&m)->active && mg_get(&m)->masked, "every cell moving at once = a global change: no motion");
    blob(a, 8, 8, 2);
    mg_feed_cells(&m, T + 1000, a);
    CHECK(!mg_get(&m)->active, "... and it stays masked for 2 s");
    mg_feed_cells(&m, T + 2500, a);
    CHECK(mg_get(&m)->active, "... then real motion counts again");
}

/* ---------------------------------------------------------------- frame differencing */
static uint8_t g_y[FW * FH];
static void plane(int bg, int sq_x, int sq_y, int sq, int val)
{
    memset(g_y, bg, sizeof g_y);
    for (int y = sq_y; y < sq_y + sq && y < FH; y++)
        for (int x = sq_x; x < sq_x + sq && x < FW; x++)
            if (x >= 0 && y >= 0) g_y[y * FW + x] = (uint8_t)val;
}

static void diff_cases(void)
{
    mg_t m;
    int64_t T = 1000;
    printf("== frame differencing: a moving square, brightness steps, sizes\n");
    mg_init(&m, NULL);
    plane(90, 0, 0, 0, 0);
    mg_feed_diff(&m, T, g_y, FW, FH, FW); T += 200;
    for (int i = 0; i < 6; i++) { plane(90, 20 + i * 30, 70, 50, 200); mg_feed_diff(&m, T, g_y, FW, FH, FW); T += 200; }
    CHECK(mg_get(&m)->active && mg_get(&m)->dir == MG_DIR_RIGHT, "a bright square moving right across the plane: motion, direction right");
    CHECK(mg_get(&m)->cx > 300 && mg_get(&m)->cx < 800, "its centroid is in the picture where the square is");
    mg_init(&m, NULL); T = 1000;
    plane(90, 0, 0, 0, 0);
    for (int i = 0; i < 10; i++) { mg_feed_diff(&m, T, g_y, FW, FH, FW); T += 200; }
    CHECK(!mg_get(&m)->active, "a static scene: no motion");
    plane(140, 0, 0, 0, 0); /* a day / night or exposure step: every pixel changes */
    mg_feed_diff(&m, T, g_y, FW, FH, FW); T += 200;
    CHECK(!mg_get(&m)->active, "a brightness step of the whole picture: no motion");
    plane(140, 0, 0, 0, 0);
    mg_feed_diff(&m, T, g_y, FW, FH, FW); T += 200;
    CHECK(!mg_get(&m)->active, "... and the frame after it neither");
    /* a slow exposure drift of a few levels per frame is absorbed by the global shift */
    mg_init(&m, NULL); T = 1000;
    for (int i = 0; i < 12; i++) { plane(90 + i * 2, 0, 0, 0, 0); mg_feed_diff(&m, T, g_y, FW, FH, FW); T += 200; if (i > 1) CHECK(!mg_get(&m)->active, "a gentle exposure drift: no motion"); }
    /* a size change (resolution switch) and a restart: the first frame after is only a reference */
    mg_init(&m, NULL); T = 1000;
    plane(90, 0, 0, 0, 0);
    for (int i = 0; i < 3; i++) { mg_feed_diff(&m, T, g_y, FW, FH, FW); T += 200; }
    {
        static uint8_t big[640 * 384];
        memset(big, 200, sizeof big);
        mg_feed_diff(&m, T, big, 640, 384, 640); T += 200;
        CHECK(!mg_get(&m)->active, "the plane changes size: no false motion");
        mg_feed_diff(&m, T, big, 640, 384, 640);
        CHECK(!mg_get(&m)->active, "... and nothing after it");
    }
    mg_init(&m, NULL); T = 1000;
    plane(90, 0, 0, 0, 0);
    mg_feed_diff(&m, T, g_y, FW, FH, FW);
    mg_reset(&m);
    plane(90, 100, 60, 60, 220);
    mg_feed_diff(&m, T + 6000, g_y, FW, FH, FW);
    CHECK(!mg_get(&m)->active, "after mg_reset (a stream restart) the first frame is a reference, not a motion");
}

/* ---------------------------------------------------------------- the vendor statistics, the four orientations */
static uint32_t g_md[MG_N];
static int g_md_rc;
static int md_reader(void *buf, unsigned n) { (void)n; memcpy(buf, g_md, sizeof g_md); return g_md_rc; }

static void md_fill(int bx, int by, int on) /* a 3x3 blob in the SENSOR frame grid, background 1000 with a little noise */
{
    for (int i = 0; i < MG_N; i++) g_md[i] = 1000 + (uint32_t)((i * 7) % 5);
    if (on)
        for (int y = by - 1; y <= by + 1; y++)
            for (int x = bx - 1; x <= bx + 1; x++)
                if (x >= 0 && x < MG_W && y >= 0 && y < MG_H) g_md[y * MG_W + x] = 6000;
}

static int g_ev_n, g_last_dir, g_last_active;
static void notify(const struct a2k_motion_info *i) { g_ev_n++; g_last_dir = i->dir; g_last_active = i->active; }
static struct tl_mot g_mot;
static void sink(const struct tl_mot *m) { g_mot = *m; }

/* feed frames of the detection channel (the luma content is flat: the MD source decides) */
static void frames(int64_t *T, int n, int bx, int by, int dx, int dy, int on)
{
    static uint8_t y[FW * FH];
    memset(y, 100, sizeof y);
    for (int i = 0; i < n; i++) {
        md_fill(bx + dx * i, by + dy * i, on);
        a2k_orient_test_clock(*T);
        a2k_motion_test_clock(*T);
        a2k_motion_frame(y, FW, FH, FW);
        *T += 200;
    }
}

static void md_orient_cases(void)
{
    static const char *const nm[4] = { "none", "flip", "mirror", "ceiling" };
    printf("== the vendor statistics in the SENSOR frame, the four orientations\n");
    for (int o = 0; o < 4; o++) {
        int flip = o & 1, mirror = o >> 1, exp_h, exp_v;
        int64_t T = 100000;
        char msg[160];
        a2k_orient_test_reset();
        a2k_motion_test_reset();
        a2k_orient_test_clock(T);
        a2k_orient_set(flip, mirror);
        memset(&g_pi, 0, sizeof g_pi);
        g_pi.known = 1; g_pi.pan = 1275; g_pi.tilt = 1300;
        a2k_motion_init(1, 5, A2K_MOTION_MD);
        a2k_motion_set_md_frame(0); /* the grid is in the sensor frame */
        a2k_motion_set_md_reader(md_reader);
        a2k_motion_set_sink(sink);
        a2k_motion_set_notify(notify);
        g_md_rc = 0;
        frames(&T, 25, 0, 0, 0, 0, 0);                 /* 5 s of a quiet scene: the orientation settled, the reference learned */
        g_ev_n = 0;
        frames(&T, 6, 4, 8, 1, 0, 1);                  /* physical motion toward the sensor's right */
        exp_h = mirror ? MG_DIR_LEFT : MG_DIR_RIGHT;
        snprintf(msg, sizeof msg, "%s: physical motion to the sensor right is %s in the picture", nm[o], mirror ? "left" : "right");
        CHECK(g_mot.active && g_mot.last_dir == exp_h, msg);
        {
            struct a2k_motion_info mi;
            a2k_motion_info(&mi);
            CHECK(mi.active && mi.dir == exp_h && mi.source == A2K_MOTION_MD, "state row: active, direction in the picture, source md");
            CHECK(strcmp(mg_dir_name(mi.dir), mirror ? "left" : "right") == 0 && g_last_active == 1 && g_ev_n >= 1, "the notification carries the picture direction");
        }
        frames(&T, 25, 0, 0, 0, 0, 0);
        frames(&T, 6, 10, 4, 0, 1, 1);                 /* physical motion toward the sensor's bottom */
        exp_v = flip ? MG_DIR_UP : MG_DIR_DOWN;
        snprintf(msg, sizeof msg, "%s: physical motion to the sensor bottom is %s in the picture", nm[o], flip ? "up" : "down");
        CHECK(g_mot.active && g_mot.last_dir == exp_v, msg);
        /* the centroid is in the picture frame too: a blob at the sensor's left edge */
        frames(&T, 25, 0, 0, 0, 0, 0);
        frames(&T, 4, 3, 8, 0, 0, 1);
        CHECK(mirror ? g_mot.cx > 700 : g_mot.cx < 300, "the centroid is mirrored with the picture");
    }
    /* the grid in the PICTURE frame (the default) is not converted */
    {
        int64_t T = 100000;
        a2k_orient_test_reset();
        a2k_motion_test_reset();
        a2k_orient_test_clock(T);
        a2k_orient_set(1, 1);
        a2k_motion_init(1, 5, A2K_MOTION_MD);
        a2k_motion_set_md_frame(1);
        a2k_motion_set_md_reader(md_reader);
        a2k_motion_set_sink(sink);
        frames(&T, 25, 0, 0, 0, 0, 0);
        frames(&T, 6, 4, 8, 1, 0, 1);
        CHECK(g_mot.active && g_mot.last_dir == MG_DIR_RIGHT, "ceiling, grid already in the picture frame: right stays right");
    }
}

static void md_health_cases(void)
{
    int64_t T = 100000;
    struct a2k_motion_info mi;
    printf("== the vendor source: unusable statistics fall back to frame differencing\n");
    a2k_orient_test_reset();
    a2k_motion_test_reset();
    a2k_orient_test_clock(T);
    memset(&g_pi, 0, sizeof g_pi);
    g_pi.known = 1; g_pi.pan = 1275; g_pi.tilt = 1300;
    a2k_motion_init(1, 5, A2K_MOTION_MD);
    a2k_motion_set_md_reader(md_reader);
    a2k_motion_set_sink(sink);
    memset(g_md, 0, sizeof g_md);
    g_md_rc = 0;
    {
        static uint8_t y[FW * FH];
        memset(y, 100, sizeof y);
        for (int i = 0; i < 130; i++) { a2k_orient_test_clock(T); a2k_motion_test_clock(T); a2k_motion_frame(y, FW, FH, FW); T += 200; }
    }
    CHECK(strcmp(a2k_motion_source_name(), "md>diff") == 0, "statistics all zero for 100 frames: md is unusable, the source reads md>diff");
    {   /* diff keeps working: a moving square on the luma plane */
        static uint8_t y[FW * FH];
        for (int i = 0; i < 6; i++) {
            memset(y, 90, sizeof y);
            for (int yy = 70; yy < 120; yy++) for (int xx = 20 + i * 30; xx < 70 + i * 30; xx++) y[yy * FW + xx] = 210;
            a2k_orient_test_clock(T); a2k_motion_test_clock(T); a2k_motion_frame(y, FW, FH, FW); T += 200;
        }
        a2k_motion_info(&mi);
        CHECK(mi.active && mi.dir == MG_DIR_RIGHT && mi.source == A2K_MOTION_DIFF, "... and the diff source sees the square move right");
    }
    a2k_motion_set_enabled(0);
    a2k_motion_info(&mi);
    CHECK(!mi.enabled && !mi.active && mi.dir == 0 && mi.x == -1, "motion.enabled 0: the state falls back to inactive");
    a2k_motion_set_enabled(1);
    a2k_motion_set_sensitivity(99);
    CHECK(a2k_motion_get_sensitivity() == 10, "the sensitivity is clamped to 1..10");
}

/* ---------------------------------------------------------------- an orientation change in the middle of a motion, the head moving, a restart (diff source) */
static void diff_wiring_cases(void)
{
    int64_t T = 100000;
    static uint8_t y[FW * FH];
    struct a2k_motion_info mi;
    printf("== the diff source in the app: orientation change, head moving, stream restart\n");
    a2k_orient_test_reset();
    a2k_motion_test_reset();
    a2k_orient_test_clock(T);
    memset(&g_pi, 0, sizeof g_pi);
    g_pi.known = 1; g_pi.pan = 1275; g_pi.tilt = 1300;
    a2k_motion_init(1, 5, A2K_MOTION_DIFF);
    a2k_motion_set_sink(sink);
    for (int i = 0; i < 8; i++) {   /* a square walks right */
        memset(y, 90, sizeof y);
        for (int yy = 70; yy < 120; yy++) for (int xx = 10 + i * 30; xx < 60 + i * 30; xx++) y[yy * FW + xx] = 210;
        a2k_orient_test_clock(T); a2k_motion_test_clock(T); a2k_motion_frame(y, FW, FH, FW); T += 200;
    }
    a2k_motion_info(&mi);
    CHECK(mi.active && mi.dir == MG_DIR_RIGHT, "before the change: right");
    /* the user turns the mirror on: the picture is mirrored, the same physical walk now goes left, with a jump of the square */
    a2k_orient_set(0, 1);
    for (int i = 0; i < 8; i++) {
        int x0 = FW - (10 + i * 30) - 50;
        memset(y, 90, sizeof y);
        for (int yy = 70; yy < 120; yy++) for (int xx = x0 < 0 ? 0 : x0; xx < x0 + 50 && xx < FW; xx++) y[yy * FW + xx] = 210;
        a2k_orient_test_clock(T); a2k_motion_test_clock(T); a2k_motion_frame(y, FW, FH, FW); T += 200;
        a2k_motion_info(&mi);
        CHECK(!mi.active && !g_mot.active && mi.dir == 0, "frames right after an orientation change: no motion, no direction");
        if (i == 6) break;
    }
    T += 3500;
    for (int i = 0; i < 8; i++) {
        int x0 = 250 - i * 30;
        memset(y, 90, sizeof y);
        for (int yy = 70; yy < 120; yy++) for (int xx = x0 < 0 ? 0 : x0; xx < x0 + 50 && xx < FW; xx++) y[yy * FW + xx] = 210;
        a2k_orient_test_clock(T); a2k_motion_test_clock(T); a2k_motion_frame(y, FW, FH, FW); T += 200;
    }
    a2k_motion_info(&mi);
    CHECK(mi.active && mi.dir == MG_DIR_LEFT, "after the settle window the mirrored picture's motion is read as left");
    /* the head moves: the position counters change, everything shifts, nothing counts */
    for (int i = 0; i < 5; i++) {
        memset(y, 90, sizeof y);
        for (int yy = 0; yy < FH; yy++) for (int xx = 0; xx < FW; xx++) y[yy * FW + xx] = (uint8_t)(60 + ((xx + i * 25) / 20 % 2) * 120);
        g_pi.pan += 40;
        a2k_orient_test_clock(T); a2k_motion_test_clock(T); a2k_motion_frame(y, FW, FH, FW); T += 200;
        CHECK(!g_mot.active, "the head is moving: the shifting picture is no motion");
    }
    /* a stream restart: a long gap, then another content */
    T += 5000;
    memset(y, 150, sizeof y);
    a2k_orient_test_clock(T); a2k_motion_test_clock(T); a2k_motion_frame(y, FW, FH, FW);
    a2k_motion_info(&mi);
    CHECK(!mi.active, "after a gap in the frames (restart) the first frame is only a reference");
}

/* ---------------------------------------------------------------- the search */
static struct tl_ptz ptz(int pan, int tilt)
{
    struct tl_ptz p;
    memset(&p, 0, sizeof p);
    p.pan = pan; p.tilt = tilt; p.pan_min = 300; p.pan_max = 3650; p.tilt_min = 300; p.tilt_max = 2100; p.tilt_home = 1300;
    return p;
}

struct run {
    tl_t t;
    struct tl_cfg c;
    struct tl_ptz p;
    int64_t T;
    struct tl_mot mot;
    int moves, pan_dir, tilt_dir, first_pan, first_tilt, homes;
    int64_t first_ms;
    int was;
};

static void run_init(struct run *r, int search, int flip, int mirror)
{
    memset(r, 0, sizeof *r);
    tl_cfg_defaults(&r->c);
    r->c.motion = TL_MOTION_CONT;
    r->c.invert_pan = a2k_orient_invert_pan(1, mirror);
    r->c.invert_tilt = a2k_orient_invert_tilt(0, flip);
    r->c.tilt_enable = 1;
    r->c.return_home = 1;
    r->c.search_mode = search ? TL_SEARCH_MOTION : TL_SEARCH_LEGACY;
    tl_init(&r->t, &r->c);
    r->p = ptz(1275, 1300);
    r->T = 1000;
    r->mot.valid = 1;
}

/* one tick; the fake head stays where it is (the policy only needs the pose); counts the moves (a start = the previous tick had no velocity command) */
static struct tl_out step(struct run *r, const struct tl_box *b)
{
    struct tl_out o;
    r->p.cap_ms = r->T - 100;
    tl_set_motion(&r->t, &r->mot);
    tl_update(&r->t, r->T, b, b ? 1 : 0, FW, FH, &r->p, &o);
    /* a move = a velocity command of the SEARCH (the follow and the prediction of the gap are not searches) */
    if (o.kind == TL_VEL && (o.pan_dir || o.tilt_dir) && strcmp(r->t.dbg.why, "vel") && strcmp(r->t.dbg.why, "predict")) {
        if (!r->was) { r->moves++; if (!r->first_ms) { r->first_ms = r->T; r->first_pan = o.pan_dir; r->first_tilt = o.tilt_dir; } }
        r->was = 1;
    } else {
        r->was = 0;
    }
    if (o.kind == TL_HOME) r->homes++;
    r->p.has_cap = 1; r->p.cap_pan = r->p.pan; r->p.cap_tilt = r->p.tilt;
    r->T += 200;
    return o;
}

/* the person is followed for `follow_ms` walking to an edge, then lost; the run goes on for `after_ms`. edge: 0 right, 1 left, 2 top */
static void walk_off(struct run *r, int edge, int follow_ms, int after_ms)
{
    int n = follow_ms / 200;
    for (int i = 0; i < n; i++) {
        int k = i * 100 / (n > 1 ? n - 1 : 1), cx = 160 + (edge == 0 ? k * 135 / 100 : edge == 1 ? -k * 135 / 100 : 0);
        struct tl_box b = { cx - 22, 48, cx + 22, 150, 0, 0 };
        if (edge == 2) { b.x1 = 140; b.x2 = 190; b.y1 = 0; b.y2 = 90 + (k > 50 ? 0 : 0); }
        if (edge == 0) { b.x2 = i == n - 1 ? FW - 1 : b.x2 > FW - 1 ? FW - 1 : b.x2; }
        if (edge == 1) { b.x1 = i == n - 1 ? 0 : b.x1 < 0 ? 0 : b.x1; }
        (void)step(r, &b);
    }
    r->moves = 0; r->first_ms = 0; r->homes = 0; r->was = 0;
    for (int i = 0; i < after_ms / 200; i++) (void)step(r, NULL);
}

static void search_cases(void)
{
    struct run r, l;
    printf("== the lost-target search\n");
    /* legacy for reference: it does its exit search AND the later scans / sweeps / home; the motion search: one look */
    run_init(&l, 0, 0, 0); walk_off(&l, 0, 6000, 60000);
    run_init(&r, 1, 0, 0); walk_off(&r, 0, 6000, 60000);
    CHECK(r.moves >= 1 && r.moves <= 2, "a person who left through the right edge: one short look (at most two moves), then nothing");
    CHECK(l.moves > r.moves, "the legacy policy moves more in the same minute");
    CHECK(r.first_ms > 0 && r.first_pan != 0 && r.first_tilt == 0, "the look is a pan toward the exit side");
    /* passer-by: followed briefly, left by the edge: one look and home early */
    run_init(&r, 1, 0, 0); walk_off(&r, 0, 2000, 20000);
    CHECK(r.moves == 1 && r.homes >= 1, "passer-by (followed 2 s): one look, then home within the 20 s");
    run_init(&l, 0, 0, 0); walk_off(&l, 0, 2000, 20000);
    CHECK(l.homes == 0, "(legacy waits the 30 s return delay: no home yet)");
    /* no evidence: lost in the middle of the picture, no motion on the grid: nothing moves at all */
    run_init(&r, 1, 0, 0);
    for (int i = 0; i < 25; i++) { struct tl_box b = { 138, 48, 182, 150, 0, 0 }; (void)step(&r, &b); }
    r.moves = 0;
    for (int i = 0; i < 100; i++) (void)step(&r, NULL);
    CHECK(r.moves == 0, "lost in the middle of the picture, nothing moves on the grid: no search");
    /* the motion appears after the loss at the left: steer toward it (one bounded move) */
    run_init(&r, 1, 0, 0);
    for (int i = 0; i < 25; i++) { struct tl_box b = { 138, 48, 182, 150, 0, 0 }; (void)step(&r, &b); }
    r.moves = 0;
    for (int i = 0; i < 8; i++) (void)step(&r, NULL);
    r.mot.active = 1; r.mot.cx = 100; r.mot.cy = 500; r.mot.last_active_ms = r.T; r.mot.valid = 1;
    for (int i = 0; i < 8; i++) { r.mot.last_active_ms = r.T; (void)step(&r, NULL); }
    CHECK(r.moves >= 1 && r.moves <= 2 && r.first_pan != 0, "motion appears after the loss: a steer toward it (one bounded move, two at most)");
    /* never two bursts inside the cooldown */
    run_init(&r, 1, 0, 0); walk_off(&r, 0, 6000, 6000);
    {
        int m1 = r.moves;
        r.moves = 0;
        for (int i = 0; i < 12; i++) { struct tl_box b = { 138, 48, 182, 150, 0, 0 }; (void)step(&r, &b); }
        for (int i = 0; i < 4; i++) { struct tl_box b = { 270, 48, 319, 150, 0, 0 }; (void)step(&r, &b); }
        r.homes = 0; r.moves = 0; r.was = 0;
        for (int i = 0; i < 30; i++) (void)step(&r, NULL);
        CHECK(m1 >= 1 && r.moves == 0, "a second loss 15 s after the first look: inside the 30 s cooldown, no look");
    }
    /* soft limit: a look toward a limit does not start within 300 steps of it */
    run_init(&r, 1, 0, 0);
    r.p.pan = 1275;
    r.p.pan_min = 300; r.p.pan_max = 1275 + 200; /* the left exit turns the head toward the high counters with the 2K binding (invert_pan) */
    walk_off(&r, 1, 6000, 8000); /* the exit is on the left, the limit is 200 steps away */
    CHECK(r.moves == 0, "no look toward a soft limit closer than the look's travel");
    /* the tilt: exit through the top edge */
    run_init(&r, 1, 0, 0);
    walk_off(&r, 2, 6000, 20000);
    CHECK(r.moves <= 2, "an exit through the top: at most two moves");
    /* the same bound for every legacy-only knob: search_mode motion never starts the full-range sweep or a scan */
    run_init(&r, 1, 0, 0); walk_off(&r, 0, 6000, 120000);
    CHECK(r.moves <= 2, "two minutes after the loss: still at most two moves (no sweep of the range)");
}

/* the same PHYSICAL exit gives the same motor direction in the four orientations, for the look and for the steer */
static void orient_search_cases(void)
{
    int pan_phys[4], tilt_phys[4], steer_phys[4];
    printf("== the search in the four orientations: the same physical way\n");
    for (int o = 0; o < 4; o++) {
        int flip = o & 1, mirror = o >> 1;
        struct run r;
        /* a person leaves through the PHYSICAL right of the sensor: the picture right when not mirrored, the picture left when mirrored */
        run_init(&r, 1, flip, mirror);
        walk_off(&r, mirror ? 1 : 0, 6000, 8000);
        pan_phys[o] = r.first_pan;
        /* a person leaves through the physical top of the sensor: the picture top when not flipped, the bottom when flipped (the bottom exit is not provable by the box alone, so the
         * picture-top exit is used and the expectation is mirrored: with flip the picture top is the physical bottom) */
        run_init(&r, 1, flip, mirror);
        walk_off(&r, 2, 6000, 8000);
        tilt_phys[o] = r.first_tilt * (flip ? -1 : 1);
        /* the steer: motion appears at the physical right of the sensor = picture right (not mirrored) / picture left (mirrored) */
        run_init(&r, 1, flip, mirror);
        for (int i = 0; i < 25; i++) { struct tl_box b = { 138, 48, 182, 150, 0, 0 }; (void)step(&r, &b); }
        r.moves = 0; r.first_ms = 0;
        for (int i = 0; i < 8; i++) (void)step(&r, NULL);
        r.mot.active = 1; r.mot.cx = mirror ? 100 : 900; r.mot.cy = 500; r.mot.valid = 1;
        for (int i = 0; i < 8; i++) { r.mot.last_active_ms = r.T; (void)step(&r, NULL); }
        steer_phys[o] = r.first_pan;
    }
    CHECK(pan_phys[0] != 0 && pan_phys[0] == pan_phys[1] && pan_phys[0] == pan_phys[2] && pan_phys[0] == pan_phys[3], "look toward the physical right: the same motor pan direction (none, flip, mirror, ceiling)");
    CHECK(tilt_phys[0] != 0 && tilt_phys[0] == tilt_phys[1] && tilt_phys[0] == tilt_phys[2] && tilt_phys[0] == tilt_phys[3], "look toward the physical top: the same motor tilt direction in all four");
    CHECK(steer_phys[0] != 0 && steer_phys[0] == steer_phys[1] && steer_phys[0] == steer_phys[2] && steer_phys[0] == steer_phys[3], "steer toward motion at the physical right: the same motor direction in all four");
    CHECK(steer_phys[0] == pan_phys[0], "look and steer toward the same physical side agree");
    {   /* sanity: in the picture, the look goes the way the person left: right exit = same sign as the tracker's pan toward a target on the right */
        struct run r;
        struct tl_box b = { 260, 48, 304, 150, 0, 0 };
        struct tl_out o;
        run_init(&r, 1, 0, 0);
        for (int i = 0; i < 6; i++) o = step(&r, &b);
        CHECK(o.kind == TL_VEL && o.pan_dir == pan_phys[0], "the look has the sign of the tracker's own follow toward a person at the right");
    }
}

int main(void)
{
    grid_cases();
    diff_cases();
    md_orient_cases();
    md_health_cases();
    diff_wiring_cases();
    search_cases();
    orient_search_cases();
    printf("rot2k motion unit: %d failed\n", fails);
    return fails ? 1 : 0;
}
