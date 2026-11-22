/* Host test of src/net/rtsp/h264_nalu.c and rtcp_sr.c (the RTP send helpers both platforms' RTSP servers share). */
#include <stdio.h>
#include <string.h>

#include "h264_nalu.h"
#include "rtcp_sr.h"

static int failed, checks;
#define CHECK(c) do { checks++; if (!(c)) { failed++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

typedef struct { int n; size_t len[8]; int vcl[8]; uint8_t first[8]; int stop_at; } rec_t;
static int rec(void *u, const uint8_t *nalu, size_t len, int vcl)
{
    rec_t *r = u;
    r->len[r->n] = len; r->vcl[r->n] = vcl; r->first[r->n] = nalu[0];
    r->n++;
    return r->n == r->stop_at ? 7 : 0;
}

static void test_nalu(void)
{
    /* SPS (7), PPS (8), IDR slice (5) with a 4-byte start code, a non-IDR slice (1) with a 3-byte one; a trailing zero before a start code belongs to it */
    static const uint8_t au[] = { 0, 0, 0, 1, 0x67, 1, 2, 0, 0, 0, 1, 0x68, 3, 0, 0, 1, 0x65, 4, 5, 6, 0, 0, 1, 0x41, 9 };
    rec_t r = { 0 };
    CHECK(h264_annexb_each(au, sizeof au, rec, &r) == 0);
    CHECK(r.n == 4);
    CHECK(r.len[0] == 3 && r.first[0] == 0x67 && r.vcl[0] == 0);
    CHECK(r.len[1] == 2 && r.first[1] == 0x68 && r.vcl[1] == 0);
    CHECK(r.len[2] == 4 && r.first[2] == 0x65 && r.vcl[2] == 1);
    CHECK(r.len[3] == 2 && r.first[3] == 0x41 && r.vcl[3] == 1);
    /* a callback error stops the walk and is returned */
    memset(&r, 0, sizeof r);
    r.stop_at = 2;
    CHECK(h264_annexb_each(au, sizeof au, rec, &r) == 7 && r.n == 2);
    /* no start code, too short, empty NAL units */
    memset(&r, 0, sizeof r);
    static const uint8_t junk[] = { 1, 2, 3, 4, 5 };
    CHECK(h264_annexb_each(junk, sizeof junk, rec, &r) == 0 && r.n == 0);
    CHECK(h264_annexb_each(au, 3, rec, &r) == 0 && r.n == 0);
    static const uint8_t empty[] = { 0, 0, 1, 0, 0, 1, 0x41, 7 };
    CHECK(h264_annexb_each(empty, sizeof empty, rec, &r) == 0 && r.n == 1 && r.first[0] == 0x41);
}

static void test_sr(void)
{
    uint8_t b[RTCP_SR_SIZE], f[RTCP_SR_FRAME_SIZE];
    /* 2000-01-01T00:00:00Z = 946684800 s: NTP seconds 946684800 + 2208988800 = 3155673600 = 0xBC17C200, fraction 0 */
    CHECK(rtcp_sr_build(b, 0x11223344u, 0x01020304u, 5, 1000, 946684800LL * 1000000LL) == RTCP_SR_SIZE);
    static const uint8_t want[RTCP_SR_SIZE] = { 0x80, 200, 0, 6, 0x11, 0x22, 0x33, 0x44, 0xBC, 0x17, 0xC2, 0x00, 0, 0, 0, 0,
                                                0x01, 0x02, 0x03, 0x04, 0, 0, 0, 5, 0, 0, 0x03, 0xE8 };
    CHECK(memcmp(b, want, sizeof want) == 0);
    CHECK(rtcp_sr_frame(f, 3, 0x11223344u, 0x01020304u, 5, 1000, 946684800LL * 1000000LL) == RTCP_SR_FRAME_SIZE);
    CHECK(f[0] == '$' && f[1] == 3 && f[2] == 0 && f[3] == RTCP_SR_SIZE && memcmp(f + 4, want, sizeof want) == 0);
}

int main(void)
{
    test_nalu();
    test_sr();
    printf("rtsp_tx_test: %d checks, %d failed\n", checks, failed);
    return failed != 0;
}
