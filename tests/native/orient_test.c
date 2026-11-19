/* Host test of src/ak_rtsp/orient.c (picture flip and mirror) with the ISP stubbed. */
#include <stdint.h>
#include <stdio.h>

#include "ak_ioctls.h"
#include "orient.h"

#define CHECK(c) do { if (!(c)) { printf("orient_test: FAILED line %d: %s\n", __LINE__, #c); return 1; } } while (0)

static int g_calls, g_flip, g_mirror, g_rc;
static uint32_t g_cmd;

int isp_tunnel(uint32_t cmd, void *payload)
{
    const int32_t *fm = payload;
    g_calls++;
    g_cmd = cmd;
    g_flip = fm[0];
    g_mirror = fm[1];
    return g_rc;
}

int main(void)
{
    /* before the ISP is up nothing is sent, and the default orientation never is */
    CHECK(orient_set(0, 0) == 0 && g_calls == 0);
    orient_after_mode_switch();
    CHECK(g_calls == 0);
    CHECK(orient_set(1, 1) == 0 && g_calls == 0);

    /* the remembered orientation goes out when the ISP is ready: two words, flip then mirror */
    orient_ready();
    CHECK(g_calls == 1 && g_cmd == ISP_INNER_FLIP_MIRROR && g_flip == 1 && g_mirror == 1);

    /* the same values again cost no ioctl; a change does */
    CHECK(orient_set(1, 1) == 0 && g_calls == 1);
    CHECK(orient_set(7, 0) == 0 && g_calls == 2 && g_flip == 1 && g_mirror == 0); /* any non-zero is 1 */

    /* the ISP forgets at a day/night switch: sent again */
    orient_after_mode_switch();
    CHECK(g_calls == 3 && g_flip == 1 && g_mirror == 0);

    /* back to the default: sent once, then nothing more at the switches */
    CHECK(orient_set(0, 0) == 0 && g_calls == 4 && g_flip == 0 && g_mirror == 0);
    orient_after_mode_switch();
    CHECK(g_calls == 4);

    /* a refusal is reported, and the wanted values are tried again at the next switch */
    g_rc = -1;
    CHECK(orient_set(0, 1) == -1 && g_calls == 5);
    g_rc = 0;
    orient_after_mode_switch();
    CHECK(g_calls == 6 && g_flip == 0 && g_mirror == 1);

    puts("orient_test: ok");
    return 0;
}
