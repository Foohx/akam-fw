#define _GNU_SOURCE
/* Host test of src/ak_rtsp/onvif_cfg.c: the flag file parser, the reload state machine (off, on, port change,
 * no_password, port in use, password OFF) over the REAL http_srv + onvif + wsd + cred, files in a temporary
 * directory, no camera. End to end: after a reload the Device service answers GetSystemDateAndTime and
 * GetDeviceInformation over HTTP and WS-Discovery answers a unicast Probe with the right XAddr. Also checks that OFF
 * leaves no extra thread or descriptor and that an idle ON service does not wake up. Run: onvif_svc_test TEMPDIR */
#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "cred.h"
#include "md5.h"
#include "onvif.h"
#include "onvif_cfg.h"
#include "onvif_device.h"
#include "rtsp_auth.h"

const char *firmware_version(void) { return "svctest"; }

static int fails;
#define CHECK(cond, msg) do { if (cond) printf("  ok   - %s\n", msg); \
    else { printf("  FAIL - %s (line %d: %s)\n", msg, __LINE__, #cond); fails++; } } while (0)

static char dir[256], cfgp[300], cpath[300], apath[300], hpath[300];
static rtsp_auth_db_t limiter;

static void put(const char *path, const char *content)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0 || write(fd, content, strlen(content)) < 0) { perror("put"); exit(2); }
    fchmod(fd, 0600);
    close(fd);
}

static int count_dir(const char *p)
{
    int n = 0;
    DIR *d = opendir(p);
    struct dirent *e;
    while (d && (e = readdir(d))) if (e->d_name[0] != '.') n++;
    if (d) closedir(d);
    return n;
}

/* pthread_join can return before the kernel drops the task from /proc (and fds close asynchronously under load):
 * poll up to 2 s for the count to settle on the expected value; print the observed value if it never does. */
static int settled(const char *p, int want)
{
    int n = count_dir(p);
    for (int i = 0; i < 400 && n != want; i++) {
        struct timespec ts = { 0, 5 * 1000 * 1000 };
        nanosleep(&ts, NULL);
        n = count_dir(p);
    }
    if (n != want) printf("  info - %s: %d entries, expected %d\n", p, n, want);
    return n == want;
}

#define USER "viewer"
#define PASS "Correct-Horse-9"
static void auth_on(void)
{
    char ha1[33], line[128], cr[128];
    md5_hex_cat(ha1, USER, ":akam:", PASS, (char *)NULL);
    snprintf(line, sizeof(line), "%s:akam:%s\n", USER, ha1);
    snprintf(cr, sizeof(cr), "user=%s\npass=%s\n", USER, PASS);
    put(apath, line);
    put(cpath, cr);
    cred_refresh(true);
}
static void auth_off(void) { unlink(apath); unlink(cpath); cred_refresh(true); }
static void auth_no_plain(void)
{
    char ha1[33], line[128];
    md5_hex_cat(ha1, USER, ":akam:", PASS, (char *)NULL);
    snprintf(line, sizeof(line), "%s:akam:%s\n", USER, ha1);
    put(apath, line);
    unlink(cpath);
    cred_refresh(true);
}

static int free_port(void)
{
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET };
    socklen_t l = sizeof(a);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    bind(s, (struct sockaddr *)&a, sizeof(a));
    getsockname(s, (struct sockaddr *)&a, &l);
    close(s);
    return ntohs(a.sin_port);
}

static void set_cfg(int en, int port)
{
    char b[64];
    snprintf(b, sizeof(b), "enabled=%d\nport=%d\n", en, port);
    put(cfgp, b);
}

static const char *reload(void)
{
    static char st[ONVIF_STATE_MAX];
    onvif_svc_reload(st, sizeof(st));
    return st;
}
static const char *state(void)
{
    static char st[ONVIF_STATE_MAX];
    onvif_svc_state(st, sizeof(st));
    return st;
}

static int http_post(int port, const char *op, char *out, size_t cap)
{
    char body[512], req[1024];
    snprintf(body, sizeof(body), "<?xml version=\"1.0\"?><s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\">"
             "<s:Body><tds:%s xmlns:tds=\"http://www.onvif.org/ver10/device/wsdl\"/></s:Body></s:Envelope>", op);
    int n = snprintf(req, sizeof(req), "POST /onvif/device_service HTTP/1.1\r\nHost: x\r\nConnection: close\r\n"
                     "Content-Type: application/soap+xml\r\nContent-Length: %zu\r\n\r\n%s", strlen(body), body);
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port) };
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr *)&a, sizeof(a)) < 0) { close(fd); return -1; }
    send(fd, req, (size_t)n, MSG_NOSIGNAL);
    size_t have = 0;
    for (;;) {
        struct pollfd p = { fd, POLLIN, 0 };
        if (poll(&p, 1, 4000) <= 0) break;
        ssize_t k = recv(fd, out + have, cap - 1 - have, 0);
        if (k <= 0) break;
        have += (size_t)k;
    }
    out[have] = 0;
    close(fd);
    return strncmp(out, "HTTP/1.1 200", 12) == 0 ? 0 : -2;
}

static int can_connect(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port) };
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int r = connect(fd, (struct sockaddr *)&a, sizeof(a));
    close(fd);
    return r == 0;
}

/* ------------------------------------------------------------------ parser */
static int pcfg(const char *s, int *en, int *port)
{
    onvif_cfg_t c = { -1, -1 };
    int r = onvif_cfg_parse(s, strlen(s), &c);
    if (en) *en = c.enabled;
    if (port) *port = c.port;
    return r;
}

static void test_parser(void)
{
    int en, port;
    CHECK(pcfg("enabled=1\nport=80\n", &en, &port) == 0 && en == 1 && port == 80, "valid: on, port 80");
    CHECK(pcfg("port=8080\nenabled=0\n", &en, &port) == 0 && en == 0 && port == 8080, "valid: any key order");
    CHECK(pcfg("enabled=1\nport=65535", &en, &port) == 0 && port == 65535, "no final newline, port 65535");
    CHECK(pcfg("enabled=1\r\nport=1\r\n", &en, &port) == 0 && port == 1, "CRLF tolerated like the helper, port 1");
    CHECK(pcfg("", NULL, NULL) != 0, "empty file invalid");
    CHECK(pcfg("enabled=1\n", NULL, NULL) != 0, "port missing");
    CHECK(pcfg("port=80\n", NULL, NULL) != 0, "enabled missing");
    CHECK(pcfg("enabled=1\nenabled=0\n", NULL, NULL) != 0, "enabled twice");
    CHECK(pcfg("enabled=1\nport=80\nport=81\n", NULL, NULL) != 0, "three lines");
    CHECK(pcfg("enabled=1\nport=80\n\n", NULL, NULL) != 0, "blank third line");
    CHECK(pcfg("enabled=1\nport=80\nfoo=1\n", NULL, NULL) != 0, "unknown key");
    CHECK(pcfg("enabled=1\nfoo=80\n", NULL, NULL) != 0, "unknown key instead of port");
    CHECK(pcfg("enabled=2\nport=80\n", NULL, NULL) != 0, "enabled=2");
    CHECK(pcfg("enabled=yes\nport=80\n", NULL, NULL) != 0, "enabled=yes");
    CHECK(pcfg("enabled=1 \nport=80\n", NULL, NULL) != 0, "trailing space");
    CHECK(pcfg(" enabled=1\nport=80\n", NULL, NULL) != 0, "leading space");
    CHECK(pcfg("Enabled=1\nport=80\n", NULL, NULL) != 0, "case sensitive keys");
    CHECK(pcfg("#c\nenabled=1\nport=80\n", NULL, NULL) != 0, "no comments");
    CHECK(pcfg("enabled=1\nport=0\n", NULL, NULL) != 0, "port 0");
    CHECK(pcfg("enabled=1\nport=65536\n", NULL, NULL) != 0, "port 65536");
    CHECK(pcfg("enabled=1\nport=080\n", NULL, NULL) == 0, "leading zero is a plain decimal like the helper");
    CHECK(pcfg("enabled=1\nport=+80\n", NULL, NULL) != 0, "port with a sign");
    CHECK(pcfg("enabled=1\nport=8x\n", NULL, NULL) != 0, "port with a letter");
    CHECK(pcfg("enabled=1\nport=\n", NULL, NULL) != 0, "empty port");
    CHECK(pcfg("enabled=1\nport=123456\n", NULL, NULL) != 0, "six digits");
    int bad[] = { 21, 22, 23, 24, 554, 8091 }, ok = 1;
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        char b[40];
        snprintf(b, sizeof(b), "enabled=1\nport=%d\n", bad[i]);
        if (pcfg(b, NULL, NULL) == 0 || onvif_cfg_port_ok(bad[i])) ok = 0;
    }
    CHECK(ok, "ports 21-24, 554 and 8091 refused");
    CHECK(onvif_cfg_port_ok(20) && onvif_cfg_port_ok(25) && onvif_cfg_port_ok(553) && onvif_cfg_port_ok(555) &&
          onvif_cfg_port_ok(8090) && onvif_cfg_port_ok(8092) && onvif_cfg_port_ok(80), "neighbours of the refused ports ok");
    char big[200];
    memset(big, 'a', sizeof(big));
    onvif_cfg_t tmp;
    CHECK(onvif_cfg_parse(big, sizeof(big), &tmp) != 0, "oversized input");
    CHECK(onvif_cfg_parse("enabled=1\0\nport=80\n", 18, &tmp) != 0, "NUL byte");

    char enc[64];
    onvif_cfg_urlenc("my cam/\xc3\xa9", enc, sizeof(enc));
    CHECK(!strcmp(enc, "my%20cam%2F%C3%A9"), "url encoding of the name scope");
    onvif_cfg_urlenc("akam-1a2b", enc, sizeof(enc));
    CHECK(!strcmp(enc, "akam-1a2b"), "plain hostname unchanged");

    /* file level */
    onvif_cfg_t c;
    unlink(cfgp);
    CHECK(onvif_cfg_load(&c) == 1 && c.enabled == 0, "absent file = off");
    put(cfgp, "enabled=1\nport=8080\n");
    CHECK(onvif_cfg_load(&c) == 0 && c.enabled == 1 && c.port == 8080, "file read");
    put(cfgp, "garbage\n");
    CHECK(onvif_cfg_load(&c) == -1 && c.enabled == 0, "invalid file = off");
    unlink(cfgp);
    CHECK(symlink("/etc/hostname", cfgp) == 0 && onvif_cfg_load(&c) == -1 && c.enabled == 0, "a symlink is refused");
    unlink(cfgp);
    CHECK(mkdir(cfgp, 0700) == 0 && onvif_cfg_load(&c) == -1, "a directory is refused");
    rmdir(cfgp);
    alarm(5); /* a FIFO must be refused, not block the start (watchdog would kill the daemon) */
    CHECK(mkfifo(cfgp, 0600) == 0 && onvif_cfg_load(&c) == -1 && c.enabled == 0, "a FIFO is refused without blocking");
    alarm(0);
    unlink(cfgp);
}

/* ------------------------------------------------------------------ state machine */
static long vol_switches_others(void)
{
    /* voluntary context switches of every thread but the calling one */
    long total = 0;
    char p[320], l[256];
    DIR *d = opendir("/proc/self/task");
    struct dirent *e;
    int self = (int)getpid();
    while (d && (e = readdir(d))) {
        if (e->d_name[0] == '.' || atoi(e->d_name) == self) continue;
        snprintf(p, sizeof(p), "/proc/self/task/%s/status", e->d_name);
        FILE *f = fopen(p, "r");
        while (f && fgets(l, sizeof(l), f)) {
            long v;
            if (sscanf(l, "voluntary_ctxt_switches: %ld", &v) == 1) total += v;
        }
        if (f) fclose(f);
    }
    if (d) closedir(d);
    return total;
}

static void test_machine(void)
{
    int fds0 = count_dir("/proc/self/fd"), thr0 = count_dir("/proc/self/task");
    int p1 = free_port(), p2 = free_port();
    while (p2 == p1) p2 = free_port();
    char b[16384];

    unlink(cfgp);
    auth_on();
    CHECK(!strcmp(state(), "off"), "initial state off");
    CHECK(!strcmp(reload(), "off"), "no flag file: off");
    CHECK(settled("/proc/self/fd", fds0) && settled("/proc/self/task", thr0), "off: no extra fd, no extra thread");

    set_cfg(0, p1);
    CHECK(!strcmp(reload(), "off") && !can_connect(p1), "enabled=0: off, nothing listens");

    put(cfgp, "enabled=1\nport=22\n");
    CHECK(!strcmp(reload(), "off") && settled("/proc/self/task", thr0), "invalid file (port 22): off, no thread");

    /* on */
    char want[ONVIF_STATE_MAX];
    set_cfg(1, p1);
    snprintf(want, sizeof(want), "on:%d", p1);
    CHECK(!strcmp(reload(), want) && !strcmp(state(), want), "enabled=1: on:<port>");
    CHECK(settled("/proc/self/task", thr0 + 2), "on: exactly two threads (http, wsd)");
    CHECK(http_post(p1, "GetSystemDateAndTime", b, sizeof(b)) == 0 && strstr(b, "GetSystemDateAndTimeResponse"),
          "end to end: GetSystemDateAndTime answered over HTTP");
    auth_off();
    reload();
    CHECK(http_post(p1, "GetDeviceInformation", b, sizeof(b)) == 0 &&
          strstr(b, "Manufacturer") && strstr(b, "svctest") && strstr(b, "GetDeviceInformationResponse"),
          "end to end: GetDeviceInformation answered (password off: reads open)");
    CHECK(http_post(p1, "SystemReboot", b, sizeof(b)) != 0 && strstr(b, "NotAuthorized"),
          "password off: a write is refused");
    CHECK(!strcmp(state(), want), "password off still starts ONVIF (spec 3.3)");
    auth_on();

    /* reload with the same port keeps serving */
    CHECK(!strcmp(reload(), want) && http_post(p1, "GetSystemDateAndTime", b, sizeof(b)) == 0,
          "reload on the same port keeps serving");
    CHECK(settled("/proc/self/task", thr0 + 2), "same-port reload does not add threads");

    /* idle wakeups */
    struct timespec ts = { 1, 0 };
    nanosleep(&ts, NULL);
    long a = vol_switches_others();
    ts.tv_sec = 3;
    nanosleep(&ts, NULL);
    long d = vol_switches_others() - a;
    printf("  info - idle wakeups of the ONVIF threads in 3 s: %ld\n", d);
    CHECK(d <= 1, "idle: at most one wakeup in 3 s (http poll has no timeout, wsd ticks every 30 s)");

    /* port change */
    set_cfg(1, p2);
    snprintf(want, sizeof(want), "on:%d", p2);
    CHECK(!strcmp(reload(), want), "port change: on:<new port>");
    CHECK(!can_connect(p1) && can_connect(p2), "old port closed, new port open");
    CHECK(http_post(p2, "GetSystemDateAndTime", b, sizeof(b)) == 0, "new port serves");
    CHECK(settled("/proc/self/task", thr0 + 2), "port change keeps two threads");

    /* no_password */
    auth_no_plain();
    CHECK(!strcmp(reload(), "no_password") && !strcmp(state(), "no_password"),
          "password on without clear-text credentials: no_password");
    CHECK(!can_connect(p2) && settled("/proc/self/task", thr0) && settled("/proc/self/fd", fds0),
          "no_password: stopped, no thread, no fd");
    put(cpath, "user=viewer\npass=short\n");
    chmod(cpath, 0644);
    cred_refresh(true);
    CHECK(!strcmp(reload(), "no_password"), "invalid credentials file (mode 0644): no_password");
    auth_on();
    CHECK(!strcmp(reload(), want), "credentials fixed: on again after a reload");

    /* port in use */
    int busy = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(busy, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a4 = { .sin_family = AF_INET };
    a4.sin_addr.s_addr = htonl(INADDR_ANY);
    CHECK(bind(busy, (struct sockaddr *)&a4, sizeof(a4)) == 0 && listen(busy, 1) == 0, "occupy a port");
    socklen_t al = sizeof(a4);
    getsockname(busy, (struct sockaddr *)&a4, &al);
    int pb = ntohs(a4.sin_port);
    set_cfg(1, pb);
    const char *s = reload();
    CHECK(!strcmp(s, "error:port_in_use") && !strcmp(state(), "error:port_in_use"), "port in use: error:port_in_use");
    CHECK(settled("/proc/self/task", thr0), "error: nothing left running (old port stopped too)");
    close(busy);
    CHECK(strncmp(reload(), "on:", 3) == 0, "port freed: a reload recovers");

    /* off again */
    set_cfg(0, pb);
    CHECK(!strcmp(reload(), "off") && !can_connect(pb), "enabled=0: off");
    CHECK(settled("/proc/self/fd", fds0) && settled("/proc/self/task", thr0),
          "off again: fds and threads back to the start");
    set_cfg(1, p1);
    reload();
    unlink(cfgp);
    CHECK(!strcmp(reload(), "off") && !can_connect(p1), "flag file removed: off");
    set_cfg(1, p1);
    reload();
    onvif_svc_stop();
    CHECK(!strcmp(state(), "off") && settled("/proc/self/fd", fds0) && settled("/proc/self/task", thr0),
          "onvif_svc_stop");
}

/* ------------------------------------------------------------------ WS-Discovery end to end */
static void test_wsd(void)
{
    int notify = socket(AF_INET, SOCK_DGRAM, 0), client = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET };
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t al = sizeof(a);
    bind(notify, (struct sockaddr *)&a, sizeof(a));
    getsockname(notify, (struct sockaddr *)&a, &al);
    static wsd_test_t t;
    t.port = 0;
    t.notify_port = ntohs(a.sin_port);
    t.group = htonl(INADDR_LOOPBACK);
    onvif_svc_wsd_test = &t;

    int p1 = free_port();
    auth_on();
    set_cfg(1, p1);
    CHECK(strncmp(reload(), "on:", 3) == 0, "on for the discovery test");
    char b[8192];
    struct pollfd pf = { notify, POLLIN, 0 };
    int n = poll(&pf, 1, 2000) > 0 ? (int)recv(notify, b, sizeof(b) - 1, 0) : -1;
    if (n > 0) b[n] = 0;
    CHECK(n > 0 && strstr(b, "<d:Hello>"), "Hello sent at start");
    char want[160];
    snprintf(want, sizeof(want), "http://127.0.0.1:%d/onvif/device_service", p1);
    CHECK(n > 0 && strstr(b, want), "Hello carries the XAddr with the HTTP port");

    char uuid[46];
    onvif_uuid(uuid);
    struct sockaddr_in w = { .sin_family = AF_INET, .sin_port = htons((uint16_t)wsd_port()) };
    w.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const char *probe =
        "<?xml version=\"1.0\"?><s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\""
        " xmlns:a=\"http://schemas.xmlsoap.org/ws/2004/08/addressing\" xmlns:d=\"http://schemas.xmlsoap.org/ws/2005/04/discovery\""
        " xmlns:dn=\"http://www.onvif.org/ver10/network/wsdl\"><s:Header><a:MessageID>urn:uuid:p1</a:MessageID>"
        "<a:To>urn:schemas-xmlsoap-org:ws:2005:04:discovery</a:To><a:Action>http://schemas.xmlsoap.org/ws/2005/04/discovery/Probe</a:Action>"
        "</s:Header><s:Body><d:Probe><d:Types>dn:NetworkVideoTransmitter</d:Types></d:Probe></s:Body></s:Envelope>";
    CHECK(wsd_port() > 0, "wsd bound");
    sendto(client, probe, strlen(probe), 0, (struct sockaddr *)&w, sizeof(w));
    pf.fd = client;
    n = poll(&pf, 1, 2000) > 0 ? (int)recv(client, b, sizeof(b) - 1, 0) : -1;
    if (n > 0) b[n] = 0;
    CHECK(n > 0 && strstr(b, "<d:ProbeMatches>") && strstr(b, want), "unicast Probe: ProbeMatches with the XAddr");
    CHECK(n > 0 && strstr(b, uuid), "ProbeMatches carries the device UUID");
    CHECK(n > 0 && strstr(b, "onvif://www.onvif.org/Profile/Streaming") && strstr(b, "onvif://www.onvif.org/Profile/S") &&
          strstr(b, "onvif://www.onvif.org/hardware/AK3918AV130") && strstr(b, "onvif://www.onvif.org/location/") &&
          strstr(b, "onvif://www.onvif.org/name/akam"), "ProbeMatches carries the scopes");
    CHECK(n > 0 && strstr(b, "dn:NetworkVideoTransmitter"), "default types");

    /* a hostname change shows up after the next reload (and the port may have changed: ask the new one) */
    put(hpath, "Lobby-Cam\n");
    reload();
    w.sin_port = htons((uint16_t)wsd_port());
    sendto(client, probe, strlen(probe), 0, (struct sockaddr *)&w, sizeof(w));
    n = poll(&pf, 1, 2000) > 0 ? (int)recv(client, b, sizeof(b) - 1, 0) : -1;
    if (n > 0) b[n] = 0;
    CHECK(n > 0 && strstr(b, "onvif://www.onvif.org/name/lobby-cam"), "hostname scope rebuilt on reload");

    onvif_svc_stop();
    close(notify);
    close(client);
    onvif_svc_wsd_test = NULL;
}

int main(int argc, char **argv)
{
    if (argc != 2) { fprintf(stderr, "usage: onvif_svc_test TEMPDIR\n"); return 2; }
    snprintf(dir, sizeof(dir), "%s", argv[1]);
    snprintf(cfgp, sizeof(cfgp), "%s/onvif", dir);
    snprintf(cpath, sizeof(cpath), "%s/credentials", dir);
    snprintf(apath, sizeof(apath), "%s/rtsp_auth", dir);
    snprintf(hpath, sizeof(hpath), "%s/hostname", dir);
    onvif_cfg_path = cfgp;
    onvif_ifname = "lo";
    onvif_hostname_path = hpath;
    rtsp_auth_db_init(&limiter, apath);
    cred_init(cpath, &limiter);
    test_parser();
    test_machine();
    test_wsd();
    printf("%s\n", fails ? "FAILED" : "all ok");
    return fails ? 1 : 0;
}
