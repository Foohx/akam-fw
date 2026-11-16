/* The process-wide tracker of the app (akam2k_track.h) bound to the real motor controller. The detection thread calls a2k_track_feed()
 * after every inference; the Home Assistant controls (main) edit the switch and the areas through the a2k_trk_* wrappers. */
#include <stdio.h>

#include "akam2k_log.h"
#include "akam2k_ptz.h"
#include "akam2k_store.h"
#include "akam2k_track.h"
#include "akam2k_track_run.h"

static a2k_track_t g_t;
static volatile int g_up;

static int real_range(int r[4])
{
    uint32_t a, b, c, d;
    if (a2k_ptz_range(&a, &b, &c, &d) != 0)
        return -1;
    r[0] = (int)a;
    r[1] = (int)b;
    r[2] = (int)c;
    r[3] = (int)d;
    return 0;
}

static int real_home(void) { return a2k_ptz_command(A2K_PTZ_HOME); }

void a2k_track_run_start(int enabled, const char *conf_areas, void (*notify)(void), int invert_pan, int invert_tilt)
{
    static const a2k_track_ops_t ops = { a2k_ptz_info, real_range, a2k_ptz_nudge, real_home, a2k_ptz_privacy_active };
    a2k_track_init(&g_t, &ops, A2K_STORE_TRACK);
    g_t.notify = notify;
    g_t.cfg.invert_pan = invert_pan; /* the 2K binding: a person on one side must turn the camera TOWARD it (measured: not inverted = away) */
    g_t.cfg.invert_tilt = invert_tilt;
    (void)tl_init(&g_t.tl, &g_t.cfg);
    (void)a2k_track_load(&g_t, conf_areas);
    a2k_track_set_enabled(&g_t, enabled);
    g_up = 1;
    a2k_log("track: invert_pan %d invert_tilt %d", invert_pan, invert_tilt);
    a2k_log("track: person tracking %s", enabled ? "on" : "off (akam2k.conf track=1 or the Home Assistant switch turns it on)");
}

void a2k_track_run_stop(void)
{
    if (!g_up)
        return;
    g_up = 0;
    a2k_track_set_enabled(&g_t, 0); /* takes the tracker lock: an inference tick in progress finishes first */
}

a2k_track_t *a2k_track_run_get(void) { return g_up ? &g_t : 0; }

void a2k_track_feed(int64_t now_ms, const struct tl_box *boxes, int n, int fw, int fh)
{
    if (g_up)
        a2k_track_tick(&g_t, now_ms, boxes, n, fw, fh);
}
