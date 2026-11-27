/* Motion detection of the Rotatable 2K own app (read-only state for the API and Home Assistant, direction hint for the tracker's search).
 * The pure grid logic is src/features/motion/motion_grid.h; this file is the wiring: the source, the masks, the thread-safe state.
 *
 * SOURCE (conf motion_source, default diff): "diff" = frame differencing of the 320x192 luma plane of the detection channel (about 15 K reads per frame at 5 Hz, no vendor call,
 * no extra channel; the picture frame by construction, flip / mirror included). "md" = the vendor VPSS motion statistics (ak_vpss_md_get_stat, 1536 bytes polled with each frame; its layout is NOT
 * decoded yet, see docs/porting/lsc-rotatable-2k/motion.md), falling back to "diff" by itself when the statistics answer nothing usable. The source in force is the read-only row motion.source.
 *
 * MASKS (motion is ignored, never false): the head moves (position counters change) and 0.7 s after, an orientation change (flip / mirror / ceiling) and its settle window (also opened after a day / night
 * switch), a stream restart (a gap in the frames), a size change, a global change of the picture. The frame handed to the policy is always the PICTURE frame, so left / right / up / down
 * are the user's; the tracker maps them to the motor with the orientation-aware invert flags it already has.
 *
 * Runs in the detection thread (a2k_motion_frame is the frame callback), reads of the state are thread safe. Off (motion.enabled 0): nothing is computed, the tracker uses the box evidence only. */
#ifndef AKAM2K_MOTION_H
#define AKAM2K_MOTION_H
#include <stdint.h>

#include "track_logic.h"

enum { A2K_MOTION_DIFF = 0, A2K_MOTION_MD = 1 };

struct a2k_motion_info {
    int enabled, valid, active;
    int dir;                 /* MG_DIR_* = TL_EXIT_* (0 none, 1 down, 2 up, 3 left, 4 right) */
    int x, y;                /* centroid, percent of the picture (0..100, -1 when not active) */
    int cells;
    int source;              /* A2K_MOTION_* in force */
};

/* Before the detection thread starts. source: A2K_MOTION_*, sensitivity 1..10. */
void a2k_motion_init(int enabled, int sensitivity, int source);
/* The tracker's input (called with each frame's state; NULL = none). The notify hook is told of every change of active / direction (never blocks). */
void a2k_motion_set_sink(void (*sink)(const struct tl_mot *m));
void a2k_motion_set_notify(void (*fn)(const struct a2k_motion_info *i));
/* The downstream frame callback (the scene check): called with the same frame after the motion step. */
void a2k_motion_set_chain(void (*cb)(const uint8_t *y, unsigned w, unsigned h, unsigned stride));
/* The vendor statistics reader (platforms/anyka-ak3918av130/motion_md.c); rc 0 = buf filled. NULL = none (the "md" source then runs on "diff"). */
void a2k_motion_set_md_reader(int (*rd)(void *buf, unsigned n));
/* The vendor statistics reader of the 2K (platforms/anyka-ak3918av130/motion_md.c). */
int a2k_motion_md_read(void *buf, unsigned n);
/* Where the vendor statistics grid lives: 1 = the picture frame (default), 0 = the sensor frame (converted with the orientation). Unknown until a camera answers (docs/porting/lsc-rotatable-2k/motion.md). */
void a2k_motion_set_md_frame(int picture);
/* The frame callback of the detection channel (a2k_detect_set_frame_cb). */
void a2k_motion_frame(const uint8_t *y, unsigned w, unsigned h, unsigned stride);
/* Live settings (API motion.enabled / motion.sensitivity); get returns the current value. */
void a2k_motion_set_enabled(int on);
int a2k_motion_get_enabled(void);
void a2k_motion_set_sensitivity(int s);
int a2k_motion_get_sensitivity(void);
void a2k_motion_info(struct a2k_motion_info *out);
const char *a2k_motion_source_name(void); /* diff | md | md>diff (md configured, unusable, diff running) | off */
/* TEST ONLY: a fixed clock in ms (-1 = the real one), and a full reset (tests provide their own a2k_ptz_info). */
void a2k_motion_test_clock(int64_t ms);
void a2k_motion_test_reset(void);

#endif
