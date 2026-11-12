#pragma once
/* Bounded blocking writes to an RTSP/TCP client.
 *
 * Policy (see handle_client()): a client that is slow but alive must keep its
 * stream, a client that is gone must not hold the single-client server for
 * long. The interleaved RTSP/TCP stream cannot survive a short write (the
 * next `$` frame would land in the middle of the previous one), so a write is
 * never abandoned half way because of a timeout: the remainder is retried
 * until it is delivered or the peer has accepted NOT A SINGLE BYTE for
 * stall_budget_ms in a row. Any progress resets the budget.
 *
 * Needs no hardware: tests/net_tx_test.sh drives it through socketpairs. */
#include <stddef.h>
#include <stdint.h>

typedef struct {
    int fd;
    int stall_budget_ms; /* give up after this long with zero progress */
    int slice_ms;        /* longest single wait for POLLOUT (<= budget) */
    /* Longest zero-progress wait seen since the last net_tx_take_stall(). */
    int worst_stall_ms;
    /* Optional private send queue (net_tx_queue_enable), NULL = blocking mode. */
    unsigned char *q;
    size_t q_cap, q_head, q_len;
    int64_t q_progress_us; /* last time the peer took bytes, or the queue became non-empty */
} net_tx_t;

void net_tx_init(net_tx_t *t, int fd, int stall_budget_ms);

/* Writes all len bytes. Returns 0 on success, -1 on failure with errno set:
 * ETIMEDOUT when the stall budget ran out, otherwise the write() error
 * (EPIPE, ECONNRESET, ...). EINTR is retried. Works on blocking sockets
 * (with or without SO_SNDTIMEO) and on non-blocking ones. */
int net_tx_write_all(net_tx_t *t, const void *buf, size_t len);

/* Queue mode (RTSP/TCP video). Problem: a frame bigger than the free kernel send buffer made
 * net_tx_write_all block while the caller still held the encoder's stream buffer. With a queue,
 * net_tx_write_all never waits: bytes the socket does not take at once are appended to a private
 * buffer (allocated once, `cap` bytes) that net_tx_pump() drains from the client loop, so the encoder
 * buffer can be released as soon as the frame has been written. Order is kept (nothing goes to the
 * socket while the queue is non-empty), so the interleaved stream is never torn; a write that does not
 * fit the queue fails with ENOBUFS (the caller drops the client, it never sends a part).
 *
 * net_tx_queue_enable: 0 on success (or when already enabled), -1 when the allocation failed (the
 * writer then stays in blocking mode). Pending bytes are discarded by net_tx_queue_free. */
int    net_tx_queue_enable(net_tx_t *t, size_t cap);
void   net_tx_queue_free(net_tx_t *t);
size_t net_tx_pending(const net_tx_t *t);
size_t net_tx_room(const net_tx_t *t);   /* free queue bytes, 0 without a queue */
size_t net_tx_queue_cap(const net_tx_t *t);
/* Non-blocking flush of the queue. 0 on success (progress or EAGAIN), -1 with errno set on a socket error. */
int    net_tx_pump(net_tx_t *t);
/* Milliseconds the queue has been non-empty without the peer taking one byte; 0 when it is empty. */
int    net_tx_queue_stalled_ms(const net_tx_t *t);

/* Returns the worst stall (ms) since the previous call and clears it. */
int net_tx_take_stall(net_tx_t *t);
