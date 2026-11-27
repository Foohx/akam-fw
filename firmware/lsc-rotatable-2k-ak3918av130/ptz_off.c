/* Stand-ins of the PTZ (the motor controller ptz.c, the velocity parser ptz_vel and the ONVIF PTZ service) of the Rotatable 2K, linked INSTEAD of it when the feature is switched off (AKAM_FEAT_PTZ := 0 in model.mk). The rest of the app
 * (settings, detection, the HTTP and MQTT bindings, main) calls the feature unconditionally; with it off every call answers "not available": a refusal (-1),
 * an empty value or nothing, never a motion and never a file. Generated from the prototypes of the headers: a signature change fails the build here. */
#include <string.h>

#include "ptz.h"
#include "ptz_vel.h"
#include "drift_match.h"
#include "onvif_services.h"

int a2k_onvif_ptz_register(void)
{
    return 0;
}

void a2k_onvif_set_ptz_ops(const a2k_onvif_ptz_ops_t *ops)
{
    (void)ops;
}

int a2k_ptz_command(int cmd)
{
    (void)cmd;
    return -1;
}

void a2k_ptz_defaults(a2k_ptz_cfg_t *c)
{
    if (c) memset(c, 0, sizeof *c);
}

void a2k_ptz_drift_info(a2k_ptz_drift_t *out)
{
    if (out) memset(out, 0, sizeof *out);
}

int a2k_ptz_drift_ref_info(unsigned long *age_s)
{
    if (age_s) *age_s = 0;
    return -1;
}

void a2k_ptz_emergency_stop(void)
{
}

int a2k_ptz_goto(int pan, int tilt, int speed)
{
    (void)pan;
    (void)tilt;
    (void)speed;
    return -1;
}

int a2k_ptz_home_tracker(void)
{
    return -1;
}

void a2k_ptz_info(a2k_ptz_info_t *out)
{
    if (out) memset(out, 0, sizeof *out);
}

int a2k_ptz_manual_gate(void)
{
    return -1;
}

int a2k_ptz_manual_stop(void)
{
    return 0;
}

void a2k_ptz_note_person(void)
{
}

int a2k_ptz_nudge(int dx, int dy)
{
    (void)dx;
    (void)dy;
    return -1;
}

int a2k_ptz_nudge_sp(int dx, int dy, int speed)
{
    (void)dx;
    (void)dy;
    (void)speed;
    return -1;
}

int a2k_ptz_oneshot_stop(void)
{
    return 0;
}

int a2k_ptz_park(int *pan, int *tilt)
{
    if (pan) *pan = 0;
    if (tilt) *tilt = 0;
    return -1;
}

int a2k_ptz_parse_boot_calibrate(const char *s)
{
    (void)s;
    return 0; /* a switched-off feature accepts and ignores its configuration line (a ptz_* line left in akam.conf must not stop the app) */
}

int a2k_ptz_parse_cal(const char *s)
{
    (void)s;
    return 0; /* a switched-off feature accepts and ignores its configuration line (a ptz_* line left in akam.conf must not stop the app) */
}

int a2k_ptz_parse_drift_correct(const char *s)
{
    (void)s;
    return 0; /* a switched-off feature accepts and ignores its configuration line (a ptz_* line left in akam.conf must not stop the app) */
}

int a2k_ptz_parse_park(const char *s, int *pan, int *tilt)
{
    (void)s;
    if (pan) *pan = 0;
    if (tilt) *tilt = 0;
    return 0; /* accepted and ignored, as the other parsers of the absent feature */
}

int a2k_ptz_parse_tilt_rehome(const char *s)
{
    (void)s;
    return 0; /* a switched-off feature accepts and ignores its configuration line (a ptz_* line left in akam.conf must not stop the app) */
}

void a2k_ptz_patrol_cancel(void)
{
}

int a2k_ptz_patrol_goto(int pan, int tilt, int speed, unsigned *ticket)
{
    (void)pan;
    (void)tilt;
    (void)speed;
    if (ticket) *ticket = 0;
    return -1;
}

int a2k_ptz_patrol_result(unsigned ticket, int *rc)
{
    (void)ticket;
    if (rc) *rc = 0;
    return -1;
}

int a2k_ptz_pos_at(int64_t t_ms, int *pan, int *tilt)
{
    (void)t_ms;
    if (pan) *pan = 0;
    if (tilt) *tilt = 0;
    return -1;
}

int a2k_ptz_privacy_active(void)
{
    return 0;
}

int a2k_ptz_range(uint32_t *pan_min, uint32_t *pan_max, uint32_t *tilt_min, uint32_t *tilt_max)
{
    (void)pan_min;
    (void)pan_max;
    (void)tilt_min;
    (void)tilt_max;
    return -1;
}

void a2k_ptz_stall_info(unsigned long *stall_est, unsigned *cal_full, unsigned *cal_short, unsigned *rehomes)
{
    if (stall_est) *stall_est = 0;
    if (cal_full) *cal_full = 0;
    if (cal_short) *cal_short = 0;
    if (rehomes) *rehomes = 0;
}

void a2k_ptz_rehome_info(unsigned *runs, unsigned *cancels)
{
    if (runs) *runs = 0;
    if (cancels) *cancels = 0;
}

void a2k_ptz_rehome_last(unsigned long *last_s, const char **reason)
{
    if (last_s) *last_s = 0;
    if (reason) *reason = "";
}

int a2k_ptz_scene_pending(void)
{
    return 0; /* a boolean: nothing is pending */
}

void a2k_ptz_set_cmd_hook(void (*fn)(int kind))
{
    (void)fn;
}

void a2k_ptz_set_lease_ms(int ms)
{
    (void)ms;
}

void a2k_ptz_set_notify(void (*fn)(const a2k_ptz_info_t *))
{
    (void)fn;
}

void a2k_ptz_set_ring_hook(void (*cb)(int64_t t_ms, int pan, int tilt))
{
    (void)cb;
}

void a2k_ptz_set_scene(const a2k_ptz_scene_t *s)
{
    (void)s;
}

int a2k_ptz_speeds(int *min, int *max, int *def)
{
    if (min) *min = 0;
    if (max) *max = 0;
    if (def) *def = 0;
    return -1;
}

int a2k_ptz_start(const a2k_ptz_cfg_t *cfg)
{
    (void)cfg;
    return -1;
}

const char *a2k_ptz_state_name(int state)
{
    (void)state; return "off";
}

void a2k_ptz_stop(void)
{
}

int a2k_ptz_tilt_home(void)
{
    return -1;
}

int a2k_ptz_toggle_park(void)
{
    return -1;
}

unsigned long a2k_ptz_travel(void)
{
    return 0;
}

int a2k_ptz_vel(int sx, int sy, int dx, int dy)
{
    (void)sx;
    (void)sy;
    (void)dx;
    (void)dy;
    return -1;
}

int a2k_ptz_vel_manual(int sx, int sy, int dx, int dy, int lease_ms)
{
    (void)sx;
    (void)sy;
    (void)dx;
    (void)dy;
    (void)lease_ms;
    return -1;
}

void a2k_ptz_wear_counts(unsigned *pos, unsigned *scene)
{
    if (pos) *pos = 0;
    if (scene) *scene = 0;
}

int dm_match(const dm_ref_t *ref, const uint8_t *y, unsigned w, unsigned h, unsigned stride, dm_work_t *wk, dm_res_t *r)
{
    (void)ref;
    (void)y;
    (void)w;
    (void)h;
    (void)stride;
    (void)wk;
    (void)r;
    return -1;
}

int dm_ref_build(const uint8_t *y, unsigned w, unsigned h, unsigned stride, dm_ref_t *ref, dm_work_t *wk)
{
    (void)y;
    (void)w;
    (void)h;
    (void)stride;
    (void)ref;
    (void)wk;
    return -1;
}

int dm_static_ok(const dm_res_t *r)
{
    (void)r;
    return 0; /* a boolean: no */
}

const char *dm_verdict_name(int v)
{
    (void)v;
    return "none";
}

int ptzv_parse_args(const char *pan, const char *tilt, const char *lease, ptzv_cmd_t *out, int *lease_ms, const char **why)
{
    (void)pan;
    (void)tilt;
    (void)lease;
    (void)out;
    if (lease_ms) *lease_ms = 0;
    if (why) *why = "";
    return -1;
}
