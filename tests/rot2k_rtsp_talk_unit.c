/* Host loopback test of the ONVIF audio backchannel of the Rotatable 2K RTSP server (rtsp_session.c, REAL server, SmolRTSP, rtsp_auth Digest, rtsp_sdp,
 * and the shared talk session / RTP decoder; only the hardware side is stubbed: the speaker engine and the capture). What is checked: the backchannel is
 * offered only to a client that asks (Require) with the password on and the speaker usable, else 551; the SDP track (streamid=2, send-only PCMU/PCMA); SETUP over
 * TCP interleaved (UDP is refused with 461: this server has no UDP); PLAY starts one talk session, the client's G.711 reaches the speaker queue decoded,
 * one talker at a time, TEARDOWN and a dropped connection release the speaker, talk switched off ends a running session; a talk-only session has no video.
 * Prints "rot2k rtsp talk unit: N failed". */
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

#include "aring.h"
#include "audio.h"
#include "cap.h"
#include "g711.h"
#include "log.h"
#include "md5.h"
#include "ptz.h"
#include "rtsp_auth.h"
#include "rtsp_session.h"

static int fails;
#define CHECK(c, msg) do { if (c) printf("  ok   - %s\n", msg); else { printf("  FAIL - %s\n", msg); fails++; } } while (0)

/* ---- stubs of the hardware side ---- */
volatile sig_atomic_t a2k_stop;
void a2k_log(const char *fmt, ...) { (void)fmt; }
int a2k_audio_up(void) { return 0; }
a2k_aring_t *a2k_audio_ring(void) { return NULL; }
int a2k_ptz_privacy_active(void) { return 0; }

static volatile int sp_available = 1, sp_half = 1, sp_claimed, sp_claims, sp_releases;
static volatile unsigned long sp_pushed;
static volatile int sp_nonzero;
int audio_out_talk_available(void) { return sp_available; }
int audio_out_talk_half_duplex(void) { return sp_half; }
size_t audio_out_queued(void) { return 0; }
size_t audio_out_push(const int16_t *pcm, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++)
        if (pcm[i])
            sp_nonzero = 1;
    sp_pushed += n;
    return n;
}
int audio_out_feeder_claim(void)
{
    if (sp_claimed)
        return -1;
    sp_claimed = 1;
    sp_claims++;
    return 0;
}
void audio_out_feeder_release(void)
{
    sp_claimed = 0;
    sp_releases++;
}

/* ---- client ---- */
static int g_port;
static char g_ha1[33];
static char g_auth_path[300];
static const char *USER = "alice", *PASS = "s3cret";

typedef struct {
    int fd, cseq;
    char nonce[64], session[64], buf[16384];
    size_t have;
} conn_t;

static int free_port(void)
{
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    socklen_t l = sizeof a;
    int fd = socket(AF_INET, SOCK_STREAM, 0), p;
    if (bind(fd, (struct sockaddr *)&a, sizeof a) < 0)
        return 0;
    getsockname(fd, (struct sockaddr *)&a, &l);
    p = ntohs(a.sin_port);
    close(fd);
    return p;
}

static int dial(conn_t *c)
{
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)g_port), .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    struct timeval tv = { 10, 0 };
    memset(c, 0, sizeof *c);
    c->fd = socket(AF_INET, SOCK_STREAM, 0);
    setsockopt(c->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    if (connect(c->fd, (struct sockaddr *)&a, sizeof a) < 0) {
        close(c->fd);
        c->fd = -1;
        return -1;
    }
    return 0;
}

static void conn_close(conn_t *c)
{
    if (c->fd >= 0)
        close(c->fd);
    c->fd = -1;
}

static void send_all(int fd, const void *p, size_t n)
{
    const char *s = p;
    while (n) {
        ssize_t r = send(fd, s, n, MSG_NOSIGNAL);
        if (r <= 0)
            return;
        s += r;
        n -= (size_t)r;
    }
}

static int fill(conn_t *c)
{
    ssize_t r;
    if (c->have >= sizeof c->buf)
        return 0;
    r = recv(c->fd, c->buf + c->have, sizeof c->buf - c->have, 0);
    if (r <= 0)
        return 0;
    c->have += (size_t)r;
    return 1;
}

static void consume(conn_t *c, size_t n)
{
    memmove(c->buf, c->buf + n, c->have - n);
    c->have -= n;
}

/* One response (interleaved frames that precede it are skipped). The status, or -1. */
static int read_response(conn_t *c, char *out, size_t cap)
{
    for (;;) {
        char *he;
        while (c->have >= 4 && c->buf[0] == '$') {
            size_t len = ((size_t)(uint8_t)c->buf[2] << 8) | (uint8_t)c->buf[3];
            if (c->have < 4 + len)
                break;
            consume(c, 4 + len);
        }
        he = c->have >= 4 && c->buf[0] != '$' ? memmem(c->buf, c->have, "\r\n\r\n", 4) : NULL;
        if (he) {
            size_t hl = (size_t)(he - c->buf) + 4, bl = 0, total;
            const char *cl = strcasestr(c->buf, "Content-Length:");
            int code = -1;
            if (cl && cl < c->buf + hl)
                bl = (size_t)atoi(cl + 15);
            if (c->have >= hl + bl) {
                total = hl + bl < cap ? hl + bl : cap - 1;
                memcpy(out, c->buf, total);
                out[total] = 0;
                sscanf(out, "RTSP/1.0 %d", &code);
                consume(c, hl + bl);
                return code;
            }
        }
        if (!fill(c))
            return -1;
    }
}

static const char *hdr(const char *resp, const char *name, char *out, size_t cap)
{
    char key[64];
    const char *p;
    size_t n;
    snprintf(key, sizeof key, "\r\n%s:", name);
    p = strcasestr(resp, key);
    if (!p)
        return NULL;
    p += strlen(key);
    while (*p == ' ')
        p++;
    n = strcspn(p, "\r\n");
    if (n >= cap)
        n = cap - 1;
    memcpy(out, p, n);
    out[n] = 0;
    return out;
}

static int rtsp(conn_t *c, const char *method, const char *path, const char *extra, int auth, char *resp, size_t cap)
{
    char uri[160], req[2048], ah[512] = "";
    int n, code;
    snprintf(uri, sizeof uri, "rtsp://127.0.0.1:%d/%s", g_port, path);
    if (auth && c->nonce[0]) {
        char r[33];
        rtsp_auth_response(r, g_ha1, c->nonce, NULL, NULL, method, uri);
        snprintf(ah, sizeof ah, "Authorization: Digest username=\"%s\", realm=\"akam\", nonce=\"%s\", uri=\"%s\", response=\"%s\"\r\n", USER, c->nonce,
                 uri, r);
    }
    n = snprintf(req, sizeof req, "%s %s RTSP/1.0\r\nCSeq: %d\r\n", method, uri, ++c->cseq);
    if (c->session[0])
        n += snprintf(req + n, sizeof req - (size_t)n, "Session: %s\r\n", c->session);
    n += snprintf(req + n, sizeof req - (size_t)n, "%s%s\r\n", ah, extra ? extra : "");
    send_all(c->fd, req, (size_t)n);
    code = read_response(c, resp, cap);
    if (code == 401 && auth) {
        const char *p = strstr(resp, "nonce=\"");
        if (p) {
            size_t k;
            p += 7;
            k = strcspn(p, "\"");
            if (k >= sizeof c->nonce)
                k = sizeof c->nonce - 1;
            memcpy(c->nonce, p, k);
            c->nonce[k] = 0;
        }
    }
    if (code == 200) {
        char s[64];
        if (hdr(resp, "Session", s, sizeof s)) {
            s[strcspn(s, ";")] = 0;
            snprintf(c->session, sizeof c->session, "%s", s);
        }
    }
    return code;
}

/* The same request through the 401 handshake when the password is on. */
static int rtsp_login(conn_t *c, const char *method, const char *path, const char *extra, char *resp, size_t cap)
{
    int code = rtsp(c, method, path, extra, 1, resp, cap);
    if (code == 401)
        code = rtsp(c, method, path, extra, 1, resp, cap);
    return code;
}

#define REQ_BC "Require: www.onvif.org/ver20/backchannel\r\n"
#define TCP_BC "Transport: RTP/AVP/TCP;unicast;interleaved=4-5\r\n"

static void set_auth(int on)
{
    if (on) {
        FILE *f = fopen(g_auth_path, "w");
        if (f) {
            fprintf(f, "%s:akam:%s\n", USER, g_ha1);
            fclose(f);
        }
    } else {
        unlink(g_auth_path);
    }
}

static size_t mk_rtp(uint8_t *p, int pt, uint16_t seq, int amp)
{
    static const int k[8] = { 0, 707, 1000, 707, 0, -707, -1000, -707 };
    int i;
    memset(p, 0, 12);
    p[0] = 0x80;
    p[1] = (uint8_t)pt;
    p[2] = (uint8_t)(seq >> 8);
    p[3] = (uint8_t)seq;
    p[11] = 0x42;
    for (i = 0; i < 160; i++) {
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

#define WAIT_FOR(cond, ms) do { int w_; for (w_ = 0; w_ < (ms) && !(cond); w_ += 5) usleep(5000); } while (0)

static int server_closed(conn_t *c, int ms)
{
    struct pollfd p = { c->fd, POLLIN, 0 };
    int w;
    for (w = 0; w < ms; w += 20) {
        if (poll(&p, 1, 20) > 0) {
            char b[256];
            ssize_t r = recv(c->fd, b, sizeof b, MSG_DONTWAIT);
            if (r == 0 || (r < 0 && errno != EAGAIN))
                return 1;
        }
    }
    return 0;
}

/* A talk-only session up to PLAY. 0, or the failing step. */
static int start_talk(conn_t *c, char *resp, size_t cap)
{
    if (dial(c) < 0)
        return 1;
    if (rtsp_login(c, "DESCRIBE", "stream0", REQ_BC, resp, cap) != 200)
        return 2;
    if (rtsp_login(c, "SETUP", "stream0/streamid=2", TCP_BC, resp, cap) != 200)
        return 3;
    if (rtsp_login(c, "PLAY", "stream0", NULL, resp, cap) != 200)
        return 4;
    return 0;
}

int main(void)
{
    static a2k_stream_t m, s;
    a2k_stream_t *sp[2] = { &m, &s };
    static const uint8_t au[] = { 0, 0, 0, 1, 0x67, 0x4D, 0x00, 0x28, 0xAB, 0x40, 0x3C, 0x01, 0, 0, 1, 0x68, 0xEE, 0x3C, 0x80, 0, 0, 0, 1, 0x65, 0x80, 0x81, 0x82 };
    static char resp[8192];
    conn_t c, c2;
    uint8_t pkt[200];
    unsigned long before;
    char tmp[300];
    const char *td = getenv("TMPDIR");
    int i, rc;

    setvbuf(stdout, NULL, _IONBF, 0);
    snprintf(g_auth_path, sizeof g_auth_path, "%s/rot2k_talk_auth_%d", td && *td ? td : ".", (int)getpid());
    md5_hex_cat(g_ha1, USER, ":" RTSP_AUTH_REALM ":", PASS, (char *)NULL);
    a2k_stream_init(&m, "stream0", 2304, 1296, 15, 30, 2000, A2K_MAX_FRAME_MAIN);
    a2k_stream_init(&s, "stream1", 640, 360, 15, 30, 512, A2K_MAX_FRAME_SUB);
    a2k_stream_publish(&m, au, sizeof au);
    a2k_stream_publish(&s, au, sizeof au);
    g_port = free_port();
    set_auth(0);
    CHECK(g_port > 0 && a2k_rtsp_start(g_port, g_auth_path, sp) == 0, "start: one listener");

    printf("rot2k rtsp talk unit\n");
    /* password off: the backchannel is never offered */
    CHECK(dial(&c) == 0, "connect (password off)");
    CHECK(rtsp(&c, "DESCRIBE", "stream0", NULL, 0, resp, sizeof resp) == 200 && !strstr(resp, "streamid=2") && !strstr(resp, "sendonly"),
          "no Require: the SDP is the plain video one (no backchannel track)");
    CHECK(rtsp(&c, "DESCRIBE", "stream0", REQ_BC, 0, resp, sizeof resp) == 551 && strcasestr(resp, "Unsupported: www.onvif.org/ver20/backchannel"),
          "Require with the password off: 551 Option not supported, naming the tag");
    CHECK(rtsp(&c, "SETUP", "stream0/streamid=2", REQ_BC TCP_BC, 0, resp, sizeof resp) == 551, "SETUP of the backchannel with Require and the password off: 551");
    CHECK(rtsp(&c, "SETUP", "stream0/streamid=2", TCP_BC, 0, resp, sizeof resp) == 404, "SETUP of the backchannel without Require and the password off: 404, as on the 1080p");
    conn_close(&c);

    /* password on, speaker usable */
    set_auth(1);
    CHECK(dial(&c) == 0, "connect (password on)");
    CHECK(rtsp_login(&c, "DESCRIBE", "stream0", NULL, resp, sizeof resp) == 200 && !strstr(resp, "streamid=2"), "password on, no Require: still no backchannel track");
    CHECK(rtsp_login(&c, "DESCRIBE", "stream0", REQ_BC, resp, sizeof resp) == 200 && strstr(resp, "m=audio 0 RTP/AVP 0 8") && strstr(resp, "a=control:streamid=2") &&
              strstr(resp, "a=rtpmap:0 PCMU/8000") && strstr(resp, "a=rtpmap:8 PCMA/8000") && strstr(resp, "a=sendonly"),
          "Require + password + speaker: the SDP offers the send-only PCMU/PCMA track streamid=2");
    sp_available = 0;
    CHECK(rtsp_login(&c, "DESCRIBE", "stream0", REQ_BC, resp, sizeof resp) == 551, "speaker not usable (talk off or no device): 551");
    sp_available = 1;
    CHECK(rtsp_login(&c, "SETUP", "stream0/streamid=2", "Transport: RTP/AVP;unicast;client_port=5000-5001\r\n", resp, sizeof resp) == 461, "UDP transport: 461 Unsupported Transport (TCP only)");
    CHECK(rtsp_login(&c, "SETUP", "stream0/streamid=2", "Transport: RTP/AVP/TCP;unicast;interleaved=4-4\r\n", resp, sizeof resp) == 400, "equal interleaved channels: 400");
    CHECK(rtsp_login(&c, "PLAY", "stream0", NULL, resp, sizeof resp) == 455, "PLAY with nothing set up: 455");
    CHECK(rtsp_login(&c, "SETUP", "stream0/streamid=2", TCP_BC, resp, sizeof resp) == 200 && strstr(resp, "interleaved=4-5"), "SETUP of the backchannel over TCP interleaved 4-5");
    CHECK(sp_claims == 0, "SETUP does not take the speaker");
    CHECK(rtsp_login(&c, "PLAY", "stream0", NULL, resp, sizeof resp) == 200, "PLAY of a talk-only session");
    WAIT_FOR(sp_claims == 1, 500);
    CHECK(sp_claims == 1 && sp_claimed, "PLAY takes the speaker (one talk session)");

    /* a second talker is refused */
    CHECK(dial(&c2) == 0 && rtsp_login(&c2, "DESCRIBE", "stream0", REQ_BC, resp, sizeof resp) == 200 &&
              rtsp_login(&c2, "SETUP", "stream0/streamid=2", TCP_BC, resp, sizeof resp) == 200,
          "a second client sets up the backchannel");
    CHECK(rtsp_login(&c2, "PLAY", "stream0", NULL, resp, sizeof resp) == 551 && sp_claims == 1, "its PLAY is refused while the first talks (one talker at a time)");
    conn_close(&c2);

    /* audio from the talker reaches the speaker, decoded; silence does not make a sound */
    before = sp_pushed;
    for (i = 0; i < 6; i++)
        send_frame(c.fd, 4, pkt, mk_rtp(pkt, 0, (uint16_t)i, 8000)); /* PCMU, channel 4 = the RTP channel of SETUP */
    WAIT_FOR(sp_pushed >= before + 6 * 160, 800);
    CHECK(sp_pushed >= before + 6 * 160, "6 PCMU packets of 160 samples reach the speaker queue (960 samples)");
    CHECK(sp_nonzero, "the pushed samples are decoded audio, not silence");
    before = sp_pushed;
    for (i = 0; i < 4; i++)
        send_frame(c.fd, 4, pkt, mk_rtp(pkt, 8, (uint16_t)(10 + i), 8000)); /* PCMA */
    WAIT_FOR(sp_pushed >= before + 4 * 160, 800);
    CHECK(sp_pushed >= before + 4 * 160, "PCMA is decoded as well");
    before = sp_pushed;
    send_frame(c.fd, 7, pkt, mk_rtp(pkt, 0, 99, 8000)); /* a channel that is not the backchannel's */
    usleep(150000);
    CHECK(sp_pushed == before, "a frame on another channel is not audio for the speaker");

    /* TEARDOWN releases the speaker */
    CHECK(rtsp_login(&c, "TEARDOWN", "stream0", NULL, resp, sizeof resp) == 200, "TEARDOWN is answered");
    WAIT_FOR(sp_releases >= 1, 800);
    CHECK(sp_releases == 1 && !sp_claimed, "TEARDOWN releases the speaker");
    conn_close(&c);

    /* a connection that just drops releases it too */
    rc = start_talk(&c, resp, sizeof resp);
    CHECK(rc == 0, "a new talk session starts after the first ended");
    WAIT_FOR(sp_claimed, 500);
    conn_close(&c);
    WAIT_FOR(!sp_claimed, 1500);
    CHECK(!sp_claimed && sp_releases == 2, "a dropped connection releases the speaker");

    /* talk switched off (or the speaker lost) ends a running session */
    rc = start_talk(&c, resp, sizeof resp);
    CHECK(rc == 0, "another talk session");
    WAIT_FOR(sp_claimed, 500);
    sp_available = 0;
    CHECK(server_closed(&c, 3000), "talk switched off: the server ends the talk-only connection");
    WAIT_FOR(!sp_claimed, 500);
    CHECK(!sp_claimed && sp_releases == 3, "and the speaker is free");
    conn_close(&c);
    sp_available = 1;

    /* the password switched off ends it as well: talk is never open */
    rc = start_talk(&c, resp, sizeof resp);
    CHECK(rc == 0, "a session with the password on");
    WAIT_FOR(sp_claimed, 500);
    set_auth(0);
    CHECK(server_closed(&c, 3000), "the password removed: the talk connection is ended");
    WAIT_FOR(!sp_claimed, 500);
    CHECK(!sp_claimed, "and the speaker is free");
    conn_close(&c);

    snprintf(tmp, sizeof tmp, "%s", g_auth_path);
    unlink(tmp);
    a2k_stop = 1;
    a2k_rtsp_stop();
    printf("rot2k rtsp talk unit: %d failed\n", fails);
    return fails != 0;
}
