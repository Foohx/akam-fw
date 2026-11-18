#include "preroll.h"

#include <string.h>

void pr_init(pr_t *p, uint8_t *arena, size_t cap, int64_t max_age_us)
{
    memset(p, 0, sizeof(*p));
    p->arena = arena;
    p->cap = cap;
    p->max_age_us = max_age_us;
}

void pr_clear(pr_t *p)
{
    p->head = p->count = 0;
    p->w = 0;
    p->playing = 0;
    p->need_key = 0;
}

static void evict(pr_t *p)
{
    p->head = (p->head + 1) % PR_MAX_FRAMES;
    p->count--;
    if (p->playing) p->need_key = 1;
}

void pr_push(pr_t *p, const uint8_t *data, size_t len, int64_t capture_us, int key)
{
    if (!p->arena || len == 0) return;
    if (len > p->cap) { /* cannot be kept: what follows it is not decodable without it */
        while (p->count) evict(p);
        p->need_key = p->playing;
        return;
    }
    if (p->count == 0) p->w = 0;
    if (p->w + len > p->cap) {
        /* Wrap. The frames from here to the end of the arena are the oldest ones. */
        while (p->count && p->f[p->head].off >= p->w) evict(p);
        p->w = 0;
    }
    while (p->count) {
        const pr_frame_t *h = &p->f[p->head];
        if (h->off < p->w + len && (size_t)h->off + h->len > p->w) evict(p);
        else break;
    }
    if (p->count == PR_MAX_FRAMES) evict(p);
    if (!p->playing)
        while (p->count && capture_us - p->f[p->head].capture_us > p->max_age_us) evict(p);

    pr_frame_t *f = &p->f[(p->head + p->count) % PR_MAX_FRAMES];
    f->off = (uint32_t)p->w;
    f->len = (uint32_t)len;
    f->capture_us = capture_us;
    f->key = key ? 1 : 0;
    memcpy(p->arena + p->w, data, len);
    p->w += len;
    p->count++;
}

unsigned pr_begin(pr_t *p, int64_t now_us, int64_t window_us)
{
    int start = -1;
    for (unsigned i = 0; i < p->count; i++) {
        const pr_frame_t *f = &p->f[(p->head + i) % PR_MAX_FRAMES];
        if (!f->key) continue;
        if (start < 0) start = (int)i;                             /* oldest keyframe */
        else if (now_us - f->capture_us >= window_us) start = (int)i; /* a later one that still gives the window */
    }
    if (start < 0) {
        pr_clear(p);
        return 0;
    }
    p->head = (p->head + (unsigned)start) % PR_MAX_FRAMES;
    p->count -= (unsigned)start;
    p->playing = 1;
    p->need_key = 0;
    return p->count;
}

const pr_frame_t *pr_peek(pr_t *p)
{
    if (p->need_key) {
        while (p->count && !p->f[p->head].key) {
            p->head = (p->head + 1) % PR_MAX_FRAMES;
            p->count--;
        }
        if (p->count) p->need_key = 0;
    }
    return p->count ? &p->f[p->head] : NULL;
}

void pr_pop(pr_t *p)
{
    if (!p->count) return;
    p->head = (p->head + 1) % PR_MAX_FRAMES;
    p->count--;
}
