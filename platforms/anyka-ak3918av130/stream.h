/* Encoded-frame hand-off between the capture threads and the RTSP client threads of the Rotatable 2K own app.
 *
 * One a2k_stream per encoder channel. The capture thread copies every encoded access unit into a heap frame and
 * publishes it into a small ring (A2K_RING frames); the encoder buffer is released right after the copy, so no
 * client can ever hold the encoder. A client thread keeps its own cursor (a sequence number) and takes frames with
 * a2k_stream_next(): a client that fell more than A2K_RING frames behind is told ("lost") and resynchronises on the
 * next keyframe. Memory is bounded: the ring (A2K_RING frames, each at most the stream's max_frame bytes) plus the frames
 * the clients are sending at that moment (reference counted).
 *
 * No sockets, no vendor calls: plain pthread code, usable from the capture side and the RTSP side alike. The shared
 * hardware-independent helpers (h264_params, rtp_clock) are compiled unmodified from src/net/rtsp. */
#ifndef AKAM2K_STREAM_H
#define AKAM2K_STREAM_H
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

#include "h264_params.h"
#include "rtp_clock.h"

#define A2K_RING 6                 /* frames kept for the clients (about 0.4 s at 15 fps) */
/* Per-stream cap on one access unit: a bigger one is dropped (and a keyframe requested). A 2304x1296 IDR is about
 * 100-250 KB at the default 2000 kbit/s (VBR max 1.25x). Worst case ring memory: 6 x 512 KiB + 6 x 128 KiB =
 * 3.75 MiB (it was 2 x 8 x 1 MiB = 16 MiB against about 22 MB available), plus the frames being sent. */
#define A2K_MAX_FRAME_MAIN (512u << 10)
#define A2K_MAX_FRAME_SUB (128u << 10)
#define A2K_MAX_WAKE 8             /* clients that can wait on one stream */

typedef struct a2k_frame {
    int refs;                      /* guarded by the stream mutex */
    uint32_t len;
    int key;                       /* carries an IDR slice (NAL 5) */
    int64_t capture_us;            /* smoothed capture time, CLOCK_MONOTONIC (rtp_clock.h) */
    uint64_t seq;
    uint8_t data[];                /* Annex B */
} a2k_frame_t;

typedef struct a2k_stream {
    pthread_mutex_t mu;
    const char *name;              /* "stream0" / "stream1" */
    int width, height, fps, gop, kbps;
    size_t max_frame;              /* largest access unit kept, see A2K_MAX_FRAME_* */
    a2k_frame_t *ring[A2K_RING];
    uint64_t head;                 /* sequence number of the next frame to be published */
    h264_params_t params;          /* latest SPS/PPS seen in a keyframe */
    rtp_vclock_t vclock;           /* capture thread only */
    int wake_fd[A2K_MAX_WAKE];     /* write ends of the clients' wake pipes */
    int nwake;
    volatile int idr_req;          /* a client asked for a keyframe: the capture thread calls request_idr */
    /* counters, guarded by mu */
    unsigned long frames, keys, oversized;
    unsigned long long bytes;
} a2k_stream_t;

void a2k_stream_init(a2k_stream_t *s, const char *name, int width, int height, int fps, int gop, int kbps,
                    size_t max_frame);
void a2k_stream_destroy(a2k_stream_t *s);

/* Capture thread: copies one Annex B access unit in. Returns 0, or -1 when it was dropped (empty, oversized,
 * out of memory). Wakes every registered client. */
int a2k_stream_publish(a2k_stream_t *s, const uint8_t *data, size_t len);

/* Client side. next(): the frame at *cursor (a new reference), or NULL when there is none yet. *lost is set when
 * frames were overwritten before the client took them; the cursor then jumps to the oldest frame still held. */
a2k_frame_t *a2k_stream_next(a2k_stream_t *s, uint64_t *cursor, int *lost);
void a2k_frame_unref(a2k_stream_t *s, a2k_frame_t *f);
/* Sequence number of the newest keyframe still in the ring, 1 and *seq set; 0 when there is none. */
int a2k_stream_latest_key(a2k_stream_t *s, uint64_t *seq);
uint64_t a2k_stream_head(a2k_stream_t *s);
/* Copy of the SPS/PPS cache. */
void a2k_stream_params(a2k_stream_t *s, h264_params_t *out);
/* The capture thread polls and clears it; any thread sets it with a2k_stream_want_idr(). */
void a2k_stream_want_idr(a2k_stream_t *s);
int a2k_stream_take_idr(a2k_stream_t *s);
/* Wake pipe registration: a byte is written to wfd (non-blocking) whenever a frame is published. 0 or -1 when full. */
int a2k_stream_wake_add(a2k_stream_t *s, int wfd);
void a2k_stream_wake_del(a2k_stream_t *s, int wfd);

#endif
