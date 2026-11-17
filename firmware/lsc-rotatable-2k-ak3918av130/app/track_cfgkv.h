/* The scalar fields of struct tl_cfg as name/value pairs: the raw recorder (track_debug=2, akam2k_track.c) writes them in its "C" line and the replay tool
 * (tests/rot2k_track_replay.c) reads them back, so a replay runs with the configuration of the recording. Header only, no I/O. A field missing here is simply
 * not recorded (the replay keeps the default); add new tl_cfg scalars to the list. */
#ifndef TRACK_CFGKV_H
#define TRACK_CFGKV_H
#include "track_logic.h"
#include <stdio.h>
#include <string.h>

#define TL_CFG_INTS(X) \
    X(deadband_pct) X(tband_pct) X(gain_pan) X(gain_tilt) X(gain_pct) X(max_step) X(min_interval_ms) X(confirm_ticks) X(settle_ms) X(fast_pct) X(speed_fast) \
    X(lead_pct) X(lead_max) X(tilt_enable) X(lost_grace_s) X(return_home) X(return_delay_s) X(head_pct) X(tilt_return_s) X(sticky_gate_pct) X(invert_pan) \
    X(invert_tilt) X(zone_x_pct) X(zone_y_pct) X(zone_head_pct) X(zone_stop_pct) X(meas_px) X(accel_mms2) X(steps_rad) X(person_h_mm) X(shoulder_mm) \
    X(full_pct) X(predict_ms) X(motion) X(lease_ms) X(rate_k) X(gap_predict_ms) X(lost_n) X(tilt_back_s) X(tilt_back_min) X(lag_ms)

/* "key=value " for every field into out; returns the length (truncated at cap - 1). */
static inline size_t tl_cfg_kv_dump(const struct tl_cfg *c, char *out, size_t cap)
{
    size_t k = 0;
    int w;
    out[0] = 0;
#define TL_KV_W(f) \
    if (k + 1 < cap) { w = snprintf(out + k, cap - k, #f "=%d ", c->f); if (w > 0) k += (size_t)w < cap - k ? (size_t)w : cap - k - 1; }
    TL_CFG_INTS(TL_KV_W)
#undef TL_KV_W
    return k;
}

/* Sets one field by name; 0 when known, -1 otherwise. */
static inline int tl_cfg_kv_set(struct tl_cfg *c, const char *name, int v)
{
#define TL_KV_S(f) if (!strcmp(name, #f)) { c->f = v; return 0; }
    TL_CFG_INTS(TL_KV_S)
#undef TL_KV_S
    return -1;
}
#endif
