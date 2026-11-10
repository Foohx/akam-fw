#include "rtsp_sdp.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

size_t rtsp_sdp_build(char *buf, size_t cap, const rtsp_sdp_params_t *p)
{
    /* Audio track uses payload type 97 with L16 (linear PCM): anyka_ipc's own
     * capture stream is raw 8kHz/mono/16-bit, not G.711, so RFC 3551 L16
     * needs no fmtp at all. PT 97 (dynamic) matches the convention anyka_ipc's
     * own RTSP SDP builder uses for all of its audio codec options ("Session 3"
     * correction), even though it's not required for L16 specifically.
     * The two time fields of the o= line are the same value, as always. */
    int n = snprintf(buf, cap,
        "v=0\r\n"
        "o=- %llu %llu IN IP4 0.0.0.0\r\n"
        "s=ak_rtsp\r\n"
        "t=0 0\r\n"
        "m=video 0 RTP/AVP 96\r\n"
        "c=IN IP4 0.0.0.0\r\n"
        "a=rtpmap:96 H264/90000\r\n"
        "a=fmtp:96 %s\r\n"
        "a=control:streamid=%d\r\n"
        "m=audio 0 RTP/AVP 97\r\n"
        "c=IN IP4 0.0.0.0\r\n"
        "a=rtpmap:97 L16/%d/%d\r\n"
        "a=control:streamid=%d\r\n",
        p->session_time, p->session_time, p->fmtp, RTSP_TRACK_VIDEO_ID,
        p->audio_rate, p->audio_channels, RTSP_TRACK_AUDIO_ID);
    if (n < 0 || (size_t)n >= cap) {
        if (cap) buf[0] = '\0';
        return 0;
    }
    if (p->backchannel) {
        /* ONVIF Streaming Specification, audio backchannel: "sendonly" is from
         * the client's side (the client sends to the camera). PCMU first. */
        int m = snprintf(buf + n, cap - (size_t)n,
            "m=audio 0 RTP/AVP 0 8\r\n"
            "c=IN IP4 0.0.0.0\r\n"
            "a=control:streamid=%d\r\n"
            "a=rtpmap:0 PCMU/8000\r\n"
            "a=rtpmap:8 PCMA/8000\r\n"
            "a=sendonly\r\n",
            RTSP_TRACK_BACKCHANNEL_ID);
        if (m < 0 || (size_t)m >= cap - (size_t)n) {
            buf[0] = '\0';
            return 0;
        }
        n += m;
    }
    return (size_t)n;
}

static int is_ws(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

int rtsp_require_has(const char *value, size_t len, const char *tag)
{
    size_t tl = strlen(tag);
    size_t i = 0;
    while (i < len) {
        size_t e = i;
        while (e < len && value[e] != ',') e++;
        size_t b = i, f = e;
        while (b < f && is_ws(value[b])) b++;
        while (f > b && is_ws(value[f - 1])) f--;
        if (f - b == tl && strncasecmp(value + b, tag, tl) == 0) return 1;
        i = e + 1;
    }
    return 0;
}

rtsp_bc_decision_t rtsp_backchannel_decide(int required, int auth_on, int available)
{
    if (!required) return RTSP_BC_NOT_REQUESTED;
    return (auth_on && available) ? RTSP_BC_OFFER : RTSP_BC_REFUSE;
}

rtsp_track_t rtsp_track_from_uri(const char *uri)
{
    if (strstr(uri, "streamid=2")) return RTSP_TRACK_BACKCHANNEL;
    if (strstr(uri, "streamid=1")) return RTSP_TRACK_AUDIO;
    return RTSP_TRACK_VIDEO;
}
