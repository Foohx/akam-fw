/* Host test of src/ak_rtsp/rtsp_idle.c. Build/run: tests/rtsp_idle_test.sh */
#include <stdio.h>
#include <string.h>
#include "rtsp_idle.h"

static int fails;
#define CHECK(cond, msg) do { if (cond) printf("  ok   - %s\n", msg); \
    else { printf("  FAIL - %s\n", msg); fails++; } } while (0)
#define S(x) ((int64_t)(x) * 1000000)

int main(void)
{
    rtsp_idle_t s;
    const char *r;

    /* Silent client */
    rtsp_idle_init(&s, S(100));
    CHECK(rtsp_idle_check(&s, 0, S(100) + S(19)) == NULL, "silent client kept before 20 s");
    r = rtsp_idle_check(&s, 0, S(100) + S(20));
    CHECK(r && !strcmp(r, "idle before PLAY"), "silent client dropped at 20 s");

    /* Slow but progressing handshake: a request every 15 s up to PLAY at 45 s */
    rtsp_idle_init(&s, 0);
    int ok = 1;
    for (int t = 0; t <= 45; t += 15) {
        if (rtsp_idle_check(&s, 0, S(t))) ok = 0;
        rtsp_idle_request(&s, S(t));
    }
    CHECK(ok, "slow progressing handshake is not dropped");
    CHECK(rtsp_idle_check(&s, 1, S(45)) == NULL, "PLAY reached");

    /* Endless trickle of requests: capped at 60 s */
    rtsp_idle_init(&s, 0);
    for (int t = 0; t < 60; t += 10) rtsp_idle_request(&s, S(t));
    r = rtsp_idle_check(&s, 0, S(60));
    CHECK(r && !strcmp(r, "too long to reach PLAY"), "never reaching PLAY is capped at 60 s");

    /* Playing client with no input for hours */
    rtsp_idle_init(&s, 0);
    rtsp_idle_request(&s, S(1));
    CHECK(rtsp_idle_check(&s, 1, S(3600 * 5)) == NULL, "playing client never dropped for idleness");

    /* poll wait */
    rtsp_idle_init(&s, 0);
    CHECK(rtsp_idle_wait_ms(&s, 0, 0, 5000) == 5000, "wait capped at max");
    CHECK(rtsp_idle_wait_ms(&s, 0, S(20) - 1500, 5000) == 2, "wait shrinks to the deadline");
    CHECK(rtsp_idle_wait_ms(&s, 0, S(30), 5000) == 1, "overdue: wait is 1 ms");
    CHECK(rtsp_idle_wait_ms(&s, 1, S(30), 4) == 4, "playing: wait unchanged");

    printf(fails ? "FAILED (%d)\n" : "all ok\n", fails);
    return fails ? 1 : 0;
}
