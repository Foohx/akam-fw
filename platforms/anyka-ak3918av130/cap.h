/* Capture side of the Rotatable 2K own app: the PoC-2 bring-up sequence for BOTH VI channels, one encoder per channel
 * (main 2304x1296 Main profile, sub 640x360 Main, Baseline if refused) and one capture thread per channel that pulls a frame, encodes
 * it and publishes the access unit into its a2k_stream (stream.h). All vendor calls of the app live in
 * cap.c. */
#ifndef AKAM2K_CAP_H
#define AKAM2K_CAP_H
#include <signal.h>

#include "detect.h"
#include "snap.h"
#include "stream.h"

struct a2k_cap_cfg {
    int main_kbps;   /* target of the main stream, VBR, max = 1.25 x */
    int sub_kbps;
    int gop;         /* frames, both streams */
    struct a2k_detect_cfg det; /* person detection (VI chn 16 + NPU thread); det.enabled 0 = none of it is set up */
    struct a2k_snap_cfg snap;  /* JPEG snapshot on detection (third encoder, sub channel); snap.enabled 0 = none of it */
    int video_live;  /* 1 (default): a video setting change is tried live (vlive.h) before the encoder re-open; 0 = always re-open */
    int venc_test;   /* 1: debug hook /tmp/akam2k_venc_test polled by the capture threads (hardware test); default 0 */
    int isp_test;    /* 1: RAM-only ISP test hook /tmp/akam2k_isp_test (isptest.h, hardware test); default 0 */
};

/* Brings up sdk, vi (both channels) and both encoders, then starts the capture threads. s[0] = main, s[1] = sub
 * (initialised here from the config). With snap.enabled a third, JPEG, encoder is opened next to the two H.264 ones; when it
 * cannot be, the snapshots are switched off (logged once) and the app goes on. Returns 0, or -1 after tearing down
 * whatever was opened. */
int a2k_cap_start(const struct a2k_cap_cfg *cfg, a2k_stream_t *s[2]);
/* 1 when a capture thread gave up (no frame or encode failure for too long): the caller should shut down. */
int a2k_cap_failed(void);
/* Stops the threads (the caller has set its stop flag: they poll `stop`) and tears venc/vi/sdk down in reverse. */
void a2k_cap_stop(void);

/* The flag the capture threads poll. */
extern volatile sig_atomic_t a2k_stop;

#endif
