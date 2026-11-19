#pragma once
/* Lead-in buffer: the last seconds of encoded video, kept in RAM while no client is playing (not in upstream
 * akam-fw). A recorder that connects because the camera reported motion (md.h) is first sent these frames, so
 * its recording starts before the movement, then the live frames follow without a gap.
 *
 * This file is the container only, with no I/O (host-tested: tests/native/preroll_test.c). Frames are copied
 * into one fixed arena used as a ring; the oldest are overwritten. Single-threaded: only the RTSP thread uses it. */
#include <stddef.h>
#include <stdint.h>

#define PR_MAX_FRAMES 512

typedef struct {
    uint32_t off, len;
    int64_t  capture_us;
    uint8_t  key;
} pr_frame_t;

typedef struct {
    uint8_t   *arena;
    size_t     cap;
    size_t     w;        /* next write offset */
    pr_frame_t f[PR_MAX_FRAMES];
    unsigned   head, count;
    int64_t    max_age_us;
    int        playing;  /* between pr_begin() and pr_clear(): frames are no longer dropped for their age */
    int        need_key; /* an unsent frame was overwritten: playout resumes at the next keyframe */
} pr_t;

void pr_init(pr_t *p, uint8_t *arena, size_t cap, int64_t max_age_us);
void pr_clear(pr_t *p);

/* Appends a frame (copied). */
void pr_push(pr_t *p, const uint8_t *data, size_t len, int64_t capture_us, int key);

/* Starts a playout: drops everything before the keyframe that gives at least window_us of lead-in (or the
 * oldest keyframe when the buffer is shorter). Returns the number of frames to play, 0 when there is no
 * keyframe (the buffer is then empty). */
unsigned pr_begin(pr_t *p, int64_t now_us, int64_t window_us);

/* Oldest frame to send, NULL when there is none. The pointer is valid until the next call of any function. */
const pr_frame_t *pr_peek(pr_t *p);
void pr_pop(pr_t *p);
