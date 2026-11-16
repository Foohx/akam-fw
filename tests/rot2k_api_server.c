/* Host harness for the control API integration test (tests/rot2k_control_api_test.sh): the REAL HTTP server, router, limiter,
 * registry table, settings worker and key store of the 2K app, linked with FAKE hardware modules (LED, IR, ISP, tracker, motor,
 * RTSP counter, day/night). Every fake records what it was asked in DIR/calls.log so the script can assert on it.
 *
 *   rot2k_api_server DIR [--no-clock] [--fast]        run: prints "PORT <n>" then serves on 127.0.0.1 until stdin closes. Lines on
 *                                                    stdin: "event button", "event person 1|0", "sys" (feed a hal_sys sample), "quit".
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

#include "akam2k_api.h"
#include "akam2k_apievt.h"
#include "akam2k_httpd.h"
#include "akam2k_image.h"
#include "akam2k_leds.h"
#include "akam2k_night.h"
#include "akam2k_ptz.h"
#include "akam2k_rtsp.h"
#include "akam2k_settings.h"
#include "akam2k_track_run.h"
#include "akam2k_video.h"

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
void a2k_track_set_enabled(a2k_track_t *t, int on)
{
    (void)t;
    g_track_on = on;
    call("track %d", on);
}
int a2k_ptz_command(int cmd)
{
    call("ptz.cmd %d", cmd);
    return 0;
}
unsigned long a2k_ptz_travel(void) { return 1234; }
void a2k_ptz_wear_counts(unsigned *pos, unsigned *scene) { *pos = 7; *scene = 3; }
int a2k_ptz_nudge(int dx, int dy)
{
    call("ptz.nudge %d %d", dx, dy);
    return 0;
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

/* ---- the harness ---- */
static int64_t wall_s(void) { return g_noclock ? 1000 : (int64_t)time(NULL); }

static int cmd_key(int argc, char **argv)
{
    a2k_api_env_t env;
    char keys[260];
    memset(&env, 0, sizeof env);
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
    char keys[260], audit[260], setf[260], netdir[260], netreq[260], vidf[260], line[128];
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
    snprintf(vidf, sizeof vidf, "%s/video", g_dir);
    memset(&env, 0, sizeof env);
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
    sc.image = &img;
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
        } else if (sscanf(line, "event person %d", &v) == 1) {
            a2k_api_event_person(v, 800);
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
    return 0;
}
