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

static tx_pace_verdict_t step(tx_pace_t *p, size_t backlog, size_t len, int key, size_t base, int64_t t)
{
    return tx_pace_decide(p, backlog, len, key, base, t, &ev);
}

int main(void)
{
    tx_pace_t p;
    size_t base = tx_pace_base(1536); /* 0.5 s = 96000 bytes */
    CHECK(base == 96000, "base is half a second of the configured bitrate");
    CHECK(tx_pace_base(500) == TX_PACE_MIN_HIGH, "base floor at the minimum");
    CHECK(tx_pace_base(0) == TX_PACE_MIN_HIGH, "base without a bitrate is the floor");
    CHECK(tx_pace_low(base) == 24000, "low mark is a quarter of the base");

    /* An idle link never skips, whatever the frame type. */
    tx_pace_init(&p);
    CHECK(step(&p, 0, 130 * KB, 1, base, 0) == TX_PACE_SEND, "keyframe on an empty queue is sent");
    CHECK(step(&p, 100 * KB, 5 * KB, 0, base, 66) == TX_PACE_SEND,
          "P-frame behind a just-sent keyframe is sent (keyframe in flight is not congestion)");
    CHECK(!p.skipping, "still in normal mode");

    /* Backlog past base + keyframe: enter skip mode. */
    CHECK(step(&p, 130 * KB + base + 1, 5 * KB, 0, base, 132) == TX_PACE_SKIP, "high backlog drops the P-frame");
    CHECK(ev.entered && ev.log_it, "enter event reported and loggable");
    CHECK(p.skipping, "skip mode on");

    /* Skip mode: P-frames dropped even when the backlog is gone; a keyframe over the low mark too. */
    CHECK(step(&p, 0, 5 * KB, 0, base, 200) == TX_PACE_SKIP, "P-frame after the skip is dropped even on an empty queue");
    CHECK(!ev.entered && !ev.left, "no event while staying in skip mode");
    CHECK(step(&p, 40 * KB, 120 * KB, 1, base, 2000) == TX_PACE_SKIP, "keyframe above the low mark is dropped");
    CHECK(p.skipping, "still skipping");
    CHECK(step(&p, 0, 5 * KB, 0, base, 2100) == TX_PACE_SKIP, "P-frame after a dropped keyframe is dropped");

    /* Resume only on a keyframe at or below the low mark. */
    CHECK(step(&p, tx_pace_low(base) + 1, 120 * KB, 1, base, 3000) == TX_PACE_SKIP, "keyframe one byte over low is dropped");
    CHECK(step(&p, tx_pace_low(base), 120 * KB, 1, base, 4000) == TX_PACE_SEND, "keyframe at the low mark resumes");
    CHECK(ev.left && ev.run_skipped == 5, "leave event counts the dropped frames of the run");
    CHECK(ev.log_it == 1, "leave line printed because the enter line was");
    CHECK(!p.skipping, "normal mode again");
    CHECK(step(&p, 10 * KB, 5 * KB, 0, base, 4066) == TX_PACE_SEND, "P-frames flow after the keyframe");

    /* Rate limiting: a second run 4 s after the first logged is silent, both lines. */
    CHECK(step(&p, 1000 * KB, 5 * KB, 0, base, 4132) == TX_PACE_SKIP, "second congestion drops");
    CHECK(ev.entered && !ev.log_it, "second enter within the log window is not loggable");
    CHECK(step(&p, 0, 100 * KB, 1, base, 5000) == TX_PACE_SEND && ev.left && !ev.log_it,
          "its leave line is suppressed too (no orphan line)");
    /* After the window a new run logs both. */
    CHECK(step(&p, 1000 * KB, 5 * KB, 0, base, 20000) == TX_PACE_SKIP && ev.log_it, "enter loggable after the window");
    CHECK(step(&p, 0, 100 * KB, 1, base, 21000) == TX_PACE_SEND && ev.left && ev.log_it && ev.run_skipped == 1,
          "leave loggable when its enter was logged");

    /* Keyframe size is counted but capped. */
    tx_pace_init(&p);
    (void)step(&p, 0, 10 * 1024 * KB, 1, base, 0);
    CHECK(tx_pace_high(&p, base) == base + TX_PACE_IKEY_CAP, "keyframe allowance is capped");

    /* The size of a keyframe that is dropped still updates the allowance. */
    tx_pace_init(&p);
    (void)step(&p, 10000 * KB, 50 * KB, 1, base, 0);
    CHECK(p.key_len == 50 * KB, "dropped keyframe size is remembered");

    /* A NULL event pointer is accepted. */
    tx_pace_init(&p);
    CHECK(tx_pace_decide(&p, 0, 1, 0, base, 0, NULL) == TX_PACE_SEND, "NULL event pointer accepted");

    /* Real TCP socket on loopback: a peer that never reads leaves unsent bytes in the send queue. */
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
    CHECK(tx_pace_backlog(-1) == 0, "bad fd gives 0 (no pacing)");
    close(cs);
    close(ps);
    close(ls);

    printf("%s\n", fails ? "tx_pace_test: FAILED" : "tx_pace_test: OK");
    return fails ? 1 : 0;
}
