#pragma once
#include <stddef.h>

/* Image-quality switches for A/B tests against the stock firmware (specs/08-spec-image-quality.md).
 * Every switch defaults to the previous behaviour; a switch is flipped for good only after the hardware
 * comparison of the spec's section 7.
 *
 * Start-up options come from the environment, then from /tmp/akam_image (RAM, gone at reboot), then from
 * /etc/config/akam/image (saved; key=value lines, '#' comments), then from the defaults:
 *   venc_rc  = legacy | stock   AKAM_VENC_RC   CREATE_ENC rate-control sub-struct: legacy = the values ak_rtsp
 *                                               always sent, stock = exactly what the stock application sends
 *   wb       = manual | conf    AKAM_IMG_WB     manual = hand-made manual white-balance block at start-up,
 *                                               conf = keep the sensor_isp.conf block (stock)
 *   poll     = 0 | 1            AKAM_IMG_POLL   1 = poll the ISP statistics the stock application polls
 *                                               (3D-NR statistics about 1.2 Hz, sensor fps about 2 Hz)
 * venc_rc=stock has crashed the kernel with other rc words before (see venc.c): keep it in /tmp/akam_image (a crash
 * then self-heals at the reboot), not in the saved file, until a hardware test has passed.
 * 8091 keys (LIST / GET / SET), debug keys: spec 07 makes 8091 read-only remotely (loopback writes only), so the
 * img.* SETs follow whatever rule the 8091 read-only step applies to SET. They never write to flash:

 *   img.venc_rc    read-only (decided when the encoder is created)
 *   img.wb         manual | conf     applies at once (day only for manual; a night -> day switch re-sends the
 *                                    file's block, so the manual block lasts until the next switch)
 *   img.poll       0 | 1             applies at once
 *   img.reload     1 (write only)    re-send the whole conf of the current mode, then the AE tuning
 *   img.dump       confirmed | all   (write only) log the live ISP blocks against the file and write
 *                                    /tmp/isp_live/isp_blocks.bin */

#define IMGQ_CONF_FILE "/etc/config/akam/image"
#define IMGQ_RAM_FILE  "/tmp/akam_image"  /* same format, RAM only, read after the file above */
#define IMGQ_DUMP_DIR  "/tmp/isp_live"

/* The keys that send ISP blocks (img.wb, img.reload, img.dump) refuse until the first isp_pipeline_init() has
 * completed and run under isp_cfg_lock(), like a day/night switch. */

/* (Re)reads `conf_file`, then `ram_file`, then the environment (NULL = no such file). Called lazily with the
 * two default paths by the getters below. */
void imgq_load(const char *conf_file, const char *ram_file);

int imgq_venc_rc_stock(void);
int imgq_wb_conf(void);
int imgq_poll_stats(void);

/* Same contract as ledsys_ctl_*: _get returns 0 and fills `out` for a key it owns, else -1; _set returns
 * -1 for a key it does not own, else 0 (done) or 1 (refused) with the answer in `msg`; _list emits one line per key. */
int  imgq_ctl_get(const char *key, char *out, size_t cap);
int  imgq_ctl_set(const char *key, const char *val, char *msg, size_t cap);
void imgq_ctl_list(void (*emit)(void *arg, const char *line), void *arg);
