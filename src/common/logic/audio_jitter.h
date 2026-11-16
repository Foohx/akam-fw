/*
 * audio_jitter.h -- fixed-memory jitter buffer for 8 kHz mono S16 audio.
 *
 * Producer (network thread) pushes decoded samples; consumer (playback
 * thread) pulls fixed-size periods at the DAC cadence.
 *   - Pre-fill: nothing is delivered (silence) until JITTER_PREFILL_MS of
 *     audio is queued.
 *   - Underrun: the buffer ran dry while playing -> the remainder of the
 *     period is silence, `underruns` is incremented and the buffer goes
 *     back to pre-filling.
 *   - Overrun: more than JITTER_MAX_MS queued -> the oldest samples are
 *     dropped down to JITTER_PREFILL_MS (this also corrects clock drift);
 *     `drops` counts the dropped samples.
 * One producer and one consumer may run concurrently (a small internal
 * mutex guards the indices; neither side ever blocks on I/O). No malloc.
 */
#ifndef AKAM_AUDIO_JITTER_H
#define AKAM_AUDIO_JITTER_H

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

#define JITTER_RATE_HZ    8000
#define JITTER_PREFILL_MS 120
#define JITTER_MAX_MS     400
#define JITTER_PREFILL_SAMPLES (JITTER_RATE_HZ * JITTER_PREFILL_MS / 1000) /* 960 */
#define JITTER_MAX_SAMPLES     (JITTER_RATE_HZ * JITTER_MAX_MS / 1000)     /* 3200 */

typedef struct {
    int16_t         buf[JITTER_MAX_SAMPLES];
    size_t          head;    /* next read index */
    size_t          level;   /* samples queued */
    int             playing; /* 0 = pre-filling */
    uint32_t        underruns;
    uint32_t        drops;   /* samples dropped by overrun */
    pthread_mutex_t lock;
} audio_jitter_t;

void audio_jitter_init(audio_jitter_t *j);
void audio_jitter_destroy(audio_jitter_t *j);
/* Drop all queued audio, back to pre-filling (counters are kept). */
void audio_jitter_reset(audio_jitter_t *j);

/* Queue n samples. Never blocks, never fails: returns n (overrun drops the
 * oldest audio instead). */
size_t audio_jitter_push(audio_jitter_t *j, const int16_t *in, size_t n);

/* Fill out[0..n) with the next n samples. Missing samples are zero.
 * Returns the number of real (non-padding) samples delivered. */
size_t audio_jitter_pull(audio_jitter_t *j, int16_t *out, size_t n);

/* Snapshots (thread-safe). */
size_t   audio_jitter_level(audio_jitter_t *j);    /* samples queued */
size_t   audio_jitter_level_ms(audio_jitter_t *j);
uint32_t audio_jitter_underruns(audio_jitter_t *j);
uint32_t audio_jitter_drops(audio_jitter_t *j);

#endif /* AKAM_AUDIO_JITTER_H */
