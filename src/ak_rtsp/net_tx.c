#include "net_tx.h"

#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <sys/socket.h>

#include "rtp_clock.h"

void net_tx_init(net_tx_t *t, int fd, int stall_budget_ms)
{
    t->fd = fd;
    t->stall_budget_ms = stall_budget_ms;
    t->slice_ms = 1000;
    t->worst_stall_ms = 0;
}

static void note_stall(net_tx_t *t, int64_t since_us)
{
    int ms = (int)((rtp_mono_us() - since_us) / 1000);
    if (ms > t->worst_stall_ms) t->worst_stall_ms = ms;
}

int net_tx_write_all(net_tx_t *t, const void *buf, size_t len)
{
    const unsigned char *p = buf;
    int64_t last_progress = rtp_mono_us();

    while (len > 0) {
        /* MSG_NOSIGNAL: a vanished peer is an EPIPE here, not a SIGPIPE. */
        ssize_t n = send(t->fd, p, len, MSG_NOSIGNAL);
        if (n > 0) {
            note_stall(t, last_progress);
            p += n;
            len -= (size_t)n;
            last_progress = rtp_mono_us();
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            /* SO_SNDTIMEO expired (or non-blocking socket) with the send
             * buffer still full: the peer is slow or gone. Keep waiting while
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
