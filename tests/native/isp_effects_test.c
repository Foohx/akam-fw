/* Host test: picture values set with isp_set_*() survive a pipeline reload (isp_reset_picture_baselines()
 * applies them again). ioctl() is replaced by a recorder; no hardware. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include "ak_rtsp.h"
#include "ak_ioctls.h"
#include "isp.h"

int g_isp_fd = 3;
int g_subdev;
static int n_effect, n_expo, n_sharp;
static uint8_t g_last_sharp[0xb5f8];

int ioctl(int fd, unsigned long req, ...)
{
    va_list ap;
    va_start(ap, req);
    struct isp_cmd_wrapper *w = va_arg(ap, struct isp_cmd_wrapper *);
    va_end(ap);
    (void)fd; (void)req;
    if ((unsigned)w->inner_cmd == (unsigned)ISP_INNER_EFFECT) n_effect++;
    if ((unsigned)w->inner_cmd == (unsigned)ISP_INNER_SET_EXPOSURE) n_expo++;
    if ((unsigned)w->inner_cmd == (unsigned)ISP_INNER_GET_SHARP) { /* a baseline with a pattern in every group */
        uint8_t *b = (uint8_t *)w->payload;
        for (int i = 0; i < 0xb5f8; i++) b[i] = (uint8_t)(i * 7 + 3);
    }
    if ((unsigned)w->inner_cmd == (unsigned)ISP_INNER_SHARP) { n_sharp++; memcpy(g_last_sharp, w->payload, sizeof(g_last_sharp)); }
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

    /* img.sharp_all: ON by default (all 16 groups); 0 = group 0 only, the stock slider's payload of before */
    uint8_t base[0xb5f8], g0only[0xb5f8], all16[0xb5f8];
    for (int i = 0; i < 0xb5f8; i++) base[i] = (uint8_t)(i * 7 + 3);
    CHECK(isp_get_sharp_all() == 1);
    CHECK(isp_set_sharpness(-15) == 0);                                           /* the default picture */
    memcpy(all16, g_last_sharp, sizeof(all16));
    CHECK(memcmp(all16 + 0x678, base + 0x678, 512) != 0);                         /* group 0 kernel rescaled */
    CHECK(memcmp(all16 + 0x678 + 0xab4, base + 0x678 + 0xab4, 512) != 0);         /* group 1 too */
    CHECK(memcmp(all16 + 0x678 + 0xab4 * 15, base + 0x678 + 0xab4 * 15, 512) != 0);
    CHECK(memcmp(all16 + 0x678 + 0xab4 * 16, base + 0x678 + 0xab4 * 16, 512) != 0);   /* entry 16 = ISO level 15 */
    CHECK(memcmp(all16, base, 0x640) == 0);                                       /* header untouched */
    isp_set_sharp_all(0);                                                         /* re-applies the slider at once */
    CHECK(isp_get_sharp_all() == 0);
    memcpy(g0only, g_last_sharp, sizeof(g0only));
    CHECK(memcmp(g0only + 0x678, all16 + 0x678, 512) == 0);                       /* group 0 identical */
    CHECK(memcmp(g0only + 0x678 + 0xab4, base + 0x678 + 0xab4, 512) == 0);        /* group 1 untouched */
    CHECK(memcmp(g0only + 0xab4 * 15 + 0x678, base + 0xab4 * 15 + 0x678, 512) == 0);
    CHECK(memcmp(g0only + 0xab4 * 16 + 0x678, base + 0xab4 * 16 + 0x678, 512) == 0);
    isp_set_sharp_all(1);
    CHECK(memcmp(g_last_sharp, all16, sizeof(all16)) == 0);                       /* back to the default payload */

    /* stock behaviour: a saved sharpness 0 is never applied, so the SHARP block stays the camera's own, byte for byte
     * (sharp_all 0 only matters once a non-zero slider is applied, and then gives the group-0-only payload checked above) */
    isp_set_sharp_all(0);
    CHECK(isp_set_sharpness(0) == 0);
    n_sharp = 0;
    isp_reset_picture_baselines();
    CHECK(n_sharp == 0);
    isp_set_sharp_all(1);

    /* 3D_NR: the kernel copies 7180 bytes, the conf block has 7076: the send buffer is 4 + 7180, zero tail (as the stock) */
    {
        uint8_t *b = isp_module_alloc(0x04, 4 + 7076);
        CHECK(b != NULL);
        if (b) {
            int tail_zero = 1;
            for (unsigned i = 4 + 7076; i < 4 + ISP_3DNR_COPY_SIZE; i++) if (b[i]) tail_zero = 0;
            CHECK(tail_zero);
            CHECK(ISP_3DNR_COPY_SIZE == 7180);
            free(b);
        }
        b = isp_module_alloc(0x03, 100);                                        /* other modules: exact size, zeroed */
        CHECK(b != NULL && b[99] == 0);
        free(b);
    }

    if (fails) return 1;
    puts("isp_effects_test: OK");
    return 0;
}
