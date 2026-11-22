/* Host unit test of the tracker wiring to the image orientation (track_run.c + track.c + orient.c on a FAKE motor): the listener track_run.c registers really
 * moves the policy's direction flags (base binding invert_pan 1 / invert_tilt 0 combined with mirror / flip), and a real change forgets the policy memory (world
 * positions built with the old signs). Prints "rot2k track orient unit: N failed". */
#define _GNU_SOURCE
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "api_events.h"
#include "orient.h"
#include "patrol.h"
#include "ptz.h"
#include "track_run.h"

static int fails;
#define CHECK(c, msg) do { if (c) printf("  ok   - %s\n", msg); else { printf("  FAIL - %s\n", msg); fails++; } } while (0)

void a2k_log(const char *fmt, ...) { (void)fmt; }
void a2k_api_event(const char *type, const char *json) { (void)type; (void)json; }

static a2k_ptz_info_t g_info;
void a2k_ptz_info(a2k_ptz_info_t *o) { *o = g_info; }
int a2k_ptz_range(uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d) { *a = 300; *b = 3650; *c = 300; *d = 1750; return 0; }
int a2k_ptz_tilt_home(void) { return 0; }
int a2k_ptz_home_tracker(void) { return 0; }
int a2k_ptz_nudge_sp(int dx, int dy, int sp) { (void)dx; (void)dy; (void)sp; return 0; }
int a2k_ptz_vel(int sx, int sy, int dx, int dy) { (void)sx; (void)sy; (void)dx; (void)dy; return 0; }
int a2k_ptz_pos_at(int64_t t, int *pan, int *tilt) { (void)t; *pan = (int)g_info.pan; *tilt = (int)g_info.tilt; return 0; }
unsigned long a2k_ptz_travel(void) { return 0; }
int a2k_ptz_privacy_active(void) { return 0; }
void a2k_ptz_set_lease_ms(int ms) { (void)ms; }
void a2k_ptz_note_person(void) {}
int a2k_patrol_start_route(const char *n) { (void)n; return -1; }
int a2k_patrol_stop_route(void) { return 0; }
int a2k_patrol_active(void) { return 0; }
int a2k_patrol_get_travel_budget(void) { return 1000000; }
void a2k_patrol_note_person(int tracking, int resume_ms) { (void)tracking; (void)resume_ms; }

int main(void)
{
    a2k_track_t *t;
    tl_t fresh;
    struct tl_box b = { 200 - 20, 96 - 40, 200 + 20, 96 + 40, 0, 0 };
    int i;
    a2k_orient_test_reset();
    memset(&g_info, 0, sizeof g_info);
    g_info.state = HAL_PTZ_HOME;
    g_info.known = 1;
    g_info.pan = 1275;
    g_info.tilt = 1300;
    a2k_track_run_start(1, NULL, NULL, 1, 0, NULL); /* the 2K binding: invert_pan 1, invert_tilt 0 */
    t = a2k_track_run_get();
    CHECK(t && t->cfg.invert_pan == 1 && t->cfg.invert_tilt == 0, "orientation off: the measured binding");

    for (i = 0; i < 6; i++)
        a2k_track_feed(1000 + i * 200, 1000 + i * 200, &b, 1, 320, 192);
    (void)tl_init(&fresh, &t->tl.cfg);
    CHECK(memcmp(&fresh, &t->tl, sizeof fresh) != 0, "a target is held: the policy has memory");

    a2k_orient_set(0, 1);
    CHECK(t->cfg.invert_pan == 0 && t->tl.cfg.invert_pan == 0 && t->cfg.invert_tilt == 0, "mirror: the listener toggled invert_pan in both copies of the config");
    (void)tl_init(&fresh, &t->tl.cfg);
    CHECK(memcmp(&fresh, &t->tl, sizeof fresh) == 0, "mirror: the policy memory was forgotten (old-sign world positions)");

    for (i = 0; i < 6; i++)
        a2k_track_feed(5000 + i * 200, 5000 + i * 200, &b, 1, 320, 192);
    (void)tl_init(&fresh, &t->tl.cfg);
    CHECK(memcmp(&fresh, &t->tl, sizeof fresh) != 0, "a new target is held");
    a2k_orient_set(1, 1);
    CHECK(t->cfg.invert_pan == 0 && t->cfg.invert_tilt == 1 && t->tl.cfg.invert_tilt == 1, "ceiling: invert_tilt toggled as well");
    (void)tl_init(&fresh, &t->tl.cfg);
    CHECK(memcmp(&fresh, &t->tl, sizeof fresh) == 0, "ceiling: the memory was forgotten again");

    a2k_orient_set(0, 0);
    CHECK(t->cfg.invert_pan == 1 && t->cfg.invert_tilt == 0, "orientation off again: the base binding is back");
    a2k_track_run_stop();
    printf("rot2k track orient unit: %d failed\n", fails);
    return fails != 0;
}
