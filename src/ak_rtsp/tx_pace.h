#pragma once
/* Backpressure pacing for the RTSP/TCP video stream.
 *
 * Problem: a blocking write of a frame to a slow client keeps the encoder's external stream buffer held
 * and unreleased; on a weak link the encoder then overflows, re-encodes at maximum QP and loses frames
 * for everybody. The fix is to keep draining the encoder at the sensor rate whatever the client does and to
 * drop frames for that client instead.
 *
 * Policy: before a frame is written, the unsent bytes of the TCP socket (the "backlog", SIOCOUTQ) are
 * compared with a high watermark. Above it the stream enters SKIP mode: every frame is dropped (and
 * released at once) until a keyframe arrives while the backlog is back under a low watermark; that
 * keyframe is sent and the stream resumes. A P-frame is therefore never sent after a dropped frame
 * without a keyframe in between (decoder references stay intact).
 *
 *   base   = max(TX_PACE_MIN_HIGH, 0.5 s of the configured bitrate)        (latency bound)
 *   high   = base + size of the last keyframe                              (a keyframe in flight is
 *                                                                            not congestion)
 *   low    = base / 4
 *
 * Pure logic (no I/O, no clock besides the injected one): tests/tx_pace_test.sh. */
#include <stddef.h>
#include <stdint.h>

#define TX_PACE_MIN_HIGH   (64u * 1024u)   /* base never below this */
#define TX_PACE_IKEY_CAP   (256u * 1024u)  /* keyframe size counted into the high mark is capped */
#define TX_PACE_LOG_MS     10000           /* at most one enter/leave log pair per this long */

typedef enum { TX_PACE_SEND = 0, TX_PACE_SKIP } tx_pace_verdict_t;

typedef struct {
    int      skipping;
    size_t   key_len;        /* size of the latest keyframe seen (sent or not) */
    unsigned run_skipped;    /* frames skipped in the current SKIP run */
    int      run_logged;     /* the enter line of the current run was logged */
    int64_t  last_log_ms;
    int      have_log;
} tx_pace_t;

typedef struct {
    int entered;   /* this call switched to SKIP mode */
    int left;      /* this call switched back to normal; run_skipped = frames dropped in that run */
    unsigned run_skipped;
    int log_it;    /* the caller may print one line for this event (rate limited) */
} tx_pace_event_t;

void   tx_pace_init(tx_pace_t *p);
/* Base watermark in bytes for a configured bitrate in kbit/s. */
size_t tx_pace_base(int kbps);
size_t tx_pace_high(const tx_pace_t *p, size_t base);
size_t tx_pace_low(size_t base);

/* Decide one video frame. backlog = unsent bytes in the socket before the write, frame_len/is_key
 * describe the frame, base from tx_pace_base(). ev may be NULL. */
tx_pace_verdict_t tx_pace_decide(tx_pace_t *p, size_t backlog, size_t frame_len, int is_key, size_t base,
                                 int64_t now_ms, tx_pace_event_t *ev);

/* Unsent bytes in the socket send queue of a TCP fd, 0 when unknown (then no pacing happens). */
size_t tx_pace_backlog(int fd);
