#pragma once
/* RTP receive side of the ONVIF audio backchannel (specs/audio.md).
 *
 * The client sends G.711 audio (PCMU PT 0, PCMA PT 8, 8 kHz mono) in RTP
 * packets, interleaved in the RTSP/TCP connection ('$' frames) or over UDP.
 * This module validates one packet at a time and decodes its payload to 16-bit
 * linear samples. Pure code: no sockets, no allocation, no clock, no hardware
 * (tests/rtp_in_test.sh).
 *
 * Every packet is bounds-checked (RFC 3550 section 5.1: version 2, CSRC list,
 * header extension, padding). Anything that is not clean audio is dropped and
 * counted by reason; nothing here ever trusts a length field. */
#include <stddef.h>
#include <stdint.h>

#define RTP_IN_PT_PCMU 0
#define RTP_IN_PT_PCMA 8
/* Largest RTP packet accepted (bytes, header included). One G.711 packet of
 * 20 to 40 ms is 172 to 332 bytes; players never exceed an Ethernet MTU. */
#define RTP_IN_MAX_PACKET 1400
/* Sequence distance (packets) beyond which a jump is taken as a restart of the
 * stream rather than loss or reordering: 1000 packets = 20 s of audio. */
#define RTP_IN_RESYNC_WINDOW 1000
/* Largest payload that can come out of one packet (G.711: one byte = one sample). */
#define RTP_IN_MAX_SAMPLES RTP_IN_MAX_PACKET

typedef enum {
    RTP_IN_OK = 0,
    RTP_IN_DROP_SHORT,     /* shorter than the fixed header or than its own CSRC/extension */
    RTP_IN_DROP_BIG,       /* over RTP_IN_MAX_PACKET */
    RTP_IN_DROP_VERSION,   /* not RTP version 2 */
    RTP_IN_DROP_PADDING,   /* padding length larger than the payload */
    RTP_IN_DROP_PT,        /* payload type other than PCMU/PCMA (telephone-event, comfort noise, video...) */
    RTP_IN_DROP_EMPTY,     /* no audio payload */
    RTP_IN_DROP_DUP        /* same or older sequence number than the last one accepted */
} rtp_in_result_t;

typedef struct {
    int      started;       /* a packet was accepted */
    uint32_t ssrc;          /* of the stream being followed */
    uint16_t last_seq;
    int      last_pt;       /* payload type of the last accepted packet, -1 before */

    /* counters (never reset by the module itself) */
    unsigned long packets;  /* accepted packets */
    unsigned long samples;  /* decoded samples */
    unsigned long lost;     /* sequence gaps (packets that never arrived) */
    unsigned long dropped;  /* rejected packets, all reasons */
    unsigned long dropped_by[RTP_IN_DROP_DUP + 1];
} rtp_in_t;

void rtp_in_init(rtp_in_t *r);

/* Validates one RTP packet and decodes its G.711 payload.
 * pcm must have room for RTP_IN_MAX_SAMPLES samples. On RTP_IN_OK, *n is the
 * number of samples written and *pt the payload type (0 or 8); on any other
 * result nothing was written and the matching drop counter was incremented.
 * A new SSRC restarts the sequence tracking (a client may restart its stream). */
rtp_in_result_t rtp_in_feed(rtp_in_t *r, const uint8_t *pkt, size_t len,
                            int16_t *pcm, size_t *n, int *pt);

const char *rtp_in_result_name(rtp_in_result_t r);
