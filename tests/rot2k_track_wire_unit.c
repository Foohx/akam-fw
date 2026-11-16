/* Host unit test of the person-tracking wiring of the Rotatable 2K app (akam2k_track.c + track_logic.c on a FAKE motor): the sign mapping,
 * off by default, never while parked / calibrating / unknown / privacy, home after loss, and the named tracking areas (parse, format,
 * overlap, enable, clip, corners, persistence). Prints "rot2k track wire unit: N failed". */
#define _GNU_SOURCE
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "akam2k_store.h"
#include "akam2k_track.h"

static int fails;
#define CHECK(c, msg) do { if (c) printf("  ok   - %s\n", msg); else { printf("  FAIL - %s\n", msg); fails++; } } while (0)

void a2k_log(const char *fmt, ...) { (void)fmt; }

/* ---- fake motor ---- */
static a2k_ptz_info_t g_info;
static int g_priv, g_nudges, g_homes, g_dx, g_dy, g_range_ok = 1;
static void f_info(a2k_ptz_info_t *o) { *o = g_info; }
static int f_range(int r[4]) { r[0] = 300; r[1] = 3650; r[2] = 300; r[3] = 1750; return g_range_ok ? 0 : -1; }
static int g_sp;
static int f_nudge(int dx, int dy, int sp) { g_nudges++; g_dx = dx; g_dy = dy; g_sp = sp; return 0; }
static int f_home(void) { g_homes++; return 0; }
static int f_priv(void) { return g_priv; }
static const a2k_track_ops_t OPS = { f_info, f_range, f_nudge, f_home, f_priv };

/* the wiring cases below pin the sign mapping / safety gates with the slow 0.1.0 policy; the new one is in test_responsive and rot2k_track_sim.c */
static void tinit(a2k_track_t *t, const char *path)
{
    struct tl_cfg c;
    a2k_track_init(t, &OPS, path);
    tl_cfg_legacy(&c);
    (void)a2k_track_set_cfg(t, &c);
}

static void at(int state, int pan, int tilt)
{
    memset(&g_info, 0, sizeof g_info);
    g_info.state = state;
    g_info.known = state == HAL_PTZ_HOME || state == HAL_PTZ_MOVED;
    g_info.pan = (uint32_t)pan;
    g_info.tilt = (uint32_t)tilt;
}
static void reset(void) { g_nudges = g_homes = g_dx = g_dy = g_priv = 0; g_range_ok = 1; at(HAL_PTZ_HOME, 1275, 1300); }

/* a person box centred at (cx, cy) of the 320x192 frame */
static struct tl_box person(int cx, int cy) { struct tl_box b = { cx - 20, cy - 40, cx + 20, cy + 40 }; return b; }

/* three ticks 200 ms apart (the confirm streak + the move), starting at t */
static void see(a2k_track_t *t, int64_t t0, struct tl_box b)
{
    for (int i = 0; i < 3; i++)
        a2k_track_tick(t, t0 + i * 200, &b, 1, 320, 192);
}

static void test_wiring(void)
{
    a2k_track_t t;
    struct tl_box b;
    printf("== wiring on a fake motor\n");
    tinit(&t, NULL);
    reset();
    b = person(290, 150); /* right of the centre, below it */
    see(&t, 1000, b);
    CHECK(g_nudges == 0, "off by default: nothing moves");
    a2k_track_set_enabled(&t, 1);
    see(&t, 5000, b);
    CHECK(g_nudges == 1 && g_dx > 0 && g_dy < 0, "a person right of the centre and below it: pan right (dx > 0), tilt DOWN (dy < 0)");
    reset();
    b = person(30, 40); /* left, above */
    a2k_track_set_enabled(&t, 0);
    a2k_track_set_enabled(&t, 1);
    see(&t, 20000, b);
    CHECK(g_nudges == 1 && g_dx < 0 && g_dy > 0, "a person left and above: pan left (dx < 0), tilt UP (dy > 0)");
    reset();
    b = person(160, 96);
    see(&t, 30000, b);
    CHECK(g_nudges == 0, "a centred person inside the dead band: no move");

    printf("== never while ...\n");
    for (int st = 0; st < 5; st++) {
        const int s[5] = { HAL_PTZ_PARKED, HAL_PTZ_CALIBRATING, HAL_PTZ_UNKNOWN, HAL_PTZ_PARKED, HAL_PTZ_UNKNOWN };
        reset();
        at(s[st], 1275, 1750);
        a2k_track_set_enabled(&t, 0);
        a2k_track_set_enabled(&t, 1);
        see(&t, 40000, person(290, 150));
        if (g_nudges) { printf("  FAIL - moved in state %d\n", s[st]); fails++; }
    }
    printf("  ok   - no nudge while parked, calibrating or unknown\n");
    reset();
    g_priv = 1;
    a2k_track_set_enabled(&t, 0);
    a2k_track_set_enabled(&t, 1);
    see(&t, 50000, person(290, 150));
    CHECK(g_nudges == 0, "no nudge in privacy mode");
    reset();
    g_range_ok = 0;
    see(&t, 60000, person(290, 150));
    CHECK(g_nudges == 0, "no nudge when the soft range is unknown (no motor)");
    reset();
    at(HAL_PTZ_MOVED, 0, 0);
    g_info.known = 0; /* a motion is running */
    a2k_track_set_enabled(&t, 0);
    a2k_track_set_enabled(&t, 1);
    see(&t, 70000, person(290, 150));
    CHECK(g_nudges == 0, "no nudge while the motor is moving");
    reset();
    at(HAL_PTZ_MOVED, 2000, 1000); /* idle away from home: tracking continues */
    a2k_track_set_enabled(&t, 0);
    a2k_track_set_enabled(&t, 1);
    see(&t, 80000, person(290, 150));
    CHECK(g_nudges == 1, "idle and away from home: it tracks");

    printf("== home after loss\n");
    reset();
    a2k_track_set_enabled(&t, 0);
    a2k_track_set_enabled(&t, 1);
    see(&t, 100000, person(290, 150));
    CHECK(g_nudges == 1 && g_homes == 0, "tracked");
    at(HAL_PTZ_MOVED, 1500, 1200);
    for (int i = 0; i < 40; i++)
        a2k_track_tick(&t, 101000 + i * 200, NULL, 0, 320, 192); /* 8 s of nobody */
    CHECK(g_homes == 0, "not home after 8 s");
    for (int i = 0; i < 20; i++)
        a2k_track_tick(&t, 109000 + i * 200, NULL, 0, 320, 192);
    CHECK(g_homes == 1, "home after 10 s without a person");
    for (int i = 0; i < 20; i++)
        a2k_track_tick(&t, 114000 + i * 200, NULL, 0, 320, 192);
    CHECK(g_homes == 1, "only once");
    /* switching it off or parking forgets the target: no home */
    reset();
    a2k_track_set_enabled(&t, 0);
    a2k_track_set_enabled(&t, 1);
    see(&t, 200000, person(290, 150));
    a2k_track_set_enabled(&t, 0);
    for (int i = 0; i < 80; i++)
        a2k_track_tick(&t, 201000 + i * 200, NULL, 0, 320, 192);
    CHECK(g_homes == 0, "switched off: no home request");
    a2k_track_destroy(&t);
}

static const char *conf3 = "track_area_door=500:1500:500:1500:1\ntrack_area_yard=1400:2400:400:1200\n";

static void test_areas_pure(void)
{
    a2k_tareas_t a, keep;
    char out[300];
    int cx, cy, r[4] = { 300, 3650, 300, 1750 };
    printf("== areas: parse / format\n");
    CHECK(a2k_tareas_parse(conf3, strlen(conf3), &a) == 0 && a.n == 2 && a.a[0].complete && a.a[0].enabled && a.a[1].pmin == 1400 && a.a[1].enabled == 1,
          "two areas, enabled flag optional (default on)");
    CHECK(a2k_tareas_format(&a, out, sizeof out) > 0 && !strcmp(out, "track_area_door=500:1500:500:1500:1\ntrack_area_yard=1400:2400:400:1200:1\n"), "format");
    {
        const char *t2 = "track_area_x=none\ntrack_area_y=1:2:3:4:0\n";
        CHECK(a2k_tareas_parse(t2, strlen(t2), &a) == 0 && a.n == 2 && !a.a[0].complete && a.a[1].enabled == 0 && a.a[1].complete, "incomplete 'none' and a disabled area");
        a2k_tareas_format(&a, out, sizeof out);
        CHECK(!strcmp(out, "track_area_x=none:1\ntrack_area_y=1:2:3:4:0\n"), "incomplete / disabled round trip");
    }
    {
        static const char *const bad[] = {
            "track_area_a=1:2:3\n", "track_area_a=2:1:3:4\n", "track_area_a=1:2:4:3\n", "track_area_a=1:1:3:4\n", "track_area_a=1:2:3:4:2\n",
            "track_area_a=1:2:3:4:\n", "track_area_A=1:2:3:4\n", "track_area_=1:2:3:4\n", "track_area_a=1:2:3:4\ntrack_area_a=5:6:7:8\n",
            "area_a=1:2:3:4\n", "track_area_a=123456:2:3:4\n", "track_area_a=-1:2:3:4\n", "track_area_a=1:2:3:4:1:1\n", "track_area_9a=1:2:3:4\n",
            "track_area_abcdefghijklmnopq=1:2:3:4\n",
            "track_area_a=1:2:3:4\ntrack_area_b=1:2:3:4\ntrack_area_c=1:2:3:4\ntrack_area_d=1:2:3:4\ntrack_area_e=1:2:3:4\n",
        };
        int ok = 1;
        memset(&keep, 0x5a, sizeof keep);
        for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
            a = keep;
            if (a2k_tareas_parse(bad[i], strlen(bad[i]), &a) == 0 || memcmp(&a, &keep, sizeof a)) { printf("  FAIL - accepted: %s", bad[i]); ok = 0; fails++; }
        }
        if (ok) printf("  ok   - %zu malformed area texts refused, output untouched\n", sizeof bad / sizeof bad[0]);
    }
    CHECK(a2k_tarea_name_ok("door_2") && !a2k_tarea_name_ok("") && !a2k_tarea_name_ok("Door") && !a2k_tarea_name_ok("2door") && !a2k_tarea_name_ok("a b"),
          "name rule = the zone names");

    printf("== areas: allowed / clip / overlap / enable\n");
    a2k_tareas_parse("", 0, &a);
    CHECK(a2k_tareas_allowed(&a, 10, 10) == 1 && a2k_tareas_clip(&a, 1275, 1300, 400, -300, &cx, &cy) == 1 && cx == 400 && cy == -300,
          "no area: the whole soft range (nothing clipped here, the policy clips to the soft range)");
    a2k_tareas_parse(conf3, strlen(conf3), &a);
    CHECK(a2k_tareas_fit(&a, r) == 1, "areas inside the soft range fit");
    r[0] = 600;
    CHECK(a2k_tareas_fit(&a, r) == 0, "an area outside the soft range does not fit");
    CHECK(a2k_tareas_allowed(&a, 1000, 1000) == 1 && a2k_tareas_allowed(&a, 2000, 800) == 1 && a2k_tareas_allowed(&a, 3000, 1000) == 0 &&
              a2k_tareas_allowed(&a, 400, 1000) == 0,
          "allowed inside any enabled area, not outside all");
    CHECK(a2k_tareas_clip(&a, 1000, 1000, 800, 0, &cx, &cy) == 1 && cx == 500 && cy == 0, "clipped to door (the only area at 1000/1000): pan max 1500");
    CHECK(a2k_tareas_clip(&a, 1450, 1000, 800, 0, &cx, &cy) == 1 && cx == 800, "in the overlap: the area that lets it go the furthest (yard allows the whole 800, door only 50)");
    CHECK(a2k_tareas_clip(&a, 1450, 1000, -800, 0, &cx, &cy) == 1 && cx == -800, "... and the other way (door allows -800, yard only -50)");
    CHECK(a2k_tareas_clip(&a, 3000, 1000, 100, 0, &cx, &cy) == 0, "outside every area: no clip result (no tracking)");
    a.a[1].enabled = 0;
    CHECK(a2k_tareas_allowed(&a, 2000, 800) == 0 && a2k_tareas_allowed(&a, 1000, 1000) == 1, "a disabled area does not allow");
    CHECK(a2k_tareas_clip(&a, 1450, 1000, 800, 0, &cx, &cy) == 1 && cx == 50, "overlap with yard disabled: door only");
    a.a[0].enabled = 0;
    CHECK(a2k_tareas_allowed(&a, 1000, 1000) == 0 && a2k_tareas_allowed(&a, 10, 10) == 0, "areas defined but all disabled: never");
    a2k_tareas_parse("track_area_x=none\n", strlen("track_area_x=none\n"), &a);
    CHECK(a2k_tareas_allowed(&a, 10, 10) == 1, "an incomplete area alone: the whole range (it is not an area yet)");
    { const char *t3 = "track_area_x=none\ntrack_area_y=1000:2000:1000:1500\n"; a2k_tareas_parse(t3, strlen(t3), &a); }
    CHECK(a2k_tareas_allowed(&a, 10, 10) == 0 && a2k_tareas_allowed(&a, 1500, 1200) == 1, "an incomplete area next to a complete one is ignored");
}

static void test_areas_run(const char *dir)
{
    a2k_track_t t, t2;
    a2k_tareas_t a;
    char p[300], buf[300], txt[200];
    printf("== areas: editing, corners, persistence, clipping in the tracker\n");
    snprintf(p, sizeof p, "%s/track_areas", dir);
    reset();
    tinit(&t, p);
    CHECK(a2k_track_load(&t, conf3) == 2, "akam2k.conf areas loaded");
    a2k_track_areas_text(&t, txt, sizeof txt);
    CHECK(!strcmp(txt, "door=500:1500:500:1500;yard=1400:2400:400:1200"), "areas text");
    CHECK(a2k_track_area_new(&t, "gate") == 0 && a2k_track_area_new(&t, "gate") == -1 && a2k_track_area_new(&t, "Bad") == -1 && a2k_track_area_new(&t, "x") == 0 &&
              a2k_track_area_new(&t, "y") == -1,
          "new area: unique valid names, at most 4");
    a2k_track_areas(&t, &a);
    CHECK(a.n == 4 && !a.a[2].complete && a.a[2].enabled, "the new areas are incomplete and enabled");
    at(HAL_PTZ_HOME, 1000, 1000);
    CHECK(a2k_track_area_corner(&t, 2, 0) == 0, "corner A = the current position");
    a2k_track_areas(&t, &a);
    CHECK(!a.a[2].complete, "one corner: still incomplete");
    at(HAL_PTZ_MOVED, 800, 1400);
    CHECK(a2k_track_area_corner(&t, 2, 1) == 0, "corner B");
    a2k_track_areas(&t, &a);
    CHECK(a.a[2].complete && a.a[2].pmin == 800 && a.a[2].pmax == 1000 && a.a[2].tmin == 1000 && a.a[2].tmax == 1400, "complete: min / max of the two corners, any order");
    at(HAL_PTZ_MOVED, 800, 1000);
    CHECK(a2k_track_area_corner(&t, 3, 0) == 0 && a2k_track_area_corner(&t, 3, 1) == -1, "a degenerate area (both corners equal) is refused");
    a2k_track_areas(&t, &a);
    CHECK(!a.a[3].complete, "... and stays incomplete");
    g_info.known = 0;
    CHECK(a2k_track_area_corner(&t, 0, 0) == -1, "no corner while the position is unknown");
    CHECK(a2k_track_area_corner(&t, 9, 0) == -1 && a2k_track_area_enable(&t, 9, 1) == -1 && a2k_track_area_delete(&t, 9) == -1, "no such slot");
    CHECK(a2k_track_area_enable(&t, 1, 0) == 0, "disable yard");
    CHECK(a2k_store_read(p, buf, sizeof buf) > 0 && strstr(buf, "track_area_yard=1400:2400:400:1200:0\n") && strstr(buf, "track_area_gate=800:1000:1000:1400:1\n") &&
              strstr(buf, "track_area_x=none:1\n"),
          "persisted (complete, enabled flag, incomplete as none)");
    /* a restart: the file wins over akam2k.conf */
    tinit(&t2, p);
    CHECK(a2k_track_load(&t2, "track_area_other=1000:2000:1000:1500\n") == 4, "a restart reads the stored areas");
    a2k_track_areas(&t2, &a);
    CHECK(!strcmp(a.a[0].name, "door") && a.a[1].enabled == 0 && a.a[2].complete && !a.a[3].complete, "the file wins over the conf keys, enabled flags kept");
    a2k_track_destroy(&t2);
    CHECK(a2k_track_area_delete(&t, 3) == 0 && a2k_track_area_delete(&t, 2) == 0, "delete");
    a2k_track_areas(&t, &a);
    CHECK(a.n == 2 && !strcmp(a.a[1].name, "yard"), "the others keep their order");
    a2k_track_area_enable(&t, 1, 1);

    /* the tracker clips to the area that contains the camera */
    a2k_track_set_enabled(&t, 1);
    reset();
    at(HAL_PTZ_MOVED, 1450, 1000); /* in the overlap of door and yard */
    see(&t, 1000, person(300, 96)); /* far right: a big pan right */
    CHECK(g_nudges == 1 && g_dx > 0 && g_dx <= 950, "a nudge in the overlap is clipped to the area with most room (yard, <= 950)");
    reset();
    at(HAL_PTZ_MOVED, 1480, 1000); /* near the right edge of door, still inside yard */
    a2k_track_area_enable(&t, 1, 0);
    see(&t, 5000, person(300, 96));
    CHECK(g_nudges == 1 && g_dx == 20, "yard disabled: clipped to door (pan max 1500): 20 steps");
    reset();
    at(HAL_PTZ_MOVED, 1500, 1000);
    a2k_track_set_enabled(&t, 0);
    a2k_track_set_enabled(&t, 1);
    see(&t, 9000, person(300, 96));
    CHECK(g_nudges == 0, "at the area edge, a person further out: nothing (clipped to 0)");
    reset();
    at(HAL_PTZ_MOVED, 3000, 1000); /* outside every enabled area */
    a2k_track_set_enabled(&t, 0);
    a2k_track_set_enabled(&t, 1);
    see(&t, 13000, person(100, 96));
    CHECK(g_nudges == 0, "the camera is outside every enabled area: no tracking");
    reset();
    at(HAL_PTZ_HOME, 1275, 1300); /* home is inside door */
    a2k_track_set_enabled(&t, 0);
    a2k_track_set_enabled(&t, 1);
    see(&t, 17000, person(300, 96));
    CHECK(g_nudges == 1, "inside an enabled area: tracking works");
    a2k_track_destroy(&t);
    /* an area outside the soft range is dropped at load */
    tinit(&t, NULL);
    CHECK(a2k_track_load(&t, "track_area_far=100:200:100:200\ntrack_area_ok=1000:2000:1000:1500\n") == 1, "an area outside the soft range is dropped");
    a2k_track_destroy(&t);
}

static void test_responsive(void)
{
    a2k_track_t t;
    struct tl_box b = person(300, 96);
    int home, delay;
    printf("== responsive policy on the fake motor\n");
    a2k_track_init(&t, &OPS, NULL);
    a2k_track_set_enabled(&t, 1);
    reset();
    a2k_track_get_return(&t, &home, &delay);
    CHECK(home == 0 && delay == 30, "defaults: stay, 30 s");
    a2k_track_tick_ts(&t, 1000, 1000, &b, 1, 320, 192);
    a2k_track_tick_ts(&t, 1200, 1200, &b, 1, 320, 192);
    CHECK(g_nudges == 1 && g_dx > 0 && g_sp == 2, "two ticks (the acquire): one nudge, a large offset asks for the fast speed (divisor 2)");
    reset();
    g_info.motion_end_ms = 1500;
    a2k_track_tick_ts(&t, 1600, 1400, &b, 1, 320, 192); /* captured during the motion */
    a2k_track_tick_ts(&t, 1800, 1640, &b, 1, 320, 192); /* captured 140 ms after the end: still inside the 150 ms settle */
    CHECK(g_nudges == 0, "frames captured during / right after the motion are dropped");
    a2k_track_tick_ts(&t, 2000, 1800, &b, 1, 320, 192);
    CHECK(g_nudges == 1, "the first settled frame corrects at once");
    /* the return policy is live */
    a2k_track_set_return(&t, 1, 5);
    a2k_track_get_return(&t, &home, &delay);
    CHECK(home == 1 && delay == 5, "set_return: home after 5 s");
    reset();
    at(HAL_PTZ_MOVED, 1500, 1200);
    for (int i = 0; i < 35; i++)
        a2k_track_tick_ts(&t, 3000 + i * 200, 0, NULL, 0, 320, 192);
    CHECK(g_homes == 0, "grace 3 s + 5 s delay not over (seen at 2000, now 9800)");
    for (int i = 0; i < 20; i++)
        a2k_track_tick_ts(&t, 10000 + i * 200, 0, NULL, 0, 320, 192);
    CHECK(g_homes == 1, "home once, 8 s after the last sighting");
    a2k_track_destroy(&t);
}

int main(void)
{
    char dir[] = "/tmp/rot2k_track_XXXXXX";
    char cmd[300];
    if (!mkdtemp(dir)) { perror("mkdtemp"); return 2; }
    test_wiring();
    test_responsive();
    test_areas_pure();
    test_areas_run(dir);
    snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
    if (system(cmd) != 0) { /* best effort */ }
    printf("rot2k track wire unit: %d failed\n", fails);
    return fails ? 1 : 0;
}
