/* Image orientation of the 1080p: image.flip (vertical), image.mirror (horizontal), image.ceiling (both). The names, the meaning and the saved
 * text are the ones of the 2K (src/features/orient/orient_rows.h, shared); this file is the 1080p part: the state, the ISP call and the file.
 *
 * THE CALL: ISP tunnel command 0x40044979 (ISP_INNER_FLIP_MIRROR) with two 32-bit words {flip, mirror}. The kernel module ak_isp.ko turns it
 * into a flip / mirror of the GC20C3 sensor readout (the sensor driver programs its own registers, the ISP adopts the new Bayer order), so
 * EVERY consumer of the picture sees the flipped image and the app spends no CPU on it (no encoder or VI pixel work). Live, no stream
 * restart; the encoder may drop a frame or two while it turns (ak_isp.ko module parameter flip_drop_frames).
 * Source of the layout: reverse engineering of ak_isp.ko and sensor_gc20c3.ko of the stock firmware, specs/video-and-streaming.md 5.7.
 * NOT yet hardware-validated on the 1080p.
 *
 * FILE: /etc/config/akam/orient ("flip=1", "mirror=1"), written only on a user change (and removed when both go back to off), applied once
 * at start-up before the video channel starts. The state is RAM; a reboot never writes it. */
#pragma once
#include <stddef.h>

#ifndef ORIENT_FILE
#define ORIENT_FILE "/etc/config/akam/orient"
#endif

/* The saved file; tests point it elsewhere. */
extern const char *image_orient_path;

/* The current orientation (0 or 1 each). */
void image_orient_get(int *flip, int *mirror);

/* Writes row `key` (image.flip, image.mirror or image.ceiling) = v (0 or 1). Returns 0 done, 1 done but the file could not be saved (errno
 * set; the picture is turned, a reboot loses it), -1 refused (unknown row, bad value, or the ISP refused: nothing changed).
 * persist: 0 keeps the change in RAM only. An unchanged value sends nothing and writes nothing. */
int image_orient_set_row(const char *key, int v, int persist);

/* Start-up: reads the file (absent = off; an invalid file is logged and means off) and turns the picture. Never writes. The ISP and the
 * sensor are up (isp_pipeline_init done), the video channel is not started yet. 0, or -1 when the ISP refused (state stays off). */
int image_orient_apply_saved(void);

/* Sends the current orientation to the ISP again, for the day / night switch (the ISP blocks are re-sent; the call is absolute and cheap, the
 * kernel does nothing when the value is unchanged). The CALLER HOLDS the ISP configuration lock (isp_cfg_lock). Sends nothing while both are off. */
void image_orient_replay_locked(void);

/* The ISP call, replaceable (tests). Returns 0 or -1. Called with the ISP configuration lock held. */
extern int (*image_orient_hw)(int flip, int mirror);
