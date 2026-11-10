/* Host test of src/ak_rtsp/rtsp_sdp.c: the SDP of the server (golden bytes) and
 * the negotiation of the ONVIF backchannel. Build/run: tests/rtsp_sdp_test.sh */
#include <stdio.h>
#include <string.h>

#include "rtsp_sdp.h"

static int fails;
#define CHECK(cond, msg) do { if (cond) printf("  ok   - %s\n", msg); \
    else { printf("  FAIL - %s\n", msg); fails++; } } while (0)

/* The SDP exactly as Client_describe() built it before the backchannel existed
 * (snprintf of the same literal, copied from rtsp.c at the commit that added
 * this module). A regression here means a player that never asked for talk saw
 * a different DESCRIBE answer. */
static void legacy_sdp(char *out, size_t cap, unsigned long long t, const char *fmtp, int rate, int ch)
{
    snprintf(out, cap,
        "v=0\r\n"
        "o=- %llu %llu IN IP4 0.0.0.0\r\n"
        "s=ak_rtsp\r\n"
        "t=0 0\r\n"
        "m=video 0 RTP/AVP 96\r\n"
        "c=IN IP4 0.0.0.0\r\n"
        "a=rtpmap:96 H264/90000\r\n"
        "a=fmtp:96 %s\r\n"
        "a=control:streamid=0\r\n"
        "m=audio 0 RTP/AVP 97\r\n"
        "c=IN IP4 0.0.0.0\r\n"
        "a=rtpmap:97 L16/%d/%d\r\n"
        "a=control:streamid=1\r\n",
        t, t, fmtp, rate, ch);
}

static const char GOLDEN_DEFAULT[] =
    "v=0\r\n"
    "o=- 1700000000 1700000000 IN IP4 0.0.0.0\r\n"
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

static const char GOLDEN_BACKCHANNEL_TAIL[] =
    "m=audio 0 RTP/AVP 0 8\r\n"
    "c=IN IP4 0.0.0.0\r\n"
    "a=control:streamid=2\r\n"
    "a=rtpmap:0 PCMU/8000\r\n"
    "a=rtpmap:8 PCMA/8000\r\n"
    "a=sendonly\r\n";

static void test_sdp(void)
{
    printf("# SDP\n");
    char buf[1280], old[1280];
    rtsp_sdp_params_t p = { 1700000000ULL, "packetization-mode=1", 8000, 1, 0 };

    size_t n = rtsp_sdp_build(buf, sizeof(buf), &p);
    CHECK(n == strlen(GOLDEN_DEFAULT) && !strcmp(buf, GOLDEN_DEFAULT), "without backchannel: golden bytes");
    legacy_sdp(old, sizeof(old), p.session_time, p.fmtp, 8000, 1);
    CHECK(!strcmp(buf, old), "without backchannel: identical to the pre-backchannel format string");

    /* a real sprop fmtp (long) and other times */
    const char *fmtp =
        "packetization-mode=1;profile-level-id=64001F;"
        "sprop-parameter-sets=Z2QAH6zZQFAFuhAAAAMAEAAAAwDI8YMZYA==,aOvssiw=";
    p.fmtp = fmtp; p.session_time = 4294967296ULL;
    rtsp_sdp_build(buf, sizeof(buf), &p);
    legacy_sdp(old, sizeof(old), p.session_time, fmtp, 8000, 1);
    CHECK(!strcmp(buf, old), "with sprop-parameter-sets and a 64-bit time: identical");

    p.fmtp = "packetization-mode=1"; p.session_time = 1700000000ULL; p.backchannel = 1;
    n = rtsp_sdp_build(buf, sizeof(buf), &p);
    char want[1280];
    snprintf(want, sizeof(want), "%s%s", GOLDEN_DEFAULT, GOLDEN_BACKCHANNEL_TAIL);
    CHECK(n == strlen(want) && !strcmp(buf, want), "with backchannel: the unchanged SDP plus one send-only G.711 section");
    CHECK(strstr(buf, "streamid=0") && strstr(buf, "streamid=1") && strstr(buf, "streamid=2"), "three tracks, ids 0 1 2");
    CHECK(strncmp(buf, GOLDEN_DEFAULT, strlen(GOLDEN_DEFAULT)) == 0, "existing tracks come first and are untouched");

    char tiny[40];
    CHECK(rtsp_sdp_build(tiny, sizeof(tiny), &p) == 0 && tiny[0] == '\0', "buffer too small: 0 and an empty string");
    char mid[sizeof(GOLDEN_DEFAULT) + 10];
    CHECK(rtsp_sdp_build(mid, sizeof(mid), &p) == 0 && mid[0] == '\0', "no room for the backchannel section: 0, never half a section");
}

static void test_require(void)
{
    printf("# Require header\n");
    const char *t = RTSP_BACKCHANNEL_TAG;
    CHECK(rtsp_require_has(t, strlen(t), t), "the tag alone");
    CHECK(rtsp_require_has("WWW.ONVIF.ORG/VER20/BACKCHANNEL", 31, t), "case-insensitive");
    const char *l = "  foo.bar ,\twww.onvif.org/ver20/backchannel , baz";
    CHECK(rtsp_require_has(l, strlen(l), t), "in a list with spaces and tabs");
    const char *c = "www.onvif.org/ver20/backchannel\r\n";
    CHECK(rtsp_require_has(c, strlen(c), t), "trailing CRLF");
    CHECK(!rtsp_require_has("", 0, t), "empty");
    CHECK(!rtsp_require_has("www.onvif.org/ver20/backchannel2", 32, t), "longer tag is another tag");
    CHECK(!rtsp_require_has("www.onvif.org/ver20/backchann", 29, t), "prefix is not the tag");
    CHECK(!rtsp_require_has("play.basic, www.onvif.org/ver20/replay", 38, t), "other ONVIF tags do not count");
    CHECK(!rtsp_require_has("www.onvif.org/ver20/backchannel", 10, t), "length limits the read (no NUL needed)");
}

static void test_decide(void)
{
    printf("# negotiation\n");
    CHECK(rtsp_backchannel_decide(0, 1, 1) == RTSP_BC_NOT_REQUESTED, "no Require: as before");
    CHECK(rtsp_backchannel_decide(0, 0, 0) == RTSP_BC_NOT_REQUESTED, "no Require, nothing available: still as before");
    CHECK(rtsp_backchannel_decide(1, 1, 1) == RTSP_BC_OFFER, "Require + password + speaker: offered");
    CHECK(rtsp_backchannel_decide(1, 0, 1) == RTSP_BC_REFUSE, "Require without password: 551");
    CHECK(rtsp_backchannel_decide(1, 1, 0) == RTSP_BC_REFUSE, "Require without speaker or soft switch off: 551");
    CHECK(rtsp_backchannel_decide(1, 0, 0) == RTSP_BC_REFUSE, "Require, nothing: 551");
}

static void test_track(void)
{
    printf("# SETUP track\n");
    CHECK(rtsp_track_from_uri("rtsp://192.0.2.1/streamid=0") == RTSP_TRACK_VIDEO, "streamid=0 video");
    CHECK(rtsp_track_from_uri("rtsp://192.0.2.1/streamid=1") == RTSP_TRACK_AUDIO, "streamid=1 microphone");
    CHECK(rtsp_track_from_uri("rtsp://192.0.2.1/streamid=2") == RTSP_TRACK_BACKCHANNEL, "streamid=2 backchannel");
    CHECK(rtsp_track_from_uri("rtsp://192.0.2.1/") == RTSP_TRACK_VIDEO, "bare URL: video (as always)");
    CHECK(rtsp_track_from_uri("rtsp://192.0.2.1/stream/streamid=2") == RTSP_TRACK_BACKCHANNEL, "base path before the control value");
}

int main(void)
{
    test_sdp();
    test_require();
    test_decide();
    test_track();
    printf(fails ? "\nFAILED: %d\n" : "\nrtsp_sdp_test: all passed\n", fails);
    return fails ? 1 : 0;
}
