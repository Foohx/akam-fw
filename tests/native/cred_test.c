/* Host test of src/ak_rtsp/cred.c: the credentials file (format, permissions,
 * reload), WS-Security PasswordDigest (the vector of the WS-Security
 * UsernameToken profile and one checked with openssl, replay cache, Created
 * window with an injected clock, malformed input), the shared failure
 * limiter and the HTTP Digest hook. Real files in a temporary directory.
 * Build/run: tests/cred_test.sh <temp-dir> */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "b64.h"
#include "cred.h"
#include "md5.h"
#include "rtsp_auth.h"
#include "sha1.h"

static int fails;
#define CHECK(cond, msg) do { if (cond) printf("  ok   - %s\n", msg); \
    else { printf("  FAIL - %s (line %d)\n", msg, __LINE__); fails++; } } while (0)

static char dir[256], cpath[300], apath[300];
static int64_t wall, mono = 1000000000LL;
static int64_t fake_wall(void) { return wall; }
static int64_t fake_mono(void) { return mono; }
static rtsp_auth_db_t limiter;

static void put(const char *path, const char *content, mode_t mode)
{
    char tmp[320];
    snprintf(tmp, sizeof(tmp), "%s.new", path);
    unlink(tmp);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) { perror("open"); exit(2); }
    if (write(fd, content, strlen(content)) < 0) exit(2);
    fchmod(fd, mode);
    close(fd);
    rename(tmp, path);
}
static void put_creds(const char *user, const char *pass)
{
    char buf[400];
    snprintf(buf, sizeof(buf), "user=%s\npass=%s\n", user, pass);
    put(cpath, buf, 0600);
}
/* every test starts from an empty directory state and a fresh store */
static void fresh(void)
{
    unlink(cpath);
    unlink(apath);
    rtsp_auth_db_init(&limiter, apath);
    cred_init(cpath, &limiter);
    mono += 100000000LL;
}
static void later(void) { mono += 4LL * 1000000; } /* past CRED_RECHECK_US */

/* Digest as a client computes it (own sha1/b64, themselves checked by vectors below). */
static void mkdigest(const char *nonce_b64, const char *created, const char *pass, char out[64])
{
    uint8_t raw[64], d[20];
    long n = b64_decode(nonce_b64, strlen(nonce_b64), raw, sizeof(raw));
    sha1_ctx c;
    sha1_init(&c);
    sha1_update(&c, raw, (size_t)n);
    sha1_update(&c, created, strlen(created));
    sha1_update(&c, pass, strlen(pass));
    sha1_final(&c, d);
    b64_encode(d, 20, out, 64);
}
static int64_t utc(int y, int mo, int d, int h, int mi, int s)
{
    struct tm t = { .tm_year = y - 1900, .tm_mon = mo - 1, .tm_mday = d, .tm_hour = h, .tm_min = mi, .tm_sec = s };
    return (int64_t)timegm(&t);
}
static int wsse(const char *u, const char *nonce, const char *created, const char *pass, unsigned *delay)
{
    char dg[64];
    mkdigest(nonce, created, pass, dg);
    return cred_check_wsse(u, nonce, created, dg, delay);
}
static void nonce_n(int i, char out[32])
{
    uint8_t raw[16];
    memset(raw, 0x5a, sizeof(raw));
    raw[0] = (uint8_t)i;
    raw[1] = (uint8_t)(i >> 8);
    b64_encode(raw, sizeof(raw), out, 32);
}

#define SPEC_NONCE "LKqI6G/AikKCQrN0zqZFlg=="
#define SPEC_CREATED "2010-09-16T07:50:45Z"
#define SPEC_PASS "userpassword"
#define SPEC_DIGEST "tuOSpGlFlIXsozq4HFNeeGeFLEI="

static void test_parse(void)
{
    printf("credentials file format\n");
    char u[CRED_USER_MAX + 1], p[CRED_PASS_MAX + 1];
#define PARSE(s) cred_parse((s), strlen(s), u, p)
    CHECK(PARSE("user=viewer\npass=Correct-Horse-9\n") == 0 && !strcmp(u, "viewer") && !strcmp(p, "Correct-Horse-9"), "valid file");
    CHECK(PARSE("user=viewer\npass=Correct-Horse-9") == 0, "no final newline");
    CHECK(PARSE("pass=abcdefgh\nuser=bob.1_@+-\n") == 0 && !strcmp(u, "bob.1_@+-"), "any key order, full user alphabet");
    CHECK(PARSE("# written by akam\n\nuser=a\n\npass=b\n") == 0, "comments and blank lines");
    CHECK(PARSE("user=a\npass= a=b #c \n") == 0 && !strcmp(p, " a=b #c "), "password taken literally (spaces, '=', '#')");
    CHECK(PARSE("user=a\npass=pa ss\xc3\xa9\n") == 0, "UTF-8 bytes in the password");
    CHECK(PARSE("user=a\n") != 0, "missing pass");
    CHECK(PARSE("pass=abcdefgh\n") != 0, "missing user");
    CHECK(PARSE("") != 0, "empty file");
    CHECK(PARSE("user=a\nuser=b\npass=c\n") != 0, "duplicate user");
    CHECK(PARSE("user=a\npass=c\npass=d\n") != 0, "duplicate pass");
    CHECK(PARSE("user=a\npass=c\nextra=1\n") != 0, "unknown key");
    CHECK(PARSE("user=a\npass=c\nfoo\n") != 0, "line without '='");
    CHECK(PARSE("user=a\npass=\n") != 0, "empty password");
    CHECK(PARSE("user=\npass=c\n") != 0, "empty user");
    CHECK(PARSE("user=a b\npass=c\n") != 0, "space in the user");
    CHECK(PARSE("user=a:b\npass=c\n") != 0, "colon in the user");
    CHECK(PARSE("user=a\r\npass=c\n") != 0, "CR in the user (not LF-only)");
    CHECK(PARSE("user=a\npass=c\r\n") != 0, "CR in the password");
    CHECK(PARSE("user=a\npass=c\td\n") != 0, "control byte in the password");
    CHECK(PARSE("user=a\npass=c\x7f\n") != 0, "DEL in the password");
    CHECK(PARSE(" user=a\npass=c\n") != 0, "leading space before a key");
    CHECK(PARSE("USER=a\npass=c\n") != 0, "keys are lowercase");
    char big[600];
    memset(big, 'u', sizeof(big));
    memcpy(big, "user=", 5);
    snprintf(big + 5 + CRED_USER_MAX, sizeof(big) - 5 - CRED_USER_MAX, "\npass=c\n");
    CHECK(PARSE(big) == 0, "user of exactly 32 characters");
    snprintf(big + 5 + CRED_USER_MAX + 1, sizeof(big) - 6 - CRED_USER_MAX, "x\npass=c\n");
    CHECK(PARSE(big) != 0, "user of 33 characters");
    char pw[CRED_PASS_MAX + 2];
    memset(pw, 'p', sizeof(pw));
    pw[CRED_PASS_MAX] = '\0';
    char line[400];
    snprintf(line, sizeof(line), "user=a\npass=%s\n", pw);
    CHECK(PARSE(line) == 0, "password of exactly 128 bytes");
    pw[CRED_PASS_MAX] = 'p';
    pw[CRED_PASS_MAX + 1] = '\0';
    snprintf(line, sizeof(line), "user=a\npass=%s\n", pw);
    CHECK(PARSE(line) != 0, "password of 129 bytes");
    memset(big, '#', sizeof(big));
    memcpy(big, "user=a\npass=c\n#", 15);
    CHECK(cred_parse(big, CRED_FILE_MAX, u, p) == 0 && cred_parse(big, CRED_FILE_MAX + 1, u, p) != 0, "file size limit 512");
    CHECK(cred_parse("user=a\0b\npass=c\n", 16, u, p) != 0, "NUL byte");
}

static void test_file(void)
{
    printf("credentials file on disk, permissions, reload\n");
    fresh();
    CHECK(cred_refresh(true) == CRED_ABSENT && !cred_have_plain() && cred_get_user() == NULL, "absent file: no plain credentials");
    put_creds("viewer", "Correct-Horse-9");
    CHECK(cred_refresh(true) == CRED_OK && cred_have_plain() && !strcmp(cred_get_user(), "viewer"), "valid 0600 file");
    CHECK(cred_problem()[0] == '\0', "no problem reported");

    /* cheap polling: a change inside the recheck interval is not seen */
    put_creds("other", "Correct-Horse-9");
    mono += 1000000;
    CHECK(!strcmp(cred_get_user(), "viewer"), "no look at the disk within 3 s");
    later();
    CHECK(!strcmp(cred_get_user(), "other"), "change seen after the interval (atomic rename, same size)");

    put_creds("other", "A-much-longer-password-1");
    later();
    CHECK(cred_have_plain(), "new password loaded");
    unlink(cpath);
    later();
    CHECK(cred_refresh(false) == CRED_ABSENT && cred_get_user() == NULL, "removal seen: no plain credentials");
    put_creds("back", "Correct-Horse-9");
    later();
    CHECK(!strcmp(cred_get_user(), "back"), "file back");

    /* in-place edit that keeps size and inode: mtime must tell */
    FILE *f = fopen(cpath, "r+");
    fseek(f, 5, SEEK_SET);
    fputs("BACK", f);
    fclose(f);
    struct timespec ts[2] = { { 1700000000, 0 }, { 1700000123, 0 } };
    utimensat(AT_FDCWD, cpath, ts, 0);
    later();
    CHECK(!strcmp(cred_get_user(), "BACK"), "in-place edit seen through the mtime");

    /* permissions */
    put("x", "", 0600);
    unlink("x");
    char buf[400];
    snprintf(buf, sizeof(buf), "user=v\npass=Correct-Horse-9\n");
    mode_t bad[] = { 0640, 0604, 0644, 0660, 0666, 0777, 0620 };
    int ok = 1;
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        put(cpath, buf, bad[i]);
        later();
        if (cred_refresh(true) != CRED_INVALID || cred_have_plain() || cred_get_user() != NULL) {
            printf("    mode %o accepted\n", (unsigned)bad[i]);
            ok = 0;
        }
    }
    CHECK(ok, "group/other access refused (fail closed)");
    CHECK(strstr(cred_problem(), "permissions") != NULL, "the problem says permissions, no secret in it");
    CHECK(strstr(cred_problem(), "Correct") == NULL, "problem text never holds the password");
    put(cpath, buf, 0400);
    later();
    CHECK(cred_refresh(true) == CRED_OK, "mode 0400 is fine");
    put(cpath, buf, 0600);
    later();
    CHECK(cred_refresh(false) == CRED_OK, "fixed permissions accepted again");

    put(cpath, "user=v\npass=Correct-Horse-9\nextra=1\n", 0600);
    later();
    CHECK(cred_refresh(false) == CRED_INVALID && strstr(cred_problem(), "format"), "bad format: invalid");
    put(cpath, "", 0600);
    later();
    CHECK(cred_refresh(false) == CRED_INVALID, "empty file: invalid");
    char huge[2000];
    memset(huge, '#', sizeof(huge) - 1);
    huge[sizeof(huge) - 1] = '\0';
    put(cpath, huge, 0600);
    later();
    CHECK(cred_refresh(false) == CRED_INVALID && strstr(cred_problem(), "large"), "oversized file: invalid");
    unlink(cpath);
    char target[320];
    snprintf(target, sizeof(target), "%s/real", dir);
    put(target, buf, 0600);
    symlink(target, cpath);
    later();
    CHECK(cred_refresh(true) == CRED_INVALID, "a symbolic link is refused");
    unlink(cpath);
    unlink(target);
    mkdir(cpath, 0700);
    later();
    CHECK(cred_refresh(true) == CRED_INVALID, "a directory is refused");
    rmdir(cpath);
    put(cpath, buf, 0600);
    later();
    CHECK(cred_refresh(false) == CRED_OK, "recovers once the file is right");
}

static void test_wsse(void)
{
    printf("WS-Security UsernameToken PasswordDigest\n");
    unsigned delay = 99;
    fresh();
    put_creds("user", SPEC_PASS);
    wall = utc(2010, 9, 16, 7, 50, 45);      /* a clock that "looks" unset (before 2024) */
    CHECK(cred_check_wsse("user", SPEC_NONCE, SPEC_CREATED, SPEC_DIGEST, &delay) == 1 && delay == 0,
          "WS-Security profile example (nonce LKqI6G/..., created 2010-09-16T07:50:45Z, password userpassword)");
    CHECK(cred_check_wsse("user", SPEC_NONCE, SPEC_CREATED, SPEC_DIGEST, &delay) == 0, "the same message again is a replay");

    /* second vector, computed with openssl: odd password (space, '=', '#', ':', UTF-8), offset and fraction */
    fresh();
    put_creds("viewer", "pa ss=w#rd:\xc3\xa9");
    wall = utc(2026, 11, 10, 11, 0, 0);
    CHECK(cred_check_wsse("viewer", "amgFZ3FtEnGcAQ5jX9R/jq+2MMrSMbEo", "2026-11-10T12:00:00.250+01:00",
                          "fTU84AWjSpXbXFJRx785UbQ770c=", NULL) == 1,
          "openssl-checked vector (24-byte nonce, +01:00 zone, fraction, odd password) inside the window");

    /* user name */
    fresh();
    put_creds("user", SPEC_PASS);
    wall = 0;
    CHECK(wsse("User", "bm9uY2UtMDAwMDAwMDAwMQ==", SPEC_CREATED, SPEC_PASS, NULL) == 0, "wrong user (case)");
    CHECK(wsse("", "bm9uY2UtMDAwMDAwMDAwMQ==", SPEC_CREATED, SPEC_PASS, NULL) == 0, "empty user");
    CHECK(wsse("userx", "bm9uY2UtMDAwMDAwMDAwMQ==", SPEC_CREATED, SPEC_PASS, NULL) == 0, "longer user");
    CHECK(wsse("use", "bm9uY2UtMDAwMDAwMDAwMQ==", SPEC_CREATED, SPEC_PASS, NULL) == 0, "shorter user");
    char longu[200];
    memset(longu, 'u', sizeof(longu) - 1);
    longu[sizeof(longu) - 1] = '\0';
    CHECK(wsse(longu, "bm9uY2UtMDAwMDAwMDAwMQ==", SPEC_CREATED, SPEC_PASS, NULL) == 0, "very long user (no overflow)");
    CHECK(wsse("user", "bm9uY2UtMDAwMDAwMDAwMQ==", SPEC_CREATED, "wrong-password", NULL) == 0, "wrong password");
    CHECK(wsse("user", "bm9uY2UtMDAwMDAwMDAwMQ==", SPEC_CREATED, SPEC_PASS, NULL) == 1, "right one afterwards (failures did not poison the nonce cache)");

    /* malformed inputs: every one refused, none crashes */
    fresh();
    put_creds("user", SPEC_PASS);
    char dg[64], n1[32];
    nonce_n(1, n1);
    mkdigest(n1, SPEC_CREATED, SPEC_PASS, dg);
    CHECK(cred_check_wsse(NULL, n1, SPEC_CREATED, dg, NULL) == 0, "NULL user");
    CHECK(cred_check_wsse("user", NULL, SPEC_CREATED, dg, NULL) == 0, "NULL nonce");
    CHECK(cred_check_wsse("user", n1, NULL, dg, NULL) == 0, "NULL created");
    CHECK(cred_check_wsse("user", n1, SPEC_CREATED, NULL, NULL) == 0, "NULL digest");
    CHECK(cred_check_wsse("user", "", SPEC_CREATED, dg, NULL) == 0, "empty nonce");
    CHECK(cred_check_wsse("user", n1, "", dg, NULL) == 0, "empty created");
    CHECK(cred_check_wsse("user", n1, SPEC_CREATED, "", NULL) == 0, "empty digest");
    CHECK(cred_check_wsse("user", "not base64!", SPEC_CREATED, dg, NULL) == 0, "nonce not base64");
    CHECK(cred_check_wsse("user", "Zm9vYg", SPEC_CREATED, dg, NULL) == 0, "nonce without padding");
    CHECK(cred_check_wsse("user", n1, SPEC_CREATED, "not base64 at all=====!!!!", NULL) == 0, "digest not base64");
    char shortd[64];
    b64_encode("0123456789012345678", 19, shortd, sizeof(shortd));
    CHECK(cred_check_wsse("user", n1, SPEC_CREATED, shortd, NULL) == 0, "19-byte digest");
    b64_encode("012345678901234567890", 21, shortd, sizeof(shortd));
    CHECK(cred_check_wsse("user", n1, SPEC_CREATED, shortd, NULL) == 0, "21-byte digest");
    char bignonce[400], bigcreated[200];
    memset(bignonce, 'A', sizeof(bignonce) - 1);
    bignonce[sizeof(bignonce) - 1] = '\0';
    memset(bigcreated, '1', sizeof(bigcreated) - 1);
    bigcreated[sizeof(bigcreated) - 1] = '\0';
    CHECK(cred_check_wsse("user", bignonce, SPEC_CREATED, dg, NULL) == 0, "oversized nonce");
    CHECK(cred_check_wsse("user", n1, bigcreated, dg, NULL) == 0, "oversized created");
    char flip[64];
    memcpy(flip, dg, sizeof(flip));
    flip[0] = flip[0] == 'A' ? 'B' : 'A';
    CHECK(cred_check_wsse("user", n1, SPEC_CREATED, flip, NULL) == 0, "one changed digest character");
    CHECK(cred_check_wsse("user", n1, "2010-09-16T07:50:46Z", dg, NULL) == 0, "created changed after signing");
    CHECK(cred_check_wsse("user", n1, SPEC_CREATED, dg, NULL) == 1, "the genuine message passes after all the junk");

    /* replay cache: 32 entries */
    fresh();
    put_creds("user", SPEC_PASS);
    char n[32];
    int ok = 1;
    for (int i = 1; i <= CRED_NONCE_CACHE; i++) {
        nonce_n(i, n);
        if (wsse("user", n, SPEC_CREATED, SPEC_PASS, NULL) != 1) ok = 0;
    }
    CHECK(ok, "32 distinct nonces accepted");
    ok = 1;
    for (int i = 1; i <= CRED_NONCE_CACHE; i++) {
        nonce_n(i, n);
        if (wsse("user", n, SPEC_CREATED, SPEC_PASS, NULL) != 0) ok = 0;
    }
    CHECK(ok, "each of the 32 is refused as a replay");
    nonce_n(33, n);
    CHECK(wsse("user", n, SPEC_CREATED, SPEC_PASS, NULL) == 1, "a 33rd nonce is accepted (evicts the oldest)");
    nonce_n(1, n);
    CHECK(wsse("user", n, SPEC_CREATED, SPEC_PASS, NULL) == 1, "the evicted nonce is accepted again (the documented bound)");
    nonce_n(3, n);
    CHECK(wsse("user", n, SPEC_CREATED, SPEC_PASS, NULL) == 0, "... while the others are still remembered");

    /* new credentials file: the old password stops working */
    put_creds("user", "A-brand-new-pass");
    later();
    nonce_n(100, n);
    CHECK(wsse("user", n, SPEC_CREATED, SPEC_PASS, NULL) == 0, "old password refused after a change");
    CHECK(wsse("user", n, SPEC_CREATED, "A-brand-new-pass", NULL) == 1, "new password accepted after a change");

    /* no / invalid plain credentials */
    unlink(cpath);
    later();
    nonce_n(101, n);
    CHECK(wsse("user", n, SPEC_CREATED, "A-brand-new-pass", NULL) == 0, "no credentials file: refused");
    put(cpath, "user=user\npass=A-brand-new-pass\n", 0644);
    later();
    CHECK(wsse("user", n, SPEC_CREATED, "A-brand-new-pass", NULL) == 0, "file with open permissions: refused");
}

static void test_clock(void)
{
    printf("Created window and the clock\n");
    fresh();
    put_creds("user", SPEC_PASS);
    char n[32];
    int i = 0;
#define TRY(created) (nonce_n(++i, n), wsse("user", n, created, SPEC_PASS, NULL))
    wall = utc(2026, 11, 10, 12, 0, 0);
    CHECK(TRY("2026-11-10T12:00:00Z") == 1, "Created = now");
    CHECK(TRY("2026-11-10T12:05:00Z") == 1, "+300 s: inside");
    CHECK(TRY("2026-11-10T12:05:01Z") == 0, "+301 s: refused");
    CHECK(TRY("2026-11-10T11:55:00Z") == 1, "-300 s: inside");
    CHECK(TRY("2026-11-10T11:54:59Z") == 0, "-301 s: refused");
    CHECK(TRY("2010-09-16T07:50:45Z") == 0, "a Created from 2010 is refused when the clock is set");
    CHECK(TRY("2026-11-10T13:00:00+01:00") == 1, "zone offset +01:00 converted");
    CHECK(TRY("2026-11-10T07:00:00-05:00") == 1, "zone offset -05:00 converted");
    CHECK(TRY("2026-11-10T12:00:00") == 1, "no zone: read as UTC");
    CHECK(TRY("2026-11-10T12:00:00.123456Z") == 1, "fractional seconds");
    CHECK(TRY("2026-11-10T12:00:00Z ") == 0, "trailing space refused");
    CHECK(TRY("garbage") == 0, "not a date");
    CHECK(TRY("2026-13-10T12:00:00Z") == 0, "month 13");
    CHECK(TRY("2026-02-30T12:00:00Z") == 0, "30 February");
    CHECK(TRY("2026-11-10T24:00:00Z") == 0, "hour 24");
    CHECK(TRY("2026-11-10 12:00:00Z") == 0, "space instead of T");
    CHECK(TRY("2026-11-10T12:00:00.Z") == 0, "empty fraction");
    CHECK(TRY("2026-11-10T12:00:00+0100") == 0, "offset without colon");
    CHECK(TRY("2026-11-10T12:00:00+15:00") == 0, "offset beyond 14 h");
    CHECK(TRY("2026-11-10T12:00:00ZZ") == 0, "junk after the zone");
    wall = utc(2024, 3, 1, 0, 0, 0);
    CHECK(TRY("2024-02-29T23:59:59Z") == 1, "leap day, one second before the clock");

    wall = utc(2023, 12, 31, 23, 59, 59);
    CHECK(TRY("2010-09-16T07:50:45Z") == 1, "clock before 2024: window skipped (old Created accepted)");
    CHECK(TRY("not even a date") == 1, "clock before 2024: Created is only part of the digest");
    wall = 0;
    CHECK(TRY("2099-01-01T00:00:00Z") == 1, "clock at 0: window skipped");
    wall = utc(2024, 1, 1, 0, 0, 0);
    CHECK(TRY("2010-09-16T07:50:45Z") == 0, "clock exactly at 2024-01-01: window applies");
    /* the replay cache stays active with a wrong clock */
    wall = 0;
    nonce_n(7777, n);
    CHECK(wsse("user", n, SPEC_CREATED, SPEC_PASS, NULL) == 1 && wsse("user", n, SPEC_CREATED, SPEC_PASS, NULL) == 0,
          "replay refused even when the window is skipped");
    /* a message that failed the window must not use up its nonce */
    wall = utc(2026, 11, 10, 12, 0, 0);
    nonce_n(8888, n);
    CHECK(wsse("user", n, "2026-11-10T10:00:00Z", SPEC_PASS, NULL) == 0, "stale Created refused");
    CHECK(wsse("user", n, "2026-11-10T12:00:00Z", SPEC_PASS, NULL) == 1, "its nonce was not burnt by the refusal");
}

static void test_limiter(void)
{
    printf("shared failure limiter\n");
    fresh();
    put_creds("user", SPEC_PASS);
    wall = 0;
    unsigned d[8];
    char n[32];
    for (int i = 0; i < 6; i++) {
        nonce_n(i, n);
        d[i] = 0;
        wsse("user", n, SPEC_CREATED, "wrong", &d[i]);
    }
    CHECK(d[0] == 0 && d[1] == 0 && d[2] == 500 && d[3] == 1000 && d[4] == 2000 && d[5] == 2000,
          "delay 0, 0, 500, 1000, 2000 ms as for RTSP");
    CHECK(limiter.global_fails == 6, "the counter is the bound RTSP one");
    nonce_n(50, n);
    unsigned dd = 77;
    CHECK(wsse("user", n, SPEC_CREATED, SPEC_PASS, &dd) == 1 && dd == 0 && limiter.global_fails == 0, "a success resets the counter");
    for (int i = 0; i < 4; i++) wsse("user", n, SPEC_CREATED, "wrong", &dd);
    CHECK(dd == 1000, "counting again");
    mono += RTSP_AUTH_FAIL_WINDOW_US + 1;
    wsse("user", n, SPEC_CREATED, "wrong", &dd);
    CHECK(dd == 0 && limiter.global_fails == 1, "forgotten after a quiet minute");
    /* failures of an RTSP client count for ONVIF and back */
    limiter.global_fails = 4;
    limiter.last_fail_us = mono;
    wsse("user", n, SPEC_CREATED, "wrong", &dd);
    CHECK(dd == 2000, "RTSP failures slow down ONVIF");
    CHECK(rtsp_auth_note_failure(&limiter, mono) == 2000 && limiter.global_fails == 6, "ONVIF failures slow down RTSP");
    /* delay pointer is optional */
    CHECK(cred_check_wsse("user", n, SPEC_CREATED, "x", NULL) == 0, "NULL delay_ms is accepted");
    /* no limiter bound: a private one is used */
    cred_init(cpath, NULL);
    wsse("user", n, SPEC_CREATED, "wrong", &dd);
    CHECK(dd == 0 && limiter.global_fails == 7, "an unbound store keeps its own counter");
}

static void hdr_digest(char *out, size_t cap, const char *user, const char *pass, const char *nonce,
                       const char *method, const char *uri)
{
    char ha1[33], resp[33];
    md5_hex_cat(ha1, user, ":" RTSP_AUTH_REALM ":", pass, (char *)NULL);
    rtsp_auth_response(resp, ha1, nonce, "00000001", "abcdef", method, uri);
    snprintf(out, cap, "Digest username=\"%s\", realm=\"akam\", nonce=\"%s\", uri=\"%s\", response=\"%s\", "
             "qop=auth, nc=00000001, cnonce=\"abcdef\"", user, nonce, uri, resp);
}

static void test_http_digest(void)
{
    printf("HTTP Digest through rtsp_auth.c\n");
    fresh();
    char nonce[RTSP_AUTH_NONCE_HEX + 1];
    int (*saved_rng)(void *, size_t) = rtsp_auth_rng;
    rtsp_auth_rng = NULL;
    CHECK(rtsp_auth_new_nonce(nonce) != 0, "no random source: no nonce");
    rtsp_auth_rng = saved_rng;
    CHECK(rtsp_auth_new_nonce(nonce) == 0 && strlen(nonce) == RTSP_AUTH_NONCE_HEX, "default generator gives a 32-hex nonce");
    CHECK(cred_auth_enabled() == false, "no htdigest file: RTSP password off");
    char ha1[33], line[128], h[800];
    md5_hex_cat(ha1, "viewer", ":akam:", "Correct-Horse-9", (char *)NULL);
    snprintf(line, sizeof(line), "viewer:akam:%s\n", ha1);
    put(apath, line, 0600);
    CHECK(cred_auth_enabled() == true, "htdigest file: enabled");
    rtsp_auth_digest_t d;
    unsigned delay = 9;
    hdr_digest(h, sizeof(h), "viewer", "Correct-Horse-9", "00112233445566778899aabbccddeeff", "POST", "/onvif/device_service");
    CHECK(cred_check_http_digest("POST", h, strlen(h), &d, &delay) == 1 && delay == 0, "right login");
    CHECK(!strcmp(d.nonce, "00112233445566778899aabbccddeeff") && d.qop && !strcmp(d.nc, "00000001") &&
          !strcmp(d.cnonce, "abcdef") && !strcmp(d.uri, "/onvif/device_service"), "nonce, nc, cnonce, uri handed to the caller");
    CHECK(cred_check_http_digest("GET", h, strlen(h), &d, &delay) == 0, "method is part of the response");
    hdr_digest(h, sizeof(h), "viewer", "wrong", "00112233445566778899aabbccddeeff", "POST", "/x");
    CHECK(cred_check_http_digest("POST", h, strlen(h), &d, &delay) == 0 && delay == 0 && limiter.global_fails == 2, "wrong password counted");
    CHECK(cred_check_http_digest("POST", h, strlen(h), &d, &delay) == 0 && delay == 500, "and delayed from the third failure");
    CHECK(cred_check_http_digest("POST", "Digest garbage", 14, &d, &delay) == 0, "malformed header");
    CHECK(cred_check_http_digest("POST", "Basic dmlld2VyOnB3", 18, &d, &delay) == 0, "Basic refused");
    unlink(apath);
    hdr_digest(h, sizeof(h), "viewer", "Correct-Horse-9", "00112233445566778899aabbccddeeff", "POST", "/x");
    CHECK(cred_check_http_digest("POST", h, strlen(h), &d, &delay) == 0, "RTSP password off: never a pass through this hook");
    put(apath, "garbage\n", 0600);
    CHECK(cred_auth_enabled() == true && cred_check_http_digest("POST", h, strlen(h), &d, &delay) == 0, "broken htdigest file: fail closed");
    unlink(apath);
}

int main(int argc, char **argv)
{
    if (argc != 2) { fprintf(stderr, "usage: cred_test TEMPDIR\n"); return 2; }
    snprintf(dir, sizeof(dir), "%s", argv[1]);
    snprintf(cpath, sizeof(cpath), "%s/credentials", dir);
    snprintf(apath, sizeof(apath), "%s/rtsp_auth", dir);
    cred_wall_s = fake_wall;
    cred_mono_us = fake_mono;
    test_parse();
    test_file();
    test_wsse();
    test_clock();
    test_limiter();
    test_http_digest();
    printf("%s\n", fails ? "FAILED" : "all ok");
    return fails ? 1 : 0;
}
