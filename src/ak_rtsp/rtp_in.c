#include "rtp_in.h"

#include <string.h>

#include "g711.h"

void rtp_in_init(rtp_in_t *r)
{
    memset(r, 0, sizeof(*r));
    r->last_pt = -1;
}

const char *rtp_in_result_name(rtp_in_result_t r)
{
    switch (r) {
    case RTP_IN_OK:           return "ok";
    case RTP_IN_DROP_SHORT:   return "short";
    case RTP_IN_DROP_BIG:     return "big";
    case RTP_IN_DROP_VERSION: return "version";
    case RTP_IN_DROP_PADDING: return "padding";
    case RTP_IN_DROP_PT:      return "payload-type";
    case RTP_IN_DROP_EMPTY:   return "empty";
    case RTP_IN_DROP_DUP:     return "duplicate";
    }
    return "?";
}

static rtp_in_result_t drop(rtp_in_t *r, rtp_in_result_t why)
{
    r->dropped++;
    r->dropped_by[why]++;
    return why;
}

rtp_in_result_t rtp_in_feed(rtp_in_t *r, const uint8_t *pkt, size_t len,
                            int16_t *pcm, size_t *n, int *pt)
{
    if (len > RTP_IN_MAX_PACKET) return drop(r, RTP_IN_DROP_BIG);
    if (len < 12) return drop(r, RTP_IN_DROP_SHORT);
    if ((pkt[0] >> 6) != 2) return drop(r, RTP_IN_DROP_VERSION);

    size_t off = 12 + 4 * (size_t)(pkt[0] & 0x0f); /* fixed header + CSRC list */
    if (pkt[0] & 0x10) {                           /* header extension: 4 bytes + 4 * length */
        if (off + 4 > len) return drop(r, RTP_IN_DROP_SHORT);
        off += 4 + 4 * (((size_t)pkt[off + 2] << 8) | pkt[off + 3]);
    }
    if (off > len) return drop(r, RTP_IN_DROP_SHORT);

    size_t end = len;
    if (pkt[0] & 0x20) {                           /* padding: last byte counts itself */
        size_t pad = pkt[len - 1];
        if (pad == 0 || pad > end - off) return drop(r, RTP_IN_DROP_PADDING);
        end -= pad;
    }

    int ptype = pkt[1] & 0x7f;
    if (ptype != RTP_IN_PT_PCMU && ptype != RTP_IN_PT_PCMA) return drop(r, RTP_IN_DROP_PT);
    if (end <= off) return drop(r, RTP_IN_DROP_EMPTY);

    uint16_t seq = (uint16_t)(((unsigned)pkt[2] << 8) | pkt[3]);
    uint32_t ssrc = ((uint32_t)pkt[8] << 24) | ((uint32_t)pkt[9] << 16) |
                    ((uint32_t)pkt[10] << 8) | pkt[11];

    if (r->started && ssrc == r->ssrc) {
        /* Serial-number arithmetic: a small positive 16-bit difference is
         * "newer" (the gap is counted as lost), zero or a small negative one a
         * duplicate or late packet. A jump far in either direction is a client
         * that restarted its sequence on the same SSRC: follow it, count nothing. */
        int diff = (int16_t)(uint16_t)(seq - r->last_seq);
        if (diff <= 0 && diff > -RTP_IN_RESYNC_WINDOW) return drop(r, RTP_IN_DROP_DUP);
        if (diff > 0 && diff <= RTP_IN_RESYNC_WINDOW) r->lost += (unsigned long)(diff - 1);
    }
    r->started = 1;
    r->ssrc = ssrc;
    r->last_seq = seq;
    r->last_pt = ptype;

    size_t count = end - off;
    if (ptype == RTP_IN_PT_PCMU) g711_ulaw_decode_buf(pkt + off, pcm, count);
    else                         g711_alaw_decode_buf(pkt + off, pcm, count);
    r->packets++;
    r->samples += count;
    *n = count;
    *pt = ptype;
    return RTP_IN_OK;
}
