/* Stand-ins of the motion tracking (track.c, track_run.c, track_logic, track_relay) of the Rotatable 2K, linked INSTEAD of it when the feature is switched off (AKAM_FEAT_TRACK := 0 in model.mk). The rest of the app
 * (settings, detection, the HTTP and MQTT bindings, main) calls the feature unconditionally; with it off every call answers "not available": a refusal (-1),
 * an empty value or nothing, never a motion and never a file. Generated from the prototypes of the headers: a signature change fails the build here. */
#include <string.h>

#include "track.h"
#include "track_run.h"
#include "track_logic.h"
#include "track_relay.h"

int a2k_tareas_parse(const char *text, size_t len, a2k_tareas_t *out)
{
    (void)text;
    (void)len;
    if (out) memset(out, 0, sizeof *out);
    return 0; /* accepted and ignored (a track_area line left in akam.conf must not stop the app) */
}

int a2k_track_area_corner(a2k_track_t *t, int slot, int corner)
{
    (void)t;
    (void)slot;
    (void)corner;
    return -1;
}

int a2k_track_area_delete(a2k_track_t *t, int slot)
{
    (void)t;
    (void)slot;
    return -1;
}

int a2k_track_area_enable(a2k_track_t *t, int slot, int on)
{
    (void)t;
    (void)slot;
    (void)on;
    return -1;
}

int a2k_track_area_new(a2k_track_t *t, const char *name)
{
    (void)t;
    (void)name;
    return -1;
}

void a2k_track_areas(a2k_track_t *t, a2k_tareas_t *out)
{
    (void)t;
    if (out) memset(out, 0, sizeof *out);
}

void a2k_track_areas_text(a2k_track_t *t, char *out, size_t cap)
{
    (void)t;
    if (out && cap) out[0] = '\0';
}

int a2k_track_enabled(a2k_track_t *t)
{
    (void)t;
    return 0;
}

void a2k_track_feed(int64_t now_ms, int64_t cap_ms, const struct tl_box *boxes, int n, int fw, int fh)
{
    (void)now_ms;
    (void)cap_ms;
    (void)boxes;
    (void)n;
    (void)fw;
    (void)fh;
}

int a2k_track_get_close_scan(a2k_track_t *t, int which)
{
    (void)t;
    (void)which;
    return 0;
}

void a2k_track_get_gains(a2k_track_t *t, int *gain_pan, int *gain_tilt, int *tilt_band_pct)
{
    (void)t;
    if (gain_pan) *gain_pan = 0;
    if (gain_tilt) *gain_tilt = 0;
    if (tilt_band_pct) *tilt_band_pct = 0;
}

int a2k_track_get_lag(a2k_track_t *t)
{
    (void)t;
    return 0;
}

void a2k_track_get_lost_policy(a2k_track_t *t, char *out, size_t cap)
{
    (void)t;
    if (out && cap) out[0] = '\0';
}

void a2k_track_get_lost_step(a2k_track_t *t, char *out, size_t cap)
{
    (void)t;
    if (out && cap) out[0] = '\0';
}

int a2k_track_get_motion(a2k_track_t *t)
{
    (void)t;
    return 0;
}

void a2k_track_get_return(a2k_track_t *t, int *home, int *delay_s)
{
    (void)t;
    if (home) *home = 0;
    if (delay_s) *delay_s = 0;
}

int a2k_track_get_tilt(a2k_track_t *t)
{
    (void)t;
    return 0;
}

void a2k_track_get_tilt_back(a2k_track_t *t, int *s)
{
    (void)t;
    if (s) *s = 0;
}

void a2k_track_get_tilt_return(a2k_track_t *t, int *s)
{
    (void)t;
    if (s) *s = 0;
}

void a2k_track_get_tune(a2k_track_t *t, int *band_pct, int *head_pct)
{
    (void)t;
    if (band_pct) *band_pct = 0;
    if (head_pct) *head_pct = 0;
}

void a2k_track_get_zones(a2k_track_t *t, int *x_pct, int *y_pct, int *head_pct)
{
    (void)t;
    if (x_pct) *x_pct = 0;
    if (y_pct) *y_pct = 0;
    if (head_pct) *head_pct = 0;
}

void a2k_track_rec_ring(int64_t t_ms, int pan, int tilt)
{
    (void)t_ms;
    (void)pan;
    (void)tilt;
}

a2k_track_t *a2k_track_run_get(void)
{
    return NULL;
}

void a2k_track_run_start(int enabled, const char *conf_areas, void (*notify)(void), int invert_pan, int invert_tilt, const struct tl_cfg *tune)
{
    (void)enabled;
    (void)conf_areas;
    (void)notify;
    (void)invert_pan;
    (void)invert_tilt;
    (void)tune;
}

void a2k_track_run_stop(void)
{
}

int a2k_track_set_close_scan(a2k_track_t *t, int which, int v)
{
    (void)t;
    (void)which;
    (void)v;
    return -1;
}

void a2k_track_set_debug(a2k_track_t *t, int on)
{
    (void)t;
    (void)on;
}

void a2k_track_set_enabled(a2k_track_t *t, int on)
{
    (void)t;
    (void)on;
}

void a2k_track_set_gains(a2k_track_t *t, int gain_pan, int gain_tilt, int tilt_band_pct)
{
    (void)t;
    (void)gain_pan;
    (void)gain_tilt;
    (void)tilt_band_pct;
}

void a2k_track_set_lag(a2k_track_t *t, int lag_ms)
{
    (void)t;
    (void)lag_ms;
}

int a2k_track_set_lost_policy(a2k_track_t *t, const char *text)
{
    (void)t;
    (void)text;
    return -1;
}

void a2k_track_set_motion(a2k_track_t *t, int motion)
{
    (void)t;
    (void)motion;
}

void a2k_track_set_return(a2k_track_t *t, int home, int delay_s)
{
    (void)t;
    (void)home;
    (void)delay_s;
}

int a2k_track_set_invert(a2k_track_t *t, int invert_pan, int invert_tilt)
{
    (void)t;
    (void)invert_pan;
    (void)invert_tilt;
    return -1;
}

int a2k_track_set_tilt(a2k_track_t *t, int on)
{
    (void)t;
    (void)on;
    return -1;
}

void a2k_track_set_tilt_back(a2k_track_t *t, int s)
{
    (void)t;
    (void)s;
}

void a2k_track_set_tilt_return(a2k_track_t *t, int s)
{
    (void)t;
    (void)s;
}

void a2k_track_set_tune(a2k_track_t *t, int band_pct, int head_pct)
{
    (void)t;
    (void)band_pct;
    (void)head_pct;
}

void a2k_track_set_zones(a2k_track_t *t, int x_pct, int y_pct, int head_pct)
{
    (void)t;
    (void)x_pct;
    (void)y_pct;
    (void)head_pct;
}

void tl_cfg_defaults(struct tl_cfg *c)
{
    if (c) memset(c, 0, sizeof *c);
}

void tl_cfg_geometry_2k(struct tl_cfg *c)
{
    if (c) memset(c, 0, sizeof *c);
}

int tl_lost_parse(const char *s, struct tl_cfg *c)
{
    (void)s;
    (void)c; /* untouched: the caller's other tracking options stay */
    return 0; /* accepted and ignored */
}

void tr_init(tr_t *t)
{
    (void)t;
}

int tr_step(tr_t *t, const uint8_t *y, unsigned fw, unsigned fh, unsigned stride, const int det[4], int64_t now_ms, int out[4])
{
    (void)t;
    (void)y;
    (void)fw;
    (void)fh;
    (void)stride;
    (void)det;
    (void)now_ms;
    (void)out;
    return 0;
}
