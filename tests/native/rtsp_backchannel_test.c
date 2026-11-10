/* End-to-end host test of the ONVIF audio backchannel through the REAL rtsp.c
 * client loop (SmolRTSP, Digest authentication, rtsp_in demultiplexer, talk.c,
 * rtp_in.c, audio_vad.c) over loopback TCP/UDP. Only the hardware is faked: the
 * video encoder (never has a frame), the microphone (a steady tone, one 256
 * sample period every 32 ms), the speaker (records what is pushed). The test is
 * the client: it speaks RTSP like go2rtc does.
 * Build/run: tests/rtsp_backchannel_test.sh */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "ak_rtsp.h"
#include "audio.h"
#include "g711.h"
#include "md5.h"
#include "rtsp.h"
#include "rtsp_auth.h"

static int fails;
#define CHECK(cond, msg) do { if (cond) printf("  ok   - %s\n", msg); \
    else { printf("  FAIL - %s\n", msg); fails++; } } while (0)

/* ======================================================================
 * Fake hardware
 * ====================================================================== */
int      g_chn0_fd = -1;
void    *g_venc_virt = NULL;
uint32_t g_venc_phys = 0;

void ledsys_stream_delta(int delta) { (void)delta; }

/* The encoder never has a frame: every ioctl on it answers EAGAIN. */
int __wrap_ioctl(int fd, unsigned long req, ...)
{
    (void)fd; (void)req;
    errno = EAGAIN;
    return -1;
}

static int64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

/* Microphone: a 1 kHz tone at -10 dBFS, one period per 32 ms of real time. */
static uint64_t mic_total;
static int64_t  mic_next_us;
int audio_get_frame_ts(int16_t *buf, size_t max_samples, size_t *out_samples,
                       uint64_t *first_sample, int64_t *t_end_us)
{
    int64_t t = now_us();
    if (!mic_next_us) mic_next_us = t;
    if (t < mic_next_us || max_samples < AUDIO_PERIOD_SAMPLES) return 0;
    mic_next_us += 32000;
    static const int16_t k[8] = { 0, 7000, 10000, 7000, 0, -7000, -10000, -7000 };
    for (size_t i = 0; i < AUDIO_PERIOD_SAMPLES; i++) buf[i] = k[i & 7];
    *out_samples = AUDIO_PERIOD_SAMPLES;
    *first_sample = mic_total;
    mic_total += AUDIO_PERIOD_SAMPLES;
    *t_end_us = t;
    return 1;
}

/* Speaker: records what the talk module pushes. */
static volatile int sp_available = 1;
static volatile int sp_half = 1;
static volatile int sp_claimed;
static volatile unsigned long sp_pushed;      /* samples */
static volatile int sp_claims, sp_releases;
static volatile int sp_nonzero;               /* a pushed sample was not zero */
int    audio_out_talk_available(void) { return sp_available; }
int    audio_out_talk_half_duplex(void) { return sp_half; }
size_t audio_out_queued(void) { return 0; }
size_t audio_out_push(const int16_t *pcm, size_t n)
{
    for (size_t i = 0; i < n; i++) if (pcm[i]) sp_nonzero = 1;
    sp_pushed += n;
    return n;
}
int  audio_out_feeder_claim(void) { if (sp_claimed) return -1; sp_claimed = 1; sp_claims++; return 0; }
void audio_out_feeder_release(void) { sp_claimed = 0; sp_releases++; }

/* ======================================================================
 * Server thread: the real handle_client(), one connection at a time
 * ====================================================================== */
static int srv_fd;
static int srv_port;

static void *server_main(void *arg)
{
    (void)arg;
    for (;;) {
        struct sockaddr_storage a;
        socklen_t al = sizeof(a);
        int fd = accept(srv_fd, (struct sockaddr *)&a, &al);
        if (fd < 0) { if (errno == EINTR) continue; return NULL; }
        handle_client(fd, &a, al);
    }
}

/* ======================================================================
 * Client helpers
 * ====================================================================== */
typedef struct {
    int fd;
    int cseq;
    char nonce[64];
    char session[64];
    char buf[16384];
    size_t have;
} conn_t;

static const char *USER = "alice", *PASS = "s3cret";
static char HA1[33];
static const char *AUTH_FILE = RTSP_AUTH_PATH;

static int tcp_connect(conn_t *c)
{
    memset(c, 0, sizeof(*c));
    c->fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)srv_port),
                             .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    if (connect(c->fd, (struct sockaddr *)&a, sizeof(a)) != 0) return -1;
    struct timeval tv = { 3, 0 };
    setsockopt(c->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    return 0;
}

static void conn_close(conn_t *c) { if (c->fd >= 0) close(c->fd); c->fd = -1; }

static void send_all(int fd, const void *p, size_t n)
{
    const char *s = p;
    while (n) {
        ssize_t r = send(fd, s, n, MSG_NOSIGNAL);
        if (r <= 0) return;
        s += r; n -= (size_t)r;
    }
}

/* Reads more bytes into the connection buffer; 0 on timeout/close. */
static int fill(conn_t *c)
{
    if (c->have >= sizeof(c->buf)) return 0;
    ssize_t r = recv(c->fd, c->buf + c->have, sizeof(c->buf) - c->have, 0);
    if (r <= 0) return 0;
    c->have += (size_t)r;
    return 1;
}

static void consume(conn_t *c, size_t n)
{
    memmove(c->buf, c->buf + n, c->have - n);
    c->have -= n;
}

/* One RTSP response (skipping interleaved frames that may precede it).
 * Fills out (header block + body, NUL terminated); returns the status code or -1. */
static int read_response(conn_t *c, char *out, size_t cap)
{
    for (;;) {
        while (c->have && c->buf[0] == '$') {
            if (c->have < 4) break;
            size_t len = ((size_t)(uint8_t)c->buf[2] << 8) | (uint8_t)c->buf[3];
            if (c->have < 4 + len) break;
            consume(c, 4 + len);
        }
        char *he = c->have >= 4 ? memmem(c->buf, c->have, "\r\n\r\n", 4) : NULL;
        if (he && c->buf[0] != '$') {
            size_t hl = (size_t)(he - c->buf) + 4, bl = 0;
            const char *cl = strcasestr(c->buf, "Content-Length:");
            if (cl && cl < c->buf + hl) bl = (size_t)atoi(cl + 15);
            if (c->have >= hl + bl) {
                size_t total = hl + bl;
                if (total >= cap) total = cap - 1;
                memcpy(out, c->buf, total);
                out[total] = '\0';
                int code = -1;
                sscanf(out, "RTSP/1.0 %d", &code);
                consume(c, hl + bl);
                return code;
            }
        }
        if (!fill(c)) return -1;
    }
}

static const char *hdr_value(const char *resp, const char *name, char *out, size_t cap)
{
    char key[64];
    snprintf(key, sizeof(key), "\r\n%s:", name);
    const char *p = strcasestr(resp, key);
    if (!p) return NULL;
    p += strlen(key);
    while (*p == ' ') p++;
    size_t n = strcspn(p, "\r\n");
    if (n >= cap) n = cap - 1;
    memcpy(out, p, n);
    out[n] = '\0';
    return out;
}

static void parse_nonce(conn_t *c, const char *resp)
{
    const char *p = strstr(resp, "nonce=\"");
    if (!p) return;
    p += 7;
    size_t n = strcspn(p, "\"");
    if (n >= sizeof(c->nonce)) n = sizeof(c->nonce) - 1;
    memcpy(c->nonce, p, n);
    c->nonce[n] = '\0';
}

/* Sends a request. extra: header lines, each ending in CRLF. auth: add an
 * Authorization header with the known nonce (empty nonce: none yet). */
static int rtsp(conn_t *c, const char *method, const char *path, const char *extra, int auth,
                char *resp, size_t cap)
{
    char uri[128], req[2048], ah[512] = "";
    snprintf(uri, sizeof(uri), "rtsp://127.0.0.1:%d/%s", srv_port, path);
    if (auth && c->nonce[0]) {
        char r[33];
        rtsp_auth_response(r, HA1, c->nonce, NULL, NULL, method, uri);
        snprintf(ah, sizeof(ah), "Authorization: Digest username=\"%s\", realm=\"akam\", nonce=\"%s\", "
                 "uri=\"%s\", response=\"%s\"\r\n", USER, c->nonce, uri, r);
    }
    int n = snprintf(req, sizeof(req), "%s %s RTSP/1.0\r\nCSeq: %d\r\n", method, uri, ++c->cseq);
    if (c->session[0]) n += snprintf(req + n, sizeof(req) - (size_t)n, "Session: %s\r\n", c->session);
    n += snprintf(req + n, sizeof(req) - (size_t)n, "%s%s\r\n", ah, extra ? extra : "");
    send_all(c->fd, req, (size_t)n);
    int code = read_response(c, resp, cap);
    if (code == 401 && auth) parse_nonce(c, resp);
    if (code == 200) {
        char s[64];
        if (hdr_value(resp, "Session", s, sizeof(s))) {
            s[strcspn(s, ";")] = '\0';
            snprintf(c->session, sizeof(c->session), "%s", s);
        }
    }
    return code;
}

/* Request that goes through the 401 handshake when credentials are on. */
static int rtsp_login(conn_t *c, const char *method, const char *path, const char *extra, char *resp, size_t cap)
{
    int code = rtsp(c, method, path, extra, 1, resp, cap);
    if (code == 401) code = rtsp(c, method, path, extra, 1, resp, cap);
    return code;
}

#define REQ_BC_LIT "Require: www.onvif.org/ver20/backchannel\r\n"
static const char *REQ_BC = REQ_BC_LIT;

static void set_auth(int on)
{
    if (on) {
        FILE *f = fopen(AUTH_FILE, "w");
        fprintf(f, "%s:akam:%s\n", USER, HA1);
        fclose(f);
    } else {
        unlink(AUTH_FILE);
    }
}

/* RTP packet of 160 G.711 samples: a tone of the given amplitude (0 = silence). */
static size_t mk_rtp(uint8_t *p, int pt, uint16_t seq, int amp)
{
    memset(p, 0, 12);
    p[0] = 0x80; p[1] = (uint8_t)pt; p[2] = (uint8_t)(seq >> 8); p[3] = (uint8_t)seq; p[11] = 0x42;
    static const int k[8] = { 0, 707, 1000, 707, 0, -707, -1000, -707 };
    for (int i = 0; i < 160; i++) {
        int16_t s = (int16_t)(amp * k[i & 7] / 1000);
        p[12 + i] = pt == 8 ? g711_alaw_encode(s) : g711_ulaw_encode(s);
    }
    return 172;
}

static void send_frame(int fd, uint8_t ch, const uint8_t *pl, size_t n)
{
    uint8_t h[4] = { '$', ch, (uint8_t)(n >> 8), (uint8_t)n };
    send_all(fd, h, 4);
    send_all(fd, pl, n);
}

static void wait_ms(int ms) { usleep((useconds_t)ms * 1000); }

/* Waits until cond becomes true, at most ms. */
#define WAIT_FOR(cond, ms) do { for (int w_ = 0; w_ < (ms) && !(cond); w_ += 5) wait_ms(5); } while (0)

/* The server noticed the end of the connection: it closed its side. */
static int server_closed(conn_t *c, int ms)
{
    struct pollfd p = { c->fd, POLLIN, 0 };
    for (int w = 0; w < ms; w += 20) {
        if (poll(&p, 1, 20) > 0) {
            char b[256];
            ssize_t r = recv(c->fd, b, sizeof(b), MSG_DONTWAIT);
            if (r == 0) return 1;
            if (r < 0 && errno != EAGAIN) return 1;
        }
    }
    return 0;
}

/* ======================================================================
 * Scenarios
 * ====================================================================== */
static const char *SDP_DEFAULT_TAIL =
    "s=ak_rtsp\r\n"
    "t=0 0\r\n"
    "m=video 0 RTP/AVP 96\r\n"
    "c=IN IP4 0.0.0.0\r\n"
    "a=rtpmap:96 H264/90000\r\n"
    "a=fmtp:96 packetization-mode=1\r\n"
    "a=control:streamid=0\r\n"
    "m=audio 0 RTP/AVP 97\r\n"
    "c=IN IP4 0.0.0.0\r\n"
    "a=rtpmap:97 L16/8000/1\r\n"
    "a=control:streamid=1\r\n";

/* The SDP body of a response without its o= line (it carries the clock). */
static const char *sdp_after_origin(const char *resp)
{
    const char *p = strstr(resp, "\r\n\r\nv=0\r\n");
    if (!p) return NULL;
    p = strstr(p + 4, "o=-");
    if (!p) return NULL;
    p = strstr(p, "\r\n");
    return p ? p + 2 : NULL;
}

static void test_no_password(void)
{
    printf("# password off\n");
    set_auth(0);
    conn_t c; char r[4096];
    CHECK(tcp_connect(&c) == 0, "connect");

    CHECK(rtsp(&c, "DESCRIBE", "x", "Accept: application/sdp\r\n", 0, r, sizeof(r)) == 200, "DESCRIBE without Require: 200");
    const char *tail = sdp_after_origin(r);
    CHECK(tail && !strcmp(tail, SDP_DEFAULT_TAIL), "SDP is byte-identical to the one the server always sent");

    char v[128];
    CHECK(rtsp(&c, "DESCRIBE", "x", REQ_BC, 0, r, sizeof(r)) == 551, "DESCRIBE with Require and no password: 551");
    CHECK(hdr_value(r, "Unsupported", v, sizeof(v)) && !strcmp(v, "www.onvif.org/ver20/backchannel"), "Unsupported header names the tag");
    CHECK(!strstr(r, "m=audio"), "no media section in the refusal");

    CHECK(rtsp(&c, "DESCRIBE", "x", "Accept: application/sdp\r\n", 0, r, sizeof(r)) == 200, "the connection goes on: a retry without Require gets the plain SDP");
    tail = sdp_after_origin(r);
    CHECK(tail && !strcmp(tail, SDP_DEFAULT_TAIL), "retry: identical SDP");

    CHECK(rtsp(&c, "DESCRIBE", "x", "require:  foo.bar,WWW.ONVIF.ORG/ver20/backchannel\r\n", 0, r, sizeof(r)) == 551,
          "Require header in a list, lower-case name, upper-case tag: still 551");

    CHECK(rtsp(&c, "SETUP", "x/streamid=2", "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n", 0, r, sizeof(r)) == 404,
          "SETUP of the backchannel without password: 404 (never offered)");
    CHECK(rtsp(&c, "SETUP", "x/streamid=2", "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n" REQ_BC_LIT,
               0, r, sizeof(r)) == 551, "same with Require: 551");
    CHECK(rtsp(&c, "PLAY", "x", "", 0, r, sizeof(r)) == 455, "PLAY with nothing SETUP: 455, the server survives");
    CHECK(rtsp(&c, "OPTIONS", "x", "", 0, r, sizeof(r)) == 200, "still alive");
    conn_close(&c);
}

static void test_auth_gating(void)
{
    printf("# password on: gating\n");
    set_auth(1);
    sp_available = 1;
    conn_t c; char r[4096];
    CHECK(tcp_connect(&c) == 0, "connect");
    CHECK(rtsp(&c, "DESCRIBE", "x", REQ_BC, 1, r, sizeof(r)) == 401, "Require without credentials: 401 first");
    CHECK(!strstr(r, "m=audio") && strstr(r, "WWW-Authenticate: Digest"), "no media section before login, a Digest challenge");
    parse_nonce(&c, r);
    int code = rtsp(&c, "DESCRIBE", "x", REQ_BC, 1, r, sizeof(r));
    CHECK(code == 200, "with credentials: 200");
    const char *tail = sdp_after_origin(r);
    size_t dl = strlen(SDP_DEFAULT_TAIL);
    CHECK(tail && !strncmp(tail, SDP_DEFAULT_TAIL, dl), "the usual SDP comes first");
    CHECK(tail && !strcmp(tail + dl,
        "m=audio 0 RTP/AVP 0 8\r\n"
        "c=IN IP4 0.0.0.0\r\n"
        "a=control:streamid=2\r\n"
        "a=rtpmap:0 PCMU/8000\r\n"
        "a=rtpmap:8 PCMA/8000\r\n"
        "a=sendonly\r\n"), "followed by exactly one send-only PCMU/PCMA section");

    code = rtsp(&c, "DESCRIBE", "x", "Accept: application/sdp\r\n", 1, r, sizeof(r));
    tail = sdp_after_origin(r);
    CHECK(code == 200 && tail && !strcmp(tail, SDP_DEFAULT_TAIL), "same connection, no Require: the plain SDP (golden)");

    sp_available = 0;
    CHECK(rtsp(&c, "DESCRIBE", "x", REQ_BC, 1, r, sizeof(r)) == 551, "speaker unavailable or audio.talk_enable=0: 551");
    sp_available = 1;
    conn_close(&c);

    /* no login at all: no talk track, no PLAY, no audio, whatever the Require tag says */
    CHECK(tcp_connect(&c) == 0, "connect");
    unsigned long p0 = sp_pushed;
    CHECK(rtsp(&c, "SETUP", "x/streamid=2", REQ_BC_LIT "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n", 0, r, sizeof(r)) == 401,
          "SETUP of the backchannel without login: 401");
    CHECK(rtsp(&c, "SETUP", "x/streamid=2", "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n", 0, r, sizeof(r)) == 401,
          "same without the Require tag: 401");
    CHECK(rtsp(&c, "PLAY", "x", "", 0, r, sizeof(r)) == 401, "PLAY without login: 401");
    {
        uint8_t pkt0[172];
        send_frame(c.fd, 0, pkt0, mk_rtp(pkt0, 0, 1, 6000));
        wait_ms(100);
        CHECK(sp_pushed == p0 && !sp_claimed, "an interleaved RTP frame before the login queues nothing and claims nothing");
    }
    conn_close(&c);

    /* wrong password: never reaches the offer */
    CHECK(tcp_connect(&c) == 0, "connect");
    rtsp(&c, "DESCRIBE", "x", REQ_BC, 1, r, sizeof(r));
    parse_nonce(&c, r);
    char saved[33]; memcpy(saved, HA1, 33);
    md5_hex_cat(HA1, "alice:akam:wrong", (char *)NULL);
    code = rtsp(&c, "DESCRIBE", "x", REQ_BC, 1, r, sizeof(r));
    memcpy(HA1, saved, 33);
    CHECK(code == 401 && !strstr(r, "m=audio"), "wrong password: 401, no SDP");
    conn_close(&c);
}

static void test_talk_only_tcp(void)
{
    printf("# talk-only session, TCP interleaved\n");
    set_auth(1);
    sp_available = 1;
    conn_t c; char r[2048], v[128];
    unsigned long p0 = sp_pushed; int cl0 = sp_claims, re0 = sp_releases;
    CHECK(tcp_connect(&c) == 0, "connect");
    CHECK(rtsp_login(&c, "DESCRIBE", "x", REQ_BC, r, sizeof(r)) == 200, "DESCRIBE with Require");
    CHECK(rtsp(&c, "SETUP", "x/streamid=2", REQ_BC_LIT "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n", 1, r, sizeof(r)) == 200,
          "SETUP of the backchannel");
    CHECK(hdr_value(r, "Transport", v, sizeof(v)) && !strcmp(v, "RTP/AVP/TCP;unicast;interleaved=0-1"), "Transport answer: same interleaved channels");
    CHECK(hdr_value(r, "Session", v, sizeof(v)) && strstr(v, "timeout=60"), "Session with timeout=60");
    CHECK(rtsp(&c, "SETUP", "x/streamid=2", "Transport: RTP/AVP/TCP;unicast;interleaved=4-4\r\n", 1, r, sizeof(r)) == 400, "RTP and RTCP on the same channel: 400");

    CHECK(sp_claims == cl0, "nothing claimed before PLAY");
    CHECK(rtsp(&c, "PLAY", "x", REQ_BC, 1, r, sizeof(r)) == 200, "PLAY");
    CHECK(sp_claims == cl0 + 1 && sp_claimed, "the speaker is claimed at PLAY");

    uint8_t pkt[172];
    for (int i = 0; i < 50; i++) { send_frame(c.fd, 0, pkt, mk_rtp(pkt, i & 1 ? 8 : 0, (uint16_t)(100 + i), 6000)); }
    WAIT_FOR(sp_pushed - p0 >= 8000, 2000);
    CHECK(sp_pushed - p0 == 8000, "50 packets of 160 samples reach the speaker (1 s of audio)");
    CHECK(sp_nonzero, "and they are decoded audio");

    char s[64];
    send_frame(c.fd, 6, pkt, mk_rtp(pkt, 0, 1, 5000));      /* unknown channel */
    uint8_t rr[8] = { 0x80, 201, 0, 1, 0, 0, 0, 1 };           /* RTCP receiver report on the RTCP channel */
    send_frame(c.fd, 1, rr, sizeof(rr));
    wait_ms(100);
    CHECK(sp_pushed - p0 == 8000, "frames on an unknown channel and RTCP reports are not audio");
    CHECK(rtsp(&c, "GET_PARAMETER", "x", "", 1, r, sizeof(r)) == 200, "keep-alive still answered while talking");

    CHECK(hdr_value(r, "Session", s, sizeof(s)) != NULL, "keep-alive carries the session");
    uint8_t bye[8] = { 0x81, 203, 0, 1, 0, 0, 0, 1 };
    send_frame(c.fd, 1, bye, sizeof(bye));
    CHECK(server_closed(&c, 2000), "RTCP BYE on the backchannel ends a talk-only session");
    WAIT_FOR(sp_releases == re0 + 1, 1000);
    CHECK(sp_releases == re0 + 1 && !sp_claimed, "the speaker is released");
    conn_close(&c);

    /* TEARDOWN path */
    CHECK(tcp_connect(&c) == 0, "second connection (the slot is free again)");
    CHECK(rtsp_login(&c, "DESCRIBE", "x", REQ_BC, r, sizeof(r)) == 200, "DESCRIBE");
    CHECK(rtsp(&c, "SETUP", "x/streamid=2", "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n", 1, r, sizeof(r)) == 200, "SETUP without Require header is fine too");
    CHECK(rtsp(&c, "PLAY", "x", "", 1, r, sizeof(r)) == 200, "PLAY");
    send_frame(c.fd, 0, pkt, mk_rtp(pkt, 0, 1, 6000));
    CHECK(rtsp(&c, "TEARDOWN", "x", "", 1, r, sizeof(r)) == 200, "TEARDOWN: 200");
    CHECK(server_closed(&c, 2000), "connection closed after TEARDOWN");
    WAIT_FOR(!sp_claimed, 1000);
    CHECK(!sp_claimed, "speaker released");
    conn_close(&c);

    /* abrupt loss */
    CHECK(tcp_connect(&c) == 0, "third connection");
    rtsp_login(&c, "DESCRIBE", "x", REQ_BC, r, sizeof(r));
    rtsp(&c, "SETUP", "x/streamid=2", "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n", 1, r, sizeof(r));
    rtsp(&c, "PLAY", "x", "", 1, r, sizeof(r));
    CHECK(sp_claimed, "talking");
    conn_close(&c);
    WAIT_FOR(!sp_claimed, 2000);
    CHECK(!sp_claimed, "client vanished: speaker released");
}

static void test_garbage_frames(void)
{
    printf("# garbage on the backchannel\n");
    set_auth(1);
    conn_t c; char r[2048];
    tcp_connect(&c);
    rtsp_login(&c, "DESCRIBE", "x", REQ_BC, r, sizeof(r));
    rtsp(&c, "SETUP", "x/streamid=2", "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n", 1, r, sizeof(r));
    unsigned long p0 = sp_pushed;
    uint8_t junk[1400];
    for (size_t i = 0; i < sizeof(junk); i++) junk[i] = (uint8_t)(i * 7);
    send_frame(c.fd, 0, junk, 172);
    wait_ms(50);
    CHECK(sp_pushed == p0, "RTP frames before PLAY are dropped");
    rtsp(&c, "PLAY", "x", "", 1, r, sizeof(r));
    send_frame(c.fd, 0, junk, 172);            /* not RTP (version bits) */
    junk[0] = 0x80; junk[1] = 96;
    send_frame(c.fd, 0, junk, 172);            /* dynamic payload type */
    send_frame(c.fd, 0, junk, 3);              /* truncated */
    uint8_t big[2000] = { 0x80, 0 };
    send_frame(c.fd, 0, big, 2000);            /* oversized */
    wait_ms(100);
    CHECK(sp_pushed == p0, "non-RTP, wrong payload type, truncated and oversized frames queue nothing");
    uint8_t pkt[172];
    send_frame(c.fd, 0, pkt, mk_rtp(pkt, 0, 7, 6000));
    WAIT_FOR(sp_pushed > p0, 1000);
    CHECK(sp_pushed == p0 + 160, "valid audio right after is played");
    CHECK(rtsp(&c, "OPTIONS", "x", "", 1, r, sizeof(r)) == 200, "the connection survived all of it");
    conn_close(&c);
    WAIT_FOR(!sp_claimed, 1000);
}

/* Parses the interleaved RTP packets of one channel out of the connection;
 * returns how many it appended to seq/ts/nonzero. */
typedef struct { uint16_t seq[400]; uint32_t ts[400]; int nz[400]; int n; } mic_log_t;

static void collect_mic(conn_t *c, uint8_t ch, mic_log_t *log, int ms)
{
    int64_t end = now_us() + (int64_t)ms * 1000;
    while (now_us() < end) {
        struct pollfd p = { c->fd, POLLIN, 0 };
        if (c->have < 4 && poll(&p, 1, 20) > 0) { if (!fill(c)) return; }
        while (c->have >= 4 && c->buf[0] == '$') {
            size_t len = ((size_t)(uint8_t)c->buf[2] << 8) | (uint8_t)c->buf[3];
            if (c->have < 4 + len) { if (!fill(c)) return; continue; }
            if ((uint8_t)c->buf[1] == ch && len >= 12 + 2 && log->n < 400) {
                const uint8_t *q = (const uint8_t *)c->buf + 4;
                log->seq[log->n] = (uint16_t)((q[2] << 8) | q[3]);
                log->ts[log->n] = ((uint32_t)q[4] << 24) | ((uint32_t)q[5] << 16) | ((uint32_t)q[6] << 8) | q[7];
                int nz = 0;
                for (size_t i = 12; i < len; i++) if (q[i]) nz = 1;
                log->nz[log->n++] = nz;
            }
            consume(c, 4 + len);
        }
        if (c->have && c->buf[0] != '$') consume(c, c->have); /* stray text: not expected */
    }
}

static void test_mic_and_talk_together(void)
{
    printf("# microphone and backchannel in ONE connection (half duplex)\n");
    wait_ms(500); /* the hold of the previous scenarios' speech has run out */
    set_auth(1);
    sp_half = 1;
    conn_t c; char r[2048];
    tcp_connect(&c);
    CHECK(rtsp_login(&c, "DESCRIBE", "x", REQ_BC, r, sizeof(r)) == 200, "DESCRIBE with Require");
    CHECK(rtsp(&c, "SETUP", "x/streamid=1", "Transport: RTP/AVP/TCP;unicast;interleaved=2-3\r\n", 1, r, sizeof(r)) == 200, "SETUP microphone (audio-only session: no video track)");
    CHECK(rtsp(&c, "SETUP", "x/streamid=2", "Transport: RTP/AVP/TCP;unicast;interleaved=4-5\r\n", 1, r, sizeof(r)) == 200, "SETUP backchannel");
    CHECK(rtsp(&c, "PLAY", "x", "", 1, r, sizeof(r)) == 200, "PLAY");

    mic_log_t log; memset(&log, 0, sizeof(log));
    uint8_t pkt[172];
    uint16_t seq = 1000;

    /* 1 s of continuous G.711 silence, like go2rtc keeps the channel open */
    int64_t until = now_us() + 1000000;
    while (now_us() < until) { send_frame(c.fd, 4, pkt, mk_rtp(pkt, 0, seq++, 0)); collect_mic(&c, 2, &log, 20); }
    int allnz = log.n > 10;
    for (int i = 0; i < log.n; i++) if (!log.nz[i]) allnz = 0;
    CHECK(log.n > 10 && allnz, "1 s of silence frames on the backchannel: the microphone stays live");

    /* speech for about 0.6 s */
    int mark = log.n;
    until = now_us() + 600000;
    while (now_us() < until) { send_frame(c.fd, 4, pkt, mk_rtp(pkt, 0, seq++, 8000)); collect_mic(&c, 2, &log, 20); }
    /* then silence frames again; the hold is 300 ms */
    int after_speech = log.n;
    until = now_us() + 1200000;
    while (now_us() < until) { send_frame(c.fd, 4, pkt, mk_rtp(pkt, 0, seq++, 0)); collect_mic(&c, 2, &log, 20); }

    int zeros_during = 0, nz_late = 0;
    for (int i = mark + 3; i < after_speech; i++) if (!log.nz[i]) zeros_during++;
    for (int i = after_speech + 25; i < log.n; i++) if (log.nz[i]) nz_late++;
    CHECK(zeros_during >= 8, "while speech plays the microphone packets carry silence");
    CHECK(nz_late >= 5, "about a second after the speech the microphone is back");

    int cont = 1, tsok = 1;
    for (int i = 1; i < log.n; i++) {
        if ((uint16_t)(log.seq[i] - log.seq[i - 1]) != 1) cont = 0;
        if (log.ts[i] - log.ts[i - 1] != 256) tsok = 0;
    }
    CHECK(cont, "microphone RTP sequence numbers are continuous through the mute");
    CHECK(tsok, "microphone RTP timestamps advance by exactly 256 samples per packet through the mute");

    /* full duplex: never muted */
    sp_half = 0;
    mark = log.n;
    until = now_us() + 500000;
    while (now_us() < until) { send_frame(c.fd, 4, pkt, mk_rtp(pkt, 0, seq++, 8000)); collect_mic(&c, 2, &log, 20); }
    int muted = 0;
    for (int i = mark + 2; i < log.n; i++) if (!log.nz[i]) muted++;
    CHECK(log.n - mark > 8 && muted == 0, "audio.talk_duplex=full: the microphone is never muted");
    sp_half = 1;

    CHECK(rtsp(&c, "TEARDOWN", "x", "", 1, r, sizeof(r)) == 200, "TEARDOWN");
    conn_close(&c);
    WAIT_FOR(!sp_claimed, 1000);
}

static void test_udp(void)
{
    printf("# UDP transport of the backchannel\n");
    set_auth(1);
    conn_t c; char r[2048], v[128];
    tcp_connect(&c);
    rtsp_login(&c, "DESCRIBE", "x", REQ_BC, r, sizeof(r));
    int us = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in me = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    bind(us, (struct sockaddr *)&me, sizeof(me));
    socklen_t ml = sizeof(me);
    getsockname(us, (struct sockaddr *)&me, &ml);
    int cport = ntohs(me.sin_port);
    char tr[160];
    snprintf(tr, sizeof(tr), "Transport: RTP/AVP;unicast;client_port=%d-%d\r\n", cport, cport + 1);
    CHECK(rtsp(&c, "SETUP", "x/streamid=2", tr, 1, r, sizeof(r)) == 200, "SETUP with client_port");
    int sport = 0;
    const char *sp = hdr_value(r, "Transport", v, sizeof(v)) ? strstr(v, "server_port=") : NULL;
    if (sp) sport = atoi(sp + 12);
    CHECK(sport > 0 && strstr(v, "client_port="), "answer carries a server_port");
    unsigned long p0 = sp_pushed;
    rtsp(&c, "PLAY", "x", "", 1, r, sizeof(r));

    struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = htons((uint16_t)sport), .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    uint8_t pkt[172];
    for (int i = 0; i < 20; i++) {
        size_t n = mk_rtp(pkt, 0, (uint16_t)(50 + i), 6000);
        sendto(us, pkt, n, 0, (struct sockaddr *)&to, sizeof(to));
        wait_ms(5);
    }
    WAIT_FOR(sp_pushed - p0 >= 3200, 2000);
    CHECK(sp_pushed - p0 == 3200, "20 UDP packets reach the speaker");
    {   /* the same packets from another address (127.0.0.2) are not from the RTSP client */
        int fs = socket(AF_INET, SOCK_DGRAM, 0);
        struct sockaddr_in other = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK + 1) };
        if (fs >= 0 && bind(fs, (struct sockaddr *)&other, sizeof(other)) == 0) {
            for (int i = 0; i < 20; i++) {
                size_t n = mk_rtp(pkt, 0, (uint16_t)(500 + i), 6000);
                sendto(fs, pkt, n, 0, (struct sockaddr *)&to, sizeof(to));
            }
            wait_ms(150);
            CHECK(sp_pushed - p0 == 3200, "UDP audio from another address is dropped");
        } else {
            printf("  skip - cannot bind 127.0.0.2 here\n");
        }
        if (fs >= 0) close(fs);
    }
    CHECK(rtsp(&c, "TEARDOWN", "x", "", 1, r, sizeof(r)) == 200, "TEARDOWN");
    conn_close(&c);
    close(us);
    WAIT_FOR(!sp_claimed, 1000);
}

static void test_busy_speaker(void)
{
    printf("# a second talker\n");
    set_auth(1);
    conn_t c; char r[2048];
    sp_claimed = 1; /* someone else holds the speaker */
    tcp_connect(&c);
    rtsp_login(&c, "DESCRIBE", "x", REQ_BC, r, sizeof(r));
    rtsp(&c, "SETUP", "x/streamid=2", "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n", 1, r, sizeof(r));
    CHECK(rtsp(&c, "PLAY", "x", "", 1, r, sizeof(r)) == 551, "talk-only PLAY while the speaker is held: refused");
    conn_close(&c);
    sp_claimed = 0;
}


/* A talk-only session that is playing: 0 = ready, -1 = something refused. */
static int open_talk_only(conn_t *c)
{
    char r[2048];
    if (tcp_connect(c) != 0) return -1;
    if (rtsp_login(c, "DESCRIBE", "x", REQ_BC, r, sizeof(r)) != 200) return -1;
    if (rtsp(c, "SETUP", "x/streamid=2", "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n", 1, r, sizeof(r)) != 200) return -1;
    if (rtsp(c, "PLAY", "x", "", 1, r, sizeof(r)) != 200) return -1;
    return 0;
}

/* What makes a running talk end without the talker's consent: the talk switch,
 * the end of the password, another password. The check runs once a second. */
static void test_revocation(void)
{
    printf("# revocation while talking\n");
    uint8_t pkt[172];
    conn_t c;

    set_auth(1); sp_available = 1;
    CHECK(open_talk_only(&c) == 0, "talk-only session playing");
    send_frame(c.fd, 0, pkt, mk_rtp(pkt, 0, 1, 6000));
    sp_available = 0; /* audio.talk_enable=0 (or audio out off, or the speaker broke) */
    CHECK(server_closed(&c, 3500), "talk switched off: the session is ended");
    WAIT_FOR(!sp_claimed, 1000);
    CHECK(!sp_claimed, "talk switched off: the speaker is released");
    conn_close(&c);
    sp_available = 1;

    CHECK(open_talk_only(&c) == 0, "second talk-only session playing");
    set_auth(0);
    CHECK(server_closed(&c, 3500), "password removed: the session is ended");
    WAIT_FOR(!sp_claimed, 1000);
    CHECK(!sp_claimed, "password removed: the speaker is released");
    conn_close(&c);

    set_auth(1);
    CHECK(open_talk_only(&c) == 0, "third talk-only session playing");
    {
        char other[33];
        md5_hex_cat(other, USER, ":" RTSP_AUTH_REALM ":", "another-password", (char *)NULL);
        FILE *f = fopen(AUTH_FILE, "w");
        fprintf(f, "%s:akam:%s\n", USER, other);
        fclose(f);
    }
    CHECK(server_closed(&c, 3500), "password changed: the session is ended");
    WAIT_FOR(!sp_claimed, 1000);
    CHECK(!sp_claimed, "password changed: the speaker is released");
    conn_close(&c);
    set_auth(1);
}

/* ======================================================================
 * The host client tools/akam-talk against the same server
 * ====================================================================== */
static int run_talk(const char *args, const char *pw, const char *stdin_file)
{
    const char *bin = getenv("AKAM_TALK_BIN");
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "%s%s%s %s rtsp://alice@127.0.0.1:%d/live %s < %s 2>/dev/null",
             pw ? "AKAM_RTSP_PASSWORD='" : "env -u AKAM_RTSP_PASSWORD ", pw ? pw : "", pw ? "'" : "", bin, srv_port, args,
             stdin_file ? stdin_file : "/dev/null");
    int st = system(cmd);
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static void write_file(const char *path, const void *p, size_t n)
{
    FILE *f = fopen(path, "wb");
    fwrite(p, 1, n, f);
    fclose(f);
}

static void test_akam_talk(void)
{
    const char *bin = getenv("AKAM_TALK_BIN");
    if (!bin) { printf("  skip - AKAM_TALK_BIN not set\n"); return; }
    printf("# akam-talk (host client) against the server\n");
    set_auth(1);
    sp_available = 1;
    unsigned long p0 = sp_pushed;
    int rc = run_talk("--tone 1", PASS, NULL);
    CHECK(rc == 0, "--tone 1: exit 0");
    CHECK(sp_pushed - p0 == 8000 && sp_nonzero, "1 s tone = 8000 decoded samples at the speaker");
    WAIT_FOR(!sp_claimed, 1000);
    CHECK(!sp_claimed, "speaker released after the client ended (TEARDOWN)");

    p0 = sp_pushed;
    CHECK(run_talk("--tone 0.5 --codec pcma", PASS, NULL) == 0 && sp_pushed - p0 == 4000, "--codec pcma works");

    /* raw s16le on stdin, 0.3 s */
    static int16_t pcm[2400];
    static const int k[8] = { 0, 3673, 5194, 3673, 0, -3673, -5194, -3673 };
    for (int i = 0; i < 2400; i++) pcm[i] = (int16_t)k[i & 7];
    char raw[128], wav[128], mu[128];
    snprintf(raw, sizeof(raw), "/tmp/akam_talk_raw_%d.pcm", (int)getpid());
    snprintf(wav, sizeof(wav), "/tmp/akam_talk_wav_%d.wav", (int)getpid());
    snprintf(mu, sizeof(mu), "/tmp/akam_talk_mu_%d.ul", (int)getpid());
    write_file(raw, pcm, sizeof(pcm));
    p0 = sp_pushed;
    CHECK(run_talk("--realtime-off", PASS, raw) == 0 && sp_pushed - p0 == 2400, "raw 8 kHz s16le from stdin (2400 samples)");

    uint8_t w[44 + sizeof(pcm)] = { 'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0,
                                    1, 0, 1, 0, 0x40, 0x1f, 0, 0, 0x80, 0x3e, 0, 0, 2, 0, 16, 0, 'd', 'a', 't', 'a' };
    uint32_t dl = sizeof(pcm), rl = 36 + dl;
    memcpy(w + 4, &rl, 4); memcpy(w + 40, &dl, 4); memcpy(w + 44, pcm, sizeof(pcm));
    write_file(wav, w, sizeof(w));
    p0 = sp_pushed;
    {
        char a[200]; snprintf(a, sizeof(a), "--realtime-off --input %s", wav);
        CHECK(run_talk(a, PASS, NULL) == 0 && sp_pushed - p0 == 2400, "WAV file (8 kHz mono 16-bit): header skipped, 2400 samples");
    }
    w[24] = 0x44; w[25] = 0xac; /* 44100 Hz */
    write_file(wav, w, sizeof(w));
    {
        char a[200]; snprintf(a, sizeof(a), "--input %s", wav);
        CHECK(run_talk(a, PASS, NULL) == 1, "WAV at another sample rate: refused with a usage error (exit 1)");
    }

    uint8_t ul[1600];
    for (int i = 0; i < 1600; i++) ul[i] = g711_ulaw_encode((int16_t)k[i & 7]);
    write_file(mu, ul, sizeof(ul));
    p0 = sp_pushed;
    CHECK(run_talk("--realtime-off --format mulaw", PASS, mu) == 0 && sp_pushed - p0 == 1600, "already-G.711 input (--format mulaw)");
    unlink(raw); unlink(wav); unlink(mu);

    CHECK(run_talk("--tone 1", "wrong-password", NULL) == 4, "wrong password: exit 4");
    CHECK(run_talk("--tone 1", NULL, NULL) == 4, "no password given: exit 4");
    sp_available = 0;
    CHECK(run_talk("--tone 1", PASS, NULL) == 3, "talk not available on the camera: exit 3 (551)");
    sp_available = 1;
    set_auth(0);
    CHECK(run_talk("--tone 1", PASS, NULL) == 3, "RTSP password off: exit 3 (551), nothing played");
    set_auth(1);
    sp_claimed = 1;
    CHECK(run_talk("--tone 1", PASS, NULL) != 0, "speaker busy: failure");
    sp_claimed = 0;
    CHECK(run_talk("--tone 1 --bogus", PASS, NULL) == 1, "unknown option: exit 1");
}

int main(void)
{
    md5_hex_cat(HA1, USER, ":" RTSP_AUTH_REALM ":", PASS, (char *)NULL);
    srv_fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    socklen_t al = sizeof(a);
    if (bind(srv_fd, (struct sockaddr *)&a, sizeof(a)) != 0 || listen(srv_fd, 4) != 0) { perror("bind"); return 2; }
    getsockname(srv_fd, (struct sockaddr *)&a, &al);
    srv_port = ntohs(a.sin_port);
    pthread_t t;
    pthread_create(&t, NULL, server_main, NULL);
    pthread_detach(t);

    test_no_password();
    test_auth_gating();
    test_talk_only_tcp();
    test_garbage_frames();
    test_mic_and_talk_together();
    test_udp();
    test_busy_speaker();
    test_revocation();
    test_akam_talk();

    unlink(AUTH_FILE);
    printf(fails ? "\nFAILED: %d\n" : "\nrtsp_backchannel_test: all passed\n", fails);
    fflush(stdout);
    _exit(fails ? 1 : 0); /* the server thread is blocked in accept(): do not wait for it */
}
