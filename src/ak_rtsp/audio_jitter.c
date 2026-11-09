/* audio_jitter.c -- see audio_jitter.h. */
#include "audio_jitter.h"

#include <string.h>

void audio_jitter_init(audio_jitter_t *j)
{
    memset(j, 0, sizeof(*j));
    pthread_mutex_init(&j->lock, NULL);
}

void audio_jitter_destroy(audio_jitter_t *j)
{
    pthread_mutex_destroy(&j->lock);
}

void audio_jitter_reset(audio_jitter_t *j)
{
    pthread_mutex_lock(&j->lock);
    j->head = 0;
    j->level = 0;
    j->playing = 0;
    pthread_mutex_unlock(&j->lock);
}

size_t audio_jitter_push(audio_jitter_t *j, const int16_t *in, size_t n)
{
    size_t total = n, i, tail;

    pthread_mutex_lock(&j->lock);
    if (j->level + n > JITTER_MAX_SAMPLES) {
        /* Keep only the newest JITTER_PREFILL_SAMPLES of (queued + new). */
        size_t discard = j->level + n - JITTER_PREFILL_SAMPLES;

        if (discard >= j->level) {
            size_t skip = discard - j->level;

            j->drops += (uint32_t)discard;
            in += skip;
            n -= skip;
            j->head = 0;
            j->level = 0;
        } else {
            j->drops += (uint32_t)discard;
            j->head = (j->head + discard) % JITTER_MAX_SAMPLES;
            j->level -= discard;
        }
    }
    tail = (j->head + j->level) % JITTER_MAX_SAMPLES;
    for (i = 0; i < n; i++) {
        j->buf[tail] = in[i];
        if (++tail == JITTER_MAX_SAMPLES)
            tail = 0;
    }
    j->level += n;
    if (!j->playing && j->level >= JITTER_PREFILL_SAMPLES)
        j->playing = 1;
    pthread_mutex_unlock(&j->lock);
    return total;
}

size_t audio_jitter_pull(audio_jitter_t *j, int16_t *out, size_t n)
{
    size_t got = 0, i;

    pthread_mutex_lock(&j->lock);
    if (j->playing) {
        got = j->level < n ? j->level : n;
        for (i = 0; i < got; i++) {
            out[i] = j->buf[j->head];
            if (++j->head == JITTER_MAX_SAMPLES)
                j->head = 0;
        }
        j->level -= got;
        if (got < n) {
            j->underruns++;
            j->playing = 0;
        }
    }
    pthread_mutex_unlock(&j->lock);
    if (got < n)
        memset(out + got, 0, (n - got) * sizeof(*out));
    return got;
}

size_t audio_jitter_level(audio_jitter_t *j)
{
    size_t v;

    pthread_mutex_lock(&j->lock);
    v = j->level;
    pthread_mutex_unlock(&j->lock);
    return v;
}

size_t audio_jitter_level_ms(audio_jitter_t *j)
{
    return audio_jitter_level(j) * 1000 / JITTER_RATE_HZ;
}

uint32_t audio_jitter_underruns(audio_jitter_t *j)
{
    uint32_t v;

    pthread_mutex_lock(&j->lock);
    v = j->underruns;
    pthread_mutex_unlock(&j->lock);
    return v;
}

uint32_t audio_jitter_drops(audio_jitter_t *j)
{
    uint32_t v;

    pthread_mutex_lock(&j->lock);
    v = j->drops;
    pthread_mutex_unlock(&j->lock);
    return v;
}
