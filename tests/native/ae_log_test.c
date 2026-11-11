/* Log volume of the AE tick thread (real ae.c, stubbed ISP): without the 8091 DEBUG mode only the first window is
 * logged within a few seconds (then one summary per minute); with DEBUG on, one line per ~1 s window.
 * Usage: ae_log_test [debug]  -- prints the [ae] lines of ~3.4 s of running. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ae.h"
#include "ak_ioctls.h"
#include "ak_rtsp.h"
#include "isp.h"

static int g_dbg;

int g_venc_fd = -1, g_chn0_fd = 5;
void *g_venc_virt;
uint32_t g_venc_phys;

int control_debug_on(void) { return g_dbg; }
int imgq_poll_stats(void) { return 0; }
int imgq_venc_rc_stock(void) { return 0; }
int imgq_wb_conf(void) { return 0; }
int isp_set_wb_manual(uint16_t r, uint16_t g, uint16_t b) { (void)r; (void)g; (void)b; return 0; }
int isp_reapply_awb_ex(void) { return 0; }
void swdog_beat(int s) { (void)s; }
void swdog_disarm(int s) { (void)s; }
int isp_tunnel(uint32_t cmd, void *payload) { (void)cmd; (void)payload; return 0; }

int main(int argc, char **argv)
{
    g_dbg = argc > 1 && !strcmp(argv[1], "debug");
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (ae_start() != 0) return 1;
    usleep(3400000);
    ae_stop();
    return 0;
}
