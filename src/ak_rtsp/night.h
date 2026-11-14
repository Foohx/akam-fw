#pragma once
#include <stddef.h>
#include <stdint.h>

/* Auto day/night mode monitor.
 *
 * Watches ae.c's AE state (via ae_get_last_stats()) and, on a sustained
 * brightness change, coordinates: IR-cut filter position, IR LED, sensor
 * frame-rate/VTS, and WB gains — mirroring stock anyka_ipc's night_mode_check
 * / day_mode_check (behaviour observed on the stock firmware; this reimplementation uses
 * a different EV proxy/thresholds than stock).
 *
 * Call night_start() after ae_start() (needs AE samples to already be
 * flowing). night_stop() on shutdown. */

#define NIGHT_CONF_FILE "/etc/config/akam/night"  /* saved night.mode (day / night); absent = auto */

int  night_start(void);
void night_stop(void);

/* Live-tunable thresholds + manual override, for control.c's TCP tuning
 * server. override=AUTO runs the normal threshold logic; FORCE_DAY/
 * FORCE_NIGHT pin the mode regardless of hw_exp (applied once per change,
 * checked first in night_loop()). */
typedef enum {
    NIGHT_MODE_AUTO        = 0,
    NIGHT_MODE_FORCE_DAY   = 1,
    NIGHT_MODE_FORCE_NIGHT = 2,
} night_override_t;

/* Defaults. See night_logic.h for what each one means. hw_exp is the AE run info exposure, whose ceiling is 16384
 * (seen in the stock capture with the lens covered); the thresholds are percentages of it, the gain ones are
 * relative to the AE's own gain limits read at run time. */
#define NIGHT_DEF_TRIGGER_HW_EXP   15500  /* day: AE saturated when hw_exp >= this (95 % of 16384) ... */
#define NIGHT_DEF_GAIN_PCT            95  /* ... and the total gain >= this % of the AE's maximum gain */
#define NIGHT_DEF_DAY_HW_EXP        2048  /* night: ambient light is enough when hw_exp stays below (12 %) */
#define NIGHT_DEF_CONFIRM_SAMPLES     30  /* polls (100 ms) of saturation before the switch to night: 3 s */
#define NIGHT_DEF_DAY_CONFIRM         50  /* polls below the day threshold before the switch to day (no IR LED) */
#define NIGHT_DEF_LOCK_MS          60000  /* anti-flap lock after any switch, stock's ps_soft_auto_lock_time_ms */
#define NIGHT_DEF_MIN_NIGHT_MS    120000  /* minimum night dwell before the first probe / return to day */
#define NIGHT_DEF_PROBE_MS        300000  /* interval of the periodic IR-off probe (doubles per failed probe) */
#define NIGHT_DEF_PROBE_SETTLE_MS   4000  /* AE settling time after the IR LEDs went off, before reading */

typedef struct {
    uint32_t         trigger_hw_exp;   /* day->night: exposure at or above this ... */
    uint32_t         gain_pct;         /* ... and total gain at or above this % of the AE maximum */
    uint32_t         day_hw_exp;       /* night->day: exposure (IR off) below this */
    int              confirm_samples;  /* consecutive samples before the switch to night */
    int              day_confirm_samples; /* same for the return to day when the IR LEDs are off by policy */
    int              lock_ms;          /* anti-flap lock duration after a switch */
    int              min_night_ms;     /* minimum time in night mode before a probe / return to day */
    int              probe_ms;         /* interval of the periodic IR-off probe */
    int              probe_settle_ms;  /* AE settling time during a probe */
    night_override_t override;
} night_tuning_t;

void night_get_tuning(night_tuning_t *out);
/* Returns 0, or errno when a changed night.mode could not be saved (the mode is applied anyway). The save happens
 * outside the night lock. night_set_persist(0) makes the next calls RAM only (network SET); default on. */
int  night_set_tuning(const night_tuning_t *in);
void night_set_persist(int on);

/* Current actual day/night state (1=night, 0=day) — distinct from
 * night_tuning_t.override, which is the requested mode, not the applied one. */
int night_is_night(void);

/* 8091 keys owned by night.c (same contract as imgq_ctl_*: _get returns 0 and fills `out` for a key it owns, else
 * -1; _set returns -1 for a key it does not own, else 0 (done) or 1 (refused) with the answer in `msg`; _list
 * emits one line per key). night.mode, night.state, night.trigger_hw_exp, night.day_hw_exp,
 * night.confirm_samples and night.lock_ms stay in control.c. New:
 *   night.gain_pct, night.day_confirm_samples, night.min_night_ms, night.probe_ms, night.probe_settle_ms
 *   (read / write), night.ae and night.logic (read-only: live AE values and the decision state). */
int  night_ctl_get(const char *key, char *out, size_t cap);
int  night_ctl_set(const char *key, const char *val, char *msg, size_t cap);
void night_ctl_list(void (*emit)(void *arg, const char *line), void *arg);
