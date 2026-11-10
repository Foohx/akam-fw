/* Host test: picture values set with isp_set_*() survive a pipeline reload (isp_reset_picture_baselines()
 * applies them again). ioctl() is replaced by a recorder; no hardware. */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include "ak_rtsp.h"
#include "ak_ioctls.h"
#include "isp.h"

int g_isp_fd = 3;
int g_subdev;
static int n_effect, n_expo, n_sharp;

int ioctl(int fd, unsigned long req, ...)
{
    va_list ap;
    va_start(ap, req);
    struct isp_cmd_wrapper *w = va_arg(ap, struct isp_cmd_wrapper *);
    va_end(ap);
    (void)fd; (void)req;
    if ((unsigned)w->inner_cmd == (unsigned)ISP_INNER_EFFECT) n_effect++;
    if ((unsigned)w->inner_cmd == (unsigned)ISP_INNER_SET_EXPOSURE) n_expo++;
    if ((unsigned)w->inner_cmd == (unsigned)ISP_INNER_SHARP) n_sharp++;
    return 0;
}

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

int main(void)
{
    isp_reset_picture_baselines();
    CHECK(n_effect + n_expo + n_sharp == 0); /* nothing set: nothing re-applied */

    CHECK(isp_set_saturation(10) == 0);
    CHECK(isp_set_contrast(5) == 0);
    CHECK(isp_set_brightness(-3) == 0);
    CHECK(isp_set_sharpness(7) == 0);
    n_effect = n_expo = n_sharp = 0;
    isp_reset_picture_baselines();
    CHECK(n_effect == 2 && n_expo == 1 && n_sharp == 1);

    CHECK(isp_set_saturation(0) == 0);
    CHECK(isp_set_contrast(0) == 0);
    CHECK(isp_set_brightness(0) == 0);
    CHECK(isp_set_sharpness(0) == 0);
    n_effect = n_expo = n_sharp = 0;
    isp_reset_picture_baselines();
    CHECK(n_effect + n_expo + n_sharp == 0); /* back to neutral: nothing to restore */

    if (fails) return 1;
    puts("isp_effects_test: OK");
    return 0;
}
