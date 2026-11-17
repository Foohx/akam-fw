/* Person detection inside akam2k_rtsp (all vendor NPU calls of the app live in akam2k_detect.c). One thread pulls the
 * third VI channel (id 16, 320x192 NV12, configured by akam2k_cap.c together with chn 0/1), runs the stock human-detection
 * model through ak_svp2_process at a configurable rate, feeds the hysteresis state machine (akam2k_detstate.h) and
 * publishes the state file. Any failure (NPU init, model load, a dead channel) is logged once and only ends the
 * detection: the encoders and the RTSP server never depend on it. */
#ifndef AKAM2K_DETECT_H
#define AKAM2K_DETECT_H

#include <stdint.h>

#include "akam2k_zones.h"

#define A2K_DET_CHN 16
#define A2K_DET_W 320u
#define A2K_DET_H 192u
/* The stock model (read by the vendor lib from the camera's own file, never copied); always under this directory. */
#define A2K_DET_MODEL "/appfs/resource/model/model_0x0A00000D_V1.00.01.bin"

struct a2k_detect_cfg {
    int enabled;     /* 0/1 */
    int hz;          /* 1..10 inferences per second */
    int threshold;   /* classify threshold 500..950 (stock 700) */
    int on_count;    /* K: consecutive positive inferences for ON */
    int off_ms;      /* M: ms without a person for OFF */
    int debug;       /* 0/1: --detect-debug (conf detect_debug), one rate-limited line per inference with frame age, count, best score, and every box of both classes with its score */
    int age_window;  /* 0/1: the frame age reference is the minimum over the last 60 s instead of the all-time minimum (conf detect_age_window, experiment E3) */
    int model_thr;   /* 0 = off; 300..950: experiment E1 only, the model's own threshold (below the classify threshold): lower-scored persons are logged but never counted or tracked, heads are kept */
    a2k_zones_t zones; /* optional detection zones (akam/zones): n = 0 is the whole image */
};

/* Defaults: enabled, 5 Hz, 700, K 2, M 3 s, no zone. */
void a2k_detect_defaults(struct a2k_detect_cfg *c);
/* Starts the thread (the NPU is initialised inside it). Returns 0, or -1 when it could not start (logged). Only call
 * after VI chn 16 is enabled and the encoders are running. */
int a2k_detect_start(const struct a2k_detect_cfg *c);
/* Before a2k_detect_start: a callback given the Y plane (A2K_DET_W x A2K_DET_H, NV12 luma, stride A2K_DET_W) of every frame the thread pulls,
 * before it is released (the scene check, akam2k_scene.h). Must be cheap and never block. NULL = none. */
void a2k_detect_set_frame_cb(void (*cb)(const uint8_t *y, unsigned w, unsigned h, unsigned stride));
/* Joins the thread (the caller has set a2k_stop), which destroys the svp2 channel and de-initialises the NPU before it
 * returns. Safe when never started. Must run before the encoders and VI are torn down. */
void a2k_detect_stop(void);
/* 1 when the detection thread disabled VI chn 16 itself after it ended (after a2k_detect_stop returns). */
int a2k_detect_chn_disabled(void);

#endif
