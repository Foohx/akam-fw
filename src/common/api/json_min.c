#include "json_min.h"

static const char k_quote = '"';

#include <stdio.h>
#include <string.h>

void jw_init(jw_t *w, char *buf, size_t cap) {
    w->buf = buf;
    w->cap = cap;
    w->len = 0;
    w->err = cap == 0;
    if (cap) buf[0] = 0;
}

static void put(jw_t *w, const char *s, size_t n) {
    if (w->err) return;
    if (w->len + n + 1 > w->cap) { w->err = 1; return; }
    memcpy(w->buf + w->len, s, n);
    w->len += n;
    w->buf[w->len] = 0;
}

void jw_raw(jw_t *w, const char *s) { put(w, s, strlen(s)); }

void jw_str(jw_t *w, const char *s) {
    static const char H[] = "0123456789abcdef";
    put(w, &k_quote, 1);
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') {
            char e[2] = {'\\', (char)c};
            put(w, e, 2);
        } else if (c < 0x20 || c >= 0x7f) {
            char e[6] = {'\\', 'u', '0', '0', H[c >> 4], H[c & 15]};
            put(w, e, 6);
        } else {
            put(w, s, 1);
        }
    }
    put(w, &k_quote, 1);
}

void jw_long(jw_t *w, long v) {
    char t[24];
    int n = snprintf(t, sizeof t, "%ld", v);
    put(w, t, (size_t)n);
}

void jw_bool(jw_t *w, int v) { jw_raw(w, v ? "true" : "false"); }

int jw_end(jw_t *w) { return w->err ? -1 : (int)w->len; }

/* ---- parser ---- */

typedef struct {
    const char *s;
    size_t len, i;
    jflat_t *j;
    size_t pool;
} P;

static void ws(P *p) {
    while (p->i < p->len && (p->s[p->i] == ' ' || p->s[p->i] == '\t' || p->s[p->i] == '\n' || p->s[p->i] == '\r')) p->i++;
}

static int hv(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* A string at p->i (on the opening quote) unescaped into the pool; *out = start. maxlen bounds the text. */
static int pstring(P *p, size_t maxlen, const char **out) {
    char *d = p->j->pool + p->pool;
    size_t room = JFLAT_POOL - p->pool, n = 0;
    p->i++;
    for (;;) {
        unsigned char c;
        if (p->i >= p->len) return JF_E_SYNTAX;
        c = (unsigned char)p->s[p->i++];
        if (c == '"') break;
        if (c < 0x20 || c >= 0x80) return JF_E_STRING;
        if (c == '\\') {
            if (p->i >= p->len) return JF_E_SYNTAX;
            c = (unsigned char)p->s[p->i++];
            switch (c) {
            case '"': case '\\': case '/': break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case 'n': c = '\n'; break;
            case 'r': c = '\r'; break;
            case 't': c = '\t'; break;
            case 'u': {
                int a, b, e, f;
                if (p->i + 4 > p->len) return JF_E_SYNTAX;
                a = hv(p->s[p->i]); b = hv(p->s[p->i + 1]); e = hv(p->s[p->i + 2]); f = hv(p->s[p->i + 3]);
                if (a < 0 || b < 0 || e < 0 || f < 0) return JF_E_SYNTAX;
                if (a != 0 || b != 0 || e > 7 || (e == 0 && f == 0)) return JF_E_STRING; /* only U+0001..U+007F */
                c = (unsigned char)(e << 4 | f);
                p->i += 4;
                break;
            }
            default: return JF_E_SYNTAX;
            }
        }
        if (n >= maxlen || n + 1 >= room) return JF_E_LIMIT;
        d[n++] = (char)c;
    }
    d[n] = 0;
    *out = d;
    p->pool += n + 1;
    return JF_OK;
}

static int pvalue(P *p, const char **out) {
    char *d = p->j->pool + p->pool;
    size_t room = JFLAT_POOL - p->pool;
    if (p->i >= p->len) return JF_E_SYNTAX;
    if (p->s[p->i] == '"') return pstring(p, JFLAT_VAL_MAX, out);
    if (p->len - p->i >= 4 && !memcmp(p->s + p->i, "true", 4)) { p->i += 4; *out = "1"; return JF_OK; }
    if (p->len - p->i >= 5 && !memcmp(p->s + p->i, "false", 5)) { p->i += 5; *out = "0"; return JF_OK; }
    if (p->s[p->i] == '-' || (p->s[p->i] >= '0' && p->s[p->i] <= '9')) {
        size_t st = p->i, digits = 0, n;
        if (p->s[p->i] == '-') p->i++;
        if (p->i >= p->len || p->s[p->i] < '0' || p->s[p->i] > '9') return JF_E_SYNTAX;
        if (p->s[p->i] == '0' && p->i + 1 < p->len && p->s[p->i + 1] >= '0' && p->s[p->i + 1] <= '9') return JF_E_SYNTAX;
        while (p->i < p->len && p->s[p->i] >= '0' && p->s[p->i] <= '9') { p->i++; digits++; }
        if (p->i < p->len && (p->s[p->i] == '.' || p->s[p->i] == 'e' || p->s[p->i] == 'E')) return JF_E_TYPE;
        if (digits > 18) return JF_E_LIMIT;
        n = p->i - st;
        if (n + 1 > room) return JF_E_LIMIT;
        if (n == 2 && p->s[st] == '-' && p->s[st + 1] == '0') { d[0] = '0'; d[1] = 0; n = 1; } /* -0 is 0 */
        else { memcpy(d, p->s + st, n); d[n] = 0; }
        *out = d;
        p->pool += n + 1;
        return JF_OK;
    }
    if (p->s[p->i] == 'n' || p->s[p->i] == '[' || p->s[p->i] == '{') return JF_E_TYPE; /* null, array, object */
    return JF_E_SYNTAX;
}

int jflat_parse(jflat_t *j, const char *s, size_t len) {
    P p;
    size_t k;
    memset(j, 0, offsetof(jflat_t, pool));
    p.s = s; p.len = len; p.i = 0; p.j = j; p.pool = 0;
    if (!s) return JF_E_SYNTAX;
    ws(&p);
    if (p.i >= len || s[p.i] != '{') return JF_E_SYNTAX;
    p.i++;
    ws(&p);
    if (p.i < len && s[p.i] == '}') {
        p.i++;
    } else {
        for (;;) {
            int r;
            const char *key, *val;
            ws(&p);
            if (p.i >= len || s[p.i] != '"') return JF_E_SYNTAX;
            r = pstring(&p, JFLAT_KEY_MAX, &key);
            if (r) return r;
            ws(&p);
            if (p.i >= len || s[p.i] != ':') return JF_E_SYNTAX;
            p.i++;
            ws(&p);
            r = pvalue(&p, &val);
            if (r) return r;
            if (j->n >= JFLAT_MAX) return JF_E_LIMIT;
            for (k = 0; k < j->n; k++)
                if (!strcmp(j->key[k], key)) return JF_E_DUP;
            j->key[j->n] = key;
            j->val[j->n] = val;
            j->n++;
            ws(&p);
            if (p.i < len && s[p.i] == ',') { p.i++; continue; }
            if (p.i < len && s[p.i] == '}') { p.i++; break; }
            return JF_E_SYNTAX;
        }
    }
    ws(&p);
    return p.i == len ? JF_OK : JF_E_SYNTAX;
}
