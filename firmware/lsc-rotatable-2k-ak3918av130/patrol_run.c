/* The 2K binding of the PTZ patrol (patrol.h): the real ops table over the PTZ controller (the same safe motion layer as every other move), the
 * event ring of the control API and the atomic store. */
#include <stdio.h>
#include <time.h>

#include "api_events.h"
#include "log.h"
#include "patrol.h"
#include "ptz.h"
#include "store.h"

_Static_assert(PP_TEXT_MAX <= A2K_STORE_MAX, "the patrol config fits one store file");

static int real_limits(pp_limits_t *l)
{
    uint32_t a, b, c, d;
    int smin, smax, sdef;
    if (a2k_ptz_range(&a, &b, &c, &d) != 0 || a2k_ptz_speeds(&smin, &smax, &sdef) != 0)
        return -1;
    l->pan_min = (int)a;
    l->pan_max = (int)b;
    l->tilt_min = (int)c;
    l->tilt_max = (int)d;
    l->speed_min = sdef; /* unattended motion: never faster than the default speed (the descriptor's fastest, smin, is for manual moves) */
    (void)smin;
    if (a2k_ptz_park(&l->park_pan, &l->park_tilt) != 0)
        return -1;
    l->park_tol = A2K_PTZ_PARK_TOL;
    l->speed_max = smax;
    l->speed_def = sdef;
    return 0;
}

static int real_move(int pan, int tilt, int speed, unsigned *ticket)
{
    /* with no ticket asked for it is a MANUAL move (goto preset): the controller tells the patrol hook, which stops a running patrol */
    if (ticket)
        return a2k_ptz_patrol_goto(pan, tilt, speed, ticket);
    return a2k_ptz_goto(pan, tilt, speed) == 0 ? 0 : -1;
}

static int real_load(char *buf, size_t cap) { return a2k_store_read(A2K_STORE_PATROL, buf, cap); }

/* 1 written, 0 unchanged, -1 failed: the store skips identical content and counts only the real writes */
static int real_save(const char *text, size_t len)
{
    unsigned before = a2k_store_writes();
    if (a2k_store_write(A2K_STORE_PATROL, text, len) != 0)
        return -1;
    return a2k_store_writes() != before;
}

static int64_t real_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int a2k_patrol_run_start(void)
{
    static const a2k_patrol_ops_t ops = { a2k_ptz_info, real_limits, real_move, a2k_ptz_patrol_result, a2k_ptz_patrol_cancel, a2k_ptz_privacy_active,
                                          a2k_api_event, real_load, real_save, real_now, a2k_ptz_scene_pending, a2k_ptz_travel };
    a2k_patrol_init(&ops);
    a2k_ptz_set_cmd_hook(a2k_patrol_note_cmd);
    if (a2k_patrol_thread_start() != 0) {
        a2k_log("patrol: thread start failed: patrol off");
        return -1;
    }
    return 0;
}

void a2k_patrol_run_stop(void)
{
    a2k_ptz_set_cmd_hook(NULL);
    a2k_patrol_stop();
}
