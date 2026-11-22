/* Detection zones of the Rotatable 2K own app: the strict parser of the optional card file akam/zones and the zone
 * membership rule. Pure logic (the file reader is the only I/O: one read-only fopen with a variable path), host
 * unit-tested and fuzzed (tests/rot2k_zones_unit.c). No file means the whole image (the behaviour without zones).
 *
 * akam/zones: key=value lines, LF (a trailing CR is tolerated), at most A2K_ZONES_FILE_MAX bytes, no blank line, no
 * comment, no space. An empty file is valid and means no zone. At most one line per key:
 *   <name>=x1,y1,x2,y2   a rectangle in PERCENT of the image, integers 0..100, x1 < x2 and y1 < y2, origin top left.
 *                        <name>: a lower case letter then up to 15 of a-z 0-9 _ (it becomes the Home Assistant entity
 *                        person_<name>). Up to A2K_ZONE_MAX (8) zones, names unique; "score" and "zone_mode" are reserved,
 *                        and so is any name starting with "ignore_".
 *   zone_mode=any|all    any (default): the global person is ON while a person stands in ANY zone; all: only while every
 *                        zone holds a person at the same time (several people, or one person where zones overlap).
 * ignore_<name>=x1,y1,x2,y2  an IGNORE rectangle (same syntax, up to A2K_IGNORE_MAX (8), names unique among the ignore rectangles): a person
 *                        whose feet point lies in one is dropped before anything else (no presence, no zone, no tracking).
 * ANCHOR (optional suffix of the rectangle, zones and ignore rectangles alike): `<name>=x1,y1,x2,y2@screen` is always active, image-relative,
 *                        whatever the camera position; `@position` (also what NO suffix means) is active only at the position it belongs to
 *                        (the ptz state home, or not managed) and suspended while the camera is moved, parked or calibrating.
 * FRAME: the rectangles of this file are in the SENSOR frame (the picture with image.flip and image.mirror off), so a zone keeps covering the same part of the
 * scene when the orientation is changed; the text entities and the detection work in the displayed frame and convert (a2k_zones_orient, orient.h).
 * Any other line, a bad value, a duplicate or a ninth zone / ninth ignore rectangle makes the whole file invalid: the app then logs it once and
 * keeps the whole image (an invalid zones file never silences the detection).
 *
 * A person counts in a zone when its FEET POINT, the bottom centre of its box, lies in the zone (edges included). The
 * feet point is where the person stands: unlike a minimum overlap it does not flip when an arm or a head leans into a
 * neighbouring zone, it needs no tuned threshold, and a box cut by the bottom edge of the image still gives the right
 * answer (the box is clamped to the image, so its bottom edge is the image edge). */
#ifndef AKAM2K_ZONES_H
#define AKAM2K_ZONES_H
#include <stddef.h>

#include "detstate.h"

#define A2K_ZONES_FILE_MAX 1024
#define A2K_IGNORE_MAX 8
#define A2K_IGNORE_PREFIX "ignore_"
#define A2K_ZONES_DEFAULT "/mnt/sdcard/akam/zones"
#define A2K_ZONE_MODE_ANY 0
#define A2K_ZONE_MODE_ALL 1
/* The detection image (detect.h): the box coordinates are pixels of this frame. */
#define A2K_ZONE_IMG_W 320
#define A2K_ZONE_IMG_H 192

/* anchor: what the text said. A2K_ZA_NONE and A2K_ZA_POSITION behave the same (kept apart so the text round-trips). */
enum { A2K_ZA_NONE, A2K_ZA_SCREEN, A2K_ZA_POSITION };

struct a2k_zone {
    char name[A2K_ZONE_NAME_MAX + 1];
    int x1, y1, x2, y2;     /* percent */
    int anchor;             /* A2K_ZA_* */
    int off;                /* 1 = suspended (set by a2k_zones_effective, never parsed): never hits, never ignores */
};

typedef struct {
    int n;                  /* 0 = the whole image */
    int mode;               /* A2K_ZONE_MODE_* */
    struct a2k_zone z[A2K_ZONE_MAX];
    int ni;                 /* ignore rectangles, 0..A2K_IGNORE_MAX (name without the ignore_ prefix) */
    struct a2k_zone ig[A2K_IGNORE_MAX];
} a2k_zones_t;

/* 0 and *out filled, -1 invalid (*out untouched). Never prints, never reads past len. */
int a2k_zones_parse(const char *text, size_t len, a2k_zones_t *out);
/* Reads and parses the file. 0 valid, 1 absent (*out zeroed: the whole image), -1 unreadable (not a regular file, too big)
 * or invalid (*out zeroed). */
int a2k_zones_load(const char *path, a2k_zones_t *out);
/* Text forms, for the runtime editing (Home Assistant text entities) and the persisted file. sep: '\n' (file) or ';' (one line).
 * a2k_zones_format: part A2K_ZF_ZONES = the zones and, when "all", zone_mode; A2K_ZF_IGNORE = the ignore rectangles WITHOUT their prefix;
 * A2K_ZF_FILE = everything as akam/zones (zones, zone_mode, ignore_ lines; every line ends with sep). Empty text = nothing. Returns the
 * length, -1 when it does not fit. */
enum { A2K_ZF_ZONES, A2K_ZF_IGNORE, A2K_ZF_FILE };
int a2k_zones_format(const a2k_zones_t *z, int part, char sep, char *out, size_t cap);
/* Replaces the zones (which = A2K_ZF_ZONES: "name=x1,y1,x2,y2;...;zone_mode=all|any") or the ignore rectangles (A2K_ZF_IGNORE:
 * "name=x1,y1,x2,y2;...") of *z from one line of text; "" and "none" clear them. 0, or -1 (invalid, *z untouched). */
int a2k_zones_set_text(a2k_zones_t *z, int which, const char *text);
/* *out = the zones in force: a copy of cfg in which every position-anchored (or unanchored) zone and ignore rectangle is marked off unless
 * `home` (the camera is where the zones were drawn); @screen ones are always on. When no zone is on, the zone part is cleared (n = 0: the
 * whole image counts, like a configuration without zones); the indexes of the zones that stay are those of cfg. */
void a2k_zones_effective(const a2k_zones_t *cfg, int home, a2k_zones_t *out);
/* Converts every zone and ignore rectangle of *zs between the SENSOR frame and the DISPLAYED frame (orient.h: mirror reverses x, flip reverses y; the same call
 * converts both ways). The stored data (the files, a2k_zlive) is in the sensor frame, so it keeps its meaning when the orientation changes; the detection and the text
 * entities work in the displayed frame. Names, anchors, off flags and the mode are untouched. */
void a2k_zones_orient(a2k_zones_t *zs, int flip, int mirror);
/* Bit mask of the zones that are not off. */
unsigned a2k_zones_active_mask(const a2k_zones_t *zs);
/* 1 when the feet point of box b is inside zone z. */
int a2k_zone_contains(const struct a2k_zone *z, const struct a2k_det_box *b);
/* 1 when the feet point of b lies in an ignore rectangle of zs. */
int a2k_zones_ignored(const a2k_zones_t *zs, const struct a2k_det_box *b);
/* Bit mask of the zones that contain the feet point of b (bit i = zone i). 0 with no zone configured. */
unsigned a2k_zones_hit(const a2k_zones_t *zs, const struct a2k_det_box *b);

#endif
