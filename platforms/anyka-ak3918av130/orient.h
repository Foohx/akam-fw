/* Image orientation of the Rotatable 2K own app: flip (vertical) and mirror (horizontal), plus the pure mappings every consumer of the picture uses so
 * that the orientation never makes two parts disagree. "Ceiling mount" = flip + mirror (a 180 degree rotation of the picture).
 *
 * THE SDK CALL: ak_vpss_switch_flip_mirror(dev 0, flip_en, mirror_en), see isp.h a2k_isp_flip_mirror and docs/porting/lsc-rotatable-2k/isp-controls.md. It flips the
 * sensor / ISP readout, so EVERY consumer of the video (both RTSP streams, the JPEG snapshots, the detection channel, the scene thumbnails) sees the flipped picture.
 *
 * FRAME CONVENTIONS (one choice, applied everywhere):
 *  - DISPLAYED frame = what the viewer sees (the streams, the snapshots) = what the detector and the tracker get = what every user-facing coordinate is in
 *    (detect.boxes, the zone text entities, ONVIF / API directions).
 *  - SENSOR frame = the picture with flip and mirror both off. Stored data that must outlive an orientation change is kept in it: the detection ZONES (the file
 *    akam/zones and /config/akam/zones), the scene / drift REFERENCES (the live frame is converted to the sensor frame before it is compared or saved).
 *  The mappings below are involutions: the same call converts sensor -> displayed and displayed -> sensor.
 *
 * Directions: the motor drivers speak SENSOR directions (HAL_PTZ_RIGHT turns the camera to the right of the sensor frame). A mirror reverses the left / right of
 * the displayed picture relative to that, a flip reverses up / down: a user command in the displayed frame (ONVIF ContinuousMove, ptz.vel, the step buttons) and
 * the tracker's pan / tilt correction have their pan sign reversed by the mirror and their tilt sign by the flip. Motor positions (presets, patrol, soft limits,
 * the counters) are motor state and never depend on the orientation.
 *
 * State: two ints (RAM only: the persistence is the settings rows image.flip / image.mirror, loaded at boot). Thread safe. */
#ifndef AKAM2K_ORIENT_H
#define AKAM2K_ORIENT_H
#include <stdint.h>

/* The current orientation; every a2k_orient_get reader sees a consistent pair. */
void a2k_orient_get(int *flip, int *mirror);
/* Sets it (each 0 or 1), calls the listeners when it changed, and opens a settling window (a2k_orient_settling). Does NOT touch the ISP: the caller
 * (settings.c) records the orientation FIRST (so every consumer is on the new frame rules while the picture turns), then applies a2k_isp_flip_mirror, and puts
 * the old orientation back if the ISP refuses. That sequence, and the replay after a day / night switch (a2k_isp_mode), run under a2k_orient_isp_lock. */
void a2k_orient_set(int flip, int mirror);
/* Opens the settling window without a change (the ISP may hand out frames of the other orientation for a moment: a day / night switch before its flip replay). */
void a2k_orient_hold(void);
/* One lock around "read / record the orientation and call the ISP", so the ISP and the recorded orientation never disagree after two concurrent callers. */
void a2k_orient_isp_lock(void);
void a2k_orient_isp_unlock(void);
/* Increases at every change (a consumer that caches a converted value compares it). */
unsigned a2k_orient_gen(void);
/* 1 during A2K_ORIENT_SETTLE_MS after a change: the frames of the detection channel may still be of the old orientation (frames already queued), a consumer
 * that compares frames (the scene check) skips them. */
#define A2K_ORIENT_SETTLE_MS 1500
int a2k_orient_settling(void);
/* Called with the new orientation (from the thread that changed it) after every change. Up to 4 listeners; 0 or -1 (full). */
int a2k_orient_listen(void (*fn)(int flip, int mirror));
/* TEST ONLY: a fixed clock in ms for the settling window (-1 = the real one), and the full reset (state, gen, listeners). */
void a2k_orient_test_clock(int64_t ms);
void a2k_orient_test_reset(void);

/* ---- pure mappings (no state) ---- */
/* The sign of a SENSOR-frame direction for a command given in the DISPLAYED frame: s in -1..1, pan axis (left / right) reversed by the mirror. */
int a2k_orient_pan_sign(int s, int mirror);
/* Same for the tilt axis (up / down), reversed by the flip. */
int a2k_orient_tilt_sign(int s, int flip);
/* The tracker's motor direction flags: base (the 2K binding's invert_pan / invert_tilt, measured with the orientation off) combined with the orientation. */
int a2k_orient_invert_pan(int base, int mirror);
int a2k_orient_invert_tilt(int base, int flip);
/* A rectangle x1,y1,x2,y2 (x1 <= x2, y1 <= y2, same units, max as above) between the frames; the result keeps x1 <= x2 and y1 <= y2. */
void a2k_orient_rect(int flip, int mirror, int max_x, int max_y, int *x1, int *y1, int *x2, int *y2);
/* A luma plane (w x h bytes, src_stride) of the DISPLAYED frame into dst (w x h, stride w) in the SENSOR frame (or the other way: an involution). dst != src.
 * 0, or -1 (bad size). With flip = mirror = 0 it is a plain copy. */
int a2k_orient_plane(int flip, int mirror, const uint8_t *src, unsigned w, unsigned h, unsigned src_stride, uint8_t *dst);

#endif
