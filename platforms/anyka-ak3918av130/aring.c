#include "aring.h"

#include <string.h>
#include <unistd.h>

static const char wake_byte = 1;

void a2k_aring_init(a2k_aring_t *r)
{
    memset(r, 0, sizeof *r);
    pthread_mutex_init(&r->mu, NULL);
}

void a2k_aring_destroy(a2k_aring_t *r) { pthread_mutex_destroy(&r->mu); }

int a2k_aring_publish(a2k_aring_t *r, const int16_t *pcm, size_t n, int64_t t_end_us)
{
    a2k_aslot_t *s;
    int i;

    if (!pcm || n == 0)
        return -1;
    pthread_mutex_lock(&r->mu);
    if (n > A2K_APERIOD_SAMPLES) {
        n = A2K_APERIOD_SAMPLES;
        r->truncated++;
    }
    s = &r->slot[r->head % A2K_ASLOTS];
    memcpy(s->pcm, pcm, n * sizeof(int16_t));
    s->n = (uint16_t)n;
    s->first_sample = r->samples_total;
    s->t_end_us = t_end_us;
    r->samples_total += n;
    r->head++;
    r->periods++;
    for (i = 0; i < r->nwake; i++)
        if (write(r->wake_fd[i], &wake_byte, 1) < 0) { /* full pipe: the reader is already awake */ }
    pthread_mutex_unlock(&r->mu);
    return 0;
}

void a2k_aring_gap(a2k_aring_t *r, int64_t gap_us)
{
    if (gap_us <= 0)
        return;
    pthread_mutex_lock(&r->mu);
    r->samples_total += (uint64_t)gap_us * A2K_AUDIO_RATE / 1000000;
    pthread_mutex_unlock(&r->mu);
}

int a2k_aring_next(a2k_aring_t *r, uint64_t *cursor, int16_t *pcm, size_t *n, uint64_t *first_sample,
                   int64_t *t_end_us, int *lost)
{
    int got = 0;
    uint64_t oldest;

    *lost = 0;
    pthread_mutex_lock(&r->mu);
    oldest = r->head > A2K_ASLOTS ? r->head - A2K_ASLOTS : 0;
    if (*cursor < oldest) {
        *cursor = oldest;
        *lost = 1;
    }
    if (*cursor < r->head) {
        const a2k_aslot_t *s = &r->slot[*cursor % A2K_ASLOTS];
        memcpy(pcm, s->pcm, (size_t)s->n * sizeof(int16_t));
        *n = s->n;
        *first_sample = s->first_sample;
        *t_end_us = s->t_end_us;
        (*cursor)++;
        got = 1;
    }
    pthread_mutex_unlock(&r->mu);
    return got;
}

uint64_t a2k_aring_head(a2k_aring_t *r)
{
    uint64_t h;
    pthread_mutex_lock(&r->mu);
    h = r->head;
    pthread_mutex_unlock(&r->mu);
    return h;
}

int a2k_aring_wake_add(a2k_aring_t *r, int wfd)
{
    int ok = -1;
    pthread_mutex_lock(&r->mu);
    if (r->nwake < A2K_AMAX_WAKE) {
        r->wake_fd[r->nwake++] = wfd;
        ok = 0;
    }
    pthread_mutex_unlock(&r->mu);
    return ok;
}

void a2k_aring_wake_del(a2k_aring_t *r, int wfd)
{
    int i;
    pthread_mutex_lock(&r->mu);
    for (i = 0; i < r->nwake; i++)
        if (r->wake_fd[i] == wfd) {
            r->wake_fd[i] = r->wake_fd[--r->nwake];
            break;
        }
    pthread_mutex_unlock(&r->mu);
}

void a2k_audio_pack_be(uint8_t *out, const int16_t *pcm, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        uint16_t v = (uint16_t)pcm[i];
        out[2 * i] = (uint8_t)(v >> 8);
        out[2 * i + 1] = (uint8_t)(v & 0xff);
    }
}

uint32_t a2k_audio_rtp_ts(uint32_t base, uint64_t first_sample) { return base + (uint32_t)first_sample; }
