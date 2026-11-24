/* Multi-client RTSP server, Rotatable 2K own app (PoC-3). See rtsp_session.h.
 *
 * Shared code used UNMODIFIED from src/ (net/rtsp) and third_party/smolrtsp (phase A of PLAN section 8ter): smolrtsp (request
 * parser, responses, RTP/NAL/FU-A packetiser, TCP interleaved transport), rtsp_in (stream framing), rtsp_idle
 * (pre-PLAY idle policy), rtsp_auth (Digest), rtsp_sdp (SDP text), h264_params (SPS/PPS,
 * sprop-parameter-sets), rtp_clock (timestamps), net_tx (bounded writer + private queue), tx_pace (backpressure).
 * The 1080p session code (platforms/anyka-ak3918/rtsp_session.c) is NOT reusable as is: it is one blocking client wired to the 1080p
 * encoder ring, the audio track, the status LED and the control port. The controller below is a thin adapter that
 * follows the same logic (same responses, same pacing) on top of the a2k_stream ring, one thread per client. */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <smolrtsp.h>
#include <smolrtsp/types/rtcp.h>
#include <interface99.h>
#include <datatype99.h>

#include "aring.h"
#include "audio.h"
#include "audio_out.h"
#include "cap.h"
#include "log.h"
#include "ptz.h"
#include "rtsp_session.h"
#include "h264_nalu.h"
#include "h264_params.h"
#include "net_tx.h"
#include "rtcp_sr.h"
#include "rtp_clock.h"
#include "rtsp_auth.h"
#include "rtsp_idle.h"
#include "rtsp_in.h"
#include "rtsp_sdp.h"
#include "talk.h"
#include "tx_pace.h"
#include "lat_stat.h"

#define SESSION_TIMEOUT_SEC 60
#define VIDEO_CLOCK_RATE 90000
#define AUDIO_PAYLOAD_TYPE 97 /* rtsp_sdp.c: "m=audio 0 RTP/AVP 97", a=rtpmap:97 L16/8000/1 */
#define RTCP_SR_INTERVAL_SEC 5
/* Audio periods sent per client-loop pass at most (the ring holds 32: a client that is behind catches up in a few passes). */
#define AUDIO_BURST 8
/* A client that accepts no byte at all for this long is gone (same budget as the 1080p server). */
#define TX_STALL_BUDGET_MS 15000
/* A write that stalled this long: restart the video on the next keyframe. */
#define TX_RESYNC_STALL_MS 1000
#define TEARDOWN_FLUSH_MS 200
/* Longest sleep of a client thread in poll(): bounds the reaction to the stop flag and the auth re-check. */
#define POLL_MAX_MS 250
/* Private TCP send queue per playing client: the kernel buffer takes the first ~240 KB of a frame, the queue the
 * rest. Allocated at PLAY and only touched when the peer is slow, so a healthy client costs no RSS for it. */
#define TX_QUEUE_MAIN (512 * 1024)
#define TX_QUEUE_SUB (128 * 1024)

static a2k_stream_t *g_streams[2];
static rtsp_auth_db_t g_auth;
static int g_listen = -1;
static pthread_t g_accept_th;
static int g_accept_started;
static pthread_mutex_t g_cl_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cl_cv = PTHREAD_COND_INITIALIZER;
static int g_nclients;
static int g_nplaying; /* clients in PLAY right now (the status LED rule `stream`) */

typedef struct {
    int sock;
    struct sockaddr_storage addr;
    socklen_t addr_len;

    uint64_t session_id;
    bool is_playing;
    bool counted; /* is_playing is reflected in g_nplaying */
    bool tx_fail;
    const char *drop_reason;
    net_tx_t tx;
    tx_pace_t pace;
    lat_stat_t lat; /* frame age at send, logged every LAT_STAT_PERIOD_MS */

    a2k_stream_t *stream;
    int stream_idx;
    uint64_t cursor;
    int wake[2]; /* pipe: the capture thread writes wake[1] for every published frame */
    bool wake_on;

    SmolRTSP_RtpTransport *rtp_transport;
    SmolRTSP_NalTransport *nal_transport;
    uint8_t rtcp_channel;
    bool need_idr;
    bool privacy; /* video is being withheld (PTZ privacy) */
    int idr_wait_frames;
    unsigned pace_recovered;
    uint64_t video_ts_base_us;

    /* Optional microphone track (--audio 1, streamid=1 of the SDP): plain RTP L16, interleaved like the video. */
    SmolRTSP_RtpTransport *audio_rtp_transport;
    uint8_t audio_rtcp_channel;
    uint32_t audio_ts_base;
    uint64_t audio_cursor;
    bool audio_wake_on;
    rtp_aclock_t audio_clock;   /* sample index -> monotonic time, for the RTCP SR */
    rtp_astart_t audio_start;   /* holds audio until the first video frame, drops the backlog before it */
    int64_t next_sr_video_us, next_sr_audio_us;

    /* ONVIF audio backchannel (streamid=2, TCP interleaved only): the client sends G.711 to the speaker. */
    bool bc_setup;   /* SETUP of the backchannel done */
    bool bc_playing; /* a talk session runs (talk_begin() succeeded) */
    bool bc_noted;   /* the idle clock of the talk session was started */
    uint8_t bc_rtp_ch, bc_rtcp_ch;

    rtsp_auth_conn_t auth;
    rtsp_in_t in;
    rtsp_idle_t idle;
    unsigned long sent, skipped;
} Client;

static int64_t ms_now(void) { return rtp_mono_us() / 1000; }

static uint64_t rand_u64(void)
{
    uint64_t v = 0;
    int i;
    for (i = 0; i < 5; i++)
        v = (v << 15) ^ (uint64_t)(rand() & 0x7fff);
    return v;
}

static const char *client_ip(const Client *c, char *buf, size_t cap)
{
    const void *a = NULL;
    if (c->addr.ss_family == AF_INET)
        a = &((const struct sockaddr_in *)&c->addr)->sin_addr;
    else if (c->addr.ss_family == AF_INET6)
        a = &((const struct sockaddr_in6 *)&c->addr)->sin6_addr;
    if (!a || !inet_ntop(c->addr.ss_family, a, buf, (socklen_t)cap))
        snprintf(buf, cap, "?");
    return buf;
}

/* "/stream1" selects the sub stream, "/stream0" the main one; anything else (also a bare "/") is not served. */
static int stream_from_uri(const char *uri)
{
    if (strstr(uri, "/stream1"))
        return 1;
    if (strstr(uri, "/stream0"))
        return 0;
    return -1;
}

declImpl(SmolRTSP_Controller, Client);

static void release_video_track(Client *self)
{
    if (self->nal_transport)
        VTABLE(SmolRTSP_NalTransport, SmolRTSP_Droppable).drop(self->nal_transport);
    else if (self->rtp_transport)
        VTABLE(SmolRTSP_RtpTransport, SmolRTSP_Droppable).drop(self->rtp_transport);
    self->nal_transport = NULL;
    self->rtp_transport = NULL;
}

static void release_audio_track(Client *self)
{
    if (self->audio_rtp_transport)
        VTABLE(SmolRTSP_RtpTransport, SmolRTSP_Droppable).drop(self->audio_rtp_transport);
    self->audio_rtp_transport = NULL;
    self->next_sr_audio_us = 0;
}

static void play_count(Client *self, bool playing)
{
    if (self->counted == playing)
        return;
    self->counted = playing;
    pthread_mutex_lock(&g_cl_mu);
    g_nplaying += playing ? 1 : -1;
    pthread_mutex_unlock(&g_cl_mu);
}

/* Ends the talk session of this client (the speaker plays what is queued, then the amplifier idles off). Only the client that
 * began it may end it: talk_end() acts on the one global session. */
static void bc_stop(Client *self)
{
    if (self->bc_playing) {
        talk_end(talk_global());
        self->bc_playing = false;
        self->bc_noted = false;
    }
}

static void Client_drop(VSelf)
{
    VSELF(Client);
    bc_stop(self);
    play_count(self, false);
    release_video_track(self);
    release_audio_track(self);
}

impl(SmolRTSP_Droppable, Client);

static void respond_session_header(const Client *self, SmolRTSP_Context *ctx)
{
    if (self->session_id != 0)
        smolrtsp_header(ctx, SMOLRTSP_HEADER_SESSION, "%llu", (unsigned long long)self->session_id);
}

static void Client_options(VSelf, SmolRTSP_Context *ctx, const SmolRTSP_Request *req)
{
    VSELF(Client);
    (void)req;
    smolrtsp_header(ctx, SMOLRTSP_HEADER_PUBLIC, "OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN, GET_PARAMETER");
    respond_session_header(self, ctx);
    smolrtsp_respond_ok(ctx);
}

static bool find_header_ci(const SmolRTSP_HeaderMap *m, const char *name, CharSlice99 *out)
{
    size_t i, n = strlen(name);
    for (i = 0; i < m->len; i++) {
        CharSlice99 k = m->headers[i].key;
        if (k.len == n && strncasecmp(k.ptr, name, n) == 0) {
            *out = m->headers[i].value;
            return true;
        }
    }
    return false;
}

/* The request carries "Require: www.onvif.org/ver20/backchannel". */
static bool request_requires_backchannel(const SmolRTSP_Request *req)
{
    CharSlice99 v;
    return find_header_ci(&req->header_map, "Require", &v) && rtsp_require_has(v.ptr, v.len, RTSP_BACKCHANNEL_TAG);
}

/* Talk is offered only with the RTSP password on (Digest, valid credentials): there is no open talk mode (same rule as the 1080p). */
static bool backchannel_available(void)
{
    return rtsp_auth_db_mode(&g_auth) == RTSP_AUTH_ON && audio_out_talk_available();
}

/* 551: the option the client requires is not supported; clients that always send Require (go2rtc) retry without it. */
static void respond_backchannel_unsupported(SmolRTSP_Context *ctx)
{
    smolrtsp_header(ctx, SMOLRTSP_HEADER_UNSUPPORTED, "%s", RTSP_BACKCHANNEL_TAG);
    smolrtsp_respond(ctx, SMOLRTSP_STATUS_OPTION_NOT_SUPPORTED, "Option not supported");
}

/* SETUP of the backchannel track. TCP interleaved only (this server has no UDP): a client that offers UDP is told 461 and retries. */
static int setup_backchannel(Client *self, SmolRTSP_Context *ctx, const SmolRTSP_Request *req)
{
    CharSlice99 transport_val;
    SmolRTSP_TransportConfig config;

    if (!backchannel_available()) {
        if (request_requires_backchannel(req))
            respond_backchannel_unsupported(ctx);
        else
            smolrtsp_respond(ctx, SMOLRTSP_STATUS_NOT_FOUND, "Not Found");
        return -1;
    }
    if (!SmolRTSP_HeaderMap_find(&req->header_map, SMOLRTSP_HEADER_TRANSPORT, &transport_val)) {
        smolrtsp_respond(ctx, SMOLRTSP_STATUS_BAD_REQUEST, "Transport not present");
        return -1;
    }
    if (smolrtsp_parse_transport(&config, transport_val) == -1) {
        smolrtsp_respond(ctx, SMOLRTSP_STATUS_BAD_REQUEST, "Malformed Transport");
        return -1;
    }
    if (config.lower != SmolRTSP_LowerTransport_TCP) {
        smolrtsp_respond(ctx, SMOLRTSP_STATUS_UNSUPPORTED_TRANSPORT, "Unsupported Transport");
        return -1;
    }
    ifLet(config.interleaved, SmolRTSP_ChannelPair_Some, interleaved)
    {
        if (interleaved->rtp_channel == interleaved->rtcp_channel) {
            smolrtsp_respond(ctx, SMOLRTSP_STATUS_BAD_REQUEST, "Bad interleaved channels");
            return -1;
        }
        self->bc_rtp_ch = interleaved->rtp_channel;
        self->bc_rtcp_ch = interleaved->rtcp_channel;
        self->bc_setup = true;
        a2k_log("backchannel: TCP interleaved (RTP ch %d, RTCP ch %d)", (int)interleaved->rtp_channel,
                (int)interleaved->rtcp_channel);
        smolrtsp_header(ctx, SMOLRTSP_HEADER_TRANSPORT, "RTP/AVP/TCP;unicast;interleaved=%d-%d",
                        (int)interleaved->rtp_channel, (int)interleaved->rtcp_channel);
        return 0;
    }
    smolrtsp_respond(ctx, SMOLRTSP_STATUS_BAD_REQUEST, "interleaved not found");
    return -1;
}

static void Client_describe(VSelf, SmolRTSP_Context *ctx, const SmolRTSP_Request *req)
{
    VSELF(Client);
    int idx = stream_from_uri(CharSlice99_alloca_c_str(req->start_line.uri));
    h264_params_t params;
    char sdp_buf[1536], fmtp[320];
    rtsp_sdp_params_t sp;
    int i;
    rtsp_bc_decision_t bc;

    if (idx < 0) {
        smolrtsp_respond(ctx, SMOLRTSP_STATUS_NOT_FOUND, "Not Found");
        return;
    }
    /* ONVIF backchannel: only for a client that asks for it (Require header). */
    bc = rtsp_backchannel_decide(request_requires_backchannel(req), rtsp_auth_db_mode(&g_auth) == RTSP_AUTH_ON,
                                 audio_out_talk_available());
    if (bc == RTSP_BC_REFUSE) {
        respond_backchannel_unsupported(ctx);
        return;
    }
    self->stream_idx = idx;
    self->stream = g_streams[idx];
    /* The SPS/PPS come from the first keyframe, normally there within a frame or two of boot; a client that
     * connects before that waits a moment (bounded) so that the SDP carries sprop-parameter-sets. */
    a2k_stream_params(self->stream, &params);
    for (i = 0; i < 150 && !h264_params_ready(&params) && !a2k_stop; i++) {
        usleep(20000);
        a2k_stream_params(self->stream, &params);
    }
    if (!h264_fmtp_build(&params, fmtp, sizeof fmtp))
        snprintf(fmtp, sizeof fmtp, "packetization-mode=1");
    memset(&sp, 0, sizeof sp);
    sp.session_time = (unsigned long long)time(NULL);
    sp.fmtp = fmtp;
    sp.audio_off = !a2k_audio_up(); /* no microphone track unless --audio 1 and the capture is streaming right now */
    sp.audio_rate = 8000;
    sp.audio_channels = 1;
    sp.backchannel = (bc == RTSP_BC_OFFER);
    if (rtsp_sdp_build(sdp_buf, sizeof sdp_buf, &sp) == 0) {
        smolrtsp_respond_internal_error(ctx);
        return;
    }
    smolrtsp_header(ctx, SMOLRTSP_HEADER_CONTENT_TYPE, "application/sdp");
    smolrtsp_body(ctx, CharSlice99_from_str(sdp_buf));
    smolrtsp_respond_ok(ctx);
}

static void Client_setup(VSelf, SmolRTSP_Context *ctx, const SmolRTSP_Request *req)
{
    VSELF(Client);
    CharSlice99 transport_val;
    SmolRTSP_TransportConfig config;
    int idx;
    bool audio_track;

    if (self->is_playing) {
        smolrtsp_respond(ctx, SMOLRTSP_STATUS_METHOD_NOT_VALID_IN_THIS_STATE, "Method Not Valid in This State");
        return;
    }
    if (rtsp_track_from_uri(CharSlice99_alloca_c_str(req->start_line.uri)) == RTSP_TRACK_BACKCHANNEL) {
        /* Not tied to a video stream: a talk-only session has none. */
        if (setup_backchannel(self, ctx, req) < 0)
            return;
        if (self->session_id == 0)
            self->session_id = (uint64_t)rand();
        smolrtsp_header(ctx, SMOLRTSP_HEADER_SESSION, "%llu;timeout=%d", (unsigned long long)self->session_id,
                        SESSION_TIMEOUT_SEC);
        smolrtsp_respond_ok(ctx);
        return;
    }
    idx = stream_from_uri(CharSlice99_alloca_c_str(req->start_line.uri));
    if (idx < 0 && self->stream)
        idx = self->stream_idx;
    if (idx < 0) {
        smolrtsp_respond(ctx, SMOLRTSP_STATUS_NOT_FOUND, "Not Found");
        return;
    }
    audio_track = rtsp_track_from_uri(CharSlice99_alloca_c_str(req->start_line.uri)) == RTSP_TRACK_AUDIO;
    if (audio_track && !a2k_audio_up()) { /* not in the SDP (off, or the capture is down): a client error, never the video */
        smolrtsp_respond(ctx, SMOLRTSP_STATUS_NOT_FOUND, "No audio track");
        return;
    }
    /* Both tracks of a session belong to one stream: an audio SETUP on the other stream than the video (or the reverse)
     * would silently re-point the client at it. */
    if (self->stream && idx != self->stream_idx && (audio_track ? self->nal_transport != NULL : self->audio_rtp_transport != NULL)) {
        smolrtsp_respond(ctx, SMOLRTSP_STATUS_BAD_REQUEST, "Tracks of one session must be on the same stream");
        return;
    }
    if (!SmolRTSP_HeaderMap_find(&req->header_map, SMOLRTSP_HEADER_TRANSPORT, &transport_val)) {
        smolrtsp_respond(ctx, SMOLRTSP_STATUS_BAD_REQUEST, "Transport not present");
        return;
    }
    if (smolrtsp_parse_transport(&config, transport_val) == -1) {
        smolrtsp_respond(ctx, SMOLRTSP_STATUS_BAD_REQUEST, "Malformed Transport");
        return;
    }
    /* PoC-3: RTP over TCP interleaved only. A client that offered UDP first is told so and retries with TCP. */
    if (config.lower != SmolRTSP_LowerTransport_TCP) {
        smolrtsp_respond(ctx, SMOLRTSP_STATUS_UNSUPPORTED_TRANSPORT, "Unsupported Transport");
        return;
    }
    ifLet(config.interleaved, SmolRTSP_ChannelPair_Some, interleaved)
    {
        SmolRTSP_Transport t;
        self->stream_idx = idx;
        self->stream = g_streams[idx];
        t = smolrtsp_transport_tcp(SmolRTSP_Context_get_writer(ctx), interleaved->rtp_channel, 0);
        if (audio_track) {
            release_audio_track(self); /* a repeated SETUP replaces the track, it does not leak the old one */
            self->audio_rtcp_channel = interleaved->rtcp_channel;
            self->audio_rtp_transport = SmolRTSP_RtpTransport_new(t, AUDIO_PAYLOAD_TYPE, A2K_AUDIO_RATE);
            self->audio_ts_base = (uint32_t)rand_u64();
            rtp_aclock_init(&self->audio_clock);
        } else {
            release_video_track(self);
            self->rtcp_channel = interleaved->rtcp_channel;
            self->rtp_transport = SmolRTSP_RtpTransport_new(t, 96, VIDEO_CLOCK_RATE);
            self->nal_transport = SmolRTSP_NalTransport_new(self->rtp_transport);
            self->video_ts_base_us = rand_u64();
        }
        smolrtsp_header(ctx, SMOLRTSP_HEADER_TRANSPORT, "RTP/AVP/TCP;unicast;interleaved=%d-%d",
                        (int)interleaved->rtp_channel, (int)interleaved->rtcp_channel);
        if (self->session_id == 0)
            self->session_id = (uint64_t)rand();
        smolrtsp_header(ctx, SMOLRTSP_HEADER_SESSION, "%llu;timeout=%d", (unsigned long long)self->session_id,
                        SESSION_TIMEOUT_SEC);
        smolrtsp_respond_ok(ctx);
        return;
    }
    smolrtsp_respond(ctx, SMOLRTSP_STATUS_BAD_REQUEST, "interleaved not found");
}

static void Client_play(VSelf, SmolRTSP_Context *ctx, const SmolRTSP_Request *req)
{
    VSELF(Client);
    const bool media = self->nal_transport || self->audio_rtp_transport;
    (void)req;
    if ((!media && !self->bc_setup) || (media && !self->stream)) {
        smolrtsp_respond(ctx, SMOLRTSP_STATUS_METHOD_NOT_VALID_IN_THIS_STATE, "Method Not Valid in This State");
        return;
    }
    if (self->bc_setup && !self->bc_playing) {
        char ip[INET6_ADDRSTRLEN];
        if (backchannel_available() && talk_begin(talk_global(), client_ip(self, ip, sizeof ip)) == 0) {
            self->bc_playing = true;
        } else {
            a2k_log("backchannel: not started (no longer available or the speaker is busy)");
            if (!media) { /* a talk-only session has nothing else to offer */
                respond_backchannel_unsupported(ctx);
                return;
            }
        }
    }
    if (media && !self->is_playing) {
        tx_pace_init(&self->pace);
        if (net_tx_queue_enable(&self->tx, self->stream_idx ? TX_QUEUE_SUB : TX_QUEUE_MAIN) < 0)
            a2k_log("no memory for the TCP send queue: this client keeps blocking writes");
        if (self->nal_transport && a2k_stream_wake_add(self->stream, self->wake[1]) < 0) {
            smolrtsp_respond(ctx, SMOLRTSP_STATUS_SERVICE_UNAVAILABLE, "Service Unavailable");
            return;
        }
        self->wake_on = self->nal_transport != NULL;
        if (self->audio_rtp_transport) {
            if (a2k_aring_wake_add(a2k_audio_ring(), self->wake[1]) < 0) {
                smolrtsp_respond(ctx, SMOLRTSP_STATUS_SERVICE_UNAVAILABLE, "Service Unavailable");
                if (self->wake_on)
                    a2k_stream_wake_del(self->stream, self->wake[1]);
                self->wake_on = false;
                return;
            }
            self->audio_wake_on = true;
            self->audio_cursor = a2k_aring_head(a2k_audio_ring()); /* only audio from now on */
            /* With video the audio is held until the first video frame goes out (released in send_video_frame) and
             * the backlog older than it is dropped; audio alone starts at once. */
            if (self->nal_transport)
                rtp_astart_arm(&self->audio_start);
            else
                rtp_astart_release(&self->audio_start, rtp_mono_us());
        }
        self->is_playing = true;
        play_count(self, true);
        if (self->nal_transport) {
            /* Start on a keyframe: when a recent one (at most A2K_MAX_AGE_MS old) is in the ring begin there (instant
             * start, a short burst), else ask the encoder for one now (request_idr, done by the capture thread) and
             * drop frames until it arrives. Never the oldest frame of the ring. */
            lat_stat_init(&self->lat);
            if (!a2k_stream_join(self->stream, &self->cursor, rtp_mono_us(), (int64_t)A2K_MAX_AGE_MS * 1000))
                a2k_stream_want_idr(self->stream); /* none, or one older than the age bound: a fresh one is cheaper than a stale start */
            self->need_idr = true;
            self->idr_wait_frames = 0;
        }
    }
    smolrtsp_header(ctx, SMOLRTSP_HEADER_RANGE, "npt=now-");
    respond_session_header(self, ctx);
    smolrtsp_respond_ok(ctx);
}

static void Client_teardown(VSelf, SmolRTSP_Context *ctx, const SmolRTSP_Request *req)
{
    VSELF(Client);
    (void)req;
    bc_stop(self);
    self->is_playing = false;
    play_count(self, false);
    respond_session_header(self, ctx);
    smolrtsp_respond_ok(ctx);
    if (!self->drop_reason)
        self->drop_reason = "TEARDOWN";
}

/* GET_PARAMETER without a body is the keepalive of live555/VLC (RFC 2326 section 10.8). */
static void Client_unknown(VSelf, SmolRTSP_Context *ctx, const SmolRTSP_Request *req)
{
    VSELF(Client);
    const SmolRTSP_Method method = req->start_line.method, get_param = SMOLRTSP_METHOD_GET_PARAMETER,
                          set_param = SMOLRTSP_METHOD_SET_PARAMETER;

    if (SmolRTSP_Method_eq(&method, &get_param)) {
        CharSlice99 session;
        if (self->session_id != 0 && SmolRTSP_HeaderMap_find(&req->header_map, SMOLRTSP_HEADER_SESSION, &session)) {
            char sbuf[64];
            size_t n = session.len < sizeof sbuf - 1 ? session.len : sizeof sbuf - 1;
            memcpy(sbuf, session.ptr, n);
            sbuf[n] = '\0';
            if (strtoull(sbuf, NULL, 10) != self->session_id) {
                smolrtsp_respond(ctx, SMOLRTSP_STATUS_SESSION_NOT_FOUND, "Session Not Found");
                return;
            }
        }
        if (!CharSlice99_is_empty(req->body)) {
            smolrtsp_respond(ctx, SMOLRTSP_STATUS_PARAMETER_NOT_UNDERSTOOD, "Parameter Not Understood");
            return;
        }
        respond_session_header(self, ctx);
        smolrtsp_respond_ok(ctx);
    } else if (SmolRTSP_Method_eq(&method, &set_param)) {
        smolrtsp_respond(ctx, SMOLRTSP_STATUS_NOT_IMPLEMENTED, "Not Implemented");
    } else {
        smolrtsp_respond(ctx, SMOLRTSP_STATUS_METHOD_NOT_ALLOWED, "Method Not Allowed");
    }
}

/* Optional Digest authentication (rtsp_auth.h): everything but OPTIONS needs credentials once the credentials
 * file exists. Neither the header nor the user name is logged. */
static SmolRTSP_ControlFlow Client_before(VSelf, SmolRTSP_Context *ctx, const SmolRTSP_Request *req)
{
    VSELF(Client);
    const char *method = CharSlice99_alloca_c_str(req->start_line.method);
    char uri[256];
    CharSlice99 auth = CharSlice99_empty();
    bool have_auth;
    rtsp_auth_result_t r;

    rtsp_auth_redact_uri(CharSlice99_alloca_c_str(req->start_line.uri), uri, sizeof uri);
    a2k_log("%s %s CSeq=%u", method, uri, req->cseq);
    (void)rtsp_auth_db_refresh(&g_auth);
    have_auth = find_header_ci(&req->header_map, "Authorization", &auth);
    rtsp_auth_check(&g_auth, &self->auth, method, have_auth ? auth.ptr : NULL, have_auth ? auth.len : 0,
                    rtp_mono_us(), &r);
    if (r.log[0]) {
        char ip[INET6_ADDRSTRLEN];
        a2k_log("%s (client %s)", r.log, client_ip(self, ip, sizeof ip));
    }
    if (r.verdict == RTSP_AUTH_ALLOW)
        return SmolRTSP_ControlFlow_Continue;
    if (r.delay_ms)
        poll(NULL, 0, (int)r.delay_ms); /* this client's own thread: slows only the one that is guessing */
    if (r.challenge[0])
        smolrtsp_header(ctx, SMOLRTSP_HEADER_WWW_AUTHENTICATE, "%s", r.challenge);
    smolrtsp_respond(ctx, SMOLRTSP_STATUS_UNAUTHORIZED, "Unauthorized");
    if (r.verdict == RTSP_AUTH_DROP && !self->drop_reason)
        self->drop_reason = "RTSP authentication refused";
    return SmolRTSP_ControlFlow_Break;
}

static void Client_after(VSelf, ssize_t ret, SmolRTSP_Context *ctx, const SmolRTSP_Request *req)
{
    VSELF(Client);
    (void)ctx;
    (void)req;
    if (ret < 0)
        self->tx_fail = true;
}

impl(SmolRTSP_Controller, Client);

/* ---- H.264 packetisation (same walk as the 1080p server: SmolRTSP FU-A fragmentation, marker on the last VCL NAL) -- */
enum { SEND_OK = 0, SEND_NOBUFS = 1, SEND_FAIL = -1 };

static int send_one_nalu(Client *client, const uint8_t *data, size_t nalu_len, uint32_t timestamp, bool is_vcl)
{
    SmolRTSP_H264NalHeader h264_hdr = SmolRTSP_H264NalHeader_parse(data[0]);
    SmolRTSP_NalUnit nalu = {
        .header = SmolRTSP_NalHeader_H264(h264_hdr),
        .payload = U8Slice99_new((uint8_t *)data + 1, nalu_len - 1),
    };
    if (SmolRTSP_NalTransport_send_packet(client->nal_transport, SmolRTSP_RtpTimestamp_Raw(timestamp), is_vcl, nalu) ==
        -1) {
        int e = errno;
        if (e == ENOBUFS)
            return SEND_NOBUFS;
        if (e != EINTR) {
            client->tx_fail = true;
            return SEND_FAIL;
        }
    }
    return SEND_OK;
}

typedef struct {
    Client *client;
    uint32_t timestamp;
} nalu_ctx_t;

static int nalu_cb(void *user, const uint8_t *nalu, size_t len, int is_vcl)
{
    const nalu_ctx_t *c = user;
    return send_one_nalu(c->client, nalu, len, c->timestamp, is_vcl != 0);
}

static int send_frame_nalus(Client *client, const uint8_t *data, size_t len, uint32_t timestamp)
{
    nalu_ctx_t c = { client, timestamp };
    return h264_annexb_each(data, len, nalu_cb, &c);
}

/* One frame for one client: pacing (drop for this client only), start on a keyframe, then packetise. */
static void send_video_frame(Client *client, const a2k_frame_t *f)
{
    a2k_stream_t *s = client->stream;
    /* PTZ privacy (ptz_privacy=1): no video while the camera is parked; the session stays up. Leaving the parked state, this client
     * resumes on the next keyframe, which is requested at once. */
    if (a2k_ptz_privacy_active()) {
        client->privacy = true;
        client->need_idr = true;
        client->idr_wait_frames = 0;
        return;
    }
    if (client->privacy) {
        client->privacy = false;
        a2k_stream_want_idr(s);
    }
    uint32_t rtp_ts = rtp_ticks_from_us(f->capture_us, client->video_ts_base_us, VIDEO_CLOCK_RATE);
    bool send_it = true, log_recovered = false;
    size_t pace_backlog = 0;
    int in_band = 0;
    tx_pace_event_t ev;
    size_t kq = tx_pace_backlog(client->tx.fd);
    size_t kcap = tx_pace_cap(tx_pace_sndbuf(client->tx.fd));
    size_t qcap = net_tx_queue_cap(&client->tx);
    size_t pending = net_tx_pending(&client->tx);
    size_t backlog = kq + pending;
    size_t kfree = pending ? 0 : (kcap > kq ? kcap - kq : 0);
    size_t room = qcap ? net_tx_room(&client->tx) + kfree : SIZE_MAX;
    tx_pace_cfg_t pcfg;
    tx_pace_verdict_t pv;

    if (f->key)
        in_band = h264_params_scan(&(h264_params_t){ 0 }, f->data, f->len, 256);

    pace_backlog = backlog;
    pcfg.base = tx_pace_base(s->kbps);
    pcfg.cap = kcap ? kcap + qcap : 0;
    pcfg.stall_ms = TX_PACE_STALL_MS;
    pcfg.max_skip_ms = tx_pace_max_skip_ms(s->gop / (s->fps ? s->fps : 1));
    pcfg.max_lag_ms = TX_PACE_MAX_LAG_MS;
    pcfg.rate_bps = (size_t)(s->kbps > 0 ? s->kbps : 1000) * 125u;
    pv = tx_pace_decide(&client->pace, &pcfg, backlog, f->len, room, f->key, ms_now(), &ev);
    if (pv != TX_PACE_SEND) {
        send_it = false;
        client->skipped++;
    }
    if (pv == TX_PACE_DROP_CLIENT && !client->drop_reason)
        client->drop_reason = ev.drop_reason;
    if (ev.log_enter && ev.entered)
        a2k_log("%s: client link slow (%zu bytes unsent%s): dropping video until the next keyframe", s->name, backlog,
                ev.lag ? ", backlog standing too long" : "");
    log_recovered = ev.log_leave && ev.left && pv == TX_PACE_SEND;
    if (log_recovered)
        client->pace_recovered = ev.run_skipped;

    if (send_it && client->need_idr) {
        if (f->key) {
            h264_params_t cached;
            client->need_idr = false;
            a2k_stream_params(s, &cached);
            if (in_band != (H264_FOUND_SPS | H264_FOUND_PPS) && h264_params_ready(&cached)) {
                if (send_one_nalu(client, cached.sps, cached.sps_len, rtp_ts, false) == SEND_OK)
                    (void)send_one_nalu(client, cached.pps, cached.pps_len, rtp_ts, false);
            }
        } else if (++client->idr_wait_frames > s->gop * 2) {
            a2k_log("%s: no keyframe within %d frames, starting anyway", s->name, client->idr_wait_frames);
            client->need_idr = false;
        } else {
            send_it = false;
        }
    }
    if (send_it && rtp_astart_holding(&client->audio_start))
        rtp_astart_release(&client->audio_start, f->capture_us); /* audio starts from this frame's capture time */
    if (send_it && !client->tx_fail) {
        int r = send_frame_nalus(client, f->data, f->len, rtp_ts);
        if (r == SEND_NOBUFS) {
            /* The queue refused a packet in the middle of the frame (only whole packets are queued, the stream is
             * intact, the picture is not): skip to the next keyframe like for a backlog over the high mark. */
            memset(&ev, 0, sizeof ev);
            (void)tx_pace_no_room(&client->pace, pace_backlog, ms_now(), &ev);
            client->skipped++;
            if (ev.log_enter && ev.entered)
                a2k_log("%s: client link slow (TCP send queue full): dropping video until the next keyframe",
                        s->name);
        } else if (r == SEND_OK) {
            client->sent++;
            lat_stat_add(&client->lat, rtp_mono_us() - f->rx_us, backlog, ms_now());
            if (log_recovered)
                a2k_log("%s: client link recovered: dropped %u frames, video resumed", s->name,
                        client->pace_recovered);
        }
    }
}

/* Every frame published since the last pass (at most the ring), in order. */
static void pump_video(Client *c)
{
    int n;
    unsigned long ln;
    long lavg, lmax;
    size_t lbl;
    if (lat_stat_take(&c->lat, ms_now(), &ln, &lavg, &lmax, &lbl))
        a2k_log("%s: frame age at send avg %ld ms max %ld ms over %lu frames, unsent backlog max %zu bytes",
                c->stream->name, lavg, lmax, ln, lbl);
    for (n = 0; n < A2K_RING + 1; n++) {
        int lost = 0;
        unsigned dropped = 0;
        a2k_frame_t *f;
        /* Behind by more than A2K_MAX_AGE_MS (slow link, slow reader): skip to the newest recent keyframe, or drop
         * the lot and wait for a new one, instead of playing the backlog late. */
        int cu = a2k_stream_catchup(c->stream, &c->cursor, rtp_mono_us(), (int64_t)A2K_MAX_AGE_MS * 1000, &dropped);
        if (cu) {
            c->need_idr = true;
            c->idr_wait_frames = 0;
            c->skipped += dropped;
            if (cu == 2)
                a2k_stream_want_idr(c->stream);
            if (c->audio_rtp_transport)
                rtp_astart_arm(&c->audio_start);
            a2k_log("%s: client %u frames behind (older than %d ms), %s", c->stream->name, dropped, A2K_MAX_AGE_MS,
                    cu == 1 ? "skipping to the newest keyframe" : "dropping to the next keyframe");
        }
        f = a2k_stream_next(c->stream, &c->cursor, &lost);
        if (lost && !c->need_idr) {
            /* Frames this client never took were overwritten: its P-frame references are gone. */
            c->need_idr = true;
            c->idr_wait_frames = 0;
            a2k_stream_want_idr(c->stream);
            a2k_log("%s: client fell behind the frame ring, resuming at the next keyframe", c->stream->name);
        }
        if (!f)
            return;
        send_video_frame(c, f);
        a2k_frame_unref(c->stream, f);
        if (c->drop_reason || c->tx_fail)
            return;
    }
}

/* ---- Audio: L16/8000 as plain RTP (no NAL framing), interleaved like the video -------------------------------------
 * Same logic as send_audio_frame() of platforms/anyka-ak3918/rtsp_session.c. Timestamp: a per-session random origin + the capture-stream
 * sample index (exact: no gap or overlap even if the ring dropped periods, the index then jumps by the dropped amount).
 * Wall-clock time never enters the packet timestamps (the 1080p history: stamping audio with CLOCK_MONOTONIC while the
 * video used the encoder's clock broke VLC's A/V sync); it only goes into the RTCP Sender Reports below. */
static void send_audio_period(Client *client, const int16_t *pcm, size_t n, uint64_t first_sample, int64_t t_end_us)
{
    uint8_t net[A2K_APERIOD_SAMPLES * 2];
    uint32_t ts;

    rtp_aclock_observe(&client->audio_clock, first_sample + n, t_end_us, A2K_AUDIO_RATE);
    /* A client whose link is backed up (video is being dropped): drop this audio period too instead of adding to the
     * queue. It was consumed and the timestamp is the sample index, so the stream keeps its place. */
    if (client->pace.skipping)
        return;
    a2k_audio_pack_be(net, pcm, n);
    ts = a2k_audio_rtp_ts(client->audio_ts_base, first_sample);
    if (SmolRTSP_RtpTransport_send_packet(client->audio_rtp_transport, SmolRTSP_RtpTimestamp_Raw(ts), true,
                                          U8Slice99_empty(), U8Slice99_new(net, n * sizeof(uint16_t))) == -1) {
        int e = errno;
        /* ENOBUFS (the TCP queue is full): this packet is dropped, the client stays. */
        if (e != ENOBUFS && e != EINTR)
            client->tx_fail = true;
    }
}

static void pump_audio(Client *c)
{
    int n;
    if (!c->audio_rtp_transport || c->tx_fail)
        return;
    for (n = 0; n < AUDIO_BURST; n++) {
        int16_t pcm[A2K_APERIOD_SAMPLES];
        size_t ns = 0;
        uint64_t first = 0;
        int64_t t_end = 0;
        int lost = 0;
        if (rtp_astart_holding(&c->audio_start))
            return; /* no video frame yet: the periods stay in the ring (stale ones are dropped once it is released) */
        if (!a2k_aring_next(a2k_audio_ring(), &c->audio_cursor, pcm, &ns, &first, &t_end, &lost))
            return;
        if (rtp_astart_stale(&c->audio_start, t_end))
            continue; /* captured before the first video frame: backlog */
        /* Half duplex (audio.talk_duplex): while speech of the backchannel plays, the microphone is replaced by silence; the packet and its
         * timestamp go on as usual. */
        (void)talk_gate_mic(talk_global(), pcm, ns, rtp_mono_us());
        send_audio_period(c, pcm, ns, first, t_end);
        if (c->tx_fail)
            return;
    }
}

/* ---- RTCP Sender Reports (only for a session that has the audio track) --------------------------------------------
 * Same reasoning as the 1080p server: VLC/live555 gives up on audio without any SR ("Timestamp conversion failed"), and
 * the SR pairs ONE instant of the monotonic clock with both tracks' RTP timestamps so that the receiver lines them up.
 * A video-only session sends none, exactly as before. */
static void send_one_rtcp_sr(Client *client, SmolRTSP_RtpTransport *transport, uint8_t tcp_channel, uint32_t rtp_ts,
                             int64_t real_us)
{
    uint8_t frame[RTCP_SR_FRAME_SIZE];

    /* RFC 2326 section 10.12: '$', channel, 16-bit length, packet. */
    if (rtcp_sr_frame(frame, tcp_channel, SmolRTSP_RtpTransport_ssrc(transport), rtp_ts, SmolRTSP_RtpTransport_pkt_count(transport),
                      SmolRTSP_RtpTransport_octet_count(transport), real_us) == 0)
        return;
    if (net_tx_write_all(&client->tx, frame, sizeof frame) < 0 && errno != ENOBUFS)
        client->tx_fail = true; /* ENOBUFS: this report is dropped, the next one is in 5 s */
}

static void send_rtcp_reports(Client *client)
{
    int64_t now, real;
    const int64_t interval = (int64_t)RTCP_SR_INTERVAL_SEC * 1000000;

    if (!client->audio_rtp_transport || client->tx_fail)
        return;
    now = rtp_mono_us();
    real = rtp_realtime_us();
    if (client->rtp_transport && SmolRTSP_RtpTransport_pkt_count(client->rtp_transport) > 0 &&
        now >= client->next_sr_video_us) {
        client->next_sr_video_us = now + interval;
        send_one_rtcp_sr(client, client->rtp_transport, client->rtcp_channel,
                         rtp_ticks_from_us(now, client->video_ts_base_us, VIDEO_CLOCK_RATE), real);
    }
    if (!client->tx_fail && client->audio_clock.init &&
        SmolRTSP_RtpTransport_pkt_count(client->audio_rtp_transport) > 0 && now >= client->next_sr_audio_us) {
        client->next_sr_audio_us = now + interval;
        send_one_rtcp_sr(client, client->audio_rtp_transport, client->audio_rtcp_channel,
                         a2k_audio_rtp_ts(client->audio_ts_base,
                                          rtp_aclock_samples_at(&client->audio_clock, now, A2K_AUDIO_RATE)),
                         real);
    }
}

/* ---- RTSP/TCP writer over net_tx (same bytes as smolrtsp_fd_writer, bounded stalls) --------------------------- */
typedef net_tx_t TxWriter;

static ssize_t TxWriter_write(VSelf, CharSlice99 data)
{
    VSELF(TxWriter);
    if (net_tx_write_all(self, data.ptr, data.len) < 0)
        return -1;
    return (ssize_t)data.len;
}

static void TxWriter_lock(VSelf)
{
    VSELF(TxWriter);
    (void)self;
}
static void TxWriter_unlock(VSelf)
{
    VSELF(TxWriter);
    (void)self;
}
static size_t TxWriter_filled(VSelf)
{
    VSELF(TxWriter);
    (void)self;
    return 0;
}

static int TxWriter_vwritef(VSelf, const char *restrict fmt, va_list ap)
{
    VSELF(TxWriter);
    char small[512];
    va_list ap2;
    int n, ret;
    va_copy(ap2, ap);
    n = vsnprintf(small, sizeof small, fmt, ap);
    ret = n;
    if (n >= 0 && (size_t)n < sizeof small) {
        if (net_tx_write_all(self, small, (size_t)n) < 0)
            ret = -1;
    } else if (n >= 0) {
        char *big = malloc((size_t)n + 1);
        if (!big) {
            ret = -1;
        } else {
            (void)vsnprintf(big, (size_t)n + 1, fmt, ap2);
            if (net_tx_write_all(self, big, (size_t)n) < 0)
                ret = -1;
            free(big);
        }
    }
    va_end(ap2);
    return ret;
}

static int TxWriter_writef(VSelf, const char *restrict fmt, ...)
{
    VSELF(TxWriter);
    va_list ap;
    int ret;
    va_start(ap, fmt);
    ret = TxWriter_vwritef(self, fmt, ap);
    va_end(ap);
    return ret;
}

impl(SmolRTSP_Writer, TxWriter);

/* ---- input: rtsp_in frames the byte stream ------------------------------------------------------------------- */
typedef struct {
    Client *client;
    SmolRTSP_Controller *controller;
    SmolRTSP_Writer *writer;
} InCtx;

static void send_error_reply(Client *client, int code, const char *reason, const char *msg, size_t len)
{
    unsigned long cseq = 0;
    char out[160];
    int n;
    if (rtsp_in_header_uint(msg, len, "CSeq", &cseq) != 1)
        cseq = 0;
    n = snprintf(out, sizeof out, "RTSP/1.0 %d %s\r\nCSeq: %lu\r\n\r\n", code, reason, cseq);
    if (n > 0 && net_tx_write_all(&client->tx, out, (size_t)n) < 0)
        client->tx_fail = true;
}

static int in_on_message(void *user, const char *data, size_t len)
{
    InCtx *c = user;
    SmolRTSP_Request req = SmolRTSP_Request_uninit();
    SmolRTSP_ParseResult res;
    bool ok = false;

    rtsp_idle_request(&c->client->idle, rtp_mono_us());
    res = SmolRTSP_Request_parse(&req, CharSlice99_new((char *)data, len));
    match(res)
    {
        of(SmolRTSP_ParseResult_Success, status)
        {
            match(*status)
            {
                of(SmolRTSP_ParseStatus_Complete, offset) { ok = (*offset == len); }
                otherwise {}
            }
        }
        of(SmolRTSP_ParseResult_Failure, err) { (void)err; }
    }
    if (ok) {
        smolrtsp_dispatch(*c->writer, *c->controller, &req);
        if (c->client->bc_playing && !c->client->bc_noted) {
            c->client->bc_noted = true; /* PLAY of the backchannel: start of its idle clock */
            rtsp_idle_talk_activity(&c->client->idle, rtp_mono_us());
        }
    } else {
        a2k_log("request parse failed (%zu bytes), answering 400", len);
        send_error_reply(c->client, 400, "Bad Request", data, len);
    }
    return c->client->drop_reason != NULL || c->client->tx_fail;
}

/* Players send RTCP receiver reports on the interleaved RTCP channel; they need no answer, only a BYE matters. */
static int in_on_interleaved(void *user, uint8_t channel, const uint8_t *data, size_t len)
{
    InCtx *c = user;
    if (c->client->bc_setup) {
        /* The backchannel's channel pair is routed first: its RTP is audio for the speaker, its RTCP is only watched for a BYE. */
        if (channel == c->client->bc_rtp_ch) {
            if (c->client->bc_playing) {
                int64_t now = rtp_mono_us();
                talk_rtp(talk_global(), data, len, now);
                rtsp_idle_talk_activity(&c->client->idle, now);
            }
            return 0;
        }
        if (channel == c->client->bc_rtcp_ch) {
            if (rtsp_rtcp_scan(data, len, NULL)) {
                bc_stop(c->client);
                if (!c->client->is_playing) {
                    c->client->drop_reason = "RTCP BYE from client";
                    return 1;
                }
            }
            return 0;
        }
    }
    if (((c->client->nal_transport && channel == c->client->rtcp_channel) ||
         (c->client->audio_rtp_transport && channel == c->client->audio_rtcp_channel)) &&
        rtsp_rtcp_scan(data, len, NULL)) {
        c->client->drop_reason = "RTCP BYE from client";
        return 1;
    }
    return 0;
}

static void set_nonblock(int fd)
{
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0)
        (void)fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

/* ---- the client thread --------------------------------------------------------------------------------------- */
static void *client_thread(void *arg)
{
    Client *client = arg;
    int fd = client->sock;
    char ip[INET6_ADDRSTRLEN];
    SmolRTSP_Controller controller = DYN(Client, SmolRTSP_Controller, client);
    SmolRTSP_Writer writer = DYN(TxWriter, SmolRTSP_Writer, &client->tx);
    InCtx in_ctx = { client, &controller, &writer };
    const rtsp_in_ops_t in_ops = { &in_ctx, in_on_message, in_on_interleaved };
    int64_t next_auth_check_us = 0;
    static const char *const names[] = { "disabled", "enabled (Digest)", "INVALID credentials file, refusing all" };

    net_tx_init(&client->tx, fd, TX_STALL_BUDGET_MS);
    rtsp_in_init(&client->in);
    rtsp_idle_init(&client->idle, rtp_mono_us());
    rtsp_auth_conn_init(&client->auth);
    client->stream_idx = -1;
    a2k_log("client %s connected, RTSP authentication %s", client_ip(client, ip, sizeof ip),
            names[rtsp_auth_db_refresh(&g_auth)]);

    while (!a2k_stop) {
        struct pollfd pfds[2];
        int wait_ms, poll_ret;

        pfds[0].fd = fd;
        pfds[0].events = POLLIN | (net_tx_pending(&client->tx) ? POLLOUT : 0);
        pfds[0].revents = 0;
        pfds[1].fd = client->wake[0];
        pfds[1].events = POLLIN;
        pfds[1].revents = 0;
        if (client->is_playing)
            wait_ms = POLL_MAX_MS;
        else if (client->bc_playing) /* a talk-only session wakes at least every 100 ms for the tail flush and its own idle clock */
            wait_ms = rtsp_idle_talk_wait_ms(&client->idle, rtp_mono_us(), 100);
        else
            wait_ms = rtsp_idle_wait_ms(&client->idle, 0, rtp_mono_us(), POLL_MAX_MS);
        poll_ret = poll(pfds, 2, wait_ms);
        if (poll_ret < 0) {
            if (errno == EINTR)
                continue;
            client->drop_reason = "poll error";
            break;
        }
        if (poll_ret > 0 && (pfds[1].revents & POLLIN)) {
            char junk[64];
            while (read(client->wake[0], junk, sizeof junk) > 0) {
            }
        }

        if (net_tx_pending(&client->tx)) {
            if (net_tx_pump(&client->tx) < 0)
                client->tx_fail = true;
            else if (net_tx_queue_stalled_ms(&client->tx) >= TX_STALL_BUDGET_MS)
                client->tx_fail = true;
            if (client->tx_fail && !client->drop_reason)
                client->drop_reason = "TCP write failed or stalled";
            if (client->drop_reason)
                break;
        }

        if (poll_ret > 0 && (pfds[0].revents & ~POLLOUT) != 0) {
            size_t cap;
            char *dst = rtsp_in_space(&client->in, &cap);
            ssize_t n = cap ? recv(fd, dst, cap, 0) : 0;
            if (cap == 0) {
                client->drop_reason = "input buffer full";
            } else if (n < 0) {
                if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
                    client->drop_reason = "recv error";
            } else if (n == 0) {
                client->drop_reason = "client closed the connection";
            } else {
                rtsp_in_commit(&client->in, (size_t)n);
                switch (rtsp_in_pump(&client->in, &in_ops)) {
                case RTSP_IN_PUMP_OVERFLOW: {
                    size_t dl;
                    const char *d = rtsp_in_data(&client->in, &dl);
                    send_error_reply(client, 413, "Request Entity Too Large", d, dl);
                    client->drop_reason = "RTSP request larger than the input buffer";
                    break;
                }
                case RTSP_IN_PUMP_GARBAGE:
                    client->drop_reason = "invalid input (neither RTSP nor interleaved frame)";
                    break;
                case RTSP_IN_PUMP_OK:
                case RTSP_IN_PUMP_STOPPED:
                    break;
                }
            }
            if (client->tx_fail && !client->drop_reason)
                client->drop_reason = "TCP write failed or stalled";
            if (client->drop_reason)
                break;
        }

        if (client->bc_playing) {
            /* Talk needs the password on and the speaker usable: switching either off (or audio.talk_enable 0) ends the talk; a video
             * stream of the same connection stays. */
            if (rtp_mono_us() >= next_auth_check_us) {
                next_auth_check_us = rtp_mono_us() + 1000000;
                if (!rtsp_auth_session_ok(&g_auth, &client->auth) || rtsp_auth_db_mode(&g_auth) != RTSP_AUTH_ON) {
                    client->drop_reason = "RTSP credentials changed or authentication enabled";
                    break;
                }
            }
            if (!audio_out_talk_available()) {
                a2k_log("backchannel: talk switched off or speaker unavailable, ending it");
                bc_stop(client);
                if (!client->is_playing) {
                    client->drop_reason = "talk is no longer available";
                    break;
                }
            } else {
                talk_tick(talk_global(), rtp_mono_us());
            }
        }
        if (!client->is_playing) {
            /* A talk-only session has its own clock: 30 s without audio. */
            const char *why = client->bc_playing ? rtsp_idle_check_talk(&client->idle, rtp_mono_us())
                                                 : rtsp_idle_check(&client->idle, 0, rtp_mono_us());
            if (why) {
                client->drop_reason = why;
                break;
            }
            continue;
        }

        /* Authentication switched on or other credentials since this stream started: log in again. */
        if (rtp_mono_us() >= next_auth_check_us) {
            next_auth_check_us = rtp_mono_us() + 1000000;
            if (!rtsp_auth_session_ok(&g_auth, &client->auth)) {
                client->drop_reason = "RTSP credentials changed or authentication enabled";
                break;
            }
        }
        if (client->nal_transport)
            pump_video(client);
        if (client->drop_reason)
            break;
        pump_audio(client);
        send_rtcp_reports(client);
        {
            int stall_ms = net_tx_take_stall(&client->tx);
            if (client->nal_transport && stall_ms >= TX_RESYNC_STALL_MS && !client->need_idr) {
                a2k_log("%s: client %s slow: TCP write stalled %d ms, resuming video at the next keyframe",
                        client->stream->name, client_ip(client, ip, sizeof ip), stall_ms);
                client->need_idr = true;
                client->idr_wait_frames = 0;
                a2k_stream_want_idr(client->stream);
                if (client->audio_rtp_transport)
                    rtp_astart_arm(&client->audio_start); /* audio restarts with the video, not from the backlog */
            }
        }
        if (client->tx_fail) {
            client->drop_reason = "TCP write failed or stalled";
            break;
        }
    }

    a2k_log("dropping client %s: %s (%lu frames sent, %lu skipped)", client_ip(client, ip, sizeof ip),
            client->drop_reason ? client->drop_reason : "server stopping", client->sent, client->skipped);
    if (client->wake_on)
        a2k_stream_wake_del(client->stream, client->wake[1]);
    if (client->audio_wake_on)
        a2k_aring_wake_del(a2k_audio_ring(), client->wake[1]);
    if (client->drop_reason && !strcmp(client->drop_reason, "TEARDOWN") && !client->tx_fail) {
        int64_t end_us = rtp_mono_us() + (int64_t)TEARDOWN_FLUSH_MS * 1000;
        while (net_tx_pending(&client->tx) > 0 && net_tx_pump(&client->tx) == 0 && net_tx_pending(&client->tx) > 0) {
            int64_t left_ms = (end_us - rtp_mono_us()) / 1000;
            struct pollfd po;
            if (left_ms <= 0)
                break;
            po.fd = fd;
            po.events = POLLOUT;
            po.revents = 0;
            (void)poll(&po, 1, (int)left_ms);
        }
    }
    VCALL(DYN(Client, SmolRTSP_Droppable, client), drop);
    net_tx_queue_free(&client->tx);
    close(fd);
    close(client->wake[0]);
    close(client->wake[1]);
    free(client);
    pthread_mutex_lock(&g_cl_mu);
    g_nclients--;
    pthread_cond_broadcast(&g_cl_cv);
    pthread_mutex_unlock(&g_cl_mu);
    return NULL;
}

/* ---- accept ----------------------------------------------------------------------------------------------- */
static void refuse_busy(int fd)
{
    /* Not rtsp_busy_reply(): its text says "one viewer at a time", which is the 1080p policy, not this one. */
    static const char busy[] = "RTSP/1.0 503 Service Unavailable\r\nRetry-After: 10\r\nContent-Length: 0\r\n"
                               "Connection: close\r\n\r\n";
    if (write(fd, busy, sizeof busy - 1) < 0) {
    }
    close(fd);
}

static void tune_client_socket(int fd)
{
    struct timeval snd_to = { 1, 0 };
    int on = 1, sndbuf = TX_PACE_SNDBUF, ka_idle = 30, ka_intvl = 10, ka_cnt = 3;
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &snd_to, sizeof snd_to);
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof on);
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof sndbuf);
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof on);
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &ka_idle, sizeof ka_idle);
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &ka_intvl, sizeof ka_intvl);
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &ka_cnt, sizeof ka_cnt);
}

static void spawn_client(int fd, const struct sockaddr_storage *sa, socklen_t sl)
{
    Client *c;
    pthread_t th;
    pthread_attr_t at;
    int admitted;

    pthread_mutex_lock(&g_cl_mu);
    admitted = g_nclients < A2K_RTSP_MAX_CLIENTS;
    if (admitted)
        g_nclients++;
    pthread_mutex_unlock(&g_cl_mu);
    if (!admitted) {
        a2k_log("too many clients (%d), answering 503", A2K_RTSP_MAX_CLIENTS);
        refuse_busy(fd);
        return;
    }
    c = calloc(1, sizeof *c);
    if (!c || pipe(c->wake) < 0) {
        free(c);
        goto fail;
    }
    set_nonblock(c->wake[0]);
    set_nonblock(c->wake[1]);
    tune_client_socket(fd);
    c->sock = fd;
    memcpy(&c->addr, sa, sl);
    c->addr_len = sl;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 128 * 1024);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&th, &at, client_thread, c) != 0) {
        pthread_attr_destroy(&at);
        close(c->wake[0]);
        close(c->wake[1]);
        free(c);
        goto fail;
    }
    pthread_attr_destroy(&at);
    return;
fail:
    pthread_mutex_lock(&g_cl_mu);
    g_nclients--;
    pthread_mutex_unlock(&g_cl_mu);
    close(fd);
}

static void *accept_thread(void *arg)
{
    (void)arg;
    srand((unsigned)(rtp_mono_us() ^ ((int64_t)getpid() << 16) ^ (int64_t)time(NULL)));
    while (!a2k_stop) {
        struct pollfd p = { .fd = g_listen, .events = POLLIN, .revents = 0 };
        int r = poll(&p, 1, 200);
        if (r > 0 && (p.revents & POLLIN)) {
            struct sockaddr_storage sa;
            socklen_t sl = sizeof sa;
            int fd = accept(g_listen, (struct sockaddr *)&sa, &sl);
            if (fd >= 0)
                spawn_client(fd, &sa, sl);
        }
    }
    return NULL;
}

rtsp_auth_db_t *a2k_rtsp_auth_db(void) { return &g_auth; }

int a2k_rtsp_start(int port, const char *auth_path, a2k_stream_t *streams[2])
{
    struct sockaddr_in a;
    int on = 1;

    g_streams[0] = streams[0];
    g_streams[1] = streams[1];
    rtsp_auth_db_init(&g_auth, auth_path);
    g_listen = socket(AF_INET, SOCK_STREAM, 0);
    if (g_listen < 0) {
        a2k_log("RTSP: socket: %s", strerror(errno));
        return -1;
    }
    setsockopt(g_listen, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons((uint16_t)port);
    if (bind(g_listen, (struct sockaddr *)&a, sizeof a) < 0 || listen(g_listen, 8) < 0) {
        a2k_log("RTSP: cannot listen on port %d: %s", port, strerror(errno));
        close(g_listen);
        g_listen = -1;
        return -1;
    }
    if (pthread_create(&g_accept_th, NULL, accept_thread, NULL) != 0) {
        a2k_log("RTSP: cannot start the accept thread");
        close(g_listen);
        g_listen = -1;
        return -1;
    }
    g_accept_started = 1;
    a2k_log("RTSP: listening on port %d (rtsp://<ip>:%d/stream0 main, /stream1 sub), authentication file %s", port,
            port, auth_path);
    return 0;
}

int a2k_rtsp_playing(void)
{
    int n;
    pthread_mutex_lock(&g_cl_mu);
    n = g_nplaying;
    pthread_mutex_unlock(&g_cl_mu);
    return n;
}

int a2k_rtsp_clients(void)
{
    int n;
    pthread_mutex_lock(&g_cl_mu);
    n = g_nclients;
    pthread_mutex_unlock(&g_cl_mu);
    return n;
}

void a2k_rtsp_stop(void)
{
    struct timespec until;
    if (g_accept_started) {
        pthread_join(g_accept_th, NULL);
        g_accept_started = 0;
    }
    if (g_listen >= 0) {
        close(g_listen);
        g_listen = -1;
    }
    /* Client threads poll the stop flag at least every POLL_MAX_MS: wait for them, bounded. */
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += 3;
    pthread_mutex_lock(&g_cl_mu);
    while (g_nclients > 0 && pthread_cond_timedwait(&g_cl_cv, &g_cl_mu, &until) == 0) {
    }
    if (g_nclients > 0)
        a2k_log("RTSP: %d client thread(s) still running after 3 s", g_nclients);
    pthread_mutex_unlock(&g_cl_mu);
}
