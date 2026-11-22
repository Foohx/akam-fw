#include "rtcp_sr.h"

#include <arpa/inet.h>
#include <stdbool.h>
#include <smolrtsp/types/rtcp.h>

#include "rtp_clock.h"

_Static_assert(RTCP_SR_SIZE == SMOLRTSP_RTCP_SR_SIZE_BASE, "RTCP_SR_SIZE (rtcp_sr.h) must stay the size SmolRTSP serialises");

size_t rtcp_sr_build(uint8_t out[RTCP_SR_SIZE], uint32_t ssrc, uint32_t rtp_ts, uint32_t pkt_count, uint32_t octet_count, int64_t real_us)
{
    uint32_t ntp_sec, ntp_frac;
    SmolRTSP_RtcpSr sr;

    rtp_ntp_from_unix_us(real_us, &ntp_sec, &ntp_frac);
    sr = (SmolRTSP_RtcpSr){
        .padding = false,
        .rc = 0,
        .ssrc = htonl(ssrc),
        .ntp_sec = htonl(ntp_sec),
        .ntp_frac = htonl(ntp_frac),
        .rtp_ts = htonl(rtp_ts),
        .pkt_count = htonl(pkt_count),
        .octet_count = htonl(octet_count),
    };
    return SmolRTSP_RtcpSr_serialize(sr, out) ? RTCP_SR_SIZE : 0;
}

size_t rtcp_sr_frame(uint8_t out[RTCP_SR_FRAME_SIZE], uint8_t channel, uint32_t ssrc, uint32_t rtp_ts, uint32_t pkt_count, uint32_t octet_count,
                     int64_t real_us)
{
    out[0] = '$';
    out[1] = channel;
    out[2] = 0;
    out[3] = RTCP_SR_SIZE;
    return rtcp_sr_build(out + 4, ssrc, rtp_ts, pkt_count, octet_count, real_us) ? RTCP_SR_FRAME_SIZE : 0;
}
