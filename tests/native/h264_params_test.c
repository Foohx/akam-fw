/* Host unit test for src/ak_rtsp/h264_params.c (SPS/PPS extraction, base64,
 * SDP fmtp builder). Synthetic bytes only. Build/run: tests/h264_params_test.sh */
#include <stdio.h>
#include <string.h>
#include "h264_params.h"

static int fails;
#define CHECK(cond, msg) do { if (cond) printf("  ok   - %s\n", msg); \
    else { printf("  FAIL - %s\n", msg); fails++; } } while (0)

static void b64(const char *in, const char *want)
{
    char out[64];
    size_t n = h264_base64((const uint8_t *)in, strlen(in), out, sizeof(out));
    char msg[96];
    snprintf(msg, sizeof(msg), "base64(\"%s\") == \"%s\"", in, want);
    CHECK(n == strlen(want) && strcmp(out, want) == 0, msg);
}

int main(void)
{
    /* RFC 4648 section 10 vectors */
    b64("", ""); b64("f", "Zg=="); b64("fo", "Zm8="); b64("foo", "Zm9v");
    b64("foob", "Zm9vYg=="); b64("fooba", "Zm9vYmE="); b64("foobar", "Zm9vYmFy");
    {
        char small[4];
        CHECK(h264_base64((const uint8_t *)"foo", 3, small, sizeof(small)) == 0,
              "base64 refuses a too small buffer");
    }

    /* 4-byte start code SPS, 3-byte start code PPS, SEI, then an IDR slice. */
    static const uint8_t sps[] = {0x67, 0x4D, 0x00, 0x28, 0xAB, 0x40, 0x3C, 0x01};
    static const uint8_t pps[] = {0x68, 0xEE, 0x3C, 0x80};
    uint8_t frame[256];
    size_t o = 0;
    static const uint8_t sc4[] = {0, 0, 0, 1}, sc3[] = {0, 0, 1};
    memcpy(frame + o, sc4, 4); o += 4; memcpy(frame + o, sps, sizeof(sps)); o += sizeof(sps);
    memcpy(frame + o, sc3, 3); o += 3; memcpy(frame + o, pps, sizeof(pps)); o += sizeof(pps);
    memcpy(frame + o, sc4, 4); o += 4; frame[o++] = 0x06; frame[o++] = 0x05; frame[o++] = 0x11;
    memcpy(frame + o, sc4, 4); o += 4; frame[o++] = 0x65;
    for (int i = 0; i < 100; i++) frame[o++] = (uint8_t)(0x80 + i);

    h264_params_t p;
    memset(&p, 0, sizeof(p));
    CHECK(!h264_params_ready(&p), "empty cache is not ready");
    int f = h264_params_scan(&p, frame, o, 64);
    CHECK(f == (H264_FOUND_SPS | H264_FOUND_PPS), "scan finds SPS and PPS");
    CHECK(p.sps_len == sizeof(sps) && memcmp(p.sps, sps, sizeof(sps)) == 0, "SPS bytes exact");
    CHECK(p.pps_len == sizeof(pps) && memcmp(p.pps, pps, sizeof(pps)) == 0, "PPS bytes exact");
    CHECK(h264_params_ready(&p), "cache ready after scan");

    char fmtp[256];
    size_t n = h264_fmtp_build(&p, fmtp, sizeof(fmtp));
    /* base64 of 67 4D 00 28 AB 40 3C 01 = Z00AKKtAPAE=, of 68 EE 3C 80 = aO48gA== */
    CHECK(n > 0 && strcmp(fmtp, "packetization-mode=1;profile-level-id=4D0028;"
                                "sprop-parameter-sets=Z00AKKtAPAE=,aO48gA==") == 0,
          "fmtp has profile-level-id and sprop-parameter-sets");
    CHECK(h264_fmtp_build(&p, fmtp, 20) == 0, "fmtp refuses a too small buffer");

    n = h264_fmtp_build(NULL, fmtp, sizeof(fmtp));
    CHECK(n > 0 && strcmp(fmtp, "packetization-mode=1") == 0, "fmtp without params is the plain form");

    /* A P-frame (slice only) yields nothing and leaves the cache intact. */
    uint8_t pframe[32];
    memcpy(pframe, sc4, 4); pframe[4] = 0x41; memset(pframe + 5, 0x55, 20);
    CHECK(h264_params_scan(&p, pframe, 25, 25) == 0, "P-frame has no parameter sets");
    CHECK(p.sps_len == sizeof(sps), "cache kept after a P-frame");

    /* SPS cut by the end of the buffer (no following start code) is accepted;
     * an oversized NAL is not. */
    h264_params_t q;
    memset(&q, 0, sizeof(q));
    uint8_t tail[16];
    memcpy(tail, sc4, 4); memcpy(tail + 4, sps, sizeof(sps));
    CHECK(h264_params_scan(&q, tail, 4 + sizeof(sps), 64) == H264_FOUND_SPS, "SPS ending at buffer end");
    uint8_t big[400];
    memcpy(big, sc4, 4); big[4] = 0x67; memset(big + 5, 0x55, 300);
    memset(&q, 0, sizeof(q));
    CHECK(h264_params_scan(&q, big, 305, 305) == 0, "oversized SPS rejected");

    printf("%s\n", fails ? "RESULT FAIL" : "RESULT PASS");
    return fails ? 1 : 0;
}
