/* PTZ of the Rotatable 2K own app: the process-wide motor controller (hal_ptz + the 2K descriptor) on a thread of its own.
 *
 * Every motor command runs on that thread, never on the video path: callers (MQTT thread, a future button / person tracking)
 * only queue a command and return. One motion at a time; the queue holds ONE pending command: a new home / park / step replaces
 * the pending one and cancels the running motion (it stops within about 50 ms); a nudge (tracking) is accepted only while idle (refused, never queued, so it cannot outlive or follow a park / home) (a tracker's small corrections are
 * resent from the next detection). While the boot calibration runs (tens of seconds) every command is refused.
 *
 * Boot (ptz_cal=auto): the driver already holds our frame (restart of the app in the same boot): adopted; otherwise (every cold
 * boot) the optional scene check (the HOME reference thumbnail compared with the live image BEFORE any motor command: a head proven to be
 * at home skips the calibration; the only claim a cold boot can make is the configured home), else the calibration against the stops
 * (hal_ptz.c), then home. A power cut while the head was away from home therefore costs one calibration at the next boot (ptz_return stays
 * "stay" by default). ptz_cal=off leaves the motor untouched: motion is refused until `akam_ptz init` + a restart.
 * State (home / parked / moved / calibrating / unknown) comes from the position. Privacy: with ptz_privacy=1 the RTSP video is
 * not sent while the state is "parked" (the streams stay up; the next keyframe is requested when it leaves "parked").
 * Files (jffs2, tiny): /config/akam/ptz.conf (optional soft range overrides, the keys of akam_ptz_safe.h, never written by the app) and the home
 * reference /config/akam/ptz.scene (+ .a<i> for a featureless home view). The position itself is NEVER written to the flash: it lives in RAM (the old
 * ptz.pos is gone; a leftover file is ignored). The reference is written once after a calibration (and only when the home view changed or the matched-boots
 * counter must restart), at most once per 24 h for a changed view; a matched boot rewrites it once for the boots counter. */
#ifndef AKAM2K_PTZ_H
#define AKAM2K_PTZ_H

#include <stdint.h>

#include "hal_ptz.h"

#define A2K_PTZ_CONF "/config/akam/ptz.conf"
#define A2K_PTZ_SCENE_MS 6000          /* quiet time after a motion / calibration before the candidate view is captured (the AE settles) */
#define A2K_PTZ_SCENE_CONFIRM_MS 6000  /* from the capture to the write: the view must still match the candidate */
#define A2K_PTZ_SCENE_GAP_MS 300000    /* at most one reference write per this long within a run; the first one has no wait */
#define A2K_PTZ_SCENE_MAX_WRITES 4     /* reference writes per run, belt and braces (a run writes one or two) */
#define A2K_PTZ_REF_TRAVEL 3000        /* steps since the calibration after which no reference is written in this run (lost steps would pair a moved view with the home position) */
#define A2K_PTZ_HOME_TOL 6             /* steps: the head counts as "at home" for the reference */
#define A2K_PTZ_SCENE_REWRITE_S 86400  /* a changed home view rewrites the reference at most once per this long (the stamp lives in the file) */
#define A2K_PTZ_SCENE "/config/akam/ptz.scene"
#define A2K_PTZ_MV_TRACK_QUIET_MS 600000 /* no multi-view excursion within this long after a tracking nudge */
#define A2K_PTZ_SCENE_QUIET_STOP_MS 3000
#define A2K_PTZ_TRAVEL_BUDGET 200000   /* steps travelled since the last calibration after which no scene reference is saved (ptz_travel_budget) */

/* Commands, in the order of the MQTT buttons ptz_left ptz_right ptz_up ptz_down ptz_home ptz_park ptz_recal (Recalibrate PTZ: the long calibration now). */
enum { A2K_PTZ_LEFT, A2K_PTZ_RIGHT, A2K_PTZ_UP, A2K_PTZ_DOWN, A2K_PTZ_HOME, A2K_PTZ_PARK, A2K_PTZ_RECAL, A2K_PTZ_CMD_N };

typedef struct {
    int cal;                    /* enum hal_ptz_cal */
    int park_pan, park_tilt;    /* -1: the descriptor's park */
    int privacy;                /* 1: no video while parked */
    unsigned long travel_budget; /* A2K_PTZ_TRAVEL_BUDGET: past it the next cold boot calibrates (no reference saved) */
    int scene_check;            /* 1 (default): skip the cold boot calibration when the scene proves the head did not move (ptz_scene_check) */
} a2k_ptz_cfg_t;

/* The scene check hooks (akam2k_scene.c; fakes in the tests). Set before a2k_ptz_start; NULL = none.
 *   boot_check: at a cold boot, before any motor command: 1 and the saved position when the head provably did not move, else 0. cancelled():
 *               1 when the controller is stopping. 2 (multi-view): the saved view is featureless but views at nearby offsets are stored; *pan / *tilt
 *               = the claimed position; the motor layer declares it, moves by an offset (probe_offset) and asks probe().
 *   boot_check: *pan / *tilt come in as the configured HOME and must stay: the reference is keyed to home (taken at another position: refused).
 *   capture:    a calibration ran, the head is at home and the motor has been quiet for A2K_PTZ_SCENE_MS (and no tracker nudged lately): keep the current
 *               view in RAM as the candidate reference. 0 taken, 1 nothing to save (the view has too little detail, or the saved reference is fine
 *               and was written less than 24 h ago), -1 not (no fresh frame, mode unknown). Called WITHOUT the controller lock.
 *   commit:     A2K_PTZ_SCENE_CONFIRM_MS later (and at the app shutdown), after the PTZ thread re-read the driver counters and found them equal to the
 *               cached position: the current view must still match the CANDIDATE (nobody moved the head or the scene meanwhile). Then the module
 *               writes ONLY when needed: no reference (or an invalid one) yes; the stored one still matching: nothing, except the matched-boots
 *               counter reset after a calibration (fresh); a changed home view: at most once per A2K_PTZ_SCENE_REWRITE_S. 0 written, 1 nothing
 *               written, -1 refused (view changed, no frame, write failed).
 *   drop:       the reference must not be used any more (a calibration failed, or a position claimed by the scene check was lost): invalidate it.
 *               Rare, best effort (0, or -1 when it could not be done; nothing is refused). Moves and calibrations do NOT drop it (keyed to home).
 *   writes:     optional, the number of flash writes of the reference files since the process start (/state sys.flash_writes_scene).
 * Multi-view (all NULL = none). capture() returns 2 when the rest view is featureless: the controller then (at home only, once per position)
 * moves by the offsets of offset(), keeping the views: for each, after a quiet time, alt_capture (0 kept, 1 too uniform, -1 no frame), after the
 * confirmation time alt_confirm (0 still the same view). Then back to the rest position and commit_multi(pan, tilt, fresh, mask of the confirmed
 * offsets): 0 written. The offsets are relative moves of the safe-motion layer (soft range, speed of the configuration), never a new ioctl.
 *   offset(i, &dpan, &dtilt): the offset table (steps), 0 or -1 at the end.
 *   probe_offset(i, &dpan, &dtilt) (boot): 0 when a stored view of offset i can be used; probe(i): the head is at that offset: 1 when it matches, 0 not, -1 cancelled
 *   (the shutdown). */
typedef struct {
    int (*boot_check)(uint32_t *pan, uint32_t *tilt, int (*cancelled)(void));
    int (*capture)(void);
    int (*commit)(uint32_t pan, uint32_t tilt, int fresh);
    int (*drop)(void);
    int (*probe_offset)(unsigned i, int *dpan, int *dtilt);
    int (*probe)(unsigned i);
    int (*offset)(unsigned i, int *dpan, int *dtilt);
    int (*alt_capture)(unsigned i);
    int (*alt_confirm)(unsigned i);
    int (*commit_multi)(uint32_t pan, uint32_t tilt, int fresh, unsigned mask);
    unsigned (*writes)(void);
} a2k_ptz_scene_t;
void a2k_ptz_set_scene(const a2k_ptz_scene_t *s);

typedef struct {
    int state;                  /* enum hal_ptz_state */
    int known;                  /* pan / tilt are valid */
    uint32_t pan, tilt;
    int64_t motion_end_ms;      /* CLOCK_MONOTONIC ms when the last motion ended (0 = none yet): a frame captured before it shows another pose */
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
int a2k_ptz_nudge(int dx, int dy);    /* a MANUAL nudge (API ptz.move): relative steps (right / up positive), clipped to the soft range; stops a patrol when accepted */
/* CONTINUOUS motion (the tracker, track_motion=continuous): ONE ongoing directional move re-targeted without a stop. vel(sx, sy, dx, dy): sx right+ / sy up+ in {-1,0,1}, dx / dy the
 * speed divisors 2..20 of the moving axes. The first call while the controller is idle starts a session on the motor thread; later calls (every detection tick) change the direction /
 * speed or just refresh the LEASE: with no call for A2K_PTZ_LEASE_MS the motion stops by itself (dead-man). sx = sy = 0 stops. 0 accepted, -1 refused (another command runs, not running,
 * calibrating, bad arguments). Any manual command (step, home, park, recalibrate, goto, an API nudge), the shutdown, privacy and a stall end the session within about 50 ms; the soft
 * range is enforced per axis by the safe layer on every poll (hal_ptz_cont_run). */
#define A2K_PTZ_LEASE_MS 450
int a2k_ptz_vel(int sx, int sy, int dx, int dy);
void a2k_ptz_emergency_stop(void);   /* async-signal-safe: the crash handler sends a motor stop (hal_ptz_emergency_stop) */
int a2k_ptz_oneshot_stop(void);      /* `akam2k_rtsp --ptz-stop`: open the node, stop, 0 / -1 (no thread, no calibration, nothing else touched) */
void a2k_ptz_set_lease_ms(int ms);   /* 150..2000, default A2K_PTZ_LEASE_MS (cfg lease_ms of the tracker) */
/* The driver position at a past instant (CLOCK_MONOTONIC ms), from the polled counters of the session (~50 ms) or the last known position while idle: 0 known (*pan / *tilt filled),
 * -1 unknown (before the first sample, a non-session motion in between, a gap while a session runs). Works DURING a continuous move: a frame is positioned by it. */
int a2k_ptz_pos_at(int64_t t_ms, int *pan, int *tilt);
/* A callback told every position sample pushed into that ring while a continuous session runs (the tracker's raw recorder, track_debug=2). Called with the motor lock held: must be quick and never call back. NULL = none. */
void a2k_ptz_set_ring_hook(void (*cb)(int64_t t_ms, int pan, int tilt));
int a2k_ptz_nudge_sp(int dx, int dy, int speed); /* the TRACKER's nudge at a speed divisor 2..20 (0 = the default); accepted only while idle; never stops a patrol */

/* PTZ patrol (ptz_patrol.h, akam2k_patrol.c). A manual command that is accepted (step, home, park, recalibrate, goto, nudge) calls the hook with its kind:
 * the A2K_PTZ_* command, A2K_PTZ_HOOK_GOTO or A2K_PTZ_HOOK_NUDGE. The hook runs under the controller lock: atomic stores only. */
#define A2K_PTZ_HOOK_GOTO 100
#define A2K_PTZ_HOOK_NUDGE 101
void a2k_ptz_set_cmd_hook(void (*fn)(int kind));
/* Manual absolute move inside the soft range at a speed divisor (0 = default): queued like a step (replaces the pending command, cancels the running one). 0 queued, -1 refused. */
int a2k_ptz_goto(int pan, int tilt, int speed);
/* The patrol's absolute move (the same safe motion layer): accepted only while the controller is idle, never preempts, does not call the hook.
 * 0 queued (*ticket filled), 1 busy (try again), -1 refused (not running, speed or position outside the limits). */
int a2k_ptz_patrol_goto(int pan, int tilt, int speed, unsigned *ticket);
/* 0 while the move is pending or running; 1 when finished: *rc = HAL_PTZ_OK, ECANCEL (cancelled or replaced), ESTALL, EIO, EREFUSED. */
int a2k_ptz_patrol_result(unsigned ticket, int *rc);
void a2k_ptz_patrol_cancel(void); /* cancels the patrol's pending or running move (never another command's) */
#define A2K_PTZ_PARK_TOL 15 /* steps: a patrol target this close to the park position is refused (the head would count as parked: privacy) */
int a2k_ptz_park(int *pan, int *tilt);   /* the park position: 0, -1 when the controller is not running */
int a2k_ptz_scene_pending(void);         /* 1 while the home scene reference is due or being built (the patrol's idle trigger waits) */
int a2k_ptz_home_tracker(void);          /* the tracker's home: like A2K_PTZ_HOME but never calls the manual-command hook */
int a2k_ptz_speeds(int *min, int *max, int *def); /* the allowed speed divisors and the default: 0, -1 when the controller is not running */
void a2k_ptz_info(a2k_ptz_info_t *out);
/* The effective soft range (after ptz.conf): 0 and filled, -1 when the controller is not running. */
int a2k_ptz_range(uint32_t *pan_min, uint32_t *pan_max, uint32_t *tilt_min, uint32_t *tilt_max);
int a2k_ptz_tilt_home(void); /* the home tilt in motor steps, 0 when the controller is not running */
unsigned long a2k_ptz_travel(void);   /* steps travelled since the last calibration (half the budget when this start did not calibrate) */
void a2k_ptz_wear_counts(unsigned *pos, unsigned *scene); /* /config writes since the start: ptz.pos (always 0: the position lives in RAM), scene reference files */
int a2k_ptz_privacy_active(void);     /* video path: 1 = send no frames. One volatile read. */
/* Called from the PTZ thread after every state change; must be cheap and never block (the MQTT module's copy). */
void a2k_ptz_set_notify(void (*fn)(const a2k_ptz_info_t *));

/* TEST ONLY: replaces the driver (NULL = the real one) and the ptz.conf path. Call before a2k_ptz_start. */
void a2k_ptz_test_set(const hal_ptz_ops_t *ops, const char *conf_path);
/* TEST ONLY: the quiet time before the capture, the capture-to-write delay and the minimum gap between two writes (0 = none), instead of the A2K_PTZ_SCENE_* defaults. */
void a2k_ptz_test_set_scene_timing(unsigned scene_ms, unsigned confirm_ms, unsigned gap_ms);

#endif
