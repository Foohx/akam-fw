/* 2K binding: the hal_ptz descriptor (hal.h). Hardware facts (ptz-findings.md, verified and measured 2026-11-15): the motor node
 * is /dev/tymotor; driver commands 0 up, 2 right, 4 down, 6 left; mechanical range pan 0..~4000 (left stop 0, right stop 3932..4151),
 * tilt 0..~2315 (bottom stop 0, top stop 2290..2340; the stock step maximum 2160 is SHORTER, so the calibration sweeps tilt twice), so the soft
 * range keeps 300 steps from the pan stops and about 190 from the tilt top stop: pan 300..3650, tilt 300..2100 (level-up view ~2104); home = the level front view 1275/1300; park = pan home, tilt at the soft min (300 steps above the bottom stop: looking down); speed
 * divisor 3 (~333 steps/s); a button step is 200 steps. The same numbers are the code defaults of ptz/akam_ptz_safe.h (the tool agrees).
 * Overrides: /config/akam/ptz.conf. */
#include "hal.h"

const hal_ptz_desc_t a2k_ptz_desc = {
    "/dev/tymotor", { 0, 2, 4, 6 }, 300, 3650, 300, 2100, 1275, 1300, 1275, 300, 4000, 2160, 3, 200,
    { 1, 3, 5, 7 }, 1, /* diagonals: up-right 1, down-right 3, down-left 5, up-left 7 (measured 2026-11-16/17: both axes move at once, ~370 steps/s each at divisor 3) */
};
