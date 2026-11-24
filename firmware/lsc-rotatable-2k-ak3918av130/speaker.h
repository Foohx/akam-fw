/* Speaker output and two-way audio of the Rotatable 2K: the glue between the shared engine (src/features/audio/audio_out.c, the
 * same one the 1080p runs) and the settings registry (settings.c rows audio.*) and the RTSP backchannel (rtsp_session.c).
 *
 * What is shared and what is here. The engine (device sequence, crash guard, amplifier policy, jitter buffer, volume ramp, beep) and the talk
 * session (G.711 RTP in, speech gate, half duplex) are the 1080p's, unchanged. This file only (1) points the engine at /config/akam and makes
 * the speaker opt-in (audio.out_enable default 0 until it has run on a camera), (2) turns the registry keys into engine keys and back, with
 * the same names and ranges as the 1080p control port, and (3) starts the playback thread when audio.out_enable becomes 1 (no restart).
 *
 * Persistence is the registry's (/config/akam/settings, written on a user change only, never periodically): the engine's own audio_out file is never
 * written (audio_out_set_persist(0)). A normal start writes NOTHING to flash (guard_arm 0 in the engine): not at boot, not when the
 * supervisor or the watchdog restarts the app. The crash guard (audio_out.trying in /config/akam) is written only when the USER turns the speaker on
 * (audio.out_enable 0 to 1 after boot: a rare event), just before that one start's first playback ioctl, and removed 5 s later or at once on a clean stop.
 * It is a safety net for the first run only: if the kernel panics in the playback ioctls then, the camera reboots, finds the marker and skips the
 * speaker until the file is removed by hand (SSH: rm /config/akam/audio_out.trying), instead of looping. Once that first run was healthy the marker is gone
 * and later starts are unguarded and unwritten (a panic that only appears later is not caught: the price of writing nothing at normal starts).
 *
 * The amplifier is not driven from here: the ak_pcm driver owns it (device tree "speak-gpios"), switched by the SET_SPEAKER ioctl of the engine.
 * No sysfs GPIO is touched (the stock's "[spk] spk_no 9" is the same line seen from user space). */
#ifndef AKAM2K_SPEAKER_H
#define AKAM2K_SPEAKER_H
#include <stddef.h>

#include "settings_reg.h"

#define A2K_SPK_CONF_DIR "/config/akam"

/* Points the engine at A2K_SPK_CONF_DIR, speaker off by default, engine persistence off. Before anything else uses the engine. Idempotent. */
void a2k_speaker_init(void);
/* Starts the playback thread when audio.out_enable is on (after the settings were loaded: the registry setter does it for a loaded value too).
 * 0 when it runs or is off, -1 when it could not start (logged). */
int a2k_speaker_boot(void);
/* Stops the playback thread, amplifier off, crash guard removed. Idempotent. */
void a2k_speaker_stop(void);

/* Registry rows (get/set hooks of st_def_t): audio.out_enable, audio.out_volume, audio.out_gain, audio.out_amp, audio.talk_enable,
 * audio.talk_duplex, and the read-only audio.out_state and audio.talk_state. */
int a2k_speaker_get(const char *key, char *out, size_t cap);
int a2k_speaker_set(const char *key, const char *val, st_apply_t *eff);
/* The action audio.beep: queues the test beep. 0, or -1 when the speaker is not running. */
int a2k_speaker_beep(void);

#endif
