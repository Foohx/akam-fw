/* Host test of src/ak_rtsp/tx_pace.c. Build/run: tests/tx_pace_test.sh */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include "tx_pace.h"

static int fails;
#define CHECK(cond, msg) do { if (cond) printf("  ok   - %s\n", msg); \
    else { printf("  FAIL - %s\n", msg); fails++; } } while (0)

#define KB 1024u
static tx_pace_event_t ev;

static tx_pace_verdict_t step(tx_pace_t *p, const tx_pace_cfg_t *c, size_t backlog, size_t len, int key,
                              int64_t t)
{
    return tx_pace_decide(p, c, backlog, len, key, t, &ev);
}

static unsigned rnd_state = 12345;
static unsigned rnd(void) { rnd_state = rnd_state * 1103515245u + 12345u; return (rnd_state >> 8) & 0xffffff; }

int main(void)
{
    tx_pace_t p;
    tx_pace_cfg_t c = { .base = tx_pace_base(1536), .cap = 0, .stall_ms = 0, .max_skip_ms = 0 };
    size_t base = c.base; /* 0.5 s = 96000 bytes */
    CHECK(base == 96000, "base is half a second of the configured bitrate");
    CHECK(tx_pace_base(500) == TX_PACE_MIN_HIGH, "base floor at the minimum");
    CHECK(tx_pace_base(0) == TX_PACE_MIN_HIGH, "base without a bitrate is the floor");
    CHECK(tx_pace_low(base) == 24000, "low mark is a quarter of the base");
    CHECK(tx_pace_max_skip_ms(2) == 10000 && tx_pace_max_skip_ms(4) == 12000, "skip limit is max(3 GOPs, 10 s)");
    CHECK(tx_pace_cap(327680) == 245760 && tx_pace_cap(0) == 0, "cap is 3/4 of the reported (doubled) buffer");

    /* An idle link never skips, whatever the frame type. */
    tx_pace_init(&p);
    CHECK(step(&p, &c, 0, 130 * KB, 1, 0) == TX_PACE_SEND, "keyframe on an empty queue is sent");
    CHECK(step(&p, &c, 100 * KB, 5 * KB, 0, 66) == TX_PACE_SEND,
          "P-frame behind a just-sent keyframe is sent (keyframe in flight is not congestion)");
    CHECK(!p.skipping, "still in normal mode");

    /* Backlog past base + keyframe: enter skip mode. */
    CHECK(step(&p, &c, 130 * KB + base + 1, 5 * KB, 0, 132) == TX_PACE_SKIP, "high backlog drops the P-frame");
    CHECK(ev.entered && ev.log_it, "enter event reported and loggable");
    CHECK(p.skipping, "skip mode on");

    CHECK(step(&p, &c, 0, 5 * KB, 0, 200) == TX_PACE_SKIP, "P-frame after the skip is dropped even on an empty queue");
    CHECK(!ev.entered && !ev.left, "no event while staying in skip mode");
    CHECK(step(&p, &c, 40 * KB, 120 * KB, 1, 2000) == TX_PACE_SKIP, "keyframe above the low mark is dropped");
    CHECK(step(&p, &c, 0, 5 * KB, 0, 2100) == TX_PACE_SKIP, "P-frame after a dropped keyframe is dropped");
    CHECK(step(&p, &c, tx_pace_low(base) + 1, 120 * KB, 1, 3000) == TX_PACE_SKIP, "keyframe one byte over low is dropped");
    CHECK(step(&p, &c, tx_pace_low(base), 120 * KB, 1, 4000) == TX_PACE_SEND, "keyframe at the low mark resumes");
    CHECK(ev.left && ev.run_skipped == 5, "leave event counts the dropped frames of the run");
    CHECK(ev.log_it == 1, "leave line printed because the enter line was");
    CHECK(step(&p, &c, 10 * KB, 5 * KB, 0, 4066) == TX_PACE_SEND, "P-frames flow after the keyframe");

    /* Rate limiting. */
    CHECK(step(&p, &c, 1000 * KB, 5 * KB, 0, 4132) == TX_PACE_SKIP && ev.entered && !ev.log_it,
          "second enter within the log window is not loggable");
    CHECK(step(&p, &c, 0, 100 * KB, 1, 5000) == TX_PACE_SEND && ev.left && !ev.log_it,
          "its leave line is suppressed too (no orphan line)");
    CHECK(step(&p, &c, 1000 * KB, 5 * KB, 0, 20000) == TX_PACE_SKIP && ev.log_it, "enter loggable after the window");
    CHECK(step(&p, &c, 0, 100 * KB, 1, 21000) == TX_PACE_SEND && ev.left && ev.log_it && ev.run_skipped == 1,
          "leave loggable when its enter was logged");

    /* Keyframe allowance capped; a dropped keyframe still updates it. */
    tx_pace_init(&p);
    (void)step(&p, &c, 0, 10 * 1024 * KB, 1, 0);
    CHECK(tx_pace_high(&p, &c, 1) == base + TX_PACE_IKEY_CAP, "keyframe allowance is capped");
    tx_pace_init(&p);
    (void)step(&p, &c, 10000 * KB, 50 * KB, 1, 0);
    CHECK(p.key_len == 50 * KB, "dropped keyframe size is remembered");
    tx_pace_init(&p);
    CHECK(tx_pace_decide(&p, &c, 0, 1, 0, 0, NULL) == TX_PACE_SEND, "NULL event pointer accepted");

    /* --- send buffer cap --- */
    /* Reported SO_SNDBUF 327680 (kernel doubled 160 KiB): cap 245760. */
    tx_pace_cfg_t cc = { .base = base, .cap = tx_pace_cap(327680) };
    tx_pace_init(&p);
    (void)step(&p, &cc, 0, 200 * KB, 1, 0); /* keyframe seen: base + 200 KB > cap */
    CHECK(tx_pace_high(&p, &cc, 10 * KB) == cc.cap - 10 * KB, "high is bounded by cap - frame_len (cap below base + keyframe)");
    CHECK(step(&p, &cc, cc.cap - 10 * KB, 10 * KB, 0, 66) == TX_PACE_SEND, "a frame that exactly fits is sent");
    CHECK(step(&p, &cc, cc.cap - 10 * KB + 1, 10 * KB, 0, 132) == TX_PACE_SKIP && ev.entered,
          "a frame that does not fit enters skip mode");
    /* Tiny buffer: high never falls below base/2. */
    tx_pace_cfg_t tiny = { .base = base, .cap = 10 * KB };
    tx_pace_init(&p);
    CHECK(tx_pace_high(&p, &tiny, 5 * KB) == base / 2, "high floor is base/2 with a tiny buffer");
    CHECK(step(&p, &tiny, 0, 5 * KB, 0, 0) == TX_PACE_SEND, "small frame on an empty queue goes through a tiny buffer");
    CHECK(step(&p, &tiny, 7 * KB, 5 * KB, 0, 66) == TX_PACE_SKIP, "backlog + frame over the cap skips, even below base/2");
    /* A keyframe larger than the buffer is sendable when the queue is at or under low. */
    tx_pace_init(&p);
    CHECK(step(&p, &tiny, tx_pace_low(base), 200 * KB, 1, 0) == TX_PACE_SEND, "oversized keyframe at backlog <= low is sent");
    tx_pace_init(&p);
    CHECK(step(&p, &tiny, tx_pace_low(base) + 1, 200 * KB, 1, 0) == TX_PACE_SKIP, "oversized keyframe above low waits");
    /* No cap known: old behaviour. */
    tx_pace_init(&p);
    CHECK(step(&p, &c, 90 * KB, 1000 * KB, 0, 0) == TX_PACE_SEND, "without a cap only the watermarks count");

    /* --- dead peer / hopeless link --- */
    tx_pace_cfg_t d = { .base = base, .cap = 0, .stall_ms = 15000, .max_skip_ms = 30000 };
    tx_pace_init(&p);
    CHECK(step(&p, &d, 400 * KB, 5 * KB, 0, 0) == TX_PACE_SKIP, "enter skip");
    CHECK(step(&p, &d, 400 * KB, 5 * KB, 0, 14999) == TX_PACE_SKIP, "no new minimum, just under the stall budget");
    CHECK(step(&p, &d, 400 * KB, 5 * KB, 0, 15000) == TX_PACE_DROP_CLIENT && ev.drop_reason &&
          strstr(ev.drop_reason, "stalled"), "no progress for the stall budget drops the client");
    tx_pace_init(&p);
    (void)step(&p, &d, 400 * KB, 5 * KB, 0, 0);
    (void)step(&p, &d, 399 * KB, 5 * KB, 0, 10000);
    CHECK(step(&p, &d, 399 * KB, 5 * KB, 0, 24999) == TX_PACE_SKIP, "a new minimum restarts the stall clock");
    CHECK(step(&p, &d, 399 * KB, 5 * KB, 0, 25000) == TX_PACE_DROP_CLIENT, "and it expires 15 s after the last minimum");
    tx_pace_init(&p);
    (void)step(&p, &d, 400 * KB, 5 * KB, 0, 0);
    for (int i = 1; i < 30; i++)
        if (step(&p, &d, (400 - (size_t)i) * KB, 5 * KB, 0, i * 1000) != TX_PACE_SKIP) break;
    CHECK(step(&p, &d, 300 * KB, 5 * KB, 0, 30000) == TX_PACE_DROP_CLIENT && ev.drop_reason &&
          strstr(ev.drop_reason, "too long"), "draining but skipping past the limit drops the client");
    tx_pace_init(&p);
    (void)step(&p, &d, 400 * KB, 5 * KB, 0, 0);
    CHECK(step(&p, &d, 0, 100 * KB, 1, 29000) == TX_PACE_SEND && !p.skipping, "a recovering keyframe is sent before any limit");
    CHECK(step(&p, &d, 400 * KB, 5 * KB, 0, 60000) == TX_PACE_SKIP, "limits restart with each run");

    /* --- invariant over random sequences: SEND after a SKIP only on a keyframe (backlog <= low) --- */
    int bad = 0;
    for (int run = 0; run < 200; run++) {
        tx_pace_cfg_t r = { .base = base, .cap = (run & 1) ? tx_pace_cap(327680) : 0,
                            .stall_ms = (run & 2) ? 15000 : 0, .max_skip_ms = (run & 4) ? 12000 : 0 };
        tx_pace_init(&p);
        int after_skip = 0;
        int64_t t = 0;
        for (int i = 0; i < 400; i++) {
            t += 66;
            int key = (i % 30) == 0;
            size_t len = key ? 20 * KB + rnd() % (240 * KB) : 1 * KB + rnd() % (20 * KB);
            size_t backlog = (rnd() % 8 == 0) ? rnd() % (500 * KB) : rnd() % (60 * KB);
            tx_pace_verdict_t v = step(&p, &r, backlog, len, key, t);
            if (v == TX_PACE_SEND) {
                if (after_skip && !(key && backlog <= tx_pace_low(base))) bad++;
                if (p.skipping) bad++;
                after_skip = 0;
            } else {
                after_skip = 1;
                if (v == TX_PACE_DROP_CLIENT) { tx_pace_init(&p); after_skip = 0; } /* session ends */
            }
        }
    }
    CHECK(bad == 0, "random sequences: never SEND a frame while skipping, nor after a skip except on a drained keyframe");

    /* --- real sockets --- */
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa;
    socklen_t sl = sizeof sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int small = 4096;
    setsockopt(ls, SOL_SOCKET, SO_RCVBUF, &small, sizeof small);
    if (ls < 0 || bind(ls, (struct sockaddr *)&sa, sizeof sa) != 0 || listen(ls, 1) != 0 ||
        getsockname(ls, (struct sockaddr *)&sa, &sl) != 0) { perror("listen"); return 2; }
    int cs = socket(AF_INET, SOCK_STREAM, 0);
    setsockopt(cs, SOL_SOCKET, SO_SNDBUF, &small, sizeof small);
    if (connect(cs, (struct sockaddr *)&sa, sizeof sa) != 0) { perror("connect"); return 2; }
    int ps = accept(ls, NULL, NULL);
    if (ps < 0) { perror("accept"); return 2; }
    CHECK(tx_pace_backlog(cs) == 0, "idle TCP socket has no backlog");
    static char buf[4096];
    long total = 0;
    for (int i = 0; i < 200; i++) {
        ssize_t n = send(cs, buf, sizeof buf, MSG_DONTWAIT);
        if (n <= 0) break;
        total += n;
    }
    CHECK(total > 0 && tx_pace_backlog(cs) > 0, "unread bytes show up as backlog");
    CHECK(tx_pace_backlog(-1) == 0 && tx_pace_sndbuf(-1) == 0, "bad fd gives 0 (no pacing)");
    /* SO_SNDBUF reads back doubled: the cap is taken from the reported value, not from what was requested. */
    int want = 64 * 1024;
    setsockopt(cs, SOL_SOCKET, SO_SNDBUF, &want, sizeof want);
    size_t got = tx_pace_sndbuf(cs);
    CHECK(got >= (size_t)want, "getsockopt reports at least the requested size (the kernel doubles it)");
    CHECK(tx_pace_cap(got) == got / 4 * 3, "cap derived from the reported value");
    close(cs);
    close(ps);
    close(ls);

    /* A frame that does not fit the room left (kernel buffer plus the private queue) goes through
     * tx_pace_no_room(): SKIP mode is entered, so no P-frame is ever dropped singly. */
    {
        tx_pace_t p;
        tx_pace_cfg_t q = { .base = tx_pace_base(1536), .cap = 245760 + 256 * KB, .stall_ms = 0, .max_skip_ms = 0 };
        tx_pace_init(&p);
        CHECK(step(&p, &q, 0, 300 * KB, 1, 0) == TX_PACE_SEND,
              "a keyframe bigger than the kernel cap is approved when the queue extends the cap");
        CHECK(tx_pace_no_room(&p, 0, 1, &ev) == TX_PACE_SKIP && p.skipping && ev.entered,
              "no room: SKIP mode entered");
        CHECK(step(&p, &q, 0, 5 * KB, 0, 66) == TX_PACE_SKIP,
              "the next P-frame is dropped too (decoder references intact)");
        CHECK(tx_pace_no_room(&p, 0, 70, &ev) == TX_PACE_SKIP && !ev.entered && p.run_skipped == 3,
              "no room while skipping only counts the frame");
        CHECK(step(&p, &q, tx_pace_low(q.base), 100 * KB, 1, 200) == TX_PACE_SEND && ev.left,
              "a keyframe that fits at a drained backlog resumes");
        CHECK(tx_pace_no_room(&p, 0, 300, NULL) == TX_PACE_SKIP, "NULL event pointer accepted");
        /* The event of the decide call survives no_room: a keyframe that resumes the stream but does not fit
         * reports both "left" (with its count and log) and "entered". */
        tx_pace_init(&p);
        step(&p, &q, 600 * KB, 5 * KB, 0, 0);            /* enter SKIP, enter line logged */
        step(&p, &q, 600 * KB, 5 * KB, 0, 100);
        CHECK(step(&p, &q, tx_pace_low(q.base), 100 * KB, 1, 200) == TX_PACE_SEND && ev.left && ev.log_leave,
              "resume approved, leave event pending");
        CHECK(tx_pace_no_room(&p, tx_pace_low(q.base), 200, &ev) == TX_PACE_SKIP && ev.left && ev.entered &&
              ev.run_skipped == 2 && ev.log_leave && !ev.log_enter && ev.log_it,
              "no_room keeps the leave event (count, log) and adds the enter one; enter is rate limited");
        CHECK(p.skipping, "and the stream is skipping again");
    }

    printf("%s\n", fails ? "tx_pace_test: FAILED" : "tx_pace_test: OK");
    return fails ? 1 : 0;
}
