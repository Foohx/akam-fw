#pragma once
/* Talk session of the ONVIF audio backchannel (specs/audio.md).
 *
 * One talker at a time sends G.711 RTP; this module validates and decodes it
 * (rtp_in.c), runs the speech gate of the half-duplex rule (audio_vad.c) and
 * queues the samples for the speaker (audio_out_push(): jitter buffer, volume,
 * amplifier policy). It also keeps the counters shown on port 8091.
 *
 * It knows nothing about RTSP, sockets or the clock: rtsp.c hands it the RTP
 * packets (interleaved '$' frames or UDP) with the time of arrival. The
 * speaker side is behind talk_ops_t so that tests/talk_test.sh can drive it
 * with a fake sink; talk_global() is the instance wired to audio_out.c.
 * Called from the RTSP client thread only, except talk_ctl_* (control thread,
 * reads of plain counters). */
#include <stddef.h>
#include <stdint.h>

#include "audio_vad.h"
#include "rtp_in.h"

typedef struct talk_ops {
    size_t (*push)(void *user, const int16_t *pcm, size_t n);  /* queue samples, never blocks */
    size_t (*queued)(void *user);                              /* samples waiting in front of the speaker */
    int    (*half_duplex)(void *user);                         /* audio.talk_duplex = half */
    int    (*claim)(void *user);                               /* 0 = got the speaker's push side */
    void   (*release)(void *user);
    void   *user;
} talk_ops_t;

/* Quiet time after which a talker that stopped sending has its queued tail
 * flushed (the jitter buffer plays nothing below its pre-fill level). */
#define TALK_TAIL_FLUSH_US 250000

typedef struct talk {
    talk_ops_t ops;
    volatile int active;
    rtp_in_t     rtp;         /* counters of the current (or last) session */
    audio_vad_t  vad;
    int64_t      last_rtp_us;
    int          tail_flushed;
    int          logged_start;
    char         peer[48];
    unsigned long sessions;   /* talk sessions since boot */
} talk_t;

void talk_init(talk_t *t, const talk_ops_t *ops);

/* Starts a session for a talker at peer (text, for the journal). Returns 0, or
 * -1 when another talker holds the speaker. */
int  talk_begin(talk_t *t, const char *peer);
/* One RTP packet of the backchannel. now_us: monotonic microseconds. Ignored
 * when no session is active. */
void talk_rtp(talk_t *t, const uint8_t *pkt, size_t len, int64_t now_us);
/* Housekeeping, cheap: call at every pass of the client loop. */
void talk_tick(talk_t *t, int64_t now_us);
/* Ends the session (idempotent): flushes the tail, frees the speaker, logs. */
void talk_end(talk_t *t);

/* 1 when the microphone must be replaced by silence now: half duplex and
 * speech playing (or played less than the hold time ago). */
int  talk_mic_muted(const talk_t *t, int64_t now_us);
/* Half-duplex gate on an outgoing microphone period: when talk_mic_muted(),
 * the n samples are overwritten with silence and 1 is returned. Only the
 * samples change: the packet, its length and its RTP timestamp go on as usual. */
int  talk_gate_mic(const talk_t *t, int16_t *pcm, size_t n, int64_t now_us);
/* Microseconds of the last accepted or dropped RTP packet (0 before any). */
int64_t talk_last_rtp_us(const talk_t *t);

/* ---- singleton on audio_out.c and its 8091 keys (read-only) ---------------
 * audio.talk_state (idle|active), audio.talk_codec (pcmu|pcma|none),
 * audio.talk_packets, audio.talk_dropped, audio.talk_lost, audio.talk_sessions,
 * audio.talk_mic_muted (0|1). The settings audio.talk_enable / audio.talk_duplex
 * live in audio_out.c. Same return convention as audio_out_ctl_*. */
talk_t *talk_global(void);
int  talk_ctl_get(const char *key, char *out, size_t cap);
int  talk_ctl_set(const char *key, const char *val, char *msg, size_t cap);
void talk_ctl_list(void (*emit)(void *arg, const char *line), void *arg);
