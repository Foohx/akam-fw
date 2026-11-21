#pragma once
/* H.264 parameter-set helpers (no dependency on the camera or SmolRTSP, so
 * they can be unit-tested on the host): pick SPS/PPS NAL units out of an
 * Annex-B buffer and format the matching SDP fmtp attribute. */
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define H264_PS_MAX 128 /* largest SPS/PPS NAL kept; real ones are ~10-30 bytes */

typedef struct {
    uint8_t sps[H264_PS_MAX];
    size_t  sps_len;        /* 0 = not seen yet */
    uint8_t pps[H264_PS_MAX];
    size_t  pps_len;
} h264_params_t;

#define H264_FOUND_SPS 1
#define H264_FOUND_PPS 2

/* Scan the first scan_limit bytes of an Annex-B buffer (data_len bytes in
 * all) for SPS (type 7) and PPS (type 8) NAL units and store copies in *p.
 * Stops at the first slice NAL. Returns a mask of H264_FOUND_SPS /
 * H264_FOUND_PPS for the units found in this call. Only NAL units that end
 * (next start code or end of buffer) within H264_PS_MAX bytes are accepted. */
int h264_params_scan(h264_params_t *p, const uint8_t *data, size_t data_len,
                     size_t scan_limit);

/* True when both an SPS (at least profile/compat/level bytes) and a PPS are known. */
bool h264_params_ready(const h264_params_t *p);

/* Standard base64 (RFC 4648, padded). Writes a NUL-terminated string to out
 * and returns its length, or 0 if cap is too small (needs 4*ceil(n/3)+1). */
size_t h264_base64(const uint8_t *in, size_t n, char *out, size_t cap);

/* Build the SDP attribute value for the video payload type, WITHOUT the
 * leading "a=fmtp:<pt> " and trailing CRLF:
 *   packetization-mode=1[;profile-level-id=XXXXXX;sprop-parameter-sets=B64,B64]
 * The profile-level-id / sprop part is omitted when p is NULL or not ready.
 * Returns the string length, or 0 if cap is too small. */
size_t h264_fmtp_build(const h264_params_t *p, char *out, size_t cap);
