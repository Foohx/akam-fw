#include "talk.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "audio_jitter.h"
#include "audio_out.h"
#include "rtp_clock.h"

void talk_init(talk_t *t, const talk_ops_t *ops)
{
    memset(t, 0, sizeof(*t));
    t->ops = *ops;
    rtp_in_init(&t->rtp);
    audio_vad_init(&t->vad, AVAD_DEFAULT_HOLD_MS);
}

static const char *codec_name(int pt)
{
    return pt == RTP_IN_PT_PCMU ? "pcmu" : pt == RTP_IN_PT_PCMA ? "pcma" : "none";
}

int talk_begin(talk_t *t, const char *peer)
{
    if (t->active) return -1;
    if (t->ops.claim(t->ops.user) < 0) return -1;
    rtp_in_init(&t->rtp);
    t->last_rtp_us = 0;
    t->tail_flushed = 1; /* nothing queued by this talker yet */
    t->logged_start = 0;
    snprintf(t->peer, sizeof(t->peer), "%s", peer ? peer : "?");
    t->sessions++;
    t->active = 1;
    return 0;
}

/* The jitter buffer plays nothing below its pre-fill level: top a short tail
 * (the end of an utterance) up with silence so that it is heard now and does
 * not come out at the start of the next talk. */
static void flush_tail(talk_t *t)
{
    if (t->tail_flushed) return;
    t->tail_flushed = 1;
    size_t q = t->ops.queued(t->ops.user);
    if (q > 0 && q < JITTER_PREFILL_SAMPLES) {
        static const int16_t zeros[JITTER_PREFILL_SAMPLES];
        t->ops.push(t->ops.user, zeros, JITTER_PREFILL_SAMPLES - q);
    }
}

void talk_rtp(talk_t *t, const uint8_t *pkt, size_t len, int64_t now_us)
{
    if (!t->active) return;
    int16_t pcm[RTP_IN_MAX_SAMPLES];
    size_t n = 0;
    int pt = -1;
    t->last_rtp_us = now_us;
    if (rtp_in_feed(&t->rtp, pkt, len, pcm, &n, &pt) != RTP_IN_OK) return;
    if (!t->logged_start) {
        t->logged_start = 1;
        printf("[ak_rtsp] backchannel: %s started %s\n", t->peer, codec_name(pt));
    }
    /* The audio already queued is heard first: the speech gate stays closed
     * that much longer. */
    unsigned latency_ms = (unsigned)(t->ops.queued(t->ops.user) / (JITTER_RATE_HZ / 1000));
    audio_vad_feed(&t->vad, pcm, n, now_us, latency_ms);
    t->ops.push(t->ops.user, pcm, n);
    t->tail_flushed = 0;
}

void talk_tick(talk_t *t, int64_t now_us)
{
    if (t->active && !t->tail_flushed && now_us - t->last_rtp_us >= TALK_TAIL_FLUSH_US)
        flush_tail(t);
}

void talk_end(talk_t *t)
{
    if (!t->active) return;
    flush_tail(t);
    t->active = 0;
    t->ops.release(t->ops.user);
    if (t->rtp.packets)
        printf("[ak_rtsp] backchannel: %s ended %s packets=%lu dropped=%lu lost=%lu\n",
               t->peer, codec_name(t->rtp.last_pt), t->rtp.packets, t->rtp.dropped, t->rtp.lost);
    else
        printf("[ak_rtsp] backchannel: %s ended without audio dropped=%lu\n", t->peer, t->rtp.dropped);
}

int talk_mic_muted(const talk_t *t, int64_t now_us)
{
    return t->ops.half_duplex(t->ops.user) && audio_vad_speech(&t->vad, now_us);
}

int talk_gate_mic(const talk_t *t, int16_t *pcm, size_t n, int64_t now_us)
{
    if (!talk_mic_muted(t, now_us)) return 0;
    memset(pcm, 0, n * sizeof(*pcm));
    return 1;
}

int64_t talk_last_rtp_us(const talk_t *t) { return t->last_rtp_us; }

/* ------------------------------------------------------------------------
 * Process-wide instance on audio_out.c
 * ------------------------------------------------------------------------ */

static size_t g_push(void *u, const int16_t *p, size_t n) { (void)u; return audio_out_push(p, n); }
static size_t g_queued(void *u) { (void)u; return audio_out_queued(); }
static int    g_half(void *u) { (void)u; return audio_out_talk_half_duplex(); }
static int    g_claim(void *u) { (void)u; return audio_out_feeder_claim(); }
static void   g_release(void *u) { (void)u; audio_out_feeder_release(); }

static talk_t         g_talk;
static pthread_once_t g_talk_once = PTHREAD_ONCE_INIT;

static void talk_single_init(void)
{
    static const talk_ops_t ops = { g_push, g_queued, g_half, g_claim, g_release, NULL };
    talk_init(&g_talk, &ops);
}

talk_t *talk_global(void)
{
    pthread_once(&g_talk_once, talk_single_init);
    return &g_talk;
}

static int is_talk_key(const char *key) { return !strncmp(key, "audio.talk_", 11); }

int talk_ctl_get(const char *key, char *out, size_t cap)
{
    if (!is_talk_key(key)) return -1;
    const talk_t *t = talk_global();
    const char *k = key + 11;
    if      (!strcmp(k, "state"))    snprintf(out, cap, "audio.talk_state=%s", t->active ? "active" : "idle");
    else if (!strcmp(k, "codec"))    snprintf(out, cap, "audio.talk_codec=%s", codec_name(t->rtp.last_pt));
    else if (!strcmp(k, "packets"))  snprintf(out, cap, "audio.talk_packets=%lu", t->rtp.packets);
    else if (!strcmp(k, "dropped"))  snprintf(out, cap, "audio.talk_dropped=%lu", t->rtp.dropped);
    else if (!strcmp(k, "lost"))     snprintf(out, cap, "audio.talk_lost=%lu", t->rtp.lost);
    else if (!strcmp(k, "sessions")) snprintf(out, cap, "audio.talk_sessions=%lu", t->sessions);
    else if (!strcmp(k, "mic_muted")) snprintf(out, cap, "audio.talk_mic_muted=%d", talk_mic_muted(t, rtp_mono_us()) ? 1 : 0);
    else return -1; /* audio.talk_enable / audio.talk_duplex: audio_out.c */
    return 0;
}

int talk_ctl_set(const char *key, const char *val, char *msg, size_t cap)
{
    (void)val;
    if (talk_ctl_get(key, msg, cap) != 0) return -1; /* not a read-only talk key */
    snprintf(msg, cap, "ERR %s is read-only", key);
    return 1;
}

void talk_ctl_list(void (*emit)(void *arg, const char *line), void *arg)
{
    static const char *const keys[] = {
        "audio.talk_state", "audio.talk_codec", "audio.talk_packets", "audio.talk_dropped",
        "audio.talk_lost", "audio.talk_sessions", "audio.talk_mic_muted"
    };
    char line[64];
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        talk_ctl_get(keys[i], line, sizeof(line));
        emit(arg, line);
    }
}
