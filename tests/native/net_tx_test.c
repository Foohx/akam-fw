/* Host test of src/ak_rtsp/net_tx.c: the stall policy of the RTSP/TCP writer.
 * Loopback socketpairs with tiny buffers; no network, no hardware.
 * Build/run: tests/net_tx_test.sh */
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include "net_tx.h"
#include "rtp_clock.h"

static int fails;
#define CHECK(cond, msg) do { if (cond) printf("  ok   - %s\n", msg); \
    else { printf("  FAIL - %s\n", msg); fails++; } } while (0)

static void pair(int sv[2])
{
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) { perror("socketpair"); exit(2); }
    int small = 4096;
    setsockopt(sv[0], SOL_SOCKET, SO_SNDBUF, &small, sizeof small);
    setsockopt(sv[1], SOL_SOCKET, SO_RCVBUF, &small, sizeof small);
    /* The same per-wait bound handle_client() sets (scaled down). */
    struct timeval to = { .tv_sec = 0, .tv_usec = 100000 };
    setsockopt(sv[0], SOL_SOCKET, SO_SNDTIMEO, &to, sizeof to);
}

struct reader {
    int fd;
    int delay_ms;      /* wait before the first read */
    int chunk_ms;      /* wait between reads */
    size_t chunk;
    size_t got;
    unsigned sum;      /* checksum of what arrived, to prove integrity */
};

static void *reader_main(void *p)
{
    struct reader *r = p;
    usleep((useconds_t)r->delay_ms * 1000);
    unsigned char buf[4096];
    for (;;) {
        ssize_t n = read(r->fd, buf, r->chunk < sizeof buf ? r->chunk : sizeof buf);
        if (n <= 0) break;
        for (ssize_t i = 0; i < n; i++) r->sum = r->sum * 31 + buf[i];
        r->got += (size_t)n;
        if (r->chunk_ms) usleep((useconds_t)r->chunk_ms * 1000);
    }
    return NULL;
}

static unsigned checksum(const unsigned char *b, size_t n)
{
    unsigned s = 0;
    for (size_t i = 0; i < n; i++) s = s * 31 + b[i];
    return s;
}

#define LEN (96 * 1024)

int main(void)
{
    static unsigned char data[LEN];
    for (size_t i = 0; i < LEN; i++) data[i] = (unsigned char)(i * 7 + (i >> 8));

    /* 1. Slow but alive reader: 1 KB every 10 ms (=100 KB/s) takes ~1 s for
     *    96 KB, far over the 100 ms per-wait timeout and over the 300 ms
     *    budget in total, but it always makes progress: no failure, no
     *    torn data. */
    {
        int sv[2]; pair(sv);
        net_tx_t t; net_tx_init(&t, sv[0], 300); t.slice_ms = 50;
        struct reader r = { .fd = sv[1], .chunk = 1024, .chunk_ms = 10 };
        pthread_t th; pthread_create(&th, NULL, reader_main, &r);
        int64_t t0 = rtp_mono_us();
        int rc = net_tx_write_all(&t, data, LEN);
        int64_t ms = (rtp_mono_us() - t0) / 1000;
        close(sv[0]); pthread_join(th, NULL); close(sv[1]);
        printf("       slow reader: wrote %d KB in %lld ms\n", LEN / 1024, (long long)ms);
        CHECK(rc == 0, "slow-but-alive client keeps its stream");
        CHECK(ms > 300, "(the write really outlasted the stall budget)");
        CHECK(r.got == LEN && r.sum == checksum(data, LEN), "every byte arrived intact and in order");
    }

    /* 2. Reader that stops for 700 ms then resumes, budget 2 s: survives. */
    {
        int sv[2]; pair(sv);
        net_tx_t t; net_tx_init(&t, sv[0], 2000); t.slice_ms = 50;
        struct reader r = { .fd = sv[1], .delay_ms = 700, .chunk = 4096 };
        pthread_t th; pthread_create(&th, NULL, reader_main, &r);
        int rc = net_tx_write_all(&t, data, LEN);
        int stall = net_tx_take_stall(&t);
        close(sv[0]); pthread_join(th, NULL); close(sv[1]);
        printf("       paused reader: worst stall %d ms\n", stall);
        CHECK(rc == 0 && r.got == LEN && r.sum == checksum(data, LEN),
              "a reader that pauses 0.7 s and resumes keeps its stream");
        CHECK(stall >= 500 && stall < 1500, "the stall is reported (drives the keyframe resync)");
        CHECK(net_tx_take_stall(&t) == 0, "and cleared once taken");
    }

    /* 3. Dead peer: socket stays open, nobody ever reads. Must be given up
     *    on after the budget (the earlier fix: it must not block forever). */
    {
        int sv[2]; pair(sv);
        net_tx_t t; net_tx_init(&t, sv[0], 600); t.slice_ms = 50;
        int64_t t0 = rtp_mono_us();
        int rc = net_tx_write_all(&t, data, LEN);
        int err = errno;
        int64_t ms = (rtp_mono_us() - t0) / 1000;
        printf("       dead peer: gave up after %lld ms\n", (long long)ms);
        CHECK(rc == -1 && err == ETIMEDOUT, "a client that accepts nothing fails with ETIMEDOUT");
        CHECK(ms >= 550 && ms < 2500, "after about the stall budget, not before and not forever");
        close(sv[0]); close(sv[1]);
    }

    /* 4. Peer closed: error right away, and no SIGPIPE kills the test. */
    {
        int sv[2]; pair(sv);
        close(sv[1]);
        net_tx_t t; net_tx_init(&t, sv[0], 5000);
        int64_t t0 = rtp_mono_us();
        int rc = net_tx_write_all(&t, data, LEN);
        int64_t ms = (rtp_mono_us() - t0) / 1000;
        CHECK(rc == -1 && (errno == EPIPE || errno == ECONNRESET), "closed peer: EPIPE");
        CHECK(ms < 500, "reported at once, without waiting for the budget");
        close(sv[0]);
    }

    /* 5. Small writes on an idle healthy socket: no stall reported. */
    {
        int sv[2]; pair(sv);
        net_tx_t t; net_tx_init(&t, sv[0], 1000);
        char b[64] = {0}, in[64];
        CHECK(net_tx_write_all(&t, b, sizeof b) == 0 && read(sv[1], in, sizeof in) == (ssize_t)sizeof in,
              "small write goes through");
        CHECK(net_tx_take_stall(&t) < 50, "and reports no stall");
        close(sv[0]); close(sv[1]);
    }

    printf(fails ? "FAILED: %d\n" : "all ok\n", fails);
    return fails ? 1 : 0;
}
