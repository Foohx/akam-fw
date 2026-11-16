#include "settings_reg.h"

static const char k_nothing[1] = {0}; /* an empty C string without the "" literal (repo C scan) */

#include <stdio.h>
#include <string.h>

#include "api_keys.h"
#include "json_min.h"

/* ---- strict parsers ---- */

/* Canonical decimal at *pp: optional '-', no '+', no leading zero, no "-0". Stops at the first non-digit. */
static int pint(const char **pp, long long *v) {
    const char *p = *pp;
    int neg = 0, digits = 0;
    long long x = 0;
    if (*p == '-') { neg = 1; p++; }
    if (*p < '0' || *p > '9') return ST_E_FORMAT;
    if (*p == '0' && (p[1] >= '0' && p[1] <= '9')) return ST_E_FORMAT;
    if (neg && *p == '0') return ST_E_FORMAT;
    while (*p >= '0' && *p <= '9') {
        if (++digits > 18) return ST_E_RANGE; /* would overflow */
        x = x * 10 + (*p - '0');
        p++;
    }
    *pp = p;
    *v = neg ? -x : x;
    return ST_OK;
}

static int whole_int(const char *s, long long lo, long long hi, long long *out) {
    long long v = 0;
    int r = pint(&s, &v);
    if (r) return r;
    if (*s) return ST_E_FORMAT;
    if (v < lo || v > hi) return ST_E_RANGE;
    if (out) *out = v;
    return ST_OK;
}

/* k colon-separated ints each within lo..hi into v[]. */
static int ints(const char *s, int k, long long lo, long long hi, long long *v) {
    int i;
    for (i = 0; i < k; i++) {
        int r = pint(&s, &v[i]);
        if (r) return r;
        if (i < k - 1) {
            if (*s != ':') return ST_E_FORMAT;
            s++;
        }
    }
    if (*s) return ST_E_FORMAT;
    for (i = 0; i < k; i++)
        if (v[i] < lo || v[i] > hi) return ST_E_RANGE;
    return ST_OK;
}

static int pattern_ok(const st_def_t *d, const char *s) {
    const char *g = d->opts ? d->opts[0] : NULL;
    long long v[4];
    int r;
    if (!g) return ST_E_FORMAT;
    if (!strcmp(g, "led")) { /* on | off | N:on_ms:off_ms:pause_ms */
        static const long long lo[4] = {1, 10, 10, 0}, hi[4] = {255, 10000, 10000, 60000};
        int i;
        if (!strcmp(s, "on") || !strcmp(s, "off")) return ST_OK;
        r = ints(s, 4, -2147483647LL, 2147483647LL, v);
        if (r) return r;
        for (i = 0; i < 4; i++)
            if (v[i] < lo[i] || v[i] > hi[i]) return ST_E_RANGE;
        return ST_OK;
    }
    if (!strcmp(g, "pan_tilt")) return ints(s, 2, d->lo, d->hi, v);
    if (!strcmp(g, "zone4")) { /* PMIN:PMAX:TMIN:TMAX */
        r = ints(s, 4, d->lo, d->hi, v);
        if (r) return r;
        return (v[0] <= v[1] && v[2] <= v[3]) ? ST_OK : ST_E_RANGE;
    }
    return ST_E_FORMAT;
}

int streg_validate(const st_def_t *d, const char *val) {
    size_t n, i;
    if (!d || !val) return ST_E_FORMAT;
    switch (d->type) {
    case ST_BOOL:
        return (val[0] == '0' || val[0] == '1') && val[1] == 0 ? ST_OK : ST_E_FORMAT;
    case ST_INT:
        return whole_int(val, d->lo, d->hi, NULL);
    case ST_ENUM:
        if (!d->opts) return ST_E_FORMAT;
        for (i = 0; d->opts[i]; i++)
            if (!strcmp(d->opts[i], val)) return ST_OK;
        return ST_E_RANGE;
    case ST_STRING:
        n = strlen(val);
        for (i = 0; i < n; i++)
            if ((unsigned char)val[i] < 0x20 || (unsigned char)val[i] > 0x7e) return ST_E_FORMAT;
        return ((long)n < d->lo || (long)n > d->hi) ? ST_E_RANGE : ST_OK;
    case ST_PATTERN:
        return pattern_ok(d, val);
    }
    return ST_E_FORMAT;
}

/* ---- table ---- */

static int key_ok(const char *k) {
    size_t i, n;
    if (!k) return 0;
    n = strlen(k);
    if (n == 0 || n > STREG_KEY_MAX) return 0;
    for (i = 0; i < n; i++)
        if (!((k[i] >= 'a' && k[i] <= 'z') || (k[i] >= '0' && k[i] <= '9') || k[i] == '_' || k[i] == '.')) return 0;
    return 1;
}

static int avail(const streg_t *r, const st_def_t *d) {
    return (d->model_mask == 0 || (d->model_mask & r->model)) && (d->cap_req & ~r->caps) == 0;
}

static int visible(const st_def_t *d, st_src_t src) {
    if (src == ST_SRC_HTTP) return (d->expose & ST_EXPOSE_HTTP) != 0;
    if (src == ST_SRC_MQTT) return (d->expose & ST_EXPOSE_MQTT) != 0;
    return 1;
}

static int row_ok(const st_def_t *d) {
    size_t i;
    if (!key_ok(d->key) || d->apply > AP_REBOOT || !(d->access & (ST_ACC_READ | ST_ACC_WRITE))) return 0;
    if ((d->access & ST_ACC_WRITE) && !d->set) return 0;
    if (d->min_scope > AKEY_ADMIN) return 0;
    switch (d->type) {
    case ST_INT:
        if (d->lo > d->hi) return 0;
        break;
    case ST_STRING:
        if (d->lo < 0 || d->hi < d->lo || d->hi > STREG_VAL_MAX - 1) return 0;
        break;
    case ST_ENUM:
        if (!d->opts || !d->opts[0]) return 0;
        for (i = 0; d->opts[i]; i++) {
            const char *o = d->opts[i];
            size_t j;
            if (i >= STREG_ENUM_MAX || !*o || strlen(o) >= STREG_VAL_MAX) return 0;
            for (j = 0; o[j]; j++)
                if ((unsigned char)o[j] <= 0x20 || (unsigned char)o[j] > 0x7e || o[j] == '"' || o[j] == '\\') return 0;
        }
        break;
    case ST_PATTERN:
        if (!d->opts || !d->opts[0] ||
            !(!strcmp(d->opts[0], "led") || !strcmp(d->opts[0], "pan_tilt") || !strcmp(d->opts[0], "zone4")))
            return 0;
        if (strcmp(d->opts[0], "led") && d->lo > d->hi) return 0;
        break;
    case ST_BOOL:
        break;
    default:
        return 0;
    }
    if (d->secret) {
        /* write-only, never listed, never persisted by the registry, no way to read it back */
        if (d->access != ST_ACC_WRITE || d->expose & ST_EXPOSE_HTTP || d->persist || d->get) return 0;
        return 1;
    }
    return d->def && streg_validate(d, d->def) == ST_OK;
}

int streg_init(streg_t *r, const st_def_t *defs, size_t n, uint32_t model_bit, uint32_t caps, size_t *bad) {
    size_t i, j;
    memset(r, 0, sizeof *r);
    if (bad) *bad = 0;
    if (!defs || n == 0 || n > STREG_MAX_DEFS) return ST_E_TABLE;
    for (i = 0; i < n; i++) {
        int dup = 0;
        for (j = 0; j < i && !dup; j++) dup = defs[j].key && defs[i].key && !strcmp(defs[j].key, defs[i].key);
        if (dup || !row_ok(&defs[i])) {
            if (bad) *bad = i;
            return ST_E_TABLE;
        }
    }
    r->defs = defs;
    r->n = n;
    r->model = model_bit;
    r->caps = caps;
    return ST_OK;
}

const st_def_t *streg_find(const streg_t *r, const char *key) {
    size_t i;
    if (!r || !key) return NULL;
    for (i = 0; i < r->n; i++)
        if (!strcmp(r->defs[i].key, key)) return avail(r, &r->defs[i]) ? &r->defs[i] : NULL;
    return NULL;
}

/* ---- reading ---- */

static int read_value(const st_def_t *d, char *out, size_t cap) {
    int n;
    if (!d->get) {
        size_t l = strlen(d->def);
        if (l + 1 > cap) return ST_E_TOOBIG;
        memcpy(out, d->def, l + 1);
        return (int)l;
    }
    n = d->get(d->key, out, cap);
    if (n < 0 || (size_t)n >= cap) return ST_E_GETTER;
    out[n] = 0;
    return n;
}

int streg_get(const streg_t *r, st_src_t src, const char *key, char *out, size_t cap) {
    const st_def_t *d = streg_find(r, key);
    if (!d || !visible(d, src)) return ST_E_UNKNOWN;
    if (d->secret) return ST_E_SECRET;
    if (!out || cap == 0) return ST_E_TOOBIG;
    return read_value(d, out, cap);
}

int streg_peek(const streg_t *r, const char *key, char *out, size_t cap) {
    size_t i;
    for (i = 0; i < r->pend_n; i++)
        if (!strcmp(r->pend_k[i], key)) {
            size_t l = strlen(r->pend_v[i]);
            if (l + 1 > cap) return ST_E_TOOBIG;
            memcpy(out, r->pend_v[i], l + 1);
            return (int)l;
        }
    return streg_get(r, ST_SRC_LOCAL, key, out, cap);
}

/* ---- writing ---- */

int streg_check_one(const streg_t *r, st_src_t src, int scope, const char *key, const char *val) {
    const st_def_t *d = streg_find(r, key);
    int need;
    if (!d || !visible(d, src)) return ST_E_UNKNOWN;
    if (d->secret && src == ST_SRC_HTTP) return ST_E_SECRET;
    if (!(d->access & ST_ACC_WRITE)) return ST_E_READONLY;
    need = d->min_scope ? d->min_scope : AKEY_WRITE;
    if (scope < need) return ST_E_SCOPE;
    return streg_validate(d, val);
}

#define PATCH_MAX 64

int streg_patch(streg_t *r, st_src_t src, int scope, size_t n, const char *const *keys, const char *const *vals,
                st_result_t *res) {
    const st_def_t *dp[PATCH_MAX];
    size_t i, j, k, okn = 0, any_err = 0;
    if (n > PATCH_MAX) {
        for (i = 0; i < n; i++) { res[i].ok = 0; res[i].err = ST_E_TOOBIG; res[i].apply = AP_LIVE; }
        return 422;
    }
    if (n == 0) return 422;
    for (i = 0; i < n; i++) {
        res[i].ok = 1; res[i].err = 0; res[i].apply = AP_LIVE;
        dp[i] = NULL;
        if (!keys[i] || !vals[i]) { res[i].ok = 0; res[i].err = ST_E_FORMAT; any_err = 1; continue; }
        res[i].err = streg_check_one(r, src, scope, keys[i], vals[i]);
        if (res[i].err == ST_OK) {
            for (j = 0; j < i; j++)
                if (keys[j] && !strcmp(keys[j], keys[i])) res[i].err = ST_E_DUP;
        }
        if (res[i].err) { res[i].ok = 0; any_err = 1; }
        else dp[i] = streg_find(r, keys[i]);
    }
    r->pend_k = keys; r->pend_v = vals; r->pend_n = n;
    for (i = 0; i < n; i++) {
        if (!res[i].ok || !dp[i]->check) continue;
        if (dp[i]->check(r, keys[i], vals[i]) != 0) { res[i].ok = 0; res[i].err = ST_E_CHECK; any_err = 1; }
    }
    r->pend_k = NULL; r->pend_v = NULL; r->pend_n = 0;
    if (any_err) {
        for (i = 0; i < n; i++)
            if (res[i].ok) { res[i].ok = 0; res[i].err = ST_E_NOTAPPLIED; }
        return 422;
    }
    for (k = 0; k < r->n; k++) { /* registry order */
        for (i = 0; i < n; i++) {
            st_apply_t eff;
            if (dp[i] != &r->defs[k]) continue;
            eff = dp[i]->apply;
            if (dp[i]->set(keys[i], vals[i], &eff) != 0) { res[i].ok = 0; res[i].err = ST_E_SETTER; }
            else { res[i].apply = eff; okn++; }
        }
    }
    return okn == n ? 200 : okn == 0 ? 422 : 207;
}

int streg_set(streg_t *r, st_src_t src, int scope, const char *key, const char *val, st_result_t *res) {
    return streg_patch(r, src, scope, 1, &key, &val, res);
}

const char *streg_err_name(int e) {
    switch (e) {
    case ST_OK: return "ok";
    case ST_E_UNKNOWN: return "unknown_key";
    case ST_E_FORMAT: return "format";
    case ST_E_RANGE: return "range";
    case ST_E_READONLY: return "readonly";
    case ST_E_SECRET: return "secret";
    case ST_E_SCOPE: return "scope";
    case ST_E_CHECK: return "check";
    case ST_E_SETTER: return "setter";
    case ST_E_DUP: return "duplicate";
    case ST_E_NOTAPPLIED: return "not_applied";
    case ST_E_TOOBIG: return "too_big";
    case ST_E_GETTER: return "getter";
    default: return "error";
    }
}

const char *streg_apply_name(st_apply_t a) {
    return a == AP_LIVE ? "applied" : a == AP_RESTART_COMPONENT ? "restart_component" : "pending_device_restart";
}

/* ---- persistence text ---- */

static int persistent(const streg_t *r, const st_def_t *d) {
    return avail(r, d) && d->persist && !d->secret && (d->access & ST_ACC_WRITE);
}

int streg_save_text(const streg_t *r, char *out, size_t cap) {
    size_t i, len = 0;
    if (!out || cap == 0) return ST_E_TOOBIG;
    out[0] = 0;
    for (i = 0; i < r->n; i++) {
        const st_def_t *d = &r->defs[i];
        char v[STREG_VAL_MAX];
        int n;
        if (!persistent(r, d)) continue;
        if (read_value(d, v, sizeof v) < 0 || !strcmp(v, d->def)) continue;
        n = snprintf(out + len, cap - len, "%s=%s\n", d->key, v);
        if (n < 0 || (size_t)n >= cap - len || len + (size_t)n > STREG_FILE_MAX) return ST_E_TOOBIG;
        len += (size_t)n;
    }
    return (int)len;
}

int streg_load_text(streg_t *r, const char *text, size_t len, size_t *err_line, size_t *ignored, size_t *failed) {
    char pool[STREG_FILE_MAX + STREG_MAX_DEFS * 2];
    const char *ks[STREG_MAX_DEFS], *vs[STREG_MAX_DEFS];
    const st_def_t *dp[STREG_MAX_DEFS];
    size_t pos = 0, line = 0, used = 0, n = 0, ign = 0, fail = 0, i, k;
    if (err_line) *err_line = 0;
    if (ignored) *ignored = 0;
    if (failed) *failed = 0;
    if (!r || (!text && len)) return ST_E_FORMAT;
    if (len > STREG_FILE_MAX) return ST_E_TOOBIG;
    while (pos < len) {
        size_t e = pos, kl, vl;
        const char *eq;
        const st_def_t *d;
        int rc;
        while (e < len && text[e] != '\n') e++;
        line++;
        eq = memchr(text + pos, '=', e - pos);
        if (e == pos || !eq) goto bad;
        kl = (size_t)(eq - (text + pos));
        vl = e - pos - kl - 1;
        if (kl == 0 || kl > STREG_KEY_MAX || vl >= STREG_VAL_MAX || memchr(text + pos, 0, e - pos)) goto bad;
        {
            char key[STREG_KEY_MAX + 1];
            memcpy(key, text + pos, kl);
            key[kl] = 0;
            d = streg_find(r, key);
            if (!d || !persistent(r, d)) { ign++; pos = e + 1; continue; }
            if (n >= STREG_MAX_DEFS) goto bad;
            for (i = 0; i < n; i++)
                if (dp[i] == d) goto bad; /* duplicate */
            ks[n] = d->key; /* the table's own string */
            memcpy(pool + used, eq + 1, vl);
            pool[used + vl] = 0;
            vs[n] = pool + used;
            used += vl + 1;
            dp[n] = d;
        }
        rc = streg_validate(d, vs[n]);
        if (rc) goto bad;
        n++;
        pos = e + 1;
    }
    r->pend_k = ks; r->pend_v = vs; r->pend_n = n;
    for (i = 0; i < n; i++)
        if (dp[i]->check && dp[i]->check(r, ks[i], vs[i]) != 0) {
            r->pend_k = NULL; r->pend_v = NULL; r->pend_n = 0;
            line = 0; /* a cross-field failure has no single line */
            if (err_line) *err_line = 0;
            return ST_E_CHECK;
        }
    r->pend_k = NULL; r->pend_v = NULL; r->pend_n = 0;
    for (k = 0; k < r->n; k++)
        for (i = 0; i < n; i++) {
            st_apply_t eff;
            if (dp[i] != &r->defs[k]) continue;
            eff = dp[i]->apply;
            if (dp[i]->set(ks[i], vs[i], &eff) != 0) fail++;
        }
    if (ignored) *ignored = ign;
    if (failed) *failed = fail;
    return ST_OK;
bad:
    if (err_line) *err_line = line;
    return ST_E_FORMAT;
}

/* ---- JSON ---- */

static const char *type_name(st_type_t t) {
    switch (t) {
    case ST_BOOL: return "bool";
    case ST_INT: return "int";
    case ST_ENUM: return "enum";
    case ST_STRING: return "string";
    default: return "pattern";
    }
}

static int http_listed(const streg_t *r, const st_def_t *d) { return avail(r, d) && !d->secret && visible(d, ST_SRC_HTTP); }

static void meta(jw_t *w, const st_def_t *d) {
    size_t i;
    int need = d->min_scope ? d->min_scope : (d->access & ST_ACC_WRITE) ? AKEY_WRITE : AKEY_READ;
    jw_raw(w, "{\"key\":"); jw_str(w, d->key);
    jw_raw(w, ",\"type\":"); jw_str(w, type_name(d->type));
    if (d->type == ST_INT || d->type == ST_STRING || (d->type == ST_PATTERN && strcmp(d->opts[0], "led"))) {
        jw_raw(w, ",\"lo\":"); jw_long(w, d->lo);
        jw_raw(w, ",\"hi\":"); jw_long(w, d->hi);
    }
    if (d->type == ST_ENUM) {
        jw_raw(w, ",\"opts\":[");
        for (i = 0; d->opts[i]; i++) { if (i) jw_raw(w, ","); jw_str(w, d->opts[i]); }
        jw_raw(w, "]");
    }
    if (d->type == ST_PATTERN) { jw_raw(w, ",\"grammar\":"); jw_str(w, d->opts[0]); }
    jw_raw(w, ",\"default\":"); jw_str(w, d->def);
    jw_raw(w, ",\"apply\":"); jw_str(w, d->apply == AP_LIVE ? "live" : d->apply == AP_RESTART_COMPONENT ? "restart_component" : "device_restart");
    jw_raw(w, ",\"persist\":"); jw_bool(w, d->persist);
    jw_raw(w, ",\"access\":"); jw_str(w, (d->access & ST_ACC_WRITE) ? "rw" : "ro");
    jw_raw(w, ",\"scope\":"); jw_str(w, akey_scope_name(need));
}

static int fin(jw_t *w) { int n = jw_end(w); return n < 0 ? ST_E_TOOBIG : n; }

int streg_json_values(const streg_t *r, const char *prefix, int state_only, char *out, size_t cap) {
    jw_t w;
    size_t i, pl = prefix ? strlen(prefix) : 0;
    int first = 1;
    jw_init(&w, out, cap);
    jw_raw(&w, "{");
    for (i = 0; i < r->n; i++) {
        const st_def_t *d = &r->defs[i];
        char v[STREG_VAL_MAX];
        if (!http_listed(r, d) || ((d->access & ST_ACC_WRITE) != 0) == (state_only != 0)) continue;
        if (strncmp(d->key, prefix ? prefix : k_nothing, pl)) continue;
        if (read_value(d, v, sizeof v) < 0) continue;
        if (!first) jw_raw(&w, ",");
        first = 0;
        jw_str(&w, d->key); jw_raw(&w, ":"); jw_str(&w, v);
    }
    jw_raw(&w, "}");
    return fin(&w);
}

int streg_json_setting(const streg_t *r, const char *key, char *out, size_t cap) {
    const st_def_t *d = streg_find(r, key);
    char v[STREG_VAL_MAX];
    jw_t w;
    if (!d || !http_listed(r, d)) return ST_E_UNKNOWN;
    if (read_value(d, v, sizeof v) < 0) return ST_E_GETTER;
    jw_init(&w, out, cap);
    meta(&w, d);
    jw_raw(&w, ",\"value\":"); jw_str(&w, v);
    jw_raw(&w, "}");
    return fin(&w);
}

int streg_json_schema(const streg_t *r, char *out, size_t cap) {
    jw_t w;
    size_t i;
    int first = 1;
    jw_init(&w, out, cap);
    jw_raw(&w, "{\"api\":1,\"settings\":[");
    for (i = 0; i < r->n; i++) {
        if (!http_listed(r, &r->defs[i])) continue;
        if (!first) jw_raw(&w, ",");
        first = 0;
        meta(&w, &r->defs[i]);
        jw_raw(&w, "}");
    }
    jw_raw(&w, "]}");
    return fin(&w);
}

int streg_json_patch_reply(const streg_t *r, size_t n, const char *const *keys, const char *const *vals,
                           const st_result_t *res, char *out, size_t cap) {
    jw_t w;
    size_t i;
    jw_init(&w, out, cap);
    jw_raw(&w, "{\"results\":{");
    for (i = 0; i < n; i++) {
        if (i) jw_raw(&w, ",");
        jw_str(&w, keys[i] ? keys[i] : k_nothing);
        if (res[i].ok) {
            jw_raw(&w, ":{\"ok\":true,\"apply\":"); jw_str(&w, streg_apply_name(res[i].apply));
            jw_raw(&w, ",\"value\":"); jw_str(&w, vals[i]);
            jw_raw(&w, "}");
        } else {
            const st_def_t *d = keys[i] ? streg_find(r, keys[i]) : NULL;
            jw_raw(&w, ":{\"ok\":false,\"error\":"); jw_str(&w, streg_err_name(res[i].err));
            if (res[i].err == ST_E_RANGE && d) {
                char t[96];
                if (d->type == ST_INT) snprintf(t, sizeof t, "[%ld,%ld]", d->lo, d->hi);
                else if (d->type == ST_STRING) snprintf(t, sizeof t, "length %ld..%ld", d->lo, d->hi);
                else if (d->type == ST_ENUM) snprintf(t, sizeof t, "one of the schema options");
                else snprintf(t, sizeof t, "out of range");
                jw_raw(&w, ",\"detail\":"); jw_str(&w, t);
            }
            jw_raw(&w, "}");
        }
    }
    jw_raw(&w, "}}");
    return fin(&w);
}
