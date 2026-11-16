/* The process-wide tracker (akam2k_track_run.c): see akam2k_track.h for the behaviour. */
#ifndef AKAM2K_TRACK_RUN_H
#define AKAM2K_TRACK_RUN_H
#include "akam2k_track.h"

/* After a2k_ptz_start. conf_areas: "track_area_<name>=..." lines of akam2k.conf or NULL; notify: after an edit of the areas (cheap). */
/* Teardown: the detection thread (stopped last) feeds nothing to the motor any more. */
void a2k_track_run_stop(void);
/* invert_pan / invert_tilt: the 2K binding's motor direction of the tracker (hardware: pan must be inverted, tilt unverified = 0). */
void a2k_track_run_start(int enabled, const char *conf_areas, void (*notify)(void), int invert_pan, int invert_tilt);
a2k_track_t *a2k_track_run_get(void); /* NULL before start */
/* The detection thread, once per inference (n = 0: nobody seen). Never blocks. */
void a2k_track_feed(int64_t now_ms, const struct tl_box *boxes, int n, int fw, int fh);

#endif
