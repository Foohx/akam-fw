/* hal_ptz: the portable pan/tilt interface (docs/hal-architecture.md). A model fills a descriptor (motor node, direction map,
 * measured soft range, home / park, speeds) and picks a driver (hal_ptz_drv.c: the Linux node + files); the engine (hal_ptz.c)
 * and the apps only name logical directions. No device path or request number appears outside the descriptor and the engine.
 *
 * The engine reuses the validated safe-motion layer of the Rotatable 2K tool (ptz/akam_ptz_safe.h, hardware verified): soft range,
 * timed moves polled every 50 ms, stall detection, the shifted driver frame (the driver's own limits ARE the soft range, so a
 * killed process cannot walk the motor into a stop), goto. On top of it: boot calibration against the mechanical stops, the
 * saved position (atomic file, minimal flash writes), cancel.
 *
 * One motor per process; every call except hal_ptz_cancel() must come from ONE thread (akam2k_ptz.c).
 * Positions are "real" steps in the stock numbering (pan 0 = left stop, tilt 0 = bottom stop; pan + = right, tilt + = up). */
#ifndef HAL_PTZ_H
#define HAL_PTZ_H

#include <stddef.h>
#include <stdint.h>

enum { HAL_PTZ_UP, HAL_PTZ_RIGHT, HAL_PTZ_DOWN, HAL_PTZ_LEFT, HAL_PTZ_NDIR };

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
} hal_ptz_desc_t;

/* The driver and the persistence, replaceable by the host tests (hal_ptz_linux_ops: the real ones). */
typedef struct hal_ptz_ops {
    int (*open_dev)(void);
    int (*ioctl_fn)(int fd, unsigned long req, void *arg);
    void (*sleep_ms)(unsigned ms);
    int (*load_file)(const char *path, char *buf, unsigned cap); /* length, or -1 with errno (ENOENT = absent) */
    int (*save_file)(const char *path, const char *data);        /* atomic replace; 0 ok */
    void (*log)(const char *line);                               /* optional */
    /* Optional scene check (scene_check.h): at a cold boot with cal auto, BEFORE any motor command, cold_check() answers "the head provably did
     * not move while off": 1 and the saved position (declared to the driver, no motion, no calibration), else 0 (calibrate as usual).
     * scene_drop() is called before every calibration: the saved reference is no longer valid. A saved position equal to the park position is never
     * declared (the parked view is the least informative one): it calibrates. */
    int (*cold_check)(uint32_t *pan, uint32_t *tilt);
    int (*scene_drop)(void); /* 0 ok, -1: the reference could not be invalidated: the calibration is refused (fail closed, like ptz.pos) */
} hal_ptz_ops_t;

/* The real driver for the node `dev`: open O_RDWR, ioctl, nanosleep, fopen + rename. */
const hal_ptz_ops_t *hal_ptz_linux_ops(const char *dev);

enum hal_ptz_cal { HAL_PTZ_CAL_AUTO, HAL_PTZ_CAL_OFF };
enum hal_ptz_state { HAL_PTZ_UNKNOWN, HAL_PTZ_CALIBRATING, HAL_PTZ_MOVED, HAL_PTZ_HOME, HAL_PTZ_PARKED };
enum { HAL_PTZ_OK = 0, HAL_PTZ_EIO = 1, HAL_PTZ_EREFUSED = 4, HAL_PTZ_ESTALL = 5, HAL_PTZ_ECANCEL = 7 };

typedef struct hal_ptz hal_ptz_t;

/* conf_text: the optional ptz.conf (NULL = none): the soft range keys of akam_ptz_safe.h ("ptz.conf", strict). A bad one is
 * refused (-1, err filled) and nothing is created. pos_path: the saved position file. The descriptor is copied. */
hal_ptz_t *hal_ptz_new(const hal_ptz_desc_t *d, const hal_ptz_ops_t *ops, const char *conf_text, const char *pos_path,
                       char *err, size_t errcap);
void hal_ptz_free(hal_ptz_t *p);
const hal_ptz_desc_t *hal_ptz_desc(const hal_ptz_t *p); /* the effective descriptor (after ptz.conf) */

/* Forces the long calibration now (the Recalibrate button): from any state with the motor open, even when trusted. Same result as the
 * calibration of hal_ptz_start (home, trusted); the scene reference is dropped first. Refused (HAL_PTZ_EREFUSED) when the motor is not
 * open. Cancel (hal_ptz_cancel) aborts it like any calibration. */
int hal_ptz_recalibrate(hal_ptz_t *p);
/* Re-reads the driver counters (PTZ thread) and compares them with the cached position: 0 when the position is trusted, known and the counters agree
 * (within a few steps), else -1. Used before the scene reference is written: the position it stores must be the real one. */
int hal_ptz_verify_pos(hal_ptz_t *p);
/* Full calibrations completed by this process (0 after an adoption, a scene-check match or cal off). */
unsigned hal_ptz_cal_count(const hal_ptz_t *p);

/* Opens the motor and brings it to a known position: (1) the driver already holds our frame (process restart in the same boot: our frame
 * file in /tmp and non-zero counters; never after a stall or a use of akam_ptz): adopted; (2) cal auto otherwise, i.e. every
 * COLD BOOT: the optional scene check (ops.cold_check: the head provably did not move: the saved position is declared, no motion), else
 * the calibration (see hal_ptz.c), then home (the saved position file is a diagnostic only, never trusted: the head
 * can move outside the app); (3) cal off: nothing is touched, the state stays unknown and every motion is refused.
 * 0 ok (also for (4)), else an error (state unknown). Blocks for up to about a minute while calibrating. */
int hal_ptz_start(hal_ptz_t *p, int cal);
void hal_ptz_close(hal_ptz_t *p); /* stops the motor, writes the pending position, closes nothing else */

/* Motions: 0 ok, HAL_PTZ_EREFUSED (soft limit, unknown position), ESTALL, EIO, ECANCEL (hal_ptz_cancel took effect). */
int hal_ptz_goto(hal_ptz_t *p, uint32_t pan, uint32_t tilt);
int hal_ptz_home(hal_ptz_t *p);
int hal_ptz_park(hal_ptz_t *p);
int hal_ptz_step(hal_ptz_t *p, int dir);          /* one bounded step of desc.step steps in a logical direction */
int hal_ptz_nudge(hal_ptz_t *p, int dx, int dy);  /* relative, in steps, clipped to the soft range */

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
/* Writes the position to the saved file when it differs from what is stored (the caller picks the moment: idle for a while). */
void hal_ptz_flush(hal_ptz_t *p);
unsigned hal_ptz_store_writes(const hal_ptz_t *p); /* saved-file writes so far (tests, flash wear) */
int hal_ptz_dirty(const hal_ptz_t *p);             /* a position waits for hal_ptz_flush */

#endif
