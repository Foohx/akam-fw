/* Host test of the decision logic of src/ak_rtsp/swdog.c on an injected clock. Build/run: tests/swdog_test.sh */
#include <stdio.h>
#include "swdog.h"

static int fails;
#define CHECK(cond, msg) do { if (cond) printf("  ok   - %s\n", msg); \
    else { printf("  FAIL - %s\n", msg); fails++; } } while (0)
#define S(x) ((uint32_t)((x) * 1000u))

int main(void)
{
    swdog_state_t s;
    int which = -1;

    swdog_state_init(&s);
    CHECK(swdog_decide(&s, S(1000), &which) == SWDOG_OK, "nothing armed: never triggers");

    /* start-up: the first frame has the grace on top of the stall limit */
    swdog_st_arm_video(&s, S(100));
    CHECK(swdog_decide(&s, S(100) + S(89), NULL) == SWDOG_OK, "no first frame yet, inside limit + grace (89 s)");
    CHECK(swdog_decide(&s, S(100) + S(91), NULL) == SWDOG_VIDEO_STALL, "no first frame after limit + grace (91 s)");

    /* steady state: stall only after the limit since the last frame */
    swdog_st_frame(&s, S(110));
    CHECK(swdog_decide(&s, S(110) + S(29), NULL) == SWDOG_OK, "29 s since the last frame: ok");
    CHECK(swdog_decide(&s, S(110) + S(31), NULL) == SWDOG_VIDEO_STALL, "31 s since the last frame: stall");
    swdog_st_frame(&s, S(130));
    CHECK(swdog_decide(&s, S(130) + S(29), NULL) == SWDOG_OK, "a new frame restarts the clock");

    /* a client blocked on write (SEND) is not an encoder stall; resuming restarts the reference */
    swdog_st_mode(&s, SWDOG_SEND, S(130));
    CHECK(swdog_decide(&s, S(130) + S(300), NULL) == SWDOG_OK, "SEND mode: any duration is ignored");
    swdog_st_mode(&s, SWDOG_FETCH, S(130) + S(300));
    CHECK(swdog_decide(&s, S(130) + S(300) + S(29), NULL) == SWDOG_OK, "back to FETCH: clock restarted at the switch");
    CHECK(swdog_decide(&s, S(130) + S(300) + S(31), NULL) == SWDOG_VIDEO_STALL, "FETCH again: stalls after the limit");

    /* no PLAY (IDLE): no frame expected */
    swdog_st_mode(&s, SWDOG_IDLE, S(500));
    CHECK(swdog_decide(&s, S(500) + S(3600), NULL) == SWDOG_OK, "IDLE session: no video rule");
    swdog_st_mode(&s, SWDOG_FETCH, S(9000));
    CHECK(swdog_decide(&s, S(9000) + S(5), NULL) == SWDOG_OK, "IDLE->FETCH does not inherit the old last frame");

    /* a night/day ISP reload (a few seconds, no frames) is far below the limit */
    swdog_st_frame(&s, S(9100));
    CHECK(swdog_decide(&s, S(9100) + S(10), NULL) == SWDOG_OK, "10 s pause (ISP reload): ok");

    /* heartbeats */
    swdog_state_init(&s);
    swdog_st_beat(&s, SWDOG_BEAT_AE, S(10));
    CHECK(swdog_decide(&s, S(10) + S(59), &which) == SWDOG_OK, "AE beat 59 s ago: ok");
    CHECK(swdog_decide(&s, S(10) + S(61), &which) == SWDOG_BEAT_STALL && which == SWDOG_BEAT_AE,
          "AE beat 61 s ago: stall, names the thread");
    swdog_st_disarm(&s, SWDOG_BEAT_AE);
    CHECK(swdog_decide(&s, S(10) + S(600), NULL) == SWDOG_OK, "a thread that exited on purpose is disarmed");
    swdog_st_beat(&s, SWDOG_BEAT_CONTROL, S(20));
    CHECK(swdog_decide(&s, S(20) + S(61), &which) == SWDOG_BEAT_STALL && which == SWDOG_BEAT_CONTROL,
          "control beat stall");
    swdog_st_beat(&s, SWDOG_BEAT_CONTROL, S(20) + S(60));
    CHECK(swdog_decide(&s, S(20) + S(61), NULL) == SWDOG_OK, "a fresh beat clears it");

    /* main beat is suspended while blocked on a client write */
    swdog_st_disarm(&s, SWDOG_BEAT_CONTROL);
    swdog_st_beat(&s, SWDOG_BEAT_MAIN, S(1000));
    swdog_st_mode(&s, SWDOG_SEND, S(1000));
    CHECK(swdog_decide(&s, S(1000) + S(120), NULL) == SWDOG_OK, "main beat ignored in SEND");
    swdog_st_mode(&s, SWDOG_IDLE, S(1000) + S(120));
    CHECK(swdog_decide(&s, S(1000) + S(120), &which) == SWDOG_BEAT_STALL && which == SWDOG_BEAT_MAIN,
          "main loop silent for 120 s outside SEND: wedged");

    /* 32-bit millisecond wrap (49.7 days of uptime) */
    swdog_state_init(&s);
    uint32_t t0 = 0xFFFFFFFFu - S(5);
    swdog_st_arm_video(&s, t0);
    swdog_st_frame(&s, t0 + S(1));
    CHECK(swdog_decide(&s, t0 + S(20), NULL) == SWDOG_OK, "across the 2^32 ms wrap: 19 s since a frame is ok");
    CHECK(swdog_decide(&s, t0 + S(32), NULL) == SWDOG_VIDEO_STALL, "across the wrap: 31 s since a frame stalls");

    printf("%s\n", fails ? "swdog_test: FAILED" : "swdog_test: all passed");
    return fails ? 1 : 0;
}
