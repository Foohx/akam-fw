/* Host test of src/ak_rtsp/preroll.c (run by tests/preroll_test.sh). */
#include <stdio.h>
#include <string.h>

#include "preroll.h"

#define CHECK(c) do { if (!(c)) { printf("preroll_test: FAILED line %d: %s\n", __LINE__, #c); return 1; } } while (0)

static uint8_t arena[10000];
static pr_t p;

/* Frame n: a keyframe every 30, 15 frames a second; its bytes are all (n & 255). */
static void push(int n, size_t len)
{
    static uint8_t buf[4000];
    memset(buf, n & 255, len);
    pr_push(&p, buf, len, (int64_t)n * 66667, n % 30 == 0);
}

static int intact(const pr_frame_t *f, int n)
{
    for (uint32_t i = 0; i < f->len; i++)
        if (arena[f->off + i] != (uint8_t)(n & 255)) return 0;
    return f->capture_us == (int64_t)n * 66667 && f->key == (n % 30 == 0);
}

int main(void)
{
    /* 12 s of age allowed; the arena is the limit here: about 90 frames of 100 bytes */
    pr_init(&p, arena, sizeof(arena), 12000000);
    for (int n = 0; n < 1000; n++) push(n, n % 30 == 0 ? 400 : 100);
    CHECK(p.count > 60 && p.count < 100);

    /* a 2 s window: starts on the keyframe at or before 2 s ago (frame 960 at 64.0 s; now is 66.6 s) */
    unsigned total = pr_begin(&p, (int64_t)999 * 66667, 2000000);
    CHECK(total == 40);
    for (int n = 960; n < 1000; n++) {
        const pr_frame_t *f = pr_peek(&p);
        CHECK(f && intact(f, n));
        pr_pop(&p);
    }
    CHECK(pr_peek(&p) == NULL);

    /* a window longer than the buffer: the oldest keyframe */
    pr_clear(&p);
    for (int n = 0; n < 1000; n++) push(n, n % 30 == 0 ? 400 : 100);
    total = pr_begin(&p, (int64_t)999 * 66667, 60000000);
    CHECK(total >= 70 && pr_peek(&p)->key && pr_peek(&p)->capture_us == (int64_t)930 * 66667);

    /* live frames keep arriving during the playout and come out in order */
    pr_pop(&p);
    push(1000, 100);
    push(1001, 100);
    int n = 931;
    for (const pr_frame_t *f; (f = pr_peek(&p)) != NULL; pr_pop(&p), n++) CHECK(intact(f, n));
    CHECK(n == 1002);

    /* a playout that cannot keep up loses frames: it resumes on a keyframe, never in the middle of a group */
    pr_clear(&p);
    for (n = 0; n < 100; n++) push(n, n % 30 == 0 ? 400 : 100);
    CHECK(pr_begin(&p, (int64_t)99 * 66667, 60000000) > 0);
    for (n = 100; n < 400; n++) push(n, n % 30 == 0 ? 400 : 100);
    const pr_frame_t *f = pr_peek(&p);
    CHECK(f && f->key && f->capture_us > (int64_t)300 * 66667);
    int first = (int)(f->capture_us / 66667 + 1) / 30 * 30;
    for (n = first; (f = pr_peek(&p)) != NULL; pr_pop(&p), n++) CHECK(intact(f, n));
    CHECK(n == 400);

    /* no keyframe at all: nothing to play */
    pr_clear(&p);
    for (n = 1; n < 20; n++) push(n, 100);
    CHECK(pr_begin(&p, 20 * 66667, 2000000) == 0 && pr_peek(&p) == NULL);

    /* old frames leave by age while nobody plays */
    pr_init(&p, arena, sizeof(arena), 1000000);
    for (n = 0; n < 60; n++) push(n, 50);
    CHECK(p.count >= 15 && p.count <= 17);

    /* a frame larger than the arena empties it */
    static uint8_t big[20000];
    pr_push(&p, big, sizeof(big), 0, 1);
    CHECK(p.count == 0);

    puts("preroll_test: ok");
    return 0;
}
