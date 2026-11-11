/* ONVIF Imaging service (version 2.0, namespace timg) at /onvif/imaging_service (specs/07 sections 3.1, 3.2).
 *
 * Video source token "vsrc". Mapping (the 8091 keys, through control_apply_setting so the code path, the lock and
 * the validation of an 8091 SET are the same):
 *   IrCutFilter  ON | OFF | AUTO   <->  night.mode day | night | auto   (ON = filter inserted = day)
 *   Brightness, ColorSaturation, Contrast, Sharpness  <->  isp.brightness / saturation / contrast / sharpness
 * Ranges: GetOptions states Min -50 Max 50 for the four, exactly the 8091 range, so a value means the same thing
 * on every interface and nothing is rescaled or rounded away (ONVIF allows any range and the clients that follow
 * GetOptions, the standard behaviour, adapt their sliders; a 0..100 transform would only add a lossy offset). A
 * request value may be a decimal ("12.5"); it is rounded to the nearest integer. Out of range: Sender fault
 * ter:InvalidArgVal / ter:SettingsInvalid, nothing is applied (the whole request is validated first).
 * WhiteBalance, Exposure, Focus, WideDynamicRange, BacklightCompensation are not reported (the camera has no
 * ONVIF-visible control of them); a request that contains one of them is refused with the same fault.
 *
 * Persistence (flash is a NOR/NAND with limited cycles and a slider sends a SetImagingSettings per step):
 *  - the picture values (isp.*) are applied in RAM at once and written to flash ONCE, ONVIF_IMAGING_PERSIST_MS
 *    after the last change (debounce), by a short-lived thread that exists only while a write is pending; the
 *    4-value file of isp_tune is written by control_persist_picture(), the same write a loopback SET does. A
 *    request with ForcePersistence=true writes at once, but at most once per ONVIF_IMAGING_FORCE_MIN_MS (a slider
 *    sends one per step); the ones in between are debounced like the others.
 *  - IrCutFilter is a discrete choice, not a slider: it is persisted at once, and only when it actually changed
 *    (night_set_tuning), exactly like a loopback SET night.mode.
 *  - SystemReboot (onvif_device.c) flushes a pending write first (onvif_imaging_flush_pending); a power cut inside
 *    the window still loses the last picture change.
 * Access: Get* READ, SetImagingSettings WRITE (refused when the RTSP password is off). GetServiceCapabilities
 * is open (pre-auth list). GetMoveOptions and GetStatus answer with empty (schema-valid) MoveOptions / Status; Move and Stop: Receiver fault
 * ter:ActionNotSupported (no focus motor). */
#pragma once
#include <stdint.h>

#define ONVIF_IMAGING_PERSIST_MS 5000
#define ONVIF_IMAGING_FORCE_MIN_MS 2000   /* ForcePersistence: at most one immediate write per this time */

/* Called by onvif_start. 0 or -1. */
int onvif_imaging_register(void);

/* ---- hooks for tests ---- */
/* Monotonic milliseconds; default CLOCK_MONOTONIC. */
extern int64_t (*onvif_imaging_now_ms)(void);
/* 0 = do not start the flush thread (the test calls onvif_imaging_poll itself); default 1. */
extern int onvif_imaging_thread;
/* Write the pending picture values if the quiet time has elapsed. 1 when it wrote (or tried), else 0. */
int onvif_imaging_poll(void);
/* Write the pending picture values now, quiet time or not (SystemReboot). 1 when it wrote, else 0. */
int onvif_imaging_flush_pending(void);
/* Number of writes done by the debounce / ForcePersistence (statistics and tests). */
unsigned onvif_imaging_flushes(void);
