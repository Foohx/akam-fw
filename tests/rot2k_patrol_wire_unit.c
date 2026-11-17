/* Host unit test of the PTZ patrol binding core (app/akam2k_patrol.c over the pure engine ptz_patrol.c; specs/ptz-patrol.md) on a FAKE motor and a FAKE
 * clock: the tick is driven directly (no thread), except in the last case. Covers: off by default (nothing moves, nothing is written), the config
 * (one flash write per accepted change, none for an unchanged one, none while patrolling, the write limiter), the priorities (manual command, park,
 * calibration, privacy, lost position, shutdown stop; a tracked person pauses and the patrol resumes at the interrupted step; the tracker does not send
 * the head home meanwhile, with the REAL tracker core), the idle trigger, the soft range, and a real thread run. Prints "rot2k patrol wire unit: N failed". */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "akam2k_patrol.h"
#include "akam2k_track.h"

static int fails;
#define CHECK(c, msg) do { if (c) printf("  ok   - %s\n", msg); else { printf("  FAIL - %s\n", msg); fails++; } } while (0)

void a2k_log(const char *fmt, ...) { (void)fmt; }

/* ---------------------------------------------------------------------------------------------- fakes */
static int64_t g_now;               /* the fake clock (ms) */
static int g_real_clock;            /* the thread test uses the real one */
static a2k_ptz_info_t g_info;
static int g_privacy;
static int g_pan = 1275, g_tilt = 1300, g_moving, g_dpan, g_dtilt;
static int64_t g_done_at, g_move_ms = 300;
static unsigned g_tk, g_done_tk;
static int g_done_rc;
static unsigned long g_travel;
static int g_hold;
static unsigned long f_travel(void) { return g_travel; }
static int f_hold(void) { return g_hold; }
static int g_nmoves, g_ncancel, g_ngoto, g_refuse, g_busy;
static int g_mv_pan[512], g_mv_tilt[512], g_mv_speed[512];
static char g_events[64][128];
static int g_nev;
static char g_file[PP_TEXT_MAX + 1];
static int g_file_len = -1, g_saves, g_fail_save;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;

static int64_t f_now(void)
{
    if (g_real_clock) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    }
    return g_now;
}

static void finish_locked(void)
{
    if (g_moving && f_now() >= g_done_at) {
        g_pan = g_dpan;
        g_tilt = g_dtilt;
        g_moving = 0;
        g_done_tk = g_tk;
        g_done_rc = 0;
    }
}

static void f_info(a2k_ptz_info_t *o)
{
    pthread_mutex_lock(&g_mu);
    finish_locked();
    *o = g_info;
    if (o->state == HAL_PTZ_HOME || o->state == HAL_PTZ_MOVED) {
        o->state = g_moving ? HAL_PTZ_MOVED : (g_pan == 1275 && g_tilt == 1300 ? HAL_PTZ_HOME : HAL_PTZ_MOVED);
        o->known = !g_moving;
        o->pan = (uint32_t)g_pan;
        o->tilt = (uint32_t)g_tilt;
    }
    pthread_mutex_unlock(&g_mu);
}

static int f_limits(pp_limits_t *l)
{
    pp_limits_t v = { 300, 3650, 300, 2100, 3, 20, 3, 1275, 300, 15 };
    *l = v;
    return 0;
}

static int f_move(int pan, int tilt, int speed, unsigned *ticket)
{
    int rc = 0;
    pthread_mutex_lock(&g_mu);
    finish_locked();
    if (!ticket) { /* a manual goto */
        g_moving = 0;
        g_pan = pan;
        g_tilt = tilt;
        g_ngoto++;
    } else if (g_refuse) {
        rc = -1;
    } else if (g_busy || g_moving) {
        rc = 1;
    } else {
        if (pan < 300 || pan > 3650 || tilt < 300 || tilt > 2100) { /* the controller refuses outside the soft range: the test catches a leak */
            fails++;
            printf("  FAIL - a move outside the soft range reached the controller: %d/%d\n", pan, tilt);
        }
        if (g_nmoves < 512) {
            g_mv_pan[g_nmoves] = pan;
            g_mv_tilt[g_nmoves] = tilt;
            g_mv_speed[g_nmoves] = speed;
        }
        g_nmoves++;
        g_moving = 1;
        g_dpan = pan;
        g_dtilt = tilt;
        g_done_at = f_now() + g_move_ms;
        *ticket = ++g_tk;
    }
    pthread_mutex_unlock(&g_mu);
    return rc;
}

static int f_result(unsigned t, int *rc)
{
    int done = 1;
    pthread_mutex_lock(&g_mu);
    finish_locked();
    if (g_moving && t == g_tk)
        done = 0;
    else
        *rc = g_done_tk == t ? g_done_rc : 7;
    pthread_mutex_unlock(&g_mu);
    return done;
}

static void f_cancel(void)
{
    pthread_mutex_lock(&g_mu);
    if (g_moving) {
        g_moving = 0;
        g_done_tk = g_tk;
        g_done_rc = 7;
        g_ncancel++;
    }
    pthread_mutex_unlock(&g_mu);
}

static int f_privacy(void) { return g_privacy; }

static void f_event(const char *type, const char *json)
{
    pthread_mutex_lock(&g_mu);
    if (!strcmp(type, "patrol") && g_nev < 64)
        snprintf(g_events[g_nev++], sizeof g_events[0], "%s", json);
    pthread_mutex_unlock(&g_mu);
}

static int f_load(char *buf, size_t cap)
{
    if (g_file_len < 0)
        return -1;
    if ((size_t)g_file_len >= cap)
        return -2;
    memcpy(buf, g_file, (size_t)g_file_len + 1);
    return g_file_len;
}

static int f_save(const char *text, size_t len)
{
    if (g_fail_save)
        return -1;
    if (g_file_len >= 0 && (size_t)g_file_len == len && !memcmp(g_file, text, len))
        return 0;
    memcpy(g_file, text, len);
    g_file[len] = '\0';
    g_file_len = (int)len;
    g_saves++;
    return 1;
}

static const a2k_patrol_ops_t OPS = { f_info, f_limits, f_move, f_result, f_cancel, f_privacy, f_event, f_load, f_save, f_now, f_hold, f_travel };

static void at_state(int state)
{
    memset(&g_info, 0, sizeof g_info);
    g_info.state = state;
    g_info.known = state == HAL_PTZ_HOME || state == HAL_PTZ_MOVED;
}

static void reset_world(void)
{
    a2k_patrol_stop();
    a2k_patrol_test_reset();
    g_now = 100000;
    g_real_clock = 0;
    g_pan = 1275;
    g_tilt = 1300;
    g_moving = 0;
    g_nmoves = g_ncancel = g_ngoto = g_nev = g_saves = 0;
    g_refuse = g_busy = g_privacy = g_fail_save = 0;
    g_travel = 0;
    g_hold = 0;
    a2k_patrol_test_set_min_idle(A2K_PATROL_MIN_IDLE_S);
    g_move_ms = 300;
    g_file_len = -1;
    g_file[0] = '\0';
    at_state(HAL_PTZ_HOME);
}

/* runs the patrol for ms of fake time, ticking every 100 ms */
static void run(int64_t ms)
{
    int64_t end = g_now + ms;
    while (g_now < end) {
        g_now += 100;
        a2k_patrol_tick();
    }
}

static char OUT[12288];
static int http(const char *method, const char *sub, const char *body)
{
    unsigned retry = 0;
    return a2k_patrol_http(method, sub, body, body ? strlen(body) : 0, OUT, sizeof OUT, &retry);
}

static int moves_now(void)
{
    int n;
    pthread_mutex_lock(&g_mu);
    n = g_nmoves;
    pthread_mutex_unlock(&g_mu);
    return n;
}

static int state_is(const char *want)
{
    char st[16], rt[17];
    int step;
    a2k_patrol_state(st, sizeof st, rt, sizeof rt, &step);
    return !strcmp(st, want);
}

static int has_event(const char *needle)
{
    int i;
    for (i = 0; i < g_nev; i++)
        if (strstr(g_events[i], needle))
            return 1;
    return 0;
}

static void setup_routes(void)
{
    http("POST", "presets", "{\"name\":\"door\",\"pan\":900,\"tilt\":1200}");
    http("POST", "presets", "{\"name\":\"porch\",\"pan\":1500,\"tilt\":1100}");
    http("POST", "routes", "{\"name\":\"yard\",\"mode\":\"loop\",\"steps\":[{\"preset\":\"door\",\"speed\":3,\"dwell_ms\":1000},{\"preset\":\"porch\",\"speed\":5,\"dwell_ms\":1000},{\"pan\":2400,\"tilt\":1300,\"dwell_ms\":1000}]}");
}

/* ---------------------------------------------------------------------------------------------- the tests */
static void test_off_by_default(void)
{
    char d[PP_NAME_MAX + 1];
    printf("== off by default\n");
    reset_world();
    a2k_patrol_init(&OPS);
    CHECK(a2k_patrol_get_idle_s() == 0 && !a2k_patrol_active(), "idle_start_s is 0, nothing active");
    a2k_patrol_get_default(d, sizeof d);
    CHECK(!d[0], "no default route");
    run(600000);
    CHECK(g_nmoves == 0 && g_saves == 0 && g_nev == 0 && state_is("idle") && a2k_patrol_writes() == 0, "ten minutes of ticks: no move, no event, no flash write");
    http("POST", "presets", "{\"name\":\"door\",\"pan\":900,\"tilt\":1200}");
    http("POST", "routes", "{\"name\":\"yard\",\"steps\":[{\"preset\":\"door\"}]}");
    a2k_patrol_set_default("yard");
    run(600000);
    CHECK(g_nmoves == 0 && state_is("idle"), "a config and a default route alone start nothing while idle_start_s is 0");
    CHECK(a2k_patrol_set_idle_s(-1) < 0 && a2k_patrol_set_idle_s(86401) < 0 && a2k_patrol_set_idle_s(86400) == 0 && a2k_patrol_get_idle_s() == 86400 &&
          a2k_patrol_set_default("Bad") < 0 && a2k_patrol_set_default("") == 0, "the settings setters validate");
}

static void test_config_and_flash(void)
{
    int i;
    printf("== config: one flash write per change, none while patrolling, the limiter\n");
    reset_world();
    a2k_patrol_init(&OPS);
    CHECK(http("POST", "presets", "{\"name\":\"door\",\"pan\":900,\"tilt\":1200}") == 200 && g_saves == 1 && a2k_patrol_writes() == 1, "a first change: 200, one write");
    CHECK(http("POST", "presets", "{\"name\":\"door\",\"pan\":900,\"tilt\":1200}") == 200 && g_saves == 1 && a2k_patrol_writes() == 1, "the same again: no write");
    CHECK(http("POST", "presets", "{\"name\":\"x\",\"pan\":10,\"tilt\":1200}") == 422 && g_saves == 1, "a refused change writes nothing");
    CHECK(http("POST", "routes", "{\"name\":\"yard\",\"mode\":\"loop\",\"steps\":[{\"preset\":\"door\",\"speed\":3,\"dwell_ms\":1000},{\"pan\":2400,\"tilt\":1300,\"dwell_ms\":1000}]}") == 200 && g_saves == 2, "a route: one write");
    g_fail_save = 1;
    CHECK(http("POST", "presets", "{\"name\":\"other\",\"pan\":1000,\"tilt\":1200}") == 500 && strstr(OUT, "store_failed") && g_saves == 2, "a failed write refuses the change");
    g_fail_save = 0;
    CHECK(http("GET", "presets/other", NULL) == 404, "...and it was not applied (all or nothing)");
    g_now += 3600000;
    CHECK(http("POST", "presets", "{\"name\":\"other\",\"pan\":1000,\"tilt\":1200}") == 200 && g_saves == 3, "after the failure the same change goes through");
    {
        int saves0 = g_saves, mv0;
        a2k_patrol_start_route("yard");
        run(60000);
        mv0 = g_nmoves;
        a2k_patrol_note_person(1, 5000);
        run(3000);
        run(8000);
        a2k_patrol_note_cmd(A2K_PTZ_LEFT);
        run(500);
        CHECK(mv0 > 10 && g_saves == saves0 && a2k_patrol_writes() == 3, "a minute of patrol, a pause, a resume and a stop: zero writes");
    }
    /* the limiter: burst 8, then one per 10 s (the fake clock) */
    reset_world();
    a2k_patrol_init(&OPS);
    for (i = 1; i <= 8; i++) {
        char b[64];
        snprintf(b, sizeof b, "{\"name\":\"p%d\",\"pan\":%d,\"tilt\":1000}", i, 400 + i * 100);
        if (http("POST", "presets", b) != 200)
            break;
    }
    CHECK(i == 9 && g_saves == 8, "8 changes in a burst");
    {
        unsigned retry = 0;
        int st = a2k_patrol_http("POST", "presets", "{\"name\":\"p9\",\"pan\":1000,\"tilt\":1000}", 36, OUT, sizeof OUT, &retry);
        CHECK(st == 429 && strstr(OUT, "\"error\":\"rate\"") && retry >= 1 && retry <= 10 && g_saves == 8, "the 9th is 429 rate with a retry time, nothing written");
    }
    g_now += 9000;
    CHECK(http("POST", "presets", "{\"name\":\"p9\",\"pan\":1000,\"tilt\":1000}") == 429, "9 s later still limited");
    g_now += 1500;
    CHECK(http("POST", "presets", "{\"name\":\"p9\",\"pan\":1000,\"tilt\":1000}") == 200 && g_saves == 9, "10 s after the burst one more write is allowed");
    CHECK(http("DELETE", "presets/p9", NULL) == 429, "(and the next one waits again)");
    CHECK(http("GET", "presets", NULL) == 200 && strstr(OUT, "\"p9\""), "reads are never limited");
    /* delete / in use / default */
    reset_world();
    a2k_patrol_init(&OPS);
    setup_routes();
    a2k_patrol_set_default("yard");
    CHECK(http("DELETE", "routes/yard", NULL) == 409 && strstr(OUT, "in_use"), "the default route cannot be deleted");
    CHECK(http("DELETE", "presets/door", NULL) == 409 && strstr(OUT, "in_use"), "a used preset cannot be deleted");
    a2k_patrol_set_default("");
    a2k_patrol_start_route("yard");
    CHECK(http("DELETE", "routes/yard", NULL) == 409 && strstr(OUT, "running"), "a running route cannot be deleted");
    CHECK(http("POST", "routes", "{\"name\":\"yard\",\"steps\":[{\"pan\":900,\"tilt\":1200}]}") == 409 && strstr(OUT, "running"), "...or replaced");
    a2k_patrol_stop_route();
    CHECK(http("POST", "routes", "{\"name\":\"yard\",\"steps\":[{\"pan\":900,\"tilt\":1200}]}") == 200 && http("DELETE", "routes/yard", NULL) == 200 && http("DELETE", "presets/door", NULL) == 200,
          "once stopped it can be replaced and deleted, then its preset");
    CHECK(http("DELETE", "routes", NULL) == 405 && http("PATCH", "routes", "{}") == 405 && http("POST", "routes/yard", "{}") == 405 && http("GET", "nothing", NULL) == 404 && http("GET", "routes/Bad", NULL) == 422,
          "wrong methods and paths");
    /* the whole config must fit 4096 bytes: a large one is refused with 413 and nothing changes */
    {
        char body[3000];
        int n = 0, saves0;
        reset_world();
        a2k_patrol_init(&OPS);
        n += snprintf(body, sizeof body, "{\"name\":\"big0\",\"steps\":[");
        for (i = 0; i < 32; i++)
            n += snprintf(body + n, sizeof body - (size_t)n, "%s{\"pan\":%d,\"tilt\":2000,\"speed\":10,\"dwell_ms\":3600000}", i ? "," : "", 3000 + i);
        snprintf(body + n, sizeof body - (size_t)n, "]}");
        for (i = 0; i < 8; i++) {
            int st;
            body[12] = (char)('0' + i);
            saves0 = g_saves;
            g_now += 20000;
            st = http("POST", "routes", body);
            if (st != 200) {
                CHECK(st == 413 && strstr(OUT, "too_big") && g_saves == saves0, "a route that makes the config exceed 4096 bytes: 413 too_big, nothing written");
                break;
            }
        }
        CHECK(i < 8 && i >= 3, "...the limit is hit after a few maximal routes");
    }
    /* load: a stored file is parsed, a bad one ignored */
    reset_world();
    strcpy(g_file, "p door 900 1200\nr yard loop\ns door 3 1000\ns 2400:1300 0 500\n");
    g_file_len = (int)strlen(g_file);
    a2k_patrol_init(&OPS);
    CHECK(http("GET", "routes/yard", NULL) == 200 && strstr(OUT, "\"preset\":\"door\",\"speed\":3,\"dwell_ms\":1000") && g_saves == 0, "the stored config is loaded at start and not rewritten");
    reset_world();
    strcpy(g_file, "r yard loop\ns nothere 3 1000\n");
    g_file_len = (int)strlen(g_file);
    a2k_patrol_init(&OPS);
    CHECK(http("GET", "routes", NULL) == 200 && strstr(OUT, "\"routes\":[]") && g_saves == 0, "an invalid stored file is ignored (empty config), not rewritten");
}

static void test_run_and_priorities(void)
{
    int mv;
    printf("== run through the binding, soft range, speeds\n");
    reset_world();
    a2k_patrol_init(&OPS);
    setup_routes();
    CHECK(a2k_patrol_start_route("nope") == PP_E_NOTFOUND && state_is("idle"), "an unknown route is not started");
    CHECK(a2k_patrol_start_route("yard") == PP_OK && state_is("running") && a2k_patrol_active() && has_event("\"event\":\"started\",\"route\":\"yard\",\"step\":0"), "start: running, event started");
    run(12000);
    CHECK(g_nmoves >= 5 && g_mv_pan[0] == 900 && g_mv_tilt[0] == 1200 && g_mv_speed[0] == 3 && g_mv_pan[1] == 1500 && g_mv_speed[1] == 5 && g_mv_pan[2] == 2400 && g_mv_speed[2] == 3 && g_mv_pan[3] == 900,
          "moves: door@3, porch@5, explicit@default 3, then the loop");
    CHECK(has_event("\"event\":\"step\",\"route\":\"yard\",\"step\":1"), "step events");
    {
        char st[16], rt[17];
        int step;
        a2k_patrol_state(st, sizeof st, rt, sizeof rt, &step);
        CHECK(!strcmp(st, "running") && !strcmp(rt, "yard") && step >= 0 && step < 3, "state: running, route yard, a step");
    }
    CHECK(a2k_patrol_stop_route() == 0 && state_is("idle") && has_event("\"event\":\"stopped\",\"route\":\"yard\"") && has_event("\"reason\":\"manual\""), "stop: event stopped, reason manual");
    mv = g_nmoves;
    run(5000);
    CHECK(g_nmoves == mv, "no move after the stop");
    CHECK(a2k_patrol_stop_route() == 0, "stop when idle is idempotent");

    printf("== the controller refuses: busy, refused, a stopped state\n");
    reset_world();
    a2k_patrol_init(&OPS);
    setup_routes();
    g_busy = 1;
    a2k_patrol_start_route("yard");
    run(10000);
    CHECK(g_nmoves == 0 && state_is("running"), "a busy controller: no move, the patrol waits (no preemption)");
    g_busy = 0;
    run(2000);
    CHECK(g_nmoves >= 1, "...and goes on as soon as it is idle");
    reset_world();
    a2k_patrol_init(&OPS);
    setup_routes();
    g_refuse = 1;
    a2k_patrol_start_route("yard");
    run(10000);
    CHECK(state_is("idle") && has_event("\"reason\":\"move_failed\"") && g_nmoves == 0, "refused moves: stopped after 3 (move_failed)");

    printf("== priorities: privacy, park, calibration, lost position, manual, shutdown\n");
    reset_world();
    a2k_patrol_init(&OPS);
    setup_routes();
    at_state(HAL_PTZ_UNKNOWN);
    CHECK(a2k_patrol_start_route("yard") == PP_E_BUSY && state_is("idle"), "start refused while the position is unknown");
    at_state(HAL_PTZ_CALIBRATING);
    CHECK(a2k_patrol_start_route("yard") == PP_E_BUSY, "...or calibrating");
    at_state(HAL_PTZ_HOME);
    g_privacy = 1;
    CHECK(a2k_patrol_start_route("yard") == PP_E_BUSY, "...or while the video is withheld (privacy)");
    g_privacy = 0;
    CHECK(a2k_patrol_start_route("yard") == PP_OK, "(ready again)");
    run(2000);
    g_privacy = 1;
    run(200);
    CHECK(state_is("idle") && has_event("\"reason\":\"privacy\""), "privacy becoming active stops the patrol");
    g_privacy = 0;
    a2k_patrol_start_route("yard");
    run(1000);
    at_state(HAL_PTZ_UNKNOWN);
    run(200);
    CHECK(state_is("idle") && has_event("\"reason\":\"state\""), "a lost position stops it (reason state)");
    at_state(HAL_PTZ_HOME);
    a2k_patrol_start_route("yard");
    run(1000);
    at_state(HAL_PTZ_CALIBRATING);
    run(200);
    CHECK(state_is("idle") && has_event("\"reason\":\"calibration\""), "a calibration stops it");
    at_state(HAL_PTZ_HOME);
    a2k_patrol_start_route("yard");
    run(1000);
    a2k_patrol_note_cmd(A2K_PTZ_PARK);
    run(200);
    CHECK(state_is("idle") && has_event("\"reason\":\"park\""), "the park command stops it (reason park)");
    a2k_patrol_start_route("yard");
    run(1000);
    a2k_patrol_note_cmd(A2K_PTZ_RECAL);
    run(200);
    CHECK(state_is("idle") && has_event("\"reason\":\"calibration\""), "a recalibration command stops it (reason calibration)");
    a2k_patrol_start_route("yard");
    run(1000);
    a2k_patrol_note_cmd(A2K_PTZ_HOOK_GOTO);
    a2k_patrol_note_cmd(A2K_PTZ_HOOK_NUDGE);
    run(200);
    CHECK(state_is("idle") && has_event("\"reason\":\"manual\""), "a manual goto / nudge stops it (reason manual)");
    a2k_patrol_note_cmd(A2K_PTZ_UP);
    run(1000);
    CHECK(state_is("idle") && g_nmoves > 0, "a command while idle starts nothing");
    {
        int ngoto = g_ngoto;
        CHECK(a2k_patrol_goto_preset("door") == PP_OK && g_ngoto == ngoto + 1 && a2k_patrol_goto_preset("nope") == PP_E_NOTFOUND, "goto preset: a manual move through the controller");
    }
    a2k_patrol_start_route("yard");
    run(1000);
    a2k_patrol_stop();
    CHECK(state_is("idle") && has_event("\"reason\":\"shutdown\"") && !a2k_patrol_active(), "shutdown cancels the patrol (reason shutdown)");
}

/* the tracker (real core) + the patrol (real core) on a fake motor and a shared fake clock */
static a2k_ptz_info_t t_info;
static int t_nudges, t_homes;
static void t_f_info(a2k_ptz_info_t *o) { *o = t_info; }
static int t_f_range(int r[4]) { r[0] = 300; r[1] = 3650; r[2] = 300; r[3] = 1750; return 0; }
static int t_f_nudge(int dx, int dy, int sp) { (void)dx; (void)dy; (void)sp; t_nudges++; return 0; }
static int t_f_home(void) { t_homes++; return 0; }
static int t_f_priv(void) { return 0; }
static int t_tilt_home(void) { return 1300; }

static void trk_run(a2k_track_t *t, int64_t ms, int person)
{
    int64_t end = g_now + ms;
    static const struct tl_box b = { 270, 56, 310, 136, 0, 0 };
    while (g_now < end) {
        g_now += 100;
        if (person)
            a2k_track_tick(t, g_now, &b, 1, 320, 192);
        else
            a2k_track_tick(t, g_now, NULL, 0, 320, 192);
        a2k_patrol_tick();
    }
}

static void test_tracking(void)
{
    static const a2k_track_ops_t TOPS = { t_f_info, t_f_range, t_f_nudge, t_f_home, t_f_priv, t_tilt_home, a2k_patrol_note_person, a2k_patrol_active, NULL, NULL, NULL, NULL, NULL, NULL, NULL };
    a2k_track_t t;
    struct tl_cfg c;
    int step, mv;
    char st[16], rt[17];
    printf("== person tracking preempts the patrol and the patrol resumes at the interrupted step (real tracker core)\n");
    reset_world();
    a2k_patrol_init(&OPS);
    setup_routes();
    memset(&t_info, 0, sizeof t_info);
    t_info.state = HAL_PTZ_MOVED;
    t_info.known = 1;
    t_info.pan = 1275;
    t_info.tilt = 1300;
    a2k_track_init(&t, &TOPS, NULL);
    tl_cfg_legacy(&c); /* grace 10 s, return home right after it */
    c.return_home = 1;
    c.return_delay_s = 0;
    (void)a2k_track_set_cfg(&t, &c);
    a2k_track_set_enabled(&t, 1);
    a2k_patrol_start_route("yard");
    trk_run(&t, 3500, 0);
    a2k_patrol_state(st, sizeof st, rt, sizeof rt, &step);
    CHECK(!strcmp(st, "running") && g_nmoves >= 1, "(the patrol runs, nobody around)");
    trk_run(&t, 1500, 1);
    CHECK(state_is("paused") && has_event("\"event\":\"interrupted\",\"route\":\"yard\"") && has_event("\"reason\":\"tracking\""), "a person with tracking enabled pauses the patrol");
    mv = g_nmoves;
    a2k_patrol_state(st, sizeof st, rt, sizeof rt, &step);
    trk_run(&t, 4000, 1);
    CHECK(g_nmoves == mv && state_is("paused") && t_nudges > 0, "while the person is there: the tracker moves the head, the patrol does not");
    trk_run(&t, 8000, 0);
    CHECK(state_is("paused") && t_homes == 0, "8 s after the person left: still paused (grace 10 s)");
    trk_run(&t, 6000, 0);
    CHECK(state_is("running") && has_event("\"event\":\"resumed\",\"route\":\"yard\""), "after the lost grace + return delay (10 s): the patrol resumes");
    CHECK(t_homes == 0, "...and the tracker did NOT send the head home (it is the patrol's turn)");
    trk_run(&t, 4000, 0);
    {
        int found = 0, i;
        for (i = mv; i < g_nmoves && i < 512; i++)
            found |= g_mv_pan[i] == (step == 0 ? 900 : step == 1 ? 1500 : 2400);
        CHECK(found, "the first move after the resume goes to the interrupted step");
    }
    /* with no patrol the tracker still goes home as before */
    a2k_patrol_stop_route();
    trk_run(&t, 2000, 1);
    trk_run(&t, 12000, 0);
    CHECK(t_homes == 1 && state_is("idle"), "no patrol: the tracker goes home after the loss as before");
    /* tracking disabled: a person does not pause the patrol, but it is not idle either */
    a2k_track_set_enabled(&t, 0);
    a2k_patrol_start_route("yard");
    trk_run(&t, 3000, 1);
    CHECK(state_is("running"), "tracking off: a person does not pause the patrol");
    a2k_patrol_stop_route();
    a2k_track_destroy(&t);
    /* tracking enabled but not acting here (the hook says tracking = 0): no pause */
    reset_world();
    a2k_patrol_init(&OPS);
    setup_routes();
    a2k_patrol_start_route("yard");
    run(1000);
    a2k_patrol_note_person(0, 33000);
    run(500);
    CHECK(state_is("running"), "a person the tracker does not act on (not allowed here / disabled) does not pause it");
    a2k_patrol_note_person(1, 4000);
    run(500);
    CHECK(state_is("paused"), "a person it acts on does");
    run(3000);
    CHECK(state_is("paused"), "paused for the resume time given by the tracker (4 s)");
    run(1500);
    CHECK(state_is("running"), "...then resumed");
}

static void test_idle_trigger(void)
{
    printf("== idle trigger\n");
    reset_world();
    a2k_patrol_init(&OPS);
    setup_routes();
    a2k_patrol_set_default("yard");
    a2k_patrol_set_idle_s(30);
    run(29000);
    CHECK(state_is("idle") && g_nmoves == 0, "not before 30 s");
    run(2500);
    CHECK(state_is("running") && has_event("\"event\":\"started\",\"route\":\"yard\""), "after 30 s without person or command the default route starts");
    a2k_patrol_stop_route();
    run(29000);
    CHECK(state_is("idle"), "a stop restarts the idle clock");
    a2k_patrol_note_cmd(A2K_PTZ_LEFT);
    run(20000);
    a2k_patrol_note_cmd(A2K_PTZ_LEFT);
    run(20000);
    CHECK(state_is("idle"), "a manual command restarts the idle clock");
    a2k_patrol_note_person(0, 33000);
    run(20000);
    a2k_patrol_note_person(0, 33000);
    run(20000);
    CHECK(state_is("idle"), "a person (tracked or not) restarts it too");
    run(15000);
    CHECK(state_is("running"), "(and then it starts)");
    a2k_patrol_stop_route();
    at_state(HAL_PTZ_PARKED);
    run(120000);
    CHECK(state_is("idle"), "parked: no idle start");
    at_state(HAL_PTZ_UNKNOWN);
    run(120000);
    CHECK(state_is("idle"), "unknown position: no idle start");
    at_state(HAL_PTZ_HOME);
    g_privacy = 1;
    run(120000);
    CHECK(state_is("idle"), "privacy: no idle start");
    g_privacy = 0;
    run(35000);
    CHECK(state_is("running"), "(ready again: it starts)");
    a2k_patrol_stop_route();
    a2k_patrol_set_default("ghost");
    a2k_patrol_set_idle_s(10);
    run(60000);
    CHECK(state_is("idle") && g_nev > 0, "a default route that does not exist is not started");
    a2k_patrol_set_idle_s(0);
    a2k_patrol_set_default("yard");
    run(600000);
    CHECK(state_is("idle"), "idle_start_s 0 turns it off");
    /* a once route that ends restarts the clock, then runs again after the idle time */
    reset_world();
    a2k_patrol_init(&OPS);
    http("POST", "presets", "{\"name\":\"door\",\"pan\":900,\"tilt\":1200}");
    http("POST", "routes", "{\"name\":\"one\",\"mode\":\"once\",\"steps\":[{\"preset\":\"door\",\"dwell_ms\":300}]}");
    a2k_patrol_set_default("one");
    a2k_patrol_set_idle_s(30);
    run(32000);
    CHECK(has_event("\"event\":\"started\",\"route\":\"one\""), "the once route starts after 30 s");
    run(5000);
    CHECK(state_is("idle") && has_event("\"reason\":\"done\""), "...and ends by itself");
    {
        int n = g_nev;
        run(12000);
        CHECK(g_nev == n, "after it ended the idle clock restarts (no immediate rerun)");
        run(20000);
        CHECK(g_nev > n, "...and it runs again after another idle period");
    }
}


static void test_review_fixes(void)
{
    int n, t0;
    printf("== bounded and safe unattended motion (review fixes)\n");
    /* M1: nothing at the park position */
    reset_world();
    a2k_patrol_init(&OPS);
    CHECK(http("POST", "presets", "{\"name\":\"park1\",\"pan\":1280,\"tilt\":310}") == 422 && strstr(OUT, "park position"), "a preset near the park position (1275/300 +-15) is refused (422 range)");
    CHECK(http("POST", "presets", "{\"name\":\"ok1\",\"pan\":1275,\"tilt\":316}") == 200, "16 steps away is fine");
    CHECK(http("POST", "routes", "{\"name\":\"r\",\"steps\":[{\"pan\":1275,\"tilt\":305}]}") == 422 && strstr(OUT, "park position"), "a route step near the park position is refused");
    CHECK(http("POST", "routes", "{\"name\":\"r\",\"pattern\":{\"type\":\"sweep_v\",\"tilt_from\":300,\"tilt_to\":900,\"pan\":1275,\"step\":300}}") == 422, "a sweep that starts at the park position is refused");
    /* M4: speed floor, dwell floor */
    CHECK(http("POST", "routes", "{\"name\":\"fast\",\"steps\":[{\"pan\":900,\"tilt\":1200,\"speed\":2}]}") == 422 && strstr(OUT, "\"speed\""), "a patrol speed faster than the default (2 < 3) is refused");
    http("POST", "routes", "{\"name\":\"z\",\"mode\":\"loop\",\"steps\":[{\"pan\":900,\"tilt\":1200},{\"pan\":1000,\"tilt\":1200}]}");
    a2k_patrol_start_route("z");
    run(30000);
    CHECK(g_nmoves >= 12 && g_nmoves <= 20, "dwell 0 in a loop route is floored at 1.5 s (about 1 move per 1.8 s, not 1 per 0.5 s)");
    a2k_patrol_stop_route();
    /* M4: travel budget */
    reset_world();
    a2k_patrol_init(&OPS);
    http("POST", "routes", "{\"name\":\"z\",\"mode\":\"loop\",\"steps\":[{\"pan\":900,\"tilt\":1200},{\"pan\":1000,\"tilt\":1200}]}");
    CHECK(a2k_patrol_set_travel_budget(999) < 0 && a2k_patrol_set_travel_budget(5000) == 0 && a2k_patrol_get_travel_budget() == 5000, "the travel budget setting is bounded");
    g_travel = 4000;
    CHECK(a2k_patrol_start_route("z") == PP_OK, "under the budget it starts");
    run(2000);
    g_travel = 5001;
    run(300);
    CHECK(state_is("idle") && has_event("\"reason\":\"travel\""), "past the travel budget the patrol stops (reason travel)");
    CHECK(a2k_patrol_start_route("z") == PP_E_BUSY, "...and does not start again until the next calibration");
    a2k_patrol_set_default("z");
    a2k_patrol_set_idle_s(30);
    n = g_nmoves;
    run(120000);
    CHECK(g_nmoves == n && state_is("idle"), "...nor through the idle trigger");
    /* M2: the idle clock starts when the PTZ is ready, never during the boot calibration or the scene reference */
    reset_world();
    a2k_patrol_init(&OPS);
    http("POST", "routes", "{\"name\":\"z\",\"mode\":\"loop\",\"steps\":[{\"pan\":900,\"tilt\":1200}]}");
    a2k_patrol_set_default("z");
    a2k_patrol_set_idle_s(1);
    at_state(HAL_PTZ_CALIBRATING);
    run(300000);
    at_state(HAL_PTZ_HOME);
    run(20000);
    CHECK(state_is("idle"), "idle_start_s 1: never fires during the calibration, nor within 30 s after the PTZ is ready (minimum 30 s)");
    g_hold = 1;
    run(200000);
    CHECK(state_is("idle"), "...nor while the home scene reference is still due");
    g_hold = 0;
    run(25000);
    CHECK(state_is("idle"), "...the clock starts when it is released");
    run(10000);
    CHECK(state_is("running"), "...then it fires");
    a2k_patrol_stop_route();
    /* M3 + L2: only a confirmed person the tracker acts on pauses and keeps it paused */
    reset_world();
    a2k_patrol_init(&OPS);
    setup_routes();
    a2k_patrol_start_route("yard");
    run(1000);
    a2k_patrol_note_person(0, 33000);
    run(500);
    CHECK(state_is("running"), "an unconfirmed sighting does not pause");
    a2k_patrol_note_person(1, 200);
    run(300);
    CHECK(state_is("paused"), "a confirmed one does");
    t0 = 0;
    while (state_is("paused") && t0 < 20000) {
        run(100);
        t0 += 100;
        a2k_patrol_note_person(0, 0); /* somebody lingers that the tracker does not act on: it must not keep the patrol paused */
    }
    CHECK(state_is("running") && t0 >= 2500 && t0 <= 4000, "the resume delay is floored at 3 s, and unconfirmed sightings do not extend the pause");
    a2k_patrol_stop_route();
    /* L1: a target where the head already is needs no move */
    reset_world();
    a2k_patrol_init(&OPS);
    http("POST", "routes", "{\"name\":\"here\",\"mode\":\"once\",\"steps\":[{\"pan\":1275,\"tilt\":1300},{\"pan\":1279,\"tilt\":1297}]}");
    a2k_patrol_start_route("here");
    run(8000);
    CHECK(g_nmoves == 0 && has_event("\"reason\":\"done\""), "steps at the known position (+-5): reached without any move");
}

/* a real thread, the real clock: the loop runs, pauses and stops */
static void test_thread(void)
{
    int i;
    printf("== thread\n");
    reset_world();
    g_real_clock = 1;
    g_move_ms = 30;
    a2k_patrol_init(&OPS);
    setup_routes();
    {
        /* short dwell route so that the real-time run moves quickly */
        http("POST", "routes", "{\"name\":\"quick\",\"mode\":\"pingpong\",\"steps\":[{\"pan\":900,\"tilt\":1200,\"dwell_ms\":250},{\"pan\":1000,\"tilt\":1200,\"dwell_ms\":250}]}");
    }
    CHECK(a2k_patrol_thread_start() == 0, "the thread starts");
    CHECK(a2k_patrol_start_route("quick") == PP_OK, "start");
    for (i = 0; i < 150 && moves_now() < 3; i++)
        usleep(100000);
    CHECK(moves_now() >= 3 && state_is("running"), "it moves on its own (pingpong, dwell floored at 1.5 s)");
    a2k_patrol_note_person(1, 400);
    usleep(300000);
    CHECK(state_is("paused"), "a person pauses it");
    for (i = 0; i < 80 && !state_is("running"); i++)
        usleep(100000);
    CHECK(state_is("running"), "it resumes by itself (the 400 ms asked for is floored at 3 s)");
    a2k_patrol_note_cmd(A2K_PTZ_LEFT);
    for (i = 0; i < 20 && !state_is("idle"); i++)
        usleep(100000);
    CHECK(state_is("idle"), "a manual command stops it");
    a2k_patrol_start_route("quick");
    usleep(300000);
    a2k_patrol_stop();
    CHECK(state_is("idle") && !a2k_patrol_active(), "stop joins the thread and cancels the patrol");
    a2k_patrol_stop(); /* idempotent */
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    test_off_by_default();
    test_config_and_flash();
    test_run_and_priorities();
    test_tracking();
    test_idle_trigger();
    test_review_fixes();
    test_thread();
    printf("rot2k patrol wire unit: %d failed\n", fails);
    return fails ? 1 : 0;
}
