/* See akam2k_settings.h. */
#include "akam2k_settings.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "akam2k_apievt.h"
#include "akam2k_leds.h"
#include "akam2k_log.h"
#include "akam2k_netreg.h"
#include "akam2k_night.h"
#include "akam2k_ptz.h"
#include "akam2k_rtsp.h"
#include "akam2k_store.h"
#include "akam2k_track_run.h"
#include "akam2k_video.h"
#include "api_keys.h"

#define RW (ST_ACC_READ | ST_ACC_WRITE)
#define RO ST_ACC_READ
#define HM (ST_EXPOSE_HTTP | ST_EXPOSE_MQTT)
#define MODEL_2K 1u
#define MAX_ROWS 64

/* ------------------------------------------------------------------ module state */
static streg_t g_reg;
static st_def_t g_defs[MAX_ROWS];
static size_t g_nrows;
static a2k_settings_cfg_t g_cfg;
static char g_path[160], g_netdir[120], g_netreq[120];
static a2k_image_cfg_t g_img;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER; /* guards g_img, g_sys, g_keys */
static hal_sys_sample_t g_sys;
static int g_have_sys, g_keys;

/* ------------------------------------------------------------------ helpers */
static int put(char *out, size_t cap, long v)
{
    int n = snprintf(out, cap, "%ld", v);
    return n < 0 || (size_t)n >= cap ? ST_E_GETTER : n;
}

static int puts_(char *out, size_t cap, const char *s)
{
    int n = snprintf(out, cap, "%s", s);
    return n < 0 || (size_t)n >= cap ? ST_E_GETTER : n;
}

static int num_after(const char *key, const char *prefix)
{
    size_t n = strlen(prefix);
    return strncmp(key, prefix, n) == 0 ? atoi(key + n) : -1;
}

/* ------------------------------------------------------------------ led.* and night.ir_led */
static const char *const LED_RULE[A2K_RULE_N] = { "boot", "nowifi", "person", "stream", "night", "idle" };

static int led_rule_of(const char *key)
{
    int r;
    for (r = 0; r < A2K_RULE_N; r++)
        if (strcmp(key + 4, LED_RULE[r]) == 0)
            return r;
    return -1;
}

static int led_get(const char *key, char *out, size_t cap)
{
    int r;
    if (strcmp(key, "led.mode") == 0)
        return puts_(out, cap, a2k_leds_mode_name(a2k_leds_run_mode()));
    if (strcmp(key, "night.ir_led") == 0)
        return puts_(out, cap, a2k_night_ir_auto() ? "auto" : "off");
    r = led_rule_of(key);
    if (r < 0)
        return ST_E_GETTER;
    a2k_leds_run_pattern(r, out, cap);
    return (int)strlen(out);
}

static int led_set(const char *key, const char *val, st_apply_t *eff)
{
    int r;
    (void)eff;
    if (strcmp(key, "led.mode") == 0)
        return a2k_leds_run_set_mode(a2k_leds_parse_mode(val)) == 0 ? 0 : ST_E_SETTER;
    if (strcmp(key, "night.ir_led") == 0)
        return a2k_night_set_ir(strcmp(val, "auto") == 0) == 0 ? 0 : ST_E_SETTER;
    r = led_rule_of(key);
    return r >= 0 && a2k_leds_run_set_pattern(r, val) == 0 ? 0 : ST_E_SETTER;
}

/* ------------------------------------------------------------------ image.* (RAM only: the ISP keeps no setting) */
static int img_field(const char *key, int **f)
{
    if (strcmp(key, "image.brightness") == 0)
        *f = &g_img.brightness;
    else if (strcmp(key, "image.contrast") == 0)
        *f = &g_img.contrast;
    else if (strcmp(key, "image.sharpness") == 0)
        *f = &g_img.sharpness;
    else if (strcmp(key, "image.mains_hz") == 0)
        *f = &g_img.mains_hz;
    else
        return -1;
    return 0;
}

static int img_get(const char *key, char *out, size_t cap)
{
    int *f, v;
    if (img_field(key, &f) != 0)
        return ST_E_GETTER;
    pthread_mutex_lock(&g_mu);
    v = *f;
    pthread_mutex_unlock(&g_mu);
    return put(out, cap, v < 0 ? 50 : v); /* "not applied" reads as the neutral 50 */
}

static int img_set(const char *key, const char *val, st_apply_t *eff)
{
    int *f, old, bad;
    a2k_image_cfg_t c;
    (void)eff;
    if (img_field(key, &f) != 0)
        return ST_E_SETTER;
    pthread_mutex_lock(&g_mu);
    old = *f;
    *f = atoi(val);
    c = g_img;
    pthread_mutex_unlock(&g_mu);
    bad = a2k_image_apply(&c); /* ISP calls: on the worker thread, never the HTTP thread */
    if (bad > 0) {
        pthread_mutex_lock(&g_mu);
        *f = old;
        pthread_mutex_unlock(&g_mu);
        return ST_E_SETTER;
    }
    return 0;
}

/* ------------------------------------------------------------------ track.enabled */
static int track_get(const char *key, char *out, size_t cap)
{
    a2k_track_t *t = a2k_track_run_get();
    (void)key;
    return puts_(out, cap, t && a2k_track_enabled(t) ? "1" : "0");
}

static int track_set(const char *key, const char *val, st_apply_t *eff)
{
    a2k_track_t *t = a2k_track_run_get();
    (void)key;
    (void)eff;
    if (!t)
        return ST_E_SETTER;
    a2k_track_set_enabled(t, strcmp(val, "1") == 0);
    a2k_log("track: person tracking %s (API)", strcmp(val, "1") == 0 ? "on" : "off");
    return 0;
}

/* ------------------------------------------------------------------ video<N>.* */
static const char *const VP_NAME[A2K_VP_N] = { "kbps", "max_kbps", "fps", "min_qp", "max_qp", "gop", "rc" };
static const char *const OPT_RC[] = { "vbr", "cbr", "avbr", NULL };
static const char *const OPT_AUTO[] = { "auto", "on", "off", NULL };
static const char *const OPT_IR[] = { "auto", "off", NULL };
static const char *const OPT_MAINS[] = { "50", "60", NULL };
static const char *const G_LED[] = { "led", NULL };
static char g_vkey[A2K_VID_STREAMS][A2K_VP_N][20], g_vdef[A2K_VID_STREAMS][A2K_VP_N][12];

static int vid_of(const char *key, int *chn, int *p)
{
    int c = num_after(key, "video"), i;
    const char *dot = strchr(key, '.');
    if (c < 0 || c >= A2K_VID_STREAMS || !dot)
        return -1;
    for (i = 0; i < A2K_VP_N; i++)
        if (strcmp(dot + 1, VP_NAME[i]) == 0) {
            *chn = c;
            *p = i;
            return 0;
        }
    return -1;
}

static int vid_get(const char *key, char *out, size_t cap)
{
    int c, p;
    if (vid_of(key, &c, &p) != 0)
        return ST_E_GETTER;
    return a2k_vid_get_text(c, p, out, cap) < 0 ? ST_E_GETTER : (int)strlen(out);
}

static int vid_set(const char *key, const char *val, st_apply_t *eff)
{
    int c, p;
    (void)eff; /* the capture thread applies it live or re-opens that one channel; the video module persists /config/akam/video */
    if (vid_of(key, &c, &p) != 0)
        return ST_E_SETTER;
    return a2k_vid_set(c, p, val) == 0 ? 0 : ST_E_SETTER;
}

/* a2k_vid_valid() as a registry rule: min QP <= max QP, and the peak (0 = 1.25 x the target) not below the target. Reads the values of
 * the same PATCH through streg_peek, so the whole batch is judged before any setter runs. */
static long peek_num(const struct streg *reg, int chn, const char *name, long dflt)
{
    char k[24], v[STREG_VAL_MAX];
    snprintf(k, sizeof k, "video%d.%s", chn, name);
    return streg_peek(reg, k, v, sizeof v) > 0 ? atol(v) : dflt;
}

static int vid_check(const struct streg *reg, const char *key, const char *val)
{
    int c, p;
    long v = atol(val);
    if (vid_of(key, &c, &p) != 0)
        return 0;
    if (p == A2K_VP_MINQP || p == A2K_VP_MAXQP) {
        long other = peek_num(reg, c, p == A2K_VP_MINQP ? "max_qp" : "min_qp", p == A2K_VP_MINQP ? 51 : 0);
        return (p == A2K_VP_MINQP ? v <= other : other <= v) ? 0 : ST_E_CHECK;
    }
    if (p == A2K_VP_KBPS || p == A2K_VP_MAXKBPS) {
        long kbps = p == A2K_VP_KBPS ? v : peek_num(reg, c, "kbps", 0), max = p == A2K_VP_MAXKBPS ? v : peek_num(reg, c, "max_kbps", 0);
        long eff = max ? max : kbps + kbps / 4;
        return eff >= kbps ? 0 : ST_E_CHECK;
    }
    return 0;
}

/* ------------------------------------------------------------------ net.* backend (request file, see the header) */
static int net_read_value(const char *file, const char *name, char *out, size_t cap)
{
    char path[200], buf[A2K_STORE_MAX + 1], *line, *save = NULL;
    int n;
    snprintf(path, sizeof path, "%s/%s", g_netdir, file);
    n = a2k_store_read(path, buf, sizeof buf);
    if (n < 0)
        return -1;
    if (!name) { /* the whole first line */
        buf[strcspn(buf, "\r\n")] = '\0';
        return buf[0] ? puts_(out, cap, buf) : -1;
    }
    for (line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        size_t l = strlen(name);
        if (strncmp(line, name, l) == 0 && line[l] == '=')
            return puts_(out, cap, line + l + 1);
    }
    return -1;
}

static int netb_get(const char *key, char *out, size_t cap)
{
    if (strcmp(key, "net.hostname") == 0)
        return net_read_value("hostname", NULL, out, cap) < 0 ? puts_(out, cap, "akam-rot2k") : (int)strlen(out);
    if (strcmp(key, "net.wifi_country") == 0)
        return net_read_value("net", "wifi_country", out, cap) < 0 ? puts_(out, cap, "FR") : (int)strlen(out);
    if (strcmp(key, "net.wifi_band") == 0)
        return net_read_value("net", "wifi_band", out, cap) < 0 ? puts_(out, cap, "auto") : (int)strlen(out);
    return -1;
}

static int netb_set(const char *key, const char *val)
{
    char line[96], old[8];
    int n;
    n = a2k_store_read(g_netreq, old, sizeof old);
    if (n >= 0 || n == -2)
        return -1; /* the previous request has not been taken yet (even a longer one): one at a time */
    if (strcmp(key, "net.hostname") == 0)
        n = snprintf(line, sizeof line, "hostname set %s\n", val);
    else if (strcmp(key, "net.wifi_country") == 0)
        n = snprintf(line, sizeof line, "wifi set country %s\n", val);
    else if (strcmp(key, "net.wifi_band") == 0)
        n = snprintf(line, sizeof line, "wifi set band %s\n", val);
    else
        return -1;
    if (n < 0 || (size_t)n >= sizeof line)
        return -1;
    a2k_log("net: request queued: %.*s", n - 1, line);
    return a2k_store_write(g_netreq, line, (size_t)n);
}

static const a2k_netreg_backend_t g_netb = { netb_get, netb_set };

/* ------------------------------------------------------------------ state rows (read only) */
static int st_get(const char *key, char *out, size_t cap)
{
    hal_sys_sample_t s;
    int have, keys;
    pthread_mutex_lock(&g_mu);
    s = g_sys;
    have = g_have_sys;
    keys = g_keys;
    pthread_mutex_unlock(&g_mu);
    if (strcmp(key, "sys.cpu_pct") == 0) return put(out, cap, have ? s.cpu_pct : HAL_SYS_NA);
    if (strcmp(key, "sys.load1_x100") == 0) return put(out, cap, have ? s.load1_x100 : HAL_SYS_NA);
    if (strcmp(key, "sys.mem_total_kb") == 0) return put(out, cap, have ? s.mem_total_kb : HAL_SYS_NA);
    if (strcmp(key, "sys.mem_avail_kb") == 0) return put(out, cap, have ? s.mem_avail_kb : HAL_SYS_NA);
    if (strcmp(key, "sys.uptime_s") == 0) return put(out, cap, have ? s.uptime_s : HAL_SYS_NA);
    if (strcmp(key, "net.wifi_rssi") == 0) return put(out, cap, have ? s.rssi_dbm : HAL_SYS_NA);
    if (strcmp(key, "net.wifi_lq") == 0) return put(out, cap, have ? s.link_quality : HAL_SYS_NA);
    if (strcmp(key, "api.keys") == 0) return put(out, cap, keys);
    if (strcmp(key, "rtsp.clients") == 0) return put(out, cap, a2k_rtsp_clients());
    if (strcmp(key, "detect.person") == 0) return put(out, cap, a2k_apievt_person());
    if (strcmp(key, "night.active") == 0) {
        a2k_night_info_t i;
        a2k_night_get(&i);
        return put(out, cap, i.valid && i.night);
    }
    if (strncmp(key, "ptz.", 4) == 0) {
        a2k_ptz_info_t i;
        a2k_ptz_info(&i);
        if (strcmp(key, "ptz.state") == 0) return puts_(out, cap, a2k_ptz_state_name(i.state));
        if (strcmp(key, "ptz.pan") == 0) return put(out, cap, i.known ? (long)i.pan : -1);
        if (strcmp(key, "ptz.tilt") == 0) return put(out, cap, i.known ? (long)i.tilt : -1);
    }
    return ST_E_GETTER;
}

/* ------------------------------------------------------------------ the table */
static void row(const char *key, st_type_t type, long lo, long hi, const char *const *opts, const char *def, uint8_t persist,
                uint8_t access, int (*get)(const char *, char *, size_t),
                int (*set)(const char *, const char *, st_apply_t *))
{
    st_def_t *d;
    if (g_nrows >= MAX_ROWS)
        return;
    d = &g_defs[g_nrows++];
    memset(d, 0, sizeof *d);
    d->key = key;
    d->type = type;
    d->lo = lo;
    d->hi = hi;
    d->opts = opts;
    d->def = def;
    d->apply = AP_LIVE;
    d->persist = persist;
    d->access = access;
    d->expose = HM;
    d->get = get;
    d->set = set;
}

static void build_table(void)
{
    static const char *const LED_KEY[A2K_RULE_N] = { "led.boot", "led.nowifi", "led.person", "led.stream", "led.night", "led.idle" };
    int c, p, r;
    size_t i;
    g_nrows = 0;
    for (i = 0; i < A2K_NETREG_N; i++)
        if (g_nrows < MAX_ROWS)
            g_defs[g_nrows++] = a2k_netreg_defs[i];
    row("led.mode", ST_ENUM, 0, 0, OPT_AUTO, "on", 1, RW, led_get, led_set);
    for (r = 0; r < A2K_RULE_N; r++)
        row(LED_KEY[r], ST_PATTERN, 0, 0, G_LED, a2k_rule_default(r), 1, RW, led_get, led_set);
    row("night.ir_led", ST_ENUM, 0, 0, OPT_IR, "auto", 1, RW, led_get, led_set);
    row("image.brightness", ST_INT, 0, 100, NULL, "50", 0, RW, img_get, img_set);
    row("image.contrast", ST_INT, 0, 100, NULL, "50", 0, RW, img_get, img_set);
    row("image.sharpness", ST_INT, 0, 100, NULL, "50", 0, RW, img_get, img_set);
    row("image.mains_hz", ST_ENUM, 0, 0, OPT_MAINS, "50", 0, RW, img_get, img_set);
    row("track.enabled", ST_BOOL, 0, 0, NULL, "0", 1, RW, track_get, track_set);
    for (c = 0; c < A2K_VID_STREAMS; c++) {
        a2k_vid_t cur;
        a2k_vid_get(c, &cur);
        for (p = 0; p < A2K_VP_N; p++) {
            int lo = 0, hi = 0;
            a2k_vid_range(c, p, &lo, &hi);
            snprintf(g_vkey[c][p], sizeof g_vkey[c][p], "video%d.%s", c, VP_NAME[p]);
            a2k_vid_text(p, cur.v[p], g_vdef[c][p], sizeof g_vdef[c][p]);
            if (p == A2K_VP_RC)
                row(g_vkey[c][p], ST_ENUM, 0, 0, OPT_RC, g_vdef[c][p], 0, RW, vid_get, vid_set);
            else /* video<N>.max_kbps also takes 0 = automatic (1.25 x the target): the setter re-validates */
                row(g_vkey[c][p], ST_INT, p == A2K_VP_MAXKBPS ? 0 : lo, hi, NULL, g_vdef[c][p], 0, RW, vid_get, vid_set);
            g_defs[g_nrows - 1].check = vid_check;
        }
    }
    /* read-only state: a getter, no setter, never persisted */
    row("sys.cpu_pct", ST_INT, HAL_SYS_NA, 100, NULL, "-1000000", 0, RO, st_get, NULL);
    row("sys.load1_x100", ST_INT, HAL_SYS_NA, 100000, NULL, "-1000000", 0, RO, st_get, NULL);
    row("sys.mem_total_kb", ST_INT, HAL_SYS_NA, 100000000, NULL, "-1000000", 0, RO, st_get, NULL);
    row("sys.mem_avail_kb", ST_INT, HAL_SYS_NA, 100000000, NULL, "-1000000", 0, RO, st_get, NULL);
    row("sys.uptime_s", ST_INT, HAL_SYS_NA, 2000000000, NULL, "-1000000", 0, RO, st_get, NULL);
    row("net.wifi_rssi", ST_INT, HAL_SYS_NA, 0, NULL, "-1000000", 0, RO, st_get, NULL);
    row("net.wifi_lq", ST_INT, HAL_SYS_NA, 1000, NULL, "-1000000", 0, RO, st_get, NULL);
    row("rtsp.clients", ST_INT, 0, 16, NULL, "0", 0, RO, st_get, NULL);
    row("detect.person", ST_BOOL, 0, 0, NULL, "0", 0, RO, st_get, NULL);
    row("night.active", ST_BOOL, 0, 0, NULL, "0", 0, RO, st_get, NULL);
    row("ptz.state", ST_STRING, 1, 16, NULL, "unknown", 0, RO, st_get, NULL);
    row("ptz.pan", ST_INT, -1, 100000, NULL, "-1", 0, RO, st_get, NULL);
    row("ptz.tilt", ST_INT, -1, 100000, NULL, "-1", 0, RO, st_get, NULL);
    row("api.keys", ST_INT, 0, AKEY_MAX, NULL, "0", 0, RO, st_get, NULL);
}

const streg_t *a2k_settings_reg(void) { return &g_reg; }

void a2k_settings_note_sys(const hal_sys_sample_t *s)
{
    pthread_mutex_lock(&g_mu);
    g_sys = *s;
    g_have_sys = 1;
    pthread_mutex_unlock(&g_mu);
}

void a2k_settings_set_keys(int n)
{
    pthread_mutex_lock(&g_mu);
    g_keys = n;
    pthread_mutex_unlock(&g_mu);
}

/* ------------------------------------------------------------------ actions */
static const a2k_action_info_t g_actions[] = {
    { "ptz.home", AKEY_WRITE, NULL },
    { "ptz.park", AKEY_WRITE, NULL },
    { "ptz.recalibrate", AKEY_WRITE, NULL },
    { "ptz.step", AKEY_WRITE, "dir:left|right|up|down" },
    { "ptz.move", AKEY_WRITE, "dx:-400..400,dy:-400..400" },
    { "settings.reset", AKEY_WRITE, "key:<setting key> | prefix:<key prefix>" },
    { "settings.save", AKEY_WRITE, NULL },
};
#define N_ACTIONS (sizeof g_actions / sizeof g_actions[0])

size_t a2k_settings_action_count(void) { return N_ACTIONS; }
const a2k_action_info_t *a2k_settings_action(size_t i) { return i < N_ACTIONS ? &g_actions[i] : NULL; }

int a2k_settings_action_find(const char *name)
{
    size_t i;
    for (i = 0; i < N_ACTIONS; i++)
        if (strcmp(name, g_actions[i].name) == 0)
            return (int)i;
    return -1;
}

static const char *arg_of(const jflat_t *in, const char *name)
{
    size_t i;
    for (i = 0; i < in->n; i++)
        if (strcmp(in->key[i], name) == 0)
            return in->val[i];
    return NULL;
}

static int parse_step(const char *s, int *out)
{
    char *end;
    long v;
    if (!s || !*s || strlen(s) > 5)
        return -1;
    errno = 0;
    v = strtol(s, &end, 10);
    if (errno || *end || v < -400 || v > 400)
        return -1;
    *out = (int)v;
    return 0;
}

static long mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static void job_err(a2k_job_t *j, int http, const char *err, const char *detail)
{
    j->http = http;
    j->err = err;
    snprintf(j->detail, sizeof j->detail, "%s", detail ? detail : "-");
}

static int g_dirty;

static void act_reset(a2k_job_t *j)
{
    const char *key = arg_of(&j->in, "key"), *pre = arg_of(&j->in, "prefix");
    size_t i;
    int done = 0, failed = 0;
    if ((key != NULL) == (pre != NULL)) {
        job_err(j, 422, "args", "exactly one of key or prefix");
        return;
    }
    for (i = 0; i < g_reg.n; i++) {
        const st_def_t *d = &g_reg.defs[i];
        st_result_t r;
        if (!(d->access & ST_ACC_WRITE) || d->secret || !d->def || !streg_find(&g_reg, d->key))
            continue;
        if (key ? strcmp(key, d->key) != 0 : strncmp(pre, d->key, strlen(pre)) != 0)
            continue;
        if (d->min_scope > j->scope)
            continue;
        if (streg_set(&g_reg, j->src, j->scope, d->key, d->def, &r) == 200)
            done++, g_dirty |= d->persist;
        else
            failed++;
    }
    if (!done && !failed) {
        job_err(j, 404, "unknown_key", "no such writable setting");
        return;
    }
    j->http = failed ? 207 : 200;
    snprintf(j->detail, sizeof j->detail, "reset %d failed %d", done, failed);
}

static void run_action(a2k_job_t *j)
{
    static long last_move;
    int idx = a2k_settings_action_find(j->name), rc = 0, dx = 0, dy = 0;
    const char *a;
    j->http = 200;
    j->err = NULL;
    snprintf(j->detail, sizeof j->detail, "ok");
    if (idx < 0) {
        job_err(j, 404, "unknown_action", NULL);
        return;
    }
    if (g_actions[idx].min_scope > j->scope) {
        job_err(j, 403, "scope", NULL);
        return;
    }
    if (strcmp(j->name, "ptz.home") == 0)
        rc = a2k_ptz_command(A2K_PTZ_HOME);
    else if (strcmp(j->name, "ptz.park") == 0)
        rc = a2k_ptz_command(A2K_PTZ_PARK);
    else if (strcmp(j->name, "ptz.recalibrate") == 0)
        rc = a2k_ptz_command(A2K_PTZ_RECAL);
    else if (strcmp(j->name, "ptz.step") == 0) {
        a = arg_of(&j->in, "dir");
        if (a && !strcmp(a, "left")) rc = a2k_ptz_command(A2K_PTZ_LEFT);
        else if (a && !strcmp(a, "right")) rc = a2k_ptz_command(A2K_PTZ_RIGHT);
        else if (a && !strcmp(a, "up")) rc = a2k_ptz_command(A2K_PTZ_UP);
        else if (a && !strcmp(a, "down")) rc = a2k_ptz_command(A2K_PTZ_DOWN);
        else {
            job_err(j, 422, "args", "dir is left, right, up or down");
            return;
        }
    } else if (strcmp(j->name, "ptz.move") == 0) {
        long now = mono_ms();
        const char *sx = arg_of(&j->in, "dx"), *sy = arg_of(&j->in, "dy");
        if ((sx && parse_step(sx, &dx)) || (sy && parse_step(sy, &dy)) || (!dx && !dy)) {
            job_err(j, 422, "args", "dx and dy are integers in -400..400, not both 0");
            return;
        }
        if (last_move && now - last_move < 150) {
            job_err(j, 429, "rate", "ptz.move at most 6 per second");
            return;
        }
        last_move = now;
        rc = a2k_ptz_nudge(dx, dy);
    } else if (strcmp(j->name, "settings.reset") == 0) {
        act_reset(j);
        return;
    } else if (strcmp(j->name, "settings.save") == 0) {
        g_dirty = 1;
        a2k_settings_test_flush();
        return;
    }
    if (rc != 0)
        job_err(j, 409, "unavailable", "the motor is not running or refused the command");
}

/* ------------------------------------------------------------------ worker */
static pthread_t g_tid;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static int g_started, g_stop;
static a2k_job_t *g_job;     /* posted, not yet taken by the worker */
static int g_running, g_done; /* a job is running; the posted job finished */
static double g_tokens = A2K_SET_FLASH_BURST;
static long g_tok_ms;

static void save_now(void)
{
    char buf[STREG_FILE_MAX + 1];
    int n = streg_save_text(&g_reg, buf, sizeof buf);
    if (n < 0) {
        a2k_log("settings: cannot build the save text (%d)", n);
        return;
    }
    if (a2k_store_write(g_path, buf, (size_t)n) == 0)
        g_dirty = 0;
    else
        a2k_log("settings: cannot write %s (will retry)", g_path);
}

void a2k_settings_test_flush(void)
{
    if (g_dirty)
        save_now();
}

static void flush_tick(void)
{
    long now = mono_ms();
    if (!g_dirty)
        return;
    g_tokens += (double)(now - g_tok_ms) / 1000.0 / A2K_SET_FLASH_GAP_S;
    g_tok_ms = now;
    if (g_tokens > A2K_SET_FLASH_BURST)
        g_tokens = A2K_SET_FLASH_BURST;
    if (g_tokens >= 1.0) {
        g_tokens -= 1.0;
        save_now();
    }
}

static void changed_event(const a2k_job_t *j)
{
    char d[A2K_EVT_DATA + 1], *p = d;
    size_t i, left = sizeof d - 8;
    int overflow = 0, n = 0;
    p += snprintf(d, sizeof d, "{\"keys\":%c", '"');
    for (i = 0; i < j->in.n; i++) {
        size_t l;
        if (!j->res[i].ok)
            continue;
        n++;
        l = strlen(j->in.key[i]);
        if (l + 1 + (size_t)(p - d) >= left) {
            overflow = 1;
            continue;
        }
        if (p[-1] != '"')
            *p++ = ',';
        memcpy(p, j->in.key[i], l);
        p += l;
    }
    if (!n)
        return;
    if (overflow)
        snprintf(d, sizeof d, "{\"n\":%d}", n);
    else
        snprintf(p, sizeof d - (size_t)(p - d), "%c}", '"');
    a2k_api_event("setting", d);
}

static void do_patch(a2k_job_t *j)
{
    const char *k[JFLAT_MAX], *v[JFLAT_MAX];
    size_t i;
    for (i = 0; i < j->in.n; i++) {
        k[i] = j->in.key[i];
        v[i] = j->in.val[i];
    }
    j->err = NULL;
    j->http = streg_patch(&g_reg, j->src, j->scope, j->in.n, k, v, j->res);
    for (i = 0; i < j->in.n; i++) {
        const st_def_t *d = j->res[i].ok ? streg_find(&g_reg, k[i]) : NULL;
        if (d && d->persist)
            g_dirty = 1;
    }
    changed_event(j);
}

static void *worker(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&g_mu);
    while (!g_stop) {
        struct timespec ts;
        a2k_job_t *j = g_job;
        if (!j) {
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += 1;
            pthread_cond_timedwait(&g_cv, &g_mu, &ts);
            if (!g_job) {
                pthread_mutex_unlock(&g_mu);
                flush_tick();
                pthread_mutex_lock(&g_mu);
            }
            continue;
        }
        g_job = NULL;
        g_running = 1;
        pthread_mutex_unlock(&g_mu);
        if (j->kind == A2K_JOB_PATCH)
            do_patch(j);
        else
            run_action(j);
        pthread_mutex_lock(&g_mu);
        g_running = 0;
        g_done = 1;
        pthread_cond_broadcast(&g_cv);
    }
    pthread_mutex_unlock(&g_mu);
    return NULL;
}

int a2k_settings_idle(void)
{
    int idle;
    pthread_mutex_lock(&g_mu);
    idle = !g_job && !g_running;
    pthread_mutex_unlock(&g_mu);
    return idle;
}

int a2k_settings_exec(a2k_job_t *j)
{
    struct timespec ts;
    int rc = 0;
    pthread_mutex_lock(&g_mu);
    if (!g_started || g_job || g_running) {
        pthread_mutex_unlock(&g_mu);
        return -2;
    }
    g_job = j;
    g_done = 0;
    pthread_cond_broadcast(&g_cv);
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += A2K_SET_WAIT_MS / 1000;
    ts.tv_nsec += (A2K_SET_WAIT_MS % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
    }
    while (!g_done && rc == 0)
        if (pthread_cond_timedwait(&g_cv, &g_mu, &ts) == ETIMEDOUT)
            rc = -1;
    if (g_done)
        rc = 0;
    pthread_mutex_unlock(&g_mu);
    return rc;
}

int a2k_settings_start(const a2k_settings_cfg_t *cfg)
{
    pthread_attr_t at;
    size_t bad = 0;
    int rc;
    if (g_started)
        return 0;
    memset(&g_cfg, 0, sizeof g_cfg);
    if (cfg)
        g_cfg = *cfg;
    snprintf(g_path, sizeof g_path, "%s", g_cfg.settings_path ? g_cfg.settings_path : A2K_SET_FILE);
    snprintf(g_netdir, sizeof g_netdir, "%s", g_cfg.net_dir ? g_cfg.net_dir : A2K_SET_NET_DIR);
    snprintf(g_netreq, sizeof g_netreq, "%s", g_cfg.net_req ? g_cfg.net_req : A2K_SET_NET_REQ);
    if (g_cfg.image)
        g_img = *g_cfg.image;
    else
        a2k_image_defaults(&g_img);
    a2k_netreg_set_backend(&g_netb);
    build_table();
    if (streg_init(&g_reg, g_defs, g_nrows, MODEL_2K, 0, &bad) != 0) {
        a2k_log("settings: the registry table is invalid at row %lu (%s): API settings disabled", (unsigned long)bad,
                bad < g_nrows ? g_defs[bad].key : "?");
        return -1;
    }
    g_stop = 0;
    g_job = NULL;
    g_running = 0;
    g_tok_ms = mono_ms();
    g_tokens = A2K_SET_FLASH_BURST;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 32 * 1024);
    rc = pthread_create(&g_tid, &at, worker, NULL);
    pthread_attr_destroy(&at);
    if (rc != 0) {
        a2k_log("settings: cannot start the worker thread");
        return -1;
    }
    g_started = 1;
    return 0;
}

void a2k_settings_load(void)
{
    char buf[A2K_STORE_MAX + 1];
    size_t err = 0, ignored = 0, failed = 0;
    int n, rc;
    if (!g_started)
        return;
    n = a2k_store_read(g_path, buf, sizeof buf);
    if (n < 0) {
        if (n == -2)
            a2k_log("settings: %s is larger than %d bytes: ignored", g_path, A2K_STORE_MAX);
        return; /* none yet: the defaults and akam2k.conf apply */
    }
    rc = streg_load_text(&g_reg, buf, (size_t)n, &err, &ignored, &failed);
    if (rc != 0)
        a2k_log("settings: %s is invalid at line %lu (%s): ignored as a whole", g_path, (unsigned long)err, streg_err_name(rc));
    else
        a2k_log("settings: loaded %s (%lu key(s) ignored, %lu setter(s) failed)", g_path, (unsigned long)ignored,
                (unsigned long)failed);
}

void a2k_settings_stop(void)
{
    if (!g_started)
        return;
    pthread_mutex_lock(&g_mu);
    g_stop = 1;
    pthread_cond_broadcast(&g_cv);
    pthread_mutex_unlock(&g_mu);
    pthread_join(g_tid, NULL);
    g_started = 0;
    if (g_dirty)
        save_now();
}
