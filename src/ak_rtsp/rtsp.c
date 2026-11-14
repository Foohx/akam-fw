#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <stdint.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <poll.h>
#include <stdbool.h>
#include <stdarg.h>
#include <smolrtsp.h>
#include <smolrtsp/types/rtcp.h>
#include <interface99.h>
#include <datatype99.h>

#include "ak_rtsp.h"
#include "video_cfg.h"
#include "ak_ioctls.h"
#include "audio.h"
#include "rtsp.h"
#include "h264_params.h"
#include "rtp_clock.h"
#include "net_tx.h"
#include "tx_pace.h"
#include "rtsp_in.h"
#include "rtsp_idle.h"
#include "control.h"
#include "rtsp_busy.h"
#include "rtsp_auth.h"
#include "rtsp_sdp.h"
#include "talk.h"
#include "audio_out.h"
#include "leds.h"
#include "ctl_sys.h"
#include "swdog.h"

/* Session timeout announced in the SETUP response (RFC 2326 section 12.37;
 * 60 s is also the protocol default). Clients refresh the session at about
 * half of it with GET_PARAMETER / OPTIONS; any such request is answered. The
 * server does not expire TCP sessions by itself: the socket's own keepalive
 * (handle_client()) reaps vanished peers. */
#define SESSION_TIMEOUT_SEC 60

/* Latest SPS/PPS seen in the encoder output (main thread only: filled by
 * drain_venc_ring() and send_video_frame(), read by DESCRIBE). */
static h264_params_t g_params;

/* The SPS/PPS sit at the very start of an I-frame, so only that much of the
 * (uncached) DMA buffer is inspected. */
#define PARAMS_SCAN_BYTES 256

/* Video capture clock, shared by all clients and also fed while nobody is
 * connected (drain_venc_ring()), so the learned frame period is already
 * locked when a client arrives. See rtp_clock.h for why the encoder's own
 * timestamps are not used. */
static rtp_vclock_t g_vclock;
static bool g_vclock_ready;

static int64_t video_capture_time_us(void)
{
    if (!g_vclock_ready) {
        rtp_vclock_init(&g_vclock, 1000000.0 / video_fps());
        g_vclock_ready = true;
    }
    return rtp_vclock_stamp(&g_vclock, rtp_mono_us());
}

#define VIDEO_CLOCK_RATE 90000

/* 64 random bits for the RTP timestamp origins (RFC 3550 section 5.1);
 * rand() may be only 15 bits wide, so draw until 64 are filled. */
static uint64_t rand_u64(void)
{
    uint64_t v = 0;
    for (int i = 0; i < 5; i++) v = (v << 15) ^ (uint64_t)(rand() & 0x7fff);
    return v;
}

/* -------------------------------------------------------------------------
 * Client connection context conforming to SmolRTSP_Controller interface
 * ------------------------------------------------------------------------- */
/* Private send queue of an RTSP/TCP client (net_tx.h), allocated at PLAY. The kernel buffer takes the first
 * ~240 KB of a frame; the queue takes the rest, so it only has to hold the part of the biggest keyframe
 * (the encoder is 1080p and its ring is 777600 bytes; a keyframe is about 130 KB) that does not fit the
 * socket, plus a little audio and RTCP. A frame that does not fit the room left is dropped (the room is given
 * to tx_pace_decide), never torn: net_tx queues a packet completely or not at all. */
#define TX_QUEUE_BYTES (256 * 1024)

typedef struct {
    int socket_fd;
    struct sockaddr_storage addr;
    socklen_t addr_len;

    uint64_t session_id;
    bool is_playing;
    /* is_playing is mirrored in the status LED's client count (privacy
     * indicator): only client_set_playing() changes it, so every exit path
     * leaves the count right. */
    /* Set when a write to an RTSP/TCP client failed (peer gone or stalled
     * past SO_SNDTIMEO): the interleaved stream is no longer valid, so the
     * client loop drops the connection instead of blocking on it again. */
    bool tx_fail;
    /* Why the client loop must end (logged once, then the loop exits). Set by
     * the first cause: TEARDOWN, RTCP BYE, bad input, write failure. */
    const char *drop_reason;
    /* Bounded, stall-tolerant writer for the RTSP/TCP socket (shared by the
     * RTSP responses, interleaved RTP and interleaved RTCP). */
    net_tx_t tx;
    /* RTSP/TCP backpressure (tx_pace.h): frames are dropped for this client
     * while its socket backlog is high, so the encoder keeps being drained. */
    tx_pace_t pace;

    bool is_tcp;
    uint8_t rtp_channel;
    uint8_t rtcp_channel;

    int rtp_udp_fd;
    int rtcp_udp_fd;
    uint16_t client_rtp_port;
    uint16_t client_rtcp_port;

    SmolRTSP_RtpTransport *rtp_transport;
    SmolRTSP_NalTransport *nal_transport;

    /* Audio track (streamid=1). Plain RTP, no NAL wrapping — payload is raw
     * L16 samples. NULL until SETUP is called for the audio URI; audio
     * frames are only sent once this is non-NULL. Separate UDP fds from the
     * video track since each track gets its own client_port pair. */
    SmolRTSP_RtpTransport *audio_rtp_transport;
    int audio_rtp_udp_fd;
    int audio_rtcp_udp_fd;

    /* Set by PLAY: video is held back until the next IDR frame so the
     * decoder starts on a keyframe (see send_video_frame()). */
    bool need_idr;
    int  idr_wait_frames;

    /* Interleaved channel of each track's RTCP (TCP transport). Kept per
     * track: rtcp_channel above only holds the last SETUP's value. */
    uint8_t video_rtcp_ch;
    uint8_t audio_rtcp_ch;
    unsigned pace_recovered; /* frames dropped in the SKIP run that the current frame ended (for the log) */

    /* RTP timestamp bases (RFC 3550 section 5.1: random origin per stream).
     * Video: microseconds added to the capture time before conversion to
     * 90 kHz ticks; audio: added to the capture-stream sample index. */
    uint64_t video_ts_base_us;
    uint32_t audio_ts_base;
    /* Monotonic time (us) the next Sender Report of each track is due;
     * 0 = as soon as the track has sent its first packet. */
    int64_t next_sr_video_us;
    int64_t next_sr_audio_us;
    /* Audio sample-index -> monotonic-time mapping, for the audio SR. */
    rtp_aclock_t audio_clock;
    /* Holds audio until the first video frame, then drops the ring backlog
     * captured before it (see rtp_astart_t). */
    rtp_astart_t audio_start;

    /* Digest authentication state of this connection (see rtsp_auth.h). */
    rtsp_auth_conn_t auth;

    /* ONVIF audio backchannel (streamid=2, specs/audio.md): the client
     * sends G.711 RTP to the camera. Set up by SETUP, started by PLAY, ended by
     * TEARDOWN, RTCP BYE or the end of the connection. Interleaved (TCP) or UDP. */
    bool bc_setup;
    bool bc_tcp;
    bool bc_playing;      /* a talk session is running (talk_begin() succeeded) */
    bool bc_noted;        /* the idle clock knows about bc_playing */
    uint8_t bc_rtp_ch;
    uint8_t bc_rtcp_ch;
    int bc_rtp_fd;        /* UDP: where the client's RTP arrives; -1 for TCP */
    int bc_rtcp_fd;       /* UDP: bound only so that the client's RTCP is not refused */
} Client;

/* RTSP credentials (file on the CONFIG partition); absent = no authentication. */
static rtsp_auth_db_t g_auth;

/* Client address as text, for the journal. */
static const char *client_ip(const Client *c, char *buf, size_t cap)
{
    const void *a = NULL;
    if (c->addr.ss_family == AF_INET)
        a = &((const struct sockaddr_in *)&c->addr)->sin_addr;
    else if (c->addr.ss_family == AF_INET6)
        a = &((const struct sockaddr_in6 *)&c->addr)->sin6_addr;
    if (!a || !inet_ntop(c->addr.ss_family, a, buf, (socklen_t)cap)) snprintf(buf, cap, "?");
    return buf;
}

declImpl(SmolRTSP_Controller, Client);

static void client_set_playing(Client *c, bool playing) {
    if (c->is_playing == playing) return;
    c->is_playing = playing;
    ctl_sys_note_rtsp_play(playing);
    ledsys_stream_delta(playing ? 1 : -1);
}

/* Releases the video / audio track of a session: its transport and UDP sockets. A UDP transport closes its
 * own RTP socket when it is dropped, so that descriptor is only forgotten here, never closed twice (a second
 * close() could hit a descriptor another thread has opened in the meantime). Used by SETUP, which replaces a
 * track that is already set up (a client that repeats SETUP must not leak two descriptors a time until
 * accept() fails with EMFILE), and when the client goes. */
static void release_video_track(Client *self) {
    bool had = self->nal_transport || self->rtp_transport;
    if (self->nal_transport) {
        VTABLE(SmolRTSP_NalTransport, SmolRTSP_Droppable).drop(self->nal_transport);
    } else if (self->rtp_transport) {
        VTABLE(SmolRTSP_RtpTransport, SmolRTSP_Droppable).drop(self->rtp_transport);
    }
    self->nal_transport = NULL;
    self->rtp_transport = NULL;
    if (!had && self->rtp_udp_fd >= 0) close(self->rtp_udp_fd);
    self->rtp_udp_fd = -1;
    if (self->rtcp_udp_fd >= 0) close(self->rtcp_udp_fd);
    self->rtcp_udp_fd = -1;
    self->next_sr_video_us = 0;
}

static void release_audio_track(Client *self) {
    bool had = self->audio_rtp_transport != NULL;
    if (self->audio_rtp_transport)
        VTABLE(SmolRTSP_RtpTransport, SmolRTSP_Droppable).drop(self->audio_rtp_transport);
    self->audio_rtp_transport = NULL;
    if (!had && self->audio_rtp_udp_fd >= 0) close(self->audio_rtp_udp_fd);
    self->audio_rtp_udp_fd = -1;
    if (self->audio_rtcp_udp_fd >= 0) close(self->audio_rtcp_udp_fd);
    self->audio_rtcp_udp_fd = -1;
    self->next_sr_audio_us = 0;
}

static void Client_drop(VSelf) {
    VSELF(Client);
    release_video_track(self);
    release_audio_track(self);
    if (self->bc_rtp_fd >= 0) close(self->bc_rtp_fd);
    if (self->bc_rtcp_fd >= 0) close(self->bc_rtcp_fd);
}

impl(SmolRTSP_Droppable, Client);

/* Generic per-track transport setup (used for both the video and audio
 * tracks — see setup_video_transport()/setup_audio_transport() below).
 * Creates the RtpTransport and its UDP fds (for UDP) but does NOT wrap it
 * in a NalTransport — that's video-specific and done by the caller. */
static int setup_rtp_transport(Client *self, SmolRTSP_Context *ctx, const SmolRTSP_Request *req,
                                uint8_t payload_ty, uint32_t clock_rate, bool other_track_set,
                                SmolRTSP_RtpTransport **out_transport,
                                int *out_rtp_fd, int *out_rtcp_fd,
                                uint8_t *out_rtcp_channel) {
    CharSlice99 transport_val;
    if (!SmolRTSP_HeaderMap_find(&req->header_map, SMOLRTSP_HEADER_TRANSPORT, &transport_val)) {
        smolrtsp_respond(ctx, SMOLRTSP_STATUS_BAD_REQUEST, "Transport not present");
        return -1;
    }

    SmolRTSP_TransportConfig config;
    if (smolrtsp_parse_transport(&config, transport_val) == -1) {
        smolrtsp_respond(ctx, SMOLRTSP_STATUS_BAD_REQUEST, "Malformed Transport");
        return -1;
    }

    /* One lower transport per session: is_tcp drives the pacing, the send queue and the failure policy of
     * the whole connection, so the audio track cannot be UDP while the video is interleaved (or the other way
     * round). The client may choose again with the transport of the track that is already set up. */
    if (other_track_set && self->is_tcp != (config.lower == SmolRTSP_LowerTransport_TCP)) {
        smolrtsp_respond(ctx, SMOLRTSP_STATUS_UNSUPPORTED_TRANSPORT, "Unsupported Transport");
        return -1;
    }

    if (config.lower == SmolRTSP_LowerTransport_TCP) {
        ifLet(config.interleaved, SmolRTSP_ChannelPair_Some, interleaved) {
            self->is_tcp = true;
            self->rtp_channel = interleaved->rtp_channel;
            self->rtcp_channel = interleaved->rtcp_channel;
            *out_rtcp_channel = interleaved->rtcp_channel;

            printf("[ak_rtsp] Transport: TCP interleaved (RTP ch %d, RTCP ch %d)\n",
                   (int)interleaved->rtp_channel, (int)interleaved->rtcp_channel);

            SmolRTSP_Transport t = smolrtsp_transport_tcp(
                SmolRTSP_Context_get_writer(ctx), interleaved->rtp_channel, 0);

            *out_transport = SmolRTSP_RtpTransport_new(t, payload_ty, clock_rate);

            smolrtsp_header(ctx, SMOLRTSP_HEADER_TRANSPORT,
                "RTP/AVP/TCP;unicast;interleaved=%d-%d",
                (int)interleaved->rtp_channel, (int)interleaved->rtcp_channel);
            return 0;
        }
        smolrtsp_respond(ctx, SMOLRTSP_STATUS_BAD_REQUEST, "interleaved not found");
        return -1;
    } else {
        ifLet(config.client_port, SmolRTSP_PortPair_Some, client_port) {
            self->is_tcp = false;
            self->client_rtp_port = client_port->rtp_port;
            self->client_rtcp_port = client_port->rtcp_port;

            char ip_str[64] = "unknown";
            if (self->addr.ss_family == AF_INET) {
                inet_ntop(AF_INET, &((struct sockaddr_in *)&self->addr)->sin_addr,
                          ip_str, sizeof(ip_str));
            }
            printf("[ak_rtsp] Transport: UDP client %s RTP:%u RTCP:%u\n",
                   ip_str, client_port->rtp_port, client_port->rtcp_port);

            void *ip = smolrtsp_sockaddr_ip((struct sockaddr *)&self->addr);
            *out_rtp_fd = smolrtsp_dgram_socket(self->addr.ss_family, ip,
                                                 client_port->rtp_port);
            if (*out_rtp_fd < 0) {
                printf("[ak_rtsp] Failed to create UDP RTP socket\n");
                smolrtsp_respond_internal_error(ctx);
                return -1;
            }

            *out_rtcp_fd = smolrtsp_dgram_socket(self->addr.ss_family, ip,
                                                  client_port->rtcp_port);

            struct sockaddr_storage local_addr;
            socklen_t local_len = sizeof(local_addr);
            uint16_t local_rtp_port = 0, local_rtcp_port = 0;

            if (getsockname(*out_rtp_fd, (struct sockaddr *)&local_addr, &local_len) == 0)
                local_rtp_port = ntohs(((struct sockaddr_in *)&local_addr)->sin_port);
            if (*out_rtcp_fd >= 0 &&
                getsockname(*out_rtcp_fd, (struct sockaddr *)&local_addr, &local_len) == 0)
                local_rtcp_port = ntohs(((struct sockaddr_in *)&local_addr)->sin_port);

            printf("[ak_rtsp] Local UDP: RTP:%u RTCP:%u\n", local_rtp_port, local_rtcp_port);

            SmolRTSP_Transport t = smolrtsp_transport_udp(*out_rtp_fd);
            *out_transport = SmolRTSP_RtpTransport_new(t, payload_ty, clock_rate);

            smolrtsp_header(ctx, SMOLRTSP_HEADER_TRANSPORT,
                "RTP/AVP/UDP;unicast;client_port=%u-%u;server_port=%u-%u",
                client_port->rtp_port, client_port->rtcp_port,
                local_rtp_port, local_rtcp_port);
            return 0;
        }
        smolrtsp_respond(ctx, SMOLRTSP_STATUS_BAD_REQUEST, "client_port not found");
        return -1;
    }
}

static int setup_video_transport(Client *self, SmolRTSP_Context *ctx, const SmolRTSP_Request *req) {
    if (setup_rtp_transport(self, ctx, req, 96, 90000, self->audio_rtp_transport != NULL,
                             &self->rtp_transport, &self->rtp_udp_fd, &self->rtcp_udp_fd,
                             &self->video_rtcp_ch) < 0)
        return -1;
    self->nal_transport = SmolRTSP_NalTransport_new(self->rtp_transport);
    self->video_ts_base_us = rand_u64();
    return 0;
}

static int setup_audio_transport(Client *self, SmolRTSP_Context *ctx, const SmolRTSP_Request *req) {
    if (setup_rtp_transport(self, ctx, req, 97, AUDIO_SAMPLE_RATE, self->nal_transport != NULL,
                            &self->audio_rtp_transport,
                            &self->audio_rtp_udp_fd, &self->audio_rtcp_udp_fd,
                            &self->audio_rtcp_ch) < 0)
        return -1;
    self->audio_ts_base = (uint32_t)rand_u64();
    rtp_aclock_init(&self->audio_clock);
    return 0;
}

/* Session header of a response inside an established session (RFC 2326
 * section 12.37): lets the client match a keepalive answer to its session. */
static void respond_session_header(const Client *self, SmolRTSP_Context *ctx) {
    if (self->session_id != 0)
        smolrtsp_header(ctx, SMOLRTSP_HEADER_SESSION, "%llu",
                        (unsigned long long)self->session_id);
}

/* OPTIONS doubles as a keepalive (ffmpeg, live555): answered like any other
 * request, with the Session header once a session exists. */
static void Client_options(VSelf, SmolRTSP_Context *ctx, const SmolRTSP_Request *req) {
    VSELF(Client);
    (void)req;
    smolrtsp_header(ctx, SMOLRTSP_HEADER_PUBLIC,
                    "OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN, GET_PARAMETER");
    respond_session_header(self, ctx);
    smolrtsp_respond_ok(ctx);
}

/* A request header, found whatever the case of its name (header names are
 * case-insensitive, RFC 2326; SmolRTSP_HeaderMap_find compares them exactly). */
static bool find_header_ci(const SmolRTSP_HeaderMap *m, const char *name, CharSlice99 *out) {
    size_t nl = strlen(name);
    for (size_t i = 0; i < m->len; i++) {
        CharSlice99 k = m->headers[i].key;
        if (k.len == nl && strncasecmp(k.ptr, name, nl) == 0) {
            *out = m->headers[i].value;
            return true;
        }
    }
    return false;
}

/* ------------------------------------------------------------------------
 * ONVIF audio backchannel (specs/audio.md)
 * ------------------------------------------------------------------------ */

/* The request carries "Require: www.onvif.org/ver20/backchannel". */
static bool request_requires_backchannel(const SmolRTSP_Request *req) {
    CharSlice99 v;
    return find_header_ci(&req->header_map, "Require", &v) &&
           rtsp_require_has(v.ptr, v.len, RTSP_BACKCHANNEL_TAG);
}

/* Talk is offered only with the RTSP password on (Digest, valid credentials):
 * there is no open talk mode. The speaker side must be usable as well. */
static bool backchannel_available(void) {
    return rtsp_auth_db_mode(&g_auth) == RTSP_AUTH_ON && audio_out_talk_available();
}

/* 551 (RFC 2326 section 11.3.12): the option the client requires is not
 * supported; clients that always send Require (go2rtc) retry without it. */
static void respond_backchannel_unsupported(SmolRTSP_Context *ctx) {
    smolrtsp_header(ctx, SMOLRTSP_HEADER_UNSUPPORTED, "%s", RTSP_BACKCHANNEL_TAG);
    smolrtsp_respond(ctx, SMOLRTSP_STATUS_OPTION_NOT_SUPPORTED, "Option not supported");
}

static void bc_close_udp(Client *self) {
    if (self->bc_rtp_fd >= 0) { close(self->bc_rtp_fd); self->bc_rtp_fd = -1; }
    if (self->bc_rtcp_fd >= 0) { close(self->bc_rtcp_fd); self->bc_rtcp_fd = -1; }
}

/* Ends the talk session (the speaker plays what is queued, then the amp idles
 * off). The SETUP stays: the connection may still stream video. */
static void bc_stop(Client *self) {
    if (self->bc_playing) {
        talk_end(talk_global());
        self->bc_playing = false;
        self->bc_noted = false;
    }
}

/* UDP backchannel: the client's RTP arrives on a socket of ours. The source
 * address is checked against the RTSP peer on every packet (see
 * bc_udp_receive()); the port is not, some clients send from another one. */
static int setup_bc_udp(Client *self, SmolRTSP_Context *ctx, uint16_t cl_rtp, uint16_t cl_rtcp) {
    if (self->addr.ss_family != AF_INET) {
        smolrtsp_respond(ctx, SMOLRTSP_STATUS_UNSUPPORTED_TRANSPORT, "Unsupported Transport");
        return -1;
    }
    bc_close_udp(self);
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0 || bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0) {
        if (fd >= 0) close(fd);
        smolrtsp_respond_internal_error(ctx);
        return -1;
    }
    socklen_t al = sizeof(a);
    uint16_t port = 0;
    if (getsockname(fd, (struct sockaddr *)&a, &al) == 0) port = ntohs(a.sin_port);
    self->bc_rtp_fd = fd;
    /* The RTCP socket is only here so that the client's reports do not bounce
     * (ICMP port unreachable); nothing is read from it. */
    int rfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (rfd >= 0) {
        a.sin_port = htons((uint16_t)(port + 1));
        if (bind(rfd, (struct sockaddr *)&a, sizeof(a)) < 0) { close(rfd); rfd = -1; }
    }
    self->bc_rtcp_fd = rfd;
    self->bc_tcp = false;
    printf("[ak_rtsp] Transport (backchannel): UDP client RTP:%u RTCP:%u, local RTP:%u\n",
           cl_rtp, cl_rtcp, port);
    smolrtsp_header(ctx, SMOLRTSP_HEADER_TRANSPORT,
        "RTP/AVP/UDP;unicast;client_port=%u-%u;server_port=%u-%u",
        cl_rtp, cl_rtcp, port, port + 1);
    return 0;
}

static int setup_backchannel(Client *self, SmolRTSP_Context *ctx, const SmolRTSP_Request *req) {
    if (!backchannel_available()) {
        if (request_requires_backchannel(req)) respond_backchannel_unsupported(ctx);
        else smolrtsp_respond(ctx, SMOLRTSP_STATUS_NOT_FOUND, "Not Found");
        return -1;
    }
    CharSlice99 transport_val;
    if (!SmolRTSP_HeaderMap_find(&req->header_map, SMOLRTSP_HEADER_TRANSPORT, &transport_val)) {
        smolrtsp_respond(ctx, SMOLRTSP_STATUS_BAD_REQUEST, "Transport not present");
        return -1;
    }
    SmolRTSP_TransportConfig config;
    if (smolrtsp_parse_transport(&config, transport_val) == -1) {
        smolrtsp_respond(ctx, SMOLRTSP_STATUS_BAD_REQUEST, "Malformed Transport");
        return -1;
    }

    if (config.lower == SmolRTSP_LowerTransport_TCP) {
        ifLet(config.interleaved, SmolRTSP_ChannelPair_Some, interleaved) {
            if (interleaved->rtp_channel == interleaved->rtcp_channel) {
                smolrtsp_respond(ctx, SMOLRTSP_STATUS_BAD_REQUEST, "Bad interleaved channels");
                return -1;
            }
            bc_close_udp(self);
            self->bc_tcp = true;
            self->bc_rtp_ch = interleaved->rtp_channel;
            self->bc_rtcp_ch = interleaved->rtcp_channel;
            printf("[ak_rtsp] Transport (backchannel): TCP interleaved (RTP ch %d, RTCP ch %d)\n",
                   (int)interleaved->rtp_channel, (int)interleaved->rtcp_channel);
            smolrtsp_header(ctx, SMOLRTSP_HEADER_TRANSPORT,
                "RTP/AVP/TCP;unicast;interleaved=%d-%d",
                (int)interleaved->rtp_channel, (int)interleaved->rtcp_channel);
            self->bc_setup = true;
            return 0;
        }
        smolrtsp_respond(ctx, SMOLRTSP_STATUS_BAD_REQUEST, "interleaved not found");
        return -1;
    }
    ifLet(config.client_port, SmolRTSP_PortPair_Some, client_port) {
        if (setup_bc_udp(self, ctx, client_port->rtp_port, client_port->rtcp_port) < 0) return -1;
        self->bc_setup = true;
        return 0;
    }
    smolrtsp_respond(ctx, SMOLRTSP_STATUS_BAD_REQUEST, "client_port not found");
    return -1;
}

/* The client's UDP datagrams of the backchannel, if any: bounded, from the
 * RTSP peer's address only. */
static void bc_udp_receive(Client *client, rtsp_idle_t *idle) {
    uint8_t buf[RTP_IN_MAX_PACKET + 1]; /* one byte more: an oversized datagram is seen as such */
    for (int i = 0; i < 8; i++) {
        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        ssize_t n = recvfrom(client->bc_rtp_fd, buf, sizeof(buf), MSG_DONTWAIT,
                             (struct sockaddr *)&from, &fl);
        if (n < 0) break;
        const struct sockaddr_in *peer = (const struct sockaddr_in *)&client->addr;
        if (from.sin_family != AF_INET || from.sin_addr.s_addr != peer->sin_addr.s_addr)
            continue; /* not the RTSP client */
        if (!client->bc_playing) continue;
        int64_t now = rtp_mono_us();
        talk_rtp(talk_global(), buf, (size_t)n, now);
        rtsp_idle_talk_activity(idle, now);
    }
}

static void Client_describe(VSelf, SmolRTSP_Context *ctx, const SmolRTSP_Request *req) {
    VSELF(Client);
    (void)self;
    /* ONVIF backchannel: only for a client that asks for it (Require header). */
    rtsp_bc_decision_t bc = rtsp_backchannel_decide(request_requires_backchannel(req),
                                                    rtsp_auth_db_mode(&g_auth) == RTSP_AUTH_ON,
                                                    audio_out_talk_available());
    if (bc == RTSP_BC_REFUSE) {
        respond_backchannel_unsupported(ctx);
        return;
    }
    char sdp_buf[1280];
    /* sprop-parameter-sets lets a player decode from the very first frame it
     * gets instead of waiting for in-band SPS/PPS. The cache is filled from
     * the encoder's I-frames (also while no client is connected), so it is
     * normally ready within one GOP (2 s) of boot; until then the plain
     * "packetization-mode=1" form is advertised, as before. */
    char fmtp[320];
    if (!h264_fmtp_build(&g_params, fmtp, sizeof(fmtp)))
        snprintf(fmtp, sizeof(fmtp), "packetization-mode=1");
    rtsp_sdp_params_t sp = {
        .session_time = (unsigned long long)time(NULL),
        .fmtp = fmtp,
        .audio_rate = AUDIO_SAMPLE_RATE,
        .audio_channels = AUDIO_CHANNELS,
        .backchannel = (bc == RTSP_BC_OFFER),
        .audio_off = !audio_mic_enabled(),
    };
    if (rtsp_sdp_build(sdp_buf, sizeof(sdp_buf), &sp) == 0) {
        smolrtsp_respond_internal_error(ctx);
        return;
    }

    smolrtsp_header(ctx, SMOLRTSP_HEADER_CONTENT_TYPE, "application/sdp");
    smolrtsp_body(ctx, CharSlice99_from_str(sdp_buf));
    smolrtsp_respond_ok(ctx);
}

static void Client_setup(VSelf, SmolRTSP_Context *ctx, const SmolRTSP_Request *req) {
    VSELF(Client);

    /* No SETUP once the stream runs: the transports are in use by the client loop (RFC 2326 section 10.4
     * allows it only for a transport change, which no client needs). */
    if (self->is_playing) {
        smolrtsp_respond(ctx, SMOLRTSP_STATUS_METHOD_NOT_VALID_IN_THIS_STATE, "Method Not Valid in This State");
        return;
    }

    /* Which track this SETUP is for is carried in the request URI, matching
     * the "a=control:streamid=N" values handed out in Client_describe above
     * (clients build the SETUP URI as <base>/<control-value>). */
    const char *uri = CharSlice99_alloca_c_str(req->start_line.uri);
    switch (rtsp_track_from_uri(uri)) {
    case RTSP_TRACK_BACKCHANNEL:
        if (setup_backchannel(self, ctx, req) < 0) return;
        break;
    case RTSP_TRACK_AUDIO:
        if (!audio_mic_enabled()) { /* G07: not in the SDP, so a SETUP for it is a client error */
            smolrtsp_respond(ctx, SMOLRTSP_STATUS_NOT_FOUND, "No audio track (microphone disabled)");
            return;
        }
        release_audio_track(self); /* a repeated SETUP replaces the track, it does not leak the old one */
        if (setup_audio_transport(self, ctx, req) < 0) return;
        break;
    default:
        release_video_track(self);
        if (setup_video_transport(self, ctx, req) < 0) return;
        break;
    }

    /* Session ID must stay the same across both tracks' SETUP calls within
     * one session — only generate it once. */
    if (self->session_id == 0) self->session_id = (uint64_t)rand();
    smolrtsp_header(ctx, SMOLRTSP_HEADER_SESSION, "%llu;timeout=%d",
                    (unsigned long long)self->session_id, SESSION_TIMEOUT_SEC);
    smolrtsp_respond_ok(ctx);
}

static void Client_play(VSelf, SmolRTSP_Context *ctx, const SmolRTSP_Request *req) {
    VSELF(Client);
    (void)req;
    bool media = self->nal_transport || self->audio_rtp_transport;
    if (!media && !self->bc_setup) {
        /* Nothing was SETUP: there is nothing to play (the loop would have no
         * transport to write to). */
        smolrtsp_respond(ctx, SMOLRTSP_STATUS_METHOD_NOT_VALID_IN_THIS_STATE,
                         "Method Not Valid in This State");
        return;
    }
    if (self->bc_setup && !self->bc_playing) {
        char ip[INET6_ADDRSTRLEN];
        if (backchannel_available() &&
            talk_begin(talk_global(), client_ip(self, ip, sizeof(ip))) == 0) {
            self->bc_playing = true;
        } else {
            printf("[ak_rtsp] backchannel: not started (no longer available or the speaker is busy)\n");
            if (!media) { /* a talk-only session has nothing else to offer */
                respond_backchannel_unsupported(ctx);
                return;
            }
        }
    }
    if (media) {
        client_set_playing(self, true);
        tx_pace_init(&self->pace); /* a new PLAY starts with a clean pacing state */
        if (self->is_tcp && net_tx_queue_enable(&self->tx, TX_QUEUE_BYTES) < 0)
            printf("[ak_rtsp] no memory for the TCP send queue: this client keeps blocking writes\n");
        if (self->nal_transport) {
            self->need_idr = true;
            self->idr_wait_frames = 0;
            rtp_astart_arm(&self->audio_start); /* released at the first sent frame */
        } else {
            rtp_astart_release(&self->audio_start, rtp_mono_us());
        }
    }
    smolrtsp_header(ctx, SMOLRTSP_HEADER_RANGE, "npt=now-");
    smolrtsp_respond_ok(ctx);
}

static void Client_teardown(VSelf, SmolRTSP_Context *ctx, const SmolRTSP_Request *req) {
    VSELF(Client);
    (void)req;
    client_set_playing(self, false);
    bc_stop(self);
    respond_session_header(self, ctx);
    smolrtsp_respond_ok(ctx);
    if (!self->drop_reason) self->drop_reason = "TEARDOWN";
}

/* The controller interface has no hook for GET_PARAMETER / SET_PARAMETER, so
 * they arrive here. GET_PARAMETER without a body is the RFC 2326 section
 * 10.8 keepalive (live555/VLC): 200 OK with the request's CSeq (added by the
 * context) and the Session. A foreign Session is 454; a parameter query is
 * not supported (451), nor is SET_PARAMETER (501). */
static void Client_unknown(VSelf, SmolRTSP_Context *ctx, const SmolRTSP_Request *req) {
    VSELF(Client);
    const SmolRTSP_Method method = req->start_line.method,
                          get_param = SMOLRTSP_METHOD_GET_PARAMETER,
                          set_param = SMOLRTSP_METHOD_SET_PARAMETER;

    if (SmolRTSP_Method_eq(&method, &get_param)) {
        CharSlice99 session;
        if (self->session_id != 0 &&
            SmolRTSP_HeaderMap_find(&req->header_map, SMOLRTSP_HEADER_SESSION, &session)) {
            /* "Session: <id>[;timeout=N]" */
            char sbuf[64];
            size_t n = session.len < sizeof(sbuf) - 1 ? session.len : sizeof(sbuf) - 1;
            memcpy(sbuf, session.ptr, n);
            sbuf[n] = '\0';
            if (strtoull(sbuf, NULL, 10) != self->session_id) {
                smolrtsp_respond(ctx, SMOLRTSP_STATUS_SESSION_NOT_FOUND, "Session Not Found");
                return;
            }
        }
        if (!CharSlice99_is_empty(req->body)) {
            smolrtsp_respond(ctx, SMOLRTSP_STATUS_PARAMETER_NOT_UNDERSTOOD,
                             "Parameter Not Understood");
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

/* The Authorization header, found whatever the case of its name (header names
 * are case-insensitive, RFC 2326; SmolRTSP_HeaderMap_find compares them exactly,
 * so "authorization:" would count as "no credentials" and a client that sends
 * it could never log in). */
static bool find_authorization(const SmolRTSP_HeaderMap *m, CharSlice99 *out) {
    return find_header_ci(m, "Authorization", out);
}

/* Logs the request (the URI without any user:password@ a client may have put
 * in it) and enforces the optional Digest authentication: everything except
 * OPTIONS needs valid credentials once /etc/config/akam/rtsp_auth exists. The
 * Authorization header, the credentials and the user name are never logged. */
static SmolRTSP_ControlFlow Client_before(VSelf, SmolRTSP_Context *ctx,
                                           const SmolRTSP_Request *req) {
    VSELF(Client);
    const char *method = CharSlice99_alloca_c_str(req->start_line.method);
    char uri[256];
    rtsp_auth_redact_uri(CharSlice99_alloca_c_str(req->start_line.uri), uri, sizeof(uri));
    printf("[ak_rtsp] %s %s CSeq=%u\n", method, uri, req->cseq);

    CharSlice99 auth = CharSlice99_empty();
    bool have_auth = find_authorization(&req->header_map, &auth);
    rtsp_auth_result_t r;
    rtsp_auth_check(&g_auth, &self->auth, method, have_auth ? auth.ptr : NULL,
                    have_auth ? auth.len : 0, rtp_mono_us(), &r);
    if (r.log[0]) {
        char ip[INET6_ADDRSTRLEN];
        printf("[ak_rtsp] %s (client %s)\n", r.log, client_ip(self, ip, sizeof(ip)));
    }
    if (r.verdict == RTSP_AUTH_ALLOW) return SmolRTSP_ControlFlow_Continue;

    /* Failed attempts are slowed down (single client at a time: the wait only
     * costs the connection that is guessing). */
    if (r.delay_ms) poll(NULL, 0, (int)r.delay_ms);
    if (r.challenge[0])
        smolrtsp_header(ctx, SMOLRTSP_HEADER_WWW_AUTHENTICATE, "%s", r.challenge);
    smolrtsp_respond(ctx, SMOLRTSP_STATUS_UNAUTHORIZED, "Unauthorized");
    if (r.verdict == RTSP_AUTH_DROP && !self->drop_reason)
        self->drop_reason = "RTSP authentication refused";
    return SmolRTSP_ControlFlow_Break;
}

static void Client_after(VSelf, ssize_t ret, SmolRTSP_Context *ctx,
                          const SmolRTSP_Request *req) {
    VSELF(Client);
    (void)ctx; (void)req;
    if (ret < 0) {
        printf("[ak_rtsp] Failed to respond to client\n");
        self->tx_fail = true; /* the response did not reach the peer */
    }
}

impl(SmolRTSP_Controller, Client);

/* -------------------------------------------------------------------------
 * H.264 frame packetization
 *
 * Walks Annex-B start codes, wraps each NALU in SmolRTSP_NalUnit, sets the
 * marker bit on the last VCL NALU of each access unit, and hands it to
 * SmolRTSP_NalTransport_send_packet for FU-A fragmentation if needed.
 * Timestamps: the 90 kHz RTP clock follows the frame capture time estimated
 * by g_vclock (CLOCK_MONOTONIC), NOT the encoder's own ms timestamp, which is
 * nominal (see rtp_clock.h).
 * ------------------------------------------------------------------------- */
/* Result of sending one packet or NAL unit of the video track. */
enum { SEND_OK = 0, SEND_NOBUFS = 1, SEND_FAIL = -1 };

/* Sends one NAL unit (data points at its header byte, no start code). SEND_NOBUFS: the RTSP/TCP queue was
 * full and refused a packet; it takes a packet completely or not at all, so the interleaved framing is still
 * valid and the client stays (the caller drops the rest of the frame and skips to the next keyframe).
 * SEND_FAIL: the connection is broken (tx_fail is set). UDP errors are not fatal: SEND_OK. */
static int send_one_nalu(Client *client, const uint8_t *data, size_t nalu_len,
                         uint32_t timestamp, bool is_vcl)
{
    SmolRTSP_H264NalHeader h264_hdr = SmolRTSP_H264NalHeader_parse(data[0]);
    SmolRTSP_NalUnit nalu = {
        .header  = SmolRTSP_NalHeader_H264(h264_hdr),
        .payload = U8Slice99_new((uint8_t *)data + 1, nalu_len - 1),
    };
    if (SmolRTSP_NalTransport_send_packet(
            client->nal_transport,
            SmolRTSP_RtpTimestamp_Raw(timestamp),
            is_vcl, nalu) == -1)
    {
        int e = errno; /* perror() may change it */
        if (client->is_tcp && e == ENOBUFS) return SEND_NOBUFS;
        errno = e;
        perror("send RTP/NAL");
        if (client->is_tcp && e != EINTR) {
            client->tx_fail = true;
            return SEND_FAIL;
        }
    }
    return SEND_OK;
}

/* Sends every NAL unit of a frame; stops at the first packet the connection refuses (SEND_NOBUFS) or breaks
 * on (SEND_FAIL): the rest of the frame would only be written into a dead or full stream. */
static int send_frame_nalus(Client *client, uint8_t *data, size_t len,
                            uint32_t timestamp)
{
    size_t i = 0;
    while (i + 3 < len) {
        size_t start_code_len = 0;
        if (data[i] == 0x00 && data[i+1] == 0x00 &&
            data[i+2] == 0x00 && data[i+3] == 0x01)
            start_code_len = 4;
        else if (data[i] == 0x00 && data[i+1] == 0x00 && data[i+2] == 0x01)
            start_code_len = 3;

        if (start_code_len > 0) {
            size_t nalu_start = i + start_code_len;
            size_t next_nalu_start = len;
            size_t j = nalu_start;
            while (j + 2 < len) {
                if (data[j] == 0x00 && data[j+1] == 0x00 && data[j+2] == 0x01) {
                    next_nalu_start = (j > 0 && data[j-1] == 0x00) ? j - 1 : j;
                    break;
                }
                j++;
            }

            size_t nalu_len = next_nalu_start - nalu_start;
            if (nalu_len > 0) {
                uint8_t nalu_type = data[nalu_start] & 0x1F;
                int r = send_one_nalu(client, data + nalu_start, nalu_len, timestamp,
                                      nalu_type >= 1 && nalu_type <= 5);
                if (r != SEND_OK) return r;
            }
            i = next_nalu_start;
        } else {
            i++;
        }
    }
    return SEND_OK;
}

/* Drain any frames queued in the VENC ring without sending them.
 * Called while waiting for a client in accept() so the ring doesn't wrap
 * and the next client gets a fresh stream starting near an I-frame.
 * Empties everything that is ready (bounded) in one call: the caller waits
 * in poll() between calls instead of spinning. */
#define DRAIN_MAX_FRAMES 16

void drain_venc_ring(void)
{
    for (int n = 0; n < DRAIN_MAX_FRAMES; n++) {
        struct venc_stream_req req;
        memset(&req, 0, sizeof(req));
        req.chn_id = 0;
        req.len    = venc_max_frame_size(ENC_WIDTH, ENC_HEIGHT);

        if (ioctl(g_chn0_fd, VENC_IOCTL_GET_STREAM, &req) != 0)
            return; /* nothing ready (EAGAIN) or transient error: wait again */
        swdog_frame();
        (void)video_capture_time_us(); /* keep the capture clock locked */
        /* Keep the SPS/PPS cache current for DESCRIBE (sprop-parameter-sets).
         * This is the only place the idle path dereferences the ring, so the
         * driver-supplied offset/length are range-checked first: a bad value
         * must cost one skipped scan, not a SIGSEGV of the whole streamer. */
        uint32_t off = req.frame_offset - g_venc_phys;
        if (req.frame_type_raw == FRAME_TYPE_I && req.len > 0 &&
            off < ENC_VENC_SIZE) {
            size_t avail = ENC_VENC_SIZE - off;
            size_t flen  = (size_t)req.len < avail ? (size_t)req.len : avail;
            h264_params_scan(&g_params, (const uint8_t *)g_venc_virt + off,
                             flen, PARAMS_SCAN_BYTES);
        }
        ioctl(g_chn0_fd, VENC_IOCTL_RELEASE_STREAM, &req);
    }
}

static int64_t ms_ts_now(void) { return rtp_mono_us() / 1000; }

/* Returns 1 when a frame was sent, 0 when none was ready, -1 on a fatal
 * error (the caller drops the client). */
static int send_video_frame(Client *client)
{
    struct venc_stream_req req;
    int max_len = venc_max_frame_size(ENC_WIDTH, ENC_HEIGHT);

    memset(&req, 0, sizeof(req));
    req.chn_id = 0;
    req.len    = max_len;

    if (ioctl(g_chn0_fd, VENC_IOCTL_GET_STREAM, &req) < 0) {
        if (errno == EAGAIN || errno == EFAULT || errno == EINTR) {
            return 0; /* nothing ready: the caller waits in poll() */
        }
        /* Unexpected error — drop this client, encoder keeps running */
        perror("VENC_IOCTL_GET_STREAM");
        fprintf(stderr, "[ak_rtsp] GET_STREAM errno=%d, dropping client\n", errno);
        return -1;
    }

    swdog_frame();
    swdog_mode(SWDOG_SEND); /* a blocked client write is not an encoder stall; the next loop pass resets this */
    void *data = (char *)g_venc_virt + (req.frame_offset - g_venc_phys);
    /* The encoder's req.ts_hi/ts_lo is a nominal frame-index clock (exactly
     * 66.667 ms per frame while the sensor delivers 14.67 fps): not used. */
    int64_t capture_us = video_capture_time_us();
    uint64_t ms_ts = (uint64_t)(capture_us / 1000);
    uint32_t rtp_ts = rtp_ticks_from_us(capture_us, client->video_ts_base_us,
                                        VIDEO_CLOCK_RATE);

    static int frame_count = 0;
    /* Per-frame lines only while the 8091 DEBUG mode is on (about 2 lines per 2 s otherwise, for ever):
     * every I-frame, and a heartbeat every ~10s (150 frames at 15fps) */
    if ((req.frame_type_raw == FRAME_TYPE_I || frame_count % 150 == 0) && control_debug_on())
        printf("[ak_rtsp] frame#%d type=%s len=%d ts=%u\n",
               frame_count,
               req.frame_type_raw == FRAME_TYPE_I ? "I" :
               req.frame_type_raw == FRAME_TYPE_P ? "P" : "B",
               req.len, rtp_ts);
    /* Measuring aid: the learned sensor rate and how far the capture
     * estimate trails the fetch time (~10 s apart). */
    static int64_t prev_cap_us;
    static unsigned prev_cap_frames;
    if (g_vclock.frames - prev_cap_frames >= 150) {
        if (prev_cap_us != 0 && control_debug_on())
            printf("[ak_rtsp] vclock: %.3f fps over the last %u frames, "
                   "capture estimate trails the fetch by %lld us\n",
                   (g_vclock.frames - prev_cap_frames) * 1e6 /
                       (double)(capture_us - prev_cap_us),
                   g_vclock.frames - prev_cap_frames,
                   (long long)(rtp_mono_us() - capture_us));
        prev_cap_us = capture_us;
        prev_cap_frames = g_vclock.frames;
    }
    frame_count++;

    /* Live active-bitrate log — accumulates encoded bytes over a window
     * using the smoothed capture clock (ms_ts above: real time, but without
     * the fetch jitter a briefly stalled process would add).
     *
     * Window length is 2 full GOPs, not a flat 1s: with VBR+ rate control
     * on a static/low-motion scene, I-frames are far larger than P-frames
     * (confirmed on hardware: ~134KB I-frame vs. ~7-9KB P-frames on an
     * unchanging scene) and a 1s window doesn't line up with the 2s GOP
     * cycle at ENC_GOP=30/ENC_FPS=15 — so a plain 1s average alternated
     * between an ~1850kbps "I-frame window" and an ~520kbps "P-frame-only
     * window", both individually correct but neither representative of the
     * actual sustained rate, and both looking alarmingly far from the
     * 1536kbps target ceiling on their own. Averaging over 2 full GOPs
     * guarantees at least one I-frame lands in every window, so the
     * reported number reflects the real blended rate the target ceiling is
     * actually meant to bound. */
    static uint64_t bitrate_window_start_ms = 0;
    static uint64_t bitrate_window_bytes    = 0;
    static int      bitrate_window_frames   = 0;
    const uint64_t bitrate_window_target_ms = (uint64_t)(video_gop_frames() * 2) * 1000 / video_fps();
    if (bitrate_window_start_ms == 0) bitrate_window_start_ms = ms_ts;
    bitrate_window_bytes += (uint64_t)req.len;
    bitrate_window_frames++;
    uint64_t window_elapsed_ms = ms_ts - bitrate_window_start_ms;
    if (window_elapsed_ms >= bitrate_window_target_ms) {
        double kbps = (double)(bitrate_window_bytes * 8) / (double)window_elapsed_ms;
        printf("[ak_rtsp] bitrate: %.0f kbps (%d frames, %llu bytes over %llums, ~2 GOPs)\n",
               kbps, bitrate_window_frames,
               (unsigned long long)bitrate_window_bytes,
               (unsigned long long)window_elapsed_ms);
        bitrate_window_start_ms = ms_ts;
        bitrate_window_bytes    = 0;
        bitrate_window_frames   = 0;
    }

    int in_band = 0;
    if (req.frame_type_raw == FRAME_TYPE_I)
        in_band = h264_params_scan(&g_params, (const uint8_t *)data, (size_t)req.len,
                                   PARAMS_SCAN_BYTES);

    /* Start the video on a keyframe: after PLAY, P-frames are dropped until
     * the next I-frame so the decoder never sees slices that reference a
     * missing SPS/PPS/reference picture. If the encoder never reports an
     * I-frame within two GOPs, stop waiting rather than send nothing. */
    bool send_it = true;
    bool log_recovered = false;
    size_t pace_backlog = 0;
    /* RTSP/TCP backpressure: never let a slow link hold the encoder. A
     * dropped frame is released below like any other; P-frames stay dropped
     * until a keyframe finds the socket drained (tx_pace.h). UDP has no such
     * backlog and is not paced. */
    if (client->is_tcp) {
        tx_pace_event_t ev;
        size_t kq = tx_pace_backlog(client->tx.fd);
        size_t kcap = tx_pace_cap(tx_pace_sndbuf(client->tx.fd));
        size_t qcap = net_tx_queue_cap(&client->tx);
        size_t pending = net_tx_pending(&client->tx);
        size_t backlog = kq + pending; /* what the peer has yet to take */
        pace_backlog = backlog;
        /* The loop pumped the queue just before this frame: bytes still pending mean the socket refused them
         * a moment ago, so what the counters call free kernel space is not (writes go behind the queue). */
        size_t kfree = pending ? 0 : (kcap > kq ? kcap - kq : 0);
        tx_pace_cfg_t pcfg = {
            .base = tx_pace_base(video_kbps()),
            .cap = kcap ? kcap + qcap : 0, /* the queue extends the kernel buffer */
            .stall_ms = TX_PACE_STALL_MS,
            .max_skip_ms = tx_pace_max_skip_ms(video_gop_frames() / (video_fps() ? video_fps() : 1)),
        };
        /* Room for this frame: never start one that cannot be queued completely (a part would tear the
         * interleaved stream). Without a queue the writer blocks instead and there is no limit to give. */
        size_t room = qcap ? net_tx_room(&client->tx) + kfree : SIZE_MAX;
        tx_pace_verdict_t pv = tx_pace_decide(&client->pace, &pcfg, backlog, (size_t)req.len, room,
                                              req.frame_type_raw == FRAME_TYPE_I, ms_ts_now(), &ev);
        if (pv != TX_PACE_SEND && ev.no_room && qcap) {
            /* Bigger than the kernel buffer plus the whole queue: it can never be sent, so the stream
             * stays in SKIP mode until the max-skip limit drops the client. Say so (at most every 10 s). */
            static int64_t last_big_ms;
            int64_t now_ms = ms_ts_now();
            if (tx_pace_wire_len((size_t)req.len) > qcap + kcap &&
                (last_big_ms == 0 || now_ms - last_big_ms >= TX_PACE_LOG_MS)) {
                last_big_ms = now_ms;
                printf("[ak_rtsp] frame of %d bytes cannot fit the TCP send room (%zu bytes): video is skipped\n",
                       req.len, qcap + kcap);
            }
        }
        if (pv != TX_PACE_SEND) {
            send_it = false;
            ctl_sys_note_video_skipped();
        }
        if (pv == TX_PACE_DROP_CLIENT && !client->drop_reason)
            client->drop_reason = ev.drop_reason; /* the client loop ends the session after this frame */
        if (ev.log_enter && ev.entered)
            printf("[ak_rtsp] client link slow (%zu bytes unsent): dropping video until the next keyframe\n",
                   backlog);
        log_recovered = ev.log_leave && ev.left && pv == TX_PACE_SEND;
        if (log_recovered) client->pace_recovered = ev.run_skipped;
    }
    if (send_it && client->need_idr) {
        if (req.frame_type_raw == FRAME_TYPE_I) {
            client->need_idr = false;
            /* Some encoders emit SPS/PPS only once at start-up: if this
             * I-frame does not carry them, send the cached ones first. */
            if (in_band != (H264_FOUND_SPS | H264_FOUND_PPS) && h264_params_ready(&g_params)) {
                if (send_one_nalu(client, g_params.sps, g_params.sps_len, rtp_ts, false) == SEND_OK)
                    (void)send_one_nalu(client, g_params.pps, g_params.pps_len, rtp_ts, false);
            }
        } else if (++client->idr_wait_frames > video_gop_frames() * 2) {
            printf("[ak_rtsp] no I-frame within %d frames, starting anyway\n",
                   client->idr_wait_frames);
            client->need_idr = false;
        } else {
            send_it = false;
        }
    }
    if (send_it) {
        /* First video frame of this start/resync: audio starts from its
         * capture time, not from the ring backlog. */
        if (rtp_astart_holding(&client->audio_start))
            rtp_astart_release(&client->audio_start, capture_us);
    }
    if (send_it && !client->tx_fail) {
        int r = send_frame_nalus(client, (uint8_t *)data, req.len, rtp_ts);
        if (r == SEND_NOBUFS) {
            /* The queue refused a packet in the middle of the frame (only whole packets were queued, so the
             * stream is intact, but this picture is incomplete and the P-frames after it reference it): skip
             * to the next keyframe, exactly as for a backlog over the high mark. */
            tx_pace_event_t ev;
            memset(&ev, 0, sizeof(ev));
            (void)tx_pace_no_room(&client->pace, pace_backlog, ms_ts_now(), &ev);
            ctl_sys_note_video_skipped();
            if (ev.log_enter && ev.entered)
                printf("[ak_rtsp] client link slow (TCP send queue full): dropping video until the next keyframe\n");
        } else {
            ctl_sys_note_video_frame(req.len);
            if (log_recovered)
                printf("[ak_rtsp] client link recovered: dropped %u frames, video resumed\n", client->pace_recovered);
        }
    }

    if (ioctl(g_chn0_fd, VENC_IOCTL_RELEASE_STREAM, &req) < 0) {
        perror("VENC_IOCTL_RELEASE_STREAM");
        return -1;
    }
    return 1;
}

/* -------------------------------------------------------------------------
 * Audio: drain whatever periods audio.c's capture thread has ready and send
 * them as plain RTP (no NAL framing — this isn't H.264).
 *
 * Timestamp: the capture-stream sample index (RFC 3551's intended scheme for
 * a fixed-rate PCM stream like L16) plus a random per-stream origin
 * (RFC 3550 section 5.1). It is exact: no gaps or overlaps even if the
 * capture thread's ring dropped periods (the index then jumps by the dropped
 * amount, so the stream keeps its place on the time axis).
 * Wall-clock time is NOT used for the packet timestamps. An earlier version
 * stamped audio with CLOCK_MONOTONIC while video used the encoder's own
 * clock (a different epoch: 30-40 s apart on hardware) and VLC's initial A/V
 * sync broke badly. The wall-clock relation only goes into the RTCP Sender
 * Reports below, via audio_clock, on the same CLOCK_MONOTONIC the video
 * capture clock uses.
 * RFC 3551 L16 payload must be network (big-endian) byte order; our samples
 * are native (little-endian on ARM), so each sample is byte-swapped before
 * sending. */
static void send_audio_frame(Client *client)
{
    if (!client->audio_rtp_transport || client->tx_fail) return;

    if (rtp_astart_holding(&client->audio_start)) return; /* no video frame yet */

    static int16_t pcm[AUDIO_PERIOD_SAMPLES];
    size_t n_samples = 0;
    uint64_t first_sample = 0;
    int64_t t_end_us = 0;
    int got;
    do {
        got = audio_get_frame_ts(pcm, AUDIO_PERIOD_SAMPLES, &n_samples,
                                 &first_sample, &t_end_us);
        if (got <= 0) return; /* 0 = nothing new yet, -1 = audio not running */
    } while (rtp_astart_stale(&client->audio_start, t_end_us)); /* backlog */

    /* Half duplex (specs/audio.md): while speech from the backchannel
     * plays on the speaker the microphone goes out as silence. Only the samples
     * change; packets, lengths and timestamps stay exactly as without talk. */
    (void)talk_gate_mic(talk_global(), pcm, n_samples, rtp_mono_us());

    static uint16_t net_pcm[AUDIO_PERIOD_SAMPLES];
    for (size_t i = 0; i < n_samples; i++)
        net_pcm[i] = htons((uint16_t)pcm[i]);

    rtp_aclock_observe(&client->audio_clock, first_sample + n_samples,
                       t_end_us, AUDIO_SAMPLE_RATE);
    uint32_t audio_rtp_ts = client->audio_ts_base + (uint32_t)first_sample;

    /* RTSP/TCP client whose link is backed up (video is being dropped): drop the
     * audio period too instead of adding to the queue. The period was consumed
     * and the RTP timestamp is the sample index, so the stream keeps its place. */
    if (client->is_tcp && client->pace.skipping) return;

    U8Slice99 payload = U8Slice99_new((uint8_t *)net_pcm, n_samples * sizeof(uint16_t));
    if (SmolRTSP_RtpTransport_send_packet(
            client->audio_rtp_transport, SmolRTSP_RtpTimestamp_Raw(audio_rtp_ts),
            true, U8Slice99_empty(), payload) == -1)
    {
        int e = errno; /* perror() may change it */
        /* ENOBUFS (the RTSP/TCP queue is full): this packet is dropped, the client stays. */
        if (!(client->is_tcp && e == ENOBUFS)) {
            errno = e;
            perror("send RTP/audio");
            if (client->is_tcp && e != EINTR) client->tx_fail = true;
        }
    }
}

/* -------------------------------------------------------------------------
 * RTCP Sender Reports.
 *
 * Added to fix a VLC(/live555)-specific bug: without ANY RTCP SR at all,
 * VLC's audio track eventually hit a hard "Timestamp conversion failed"
 * error and dropped audio permanently for the rest of the session. An SR
 * gives a receiver an (NTP time, RTP timestamp) pair per track so it can map
 * every RTP timestamp to wall-clock time, and so line the audio and video
 * tracks up against each other (live555 does this for TCP-interleaved
 * sessions too, hence the interleaved path below).
 *
 * The pair must describe ONE instant for both tracks, with the RTP timestamp
 * exactly as the stream computes it for that instant:
 *   - video: the capture-clock formula of send_video_frame() applied to
 *     "now" on CLOCK_MONOTONIC;
 *   - audio: origin + the sample index the capture stream reaches at "now"
 *     (audio_clock maps that stream onto the same CLOCK_MONOTONIC).
 * NTP time is CLOCK_REALTIME read next to the monotonic reading.
 * A track sends nothing until it has sent a packet (an SR with no packet
 * behind it would carry a made-up mapping and a zero packet count), then
 * reports at once and every RTCP_SR_INTERVAL_SEC afterwards. */
#define RTCP_SR_INTERVAL_SEC 5

static void send_one_rtcp_sr(Client *client, SmolRTSP_RtpTransport *transport,
                             int rtcp_fd, uint8_t tcp_channel, uint32_t rtp_ts,
                             int64_t real_us)
{
    uint32_t ntp_sec, ntp_frac;
    rtp_ntp_from_unix_us(real_us, &ntp_sec, &ntp_frac);

    SmolRTSP_RtcpSr sr = {
        .padding     = false,
        .rc          = 0,
        .ssrc        = htonl(SmolRTSP_RtpTransport_ssrc(transport)), /* as in the RTP header */
        .ntp_sec     = htonl(ntp_sec),
        .ntp_frac    = htonl(ntp_frac),
        .rtp_ts      = htonl(rtp_ts),
        .pkt_count   = htonl(SmolRTSP_RtpTransport_pkt_count(transport)),
        .octet_count = htonl(SmolRTSP_RtpTransport_octet_count(transport)),
    };

    if (client->is_tcp) {
        /* RFC 2326 section 10.12: '$', channel, 16-bit length, packet. */
        uint8_t frame[4 + SMOLRTSP_RTCP_SR_SIZE_BASE];
        frame[0] = '$';
        frame[1] = tcp_channel;
        frame[2] = 0;
        frame[3] = SMOLRTSP_RTCP_SR_SIZE_BASE;
        (void)SmolRTSP_RtcpSr_serialize(sr, frame + 4);
        if (net_tx_write_all(&client->tx, frame, sizeof(frame)) < 0) {
            int e = errno; /* perror() may change it */
            if (e == ENOBUFS) return; /* queue full: this report is dropped (the next one is in 5 s) */
            errno = e;
            perror("send RTCP SR (interleaved)");
            client->tx_fail = true;
        }
        return;
    }

    if (rtcp_fd < 0) return;
    uint8_t buf[SMOLRTSP_RTCP_SR_SIZE_BASE];
    if (write(rtcp_fd, SmolRTSP_RtcpSr_serialize(sr, buf), sizeof(buf)) < 0)
        perror("send RTCP SR");
}

/* Called once per client-loop iteration; cheap, no-ops between intervals. */
static void send_rtcp_reports(Client *client) {
    if (client->tx_fail) return;
    int64_t now = rtp_mono_us();
    int64_t real = rtp_realtime_us();
    const int64_t interval = (int64_t)RTCP_SR_INTERVAL_SEC * 1000000;

    if (client->rtp_transport &&
        SmolRTSP_RtpTransport_pkt_count(client->rtp_transport) > 0 &&
        now >= client->next_sr_video_us) {
        client->next_sr_video_us = now + interval;
        send_one_rtcp_sr(client, client->rtp_transport, client->rtcp_udp_fd,
                         client->video_rtcp_ch,
                         rtp_ticks_from_us(now, client->video_ts_base_us,
                                           VIDEO_CLOCK_RATE),
                         real);
    }

    if (client->audio_rtp_transport && !client->tx_fail && client->audio_clock.init &&
        SmolRTSP_RtpTransport_pkt_count(client->audio_rtp_transport) > 0 &&
        now >= client->next_sr_audio_us) {
        client->next_sr_audio_us = now + interval;
        uint32_t ts = client->audio_ts_base +
            (uint32_t)rtp_aclock_samples_at(&client->audio_clock, now,
                                            AUDIO_SAMPLE_RATE);
        send_one_rtcp_sr(client, client->audio_rtp_transport,
                         client->audio_rtcp_udp_fd, client->audio_rtcp_ch,
                         ts, real);
    }
}

/* -------------------------------------------------------------------------
 * RTSP/TCP writer over net_tx: the same bytes smolrtsp_fd_writer() would
 * write, but a write only fails after the peer has accepted nothing for
 * TX_STALL_BUDGET_MS (see handle_client() for the policy), and a short write
 * is completed instead of being left half way in the interleaved stream.
 * ------------------------------------------------------------------------- */
typedef net_tx_t TxWriter;

static ssize_t TxWriter_write(VSelf, CharSlice99 data) {
    VSELF(TxWriter);
    if (net_tx_write_all(self, data.ptr, data.len) < 0) return -1;
    return (ssize_t)data.len;
}

static void TxWriter_lock(VSelf) { VSELF(TxWriter); (void)self; }
static void TxWriter_unlock(VSelf) { VSELF(TxWriter); (void)self; }
static size_t TxWriter_filled(VSelf) { VSELF(TxWriter); (void)self; return 0; }

static int TxWriter_vwritef(VSelf, const char *restrict fmt, va_list ap) {
    VSELF(TxWriter);
    char small[512];
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(small, sizeof(small), fmt, ap);
    int ret = n;
    if (n >= 0 && (size_t)n < sizeof(small)) {
        if (net_tx_write_all(self, small, (size_t)n) < 0) ret = -1;
    } else if (n >= 0) {
        char *big = malloc((size_t)n + 1);
        if (!big) {
            ret = -1;
        } else {
            (void)vsnprintf(big, (size_t)n + 1, fmt, ap2);
            if (net_tx_write_all(self, big, (size_t)n) < 0) ret = -1;
            free(big);
        }
    }
    va_end(ap2);
    return ret;
}

static int TxWriter_writef(VSelf, const char *restrict fmt, ...) {
    VSELF(TxWriter);
    va_list ap;
    va_start(ap, fmt);
    int ret = TxWriter_vwritef(self, fmt, ap);
    va_end(ap);
    return ret;
}

impl(SmolRTSP_Writer, TxWriter);

/* -------------------------------------------------------------------------
 * Input handling: rtsp_in frames the byte stream, these act on each item.
 * ------------------------------------------------------------------------- */
typedef struct {
    Client *client;
    SmolRTSP_Controller *controller;
    SmolRTSP_Writer *writer;
    rtsp_idle_t *idle;
} InCtx;

/* Error response written without the controller (request not parsed, or too
 * big to buffer). The CSeq is taken from the message when it can be read. */
static void send_error_reply(Client *client, int code, const char *reason,
                             const char *msg, size_t len)
{
    unsigned long cseq = 0;
    char out[160];
    if (rtsp_in_header_uint(msg, len, "CSeq", &cseq) != 1) cseq = 0;
    int n = snprintf(out, sizeof(out), "RTSP/1.0 %d %s\r\nCSeq: %lu\r\n\r\n",
                     code, reason, cseq);
    if (n > 0 && net_tx_write_all(&client->tx, out, (size_t)n) < 0)
        client->tx_fail = true;
}

/* One complete RTSP request. A request SmolRTSP rejects is answered 400 and
 * the session goes on: the framing (and thus the stream) is intact. */
static int in_on_message(void *user, const char *data, size_t len)
{
    InCtx *c = user;
    rtsp_idle_request(c->idle, rtp_mono_us()); /* a complete request: progress */
    SmolRTSP_Request req = SmolRTSP_Request_uninit();
    SmolRTSP_ParseResult res = SmolRTSP_Request_parse(&req, CharSlice99_new((char *)data, len));
    bool ok = false;

    match(res) {
        of(SmolRTSP_ParseResult_Success, status) {
            match(*status) {
                of(SmolRTSP_ParseStatus_Complete, offset) {
                    ok = (*offset == len);
                }
                otherwise {}
            }
        }
        of(SmolRTSP_ParseResult_Failure, err) {
            (void)err;
        }
    }

    if (ok) {
        smolrtsp_dispatch(*c->writer, *c->controller, &req);
        if (c->client->bc_playing && !c->client->bc_noted) {
            c->client->bc_noted = true; /* PLAY of the backchannel: start of its idle clock */
            rtsp_idle_talk_activity(c->idle, rtp_mono_us());
        }
    } else {
        printf("[ak_rtsp] Request parse failed (%zu bytes), answering 400\n", len);
        send_error_reply(c->client, 400, "Bad Request", data, len);
    }
    return c->client->drop_reason != NULL || c->client->tx_fail;
}

/* Interleaved frame from the client (RFC 2326 section 10.12). Players send
 * RTCP receiver reports on the RTCP channel of each track every few seconds;
 * they need no answer. Only a BYE matters: the client is leaving. */
static int in_on_interleaved(void *user, uint8_t channel, const uint8_t *data, size_t len)
{
    InCtx *c = user;
    Client *client = c->client;
    if (client->bc_setup && client->bc_tcp) {
        /* The backchannel's channel pair is routed first: its RTP is audio for
         * the speaker, its RTCP is only watched for a BYE. */
        if (channel == client->bc_rtp_ch) {
            if (client->bc_playing) {
                int64_t now = rtp_mono_us();
                talk_rtp(talk_global(), data, len, now);
                rtsp_idle_talk_activity(c->idle, now);
            }
            return 0;
        }
        if (channel == client->bc_rtcp_ch) {
            if (rtsp_rtcp_scan(data, len, NULL)) {
                bc_stop(client);
                if (!client->is_playing) {
                    client->drop_reason = "RTCP BYE from client";
                    return 1;
                }
            }
            return 0;
        }
    }
    bool rtcp_ch = client->is_tcp &&
        ((client->rtp_transport && channel == client->video_rtcp_ch) ||
         (client->audio_rtp_transport && channel == client->audio_rtcp_ch));
    if (rtcp_ch && rtsp_rtcp_scan(data, len, NULL)) {
        client->drop_reason = "RTCP BYE from client";
        return 1;
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * Client connection loop
 *
 * Uses poll() for incoming RTSP commands and for the socket becoming writable (POLLOUT, only while the
 * private send queue holds bytes). Writes never block for long: net_tx sends without waiting and, for
 * RTSP/TCP video, queues what the kernel does not take.
 *   timeout = 0 / PLAY_IDLE_POLL_MS when is_playing (0 right after a frame,
 *             PLAY_IDLE_POLL_MS when the encoder had nothing ready)
 *   timeout = at most VENC_DRAIN_POLL_MS when connected but not playing (the encoder ring is drained
 *             on every pass, as while no client is connected), else until the idle deadline
 * ------------------------------------------------------------------------- */
/* Poll timeout while playing and the encoder has no frame ready. Short
 * enough for 15 fps video (66 ms) and the 32 ms audio period. */
#define PLAY_IDLE_POLL_MS 4

/* Poll timeout while a client is connected but not playing: the encoder ring is drained on every pass so it
 * does not wrap (about 1.5 s of video) while the client takes its time between DESCRIBE and PLAY. */
#define VENC_DRAIN_POLL_MS 20

/* After TEARDOWN: the longest wait for the last bytes (the 200 OK, a queued tail) to be taken by the peer. */
#define TEARDOWN_FLUSH_MS 200

/* A client that accepts no byte at all for this long is considered gone. */
#define TX_STALL_BUDGET_MS 15000
/* A write that stalled this long left the VENC ring to wrap and frames
 * behind: restart the video on the next keyframe rather than send P-frames
 * whose references were overwritten. */
#define TX_RESYNC_STALL_MS 1000

/* Set the process-wide credentials state up. Called ONCE from main() before any thread starts (the RTSP loop,
 * the control port and the ONVIF HTTP server all use it afterwards under rtsp_auth.c's mutex); idempotent. */
void rtsp_auth_shared_init(void)
{
    static bool ready;
    if (ready) return;
    ready = true;
    rtsp_auth_db_init(&g_auth, RTSP_AUTH_PATH);
}

/* Credentials state for rtsp.auth (8091 metrics): re-read at most every 5 s while no client is connected (the
 * idle loop of main() calls this; a connected client refreshes it itself). One open() of a small file. */
void rtsp_auth_tick(bool force)
{
    static int64_t next_us;
    int64_t now = rtp_mono_us();
    rtsp_auth_shared_init();
    if (!force && now < next_us) return;
    next_us = now + 5000000;
    ctl_sys_note_rtsp_auth((int)rtsp_auth_db_refresh(&g_auth));
}

/* The process-wide credentials state, for cred_init(): ONVIF shares the RTSP failure limiter and its htdigest
 * data. rtsp_auth_shared_init() must have run (main does it first); every access is under rtsp_auth.c's mutex. */
rtsp_auth_db_t *rtsp_auth_shared_db(void)
{
    rtsp_auth_shared_init();
    return &g_auth;
}

/* G06: the listening socket, polled during a session so a second client gets "503" at once (rtsp_busy.h). */
static int g_busy_listen_fd = -1;
void rtsp_set_listen_fd(int fd) { g_busy_listen_fd = fd; }

void handle_client(int client_fd, struct sockaddr_storage *client_addr,
                   socklen_t addr_len)
{
    /* Blocking writes (see above) must not wedge the whole single-client
     * server on a peer that vanished without FIN (Wi-Fi drop), yet must not
     * kill a client that is merely slow (a player that stops reading for a
     * while, e.g. VLC stalling on its own A/V sync, or a Wi-Fi hiccup).
     * Policy:
     *  - SO_SNDTIMEO (1 s) only bounds ONE wait; it is not a verdict. The
     *    previous 5 s value turned any 5 s read stall into a dropped client
     *    and, because a timed-out write can be short, left a torn interleaved
     *    frame behind it.
     *  - net_tx completes every write and gives up only when the peer has
     *    accepted NOT ONE BYTE for TX_STALL_BUDGET_MS (15 s). A slow reader
     *    that still drains keeps its session; a dead one (no ACKs, send
     *    buffer full) is freed after 15 s instead of wedging the server until
     *    the kernel's ~15 min retransmission limit.
     *  - TCP keepalive reaps peers that vanished while idle (no writes). */
    struct timeval snd_to = { .tv_sec = 1, .tv_usec = 0 };
    int keepalive = 1;
    setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &snd_to, sizeof(snd_to));
    setsockopt(client_fd, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive));
    /* Send buffer: ask for 256 KiB. The kernel clamps to net.core.wmem_max (160 KiB on the camera) and
     * doubles it (~320 KiB sk_sndbuf), and a fixed SO_SNDBUF switches the buffer autotuning off. The pacing
     * (tx_pace.h) reads the resulting size back every frame and keeps its watermarks inside it. */
    int sndbuf = 256 * 1024;
    setsockopt(client_fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
    /* The kernel default probes only after 2 h of silence: a peer that vanished
     * before PLAY (or while paused) would keep this single-client server busy
     * for over 2 h. Probe after 30 s idle, every 10 s, give up after 3. */
    int ka_idle = 30, ka_intvl = 10, ka_cnt = 3;
    setsockopt(client_fd, IPPROTO_TCP, TCP_KEEPIDLE, &ka_idle, sizeof(ka_idle));
    setsockopt(client_fd, IPPROTO_TCP, TCP_KEEPINTVL, &ka_intvl, sizeof(ka_intvl));
    setsockopt(client_fd, IPPROTO_TCP, TCP_KEEPCNT, &ka_cnt, sizeof(ka_cnt));

    Client client;
    memset(&client, 0, sizeof(client));
    client.socket_fd = client_fd;
    memcpy(&client.addr, client_addr, addr_len);
    client.addr_len = addr_len;
    client.rtp_udp_fd = -1;
    client.rtcp_udp_fd = -1;
    client.audio_rtp_udp_fd = -1;
    client.audio_rtcp_udp_fd = -1;
    client.bc_rtp_fd = -1;
    client.bc_rtcp_fd = -1;
    net_tx_init(&client.tx, client_fd, TX_STALL_BUDGET_MS);

    /* rand() feeds the SSRCs, session id and RTP timestamp origins: seed it
     * (once) so they differ from boot to boot. */
    static bool seeded;
    if (!seeded) {
        seeded = true;
        srand((unsigned)(rtp_mono_us() ^ ((int64_t)getpid() << 16) ^ (int64_t)time(NULL)));
    }

    SmolRTSP_Controller controller = DYN(Client, SmolRTSP_Controller, &client);
    SmolRTSP_Writer writer = DYN(TxWriter, SmolRTSP_Writer, &client.tx);

    rtsp_in_t in;
    rtsp_in_init(&in);
    rtsp_idle_t idle;
    rtsp_idle_init(&idle, rtp_mono_us());
    rtsp_busy_t busy;
    rtsp_busy_init(&busy);
    InCtx in_ctx = { &client, &controller, &writer, &idle };
    const rtsp_in_ops_t in_ops = { &in_ctx, in_on_message, in_on_interleaved };
    bool video_idle = false;

    rtsp_auth_tick(true);
    rtsp_auth_conn_init(&client.auth);
    ctl_sys_note_rtsp_client(1);
    int64_t next_auth_check_us = 0;
    {
        static const char *const names[] = { "disabled", "enabled (Digest)",
                                             "INVALID credentials file, refusing all" };
        printf("[ak_rtsp] Client connected, RTSP authentication %s\n",
               names[rtsp_auth_db_refresh(&g_auth)]);
        ctl_sys_note_rtsp_auth((int)rtsp_auth_db_mode(&g_auth));
    }

    while (client_fd >= 0) {
        swdog_beat(SWDOG_BEAT_MAIN);
        /* Frames are fetched only while PLAYing (or draining for a talk-only session). */
        swdog_mode((client.is_playing || client.bc_playing) ? SWDOG_FETCH : SWDOG_IDLE);
        struct pollfd pfds[2 + RTSP_BUSY_NFDS];
        pfds[0].fd = client_fd;
        /* POLLOUT only while the private send queue holds bytes: the loop wakes when the socket can take them. */
        pfds[0].events = POLLIN | (net_tx_pending(&client.tx) ? POLLOUT : 0);
        pfds[0].revents = 0;
        pfds[1].fd = client.bc_playing ? client.bc_rtp_fd : -1; /* UDP backchannel; -1 is ignored */
        pfds[1].events = POLLIN;
        pfds[1].revents = 0;
        /* While playing, wait PLAY_IDLE_POLL_MS in poll() when the last pass
         * found no video frame (this replaces a usleep(2000) after every empty
         * GET_STREAM and wakes at once on an RTSP request); poll again with
         * no delay right after a frame, in case the next one is queued.
         * A talk-only session (nothing streams to the client) wakes at least
         * every 100 ms for the tail flush and its own idle clock. */
        int wait_ms;
        if (client.is_playing)
            wait_ms = video_idle ? PLAY_IDLE_POLL_MS : 0;
        else if (client.bc_playing)
            wait_ms = rtsp_idle_talk_wait_ms(&idle, rtp_mono_us(), 100);
        else
            wait_ms = rtsp_idle_wait_ms(&idle, 0, rtp_mono_us(), 5000);
        if (!client.is_playing && wait_ms > VENC_DRAIN_POLL_MS) wait_ms = VENC_DRAIN_POLL_MS;
        rtsp_busy_fds(&busy, g_busy_listen_fd, &pfds[2]);
        wait_ms = rtsp_busy_wait_ms(&busy, rtp_mono_us() / 1000, wait_ms);
        int poll_ret = poll(pfds, 2 + RTSP_BUSY_NFDS, wait_ms);

        if (poll_ret < 0) {
            if (errno == EINTR) continue;
            perror("poll");
            break;
        }

        if (poll_ret > 0 && (pfds[1].revents & POLLIN)) bc_udp_receive(&client, &idle);

        /* Private send queue (RTSP/TCP): drain what the socket takes now. A peer that has taken nothing for the
         * stall budget is gone, exactly as for a blocking write. */
        if (net_tx_pending(&client.tx)) {
            if (net_tx_pump(&client.tx) < 0) {
                perror("send (queued)");
                client.tx_fail = true;
            } else if (net_tx_queue_stalled_ms(&client.tx) >= TX_STALL_BUDGET_MS) {
                client.tx_fail = true;
            }
            if (client.tx_fail && !client.drop_reason) client.drop_reason = "TCP write failed or stalled";
            if (client.drop_reason) break;
        }

        if (poll_ret > 0 && (pfds[0].revents & ~POLLOUT) != 0) {
            /* rtsp_in guarantees room for at least one byte here: a request
             * that fills the buffer is answered/dropped below, '$' frames are
             * consumed as they complete. recv() is never asked for 0 bytes
             * (which would read as end-of-stream). */
            size_t cap;
            char *dst = rtsp_in_space(&in, &cap);
            ssize_t n = cap ? recv(client_fd, dst, cap, 0) : 0;
            if (cap == 0) {
                client.drop_reason = "input buffer full";
            } else if (n < 0) {
                if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                    perror("recv");
                    client.drop_reason = "recv error";
                }
            } else if (n == 0) {
                client.drop_reason = "client closed the connection";
            } else {
                rtsp_in_commit(&in, (size_t)n);
                switch (rtsp_in_pump(&in, &in_ops)) {
                case RTSP_IN_PUMP_OVERFLOW: {
                    size_t dl;
                    const char *d = rtsp_in_data(&in, &dl);
                    send_error_reply(&client, 413, "Request Entity Too Large", d, dl);
                    client.drop_reason = "RTSP request larger than the input buffer";
                    break;
                }
                case RTSP_IN_PUMP_GARBAGE:
                    client.drop_reason = "invalid input (neither RTSP nor interleaved frame)";
                    break;
                case RTSP_IN_PUMP_OK:
                case RTSP_IN_PUMP_STOPPED:
                    break;
                }
            }
            /* A response that could not be written: leave now instead of
             * writing video to the dead socket (each write could wait out
             * the whole stall budget again). */
            if (client.tx_fail && !client.drop_reason)
                client.drop_reason = "TCP write failed or stalled";
            if (client.drop_reason) break;
        }

        /* Second clients: only after the current client's socket was handled, and never in a round where
         * the session ends. A newcomer that connects right after this client closed stays in the listen
         * backlog for main()'s accept(); serving it here would put it in a busy slot that the end of
         * the session closes without a word. */
        rtsp_busy_service(&busy, g_busy_listen_fd, &pfds[2], rtp_mono_us() / 1000);

        /* Before PLAY the single slot must not be held by a silent or stuck
         * client. While playing there is no idleness drop (see rtsp_idle.h). */
        if (!client.is_playing) {
            /* A talk-only session has its own clock: 30 s without audio. */
            const char *why = client.bc_playing ? rtsp_idle_check_talk(&idle, rtp_mono_us())
                                                : rtsp_idle_check(&idle, 0, rtp_mono_us());
            if (why) {
                client.drop_reason = why;
                break;
            }
        }

        if (client.is_playing || client.bc_playing) {
            /* Authentication switched on, or other credentials: a stream that
             * started before must log in again (it reconnects and is asked).
             * Talk needs the password on, so switching it off ends it too. */
            int64_t now_us = rtp_mono_us();
            if (now_us >= next_auth_check_us) {
                next_auth_check_us = now_us + 1000000;
                ctl_sys_note_rtsp_auth((int)rtsp_auth_db_mode(&g_auth));
                if (!rtsp_auth_session_ok(&g_auth, &client.auth) ||
                    (client.bc_playing && rtsp_auth_db_mode(&g_auth) != RTSP_AUTH_ON)) {
                    client.drop_reason = "RTSP credentials changed or authentication enabled";
                    break;
                }
                /* audio.talk_enable=0 (or audio out disabled, or the speaker
                 * device failed) ends a running talk: the switch is the way to
                 * cut off a talker. A video stream in the same connection stays. */
                if (client.bc_playing && !audio_out_talk_available()) {
                    printf("[ak_rtsp] backchannel: talk switched off or speaker unavailable, ending it\n");
                    bc_stop(&client);
                    if (!client.is_playing) {
                        client.drop_reason = "talk is no longer available";
                        break;
                    }
                }
            }
        }
        if (client.bc_playing) talk_tick(talk_global(), rtp_mono_us());
        /* A client that is connected but does not play (before PLAY, talk-only, paused) fetches no video, but
         * the encoder ring must not wrap (and the SPS/PPS cache and the capture clock must stay current) for
         * the next PLAY or viewer, exactly as while no client is connected. */
        if (!client.is_playing) drain_venc_ring();

        if (client.is_playing) {
            /* A session without a video track (microphone only) must not touch
             * the video transport: keep the encoder ring drained instead. */
            int vret;
            if (client.nal_transport) {
                vret = send_video_frame(&client);
            } else {
                drain_venc_ring();
                vret = 0;
            }
            if (vret < 0) {
                client.drop_reason = "error sending video frame";
                break;
            }
            if (client.drop_reason) break; /* set by the TCP pacing: link stalled or hopeless */
            video_idle = (vret == 0);
            send_audio_frame(&client); /* no-op until the audio track is SETUP */
            send_rtcp_reports(&client); /* no-op for tracks not yet SETUP, or off-interval */
            int stall_ms = net_tx_take_stall(&client.tx);
            if (stall_ms >= TX_RESYNC_STALL_MS && client.is_tcp) {
                printf("[ak_rtsp] client slow: TCP write stalled %d ms, "
                       "resuming video at the next keyframe\n", stall_ms);
                if (client.nal_transport) {
                    client.need_idr = true;
                    client.idr_wait_frames = 0;
                    rtp_astart_arm(&client.audio_start);
                }
            }
        }
        if (client.tx_fail) {
            client.drop_reason = "TCP write failed or stalled";
            break;
        }
    }

    printf("[ak_rtsp] Dropping client: %s\n",
           client.drop_reason ? client.drop_reason : "loop ended");
    /* TEARDOWN: the 200 OK (and the tail of the queue) should be out before the socket closes; the loop left
     * as soon as the request was handled. Bounded, and only while the connection is healthy. */
    if (client.drop_reason && !strcmp(client.drop_reason, "TEARDOWN") && !client.tx_fail) {
        int64_t end_us = rtp_mono_us() + (int64_t)TEARDOWN_FLUSH_MS * 1000;
        while (net_tx_pending(&client.tx) > 0 && net_tx_pump(&client.tx) == 0 &&
               net_tx_pending(&client.tx) > 0) {
            int64_t left_ms = (end_us - rtp_mono_us()) / 1000;
            if (left_ms <= 0) break;
            struct pollfd po = { .fd = client_fd, .events = POLLOUT, .revents = 0 };
            (void)poll(&po, 1, (int)left_ms);
        }
    }
    client_set_playing(&client, false);
    ctl_sys_note_rtsp_client(-1);
    bc_stop(&client);
    VCALL(DYN(Client, SmolRTSP_Droppable, &client), drop);
    rtsp_busy_close_all(&busy);
    net_tx_queue_free(&client.tx);
    close(client_fd);
    printf("[ak_rtsp] Client connection closed\n");
}
