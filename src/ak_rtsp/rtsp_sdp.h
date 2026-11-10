#pragma once
/* Session description and ONVIF backchannel negotiation (specs/07 section 1.2).
 *
 * Pure text and decision code, no SmolRTSP and no hardware, so that
 * tests/rtsp_sdp_test.sh can pin the bytes of the SDP:
 *   - a client that does not ask for the backchannel gets exactly the SDP the
 *     server always sent (video + L16 microphone), byte for byte;
 *   - a client that sends "Require: www.onvif.org/ver20/backchannel" and may be
 *     served gets one more media section, a send-only PCMU/PCMA track. */
#include <stddef.h>

#define RTSP_BACKCHANNEL_TAG "www.onvif.org/ver20/backchannel"
/* "a=control:streamid=N" of each track; clients build the SETUP URI from it. */
#define RTSP_TRACK_VIDEO_ID 0
#define RTSP_TRACK_AUDIO_ID 1
#define RTSP_TRACK_BACKCHANNEL_ID 2

typedef struct {
    unsigned long long session_time; /* o= line: session id and version (seconds) */
    const char *fmtp;                /* H.264 fmtp parameters, without "a=fmtp:96 " */
    int audio_rate;                  /* L16 microphone track: sample rate */
    int audio_channels;
    int backchannel;                 /* 1: add the send-only G.711 track */
} rtsp_sdp_params_t;

/* Writes the SDP, NUL terminated. Returns its length, or 0 when cap is too small
 * (buf then holds an empty string). */
size_t rtsp_sdp_build(char *buf, size_t cap, const rtsp_sdp_params_t *p);

/* 1 when the comma separated option-tag list of a Require header (not NUL
 * terminated) contains tag (case-insensitive, whitespace around items ignored). */
int rtsp_require_has(const char *value, size_t len, const char *tag);

typedef enum {
    RTSP_BC_NOT_REQUESTED, /* the client did not ask: behave as before */
    RTSP_BC_OFFER,         /* asked, and served: add the track */
    RTSP_BC_REFUSE         /* asked, cannot be served: 551 Option not supported */
} rtsp_bc_decision_t;

/* required: the request carries the backchannel Require tag.
 * auth_on: RTSP Digest is enabled and its credentials file is valid (talk
 *          is never offered without a password: specs/07 decision U6/section 1.6).
 * available: audio out is enabled, its device is running and the talk soft
 *          switch (audio.talk_enable) is on. */
rtsp_bc_decision_t rtsp_backchannel_decide(int required, int auth_on, int available);

typedef enum { RTSP_TRACK_VIDEO, RTSP_TRACK_AUDIO, RTSP_TRACK_BACKCHANNEL } rtsp_track_t;

/* Track a SETUP request URI refers to ("...streamid=N"). Anything that is not
 * streamid=1 or streamid=2 has always meant the video track. */
rtsp_track_t rtsp_track_from_uri(const char *uri);
