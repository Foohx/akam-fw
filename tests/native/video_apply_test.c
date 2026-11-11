/* Records what ae_init_isp_params() (SET_FRAME_RATE, SET_SENSOR_FPS) and venc_create_encoder() (CREATE_ENC, 96
 * bytes) send to the kernel, one hex line each, so the saved video settings can be proved to leave the payloads
 * bit for bit unchanged when the file is absent. Real ae.c, venc.c and video_cfg.c; the ISP tunnel and the
 * ioctl are stubs that only record. With -DNO_VIDEO_CFG the harness builds against the pre-video_cfg ae.c/venc.c
 * (used once to produce tests/fixtures/video_apply_default.txt from dev). */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>

#include "ak_ioctls.h"
#include "ak_rtsp.h"
#include "ae.h"
#include "isp.h"
#include "venc.h"
#ifndef NO_VIDEO_CFG
#include "video_cfg.h"
#endif

int g_venc_fd = -1, g_chn0_fd = 5;
void *g_venc_virt;
uint32_t g_venc_phys;

int imgq_poll_stats(void) { return 0; }
int imgq_venc_rc_stock(void) { return 0; }
int imgq_wb_conf(void) { return 0; }
int isp_set_wb_manual(uint16_t r, uint16_t g, uint16_t b) { (void)r; (void)g; (void)b; return 0; }
int isp_reapply_awb_ex(void) { return 0; }
void swdog_beat(int s) { (void)s; }
void swdog_disarm(int s) { (void)s; }
int control_debug_on(void) { return 0; }

static void hex(const char *tag, const void *p, size_t n)
{
    printf("%s ", tag);
    for (size_t i = 0; i < n; i++) printf("%02x", ((const uint8_t *)p)[i]);
    printf("\n");
}

int isp_tunnel(uint32_t cmd, void *payload)
{
    if (cmd == ISP_INNER_SET_FRAME_RATE || cmd == ISP_INNER_SET_SENSOR_FPS) {
        char tag[32];
        snprintf(tag, sizeof(tag), "TUNNEL %08x", (unsigned)cmd);
        hex(tag, payload, 16);
    }
    return 0;
}

int ioctl(int fd, unsigned long req, ...)
{
    (void)fd;
    if (req == VENC_IOCTL_CREATE_ENC) {
        va_list ap;
        va_start(ap, req);
        void *arg = va_arg(ap, void *);
        va_end(ap);
        hex("CREATE_ENC", arg, 96);
    }
    return 0;
}

int main(void)
{
#ifndef NO_VIDEO_CFG
    video_cfg_load();
#endif
    ae_init_isp_params();
    venc_create_encoder();
    return 0;
}
