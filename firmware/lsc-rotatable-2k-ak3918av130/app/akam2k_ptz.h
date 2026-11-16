/* PTZ of the Rotatable 2K own app: the process-wide motor controller (hal_ptz + the 2K descriptor) on a thread of its own.
 *
 * Every motor command runs on that thread, never on the video path: callers (MQTT thread, a future button / person tracking)
 * only queue a command and return. One motion at a time; the queue holds ONE pending command: a new home / park / step replaces
 * the pending one and cancels the running motion (it stops within about 50 ms); nudges add up (a tracker's small corrections are
 * never dropped, never cancel). While the boot calibration runs (tens of seconds) every command is refused.
 *
 * Boot (ptz_cal=auto): the driver already holds our frame (restart of the app in the same boot): adopted; otherwise (every cold
 * boot) the optional scene check (a saved reference thumbnail + position compared with the live image BEFORE any motor command: a
 * proven-unmoved head skips the calibration), else the calibration against the stops (hal_ptz.c), then home; ptz.pos is never used to skip it. ptz_cal=off leaves the motor untouched: motion is refused until `akam_ptz init` + a restart.
 * State (home / parked / moved / calibrating / unknown) comes from the position. Privacy: with ptz_privacy=1 the RTSP video is
 * not sent while the state is "parked" (the streams stay up; the next keyframe is requested when it leaves "parked").
 * Files (jffs2, tiny): /config/akam/ptz.conf (optional soft range overrides, the keys of akam_ptz_safe.h) and ptz.pos (diagnostic: the last
 * position from a clean stop; "unknown" while a motion runs; written after the motor has been quiet for a moment). */
#ifndef AKAM2K_PTZ_H
#define AKAM2K_PTZ_H

#include <stdint.h>

#include "hal_ptz.h"

#define A2K_PTZ_CONF "/config/akam/ptz.conf"
#define A2K_PTZ_POS "/config/akam/ptz.pos"
#define A2K_PTZ_FLUSH_MS 2000
#define A2K_PTZ_SCENE_MS 6000          /* quiet time after a motion / calibration before the candidate view is captured (the AE settles) */
#define A2K_PTZ_SCENE_CONFIRM_MS 6000  /* from the capture to the write: the view must still match the candidate */
#define A2K_PTZ_SCENE_GAP_MS 300000    /* at most one reference write per this long (flash wear: a tracker moves often); the first one has no wait */
#define A2K_PTZ_SCENE "/config/akam/ptz.scene"
#define A2K_PTZ_SCENE_QUIET_STOP_MS 3000

/* Commands, in the order of the MQTT buttons ptz_left ptz_right ptz_up ptz_down ptz_home ptz_park ptz_recal (Recalibrate PTZ: the long calibration now). */
enum { A2K_PTZ_LEFT, A2K_PTZ_RIGHT, A2K_PTZ_UP, A2K_PTZ_DOWN, A2K_PTZ_HOME, A2K_PTZ_PARK, A2K_PTZ_RECAL, A2K_PTZ_CMD_N };

typedef struct {
    int cal;                    /* enum hal_ptz_cal */
    int park_pan, park_tilt;    /* -1: the descriptor's park */
    int privacy;                /* 1: no video while parked */
    int scene_check;            /* 1 (default): skip the cold boot calibration when the scene proves the head did not move (ptz_scene_check) */
} a2k_ptz_cfg_t;

/* The scene check hooks (akam2k_scene.c; fakes in the tests). Set before a2k_ptz_start; NULL = none.
 *   boot_check: at a cold boot, before any motor command: 1 and the saved position when the head provably did not move, else 0. cancelled():
 *               1 when the controller is stopping.
 *   capture:    the position is known and the motor has been quiet for A2K_PTZ_SCENE_MS: keep the current view in RAM as the candidate
 *               reference. 0 taken, 1 nothing to save (the view has too little detail), -1 not (no fresh frame, mode unknown).
 *   commit:     A2K_PTZ_SCENE_CONFIRM_MS later (and at the app shutdown), after the PTZ thread re-read the driver counters and found them equal to the
 *               cached position: write the position with the CANDIDATE, but only if the current view still matches it (nobody moved the head or
 *               the scene meanwhile). fresh: a calibration ran since the last write (the matched-boots counter restarts). 0 written, 1 nothing
 *               written, -1 refused (view changed, no frame, write failed).
 *   drop:       the position is no longer trusted (a motion starts, a calibration starts): invalidate the stored reference. 0, or -1 when it
 *               could not be done: the motion / calibration is then REFUSED (fail closed, like ptz.pos). Idempotent. */
typedef struct {
    int (*boot_check)(uint32_t *pan, uint32_t *tilt, int (*cancelled)(void));
    int (*capture)(void);
    int (*commit)(uint32_t pan, uint32_t tilt, int fresh);
    int (*drop)(void);
} a2k_ptz_scene_t;
void a2k_ptz_set_scene(const a2k_ptz_scene_t *s);

typedef struct {
    int state;                  /* enum hal_ptz_state */
    int known;                  /* pan / tilt are valid */
    uint32_t pan, tilt;
} a2k_ptz_info_t;

void a2k_ptz_defaults(a2k_ptz_cfg_t *c);
int a2k_ptz_parse_cal(const char *s);                       /* "auto" | "off" -> enum, else -1 */
int a2k_ptz_parse_park(const char *s, int *pan, int *tilt); /* "pan:tilt" digits only, 0 ok, -1 */
const char *a2k_ptz_state_name(int state);                  /* "home" "parked" "moved" "calibrating" "unknown" */

/* Starts the thread (which then calibrates / restores). 0 started, -1 not (bad ptz.conf, logged: the app goes on without PTZ). */
int a2k_ptz_start(const a2k_ptz_cfg_t *cfg);
void a2k_ptz_stop(void);    /* stops the motor (aborts a calibration), writes the position, joins the thread */

/* Queue a command. 0 queued, -1 refused (not running, calibrating, unknown command). Cheap, never blocks on the motor. */
int a2k_ptz_command(int cmd);
int a2k_ptz_toggle_park(void);        /* parked -> home, anything else -> park */
int a2k_ptz_nudge(int dx, int dy);    /* relative steps (right / up positive), clipped to the soft range */
void a2k_ptz_info(a2k_ptz_info_t *out);
/* The effective soft range (after ptz.conf): 0 and filled, -1 when the controller is not running. */
int a2k_ptz_range(uint32_t *pan_min, uint32_t *pan_max, uint32_t *tilt_min, uint32_t *tilt_max);
int a2k_ptz_privacy_active(void);     /* video path: 1 = send no frames. One volatile read. */
/* Called from the PTZ thread after every state change; must be cheap and never block (the MQTT module's copy). */
void a2k_ptz_set_notify(void (*fn)(const a2k_ptz_info_t *));

/* TEST ONLY: replaces the driver (NULL = the real one), the file paths and the flush delay. Call before a2k_ptz_start. */
void a2k_ptz_test_set(const hal_ptz_ops_t *ops, const char *conf_path, const char *pos_path, unsigned flush_ms);
/* TEST ONLY: the quiet time before the capture, the capture-to-write delay and the minimum gap between two writes (0 = none), instead of the A2K_PTZ_SCENE_* defaults. */
void a2k_ptz_test_set_scene_timing(unsigned scene_ms, unsigned confirm_ms, unsigned gap_ms);

#endif
