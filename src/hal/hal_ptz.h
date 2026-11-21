/* hal_ptz: the portable pan/tilt interface (docs/hal-architecture.md). A model fills a descriptor (motor node, direction map,
 * measured soft range, home / park, speeds) and picks a driver (hal_ptz_drv.c: the Linux node + files); the engine (hal_ptz.c)
 * and the apps only name logical directions. No device path or request number appears outside the descriptor and the engine.
 *
 * The engine reuses the validated safe-motion layer of the Rotatable 2K tool (ptz/akam_ptz_safe.h, hardware verified): soft range,
 * timed moves polled every 50 ms, stall detection, the shifted driver frame (the driver's own limits ARE the soft range, so a
 * killed process cannot walk the motor into a stop), goto. On top of it: boot calibration against the mechanical stops, the
 * cancel. The position lives in RAM only: nothing about the motor is written to the flash (a cold boot proves HOME from the scene
 * or calibrates).
 *
 * One motor per process; every call except hal_ptz_cancel() must come from ONE thread (ptz.c).
 * Positions are "real" steps in the stock numbering (pan 0 = left stop, tilt 0 = bottom stop; pan + = right, tilt + = up). */
#ifndef HAL_PTZ_H
#define HAL_PTZ_H

#include <stddef.h>
#include <stdint.h>

enum { HAL_PTZ_UP, HAL_PTZ_RIGHT, HAL_PTZ_DOWN, HAL_PTZ_LEFT, HAL_PTZ_NDIR };

/* The speed divisors the safe motion layer accepts (smaller = faster: one step per `speed` ms). */
#define HAL_PTZ_SPEED_MIN 2
#define HAL_PTZ_SPEED_MAX 20

typedef struct hal_ptz_desc {
    const char *dev;                 /* motor node */
    uint8_t dir[HAL_PTZ_NDIR];       /* driver command word (0..7) of each logical direction */
    uint32_t pan_min, pan_max;       /* soft range (a motion never leaves it) */
    uint32_t tilt_min, tilt_max;
    uint32_t pan_home, tilt_home;    /* "home" */
    uint32_t pan_park, tilt_park;    /* "park" (default pan home, tilt soft max: looking up) */
    uint32_t pan_step_max, tilt_step_max; /* the stock numbering: driver limits of the calibration sweep */
    uint32_t speed;                  /* speed divisor (one step per `speed` ms), 2..20 */
    uint32_t step;                   /* one button step, in steps */
    /* Diagonal command words (continuous motion): up-right, down-right, down-left, up-left; diag_ok 0 = the driver has none (a diagonal then becomes the faster axis alone). */
    uint8_t diag[4];
    int diag_ok;
} hal_ptz_desc_t;

/* The driver and the persistence, replaceable by the host tests (hal_ptz_linux_ops: the real ones). */
typedef struct hal_ptz_ops {
    int (*open_dev)(void);
    int (*ioctl_fn)(int fd, unsigned long req, void *arg);
    void (*sleep_ms)(unsigned ms);
    int (*load_file)(const char *path, char *buf, unsigned cap); /* length, or -1 with errno (ENOENT = absent) */
    int (*save_file)(const char *path, const char *data);        /* atomic replace; 0 ok */
    void (*log)(const char *line);                               /* optional */
    /* Optional scene check (scene_check.h): at a cold boot with cal auto, BEFORE any motor command, cold_check() answers "the head is provably
     * at HOME": *pan / *tilt come in as the configured home and must come out unchanged (the reference is keyed to home; anything else is
     * refused); 1 = proven (home declared to the driver, no motion, no calibration), else 0 (calibrate as usual). A home equal to the park
     * position is never claimed (the parked view is the least informative one): it calibrates.
     * scene_drop() is called, best effort and rarely, when the reference must not be used any more: a calibration failed (not a cancel), or the
     * position claimed by the scene check was lost (stall, I/O error). It is NOT called before moves or calibrations: the reference is keyed
     * to home, so moving the head away does not invalidate it. */
    int (*cold_check)(uint32_t *pan, uint32_t *tilt);
    /* cold_check may also answer 3 = "very likely home, not proven" (zero shift, matching blocks, see scene_check.h SC_LIKELY): the SHORT calibration runs: the counters are declared a little
     * above home (pan + 400, tilt + 300), both axes walk to their stops, the head goes home, and cold_check is called AGAIN: only a 1 trusts the position. Anything else (also a short sweep that
     * fails or is cancelled): the full calibration, from wherever the head is. The risk is analysed in ptz-findings.md. */
    /* Multi-view (optional, both or none). cold_check may answer 2 = "the saved view is featureless; stored views at nearby offsets may prove the
     * position": *pan / *tilt = the claimed position. The claimed position is then declared to the driver (no motion), the head is moved by
     * cold_offset(i) (a relative offset in steps, at most 200 per axis whatever the hook says; each target inside the soft range) with the
     * safe-motion layer, cold_probe(i) says whether the live view there matches the stored one (1), and on a match the head goes back to the
     * claimed position (trusted, no calibration). Anything else (no match at any offset, a refused or failed move, a cancel): the position is
     * forgotten again and the full calibration runs. */
    int (*cold_offset)(unsigned i, int *dpan, int *dtilt); /* 0: offset i is available, -1: not (i = 0..7 are asked) */
    int (*cold_probe)(unsigned i); /* 1 match, 0 no match, -1 cancelled (shutdown): nothing more is moved, no calibration starts */
    int (*scene_drop)(void); /* 0 ok, -1 not done (logged, nothing refused: a stale reference is only ever used when the live view matches it) */
} hal_ptz_ops_t;

/* The real driver for the node `dev`: open O_RDWR, ioctl, nanosleep, fopen + rename. */
const hal_ptz_ops_t *hal_ptz_linux_ops(const char *dev);

enum hal_ptz_cal { HAL_PTZ_CAL_AUTO, HAL_PTZ_CAL_OFF };
enum hal_ptz_state { HAL_PTZ_UNKNOWN, HAL_PTZ_CALIBRATING, HAL_PTZ_MOVED, HAL_PTZ_HOME, HAL_PTZ_PARKED };
enum { HAL_PTZ_OK = 0, HAL_PTZ_EIO = 1, HAL_PTZ_EREFUSED = 4, HAL_PTZ_ESTALL = 5, HAL_PTZ_ECANCEL = 7 };

typedef struct hal_ptz hal_ptz_t;

/* conf_text: the optional ptz.conf (NULL = none): the soft range keys of akam_ptz_safe.h ("ptz.conf", strict). A bad one is
 * refused (-1, err filled) and nothing is created. The descriptor is copied. */
hal_ptz_t *hal_ptz_new(const hal_ptz_desc_t *d, const hal_ptz_ops_t *ops, const char *conf_text, char *err, size_t errcap);
void hal_ptz_free(hal_ptz_t *p);
const hal_ptz_desc_t *hal_ptz_desc(const hal_ptz_t *p); /* the effective descriptor (after ptz.conf) */

/* Forces the long calibration now (the Recalibrate button): from any state with the motor open, even when trusted. Same result as the
 * calibration of hal_ptz_start (home, trusted); the home reference stays (it is keyed to home). Refused (HAL_PTZ_EREFUSED) when the motor is not
 * open. Cancel (hal_ptz_cancel) aborts it like any calibration. */
int hal_ptz_recalibrate(hal_ptz_t *p);
/* The tilt-only re-home (tilt slip is non-cumulative): tilt DOWN into the bottom stop at the configured speed the full stroke (declared at the stock maximum, then 300 more: reaches the stop from any real tilt, however large the slip), the counter re-declared
 * as the stop (real 1), then UP to the tilt home (last leg from below). The pan is never commanded and keeps its counter. No flash write. Needs a trusted known position (EREFUSED otherwise).
 * hal_ptz_cancel stops it: the believed position of before is kept (ECANCEL). 0 ok; an I/O error leaves the position unknown or the believed one (the caller recalibrates). Takes up to ~15 s. */
int hal_ptz_tilt_rehome(hal_ptz_t *p);
/* Re-reads the driver counters (PTZ thread) and compares them with the cached position: 0 when the position is trusted, known and the counters agree
 * (within a few steps), else -1. Used before the scene reference is written: the position it stores must be the real one. */
int hal_ptz_verify_pos(hal_ptz_t *p);
/* Full calibrations completed by this process (0 after an adoption, a scene-check match or cal off). */
unsigned hal_ptz_cal_count(const hal_ptz_t *p);
unsigned long hal_ptz_travel(const hal_ptz_t *p);   /* steps travelled by motions since the last calibration of this process (the mechanical wear budget; continuous sessions count it WEIGHTED: fast steps and reversals cost more, see akam_ptz_safe.h) */
unsigned long hal_ptz_reversals(const hal_ptz_t *p); /* axis reversals of continuous sessions since the last calibration of this process */
unsigned hal_ptz_drift_cum_max(const hal_ptz_t *p); /* ptz.conf drift_cum_max (default 120): the signed corrected steps per axis since the calibration beyond which the drift logic recalibrates */
void hal_ptz_px_steps(const hal_ptz_t *p, unsigned *pan_x10, unsigned *tilt_x10); /* steps per thumbnail pixel x10 of the scene check (ptz.conf px_pan_x10 / px_tilt_x10; defaults 9.6 / 9.6) */
/* Re-declares the position counters to (pan, tilt), real steps inside the soft range: NO motion. For a head that the scene check proved to be elsewhere than the counters say (lost steps).
 * Only when trusted, idle and the position is known (HAL_PTZ_EREFUSED otherwise); takes about 2 s. A failure leaves the position unknown (HAL_PTZ_EIO). */
int hal_ptz_declare(hal_ptz_t *p, uint32_t pan, uint32_t tilt);

/* Opens the motor and brings it to a known position: (1) the driver already holds our frame (process restart in the same boot: our frame
 * file in /tmp and non-zero counters; never after a stall or a use of akam_ptz): adopted; (2) cal auto otherwise, i.e. every
 * COLD BOOT: the optional scene check (ops.cold_check: the head is provably at home: home is declared, no motion), else
 * the calibration (see hal_ptz.c), then home (no position is stored on the flash: the head can move outside the app, and a power cut while
 * it was away from home costs one calibration at the next boot); (3) cal off: nothing is touched, the state stays unknown and every motion is refused.
 * 0 ok (also for (4)), else an error (state unknown). Blocks for up to about a minute while calibrating. */
int hal_ptz_start(hal_ptz_t *p, int cal);
void hal_ptz_close(hal_ptz_t *p); /* stops the motor, closes nothing else */

/* Motions: 0 ok, HAL_PTZ_EREFUSED (soft limit, unknown position), ESTALL, EIO, ECANCEL (hal_ptz_cancel took effect). */
int hal_ptz_goto(hal_ptz_t *p, uint32_t pan, uint32_t tilt);
int hal_ptz_home(hal_ptz_t *p);
int hal_ptz_park(hal_ptz_t *p);
int hal_ptz_step(hal_ptz_t *p, int dir);          /* one bounded step of desc.step steps in a logical direction */
int hal_ptz_nudge(hal_ptz_t *p, int dx, int dy);  /* relative, in steps, clipped to the soft range */
int hal_ptz_nudge_sp(hal_ptz_t *p, int dx, int dy, int speed); /* the same at a speed divisor (2..20, else refused by the safe layer; 0 = default) */
/* Absolute move at a speed divisor (HAL_PTZ_SPEED_MIN..MAX, 0 = the configured one). A target outside the soft range is REFUSED (the patrol engine). */
int hal_ptz_goto_sp(hal_ptz_t *p, uint32_t pan, uint32_t tilt, int speed);

/* ---- Continuous motion (hal_ptz_cont_run): ONE ongoing directional move, re-targeted without a stop (see the continuous block of ptz/akam_ptz_safe.h for the guarantees:
 * soft range per axis, lease, stall / wrong-way / uncommanded-motion detection, travel counted). Logical commands: sx right+ / sy up+ in {-1, 0, 1}, dx / dy the speed
 * divisors HAL_PTZ_SPEED_MIN..MAX of the moving axes (0 = the configured one); both signs 0 = stop. Runs on the motor thread and returns when the motion has ended. */
typedef struct hal_ptz_cmd { int sx, sy, dx, dy; } hal_ptz_cmd_t;
typedef struct hal_ptz_cstate {
    uint32_t pan, tilt;  /* the polled position (real steps), every ~50 ms */
    unsigned t_ms;       /* polled time since the start of the session */
} hal_ptz_cstate_t;
enum { HAL_PTZ_CONT_KEEP = 0, HAL_PTZ_CONT_CMD = 1, HAL_PTZ_CONT_STOP = 2 };
/* Called after every poll: KEEP (nothing new: the lease runs), CMD (*cmd is the new or refreshed command: the lease restarts), STOP (end the session). */
typedef int (*hal_ptz_cont_cb)(void *ctx, const hal_ptz_cstate_t *st, hal_ptz_cmd_t *cmd);
/* Starts with `first` and returns when the session ended: HAL_PTZ_OK (STOP, the lease expired, every axis at its soft limit), HAL_PTZ_EREFUSED (soft limit / unknown position / bad command,
 * nothing moved), ESTALL, EIO, ECANCEL (hal_ptz_cancel). *travel (optional) = the steps travelled. lease_ms 150..2000. */
int hal_ptz_cont_run(hal_ptz_t *p, const hal_ptz_cmd_t *first, unsigned lease_ms, hal_ptz_cont_cb cb, void *ctx);

/* The motor stop for a process that is going down or already gone: hal_ptz_emergency_stop() is async-signal-safe (the crash handler of the app: a stop request on the open node, nothing else);
 * hal_ptz_oneshot_stop(ops) opens the node and sends the stop request (the supervisor's `akam2k_rtsp --ptz-stop` after the app died). No other request is used. 0 / -1. */
void hal_ptz_emergency_stop(void);
int hal_ptz_oneshot_stop(const hal_ptz_ops_t *ops);

/* Any thread: the running motion stops within about 50 ms and returns HAL_PTZ_ECANCEL (a cancel with nothing running is dropped by
 * the next motion). It also aborts a start in progress and a calibration (the motor is stopped, the position stays unknown): use it
 * for a shutdown. */
void hal_ptz_cancel(hal_ptz_t *p);

/* For a caller that dequeues commands: call it (same thread as the motions) at the moment a command is taken; a hal_ptz_cancel
 * between that and the motion's start makes the motion return HAL_PTZ_ECANCEL instead of being lost. One-shot (consumed by the
 * next motion). Without it, a cancel with nothing running is dropped by the next motion. */
void hal_ptz_mark(hal_ptz_t *p);

int hal_ptz_state(const hal_ptz_t *p);            /* enum hal_ptz_state (HAL_PTZ_MOVED while a motion runs) */
int hal_ptz_pos(const hal_ptz_t *p, uint32_t *pan, uint32_t *tilt); /* 1 known (filled), 0 unknown */

#endif
