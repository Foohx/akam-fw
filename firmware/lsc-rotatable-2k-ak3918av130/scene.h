/* Runtime side of the scene check (scene_check.h): the thumbnails of the detection channel, the saved reference file, the day/night mode.
 * Opt-in by ptz_scene_check (default 1; the detection must run, it is the only frame source: without it every cold boot calibrates).
 *
 * The detection thread calls a2k_scene_feed() with each 320x192 NV12 frame it already holds (the luma plane is box filtered to 80x48 at most
 * every 500 ms: a tenth of a millisecond, no allocation, no extra capture channel). The PTZ controller uses the hooks of a2k_scene_hooks():
 * boot check before any motor command (the reference is keyed to the configured HOME: it proves "the head is at home", whatever moved it since),
 * the reference write after a calibration (/config/akam/ptz.scene, < 4 KB, atomic; unchanged view = no write, a changed one at most once per 24 h,
 * the stamp in the file), drop only on a failed calibration or a lost claimed position. Moves never touch it. */
#ifndef AKAM2K_SCENE_H
#define AKAM2K_SCENE_H

#include <stdint.h>

#include "ptz.h"

#define A2K_SCENE_FEED_MS 500
#define A2K_SCENE_STALE_MS 2000     /* a thumbnail older than this is no frame */
#define A2K_SCENE_SETTLE_MS 4000    /* AE settling before the boot comparison */
#define A2K_SCENE_SETTLE_FRAMES 6
#define A2K_SCENE_TIMEOUT_MS 30000  /* no usable frame by then: calibrate */

/* ptz_scene_min as permille (sc_parse_min); default SC_MIN_DEFAULT. Before a2k_ptz_start. */
void a2k_scene_set_min(int permille);
/* Detection thread: the Y plane of one frame (w x h, stride bytes per line). Cheap; throttled inside. */
void a2k_scene_feed(const uint8_t *y, unsigned w, unsigned h, unsigned stride);
/* ptz_drift_ref (default 1): the drift reference (src/features/track/drift_match.h) is captured in RAM after a calibration and used by the drift check; 0 = neither. Before a2k_ptz_start. */
void a2k_scene_set_drift_ref(int on);
/* Diagnostics / tests: reference captures and mode bridges so far, which modes hold a reference. */
void a2k_scene_drift_ref_info(unsigned *captures, unsigned *bridges, int *have_day, int *have_night);
const a2k_ptz_scene_t *a2k_scene_hooks(void);
/* TEST ONLY: the reference file, the feed throttle and the boot check timing (instead of the defaults). Also forgets the frame, the candidate and the boots. */
void a2k_scene_test_set(const char *path, unsigned feed_ms, unsigned settle_ms, unsigned settle_frames, unsigned timeout_ms);
/* TEST ONLY: a fixed wall clock in seconds since the epoch (the rewrite rate limit); -1 = the real one. */
void a2k_scene_test_clock(int64_t secs);

#endif
