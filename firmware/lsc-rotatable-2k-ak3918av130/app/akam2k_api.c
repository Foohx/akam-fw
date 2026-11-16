/* See akam2k_api.h. */
#include "akam2k_api.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "akam2k_apievt.h"
#include "akam2k_log.h"
#include "akam2k_mqttcfg.h" /* A2K_APP_VERSION */
#include "akam2k_settings.h"
#include "akam2k_store.h"
#include "api_keys.h"
#include "api_limit.h"
#include "api_sig.h"
#include "json_min.h"
#include "rtsp_auth.h" /* rtsp_auth_rng: getrandom with a non-blocking fallback, shared with the RTSP digest nonces */
#include "settings_reg.h"
#include "sha256.h"

enum { R_NONE, R_HELLO, R_INFO, R_SCHEMA, R_SETTINGS, R_SETTING_ONE, R_STATE, R_EVENTS, R_PATCH, R_ACTION, R_AUDIT, R_KEYS_LIST,
       R_KEYS_ADD, R_KEYS_DEL };

typedef struct {
    char line[256];
    uint8_t write;
} audit_t;

static a2k_api_env_t g_env;
static char g_keys_path[120], g_audit_path[120];
static akeys_t g_keys;
static const char *g_keys_err;
static time_t g_kmtime, g_kctime;
static mode_t g_kmode;
static off_t g_ksize;
static ino_t g_kino;
static int g_kseen;
static api_sig_t g_sig;
static alim_t g_lim;
static audit_t g_audit[A2K_API_AUDIT_RING];
static audit_t g_wr[A2K_API_AUDIT_PERSIST]; /* the write events only: reads and failed logins cannot push them out of the file */
static unsigned g_wr_n;
static unsigned g_audit_n;      /* total entries ever added */
static int g_audit_dirty;
static uint64_t g_audit_tok_ms; /* the persist limiter */
static double g_audit_tokens = 2.0;
static uint64_t g_start_ms;
static a2k_job_t g_job;
static const char k_nobody[1] = { 0 }; /* an absent body parses as the empty text */

/* ------------------------------------------------------------------ clocks and randomness */
static int64_t wall_s(void)
{
    return g_env.wall_s ? g_env.wall_s() : (int64_t)time(NULL);
}

static uint64_t mono_ms(void)
{
    struct timespec ts;
    if (g_env.mono_ms)
        return g_env.mono_ms();
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static int clock_set(void)
{
    return wall_s() >= A2K_API_CLOCK_FLOOR;
}

static int rng(void *buf, size_t n)
{
    return g_env.rng ? g_env.rng(buf, n) : rtsp_auth_rng(buf, n);
}

/* ------------------------------------------------------------------ small JSON replies */
static int finish(jw_t *w, a2k_api_out_t *res, int status)
{
    int n = jw_end(w);
    res->status = status;
    res->len = n < 0 ? 0 : (size_t)n;
    if (n < 0) {
        res->status = 500;
        res->len = 0;
    }
    return status;
}

static void err_reply(char *out, size_t cap, a2k_api_out_t *res, int status, const char *code, const char *detail)
{
    jw_t w;
    jw_init(&w, out, cap);
    jw_raw(&w, "{\"error\":");
    jw_str(&w, code);
    jw_raw(&w, ",\"detail\":");
    jw_str(&w, detail ? detail : code);
    jw_raw(&w, "}");
    finish(&w, res, status);
}

static void ok_reply(char *out, size_t cap, a2k_api_out_t *res, const char *detail)
{
    jw_t w;
    jw_init(&w, out, cap);
    jw_raw(&w, "{\"ok\":true");
    if (detail) {
        jw_raw(&w, ",\"detail\":");
        jw_str(&w, detail);
    }
    jw_raw(&w, "}");
    finish(&w, res, 200);
}

/* ------------------------------------------------------------------ keys */
static int keys_writer(void *ctx, const char *text, size_t len)
{
    (void)ctx;
    return a2k_store_write_mode(g_keys_path, text, len, 0600);
}

static void keys_changed(void)
{
    a2k_settings_set_keys((int)g_keys.n);
}

int a2k_api_keys_count(void) { return (int)g_keys.n; }
const char *a2k_api_keys_error(void) { return g_keys_err; }

int a2k_api_keys_refresh(void)
{
    struct stat st;
    char buf[AKEY_FILE_MAX + 1], a[AKEY_FILE_MAX + 64], b[AKEY_FILE_MAX + 64];
    akeys_t k;
    int n, na, nb;
    size_t i;
    if (stat(g_keys_path, &st) != 0) {
        if (g_kseen || g_keys.n) { /* the file is gone (a factory reset wipes /config): back to closed and unprovisioned */
            memset(&g_keys, 0, sizeof g_keys);
            g_kseen = 0;
            g_keys_err = NULL;
            keys_changed();
        }
        return 0;
    }
    if (g_kseen && st.st_mtime == g_kmtime && st.st_ctime == g_kctime && st.st_mode == g_kmode && st.st_size == g_ksize &&
        st.st_ino == g_kino)
        return (int)g_keys.n;
    g_kseen = 1;
    g_kmtime = st.st_mtime;
    g_kctime = st.st_ctime;
    g_kmode = st.st_mode;
    g_ksize = st.st_size;
    g_kino = st.st_ino;
    if (st.st_mode & 077) { /* readable by others: refused like mqtt_pass; the file is kept, the API stays closed */
        if (!g_keys_err)
            a2k_log("api: %s is readable by others (mode %o): the API stays closed", g_keys_path, (unsigned)(st.st_mode & 0777));
        memset(&g_keys, 0, sizeof g_keys);
        g_keys_err = "keys_perm";
        keys_changed();
        return 0;
    }
    n = a2k_store_read(g_keys_path, buf, sizeof buf);
    if (n < 0 || akeys_parse(&k, buf, (size_t)n, NULL) != AKEYS_OK) {
        akam_wipe(buf, sizeof buf);
        akam_wipe(&k, sizeof k);
        a2k_log("api: %s is invalid: the API stays closed (the file is kept)", g_keys_path);
        memset(&g_keys, 0, sizeof g_keys);
        g_keys_err = "keys_invalid";
        keys_changed();
        return 0;
    }
    g_keys_err = NULL;
    na = akeys_serialize(&g_keys, a, sizeof a);
    nb = akeys_serialize(&k, b, sizeof b);
    if (na != nb || (na > 0 && memcmp(a, b, (size_t)na) != 0)) {
        for (i = 0; i < g_keys.n; i++) /* revoked or replaced keys lose their nonces */
            if (akeys_find(&k, g_keys.k[i].id) < 0 || memcmp(k.k[akeys_find(&k, g_keys.k[i].id)].secret, g_keys.k[i].secret, 32))
                apisig_drop_key(&g_sig, g_keys.k[i].id);
        g_keys = k;
        a2k_log("api: key file reloaded, %d key(s)", (int)g_keys.n);
        keys_changed();
    }
    akam_wipe(buf, sizeof buf); /* the secrets must not stay on the stack */
    akam_wipe(a, sizeof a);
    akam_wipe(b, sizeof b);
    akam_wipe(&k, sizeof k);
    return (int)g_keys.n;
}

/* ------------------------------------------------------------------ audit */
static void audit_load(void)
{
    char buf[A2K_STORE_MAX + 1], *line, *save = NULL;
    int n = a2k_store_read(g_audit_path, buf, sizeof buf);
    if (n <= 0)
        return;
    for (line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        audit_t *a = &g_audit[g_audit_n % A2K_API_AUDIT_RING];
        if (strlen(line) >= sizeof a->line || line[0] != '{')
            continue;
        snprintf(a->line, sizeof a->line, "%s", line);
        a->write = 1;
        g_audit_n++;
        g_wr[g_wr_n % A2K_API_AUDIT_PERSIST] = *a;
        g_wr_n++;
    }
}

static void audit_persist(void)
{
    char buf[A2K_STORE_MAX + 1];
    size_t len = 0;
    unsigned i, total = g_wr_n < A2K_API_AUDIT_PERSIST ? g_wr_n : A2K_API_AUDIT_PERSIST;
    unsigned first = total;
    /* newest entries that fit the file, written oldest first */
    for (i = 0; i < total; i++) {
        size_t l = strlen(g_wr[(g_wr_n - 1 - i) % A2K_API_AUDIT_PERSIST].line);
        if (len + l + 1 > A2K_STORE_MAX)
            break;
        len += l + 1;
        first = total - 1 - i;
    }
    len = 0;
    for (i = first; i < total; i++) {
        const char *line = g_wr[(g_wr_n - total + i) % A2K_API_AUDIT_PERSIST].line;
        size_t l = strlen(line);
        memcpy(buf + len, line, l);
        len += l;
        buf[len++] = '\n';
    }
    if (a2k_store_write_mode(g_audit_path, buf, len, 0600) == 0)
        g_audit_dirty = 0;
}

static void audit_add(const a2k_api_in_t *in, int status, const char *keyid, int scope, const char *what, int write)
{
    audit_t *a = &g_audit[g_audit_n % A2K_API_AUDIT_RING];
    char kid[9];
    size_t i;
    int64_t t = wall_s();
    for (i = 0; i < 8 && keyid && keyid[i]; i++)
        kid[i] = (keyid[i] >= '0' && keyid[i] <= '9') || (keyid[i] >= 'a' && keyid[i] <= 'f') ? keyid[i] : '?';
    kid[i] = '\0';
    snprintf(a->line, sizeof a->line,
             "{\"t\":%lld,\"mono_s\":%llu,\"key\":\"%s\",\"scope\":\"%s\",\"peer\":\"%s\",\"req\":\"%.7s %.40s\",\"what\":\"%.80s\",\"res\":%d}",
             (long long)(clock_set() ? t : 0), (unsigned long long)(mono_ms() / 1000u), kid, akey_scope_name(scope),
             in->peer_str ? in->peer_str : "?", in->rq->method, in->rq->path, what ? what : "-", status);
    a->write = (uint8_t)write;
    g_audit_n++;
    if (write) {
        g_wr[g_wr_n % A2K_API_AUDIT_PERSIST] = *a;
        g_wr_n++;
        g_audit_dirty = 1;
    }
    a2k_log("api: %s %s %s key=%s -> %d", in->peer_str ? in->peer_str : "?", in->rq->method, in->rq->path, kid, status);
}

void a2k_api_tick(void)
{
    uint64_t now = mono_ms();
    a2k_api_keys_refresh();
    g_audit_tokens += (double)(now - g_audit_tok_ms) / 30000.0;
    g_audit_tok_ms = now;
    if (g_audit_tokens > 2.0)
        g_audit_tokens = 2.0;
    if (g_audit_dirty && g_audit_tokens >= 1.0) {
        g_audit_tokens -= 1.0;
        audit_persist();
    }
}

/* ------------------------------------------------------------------ init */
int a2k_api_init(const a2k_api_env_t *env)
{
    memset(&g_env, 0, sizeof g_env);
    if (env)
        g_env = *env;
    snprintf(g_keys_path, sizeof g_keys_path, "%s", g_env.keys_path ? g_env.keys_path : A2K_API_KEYS);
    snprintf(g_audit_path, sizeof g_audit_path, "%s", g_env.audit_path ? g_env.audit_path : A2K_API_AUDIT);
    memset(&g_keys, 0, sizeof g_keys);
    g_keys_err = NULL;
    g_kseen = 0;
    g_audit_n = 0;
    g_wr_n = 0;
    g_audit_dirty = 0;
    g_start_ms = mono_ms();
    g_audit_tok_ms = g_start_ms;
    g_audit_tokens = 2.0;
    apisig_init(&g_sig, APISIG_WINDOW_DEFAULT);
    alim_init(&g_lim, g_env.rate_milli ? g_env.rate_milli : APISIG_RATE_MILLI_DEFAULT,
              g_env.burst ? g_env.burst : APISIG_BURST_DEFAULT, g_start_ms);
    audit_load();
    return a2k_api_keys_refresh();
}

/* ------------------------------------------------------------------ query strings (no decoding: values are [A-Za-z0-9_.]) */
/* 1 found (copied into val), 0 absent, -1 bad query (unknown name, bad character, too long, repeated). names: NULL-terminated. */
static int query_get(const char *q, const char *const *names, const char *want, char *val, size_t cap)
{
    char buf[HREQ_QUERY_MAX + 1], *seg, *save = NULL;
    int found = 0;
    if (!q || !*q)
        return 0;
    snprintf(buf, sizeof buf, "%s", q);
    for (seg = strtok_r(buf, "&", &save); seg; seg = strtok_r(NULL, "&", &save)) {
        char *eq = strchr(seg, '=');
        const char *const *n;
        size_t i, vl;
        int known = 0;
        if (!eq)
            return -1;
        *eq = '\0';
        for (n = names; *n; n++)
            known |= strcmp(*n, seg) == 0;
        vl = strlen(eq + 1);
        if (!known || vl >= cap)
            return -1;
        for (i = 0; i < vl; i++) {
            char c = eq[1 + i];
            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-'))
                return -1;
        }
        if (strcmp(seg, want) == 0) {
            if (found)
                return -1;
            memcpy(val, eq + 1, vl + 1);
            found = 1;
        }
    }
    return found;
}

static int parse_u32(const char *s, uint32_t *out, uint32_t max)
{
    unsigned long v = 0;
    size_t i;
    if (!*s || strlen(s) > 9)
        return -1;
    for (i = 0; s[i]; i++) {
        if (s[i] < '0' || s[i] > '9')
            return -1;
        v = v * 10 + (unsigned long)(s[i] - '0');
    }
    if (v > max)
        return -1;
    *out = (uint32_t)v;
    return 0;
}

/* ------------------------------------------------------------------ routing */
static int route(const char *method, const char *sub, int *scope)
{
    int get = !strcmp(method, "GET"), patch = !strcmp(method, "PATCH"), post = !strcmp(method, "POST"), del = !strcmp(method, "DELETE");
    *scope = AKEY_READ;
    if (!strcmp(sub, "/hello")) return get ? R_HELLO : -405;
    if (!strcmp(sub, "/info")) return get ? R_INFO : -405;
    if (!strcmp(sub, "/schema")) return get ? R_SCHEMA : -405;
    if (!strcmp(sub, "/state")) return get ? R_STATE : -405;
    if (!strcmp(sub, "/events")) return get ? R_EVENTS : -405;
    if (!strcmp(sub, "/settings")) {
        if (get) return R_SETTINGS;
        *scope = AKEY_WRITE;
        return patch ? R_PATCH : -405;
    }
    if (!strncmp(sub, "/settings/", 10) && sub[10]) return get ? R_SETTING_ONE : -405;
    if (!strncmp(sub, "/actions/", 9) && sub[9]) {
        *scope = AKEY_WRITE;
        return post ? R_ACTION : -405;
    }
    *scope = AKEY_ADMIN;
    if (!strcmp(sub, "/audit")) return get ? R_AUDIT : -405;
    if (!strcmp(sub, "/keys")) return get ? R_KEYS_LIST : post ? R_KEYS_ADD : -405;
    if (!strncmp(sub, "/keys/", 6) && sub[6]) return del ? R_KEYS_DEL : -405;
    return -404;
}

/* ------------------------------------------------------------------ handlers */
static void h_info(const akeys_t *keys, int key_idx, char *out, size_t cap, a2k_api_out_t *res)
{
    jw_t w;
    jw_init(&w, out, cap);
    jw_raw(&w, "{\"api\":1,\"model\":\"lsc-rotatable-2k\",\"app\":");
    jw_str(&w, A2K_APP_VERSION);
    jw_raw(&w, ",\"app_uptime_s\":");
    jw_long(&w, (long)((mono_ms() - g_start_ms) / 1000u));
    jw_raw(&w, ",\"clock_set\":");
    jw_bool(&w, clock_set());
    jw_raw(&w, ",\"time\":");
    jw_long(&w, clock_set() ? (long)wall_s() : 0);
    jw_raw(&w, ",\"key_id\":");
    jw_str(&w, keys->k[key_idx].id);
    jw_raw(&w, ",\"scope\":");
    jw_str(&w, akey_scope_name(keys->k[key_idx].scope));
    jw_raw(&w, ",\"keys\":");
    jw_long(&w, (long)keys->n);
    jw_raw(&w, ",\"events_seq\":");
    jw_long(&w, (long)a2k_apievt_seq());
    jw_raw(&w, ",\"limits\":{\"rate_per_s\":");
    jw_long(&w, APISIG_RATE_MILLI_DEFAULT);
    jw_raw(&w, ",\"rate_unit\":\"milli\",\"burst\":");
    jw_long(&w, APISIG_BURST_DEFAULT);
    jw_raw(&w, ",\"body_max\":");
    jw_long(&w, HREQ_BODY_MAX);
    jw_raw(&w, ",\"event_wait_max_s\":");
    jw_long(&w, A2K_API_EVENT_WAIT_MAX);
    jw_raw(&w, "}}");
    finish(&w, res, 200);
}

static void h_schema(char *out, size_t cap, a2k_api_out_t *res)
{
    int n = streg_json_schema(a2k_settings_reg(), out, cap);
    size_t i, len;
    jw_t w;
    if (n < 0 || out[n - 1] != '}') {
        err_reply(out, cap, res, 500, "too_big", "schema does not fit");
        return;
    }
    len = (size_t)n - 1; /* reopen the object: ,"actions":[...]} */
    jw_init(&w, out + len, cap - len);
    jw_raw(&w, ",\"actions\":[");
    for (i = 0; i < a2k_settings_action_count(); i++) {
        const a2k_action_info_t *a = a2k_settings_action(i);
        if (i)
            jw_raw(&w, ",");
        jw_raw(&w, "{\"name\":");
        jw_str(&w, a->name);
        jw_raw(&w, ",\"scope\":");
        jw_str(&w, akey_scope_name(a->min_scope));
        if (a->args) {
            jw_raw(&w, ",\"args\":");
            jw_str(&w, a->args);
        }
        jw_raw(&w, "}");
    }
    jw_raw(&w, "]}");
    n = jw_end(&w);
    if (n < 0) {
        err_reply(out, cap, res, 500, "too_big", "schema does not fit");
        return;
    }
    res->status = 200;
    res->len = len + (size_t)n;
}

static void h_settings_get(const hreq_t *rq, int state_only, char *out, size_t cap, a2k_api_out_t *res)
{
    static const char *const names[] = { "prefix", NULL };
    char prefix[STREG_KEY_MAX + 1];
    int q = query_get(rq->query, names, "prefix", prefix, sizeof prefix), n;
    if (q < 0) {
        err_reply(out, cap, res, 400, "bad_query", "only prefix=<key prefix> is accepted");
        return;
    }
    n = streg_json_values(a2k_settings_reg(), q == 1 ? prefix : NULL, state_only, out, cap);
    if (n < 0)
        err_reply(out, cap, res, 500, "too_big", "reply does not fit");
    else
        res->status = 200, res->len = (size_t)n;
}

static void h_setting_one(const char *key, char *out, size_t cap, a2k_api_out_t *res)
{
    int n = streg_json_setting(a2k_settings_reg(), key, out, cap);
    if (n == ST_E_UNKNOWN)
        err_reply(out, cap, res, 404, "unknown_key", "no such setting");
    else if (n < 0)
        err_reply(out, cap, res, 500, "getter", "the value cannot be read");
    else
        res->status = 200, res->len = (size_t)n;
}

static const char *jf_err(int rc)
{
    switch (rc) {
    case JF_E_TYPE: return "a value is not a string, integer, true or false";
    case JF_E_LIMIT: return "too many pairs, or a key or value is too long";
    case JF_E_DUP: return "duplicate key";
    case JF_E_STRING: return "a string is not plain ASCII";
    default: return "not a flat JSON object";
    }
}

/* Runs the job on the worker; fills the reply for the transport errors. Returns 1 when the job finished. */
static int run_job(char *out, size_t cap, a2k_api_out_t *res)
{
    int rc = a2k_settings_exec(&g_job);
    if (rc == -2) {
        err_reply(out, cap, res, 503, "busy", "a previous change is still running");
        res->retry_s = 3;
        return 0;
    }
    if (rc == -1) {
        err_reply(out, cap, res, 504, "busy", "the change is still being applied; read /state to see the result");
        return 0;
    }
    return 1;
}

static void h_patch(const a2k_api_in_t *in, int scope, char *out, size_t cap, a2k_api_out_t *res, char *what, size_t wcap)
{
    int rc;
    const char *k[JFLAT_MAX], *v[JFLAT_MAX];
    size_t i;
    if (!a2k_settings_idle()) {
        err_reply(out, cap, res, 503, "busy", "a previous change is still running");
        res->retry_s = 3;
        return;
    }
    memset(&g_job.in, 0, sizeof g_job.in);
    rc = jflat_parse(&g_job.in, in->body ? (const char *)in->body : k_nobody, in->body_len);
    if (rc != JF_OK) {
        err_reply(out, cap, res, 400, "bad_json", jf_err(rc));
        return;
    }
    if (g_job.in.n == 0) {
        err_reply(out, cap, res, 422, "empty", "no setting in the body");
        return;
    }
    g_job.kind = A2K_JOB_PATCH;
    g_job.scope = scope;
    g_job.src = ST_SRC_HTTP;
    g_job.name = NULL;
    if (!run_job(out, cap, res))
        return;
    for (i = 0; i < g_job.in.n; i++) {
        k[i] = g_job.in.key[i];
        v[i] = g_job.in.val[i];
        if (g_job.res[i].ok && strlen(what) + strlen(k[i]) + 2 < wcap) {
            if (what[0])
                strcat(what, ",");
            strcat(what, k[i]);
        }
    }
    rc = streg_json_patch_reply(a2k_settings_reg(), g_job.in.n, k, v, g_job.res, out, cap);
    if (rc < 0)
        err_reply(out, cap, res, 500, "too_big", "reply does not fit");
    else
        res->status = g_job.http, res->len = (size_t)rc;
}

static void h_action(const a2k_api_in_t *in, const char *name, int scope, char *out, size_t cap, a2k_api_out_t *res)
{
    int idx = a2k_settings_action_find(name), rc;
    if (idx < 0) {
        err_reply(out, cap, res, 404, "unknown_action", "no such action");
        return;
    }
    if (a2k_settings_action(idx)->min_scope > scope) {
        err_reply(out, cap, res, 403, "scope", "this key may not call that action");
        return;
    }
    if (!a2k_settings_idle()) {
        err_reply(out, cap, res, 503, "busy", "a previous change is still running");
        res->retry_s = 3;
        return;
    }
    memset(&g_job.in, 0, sizeof g_job.in);
    if (in->body_len > 0) {
        rc = jflat_parse(&g_job.in, (const char *)in->body, in->body_len);
        if (rc != JF_OK) {
            err_reply(out, cap, res, 400, "bad_json", jf_err(rc));
            return;
        }
    }
    g_job.kind = A2K_JOB_ACTION;
    g_job.scope = scope;
    g_job.src = ST_SRC_HTTP;
    g_job.name = a2k_settings_action(idx)->name;
    if (!run_job(out, cap, res))
        return;
    if (g_job.err)
        err_reply(out, cap, res, g_job.http, g_job.err, g_job.detail);
    else {
        ok_reply(out, cap, res, g_job.detail);
        res->status = g_job.http;
    }
}

static void h_events(const hreq_t *rq, char *out, size_t cap, a2k_api_out_t *res)
{
    static const char *const names[] = { "since", "timeout", NULL };
    char sv[16], tv[16];
    int qs = query_get(rq->query, names, "since", sv, sizeof sv), qt = query_get(rq->query, names, "timeout", tv, sizeof tv);
    uint32_t since = 0, to = 0;
    int n, count = 0;
    if (qs < 0 || qt < 0 || (qs == 1 && parse_u32(sv, &since, 0xffffffffu) != 0) ||
        (qt == 1 && parse_u32(tv, &to, A2K_API_EVENT_WAIT_MAX) != 0)) {
        err_reply(out, cap, res, 400, "bad_query", "since=<seq> and timeout=0..25 are accepted");
        return;
    }
    if (qs == 0)
        since = a2k_apievt_seq(); /* no cursor: report the current one, no events */
    n = a2k_apievt_json(since, out, cap, &count);
    if (n < 0) {
        err_reply(out, cap, res, 500, "too_big", "events do not fit");
        return;
    }
    res->status = 200;
    res->len = (size_t)n;
    if (count == 0 && to > 0) {
        res->status = 0;
        res->wait = 1;
        res->since = since;
        res->wait_s = to;
    }
}

int a2k_api_events_reply(uint32_t since, char *out, size_t cap)
{
    int count = 0, n = a2k_apievt_json(since, out, cap, &count);
    return n < 0 ? 0 : n;
}

static void h_audit(char *out, size_t cap, a2k_api_out_t *res)
{
    jw_t w;
    unsigned i, total = g_audit_n < A2K_API_AUDIT_RING ? g_audit_n : A2K_API_AUDIT_RING, take = 0;
    size_t used = 32; /* {"audit":[ ... ]} and the NUL */
    /* 64 lines of about 200 bytes do not fit one reply: the newest ones that do, oldest first */
    for (i = 0; i < total; i++) {
        size_t l = strlen(g_audit[(g_audit_n - 1 - i) % A2K_API_AUDIT_RING].line) + 1;
        if (used + l > cap)
            break;
        used += l;
        take++;
    }
    jw_init(&w, out, cap);
    jw_raw(&w, "{\"audit\":[");
    for (i = 0; i < take; i++) {
        if (i)
            jw_raw(&w, ",");
        jw_raw(&w, g_audit[(g_audit_n - take + i) % A2K_API_AUDIT_RING].line); /* built by audit_add: JSON of plain ASCII */
    }
    jw_raw(&w, "]}");
    if (jw_end(&w) < 0 || w.err)
        err_reply(out, cap, res, 500, "too_big", "audit does not fit");
    else
        res->status = 200, res->len = w.len;
}

static void h_keys_add(const a2k_api_in_t *in, char *out, size_t cap, a2k_api_out_t *res, char *what, size_t wcap)
{
    jflat_t j;
    const char *name = NULL, *scope = NULL;
    uint8_t rnd[36];
    akey_t k;
    int rc, tries, sc;
    size_t i;
    char hex[65];
    jw_t w;
    memset(&j, 0, sizeof j);
    rc = jflat_parse(&j, in->body ? (const char *)in->body : k_nobody, in->body_len);
    if (rc != JF_OK) {
        err_reply(out, cap, res, 400, "bad_json", jf_err(rc));
        return;
    }
    for (i = 0; i < j.n; i++) {
        if (!strcmp(j.key[i], "name")) name = j.val[i];
        else if (!strcmp(j.key[i], "scope")) scope = j.val[i];
        else {
            err_reply(out, cap, res, 422, "args", "only name and scope are accepted");
            return;
        }
    }
    sc = scope ? akey_scope_parse(scope) : 0;
    if (!name || !akey_label_ok(name) || !sc) {
        err_reply(out, cap, res, 422, "args", "name is 1..24 of A-Za-z0-9 ._- and scope is read, write or admin");
        return;
    }
    for (tries = 0; tries < 4; tries++) {
        if (rng(rnd, sizeof rnd) != 0) {
            err_reply(out, cap, res, 503, "entropy", "the random pool is not ready");
            return;
        }
        if (akey_make(&k, sc, name, clock_set() ? wall_s() : 0, rnd) != 0) {
            err_reply(out, cap, res, 500, "make", "key creation failed");
            return;
        }
        rc = akeys_add_save(&g_keys, &k, keys_writer, NULL);
        if (rc == AKEYS_E_DUP && akeys_find_name(&g_keys, name) < 0)
            continue; /* the random id collided: new bytes */
        break;
    }
    akam_wipe(rnd, sizeof rnd);
    if (rc == AKEYS_E_DUP) {
        err_reply(out, cap, res, 409, "duplicate", "a key with that name exists");
    } else if (rc == AKEYS_E_TOOBIG) {
        err_reply(out, cap, res, 409, "full", "the key table is full (8 keys)");
    } else if (rc != AKEYS_OK) {
        err_reply(out, cap, res, 500, "store", "the key file could not be written");
    } else {
        akam_hex(k.secret, 32, hex);
        jw_init(&w, out, cap);
        jw_raw(&w, "{\"ok\":true,\"id\":");
        jw_str(&w, k.id);
        jw_raw(&w, ",\"name\":");
        jw_str(&w, k.label);
        jw_raw(&w, ",\"scope\":");
        jw_str(&w, akey_scope_name(k.scope));
        jw_raw(&w, ",\"secret\":");
        jw_str(&w, hex);
        jw_raw(&w, ",\"note\":\"the secret is shown once\"}");
        finish(&w, res, 201);
        snprintf(what, wcap, "new key %s %s", k.id, akey_scope_name(k.scope));
        keys_changed();
        akam_wipe(hex, sizeof hex);
    }
    akam_wipe(&k, sizeof k);
}

static void h_keys_del(const char *id, char *out, size_t cap, a2k_api_out_t *res, char *what, size_t wcap)
{
    int rc;
    if (!akey_id_ok(id)) {
        err_reply(out, cap, res, 400, "bad_id", "a key id is 8 lower-case hex digits");
        return;
    }
    rc = akeys_remove_save(&g_keys, id, 0, keys_writer, NULL);
    if (rc == AKEYS_OK) {
        apisig_drop_key(&g_sig, id);
        keys_changed();
        snprintf(what, wcap, "revoked %s", id);
        ok_reply(out, cap, res, NULL);
    } else if (rc == AKEYS_E_LAST_ADMIN) {
        err_reply(out, cap, res, 409, "last_admin", "the last admin key cannot be revoked");
    } else if (rc == AKEYS_E_NOTFOUND) {
        err_reply(out, cap, res, 404, "unknown_key", "no such key");
    } else {
        err_reply(out, cap, res, 500, "store", "the key file could not be written");
    }
}

/* ------------------------------------------------------------------ the entry point */
static int64_t parse_num_skew(const apisig_result_t *r) { return r->skew_s; }

static void auth_fail_reply(const apisig_result_t *r, char *out, size_t cap, a2k_api_out_t *res)
{
    jw_t w;
    if (r->http == 401 && r->error && !strcmp(r->error, "clock_skew")) {
        jw_init(&w, out, cap);
        jw_raw(&w, "{\"error\":\"clock_skew\",\"detail\":\"the request timestamp is outside the window\",\"camera_time\":");
        jw_long(&w, clock_set() ? (long)wall_s() : 0);
        jw_raw(&w, ",\"skew_s\":");
        jw_long(&w, (long)parse_num_skew(r));
        jw_raw(&w, "}");
        finish(&w, res, 401);
        return;
    }
    err_reply(out, cap, res, r->http, r->error ? r->error : "denied", r->error ? r->error : "denied");
}

void a2k_api_handle(const a2k_api_in_t *in, char *out, size_t cap, a2k_api_out_t *res)
{
    const hreq_t *rq = in->rq;
    const char *sub;
    char what[96] = { 0 };
    int rt, need, scope = 0, write;
    uint64_t now = mono_ms();
    apisig_req_t sr;
    apisig_result_t ar;
    unsigned retry;
    memset(res, 0, sizeof *res);

    if (rq->expect) { err_reply(out, cap, res, 417, "expect", "Expect is not supported"); return; }
    if (rq->chunked) { err_reply(out, cap, res, 411, "length_required", "chunked bodies are not accepted"); return; }
    if (strncmp(rq->path, A2K_API_PREFIX, strlen(A2K_API_PREFIX)) != 0 || rq->path[strlen(A2K_API_PREFIX)] != '/') {
        err_reply(out, cap, res, 404, "unknown_path", "no such path");
        return;
    }
    sub = rq->path + strlen(A2K_API_PREFIX);
    rt = route(rq->method, sub, &need);
    if (rt == -404) { err_reply(out, cap, res, 404, "unknown_path", "no such path"); return; }
    if (rt == -405) { err_reply(out, cap, res, 405, "method", "method not allowed on this path"); return; }
    if (!apisig_path_ok(rq->path) || !apisig_method_ok(rq->method)) {
        err_reply(out, cap, res, 400, "bad_path", "the path is not canonical");
        return;
    }
    if ((rt == R_PATCH || rt == R_KEYS_ADD) && rq->content_length < 0) {
        err_reply(out, cap, res, 411, "length_required", "Content-Length is required");
        return;
    }
    if (g_keys.n == 0) {
        err_reply(out, cap, res, 503, "api_closed", "no API key is installed");
        return;
    }

    if (rt == R_HELLO) { /* unsigned */
        uint8_t rnd[16];
        char hex[33];
        retry = alim_hello(&g_lim, in->peer_ip, now);
        if (retry) {
            err_reply(out, cap, res, 429, "rate", "too many hello requests");
            res->retry_s = retry;
            return;
        }
        if (rng(rnd, sizeof rnd) != 0) { err_reply(out, cap, res, 503, "entropy", "the random pool is not ready"); return; }
        if (apisig_hello(&g_sig, now, rnd, hex) != 0) {
            err_reply(out, cap, res, 429, "busy", "too many challenges pending");
            res->retry_s = 5;
            return;
        }
        {
            int n = apisig_hello_json(out, cap, clock_set(), hex);
            if (n < 0) err_reply(out, cap, res, 500, "too_big", "reply does not fit");
            else res->status = 200, res->len = (size_t)n;
        }
        return;
    }

    /* signed endpoints, cheap checks first (spec 5.3) */
    retry = alim_peer_check(&g_lim, in->peer_ip, now);
    if (retry) {
        err_reply(out, cap, res, 429, "locked", "too many failed attempts");
        res->retry_s = retry;
        return;
    }
    if (!alim_accept_peek(&g_lim, now)) {
        err_reply(out, cap, res, 429, "rate", "request rate limit");
        res->retry_s = alim_accept_retry_s(&g_lim);
        return;
    }
    memset(&sr, 0, sizeof sr);
    sr.method = rq->method;
    sr.path = rq->path;
    sr.query = rq->query;
    sr.body = in->body;
    sr.body_len = in->body_len;
    sr.h_key = rq->key;
    sr.h_timestamp = rq->timestamp;
    sr.h_nonce = rq->nonce;
    sr.h_challenge = rq->challenge;
    sr.h_signature = rq->signature;
    if (apisig_verify(&g_sig, &g_keys, clock_set(), clock_set() ? wall_s() : 0, now, &sr, need, &ar) != 0) {
        if (ar.http == 403) { /* a valid signature without the scope: the peer is genuine, the nonce is spent */
            alim_peer_ok(&g_lim, in->peer_ip, now);
            alim_accept_take(&g_lim, now);
        } else if (ar.http != 429) {
            alim_peer_fail(&g_lim, in->peer_ip, now);
        }
        auth_fail_reply(&ar, out, cap, res);
        if (ar.http == 429)
            res->retry_s = 5;
        audit_add(in, res->status, rq->key, ar.scope, ar.error, 0);
        return;
    }
    alim_peer_ok(&g_lim, in->peer_ip, now);
    if (!alim_accept_take(&g_lim, now)) {
        err_reply(out, cap, res, 429, "rate", "request rate limit");
        res->retry_s = alim_accept_retry_s(&g_lim);
        return;
    }
    scope = ar.scope;
    write = rt == R_PATCH || rt == R_ACTION || rt == R_KEYS_ADD || rt == R_KEYS_DEL;

    switch (rt) {
    case R_INFO: h_info(&g_keys, ar.key_idx, out, cap, res); break;
    case R_SCHEMA: h_schema(out, cap, res); break;
    case R_SETTINGS: h_settings_get(rq, 0, out, cap, res); break;
    case R_STATE: h_settings_get(rq, 1, out, cap, res); break;
    case R_SETTING_ONE: h_setting_one(sub + 10, out, cap, res); break;
    case R_EVENTS: h_events(rq, out, cap, res); break;
    case R_AUDIT: h_audit(out, cap, res); break;
    case R_KEYS_LIST: {
        int n = akeys_json_list(&g_keys, out, cap);
        if (n < 0) err_reply(out, cap, res, 500, "too_big", "reply does not fit");
        else res->status = 200, res->len = (size_t)n;
        break;
    }
    case R_PATCH: h_patch(in, scope, out, cap, res, what, sizeof what); break;
    case R_ACTION:
        snprintf(what, sizeof what, "%.60s", sub + 9);
        h_action(in, sub + 9, scope, out, cap, res);
        break;
    case R_KEYS_ADD: h_keys_add(in, out, cap, res, what, sizeof what); break;
    case R_KEYS_DEL: h_keys_del(sub + 6, out, cap, res, what, sizeof what); break;
    default: err_reply(out, cap, res, 404, "unknown_path", "no such path"); break;
    }
    if (write || res->status >= 400)
        audit_add(in, res->status, rq->key, scope, what[0] ? what : NULL, write);
}

/* ------------------------------------------------------------------ on-camera key tool */
static int cli_usage(void)
{
    fprintf(stderr, "usage: akam_api_key new --scope read|write|admin --name LABEL | list | revoke ID|NAME [--force]\n");
    return 2;
}

int a2k_api_key_cli(const a2k_api_env_t *env, int argc, char **argv)
{
    char buf[AKEY_FILE_MAX + 1], text[AKEY_FILE_MAX + 64];
    akeys_t keys;
    int n, rc;
    size_t i;
    memset(&g_env, 0, sizeof g_env);
    if (env)
        g_env = *env;
    snprintf(g_keys_path, sizeof g_keys_path, "%s", g_env.keys_path ? g_env.keys_path : A2K_API_KEYS);
    if (argc < 1)
        return cli_usage();
    n = a2k_store_read(g_keys_path, buf, sizeof buf);
    if (n < 0)
        n = 0, buf[0] = '\0'; /* no file yet: no key */
    if (akeys_parse(&keys, buf, (size_t)n, NULL) != AKEYS_OK) {
        fprintf(stderr, "akam_api_key: %s is invalid; it is left untouched\n", g_keys_path);
        return 1;
    }
    if (!strcmp(argv[0], "list")) {
        rc = akeys_json_list(&keys, text, sizeof text);
        if (rc < 0)
            return 1;
        printf("%s\n", text);
        return 0;
    }
    if (!strcmp(argv[0], "revoke")) {
        int force = 0;
        if (argc < 2 || argc > 3 || (argc == 3 && strcmp(argv[2], "--force") != 0))
            return cli_usage();
        force = argc == 3;
        rc = akeys_remove_save(&keys, argv[1], force, keys_writer, NULL);
        if (rc == AKEYS_E_LAST_ADMIN)
            fprintf(stderr, "akam_api_key: refused, that is the last admin key (use --force)\n");
        else if (rc == AKEYS_E_NOTFOUND)
            fprintf(stderr, "akam_api_key: no such key\n");
        else if (rc != AKEYS_OK)
            fprintf(stderr, "akam_api_key: the key file could not be written\n");
        else
            printf("revoked %s\n", argv[1]);
        return rc == AKEYS_OK ? 0 : 1;
    }
    if (!strcmp(argv[0], "new")) {
        const char *name = NULL, *scope = NULL;
        uint8_t rnd[36];
        akey_t k;
        char hex[65];
        int sc, tries;
        for (i = 1; i + 1 < (size_t)argc; i += 2) {
            if (!strcmp(argv[i], "--name")) name = argv[i + 1];
            else if (!strcmp(argv[i], "--scope")) scope = argv[i + 1];
            else return cli_usage();
        }
        if (!name || !scope || (argc - 1) % 2 != 0)
            return cli_usage();
        sc = akey_scope_parse(scope);
        if (!sc || !akey_label_ok(name)) {
            fprintf(stderr, "akam_api_key: bad --scope or --name (1..24 of A-Za-z0-9 ._-)\n");
            return 2;
        }
        for (tries = 0; tries < 4; tries++) {
            if (rng(rnd, sizeof rnd) != 0) {
                fprintf(stderr, "akam_api_key: the random pool is not ready, no key made\n");
                return 4;
            }
            if (akey_make(&k, sc, name, wall_s() >= A2K_API_CLOCK_FLOOR ? wall_s() : 0, rnd) != 0)
                return 1;
            rc = akeys_add_save(&keys, &k, keys_writer, NULL);
            if (rc != AKEYS_E_DUP || akeys_find_name(&keys, name) >= 0)
                break;
        }
        akam_wipe(rnd, sizeof rnd);
        if (rc == AKEYS_E_DUP)
            fprintf(stderr, "akam_api_key: a key named %s exists\n", name);
        else if (rc == AKEYS_E_TOOBIG)
            fprintf(stderr, "akam_api_key: the key table is full (8 keys)\n");
        else if (rc != AKEYS_OK)
            fprintf(stderr, "akam_api_key: the key file could not be written\n");
        if (rc != AKEYS_OK) {
            akam_wipe(&k, sizeof k);
            return 1;
        }
        akam_hex(k.secret, 32, hex);
        printf("key_id:   %s\nsecret:   %s\nscope:    %s\nname:     %s\n", k.id, hex, akey_scope_name(k.scope), k.label);
        printf("# the secret is shown ONCE; the API (port %d) picks the key up within seconds\n", A2K_API_PORT);
        akam_wipe(hex, sizeof hex);
        akam_wipe(&k, sizeof k);
        return 0;
    }
    return cli_usage();
}
