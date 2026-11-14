#include "swdog.h"
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "video_cfg.h"

/* The video stall limit is wall-clock time, not a frame count, so a lower configured frame rate does not move it.
 * Guard the margin anyway: even at the lowest allowed rate (VIDEO_FPS_LOW) the limit must stay at least 100 frame
 * periods, far above the longest legitimate gap (a day/night ISP reload, well under 2 s). */
_Static_assert(SWDOG_VIDEO_STALL_MS >= 100u * (1000u / VIDEO_FPS_LOW), "swdog video stall limit too tight for the lowest fps");

static int32_t sw_diff(uint32_t a, uint32_t b) { return (int32_t)(a - b); } /* a - b, wrap-safe */

void swdog_state_init(swdog_state_t *s) { memset((void *)s, 0, sizeof(*s)); }

void swdog_st_arm_video(swdog_state_t *s, uint32_t now_ms)
{
    s->arm_ms = now_ms;
    s->frames_seen = 0;
    s->mode = SWDOG_FETCH;
    s->mode_since_ms = now_ms;
    s->video_armed = 1;
}

void swdog_st_frame(swdog_state_t *s, uint32_t now_ms)
{
    s->last_frame_ms = now_ms;
    s->frames_seen = 1;
}

void swdog_st_mode(swdog_state_t *s, swdog_mode_t m, uint32_t now_ms)
{
    if (s->mode == (uint32_t)m) return;
    s->mode_since_ms = now_ms;
    s->mode = m;
}

void swdog_st_beat(swdog_state_t *s, swdog_beat_t b, uint32_t now_ms)
{
    if ((unsigned)b >= SWDOG_BEAT_N) return;
    s->beat_ms[b] = now_ms;
    s->beat_armed[b] = 1;
}

void swdog_st_disarm(swdog_state_t *s, swdog_beat_t b)
{
    if ((unsigned)b < SWDOG_BEAT_N) s->beat_armed[b] = 0;
}

swdog_verdict_t swdog_decide(const swdog_state_t *s, uint32_t now_ms, int *which)
{
    uint32_t mode = s->mode;

    if (s->video_armed && mode == SWDOG_FETCH) {
        /* Reference: the latest of the last frame, the moment fetching was (re)started and the arming. */
        uint32_t ref = s->arm_ms;
        if (sw_diff(s->mode_since_ms, ref) > 0) ref = s->mode_since_ms;
        if (s->frames_seen && sw_diff(s->last_frame_ms, ref) > 0) ref = s->last_frame_ms;
        uint32_t limit = SWDOG_VIDEO_STALL_MS;
        if (!s->frames_seen) limit += SWDOG_START_GRACE_MS;
        if (sw_diff(now_ms, ref) > (int32_t)limit) return SWDOG_VIDEO_STALL;
    }
    for (int i = 0; i < SWDOG_BEAT_N; i++) {
        if (!s->beat_armed[i]) continue;
        /* The main thread legitimately sits in a blocked client write: suspended in SWDOG_SEND. */
        if (i == SWDOG_BEAT_MAIN && mode == SWDOG_SEND) continue;
        if (sw_diff(now_ms, s->beat_ms[i]) > (int32_t)SWDOG_BEAT_STALL_MS) {
            if (which) *which = i;
            return SWDOG_BEAT_STALL;
        }
    }
    return SWDOG_OK;
}

/* ---- process instance ------------------------------------------------------------------------ */
static swdog_state_t g_sw;
static pthread_t     g_sw_tid;
static void        (*g_sw_exit_hook)(void);

static uint32_t sw_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u);
}

void swdog_frame(void)               { swdog_st_frame(&g_sw, sw_now_ms()); }
void swdog_mode(swdog_mode_t m)      { swdog_st_mode(&g_sw, m, sw_now_ms()); }
void swdog_beat(swdog_beat_t b)      { swdog_st_beat(&g_sw, b, sw_now_ms()); }
void swdog_disarm(swdog_beat_t b)    { swdog_st_disarm(&g_sw, b); }

static void *swdog_thread(void *arg)
{
    static const char *const names[SWDOG_BEAT_N] = { "main/rtsp loop", "ae tick", "control accept" };
    (void)arg;
    for (;;) {
        usleep(SWDOG_CHECK_MS * 1000u);
        int which = 0;
        swdog_verdict_t v = swdog_decide(&g_sw, sw_now_ms(), &which);
        if (v == SWDOG_OK) continue;
        /* One line, written straight to the console (the stdout tee may itself be what is stuck), then
         * the speaker amplifier off and a distinct exit status for the wrapper. */
        char msg[112];
        int n;
        if (v == SWDOG_VIDEO_STALL)
            n = snprintf(msg, sizeof(msg), "[swdog] no video frame from the encoder for %u s, exiting (code %d)\n",
                         SWDOG_VIDEO_STALL_MS / 1000u, SWDOG_EXIT_CODE);
        else
            n = snprintf(msg, sizeof(msg), "[swdog] thread '%s' silent for %u s, exiting (code %d)\n",
                         names[which], SWDOG_BEAT_STALL_MS / 1000u, SWDOG_EXIT_CODE);
        if (n > 0) (void)!write(2, msg, (size_t)n);
        if (g_sw_exit_hook) g_sw_exit_hook();
        _exit(SWDOG_EXIT_CODE);
    }
    return NULL;
}

void swdog_init_deadline(unsigned seconds)
{
    signal(SIGALRM, SIG_DFL);
    alarm(seconds);
}

void swdog_start(void (*exit_hook)(void))
{
    alarm(0); /* the init deadline is replaced by the stall checker */
    g_sw_exit_hook = exit_hook;
    swdog_st_arm_video(&g_sw, sw_now_ms());
    if (pthread_create(&g_sw_tid, NULL, swdog_thread, NULL) != 0) perror("[swdog] pthread_create");
}
