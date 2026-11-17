/* See ak1080_settings.h. */
#include "ak1080_settings.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "api_keys.h"
#include "cfg_file.h"
#include "control.h"
#include "ctl_sys.h"
#include "hal_sys.h"
#include "isp_tune.h"
#include "leds.h"
#include "mqtt_svc.h"
#include "night.h"
#include "onvif_cfg.h"
#include "onvif_device.h"
#include "version.h"
#include "video_cfg.h"
#include "video_ctl.h"

#define MODEL_1080P 2u
#define MAX_ROWS 56
#define RO ST_ACC_READ
#define RW (ST_ACC_READ | ST_ACC_WRITE)

#ifndef AK1080_NTP_FILE /* tests point it elsewhere */
#define AK1080_NTP_FILE "/etc/config/akam/ntp"
#endif

const char *ak1080_ntp_path = AK1080_NTP_FILE;

static st_def_t g_defs[MAX_ROWS];
static size_t g_nrows;
static int g_overflow, g_ready;
static streg_t g_reg;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER; /* streg_patch keeps the pending pairs in the registry: one patch at a time */
static int g_keys;

/* What the setter that is running says about its own persistence: set per thread by the door the write came through (a loopback or
 * an authenticated write is saved, a network 8091 write in DEBUG mode is RAM only where the module has that choice). */
static __thread int t_persist = 1;
static __thread char t_note[64]; /* " (not saved: reason)" of the last legacy answer */

/* ------------------------------------------------------------------ the names of the rows the old 8091 code already has */
static const struct { const char *key, *legacy; } k_map[] = {
    { "led.mode", "led.status" },
    { "night.ir_led", "night.ir_led" },
    { "night.mode", "night.mode" },
    { "image.brightness", "isp.brightness" },
    { "image.contrast", "isp.contrast" },
    { "image.saturation", "isp.saturation" },
    { "image.sharpness", "isp.sharpness" },
    { "image.denoise", "isp.denoise" },
    { "video0.kbps", "video.bitrate_cfg" },
    { "video0.fps", "video.fps_cfg" },
    { "video0.gop", "video.gop_cfg" },
    { "video0.min_qp", "video.minqp_cfg" },
    { "video0.max_qp", "video.maxqp_cfg" },
    { "video0.cfg_pending", "video.cfg_pending" },
};

static const char *legacy_of(const char *key)
{
    for (size_t i = 0; i < sizeof k_map / sizeof k_map[0]; i++)
        if (!strcmp(key, k_map[i].key)) return k_map[i].legacy;
    return NULL;
}

/* "key=value" (the answer of a legacy getter) -> value */
static int value_of(const char *line, const char *legacy, char *out, size_t cap)
{
    size_t kl = strlen(legacy);
    if (strncmp(line, legacy, kl) || line[kl] != '=') return ST_E_GETTER;
    int n = snprintf(out, cap, "%s", line + kl + 1);
    char *sp = strstr(out, " (");   /* "video.bitrate_cfg=2000" never has a note, "(read-only)" lines are not used */
    if (sp) { *sp = '\0'; n = (int)strlen(out); }
    return n;
}

/* ------------------------------------------------------------------ rows backed by the 8091 modules */
static int legacy_get(const char *key, char *out, size_t cap)
{
    const char *lg = legacy_of(key);
    char line[200];
    if (!lg) return ST_E_GETTER;
    if (!strncmp(lg, "isp.", 4) || !strcmp(lg, "night.mode")) {
        if (control_get_setting(lg, out, cap) != 0) return ST_E_GETTER;
        return (int)strlen(out);
    }
    if (!strncmp(lg, "video.", 6)) {
        if (video_ctl_get(lg, line, sizeof line) != 0) return ST_E_GETTER;
        return value_of(line, lg, out, cap);
    }
    if (ledsys_ctl_get(lg, line, sizeof line) != 0) return ST_E_GETTER;
    return value_of(line, lg, out, cap);
}

static int legacy_set(const char *key, const char *val, st_apply_t *eff)
{
    const char *lg = legacy_of(key);
    char msg[200], *p;
    (void)eff; /* the apply mode of the row (live, or restart_component for video0.*: ak_rtsp reads it at its next start) is the row's own */
    if (!lg) return -1;
    t_note[0] = '\0';
    if (control_apply_setting(lg, val, t_persist, msg, sizeof msg) != 0) return -1;
    p = strstr(msg, " (not saved");
    if (p) snprintf(t_note, sizeof t_note, "%s", p);
    return 0;
}

/* ------------------------------------------------------------------ LED patterns */
static int led_rule(const char *key) { return leds_rule_find(key + 4); } /* "led.stream" -> stream */

static int pat_get(const char *key, char *out, size_t cap)
{
    if (ledsys_pat_get(led_rule(key), out, cap) != 0) return ST_E_GETTER;
    return (int)strlen(out);
}

static int pat_set(const char *key, const char *val, st_apply_t *eff)
{
    (void)eff;
    t_note[0] = '\0';
    int rc = ledsys_pat_set(led_rule(key), val);
    if (rc == -2) snprintf(t_note, sizeof t_note, " (not saved: %s)", strerror(errno));
    return rc == -1 ? -1 : 0;
}

/* The registry grammar of a LED pattern is the wide one shared with the 2K table; the engine has the strict one (N 1..10, times
 * 50 ms and up, cycle at most 2 minutes). Checked BEFORE any setter runs, so a bad pattern refuses the whole PATCH instead of failing
 * after other rows were applied. */
static int pat_check(const streg_t *reg, const char *key, const char *val)
{
    a2k_pat_t p;
    (void)reg;
    (void)key;
    return a2k_pat_parse(val, &p);
}

/* ------------------------------------------------------------------ video: min QP below max QP over the whole patch */
static int video_check(const streg_t *reg, const char *key, const char *val)
{
    char a[16], b[16];
    int lo, hi;
    if (strcmp(key, "video0.min_qp") && strcmp(key, "video0.max_qp")) return 0;
    if (streg_peek(reg, "video0.min_qp", a, sizeof a) < 0 || streg_peek(reg, "video0.max_qp", b, sizeof b) < 0) return 0;
    (void)val;
    lo = atoi(a);
    hi = atoi(b);
    return video_cfg_qp_ok(lo, hi) ? 0 : -1;
}

/* ------------------------------------------------------------------ state rows */
static int put_long(char *out, size_t cap, long v) { return snprintf(out, cap, "%ld", v); }
static int put_str(char *out, size_t cap, const char *s) { return snprintf(out, cap, "%s", s); }

/* A ctl_sys number: "unknown" is HAL_SYS_NA, as on the 2K. */
static int sys_num(const char *key, char *out, size_t cap)
{
    char v[48];
    if (ctl_sys_get(key, v, sizeof v) != 0) return ST_E_GETTER;
    return put_str(out, cap, !strcmp(v, "unknown") ? "-1000000" : v);
}

static int state_get(const char *key, char *out, size_t cap)
{
    char v[96];
    if (!strncmp(key, "sys.", 4) && strcmp(key, "sys.version") && strcmp(key, "sys.time_set")) return sys_num(key, out, cap);
    if (!strcmp(key, "net.wifi_rssi") || !strcmp(key, "net.wifi_lq") || !strcmp(key, "rtsp.clients") ||
        !strcmp(key, "rtsp.skipped_frames") || !strcmp(key, "sys.time_set"))
        return sys_num(key, out, cap);
    if (!strcmp(key, "rtsp.auth")) {
        if (ctl_sys_get("rtsp.auth", v, sizeof v) != 0) return ST_E_GETTER;
        return put_str(out, cap, v);
    }
    if (!strcmp(key, "video0.fps_x10")) return sys_num("video.fps_actual", out, cap);
    if (!strcmp(key, "video0.bitrate_kbps")) return sys_num("video.bitrate", out, cap);
    if (!strcmp(key, "night.active")) return put_str(out, cap, night_is_night() ? "1" : "0");
    if (!strcmp(key, "sys.version")) return put_str(out, cap, firmware_version());
    if (!strcmp(key, "api.keys")) return put_long(out, cap, g_keys);
    if (!strcmp(key, "net.hostname")) {
        char hn[33];
        onvif_hostname_get(hn);
        return put_str(out, cap, hn);
    }
    if (!strcmp(key, "onvif.state")) {
        onvif_svc_state(v, ONVIF_STATE_MAX);
        return put_str(out, cap, v);
    }
    if (!strcmp(key, "mqtt.state")) {
        mqtt_svc_state(v, MQTT_STATE_MAX);
        return put_str(out, cap, v);
    }
    if (!strcmp(key, "net.ntp")) { /* the servers of /etc/config/akam/ntp, space separated; "" while NTP is off (no file) */
        char buf[256], *save = NULL, *line;
        FILE *f = fopen(ak1080_ntp_path, "r");
        size_t n;
        out[0] = '\0';
        if (!f) return 0;
        n = fread(buf, 1, sizeof buf - 1, f);
        fclose(f);
        buf[n] = '\0';
        for (line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
            size_t l = strlen(out), s;
            if (strncmp(line, "server=", 7)) continue;
            s = strlen(line + 7);
            if (s == 0 || l + s + 2 >= cap) continue;
            snprintf(out + l, cap - l, "%s%s", l ? " " : "", line + 7);
        }
        return (int)strlen(out);
    }
    return ST_E_GETTER;
}

/* ------------------------------------------------------------------ ONVIF and MQTT on/off (live) */
static int onvif_get(const char *key, char *out, size_t cap)
{
    onvif_cfg_t c;
    (void)key;
    (void)onvif_cfg_load(&c);
    return put_str(out, cap, c.enabled ? "1" : "0");
}

static int write_onvif(int enabled, int port)
{
    char t[40];
    int n = snprintf(t, sizeof t, "enabled=%d\nport=%d\n", enabled, port);
    return cfg_write_atomic(onvif_cfg_path, t, (size_t)n);
}

/* The same rule as akam_onvif.sh: enabling is refused unless the service can start (the stream password is set for its login),
 * and the previous flag file is put back. Disabling always works. */
static int onvif_set(const char *key, const char *val, st_apply_t *eff)
{
    onvif_cfg_t cur;
    char st[ONVIF_STATE_MAX];
    int want = val[0] == '1', had, port;
    (void)key;
    (void)eff;
    had = onvif_cfg_load(&cur); /* 0 valid, 1 absent, -1 invalid (read as off) */
    port = cur.port > 0 ? cur.port : 80;
    if (had == 0 && cur.enabled == want) return 0;
    if (had != 0 && !want) return 0; /* already off */
    if (write_onvif(want, port) != 0) return -1;
    (void)onvif_svc_reload(st, sizeof st);
    if (want && strncmp(st, "on:", 3) != 0) { /* no_password, port in use...: back to the old state */
        if (had == 0) (void)write_onvif(cur.enabled, port);
        else (void)unlink(onvif_cfg_path);
        (void)onvif_svc_reload(st, sizeof st);
        return -1;
    }
    return 0;
}

static int mqtt_get(const char *key, char *out, size_t cap)
{
    mqtt_cfg_t c;
    (void)key;
    (void)mqtt_cfg_load(&c);
    return put_str(out, cap, c.enabled ? "1" : "0");
}

/* The file is written by akam_mqtt.sh (host, credentials): this row only flips enabled= in it, and refuses when there is no valid file. */
static int mqtt_set(const char *key, const char *val, st_apply_t *eff)
{
    char old[MQTT_CFG_MAX + 2], nu[MQTT_CFG_MAX + 32], st[MQTT_STATE_MAX], *save = NULL, *line, copy[MQTT_CFG_MAX + 2];
    size_t n, o = 0;
    FILE *f;
    mqtt_cfg_t chk;
    int want = val[0] == '1', seen = 0, rc;
    (void)key;
    (void)eff;
    f = fopen(mqtt_cfg_path, "r");
    if (!f) return -1; /* nothing to switch: configure the broker first (akam_mqtt.sh) */
    n = fread(old, 1, MQTT_CFG_MAX + 1, f);
    fclose(f);
    if (n > MQTT_CFG_MAX) return -1;
    old[n] = '\0';
    snprintf(copy, sizeof copy, "%s", old);
    for (line = strtok_r(copy, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        int w;
        if (!strncmp(line, "enabled=", 8)) {
            w = snprintf(nu + o, sizeof nu - o, "enabled=%d\n", want);
            seen = 1;
        } else {
            w = snprintf(nu + o, sizeof nu - o, "%s\n", line);
        }
        if (w < 0 || (size_t)w >= sizeof nu - o) return -1;
        o += (size_t)w;
    }
    if (!seen) {
        int w = snprintf(nu + o, sizeof nu - o, "enabled=%d\n", want);
        if (w < 0 || (size_t)w >= sizeof nu - o) return -1;
        o += (size_t)w;
    }
    if (o > MQTT_CFG_MAX || mqtt_cfg_parse(nu, o, &chk) != 0) return -1;
    if (!(o == n && !memcmp(old, nu, n)) && cfg_write_atomic(mqtt_cfg_path, nu, o) != 0) /* an unchanged file is not rewritten */
        return -1;
    rc = mqtt_svc_reload(st, sizeof st);
    if (want && rc != 0) { /* the thread could not start: put the old file back */
        (void)cfg_write_atomic(mqtt_cfg_path, old, n);
        (void)mqtt_svc_reload(st, sizeof st);
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ the table */
static char g_def_buf[16][16];
static int g_def_n;

static const char *defnum(int v)
{
    snprintf(g_def_buf[g_def_n], sizeof g_def_buf[0], "%d", v);
    return g_def_buf[g_def_n++];
}

static st_def_t *row(const char *key, st_type_t type, long lo, long hi, const char *const *opts, const char *def, uint8_t access,
                     int (*get)(const char *, char *, size_t), int (*set)(const char *, const char *, st_apply_t *))
{
    st_def_t *d;
    if (g_nrows >= MAX_ROWS) {
        g_overflow++; /* never silent: init reports it and refuses to start the registry */
        return &g_defs[MAX_ROWS - 1];
    }
    d = &g_defs[g_nrows++];
    memset(d, 0, sizeof *d);
    d->key = key;
    d->type = type;
    d->lo = lo;
    d->hi = hi;
    d->opts = opts;
    d->def = def;
    d->apply = AP_LIVE;
    d->access = access;
    d->expose = ST_EXPOSE_HTTP;
    d->persist = 0; /* the module of the row owns its file */
    d->get = get;
    d->set = set;
    return d;
}

static const char *const OPT_MODE[] = { "auto", "on", "off", NULL };
static const char *const OPT_IR[] = { "auto", "off", NULL };
static const char *const OPT_NIGHT[] = { "auto", "day", "night", NULL };
static const char *const OPT_FPS[] = { "10", "15", "20", "25", NULL };
static const char *const G_LED[] = { "led", NULL };
static char g_pat_def[LEDS_RULE_N][A2K_PAT_MAX];
static char g_led_key[LEDS_RULE_N][16];
static const char k_empty[1] = { 0 };

static void build_table(void)
{
    video_cfg_t vd;
    int r;
    g_nrows = 0;
    g_def_n = 0;
    video_cfg_defaults(&vd);
    row("led.mode", ST_ENUM, 0, 0, OPT_MODE, "on", RW, legacy_get, legacy_set);
    for (r = 0; r < LEDS_RULE_N; r++) {
        snprintf(g_led_key[r], sizeof g_led_key[r], "led.%s", leds_rule_name(r));
        ledsys_pat_default(r, g_pat_def[r], sizeof g_pat_def[r]);
        row(g_led_key[r], ST_PATTERN, 0, 0, G_LED, g_pat_def[r], RW, pat_get, pat_set)->check = pat_check;
    }
    row("night.ir_led", ST_ENUM, 0, 0, OPT_IR, "auto", RW, legacy_get, legacy_set);
    row("night.mode", ST_ENUM, 0, 0, OPT_NIGHT, "auto", RW, legacy_get, legacy_set);
    row("image.brightness", ST_INT, ISP_TUNE_MIN, ISP_TUNE_MAX, NULL, "0", RW, legacy_get, legacy_set);
    row("image.contrast", ST_INT, ISP_TUNE_MIN, ISP_TUNE_MAX, NULL, "0", RW, legacy_get, legacy_set);
    row("image.saturation", ST_INT, ISP_TUNE_MIN, ISP_TUNE_MAX, NULL, "0", RW, legacy_get, legacy_set);
    row("image.sharpness", ST_INT, ISP_TUNE_MIN, ISP_TUNE_MAX, NULL, defnum(ISP_TUNE_DEFAULT_SHARPNESS), RW, legacy_get, legacy_set);
    row("image.denoise", ST_INT, 0, ISP_TUNE_DENOISE_MAX, NULL, defnum(ISP_TUNE_DEFAULT_DENOISE), RW, legacy_get, legacy_set);
    /* video: saved now, used by the encoder at the next start of ak_rtsp (the kernel has no proven live setter, convergence P10) */
    row("video0.kbps", ST_INT, VIDEO_KBPS_MIN, VIDEO_KBPS_MAX, NULL, defnum(vd.bitrate), RW, legacy_get, legacy_set)->apply = AP_RESTART_COMPONENT;
    row("video0.fps", ST_ENUM, 0, 0, OPT_FPS, defnum(vd.fps), RW, legacy_get, legacy_set)->apply = AP_RESTART_COMPONENT;
    row("video0.gop", ST_INT, VIDEO_GOP_S_MIN, VIDEO_GOP_S_MAX, NULL, defnum(vd.gop_s), RW, legacy_get, legacy_set)->apply = AP_RESTART_COMPONENT;
    row("video0.min_qp", ST_INT, VIDEO_QP_MIN, VIDEO_QP_MAX, NULL, defnum(vd.minqp), RW, legacy_get, legacy_set)->apply = AP_RESTART_COMPONENT;
    g_defs[g_nrows - 1].check = video_check;
    row("video0.max_qp", ST_INT, VIDEO_QP_MIN, VIDEO_QP_MAX, NULL, defnum(vd.maxqp), RW, legacy_get, legacy_set)->apply = AP_RESTART_COMPONENT;
    g_defs[g_nrows - 1].check = video_check;
    row("onvif.enabled", ST_BOOL, 0, 0, NULL, "0", RW, onvif_get, onvif_set);
    row("mqtt.enabled", ST_BOOL, 0, 0, NULL, "0", RW, mqtt_get, mqtt_set);
    /* read-only state */
    row("sys.cpu_pct", ST_INT, HAL_SYS_NA, 100, NULL, "-1000000", RO, state_get, NULL);
    row("sys.load1_x100", ST_INT, HAL_SYS_NA, 100000, NULL, "-1000000", RO, state_get, NULL);
    row("sys.mem_total_kb", ST_INT, HAL_SYS_NA, 100000000, NULL, "-1000000", RO, state_get, NULL);
    row("sys.mem_avail_kb", ST_INT, HAL_SYS_NA, 100000000, NULL, "-1000000", RO, state_get, NULL);
    row("sys.uptime_s", ST_INT, HAL_SYS_NA, 2000000000, NULL, "-1000000", RO, state_get, NULL);
    row("sys.time_set", ST_BOOL, 0, 0, NULL, "0", RO, state_get, NULL);
    row("sys.version", ST_STRING, 1, 63, NULL, "unknown", RO, state_get, NULL);
    row("net.wifi_rssi", ST_INT, HAL_SYS_NA, 0, NULL, "-1000000", RO, state_get, NULL);
    row("net.wifi_lq", ST_INT, HAL_SYS_NA, 1000, NULL, "-1000000", RO, state_get, NULL);
    row("net.hostname", ST_STRING, 1, 32, NULL, "akam", RO, state_get, NULL);
    row("net.ntp", ST_STRING, 0, 200, NULL, k_empty, RO, state_get, NULL);
    row("rtsp.clients", ST_INT, 0, 16, NULL, "0", RO, state_get, NULL);
    row("rtsp.auth", ST_STRING, 1, 8, NULL, "unknown", RO, state_get, NULL);
    row("rtsp.skipped_frames", ST_INT, 0, 2000000000, NULL, "0", RO, state_get, NULL);
    row("video0.fps_x10", ST_INT, HAL_SYS_NA, 1000, NULL, "-1000000", RO, state_get, NULL);
    row("video0.bitrate_kbps", ST_INT, 0, 100000, NULL, "0", RO, state_get, NULL);
    row("video0.cfg_pending", ST_BOOL, 0, 0, NULL, "0", RO, legacy_get, NULL);
    row("night.active", ST_BOOL, 0, 0, NULL, "0", RO, state_get, NULL);
    row("onvif.state", ST_STRING, 1, 47, NULL, "off", RO, state_get, NULL);
    row("mqtt.state", ST_STRING, 1, 39, NULL, "off", RO, state_get, NULL);
    row("api.keys", ST_INT, 0, AKEY_MAX, NULL, "0", RO, state_get, NULL);
}

int ak1080_settings_init(void)
{
    size_t bad = 0;
    if (g_ready) return 0;
    g_overflow = 0;
    build_table();
    if (g_overflow) {
        printf("[settings] %d registry row(s) do not fit MAX_ROWS (%d): raise it\n", g_overflow, MAX_ROWS);
        return -1;
    }
    if (streg_init(&g_reg, g_defs, g_nrows, MODEL_1080P, 0, &bad) != 0) {
        printf("[settings] the registry table is invalid at row %lu (%s)\n", (unsigned long)bad, bad < g_nrows ? g_defs[bad].key : "?");
        return -1;
    }
    g_ready = 1;
    return 0;
}

const streg_t *ak1080_settings_reg(void) { return g_ready ? &g_reg : NULL; }

void ak1080_settings_set_keys(int n) { g_keys = n; }

/* ------------------------------------------------------------------ doors */
int ak1080_settings_ctl_get(const char *key, char *line, size_t cap)
{
    char v[STREG_VAL_MAX];
    if (!g_ready || !streg_find(&g_reg, key)) return -1;
    if (streg_get(&g_reg, ST_SRC_LOCAL, key, v, sizeof v) < 0) return -1;
    snprintf(line, cap, "%s=%s", key, v);
    return 0;
}

int ak1080_settings_ctl_set(const char *key, const char *val, int persist, char *msg, size_t cap)
{
    st_result_t r;
    int http;
    const st_def_t *d;
    if (!g_ready || !(d = streg_find(&g_reg, key))) return -1;
    /* A network peer in DEBUG mode (persist 0) gets RAM-only writes. The rows with no RAM-only mode (the ONVIF and MQTT switches, the LED
     * patterns) always write a file, and the services are local-only on the text port: not through the network door, at all. */
    if (!persist && (d->access & ST_ACC_WRITE) && d->set != legacy_set) {
        snprintf(msg, cap, "ERR %s can only be set from the camera", key);
        return 1;
    }
    pthread_mutex_lock(&g_mu);
    t_persist = persist;
    t_note[0] = '\0';
    http = streg_set(&g_reg, ST_SRC_LOCAL, AKEY_ADMIN, key, val, &r);
    t_persist = 1;
    pthread_mutex_unlock(&g_mu);
    if (http == 200) {
        snprintf(msg, cap, "OK %s=%s%s%s", key, val, r.apply != AP_LIVE ? " (restart to apply)" : "", t_note);
        return 0;
    }
    if (r.err == ST_E_READONLY) snprintf(msg, cap, "ERR %s is read-only", key);
    else snprintf(msg, cap, "ERR invalid %s value %s (%s)", key, val, streg_err_name(r.err));
    return 1;
}

int ak1080_settings_patch(int scope, size_t n, const char *const *keys, const char *const *vals, st_result_t *res)
{
    int http;
    if (!g_ready) return 422;
    pthread_mutex_lock(&g_mu);
    t_persist = 1;
    http = streg_patch(&g_reg, ST_SRC_HTTP, scope, n, keys, vals, res);
    pthread_mutex_unlock(&g_mu);
    return http;
}

int ak1080_settings_key_persists(const char *key)
{
    const st_def_t *d;
    if (!g_ready || !(d = streg_find(&g_reg, key)) || !(d->access & ST_ACC_WRITE)) return 0;
    return 1; /* every writable row saves a file of its module (only when the value changed: skip-if-equal in each module) */
}
