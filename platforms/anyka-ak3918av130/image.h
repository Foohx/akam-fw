/* Image effects of the Rotatable 2K own app, applied once after the streams are up, like the stock does after venc open
 * (yy_media_av130_video_init, RE): brightness, contrast, sharpness (each 0..100, stored by the stock as [dsp] brightness /
 * contrast / sharpness, default -1 = "not set", then NOTHING is applied and the ISP conf file values stay), the mains
 * frequency (anti_flicker: 0 and 1 mean 50 Hz, 2 means 60 Hz, the stock applies it at every start) and flip / mirror
 * (not here: they are user switches with their own state, orient.h, applied by the settings rows image.flip / image.mirror).
 *
 * The 0..100 setting v is applied to the ISP as v - 50 (100 is treated as 99, like the stock): 50 = the conf file value.
 * Config keys (akam.conf, passed by the supervisor): image_brightness, image_contrast, image_sharpness (0..100, absent =
 * not applied), mains_hz (50 or 60, default 50). The ISP keeps nothing: the settings registry saves the four
 * image.* rows (only on a user change) and applies them at start, and a2k_image_replay puts them back after a day / night switch. */
#ifndef AKAM2K_IMAGE_H
#define AKAM2K_IMAGE_H

typedef struct {
    int brightness; /* 0..100, -1 = not applied */
    int contrast;
    int sharpness;
    int mains_hz;   /* 50 or 60 */
} a2k_image_cfg_t;

void a2k_image_defaults(a2k_image_cfg_t *c);
/* The ISP value of a 0..100 setting (-50..49), or -1000 when pct is out of range. */
int a2k_image_effect_value(int pct);
/* 1 when every field is valid. */
int a2k_image_cfg_ok(const a2k_image_cfg_t *c);
/* Applies the effects; a failing call is logged and counted, the others still run. Returns the number of failures. */
int a2k_image_apply(const a2k_image_cfg_t *c);
/* After a successful ak_vi_switch_mode (the conf set was reloaded): applies the last valid configuration again when brightness, contrast or sharpness
 * was set; sends nothing otherwise. Returns the number of failures. Under the caller's ISP lock (a2k_isp_mode). */
int a2k_image_replay(void);

#endif
