/* Host test of src/ak_rtsp/md5.c and rtsp_auth.c: RFC 1321 vectors, the RFC 2617
 * and RFC 2069 Digest examples, and the authentication state machine (401 ->
 * right answer -> served; wrong password; stale and unknown nonces; file
 * reload; fail-closed on a broken file; rate limiting; no secret in any log
 * line or challenge). Simulated clock and randomness, a real file in a
 * temporary directory. Build/run: tests/rtsp_auth_test.sh */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include "md5.h"
#include "rtsp_auth.h"

static int fails;
#define CHECK(cond, msg) do { if (cond) printf("  ok   - %s\n", msg); \
    else { printf("  FAIL - %s (line %d)\n", msg, __LINE__); fails++; } } while (0)

/* ---- secrets, and the transcript every log/challenge is checked against ---- */
#define PASSWORD "S3NTINEL-passw0rd"
#define USER "cam_user"
static char ha1_hex[33];
static char transcript[1 << 16];
static void note(const rtsp_auth_result_t *r)
{
    strncat(transcript, r->log, sizeof(transcript) - strlen(transcript) - 1);
    strncat(transcript, "\n", sizeof(transcript) - strlen(transcript) - 1);
    strncat(transcript, r->challenge, sizeof(transcript) - strlen(transcript) - 1);
    strncat(transcript, "\n", sizeof(transcript) - strlen(transcript) - 1);
}

/* ---- fake randomness: distinct, recognisable nonces ---- */
static unsigned rng_counter;
static int rng_fail;
static int fake_rng(void *buf, size_t n)
{
    if (rng_fail) return -1;
    uint8_t *p = buf;
    rng_counter++;
    for (size_t i = 0; i < n; i++) p[i] = (uint8_t)(rng_counter * 37 + i * 11 + 5);
    return 0;
}

static char path[256];
static void put_file(const char *content)
{
    char tmp[300];
    snprintf(tmp, sizeof(tmp), "%s.new", path);
    FILE *f = fopen(tmp, "w");
    fputs(content, f);
    fclose(f);
    rename(tmp, path);
}
static void put_creds(const char *user, const char *pass)
{
    char ha1[33], line[128];
    md5_hex_cat(ha1, user, ":" RTSP_AUTH_REALM ":", pass, (char *)NULL);
    snprintf(line, sizeof(line), "%s:%s:%s\n", user, RTSP_AUTH_REALM, ha1);
    put_file(line);
}

/* nonce="..." out of a challenge */
static int nonce_of(const rtsp_auth_result_t *r, char out[33])
{
    const char *p = strstr(r->challenge, "nonce=\"");
    if (!p) return 0;
    memcpy(out, p + 7, 32);
    out[32] = '\0';
    return 1;
}

static void hdr_digest(char *out, size_t cap, const char *user, const char *pass, const char *nonce,
                       const char *method, const char *uri, const char *nc, const char *cnonce)
{
    char ha1[33], resp[33];
    md5_hex_cat(ha1, user, ":" RTSP_AUTH_REALM ":", pass, (char *)NULL);
    rtsp_auth_response(resp, ha1, nonce, nc, cnonce, method, uri);
    if (nc)
        snprintf(out, cap, "Digest username=\"%s\", realm=\"akam\", nonce=\"%s\", uri=\"%s\", response=\"%s\", "
                 "qop=auth, nc=%s, cnonce=\"%s\"", user, nonce, uri, resp, nc, cnonce);
    else
        snprintf(out, cap, "Digest username=\"%s\", realm=\"akam\", nonce=\"%s\", uri=\"%s\", "
                 "response=\"%s\", algorithm=MD5", user, nonce, uri, resp);
}

static int64_t now;
static rtsp_auth_db_t db;
static rtsp_auth_result_t res;

static void req(rtsp_auth_conn_t *c, const char *method, const char *hdr)
{
    rtsp_auth_check(&db, c, method, hdr, hdr ? strlen(hdr) : 0, now, &res);
    note(&res);
}

static void test_md5(void)
{
    static const struct { const char *in; const char *hex; } v[] = {
        { "", "d41d8cd98f00b204e9800998ecf8427e" },
        { "a", "0cc175b9c0f1b6a831c399e269772661" },
        { "abc", "900150983cd24fb0d6963f7d28e17f72" },
        { "message digest", "f96b697d7cb7938d525a2f31aaf161d0" },
        { "abcdefghijklmnopqrstuvwxyz", "c3fcd3d76192e4007dfb496cca67e13b" },
        { "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789", "d174ab98d277d9f5a5611c2c9f419d9f" },
        { "12345678901234567890123456789012345678901234567890123456789012345678901234567890",
          "57edf4a22be3c955ac49da2e2107b67a" },
    };
    printf("MD5 (RFC 1321 test suite)\n");
    char hex[33];
    for (size_t i = 0; i < sizeof(v) / sizeof(v[0]); i++) {
        md5_hex_cat(hex, v[i].in, (char *)NULL);
        char m[80];
        snprintf(m, sizeof(m), "vector %zu (%zu bytes)", i, strlen(v[i].in));
        CHECK(strcmp(hex, v[i].hex) == 0, m);
    }
    /* Padding boundaries (reference values from Python hashlib). */
    static const struct { int n; const char *hex; } b[] = {
        { 55, "04364420e25c512fd958a70738aa8f72" }, { 56, "668a72d5ba17f08e62dabcafad6db14b" },
        { 57, "693037871c4a9d3d8685018905cb530a" }, { 63, "7dc2ca208106a2f703567bdff99d8981" },
        { 64, "c1bb4f81d892b2d57947682aeb252456" }, { 65, "1bc932052302d074bdec39795fe00cf6" },
        { 119, "ab347a5f68c8a443cfcddc633f12c24f" }, { 120, "fb98667f98096de92620b64f46e1c5b5" },
        { 121, "e8bb0ccd83750100b37f4831cd5d7ee2" }, { 128, "d69cb61a6ee87200676eb0d4b90edbcb" },
    };
    char x[130];
    memset(x, 'x', sizeof(x));
    int ok = 1;
    for (size_t i = 0; i < sizeof(b) / sizeof(b[0]); i++) {
        md5_ctx c;
        uint8_t d[16];
        md5_init(&c);
        /* fed in uneven pieces: 1, 2, 3, ... bytes */
        int left = b[i].n, step = 1, off = 0;
        while (left > 0) {
            int k = step < left ? step : left;
            md5_update(&c, x + off, (size_t)k);
            off += k; left -= k; step++;
        }
        md5_final(&c, d);
        char h[33];
        for (int j = 0; j < 16; j++) sprintf(h + 2 * j, "%02x", d[j]);
        if (strcmp(h, b[i].hex) != 0) { ok = 0; printf("       length %d gives %s\n", b[i].n, h); }
    }
    CHECK(ok, "padding boundaries 55..128 bytes, fed in uneven pieces");
    md5_ctx c;
    uint8_t d[16];
    char big[1000];
    memset(big, 'a', sizeof(big));
    md5_init(&c);
    for (int i = 0; i < 1000; i++) md5_update(&c, big, sizeof(big));
    md5_final(&c, d);
    char h[33];
    for (int j = 0; j < 16; j++) sprintf(h + 2 * j, "%02x", d[j]);
    CHECK(strcmp(h, "7707d6ae4e027c70eea2a935c2296f21") == 0, "one million 'a'");
}

static void test_digest_vectors(void)
{
    printf("Digest response (RFC 2617 section 3.5, RFC 2069)\n");
    char ha1[33], r[33];
    md5_hex_cat(ha1, "Mufasa", ":", "testrealm@host.com", ":", "Circle Of Life", (char *)NULL);
    CHECK(strcmp(ha1, "939e7578ed9e3c518a452acee763bce9") == 0, "HA1 of the RFC 2617 example");
    rtsp_auth_response(r, ha1, "dcd98b7102dd2f0e8b11d0f600bfb0c093", "00000001", "0a4f113b", "GET", "/dir/index.html");
    CHECK(strcmp(r, "6629fae49393a05397450978507c4ef1") == 0, "RFC 2617 response (qop=auth)");
    rtsp_auth_response(r, ha1, "dcd98b7102dd2f0e8b11d0f600bfb0c093", NULL, NULL, "GET", "/dir/index.html");
    CHECK(strcmp(r, "670fd8c2df070c60b045671b8b24ff02") == 0, "RFC 2069 response (no qop)");
}

static void test_file_format(void)
{
    printf("Credentials file format\n");
    char u[33], h[33];
    const char *h1 = "0123456789abcdef0123456789abcdef";
    char line[200];
    snprintf(line, sizeof(line), "cam:akam:%s\n", h1);
    CHECK(rtsp_auth_parse_file(line, strlen(line), u, h) == 0 && strcmp(u, "cam") == 0 && strcmp(h, h1) == 0,
          "valid line with LF");
    snprintf(line, sizeof(line), "cam:akam:%s\r\n", h1);
    CHECK(rtsp_auth_parse_file(line, strlen(line), u, h) == 0, "CRLF accepted");
    snprintf(line, sizeof(line), "a.b_c@d+e-1:akam:%s", h1);
    CHECK(rtsp_auth_parse_file(line, strlen(line), u, h) == 0, "no final newline, allowed user characters");
    snprintf(line, sizeof(line), "cam:other:%s\n", h1);
    CHECK(rtsp_auth_parse_file(line, strlen(line), u, h) != 0, "wrong realm refused");
    snprintf(line, sizeof(line), "cam:akam:%.31s\n", h1);
    CHECK(rtsp_auth_parse_file(line, strlen(line), u, h) != 0, "short hash refused");
    snprintf(line, sizeof(line), "cam:akam:0123456789ABCDEF0123456789abcdef\n");
    CHECK(rtsp_auth_parse_file(line, strlen(line), u, h) != 0, "uppercase hash refused");
    snprintf(line, sizeof(line), "cam:akam:%s\nbob:akam:%s\n", h1, h1);
    CHECK(rtsp_auth_parse_file(line, strlen(line), u, h) != 0, "second line refused");
    snprintf(line, sizeof(line), "ca m:akam:%s\n", h1);
    CHECK(rtsp_auth_parse_file(line, strlen(line), u, h) != 0, "space in the user refused");
    snprintf(line, sizeof(line), "c\"am:akam:%s\n", h1);
    CHECK(rtsp_auth_parse_file(line, strlen(line), u, h) != 0, "quote in the user refused");
    snprintf(line, sizeof(line), ":akam:%s\n", h1);
    CHECK(rtsp_auth_parse_file(line, strlen(line), u, h) != 0, "empty user refused");
    snprintf(line, sizeof(line), "%033d:akam:%s\n", 0, h1);
    CHECK(rtsp_auth_parse_file(line, strlen(line), u, h) != 0, "33-character user refused");
    snprintf(line, sizeof(line), "cam:akam:%s:x\n", h1);
    CHECK(rtsp_auth_parse_file(line, strlen(line), u, h) != 0, "extra field refused");
    CHECK(rtsp_auth_parse_file("", 0, u, h) != 0, "empty file refused");
}

static void test_state_machine(void)
{
    printf("State machine\n");
    rtsp_auth_conn_t c;
    char nonce[33], hdr[1024];
    rtsp_auth_db_init(&db, path);
    rtsp_auth_rng = fake_rng;
    unlink(path);
    now = 1000000;

    rtsp_auth_conn_init(&c);
    req(&c, "DESCRIBE", NULL);
    CHECK(res.verdict == RTSP_AUTH_ALLOW, "no file: DESCRIBE served without credentials");
    CHECK(rtsp_auth_session_ok(&db, &c), "no file: session stays valid");

    put_creds(USER, PASSWORD);
    md5_hex_cat(ha1_hex, USER, ":" RTSP_AUTH_REALM ":", PASSWORD, (char *)NULL);
    rtsp_auth_conn_init(&c);
    req(&c, "OPTIONS", NULL);
    CHECK(res.verdict == RTSP_AUTH_ALLOW, "OPTIONS stays open");
    req(&c, "DESCRIBE", NULL);
    CHECK(res.verdict == RTSP_AUTH_DENY, "file present: DESCRIBE without credentials -> 401");
    CHECK(strncmp(res.challenge, "Digest realm=\"akam\", nonce=\"", 28) == 0 &&
          strstr(res.challenge, "algorithm=MD5") && !strstr(res.challenge, "qop") &&
          !strstr(res.challenge, "stale"), "challenge: realm, nonce, algorithm=MD5, no qop");
    CHECK(nonce_of(&res, nonce) && strlen(nonce) == 32, "challenge carries a 32-hex nonce");
    CHECK(res.delay_ms == 0 && res.log[0] == '\0', "plain challenge: no delay, nothing logged");
    CHECK(!rtsp_auth_session_ok(&db, &c), "unauthenticated connection is not valid once auth is on");

    const char *uri = "rtsp://192.0.2.50:554/";
    hdr_digest(hdr, sizeof(hdr), USER, PASSWORD, nonce, "DESCRIBE", uri, NULL, NULL);
    req(&c, "DESCRIBE", hdr);
    CHECK(res.verdict == RTSP_AUTH_ALLOW, "right response -> 200 path");
    CHECK(strstr(res.log, "authenticated") != NULL, "first success is journaled");
    req(&c, "SETUP", NULL);
    CHECK(res.verdict == RTSP_AUTH_ALLOW, "later request of an authenticated connection without header");
    CHECK(rtsp_auth_session_ok(&db, &c), "authenticated connection is valid");
    hdr_digest(hdr, sizeof(hdr), USER, PASSWORD, nonce, "PLAY", uri, NULL, NULL);
    req(&c, "PLAY", hdr);
    CHECK(res.verdict == RTSP_AUTH_ALLOW && res.log[0] == '\0', "PLAY with a fresh header also passes (not re-journaled)");

    /* A header for another method does not authorise this one. */
    rtsp_auth_conn_t c2;
    rtsp_auth_conn_init(&c2);
    req(&c2, "DESCRIBE", NULL);
    nonce_of(&res, nonce);
    hdr_digest(hdr, sizeof(hdr), USER, PASSWORD, nonce, "DESCRIBE", uri, NULL, NULL);
    req(&c2, "SETUP", hdr);
    CHECK(res.verdict == RTSP_AUTH_DENY && strstr(res.log, "bad credentials"), "response computed for another method refused");

    /* Wrong password, then wrong user: same answer, counted, slowed down. */
    rtsp_auth_conn_t w;
    rtsp_auth_conn_init(&w);
    now += 5 * 1000000LL;
    db.global_fails = 0;
    req(&w, "DESCRIBE", NULL);
    nonce_of(&res, nonce);
    hdr_digest(hdr, sizeof(hdr), USER, "wrong-password", nonce, "DESCRIBE", uri, NULL, NULL);
    req(&w, "DESCRIBE", hdr);
    CHECK(res.verdict == RTSP_AUTH_DENY && res.delay_ms == 0, "wrong password -> 401, first attempt not delayed");
    char nonce2[33];
    CHECK(nonce_of(&res, nonce2) && strcmp(nonce, nonce2) == 0, "the same nonce is kept for the retry");
    hdr_digest(hdr, sizeof(hdr), "not_the_user", PASSWORD, nonce, "DESCRIBE", uri, NULL, NULL);
    req(&w, "DESCRIBE", hdr);
    CHECK(res.verdict == RTSP_AUTH_DENY && strstr(res.log, "bad credentials"), "wrong user, right password -> same 401");
    hdr_digest(hdr, sizeof(hdr), USER, "wrong-password", nonce, "DESCRIBE", uri, NULL, NULL);
    req(&w, "DESCRIBE", hdr);
    CHECK(res.verdict == RTSP_AUTH_DENY && res.delay_ms == 500, "third failure: 500 ms delay");
    req(&w, "DESCRIBE", hdr);
    CHECK(res.delay_ms == 1000, "fourth failure: 1 s delay");
    req(&w, "DESCRIBE", hdr);
    CHECK(res.verdict == RTSP_AUTH_DROP && res.delay_ms == 2000, "fifth failure: connection closed, 2 s delay");
    /* The delay survives a reconnect (process-wide counter) ... */
    rtsp_auth_conn_init(&w);
    req(&w, "DESCRIBE", NULL);
    nonce_of(&res, nonce);
    hdr_digest(hdr, sizeof(hdr), USER, "wrong-password", nonce, "DESCRIBE", uri, NULL, NULL);
    req(&w, "DESCRIBE", hdr);
    CHECK(res.verdict == RTSP_AUTH_DENY && res.delay_ms == 2000, "a new connection inherits the delay");
    /* ... and is forgotten after a quiet minute. */
    now += RTSP_AUTH_FAIL_WINDOW_US + 1;
    rtsp_auth_conn_init(&w);
    req(&w, "DESCRIBE", NULL);
    nonce_of(&res, nonce);
    hdr_digest(hdr, sizeof(hdr), USER, "wrong-password", nonce, "DESCRIBE", uri, NULL, NULL);
    req(&w, "DESCRIBE", hdr);
    CHECK(res.delay_ms == 0, "counter forgotten after the quiet window");
    hdr_digest(hdr, sizeof(hdr), USER, PASSWORD, nonce, "DESCRIBE", uri, NULL, NULL);
    req(&w, "DESCRIBE", hdr);
    CHECK(res.verdict == RTSP_AUTH_ALLOW, "right password after a typo");

    /* Stale nonce: right credentials, nonce older than its lifetime. */
    rtsp_auth_conn_t s;
    rtsp_auth_conn_init(&s);
    req(&s, "DESCRIBE", NULL);
    nonce_of(&res, nonce);
    now += RTSP_AUTH_NONCE_LIFETIME_US + 1;
    hdr_digest(hdr, sizeof(hdr), USER, PASSWORD, nonce, "DESCRIBE", uri, NULL, NULL);
    req(&s, "DESCRIBE", hdr);
    CHECK(res.verdict == RTSP_AUTH_DENY && strstr(res.challenge, "stale=true") && res.delay_ms == 0,
          "expired nonce -> 401 stale=true, no penalty");
    CHECK(nonce_of(&res, nonce2) && strcmp(nonce, nonce2) != 0, "a new nonce is issued");
    hdr_digest(hdr, sizeof(hdr), USER, PASSWORD, nonce2, "DESCRIBE", uri, NULL, NULL);
    req(&s, "DESCRIBE", hdr);
    CHECK(res.verdict == RTSP_AUTH_ALLOW, "retry with the new nonce passes");
    /* An authenticated connection keeps working past the nonce lifetime. */
    now += 10 * RTSP_AUTH_NONCE_LIFETIME_US;
    hdr_digest(hdr, sizeof(hdr), USER, PASSWORD, nonce2, "GET_PARAMETER", uri, NULL, NULL);
    req(&s, "GET_PARAMETER", hdr);
    CHECK(res.verdict == RTSP_AUTH_ALLOW, "keepalive long after authentication still passes");

    /* A nonce this connection never issued (replay from another connection). */
    rtsp_auth_conn_t u;
    rtsp_auth_conn_init(&u);
    req(&u, "DESCRIBE", NULL);
    hdr_digest(hdr, sizeof(hdr), USER, PASSWORD, nonce2, "DESCRIBE", uri, NULL, NULL);
    req(&u, "DESCRIBE", hdr);
    CHECK(res.verdict == RTSP_AUTH_DENY && strstr(res.challenge, "stale=true") && res.delay_ms == 0,
          "header replayed on another connection -> challenged again, never served");
    CHECK(u.authed == false, "the replaying connection is not authenticated");

    /* Lower-case/upper-case hex of the response. */
    rtsp_auth_conn_t k;
    rtsp_auth_conn_init(&k);
    req(&k, "DESCRIBE", NULL);
    nonce_of(&res, nonce);
    hdr_digest(hdr, sizeof(hdr), USER, PASSWORD, nonce, "DESCRIBE", uri, NULL, NULL);
    for (char *p = strstr(hdr, "response=\"") + 10; *p != '"'; p++)
        if (*p >= 'a' && *p <= 'f') *p = (char)(*p - 32);
    req(&k, "DESCRIBE", hdr);
    CHECK(res.verdict == RTSP_AUTH_ALLOW, "upper-case hex response accepted");

    /* qop=auth responses (clients that answer with it anyway). */
    rtsp_auth_conn_t q;
    rtsp_auth_conn_init(&q);
    req(&q, "DESCRIBE", NULL);
    nonce_of(&res, nonce);
    hdr_digest(hdr, sizeof(hdr), USER, PASSWORD, nonce, "DESCRIBE", uri, "00000001", "0a4f113b");
    req(&q, "DESCRIBE", hdr);
    CHECK(res.verdict == RTSP_AUTH_ALLOW, "qop=auth response accepted");
    q.authed = false;   /* force verification again */
    req(&q, "DESCRIBE", hdr);
    CHECK(res.verdict == RTSP_AUTH_DENY && strstr(res.log, "nc"), "same nc again refused (replay)");
    hdr_digest(hdr, sizeof(hdr), USER, PASSWORD, nonce, "DESCRIBE", uri, "00000002", "0a4f113b");
    req(&q, "DESCRIBE", hdr);
    CHECK(res.verdict == RTSP_AUTH_ALLOW, "next nc accepted");

    /* Malformed headers: refused, never a crash. */
    static const char *bad[] = {
        "Basic dXNlcjpwYXNz", "Digest", "Digest ", "Digest username=\"cam_user\"",
        "Digest username=\"cam_user, nonce=\"x\"", "Digest username", "Digest =x",
        "Digest username=\"a\", username=\"b\", nonce=\"n\", uri=\"/\", response=\"r\"",
        "Digest username=\"cam_user\", nonce=\"n\", uri=\"/\", response=\"short\"",
    };
    int all = 1;
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        rtsp_auth_conn_t m;
        rtsp_auth_conn_init(&m);
        db.global_fails = 0;
        req(&m, "DESCRIBE", bad[i]);
        if (res.verdict == RTSP_AUTH_ALLOW) { all = 0; printf("       served: %s\n", bad[i]); }
    }
    CHECK(all, "malformed / foreign Authorization headers never served");
    char huge[4096];
    memset(huge, 'a', sizeof(huge));
    memcpy(huge, "Digest username=\"", 17);
    huge[sizeof(huge) - 1] = '\0';
    rtsp_auth_conn_t m;
    rtsp_auth_conn_init(&m);
    req(&m, "DESCRIBE", huge);
    CHECK(res.verdict != RTSP_AUTH_ALLOW, "oversized header refused");
    snprintf(hdr, sizeof(hdr), "Digest username=\"%s\", nonce=\"n\", uri=\"/\", response=\"r\", algorithm=SHA-256", USER);
    rtsp_auth_conn_init(&m);
    req(&m, "DESCRIBE", hdr);
    CHECK(res.verdict != RTSP_AUTH_ALLOW, "other algorithm refused");

    /* No random source: refuse (fail closed), do not serve. */
    rng_fail = 1;
    rtsp_auth_conn_init(&m);
    req(&m, "DESCRIBE", NULL);
    CHECK(res.verdict == RTSP_AUTH_DROP, "no randomness: refused");
    rng_fail = 0;
}

static void test_reload(void)
{
    printf("File reload\n");
    rtsp_auth_conn_t c, d;
    char nonce[33], hdr[1024];
    const char *uri = "rtsp://cam/";
    now += 100 * 1000000LL;
    put_creds(USER, PASSWORD);
    rtsp_auth_conn_init(&c);
    req(&c, "DESCRIBE", NULL);
    nonce_of(&res, nonce);
    hdr_digest(hdr, sizeof(hdr), USER, PASSWORD, nonce, "DESCRIBE", uri, NULL, NULL);
    req(&c, "DESCRIBE", hdr);
    CHECK(res.verdict == RTSP_AUTH_ALLOW, "logged in");
    unsigned gen = db.generation;
    put_creds(USER, PASSWORD);
    req(&c, "SETUP", NULL);
    CHECK(res.verdict == RTSP_AUTH_ALLOW && db.generation == gen, "file rewritten with the same credentials: nothing changes");

    put_creds(USER, "another-password-1");
    CHECK(!rtsp_auth_session_ok(&db, &c), "new password: the running session is no longer valid");
    req(&c, "PLAY", NULL);
    CHECK(res.verdict == RTSP_AUTH_DENY, "new password: request without credentials challenged");
    nonce_of(&res, nonce);
    hdr_digest(hdr, sizeof(hdr), USER, PASSWORD, nonce, "PLAY", uri, NULL, NULL);
    req(&c, "PLAY", hdr);
    CHECK(res.verdict == RTSP_AUTH_DENY && strstr(res.log, "bad credentials"), "old password refused");
    hdr_digest(hdr, sizeof(hdr), USER, "another-password-1", nonce, "PLAY", uri, NULL, NULL);
    req(&c, "PLAY", hdr);
    CHECK(res.verdict == RTSP_AUTH_ALLOW, "new password accepted without restart");

    put_file("garbage without structure\n");
    CHECK(rtsp_auth_db_refresh(&db) == RTSP_AUTH_BROKEN, "garbage file -> BROKEN");
    req(&c, "PLAY", NULL);
    CHECK(res.verdict == RTSP_AUTH_DENY && strstr(res.log, "invalid"), "BROKEN: refuses even an authenticated connection");
    nonce_of(&res, nonce);
    hdr_digest(hdr, sizeof(hdr), USER, "another-password-1", nonce, "PLAY", uri, NULL, NULL);
    req(&c, "PLAY", hdr);
    CHECK(res.verdict == RTSP_AUTH_DENY, "BROKEN: the old credentials do not work");
    CHECK(!rtsp_auth_session_ok(&db, &c), "BROKEN: the running session is cut");
    req(&c, "OPTIONS", NULL);
    CHECK(res.verdict == RTSP_AUTH_ALLOW, "BROKEN: OPTIONS still answered");
    put_file("");
    CHECK(rtsp_auth_db_refresh(&db) == RTSP_AUTH_BROKEN, "empty file -> BROKEN");
    {
        char big[600];
        memset(big, 'a', sizeof(big));
        big[sizeof(big) - 1] = '\0';
        put_file(big);
        CHECK(rtsp_auth_db_refresh(&db) == RTSP_AUTH_BROKEN, "oversized file -> BROKEN");
    }
    put_creds(USER, PASSWORD);
    CHECK(rtsp_auth_db_refresh(&db) == RTSP_AUTH_ON, "repaired file -> ON again");

    {   /* an unchanged old file is not opened again: a content change that leaves inode, size and mtime alone is not seen */
        struct stat st;
        CHECK(stat(path, &st) == 0, "stat");
        struct timeval old[2] = { { st.st_mtime - 3600, 0 }, { st.st_mtime - 3600, 0 } };
        CHECK(utimes(path, old) == 0, "age the file");
        CHECK(rtsp_auth_db_refresh(&db) == RTSP_AUTH_ON, "aged file read");
        int fd = open(path, O_WRONLY);
        CHECK(fd >= 0 && write(fd, "!!!!!!!", 7) == 7, "overwrite in place");
        close(fd);
        CHECK(utimes(path, old) == 0, "restore the old mtime");
        CHECK(rtsp_auth_db_refresh(&db) == RTSP_AUTH_ON, "same inode, size and mtime: the cached answer, no re-read");
        struct timeval old2[2] = { { st.st_mtime - 1800, 0 }, { st.st_mtime - 1800, 0 } };
        CHECK(utimes(path, old2) == 0, "touch the file");
        CHECK(rtsp_auth_db_refresh(&db) == RTSP_AUTH_BROKEN, "a changed mtime makes it read again");
        put_creds(USER, PASSWORD);
        CHECK(rtsp_auth_db_refresh(&db) == RTSP_AUTH_ON, "a file modified within the last 2 s is always read again");
        put_file("");
        CHECK(rtsp_auth_db_refresh(&db) == RTSP_AUTH_BROKEN, "...even when the size changes at once");
        put_creds(USER, PASSWORD);
    }
    unlink(path);
    rtsp_auth_conn_init(&d);
    req(&d, "DESCRIBE", NULL);
    CHECK(res.verdict == RTSP_AUTH_ALLOW, "file removed: open again, no restart");
    CHECK(rtsp_auth_session_ok(&db, &d) && rtsp_auth_session_ok(&db, &c), "file removed: no session is cut");
    put_creds(USER, PASSWORD);
    CHECK(!rtsp_auth_session_ok(&db, &d), "auth enabled while a client streams: that session is cut");
    unlink(path);
}

static void test_logs(void)
{
    printf("No secret in the logs\n");
    CHECK(strstr(transcript, PASSWORD) == NULL, "password never appears in a log line or challenge");
    CHECK(strstr(transcript, "S3NTINEL") == NULL, "sentinel never appears");
    CHECK(strstr(transcript, ha1_hex) == NULL, "HA1 never appears");
    CHECK(strstr(transcript, USER) == NULL, "user name never appears");
    CHECK(strstr(transcript, "another-password") == NULL, "other passwords never appear");
    CHECK(strstr(transcript, "response=") == NULL, "no Authorization content in the log");
    CHECK(strstr(transcript, "bad credentials") != NULL, "failures are logged (reason only)");

    char out[256];
    rtsp_auth_redact_uri("rtsp://admin:" PASSWORD "@192.0.2.50:554/stream", out, sizeof(out));
    CHECK(strcmp(out, "rtsp://***@192.0.2.50:554/stream") == 0, "URI with user:password@ redacted");
    rtsp_auth_redact_uri("rtsp://adm@in:p@ss@host/", out, sizeof(out));
    CHECK(strcmp(out, "rtsp://***@host/") == 0, "'@' inside the password");
    rtsp_auth_redact_uri("rtsp://192.0.2.50:554/a@b", out, sizeof(out));
    CHECK(strcmp(out, "rtsp://192.0.2.50:554/a@b") == 0, "'@' in the path left alone");
    rtsp_auth_redact_uri("*", out, sizeof(out));
    CHECK(strcmp(out, "*") == 0, "plain URI unchanged");
    rtsp_auth_redact_uri("rtsp://u:p@h/", out, 8);
    CHECK(strlen(out) == 7, "output truncated, never overflowed");
}

int main(void)
{
    snprintf(path, sizeof(path), "/tmp/rtsp_auth_test.%d", (int)getpid());
    test_md5();
    test_digest_vectors();
    test_file_format();
    test_state_machine();
    test_reload();
    test_logs();
    unlink(path);
    printf("%s (%d failure%s)\n", fails ? "FAILED" : "PASSED", fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
