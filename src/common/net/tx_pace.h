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
 * without a keyframe in between (decoder references stay intact), and a P-frame is only ever dropped
 * by entering SKIP mode (never singly outside it).
 *
 *   base   = max(TX_PACE_MIN_HIGH, 0.5 s of the configured bitrate)        (latency bound)
 *   cap    = 3/4 of the socket's SO_SNDBUF as reported by getsockopt (the kernel already doubled the
 *            value that was set; the rest is skb overhead). 0 = unknown, no cap.
 *   high   = min(base + size of the last keyframe, cap - frame_len), never below base/2
 *            (a keyframe in flight is not congestion, but a write that cannot fit in the send buffer
 *            would block while the encoder buffer is held, which is what this unit prevents)
 *   low    = base / 4
 *   A keyframe is admitted in normal mode only at a backlog <= min(base, high): behind a deeper backlog it
 *   would be sent and then followed by P-frames that hit the high mark (a wasted keyframe).
 *   A keyframe at a backlog <= low is always sent, even when it is larger than the buffer (when it fits `room`).
 *
 * Dead or hopeless peers: while skipping, the client is dropped (TX_PACE_DROP_CLIENT, the frame is
 * dropped too) when the backlog is above the low mark and has not reached a new minimum for stall_ms (dead
 * peer: tcp_retries2 alone would take ~15 min; a drained link waiting for its keyframe is not a dead peer), or when the skip run lasts longer than max_skip_ms (the link cannot carry
 * the stream; no force-IDR ioctl exists in venc.c, so waiting for a lucky keyframe is all there is).
 * The server is single-client: freeing the slot beats a frozen picture.
 *
 * Private send queue (net_tx.h): for RTSP/TCP the writer never blocks. Bytes the kernel does not take at
 * once wait in a per-client queue (256 KiB, allocated at PLAY) that the client loop drains with POLLOUT, so
 * the encoder buffer is released as soon as the frame has been serialised, whatever the link does. A keyframe
 * larger than the free kernel buffer therefore no longer blocks the encoder. The "backlog" given to
 * tx_pace_decide() is the kernel backlog plus the queued bytes, and `cap` is the kernel cap plus the queue
 * size. `room` (given to tx_pace_decide) is what is left of both; a frame whose wire size does not fit it is
 * dropped like a frame over the high mark (SKIP mode) and is never approved to leave SKIP mode, so the run
 * clocks keep running for a keyframe that never fits and max_skip_ms ends the session. A queue that makes no
 * progress for the stall budget ends the session like a stalled blocking write.
 *
 * Pure logic (no I/O, no clock besides the injected one): tests/tx_pace_test.sh. */
#include <stddef.h>
#include <stdint.h>

#define TX_PACE_MIN_HIGH   (64u * 1024u)   /* base never below this */
#define TX_PACE_IKEY_CAP   (256u * 1024u)  /* keyframe size counted into the high mark is capped */
#define TX_PACE_STALL_MS   15000           /* same as TX_STALL_BUDGET_MS in rtsp.c */
#define TX_PACE_LOG_MS     10000           /* at most one enter/leave log pair per this long */

typedef enum { TX_PACE_SEND = 0, TX_PACE_SKIP, TX_PACE_DROP_CLIENT } tx_pace_verdict_t;

typedef struct {
    int      skipping;
    size_t   key_len;        /* size of the latest keyframe seen (sent or not) */
    unsigned run_skipped;    /* frames skipped in the current SKIP run */
    int      run_logged;     /* the enter line of the current run was logged */
    int64_t  last_log_ms;
    int      have_log;
    int64_t  skip_since_ms;  /* start of the current SKIP run */
    size_t   best_backlog;   /* smallest backlog seen in the run */
    int64_t  best_ms;        /* when it was reached */
} tx_pace_t;

typedef struct {
    size_t  base;         /* tx_pace_base() */
    size_t  cap;          /* tx_pace_cap(), 0 = unknown */
    int64_t stall_ms;     /* skipping without a new backlog minimum for this long: drop the client */
    int64_t max_skip_ms;  /* a skip run longer than this: drop the client */
} tx_pace_cfg_t;

typedef struct {
    int entered;   /* this call switched to SKIP mode */
    int left;      /* this call switched back to normal; run_skipped = frames dropped in that run */
    unsigned run_skipped;
    int log_it;    /* log_enter || log_leave */
    int log_enter; /* the caller may print the "link slow" line (rate limited) */
    int log_leave; /* the caller may print the "recovered" line (only when the enter line was printed) */
    int no_room;   /* the frame is dropped (or kept dropped) because it does not fit `room` */
    const char *drop_reason; /* set with TX_PACE_DROP_CLIENT */
} tx_pace_event_t;

void   tx_pace_init(tx_pace_t *p);
/* Base watermark in bytes for a configured bitrate in kbit/s. */
size_t tx_pace_base(int kbps);
/* Cap from the value getsockopt(SO_SNDBUF) returned (already doubled by the kernel); 0 stays 0. */
size_t tx_pace_cap(size_t sndbuf_reported);
/* Skip-run limit: max(3 GOPs, 10 s) in ms. */
int64_t tx_pace_max_skip_ms(int gop_s);
size_t tx_pace_high(const tx_pace_t *p, const tx_pace_cfg_t *c, size_t frame_len);
size_t tx_pace_low(size_t base);

/* Bytes a frame of frame_len payload bytes takes on the wire: + 4 B interleave + 12 B RTP + 2 B FU-A per
 * ~1400 B packet, rounded up. */
size_t tx_pace_wire_len(size_t frame_len);

/* Decide one video frame. backlog = unsent bytes in the socket before the write; room = bytes that can still
 * be queued for it (free kernel send buffer when nothing is pending, plus the free private queue), SIZE_MAX
 * without a private queue. A frame whose tx_pace_wire_len() exceeds room is never sent. ev may be NULL. */
tx_pace_verdict_t tx_pace_decide(tx_pace_t *p, const tx_pace_cfg_t *c, size_t backlog, size_t frame_len,
                                 size_t room, int is_key, int64_t now_ms, tx_pace_event_t *ev);

/* The frame just approved by tx_pace_decide() (TX_PACE_SEND) could not be written completely (the queue
 * refused a packet in the middle of it): enter SKIP mode now, so the next P-frames, which reference the
 * broken picture, are dropped until a keyframe. Also valid when already skipping (then it just counts the
 * frame). Always returns TX_PACE_SKIP. ev may be NULL. *ev is merged, not reset. */
tx_pace_verdict_t tx_pace_no_room(tx_pace_t *p, size_t backlog, int64_t now_ms, tx_pace_event_t *ev);

/* Unsent bytes in the socket send queue of a TCP fd, 0 when unknown (then no pacing happens). */
size_t tx_pace_backlog(int fd);
/* SO_SNDBUF as getsockopt reports it (kernel-doubled), 0 when unknown. */
size_t tx_pace_sndbuf(int fd);
