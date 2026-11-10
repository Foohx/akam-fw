#pragma once
/* Energy gate for the half-duplex rule of the audio backchannel
 * (specs/07 section 1.4).
 *
 * Standard clients (go2rtc, WebRTC cards) may keep the backchannel open and
 * send RTP continuously, silence included, for as long as the viewer looks at
 * the camera. Muting the microphone whenever a talker is *connected* would
 * mute it for good. So the gate answers a narrower question: "is speech being
 * played on the speaker right now?"
 *   - the decoded audio is cut in windows of AVAD_WINDOW samples (20 ms);
 *   - a window whose mean square is above the threshold (about -45 dBFS RMS)
 *     is speech;
 *   - speech keeps the gate closed for the hold time (default 300 ms) plus the
 *     time the jitter buffer still delays that audio (what is queued has not
 *     been heard yet);
 *   - silence frames never close it.
 * Pure integer code, no clock of its own (the caller passes monotonic
 * microseconds), tested on the host (tests/audio_vad_test.sh). */
#include <stddef.h>
#include <stdint.h>

#define AVAD_RATE_HZ      8000
#define AVAD_WINDOW       160          /* samples: 20 ms */
/* (32768 * 10^(-45/20))^2 = 33955: mean square of a -45 dBFS RMS signal. */
#define AVAD_THRESHOLD_MS 33955u
#define AVAD_DEFAULT_HOLD_MS 300

typedef struct {
    uint64_t sumsq;         /* sum of squares of the window being filled */
    unsigned cnt;           /* samples in that window */
    int64_t  hold_us;
    int64_t  mute_until_us; /* gate closed until this time (0 = open) */
    unsigned long speech_windows; /* windows judged speech, for diagnostics */
} audio_vad_t;

void audio_vad_init(audio_vad_t *v, unsigned hold_ms);
/* Back to "no speech" (counters kept). */
void audio_vad_reset(audio_vad_t *v);
/* Feed decoded samples that were just queued for playback. latency_ms is how
 * much audio was already waiting in front of them (the jitter buffer level
 * before this push): the speech is heard that much later, so the gate stays
 * closed that much longer. */
void audio_vad_feed(audio_vad_t *v, const int16_t *pcm, size_t n, int64_t now_us,
                    unsigned latency_ms);
/* 1 while speech is (or was a moment ago) audible on the speaker. */
int  audio_vad_speech(const audio_vad_t *v, int64_t now_us);
