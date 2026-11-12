/* Saved video settings (specs/10 G11): bitrate cap, frame rate and GOP length, read once at start-up.
 *
 * File /etc/config/akam/video, mode 0600, written atomically, key=value lines (LF, a trailing CR is tolerated):
 *   bitrate=N   VBR+ ceiling in kbit/s, whole number VIDEO_KBPS_MIN..VIDEO_KBPS_MAX (default ENC_KBPS)
 *   fps=N       10, 15, 20 or 25 (default ENC_FPS). Only the pairs {fps, VTS} listed in video_day_vts() are allowed
 *               (VTS = 34260 / fps: 10 -> 3426, 15 -> 2284, 20 -> 1713, 25 -> 1370; checked on a real camera: exactly
 *               that frame rate over RTSP, no encoder error). The sensor frame rate is changed together with the
 *               encoder one, so the encoder never receives frames faster than it is told to expect. 30 fps (VTS
 *               1142) was tested and is refused: the picture was dark, grainy and nearly grey. A higher rate
 *               shortens the longest exposure (frame time), so dim scenes get darker and night mode starts earlier;
 *               night mode itself always runs at 10 fps / VTS 3426 (night.c).
 *   gop=S       key frame interval in whole seconds, VIDEO_GOP_S_MIN..VIDEO_GOP_S_MAX (default 2 s: ENC_GOP frames
 *               at the default 15 fps). The encoder gets S * fps frames (S * 25 = 100 at most).
 * An absent file, or an absent key, means the compiled-in default: the behaviour is then exactly the one of a build
 * without this module. A bad line (unknown key, bad number, out of range) is skipped with one log line and its key
 * keeps the default; nothing else is affected.
 *
 * The values are loaded once (video_cfg_load, first thing in main) and never change while ak_rtsp runs: the
 * encoder is created from them, and the SDP, ONVIF and the bitrate/GOP bookkeeping of rtsp.c report them. A change
 * of the file (./akam video, 8091 SET video.*_cfg) applies at the next start of ak_rtsp.
 * Not offered, on purpose (specs/10 G11): resolution (ISP crop, VI formats and the encoder DMA pool are sized for
 * 1920x1080 only) and mirror/flip (the ISP flip ioctl number is known, its payload and the sensor register
 * sequence are not). */
#pragma once
#include <stddef.h>

#include "ak_rtsp.h"

#ifndef VIDEO_CFG_PATH /* tests point it elsewhere */
#define VIDEO_CFG_PATH "/etc/config/akam/video"
#endif

#define VIDEO_CFG_MAX 256
#define VIDEO_KBPS_MIN 500
#define VIDEO_KBPS_MAX 3000
#define VIDEO_FPS_LOW 10
#define VIDEO_FPS_HIGH 25 /* allowed: 10, 15, 20, 25 (30 is refused, see above) */
#define VIDEO_GOP_S_MIN 1
#define VIDEO_GOP_S_MAX 4
#define VIDEO_GOP_S_DEF (ENC_GOP / ENC_FPS)

typedef struct {
    int bitrate; /* kbit/s */
    int fps;
    int gop_s;
} video_cfg_t;

/* Defaults (the compiled-in constants). */
void video_cfg_defaults(video_cfg_t *c);
/* 1 when fps is one of the allowed values. */
int video_cfg_fps_ok(long fps);
/* Strict parse of one number: whole decimal, no sign, no spaces. 0 and *out set, else -1. */
int video_cfg_num(const char *s, long *out);
/* One key=value pair: 0 and the field set when key and value are valid, else -1 (unknown key or bad value). */
int video_cfg_set_key(video_cfg_t *c, const char *key, const char *val);
/* Parse the file content over the defaults. Returns the number of lines skipped (0 = all valid). */
int video_cfg_parse(const char *text, size_t len, video_cfg_t *out);
/* Serialise (only the keys that differ from the defaults, so the defaults give ""). Returns the length, or -1 when
 * `cap` is too small. */
int video_cfg_format(const video_cfg_t *c, char *out, size_t cap);
/* Read VIDEO_CFG_PATH (0 = read, 1 = absent, -1 = unreadable) into *out (defaults on 1 and -1). */
int video_cfg_read(video_cfg_t *out);

/* Load the file once into the active settings (idempotent: later calls do nothing). */
void video_cfg_load(void);
/* The active settings (defaults before video_cfg_load). Never changes after the load. */
const video_cfg_t *video_cfg_active(void);
/* Derived, from the active settings. */
int video_fps(void);
int video_kbps(void);
int video_gop_frames(void);
/* Frame time VTS the ISP wants with the active (day) frame rate. */
int video_day_vts(void);
/* TEST-ONLY: drop the loaded state so the next video_cfg_load reads again. */
void video_cfg_reset_for_test(void);
