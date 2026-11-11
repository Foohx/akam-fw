/* WS-Discovery responder, see wsd.h. */
#define _GNU_SOURCE
#include "wsd.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include "soap.h"

#define WSD_NICE        10
#define WSD_STACK_BYTES (48 * 1024)
#define WSD_OBUF        4096
#define WSD_TICK_FAST   5000   /* retry join / Hello */
#define WSD_TICK_SLOW   30000  /* membership refresh */
#define WSD_URI_ANON    "http://schemas.xmlsoap.org/ws/2004/08/addressing/role/anonymous" /* matches the 2004/08 wsa */
#define WSD_URI_DISC    "urn:schemas-xmlsoap-org:ws:2005:04:discovery"
#define WSD_ACT         "http://schemas.xmlsoap.org/ws/2005/04/discovery/"

/* ---------------- pure parts ---------------- */

static int is_ws(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

int wsd_parse(const char *buf, size_t len, wsd_msg_t *m)
{
    memset(m, 0, sizeof(*m));
    soap_elem_t op, e, e2;
    int r = soap_body_op(buf, len, &op);
    if (r == SOAP_NOTFOUND) return WSD_K_NONE;
    if (r != SOAP_OK) return r;
    int kind;
    if (soap_is(&op, "Probe")) kind = WSD_K_PROBE;
    else if (soap_is(&op, "Resolve")) kind = WSD_K_RESOLVE;
    else return WSD_K_NONE;

    soap_span_t all = { buf, len };
    r = soap_find(all, "Header", &e);
    if (r == SOAP_NOTFOUND) return WSD_K_NONE;
    if (r != SOAP_OK) return r;
    r = soap_find(e.inner, "MessageID", &e2);
    if (r == SOAP_NOTFOUND) return WSD_K_NONE;
    if (r != SOAP_OK) return r;
    r = soap_text(&e2, m->msgid, sizeof(m->msgid), SOAP_TRIM);
    if (r < 0) return r;
    if (r == 0) return WSD_K_NONE;

    if (kind == WSD_K_PROBE) {
        r = soap_find(op.inner, "Types", &e);
        if (r == SOAP_OK) { r = soap_text(&e, m->types, sizeof(m->types), SOAP_TRIM); if (r < 0) return r; }
        else if (r != SOAP_NOTFOUND) return r;
        r = soap_find(op.inner, "Scopes", &e);
        if (r == SOAP_OK) {
            r = soap_text(&e, m->scopes, sizeof(m->scopes), SOAP_TRIM);
            if (r < 0) return r;
            r = soap_attr(&e, "MatchBy", m->matchby, sizeof(m->matchby));
            if (r < 0 && r != SOAP_NOTFOUND) return r;
            if (r == SOAP_NOTFOUND) m->matchby[0] = 0;
        } else if (r != SOAP_NOTFOUND) return r;
    } else {
        r = soap_find(op.inner, "EndpointReference", &e);
        if (r == SOAP_OK) r = soap_find(e.inner, "Address", &e2);
        if (r == SOAP_OK) { r = soap_text(&e2, m->epr, sizeof(m->epr), SOAP_TRIM); if (r < 0) return r; }
        else if (r != SOAP_NOTFOUND) return r;
    }
    m->kind = kind;
    return kind;
}

/* Probe scope vs one of our scopes, RFC 3986 style: scheme and authority case-insensitive, then a path prefix on a
 * segment boundary. */
static int scope_prefix(const char *p, size_t pl, const char *o, size_t ol)
{
    if (pl == 0 || pl > ol) return 0;
    size_t h = pl;
    const char *sep = memmem(p, pl, "://", 3);
    if (sep) {
        const char *s = memchr(sep + 3, '/', pl - (size_t)(sep + 3 - p));
        h = s ? (size_t)(s - p) : pl;
    }
    if (sep ? strncasecmp(p, o, h) != 0 : memcmp(p, o, h) != 0) return 0;
    if (memcmp(p + h, o + h, pl - h) != 0) return 0;
    return pl == ol || p[pl - 1] == '/' || o[pl] == '/';
}

int wsd_scope_match(const char *ps, size_t pl, const char *ours, size_t ol, const char *matchby)
{
    if (matchby && *matchby) {
        if (!strcmp(matchby, "http://schemas.xmlsoap.org/ws/2005/04/discovery/strcmp0"))
            return pl == ol && memcmp(ps, ours, pl) == 0;
        if (strcmp(matchby, "http://schemas.xmlsoap.org/ws/2005/04/discovery/rfc3986")) return 0;
    }
    return scope_prefix(ps, pl, ours, ol);
}

static int types_match(const char *types, size_t n)
{
    size_t i = 0;
    int any = 0;
    while (i < n) {
        while (i < n && is_ws(types[i])) i++;
        size_t s = i;
        while (i < n && !is_ws(types[i])) i++;
        if (i == s) break;
        any = 1;
        size_t ls = s;
        for (size_t k = s; k < i; k++) if (types[k] == ':') ls = k + 1;
        size_t ll = i - ls;
        if ((ll == 23 && !memcmp(types + ls, "NetworkVideoTransmitter", 23)) ||
            (ll == 6 && !memcmp(types + ls, "Device", 6)))
            return 1;
    }
    return !any;
}

int wsd_match(const wsd_info_t *info, const wsd_msg_t *m)
{
    if (m->kind == WSD_K_RESOLVE) return m->epr[0] && !strcmp(m->epr, info->uuid);
    if (m->kind != WSD_K_PROBE) return 0;
    if (!types_match(m->types, strlen(m->types))) return 0;
    const char *s = m->scopes;
    size_t sl = strlen(s), i = 0;
    while (i < sl) {
        while (i < sl && is_ws(s[i])) i++;
        size_t b = i;
        while (i < sl && !is_ws(s[i])) i++;
        if (i == b) break;
        int ok = 0;
        const char *o = info->scopes;
        size_t ol = strlen(o), j = 0;
        while (j < ol && !ok) {
            while (j < ol && is_ws(o[j])) j++;
            size_t ob = j;
            while (j < ol && !is_ws(o[j])) j++;
            if (j > ob && wsd_scope_match(s + b, i - b, o + ob, j - ob, m->matchby)) ok = 1;
        }
        if (!ok) return 0;
    }
    return 1;
}

#define NS_DECL \
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?><s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\"" \
    " xmlns:a=\"http://schemas.xmlsoap.org/ws/2004/08/addressing\"" \
    " xmlns:d=\"http://schemas.xmlsoap.org/ws/2005/04/discovery\"" \
    " xmlns:dn=\"http://www.onvif.org/ver10/network/wsdl\"" \
    " xmlns:tds=\"http://www.onvif.org/ver10/device/wsdl\">"
#define HDR_ID "<s:Header><a:MessageID>{{mid}}</a:MessageID>"
#define HDR_REL "<a:RelatesTo>{{rel}}</a:RelatesTo>"
#define HDR_TAIL(to, act) \
    "<a:To>" to "</a:To><a:Action>" WSD_ACT act "</a:Action>" \
    "<d:AppSequence InstanceId=\"{{inst}}\" MessageNumber=\"{{num}}\"/></s:Header><s:Body>"
#define EPR "<a:EndpointReference><a:Address>{{epr}}</a:Address></a:EndpointReference>"
#define MATCH_BODY EPR "<d:Types>{{types}}</d:Types><d:Scopes>{{scopes}}</d:Scopes><d:XAddrs>{{xaddrs}}</d:XAddrs>" \
                       "<d:MetadataVersion>{{mv}}</d:MetadataVersion>"
#define TAIL "</s:Body></s:Envelope>"

static const char T_PROBEMATCH[] = NS_DECL HDR_ID HDR_REL HDR_TAIL(WSD_URI_ANON, "ProbeMatches")
    "<d:ProbeMatches><d:ProbeMatch>" MATCH_BODY "</d:ProbeMatch></d:ProbeMatches>" TAIL;
static const char T_RESOLVEMATCH[] = NS_DECL HDR_ID HDR_REL HDR_TAIL(WSD_URI_ANON, "ResolveMatches")
    "<d:ResolveMatches><d:ResolveMatch>" MATCH_BODY "</d:ResolveMatch></d:ResolveMatches>" TAIL;
static const char T_HELLO[] = NS_DECL HDR_ID HDR_TAIL(WSD_URI_DISC, "Hello")
    "<d:Hello>" MATCH_BODY "</d:Hello>" TAIL;
static const char T_BYE[] = NS_DECL HDR_ID HDR_TAIL(WSD_URI_DISC, "Bye")
    "<d:Bye>" EPR "</d:Bye>" TAIL;

size_t wsd_build(char *out, size_t cap, int kind, const wsd_info_t *info, const char *xaddrs, const char *msgid,
                 const char *relates, unsigned instance, unsigned msgno)
{
    static const char *const tpls[] = { T_PROBEMATCH, T_RESOLVEMATCH, T_HELLO, T_BYE };
    if (kind < 0 || kind > WSD_B_BYE) return 0;
    char inst[16], num[16], mv[16];
    snprintf(inst, sizeof(inst), "%u", instance);
    snprintf(num, sizeof(num), "%u", msgno);
    snprintf(mv, sizeof(mv), "%u", info->metadata_version);
    soap_tpl_t t;
    soap_tpl_init(&t, tpls[kind]);
    int bad = soap_tpl_ref(&t, "mid", msgid) | soap_tpl_ref(&t, "inst", inst) | soap_tpl_ref(&t, "num", num) |
              soap_tpl_ref(&t, "epr", info->uuid);
    if (kind != WSD_B_BYE) {
        bad |= soap_tpl_ref(&t, "types", info->types) | soap_tpl_ref(&t, "scopes", info->scopes) |
               soap_tpl_ref(&t, "xaddrs", xaddrs) | soap_tpl_ref(&t, "mv", mv);
    }
    if (kind == WSD_B_PROBEMATCH || kind == WSD_B_RESOLVEMATCH) bad |= soap_tpl_ref(&t, "rel", relates);
    if (bad) return 0;
    size_t n = soap_tpl_len(&t);
    if (n == 0 || n > cap) return 0;
    size_t got = 0, r;
    while (got < n && (r = soap_tpl_read(&t, out + got, n - got)) > 0) got += r;
    return got == n ? n : 0;
}

static void rand_bytes(void *buf, size_t n)
{
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    ssize_t r = fd >= 0 ? read(fd, buf, n) : -1;
    if (fd >= 0) close(fd);
    if (r == (ssize_t)n) return;
    /* degraded system without /dev/urandom: clock mix */
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint32_t x = (uint32_t)ts.tv_nsec ^ ((uint32_t)ts.tv_sec * 2654435761u) ^ (uint32_t)(uintptr_t)buf;
    if (!x) x = 1;
    for (size_t i = 0; i < n; i++) {
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        ((unsigned char *)buf)[i] = (unsigned char)x;
    }
}

void wsd_uuid(char *out)
{
    unsigned char b[16];
    rand_bytes(b, sizeof(b));
    b[6] = (unsigned char)((b[6] & 0x0f) | 0x40);
    b[8] = (unsigned char)((b[8] & 0x3f) | 0x80);
    snprintf(out, 46, "urn:uuid:%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
}

void wsd_rl_init(wsd_rl_t *r, long long now_ms)
{
    r->t_ms = now_ms;
    r->tokens_x1000 = WSD_RATE_PER_SEC * 1000;
}

int wsd_rl_take(wsd_rl_t *r, long long now_ms)
{
    long long dt = now_ms - r->t_ms;
    if (dt < 0) dt = 0;
    r->t_ms = now_ms;
    long long tk = r->tokens_x1000 + dt * WSD_RATE_PER_SEC; /* ms * tokens/s = milli-tokens */
    if (tk > WSD_RATE_PER_SEC * 1000) tk = WSD_RATE_PER_SEC * 1000;
    if (tk < 1000) { r->tokens_x1000 = (int)tk; return 0; }
    r->tokens_x1000 = (int)(tk - 1000);
    return 1;
}

/* ---------------- runtime ---------------- */

typedef struct {
    int  kind;                 /* WSD_B_* */
    long long due;
    struct sockaddr_in dst;
    char relates[WSD_ID_MAX];
} job_t;

static struct {
    pthread_mutex_t lock;
    int running;
    volatile int stop;
    pthread_t tid;
    int fd, wake_r, wake_w;
    volatile int port;  /* plain int: read lock-free by wsd_port(), written under the lock */
    wsd_info_t info;
    char uuid[WSD_ID_MAX], scopes[WSD_SCOPES_MAX], types[WSD_TYPES_MAX];
    struct sockaddr_in group;  /* Hello/Bye destination */
    int do_join;
    unsigned instance, msgno;
    job_t jobs[WSD_MAX_PENDING];
    int njobs;
    wsd_rl_t rl;
} g = { .lock = PTHREAD_MUTEX_INITIALIZER, .fd = -1, .wake_r = -1, .wake_w = -1 };

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Local address the kernel would use to reach dst. */
static int local_for(const struct sockaddr_in *dst, struct in_addr *out)
{
    int s = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (s < 0) return -1;
    struct sockaddr_in a;
    socklen_t al = sizeof(a);
    int ok = connect(s, (const struct sockaddr *)dst, sizeof(*dst)) == 0 &&
             getsockname(s, (struct sockaddr *)&a, &al) == 0 && a.sin_addr.s_addr != 0;
    if (ok) *out = a.sin_addr;
    close(s);
    return ok ? 0 : -1;
}

/* Returns 0 when sent (or dropped for good), -1 when it should be retried later (no local address yet). */
static int send_msg(int kind, const struct sockaddr_in *dst, const char *relates)
{
    char xa[WSD_XADDR_MAX] = "";
    if (kind != WSD_B_BYE) {
        struct in_addr la;
        if (IN_MULTICAST(ntohl(dst->sin_addr.s_addr)) && g.info.mcast_if) la.s_addr = g.info.mcast_if;
        else if (local_for(dst, &la) < 0) {
            if (!g.info.mcast_if) return -1;
            la.s_addr = g.info.mcast_if;
        }
        int n = g.info.xaddr(&la, xa, sizeof(xa), g.info.ctx);
        if (n < 0) return 0;
        xa[sizeof(xa) - 1] = 0;
    }
    char mid[46], out[WSD_OBUF];
    wsd_uuid(mid);
    size_t n = wsd_build(out, sizeof(out), kind, &g.info, xa, mid, relates, g.instance, g.msgno + 1);
    if (n) {
        g.msgno++;
        (void)!sendto(g.fd, out, n, MSG_DONTWAIT | MSG_NOSIGNAL, (const struct sockaddr *)dst, sizeof(*dst));
    }
    return 0;
}

static int enqueue(int kind, const struct sockaddr_in *dst, const char *relates, long long now)
{
    if (g.njobs >= WSD_MAX_PENDING) return 0;
    if (!wsd_rl_take(&g.rl, now)) return 0;
    unsigned char r[2];
    rand_bytes(r, 2);
    job_t *j = &g.jobs[g.njobs++];
    j->kind = kind;
    j->due = now + (long long)((r[0] << 8 | r[1]) % (WSD_MAX_DELAY_MS + 1));
    j->dst = *dst;
    snprintf(j->relates, sizeof(j->relates), "%s", relates ? relates : "");
    return 1;
}

/* Per-source limit (reflection to a spoofed victim): WSD_SRC_RATE requests/s per IP, small LRU table. Wsd thread only. */
static struct { uint32_t ip; long long t_ms; int tokens_x1000; } srcs[WSD_SRC_SLOTS];

int wsd_src_ok(uint32_t ip, long long now)
{
    int k = -1, old = 0;
    for (int i = 0; i < WSD_SRC_SLOTS; i++) {
        if (srcs[i].ip == ip) { k = i; break; }
        if (srcs[i].t_ms < srcs[old].t_ms) old = i;
    }
    if (k < 0) { k = old; srcs[k].ip = ip; srcs[k].t_ms = now; srcs[k].tokens_x1000 = WSD_SRC_RATE * 1000; }
    long long dt = now - srcs[k].t_ms;
    if (dt < 0) dt = 0;
    long long tk = srcs[k].tokens_x1000 + dt * WSD_SRC_RATE;
    if (tk > WSD_SRC_RATE * 1000) tk = WSD_SRC_RATE * 1000;
    srcs[k].t_ms = now;
    if (tk < 1000) { srcs[k].tokens_x1000 = (int)tk; return 0; }
    srcs[k].tokens_x1000 = (int)(tk - 1000);
    return 1;
}

/* A reply is only sent to a plausible unicast source. */
static int src_valid(const struct sockaddr_in *a)
{
    uint32_t h = ntohl(a->sin_addr.s_addr);
    return a->sin_family == AF_INET && a->sin_port != 0 && h != 0 && h != 0xffffffffu && !IN_MULTICAST(h) &&
           (h >> 24) != 0 && (h >> 28) != 0xf;
}

/* 0 failed, 1 joined (already, or refreshed), 2 newly joined. With `refresh` the membership is dropped and added
 * again: adding a group the socket already belongs to just fails with EADDRINUSE and sends no IGMP report, so a
 * switch that snoops IGMP and forgot us (or a querier that restarted) would never see us again. */
static int join_group(int refresh)
{
    if (!g.do_join) return 1;
    struct ip_mreq mr;
    mr.imr_multiaddr = g.group.sin_addr;
    mr.imr_interface.s_addr = g.info.mcast_if;
    if (refresh) (void)setsockopt(g.fd, IPPROTO_IP, IP_DROP_MEMBERSHIP, &mr, sizeof(mr));
    if (setsockopt(g.fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mr, sizeof(mr)) == 0) return refresh ? 1 : 2;
    return errno == EADDRINUSE ? 1 : 0;
}

static void *wsd_thread(void *arg)
{
    (void)arg;
    setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), WSD_NICE);
    static char rx[WSD_MAX_DGRAM + 1];
    long long next_tick = 0, next_rejoin = now_ms() + WSD_REJOIN_MS;
    int need_hello = 1, joined = 0;
    while (!g.stop) {
        long long now = now_ms();
        for (int i = 0; i < g.njobs;) {
            if (g.jobs[i].due <= now) {
                job_t j = g.jobs[i];
                g.jobs[i] = g.jobs[--g.njobs];
                if (send_msg(j.kind, &j.dst, j.relates) < 0 && j.kind == WSD_B_HELLO) {
                    need_hello = 1;
                    next_tick = now + WSD_TICK_FAST;
                }
            } else {
                i++;
            }
        }
        if (now >= next_tick) {
            int refresh = joined && now >= next_rejoin;
            if (refresh) next_rejoin = now + WSD_REJOIN_MS;
            int r = join_group(refresh);
            if (refresh && r == 0) joined = 0;       /* the drop worked, the add did not: retry fast, Hello when back */
            if (r == 2) need_hello = 1; /* (re)joined after a failure or at start */
            joined = r != 0;
            if (need_hello && enqueue(WSD_B_HELLO, &g.group, NULL, now)) need_hello = 0;
            next_tick = now + (joined && !need_hello ? WSD_TICK_SLOW : WSD_TICK_FAST);
        }
        long long wake = next_tick;
        for (int i = 0; i < g.njobs; i++) if (g.jobs[i].due < wake) wake = g.jobs[i].due;
        long long to = wake - now_ms();
        struct pollfd p[2] = { { g.fd, POLLIN, 0 }, { g.wake_r, POLLIN, 0 } };
        if (poll(p, 2, to < 0 ? 0 : (int)(to > 60000 ? 60000 : to)) < 0 && errno != EINTR) break;
        if (p[1].revents) break;
        for (int k = 0; k < 8 && (p[0].revents & POLLIN); k++) {
            struct sockaddr_in from;
            socklen_t fl = sizeof(from);
            ssize_t n = recvfrom(g.fd, rx, sizeof(rx), MSG_DONTWAIT, (struct sockaddr *)&from, &fl);
            if (n < 0) break;
            if (n > WSD_MAX_DGRAM || fl != sizeof(from)) continue;
            wsd_msg_t m;
            int kd = wsd_parse(rx, (size_t)n, &m);
            if (kd > 0 && src_valid(&from) && wsd_match(&g.info, &m) && wsd_src_ok(from.sin_addr.s_addr, now_ms()))
                enqueue(kd == WSD_K_PROBE ? WSD_B_PROBEMATCH : WSD_B_RESOLVEMATCH, &from, m.msgid, now_ms());
        }
    }
    send_msg(WSD_B_BYE, &g.group, NULL);
    return NULL;
}

static void teardown(void)
{
    if (g.fd >= 0) close(g.fd);
    if (g.wake_r >= 0) close(g.wake_r);
    if (g.wake_w >= 0) close(g.wake_w);
    g.fd = g.wake_r = g.wake_w = -1;
    g.port = 0;
}

unsigned wsd_next_instance(const char *path, long long wall_s)
{
    unsigned long long prev = 0, next;
    if (path) {
        FILE *f = fopen(path, "re");
        if (f) {
            char b[24];
            if (fgets(b, sizeof(b), f) && strspn(b, "0123456789") >= 1) prev = strtoull(b, NULL, 10);
            if (prev > 0xfffffff0ULL) prev = 0;                  /* garbage or overflow */
            fclose(f);
        }
    }
    next = prev + 1;
    if (wall_s >= 1704067200LL && (unsigned long long)wall_s > next) next = (unsigned long long)wall_s;
    if (next > 0xfffffff0ULL) next = 1;                         /* xs:unsignedInt: wrap rather than overflow */
    if (path && next != prev) {
        char tmp[300], line[24];
        snprintf(tmp, sizeof(tmp), "%s.new", path);
        int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0644);
        if (fd >= 0) {
            int n = snprintf(line, sizeof(line), "%llu\n", next);
            int ok = write(fd, line, (size_t)n) == n && fsync(fd) == 0;   /* data first, then the rename */
            close(fd);
            if (!ok || rename(tmp, path) != 0) unlink(tmp);
        }
    }
    return (unsigned)next;
}

int wsd_start(const wsd_info_t *in)
{
    int rc = -1;
    pthread_mutex_lock(&g.lock);
    if (g.running) { rc = 0; goto out; }
    if (!in || !in->uuid || !in->xaddr || strlen(in->uuid) >= sizeof(g.uuid) ||
        (in->scopes && strlen(in->scopes) >= sizeof(g.scopes)) ||
        (in->types && strlen(in->types) >= sizeof(g.types))) { errno = EINVAL; goto out; }
    g.info = *in;
    snprintf(g.uuid, sizeof(g.uuid), "%s", in->uuid);
    snprintf(g.scopes, sizeof(g.scopes), "%s", in->scopes ? in->scopes : "");
    snprintf(g.types, sizeof(g.types), "%s", in->types ? in->types : "dn:NetworkVideoTransmitter tds:Device");
    g.info.uuid = g.uuid;
    g.info.scopes = g.scopes;
    g.info.types = g.types;
    g.info.test = NULL;
    wsd_test_t tst = { 0, 0, 0 };
    if (in->test) tst = *in->test;
    memset(&g.group, 0, sizeof(g.group));
    g.group.sin_family = AF_INET;
    g.group.sin_port = htons(tst.notify_port ? tst.notify_port : WSD_PORT);
    if (tst.group) g.group.sin_addr.s_addr = tst.group;
    else inet_pton(AF_INET, WSD_GROUP, &g.group.sin_addr);
    g.do_join = IN_MULTICAST(ntohl(g.group.sin_addr.s_addr));
    g.njobs = 0;
    /* One InstanceId and one flash write per boot: a rebuild of the scopes (wsd_stop + wsd_start in the same process)
     * keeps the value and carries on counting messages, it must not write the file again. */
    static int inst_done;
    static unsigned inst_val;
    if (in->test || !inst_done) {
        g.msgno = 0;
        g.instance = wsd_next_instance(in->test ? NULL : WSD_INSTANCE_FILE, (long long)time(NULL));
        if (!in->test) { inst_done = 1; inst_val = g.instance; }
    } else {
        g.instance = inst_val;
    }
    wsd_rl_init(&g.rl, now_ms());
    g.stop = 0;

    int wp[2];
    if (pipe2(wp, O_CLOEXEC | O_NONBLOCK) < 0) goto out;
    g.wake_r = wp[0];
    g.wake_w = wp[1];
    g.fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (g.fd < 0) { int e = errno; teardown(); errno = e; goto out; }
    int one = 1, ttl = 1, zero = 0;
    setsockopt(g.fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    setsockopt(g.fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
    if (!in->test) setsockopt(g.fd, IPPROTO_IP, IP_MULTICAST_LOOP, &zero, sizeof(zero));
    if (g.info.mcast_if) {
        struct in_addr ia = { g.info.mcast_if };
        setsockopt(g.fd, IPPROTO_IP, IP_MULTICAST_IF, &ia, sizeof(ia));
    }
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons(in->test ? tst.port : WSD_PORT); /* test hook: 0 = ephemeral */
    socklen_t al = sizeof(a);
    if (bind(g.fd, (struct sockaddr *)&a, sizeof(a)) < 0 || getsockname(g.fd, (struct sockaddr *)&a, &al) < 0) {
        int e = errno;
        teardown();
        errno = e;
        goto out;
    }
    g.port = ntohs(a.sin_port);
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, WSD_STACK_BYTES);
    int pr = pthread_create(&g.tid, &attr, wsd_thread, NULL);
    pthread_attr_destroy(&attr);
    if (pr != 0) { teardown(); errno = pr; goto out; }
    g.running = 1;
    printf("[wsd] listening on UDP %d\n", g.port);
    rc = 0;
out:
    pthread_mutex_unlock(&g.lock);
    return rc;
}

void wsd_stop(void)
{
    pthread_mutex_lock(&g.lock);
    if (g.running) {
        g.stop = 1;
        (void)!write(g.wake_w, "x", 1);
        pthread_join(g.tid, NULL);
        g.running = 0;
        teardown();
        printf("[wsd] stopped\n");
    }
    pthread_mutex_unlock(&g.lock);
}

int wsd_port(void)
{
    return g.port;
}
