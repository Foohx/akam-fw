/* Microphone period hand-off between the capture thread and the RTSP client threads of the Rotatable 2K own app, and
 * the two pure helpers of the audio RTP track (payload byte order, timestamp).
 *
 * One ring, one producer (the capture thread of akam2k_audio.c), any number of readers. Fixed memory: A2K_ASLOTS
 * periods of at most A2K_APERIOD_SAMPLES samples (32 slots x 256 samples x 2 bytes, about 17 KiB, 1.0 s of audio at
 * 8 kHz). A reader owns a cursor (a sequence number): one that fell more than A2K_ASLOTS periods behind is told
 * ("lost") and continues at the oldest period still held, so the producer is never blocked by a slow client (the
 * oldest audio is overwritten). The mutex is held only for a 512-byte copy.
 *
 * Every period carries the index of its first sample on the capture stream (first_sample: it keeps counting over
 * gaps, see a2k_aring_gap) and the CLOCK_MONOTONIC time at which its read() returned (t_end_us), exactly the data the
 * 1080p audio.c hands to rtsp.c, so the RTP side can use rtp_astart_t / rtp_aclock_t of src/common/net/rtp_clock.h with
 * the same semantics.
 *
 * No device, no socket, no vendor call: plain pthread code, host-testable (tests/rot2k_audio_unit.c). */
#ifndef AKAM2K_ARING_H
#define AKAM2K_ARING_H
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

#define A2K_AUDIO_RATE 8000          /* the L16/8000 track of rtsp_sdp.c, mono, 16 bit */
#define A2K_APERIOD_SAMPLES 256      /* one 512-byte capture period (32 ms) */
#define A2K_ASLOTS 32
#define A2K_AMAX_WAKE 8              /* readers that can wait on the ring (2 streams x 4 clients would be 8) */

typedef struct {
    uint64_t first_sample;           /* index of pcm[0] on the capture stream */
    int64_t t_end_us;                /* CLOCK_MONOTONIC when the period's last sample had just been read */
    uint16_t n;                      /* samples in pcm (1..A2K_APERIOD_SAMPLES) */
    int16_t pcm[A2K_APERIOD_SAMPLES];
} a2k_aslot_t;

typedef struct {
    pthread_mutex_t mu;
    a2k_aslot_t slot[A2K_ASLOTS];
    uint64_t head;                   /* sequence number of the next period to be published */
    uint64_t samples_total;          /* capture stream position: index of the next sample */
    int wake_fd[A2K_AMAX_WAKE];
    int nwake;
    unsigned long periods, truncated; /* counters, guarded by mu */
} a2k_aring_t;

void a2k_aring_init(a2k_aring_t *r);
void a2k_aring_destroy(a2k_aring_t *r);

/* Producer: stores one period (n samples, 1..A2K_APERIOD_SAMPLES; more are cut and counted) read at t_end_us, and
 * wakes every registered reader. Returns 0, or -1 for an empty period. */
int a2k_aring_publish(a2k_aring_t *r, const int16_t *pcm, size_t n, int64_t t_end_us);
/* Producer: the capture was down for gap_us; the stream position advances by that time in samples, so the RTP
 * timestamp of the next period jumps over the hole instead of overlapping the audio before it (as audio.c does). */
void a2k_aring_gap(a2k_aring_t *r, int64_t gap_us);

/* Reader: the period at *cursor copied into pcm (room for A2K_APERIOD_SAMPLES) -> 1 and the cursor advances; 0 when
 * nothing new. *lost is set when periods were overwritten before this reader took them (the cursor then jumps to the
 * oldest one still held). */
int a2k_aring_next(a2k_aring_t *r, uint64_t *cursor, int16_t *pcm, size_t *n, uint64_t *first_sample,
                   int64_t *t_end_us, int *lost);
uint64_t a2k_aring_head(a2k_aring_t *r);

/* Wake pipe registration: a byte is written to wfd (non-blocking) whenever a period is published. 0, or -1 when full. */
int a2k_aring_wake_add(a2k_aring_t *r, int wfd);
void a2k_aring_wake_del(a2k_aring_t *r, int wfd);

/* RFC 3551 L16: samples go out in network byte order. out needs 2*n bytes. Independent of the host byte order. */
void a2k_audio_pack_be(uint8_t *out, const int16_t *pcm, size_t n);
/* RTP timestamp of a period: per-session random origin + the capture-stream sample index (clock rate = sample rate,
 * wraps modulo 2^32). Exactly the 1080p scheme; wall-clock time never enters the packet timestamps. */
uint32_t a2k_audio_rtp_ts(uint32_t base, uint64_t first_sample);

#endif
