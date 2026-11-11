#pragma once
#include <stdint.h>

/* Software stall watchdog (G01). Turns a wedged ak_rtsp into a clean exit so the wrapper respawns it.
 * It is NOT the hardware watchdog: /usr/bin/watchdog keeps feeding /dev/watchdog and is never touched.
 *
 * What is watched (monotonic milliseconds, uint32, wrap-safe):
 *  1. Video production: frames fetched from the encoder (GET_STREAM success), NOT frames sent to a
 *     client. The rule is suspended while the consumer is blocked writing to a client (SWDOG_SEND) or
 *     while no frame is expected (SWDOG_IDLE: a session that has not issued PLAY).
 *  2. Heartbeats of long-lived threads (main/RTSP loop, AE tick, control accept loop). A thread arms
 *     itself with its first beat and disarms when it exits on purpose.
 *  3. Nothing before swdog_start(): the init sequence is not supervised (it exits on failure).
 *
 * Longest legitimate pauses found in the code: a night/day ISP reload and the IR-cut pulses take well
 * under 2 s; a blocked client write is bounded by TX_STALL_BUDGET_MS (15 s) and is excluded by
 * SWDOG_SEND anyway. The limits below leave a wide margin over all of them.
 *
 * A process stopped by SIGSTOP (or hung in an uninterruptible kernel wait) cannot run this thread:
 * that is outside what an in-process watchdog can do. */

#define SWDOG_EXIT_CODE        70      /* distinct exit status: "software watchdog stall" */
#define SWDOG_START_GRACE_MS   60000u  /* extra time allowed for the very first frame after arming */
#define SWDOG_VIDEO_STALL_MS   30000u  /* no frame fetched for this long while one is expected */
#define SWDOG_BEAT_STALL_MS    60000u  /* a beating thread silent for this long is wedged */
#define SWDOG_CHECK_MS         1000u

typedef enum { SWDOG_BEAT_MAIN = 0, SWDOG_BEAT_AE, SWDOG_BEAT_CONTROL, SWDOG_BEAT_N } swdog_beat_t;
typedef enum { SWDOG_IDLE = 0, SWDOG_FETCH, SWDOG_SEND } swdog_mode_t;

typedef enum { SWDOG_OK = 0, SWDOG_VIDEO_STALL, SWDOG_BEAT_STALL } swdog_verdict_t;

typedef struct {
    volatile uint32_t video_armed;
    volatile uint32_t arm_ms;
    volatile uint32_t frames_seen;
    volatile uint32_t last_frame_ms;
    volatile uint32_t mode;
    volatile uint32_t mode_since_ms;
    volatile uint32_t beat_armed[SWDOG_BEAT_N];
    volatile uint32_t beat_ms[SWDOG_BEAT_N];
} swdog_state_t;

/* Pure logic, injectable clock (host-tested). */
void            swdog_state_init(swdog_state_t *s);
void            swdog_st_arm_video(swdog_state_t *s, uint32_t now_ms);
void            swdog_st_frame(swdog_state_t *s, uint32_t now_ms);
void            swdog_st_mode(swdog_state_t *s, swdog_mode_t m, uint32_t now_ms);
void            swdog_st_beat(swdog_state_t *s, swdog_beat_t b, uint32_t now_ms);
void            swdog_st_disarm(swdog_state_t *s, swdog_beat_t b);
/* *which receives the beat index for SWDOG_BEAT_STALL, else is untouched. */
swdog_verdict_t swdog_decide(const swdog_state_t *s, uint32_t now_ms, int *which);

/* Process-wide instance on CLOCK_MONOTONIC. Cheap (a few stores) and safe from any thread. */
void swdog_frame(void);
void swdog_mode(swdog_mode_t m);
void swdog_beat(swdog_beat_t b);
void swdog_disarm(swdog_beat_t b);
/* Arms the video rule and starts the checker thread (once, from main, when the stream is up).
 * exit_hook (async-signal-safe, may be NULL) runs just before _exit, e.g. the speaker amplifier off. */
void swdog_start(void (*exit_hook)(void));
