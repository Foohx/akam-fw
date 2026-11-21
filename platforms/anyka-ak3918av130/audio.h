/* Microphone capture of the Rotatable 2K own app (plan step "RTSP track", docs/porting/lsc-rotatable-2k/audio-findings.md).
 *
 * OFF by default (privacy): the app starts it only with --audio 1 (akam2k.conf audio=1). Off, the device is never
 * opened, no thread runs, the ring is never touched and the SDP has no audio track.
 *
 * One capture thread reads the PCM capture node (read only, 8000 Hz mono 16 bit, 512-byte periods, the ioctl sequence verified
 * on the hardware by audio/mic.c) and publishes every period into the a2k_aring (aring.h). It never
 * blocks on a client. A failed open or a streak of read errors is logged and retried with a backoff (1, 2, 4 ... 60 s);
 * the video never depends on it. This is the ONLY app file that may name the capture node and call ioctl (the audit in
 * tests/rot2k_overlay_test.sh allows exactly that literal here and in audio/mic.c). */
#ifndef AKAM2K_AUDIO_H
#define AKAM2K_AUDIO_H

#include "aring.h"

/* Starts the capture thread (returns at once, the device is opened by the thread). 0, or -1 when the thread cannot be
 * created (logged; the app goes on without audio). */
int a2k_audio_start(void);
/* The caller has set a2k_stop: waits (bounded, 2 s) for the thread, closes the device. Safe when never started. */
void a2k_audio_stop(void);
/* 1 while the capture is configured and streaming right now (the SDP offers the audio track only then), else 0. */
int a2k_audio_up(void);
/* 1 when --audio 1 started the module. */
int a2k_audio_enabled(void);
/* The ring the clients read (valid after a2k_audio_start). */
a2k_aring_t *a2k_audio_ring(void);
/* Times the capture was lost and re-opened since start (for the stats line). */
unsigned a2k_audio_restarts(void);

#endif
