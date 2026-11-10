#pragma once
#include <stddef.h>
#include <stdint.h>

#include "night.h"

/* Day / night decision of the auto mode, as a pure state machine: no ISP, no GPIO, no clock. night.c feeds it one
 * AE sample per poll (NL_POLL_MS) and performs the action it returns; tests/native/night_logic_test.c feeds it
 * simulated AE traces. Time is counted in polls, so it needs no clock source (a bad one once made the 60 s lock
 * expire within seconds on the camera, see night.c).
 *
 * Why this shape (specs/08-spec-image-quality.md, section 3.4 and the decisions of section 11):
 *
 * DAY -> NIGHT, like the stock firmware: only when the AE is saturated, i.e. the exposure is at its ceiling AND
 *   the total gain (sensor gain x ISP gain) is at (gain_pct % of) the maximum the AE is allowed, for
 *   confirm_samples consecutive samples. The gain maximum is read from the camera's own exposure block at run
 *   time (ae_get_limits()), not from a table. A dim but usable room never gets there: the AE reaches its target
 *   before the ceiling. The old rule (hw_exp > 3000, 18 % of the range) put a lit room into night mode.
 *
 * NIGHT -> DAY is the hard part. With the IR LEDs on, the scene is lit by the camera itself, so the AE sees a
 *   bright picture whatever the room does (the stock capture: exposure at its floor for 2 minutes with the lens
 *   covered). Exposure with the IR on therefore says nothing about the ambient light, and a plain "exposure is
 *   low, go to day" would flip every few minutes (day, dark, night, IR bright, day, ...). The stock firmware
 *   avoids it with AWB statistics (a "non infrared ratio") which this firmware does not decode. Instead the IR
 *   LEDs are switched off for a few seconds (a "probe") and the AE is read in the real ambient light:
 *     - in a dark room it climbs back to its ceiling within about 2 s: the probe ends at once (IR LEDs back on)
 *       and the next one is later (exponential back-off);
 *     - in a lit room it stays low: the camera goes to day (the LEDs are already off, no flash).
 *   A probe starts when the minimum night dwell is over and either the probe interval has elapsed, or the
 *   exposure fell below half of its slow average for 3 s (a lamp was switched on: the IR lit scene got brighter).
 *   The pass threshold (day_hw_exp, 12 % of the ceiling) is far below the day trigger (95 %), and the night
 *   dwell doubles (up to x8) when the camera falls back to night within 10 minutes of a return to day.
 *   With night.ir_led=off there is no feedback loop: the exposure itself is read and no probe is needed.
 *
 * night.state stays "night" during a probe (the IR-cut filter does not move). */

#define NL_POLL_MS 100  /* one nl_step() per poll, matching stock's ht_night_mode_auto_th cadence */

typedef enum {
    NL_NONE = 0,
    NL_GO_NIGHT,     /* day -> night now */
    NL_GO_DAY,       /* night -> day now (also ends a probe: ircut_switch(0) leaves the LEDs off) */
    NL_PROBE_START,  /* turn the IR LEDs off, keep the filter in the night position */
    NL_PROBE_END,    /* the probe failed: turn the IR LEDs back on */
} nl_action_t;

typedef enum { NL_DAY = 0, NL_NIGHT = 1, NL_PROBING = 2 } nl_mode_t;

typedef struct {
    uint32_t hw_exp;    /* AE run info: exposure (ceiling 16384) */
    uint32_t sgain;     /* sensor gain, Q8 (256 = 1.0x) */
    uint32_t isp;       /* ISP gain, Q8 */
    int      lum;
} nl_sample_t;

typedef struct {
    uint32_t sgain_max; /* the AE's own limits, Q8; 0 = unknown (the gain test is then skipped) */
    uint32_t isp_max;
} nl_limits_t;

typedef struct {
    nl_mode_t mode;
    uint32_t  tick;           /* polls since nl_reset() */
    uint32_t  lock_until;     /* tick: no decision before (anti-flap lock after a switch) */
    uint32_t  mode_since;     /* tick of the last switch (or reset) */
    int       confirm;        /* consecutive confirming samples */
    int       ema_ok;
    uint32_t  ema_exp;        /* slow average of hw_exp in night mode (IR on) */
    int       lamp_cnt;       /* consecutive samples well below the average */
    int       lamp_pending;   /* a lamp was seen (lamp_cnt reached its limit) and no probe has run since */
    uint32_t  next_probe;     /* tick of the next periodic probe */
    uint32_t  lamp_ok_at;     /* tick before which a lamp probe is not allowed */
    uint32_t  probe_start;
    int       probe_pass;
    int       fails;          /* failed probes in this night */
    int       flap;           /* 0..3: night dwell multiplier 1 << flap */
    int       left_night_valid;
    uint32_t  left_night_at;
    unsigned  probes;         /* counters for the status line */
    unsigned  switches;
    char      last[160];      /* the values behind the last decision */
} nl_state_t;

/* Starts in `is_night` mode with the anti-flap lock cleared (boot, or a change of night.mode). */
void nl_reset(nl_state_t *st, const night_tuning_t *t, int is_night);

/* One poll. `s` NULL = no AE sample yet (time still passes, confirmations restart). `ir_active` = the IR LEDs
 * light in night mode (night.ir_led=auto). When the action is not NL_NONE, `why` (if not NULL) holds a one-line
 * description with the values that triggered it. */
nl_action_t nl_step(nl_state_t *st, const night_tuning_t *t, const nl_sample_t *s, const nl_limits_t *lim,
                    int ir_active, char *why, size_t why_cap);

/* The saturation test, exposed for the 8091 status line and the tests. */
int nl_saturated(const night_tuning_t *t, const nl_sample_t *s, const nl_limits_t *lim);

/* One line for the 8091 key night.logic: mode, confirmations, probes, next probe, last decision. */
void nl_status(const nl_state_t *st, const night_tuning_t *t, char *out, size_t cap);

const char *nl_mode_name(nl_mode_t m);

/* Saved manual mode (NIGHT_CONF_FILE, line "mode=day|night"; no file = auto, nothing is written until the user
 * changes the mode). _load returns 1 and sets *out for a valid forced mode, else 0 (no file, auto, or an invalid
 * line: logged, ignored). _save writes the file atomically for day / night and removes it for auto (the
 * default); 0 on success, -1 with errno set. */
int night_mode_load(const char *path, night_override_t *out);
int night_mode_save(const char *path, night_override_t m);
