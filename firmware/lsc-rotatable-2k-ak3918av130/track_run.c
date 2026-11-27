/* The process-wide tracker of the app (track.h) bound to the real motor controller. The detection thread calls a2k_track_feed()
 * after every inference; the Home Assistant controls (main) edit the switch and the areas through the a2k_trk_* wrappers. */
#include <stdio.h>

#include "api_events.h"
#include "log.h"
#include "orient.h"
#include "patrol.h"
#include "ptz.h"
#include "store.h"
#include "track.h"
#include "track_run.h"

static a2k_track_t g_t;
static volatile int g_up;
static int g_base_pan, g_base_tilt, g_listening; /* the 2K binding's invert_pan / invert_tilt, measured with the orientation off */

/* The tracker works in the DISPLAYED frame (the boxes come from the flipped picture), the motor in the sensor frame: a mirror reverses the pan direction, a flip the tilt (orient.h). */
static void orient_changed(int flip, int mirror)
{
    if (g_up)
        (void)a2k_track_set_invert(&g_t, a2k_orient_invert_pan(g_base_pan, mirror), a2k_orient_invert_tilt(g_base_tilt, flip));
}

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

static int real_tilt_home(void) { return a2k_ptz_tilt_home(); }
static int real_home(void) { return a2k_ptz_home_tracker(); } /* hookless: a tracker home is not a manual command */
static int real_nudge(int dx, int dy, int speed) { return a2k_ptz_nudge_sp(dx, dy, speed); }
static int real_route_start(const char *name) { return a2k_patrol_start_route(name); }
static int real_route_stop(void) { return a2k_patrol_stop_route(); }
static int real_route_busy(void) { return a2k_patrol_active(); }
static int real_over_budget(void) { return a2k_ptz_travel() > (unsigned long)a2k_patrol_get_travel_budget(); }
static void real_lost_event(const char *step)
{
    char js[96];
    snprintf(js, sizeof js, "{\"lost_step\":\"%s\"}", step[0] ? step : "idle");
    a2k_api_event("track", js);
}
static int real_vel(int sx, int sy, int dx, int dy) { return a2k_ptz_vel(sx, sy, dx, dy); }
static int real_pos_at(int64_t t, int *pan, int *tilt) { return a2k_ptz_pos_at(t, pan, tilt); }

void a2k_track_run_start(int enabled, const char *conf_areas, void (*notify)(void), int invert_pan, int invert_tilt, const struct tl_cfg *tune)
{
    static const a2k_track_ops_t ops = { a2k_ptz_info, real_range, real_nudge, real_home, a2k_ptz_privacy_active, real_tilt_home, a2k_patrol_note_person, a2k_patrol_active, real_vel, real_pos_at, real_route_start, real_route_stop, real_route_busy, real_lost_event, real_over_budget };
    a2k_track_init(&g_t, &ops, A2K_STORE_TRACK);
    g_t.notify = notify;
    if (tune)
        g_t.cfg = *tune;
    {
        int flip, mirror;
        g_base_pan = invert_pan;
        g_base_tilt = invert_tilt;
        a2k_orient_get(&flip, &mirror);
        invert_pan = a2k_orient_invert_pan(g_base_pan, mirror);
        invert_tilt = a2k_orient_invert_tilt(g_base_tilt, flip);
    }
    g_t.cfg.invert_pan = invert_pan; /* the 2K binding: a person on one side must turn the camera TOWARD it (measured: not inverted = away); combined with the image orientation */
    g_t.cfg.invert_tilt = invert_tilt;
    (void)tl_init(&g_t.tl, &g_t.cfg);
    a2k_ptz_set_lease_ms(g_t.cfg.lease_ms);
    (void)a2k_track_load(&g_t, conf_areas);
    a2k_track_set_enabled(&g_t, enabled);
    g_up = 1;
    if (!g_listening) {
        g_listening = 1;
        (void)a2k_orient_listen(orient_changed);
    }
    a2k_log("track: invert_pan %d invert_tilt %d, band %d%%, tilt %d (head %d%%), lead %d%%, return %s after %d s + %d s grace", invert_pan, invert_tilt,
            g_t.cfg.deadband_pct, g_t.cfg.tilt_enable, g_t.cfg.head_pct, g_t.cfg.lead_pct, g_t.cfg.return_home ? "home" : "stay", g_t.cfg.return_delay_s,
            g_t.cfg.lost_grace_s);
    a2k_log("track: motion %s, comfort zone %d%% x %d%% (head %d%%), lease %d ms", g_t.cfg.motion == TL_MOTION_CONT ? "continuous" : "steps", g_t.cfg.zone_x_pct, g_t.cfg.zone_y_pct,
            g_t.cfg.zone_head_pct, g_t.cfg.lease_ms);
    a2k_log("track: person tracking %s", enabled ? "on" : "off (akam.conf track=1 or the Home Assistant switch turns it on)");
}

void a2k_track_run_stop(void)
{
    if (!g_up)
        return;
    g_up = 0;
    a2k_track_set_enabled(&g_t, 0); /* takes the tracker lock: an inference tick in progress finishes first */
}

a2k_track_t *a2k_track_run_get(void) { return g_up ? &g_t : 0; }

void a2k_track_run_set_motion(const struct tl_mot *m)
{
    if (g_up)
        tl_set_motion(&g_t.tl, m); /* the detection thread both sets it and ticks the policy: no race with the tick */
}

void a2k_track_feed(int64_t now_ms, int64_t cap_ms, const struct tl_box *boxes, int n, int fw, int fh)
{
    if (n > 0)
        a2k_ptz_note_person(); /* a person is in view: no drift check / drift recalibration of the head meanwhile */
    if (g_up)
        a2k_track_tick_ts(&g_t, now_ms, cap_ms, boxes, n, fw, fh);
}
