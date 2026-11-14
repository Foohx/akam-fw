/* Composition of the RTSP/TCP video send path of rtsp.c (send_video_frame): tx_pace_decide() with the room of
 * net_tx, packet-wise writes into the private queue, ENOBUFS handled as "stop the frame and skip to the next
 * keyframe", against a real loopback TCP socket whose reader is slow. Virtual time (no sleeps): every tick is one
 * frame period of 66 ms. Build/run: tests/pace_compose_test.sh */
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include "net_tx.h"
#include "tx_pace.h"

static int fails;
#define CHECK(cond, ...) do { if (cond) { printf("  ok   - "); printf(__VA_ARGS__); printf("\n"); } \
    else { printf("  FAIL - "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

typedef struct {
    int frames_sent, frames_skipped, enobufs_mid_frame, left_events, drop_at_ms, hard_fail;
    int framing_ok;
    size_t bytes_read;
} result_t;

static void connect_pair(int *cs, int *rs)
{
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa;
    socklen_t sl = sizeof sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int small = 8192;
    setsockopt(ls, SOL_SOCKET, SO_RCVBUF, &small, sizeof small);
    if (bind(ls, (struct sockaddr *)&sa, sizeof sa) != 0 || listen(ls, 1) != 0 ||
        getsockname(ls, (struct sockaddr *)&sa, &sl) != 0) { perror("listen"); exit(2); }
    *cs = socket(AF_INET, SOCK_STREAM, 0);
    int sndbuf = 256 * 1024; /* as handle_client() */
    setsockopt(*cs, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof sndbuf);
    if (connect(*cs, (struct sockaddr *)&sa, sizeof sa) != 0) { perror("connect"); exit(2); }
    *rs = accept(ls, NULL, NULL);
    close(ls);
}

/* Stream checker: every '$' frame the peer receives must be complete and well formed. */
typedef struct { unsigned char hdr[4]; size_t have_hdr, body_left; int ok; } framer_t;
static void framer_feed(framer_t *f, const unsigned char *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (f->body_left) { f->body_left--; continue; }
        f->hdr[f->have_hdr++] = b[i];
        if (f->have_hdr == 4) {
            if (f->hdr[0] != '$') f->ok = 0;
            f->body_left = ((size_t)f->hdr[2] << 8) | f->hdr[3];
            f->have_hdr = 0;
        }
    }
}

static void drain_peer(int rs, framer_t *fr, result_t *R)
{
    static unsigned char rdbuf[65536];
    for (;;) {
        ssize_t n = read(rs, rdbuf, sizeof rdbuf);
        if (n <= 0) break;
        framer_feed(fr, rdbuf, (size_t)n);
        R->bytes_read += (size_t)n;
    }
}

/* One run of the loop of rtsp.c: 15 fps, a keyframe every 30 frames. rd_bytes = what the peer reads per tick. */
static result_t run(int kbps, size_t keylen, size_t plen, size_t rd_bytes, int ticks, int64_t max_skip_ms)
{
    result_t R;
    memset(&R, 0, sizeof R);
    R.drop_at_ms = -1;
    int cs, rs;
    connect_pair(&cs, &rs);
    int fl = 1;
    ioctl(rs, FIONBIO, &fl);
    ioctl(cs, FIONBIO, &fl);
    net_tx_t tx;
    net_tx_init(&tx, cs, 15000);
    net_tx_queue_enable(&tx, 256 * 1024);
    tx_pace_t pace;
    tx_pace_init(&pace);
    framer_t fr;
    memset(&fr, 0, sizeof fr);
    fr.ok = 1;
    static unsigned char pkt[1216], rdbuf[65536];
    size_t rd_budget = 0;
    for (int i = 0; i < ticks; i++) {
        int64_t now_ms = (int64_t)i * 66;
        /* the peer reads what its link carries in one frame period, then the loop pumps the queue */
        rd_budget += rd_bytes;
        while (rd_budget > 0) {
            ssize_t n = read(rs, rdbuf, rd_budget < sizeof rdbuf ? rd_budget : sizeof rdbuf);
            if (n <= 0) break;
            framer_feed(&fr, rdbuf, (size_t)n);
            R.bytes_read += (size_t)n;
            rd_budget -= (size_t)n;
        }
        if (rd_budget > rd_bytes * 4) rd_budget = rd_bytes * 4; /* an idle reader does not bank bytes */
        if (net_tx_pending(&tx) && net_tx_pump(&tx) < 0) { R.hard_fail = 1; break; }

        int key = (i % 30) == 0;
        size_t len = key ? keylen : plen;
        /* ---- the same arithmetic as send_video_frame() ---- */
        size_t kq = tx_pace_backlog(cs), kcap = tx_pace_cap(tx_pace_sndbuf(cs)), qcap = net_tx_queue_cap(&tx);
        size_t pending = net_tx_pending(&tx), backlog = kq + pending;
        size_t kfree = pending ? 0 : (kcap > kq ? kcap - kq : 0);
        tx_pace_cfg_t c = { .base = tx_pace_base(kbps), .cap = kcap ? kcap + qcap : 0, .stall_ms = TX_PACE_STALL_MS,
                            .max_skip_ms = max_skip_ms };
        tx_pace_event_t ev;
        tx_pace_verdict_t pv = tx_pace_decide(&pace, &c, backlog, len, net_tx_room(&tx) + kfree, key, now_ms, &ev);
        if (pv == TX_PACE_DROP_CLIENT) { R.drop_at_ms = (int)now_ms; break; }
        if (pv != TX_PACE_SEND) { R.frames_skipped++; continue; }
        if (ev.left) R.left_events++;
        int stopped = 0;
        for (size_t off = 0; off < len && !stopped; off += 1198) { /* one interleaved packet per write */
            size_t n = len - off < 1198 ? len - off : 1198;
            pkt[0] = '$'; pkt[1] = 0; pkt[2] = (unsigned char)((n + 12) >> 8); pkt[3] = (unsigned char)(n + 12);
            if (net_tx_write_all(&tx, pkt, n + 16) < 0) {
                if (errno == ENOBUFS) {
                    R.enobufs_mid_frame++;
                    (void)tx_pace_no_room(&pace, backlog, now_ms, NULL);
                } else {
                    R.hard_fail = 1;
                }
                stopped = 1;
            }
        }
        if (!stopped) R.frames_sent++;
        if (R.hard_fail) break;
    }
    for (int i = 0; i < 4000 && net_tx_pending(&tx); i++) { /* let the peer take the rest, then check the stream */
        drain_peer(rs, &fr, &R);
        (void)net_tx_pump(&tx);
    }
    drain_peer(rs, &fr, &R);
    R.framing_ok = fr.ok && fr.body_left == 0 && fr.have_hdr == 0;
    net_tx_queue_free(&tx);
    close(cs);
    close(rs);
    return R;
}

int main(void)
{
    alarm(120); /* a hang fails the test instead of the CI job */

    printf("slow link (about 90 kB/s for a 3 Mbit/s stream), 200 KB keyframes: never torn, framing valid\n");
    result_t a = run(3000, 200 * 1024, 12 * 1024, 1500, 15 * 40, 0);
    CHECK(!a.hard_fail, "no hard write failure");
    CHECK(a.enobufs_mid_frame == 0, "no frame was admitted and then refused in the middle (%d): kernel room that the queue is already waiting on is not counted as free", a.enobufs_mid_frame);
    CHECK(a.framing_ok, "every byte the peer received is whole interleaved frames");
    CHECK(a.frames_skipped > 0, "frames were dropped for the slow link (%d)", a.frames_skipped);

    printf("fast link: everything goes out\n");
    result_t b = run(1536, 130 * 1024, 8 * 1024, 60000, 15 * 20, 0);
    CHECK(!b.hard_fail && b.framing_ok && b.frames_skipped == 0 && b.frames_sent == 15 * 20,
          "all %d frames sent, none skipped, framing valid (%d, %d)", 15 * 20, b.frames_sent, b.frames_skipped);

    printf("keyframes that can never fit (bigger than the kernel buffer plus the queue)\n");
    result_t d = run(3000, 700 * 1024, 12 * 1024, 20000, 15 * 60, 10000);
    CHECK(d.drop_at_ms >= 0, "the client is dropped, the stream does not freeze for ever");
    CHECK(d.drop_at_ms >= 10000 && d.drop_at_ms <= 10000 + 66, "... at max_skip_ms (%d ms), the run clock was never restarted", d.drop_at_ms);
    CHECK(d.frames_sent == 0 && d.left_events == 0, "nothing was sent, no 'recovered' event for an unsent frame");

    printf(fails ? "pace_compose_test: FAILED\n" : "pace_compose_test: OK\n");
    return fails ? 1 : 0;
}
