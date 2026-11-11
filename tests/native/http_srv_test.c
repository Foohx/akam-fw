#define _GNU_SOURCE
/* Host test of src/ak_rtsp/http_srv.c over real localhost sockets: routing, keep-alive, pipelining, limits,
 * timeouts, connection cap, streamed responses, start/stop/start without fd or thread leaks. Built with
 * ASan/UBSan by tests/http_srv_test.sh. SIGPIPE is left at its default on purpose: a send without MSG_NOSIGNAL
 * would kill this test. */
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/resource.h>
#include <sys/socket.h>

#include "http_srv.h"
#include "soap.h"

static int g_failed;
static void test_defer(int port);
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); g_failed++; } } while (0)

static int count_fds(void)
{
    int n = 0;
    DIR *d = opendir("/proc/self/fd");
    while (readdir(d)) n++;
    closedir(d);
    return n - 3;       /* ".", "..", and the DIR's own fd */
}

static int count_threads(void)
{
    int n = 0;
    DIR *d = opendir("/proc/self/task");
    struct dirent *e;
    while ((e = readdir(d))) if (e->d_name[0] != '.') n++;
    closedir(d);
    return n;
}

static long long now_ms(void);

/* fd and thread counts back to the baseline. A closed fd or a joined thread can take a moment to leave /proc
 * (and the server reaps reset connections on its next poll round), more so on a loaded host: wait up to 3 s.
 * A real leak never settles and still fails. */
static int settled(int base_fds, int base_thr)
{
    long long end = now_ms() + 3000;
    for (;;) {
        if (count_fds() == base_fds && count_threads() == base_thr) return 1;
        if (now_ms() >= end) return 0;
        usleep(10000);
    }
}

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ---- handlers ---- */

static void h_echo(const http_req_t *q, http_resp_t *r, void *u)
{
    (void)u;
    http_resp_set(r, 200, "text/plain");
    http_resp_body_mem(r, q->body, q->body_len);       /* points into the request buffer: stays valid */
}

#define BIG_LEN 100000
static size_t big_produce(void *ctx, char *buf, size_t cap)
{
    size_t *off = ctx;
    size_t n = BIG_LEN - *off < cap ? BIG_LEN - *off : cap;
    for (size_t i = 0; i < n; i++) buf[i] = (char)('a' + (*off + i) % 26);
    *off += n;
    return n;
}
static void h_big(const http_req_t *q, http_resp_t *r, void *u)
{
    (void)q; (void)u;
    size_t *off = http_resp_alloc(r, sizeof(*off));
    http_resp_set(r, 200, "application/octet-stream");
    http_resp_body_stream(r, BIG_LEN, big_produce, off);
}

static size_t zero_produce(void *ctx, char *buf, size_t cap) { (void)ctx; (void)buf; (void)cap; return 0; }
static void h_stall(const http_req_t *q, http_resp_t *r, void *u)
{
    (void)q; (void)u;
    http_resp_set(r, 200, "text/plain");
    http_resp_body_stream(r, 500, zero_produce, NULL);   /* promises 500 bytes, delivers none */
}

static void h_auth(const http_req_t *q, http_resp_t *r, void *u)
{
    (void)u;
    if (!q->authorization) {
        http_resp_set(r, 401, NULL);
        http_resp_header(r, "WWW-Authenticate", "Digest realm=\"akam\", nonce=\"abc\"");
        return;
    }
    http_resp_set(r, 200, "text/plain");
    http_resp_body_mem(r, q->authorization, strlen(q->authorization));
}

static void h_addr(const http_req_t *q, http_resp_t *r, void *u)
{
    (void)u;
    char *b = http_resp_alloc(r, 128);
    char p[INET_ADDRSTRLEN], l[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &q->peer.sin_addr, p, sizeof p);
    inet_ntop(AF_INET, &q->local.sin_addr, l, sizeof l);
    snprintf(b, 128, "peer=%s local=%s:%d q=%s", p, l, ntohs(q->local.sin_port), q->query ? q->query : "-");
    http_resp_set(r, 200, "text/plain");
    http_resp_body_mem(r, b, strlen(b));
}

static void h_soap(const http_req_t *q, http_resp_t *r, void *u)
{
    (void)u;
    soap_tpl_t *t = http_resp_alloc(r, sizeof(*t));
    soap_elem_t op;
    if (soap_body_op(q->body, q->body_len, &op) != SOAP_OK) {
        soap_fault_init(t, 0, "ter:InvalidArgs", NULL, "bad <request>");
        http_resp_set(r, soap_fault_status(0), SOAP_CONTENT_TYPE);
    } else {
        soap_tpl_init(t, "<r>{{op}}</r>");
        char nm[64];
        snprintf(nm, sizeof nm, "%.*s", (int)op.name.n, op.name.p);
        soap_tpl_set(t, "op", nm);
        http_resp_set(r, 200, SOAP_CONTENT_TYPE);
    }
    http_resp_body_stream(r, soap_tpl_len(t), soap_tpl_read, t);
}

static void h_defer(const http_req_t *q, http_resp_t *r, void *u)
{
    (void)u;
    unsigned ms = q->query && !strncmp(q->query, "ms=", 3) ? (unsigned)atoi(q->query + 3) : 0;
    http_resp_set(r, 200, "text/plain");
    http_resp_body_mem(r, "deferred", 8);
    http_resp_defer(r, ms);
}

/* ---- client ---- */

static int dial(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port) };
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr *)&a, sizeof a) < 0) { close(fd); return -1; }
    return fd;
}

static void send_all(int fd, const char *s, size_t n)
{
    while (n) {
        ssize_t k = send(fd, s, n, MSG_NOSIGNAL);
        if (k <= 0) return;
        s += k;
        n -= (size_t)k;
    }
}
static void sendstr(int fd, const char *s) { send_all(fd, s, strlen(s)); }

typedef struct { int status; long clen; char *body; size_t blen; char hdr[2048]; int eof; } resp_t;

/* Read one response from fd (leftover bytes of the next one stay in *carry). timeout in ms. */
static int read_resp(int fd, resp_t *r, char *carry, size_t *clen, int timeout, size_t chunk)
{
    static char buf[262144];
    size_t have = *clen;
    memcpy(buf, carry, have);
    memset(r, 0, sizeof(*r));
    r->clen = -1;
    long long end = now_ms() + timeout;
    char *hdr_end = NULL;
    for (;;) {
        buf[have] = 0;
        hdr_end = strstr(buf, "\r\n\r\n");
        if (hdr_end) {
            size_t hl = (size_t)(hdr_end - buf) + 4;
            if (!r->status) {
                sscanf(buf, "HTTP/1.1 %d", &r->status);
                snprintf(r->hdr, sizeof r->hdr, "%.*s", (int)(hl < sizeof r->hdr ? hl : sizeof r->hdr - 1), buf);
                const char *c = strcasestr(r->hdr, "Content-Length:");
                r->clen = c ? atol(c + 15) : -1;
            }
            if (r->clen >= 0 && have >= hl + (size_t)r->clen) {
                r->blen = (size_t)r->clen;
                r->body = malloc(r->blen + 1);
                memcpy(r->body, buf + hl, r->blen);
                r->body[r->blen] = 0;
                have -= hl + r->blen;
                memcpy(carry, buf + hl + r->blen, have);
                *clen = have;
                return 0;
            }
        }
        long long left = end - now_ms();
        if (left <= 0) return -2;
        struct pollfd p = { fd, POLLIN, 0 };
        if (poll(&p, 1, (int)left) <= 0) return -2;
        size_t room = sizeof(buf) - 1 - have;
        ssize_t k = recv(fd, buf + have, room < chunk ? room : chunk, 0);
        if (k <= 0) { r->eof = 1; return -1; }
        have += (size_t)k;
    }
}

/* One request on a fresh or existing connection, returns the status or <0. */
static int simple(int fd, const char *req, resp_t *r)
{
    char carry[8] = "";
    size_t cl = 0;
    sendstr(fd, req);
    return read_resp(fd, r, carry, &cl, 3000, 65536);
}

static int expect_closed(int fd, int timeout_ms)
{
    long long end = now_ms() + timeout_ms;
    for (;;) {
        long long left = end - now_ms();
        if (left <= 0) return 0;
        struct pollfd p = { fd, POLLIN, 0 };
        if (poll(&p, 1, (int)left) <= 0) return 0;
        char b[256];
        ssize_t k = recv(fd, b, sizeof b, 0);
        if (k <= 0) return 1;
    }
}

#define POST_SOAP(len, body) "POST /onvif/device_service HTTP/1.1\r\nHost: x\r\nContent-Type: application/soap+xml\r\nContent-Length: " #len "\r\n\r\n" body

static void test_dormant_and_basic(int port)
{
    resp_t r;
    int fd = dial(port);
    CHECK(fd >= 0);

    CHECK(simple(fd, "POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\nhello", &r) == 0 && r.status == 200 &&
          r.blen == 5 && memcmp(r.body, "hello", 5) == 0);
    free(r.body);
    /* the same connection again (keep-alive), with a header written in odd case and a query string */
    CHECK(simple(fd, "GET /addr?x=1&y=2 HTTP/1.1\r\nhOsT: x\r\n\r\n", &r) == 0 && r.status == 200);
    CHECK(strstr(r.body, "peer=127.0.0.1 local=127.0.0.1:") && strstr(r.body, "q=x=1&y=2"));
    char want[32];
    snprintf(want, sizeof want, ":%d ", port);
    CHECK(strstr(r.body, want));
    CHECK(strstr(r.hdr, "Connection: keep-alive") != NULL);
    free(r.body);
    /* 404 / 405, connection stays usable */
    CHECK(simple(fd, "GET /nothing HTTP/1.1\r\nHost: x\r\n\r\n", &r) == 0 && r.status == 404 && r.clen == 0);
    free(r.body);
    CHECK(simple(fd, "GET /onvif/device_service HTTP/1.1\r\nHost: x\r\n\r\n", &r) == 0 && r.status == 405 && strstr(r.hdr, "Allow: POST"));
    free(r.body);
    CHECK(simple(fd, "POST /addr HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n\r\n", &r) == 0 && r.status == 405 && strstr(r.hdr, "Allow: GET"));
    free(r.body);
    CHECK(simple(fd, "DELETE /echo HTTP/1.1\r\nHost: x\r\n\r\n", &r) == 0 && r.status == 405);
    free(r.body);
    CHECK(simple(fd, POST_SOAP(0, ""), &r) == 0 && r.status == 400 && strstr(r.hdr, "application/soap+xml") && strstr(r.body, "env:Sender"));
    free(r.body);
    const char *sb = "<s:Envelope xmlns:s='x'><s:Body><t:Ping xmlns:t='y'/></s:Body></s:Envelope>";
    char rq[512];
    snprintf(rq, sizeof rq, "POST /onvif/device_service HTTP/1.1\r\nContent-Length: %zu\r\n\r\n%s", strlen(sb), sb);
    CHECK(simple(fd, rq, &r) == 0 && r.status == 200 && strcmp(r.body, "<r>t:Ping</r>") == 0);
    free(r.body);
    /* 401 with WWW-Authenticate, then with an Authorization header */
    CHECK(simple(fd, "GET /auth HTTP/1.1\r\n\r\n", &r) == 0 && r.status == 401 && strstr(r.hdr, "WWW-Authenticate: Digest realm=\"akam\""));
    free(r.body);
    CHECK(simple(fd, "GET /auth HTTP/1.1\r\nAuthorization:   Digest abc  \r\n\r\n", &r) == 0 && r.status == 200 && strcmp(r.body, "Digest abc") == 0);
    free(r.body);
    close(fd);
}

static void test_errors(int port)
{
    resp_t r;
    int fd;
    /* 411: POST without Content-Length, and chunked */
    fd = dial(port);
    CHECK(simple(fd, "POST /echo HTTP/1.1\r\nHost: x\r\n\r\n", &r) == 0 && r.status == 411 && strstr(r.hdr, "Connection: close"));
    free(r.body);
    CHECK(expect_closed(fd, 2000));
    close(fd);
    fd = dial(port);
    CHECK(simple(fd, "POST /echo HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n", &r) == 0 && r.status == 411);
    free(r.body);
    close(fd);
    /* 413: declared size over 16 KB, answered before the body is read */
    fd = dial(port);
    CHECK(simple(fd, "POST /echo HTTP/1.1\r\nContent-Length: 16385\r\n\r\n", &r) == 0 && r.status == 413);
    free(r.body);
    close(fd);
    /* ... and an actual 20 KB body that follows: the reply still arrives (lingering close) */
    fd = dial(port);
    {
        char *big = malloc(20000 + 100);
        int h = sprintf(big, "POST /echo HTTP/1.1\r\nContent-Length: 20000\r\n\r\n");
        memset(big + h, 'x', 20000);
        send_all(fd, big, (size_t)h + 20000);
        free(big);
        char carry[8];
        size_t cl = 0;
        CHECK(read_resp(fd, &r, carry, &cl, 3000, 262000) == 0 && r.status == 413);
        free(r.body);
    }
    close(fd);
    /* exactly 16 KB is accepted */
    fd = dial(port);
    {
        char *big = malloc(HTTP_BODY_MAX + 100);
        int h = sprintf(big, "POST /echo HTTP/1.1\r\nContent-Length: %d\r\n\r\n", HTTP_BODY_MAX);
        memset(big + h, 'y', HTTP_BODY_MAX);
        send_all(fd, big, (size_t)h + HTTP_BODY_MAX);
        free(big);
        char carry[8];
        size_t cl = 0;
        CHECK(read_resp(fd, &r, carry, &cl, 3000, 262000) == 0 && r.status == 200 && r.blen == HTTP_BODY_MAX);
        free(r.body);
    }
    close(fd);
    /* 431: header block over 4 KB */
    fd = dial(port);
    {
        char *big = malloc(6000);
        int h = sprintf(big, "GET /addr HTTP/1.1\r\nX-Big: ");
        memset(big + h, 'a', 5000);
        memcpy(big + h + 5000, "\r\n\r\n", 5);
        send_all(fd, big, (size_t)h + 5004);
        free(big);
        char carry[8];
        size_t cl = 0;
        CHECK(read_resp(fd, &r, carry, &cl, 3000, 262000) == 0 && r.status == 431);
        free(r.body);
    }
    close(fd);
    /* 400: garbage, bare LF, folded header, bad Content-Length, conflicting lengths; 505 */
    const char *bad[] = {
        "BLAH\r\n\r\n",
        "GET /addr HTTP/1.1\r\nBad header line\r\n\r\n",
        "GET /addr HTTP/1.1\r\nA: b\r\n folded\r\n\r\n",
        "POST /echo HTTP/1.1\r\nContent-Length: abc\r\n\r\n",
        "POST /echo HTTP/1.1\r\nContent-Length: 3\r\nContent-Length: 4\r\n\r\nabc",
        "GET addr HTTP/1.1\r\n\r\n",
        "GET /addr HTTP/1.1 extra\r\n\r\n",
        "GET /addr HTTPS/1.1\r\n\r\n",
        "POST /echo HTTP/1.1\r\nContent-Length: +3\r\n\r\nabc",
        "POST /echo HTTP/1.1\r\nContent-Length: -3\r\n\r\nabc",
        "POST /echo HTTP/1.1\r\nContent-Length: 4294967299\r\n\r\nabc",
        "POST /echo HTTP/1.1\r\nContent-Length: 3, 3\r\n\r\nabc",
    };
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
        fd = dial(port);
        int rc = simple(fd, bad[i], &r);
        CHECK(rc == 0 && r.status == 400);
        if (rc == 0 && r.status != 400) printf("  case %zu got %d\n", i, r.status);
        free(r.body);
        close(fd);
    }
    /* a NUL byte in the head (strlen-based senders cannot carry it, so send by length) */
    {
        static const char n1[] = "GET /addr HTTP/1.1\r\nX-A: b\0c\r\n\r\n", n2[] = "GET /ad\0dr HTTP/1.1\r\n\r\n";
        const char *nr[] = { n1, n2 };
        size_t nn[] = { sizeof n1 - 1, sizeof n2 - 1 };
        for (int i = 0; i < 2; i++) {
            fd = dial(port);
            send_all(fd, nr[i], nn[i]);
            char carry[8];
            size_t cl = 0;
            CHECK(read_resp(fd, &r, carry, &cl, 3000, 65536) == 0 && r.status == 400);
            free(r.body);
            close(fd);
        }
    }
    /* Content-Length together with Transfer-Encoding is refused (request smuggling) */
    fd = dial(port);
    CHECK(simple(fd, "POST /echo HTTP/1.1\r\nContent-Length: 3\r\nTransfer-Encoding: chunked\r\n\r\nabc", &r) == 0 && r.status == 411);
    free(r.body);
    close(fd);
    /* a producer that returns 0 before the declared length: the connection is dropped, no busy loop */
    fd = dial(port);
    sendstr(fd, "GET /stall HTTP/1.1\r\n\r\n");
    CHECK(expect_closed(fd, 2000));
    close(fd);
    fd = dial(port);
    CHECK(simple(fd, "GET /addr HTTP/2.0\r\n\r\n", &r) == 0 && r.status == 505);
    free(r.body);
    close(fd);
    /* HTTP/1.0 and Connection: close are honored */
    fd = dial(port);
    CHECK(simple(fd, "GET /addr HTTP/1.0\r\n\r\n", &r) == 0 && r.status == 200 && strstr(r.hdr, "Connection: close"));
    free(r.body);
    CHECK(expect_closed(fd, 2000));
    close(fd);
    fd = dial(port);
    CHECK(simple(fd, "GET /addr HTTP/1.1\r\nConnection: close\r\n\r\n", &r) == 0 && r.status == 200);
    free(r.body);
    CHECK(expect_closed(fd, 2000));
    close(fd);
    /* Expect: 100-continue */
    fd = dial(port);
    sendstr(fd, "POST /echo HTTP/1.1\r\nContent-Length: 3\r\nExpect: 100-continue\r\n\r\n");
    char b[64] = "";
    struct pollfd p = { fd, POLLIN, 0 };
    CHECK(poll(&p, 1, 2000) == 1);
    ssize_t k = recv(fd, b, sizeof b - 1, 0);
    CHECK(k > 0 && strncmp(b, "HTTP/1.1 100", 12) == 0);
    sendstr(fd, "abc");
    char carry[8];
    size_t cl = 0;
    CHECK(read_resp(fd, &r, carry, &cl, 3000, 262000) == 0 && r.status == 200 && memcmp(r.body, "abc", 3) == 0);
    free(r.body);
    close(fd);
}

static double cpu_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* accept() failing with EMFILE must not turn the poll loop into a busy loop */
static void test_emfile(int port)
{
    struct rlimit old, lim;
    getrlimit(RLIMIT_NOFILE, &old);
    lim = old;
    lim.rlim_cur = 128;
    if (lim.rlim_cur > old.rlim_max) { printf("  emfile test skipped\n"); return; }
    setrlimit(RLIMIT_NOFILE, &lim);
    int *extra = malloc(sizeof(int) * 128);
    int n = 0, fd;
    while (n < 128 && (fd = dup(0)) >= 0) extra[n++] = fd;
    close(extra[--n]);                                      /* one free descriptor for the client */
    int cl = dial(port);
    CHECK(cl >= 0);
    double t0 = cpu_s();
    usleep(600000);
    CHECK(cpu_s() - t0 < 0.25);                             /* a spin would burn ~0.6 s */
    while (n > 0) close(extra[--n]);
    free(extra);
    resp_t r;
    CHECK(simple(cl, "GET /addr HTTP/1.1\r\n\r\n", &r) == 0 && r.status == 200);   /* served once fds are back */
    free(r.body);
    close(cl);
    setrlimit(RLIMIT_NOFILE, &old);
}

static void test_pipeline_and_stream(int port)
{
    resp_t r;
    int fd = dial(port);
    /* three requests in one write, the last split across two packets */
    const char *three =
        "POST /echo HTTP/1.1\r\nContent-Length: 3\r\n\r\none"
        "POST /echo HTTP/1.1\r\nContent-Length: 3\r\n\r\ntwo"
        "POST /echo HTTP/1.1\r\nContent-Len";
    sendstr(fd, three);
    static char carry[262144];
    size_t cl = 0;
    CHECK(read_resp(fd, &r, carry, &cl, 3000, 262000) == 0 && memcmp(r.body, "one", 3) == 0);
    free(r.body);
    CHECK(read_resp(fd, &r, carry, &cl, 3000, 262000) == 0 && memcmp(r.body, "two", 3) == 0);
    free(r.body);
    usleep(50000);
    sendstr(fd, "gth: 5\r\n\r\nthree");
    CHECK(read_resp(fd, &r, carry, &cl, 3000, 262000) == 0 && r.blen == 5 && memcmp(r.body, "three", 5) == 0);
    free(r.body);
    /* pipelined big responses read in small pieces (back-pressure, partial writes) */
    sendstr(fd, "GET /big HTTP/1.1\r\n\r\nGET /big HTTP/1.1\r\n\r\n");
    for (int i = 0; i < 2; i++) {
        usleep(100000);
        CHECK(read_resp(fd, &r, carry, &cl, 5000, 700) == 0 && r.blen == BIG_LEN);
        int ok = r.blen == BIG_LEN;
        for (size_t j = 0; ok && j < BIG_LEN; j++) ok = r.body[j] == (char)('a' + j % 26);
        CHECK(ok);
        free(r.body);
    }
    close(fd);
    /* a client that asks for a big body and vanishes must not kill the process (SIGPIPE) */
    for (int i = 0; i < 5; i++) {
        fd = dial(port);
        sendstr(fd, "GET /big HTTP/1.1\r\n\r\n");
        usleep(20000);
        struct linger lg = { 1, 0 };
        setsockopt(fd, SOL_SOCKET, SO_LINGER, &lg, sizeof lg);   /* RST */
        close(fd);
    }
    fd = dial(port);
    CHECK(simple(fd, "GET /addr HTTP/1.1\r\n\r\n", &r) == 0 && r.status == 200);
    free(r.body);
    close(fd);
}

static void test_slowloris_default(int port)
{
    /* default 5 s: a client that trickles a head is cut off near 5 s even when it keeps sending, and other
     * clients are served meanwhile */
    int fd = dial(port);
    long long t0 = now_ms();
    sendstr(fd, "GET /addr HTTP/1.1\r\nX-A: ");
    resp_t r;
    /* connections reset by the previous test may still hold slots on a loaded host (the server answers 503
     * until it reaps them): retry for up to 3 s; the point is that a trickling client does not block others */
    int other_ok = 0;
    for (long long end = now_ms() + 3000; !other_ok && now_ms() < end; usleep(20000)) {
        int other = dial(port);
        other_ok = simple(other, "GET /addr HTTP/1.1\r\n\r\n", &r) == 0 && r.status == 200;
        free(r.body);
        close(other);
    }
    CHECK(other_ok);
    int got = 0;
    while (now_ms() - t0 < 9000) {
        struct pollfd p = { fd, POLLIN, 0 };
        if (poll(&p, 1, 300) > 0) { got = 1; break; }
        send_all(fd, "b", 1);                    /* a byte every 300 ms: does not reset the deadline */
    }
    long long dt = now_ms() - t0;
    CHECK(got);
    CHECK(dt >= 4500 && dt <= 6500);
    char b[256] = "";
    ssize_t k = recv(fd, b, sizeof b - 1, 0);
    CHECK(k > 0 && strncmp(b, "HTTP/1.1 408", 12) == 0);
    close(fd);
}

static void test_timeouts_short(int port)
{
    /* timeouts set to 700 ms (request) and 1500 ms (idle) by main() */
    int fd = dial(port);
    resp_t r;
    CHECK(simple(fd, "GET /addr HTTP/1.1\r\n\r\n", &r) == 0);
    free(r.body);
    long long t0 = now_ms();
    CHECK(expect_closed(fd, 5000));
    long long dt = now_ms() - t0;
    CHECK(dt >= 1200 && dt <= 2500);          /* idle keep-alive closed */
    close(fd);
    /* a body that never arrives */
    fd = dial(port);
    t0 = now_ms();
    sendstr(fd, "POST /echo HTTP/1.1\r\nContent-Length: 10\r\n\r\nabc");
    char carry[8];
    size_t cl = 0;
    CHECK(read_resp(fd, &r, carry, &cl, 4000, 262000) == 0 && r.status == 408);
    free(r.body);
    CHECK(now_ms() - t0 < 2000);
    close(fd);
    /* a connection that never sends anything is closed by the idle timeout */
    fd = dial(port);
    t0 = now_ms();
    CHECK(expect_closed(fd, 5000));
    CHECK(now_ms() - t0 <= 2500);
    close(fd);
}

static void test_conn_cap(int port)
{
    int fds[HTTP_MAX_CONNS];
    resp_t r;
    for (int i = 0; i < HTTP_MAX_CONNS; i++) {
        fds[i] = dial(port);
        CHECK(simple(fds[i], "GET /addr HTTP/1.1\r\n\r\n", &r) == 0 && r.status == 200);
        free(r.body);
    }
    int extra = dial(port);
    CHECK(extra >= 0);
    CHECK(simple(extra, "GET /addr HTTP/1.1\r\n\r\n", &r) == 0 && r.status == 503);
    free(r.body);
    CHECK(expect_closed(extra, 2000));
    close(extra);
    /* the established ones still work */
    CHECK(simple(fds[0], "GET /addr HTTP/1.1\r\n\r\n", &r) == 0 && r.status == 200);
    free(r.body);
    /* freeing a slot lets a new client in */
    close(fds[1]);
    usleep(100000);
    int n = dial(port);
    CHECK(simple(n, "GET /addr HTTP/1.1\r\n\r\n", &r) == 0 && r.status == 200);
    free(r.body);
    close(n);
    for (int i = 0; i < HTTP_MAX_CONNS; i++) if (i != 1) close(fds[i]);
    usleep(100000);
}

/* http_resp_defer: the answer is held back without blocking the loop. */
static void test_defer(int port)
{
    resp_t r;
    int fd = dial(port);
    long long t0 = now_ms();
    CHECK(simple(fd, "GET /defer?ms=400 HTTP/1.1\r\n\r\n", &r) == 0 && r.status == 200 && !strcmp(r.body, "deferred"));
    CHECK(now_ms() - t0 >= 380 && now_ms() - t0 < 1500);
    free(r.body);
    CHECK(simple(fd, "GET /defer?ms=0 HTTP/1.1\r\n\r\n", &r) == 0 && now_ms() - t0 < 1500);   /* keep-alive still works */
    free(r.body);

    /* a deferred response does not block the other connections */
    int a = dial(port), b = dial(port);
    char carry[8] = "";
    size_t cl = 0;
    t0 = now_ms();
    sendstr(a, "GET /defer?ms=600 HTTP/1.1\r\n\r\n");
    usleep(50000);
    long long t1 = now_ms();
    CHECK(simple(b, "GET /addr HTTP/1.1\r\n\r\n", &r) == 0 && r.status == 200);
    CHECK(now_ms() - t1 < 300);          /* served while A waits */
    free(r.body);
    CHECK(read_resp(a, &r, carry, &cl, 3000, 65536) == 0 && r.status == 200);
    CHECK(now_ms() - t0 >= 580);
    free(r.body);

    /* pipelined requests behind a deferred one keep their order */
    char carry2[8192] = "";
    size_t cl2 = 0;
    sendstr(b, "GET /defer?ms=200 HTTP/1.1\r\n\r\nGET /addr HTTP/1.1\r\n\r\n");
    CHECK(read_resp(b, &r, carry2, &cl2, 3000, 65536) == 0 && !strcmp(r.body, "deferred"));
    free(r.body);
    CHECK(read_resp(b, &r, carry2, &cl2, 3000, 65536) == 0 && strstr(r.body, "peer=") != NULL);
    free(r.body);

    /* a client that leaves during the wait costs nothing afterwards */
    int c = dial(port);
    sendstr(c, "GET /defer?ms=300 HTTP/1.1\r\n\r\n");
    usleep(30000);
    close(c);
    usleep(700000);
    CHECK(simple(a, "GET /addr HTTP/1.1\r\n\r\n", &r) == 0 && r.status == 200);
    free(r.body);
    close(a);
    close(b);
    close(fd);
}

int main(void)
{
    int base_fds = count_fds(), base_thr = count_threads();

    CHECK(http_srv_route("/echo", HTTP_M_POST, h_echo, NULL) == 0);
    CHECK(http_srv_route("/big", HTTP_M_GET, h_big, NULL) == 0);
    CHECK(http_srv_route("/stall", HTTP_M_GET, h_stall, NULL) == 0);
    CHECK(http_srv_route("/auth", HTTP_M_GET, h_auth, NULL) == 0);
    CHECK(http_srv_route("/addr", HTTP_M_GET, h_addr, NULL) == 0);
    CHECK(http_srv_route("/defer", HTTP_M_GET, h_defer, NULL) == 0);
    CHECK(http_srv_route("/onvif/device_service", HTTP_M_POST, h_soap, NULL) == 0);
    CHECK(http_srv_route("relative", HTTP_M_GET, h_addr, NULL) == -1);

    /* dormant: no thread, no fd, nothing listening */
    CHECK(settled(base_fds, base_thr));
    CHECK(http_srv_port() == 0);
    http_srv_stop();                                       /* stop when stopped: a no-op */

    CHECK(http_srv_start(0) == 0);
    int port = http_srv_port();
    CHECK(port > 0);
    CHECK(http_srv_route("/late", HTTP_M_GET, h_addr, NULL) == -1);   /* not while running */
    CHECK(count_threads() == base_thr + 1);
    CHECK(http_srv_start(0) == 0 && http_srv_start(port) == 0 && http_srv_port() == port);
    CHECK(http_srv_start(port + 1) == -1);
    test_dormant_and_basic(port);
    test_errors(port);
    test_pipeline_and_stream(port);
    test_defer(port);
    test_slowloris_default(port);

    http_srv_stop();
    CHECK(http_srv_port() == 0);
    CHECK(dial(port) < 0);
    CHECK(settled(base_fds, base_thr));

    /* start again with short timeouts */
    http_srv_set_timeouts(700, 1500);
    CHECK(http_srv_start(0) == 0);
    port = http_srv_port();
    test_timeouts_short(port);
    test_conn_cap(port);
    test_emfile(port);
    for (int i = 0; i < 3; i++) {                           /* start/stop cycling with open clients */
        int fd = dial(port);
        resp_t r;
        CHECK(simple(fd, "GET /addr HTTP/1.1\r\n\r\n", &r) == 0);
        free(r.body);
        http_srv_stop();                                    /* the client is still connected */
        CHECK(expect_closed(fd, 2000));
        close(fd);
        CHECK(settled(base_fds, base_thr));
        CHECK(http_srv_start(0) == 0);
        port = http_srv_port();
    }
    /* a start on a port that is taken fails cleanly, leaking nothing */
    int blocker = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET };
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    CHECK(bind(blocker, (struct sockaddr *)&a, sizeof a) == 0);
    listen(blocker, 1);
    socklen_t al = sizeof a;
    getsockname(blocker, (struct sockaddr *)&a, &al);
    http_srv_stop();
    CHECK(http_srv_start(ntohs(a.sin_port)) == -1);
    CHECK(http_srv_port() == 0);
    close(blocker);
    CHECK(settled(base_fds, base_thr));
    CHECK(http_srv_start(0) == 0);
    http_srv_stop();
    CHECK(settled(base_fds, base_thr));

    if (g_failed) { printf("http_srv_test: %d FAILED\n", g_failed); return 1; }
    printf("http_srv_test: all passed\n");
    return 0;
}
