/* Credential store, see cred.h. No printing in this file: the password never
 * reaches a log, and neither does anything derived from it. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "b64.h"
#include "cred.h"
#include "sha1.h"

/* ------------------------------------------------------------------------
 * Clocks
 * ------------------------------------------------------------------------ */
static int64_t default_wall_s(void) { return (int64_t)time(NULL); }
static int64_t default_mono_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}
int64_t (*cred_wall_s)(void) = default_wall_s;
int64_t (*cred_mono_us)(void) = default_mono_us;

/* ------------------------------------------------------------------------
 * State
 * ------------------------------------------------------------------------ */
typedef struct {
    ino_t ino;
    off_t size;
    time_t mtime;
    long mtime_ns;
    mode_t mode;
    uid_t uid;
} sig_t;

/* A verified nonce: the first 8 bytes of its SHA-1 and the monotonic second (cred_mono_us() / 1e6) after which its
 * token can no longer pass the Created check (0 = free slot). 12 bytes each: CRED_NONCE_CACHE of them fit in 12 KB. */
typedef struct {
    uint8_t h[CRED_NONCE_HASH];
    uint32_t exp_s;
} nonce_t;

/* Failed logins of one peer address (ONVIF over HTTP): lockout, see cred_peer_locked. */
typedef struct {
    uint32_t ip;                     /* network order; 0 = free */
    unsigned fails;
    int64_t last_us, until_us;
} peer_t;

/* One mutex around every public entry point (the RTSP thread and the ONVIF HTTP thread both come here). Lock
 * order: cred, then rtsp_auth. Nothing below sleeps: the limiter delay is returned to the caller. */
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
#define LOCK()   pthread_mutex_lock(&g_mu)
#define UNLOCK() pthread_mutex_unlock(&g_mu)

static struct {
    const char *path;
    rtsp_auth_db_t own;              /* limiter and htdigest db when none is bound */
    rtsp_auth_db_t *limiter;
    bool inited;
    bool loaded;
    int64_t next_check_us;
    cred_state_t state;
    const char *problem;
    sig_t sig;
    char user[CRED_USER_MAX + 1];
    char pass[CRED_PASS_MAX + 1];
    nonce_t cache[CRED_NONCE_CACHE]; /* verified nonces with their expiry */
    uint32_t floor_s;                /* tokens whose expiry is <= this were evicted from the cache: refused */
    /* Token clock: offset (token time - monotonic seconds) of the wall clock, or, while the wall clock is unset,
     * the one anchored on the first authenticated token; the previous one stays valid for a while after a step. */
    bool have_off, have_prev;
    int64_t off_s, prev_off_s;
    int64_t prev_until_s;
    peer_t peers[CRED_PEERS];
} S;

static void wipe(void *p, size_t n)
{
    volatile uint8_t *v = p;
    while (n--) *v++ = 0;
}

static int ct_eq(const void *a, const void *b, size_t n)
{
    const volatile uint8_t *x = a, *y = b;
    uint8_t d = 0;
    for (size_t i = 0; i < n; i++) d |= (uint8_t)(x[i] ^ y[i]);
    return d == 0;
}

static void init_l(const char *path, rtsp_auth_db_t *limiter)
{
    wipe(&S, sizeof(S));
    S.path = path ? path : CRED_PATH;
    rtsp_auth_db_init(&S.own, RTSP_AUTH_PATH);
    S.limiter = limiter ? limiter : &S.own;
    S.inited = true;
    S.state = CRED_ABSENT;
    S.problem = "";
}

void cred_init(const char *path, rtsp_auth_db_t *limiter)
{
    LOCK();
    init_l(path, limiter);
    UNLOCK();
}

/* ------------------------------------------------------------------------
 * File
 * ------------------------------------------------------------------------ */
static bool user_ok(const char *u, size_t n)
{
    if (n < 1 || n > CRED_USER_MAX) return false;
    for (size_t i = 0; i < n; i++) {
        char ch = u[i];
        bool okc = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
                   ch == '.' || ch == '_' || ch == '@' || ch == '+' || ch == '-';
        if (!okc) return false;
    }
    return true;
}

static bool pass_ok(const char *p, size_t n)
{
    if (n < 1 || n > CRED_PASS_MAX) return false;
    for (size_t i = 0; i < n; i++) {
        unsigned char ch = (unsigned char)p[i];
        if (ch < 0x20 || ch == 0x7f) return false;
    }
    return true;
}

int cred_parse(const char *text, size_t len, char *user, char *pass)
{
    bool have_u = false, have_p = false;
    char u[CRED_USER_MAX + 1], p[CRED_PASS_MAX + 1];
    int rc = -1;
    if (len > CRED_FILE_MAX) return -1;
    size_t i = 0;
    while (i < len) {
        size_t e = i;
        while (e < len && text[e] != '\n') e++;
        const char *line = text + i;
        size_t ll = e - i;
        i = e < len ? e + 1 : e;
        if (ll == 0 || line[0] == '#') continue;
        const char *eq = memchr(line, '=', ll);
        if (!eq) goto out;
        size_t kl = (size_t)(eq - line), vl = ll - kl - 1;
        const char *v = eq + 1;
        if (kl == 4 && memcmp(line, "user", 4) == 0) {
            if (have_u || !user_ok(v, vl)) goto out;
            memcpy(u, v, vl);
            u[vl] = '\0';
            have_u = true;
        } else if (kl == 4 && memcmp(line, "pass", 4) == 0) {
            if (have_p || !pass_ok(v, vl)) goto out;
            memcpy(p, v, vl);
            p[vl] = '\0';
            have_p = true;
        } else {
            goto out;
        }
    }
    if (have_u && have_p) {
        memcpy(user, u, strlen(u) + 1);
        memcpy(pass, p, strlen(p) + 1);
        rc = 0;
    }
out:
    wipe(p, have_p ? sizeof(p) : 0);
    return rc;
}

static void sig_of(const struct stat *st, sig_t *s)
{
    s->ino = st->st_ino;
    s->size = st->st_size;
    s->mtime = st->st_mtim.tv_sec;
    s->mtime_ns = st->st_mtim.tv_nsec;
    s->mode = st->st_mode;
    s->uid = st->st_uid;
}

static void set_state(cred_state_t st, const char *problem)
{
    S.state = st;
    S.problem = problem;
    if (st != CRED_OK) {
        wipe(S.user, sizeof(S.user));
        wipe(S.pass, sizeof(S.pass));
    }
}

static void load(void)
{
    int fd = open(S.path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        if (errno == ENOENT || errno == ENOTDIR) set_state(CRED_ABSENT, "");
        else set_state(CRED_INVALID, "cannot open the file");
        memset(&S.sig, 0, sizeof(S.sig));
        S.loaded = true;
        return;
    }
    struct stat st;
    char buf[CRED_FILE_MAX + 1];
    ssize_t n = 0;
    if (fstat(fd, &st) != 0) {
        close(fd);
        set_state(CRED_INVALID, "cannot stat the file");
        S.loaded = true;
        return;
    }
    sig_of(&st, &S.sig);
    S.loaded = true;
    if (!S_ISREG(st.st_mode)) set_state(CRED_INVALID, "not a regular file");
    else if (st.st_mode & 077) set_state(CRED_INVALID, "permissions too open (must be 0600)");
    else if (st.st_uid != 0 && st.st_uid != geteuid()) set_state(CRED_INVALID, "wrong owner");
    else if (st.st_size > CRED_FILE_MAX) set_state(CRED_INVALID, "file too large");
    else {
        ssize_t r;
        while (n < (ssize_t)sizeof(buf) && (r = read(fd, buf + n, sizeof(buf) - (size_t)n)) != 0) {
            if (r < 0) {
                if (errno == EINTR) continue;
                n = -1;
                break;
            }
            n += r;
        }
        char user[CRED_USER_MAX + 1], pass[CRED_PASS_MAX + 1];
        if (n < 0) set_state(CRED_INVALID, "cannot read the file");
        else if (cred_parse(buf, (size_t)n, user, pass) != 0) set_state(CRED_INVALID, "bad format");
        else {
            memcpy(S.user, user, sizeof(S.user));
            memcpy(S.pass, pass, sizeof(S.pass));
            set_state(CRED_OK, "");
        }
        wipe(pass, sizeof(pass));
    }
    wipe(buf, sizeof(buf));
    close(fd);
}

static cred_state_t refresh_l(bool force)
{
    if (!S.inited) init_l(NULL, NULL);
    int64_t now = cred_mono_us();
    if (S.loaded && !force && now < S.next_check_us) return S.state;
    S.next_check_us = now + CRED_RECHECK_US;
    struct stat st;
    if (S.loaded && !force) {
        if (lstat(S.path, &st) == 0) {
            sig_t s;
            sig_of(&st, &s);
            if (memcmp(&s, &S.sig, sizeof(s)) == 0) return S.state;
        } else if (S.state == CRED_ABSENT && (errno == ENOENT || errno == ENOTDIR)) {
            return S.state;
        }
    }
    load();
    return S.state;
}

cred_state_t cred_refresh(bool force)
{
    LOCK();
    cred_state_t st = refresh_l(force);
    UNLOCK();
    return st;
}

const char *cred_problem(void)
{
    LOCK();
    const char *p = S.problem ? S.problem : ""; /* always a string literal */
    UNLOCK();
    return p;
}

bool cred_get_user(char *out, size_t cap)
{
    bool ok = false;
    LOCK();
    if (refresh_l(false) == CRED_OK && cap > strlen(S.user)) {
        memcpy(out, S.user, strlen(S.user) + 1);
        ok = true;
    } else if (cap) {
        out[0] = '\0';
    }
    UNLOCK();
    return ok;
}

bool cred_have_plain(void)
{
    LOCK();
    bool ok = refresh_l(false) == CRED_OK;
    UNLOCK();
    return ok;
}

/* ------------------------------------------------------------------------
 * xsd:dateTime -> Unix seconds
 * ------------------------------------------------------------------------ */
static bool num(const char *s, size_t n, int *out)
{
    int v = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') return false;
        v = v * 10 + (s[i] - '0');
    }
    *out = v;
    return true;
}

/* Days since 1970-01-01 of a proleptic Gregorian date. */
static int64_t days_from_civil(int64_t y, int m, int d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static int days_in_month(int y, int m)
{
    static const int dm[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) return 29;
    return dm[m - 1];
}

/* YYYY-MM-DDThh:mm:ss[.fraction][Z|+hh:mm|-hh:mm]; no zone = UTC. */
static bool parse_created(const char *s, int64_t *out)
{
    size_t n = strlen(s);
    int Y, M, D, h, m, sec;
    if (n < 19 || s[4] != '-' || s[7] != '-' || (s[10] != 'T' && s[10] != 't') || s[13] != ':' || s[16] != ':')
        return false;
    if (!num(s, 4, &Y) || !num(s + 5, 2, &M) || !num(s + 8, 2, &D) || !num(s + 11, 2, &h) ||
        !num(s + 14, 2, &m) || !num(s + 17, 2, &sec))
        return false;
    if (Y < 1970 || M < 1 || M > 12 || D < 1 || D > days_in_month(Y, M) || h > 23 || m > 59 || sec > 60)
        return false;
    size_t i = 19;
    if (i < n && s[i] == '.') {
        size_t f = ++i;
        while (i < n && s[i] >= '0' && s[i] <= '9') i++;
        if (i == f) return false;
    }
    int64_t off = 0;
    if (i < n && (s[i] == 'Z' || s[i] == 'z')) {
        i++;
    } else if (i < n && (s[i] == '+' || s[i] == '-')) {
        int oh, om;
        if (n - i != 6 || s[i + 3] != ':' || !num(s + i + 1, 2, &oh) || !num(s + i + 4, 2, &om) || oh > 14 || om > 59)
            return false;
        off = (oh * 3600 + om * 60) * (s[i] == '+' ? 1 : -1);
        i = n;
    }
    if (i != n) return false;
    *out = days_from_civil(Y, M, D) * 86400 + h * 3600 + m * 60 + sec - off;
    return true;
}

/* ------------------------------------------------------------------------
 * WS-Security
 * ------------------------------------------------------------------------ */
/* Update the time base for a token that passed the digest and keep the previous base across a clock step. */
static void clock_track(int64_t mono_s, int64_t wall, int64_t created)
{
    if (wall >= CRED_CLOCK_MIN_YEAR_S) {
        int64_t off = wall - mono_s;
        if (S.have_off && (off > S.off_s + 2 || off < S.off_s - 2)) {     /* SetSystemDateAndTime, NTP, or the clock got set */
            S.prev_off_s = S.off_s;
            S.have_prev = true;
            S.prev_until_s = mono_s + CRED_STEP_GRACE_S;
            S.floor_s = 0;                                                /* expiries of the old base are not comparable */
        }
        if (!S.have_off || off > S.off_s + 2 || off < S.off_s - 2) S.off_s = off;   /* +-2 s: rounding, not a step */
        S.have_off = true;
    } else if (!S.have_off) {
        S.off_s = created - mono_s;           /* unset clock: anchor on the first authenticated token */
        S.have_off = true;
    }                                          /* else keep the base: the monotonic clock still runs */
}

static bool in_window(int64_t created, int64_t mono_s, int64_t off, int64_t *exp)
{
    int64_t d = created - (mono_s + off);
    if (d > CRED_CLOCK_WINDOW_S || d < -CRED_CLOCK_WINDOW_S) return false;
    *exp = created + CRED_CLOCK_WINDOW_S - off;   /* monotonic second after which this token is stale */
    return true;
}

/* 1 = accepted and remembered, 0 = refused (counts as a failed login), -1 = authentic but older than every cached
 * nonce while the cache is full (refused, not counted: nothing suggests an attack). */
static int fresh_and_remember(const char *created, const uint8_t *nonce, size_t nn)
{
    int64_t c, exp1, exp2 = 0, mono_s = cred_mono_us() / 1000000, wall = cred_wall_s();
    if (!parse_created(created, &c)) return 0;
    clock_track(mono_s, wall, c);
    bool ok = in_window(c, mono_s, S.off_s, &exp1);
    if (S.have_prev && mono_s <= S.prev_until_s && in_window(c, mono_s, S.prev_off_s, &exp2)) {
        if (!ok || exp2 > exp1) exp1 = exp2;
        ok = true;
    }
    if (!ok) return 0;
    uint32_t stored = (uint32_t)(exp1 + 1);      /* 0 stays "free" */

    uint8_t h[SHA1_DIGEST_LEN];
    sha1_ctx x;
    sha1_init(&x);
    sha1_update(&x, nonce, nn);
    sha1_final(&x, h);
    nonce_t *slot = NULL, *oldest = NULL;
    for (unsigned i = 0; i < CRED_NONCE_CACHE; i++) {
        nonce_t *e = &S.cache[i];
        if (e->exp_s && (uint32_t)mono_s >= e->exp_s) e->exp_s = 0;
        if (e->exp_s) {
            if (ct_eq(e->h, h, CRED_NONCE_HASH)) return 0;                  /* replay */
            if (!oldest || e->exp_s < oldest->exp_s) oldest = e;
        } else if (!slot) {
            slot = e;
        }
    }
    if (stored <= S.floor_s) return -1;                                      /* older than something already forgotten */
    if (!slot) {
        /* Full of live nonces: forget the one with the oldest Created and refuse anything not newer than it from
         * now on, so that a forgotten nonce can never be replayed. Logins stay possible however many there are. */
        if (stored <= oldest->exp_s) return -1;
        if (oldest->exp_s > S.floor_s) S.floor_s = oldest->exp_s;
        slot = oldest;
    }
    memcpy(slot->h, h, CRED_NONCE_HASH);
    slot->exp_s = stored;
    return 1;
}

/* 1 = authenticated, 0 = refused and counted, -1 = refused, not counted. */
static int wsse_verify(const char *username, const char *nonce_b64, const char *created, const char *digest_b64)
{
    if (!username || !nonce_b64 || !created || !digest_b64) return 0;
    if (refresh_l(false) != CRED_OK) return 0;

    size_t nl = strlen(nonce_b64), cl = strlen(created), dl = strlen(digest_b64);
    if (nl == 0 || nl > B64_ENCODED_LEN(CRED_NONCE_RAW_MAX) || cl == 0 || cl > CRED_CREATED_MAX ||
        dl != B64_ENCODED_LEN(SHA1_DIGEST_LEN))
        return 0;
    uint8_t nonce[CRED_NONCE_RAW_MAX], want[SHA1_DIGEST_LEN + 3];
    long nn = b64_decode(nonce_b64, nl, nonce, sizeof(nonce));
    long dn = b64_decode(digest_b64, dl, want, sizeof(want));
    if (nn < 1 || dn != SHA1_DIGEST_LEN) return 0;

    /* Everything below runs whatever the user name is. */
    char ub_client[CRED_USER_MAX + 1] = "", ub_db[CRED_USER_MAX + 1] = "";
    size_t ul = strlen(username);
    memcpy(ub_client, username, ul <= CRED_USER_MAX ? ul : CRED_USER_MAX);
    memcpy(ub_db, S.user, strlen(S.user));
    int good = ul <= CRED_USER_MAX && ct_eq(ub_client, ub_db, sizeof(ub_db));

    sha1_ctx c;
    uint8_t mine[SHA1_DIGEST_LEN];
    sha1_init(&c);
    sha1_update(&c, nonce, (size_t)nn);
    sha1_update(&c, created, cl);
    sha1_update(&c, S.pass, strlen(S.pass));
    sha1_final(&c, mine);
    good &= ct_eq(mine, want, SHA1_DIGEST_LEN);
    wipe(mine, sizeof(mine));
    if (!good) return 0;
    return fresh_and_remember(created, nonce, (size_t)nn);
}

static int finish(int ok, unsigned *delay_ms)
{
    if (ok < 0) {                  /* refused without blaming the peer */
        if (delay_ms) *delay_ms = 0;
        return 0;
    }
    if (ok) {
        rtsp_auth_note_success(S.limiter);
        if (delay_ms) *delay_ms = 0;
        return 1;
    }
    unsigned d = rtsp_auth_note_failure(S.limiter, cred_mono_us());
    if (delay_ms) *delay_ms = d;
    return 0;
}

int cred_check_wsse(const char *username, const char *nonce_b64, const char *created,
                    const char *digest_b64, unsigned *delay_ms)
{
    LOCK();
    if (!S.inited) init_l(NULL, NULL);
    int r = finish(wsse_verify(username, nonce_b64, created, digest_b64), delay_ms);
    UNLOCK();
    return r;
}

void cred_note_failure(unsigned *delay_ms)
{
    LOCK();
    if (!S.inited) init_l(NULL, NULL);
    finish(0, delay_ms);
    UNLOCK();
}

/* ------------------------------------------------------------------------
 * HTTP Digest, through rtsp_auth.c
 * ------------------------------------------------------------------------ */
int cred_check_http_digest(const char *method, const char *hdr, size_t hdr_len,
                           rtsp_auth_digest_t *d, unsigned *delay_ms)
{
    LOCK();
    if (!S.inited) init_l(NULL, NULL);
    rtsp_auth_db_refresh(S.limiter);
    int r = finish(rtsp_auth_digest_verify(S.limiter, method, hdr, hdr_len, d) == 1, delay_ms);
    UNLOCK();
    return r;
}

rtsp_auth_mode_t cred_auth_mode(void)
{
    LOCK();
    if (!S.inited) init_l(NULL, NULL);
    rtsp_auth_mode_t m = rtsp_auth_db_refresh(S.limiter);
    UNLOCK();
    return m;
}

bool cred_auth_enabled(void) { return cred_auth_mode() != RTSP_AUTH_OFF; }

/* ------------------------------------------------------------------------
 * Per-peer lockout (ONVIF over HTTP)
 * ------------------------------------------------------------------------ */
static unsigned lock_ms(unsigned fails)
{
    if (fails < CRED_PEER_FREE_FAILS) return 0;
    unsigned sh = fails - CRED_PEER_FREE_FAILS;
    unsigned ms = 1000u << (sh > 4 ? 4 : sh);        /* 1, 2, 4, 8, 16 s */
    return ms;
}

static peer_t *peer_find(uint32_t ip, int64_t now, bool create)
{
    peer_t *free_e = NULL, *old = NULL;
    for (unsigned i = 0; i < CRED_PEERS; i++) {
        peer_t *p = &S.peers[i];
        if (p->ip && now - p->last_us > CRED_PEER_FORGET_US && now >= p->until_us) p->ip = 0;
        if (p->ip == ip && ip) return p;
        if (!p->ip) { if (!free_e) free_e = p; }
        else if (!old || p->last_us < old->last_us) old = p;
    }
    if (!create) return NULL;
    peer_t *p = free_e ? free_e : old;
    memset(p, 0, sizeof(*p));
    p->ip = ip;
    p->last_us = now;
    return p;
}

bool cred_peer_locked(uint32_t ip, unsigned *retry_ms)
{
    LOCK();
    if (!S.inited) init_l(NULL, NULL);
    int64_t now = cred_mono_us();
    peer_t *p = peer_find(ip, now, false);
    bool lk = p && now < p->until_us;
    if (retry_ms) *retry_ms = lk ? (unsigned)((p->until_us - now + 999) / 1000) : 0;
    UNLOCK();
    return lk;
}

void cred_peer_failed(uint32_t ip)
{
    LOCK();
    if (!S.inited) init_l(NULL, NULL);
    int64_t now = cred_mono_us();
    peer_t *p = peer_find(ip, now, true);
    if (p->fails < 1000) p->fails++;
    p->last_us = now;
    unsigned ms = lock_ms(p->fails);
    if (ms) p->until_us = now + (int64_t)ms * 1000;
    UNLOCK();
}

void cred_peer_ok(uint32_t ip)
{
    LOCK();
    if (!S.inited) init_l(NULL, NULL);
    peer_t *p = peer_find(ip, cred_mono_us(), false);
    if (p) p->ip = 0;
    UNLOCK();
}
