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

typedef struct {
    int fd;
    int stall_budget_ms; /* give up after this long with zero progress */
    int slice_ms;        /* longest single wait for POLLOUT (<= budget) */
    /* Longest zero-progress wait seen since the last net_tx_take_stall(). */
    int worst_stall_ms;
} net_tx_t;

void net_tx_init(net_tx_t *t, int fd, int stall_budget_ms);

/* Writes all len bytes. Returns 0 on success, -1 on failure with errno set:
 * ETIMEDOUT when the stall budget ran out, otherwise the write() error
 * (EPIPE, ECONNRESET, ...). EINTR is retried. Works on blocking sockets
 * (with or without SO_SNDTIMEO) and on non-blocking ones. */
int net_tx_write_all(net_tx_t *t, const void *buf, size_t len);

/* Returns the worst stall (ms) since the previous call and clears it. */
int net_tx_take_stall(net_tx_t *t);
