/* Host unit test for src/ak_rtsp/rtsp_in.c: the RTSP/TCP input demultiplexer.
 * Regression for "VLC over TCP is dropped after ~13 minutes": its RTCP
 * receiver reports ('$' frames) piled up unconsumed in the read buffer until
 * it was full and recv(.., 0) was taken for a disconnect. Pure byte-stream
 * test, no sockets, no hardware. Build/run: tests/rtsp_in_test.sh */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rtsp_in.h"

#define CHECK(c)                                                              \
    do {                                                                      \
        if (!(c)) {                                                           \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);      \
            exit(1);                                                          \
        }                                                                     \
    } while (0)

/* ---- stream builder ----------------------------------------------------- */
typedef struct {
    uint8_t *p;
    size_t len, cap;
} bytes_t;

static void put(bytes_t *b, const void *d, size_t n)
{
    if (b->len + n > b->cap) {
        b->cap = (b->len + n) * 2;
        b->p = realloc(b->p, b->cap);
        CHECK(b->p);
    }
    memcpy(b->p + b->len, d, n);
    b->len += n;
}

static void put_str(bytes_t *b, const char *s) { put(b, s, strlen(s)); }

static void put_req(bytes_t *b, const char *method, unsigned cseq,
                    const char *extra, const char *body)
{
    char h[512];
    size_t bl = body ? strlen(body) : 0;
    int n = snprintf(h, sizeof(h), "%s rtsp://192.168.1.2/stream RTSP/1.0\r\n"
                     "CSeq: %u\r\n%s", method, cseq, extra ? extra : "");
    if (bl) n += snprintf(h + n, sizeof(h) - (size_t)n, "Content-Length: %zu\r\n", bl);
    n += snprintf(h + n, sizeof(h) - (size_t)n, "\r\n");
    put(b, h, (size_t)n);
    if (bl) put(b, body, bl);
}

/* '$' + channel + length + payload; rr: RTCP RR (+ SDES CNAME) like live555. */
static void put_frame(bytes_t *b, uint8_t ch, const uint8_t *pl, size_t n)
{
    uint8_t h[4] = { '$', ch, (uint8_t)(n >> 8), (uint8_t)n };
    put(b, h, 4);
    put(b, pl, n);
}

static void put_rtcp_rr(bytes_t *b, uint8_t ch, int with_bye)
{
    uint8_t rtcp[64];
    size_t n = 0;
    memset(rtcp, 0, sizeof(rtcp));
    /* RR, 1 report block: 8 + 24 bytes = 8 words, length field 7 */
    rtcp[0] = 0x81; rtcp[1] = 201; rtcp[3] = 7; n += 32;
    /* SDES, 1 chunk with CNAME "vlc": 4 hdr + 4 ssrc + 2 + 3 + pad = 16 */
    rtcp[n] = 0x81; rtcp[n + 1] = 202; rtcp[n + 3] = 3;
    rtcp[n + 8] = 1; rtcp[n + 9] = 3; memcpy(rtcp + n + 10, "vlc", 3);
    n += 16;
    if (with_bye) { rtcp[n] = 0x81; rtcp[n + 1] = 203; rtcp[n + 3] = 1; n += 8; }
    put_frame(b, ch, rtcp, n);
}

/* ---- sink: stands in for the RTSP controller ----------------------------- */
typedef struct {
    unsigned requests, answered, frames, rr, byes;
    unsigned next_cseq;     /* requests must arrive in order, none lost */
    int bad_order;
    unsigned bodies;
    size_t last_body;
    unsigned stop_at;       /* stop the pump at this request count, 0 = never */
} sink_t;

static int on_message(void *u, const char *d, size_t n)
{
    sink_t *s = u;
    unsigned long cseq = 0;
    CHECK(rtsp_in_header_uint(d, n, "CSeq", &cseq) == 1);
    if (cseq != s->next_cseq) s->bad_order = 1;
    s->next_cseq = (unsigned)cseq + 1;
    s->requests++;
    s->answered++;  /* the real handler always answers (200/4xx/5xx) */
    unsigned long cl = 0;
    if (rtsp_in_header_uint(d, n, "Content-Length", &cl) == 1) {
        s->bodies++;
        s->last_body = (size_t)cl;
        CHECK(n > cl);
    }
    return s->stop_at && s->requests == s->stop_at;
}

static int on_frame(void *u, uint8_t ch, const uint8_t *d, size_t n)
{
    sink_t *s = u;
    unsigned rr = 0;
    (void)ch;
    s->frames++;
    if (rtsp_rtcp_scan(d, n, &rr)) s->byes++;
    s->rr += rr;
    return 0;
}

static unsigned lcg_state = 12345;
static unsigned lcg(void)
{
    lcg_state = lcg_state * 1103515245u + 12345u;
    return (lcg_state >> 8) & 0xffffff;
}

/* Feeds the stream the way recv() would: chunks of chunk_max (or random
 * 1..chunk_max when random_chunks) bytes written into rtsp_in_space(). */
static rtsp_in_pump_t feed(rtsp_in_t *in, sink_t *s, const bytes_t *st,
                           size_t chunk_max, int random_chunks, size_t *max_len)
{
    rtsp_in_ops_t ops = { s, on_message, on_frame };
    size_t off = 0;
    while (off < st->len) {
        size_t cap;
        char *dst = rtsp_in_space(in, &cap);
        CHECK(cap > 0); /* recv() is never called with length 0 */
        size_t n = random_chunks ? 1 + lcg() % chunk_max : chunk_max;
        if (n > cap) n = cap;
        if (n > st->len - off) n = st->len - off;
        memcpy(dst, st->p + off, n);
        off += n;
        rtsp_in_commit(in, n);
        CHECK(in->len <= RTSP_IN_CAP);
        rtsp_in_pump_t r = rtsp_in_pump(in, &ops);
        if (in->len > *max_len) *max_len = in->len;
        if (r != RTSP_IN_PUMP_OK) return r;
    }
    return RTSP_IN_PUMP_OK;
}

/* Typical live555/VLC TCP session: handshake, then every 5 s an RR on each
 * track's RTCP channel, a GET_PARAMETER keepalive every 30 s, for 30 min. */
static void test_vlc_30_minutes(size_t chunk_max, int random_chunks)
{
    bytes_t st = { 0 };
    unsigned cseq = 1, expect_req = 0, expect_rr = 0, expect_ka = 0;

    put_req(&st, "OPTIONS", cseq++, NULL, NULL); expect_req++;
    put_req(&st, "DESCRIBE", cseq++, "Accept: application/sdp\r\n", NULL); expect_req++;
    put_req(&st, "SETUP", cseq++, "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n", NULL); expect_req++;
    put_req(&st, "SETUP", cseq++, "Transport: RTP/AVP/TCP;unicast;interleaved=2-3\r\nSession: 4242\r\n", NULL); expect_req++;
    put_req(&st, "PLAY", cseq++, "Session: 4242\r\nRange: npt=0.000-\r\n", NULL); expect_req++;
    for (int t = 5; t <= 30 * 60; t += 5) {
        put_rtcp_rr(&st, 1, 0); expect_rr++;
        put_rtcp_rr(&st, 3, 0); expect_rr++;
        if (t % 30 == 0) {
            put_req(&st, "GET_PARAMETER", cseq++, "Session: 4242\r\n", NULL);
            expect_req++; expect_ka++;
        }
        if (t % 150 == 0) {
            put_req(&st, "OPTIONS", cseq++, "Session: 4242\r\n", NULL); expect_req++;
        }
    }
    /* A CRLF after a frame, and a SET_PARAMETER with a body, then TEARDOWN. */
    put(&st, "\r\n", 2);
    put_req(&st, "SET_PARAMETER", cseq++, "Session: 4242\r\nContent-Type: text/parameters\r\n", "a: b\r\n"); expect_req++;
    put_req(&st, "TEARDOWN", cseq++, "Session: 4242\r\n", NULL); expect_req++;
    (void)expect_ka;

    rtsp_in_t *in = malloc(sizeof(*in));
    CHECK(in);
    rtsp_in_init(in);
    sink_t s = { .next_cseq = 1 };
    size_t max_len = 0;
    CHECK(feed(in, &s, &st, chunk_max, random_chunks, &max_len) == RTSP_IN_PUMP_OK);
    CHECK(s.requests == expect_req);
    CHECK(s.answered == s.requests);
    CHECK(!s.bad_order);
    CHECK(s.frames == expect_rr);
    CHECK(s.rr == expect_rr);
    CHECK(s.byes == 0);
    CHECK(s.bodies == 1 && s.last_body == 6);
    CHECK(in->len == 0);
    CHECK(max_len < RTSP_IN_CAP);
    printf("  chunk<=%zu%s: %u requests answered, %u RR frames, %zu bytes, "
           "peak buffer %zu/%d\n", chunk_max, random_chunks ? " random" : "",
           s.requests, s.frames, st.len, max_len, RTSP_IN_CAP);
    free(in);
    free(st.p);
}

static void test_bye_and_stop(void)
{
    bytes_t st = { 0 };
    put_req(&st, "PLAY", 1, "Session: 1\r\n", NULL);
    put_rtcp_rr(&st, 1, 1);
    put_req(&st, "OPTIONS", 2, NULL, NULL);
    rtsp_in_t in;
    rtsp_in_init(&in);
    sink_t s = { .next_cseq = 1 };
    size_t m = 0;
    CHECK(feed(&in, &s, &st, 4096, 0, &m) == RTSP_IN_PUMP_OK);
    CHECK(s.byes == 1 && s.requests == 2);

    rtsp_in_init(&in);
    sink_t s2 = { .next_cseq = 1, .stop_at = 1 };
    CHECK(feed(&in, &s2, &st, 4096, 0, &m) == RTSP_IN_PUMP_STOPPED);
    CHECK(s2.requests == 1);
    free(st.p);
}

static void test_overflow_and_garbage(void)
{
    rtsp_in_t in;
    sink_t s = { .next_cseq = 1 };
    size_t m = 0;
    bytes_t st = { 0 };

    /* Header block that never ends: clean OVERFLOW, not a silent wedge. */
    put_str(&st, "OPTIONS * RTSP/1.0\r\nCSeq: 1\r\n");
    for (int i = 0; i < 600; i++) put(&st, "X-Pad: aaaaaaaa\r\n", 17);
    rtsp_in_init(&in);
    CHECK(feed(&in, &s, &st, 100, 0, &m) == RTSP_IN_PUMP_OVERFLOW);
    size_t dl;
    const char *d = rtsp_in_data(&in, &dl);
    unsigned long cs = 0;
    CHECK(rtsp_in_header_uint(d, dl, "cseq", &cs) == 1 && cs == 1); /* 400/413 can carry it */
    free(st.p);

    /* Content-Length larger than the buffer. */
    memset(&st, 0, sizeof(st));
    put_str(&st, "SET_PARAMETER x RTSP/1.0\r\nCSeq: 2\r\nContent-Length: 100000\r\n\r\n");
    rtsp_in_init(&in);
    CHECK(feed(&in, &s, &st, 50, 0, &m) == RTSP_IN_PUMP_OVERFLOW);
    free(st.p);

    /* Invalid Content-Length. */
    memset(&st, 0, sizeof(st));
    put_str(&st, "SET_PARAMETER x RTSP/1.0\r\nCSeq: 3\r\nContent-Length: abc\r\n\r\n");
    rtsp_in_init(&in);
    CHECK(feed(&in, &s, &st, 4096, 0, &m) == RTSP_IN_PUMP_GARBAGE);
    free(st.p);

    /* Binary garbage that is neither '$' nor text. */
    memset(&st, 0, sizeof(st));
    put(&st, "\x00\x01\x02\x03", 4);
    rtsp_in_init(&in);
    CHECK(feed(&in, &s, &st, 4096, 0, &m) == RTSP_IN_PUMP_GARBAGE);
    free(st.p);
}

/* An oversized '$' frame (never sent by a sane client) is discarded as it
 * streams in; the request behind it is still served and nothing overflows. */
static void test_oversized_frame_skipped(void)
{
    bytes_t st = { 0 };
    static uint8_t big[60000];
    memset(big, 0xAB, sizeof(big));
    put_frame(&st, 1, big, sizeof(big));
    put_req(&st, "GET_PARAMETER", 1, "Session: 7\r\n", NULL);
    put_rtcp_rr(&st, 1, 0);
    put_req(&st, "OPTIONS", 2, NULL, NULL);

    for (size_t chunk = 1; chunk <= 4096; chunk = chunk < 64 ? chunk + 31 : chunk * 4) {
        rtsp_in_t in;
        rtsp_in_init(&in);
        sink_t s = { .next_cseq = 1 };
        size_t m = 0;
        CHECK(feed(&in, &s, &st, chunk, 0, &m) == RTSP_IN_PUMP_OK);
        CHECK(s.requests == 2 && s.frames == 1 && !s.bad_order);
        CHECK(in.skipped_frames == 1);
    }
    free(st.p);
}

static void test_header_uint(void)
{
    const char *m = "PLAY x RTSP/1.0\r\ncseq:  17 \r\nContent-Length: 0\r\n\r\n";
    unsigned long v = 0;
    CHECK(rtsp_in_header_uint(m, strlen(m), "CSeq", &v) == 1 && v == 17);
    CHECK(rtsp_in_header_uint(m, strlen(m), "Session", &v) == 0);
    const char *bad = "PLAY x RTSP/1.0\r\nCSeq: 1x\r\n\r\n";
    CHECK(rtsp_in_header_uint(bad, strlen(bad), "CSeq", &v) == -1);
    /* A name only appearing in the body must not match. */
    const char *body = "PLAY x RTSP/1.0\r\nCSeq: 1\r\n\r\nSession: 9\r\n";
    CHECK(rtsp_in_header_uint(body, strlen(body), "Session", &v) == 0);
}

static void test_rtcp_scan(void)
{
    bytes_t st = { 0 };
    put_rtcp_rr(&st, 0, 0);
    unsigned rr = 0;
    CHECK(rtsp_rtcp_scan(st.p + 4, st.len - 4, &rr) == 0 && rr == 1);
    free(st.p);
    memset(&st, 0, sizeof(st));
    put_rtcp_rr(&st, 0, 1);
    CHECK(rtsp_rtcp_scan(st.p + 4, st.len - 4, &rr) == 1 && rr == 1);
    CHECK(rtsp_rtcp_scan(st.p + 4, 10, &rr) == 0); /* truncated: harmless */
    free(st.p);
}

int main(void)
{
    printf("rtsp_in: 30 min of VLC-like RTCP RR + keepalives\n");
    test_vlc_30_minutes(4096, 0);  /* one big read */
    test_vlc_30_minutes(1, 0);     /* every item split at every byte */
    test_vlc_30_minutes(7, 1);     /* frames split across reads */
    test_vlc_30_minutes(300, 1);
    test_bye_and_stop();
    test_overflow_and_garbage();
    test_oversized_frame_skipped();
    test_header_uint();
    test_rtcp_scan();
    printf("rtsp_in_test: OK\n");
    return 0;
}
