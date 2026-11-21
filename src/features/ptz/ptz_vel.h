/* Generic pan/tilt VELOCITY mapping (pure: no I/O, no clock, no global state), shared by every front end that moves a PTZ head continuously:
 * the ONVIF PTZ service (ContinuousMove), the control API action ptz.vel and the MQTT command ptz_vel. A velocity is the ONVIF generic velocity
 * space, -1.0 .. 1.0 per axis, passed in thousandths (pan right+, tilt up+). The motor takes a SPEED DIVISOR per axis (one step per `divisor` ms,
 * smaller = faster, validated set 2..20) and a direction sign; this module converts one into the other.
 *
 * Mapping (the table in tests/native/ptz_vel_test.c pins it): |v| below PTZV_DEADZONE_MILLI (0.05) is "no motion" on that axis. Above it the step
 * RATE grows linearly from 50 steps/s (divisor 20) at the dead zone edge to 500 steps/s (divisor 2) at |v| = 1.0:
 *     rate = 50 + 450 * (|v| - 0.05) / 0.95        divisor = round(1000 / rate), clamped to 2..20
 * A diagonal (both axes moving) is capped at divisor 3 per axis (the validated limit of the diagonal driver words: divisor 2 was only verified on a
 * single axis). These are the REQUESTED divisors. The EFFECTIVE ones are decided by the motor's safe layer (ptz/akam_ptz_safe.h, 2K): a speed ladder (a start from rest at divisor 10, then 6, 4, 3, 2), and
 * tilt_min_div (default 3) caps the tilt axis and both axes of a diagonal: so the effective divisor is 2 for a pan alone at |v| >= 0.8, 3 for the tilt alone (a requested 2 becomes 3) and 3 per axis for a diagonal. Lease: the dead-man time of one command, 150..2000 ms, default 450 ms (the tracker's). */
#ifndef AKAM_PTZ_VEL_H
#define AKAM_PTZ_VEL_H
#include <stddef.h>

#define PTZV_DEADZONE_MILLI 50
#define PTZV_DIV_MIN 2
#define PTZV_DIV_MIN_DIAG 3
#define PTZV_DIV_MAX 20
#define PTZV_LEASE_MIN_MS 150
#define PTZV_LEASE_MAX_MS 2000
#define PTZV_LEASE_DEF_MS 450

typedef struct {
    int sx, sy;  /* direction signs: right+ / up+ in {-1, 0, 1} */
    int dx, dy;  /* speed divisors of the moving axes (0 for an axis that does not move) */
} ptzv_cmd_t;

/* Strict decimal text of a velocity component ("-0.35", "1", up to 4 fractional digits, no exponent, no leading '+' or '.') into thousandths,
 * rounded to the nearest. 0 ok, -1 not a number, -2 outside -1..1. */
int ptzv_parse_milli(const char *s, int *milli);

/* One axis: sign (0 inside the dead zone) and the single-axis divisor. milli is -1000..1000 (else the sign is 0). */
void ptzv_axis(int milli, int *sign, int *divisor);

/* Both axes. Returns 1 when something moves, 0 for a stop (both in the dead zone). Never fails: out-of-range input is clamped to +-1000. */
int ptzv_map(int pan_milli, int tilt_milli, ptzv_cmd_t *out);

/* A lease in ms clamped to 150..2000; ms <= 0 (absent) gives the default 450. */
int ptzv_lease_clamp(long ms);

/* The text arguments of the API action ptz.vel and of the MQTT command ptz_vel: pan / tilt = velocity text (ptzv_parse_milli; NULL = absent = 0, but not both absent), lease = whole
 * milliseconds 150..2000 as digits (NULL = absent = 450). 0 ok (*out, *lease_ms filled; out->sx == out->sy == 0 means STOP), -1 a bad argument or nothing to do, with *why set to a
 * static text for the reply. */
int ptzv_parse_args(const char *pan, const char *tilt, const char *lease, ptzv_cmd_t *out, int *lease_ms, const char **why);

/* xsd:duration of the ONVIF Timeout ("PT1S", "PT0.5S", "PT1.5S", "PT2S", "PT1M", "PT0S"): 0 and *ms filled (truncated to ms), -1 not a duration or negative. */
int ptzv_parse_duration_ms(const char *s, long *ms);

#endif
