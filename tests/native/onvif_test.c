#define _GNU_SOURCE
/* Host test of src/ak_rtsp/onvif.c (dispatch, authentication gate, identity) and onvif_device.c (every Device
 * operation), over real HTTP requests to http_srv on a localhost port, with the real cred.c / rtsp_auth.c and real
 * files in a temporary directory. Clocks: the monotonic clock of cred.c is injected (nonce lifetimes), the wall clock
 * is the real one (WS-Security Created). Golden responses live in tests/fixtures/onvif/; the port is written
 * @PORT@ in them. Set ONVIF_UPDATE_GOLDEN=1 to rewrite them (then review the diff).
 * Run: onvif_test TEMPDIR FIXTURE_DIR */
#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "b64.h"
#include "cred.h"
#include "md5.h"
#include "onvif.h"
#include "onvif_device.h"
#include "rtsp_auth.h"
#include "sha1.h"

const char *firmware_version(void) { return "btest"; }

static int fails;
#define CHECK(cond, msg) do { if (cond) printf("  ok   - %s\n", msg); \
    else { printf("  FAIL - %s (line %d)\n", msg, __LINE__); fails++; } } while (0)

static char dir[256], fixdir[300], cpath[300], apath[300], hpath[300], rpath[300];
static int g_port;
static rtsp_auth_db_t limiter;
static int64_t g_mono = 5000000000LL;
/* 1 ms per reading: strictly increasing, so "oldest nonce" is well defined */
static int64_t fake_mono(void) { return __atomic_add_fetch(&g_mono, 1000, __ATOMIC_SEQ_CST); }
static void advance_s(int s) { __atomic_add_fetch(&g_mono, (int64_t)s * 1000000, __ATOMIC_SEQ_CST); }

static int64_t g_settime = -1;
static int g_reboots;
static int fake_settime(int64_t s) { g_settime = s; return 0; }
static void fake_reboot(void) { g_reboots++; }
static int64_t fake_now(void) { return 1794314096; }   /* 2026-11-10T12:34:56Z */

static void put(const char *path, const char *content, mode_t mode)
{
    char tmp[320];
    snprintf(tmp, sizeof(tmp), "%s.new", path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0 || write(fd, content, strlen(content)) < 0) { perror("put"); exit(2); }
    fchmod(fd, mode);
    close(fd);
    rename(tmp, path);
}

#define USER "viewer"
#define PASS "Correct-Horse-9"
static void auth_on(void)
{
    char ha1[33], line[128], cr[128];
    md5_hex_cat(ha1, USER, ":akam:", PASS, (char *)NULL);
    snprintf(line, sizeof(line), "%s:akam:%s\n", USER, ha1);
    snprintf(cr, sizeof(cr), "user=%s\npass=%s\n", USER, PASS);
    put(apath, line, 0600);
    put(cpath, cr, 0600);
    cred_refresh(true);
}
static void auth_off(void)
{
    unlink(apath);
    unlink(cpath);
    cred_refresh(true);
}
static void reset_state(void)
{
    onvif_reset();
    rtsp_auth_note_success(&limiter);
    g_settime = -1;
    g_reboots = 0;
    unlink(hpath);
}

/* ---------------------------------------------------------------- client */

typedef struct { int status; char hdr[2048]; char body[24576]; long ms; } hresp_t;

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int request(const char *method, const char *target, const char *extra, const char *body, hresp_t *r)
{
    memset(r, 0, sizeof(*r));
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)g_port) };
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr *)&a, sizeof(a)) < 0) { close(fd); return -1; }
    char head[2048];
    size_t bl = body ? strlen(body) : 0;
    int n = snprintf(head, sizeof(head), "%s %s HTTP/1.1\r\nHost: x\r\nConnection: close\r\n"
                     "Content-Type: application/soap+xml\r\n%sContent-Length: %zu\r\n\r\n", method, target, extra ? extra : "", bl);
    send(fd, head, (size_t)n, MSG_NOSIGNAL);
    if (bl) send(fd, body, bl, MSG_NOSIGNAL);
    long long t0 = now_ms();
    static char buf[40000];
    size_t have = 0;
    for (;;) {
        struct pollfd p = { fd, POLLIN, 0 };
        if (poll(&p, 1, 8000) <= 0) break;
        ssize_t k = recv(fd, buf + have, sizeof(buf) - 1 - have, 0);
        if (k <= 0) break;
        have += (size_t)k;
    }
    r->ms = (long)(now_ms() - t0);
    close(fd);
    buf[have] = 0;
    char *he = strstr(buf, "\r\n\r\n");
    if (!he || sscanf(buf, "HTTP/1.1 %d", &r->status) != 1) return -2;
    snprintf(r->hdr, sizeof(r->hdr), "%.*s", (int)(he - buf), buf);
    snprintf(r->body, sizeof(r->body), "%s", he + 4);
    return 0;
}

#define NS_OPEN "<?xml version=\"1.0\"?><s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\">"
static void envelope(char *out, size_t cap, const char *header, const char *op)
{
    snprintf(out, cap, NS_OPEN "<s:Header>%s</s:Header><s:Body>%s</s:Body></s:Envelope>", header ? header : "", op);
}

static int soap(const char *path, const char *extra, const char *header, const char *op, hresp_t *r)
{
    char body[4096];
    envelope(body, sizeof(body), header, op);
    return request("POST", path, extra, body, r);
}
#define DEV "/onvif/device_service"
#define TDS "xmlns:tds=\"http://www.onvif.org/ver10/device/wsdl\""
static int dev(const char *header, const char *extra, const char *op, hresp_t *r) { return soap(DEV, extra, header, op, r); }
#define OPX(name) "<tds:" name " " TDS "/>"

static void wsse_header(char *out, size_t cap, const char *user, const char *pass, const char *nonce_b64,
                        const char *created, const char *type)
{
    uint8_t raw[64], d[20];
    char dg[64];
    long n = b64_decode(nonce_b64, strlen(nonce_b64), raw, sizeof(raw));
    sha1_ctx c;
    sha1_init(&c);
    sha1_update(&c, raw, (size_t)(n > 0 ? n : 0));
    sha1_update(&c, created, strlen(created));
    sha1_update(&c, pass, strlen(pass));
    sha1_final(&c, d);
    b64_encode(d, 20, dg, sizeof(dg));
    snprintf(out, cap,
        "<wsse:Security xmlns:wsse=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-secext-1.0.xsd\""
        " xmlns:wsu=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-utility-1.0.xsd\">"
        "<wsse:UsernameToken><wsse:Username>%s</wsse:Username>"
        "<wsse:Password Type=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-username-token-profile-1.0#%s\">%s</wsse:Password>"
        "<wsse:Nonce>%s</wsse:Nonce><wsu:Created>%s</wsu:Created></wsse:UsernameToken></wsse:Security>",
        user, type, strcmp(type, "PasswordText") == 0 ? pass : dg, nonce_b64, created);
}

static int g_nonce_ctr;
static void good_wsse(char *out, size_t cap, const char *pass)
{
    char nonce[32], created[40];
    uint8_t raw[16] = { 0x77, (uint8_t)g_nonce_ctr, (uint8_t)(g_nonce_ctr >> 8) };
    g_nonce_ctr++;
    b64_encode(raw, sizeof(raw), nonce, sizeof(nonce));
    time_t now = time(NULL);
    struct tm t;
    gmtime_r(&now, &t);
    strftime(created, sizeof(created), "%Y-%m-%dT%H:%M:%SZ", &t);
    wsse_header(out, cap, USER, pass, nonce, created, "PasswordDigest");
}

static int has(const hresp_t *r, const char *s) { return strstr(r->body, s) != NULL; }
static int hdr_has(const hresp_t *r, const char *s) { return strstr(r->hdr, s) != NULL; }

static int nonce_from(const hresp_t *r, char *out)
{
    const char *p = strstr(r->hdr, "nonce=\"");
    if (!p) return -1;
    p += 7;
    const char *e = strchr(p, '"');
    if (!e || e - p != 32) return -1;
    memcpy(out, p, 32);
    out[32] = 0;
    return 0;
}

static void digest_hdr(char *out, size_t cap, const char *pass, const char *nonce, const char *nc, const char *uri)
{
    char ha1[33], resp[33];
    md5_hex_cat(ha1, USER, ":akam:", pass, (char *)NULL);
    rtsp_auth_response(resp, ha1, nonce, nc, "cn0nce", "POST", uri);
    snprintf(out, cap,
             "Authorization: Digest username=\"%s\", realm=\"akam\", nonce=\"%s\", uri=\"%s\", algorithm=MD5, qop=auth, "
             "nc=%s, cnonce=\"cn0nce\", response=\"%s\"\r\n", USER, nonce, uri, nc, resp);
}

/* ---------------------------------------------------------------- tests */

static void test_password_off(void)
{
    printf("RTSP password OFF: reads open, state changes refused\n");
    auth_off();
    reset_state();
    hresp_t r;
    CHECK(dev(NULL, NULL, OPX("GetSystemDateAndTime"), &r) == 0 && r.status == 200, "GetSystemDateAndTime (pre-auth) open");
    CHECK(dev(NULL, NULL, OPX("GetDeviceInformation"), &r) == 0 && r.status == 200 && has(&r, "<tds:Manufacturer>Akam"),
          "GetDeviceInformation (read) open");
    CHECK(dev(NULL, NULL, OPX("GetUsers"), &r) == 0 && r.status == 200 && !has(&r, "<tt:Username>"), "GetUsers: no user when no password");
    CHECK(dev(NULL, NULL, "<tds:SetHostname " TDS "><tds:Name>evil</tds:Name></tds:SetHostname>", &r) == 0 && r.status == 400 &&
          has(&r, "ter:NotAuthorized"), "SetHostname refused (ter:NotAuthorized, HTTP 400)");
    CHECK(access(hpath, F_OK) != 0, "... and nothing written");
    CHECK(dev(NULL, NULL, "<tds:SetSystemDateAndTime " TDS "><tds:DateTimeType>Manual</tds:DateTimeType></tds:SetSystemDateAndTime>", &r) == 0 &&
          r.status == 400 && has(&r, "ter:NotAuthorized") && g_settime == -1, "SetSystemDateAndTime refused, clock untouched");
    CHECK(dev(NULL, NULL, OPX("SystemReboot"), &r) == 0 && r.status == 400 && has(&r, "ter:NotAuthorized") && g_reboots == 0,
          "SystemReboot refused, no reboot");
    CHECK(dev(NULL, NULL, OPX("SetNTP"), &r) == 0 && r.status == 400 && has(&r, "ter:NotAuthorized"), "SetNTP refused");
    char wsse[1200];
    good_wsse(wsse, sizeof(wsse), PASS);
    CHECK(dev(wsse, NULL, OPX("SystemReboot"), &r) == 0 && r.status == 400 && g_reboots == 0, "a token does not unlock state changes when the password is off");
}

/* A request after a clean slate of the limiter: the tests of the gate are not about the delay. */
static int devc(const char *header, const char *extra, const char *op, hresp_t *r)
{
    rtsp_auth_note_success(&limiter);
    return dev(header, extra, op, r);
}

static void test_wsse(void)
{
    printf("RTSP password ON: WS-Security\n");
    auth_on();
    reset_state();
    hresp_t r;
    char h[1500];
    CHECK(devc(NULL, NULL, OPX("GetSystemDateAndTime"), &r) == 0 && r.status == 200, "pre-auth op open without credentials");
    CHECK(devc(NULL, NULL, OPX("GetCapabilities"), &r) == 0 && r.status == 200, "GetCapabilities open");
    CHECK(devc(NULL, NULL, OPX("GetServices"), &r) == 0 && r.status == 200, "GetServices open");
    CHECK(devc(NULL, NULL, OPX("GetServiceCapabilities"), &r) == 0 && r.status == 200, "GetServiceCapabilities open");
    CHECK(devc(NULL, NULL, OPX("GetScopes"), &r) == 0 && r.status == 200, "GetScopes open");
    CHECK(devc(NULL, NULL, OPX("GetHostname"), &r) == 0 && r.status == 200, "GetHostname open");
    CHECK(devc(NULL, NULL, OPX("GetDeviceInformation"), &r) == 0 && r.status == 401 && hdr_has(&r, "WWW-Authenticate: Digest realm=\"akam\""),
          "read op without credentials: 401 with a Digest challenge");
    good_wsse(h, sizeof(h), PASS);
    CHECK(devc(h, NULL, OPX("GetDeviceInformation"), &r) == 0 && r.status == 200 && has(&r, "GetDeviceInformationResponse"), "valid UsernameToken");
    CHECK(devc(h, NULL, OPX("GetDeviceInformation"), &r) == 0 && r.status == 400 && has(&r, "ter:NotAuthorized"), "the same token again: replay refused");
    good_wsse(h, sizeof(h), "not-the-password");
    CHECK(devc(h, NULL, OPX("GetDeviceInformation"), &r) == 0 && r.status == 400 && has(&r, "ter:NotAuthorized") && !has(&r, "Manufacturer"),
          "bad password: Sender fault ter:NotAuthorized, HTTP 400");
    char nonce[32], created[40];
    uint8_t raw[16] = { 9, 9, 9 };
    b64_encode(raw, sizeof(raw), nonce, sizeof(nonce));
    time_t now = time(NULL);
    struct tm t;
    gmtime_r(&now, &t);
    strftime(created, sizeof(created), "%Y-%m-%dT%H:%M:%SZ", &t);
    wsse_header(h, sizeof(h), USER, PASS, nonce, created, "PasswordText");
    CHECK(devc(h, NULL, OPX("GetDeviceInformation"), &r) == 0 && r.status == 400 && has(&r, "ter:NotAuthorized"), "PasswordText refused");
    raw[0] = 10;
    b64_encode(raw, sizeof(raw), nonce, sizeof(nonce));
    wsse_header(h, sizeof(h), "someone", PASS, nonce, created, "PasswordDigest");
    CHECK(devc(h, NULL, OPX("GetDeviceInformation"), &r) == 0 && r.status == 400, "wrong user refused");
    raw[0] = 11;
    b64_encode(raw, sizeof(raw), nonce, sizeof(nonce));
    wsse_header(h, sizeof(h), USER, PASS, nonce, "2001-01-01T00:00:00Z", "PasswordDigest");
    CHECK(devc(h, NULL, OPX("GetDeviceInformation"), &r) == 0 && r.status == 400, "Created far from the clock refused");
    CHECK(devc("<wsse:Security xmlns:wsse=\"x\"><wsse:UsernameToken><wsse:Username>viewer</wsse:Username></wsse:UsernameToken></wsse:Security>",
              NULL, OPX("GetDeviceInformation"), &r) == 0 && r.status == 400, "token without password/nonce/created refused");
    good_wsse(h, sizeof(h), PASS);
    CHECK(devc(h, NULL, OPX("GetUsers"), &r) == 0 && r.status == 200 && has(&r, "<tt:Username>" USER "</tt:Username>") &&
          has(&r, "Administrator"), "GetUsers returns the one user, Administrator");
    /* state changes with a good token */
    good_wsse(h, sizeof(h), PASS);
    CHECK(devc(h, NULL, "<tds:SetHostname " TDS "><tds:Name>Cam-Salon</tds:Name></tds:SetHostname>", &r) == 0 && r.status == 200, "SetHostname with a token");
    char hn[64] = "";
    FILE *f = fopen(hpath, "r");
    if (f) { if (!fgets(hn, sizeof(hn), f)) hn[0] = 0; fclose(f); }
    CHECK(!strcmp(hn, "cam-salon\n"), "hostname stored lowercase, one line, as akam_hostname.sh does");
    struct stat st;
    CHECK(stat(hpath, &st) == 0 && (st.st_mode & 0777) == 0644, "hostname file mode 0644");
    CHECK(devc(NULL, NULL, OPX("GetHostname"), &r) == 0 && has(&r, "<tt:Name>cam-salon</tt:Name>"), "GetHostname follows at once");
    good_wsse(h, sizeof(h), PASS);
    CHECK(devc(h, NULL, "<tds:SetHostname " TDS "><tds:Name>-bad</tds:Name></tds:SetHostname>", &r) == 0 && r.status == 400 &&
          has(&r, "ter:InvalidHostname"), "invalid hostname: Sender fault");
    good_wsse(h, sizeof(h), PASS);
    CHECK(devc(h, NULL, "<tds:SetHostname " TDS "><tds:Name>12345</tds:Name></tds:SetHostname>", &r) == 0 && r.status == 400, "all-digit hostname refused");
    good_wsse(h, sizeof(h), PASS);
    CHECK(devc(h, NULL, OPX("SetHostname"), &r) == 0 && r.status == 400, "SetHostname without Name refused");
    /* time */
    good_wsse(h, sizeof(h), PASS);
    CHECK(devc(h, NULL, "<tds:SetSystemDateAndTime " TDS " xmlns:tt=\"x\"><tds:DateTimeType>Manual</tds:DateTimeType><tds:DaylightSavings>false</tds:DaylightSavings>"
              "<tds:UTCDateTime><tt:Time><tt:Hour>12</tt:Hour><tt:Minute>34</tt:Minute><tt:Second>56</tt:Second></tt:Time>"
              "<tt:Date><tt:Year>2026</tt:Year><tt:Month>11</tt:Month><tt:Day>10</tt:Day></tt:Date></tds:UTCDateTime></tds:SetSystemDateAndTime>", &r) == 0 &&
          r.status == 200 && g_settime == 1794314096, "SetSystemDateAndTime Manual sets the clock (2026-11-10T12:34:56Z)");
    g_settime = -1;
    good_wsse(h, sizeof(h), PASS);
    CHECK(devc(h, NULL, "<tds:SetSystemDateAndTime " TDS "><tds:DateTimeType>NTP</tds:DateTimeType></tds:SetSystemDateAndTime>", &r) == 0 &&
          r.status == 400 && g_settime == -1, "NTP type refused with a fault");
    good_wsse(h, sizeof(h), PASS);
    CHECK(devc(h, NULL, "<tds:SetSystemDateAndTime " TDS " xmlns:tt=\"x\"><tds:DateTimeType>Manual</tds:DateTimeType>"
              "<tds:UTCDateTime><tt:Time><tt:Hour>12</tt:Hour><tt:Minute>34</tt:Minute><tt:Second>56</tt:Second></tt:Time>"
              "<tt:Date><tt:Year>2026</tt:Year><tt:Month>2</tt:Month><tt:Day>30</tt:Day></tt:Date></tds:UTCDateTime></tds:SetSystemDateAndTime>", &r) == 0 &&
          r.status == 400 && has(&r, "ter:InvalidDateTime") && g_settime == -1, "30 February refused");
    good_wsse(h, sizeof(h), PASS);
    CHECK(devc(h, NULL, OPX("SetSystemDateAndTime"), &r) == 0 && r.status == 400 && g_settime == -1, "no DateTimeType refused");
    good_wsse(h, sizeof(h), PASS);
    CHECK(devc(h, NULL, OPX("SetNTP"), &r) == 0 && r.status == 400, "SetNTP: fault even with credentials");
    good_wsse(h, sizeof(h), PASS);
    CHECK(devc(h, NULL, OPX("SystemReboot"), &r) == 0 && r.status == 200 && has(&r, "SystemRebootResponse") && g_reboots == 1,
          "SystemReboot with a token: reply, then the reboot hook once");
}

static void test_digest(void)
{
    printf("RTSP password ON: HTTP Digest\n");
    auth_on();
    reset_state();
    hresp_t r, c;
    char nonce[40], h[800];
    CHECK(devc(NULL, NULL, OPX("GetDeviceInformation"), &c) == 0 && c.status == 401 && nonce_from(&c, nonce) == 0, "challenge carries a 32-hex nonce");
    CHECK(hdr_has(&c, "qop=\"auth\"") && hdr_has(&c, "algorithm=MD5") && !hdr_has(&c, "stale"), "qop=auth, MD5, not stale");
    CHECK(c.body[0] == 0, "401 has no body");
    digest_hdr(h, sizeof(h), PASS, nonce, "00000001", DEV);
    CHECK(devc(NULL, h, OPX("GetDeviceInformation"), &r) == 0 && r.status == 200 && has(&r, "Manufacturer"), "valid Digest");
    digest_hdr(h, sizeof(h), PASS, nonce, "00000001", DEV);
    CHECK(devc(NULL, h, OPX("GetDeviceInformation"), &r) == 0 && r.status == 401, "nc replay refused");
    digest_hdr(h, sizeof(h), PASS, nonce, "00000002", DEV);
    CHECK(devc(NULL, h, OPX("GetDeviceInformation"), &r) == 0 && r.status == 200, "higher nc accepted (nonce reused as RFC 7616 allows)");
    digest_hdr(h, sizeof(h), PASS, nonce, "00000003", "/onvif/media_service");
    CHECK(devc(NULL, h, OPX("GetDeviceInformation"), &r) == 0 && r.status == 401 && !hdr_has(&r, "stale"), "uri of another resource refused (credentials were right)");
    digest_hdr(h, sizeof(h), PASS, nonce, "00000004", DEV "?x=1");
    CHECK(devc(NULL, h, OPX("GetDeviceInformation"), &r) == 0 && r.status == 401, "uri not equal to the request target refused");
    char target[80];
    snprintf(target, sizeof(target), DEV "?x=1");
    digest_hdr(h, sizeof(h), PASS, nonce, "00000005", target);
    char body[1024];
    envelope(body, sizeof(body), NULL, OPX("GetDeviceInformation"));
    CHECK(request("POST", target, h, body, &r) == 0 && r.status == 200, "uri with a query matches the target when it is the same");
    digest_hdr(h, sizeof(h), PASS, "0123456789abcdef0123456789abcdef", "00000001", DEV);
    CHECK(devc(NULL, h, OPX("GetDeviceInformation"), &r) == 0 && r.status == 401 && hdr_has(&r, "stale=true"), "nonce we never issued: 401 stale=true");
    digest_hdr(h, sizeof(h), "wrong-password", nonce, "00000009", DEV);
    CHECK(devc(NULL, h, OPX("GetDeviceInformation"), &r) == 0 && r.status == 401 && !hdr_has(&r, "stale"), "wrong password: 401 with a fresh challenge");
    char n2[40];
    CHECK(devc(NULL, NULL, OPX("GetDeviceInformation"), &c) == 0 && nonce_from(&c, n2) == 0 && strcmp(n2, nonce) != 0, "each challenge is a new nonce");
    digest_hdr(h, sizeof(h), PASS, n2, "00000001", DEV);
    advance_s((int)(ONVIF_NONCE_LIFETIME_US / 1000000) + 1);
    CHECK(devc(NULL, h, OPX("GetDeviceInformation"), &r) == 0 && r.status == 401 && hdr_has(&r, "stale=true"), "an expired nonce: stale=true");
    CHECK(devc(NULL, "Authorization: Basic dmlld2VyOnB3\r\n", OPX("GetDeviceInformation"), &r) == 0 && r.status == 401, "Basic refused");
    CHECK(devc(NULL, "Authorization: Digest garbage\r\n", OPX("GetDeviceInformation"), &r) == 0 && r.status == 401, "malformed Digest refused");

    /* the table is bounded: the 9th outstanding nonce evicts the oldest */
    char first[40], cur[40];
    CHECK(devc(NULL, NULL, OPX("GetDeviceInformation"), &c) == 0 && nonce_from(&c, first) == 0, "first of many nonces");
    for (int i = 0; i < ONVIF_NONCES; i++) {
        devc(NULL, NULL, OPX("GetDeviceInformation"), &c);
        nonce_from(&c, cur);
    }
    digest_hdr(h, sizeof(h), PASS, first, "00000001", DEV);
    CHECK(devc(NULL, h, OPX("GetDeviceInformation"), &r) == 0 && r.status == 401 && hdr_has(&r, "stale=true"), "evicted nonce is unknown (bounded table)");
    digest_hdr(h, sizeof(h), PASS, cur, "00000001", DEV);
    CHECK(devc(NULL, h, OPX("GetDeviceInformation"), &r) == 0 && r.status == 200, "the newest one works");

    /* state change with Digest */
    devc(NULL, NULL, OPX("SystemReboot"), &c);
    nonce_from(&c, cur);
    digest_hdr(h, sizeof(h), PASS, cur, "00000001", DEV);
    CHECK(devc(NULL, h, OPX("SystemReboot"), &r) == 0 && r.status == 200 && g_reboots == 1, "SystemReboot with Digest");
}

static void test_broken_and_delay(void)
{
    printf("Invalid credentials file, limiter delay\n");
    auth_on();
    reset_state();
    hresp_t r;
    put(apath, "garbage\n", 0600);
    cred_refresh(true);
    char h[1500];
    good_wsse(h, sizeof(h), PASS);
    CHECK(dev(h, NULL, OPX("GetDeviceInformation"), &r) == 0 && r.status == 400, "invalid htdigest file: even a good token is refused (fail closed)");
    CHECK(dev(NULL, NULL, OPX("GetDeviceInformation"), &r) == 0 && r.status == 401, "... and a read without credentials is challenged");
    CHECK(dev(NULL, NULL, OPX("GetScopes"), &r) == 0 && r.status == 200, "pre-auth ops still answer");
    CHECK(dev(NULL, NULL, OPX("SystemReboot"), &r) == 0 && r.status == 401 && g_reboots == 0, "state change refused");
    auth_on();
    reset_state();
    long ms[4];
    for (int i = 0; i < 4; i++) {
        good_wsse(h, sizeof(h), "wrong-password");
        dev(h, NULL, OPX("GetDeviceInformation"), &r);
        ms[i] = r.ms;
    }
    CHECK(ms[0] < 300 && ms[1] < 300, "first failures are answered at once");
    CHECK(ms[2] >= 450 && ms[2] < 1500, "third failure: answer deferred by about 500 ms");
    CHECK(ms[3] >= 950 && ms[3] < 2500, "fourth failure: about 1 s");
    /* while a delayed answer waits, another client is served */
    int ok = 0;
    {
        hresp_t a, b;
        good_wsse(h, sizeof(h), "wrong-password");
        /* issue the slow one from a helper process-like sequence: send, then query before reading the reply */
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons((uint16_t)g_port) };
        sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        connect(fd, (struct sockaddr *)&sa, sizeof(sa));
        char body[4096], req[6000];
        envelope(body, sizeof(body), h, OPX("GetDeviceInformation"));
        int n = snprintf(req, sizeof(req), "POST " DEV " HTTP/1.1\r\nHost: x\r\nConnection: close\r\nContent-Length: %zu\r\n\r\n%s", strlen(body), body);
        send(fd, req, (size_t)n, 0);
        usleep(100000);
        long long t = now_ms();
        ok = dev(NULL, NULL, OPX("GetScopes"), &b) == 0 && b.status == 200 && now_ms() - t < 400;
        struct pollfd p = { fd, POLLIN, 0 };
        ok = ok && poll(&p, 1, 4000) > 0;
        close(fd);
        (void)a;
    }
    CHECK(ok, "the poll loop is not blocked while a failed login waits");
    rtsp_auth_note_success(&limiter);
}

static void test_dispatch(void)
{
    printf("Dispatch\n");
    auth_off();
    reset_state();
    hresp_t r;
    CHECK(dev(NULL, NULL, OPX("FlyToTheMoon"), &r) == 0 && r.status == 400 && has(&r, "ter:ActionNotSupported"), "unknown op: ter:ActionNotSupported");
    CHECK(request("POST", DEV, NULL, "not xml at all", &r) == 0 && r.status == 400 && has(&r, "ter:InvalidArgs"), "malformed body: Sender fault");
    CHECK(request("POST", DEV, NULL, NS_OPEN "<s:Body/></s:Envelope>", &r) == 0 && r.status == 400, "empty Body: Sender fault");
    CHECK(request("GET", DEV, NULL, NULL, &r) == 0 && r.status == 405, "GET on a service: 405");
    CHECK(request("POST", "/onvif/nothing", NULL, "x", &r) == 0 && r.status == 404, "other path: 404");
    CHECK(request("POST", DEV, NULL, NS_OPEN "<!DOCTYPE x [<!ENTITY y \"z\">]><s:Body><a/></s:Body></s:Envelope>", &r) == 0 && r.status == 400, "DTD refused");
    CHECK(dev(NULL, NULL, "<tds:GetScopes xmlns:tds=\"urn:other\"/>", &r) == 0 && r.status == 200, "operation matched by local name");
    CHECK(strstr(r.hdr, "application/soap+xml") != NULL, "SOAP content type");
}

static void test_uuid(void)
{
    printf("Device UUID\n");
    char a[46], b[46], mac[18];
    onvif_uuid(a);
    onvif_uuid(b);
    CHECK(!strcmp(a, b), "stable across calls");
    CHECK(strlen(a) == 45 && !strncmp(a, "urn:uuid:", 9) && a[17] == '-' && a[22] == '-' && a[27] == '-' && a[32] == '-', "urn:uuid: 8-4-4-4-12");
    CHECK(a[23] == '5', "version 5");
    CHECK(strchr("89ab", a[28]) != NULL, "RFC 4122 variant");
    onvif_mac(mac);
    char p[320];   /* the .sh checks this value with an independent implementation */
    snprintf(p, sizeof(p), "%s/uuid.txt", dir);
    FILE *f = fopen(p, "w");
    fprintf(f, "%s %s\n", mac, a);
    fclose(f);
    const char *saved = onvif_ifname;
    onvif_ifname = "no-such-if0";
    char c[46];
    onvif_uuid(c);
    CHECK(!strcmp(c, a) || onvif_mac(mac) == -1, "no interface: falls back to the all-zero MAC (still a valid UUID)");
    onvif_ifname = saved;
}

static void write_body(const char *name, const hresp_t *r)
{
    char p[400];
    snprintf(p, sizeof(p), "%s/resp_%s.xml", dir, name);
    FILE *f = fopen(p, "w");
    if (f) { fputs(r->body, f); fclose(f); }
}

static void golden(const char *name, const char *header, const char *op, int expect_status)
{
    hresp_t r;
    char p[400], msg[160], want[24576], got[24576], portstr[16];
    if (dev(header, NULL, op, &r) != 0 || r.status != expect_status) {
        snprintf(msg, sizeof(msg), "golden %s: status %d", name, r.status);
        CHECK(0, msg);
        return;
    }
    write_body(name, &r);
    snprintf(portstr, sizeof(portstr), ":%d", g_port);
    /* the port is dynamic: write it as @PORT@ */
    char *o = got;
    for (const char *s = r.body; *s;) {
        if (!strncmp(s, portstr, strlen(portstr))) { o += sprintf(o, ":@PORT@"); s += strlen(portstr); }
        else *o++ = *s++;
    }
    *o = 0;
    snprintf(p, sizeof(p), "%s/%s.xml", fixdir, name);
    if (getenv("ONVIF_UPDATE_GOLDEN")) {
        FILE *f = fopen(p, "w");
        if (f) { fputs(got, f); fputc('\n', f); fclose(f); }
    }
    FILE *f = fopen(p, "r");
    size_t n = 0;
    if (f) { n = fread(want, 1, sizeof(want) - 1, f); fclose(f); }
    want[n] = 0;
    while (n && (want[n - 1] == '\n' || want[n - 1] == '\r')) want[--n] = 0;
    snprintf(msg, sizeof(msg), "golden %s (HTTP %d)", name, expect_status);
    CHECK(f && !strcmp(want, got), msg);
    if (f && strcmp(want, got)) printf("      want: %s\n      got:  %s\n", want, got);
}

static void test_golden(void)
{
    printf("Golden responses\n");
    auth_on();
    reset_state();
    put(rpath, "# comment\nsearch lan\nnameserver 192.168.1.1\nnameserver fe80::1\nnameserver 8.8.8.8\n", 0644);
    char h[1500];
    golden("GetSystemDateAndTime", NULL, OPX("GetSystemDateAndTime"), 200);
    golden("GetCapabilities", NULL, OPX("GetCapabilities"), 200);
    golden("GetServices", NULL, OPX("GetServices"), 200);
    golden("GetServiceCapabilities", NULL, OPX("GetServiceCapabilities"), 200);
    golden("GetScopes", NULL, OPX("GetScopes"), 200);
    golden("GetHostname", NULL, OPX("GetHostname"), 200);
    good_wsse(h, sizeof(h), PASS);
    golden("GetDeviceInformation", h, OPX("GetDeviceInformation"), 200);
    good_wsse(h, sizeof(h), PASS);
    golden("GetNetworkInterfaces", h, OPX("GetNetworkInterfaces"), 200);
    good_wsse(h, sizeof(h), PASS);
    golden("GetNTP", h, OPX("GetNTP"), 200);
    good_wsse(h, sizeof(h), PASS);
    golden("GetDNS", h, OPX("GetDNS"), 200);
    good_wsse(h, sizeof(h), PASS);
    golden("GetUsers", h, OPX("GetUsers"), 200);
    good_wsse(h, sizeof(h), PASS);
    golden("SetHostname", h, "<tds:SetHostname " TDS "><tds:Name>akam-test</tds:Name></tds:SetHostname>", 200);
    good_wsse(h, sizeof(h), PASS);
    golden("SetSystemDateAndTime", h, "<tds:SetSystemDateAndTime " TDS " xmlns:tt=\"x\"><tds:DateTimeType>Manual</tds:DateTimeType>"
           "<tds:UTCDateTime><tt:Time><tt:Hour>1</tt:Hour><tt:Minute>2</tt:Minute><tt:Second>3</tt:Second></tt:Time>"
           "<tt:Date><tt:Year>2025</tt:Year><tt:Month>2</tt:Month><tt:Day>28</tt:Day></tt:Date></tds:UTCDateTime></tds:SetSystemDateAndTime>", 200);
    good_wsse(h, sizeof(h), PASS);
    golden("SystemReboot", h, OPX("SystemReboot"), 200);
    good_wsse(h, sizeof(h), "wrong-password");
    golden("Fault_NotAuthorized", h, OPX("GetDeviceInformation"), 400);
    golden("Fault_ActionNotSupported", NULL, OPX("Nope"), 400);
    golden("GetHostname_after_set", NULL, OPX("GetHostname"), 200);
}

int main(int argc, char **argv)
{
    if (argc != 3) { fprintf(stderr, "usage: onvif_test TEMPDIR FIXTURE_DIR\n"); return 2; }
    snprintf(dir, sizeof(dir), "%s", argv[1]);
    snprintf(fixdir, sizeof(fixdir), "%s", argv[2]);
    snprintf(cpath, sizeof(cpath), "%s/credentials", dir);
    snprintf(apath, sizeof(apath), "%s/rtsp_auth", dir);
    snprintf(hpath, sizeof(hpath), "%s/hostname", dir);
    snprintf(rpath, sizeof(rpath), "%s/resolv.conf", dir);
    setenv("TZ", "UTC", 1);
    tzset();
    cred_mono_us = fake_mono;
    rtsp_auth_db_init(&limiter, apath);
    cred_init(cpath, &limiter);
    onvif_ifname = "lo";
    onvif_hostname_path = hpath;
    onvif_resolv_path = rpath;
    onvif_time_fn = fake_now;
    onvif_settime_fn = fake_settime;
    onvif_reboot_fn = fake_reboot;

    CHECK(onvif_start(0) == 0, "onvif_start on a free port");
    g_port = http_srv_port();
    CHECK(g_port > 0, "port bound");
    test_dispatch();
    test_password_off();
    test_wsse();
    test_digest();
    test_broken_and_delay();
    test_uuid();
    test_golden();
    onvif_stop();
    CHECK(http_srv_port() == 0, "onvif_stop closes the port");
    CHECK(onvif_start(0) == 0, "start again after stop (routes survive)");
    g_port = http_srv_port();
    hresp_t r;
    CHECK(dev(NULL, NULL, OPX("GetScopes"), &r) == 0 && r.status == 200, "still serving");
    onvif_stop();
    printf("%s\n", fails ? "FAILED" : "all ok");
    return fails ? 1 : 0;
}
