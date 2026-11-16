#include "akam2k_stream.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char wake_byte = 1;

/* True when the access unit carries an IDR slice. SPS/PPS/SEI precede the slice, so only the head is walked. */
static int au_is_key(const uint8_t *d, size_t n)
{
    size_t lim = n < 512 ? n : 512, i;
    for (i = 0; i + 3 < lim; i++) {
        if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) {
            unsigned t = d[i + 3] & 0x1f;
            if (t == 5)
                return 1;
            if (t >= 1 && t <= 4)
                return 0; /* a non-IDR slice comes first: not a keyframe */
            i += 2;
        }
    }
    return 0;
}

void a2k_stream_init(a2k_stream_t *s, const char *name, int width, int height, int fps, int gop, int kbps,
                    size_t max_frame)
{
    memset(s, 0, sizeof *s);
    pthread_mutex_init(&s->mu, NULL);
    s->name = name;
    s->width = width;
    s->height = height;
    s->fps = fps;
    s->gop = gop;
    s->kbps = kbps;
    s->max_frame = max_frame;
    rtp_vclock_init(&s->vclock, 1000000.0 / (fps > 0 ? fps : 15));
}

void a2k_stream_destroy(a2k_stream_t *s)
{
    int i;
    for (i = 0; i < A2K_RING; i++) {
        free(s->ring[i]);
        s->ring[i] = NULL;
    }
    pthread_mutex_destroy(&s->mu);
}

int a2k_stream_publish(a2k_stream_t *s, const uint8_t *data, size_t len)
{
    a2k_frame_t *f, *old;
    int i, key, found = 0;
    h264_params_t p;

    if (!data || len == 0 || len > s->max_frame) {
        pthread_mutex_lock(&s->mu);
        s->oversized++;
        pthread_mutex_unlock(&s->mu);
        a2k_stream_want_idr(s); /* a hole in the P-frame chain: ask for a keyframe to heal it */
        return -1;
    }
    f = malloc(sizeof *f + len);
    if (!f) {
        a2k_stream_want_idr(s);
        return -1;
    }
    memcpy(f->data, data, len);
    f->len = (uint32_t)len;
    key = au_is_key(f->data, len);
    f->key = key;
    f->refs = 1; /* the ring's */
    f->capture_us = rtp_vclock_stamp(&s->vclock, rtp_mono_us());
    memset(&p, 0, sizeof p);
    if (key)
        found = h264_params_scan(&p, f->data, len, 256);

    pthread_mutex_lock(&s->mu);
    f->seq = s->head++;
    old = s->ring[f->seq % A2K_RING];
    s->ring[f->seq % A2K_RING] = f;
    if (old && --old->refs == 0)
        free(old);
    if (found & H264_FOUND_SPS) {
        s->params.sps_len = p.sps_len;
        memcpy(s->params.sps, p.sps, p.sps_len);
    }
    if (found & H264_FOUND_PPS) {
        s->params.pps_len = p.pps_len;
        memcpy(s->params.pps, p.pps, p.pps_len);
    }
    s->frames++;
    s->bytes += len;
    if (key)
        s->keys++;
    for (i = 0; i < s->nwake; i++)
        if (write(s->wake_fd[i], &wake_byte, 1) < 0) { /* full pipe: the client is already awake */ }
    pthread_mutex_unlock(&s->mu);
    return 0;
}

a2k_frame_t *a2k_stream_next(a2k_stream_t *s, uint64_t *cursor, int *lost)
{
    a2k_frame_t *f = NULL;
    uint64_t oldest;

    *lost = 0;
    pthread_mutex_lock(&s->mu);
    oldest = s->head > A2K_RING ? s->head - A2K_RING : 0;
    if (*cursor < oldest) {
        *cursor = oldest;
        *lost = 1;
    }
    if (*cursor < s->head) {
        f = s->ring[*cursor % A2K_RING];
        f->refs++;
        (*cursor)++;
    }
    pthread_mutex_unlock(&s->mu);
    return f;
}

void a2k_frame_unref(a2k_stream_t *s, a2k_frame_t *f)
{
    int dead;
    pthread_mutex_lock(&s->mu);
    dead = (--f->refs == 0);
    pthread_mutex_unlock(&s->mu);
    if (dead)
        free(f);
}

int a2k_stream_latest_key(a2k_stream_t *s, uint64_t *seq)
{
    int ok = 0;
    uint64_t q, oldest;
    pthread_mutex_lock(&s->mu);
    oldest = s->head > A2K_RING ? s->head - A2K_RING : 0;
    for (q = s->head; q > oldest; q--)
        if (s->ring[(q - 1) % A2K_RING]->key) {
            *seq = q - 1;
            ok = 1;
            break;
        }
    pthread_mutex_unlock(&s->mu);
    return ok;
}

uint64_t a2k_stream_head(a2k_stream_t *s)
{
    uint64_t h;
    pthread_mutex_lock(&s->mu);
    h = s->head;
    pthread_mutex_unlock(&s->mu);
    return h;
}

void a2k_stream_params(a2k_stream_t *s, h264_params_t *out)
{
    pthread_mutex_lock(&s->mu);
    *out = s->params;
    pthread_mutex_unlock(&s->mu);
}

void a2k_stream_want_idr(a2k_stream_t *s) { s->idr_req = 1; }

int a2k_stream_take_idr(a2k_stream_t *s)
{
    int r = s->idr_req;
    if (r)
        s->idr_req = 0;
    return r;
}

int a2k_stream_wake_add(a2k_stream_t *s, int wfd)
{
    int ok = -1;
    pthread_mutex_lock(&s->mu);
    if (s->nwake < A2K_MAX_WAKE) {
        s->wake_fd[s->nwake++] = wfd;
        ok = 0;
    }
    pthread_mutex_unlock(&s->mu);
    return ok;
}

void a2k_stream_wake_del(a2k_stream_t *s, int wfd)
{
    int i;
    pthread_mutex_lock(&s->mu);
    for (i = 0; i < s->nwake; i++)
        if (s->wake_fd[i] == wfd) {
            s->wake_fd[i] = s->wake_fd[--s->nwake];
            break;
        }
    pthread_mutex_unlock(&s->mu);
}
