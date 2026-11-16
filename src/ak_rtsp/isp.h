#pragma once
#include <stddef.h>
#include <stdint.h>

/* Load the GC20C3 sensor I2C register table from /tmp/sensor_isp.conf and
 * program it into the sensor via ioctl on /dev/v4l-subdev0. */
int isp_load_sensor_conf(void);

/* Program all 24 ISP image-pipeline modules from /tmp/sensor_isp.conf via
 * the tunnel ioctl on /dev/isp-param-0. */
int isp_pipeline_init(void);

/* Program the NIGHT ISP calibration — a real, separate 24-module subfile
 * (mode=1) found concatenated after the day subfile inside the same
 * sensor_isp.conf, not a separate file. See isp.c's comment above
 * isp_pipeline_init_night() for the full discovery trail. Call this
 * instead of isp_pipeline_init() when entering night mode; call
 * isp_pipeline_init() again to restore day. Returns -1 (and logs, without
 * sending anything) if the second subfile isn't present or doesn't
 * validate as mode=1 — never sends unverified data. */
int isp_pipeline_init_night(void);

/* Re-send the AWB_EX sub-payload (5124 bytes at module-0x13 block offset +0x7e4)
 * from sensor_isp.conf. Needed in ae_setup() because 256-byte stub buffers
 * cause EFAULT — the kernel reads all 5124 bytes via copy_from_user. */
int isp_reapply_awb_ex(void);

/* Shared ISP tunnel call — wraps ioctl(g_isp_fd, ISP_IOCTL_CMD, {flag=1,
 * inner_cmd, payload}). Used by ae.c and night.c for runtime AE/WB/frame-rate
 * control (isp.c's own pipeline-init code uses the ISP_SEND macro directly
 * since it iterates over module blocks with computed offsets). */
int isp_tunnel(uint32_t inner_cmd, void *payload);

/* Set manual white balance gains (256 = 1.0x). Used at startup (day WB) and
 * by night.c when switching day/night (night mode uses neutral gains since
 * color is meaningless under IR illumination). */
int isp_set_wb_manual(uint16_t r_gain, uint16_t g_gain, uint16_t b_gain);

/* Lock for the multi-ioctl ISP re-configurations (night switch, img.* keys of the 8091 server). Not
 * recursive; not taken by isp_tunnel() or by the single-ioctl getters/setters. */
void isp_cfg_lock(void);
void isp_cfg_unlock(void);
/* 1 once the first isp_pipeline_init() has completed (the 8091 server starts before it). */
int  isp_pipeline_ready(void);

/* Re-send the WB block of the day (0) or night (1) subfile of sensor_isp.conf as it is in the file
 * (the stock behaviour), instead of the hand-made manual block of isp_set_wb_manual(). */
int isp_reapply_wb_conf(int night);

/* Live-tunable picture controls (-50..50, matching anyka_ipc's own UI
 * slider range). All GET-modify-SET against real hardware, checked against
 * a capture of the stock firmware (see isp.c's comments on each function for
 * what is verified vs. what's a known gap):
 *   - saturation/contrast: small "effect" struct (module 0x0f).
 *   - brightness: the LIVE 0x720-byte exposure-attr struct ae.c's AE
 *     thread also polls — a different, riskier mechanism than the other
 *     three; see isp_set_brightness()'s comment for the safety reasoning.
 *   - sharpness: rescales most of a ~46KB blob (module 0x0b) from a cached
 *     baseline, on all 17 entries (sharp_all, default) or entry 0 only
 *     (default isp.sharpness -15, see isp_tune.h). */
int isp_set_saturation(int value);
int isp_set_contrast(int value);
int isp_set_brightness(int value);
int isp_set_sharpness(int value);
#define ISP_3DNR_COPY_SIZE 7180u /* bytes the kernel copies for the 3D_NR ioctl (the conf block has 7076) */
uint8_t *isp_module_alloc(int mod, uint32_t size); /* zeroed module buffer, 3D_NR padded to 4 + 7180 bytes */
void isp_set_sharp_all(int all); /* img.sharp_all: slider on all 17 entries (default 1; 0 = entry 0 only, the stock slider) */
int  isp_get_sharp_all(void);

/* Forget cached picture-control baselines — call after any full pipeline
 * reload (isp_pipeline_init()/isp_pipeline_init_night()) so a subsequent
 * slider move computes its delta against the freshly-loaded calibration,
 * not a stale one captured before the reload. The reload also drops the applied values, so the non-zero ones are
 * applied again here. See isp.c for detail. */
void isp_reset_picture_baselines(void);

/* NR and sharpening knobs (isp_nr_ctl.c, isp_nr.h; specs/video-and-streaming.md 5.5.7). All with the ISP lock held.
 * isp_nr_knob_set(knob, v): value already range-checked; >= 0 done (blocks sent, 0 = nothing to send), -1 ISP error
 * (errno), ISP_NR_REFUSED (-2) the live table is not as decoded. The old value is kept on failure. */
struct isp_nr_knobs;
const struct isp_nr_knobs *isp_nr_knobs(void);
int  isp_nr_knob_set(int knob, int v);
int  isp_nr_knob_get(int knob);
int  isp_set_denoise(int bias);              /* isp.denoise: the 3D-NR bias knob; 0 done, else the isp_nr_knob_set error */
int  isp_nr_status(char *out, size_t cap);   /* GET img.nr */
int  isp_nr_dump_live(void);                 /* SET img.dump nr: number of blocks read */
void isp_nr_reset(void);
void isp_nr_reapply(void);
int  isp_sharp_refresh(void);                /* 1 sent, 0 nothing to send, -1, ISP_NR_REFUSED */
