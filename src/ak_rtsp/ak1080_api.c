/* See ak1080_api.h. */
#include "ak1080_api.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "ak1080_settings.h"
#include "api_events.h"
#include "api_httpd.h"
#include "api_keys.h"
#include "cfg_file.h"
#include "leds.h"
#include "night.h"
#include "version.h"

#define FLASH_BURST 6.0
#define FLASH_GAP_S 20.0

/* ------------------------------------------------------------------ the binding: log and the file store */
static void log_line(const char *line) { printf("[api] %s\n", line); }

/* Whole small file: length (0 = empty), -1 absent or unreadable, -2 too big; buf is NUL terminated. */
static int store_read(const char *path, char *buf, size_t cap)
{
    struct stat st;
    FILE *f;
    size_t n;
    if (!cap)
        return -1;
    buf[0] = '\0';
    if (lstat(path, &st) != 0 || !S_ISREG(st.st_mode))
        return -1;
    f = fopen(path, "r");
    if (!f)
        return -1;
    n = fread(buf, 1, cap - 1, f);
    if (n == cap - 1 && fgetc(f) != EOF) {
        fclose(f);
        buf[0] = '\0';
        return -2;
    }
    fclose(f);
    buf[n] = '\0';
    return (int)n;
}

/* Atomic replace (tmp + fsync + rename, directory 0700, file 0600: cfg_file.c), skipped when the content is unchanged (flash wear). The
 * two users, the key file and the audit file, are both secrets-grade: 0600 is exactly what they need, so `mode` is not needed. */
static int store_write(const char *path, const char *text, size_t len, unsigned mode)
{
    char old[A2K_API_STORE_MAX + 1];
    int n;
    (void)mode;
    if (len > A2K_API_STORE_MAX)
        return -1;
    n = store_read(path, old, sizeof old);
    if (n >= 0 && (size_t)n == len && memcmp(old, text, len) == 0)
        return 0;
    return cfg_write_atomic(path, text, len) == 0 ? 0 : -1;
}

/* ------------------------------------------------------------------ the flash limiter */
static double g_tokens = FLASH_BURST;
static long g_tok_s;

static long mono_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec;
}

static int flash_take(void)
{
    long now = mono_s();
    if (g_tok_s)
        g_tokens += (double)(now - g_tok_s) / FLASH_GAP_S;
    g_tok_s = now;
    if (g_tokens > FLASH_BURST)
        g_tokens = FLASH_BURST;
    if (g_tokens < 1.0)
        return 0;
    g_tokens -= 1.0;
    return 1;
}

void ak1080_api_test_set_flash(double tokens)
{
    g_tokens = tokens;
    g_tok_s = mono_s();
}

/* ------------------------------------------------------------------ jobs (inline on the HTTP thread) */
static void job_err(a2k_job_t *j, int http, const char *err, const char *detail)
{
    j->http = http;
    j->err = err;
    snprintf(j->detail, sizeof j->detail, "%s", detail ? detail : "-");
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
    if (!flash_take()) {
        job_err(j, 429, "flash_rate", "settings are saved to flash at most 6 times, then once every 20 s");
        return;
    }
    for (i = 0; i < j->in.n; i++) {
        k[i] = j->in.key[i];
        v[i] = j->in.val[i];
    }
    j->err = NULL;
    j->http = ak1080_settings_patch(j->scope, j->in.n, k, v, j->res);
    if (j->http == 422)
        g_tokens += 1.0; /* nothing was applied or saved: the token is given back */
    changed_event(j);
}

static const a2k_action_info_t g_actions[] = {
    { "led.blip", AKEY_WRITE, "pattern:<on|off|N:on_ms:off_ms:pause_ms>,ms:1..60000 (default 5000)" },
    { "led.blip_stop", AKEY_WRITE, NULL },
    { "settings.reset", AKEY_WRITE, "key:<setting key> | prefix:<key prefix>" },
};
#define N_ACTIONS (sizeof g_actions / sizeof g_actions[0])

static size_t be_action_count(void) { return N_ACTIONS; }
static const a2k_action_info_t *be_action(size_t i) { return i < N_ACTIONS ? &g_actions[i] : NULL; }

static int be_action_find(const char *name)
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

/* settings.reset: the defaults of the rows matched by key or prefix, as ONE patch (so a pair of rows with a rule between them, min and max
 * QP, is checked together). Each row writes the file of its module only when its value changes. */
static void act_reset(a2k_job_t *j)
{
    const char *key = arg_of(&j->in, "key"), *pre = arg_of(&j->in, "prefix");
    const streg_t *reg = ak1080_settings_reg();
    const char *k[JFLAT_MAX], *v[JFLAT_MAX];
    st_result_t res[JFLAT_MAX];
    size_t i, n = 0;
    int http, ok = 0, bad = 0;
    if ((key != NULL) == (pre != NULL) || (pre && !pre[0])) { /* an empty prefix would reset every row, the ONVIF and MQTT switches included */
        job_err(j, 422, "args", "exactly one of key or a non-empty prefix");
        return;
    }
    for (i = 0; reg && i < reg->n && n < JFLAT_MAX; i++) {
        const st_def_t *d = &reg->defs[i];
        if (!(d->access & ST_ACC_WRITE) || d->secret || !d->def)
            continue;
        if (key ? strcmp(key, d->key) != 0 : strncmp(pre, d->key, strlen(pre)) != 0)
            continue;
        k[n] = d->key;
        v[n] = d->def;
        n++;
    }
    if (!n) {
        job_err(j, 404, "unknown_key", "no such writable setting");
        return;
    }
    if (!flash_take()) {
        job_err(j, 429, "flash_rate", "settings are saved to flash at most 6 times, then once every 20 s");
        return;
    }
    http = ak1080_settings_patch(j->scope, n, k, v, res);
    for (i = 0; i < n; i++) {
        ok += res[i].ok;
        bad += !res[i].ok;
    }
    if (!ok)
        g_tokens += 1.0;
    j->http = http == 200 ? 200 : http == 207 ? 207 : 422;
    snprintf(j->detail, sizeof j->detail, "reset %d failed %d", ok, bad);
    if (http != 200 && http != 207) {
        j->err = "refused";
        return;
    }
    if (ok) { /* tell the pollers which rows moved */
        char d[A2K_EVT_DATA + 1];
        snprintf(d, sizeof d, "{\"n\":%d}", ok);
        a2k_api_event("setting", d);
    }
}

static int parse_ms(const char *s, long *out)
{
    char *end;
    long v;
    if (!s || !*s || strlen(s) > 5)
        return -1;
    errno = 0;
    v = strtol(s, &end, 10);
    if (errno || *end || v < 1 || v > 60000)
        return -1;
    *out = v;
    return 0;
}

static void run_action(a2k_job_t *j)
{
    int idx = be_action_find(j->name);
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
    if (!strcmp(j->name, "led.blip")) {
        const char *pat = arg_of(&j->in, "pattern"), *ms = arg_of(&j->in, "ms");
        long dur = 5000;
        int rc = (!pat || (ms && parse_ms(ms, &dur) != 0)) ? -1 : ledsys_blip(pat, dur);
        if (rc == -2)
            job_err(j, 500, "led", "the LED could not be driven");
        else if (rc != 0)
            job_err(j, 422, "args", "pattern is on, off or N:on_ms:off_ms:pause_ms (N 1..10, times from 50 ms); ms is 1..60000");
    } else if (!strcmp(j->name, "led.blip_stop")) {
        ledsys_blip_clear();
    } else if (!strcmp(j->name, "settings.reset")) {
        act_reset(j);
    }
}

static int be_exec(a2k_job_t *j)
{
    if (j->kind == A2K_JOB_PATCH)
        do_patch(j);
    else
        run_action(j);
    return 0;
}

static int be_idle(void) { return 1; }
static const streg_t *be_reg(void) { return ak1080_settings_reg(); }
static void be_keys(int n) { ak1080_settings_set_keys(n); }

static const a2k_api_backend_t g_backend = {
    be_reg, be_exec, be_idle, be_action_count, be_action, be_action_find, be_keys,
};

/* ------------------------------------------------------------------ events */
static void night_cb(int night, int ir_on)
{
    char d[48];
    snprintf(d, sizeof d, "{\"night\":%s,\"ir_on\":%s}", night ? "true" : "false", ir_on ? "true" : "false");
    a2k_api_event("night", d);
}

/* ------------------------------------------------------------------ start, stop, the key tool */
void ak1080_api_env(a2k_api_env_t *env)
{
    memset(env, 0, sizeof *env);
    env->keys_path = AK1080_API_KEYS;
    env->audit_path = AK1080_API_AUDIT;
    env->model = "lsc-smart-indoor-1080p";
    env->version = firmware_version();
    env->log = log_line;
    env->store_read = store_read;
    env->store_write = store_write;
    env->backend = &g_backend;
    env->ext = NULL; /* no PTZ, no button: nothing of them exists on this camera */
    env->n_ext = 0;
}

int ak1080_api_start(int port, int loopback_only, const a2k_api_env_t *env)
{
    a2k_api_env_t e;
    if (ak1080_settings_init() != 0)
        return -1;
    if (env)
        e = *env;
    else
        ak1080_api_env(&e);
    night_set_notify(night_cb);
    return a2k_httpd_start(port, loopback_only, &e);
}

void ak1080_api_stop(void)
{
    a2k_httpd_stop(); /* first: no remote command may reach a setter while the modules stop */
    night_set_notify(NULL);
}

int ak1080_api_key_main(int argc, char **argv)
{
    a2k_api_env_t e;
    ak1080_api_env(&e);
    return a2k_api_key_cli(&e, argc, argv);
}
