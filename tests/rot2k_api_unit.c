/* Host unit tests of the control API transport pieces (specs/control-api.md phase 2): the strict HTTP request-head parser
 * (http_req.c), the rate limits (api_limit.c) and the event ring (api_events.c). The router, the registry table and the server
 * are covered by tests/rot2k_control_api_http_test.sh. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "api_events.h"
#include "api_limit.h"
#include "api_sig.h"
#include "http_req.h"

static int fails, total;
#define CHECK(c) do { total++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static int parse(const char *text, hreq_t *r)
{
    static char buf[HREQ_HEAD_MAX + 64];
    long he;
    size_t n = strlen(text);
    memcpy(buf, text, n + 1);
    he = hreq_head_end(buf, n);
    if (he <= 0)
        return he == 0 ? -1 : he == -1 ? 431 : 400;
    return hreq_parse(buf, (size_t)he, r);
}

static void test_parser(void)
{
    hreq_t r;
    char big[HREQ_HEAD_MAX + 64];
    CHECK(parse("GET /akam/v1/info HTTP/1.1\r\nHost: x\r\n\r\n", &r) == 0);
    CHECK(!strcmp(r.method, "GET") && !strcmp(r.path, "/akam/v1/info") && r.query == NULL && r.content_length == -1);
    CHECK(parse("GET /akam/v1/settings?prefix=led.&a=%2B+b HTTP/1.1\r\n\r\n", &r) == 0);
    CHECK(!strcmp(r.path, "/akam/v1/settings") && !strcmp(r.query, "prefix=led.&a=%2B+b")); /* the raw '+' is kept */
    CHECK(parse("GET /a?x HTTP/1.0\r\n\r\n", &r) == 0 && r.http10);
    CHECK(parse("PATCH /akam/v1/settings HTTP/1.1\r\nContent-Length: 12\r\nX-Akam-Key: 3fa91c07\r\nx-akam-nonce: ab\r\n"
                "X-AKAM-SIGNATURE:  cd  \r\n\r\n", &r) == 0);
    CHECK(r.content_length == 12 && !strcmp(r.key, "3fa91c07") && !strcmp(r.nonce, "ab") && !strcmp(r.signature, "cd"));
    CHECK(r.timestamp == NULL && r.challenge == NULL);
    /* limits and rejections */
    CHECK(parse("GET /a HTTP/1.1\r\nContent-Length: 8193\r\n\r\n", &r) == 413);
    CHECK(parse("GET /a HTTP/1.1\r\nContent-Length: 8192\r\n\r\n", &r) == 0 && r.content_length == 8192);
    CHECK(parse("GET /a HTTP/1.1\r\nContent-Length: 1\r\nContent-Length: 1\r\n\r\n", &r) == 400); /* twice, even equal */
    CHECK(parse("GET /a HTTP/1.1\r\nContent-Length: +1\r\n\r\n", &r) == 400);
    CHECK(parse("GET /a HTTP/1.1\r\nContent-Length: 1x\r\n\r\n", &r) == 400);
    CHECK(parse("GET /a HTTP/1.1\r\nContent-Length: \r\n\r\n", &r) == 400);
    CHECK(parse("GET /a HTTP/1.1\r\nX-Akam-Key: a\r\nX-Akam-Key: b\r\n\r\n", &r) == 400);
    CHECK(parse("GET /a HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n", &r) == 0 && r.chunked);
    CHECK(parse("GET /a HTTP/1.1\r\nExpect: 100-continue\r\n\r\n", &r) == 0 && r.expect);
    CHECK(parse("GET /a HTTP/1.1\r\n folded: x\r\n\r\n", &r) == 400);       /* obsolete folding */
    CHECK(parse("GET /a HTTP/1.1\r\nBad Name: x\r\n\r\n", &r) == 400);
    CHECK(parse("GET /a HTTP/1.1\r\nNoColon\r\n\r\n", &r) == 400);
    CHECK(parse("GET /a HTTP/1.1\r\n: x\r\n\r\n", &r) == 400);
    CHECK(parse("GET /a HTTP/1.1\r\nX: a\x01" "b\r\n\r\n", &r) == 400);     /* control byte */
    CHECK(parse("GET /a HTTP/1.1\r\nX: a\xc3\xa9\r\n\r\n", &r) == 400);     /* non-ASCII */
    CHECK(parse("GET /a HTTP/1.1\r\nX: a\rb\r\n\r\n", &r) == 400);          /* lone CR */
    CHECK(parse("GET /a HTTP/1.1\nHost: x\n\n", &r) == 400);                /* bare LF */
    CHECK(parse("GET /a HTTP/1.1\r\nHost: x\n\r\n\r\n", &r) == 400);
    CHECK(parse("get /a HTTP/1.1\r\n\r\n", &r) == 400);                     /* method must be upper case */
    CHECK(parse("G /a HTTP/1.1\r\n\r\n", &r) == 400);
    CHECK(parse("GETGETGET /a HTTP/1.1\r\n\r\n", &r) == 400);
    CHECK(parse("GET a HTTP/1.1\r\n\r\n", &r) == 400);                      /* target must start with '/' */
    CHECK(parse("GET /a#frag HTTP/1.1\r\n\r\n", &r) == 400);
    CHECK(parse("GET /a b HTTP/1.1\r\n\r\n", &r) == 400);                   /* two spaces */
    CHECK(parse("GET /a  HTTP/1.1\r\n\r\n", &r) == 400);
    CHECK(parse("GET /a HTTP/2.0\r\n\r\n", &r) == 505);
    CHECK(parse("GET /a FTP/1.1\r\n\r\n", &r) == 400);
    CHECK(parse("GET /a\r\n\r\n", &r) == 400);
    CHECK(parse("\r\n\r\n", &r) == 400);
    { /* path 128 ok, 129 too long; query 512 ok, 513 too long */
        char t[900], p[200], q[600];
        memset(p, 'a', 127);
        p[127] = '\0';
        snprintf(t, sizeof t, "GET /%s HTTP/1.1\r\n\r\n", p);
        CHECK(parse(t, &r) == 0 && strlen(r.path) == 128);
        snprintf(t, sizeof t, "GET /%sa HTTP/1.1\r\n\r\n", p);
        CHECK(parse(t, &r) == 414);
        memset(q, 'b', 512);
        q[512] = '\0';
        snprintf(t, sizeof t, "GET /a?%s HTTP/1.1\r\n\r\n", q);
        CHECK(parse(t, &r) == 0 && strlen(r.query) == 512);
        snprintf(t, sizeof t, "GET /a?%sb HTTP/1.1\r\n\r\n", q);
        CHECK(parse(t, &r) == 414);
    }
    { /* 40 headers ok, 41 too many */
        char t[1500];
        int i;
        size_t n = (size_t)snprintf(t, sizeof t, "GET /a HTTP/1.1\r\n");
        for (i = 0; i < 40; i++)
            n += (size_t)snprintf(t + n, sizeof t - n, "H%d: v\r\n", i);
        snprintf(t + n, sizeof t - n, "\r\n");
        CHECK(parse(t, &r) == 0);
        n -= 2;
        n += (size_t)snprintf(t + n, sizeof t - n, "\r\nH41: v\r\n\r\n");
        CHECK(parse(t, &r) == 431);
    }
    {
        char t[400];
        size_t n = (size_t)snprintf(t, sizeof t, "GET /a HTTP/1.1\r\nX-Akam-Nonce: ");
        memset(t + n, '1', HREQ_VALUE_MAX);
        snprintf(t + n + HREQ_VALUE_MAX, sizeof t - n - HREQ_VALUE_MAX, "\r\n\r\n");
        CHECK(parse(t, &r) == 0);
        memset(t + n, '1', HREQ_VALUE_MAX + 1);
        snprintf(t + n + HREQ_VALUE_MAX + 1, sizeof t - n - HREQ_VALUE_MAX - 1, "\r\n\r\n");
        CHECK(parse(t, &r) == 431);
    }
    /* the head end search */
    memset(big, 'a', sizeof big);
    CHECK(hreq_head_end(big, HREQ_HEAD_MAX) == -1);
    CHECK(hreq_head_end("GET / HTTP/1.1\r\n", 16) == 0);
    CHECK(hreq_head_end("GET / HTTP/1.1\r\n\r\n", 18) == 18);
    CHECK(hreq_head_end("GET / HTTP/1.1\r\n\r\nBODY", 22) == 18);
    CHECK(hreq_head_end("\n", 1) == -2);
    /* the parser never reads past head_len even when the buffer holds more */
    {
        char b[64] = "GET /a HTTP/1.1\r\n\r\nPOST /b HTTP/1.1\r\n\r\n";
        CHECK(hreq_parse(b, 19, &r) == 0 && !strcmp(r.path, "/a"));
    }
}

static void test_limits(void)
{
    alim_t l;
    uint64_t t = 1000000;
    unsigned i, r;
    alim_init(&l, APISIG_RATE_MILLI_DEFAULT, APISIG_BURST_DEFAULT, t);
    /* the spec 5.5 invariant holds for the defaults */
    CHECK(apisig_cache_invariant_ok(APISIG_NONCE_CAP, APISIG_RATE_MILLI_DEFAULT, 125, APISIG_BURST_DEFAULT));
    /* token bucket: the burst, then 0.4/s */
    for (i = 0; i < APISIG_BURST_DEFAULT; i++)
        CHECK(alim_accept_take(&l, t));
    CHECK(!alim_accept_peek(&l, t) && !alim_accept_take(&l, t));
    CHECK(alim_accept_retry_s(&l) >= 2 && alim_accept_retry_s(&l) <= 3);
    t += 2400;
    CHECK(!alim_accept_peek(&l, t));             /* 0.96 token */
    t += 100;
    CHECK(alim_accept_peek(&l, t) && alim_accept_take(&l, t) && !alim_accept_take(&l, t));
    t += 3600000;                                 /* an hour later: capped at the burst, not more */
    for (i = 0; i < APISIG_BURST_DEFAULT; i++)
        CHECK(alim_accept_take(&l, t));
    CHECK(!alim_accept_take(&l, t));
    /* sustained rate never exceeds the cache capacity over the retention window */
    {
        unsigned accepted = 0;
        uint64_t u = t;
        alim_init(&l, APISIG_RATE_MILLI_DEFAULT, APISIG_BURST_DEFAULT, u);
        for (i = 0; i < 125000 / 50; i++) { /* 125 s, a request every 50 ms */
            u += 50;
            accepted += (unsigned)alim_accept_take(&l, u);
        }
        CHECK(accepted <= APISIG_NONCE_CAP);
    }
    /* per-peer lockout: 4 free failures, then 1 s, 2 s, 4 s ... capped at 5 min */
    alim_init(&l, APISIG_RATE_MILLI_DEFAULT, APISIG_BURST_DEFAULT, t);
    for (i = 0; i < 4; i++) {
        CHECK(alim_peer_check(&l, 0x0a000001, t) == 0);
        alim_peer_fail(&l, 0x0a000001, t);
    }
    CHECK(alim_peer_check(&l, 0x0a000001, t) == 0);
    alim_peer_fail(&l, 0x0a000001, t);               /* the 5th */
    r = alim_peer_check(&l, 0x0a000001, t);
    CHECK(r == 1);
    CHECK(alim_peer_check(&l, 0x0a000001, t + 999) == 1);
    CHECK(alim_peer_check(&l, 0x0a000001, t + 1001) == 0);
    CHECK(alim_peer_check(&l, 0x0a000002, t) == 0);   /* another peer is unaffected */
    t += 1001;
    alim_peer_fail(&l, 0x0a000001, t);                /* the 6th: 2 s */
    CHECK(alim_peer_check(&l, 0x0a000001, t) == 2);
    t += 2001;
    for (i = 0; i < 30; i++) {
        alim_peer_fail(&l, 0x0a000001, t);
        t += 1;
    }
    CHECK(alim_peer_check(&l, 0x0a000001, t) == 300);  /* capped */
    alim_peer_ok(&l, 0x0a000001, t);
    CHECK(alim_peer_check(&l, 0x0a000001, t) == 0);    /* one success clears everything */
    alim_peer_fail(&l, 0x0a000003, t);
    alim_peer_fail(&l, 0x0a000003, t);
    alim_peer_fail(&l, 0x0a000003, t);
    alim_peer_fail(&l, 0x0a000003, t);
    alim_peer_fail(&l, 0x0a000003, t);
    CHECK(alim_peer_check(&l, 0x0a000003, t) == 1);
    CHECK(alim_peer_check(&l, 0x0a000003, t + APILIM_QUIET_MS + 1) == 0); /* ten quiet minutes forget the failures */
    /* the global cap of 20 failed authentications per minute; a recent good peer is exempt */
    alim_init(&l, APISIG_RATE_MILLI_DEFAULT, APISIG_BURST_DEFAULT, t);
    alim_peer_ok(&l, 0x0a0000aa, t);
    for (i = 0; i < APILIM_FAIL_PER_MIN; i++)
        alim_peer_fail(&l, 0x0b000000 + i, t); /* 20 different attackers, one failure each */
    CHECK(alim_peer_check(&l, 0x0c000001, t) >= 1);  /* a fresh peer is refused during the flood (fail closed) */
    CHECK(alim_peer_check(&l, 0x0a0000aa, t) == 0);  /* the owner (a good peer) is not */
    CHECK(alim_peer_check(&l, 0x0c000001, t + 60001) == 0); /* the window rolls */
    /* the peer table is bounded: 16 entries, a locked attacker is not evicted by new addresses */
    alim_init(&l, APISIG_RATE_MILLI_DEFAULT, APISIG_BURST_DEFAULT, t);
    for (i = 0; i < 5; i++)
        alim_peer_fail(&l, 0x0d000001, t);
    for (i = 0; i < 100; i++)
        alim_peer_fail(&l, 0x0e000000 + i, t + i);
    CHECK(alim_peer_check(&l, 0x0d000001, t + 100) >= 1);
    /* /hello: 2 per second per peer, 10 per minute globally */
    alim_init(&l, APISIG_RATE_MILLI_DEFAULT, APISIG_BURST_DEFAULT, t);
    t = (t / 1000) * 1000;
    CHECK(alim_hello(&l, 1, t) == 0 && alim_hello(&l, 1, t + 10) == 0 && alim_hello(&l, 1, t + 20) != 0);
    CHECK(alim_hello(&l, 2, t + 30) == 0);
    t += 1000;
    CHECK(alim_hello(&l, 1, t) == 0);
    for (i = 0; i < 20; i++)
        (void)alim_hello(&l, 100 + i, t + 2000 + i * 1000u / 10u);
    CHECK(alim_hello(&l, 999, t + 5000) >= 1);          /* global cap reached */
    CHECK(alim_hello(&l, 999, t + 70000) == 0);
}

static void test_events(void)
{
    char out[2048];
    int count = -1, n;
    a2k_apievt_reset();
    CHECK(a2k_apievt_seq() == 0);
    n = a2k_apievt_json(0, out, sizeof out, &count);
    CHECK(n > 0 && count == 0 && strstr(out, "\"events\":[]") && strstr(out, "\"seq\":0"));
    a2k_api_event_button();
    a2k_api_event_person(1, 800);
    a2k_api_event_person(1, 810); /* no change: no event */
    a2k_api_event_person(0, 0);
    CHECK(a2k_apievt_seq() == 3);
    n = a2k_apievt_json(0, out, sizeof out, &count);
    CHECK(n > 0 && count == 3 && strstr(out, "\"type\":\"button\"") && strstr(out, "\"present\":true") && strstr(out, "\"present\":false"));
    n = a2k_apievt_json(2, out, sizeof out, &count);
    CHECK(count == 1 && strstr(out, "\"seq\":3") && !strstr(out, "\"seq\":2,"));
    n = a2k_apievt_json(3, out, sizeof out, &count);
    CHECK(count == 0 && strstr(out, "\"missed\":false"));
    n = a2k_apievt_json(99, out, sizeof out, &count); /* a cursor from the future (a restart): clamped, no events */
    CHECK(count == 0 && strstr(out, "\"seq\":3"));
    /* the ring: older events are lost and said so */
    a2k_apievt_reset();
    for (int i = 0; i < A2K_EVT_RING + 8; i++)
        a2k_api_event("setting", "{\"n\":1}");
    n = a2k_apievt_json(0, out, sizeof out, &count);
    CHECK(n < 0 || count == A2K_EVT_RING); /* may not fit the small buffer */
    {
        static char big[8192];
        n = a2k_apievt_json(0, big, sizeof big, &count);
        CHECK(n > 0 && count == A2K_EVT_RING && strstr(big, "\"missed\":true"));
    }
    CHECK(a2k_apievt_json(0, out, 8, &count) == -1);     /* too small: an error, never an overrun */
    /* bad types and over-long data are dropped */
    a2k_apievt_reset();
    a2k_api_event("Bad Type", NULL);
    a2k_api_event("", NULL);
    {
        char longdata[A2K_EVT_DATA + 20];
        memset(longdata, 'x', sizeof longdata - 1);
        longdata[sizeof longdata - 1] = '\0';
        memcpy(longdata, "{\"x\":\"", 6);
        longdata[sizeof longdata - 3] = '"';
        longdata[sizeof longdata - 2] = '}';
        a2k_api_event("ok", longdata); /* over the data limit: dropped */
    }
    CHECK(a2k_apievt_seq() == 0);
    a2k_api_event("ok", NULL);
    CHECK(a2k_apievt_seq() == 1);
    n = a2k_apievt_json(0, out, sizeof out, &count);
    CHECK(strstr(out, "\"data\":{}"));
}

static void test_det_ring(void)
{
    static char big[12288], det[A2K_EVT_DDATA + 1];
    int count = -1, n, i;
    char *q;
    a2k_apievt_reset();
    memset(det, 'x', sizeof det);
    det[0] = '"';
    det[A2K_EVT_DDATA - 1] = '"';
    det[A2K_EVT_DDATA] = '\0';
    a2k_api_event_button();
    a2k_api_event("ptz", "{\"state\":\"moved\"}");
    a2k_api_event("setting", "{\"key\":\"led.mode\"}");
    for (i = 0; i < 50; i++) /* a long burst of detections of the largest size */
        a2k_api_event(A2K_EVT_DETECTIONS, det);
    CHECK(a2k_apievt_seq() == 53);
    n = a2k_apievt_json(0, big, sizeof big, &count);
    CHECK(n > 0 && n < (int)sizeof big);
    CHECK(strstr(big, "\"type\":\"button\"") && strstr(big, "\"type\":\"ptz\"") && strstr(big, "\"type\":\"setting\""));
    for (i = 0, q = big; (q = strstr(q, "\"type\":\"detections\"")) != NULL; q++)
        i++;
    CHECK(i == A2K_EVT_DRING && count == 3 + A2K_EVT_DRING);
    CHECK(strstr(big, "\"missed\":true")); /* the older detections were overwritten, and that is said */
    /* the oldest event first, whatever the ring: the sequence numbers are increasing */
    {
        long last = 0;
        int ok = 1;
        for (q = big; (q = strstr(q, "{\"seq\":")) != NULL; q++) {
            long v = strtol(q + 7, NULL, 10);
            if (q == big)
                continue; /* the envelope */
            ok &= v > last;
            last = v;
        }
        CHECK(ok && last == 53);
    }
    /* a cursor inside the detections: only the ones after it, nothing missed */
    n = a2k_apievt_json(51, big, sizeof big, &count);
    CHECK(count == 2 && strstr(big, "\"missed\":false"));
    /* too long for the detections ring: dropped; a detections event does not fit the other ring either */
    a2k_apievt_reset();
    memset(det, 'x', sizeof det - 1);
    a2k_api_event(A2K_EVT_DETECTIONS, det); /* 800 x's: the limit, accepted */
    CHECK(a2k_apievt_seq() == 1);
    det[A2K_EVT_DDATA] = 'x';
    det[A2K_EVT_DDATA + 1 - 1] = 'x';
    {
        static char longer[A2K_EVT_DDATA + 8];
        memset(longer, 'x', sizeof longer - 1);
        longer[sizeof longer - 1] = '\0';
        a2k_api_event(A2K_EVT_DETECTIONS, longer);
        CHECK(a2k_apievt_seq() == 1);
    }
}

static void test_patrol_ring(void)
{
    static char big[12288];
    int count = -1, i;
    char *q;
    long last = 0;
    int ok = 1;
    a2k_apievt_reset();
    a2k_api_event_button();
    a2k_api_event("ptz", "{\"state\":\"moved\"}");
    for (i = 0; i < 100; i++) /* a patrol that talks a lot cannot push the others out */
        a2k_api_event(A2K_EVT_PATROL, "{\"event\":\"step\"}");
    a2k_api_event_person(1, 800);
    CHECK(a2k_apievt_json(0, big, sizeof big, &count) > 0);
    CHECK(strstr(big, "\"type\":\"button\"") && strstr(big, "\"type\":\"ptz\"") && strstr(big, "\"type\":\"person\""));
    for (i = 0, q = big; (q = strstr(q, "\"type\":\"patrol\"")) != NULL; q++)
        i++;
    CHECK(i == A2K_EVT_PRING && count == 3 + A2K_EVT_PRING && strstr(big, "\"missed\":true"));
    for (q = big; (q = strstr(q, "{\"seq\":")) != NULL; q++) {
        long v = strtol(q + 7, NULL, 10);
        if (q == big)
            continue;
        ok &= v > last;
        last = v;
    }
    CHECK(ok && last == 103);
}

int main(void)
{
    test_patrol_ring();
    test_det_ring();
    test_parser();
    test_limits();
    test_events();
    printf("%d checks, %d failed\n", total, fails);
    return fails ? 1 : 0;
}
