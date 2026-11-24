/* Host harness for the control API integration test (tests/rot2k_control_api_test.sh): the REAL HTTP server, router, limiter,
 * registry table, settings worker and key store of the 2K app, linked with FAKE hardware modules (LED, IR, ISP, tracker, motor,
 * RTSP counter, day/night). Every fake records what it was asked in DIR/calls.log so the script can assert on it.
 *
 *   rot2k_api_server DIR [--no-clock] [--fast]        run: prints "PORT <n>" then serves on 127.0.0.1 until stdin closes. Lines on
 *                                                    stdin: "event button", "btn S S L" (presses of the front button on a fake line and clock: S short, L long, V very long), "locked 0|1" (the PTZ calibration), "event person 1|0", "det N" (publish a list of N person boxes as the detection thread does; the rate follows detect.publish_hz), "sys" (feed a hal_sys sample), "person 1" (the tracker sees a person and is active: the PTZ patrol pauses, resumes 1.5 s after the last one), "cmd" (a manual PTZ command was accepted: the patrol stops), "quit".
 *   rot2k_api_server DIR key new|list|revoke ...     the on-camera key tool (a2k_api_key_cli) against DIR/api_keys
 * --no-clock: the wall clock reads 1000 (not set: the challenge flow). --fast: a high accepted-request rate (the script sends
 * more than the default burst); without it the spec defaults apply. DIR/net.req receives net.* requests; DIR/net holds hostname. */
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "api_bind.h"
#include "api_events.h"
#include "button_rt.h"
#include "detpub.h"
#include "api_httpd.h"
#include "image.h"
#include "leds.h"
#include "night_ctl.h"
#include "patrol.h"
#include "store.h"
#include "ptz.h"
#include "rtsp_session.h"
#include "audio_out.h"
#include "picture.h"
#include "settings.h"
#include "track_run.h"
#include "video.h"

static char g_dir[200];
static int g_noclock;

void a2k_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

static void call(const char *fmt, ...)
{
    char path[260];
    FILE *f;
    va_list ap;
    snprintf(path, sizeof path, "%s/calls.log", g_dir);
    f = fopen(path, "a");
    if (!f)
        return;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

/* ---- fake modules ---- */
static int g_led_mode = A2K_LEDS_ON, g_ir_auto = 1, g_track_on;
static char g_pat[A2K_RULE_N][A2K_PAT_MAX];
static a2k_image_cfg_t g_last_img;

int a2k_leds_parse_mode(const char *s)
{
    return !s ? -1 : !strcmp(s, "on") ? A2K_LEDS_ON : !strcmp(s, "off") ? A2K_LEDS_OFF : !strcmp(s, "auto") ? A2K_LEDS_AUTO : -1;
}
const char *a2k_leds_mode_name(int m) { return m == A2K_LEDS_ON ? "on" : m == A2K_LEDS_OFF ? "off" : "auto"; }
static const char *const RDEF[A2K_RULE_N] = { "1:100:100:0", "1:250:250:0", "on", "on", "on", "on" };
const char *a2k_rule_default(int r) { return r >= 0 && r < A2K_RULE_N ? RDEF[r] : "on"; }
int a2k_leds_run_mode(void) { return g_led_mode; }
int a2k_leds_run_set_mode(int m)
{
    if (m < 0)
        return -1;
    g_led_mode = m;
    call("led.mode %s", a2k_leds_mode_name(m));
    return 0;
}
void a2k_leds_run_pattern(int r, char *out, size_t cap) { snprintf(out, cap, "%s", g_pat[r][0] ? g_pat[r] : RDEF[r]); }
int a2k_leds_run_set_pattern(int r, const char *t)
{
    snprintf(g_pat[r], sizeof g_pat[r], "%s", t);
    call("led.rule %d %s", r, t);
    return 0;
}
int a2k_night_ir_auto(void) { return g_ir_auto; }
int a2k_night_set_ir(int a)
{
    g_ir_auto = a;
    call("night.ir %d", a);
    return 0;
}
void a2k_night_get(a2k_night_info_t *o)
{
    memset(o, 0, sizeof *o);
    o->valid = 1;
    o->night = 1;
}
void a2k_image_defaults(a2k_image_cfg_t *c)
{
    c->brightness = c->contrast = c->sharpness = -1;
    c->mains_hz = 50;
}
int a2k_image_apply(const a2k_image_cfg_t *c)
{
    g_last_img = *c;
    call("image %d %d %d %d", c->brightness, c->contrast, c->sharpness, c->mains_hz);
    return 0;
}
/* ---- the fake ISP of the picture rows (the REAL picture.c runs on it): every call is logged in calls.log ---- */
static int fx_set(int type, int v)
{
    call("isp.effect %d %d", type, v);
    return 0;
}
static uint8_t g_nr3d[A2K_PIC_NR3D_SIZE];
static int32_t g_wbtype = 1;
static int fk_get(int what, void *buf)
{
    if (what == A2K_IT_NR3D) {
        memcpy(buf, g_nr3d, sizeof g_nr3d);
        return 0;
    }
    if (what == A2K_IT_WBTYPE) {
        memcpy(buf, &g_wbtype, 4);
        return 0;
    }
    return -1;
}
static int fk_set(int what, const void *buf)
{
    if (what == A2K_IT_NR3D) {
        const uint8_t *r1 = (const uint8_t *)buf + 4 + A2K_NR3D_REC, *r16 = (const uint8_t *)buf + 4 + 16 * A2K_NR3D_REC;
        memcpy(g_nr3d, buf, sizeof g_nr3d);
        call("isp.nr3d rec1 %d rec16 %d", r1[4], r16[4]);
        return 0;
    }
    if (what == A2K_IT_WBTYPE) {
        memcpy(&g_wbtype, buf, 4);
        call("isp.wbtype %d", (int)g_wbtype);
        return 0;
    }
    return -1;
}
static const a2k_isptest_ops_t g_fake_isp = { fx_set, NULL, fk_get, fk_set, NULL };
static void fake_isp_init(void)
{
    int i;
    memset(g_nr3d, 0, sizeof g_nr3d);
    g_nr3d[0] = 1;
    for (i = 0; i < A2K_REC_N; i++)
        g_nr3d[4 + i * A2K_NR3D_REC + 4] = (uint8_t)(100 + i); /* a marker per level */
    a2k_pic_init(&g_fake_isp);
}
static int g_isp_flip_rc; /* the fake ISP answer of a2k_isp_flip_mirror (the test script flips it through the file DIR/flip_fail) */
int a2k_isp_flip_mirror(int flip, int mirror)
{
    char path[320];
    snprintf(path, sizeof path, "%s/flip_fail", g_dir);
    g_isp_flip_rc = access(path, F_OK) == 0 ? -1 : 0;
    call("isp.flip_mirror %d %d rc %d", flip, mirror, g_isp_flip_rc);
    return g_isp_flip_rc;
}
static int g_track_dummy;
a2k_track_t *a2k_track_run_get(void) { return (a2k_track_t *)&g_track_dummy; }
int a2k_track_enabled(a2k_track_t *t)
{
    (void)t;
    return g_track_on;
}
static int g_track_home, g_track_delay = 30;
void a2k_track_get_return(a2k_track_t *t, int *home, int *delay_s)
{
    (void)t;
    *home = g_track_home;
    *delay_s = g_track_delay;
}
void a2k_track_set_return(a2k_track_t *t, int home, int delay_s)
{
    (void)t;
    g_track_home = home;
    g_track_delay = delay_s;
    call("track.return %d %d", home, delay_s);
}
static int g_track_band = 8, g_track_head = 15;
void a2k_track_get_tune(a2k_track_t *t, int *band_pct, int *head_pct)
{
    (void)t;
    *band_pct = g_track_band;
    *head_pct = g_track_head;
}
void a2k_track_set_tune(a2k_track_t *t, int band_pct, int head_pct)
{
    (void)t;
    g_track_band = band_pct;
    g_track_head = head_pct;
    call("track.tune %d %d", band_pct, head_pct);
}
static int g_track_gp = 640, g_track_gt = 384, g_track_tb = 10;
void a2k_track_get_gains(a2k_track_t *t, int *gain_pan, int *gain_tilt, int *tilt_band_pct)
{
    (void)t;
    *gain_pan = g_track_gp;
    *gain_tilt = g_track_gt;
    *tilt_band_pct = g_track_tb;
}
void a2k_track_set_gains(a2k_track_t *t, int gain_pan, int gain_tilt, int tilt_band_pct)
{
    (void)t;
    g_track_gp = gain_pan;
    g_track_gt = gain_tilt;
    g_track_tb = tilt_band_pct;
    call("track.gains %d %d %d", gain_pan, gain_tilt, tilt_band_pct);
}
static int g_tback = 5;
void a2k_track_get_tilt_back(a2k_track_t *t, int *s) { (void)t; *s = g_tback; }
void a2k_track_set_tilt_back(a2k_track_t *t, int s) { (void)t; g_tback = s; call("track.tilt_back %d", s); }
static int g_tilt = 0;
int a2k_track_get_tilt(a2k_track_t *t) { (void)t; return g_tilt; }
int a2k_track_set_tilt(a2k_track_t *t, int on) { (void)t; g_tilt = on; call("track.tilt %d", on); return 0; }
static int g_cscan[3] = { 1, 250, 300 };
int a2k_track_get_close_scan(a2k_track_t *t, int which) { (void)t; return g_cscan[which]; }
int a2k_track_set_close_scan(a2k_track_t *t, int which, int v) { (void)t; g_cscan[which] = v; call("track.close_scan %d %d", which, v); return 0; }
static int g_lag = 50;
int a2k_track_get_lag(a2k_track_t *t) { (void)t; return g_lag; }
void a2k_track_set_lag(a2k_track_t *t, int v) { (void)t; g_lag = v; call("track.lag %d", v); }
static char g_lost[160] = "backsweep,exit,hold:30,home";
int a2k_track_set_lost_policy(a2k_track_t *t, const char *text)
{
    (void)t;
    if (strlen(text) >= sizeof g_lost) return -1;
    snprintf(g_lost, sizeof g_lost, "%s", text);
    call("track.lost %s", text);
    return 0;
}
void a2k_track_get_lost_policy(a2k_track_t *t, char *out, size_t cap)
{
    (void)t;
    snprintf(out, cap, "%s", g_lost);
}
void a2k_track_get_lost_step(a2k_track_t *t, char *out, size_t cap)
{
    (void)t;
    snprintf(out, cap, "idle");
}
static int g_zx = 40, g_zy = 30, g_zh = 20, g_motion = 1;
void a2k_track_get_zones(a2k_track_t *t, int *x_pct, int *y_pct, int *head_pct)
{
    (void)t;
    *x_pct = g_zx;
    *y_pct = g_zy;
    *head_pct = g_zh;
}
void a2k_track_set_zones(a2k_track_t *t, int x_pct, int y_pct, int head_pct)
{
    (void)t;
    g_zx = x_pct;
    g_zy = y_pct;
    g_zh = head_pct;
    call("track.zones %d %d %d", x_pct, y_pct, head_pct);
}
int a2k_track_get_motion(a2k_track_t *t)
{
    (void)t;
    return g_motion;
}
void a2k_track_set_motion(a2k_track_t *t, int motion)
{
    (void)t;
    g_motion = motion;
    call("track.motion %d", motion);
}
static int g_track_tret = 10;
void a2k_track_get_tilt_return(a2k_track_t *t, int *s)
{
    (void)t;
    *s = g_track_tret;
}
void a2k_track_set_tilt_return(a2k_track_t *t, int s)
{
    (void)t;
    g_track_tret = s;
    call("track.tilt_return %d", s);
}
void a2k_track_set_debug(a2k_track_t *t, int on)
{
    (void)t;
    (void)on;
}
void a2k_track_set_enabled(a2k_track_t *t, int on)
{
    (void)t;
    g_track_on = on;
    call("track %d", on);
}
int a2k_ptz_command(int cmd)
{
    call("ptz.cmd %d", cmd);
    a2k_patrol_note_cmd(cmd); /* what the controller does when it accepts a manual command */
    return 0;
}
static int g_vel_rc; /* what the fake controller answers to a manual velocity command (stdin: "velrc N") */
int a2k_ptz_vel_manual(int sx, int sy, int dx, int dy, int lease_ms)
{
    call("ptz.vel %d %d %d %d %d", sx, sy, dx, dy, lease_ms);
    if (g_vel_rc == 0)
        a2k_patrol_note_cmd(A2K_PTZ_HOOK_NUDGE);
    return g_vel_rc;
}
int a2k_ptz_manual_stop(void)
{
    call("ptz.stop");
    return 0;
}
unsigned long a2k_ptz_travel(void) { return 1234; }
void a2k_ptz_drift_info(a2k_ptz_drift_t *o) { memset(o, 0, sizeof *o); o->have = 1; o->dx = -2; o->dy = 3; o->corrections = 2; snprintf(o->recal_reason, sizeof o->recal_reason, "none"); }
/* real, non-zero values so that the HTTP test proves the REAL settings code (settings.c) serves the re-home / drift-reference keys, not that the rows merely exist */
void a2k_ptz_rehome_last(unsigned long *last_s, const char **reason) { *last_s = 4242; *reason = "flat"; }
void a2k_ptz_rehome_info(unsigned *runs, unsigned *cancels) { if (runs) *runs = 3; if (cancels) *cancels = 2; }
int a2k_ptz_drift_ref_info(unsigned long *age_s) { if (age_s) *age_s = 12; return 3; }
void a2k_ptz_wear_counts(unsigned *pos, unsigned *scene) { *pos = 7; *scene = 3; }
int a2k_ptz_nudge(int dx, int dy)
{
    call("ptz.nudge %d %d", dx, dy);
    a2k_patrol_note_cmd(A2K_PTZ_HOOK_NUDGE);
    return 0;
}

/* ---- the PTZ patrol's fake motor: a position and a move that takes a little time (40 ms + distance x speed / 100) ---- */
static pthread_mutex_t g_pm = PTHREAD_MUTEX_INITIALIZER;
static int g_ppan = 1275, g_ptilt = 1300, g_dpan, g_dtilt, g_pmoving, g_pdone_rc;
static int64_t g_pdeadline;
static unsigned g_pticket, g_pdone;

static int64_t pnow(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
static void pfinish_locked(void)
{
    if (g_pmoving && pnow() >= g_pdeadline) {
        g_ppan = g_dpan;
        g_ptilt = g_dtilt;
        g_pmoving = 0;
        g_pdone = g_pticket;
        g_pdone_rc = 0;
    }
}
static void p_info(a2k_ptz_info_t *i)
{
    pthread_mutex_lock(&g_pm);
    pfinish_locked();
    memset(i, 0, sizeof *i);
    i->state = g_pmoving ? HAL_PTZ_MOVED : HAL_PTZ_HOME;
    i->known = !g_pmoving;
    i->pan = (uint32_t)g_ppan;
    i->tilt = (uint32_t)g_ptilt;
    pthread_mutex_unlock(&g_pm);
}
static int p_limits(pp_limits_t *l)
{
    pp_limits_t v = { 300, 3650, 300, 2100, 3, 20, 3, 1275, 300, 15 };
    *l = v;
    return 0;
}
static int p_move(int pan, int tilt, int speed, unsigned *ticket)
{
    int rc = 0;
    pthread_mutex_lock(&g_pm);
    pfinish_locked();
    if (ticket) { /* the patrol's own move: only while idle */
        if (g_pmoving) {
            rc = 1;
        } else {
            int d = abs(pan - g_ppan) + abs(tilt - g_ptilt);
            g_dpan = pan;
            g_dtilt = tilt;
            g_pmoving = 1;
            g_pdeadline = pnow() + 40 + (int64_t)d * (speed ? speed : 3) / 100;
            *ticket = ++g_pticket;
            call("patrol.move %d %d %d", pan, tilt, speed);
        }
    } else { /* a manual goto: lands at once, tells the patrol hook like the controller */
        g_pmoving = 0;
        g_ppan = pan;
        g_ptilt = tilt;
        call("ptz.goto %d %d", pan, tilt);
        a2k_patrol_note_cmd(A2K_PTZ_HOOK_GOTO);
    }
    pthread_mutex_unlock(&g_pm);
    return rc;
}
static int p_result(unsigned t, int *rc)
{
    int done = 1;
    pthread_mutex_lock(&g_pm);
    pfinish_locked();
    if (g_pmoving && t == g_pticket)
        done = 0;
    else
        *rc = g_pdone == t ? g_pdone_rc : 7;
    pthread_mutex_unlock(&g_pm);
    return done;
}
static void p_cancel(void)
{
    pthread_mutex_lock(&g_pm);
    if (g_pmoving) {
        g_pmoving = 0;
        g_pdone = g_pticket;
        g_pdone_rc = 7;
        call("patrol.cancel");
    }
    pthread_mutex_unlock(&g_pm);
}
static int p_privacy(void) { return 0; }
static char g_pcfg[260];
static int p_load(char *buf, size_t cap) { return a2k_store_read(g_pcfg, buf, cap); }
static int p_save(const char *text, size_t len)
{
    unsigned before = a2k_store_writes();
    if (a2k_store_write(g_pcfg, text, len) != 0)
        return -1;
    return a2k_store_writes() != before;
}
void a2k_ptz_info(a2k_ptz_info_t *i)
{
    memset(i, 0, sizeof *i);
    i->state = 1;
    i->known = 1;
    i->pan = 1275;
    i->tilt = 300;
}
const char *a2k_ptz_state_name(int s) { return s == 1 ? "parked" : "unknown"; }
int a2k_rtsp_clients(void) { return 2; }

/* ---- the programmable button: the REAL runtime (button_rt.c, policy, config, HTTP) over fake actions; the mode apply goes through the real registry ---- */
static const hal_input_t g_btn_in = { "button", 71, 1, 50 };
static int64_t g_vt = 100000;
static int g_btn_locked;
static int h_run(void *u, int kind, const char *arg)
{
    (void)u;
    call("btn.run %d %s", kind, arg);
    return 0;
}
static int h_apply(void *u, int n, const char *const *k, const char *const *v)
{
    (void)u;
    return a2k_settings_apply((size_t)n, k, v, NULL, 0);
}
static int h_locked(void *u) { (void)u; return g_btn_locked; }
static void h_led(void *u, int cue) { (void)u; call("btn.cue %d", cue); }
static void h_event(void *u, const char *json) { (void)u; a2k_api_event("button", json); }
static void h_mqtt(void *u, const char *g, const char *json) { (void)u; call("btn.mqtt %s %s", g, json); }
static void h_changed(void *u) { (void)u; call("btn.changed"); }
static int64_t h_now(void *u) { (void)u; return g_vt; }
static const a2k_btn_ops_t g_bops = { 0, h_run, h_apply, h_locked, h_led, h_event, h_mqtt, h_changed, a2k_settings_mode_key_ok, h_now };

/* the line low for `ms` (or high), on the virtual clock: no sleeping */
static void sim_raw(int raw, int ms)
{
    int64_t end = g_vt + ms;
    for (; g_vt < end; g_vt += 20)
        (void)a2k_btn_step(&g_btn_in, g_vt, raw);
}

/* "btn S S L": S = 200 ms, L = 2.5 s, V = 6 s, 150 ms between presses, then 1.5 s of released line (every pending gesture is reported) */
static void sim_gesture(const char *seq)
{
    sim_raw(1, 200);
    for (; *seq; seq++) {
        int hold = *seq == 'S' ? 200 : *seq == 'L' ? 2500 : *seq == 'V' ? 6000 : 0;
        if (!hold)
            continue;
        sim_raw(0, hold);
        sim_raw(1, 150);
    }
    sim_raw(1, 1500);
}

/* ---- the harness ---- */
static int64_t wall_s(void) { return g_noclock ? 1000 : (int64_t)time(NULL); }

static int cmd_key(int argc, char **argv)
{
    a2k_api_env_t env;
    char keys[260];
    a2k_api_env_2k(&env);
    snprintf(keys, sizeof keys, "%s/api_keys", g_dir);
    env.keys_path = keys;
    return a2k_api_key_cli(&env, argc, argv);
}

int main(int argc, char **argv)
{
    a2k_api_env_t env;
    a2k_settings_cfg_t sc;
    a2k_image_cfg_t img;
    a2k_vid_t base[A2K_VID_STREAMS];
    char keys[260], audit[260], setf[260], netdir[260], netreq[260], cfgdir[260], cfgreq[260], vidf[260], line[128], bpath[260], mpath[260];
    int i, fast = 0;
    if (argc < 2)
        return 2;
    snprintf(g_dir, sizeof g_dir, "%s", argv[1]);
    if (argc >= 3 && !strcmp(argv[2], "key"))
        return cmd_key(argc - 3, argv + 3);
    for (i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--no-clock")) g_noclock = 1;
        else if (!strcmp(argv[i], "--fast")) fast = 1;
    }
    snprintf(keys, sizeof keys, "%s/api_keys", g_dir);
    snprintf(audit, sizeof audit, "%s/audit", g_dir);
    snprintf(setf, sizeof setf, "%s/settings", g_dir);
    snprintf(netdir, sizeof netdir, "%s/net", g_dir);
    snprintf(netreq, sizeof netreq, "%s/net.req", g_dir);
    snprintf(cfgdir, sizeof cfgdir, "%s/cfg", g_dir);
    snprintf(cfgreq, sizeof cfgreq, "%s/cfg.req", g_dir);
    snprintf(vidf, sizeof vidf, "%s/video", g_dir);
    a2k_api_env_2k(&env);
    env.keys_path = keys;
    env.audit_path = audit;
    if (g_noclock)
        env.wall_s = wall_s;
    if (fast) {
        env.rate_milli = 50000;
        env.burst = 60;
    }
    a2k_vid_defaults(0, 2000, 30, &base[0]);
    a2k_vid_defaults(1, 512, 30, &base[1]);
    a2k_vid_init(base, vidf);
    a2k_image_defaults(&img);
    memset(&sc, 0, sizeof sc);
    sc.settings_path = setf;
    sc.net_dir = netdir;
    sc.net_req = netreq;
    sc.cfg_dir = cfgdir;
    sc.cfg_req = cfgreq;
    sc.image = &img;
    snprintf(g_pcfg, sizeof g_pcfg, "%s/ptz_patrol", g_dir);
    {
        static const a2k_patrol_ops_t pops = { p_info, p_limits, p_move, p_result, p_cancel, p_privacy, a2k_api_event, p_load, p_save, pnow, NULL, NULL };
        a2k_patrol_init(&pops);
        a2k_patrol_test_set_min_idle(1); /* the real minimum is 30 s */
        if (a2k_patrol_thread_start() != 0)
            return 1;
    }
    snprintf(bpath, sizeof bpath, "%s/button", g_dir);
    snprintf(mpath, sizeof mpath, "%s/modes", g_dir);
    a2k_btn_set_paths(bpath, mpath);
    a2k_btn_init(&g_bops, 2000);
    /* the speaker rows run on the REAL shared engine (no playback node on a host: enabling it fails cleanly), its files in this directory */
    audio_out_setup("/nonexistent/pcm_playback", g_dir, 0);
    audio_out_set_persist(0);
    fake_isp_init();
    if (a2k_settings_start(&sc) != 0)
        return 1;
    a2k_settings_load();
    if (a2k_httpd_start(0, 1, &env) != 0)
        return 1;
    for (i = 0; i < 50 && a2k_httpd_port() == 0 && a2k_api_keys_count() > 0; i++)
        usleep(100000);
    printf("PORT %d\n", a2k_httpd_port());
    fflush(stdout);
    while (fgets(line, sizeof line, stdin)) {
        int v;
        if (!strncmp(line, "event button", 12)) {
            a2k_api_event_button();
        } else if (!strncmp(line, "btn ", 4)) {
            sim_gesture(line + 4); /* presses of the front button on the fake line: S short, L long, V very long */
        } else if (sscanf(line, "velrc %d", &v) == 1) {
            g_vel_rc = v;
        } else if (sscanf(line, "locked %d", &v) == 1) {
            g_btn_locked = v; /* the PTZ calibration */
        } else if (sscanf(line, "event person %d", &v) == 1) {
            a2k_api_event_person(v, 800);
        } else if (sscanf(line, "det %d", &v) == 1) {
            static a2k_dp_t dp;
            static int64_t now = 100000;
            static int inited;
            struct a2k_dp_in b[2] = { { 100, 20, 140, 120, 900, 0, 1 }, { 200, 30, 230, 100, 800, 1, 0 } };
            char js[A2K_DP_JSON];
            if (!inited) {
                a2k_dp_init(&dp);
                inited = 1;
            }
            now += 1000;
            if (a2k_dp_update(&dp, now, 1700000000000LL, a2k_dp_hz(), 0, b, v, 1, 1234, 56, js, sizeof js) > 0) {
                a2k_dp_set_latest(js);
                a2k_api_event(A2K_EVT_DETECTIONS, js);
            }
        } else if (sscanf(line, "person %d", &v) == 1) {
            if (v)
                a2k_patrol_note_person(1, 1500); /* tracking is active: the patrol pauses; it resumes 1.5 s after the last one */
        } else if (!strncmp(line, "cmd", 3)) {
            a2k_patrol_note_cmd(A2K_PTZ_LEFT);
        } else if (!strncmp(line, "sys", 3)) {
            hal_sys_sample_t s = { 17, 123, 13000, 9000, 4242, -55, 70, HAL_SYS_NA };
            a2k_settings_note_sys(&s);
        } else if (!strncmp(line, "keys?", 5)) {
            printf("KEYS %d %s\n", a2k_api_keys_count(), a2k_api_keys_error() ? a2k_api_keys_error() : "ok");
            fflush(stdout);
        } else if (!strncmp(line, "port?", 5)) {
            printf("PORT %d\n", a2k_httpd_port());
            fflush(stdout);
        } else if (!strncmp(line, "quit", 4)) {
            break;
        }
    }
    a2k_httpd_stop();
    a2k_settings_stop();
    a2k_patrol_stop();
    return 0;
}
