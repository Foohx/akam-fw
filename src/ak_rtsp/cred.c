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

/* A verified nonce, remembered as its SHA-1 until its token can no longer pass the Created check. */
typedef struct {
    uint8_t h[SHA1_DIGEST_LEN];
    int64_t expire_us;               /* cred_mono_us() time after which the entry is free */
    bool used;
} nonce_t;

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
static bool wsse_verify(const char *username, const char *nonce_b64, const char *created, const char *digest_b64)
{
    if (!username || !nonce_b64 || !created || !digest_b64) return false;
    if (refresh_l(false) != CRED_OK) return false;

    size_t nl = strlen(nonce_b64), cl = strlen(created), dl = strlen(digest_b64);
    if (nl == 0 || nl > B64_ENCODED_LEN(CRED_NONCE_RAW_MAX) || cl == 0 || cl > CRED_CREATED_MAX ||
        dl != B64_ENCODED_LEN(SHA1_DIGEST_LEN))
        return false;
    uint8_t nonce[CRED_NONCE_RAW_MAX], want[SHA1_DIGEST_LEN + 3];
    long nn = b64_decode(nonce_b64, nl, nonce, sizeof(nonce));
    long dn = b64_decode(digest_b64, dl, want, sizeof(want));
    if (nn < 1 || dn != SHA1_DIGEST_LEN) return false;

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
    if (!good) return false;

    /* Authentic: now the freshness. */
    int64_t wall = cred_wall_s();
    if (wall >= CRED_CLOCK_MIN_YEAR_S) {
        int64_t t;
        if (!parse_created(created, &t)) return false;
        int64_t d = wall - t;
        if (d > CRED_CLOCK_WINDOW_S || d < -CRED_CLOCK_WINDOW_S) return false;
    }
    /* Replay: the nonce is remembered until its token could no longer pass the Created check (Created + window),
     * or receive time + window when the clock is not set (no Created check then). Only expired entries are
     * reused; when every slot still holds a live nonce the request is refused (fail closed) rather than
     * forgetting one that could still be replayed. */
    int64_t now = cred_mono_us(), expire = now + (int64_t)CRED_CLOCK_WINDOW_S * 1000000;
    if (wall >= CRED_CLOCK_MIN_YEAR_S) {
        int64_t t;
        parse_created(created, &t); /* already known good above */
        expire = now + (t + CRED_CLOCK_WINDOW_S - wall) * 1000000;
    }
    uint8_t h[SHA1_DIGEST_LEN];
    sha1_init(&c);
    sha1_update(&c, nonce, (size_t)nn);
    sha1_final(&c, h);
    nonce_t *slot = NULL;
    for (unsigned i = 0; i < CRED_NONCE_CACHE; i++) {
        nonce_t *e = &S.cache[i];
        if (e->used && now >= e->expire_us) e->used = false;
        if (e->used) {
            if (ct_eq(e->h, h, sizeof(h))) return false;
        } else if (!slot) {
            slot = e;
        }
    }
    if (!slot) return false;
    memcpy(slot->h, h, sizeof(h));
    slot->expire_us = expire;
    slot->used = true;
    return true;
}

static int finish(bool ok, unsigned *delay_ms)
{
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
    finish(false, delay_ms);
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
