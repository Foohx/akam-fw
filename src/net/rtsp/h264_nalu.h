/* Annex B H.264 access unit -> NAL units, shared by the RTSP servers of both platforms (the packetiser itself stays SmolRTSP's). */
#ifndef AKAM_H264_NALU_H
#define AKAM_H264_NALU_H

#include <stddef.h>
#include <stdint.h>

/* Called once per NAL unit, start code excluded: `nalu` points into the buffer, `len` > 0, `is_vcl` is 1 for slice types 1..5 (the marker bit of
 * the RTP packet follows it). A non-zero return stops the walk and is returned by h264_annexb_each. */
typedef int (*h264_nalu_fn)(void *user, const uint8_t *nalu, size_t len, int is_vcl);

/* Walks the 00 00 01 / 00 00 00 01 delimited NAL units of an Annex B buffer in order (a trailing zero before a start code belongs to the start
 * code, an empty unit is skipped, bytes before the first start code are ignored). Returns 0 when every callback returned 0. */
int h264_annexb_each(const uint8_t *data, size_t len, h264_nalu_fn fn, void *user);

#endif
