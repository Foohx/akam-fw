/* Host unit test of the PTZ patrol engine (app/ptz_patrol.c, pure logic; specs/ptz-patrol.md): data model and limits, validation, expansion of the
 * generated patterns, text and JSON forms, a small deterministic fuzz of the JSON reader, and the run state machine driven by a SIMULATED MOTOR with a
 * fake clock (move time = distance x speed, polled every 100 ms like the binding): sequences, dwell timing, loop / pingpong / once, preemption and resume,
 * stop conditions, failed moves, clamping. No thread, no hardware. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../firmware/lsc-rotatable-2k-ak3918av130/app/ptz_patrol.h"

static int fails;
#define CHECK(c, msg) do { if (c) printf("  ok   - %s\n", msg); else { printf("  FAIL - %s\n", msg); fails++; } } while (0)

static const pp_limits_t LIM = { 300, 3650, 300, 2100, 2, 20, 3, 1275, 300, 15 };

static pp_route_t mkroute(const char *name, int mode, int n, const int (*pt)[4])
{
    pp_route_t r;
    int i;
    memset(&r, 0, sizeof r);
    snprintf(r.name, sizeof r.name, "%s", name);
    r.mode = mode;
    r.nsteps = n;
    for (i = 0; i < n; i++) {
        r.step[i].pan = pt[i][0];
        r.step[i].tilt = pt[i][1];
        r.step[i].speed = pt[i][2];
        r.step[i].dwell_ms = (unsigned)pt[i][3];
    }
    return r;
}

static void test_model(void)
{
    static pp_cfg_t c;
    pp_diag_t dg;
    int i, rc;
    char nm[24];
    printf("== names, presets, routes\n");
    CHECK(pp_name_ok("door") && pp_name_ok("a") && pp_name_ok("a_1") && pp_name_ok("abcdefghijklmnop") && !pp_name_ok("abcdefghijklmnopq") && !pp_name_ok("") &&
          !pp_name_ok("Door") && !pp_name_ok("1door") && !pp_name_ok("_x") && !pp_name_ok("a b") && !pp_name_ok("a-b") && !pp_name_ok(NULL), "names: a-z first, a-z 0-9 _, 1..16");
    CHECK(pp_mode_parse("once") == PP_ONCE && pp_mode_parse("loop") == PP_LOOP && pp_mode_parse("pingpong") == PP_PINGPONG && pp_mode_parse("x") < 0 && pp_mode_parse(NULL) < 0 &&
          !strcmp(pp_mode_name(PP_PINGPONG), "pingpong"), "modes");
    pp_cfg_init(&c);
    CHECK(pp_preset_set(&c, &LIM, "door", 900, 1200, &dg) == PP_OK && c.npresets == 1, "a preset is stored");
    CHECK(pp_preset_set(&c, &LIM, "door", 950, 1250, &dg) == PP_OK && c.npresets == 1 && c.preset[0].pan == 950, "the same name replaces");
    CHECK(pp_preset_set(&c, &LIM, "far", 4000, 1200, &dg) == PP_E_RANGE && strstr(dg.detail, "outside the soft range"), "a preset outside the soft range is refused (pan)");
    CHECK(pp_preset_set(&c, &LIM, "far", 900, 299, &dg) == PP_E_RANGE && pp_preset_set(&c, &LIM, "far", 299, 900, &dg) == PP_E_RANGE &&
          pp_preset_set(&c, &LIM, "far", 3650, 2100, &dg) == PP_OK, "the soft range is inclusive, tilt too");
    CHECK(pp_preset_set(&c, &LIM, "Bad", 900, 1200, &dg) == PP_E_NAME && c.npresets == 2, "a bad name is refused, nothing stored");
    for (i = 0; i < PP_PRESETS - 2; i++) {
        snprintf(nm, sizeof nm, "p%d", i);
        rc = pp_preset_set(&c, &LIM, nm, 500 + i, 500, &dg);
        if (rc != PP_OK)
            break;
    }
    CHECK(c.npresets == PP_PRESETS && pp_preset_set(&c, &LIM, "one_more", 500, 500, &dg) == PP_E_FULL && c.npresets == PP_PRESETS, "at most 16 presets");
    CHECK(pp_preset_set(&c, &LIM, "p0", 600, 600, &dg) == PP_OK, "a replace is fine when full");
    CHECK(pp_preset_set(&c, NULL, "outside", 70000, 5, NULL) == PP_E_RANGE, "even without limits the stored range is 0..65535");

    {
        static const int pts[2][4] = { { 900, 1200, 3, 500 }, { 1500, 1300, 0, 0 } };
        pp_route_t r = mkroute("yard", PP_LOOP, 2, pts);
        strcpy(r.step[1].preset, "door");
        CHECK(pp_route_set(&c, &LIM, &r, &dg) == PP_OK && c.nroutes == 1, "a route with an explicit step and a preset step");
        CHECK(pp_preset_del(&c, "door", &dg) == PP_E_INUSE && strstr(dg.detail, "yard"), "a preset used by a route cannot be deleted");
        r.step[1].preset[0] = '\0';
        CHECK(pp_route_set(&c, &LIM, &r, &dg) == PP_OK && pp_preset_del(&c, "door", &dg) == PP_OK && pp_preset_del(&c, "door", &dg) == PP_E_NOTFOUND, "...after the route no longer uses it it can");
        strcpy(r.step[0].preset, "ghost");
        CHECK(pp_route_set(&c, &LIM, &r, &dg) == PP_E_NOPRESET && strstr(dg.detail, "ghost"), "a step with an unknown preset is refused");
        r.step[0].preset[0] = '\0';
        r.step[0].speed = 25;
        CHECK(pp_route_set(&c, &LIM, &r, &dg) == PP_E_SPEED, "speed 25 is outside the descriptor's 2..20");
        r.step[0].speed = 1;
        CHECK(pp_route_set(&c, &LIM, &r, &dg) == PP_E_SPEED, "speed 1 is outside it too");
        r.step[0].speed = 2;
        r.step[0].dwell_ms = PP_DWELL_MAX_MS + 1;
        CHECK(pp_route_set(&c, &LIM, &r, &dg) == PP_E_DWELL, "dwell over 3600000 ms is refused");
        r.step[0].dwell_ms = PP_DWELL_MAX_MS;
        r.step[1].pan = 299;
        CHECK(pp_route_set(&c, &LIM, &r, &dg) == PP_E_RANGE && strstr(dg.detail, "step 1"), "a step outside the soft range is refused, the detail names the step");
        r.step[1].pan = 1500;
        r.nsteps = 0;
        CHECK(pp_route_set(&c, &LIM, &r, &dg) == PP_E_EMPTY, "no step is refused");
        r.nsteps = PP_STEPS + 1;
        CHECK(pp_route_set(&c, &LIM, &r, &dg) == PP_E_TOOMANY, "33 steps are refused");
        r.nsteps = 2;
        r.mode = 9;
        CHECK(pp_route_set(&c, &LIM, &r, &dg) == PP_E_MODE, "a bad mode is refused");
        r.mode = PP_PINGPONG;
        CHECK(pp_route_set(&c, &LIM, &r, &dg) == PP_OK && c.nroutes == 1 && c.route[0].mode == PP_PINGPONG, "a valid replace keeps one route");
        for (i = 1; i < PP_ROUTES; i++) {
            snprintf(r.name, sizeof r.name, "r%d", i);
            CHECK(pp_route_set(&c, &LIM, &r, &dg) == PP_OK, "up to 8 routes");
        }
        strcpy(r.name, "r9");
        CHECK(pp_route_set(&c, &LIM, &r, &dg) == PP_E_FULL, "the 9th route is refused");
        CHECK(pp_route_del(&c, "r3", &dg) == PP_OK && c.nroutes == 7 && pp_find_route(&c, "r3") < 0 && pp_route_del(&c, "r3", &dg) == PP_E_NOTFOUND, "a route is deleted");
    }
}

static void test_park(void)
{
    static pp_cfg_t c, wide;
    pp_diag_t dg;
    pp_route_t r;
    pp_t p;
    pp_out_t o;
    printf("== the park position is never a patrol target (privacy)\n");
    pp_cfg_init(&c);
    CHECK(pp_preset_set(&c, &LIM, "pk", 1275, 300, &dg) == PP_E_RANGE && strstr(dg.detail, "park") && pp_preset_set(&c, &LIM, "pk", 1290, 315, &dg) == PP_E_RANGE &&
          pp_preset_set(&c, &LIM, "pk", 1291, 300, &dg) == PP_OK && pp_preset_set(&c, &LIM, "pk2", 1275, 316, &dg) == PP_OK, "presets within +-15 of the park position are refused, 16 away is fine");
    memset(&r, 0, sizeof r);
    strcpy(r.name, "a");
    r.nsteps = 1;
    r.step[0].pan = 1280;
    r.step[0].tilt = 290;
    CHECK(pp_route_set(&c, &LIM, &r, &dg) == PP_E_RANGE, "a route step near the park position is refused");
    CHECK(pp_expand_sweep_h(&LIM, 400, 2000, 300, 175, 0, 0, &r, &dg) == PP_E_RANGE, "a sweep that crosses the park position is refused (the step lands on it)");
    /* a config stored under other limits: the run refuses to start rather than park the head */
    pp_cfg_init(&wide);
    pp_preset_set(&wide, NULL, "pk", 1275, 300, NULL);
    strcpy(r.name, "b");
    strcpy(r.step[0].preset, "pk");
    pp_route_set(&wide, NULL, &r, NULL);
    pp_init(&p);
    CHECK(pp_start(&p, &wide, &LIM, "b", 0, &o) == PP_E_RANGE && p.phase == PP_IDLE, "pp_start refuses a stored step near the park position");
    CHECK(pp_route_check(&wide, &LIM, &r, &dg) == PP_E_RANGE && strstr(dg.detail, "preset pk"), "...and a route check names the preset");
}

static void test_patterns(void)
{
    pp_route_t r;
    pp_diag_t dg;
    int n, i;
    printf("== generated patterns\n");
    memset(&r, 0, sizeof r);
    n = pp_expand_sweep_h(&LIM, 400, 3000, 1300, 500, 4, 300, &r, &dg);
    CHECK(n == 7 && r.nsteps == 7 && r.step[0].pan == 400 && r.step[1].pan == 900 && r.step[5].pan == 2900 && r.step[6].pan == 3000 && r.step[3].tilt == 1300 && r.step[2].speed == 4 &&
          r.step[2].dwell_ms == 300, "sweep_h 400..3000 step 500: 7 steps, the last lands exactly on pan_to, tilt fixed, speed and dwell copied");
    n = pp_expand_sweep_h(&LIM, 3000, 400, 1300, 1300, 0, 0, &r, &dg);
    CHECK(n == 3 && r.step[0].pan == 3000 && r.step[1].pan == 1700 && r.step[2].pan == 400, "sweep_h descending: 3000, 1700, 400");
    n = pp_expand_sweep_h(&LIM, 400, 1400, 1300, 500, 0, 0, &r, &dg);
    CHECK(n == 3 && r.step[2].pan == 1400, "an exact multiple does not repeat the end");
    n = pp_expand_sweep_h(&LIM, 400, 401, 1300, 500, 0, 0, &r, &dg);
    CHECK(n == 2 && r.step[0].pan == 400 && r.step[1].pan == 401, "a step larger than the span: both ends");
    CHECK(pp_expand_sweep_h(&LIM, 400, 400, 1300, 100, 0, 0, &r, &dg) == PP_E_PATTERN, "an empty range is refused");
    CHECK(pp_expand_sweep_h(&LIM, 400, 900, 1300, 0, 0, 0, &r, &dg) == PP_E_PATTERN, "step 0 is refused");
    CHECK(pp_expand_sweep_h(&LIM, 100, 900, 1300, 100, 0, 0, &r, &dg) == PP_E_RANGE, "pan_from outside the soft range");
    CHECK(pp_expand_sweep_h(&LIM, 400, 9000, 1300, 100, 0, 0, &r, &dg) == PP_E_RANGE, "pan_to outside the soft range");
    CHECK(pp_expand_sweep_h(&LIM, 400, 900, 100, 100, 0, 0, &r, &dg) == PP_E_RANGE, "tilt outside the soft range");
    CHECK(pp_expand_sweep_h(&LIM, 400, 900, 1300, 100, 21, 0, &r, &dg) == PP_E_SPEED && pp_expand_sweep_h(&LIM, 400, 900, 1300, 100, 1, 0, &r, &dg) == PP_E_SPEED, "speed outside 2..20");
    CHECK(pp_expand_sweep_h(&LIM, 400, 900, 1300, 100, 5, PP_DWELL_MAX_MS + 1, &r, &dg) == PP_E_DWELL, "dwell too long");
    n = pp_expand_sweep_h(&LIM, 300, 3650, 1300, 109, 0, 0, &r, &dg);
    CHECK(n == 32 && r.step[31].pan == 3650, "32 steps are the maximum: 3350 / 109 -> 32");
    CHECK(pp_expand_sweep_h(&LIM, 300, 3650, 1300, 108, 0, 0, &r, &dg) == PP_E_TOOMANY && strstr(dg.detail, "raise step"), "33 steps are refused with a hint");
    n = pp_expand_sweep_v(&LIM, 300, 2100, 2000, 600, 6, 100, &r, &dg);
    CHECK(n == 4 && r.step[0].tilt == 300 && r.step[1].tilt == 900 && r.step[3].tilt == 2100 && r.step[2].pan == 2000 && r.step[2].speed == 6, "sweep_v: tilt 300..2100 step 600, pan fixed");
    CHECK(pp_expand_sweep_v(&LIM, 300, 2200, 2000, 600, 0, 0, &r, &dg) == PP_E_RANGE, "sweep_v tilt_to outside");
    n = pp_expand_raster(&LIM, 600, 2400, 800, 1600, 3, 3, 6, 200, &r, &dg);
    CHECK(n == 9 && r.step[0].pan == 600 && r.step[2].pan == 2400 && r.step[0].tilt == 800 && r.step[3].pan == 2400 && r.step[5].pan == 600 && r.step[3].tilt == 1200 && r.step[6].pan == 600 &&
          r.step[8].pan == 2400 && r.step[8].tilt == 1600, "raster 3 rows x 3 cols: evenly spaced rows, the pan direction alternates");
    n = pp_expand_raster(&LIM, 600, 2400, 800, 1600, 1, 2, 0, 0, &r, &dg);
    CHECK(n == 2 && r.step[0].tilt == 800 && r.step[1].pan == 2400, "raster with one row uses tilt_from");
    n = pp_expand_raster(&LIM, 600, 2400, 800, 1600, 16, 2, 0, 0, &r, &dg);
    CHECK(n == 32 && r.step[31].tilt == 1600, "raster 16 x 2 = 32 steps");
    CHECK(pp_expand_raster(&LIM, 600, 2400, 800, 1600, 17, 2, 0, 0, &r, &dg) == PP_E_TOOMANY && pp_expand_raster(&LIM, 600, 2400, 800, 1600, 0, 2, 0, 0, &r, &dg) == PP_E_PATTERN &&
          pp_expand_raster(&LIM, 600, 2400, 800, 1600, 3, 1, 0, 0, &r, &dg) == PP_E_PATTERN && pp_expand_raster(&LIM, 600, 600, 800, 1600, 3, 2, 0, 0, &r, &dg) == PP_E_PATTERN &&
          pp_expand_raster(&LIM, 600, 2400, 800, 800, 3, 2, 0, 0, &r, &dg) == PP_E_PATTERN, "raster: too many, no rows, one column, empty ranges");
    for (i = 0; i < 9; i++) { /* every generated step is inside the range */
        n = pp_expand_raster(&LIM, 300, 3650, 300, 2100, 3, 3, 0, 0, &r, &dg);
        if (n != 9 || r.step[i].pan < 300 || r.step[i].pan > 3650 || r.step[i].tilt < 300 || r.step[i].tilt > 2100)
            break;
    }
    CHECK(i == 9, "a raster over the whole range stays inside it");
}

static void test_text(void)
{
    static pp_cfg_t c, d;
    static char buf[PP_TEXT_MAX + 64];
    pp_diag_t dg;
    pp_route_t r;
    int n, i;
    printf("== stored text\n");
    pp_cfg_init(&c);
    pp_preset_set(&c, &LIM, "door", 900, 1200, &dg);
    pp_preset_set(&c, &LIM, "porch", 1500, 1100, &dg);
    {
        static const int pts[3][4] = { { 900, 1200, 3, 300 }, { 1500, 1300, 5, 0 }, { 0, 0, 0, 750 } };
        r = mkroute("yard", PP_PINGPONG, 3, pts);
        strcpy(r.step[2].preset, "porch");
        pp_route_set(&c, &LIM, &r, &dg);
    }
    n = pp_cfg_format(&c, buf, sizeof buf);
    CHECK(n > 0 && !strcmp(buf, "p door 900 1200\np porch 1500 1100\nr yard pingpong\ns 900:1200 3 300\ns 1500:1300 5 0\ns porch 0 750\n"), "the text form: presets, then routes with their steps");
    CHECK(pp_cfg_parse(buf, (size_t)n, &d) == PP_OK && d.npresets == 2 && d.nroutes == 1 && d.route[0].nsteps == 3 && d.route[0].mode == PP_PINGPONG && !strcmp(d.route[0].step[2].preset, "porch") &&
          d.route[0].step[1].pan == 1500 && d.route[0].step[1].speed == 5 && d.route[0].step[2].dwell_ms == 750, "...and it parses back to the same config");
    CHECK(pp_cfg_text_size(&c) == n, "pp_cfg_text_size equals the formatted length");
    CHECK(pp_cfg_format(&c, buf, 20) == PP_E_TOOBIG, "a buffer too small is refused");
    CHECK(pp_cfg_parse("", 0, &d) == PP_OK && d.npresets == 0 && d.nroutes == 0, "an empty text is an empty config");
    {
        static const char *const bad[] = { "x door 1 2\n", "p door 1\n", "p Door 1 2\n", "p door 1 2\np door 3 4\n", "p door 1 x\n", "p door -1 2\n", "r a loop\n", "r a zigzag\ns 1:2 0 0\n",
                                           "s 1:2 0 0\n", "r a loop\n", "r a loop\ns 1:2 0\n", "r a loop\ns 1:2 0 0 0\n", "r a loop\ns nothere 0 0\n", "r a loop\ns 1:2 0 0\nr a loop\ns 1:2 0 0\n",
                                           "r a loop\ns 1:2 0 99999999999\n", "p door 1 2\n\n", "p  door 1 2\n", "p door 1 2", "r a loop\ns 1:2 0 3600001\n", "r a loop\ns 1: 0 0\n" };
        for (i = 0; i < (int)(sizeof bad / sizeof bad[0]); i++) {
            int rc = pp_cfg_parse(bad[i], strlen(bad[i]), &d);
            /* "p door 1 2" without LF is accepted: the last line need not end with one */
            if (i == 17 ? rc != PP_OK : (rc == PP_OK || d.npresets || d.nroutes))
                break;
        }
        CHECK(i == (int)(sizeof bad / sizeof bad[0]), "invalid texts are refused and leave an empty config (bad letter, fields, names, duplicates, step before route, range, long numbers)");
    }
    CHECK(pp_cfg_parse(buf, PP_TEXT_MAX + 1, &d) == PP_E_SYNTAX, "a text over the limit is refused");
    /* the size limit: fill the config until the text would exceed 4096 bytes */
    pp_cfg_init(&c);
    for (i = 0; i < PP_PRESETS; i++) {
        char nm[24];
        snprintf(nm, sizeof nm, "preset_number_%c", 'a' + i); /* 15 characters */
        pp_preset_set(&c, &LIM, nm, 1000 + i, 1000, &dg);
    }
    memset(&r, 0, sizeof r);
    r.mode = PP_LOOP;
    r.nsteps = PP_STEPS;
    for (i = 0; i < PP_STEPS; i++) {
        r.step[i].pan = 3000 + i;
        r.step[i].tilt = 2000;
        r.step[i].speed = 10;
        r.step[i].dwell_ms = 3600000;
    }
    for (i = 0; i < PP_ROUTES; i++) {
        int rc;
        snprintf(r.name, sizeof r.name, "route%d", i);
        rc = pp_route_set(&c, &LIM, &r, &dg);
        if (rc != PP_OK) {
            CHECK(rc == PP_E_TOOBIG && c.nroutes == i && pp_cfg_text_size(&c) > 0, "a route that would push the text over 4096 bytes is refused (413 too_big), the config is unchanged");
            break;
        }
    }
    CHECK(i < PP_ROUTES, "...the limit is reached before 8 maximal routes");
}

static int jroute(const char *s, pp_route_t *r, pp_diag_t *dg) { return pp_route_from_json(s, strlen(s), &LIM, r, dg); }

static void test_json(void)
{
    static pp_route_t r;
    static pp_cfg_t c;
    pp_diag_t dg;
    char name[PP_NAME_MAX + 1], out[4096];
    int pan, tilt, here, n;
    printf("== JSON in and out\n");
    CHECK(jroute("{\"name\":\"yard\",\"mode\":\"pingpong\",\"steps\":[{\"preset\":\"door\",\"speed\":3,\"dwell_ms\":2000},{\"pan\":900,\"tilt\":1200}]}", &r, &dg) == PP_OK && r.nsteps == 2 &&
          !strcmp(r.name, "yard") && r.mode == PP_PINGPONG && !strcmp(r.step[0].preset, "door") && r.step[0].speed == 3 && r.step[0].dwell_ms == 2000 && r.step[1].pan == 900 && !r.step[1].preset[0],
          "a route body: steps with a preset and an explicit position");
    CHECK(jroute(" { \"name\" : \"a\" , \"steps\" : [ { \"pan\" : 900 , \"tilt\" : 1200 } ] } ", &r, &dg) == PP_OK && r.mode == PP_LOOP, "whitespace is accepted, the mode defaults to loop");
    CHECK(jroute("{\"name\":\"sw\",\"mode\":\"pingpong\",\"pattern\":{\"type\":\"sweep_h\",\"pan_from\":400,\"pan_to\":3000,\"tilt\":1300,\"step\":500,\"speed\":4,\"dwell_ms\":300}}", &r, &dg) == PP_OK &&
          r.nsteps == 7 && r.step[6].pan == 3000, "a sweep_h pattern is expanded");
    CHECK(jroute("{\"name\":\"sv\",\"pattern\":{\"type\":\"sweep_v\",\"tilt_from\":300,\"tilt_to\":2100,\"pan\":2000,\"step\":900}}", &r, &dg) == PP_OK && r.nsteps == 3, "a sweep_v pattern");
    CHECK(jroute("{\"name\":\"ra\",\"pattern\":{\"type\":\"raster\",\"pan_from\":600,\"pan_to\":2400,\"tilt_from\":800,\"tilt_to\":1600,\"rows\":2}}", &r, &dg) == PP_OK && r.nsteps == 4, "a raster pattern (cols default 2)");
    {
        static const char *const bad[] = {
            "", "{", "[]", "{\"name\":\"a\"}", "{\"steps\":[{\"pan\":900,\"tilt\":1200}]}", "{\"name\":\"a\",\"steps\":[{\"pan\":900,\"tilt\":1200}]} x", "{\"name\":\"a\",\"steps\":[{\"pan\":900,\"tilt\":1200}],\"x\":1}",
            "{\"name\":\"a\",\"name\":\"b\",\"steps\":[{\"pan\":900,\"tilt\":1200}]}", "{\"name\":\"a\",\"steps\":[{\"pan\":900,\"pan\":900,\"tilt\":1200}]}", "{\"name\":1,\"steps\":[{\"pan\":900,\"tilt\":1200}]}",
            "{\"name\":\"a\",\"steps\":[{\"pan\":\"900\",\"tilt\":1200}]}", "{\"name\":\"a\",\"steps\":[{\"pan\":900.5,\"tilt\":1200}]}", "{\"name\":\"a\",\"steps\":[{\"pan\":9e2,\"tilt\":1200}]}",
            "{\"name\":\"a\",\"steps\":[{\"pan\":0900,\"tilt\":1200}]}", "{\"name\":\"a\",\"steps\":[{\"pan\":900}]}", "{\"name\":\"a\",\"steps\":[{\"preset\":\"door\",\"pan\":900,\"tilt\":1200}]}",
            "{\"name\":\"a\",\"steps\":[{}]}", "{\"name\":\"a\",\"steps\":[{\"pan\":900,\"tilt\":1200},]}", "{\"name\":\"a\",\"steps\":{\"pan\":900}}", "{\"name\":\"a\",\"steps\":[[]]}",
            "{\"name\":\"a\",\"steps\":[{\"pan\":900,\"tilt\":1200}],\"pattern\":{\"type\":\"sweep_h\",\"pan_from\":400,\"pan_to\":900,\"tilt\":1200,\"step\":250}}", "{\"name\":\"a\"}",
            "{\"name\":\"a\",\"pattern\":{\"type\":\"sweep_h\",\"pan_from\":400}}", "{\"name\":\"a\",\"pattern\":{\"type\":\"spiral\"}}", "{\"name\":\"a\",\"pattern\":{\"pan_from\":400}}",
            "{\"name\":\"a\",\"pattern\":{\"type\":\"sweep_h\",\"pan_from\":400,\"pan_to\":900,\"tilt\":1200,\"step\":250,\"rows\":2}}", "{\"name\":\"a\",\"pattern\":{\"type\":\"sweep_h\",\"pan_from\":400,\"pan_to\":900,\"tilt\":1200,\"step\":250,\"speed\":-1}}",
            "{\"name\":\"a\\u0041\",\"steps\":[{\"pan\":900,\"tilt\":1200}]}", "{\"name\":\"a\\\"b\",\"steps\":[{\"pan\":900,\"tilt\":1200}]}", "{\"name\":\"a\",\"mode\":null,\"steps\":[{\"pan\":900,\"tilt\":1200}]}",
            "{\"name\":\"a\",\"steps\":[{\"pan\":900,\"tilt\":1200,\"speed\":100}]}", "{\"name\":\"a\",\"steps\":[{\"pan\":900,\"tilt\":1200,\"dwell_ms\":-1}]}", "{\"name\":\"a\",\"steps\":[{\"pan\":9999999999,\"tilt\":1200}]}",
        };
        int i;
        for (i = 0; i < (int)(sizeof bad / sizeof bad[0]); i++)
            if (jroute(bad[i], &r, &dg) == PP_OK)
                break;
        CHECK(i == (int)(sizeof bad / sizeof bad[0]), "malformed and invalid route bodies are all refused (duplicates, types, fractions, exponents, leading zero, escapes, trailing commas, both steps and pattern)");
        if (i < (int)(sizeof bad / sizeof bad[0]))
            printf("    accepted: %s\n", bad[i]);
    }
    CHECK(jroute("{\"name\":\"a\",\"mode\":\"zigzag\",\"steps\":[{\"pan\":900,\"tilt\":1200}]}", &r, &dg) == PP_E_MODE && jroute("{\"name\":\"Bad\",\"steps\":[{\"pan\":900,\"tilt\":1200}]}", &r, &dg) == PP_E_NAME &&
          jroute("{\"name\":\"a\",\"steps\":[]}", &r, &dg) == PP_E_EMPTY, "mode / name / empty errors carry their own code");
    {
        char big[4096];
        int i, w = snprintf(big, sizeof big, "{\"name\":\"a\",\"steps\":[");
        for (i = 0; i < 33; i++)
            w += snprintf(big + w, sizeof big - (size_t)w, "%s{\"pan\":900,\"tilt\":1200}", i ? "," : "");
        snprintf(big + w, sizeof big - (size_t)w, "]}");
        CHECK(jroute(big, &r, &dg) == PP_E_TOOMANY, "33 steps in a body are refused while parsing");
    }
    CHECK(pp_preset_from_json("{\"name\":\"door\",\"pan\":900,\"tilt\":1200}", 37, NULL, name, &pan, &tilt, &here, &dg) == PP_OK && !strcmp(name, "door") && pan == 900 && tilt == 1200 && !here, "a preset body");
    {
        const char *s = "{\"name\":\"door\",\"here\":true}";
        CHECK(pp_preset_from_json(s, strlen(s), NULL, name, &pan, &tilt, &here, &dg) == PP_OK && here, "a preset body with here:true");
        s = "{\"name\":\"door\",\"here\":true,\"pan\":1}";
        CHECK(pp_preset_from_json(s, strlen(s), NULL, name, &pan, &tilt, &here, &dg) == PP_E_SYNTAX, "here excludes pan / tilt");
        s = "{\"pan\":900,\"tilt\":1200}";
        CHECK(pp_preset_from_json(s, strlen(s), NULL, name, &pan, &tilt, &here, &dg) == PP_E_NAME && pp_preset_from_json(s, strlen(s), "porch", name, &pan, &tilt, &here, &dg) == PP_OK &&
              !strcmp(name, "porch"), "the name may come from the path");
        s = "{\"name\":\"door\",\"pan\":900,\"tilt\":1200}";
        CHECK(pp_preset_from_json(s, strlen(s), "porch", name, &pan, &tilt, &here, &dg) == PP_E_NAME, "a body name that differs from the path is refused");
        s = "{\"name\":\"door\",\"pan\":900}";
        CHECK(pp_preset_from_json(s, strlen(s), NULL, name, &pan, &tilt, &here, &dg) == PP_E_SYNTAX, "pan without tilt is refused");
        s = "{\"name\":\"door\",\"pan\":900,\"tilt\":1200,\"extra\":1}";
        CHECK(pp_preset_from_json(s, strlen(s), NULL, name, &pan, &tilt, &here, &dg) == PP_E_SYNTAX, "an unknown preset key is refused");
    }
    pp_cfg_init(&c);
    pp_preset_set(&c, &LIM, "door", 900, 1200, &dg);
    pp_preset_set(&c, &LIM, "porch", 1500, 1100, &dg);
    jroute("{\"name\":\"yard\",\"mode\":\"once\",\"steps\":[{\"preset\":\"door\",\"speed\":3,\"dwell_ms\":2000},{\"pan\":900,\"tilt\":1200}]}", &r, &dg);
    pp_route_set(&c, &LIM, &r, &dg);
    n = pp_route_json(&c.route[0], out, sizeof out);
    CHECK(n > 0 && !strcmp(out, "{\"name\":\"yard\",\"mode\":\"once\",\"steps\":[{\"preset\":\"door\",\"speed\":3,\"dwell_ms\":2000},{\"pan\":900,\"tilt\":1200,\"speed\":0,\"dwell_ms\":0}]}"), "pp_route_json");
    CHECK(pp_route_json(&c.route[0], out, 30) < 0, "a buffer too small is reported");
    n = pp_presets_json(&c, out, sizeof out);
    CHECK(n > 0 && !strcmp(out, "{\"presets\":[{\"name\":\"door\",\"pan\":900,\"tilt\":1200},{\"name\":\"porch\",\"pan\":1500,\"tilt\":1100}],\"max\":16}"), "pp_presets_json");
    n = pp_routes_json(&c, 0, "yard", out, sizeof out);
    CHECK(n > 0 && !strcmp(out, "{\"routes\":[{\"name\":\"yard\",\"mode\":\"once\",\"steps\":2,\"running\":true}],\"max\":8,\"default\":\"yard\"}"), "pp_routes_json");
    pp_error_json(PP_E_RANGE, &dg, out, sizeof out);
    CHECK(strstr(out, "{\"error\":\"range\",\"detail\":\"") == out, "pp_error_json");
    {
        pp_diag_t evil;
        memset(&evil, 0, sizeof evil);
        strcpy(evil.detail, "a\"b\\c\n");
        pp_error_json(PP_E_NAME, &evil, out, sizeof out);
        CHECK(!strchr(out + 28, '\n') && strstr(out, "a b c "), "the detail text is made JSON safe");
    }
    CHECK(!strcmp(pp_err_code(PP_E_SYNTAX), "bad_json") && !strcmp(pp_err_code(PP_E_TOOMANY), "too_many_steps") && !strcmp(pp_err_code(PP_E_INUSE), "in_use") && !strcmp(pp_err_code(-99), "error"), "error codes");
}

/* deterministic fuzz of the JSON reader: mutated valid bodies never crash (ASan/UBSan) and the accepted ones are valid routes */
static void test_fuzz(void)
{
    static const char *const seeds[] = {
        "{\"name\":\"yard\",\"mode\":\"loop\",\"steps\":[{\"preset\":\"door\",\"speed\":3,\"dwell_ms\":2000},{\"pan\":900,\"tilt\":1200}]}",
        "{\"name\":\"sw\",\"mode\":\"pingpong\",\"pattern\":{\"type\":\"sweep_h\",\"pan_from\":400,\"pan_to\":3000,\"tilt\":1300,\"step\":500,\"speed\":4,\"dwell_ms\":300}}",
        "{\"name\":\"ra\",\"pattern\":{\"type\":\"raster\",\"pan_from\":600,\"pan_to\":2400,\"tilt_from\":800,\"tilt_to\":1600,\"rows\":3,\"cols\":3}}",
    };
    unsigned long long s = 88172645463325252ULL;
    static pp_route_t r;
    static pp_cfg_t cf;
    pp_diag_t dg;
    char buf[400];
    int it, accepted = 0, bad = 0;
    printf("== fuzz of the JSON reader\n");
    pp_cfg_init(&cf);
    pp_preset_set(&cf, &LIM, "door", 900, 1200, &dg);
    for (it = 0; it < 60000; it++) {
        const char *seed = seeds[it % 3];
        size_t n = strlen(seed), len = n;
        int k, muts = 1 + (int)(s % 4);
        memcpy(buf, seed, n + 1);
        for (k = 0; k < muts; k++) {
            s ^= s << 13;
            s ^= s >> 7;
            s ^= s << 17;
            switch (s % 5) {
            case 0: buf[(s >> 8) % len] = (char)(s >> 20); break;
            case 1: buf[(s >> 8) % len] = "{}[]\",:0123456789-.eEtrufalsn \\"[(s >> 20) % 28]; break;
            case 2: len = (s >> 8) % len + 1; break; /* truncate */
            case 3: memmove(buf + (s >> 8) % len, buf + (s >> 8) % len + 1, len - (s >> 8) % len), len--; break;
            default: if (len + 1 < sizeof buf) { size_t at = (s >> 8) % len; memmove(buf + at + 1, buf + at, len - at); buf[at] = "\"{}:,0 "[(s >> 20) % 7]; len++; }
            }
            if (!len)
                len = 1;
        }
        if (pp_route_from_json(buf, len, &LIM, &r, &dg) == PP_OK) {
            accepted++;
            int rc = pp_route_check(&cf, &LIM, &r, &dg); /* a mutated preset name is unknown to the config: the only legitimate failure */
            if (rc != PP_OK && rc != PP_E_NOPRESET && rc != PP_E_NAME && rc != PP_E_RANGE && rc != PP_E_SPEED) {
                if (!bad)
                    printf("    rc %d for %.*s\n", rc, (int)len, buf);
                bad++;
            }
        }
    }
    CHECK(bad == 0, "every accepted mutation is a valid route");
    printf("  info - %d of 60000 mutations were still accepted\n", accepted);
    CHECK(accepted > 0, "(sanity: some mutations are harmless)");
}

/* ------------------------------------------------------------------------------------------------ the simulated motor and the driver loop */
typedef struct {
    int pan, tilt;
    int moving;
    int64_t done_at;
    int dest_pan, dest_tilt;
    int nmoves;
    struct { int64_t t; int pan, tilt, speed; } moves[256];
    int nev;
    struct { int64_t t; int type, step; char reason[16]; } ev[256];
} sim_t;

static void sim_init(sim_t *m, int pan, int tilt) { memset(m, 0, sizeof *m); m->pan = pan; m->tilt = tilt; }

static void sim_events(sim_t *m, const pp_out_t *o, int64_t now)
{
    int i;
    for (i = 0; i < o->nev && m->nev < 256; i++) {
        m->ev[m->nev].t = now;
        m->ev[m->nev].type = o->ev[i].type;
        m->ev[m->nev].step = o->ev[i].step;
        snprintf(m->ev[m->nev].reason, sizeof m->ev[m->nev].reason, "%s", o->ev[i].reason ? o->ev[i].reason : "");
        m->nev++;
    }
}

static void sim_act(sim_t *m, const pp_out_t *o)
{
    if (o->act == PP_ACT_CANCEL)
        m->moving = 0;
}

/* one binding tick: the move result, pp_tick, a MOVE action issued to the motor (time: pan then tilt at `speed` ms per step) */
static void sim_tick(pp_t *p, sim_t *m, int64_t now, int busy_motor, int refuse)
{
    pp_in_t in;
    pp_out_t o, o2;
    in.now_ms = now;
    in.arrived = in.failed = 0;
    if (m->moving && now >= m->done_at) {
        m->moving = 0;
        m->pan = m->dest_pan;
        m->tilt = m->dest_tilt;
        in.arrived = 1;
    }
    pp_tick(p, &in, &o);
    sim_events(m, &o, now);
    sim_act(m, &o);
    if (o.act == PP_ACT_MOVE) {
        if (refuse) {
            pp_moved(p, -1, now, &o2);
        } else if (busy_motor) {
            pp_moved(p, 1, now, &o2);
        } else {
            int d = abs(o.pan - m->pan) + abs(o.tilt - m->tilt);
            m->moving = 1;
            m->dest_pan = o.pan;
            m->dest_tilt = o.tilt;
            m->done_at = now + (int64_t)d * o.speed;
            if (m->nmoves < 256) {
                m->moves[m->nmoves].t = now;
                m->moves[m->nmoves].pan = o.pan;
                m->moves[m->nmoves].tilt = o.tilt;
                m->moves[m->nmoves].speed = o.speed;
                m->nmoves++;
            }
            pp_moved(p, 0, now, &o2);
        }
        sim_events(m, &o2, now);
        sim_act(m, &o2);
    }
}

static void run_for(pp_t *p, sim_t *m, int64_t *now, int64_t ms)
{
    int64_t end = *now + ms;
    while (*now < end) {
        sim_tick(p, m, *now, 0, 0);
        *now += 100;
    }
}

static int nev_of(const sim_t *m, int type)
{
    int i, n = 0;
    for (i = 0; i < m->nev; i++)
        n += m->ev[i].type == type;
    return n;
}

static void test_run(void)
{
    static pp_cfg_t c;
    static pp_t p;
    static sim_t m;
    pp_diag_t dg;
    pp_out_t o;
    int64_t now = 1000;
    int i;
    printf("== run: sequences, dwell timing, modes (simulated motor, fake clock)\n");
    pp_cfg_init(&c);
    pp_preset_set(&c, &LIM, "door", 900, 1200, &dg);
    {
        static const int pts[3][4] = { { 0, 0, 3, 2000 }, { 1500, 1300, 5, 400 }, { 2400, 1300, 2, 0 } };
        pp_route_t r = mkroute("yard", PP_LOOP, 3, pts);
        strcpy(r.step[0].preset, "door");
        CHECK(pp_route_set(&c, &LIM, &r, &dg) == PP_OK, "route set");
    }
    sim_init(&m, 1275, 1300);
    pp_init(&p);
    CHECK(pp_start(&p, &c, &LIM, "nothere", now, &o) == PP_E_NOTFOUND && p.phase == PP_IDLE, "an unknown route is not started");
    CHECK(pp_start(&p, &c, &LIM, "yard", now, &o) == PP_OK && p.phase == PP_RUNNING && o.nev == 1 && o.ev[0].type == PP_EV_STARTED && o.ev[0].step == 0 && o.act == PP_ACT_NONE, "start: event started, no move yet");
    sim_events(&m, &o, now);
    run_for(&p, &m, &now, 12000);
    CHECK(m.nmoves >= 4 && m.moves[0].pan == 900 && m.moves[0].tilt == 1200 && m.moves[0].speed == 3 && m.moves[1].pan == 1500 && m.moves[1].speed == 5 && m.moves[2].pan == 2400 && m.moves[2].speed == 2 &&
          m.moves[3].pan == 900, "the moves follow the steps with their speeds, the preset position is used, then it loops");
    {
        /* dwell timing: from the arrival of move 0 to the start of move 1 is the dwell (1000 ms) */
        int64_t arr0 = m.moves[0].t + (abs(900 - 1275) + abs(1200 - 1300)) * 3; /* 1475 steps x 3 ms */
        CHECK(m.moves[1].t - arr0 >= 2000 && m.moves[1].t - arr0 <= 2000 + 200, "dwell 2000 ms is kept (within one 100 ms tick)");
        CHECK(m.moves[1].t > m.moves[0].t + 1000 + 1000, "a move is never issued before the previous one arrived (travel time included)");
    }
    CHECK(nev_of(&m, PP_EV_STARTED) == 1 && nev_of(&m, PP_EV_STEP) >= 3, "events: started once, a step event per step (rate limited to 1 per second)");
    CHECK(p.cycles >= 1, "the loop wrapped (cycles counted)");
    pp_stop(&p, "manual", &o);
    CHECK(p.phase == PP_IDLE && o.nev == 1 && o.ev[0].type == PP_EV_STOPPED && !strcmp(o.ev[0].reason, "manual"), "stop: event stopped with its reason");
    CHECK(o.act == PP_ACT_CANCEL || !m.moving, "...a running move is cancelled");

    /* dwell floor: dwell 0 never spins */
    {
        static const int pts[2][4] = { { 600, 1000, 2, 0 }, { 700, 1000, 2, 0 } };
        pp_route_t r = mkroute("fast", PP_LOOP, 2, pts);
        sim_t m2;
        pp_t p2;
        int64_t t = 0;
        pp_route_set(&c, &LIM, &r, &dg);
        sim_init(&m2, 600, 1000);
        pp_init(&p2);
        pp_start(&p2, &c, &LIM, "fast", t, &o);
        run_for(&p2, &m2, &t, 10000);
        CHECK(m2.nmoves >= 4 && m2.nmoves <= 8, "dwell 0 in a loop route is floored at 1.5 s: a hop every ~1.7 s, not a busy loop");
        CHECK(nev_of(&m2, PP_EV_STEP) <= 11, "step events are at most one per second");
    }

    /* once */
    {
        static const int pts[2][4] = { { 600, 1000, 2, 300 }, { 700, 1000, 2, 300 } };
        pp_route_t r = mkroute("once", PP_ONCE, 2, pts);
        sim_t m2;
        pp_t p2;
        int64_t t = 0;
        pp_route_set(&c, &LIM, &r, &dg);
        sim_init(&m2, 650, 1000);
        pp_init(&p2);
        pp_start(&p2, &c, &LIM, "once", t, &o);
        run_for(&p2, &m2, &t, 5000);
        CHECK(p2.phase == PP_IDLE && m2.nmoves == 2 && nev_of(&m2, PP_EV_STOPPED) == 1 && !strcmp(m2.ev[m2.nev - 1].reason, "done") && m2.ev[m2.nev - 1].step == 1, "once: two moves, then stopped with reason done at the last step");
        run_for(&p2, &m2, &t, 3000);
        CHECK(m2.nmoves == 2, "...and nothing moves afterwards");
    }

    /* pingpong */
    {
        static const int pts[4][4] = { { 600, 1000, 2, 250 }, { 700, 1000, 2, 250 }, { 800, 1000, 2, 250 }, { 900, 1000, 2, 250 } };
        pp_route_t r = mkroute("pp", PP_PINGPONG, 4, pts);
        sim_t m2;
        pp_t p2;
        int64_t t = 0;
        static const int want[] = { 600, 700, 800, 900, 800, 700, 600, 700, 800, 900, 800 };
        pp_route_set(&c, &LIM, &r, &dg);
        sim_init(&m2, 600, 1000);
        pp_init(&p2);
        pp_start(&p2, &c, &LIM, "pp", t, &o);
        run_for(&p2, &m2, &t, 30000);
        for (i = 0; i < 11; i++)
            if (m2.nmoves <= i || m2.moves[i].pan != want[i])
                break;
        CHECK(i == 11, "pingpong: 0 1 2 3 2 1 0 1 2 3 2 (the end steps are visited once per pass)");
    }
    {
        static const int pts[1][4] = { { 600, 1000, 2, 250 } };
        pp_route_t r = mkroute("one", PP_PINGPONG, 1, pts);
        sim_t m2;
        pp_t p2;
        int64_t t = 0;
        pp_route_set(&c, &LIM, &r, &dg);
        sim_init(&m2, 600, 1000);
        pp_init(&p2);
        pp_start(&p2, &c, &LIM, "one", t, &o);
        run_for(&p2, &m2, &t, 10000);
        CHECK(p2.phase == PP_RUNNING && m2.nmoves >= 4 && m2.nmoves <= 8, "a one-step pingpong route is a held position, not a spin");
    }

    printf("== run: clamping and descriptor speeds\n");
    {
        static pp_cfg_t cs; /* a config stored under wider limits than the model has now (structure-only validation) */
        pp_route_t r;
        sim_t m2;
        pp_t p2;
        int64_t t = 0;
        static const int pts[3][4] = { { 100, 100, 1, 250 }, { 9000, 9000, 99, 250 }, { 1000, 1000, 0, 250 } };
        pp_cfg_init(&cs);
        r = mkroute("wide", PP_ONCE, 3, pts);
        CHECK(pp_route_set(&cs, NULL, &r, &dg) == PP_OK, "(structure-only validation accepts it)");
        sim_init(&m2, 1000, 1000);
        pp_init(&p2);
        pp_start(&p2, &cs, &LIM, "wide", t, &o);
        run_for(&p2, &m2, &t, 120000);
        CHECK(m2.nmoves == 3 && m2.moves[0].pan == 300 && m2.moves[0].tilt == 300 && m2.moves[1].pan == 3650 && m2.moves[1].tilt == 2100, "run-time clamp: every target is clamped to the soft range");
        CHECK(m2.moves[0].speed == 2 && m2.moves[1].speed == 20 && m2.moves[2].speed == 3, "run-time speeds: clamped to the descriptor's 2..20, 0 = the default");
    }
    {
        static const int pts[2][4] = { { 900, 1200, 3, 250 }, { 1000, 1200, 3, 250 } };
        pp_route_t r = mkroute("snap", PP_LOOP, 2, pts);
        sim_t m2;
        pp_t p2;
        int64_t t = 0;
        pp_route_set(&c, &LIM, &r, &dg);
        sim_init(&m2, 900, 1200);
        pp_init(&p2);
        pp_start(&p2, &c, &LIM, "snap", t, &o);
        r.step[0].pan = 1111; /* editing the config after the start changes nothing of the run (it keeps a resolved copy) */
        pp_route_set(&c, &LIM, &r, &dg);
        pp_preset_set(&c, &LIM, "door", 2000, 2000, &dg);
        run_for(&p2, &m2, &t, 3000);
        CHECK(m2.nmoves >= 2 && m2.moves[0].pan == 900 && m2.moves[1].pan == 1000, "a running route is a snapshot: later config edits do not disturb it");
    }
}

static void test_priority(void)
{
    static pp_cfg_t c;
    static pp_t p;
    static sim_t m;
    pp_diag_t dg;
    pp_out_t o;
    int64_t now = 0;
    int i, step_at_pause;
    printf("== run: tracking preemption and resume, stop conditions, failures\n");
    pp_cfg_init(&c);
    {
        static const int pts[4][4] = { { 600, 1000, 3, 500 }, { 1600, 1000, 3, 500 }, { 2600, 1000, 3, 500 }, { 3600, 1000, 3, 500 } };
        pp_route_t r = mkroute("route", PP_PINGPONG, 4, pts);
        pp_route_set(&c, &LIM, &r, &dg);
    }
    sim_init(&m, 600, 1000);
    pp_init(&p);
    pp_start(&p, &c, &LIM, "route", now, &o);
    sim_events(&m, &o, now);
    for (i = 0; i < 400 && !(p.phase == PP_RUNNING && p.sub == PP_ST_MOVING && p.step == 2); i++, now += 100)
        sim_tick(&p, &m, now, 0, 0); /* the long move to step 2 (1000 steps x 3 ms) */
    CHECK(p.phase == PP_RUNNING && p.sub == PP_ST_MOVING, "(a move is in flight)");
    step_at_pause = p.step;
    pp_interrupt(&p, now, &o);
    sim_events(&m, &o, now);
    sim_act(&m, &o);
    CHECK(p.phase == PP_PAUSED && o.act == PP_ACT_CANCEL && o.nev == 1 && o.ev[0].type == PP_EV_INTERRUPTED && !strcmp(o.ev[0].reason, "tracking") && o.ev[0].step == step_at_pause && !m.moving,
          "a person: running -> paused, the move in flight is cancelled, event interrupted (reason tracking)");
    {
        int n0 = m.nmoves;
        run_for(&p, &m, &now, 5000);
        CHECK(m.nmoves == n0 && p.phase == PP_PAUSED, "paused: no move at all");
    }
    pp_interrupt(&p, now, &o);
    CHECK(o.nev == 0 && o.act == PP_ACT_NONE, "a second interrupt does nothing");
    {
        int64_t tp = p.paused_at + 7000; /* a person seen 7 s after the interruption */
        CHECK(!pp_resume_due(&p, tp, 33000, tp + 32999) && pp_resume_due(&p, tp, 33000, tp + 33000) && !pp_resume_due(&p, tp, 33000, tp + 1000),
              "resume is due once no person was seen for the return time (grace + delay), counted from the last person");
        CHECK(pp_resume_due(&p, 0, 4000, p.paused_at + 4000) && !pp_resume_due(&p, 0, 9000, p.paused_at + 4000),
              "the interruption time counts when no later person time exists (paused_at)");
    }
    {
        pp_t idle;
        pp_init(&idle);
        CHECK(!pp_resume_due(&idle, 0, 0, now), "an idle patrol never resumes");
    }
    {
        int n0 = m.nmoves;
        pp_resume(&p, now, &o);
        sim_events(&m, &o, now);
        CHECK(p.phase == PP_RUNNING && o.nev == 1 && o.ev[0].type == PP_EV_RESUMED && o.ev[0].step == step_at_pause, "resume: event resumed at the interrupted step");
        run_for(&p, &m, &now, 1000);
        CHECK(m.nmoves == n0 + 1 && m.moves[n0].pan == 600 + 1000 * (step_at_pause) && p.step == step_at_pause, "...and it moves to that step again (not home, not the next one)");
    }
    pp_resume(&p, now, &o);
    CHECK(o.nev == 0, "resume while running does nothing");
    /* pingpong direction is kept across a pause */
    pp_init(&p);
    sim_init(&m, 3600, 1000);
    now = 0;
    pp_start(&p, &c, &LIM, "route", now, &o);
    for (i = 0; i < 4; i++) /* walk to the end and one step back */
        run_for(&p, &m, &now, 6000);
    for (i = 0; i < 200 && !(p.dir < 0 && p.sub == PP_ST_DWELL); i++)
        sim_tick(&p, &m, now, 0, 0), now += 100;
    {
        int step = p.step, dir = p.dir;
        pp_interrupt(&p, now, &o);
        pp_resume(&p, now, &o);
        CHECK(p.step == step && p.dir == dir && dir < 0, "the pingpong direction and step survive a pause");
    }
    /* stop conditions */
    pp_stop(&p, "privacy", &o);
    CHECK(p.phase == PP_IDLE && o.ev[0].type == PP_EV_STOPPED && !strcmp(o.ev[0].reason, "privacy"), "stop from running (privacy)");
    pp_stop(&p, "again", &o);
    CHECK(o.nev == 0 && o.act == PP_ACT_NONE, "stop when idle is a no-op");
    pp_start(&p, &c, &LIM, "route", now, &o);
    pp_interrupt(&p, now, &o);
    pp_stop(&p, "park", &o);
    CHECK(p.phase == PP_IDLE && o.nev == 1 && !strcmp(o.ev[0].reason, "park") && o.act == PP_ACT_NONE, "stop from paused: no move to cancel");
    pp_interrupt(&p, now, &o);
    CHECK(o.nev == 0 && p.phase == PP_IDLE, "interrupt when idle does nothing");
    /* replace */
    pp_start(&p, &c, &LIM, "route", now, &o);
    {
        static const int pts[1][4] = { { 900, 1000, 3, 500 } };
        pp_route_t r = mkroute("other", PP_LOOP, 1, pts);
        pp_route_set(&c, &LIM, &r, &dg);
    }
    pp_start(&p, &c, &LIM, "other", now, &o);
    CHECK(o.nev == 2 && o.ev[0].type == PP_EV_STOPPED && !strcmp(o.ev[0].reason, "replaced") && o.ev[1].type == PP_EV_STARTED && !strcmp(p.route.name, "other"), "start over a running route: stopped (replaced) then started");
    pp_stop(&p, "x", &o);

    /* failed moves */
    {
        sim_t m2;
        pp_t p2;
        int64_t t = 0;
        sim_init(&m2, 600, 1000);
        pp_init(&p2);
        pp_start(&p2, &c, &LIM, "route", t, &o);
        sim_tick(&p2, &m2, t, 0, 1); /* the controller refuses the move */
        CHECK(p2.phase == PP_RUNNING && p2.fails == 1, "a refused move is a failure, the patrol goes on");
        t += 100;
        sim_tick(&p2, &m2, t, 0, 0);
        CHECK(m2.nmoves == 0, "...and retries only after a second");
        t += 1000;
        sim_tick(&p2, &m2, t, 0, 1);
        t += 1100;
        sim_tick(&p2, &m2, t, 0, 1);
        CHECK(p2.phase == PP_IDLE && nev_of(&m2, PP_EV_STOPPED) == 1 && !strcmp(m2.ev[m2.nev - 1].reason, "move_failed"), "3 failed moves in a row stop the patrol (move_failed)");
    }
    {
        sim_t m2;
        pp_t p2;
        int64_t t = 0;
        pp_in_t in;
        sim_init(&m2, 600, 1000);
        pp_init(&p2);
        pp_start(&p2, &c, &LIM, "route", t, &o);
        sim_tick(&p2, &m2, t, 0, 0); /* move issued */
        t += 100;
        in.now_ms = t;
        in.arrived = 0;
        in.failed = 1; /* it failed or was cancelled by someone else */
        pp_tick(&p2, &in, &o);
        CHECK(p2.fails == 1 && p2.sub == PP_ST_PENDING && p2.retry_at == t + PP_RETRY_MS, "a failed move: retry in one second");
        t += 1000;
        for (i = 0; i < 100; i++, t += 100)
            sim_tick(&p2, &m2, t, 0, 0);
        CHECK(p2.fails == 0 && p2.phase == PP_RUNNING, "a successful move resets the failure count");
    }
    {
        sim_t m2;
        pp_t p2;
        int64_t t = 0;
        sim_init(&m2, 600, 1000);
        pp_init(&p2);
        pp_start(&p2, &c, &LIM, "route", t, &o);
        for (i = 0; i < 100; i++, t += 100)
            sim_tick(&p2, &m2, t, 1, 0); /* the controller is busy */
        CHECK(p2.phase == PP_RUNNING && p2.fails == 0 && m2.nmoves == 0, "a busy controller is retried every tick and is no failure");
        for (; t < PP_BUSY_MAX_MS + 1000; t += 100)
            sim_tick(&p2, &m2, t, 1, 0);
        CHECK(p2.phase == PP_IDLE && !strcmp(m2.ev[m2.nev - 1].reason, "busy"), "a controller busy for a minute ends the patrol");
    }
    /* idle trigger helper */
    CHECK(!pp_idle_due(0, 0, 1000000) && !pp_idle_due(30, 0, 29999) && pp_idle_due(30, 0, 30000) && pp_idle_due(1, 5000, 6000) && !pp_idle_due(-5, 0, 100000), "idle trigger: off at 0, due after N seconds");
    CHECK(!strcmp(pp_phase_name(&p), "idle") && !strcmp(pp_event_name(PP_EV_RESUMED), "resumed") && !strcmp(pp_event_name(PP_EV_STEP), "step"), "names");
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    test_model();
    test_park();
    test_patterns();
    test_text();
    test_json();
    test_fuzz();
    test_run();
    test_priority();
    printf("rot2k ptz_patrol unit: %d failed\n", fails);
    return fails ? 1 : 0;
}
