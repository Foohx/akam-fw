/* RTCP Sender Report writer shared by the RTSP servers of both platforms (serialised by SmolRTSP, which both link). */
#ifndef AKAM_RTCP_SR_H
#define AKAM_RTCP_SR_H

#include <stddef.h>
#include <stdint.h>

#define RTCP_SR_SIZE 28 /* a Sender Report without report blocks (SMOLRTSP_RTCP_SR_SIZE_BASE) */
#define RTCP_SR_FRAME_SIZE (4 + RTCP_SR_SIZE)

/* Writes the 28-byte SR (no padding, no report block) for a stream: `ssrc`, `rtp_ts`, the packet and octet counts so far (host order, converted
 * here) and the wall clock `real_us` (microseconds since the Unix epoch, converted to NTP by rtp_ntp_from_unix_us). Returns RTCP_SR_SIZE, or 0. */
size_t rtcp_sr_build(uint8_t out[RTCP_SR_SIZE], uint32_t ssrc, uint32_t rtp_ts, uint32_t pkt_count, uint32_t octet_count, int64_t real_us);
/* The same behind the RFC 2326 section 10.12 interleaved header '$', channel, 0, 28. Returns RTCP_SR_FRAME_SIZE, or 0. */
size_t rtcp_sr_frame(uint8_t out[RTCP_SR_FRAME_SIZE], uint8_t channel, uint32_t ssrc, uint32_t rtp_ts, uint32_t pkt_count, uint32_t octet_count,
                     int64_t real_us);

#endif
