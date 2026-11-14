/* Host test of src/ak_rtsp/wsd.c. Pure parts first (parse, match, build, rate limit), then the thread on loopback
 * through the test hook (wsd_test_t): Probes are sent by unicast to the bound port, Hello/Bye are redirected to a
 * receiver socket, so no multicast support is required from the host. */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <dirent.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "wsd.h"

static int g_failed;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); g_failed++; } } while (0)

#define UUID   "urn:uuid:11111111-2222-4333-8444-555555555555"
#define SCOPES "onvif://www.onvif.org/Profile/S onvif://www.onvif.org/name/Akam onvif://www.onvif.org/hardware/AK3918AV130"
#define MB0 "http://schemas.xmlsoap.org/ws/2005/04/discovery/strcmp0"

static int xaddr_cb(const struct in_addr *l, char *out, size_t cap, void *ctx)
{
    (void)ctx;
    char ip[32];
    inet_ntop(AF_INET, l, ip, sizeof(ip));
    return snprintf(out, cap, "http://%s:80/onvif/device_service", ip);
}

static const wsd_info_t INFO = { UUID, SCOPES, "dn:NetworkVideoTransmitter tds:Device", 7, xaddr_cb, NULL, 0, NULL };

#define ENVP(mid, body) \
    "<?xml version=\"1.0\"?><s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\"" \
    " xmlns:a=\"http://schemas.xmlsoap.org/ws/2004/08/addressing\" xmlns:d=\"http://schemas.xmlsoap.org/ws/2005/04/discovery\"" \
    " xmlns:dn=\"http://www.onvif.org/ver10/network/wsdl\"><s:Header><a:MessageID>" mid "</a:MessageID>" \
    "<a:To>urn:schemas-xmlsoap-org:ws:2005:04:discovery</a:To><a:Action>http://schemas.xmlsoap.org/ws/2005/04/discovery/Probe</a:Action>" \
    "</s:Header><s:Body>" body "</s:Body></s:Envelope>"

static int match_xml(const char *xml)
{
    wsd_msg_t m;
    int k = wsd_parse(xml, strlen(xml), &m);
    return k > 0 && wsd_match(&INFO, &m);
}

#define PROBE(inner) ENVP("urn:uuid:a", "<d:Probe>" inner "</d:Probe>")

static void test_match(void)
{
    CHECK(match_xml(ENVP("urn:uuid:a", "<d:Probe/>")));
    CHECK(match_xml(PROBE("<d:Types>dn:NetworkVideoTransmitter</d:Types>")));
    CHECK(match_xml(PROBE("<d:Types>foo:NetworkVideoTransmitter</d:Types>")));
    CHECK(match_xml(PROBE("<d:Types>  tds:Device  </d:Types>")));
    CHECK(match_xml(PROBE("<d:Types>x:Other dn:NetworkVideoTransmitter</d:Types>")));
    CHECK(!match_xml(PROBE("<d:Types>dn:NetworkVideoDisplay</d:Types>")));
    CHECK(!match_xml(PROBE("<d:Types>dn:Device2</d:Types>")));
    /* scopes */
    CHECK(match_xml(PROBE("<d:Scopes>onvif://www.onvif.org/Profile/S</d:Scopes>")));
    CHECK(match_xml(PROBE("<d:Scopes>onvif://www.onvif.org</d:Scopes>")));
    CHECK(match_xml(PROBE("<d:Scopes>onvif://www.onvif.org/Profile</d:Scopes>")));
    CHECK(match_xml(PROBE("<d:Scopes>ONVIF://WWW.ONVIF.ORG/Profile</d:Scopes>")));
    CHECK(match_xml(PROBE("<d:Scopes>onvif://www.onvif.org/Profile/S onvif://www.onvif.org/name</d:Scopes>")));
    CHECK(!match_xml(PROBE("<d:Scopes>onvif://www.onvif.org/Profile/T</d:Scopes>")));
    CHECK(!match_xml(PROBE("<d:Scopes>onvif://www.onvif.org/Prof</d:Scopes>")));   /* not a segment boundary */
    CHECK(!match_xml(PROBE("<d:Scopes>onvif://www.onvif.org/Profile/S onvif://www.onvif.org/location/x</d:Scopes>")));
    CHECK(!match_xml(PROBE("<d:Scopes>onvif://www.onvif.org/profile/S</d:Scopes>")));  /* path is case sensitive */
    CHECK(match_xml(PROBE("<d:Scopes MatchBy=\"" MB0 "\">onvif://www.onvif.org/Profile/S</d:Scopes>")));
    CHECK(!match_xml(PROBE("<d:Scopes MatchBy=\"" MB0 "\">onvif://www.onvif.org/Profile</d:Scopes>")));
    CHECK(!match_xml(PROBE("<d:Scopes MatchBy=\"http://schemas.xmlsoap.org/ws/2005/04/discovery/none\">onvif://www.onvif.org/Profile/S</d:Scopes>")));
    CHECK(!match_xml(PROBE("<d:Types>dn:NetworkVideoTransmitter</d:Types><d:Scopes>onvif://x/y</d:Scopes>")));
    /* not answered */
    CHECK(!match_xml(ENVP("urn:uuid:a", "<d:Hello/>")));
    CHECK(!match_xml(ENVP("urn:uuid:a", "<d:ProbeMatches/>")));
    CHECK(!match_xml(ENVP("urn:uuid:a", "<d:Bye/>")));
    CHECK(!match_xml("<s:Envelope xmlns:s=\"x\"><s:Body><d:Probe/></s:Body></s:Envelope>")); /* no MessageID */
    CHECK(!match_xml("garbage"));
    CHECK(!match_xml("<!DOCTYPE x><a/>"));
    /* Resolve */
    CHECK(match_xml(ENVP("urn:uuid:a", "<d:Resolve><a:EndpointReference><a:Address>" UUID "</a:Address></a:EndpointReference></d:Resolve>")));
    CHECK(!match_xml(ENVP("urn:uuid:a", "<d:Resolve><a:EndpointReference><a:Address>urn:uuid:other</a:Address></a:EndpointReference></d:Resolve>")));
    CHECK(!match_xml(ENVP("urn:uuid:a", "<d:Resolve/>")));
}

static int count_of(const char *hay, const char *needle)
{
    int n = 0;
    for (const char *p = hay; (p = strstr(p, needle)); p += strlen(needle)) n++;
    return n;
}

static void test_build(void)
{
    char o[4096];
    size_t n = wsd_build(o, sizeof(o), WSD_B_PROBEMATCH, &INFO, "http://10.0.0.5:80/onvif/device_service",
                         "urn:uuid:new", "urn:uuid:probe&<>", 99, 5);
    CHECK(n > 0);
    o[n] = 0;
    CHECK(strstr(o, "<a:RelatesTo>urn:uuid:probe&amp;&lt;&gt;</a:RelatesTo>"));
    CHECK(strstr(o, "<a:MessageID>urn:uuid:new</a:MessageID>"));
    CHECK(strstr(o, "InstanceId=\"99\" MessageNumber=\"5\""));
    CHECK(strstr(o, "<d:XAddrs>http://10.0.0.5:80/onvif/device_service</d:XAddrs>"));
    CHECK(strstr(o, "<d:Types>dn:NetworkVideoTransmitter tds:Device</d:Types>"));
    CHECK(strstr(o, "<d:Scopes>" SCOPES "</d:Scopes>"));
    CHECK(strstr(o, "<d:MetadataVersion>7</d:MetadataVersion>"));
    CHECK(strstr(o, "<a:Address>" UUID "</a:Address>"));
    CHECK(strstr(o, "discovery/ProbeMatches</a:Action>"));
    CHECK(strstr(o, "<a:To>http://schemas.xmlsoap.org/ws/2004/08/addressing/role/anonymous</a:To>"));
    CHECK(count_of(o, "<d:ProbeMatch>") == 1);
    wsd_msg_t m;
    CHECK(wsd_parse(o, n, &m) == WSD_K_NONE);   /* our own reply is well-formed and ignored */

    n = wsd_build(o, sizeof(o), WSD_B_RESOLVEMATCH, &INFO, "http://h/x", "urn:uuid:n", "urn:uuid:r", 1, 2);
    o[n] = 0;
    CHECK(strstr(o, "<d:ResolveMatches><d:ResolveMatch>") && strstr(o, "ResolveMatches</a:Action>") &&
          strstr(o, "<a:RelatesTo>urn:uuid:r<"));
    n = wsd_build(o, sizeof(o), WSD_B_HELLO, &INFO, "http://h/x", "urn:uuid:n", NULL, 1, 3);
    o[n] = 0;
    CHECK(strstr(o, "<d:Hello>") && strstr(o, "discovery/Hello</a:Action>") && !strstr(o, "RelatesTo") &&
          strstr(o, "<a:To>urn:schemas-xmlsoap-org:ws:2005:04:discovery</a:To>") && strstr(o, "MessageNumber=\"3\""));
    n = wsd_build(o, sizeof(o), WSD_B_BYE, &INFO, "", "urn:uuid:n", NULL, 1, 4);
    o[n] = 0;
    CHECK(strstr(o, "<d:Bye>") && strstr(o, "discovery/Bye</a:Action>") && !strstr(o, "XAddrs"));
    char small[100];
    CHECK(wsd_build(small, sizeof(small), WSD_B_HELLO, &INFO, "x", "y", NULL, 1, 1) == 0);

    char u[46], u2[46];
    wsd_uuid(u);
    wsd_uuid(u2);
    CHECK(strlen(u) == 45 && !strncmp(u, "urn:uuid:", 9) && u[17] == '-' && u[22] == '-');
    CHECK(u[23] == '4' && strchr("89ab", u[28]));
    CHECK(strcmp(u, u2) != 0);
}

static void test_rl(void)
{
    wsd_rl_t r;
    wsd_rl_init(&r, 1000);
    int ok = 0;
    for (int i = 0; i < 100; i++) ok += wsd_rl_take(&r, 1000);
    CHECK(ok == WSD_RATE_PER_SEC);
    CHECK(!wsd_rl_take(&r, 1010));
    CHECK(wsd_rl_take(&r, 1060));   /* 50 ms = 1 token at 20/s */
    ok = 0;
    for (int i = 0; i < 100; i++) ok += wsd_rl_take(&r, 5000);
    CHECK(ok == WSD_RATE_PER_SEC);  /* burst is capped */
}

static long long now_ms_t(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int count_dir(const char *d)
{
    int n = 0;
    DIR *dd = opendir(d);
    struct dirent *e;
    while (dd && (e = readdir(dd))) if (e->d_name[0] != '.') n++;
    if (dd) closedir(dd);
    return n;
}

static int udp(void)
{
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET };
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(s, (struct sockaddr *)&a, sizeof(a)) < 0) { close(s); return -1; }
    return s;
}

static uint16_t port_of(int s)
{
    struct sockaddr_in a;
    socklen_t l = sizeof(a);
    getsockname(s, (struct sockaddr *)&a, &l);
    return ntohs(a.sin_port);
}

static int rx(int s, char *buf, size_t cap, int ms)
{
    struct pollfd p = { s, POLLIN, 0 };
    if (poll(&p, 1, ms) <= 0) return -1;
    ssize_t n = recv(s, buf, cap - 1, 0);
    if (n < 0) return -1;
    buf[n] = 0;
    return (int)n;
}

static void tx(int s, uint16_t port, const char *d, size_t n)
{
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port) };
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    (void)!sendto(s, d, n, 0, (struct sockaddr *)&a, sizeof(a));
}

static unsigned num_after(const char *b, const char *key)
{
    const char *p = strstr(b, key);
    return p ? (unsigned)strtoul(p + strlen(key), NULL, 10) : 0;
}

/* Per-source limiter with an injected clock: deterministic, no sleeping. */
static void test_src_limit(void)
{
    /* The table is static and lives until the process ends: these buckets keep their fake clock (~1e6 ms) and an
     * empty bucket. Never use 127.0.0.1 here: test_socket probes from it with the real CLOCK_MONOTONIC (the uptime),
     * and on a freshly booted CI runner (uptime < 17 min) the stale bucket would make the daemon drop the probe. */
    const uint32_t A = 0x0200007f, B = 0x0300007f; /* 127.0.0.2, 127.0.0.3 */
    long long t = 1000000;
    int ok = 0;
    for (int i = 0; i < 50; i++) ok += wsd_src_ok(A, t);
    CHECK(ok == WSD_SRC_RATE);                       /* the burst, nothing more at one instant */
    CHECK(wsd_src_ok(B, t) == 1);                    /* another address has its own bucket */
    t += 1000;
    ok = 0;
    for (int i = 0; i < 50; i++) ok += wsd_src_ok(A, t);
    CHECK(ok == WSD_SRC_RATE);                       /* one second refills one burst */
    t += 200;
    ok = 0;
    for (int i = 0; i < 50; i++) ok += wsd_src_ok(A, t);
    CHECK(ok == 1);                                  /* 200 ms: one token */
    t += 60000;
    ok = 0;
    for (int i = 0; i < 50; i++) ok += wsd_src_ok(A, t);
    CHECK(ok == WSD_SRC_RATE);                       /* the bucket never exceeds the burst */
}

/* AppSequence InstanceId: grows at every start, with or without a usable clock. */
static void test_instance(void)
{
    char path[] = "/tmp/wsd_inst_XXXXXX";
    int fd = mkstemp(path);
    CHECK(fd >= 0);
    close(fd);
    unlink(path);
    unsigned a = wsd_next_instance(path, 0);
    unsigned b = wsd_next_instance(path, 0);
    unsigned c = wsd_next_instance(path, 0);
    CHECK(a == 1 && b == 2 && c == 3);               /* clock unset (1970) on every boot: still increasing */
    unsigned d = wsd_next_instance(path, 1794314096LL);
    CHECK(d == 1794314096u);                         /* a valid clock is used when it is ahead */
    unsigned e = wsd_next_instance(path, 100);       /* then the clock is lost again at the next boot */
    CHECK(e == d + 1);
    unsigned f = wsd_next_instance(path, 1794314090LL);   /* clock set a little earlier than before: never backwards */
    CHECK(f == e + 1);
    FILE *fp = fopen(path, "r");
    char buf[32] = "";
    CHECK(fp && fgets(buf, sizeof(buf), fp) && strtoul(buf, NULL, 10) == f);
    if (fp) fclose(fp);
    struct stat st;
    CHECK(stat(path, &st) == 0 && (st.st_mode & 0777) == 0644);
    unlink(path);
    char junk[300];
    snprintf(junk, sizeof(junk), "%s", path);
    fp = fopen(junk, "w");
    fputs("not a number\n", fp);
    fclose(fp);
    CHECK(wsd_next_instance(junk, 0) == 1);          /* garbage counts as 0 */
    fp = fopen(junk, "w");
    fputs("99999999999999999999\n", fp);
    fclose(fp);
    CHECK(wsd_next_instance(junk, 0) == 1);          /* overflowing counter is treated as garbage, not wrapped to 0 */
    unlink(junk);
    char nope[] = "/nonexistent-dir/wsd_instance";
    CHECK(wsd_next_instance(nope, 0) == 1);          /* an unwritable path does not break the start */
    CHECK(wsd_next_instance(NULL, 0) == 1 && wsd_next_instance(NULL, 1794314096LL) == 1794314096u);
    char tmp[310];
    snprintf(tmp, sizeof(tmp), "%s.new", path);
    CHECK(access(tmp, F_OK) != 0);                   /* no temp file left behind */
}

static void test_socket(void)
{
    int fds0 = count_dir("/proc/self/fd"), thr0 = count_dir("/proc/self/task");
    int notify = udp(), client = udp();
    CHECK(notify >= 0 && client >= 0);
    wsd_test_t t = { 0, port_of(notify), htonl(INADDR_LOOPBACK) };
    wsd_info_t info = INFO;
    info.test = &t;
    CHECK(wsd_port() == 0);
    CHECK(wsd_start(&info) == 0);
    CHECK(wsd_start(&info) == 0);   /* idempotent */
    uint16_t port = (uint16_t)wsd_port();
    CHECK(port != 0);
    CHECK(count_dir("/proc/self/task") == thr0 + 1);

    char b[8192];
    int n = rx(notify, b, sizeof(b), 1500);   /* Hello within 0.5 s */
    CHECK(n > 0 && strstr(b, "<d:Hello>") && strstr(b, "http://127.0.0.1:80/onvif/device_service"));
    unsigned hello_no = num_after(b, "MessageNumber=\"");
    unsigned inst = num_after(b, "InstanceId=\"");
    CHECK(hello_no == 1 && inst > 0);

    /* matching probe: unicast reply to the sender, RelatesTo = MessageID */
    const char *probe = ENVP("urn:uuid:probe-1", "<d:Probe><d:Types>dn:NetworkVideoTransmitter</d:Types></d:Probe>");
    long long t_first = now_ms_t();
    tx(client, port, probe, strlen(probe));
    n = rx(client, b, sizeof(b), 1500);
    CHECK(n > 0 && strstr(b, "<d:ProbeMatches>") && strstr(b, "<a:RelatesTo>urn:uuid:probe-1</a:RelatesTo>"));
    CHECK(strstr(b, "<d:XAddrs>http://127.0.0.1:80/onvif/device_service</d:XAddrs>"));
    unsigned m1 = num_after(b, "MessageNumber=\"");
    CHECK(m1 > hello_no && num_after(b, "InstanceId=\"") == inst);
    tx(client, port, probe, strlen(probe));
    n = rx(client, b, sizeof(b), 1500);
    CHECK(n > 0 && num_after(b, "MessageNumber=\"") == m1 + 1);

    /* silence: wrong scope, garbage, a Hello, an oversized datagram */
    const char *bad = ENVP("urn:uuid:p2", "<d:Probe><d:Scopes>onvif://www.onvif.org/Profile/T</d:Scopes></d:Probe>");
    tx(client, port, bad, strlen(bad));
    tx(client, port, "garbage", 7);
    const char *hello = ENVP("urn:uuid:p3", "<d:Hello/>");
    tx(client, port, hello, strlen(hello));
    static char big[9000];
    memset(big, ' ', sizeof(big));
    memcpy(big, probe, strlen(probe));
    tx(client, port, big, sizeof(big));
    CHECK(rx(client, b, sizeof(b), 900) < 0);

    const char *res = ENVP("urn:uuid:r1", "<d:Resolve><a:EndpointReference><a:Address>" UUID "</a:Address></a:EndpointReference></d:Resolve>");
    tx(client, port, res, strlen(res));
    n = rx(client, b, sizeof(b), 1500);
    CHECK(n > 0 && strstr(b, "<d:ResolveMatches>") && strstr(b, "<a:RelatesTo>urn:uuid:r1</a:RelatesTo>"));

    /* flood: replies are bounded (queue of 8, 20 tokens/s, WSD_SRC_RATE per source and second). The bucket refills
     * with real time, so the bound is the burst plus what the elapsed time since the first probe of this client
     * can have refilled (not a fixed number: a slow host would make that flaky). */
    for (int i = 0; i < 200; i++) tx(client, port, probe, strlen(probe));
    int got = 0;
    while (rx(client, b, sizeof(b), 900) > 0) got++;
    long long elapsed_ms = now_ms_t() - t_first;
    CHECK(got >= 1 && got <= WSD_SRC_RATE + (WSD_SRC_RATE * elapsed_ms) / 1000 + 1);
    CHECK(got <= 2 * WSD_SRC_RATE + 1 || elapsed_ms > 1000);   /* and a fast run really is cut off near the burst */

    wsd_stop();
    n = rx(notify, b, sizeof(b), 500);
    CHECK(n > 0 && strstr(b, "<d:Bye>"));
    CHECK(wsd_port() == 0);
    close(notify);
    close(client);
    CHECK(count_dir("/proc/self/fd") == fds0);
    /* the joined thread can take a moment to leave /proc/self/task */
    int thr_ok = 0;
    for (int i = 0; i < 200 && !(thr_ok = count_dir("/proc/self/task") == thr0); i++) usleep(10000);
    CHECK(thr_ok);

    CHECK(wsd_start(&info) == 0);   /* restart */
    wsd_stop();
    CHECK(count_dir("/proc/self/fd") == fds0);
    wsd_info_t bad_info = INFO;
    bad_info.xaddr = NULL;
    CHECK(wsd_start(&bad_info) == -1);
}

int main(void)
{
    test_match();
    test_build();
    test_rl();
    test_src_limit();
    test_instance();
    test_socket();
    if (g_failed) { printf("%d check(s) failed\n", g_failed); return 1; }
    printf("wsd_test: all passed\n");
    return 0;
}
