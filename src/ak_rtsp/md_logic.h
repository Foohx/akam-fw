#pragma once
/* Motion detection by frame differencing, the part with no I/O (host-tested: tests/native/md_logic_test.c).
 *
 * Input is the luma plane of the 320x180 channel. A frame is reduced to 80x45 means of 4x4 pixels, which hides
 * sensor noise and compression shimmer. Two reduced frames are compared after removing the mean change of the
 * whole picture, so a lamp or an exposure step (every block moves by about the same amount) does not count while
 * an object does. The score is the share of blocks that changed by at least `pixel_threshold`, in hundredths of
 * a percent. This is the same measure camd uses on the server (internal/motion/detector.go). */
#include <stdint.h>

#define MD_W       320
#define MD_H       180
#define MD_BLOCK   4
#define MD_BW      (MD_W / MD_BLOCK)
#define MD_BH      (MD_H / MD_BLOCK)
#define MD_BLOCKS  (MD_BW * MD_BH)

/* Reduces a MD_W x MD_H luma plane to MD_BLOCKS block means. */
void md_reduce(const uint8_t *luma, uint8_t *blocks);

/* Share of blocks that differ, in 1/100 %, 0..10000. */
unsigned md_score(const uint8_t *before, const uint8_t *after, int pixel_threshold);

typedef struct {
    int      pixel_threshold; /* block change that counts, 0..255 */
    unsigned area_x100;       /* score that makes a sample "moving", in 1/100 % */
    int      trigger_samples; /* consecutive moving samples that start motion */
    int      hold_ms;         /* motion ends after this long without a moving sample */
} md_params_t;

typedef struct {
    int      active;
    int      streak;
    int64_t  last_motion_ms;
    unsigned events;          /* number of times motion started */
    unsigned score;           /* latest sample */
    unsigned peak;            /* highest sample of the current or last motion */
} md_state_t;

/* Feeds one sample taken at now_ms. Returns 1 when motion starts, -1 when it ends, else 0. */
int md_feed(md_state_t *s, const md_params_t *p, unsigned score, int64_t now_ms);

/* Sensitivity presets: "low", "medium", "high". Returns 0, or -1 for an unknown name. */
int md_preset(const char *name, md_params_t *p);
