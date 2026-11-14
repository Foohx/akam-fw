/* Host test of src/ak_rtsp/talk.c: the talk session of the audio backchannel
 * (RTP in, decode, speech gate, speaker queue, counters) with a fake speaker.
 * Build/run: tests/talk_test.sh */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio_jitter.h"
#include "audio_out.h"
#include "g711.h"
#include "talk.h"

static int fails;
#define CHECK(cond, msg) do { if (cond) printf("  ok   - %s\n", msg); \
    else { printf("  FAIL - %s\n", msg); fails++; } } while (0)

#define MS 1000LL

/* ---------------------------------------------------------------- fake speaker */
static struct {
    size_t   queued;       /* samples waiting */
    size_t   pushed;       /* total samples pushed */
    int      claimed;
    int      half;
    int      claim_busy;   /* another talker holds the speaker */
    int16_t  first[16];
    int      have_first;
} S;

static size_t f_push(void *u, const int16_t *p, size_t n)
{
    (void)u;
    if (!S.have_first) { memcpy(S.first, p, (n < 16 ? n : 16) * sizeof(int16_t)); S.have_first = 1; }
    S.queued += n; S.pushed += n;
    return n;
}
static size_t f_queued(void *u) { (void)u; return S.queued; }
static int    f_half(void *u) { (void)u; return S.half; }
static int    f_claim(void *u) { (void)u; if (S.claim_busy || S.claimed) return -1; S.claimed = 1; return 0; }
static void   f_release(void *u) { (void)u; S.claimed = 0; }
static const talk_ops_t OPS = { f_push, f_queued, f_half, f_claim, f_release, NULL };

static void reset(talk_t *t)
{
    memset(&S, 0, sizeof(S));
    S.half = 1;
    talk_init(t, &OPS);
}

/* RTP packet of 160 G.711 samples built from linear samples. */
static size_t mk(uint8_t *p, int pt, uint16_t seq, const int16_t *pcm)
{
    memset(p, 0, 12);
    p[0] = 0x80; p[1] = (uint8_t)pt; p[2] = (uint8_t)(seq >> 8); p[3] = (uint8_t)seq; p[11] = 9;
    for (int i = 0; i < 160; i++)
        p[12 + i] = pt == 8 ? g711_alaw_encode(pcm[i]) : g711_ulaw_encode(pcm[i]);
    return 172;
}

static void tone(int16_t *out, double dbfs, double phase)
{
    double amp = 32768.0 * pow(10.0, dbfs / 20.0) * 1.41421356;
    for (int i = 0; i < 160; i++) out[i] = (int16_t)lrint(amp * sin(2 * M_PI * 440.0 * ((double)i + phase) / 8000.0));
}

static void send_pkt(talk_t *t, int pt, uint16_t seq, double dbfs, int64_t now_us)
{
    int16_t pcm[160];
    uint8_t pkt[172];
    tone(pcm, dbfs, seq * 160.0);
    if (dbfs <= -100.0) memset(pcm, 0, sizeof(pcm));
    talk_rtp(t, pkt, mk(pkt, pt, seq, pcm), now_us);
}

static void test_session(void)
{
    printf("# session\n");
    talk_t t;
    reset(&t);
    CHECK(!t.active, "idle at start");
    uint8_t pkt[172]; int16_t z[160] = {0};
    talk_rtp(&t, pkt, mk(pkt, 0, 1, z), 0);
    CHECK(S.pushed == 0 && t.rtp.packets == 0, "no session: packets ignored");

    CHECK(talk_begin(&t, "192.0.2.7") == 0 && t.active && S.claimed, "begin claims the speaker");
    CHECK(talk_begin(&t, "192.0.2.8") == -1, "a second begin on the same session is refused");
    talk_t t2;
    talk_init(&t2, &OPS);
    CHECK(talk_begin(&t2, "192.0.2.8") == -1 && !t2.active, "another talker holding the speaker: refused");
    S.claimed = 0;

    send_pkt(&t, 0, 10, -20.0, 1000 * MS);
    CHECK(S.pushed == 160 && t.rtp.packets == 1, "PCMU packet: 160 samples queued");
    send_pkt(&t, 8, 11, -20.0, 1020 * MS);
    CHECK(S.pushed == 320 && t.rtp.last_pt == 8, "PCMA packet: 160 samples queued");
    CHECK((S.first[3] != 0 || S.first[7] != 0), "decoded audio, not zeros");
    uint8_t bad[12] = { 0x80, 96 };
    talk_rtp(&t, bad, sizeof(bad), 1040 * MS);
    CHECK(t.rtp.dropped == 1 && S.pushed == 320, "a dropped packet queues nothing and is counted");
    send_pkt(&t, 8, 11, -20.0, 1040 * MS);
    CHECK(t.rtp.dropped == 2 && S.pushed == 320, "duplicate sequence dropped");
    send_pkt(&t, 8, 14, -20.0, 1060 * MS);
    CHECK(t.rtp.lost == 2, "two missing packets counted as lost");

    talk_end(&t);
    CHECK(!t.active && !S.claimed, "end frees the speaker");
    talk_end(&t);
    CHECK(!S.claimed, "end is idempotent");
    size_t p = S.pushed;
    send_pkt(&t, 0, 15, -20.0, 1100 * MS);
    CHECK(S.pushed == p, "after the end packets are ignored");
    CHECK(talk_begin(&t, "192.0.2.9") == 0 && t.rtp.packets == 0 && t.sessions == 2, "next session: fresh counters, session count 2");
    talk_end(&t);
}

static void test_half_duplex(void)
{
    printf("# half duplex gate\n");
    talk_t t;
    reset(&t);
    talk_begin(&t, "192.0.2.7");
    int64_t now = 10000 * MS;
    int16_t mic[256];
    for (int i = 0; i < 256; i++) mic[i] = 1234;

    /* 5 s of continuous silence frames, like go2rtc keeping the channel open */
    S.queued = 0;
    for (int i = 0; i < 250; i++) {
        send_pkt(&t, 0, (uint16_t)i, -120.0, now + i * 20 * MS);
        S.queued = 0;
    }
    int16_t m2[256]; memcpy(m2, mic, sizeof(mic));
    CHECK(!talk_mic_muted(&t, now + 5000 * MS), "5 s of silence frames: microphone never muted");
    CHECK(talk_gate_mic(&t, m2, 256, now + 5000 * MS) == 0 && !memcmp(m2, mic, sizeof(mic)), "gate leaves the samples alone");

    now += 5000 * MS;
    send_pkt(&t, 0, 250, -20.0, now);
    S.queued = 0;
    CHECK(talk_mic_muted(&t, now), "speech: muted");
    memcpy(m2, mic, sizeof(mic));
    CHECK(talk_gate_mic(&t, m2, 256, now) == 1, "gate reports the mute");
    int allzero = 1; for (int i = 0; i < 256; i++) if (m2[i]) allzero = 0;
    CHECK(allzero, "muted period is exact silence, same length");
    CHECK(talk_mic_muted(&t, now + 299 * MS), "still muted 299 ms after the last speech");
    CHECK(!talk_mic_muted(&t, now + 300 * MS), "open again 300 ms after (queue was empty)");

    for (int i = 1; i <= 10; i++) { send_pkt(&t, 0, (uint16_t)(250 + i), -20.0, now + i * 20 * MS); S.queued = 0; }
    CHECK(talk_mic_muted(&t, now + 450 * MS) && !talk_mic_muted(&t, now + 500 * MS), "continuous speech extends the mute to 300 ms after its end");
    for (int i = 11; i <= 20; i++) { send_pkt(&t, 0, (uint16_t)(250 + i), -120.0, now + i * 20 * MS); S.queued = 0; }
    CHECK(!talk_mic_muted(&t, now + 600 * MS), "silence frames after speech do not extend it");

    /* queued audio is heard later: the gate holds that long as well */
    now += 10000 * MS;
    S.queued = 960; /* 120 ms in front */
    send_pkt(&t, 0, 300, -20.0, now);
    CHECK(talk_mic_muted(&t, now + 419 * MS) && !talk_mic_muted(&t, now + 420 * MS), "120 ms queued in front: gate holds 300 + 120 ms");

    talk_end(&t);
    now += 20000 * MS;
    t.active = 0;
    talk_begin(&t, "192.0.2.7");
    send_pkt(&t, 0, 400, -20.0, now);
    talk_end(&t);
    CHECK(talk_mic_muted(&t, now + 100 * MS), "the hold outlives the session (speech already played)");
    CHECK(!talk_mic_muted(&t, now + 1000 * MS), "and ends");

    /* full duplex: never muted */
    reset(&t);
    S.half = 0;
    talk_begin(&t, "192.0.2.7");
    S.queued = 0;
    send_pkt(&t, 0, 1, -10.0, now);
    CHECK(!talk_mic_muted(&t, now), "audio.talk_duplex=full: never muted, even for loud speech");
    talk_end(&t);
}

static void test_tail(void)
{
    printf("# tail of an utterance\n");
    talk_t t;
    reset(&t);
    talk_begin(&t, "192.0.2.7");
    send_pkt(&t, 0, 1, -20.0, 0);          /* 160 samples, below the 960 pre-fill */
    talk_tick(&t, 200 * MS);
    CHECK(S.pushed == 160, "200 ms of quiet: not flushed yet");
    talk_tick(&t, 250 * MS);
    CHECK(S.pushed == JITTER_PREFILL_SAMPLES && S.queued == JITTER_PREFILL_SAMPLES, "250 ms of quiet: topped up to the pre-fill so it plays");
    talk_tick(&t, 900 * MS);
    CHECK(S.pushed == JITTER_PREFILL_SAMPLES, "flushed once");
    send_pkt(&t, 0, 2, -20.0, 1000 * MS);
    S.queued = 200;
    talk_end(&t);
    CHECK(S.queued == JITTER_PREFILL_SAMPLES, "end flushes the tail too");

    reset(&t);
    talk_begin(&t, "192.0.2.7");
    S.queued = 2000;
    send_pkt(&t, 0, 1, -20.0, 0);
    size_t pushed = S.pushed;
    talk_tick(&t, 500 * MS);
    CHECK(S.pushed == pushed, "enough already queued: nothing added");
    talk_end(&t);
    reset(&t);
    talk_begin(&t, "192.0.2.7");
    talk_end(&t);
    CHECK(S.pushed == 0, "a session without audio pushes nothing");
}

static void collect(void *arg, const char *line) { (void)line; (*(int *)arg)++; }

static void test_ctl(void)
{
    printf("# 8091 keys\n");
    char out[96], msg[96];
    CHECK(talk_ctl_get("audio.talk_state", out, sizeof(out)) == 0 && !strcmp(out, "audio.talk_state=idle"), "state idle");
    CHECK(talk_ctl_get("audio.talk_codec", out, sizeof(out)) == 0 && !strcmp(out, "audio.talk_codec=none"), "codec none");
    CHECK(talk_ctl_get("audio.talk_packets", out, sizeof(out)) == 0 && !strcmp(out, "audio.talk_packets=0"), "packets 0");
    CHECK(talk_ctl_get("audio.talk_mic_muted", out, sizeof(out)) == 0 && !strcmp(out, "audio.talk_mic_muted=0"), "mic not muted");
    CHECK(talk_ctl_get("audio.talk_enable", out, sizeof(out)) == -1, "audio.talk_enable is audio_out.c's");
    CHECK(talk_ctl_get("ae.speed", out, sizeof(out)) == -1, "foreign key: -1");
    CHECK(talk_ctl_set("audio.talk_packets", "5", msg, sizeof(msg)) == 1 && strstr(msg, "read-only"), "counters are read-only");
    CHECK(talk_ctl_set("audio.talk_enable", "0", msg, sizeof(msg)) == -1, "settings are not handled here");
    int n = 0;
    talk_ctl_list(collect, &n);
    CHECK(n == 7, "LIST emits seven talk lines");

    /* the singleton works on the real audio_out engine, which is not started here */
    talk_t *g = talk_global();
    CHECK(talk_begin(g, "192.0.2.7") == 0, "singleton: begin");
    CHECK(talk_ctl_get("audio.talk_state", out, sizeof(out)) == 0 && !strcmp(out, "audio.talk_state=active"), "state active");
    CHECK(talk_begin(talk_global(), "192.0.2.8") == -1, "singleton: one talker");
    talk_end(g);
    CHECK(talk_ctl_get("audio.talk_sessions", out, sizeof(out)) == 0 && !strcmp(out, "audio.talk_sessions=1"), "sessions counted");
}

static uint32_t rng = 777;
static uint32_t rnd(void) { rng = rng * 1664525u + 1013904223u; return rng >> 8; }

static void test_fuzz(void)
{
    printf("# fuzz\n");
    talk_t t;
    reset(&t);
    talk_begin(&t, "192.0.2.7");
    for (int i = 0; i < 100000; i++) {
        size_t len = rnd() % 300;
        uint8_t *b = malloc(len ? len : 1);
        for (size_t k = 0; k < len; k++) b[k] = (uint8_t)rnd();
        if (len >= 12 && (rnd() & 1)) { b[0] = (uint8_t)((b[0] & 0x3f) | 0x80); b[1] = (uint8_t)((rnd() & 1) ? 0 : 8); }
        talk_rtp(&t, b, len, (int64_t)i * 20 * MS);
        if (i % 7 == 0) { talk_tick(&t, (int64_t)i * 20 * MS + 300 * MS); S.queued = rnd() % 4000; }
        free(b);
    }
    CHECK(t.rtp.packets + t.rtp.dropped == 100000, "100000 random packets: each accepted or dropped");
    talk_end(&t);
}

int main(void)
{
    test_session();
    test_half_duplex();
    test_tail();
    test_ctl();
    test_fuzz();
    printf(fails ? "\nFAILED: %d\n" : "\ntalk_test: all passed\n", fails);
    return fails ? 1 : 0;
}
