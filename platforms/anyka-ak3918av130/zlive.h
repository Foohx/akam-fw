/* Runtime (live) detection zones and ignore rectangles of the Rotatable 2K own app: the process-wide copy of the zones.h data that
 * the detection thread reads every tick and the Home Assistant `text` entities edit. Thread safe (one mutex, copies only).
 *
 * Source at start: the runtime file /config/akam/zones (written by a text edit, same format as akam/zones extended with ignore_ lines),
 * else the card file akam/zones, else nothing (the whole image). A runtime file that is invalid is logged and the card file is used.
 * An edit is validated as a whole: invalid text keeps the old value (the caller publishes the real value back), a valid one is
 * stored atomically in /config/akam/zones and the generation counter moves.
 *
 * WHEN THEY APPLY: the rectangles are percent of the detector image, so a POSITION zone only means something while the camera looks where it
 * looked when it was drawn. A zone or ignore rectangle without suffix or with `@position` is applied ONLY while the camera is at HOME (the ptz
 * state "home") or its position is not managed (state "unknown": ptz_cal off, the motor is never moved); while it is "moved", "parked" or
 * "calibrating" it is suspended (a2k_zones_effective: nothing ignored, the zone's sensor reads off, and with no zone left in force the whole
 * image counts). A `@screen` zone or ignore rectangle is image-relative and ALWAYS applies, whatever the camera position.
 */
#ifndef AKAM2K_ZLIVE_H
#define AKAM2K_ZLIVE_H
#include <stddef.h>

#include "zones.h"

/* init: `initial` = the zones read from the card file (NULL = none); store_path = the runtime file (NULL = no persistence, tests).
 * Returns 1 when the runtime file was used, else 0. Call before the detection thread and MQTT start. */
int a2k_zlive_init(const a2k_zones_t *initial, const char *store_path);
void a2k_zlive_get(a2k_zones_t *out, unsigned *gen);            /* the configured zones (all of them), whatever the ptz state, in the SENSOR frame (orient.h) */
/* The edit of one text (A2K_ZF_ZONES or A2K_ZF_IGNORE, zones.h). The text is in the DISPLAYED frame (what the viewer sees: with image.flip / image.mirror on, the coordinates
 * are those of the flipped picture) and is converted to the sensor frame for the store; a2k_zlive_get_text converts back. 0 applied, -1 invalid (nothing changed). */
int a2k_zlive_set_text(int which, const char *text);
void a2k_zlive_get_text(int which, char *out, size_t cap);
/* 1 when the position-anchored zones apply in this ptz state (enum hal_ptz_state): home and unknown. @screen zones apply always. */
int a2k_zlive_applies(int ptz_state);
/* Called (from the editing thread) after every applied change with the new configured zones; cheap, never blocks on the network. */
void a2k_zlive_set_notify(void (*fn)(const a2k_zones_t *));

#endif
