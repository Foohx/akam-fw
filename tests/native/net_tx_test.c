/* Host test of src/ak_rtsp/net_tx.c: the stall policy of the RTSP/TCP writer.
 * Loopback socketpairs with tiny buffers; no network, no hardware.
 * Build/run: tests/net_tx_test.sh */
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
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
        int worst = net_tx_take_stall(&t);
        close(sv[0]); pthread_join(th, NULL); close(sv[1]);
        printf("       slow reader: wrote %d KB in %lld ms\n", LEN / 1024, (long long)ms);
        CHECK(rc == 0, "slow-but-alive client keeps its stream");
        /* The reader accepts a byte every 10 ms: the zero-progress stall is
         * far below the 100 ms SO_SNDTIMEO. A blocking send() that kept
         * progressing for the whole timeout used to be reported as a stall
         * of that length (and, on the camera, forced a keyframe resync). */
        CHECK(worst < 70, "progress is not reported as a stall");
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

    /* 5. Queue mode: a write bigger than the socket takes returns at once, nothing is torn. */
    {
        int sv[2]; pair(sv);
        net_tx_t t; net_tx_init(&t, sv[0], 600);
        CHECK(net_tx_queue_enable(&t, 256 * 1024) == 0 && net_tx_queue_cap(&t) == 256 * 1024, "queue enabled");
        CHECK(net_tx_queue_enable(&t, 1) == 0 && net_tx_queue_cap(&t) == 256 * 1024, "enabling twice keeps the queue");
        int64_t t0 = rtp_mono_us();
        int rc = net_tx_write_all(&t, data, LEN); /* nobody reads: blocking mode would wait 600 ms and fail */
        int64_t ms = (rtp_mono_us() - t0) / 1000;
        CHECK(rc == 0 && ms < 200, "a frame bigger than the send buffer is accepted without waiting");
        CHECK(net_tx_pending(&t) > 0 && net_tx_pending(&t) < LEN, "the part the socket refused is queued");
        CHECK(net_tx_room(&t) == net_tx_queue_cap(&t) - net_tx_pending(&t), "room is capacity minus pending");
        size_t before = net_tx_pending(&t);
        unsigned char tail[1000];
        for (size_t i = 0; i < sizeof tail; i++) tail[i] = (unsigned char)(255 - i);
        CHECK(net_tx_write_all(&t, tail, sizeof tail) == 0 && net_tx_pending(&t) == before + sizeof tail,
              "a later write queues behind the first (order kept)");
        CHECK(net_tx_queue_stalled_ms(&t) < 300, "stall clock only counts since the data started waiting");
        usleep(250000);
        CHECK(net_tx_queue_stalled_ms(&t) >= 200, "a peer that takes nothing shows as a growing stall");
        /* The reader now drains everything: pump until the queue is empty and check the stream byte for byte. */
        int fl = 1; ioctl(sv[1], FIONBIO, &fl);
        unsigned char *got = malloc(LEN + sizeof tail + 16);
        size_t ng = 0;
        for (int i = 0; i < 2000 && (net_tx_pending(&t) || ng < LEN + sizeof tail); i++) {
            ssize_t n = read(sv[1], got + ng, 4096);
            if (n > 0) ng += (size_t)n;
            if (net_tx_pump(&t) < 0) break;
        }
        CHECK(net_tx_pending(&t) == 0 && ng == LEN + sizeof tail, "queue drains completely");
        CHECK(ng == LEN + sizeof tail && memcmp(got, data, LEN) == 0 && memcmp(got + LEN, tail, sizeof tail) == 0,
              "stream is intact: frame then tail, no byte lost, duplicated or reordered");
        CHECK(net_tx_queue_stalled_ms(&t) == 0, "empty queue: no stall");
        free(got);
        net_tx_queue_free(&t);
        CHECK(net_tx_pending(&t) == 0 && net_tx_room(&t) == 0, "freed queue reports nothing");
        close(sv[0]); close(sv[1]);
    }

    /* 5b. Ring wrap-around: a small queue filled and drained in odd sizes keeps every byte in order. */
    {
        int sv[2]; pair(sv);
        net_tx_t t; net_tx_init(&t, sv[0], 600);
        net_tx_queue_enable(&t, 5000);
        int fl = 1; ioctl(sv[1], FIONBIO, &fl);
        int fl0 = 1; ioctl(sv[0], FIONBIO, &fl0);
        static unsigned char src[400000], dst[400000];
        for (size_t i = 0; i < sizeof src; i++) src[i] = (unsigned char)(i * 13 + (i >> 7));
        size_t put = 0, got = 0;
        int bad = 0;
        unsigned seed = 7;
        alarm(60); /* a stuck ring (no progress) kills the test instead of the CI job */
        while (got < sizeof src && !bad) {
            seed = seed * 1103515245u + 12345u;
            size_t n = 1 + (seed >> 8) % 3000;
            if (put + n > sizeof src) n = sizeof src - put;
            if (n && net_tx_room(&t) >= n) {
                if (net_tx_write_all(&t, src + put, n) != 0) bad = 1; else put += n;
            }
            if (net_tx_pump(&t) < 0) bad = 1;
            ssize_t r = read(sv[1], dst + got, 700);
            if (r > 0) got += (size_t)r;
        }
        alarm(0);
        CHECK(!bad && got == sizeof src && memcmp(src, dst, sizeof src) == 0,
              "400 KB through a 5000-byte ring in random sizes arrives intact (wrap-around)");
        net_tx_queue_free(&t);
        close(sv[0]); close(sv[1]);
    }

    /* 6. Queue mode: a write that does not fit the queue fails with ENOBUFS; what was queued is still intact. */
    {
        int sv[2]; pair(sv);
        net_tx_t t; net_tx_init(&t, sv[0], 600);
        net_tx_queue_enable(&t, 8 * 1024);
        int rc = net_tx_write_all(&t, data, LEN);
        CHECK(rc == -1 && errno == ENOBUFS, "overflow of the queue fails with ENOBUFS");
        net_tx_queue_free(&t);
        close(sv[0]); close(sv[1]);
    }

    /* 6b. ENOBUFS is all-or-nothing per write: interleaved packets (one write each) that do not fit are refused
     * whole, so the byte stream the peer finally reads holds only complete packets and framing stays valid.
     * The RTSP loop relies on this to treat ENOBUFS as "drop the rest of the frame" instead of "drop the client". */
    {
        int sv[2]; pair(sv);
        net_tx_t t; net_tx_init(&t, sv[0], 600);
        net_tx_queue_enable(&t, 5000);
        int fl = 1; ioctl(sv[1], FIONBIO, &fl);
        int fl0 = 1; ioctl(sv[0], FIONBIO, &fl0);
        enum { PKT = 1216 };
        static unsigned char pkt[PKT], rd[200000];
        int accepted = 0, refused = 0, torn = 0;
        size_t rd_len = 0;
        alarm(60);
        for (int i = 0; i < 60; i++) {
            memset(pkt, 0, sizeof pkt);
            pkt[0] = '$'; pkt[1] = 0; pkt[2] = (PKT - 4) >> 8; pkt[3] = (PKT - 4) & 0xff;
            pkt[4] = (unsigned char)i;
            for (int k = 5; k < PKT; k++) pkt[k] = (unsigned char)(i + k);
            size_t before = net_tx_pending(&t);
            int rc = net_tx_write_all(&t, pkt, PKT);
            if (rc == 0) accepted++;
            else if (errno == ENOBUFS) {
                refused++;
                if (net_tx_pending(&t) != before) torn = 1; /* nothing of the refused packet may be queued */
            } else torn = 1;
            if (i % 7 == 6) { /* the peer reads a little now and then */
                ssize_t r = read(sv[1], rd + rd_len, 3000);
                if (r > 0) rd_len += (size_t)r;
                (void)net_tx_pump(&t);
            }
        }
        for (int i = 0; i < 1000 && net_tx_pending(&t); i++) {
            ssize_t r = read(sv[1], rd + rd_len, 4096);
            if (r > 0) rd_len += (size_t)r;
            (void)net_tx_pump(&t);
        }
        for (;;) { ssize_t r = read(sv[1], rd + rd_len, 4096); if (r <= 0) break; rd_len += (size_t)r; }
        alarm(0);
        CHECK(accepted > 0 && refused > 0, "some packets were taken and some refused (ENOBUFS) while the peer lagged");
        CHECK(!torn, "a refused packet leaves no byte in the queue, other errors do not occur");
        int framing_ok = rd_len == (size_t)accepted * PKT;
        for (size_t o = 0; framing_ok && o < rd_len; o += PKT) {
            const unsigned char *q = rd + o;
            if (q[0] != '$' || ((q[2] << 8) | q[3]) != PKT - 4) framing_ok = 0;
            for (int k = 5; framing_ok && k < PKT; k++)
                if (q[k] != (unsigned char)(q[4] + k)) framing_ok = 0;
        }
        CHECK(framing_ok, "the stream the peer reads is exactly the accepted packets, each whole, framing valid");
        net_tx_queue_free(&t);
        close(sv[0]); close(sv[1]);
    }

    /* 7. Queue mode: a closed peer is an error from pump/write, never a SIGPIPE. */
    {
        int sv[2]; pair(sv);
        net_tx_t t; net_tx_init(&t, sv[0], 600);
        net_tx_queue_enable(&t, 256 * 1024);
        net_tx_write_all(&t, data, LEN);
        close(sv[1]);
        int rc = -2;
        for (int i = 0; i < 100 && rc != -1; i++) rc = net_tx_pump(&t);
        CHECK(rc == -1 && (errno == EPIPE || errno == ECONNRESET), "pump reports a vanished peer");
        net_tx_queue_free(&t);
        close(sv[0]);
    }

    /* 8. Without a queue everything above is unchanged: queue accessors are inert. */
    {
        int sv[2]; pair(sv);
        net_tx_t t; net_tx_init(&t, sv[0], 300);
        CHECK(net_tx_pending(&t) == 0 && net_tx_room(&t) == 0 && net_tx_queue_cap(&t) == 0 && net_tx_pump(&t) == 0,
              "blocking mode has no queue");
        close(sv[0]); close(sv[1]);
    }

    printf(fails ? "FAILED: %d\n" : "all ok\n", fails);
    return fails ? 1 : 0;
}
