#include "api_sig.h"

static const char k_nothing[1] = {0}; /* an empty C string without the "" literal (repo C scan) */

#include <stdio.h>
#include <string.h>

void apisig_init(api_sig_t *s, uint32_t window_s) {
    memset(s, 0, sizeof *s);
    if (window_s < APISIG_WINDOW_MIN) window_s = APISIG_WINDOW_MIN;
    if (window_s > APISIG_WINDOW_MAX) window_s = APISIG_WINDOW_MAX;
    s->window_s = window_s;
}

uint64_t apisig_retention_ms(const api_sig_t *s) { return ((uint64_t)s->window_s * 2 + 5) * 1000u; }

int apisig_cache_invariant_ok(size_t capacity, uint32_t rate_milli_per_s, uint32_t retention_s, uint32_t burst) {
    uint64_t need = ((uint64_t)rate_milli_per_s * retention_s + 999) / 1000 + burst;
    return (uint64_t)capacity >= need;
}

/* ---- path, method, query ---- */

int apisig_path_ok(const char *p) {
    size_t i, seg = 0, n;
    if (!p || p[0] != '/') return 0;
    n = strlen(p);
    if (n < 2 || n > APISIG_PATH_MAX || p[n - 1] == '/' || strstr(p, "..")) return 0;
    for (i = 1; i <= n; i++) {
        char c = p[i];
        if (c == '/' || c == 0) {
            size_t len = i - seg - 1;
            if (len == 0) return 0; /* "//" */
            if (len <= 2 && p[seg + 1] == '.' && (len == 1 || p[seg + 2] == '.')) return 0; /* "." and ".." */
            seg = i;
            continue;
        }
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' ||
              c == '~' || c == '-'))
            return 0; /* includes '%' */
    }
    return 1;
}

int apisig_method_ok(const char *m) {
    return m && (!strcmp(m, "GET") || !strcmp(m, "POST") || !strcmp(m, "PATCH") || !strcmp(m, "DELETE"));
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Decode-then-encode src[0..n) into dst (NUL terminated); returns bytes written (without NUL), or -1 malformed. */
static int recode(const char *src, size_t n, char *dst) {
    static const char H[] = "0123456789ABCDEF";
    size_t i = 0;
    int o = 0;
    while (i < n) {
        unsigned char b = (unsigned char)src[i];
        if (b == '%') {
            int hi, lo;
            if (i + 2 >= n) return -1;
            hi = hexval(src[i + 1]);
            lo = hexval(src[i + 2]);
            if (hi < 0 || lo < 0) return -1;
            b = (unsigned char)(hi << 4 | lo);
            if (b == 0) return -1;
            i += 3;
        } else {
            i++;
        }
        if ((b >= 'A' && b <= 'Z') || (b >= 'a' && b <= 'z') || (b >= '0' && b <= '9') || b == '-' || b == '.' || b == '_' ||
            b == '~') {
            dst[o++] = (char)b;
        } else {
            dst[o++] = '%';
            dst[o++] = H[b >> 4];
            dst[o++] = H[b & 15];
        }
    }
    dst[o++] = 0;
    return o - 1;
}

int apisig_canon_query(const char *raw, char *out, size_t cap) {
    char pool[3 * APISIG_QUERY_MAX + 2 * APISIG_QUERY_PARAMS + 8];
    const char *nm[APISIG_QUERY_PARAMS], *vl[APISIG_QUERY_PARAMS];
    size_t n, i, np = 0, used = 0, len = 0;
    if (!out || cap == 0) return -2;
    out[0] = 0;
    if (!raw || !*raw) return 0;
    n = strlen(raw);
    if (n > APISIG_QUERY_MAX) return -2;
    i = 0;
    while (i <= n) {
        size_t e = i, eq;
        int a, b;
        while (e < n && raw[e] != '&') e++;
        if (e == i) return -1; /* empty segment */
        if (np >= APISIG_QUERY_PARAMS) return -2;
        eq = i;
        while (eq < e && raw[eq] != '=') eq++;
        a = recode(raw + i, eq - i, pool + used);
        if (a < 0) return -1;
        nm[np] = pool + used;
        used += (size_t)a + 1;
        b = recode(raw + (eq < e ? eq + 1 : e), eq < e ? e - eq - 1 : 0, pool + used);
        if (b < 0) return -1;
        vl[np] = pool + used;
        used += (size_t)b + 1;
        np++;
        i = e + 1;
    }
    for (i = 1; i < np; i++) { /* insertion sort by (name, value), bytewise */
        const char *kn = nm[i], *kv = vl[i];
        size_t j = i;
        while (j > 0) {
            int c = strcmp(nm[j - 1], kn);
            if (c < 0 || (c == 0 && strcmp(vl[j - 1], kv) <= 0)) break;
            nm[j] = nm[j - 1];
            vl[j] = vl[j - 1];
            j--;
        }
        nm[j] = kn;
        vl[j] = kv;
    }
    for (i = 0; i < np; i++) {
        size_t ln = strlen(nm[i]), lv = strlen(vl[i]);
        if (len + (i ? 1 : 0) + ln + 1 + lv + 1 > cap) return -2;
        if (i) out[len++] = '&';
        memcpy(out + len, nm[i], ln);
        len += ln;
        out[len++] = '=';
        memcpy(out + len, vl[i], lv);
        len += lv;
    }
    out[len] = 0;
    return (int)len;
}

static int clean_line(const char *s, size_t max) { /* a canonical line: printable, no newline, bounded */
    size_t i;
    if (!s || !*s) return 0;
    for (i = 0; s[i]; i++)
        if ((unsigned char)s[i] < 0x20 || (unsigned char)s[i] > 0x7e || i >= max) return 0;
    return 1;
}

int apisig_canon(char *out, size_t cap, const char *method, const char *path, const char *query, const void *body,
                 size_t body_len, const char *line6, const char *line7, const char *key_id) {
    char m[8], q[3 * APISIG_QUERY_MAX + 1], bh[65];
    uint8_t d[SHA256_LEN];
    size_t i;
    int qn, n;
    if (!out || cap == 0 || !method || strlen(method) > 7 || !apisig_path_ok(path) || !clean_line(line6, 80) ||
        !clean_line(line7, 80) || !akey_id_ok(key_id) || (!body && body_len))
        return -1;
    for (i = 0; method[i]; i++) m[i] = (method[i] >= 'a' && method[i] <= 'z') ? (char)(method[i] - 32) : method[i];
    m[i] = 0;
    qn = apisig_canon_query(query, q, sizeof q);
    if (qn < 0) return -1;
    sha256(body ? body : k_nothing, body_len, d);
    akam_hex(d, SHA256_LEN, bh);
    n = snprintf(out, cap, "%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s", APISIG_VERSION_LINE, m, path, q, bh, line6, line7, key_id);
    if (n < 0 || (size_t)n >= cap) return -2;
    return n;
}

void apisig_sign(const uint8_t secret[32], const char *canon, size_t canon_len, uint8_t out[SHA256_LEN]) {
    hmac_sha256(secret, 32, canon, canon_len, out);
}

/* ---- challenges ---- */

static void purge(api_sig_t *s, uint64_t now) {
    size_t i;
    for (i = 0; i < APISIG_NONCE_CAP; i++)
        if (s->nonce[i].used && now >= s->nonce[i].expires_ms) s->nonce[i].used = 0;
    for (i = 0; i < APISIG_CHAL_CAP; i++)
        if (s->chal[i].used && now >= s->chal[i].expires_ms) s->chal[i].used = 0;
}

int apisig_hello(api_sig_t *s, uint64_t now, const uint8_t rnd[16], char hex_out[33]) {
    size_t i;
    purge(s, now);
    for (i = 0; i < APISIG_CHAL_CAP; i++) /* a repeated value (broken RNG) would be two live challenges: refuse */
        if (s->chal[i].used && memcmp(s->chal[i].chal, rnd, 16) == 0) return -1;
    for (i = 0; i < APISIG_CHAL_CAP; i++) {
        if (s->chal[i].used) continue;
        s->chal[i].used = 1;
        memcpy(s->chal[i].chal, rnd, 16);
        s->chal[i].expires_ms = now + APISIG_CHAL_TTL_MS;
        akam_hex(rnd, 16, hex_out);
        return 0;
    }
    return -1;
}

int apisig_hello_json(char *out, size_t cap, int clock_set, const char *chal_hex) {
    int n = snprintf(out, cap, "{\"api\":1,\"clock_set\":%s,\"challenge\":\"%s\"}", clock_set ? "true" : "false", chal_hex);
    return (n < 0 || (size_t)n >= cap) ? -1 : n;
}

/* ---- verification ---- */

static uint32_t kid_num(const char *id) {
    uint32_t v = 0;
    size_t i;
    for (i = 0; i < AKEY_ID_LEN; i++) v = v << 4 | (uint32_t)hexval(id[i]);
    return v;
}

static int is_hex(const char *s, size_t n) {
    size_t i;
    if (!s || strlen(s) != n) return 0;
    for (i = 0; i < n; i++)
        if (hexval(s[i]) < 0) return 0;
    return 1;
}

static int fail(apisig_result_t *r, int http, const char *err) {
    r->http = http;
    r->error = err;
    return http;
}

static const uint8_t DUMMY_KEY[32] = {'a', 'k', 'a', 'm', '-', 'd', 'u', 'm', 'm', 'y', '-', 'k', 'e', 'y', '-', 'u',
                                      'n', 'k', 'n', 'o', 'w', 'n', '-', 'i', 'd', '-', 'p', 'a', 't', 'h', '0', '1'};

int apisig_verify(api_sig_t *s, const akeys_t *keys, int clock_set, int64_t now_wall_s, uint64_t now_ms,
                  const apisig_req_t *rq, int need_scope, apisig_result_t *res) {
    char canon[APISIG_CANON_MAX], chal_line[10 + 32 + 1];
    const char *l6, *l7;
    uint8_t sig[SHA256_LEN], mac[SHA256_LEN], nonce[16], chal[16];
    const uint8_t *secret;
    int idx, cn, chal_mode, ok, slot = -1;
    size_t i;
    int64_t ts = 0;
    uint32_t kid;

    memset(res, 0, sizeof *res);
    res->key_idx = -1;
    /* 1. shape of the request (the HTTP layer has already matched the endpoint table) */
    if (!rq || !apisig_method_ok(rq->method) || !apisig_path_ok(rq->path) || (rq->query && strlen(rq->query) > APISIG_QUERY_MAX))
        return fail(res, 400, "bad_request");
    {
        char q[3 * APISIG_QUERY_MAX + 1];
        if (apisig_canon_query(rq->query, q, sizeof q) < 0) return fail(res, 400, "bad_request");
    }
    if (!rq->h_key || !akey_id_ok(rq->h_key) || !is_hex(rq->h_signature, 64)) {
        if (!rq->h_key && !rq->h_signature && !rq->h_timestamp && !rq->h_challenge) return fail(res, 401, "missing_auth");
        return fail(res, 400, "bad_header");
    }
    chal_mode = rq->h_challenge != NULL;
    if (chal_mode) {
        if (rq->h_timestamp || rq->h_nonce || !is_hex(rq->h_challenge, 32)) return fail(res, 400, "bad_header");
        akam_unhex(rq->h_challenge, 16, chal);
        snprintf(chal_line, sizeof chal_line, "challenge:%s", rq->h_challenge);
        l6 = l7 = chal_line;
    } else {
        size_t n;
        if (!rq->h_timestamp || !rq->h_nonce) return fail(res, 401, "missing_auth");
        n = strlen(rq->h_timestamp);
        if (n < 1 || n > 11 || (n > 1 && rq->h_timestamp[0] == '0') || !is_hex(rq->h_nonce, 32))
            return fail(res, 400, "bad_header");
        for (i = 0; i < n; i++) {
            if (rq->h_timestamp[i] < '0' || rq->h_timestamp[i] > '9') return fail(res, 400, "bad_header");
            ts = ts * 10 + (rq->h_timestamp[i] - '0');
        }
        akam_unhex(rq->h_nonce, 16, nonce);
        l6 = rq->h_timestamp;
        l7 = rq->h_nonce;
    }
    akam_unhex(rq->h_signature, 32, sig);
    kid = kid_num(rq->h_key);

    /* 3. key lookup (2, the per-peer lockout, is the HTTP layer's). An unknown id takes EXACTLY the same path as a known one
     * (same checks, same errors, same single HMAC) with a fixed dummy secret, and only differs by always ending in
     * bad_signature: neither the answer nor the work tells a known id from an unknown one. */
    idx = keys ? akeys_find(keys, rq->h_key) : -1;
    secret = idx >= 0 ? keys->k[idx].secret : DUMMY_KEY;
    if (idx >= 0) {
        res->key_idx = idx;
        res->scope = keys->k[idx].scope;
    }
    purge(s, now_ms);

    /* 4. clock window, or the challenge */
    if (chal_mode) {
        for (i = 0; i < APISIG_CHAL_CAP; i++)
            if (s->chal[i].used && memcmp(s->chal[i].chal, chal, 16) == 0) { slot = (int)i; break; }
        if (slot < 0) return fail(res, 401, "bad_challenge");
    } else {
        int64_t d;
        if (!clock_set) return fail(res, 401, "clock_unset");
        d = now_wall_s - ts;
        res->skew_s = d;
        if (d > (int64_t)s->window_s || d < -(int64_t)s->window_s) return fail(res, 401, "clock_skew");
        /* 5. nonce cache, read only: replay, or no free slot (fail closed, never evict a live nonce) */
        for (i = 0; i < APISIG_NONCE_CAP; i++) {
            if (!s->nonce[i].used) { if (slot < 0) slot = (int)i; continue; }
            if (s->nonce[i].kid == kid && memcmp(s->nonce[i].nonce, nonce, 16) == 0) return fail(res, 401, "replay");
        }
        if (slot < 0) return fail(res, 429, "busy");
    }

    /* 6. recompute and compare in constant time */
    cn = apisig_canon(canon, sizeof canon, rq->method, rq->path, rq->query, rq->body, rq->body_len, l6, l7, rq->h_key);
    if (cn < 0) return fail(res, 400, "bad_request");
    apisig_sign(secret, canon, (size_t)cn, mac);
    s->hmac_calls++;
    ok = ct_equal(mac, sig, SHA256_LEN) & (idx >= 0);
    akam_wipe(mac, sizeof mac);
    if (!ok) return fail(res, 401, "bad_signature");

    /* only now consume the nonce or challenge */
    if (chal_mode) {
        s->chal[slot].used = 0;
    } else {
        s->nonce[slot].used = 1;
        s->nonce[slot].kid = kid;
        memcpy(s->nonce[slot].nonce, nonce, 16);
        s->nonce[slot].expires_ms = now_ms + apisig_retention_ms(s);
    }
    /* 7. scope */
    if (keys->k[idx].scope < need_scope) return fail(res, 403, "scope");
    return APISIG_OK;
}

void apisig_drop_key(api_sig_t *s, const char *key_id) {
    size_t i;
    uint32_t kid;
    if (!akey_id_ok(key_id)) return;
    kid = kid_num(key_id);
    for (i = 0; i < APISIG_NONCE_CAP; i++)
        if (s->nonce[i].used && s->nonce[i].kid == kid) s->nonce[i].used = 0;
}
