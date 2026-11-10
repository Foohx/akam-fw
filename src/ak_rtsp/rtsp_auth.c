#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <unistd.h>

#include "md5.h"
#include "rtsp_auth.h"

/* ------------------------------------------------------------------------
 * Randomness
 * ------------------------------------------------------------------------ */
static int default_rng(void *buf, size_t n)
{
    uint8_t *p = buf;
    size_t got = 0;
    /* GRND_NONBLOCK: never wait for the entropy pool (early boot); the
     * /dev/urandom fallback does not wait either. */
    while (got < n) {
        ssize_t r = getrandom(p + got, n - got, GRND_NONBLOCK);
        if (r < 0) {
            if (errno == EINTR) continue;
            break;
        }
        got += (size_t)r;
    }
    if (got == n) return 0;
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    while (got < n) {
        ssize_t r = read(fd, p + got, n - got);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        got += (size_t)r;
    }
    close(fd);
    return got == n ? 0 : -1;
}

int (*rtsp_auth_rng)(void *buf, size_t n) = default_rng;

/* ------------------------------------------------------------------------
 * Small helpers
 * ------------------------------------------------------------------------ */

/* Constant-time equality of two n-byte buffers: 1 = equal. */
static int ct_eq(const void *a, const void *b, size_t n)
{
    const volatile uint8_t *x = a, *y = b;
    uint8_t d = 0;
    for (size_t i = 0; i < n; i++) d |= (uint8_t)(x[i] ^ y[i]);
    return d == 0;
}

static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

/* Case-insensitive compare of the n chars at s with the C string t. */
static bool ieq(const char *s, size_t n, const char *t)
{
    if (strlen(t) != n) return false;
    for (size_t i = 0; i < n; i++)
        if (lower(s[i]) != lower(t[i])) return false;
    return true;
}

static bool is_hex32(const char *s)
{
    for (int i = 0; i < 32; i++)
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) return false;
    return s[32] == '\0';
}

/* ------------------------------------------------------------------------
 * Credentials file
 * ------------------------------------------------------------------------ */
int rtsp_auth_parse_file(const char *text, size_t len, char *user, char *ha1)
{
    /* One line, with or without its line end. */
    if (len > 0 && text[len - 1] == '\n') len--;
    if (len > 0 && text[len - 1] == '\r') len--;
    const char *c1 = NULL, *c2 = NULL;
    for (size_t i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)text[i];
        if (ch == '\n' || ch == '\r' || ch == '\0') return -1;
        if (ch == ':') {
            if (!c1) c1 = text + i;
            else if (!c2) c2 = text + i;
            else return -1;
        }
    }
    if (!c1 || !c2) return -1;
    size_t ulen = (size_t)(c1 - text), rlen = (size_t)(c2 - c1 - 1), hlen = len - (size_t)(c2 - text) - 1;
    if (ulen < 1 || ulen > RTSP_AUTH_USER_MAX) return -1;
    for (size_t i = 0; i < ulen; i++) {
        char ch = text[i];
        bool okc = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
                   ch == '.' || ch == '_' || ch == '@' || ch == '+' || ch == '-';
        if (!okc) return -1;
    }
    if (rlen != strlen(RTSP_AUTH_REALM) || memcmp(c1 + 1, RTSP_AUTH_REALM, rlen) != 0) return -1;
    if (hlen != 32) return -1;
    char h[33];
    memcpy(h, c2 + 1, 32);
    h[32] = '\0';
    if (!is_hex32(h)) return -1;
    memcpy(user, text, ulen);
    user[ulen] = '\0';
    memcpy(ha1, h, 33);
    return 0;
}

void rtsp_auth_db_init(rtsp_auth_db_t *db, const char *path)
{
    memset(db, 0, sizeof(*db));
    db->path = path;
    db->mode = RTSP_AUTH_OFF;
}

rtsp_auth_mode_t rtsp_auth_db_refresh(rtsp_auth_db_t *db)
{
    rtsp_auth_mode_t mode = RTSP_AUTH_BROKEN;
    char user[RTSP_AUTH_USER_MAX + 1] = "", ha1[33] = "";
    int fd = open(db->path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        if (errno == ENOENT || errno == ENOTDIR) mode = RTSP_AUTH_OFF;
    } else {
        char buf[257];
        ssize_t n = 0, r;
        while (n < (ssize_t)sizeof(buf) && (r = read(fd, buf + n, sizeof(buf) - (size_t)n)) != 0) {
            if (r < 0) {
                if (errno == EINTR) continue;
                n = -1;
                break;
            }
            n += r;
        }
        close(fd);
        if (n >= 0 && n <= 256 && rtsp_auth_parse_file(buf, (size_t)n, user, ha1) == 0)
            mode = RTSP_AUTH_ON;
    }
    if (!db->loaded || mode != db->mode || strcmp(user, db->user) != 0 || strcmp(ha1, db->ha1) != 0) {
        db->generation++;
        db->loaded = true;
        db->mode = mode;
        memcpy(db->user, user, sizeof(db->user));
        memcpy(db->ha1, ha1, sizeof(db->ha1));
    }
    return mode;
}

/* ------------------------------------------------------------------------
 * Digest
 * ------------------------------------------------------------------------ */
void rtsp_auth_response(char out[33], const char *ha1, const char *nonce,
                        const char *nc, const char *cnonce, const char *method,
                        const char *uri)
{
    char ha2[33];
    md5_hex_cat(ha2, method, ":", uri, (char *)NULL);
    if (nc)
        md5_hex_cat(out, ha1, ":", nonce, ":", nc, ":", cnonce, ":", "auth", ":", ha2, (char *)NULL);
    else
        md5_hex_cat(out, ha1, ":", nonce, ":", ha2, (char *)NULL);
}

typedef struct {
    char username[64], realm[32], nonce[64], uri[512], response[64];
    char algorithm[16], qop[16], nc[16], cnonce[128];
} dparams_t;

/* Parse `Digest k=v, k="v", ...`. Every value must fit its buffer (no
 * truncation: a cut value could verify as something else), a known key may
 * appear once. 0 = ok, with the four mandatory ones present. */
static int parse_digest(const char *h, size_t n, dparams_t *p)
{
    struct { const char *name; char *buf; size_t cap; bool seen; } t[] = {
        { "username", p->username, sizeof(p->username), false },
        { "realm", p->realm, sizeof(p->realm), false },
        { "nonce", p->nonce, sizeof(p->nonce), false },
        { "uri", p->uri, sizeof(p->uri), false },
        { "response", p->response, sizeof(p->response), false },
        { "algorithm", p->algorithm, sizeof(p->algorithm), false },
        { "qop", p->qop, sizeof(p->qop), false },
        { "nc", p->nc, sizeof(p->nc), false },
        { "cnonce", p->cnonce, sizeof(p->cnonce), false },
    };
    const size_t nt = sizeof(t) / sizeof(t[0]);
    memset(p, 0, sizeof(*p));
    size_t i = 0;
    while (i < n && (h[i] == ' ' || h[i] == '\t')) i++;
    if (n - i < 7 || !ieq(h + i, 6, "Digest") || (h[i + 6] != ' ' && h[i + 6] != '\t')) return -1;
    i += 6;
    for (;;) {
        while (i < n && (h[i] == ' ' || h[i] == '\t' || h[i] == ',')) i++;
        if (i >= n) break;
        size_t ks = i;
        while (i < n && h[i] != '=' && h[i] != ' ' && h[i] != '\t' && h[i] != ',') i++;
        size_t kl = i - ks;
        while (i < n && (h[i] == ' ' || h[i] == '\t')) i++;
        if (kl == 0 || i >= n || h[i] != '=') return -1;
        i++;
        while (i < n && (h[i] == ' ' || h[i] == '\t')) i++;
        char val[512 + 1];
        size_t vl = 0;
        if (i < n && h[i] == '"') {
            i++;
            for (;;) {
                if (i >= n) return -1;
                char ch = h[i++];
                if (ch == '"') break;
                if (ch == '\\') {
                    if (i >= n) return -1;
                    ch = h[i++];
                }
                if (vl >= 512) return -1;
                val[vl++] = ch;
            }
        } else {
            while (i < n && h[i] != ',' && h[i] != ' ' && h[i] != '\t') {
                if (vl >= 512) return -1;
                val[vl++] = h[i++];
            }
        }
        val[vl] = '\0';
        if (memchr(val, '\0', vl) != NULL || strlen(val) != vl) return -1;
        for (size_t k = 0; k < nt; k++) {
            if (!ieq(h + ks, kl, t[k].name)) continue;
            if (t[k].seen || vl >= t[k].cap) return -1;
            memcpy(t[k].buf, val, vl + 1);
            t[k].seen = true;
            break;
        }
    }
    return (t[0].seen && t[2].seen && t[3].seen && t[4].seen) ? 0 : -1;
}

/* ------------------------------------------------------------------------
 * Connection state machine
 * ------------------------------------------------------------------------ */
void rtsp_auth_conn_init(rtsp_auth_conn_t *c) { memset(c, 0, sizeof(*c)); }

bool rtsp_auth_method_protected(const char *method)
{
    return strcmp(method, "OPTIONS") != 0;
}

static unsigned penalty_ms(unsigned fails)
{
    if (fails <= 2) return 0;
    if (fails == 3) return 500;
    if (fails == 4) return 1000;
    return 2000;
}

int rtsp_auth_new_nonce(char out[RTSP_AUTH_NONCE_HEX + 1])
{
    static const char hex[] = "0123456789abcdef";
    uint8_t r[RTSP_AUTH_NONCE_HEX / 2];
    if (!rtsp_auth_rng || rtsp_auth_rng(r, sizeof(r)) != 0) return -1;
    for (size_t i = 0; i < sizeof(r); i++) {
        out[2 * i] = hex[r[i] >> 4];
        out[2 * i + 1] = hex[r[i] & 15];
    }
    out[RTSP_AUTH_NONCE_HEX] = '\0';
    return 0;
}

/* Keep the connection's nonce while it is usable, else draw a new one. */
static int ensure_nonce(rtsp_auth_conn_t *c, int64_t now_us)
{
    if (c->has_nonce && now_us - c->nonce_us <= RTSP_AUTH_NONCE_LIFETIME_US) return 0;
    if (rtsp_auth_new_nonce(c->nonce) != 0) return -1;
    c->has_nonce = true;
    c->nonce_us = now_us;
    c->last_nc = 0;
    return 0;
}

static void deny(rtsp_auth_conn_t *c, int64_t now_us, bool stale, rtsp_auth_result_t *out)
{
    if (ensure_nonce(c, now_us) != 0) {
        out->verdict = RTSP_AUTH_DROP;
        out->challenge[0] = '\0';
        snprintf(out->log, sizeof(out->log), "auth: no random source for the nonce, refusing");
        return;
    }
    out->verdict = RTSP_AUTH_DENY;
    snprintf(out->challenge, sizeof(out->challenge),
             "Digest realm=\"%s\", nonce=\"%s\", algorithm=MD5%s", RTSP_AUTH_REALM, c->nonce,
             stale ? ", stale=true" : "");
}

unsigned rtsp_auth_note_failure(rtsp_auth_db_t *db, int64_t now_us)
{
    if (db->global_fails && now_us - db->last_fail_us > RTSP_AUTH_FAIL_WINDOW_US) db->global_fails = 0;
    db->global_fails++;
    db->last_fail_us = now_us;
    return penalty_ms(db->global_fails);
}

void rtsp_auth_note_success(rtsp_auth_db_t *db) { db->global_fails = 0; }

/* A failed attempt: count it (connection and process), slow the answer down,
 * close the connection after too many. */
static void fail(rtsp_auth_db_t *db, rtsp_auth_conn_t *c, int64_t now_us, const char *why,
                 rtsp_auth_result_t *out)
{
    c->authed = false;
    c->fails++;
    out->delay_ms = rtsp_auth_note_failure(db, now_us);
    if (c->fails >= RTSP_AUTH_CONN_MAX_FAILS) {
        out->verdict = RTSP_AUTH_DROP;
        out->challenge[0] = '\0';
        snprintf(out->log, sizeof(out->log), "auth failed: %s; closing the connection after %u failed attempts",
                 why, c->fails);
        return;
    }
    deny(c, now_us, false, out);
    if (out->verdict == RTSP_AUTH_DENY)
        snprintf(out->log, sizeof(out->log), "auth failed: %s (attempt %u of %u on this connection, delay %u ms)",
                 why, c->fails, (unsigned)RTSP_AUTH_CONN_MAX_FAILS, out->delay_ms);
}

/* User, realm, algorithm and response, all evaluated whatever the user name is,
 * and the comparisons do not stop at the first different byte. */
static bool creds_good(const rtsp_auth_db_t *db, const char *method, const dparams_t *p, bool qop)
{
    char ub_client[RTSP_AUTH_USER_MAX + 1] = "", ub_db[RTSP_AUTH_USER_MAX + 1] = "";
    size_t ul = strlen(p->username);
    memcpy(ub_client, p->username, ul <= RTSP_AUTH_USER_MAX ? ul : RTSP_AUTH_USER_MAX);
    memcpy(ub_db, db->user, strlen(db->user));
    int good = ul <= RTSP_AUTH_USER_MAX && ct_eq(ub_client, ub_db, sizeof(ub_db));
    good &= (p->realm[0] == '\0' || strcmp(p->realm, RTSP_AUTH_REALM) == 0);
    good &= (p->algorithm[0] == '\0' || ieq(p->algorithm, strlen(p->algorithm), "MD5"));

    char expect[33], got[33] = "";
    rtsp_auth_response(expect, db->ha1, p->nonce, qop ? p->nc : NULL, p->cnonce, method, p->uri);
    size_t rl = strlen(p->response);
    for (size_t i = 0; i < rl && i < 32; i++) got[i] = lower(p->response[i]);
    good &= (rl == 32) && ct_eq(got, expect, 32);
    return good != 0;
}

int rtsp_auth_digest_verify(const rtsp_auth_db_t *db, const char *method, const char *hdr,
                            size_t hdr_len, rtsp_auth_digest_t *out)
{
    dparams_t p;
    memset(out, 0, sizeof(*out));
    if (db->mode != RTSP_AUTH_ON || parse_digest(hdr, hdr_len, &p) != 0) return 0;
    bool qop = p.qop[0] != '\0';
    if (qop && (!ieq(p.qop, strlen(p.qop), "auth") || !p.nc[0] || !p.cnonce[0])) return 0;
    if (!creds_good(db, method, &p, qop)) return 0;
    memcpy(out->nonce, p.nonce, sizeof(out->nonce));
    memcpy(out->nc, p.nc, sizeof(out->nc));
    memcpy(out->cnonce, p.cnonce, sizeof(out->cnonce));
    memcpy(out->uri, p.uri, sizeof(out->uri));
    out->qop = qop;
    return 1;
}

void rtsp_auth_check(rtsp_auth_db_t *db, rtsp_auth_conn_t *c, const char *method,
                     const char *hdr, size_t hdr_len, int64_t now_us,
                     rtsp_auth_result_t *out)
{
    memset(out, 0, sizeof(*out));
    out->verdict = RTSP_AUTH_ALLOW;
    if (!rtsp_auth_method_protected(method)) return;

    rtsp_auth_mode_t mode = rtsp_auth_db_refresh(db);
    if (mode == RTSP_AUTH_OFF) {
        c->authed = false;
        return;
    }
    if (mode == RTSP_AUTH_BROKEN) {
        c->authed = false;
        deny(c, now_us, false, out);
        if (out->verdict == RTSP_AUTH_DENY)
            snprintf(out->log, sizeof(out->log),
                     "auth: credentials file is invalid, refusing every protected request");
        return;
    }

    /* RTSP_AUTH_ON */
    if (!hdr) {
        if (c->authed && c->gen == db->generation) return;
        deny(c, now_us, false, out);
        return;
    }

    dparams_t p;
    if (parse_digest(hdr, hdr_len, &p) != 0) {
        fail(db, c, now_us, "malformed Authorization header", out);
        return;
    }
    bool qop = p.qop[0] != '\0';
    if (qop && (!ieq(p.qop, strlen(p.qop), "auth") || !p.nc[0] || !p.cnonce[0])) {
        fail(db, c, now_us, "unsupported qop", out);
        return;
    }

    if (!creds_good(db, method, &p, qop)) {
        fail(db, c, now_us, "bad credentials", out);
        return;
    }

    /* The credentials are right: what remains is the nonce and nc. */
    bool nonce_same = c->has_nonce && strlen(p.nonce) == RTSP_AUTH_NONCE_HEX &&
                      ct_eq(p.nonce, c->nonce, RTSP_AUTH_NONCE_HEX);
    bool already = c->authed && c->gen == db->generation;
    bool fresh = c->has_nonce && now_us - c->nonce_us <= RTSP_AUTH_NONCE_LIFETIME_US;
    if (!nonce_same || (!fresh && !already)) {
        /* Not an attack: the client simply has to answer the new nonce. */
        deny(c, now_us, true, out);
        if (out->verdict == RTSP_AUTH_DENY)
            snprintf(out->log, sizeof(out->log), "auth: stale or unknown nonce, asking the client to retry");
        return;
    }
    if (qop) {
        char *end;
        unsigned long nc = strtoul(p.nc, &end, 16);
        if (*end != '\0' || nc == 0 || nc > 0xffffffffUL || (uint32_t)nc <= c->last_nc) {
            fail(db, c, now_us, "replayed or invalid nc", out);
            return;
        }
        c->last_nc = (uint32_t)nc;
    }
    if (!already) snprintf(out->log, sizeof(out->log), "auth: client authenticated");
    c->authed = true;
    c->gen = db->generation;
    c->fails = 0;
    rtsp_auth_note_success(db);
}

bool rtsp_auth_session_ok(rtsp_auth_db_t *db, const rtsp_auth_conn_t *c)
{
    rtsp_auth_mode_t mode = rtsp_auth_db_refresh(db);
    if (mode == RTSP_AUTH_OFF) return true;
    if (mode == RTSP_AUTH_BROKEN) return false;
    return c->authed && c->gen == db->generation;
}

void rtsp_auth_redact_uri(const char *uri, char *out, size_t cap)
{
    if (cap == 0) return;
    const char *scheme = strstr(uri, "://");
    if (scheme) {
        const char *host = scheme + 3;
        size_t span = strcspn(host, "/");
        const char *at = NULL;
        for (size_t i = 0; i < span; i++)
            if (host[i] == '@') at = host + i;   /* the last one: passwords may hold '@' */
        if (at) {
            int n = snprintf(out, cap, "%.*s***%s", (int)(host - uri), uri, at);
            (void)n;
            return;
        }
    }
    snprintf(out, cap, "%s", uri);
}
