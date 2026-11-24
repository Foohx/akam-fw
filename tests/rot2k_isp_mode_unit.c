/* Host test of the day / night switch with an orientation on (a2k_isp_mode in cap.c, vendor calls stubbed): the settling window is open BEFORE the mode switch
 * (the lib may reset the flip inside it, so frames between the switch and the replay can be unflipped), the flip is replayed after it, and the window is
 * still open after the replay. With the orientation off nothing is replayed and no window opens. Prints "rot2k isp mode unit: N failed". */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "cap.h"
#include "isp.h"
#include "orient.h"

static int fails;
#define CHECK(c, msg) do { if (c) printf("  ok   - %s\n", msg); else { printf("  FAIL - %s\n", msg); fails++; } } while (0)

static int g_sw_n, g_sw_settling, g_fm_n, g_fm_f, g_fm_m, g_order_ok;
void a2k_log(const char *fmt, ...) { (void)fmt; }
int ak_vi_switch_mode(int dev, int night)
{
    (void)dev;
    (void)night;
    g_sw_n++;
    g_sw_settling = a2k_orient_settling();
    return 0;
}
static int g_pic_n, g_pic_night = -1, g_pic_after_switch;
int a2k_pic_replay(int night) /* picture.c: saturation, denoise and white balance are put back after the conf set was reloaded */
{
    g_pic_n++;
    g_pic_night = night;
    g_pic_after_switch = g_sw_n > 0;
    return 0;
}
int ak_vpss_switch_flip_mirror(int dev, int f, int m)
{
    (void)dev;
    g_fm_n++;
    g_fm_f = f;
    g_fm_m = m;
    g_order_ok = g_sw_n > 0;
    return 0;
}

int main(void)
{
    a2k_orient_test_reset();
    a2k_orient_test_clock(10000);
    CHECK(a2k_isp_mode(1) == 0 && g_sw_n == 1 && g_fm_n == 0 && !g_sw_settling && !a2k_orient_settling(), "orientation off: no replay, no settling window");
    a2k_orient_set(1, 0);
    a2k_orient_test_clock(10000 + A2K_ORIENT_SETTLE_MS + 100); /* the window of the change itself is over */
    CHECK(!a2k_orient_settling(), "the change's own window is over");
    g_sw_n = g_fm_n = g_pic_n = 0;
    CHECK(a2k_isp_mode(0) == 0 && g_sw_n == 1, "the mode switch ran");
    CHECK(g_sw_settling, "the settling window was already open when the lib switched the mode");
    CHECK(g_fm_n == 1 && g_fm_f == 1 && g_fm_m == 0 && g_order_ok, "the orientation is replayed after the switch");
    CHECK(a2k_orient_settling(), "the window is still open after the replay (the first frames after it may still be unflipped)");
    CHECK(g_pic_n == 1 && g_pic_night == 0 && g_pic_after_switch, "the picture settings (saturation, denoise, white balance) are replayed once, after the switch, with the day flag");
    g_pic_n = 0;
    CHECK(a2k_isp_mode(1) == 0 && g_pic_n == 1 && g_pic_night == 1, "and with the night flag after a switch to night");
    a2k_orient_test_reset();
    printf("rot2k isp mode unit: %d failed\n", fails);
    return fails != 0;
}
