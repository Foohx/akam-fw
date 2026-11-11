#include "net_tx.h"

#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#include "rtp_clock.h"

void net_tx_init(net_tx_t *t, int fd, int stall_budget_ms)
{
    t->fd = fd;
    t->stall_budget_ms = stall_budget_ms;
    t->slice_ms = 1000;
    t->worst_stall_ms = 0;
    t->q = NULL;
    t->q_cap = t->q_head = t->q_len = 0;
    t->q_progress_us = 0;
}

int net_tx_queue_enable(net_tx_t *t, size_t cap)
{
    if (t->q) return 0;
    if (cap == 0) return -1;
    t->q = malloc(cap);
    if (!t->q) return -1;
    t->q_cap = cap;
    t->q_head = t->q_len = 0;
    return 0;
}

void net_tx_queue_free(net_tx_t *t)
{
    free(t->q);
    t->q = NULL;
    t->q_cap = t->q_head = t->q_len = 0;
}

size_t net_tx_pending(const net_tx_t *t) { return t->q_len; }
size_t net_tx_room(const net_tx_t *t) { return t->q ? t->q_cap - t->q_len : 0; }
size_t net_tx_queue_cap(const net_tx_t *t) { return t->q_cap; }

int net_tx_queue_stalled_ms(const net_tx_t *t)
{
    if (t->q_len == 0) return 0;
    return (int)((rtp_mono_us() - t->q_progress_us) / 1000);
}

int net_tx_pump(net_tx_t *t)
{
    while (t->q_len > 0) {
        ssize_t n = send(t->fd, t->q + t->q_head, t->q_len, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (n > 0) {
            t->q_head += (size_t)n;
            t->q_len -= (size_t)n;
            t->q_progress_us = rtp_mono_us();
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        if (n == 0) errno = EPIPE;
        return -1;
    }
    if (t->q_len == 0) t->q_head = 0;
    return 0;
}

static int queue_append(net_tx_t *t, const unsigned char *p, size_t len)
{
    if (len > t->q_cap - t->q_len) {
        errno = ENOBUFS;
        return -1;
    }
    if (t->q_len == 0) {
        t->q_head = 0;
        t->q_progress_us = rtp_mono_us(); /* the stall clock starts when data starts waiting */
    } else if (t->q_head + t->q_len + len > t->q_cap) {
        memmove(t->q, t->q + t->q_head, t->q_len);
        t->q_head = 0;
    }
    memcpy(t->q + t->q_head + t->q_len, p, len);
    t->q_len += len;
    return 0;
}

/* Queue mode of net_tx_write_all: never waits. */
static int write_queued(net_tx_t *t, const unsigned char *p, size_t len)
{
    if (t->q_len > 0 && net_tx_pump(t) < 0) return -1;
    while (t->q_len == 0 && len > 0) {
        ssize_t n = send(t->fd, p, len, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (n > 0) {
            p += n;
            len -= (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        if (n == 0) errno = EPIPE;
        return -1;
    }
    return len > 0 ? queue_append(t, p, len) : 0;
}

static void note_stall(net_tx_t *t, int64_t since_us)
{
    int ms = (int)((rtp_mono_us() - since_us) / 1000);
    if (ms > t->worst_stall_ms) t->worst_stall_ms = ms;
}

int net_tx_write_all(net_tx_t *t, const void *buf, size_t len)
{
    const unsigned char *p = buf;
    if (t->q) return write_queued(t, p, len);
    int64_t last_progress = rtp_mono_us();

    while (len > 0) {
        /* MSG_NOSIGNAL: a vanished peer is an EPIPE here, not a SIGPIPE.
         * MSG_DONTWAIT: never sleep inside send(). With SO_SNDTIMEO a
         * blocking send() that keeps making progress still returns after
         * the timeout, so the time spent in it cannot be told apart from a
         * real zero-progress stall; waiting in poll() below measures
         * exactly the time the peer accepted nothing. */
        ssize_t n = send(t->fd, p, len, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (n > 0) {
            p += n;
            len -= (size_t)n;
            last_progress = rtp_mono_us();
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            /* Send buffer full: the peer is slow or gone. Keep waiting while
             * the budget lasts. */
            note_stall(t, last_progress);
            int stalled = (int)((rtp_mono_us() - last_progress) / 1000);
            if (stalled >= t->stall_budget_ms) {
                errno = ETIMEDOUT;
                return -1;
            }
            int wait = t->stall_budget_ms - stalled;
            if (wait > t->slice_ms) wait = t->slice_ms;
            struct pollfd pfd = { .fd = t->fd, .events = POLLOUT };
            (void)poll(&pfd, 1, wait);
            note_stall(t, last_progress);
            continue;
        }
        if (n == 0) errno = EPIPE;
        return -1;
    }
    return 0;
}

int net_tx_take_stall(net_tx_t *t)
{
    int w = t->worst_stall_ms;
    t->worst_stall_ms = 0;
    return w;
}
